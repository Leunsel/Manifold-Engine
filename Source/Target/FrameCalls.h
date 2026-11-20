#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;

    struct FrameArg
    {
        std::uint64_t Value = 0;
        int From = -1;

        FrameArg() = default;
        FrameArg(std::uint64_t value) : Value(value) {}
        FrameArg(std::uint64_t value, int from) : Value(value), From(from) {}

        static FrameArg Of(int call, std::uint64_t add = 0) { return FrameArg(add, call); }
    };

    class FrameBatch
    {
    public:
        int Call(std::uint64_t function, FrameArg a0 = {}, FrameArg a1 = {}, FrameArg a2 = {}, FrameArg a3 = {});
        std::uint64_t Cell(std::size_t bytes, const void* initial = nullptr);
        std::size_t Calls() const { return m_calls.size(); }
        std::size_t Bytes() const { return m_data.size(); }
        bool Fits(std::size_t calls, std::size_t bytes) const;
        bool Overflowed() const { return m_overflow; }

        std::uint64_t Result(int call) const;
        bool Read(std::uint64_t cell, void* out, std::size_t size) const;
        template <typename T> T Value(std::uint64_t cell) const
        {
            T value{};
            Read(cell, &value, sizeof(T));
            return value;
        }
        std::uint64_t Pointer(std::uint64_t cell) const;

    private:
        friend class FrameCalls;

        struct Entry
        {
            std::uint64_t Function = 0;
            std::uint64_t Args[4] = {};
            std::uint64_t Result = 0;
            std::uint16_t Refs[4] = {};
            std::uint8_t Pad[8] = {};
        };

        std::vector<Entry> m_calls;
        std::vector<std::uint8_t> m_data;
        std::uint64_t m_base = 0;
        std::size_t m_maxCalls = 0;
        std::size_t m_maxBytes = 0;
        int m_pointer = 8;
        bool m_overflow = false;
    };

    class FrameCalls
    {
    public:
        static constexpr std::size_t kMaxCalls = 8192;
        static constexpr std::size_t kDataSize = 0x100000;
        static constexpr int kHookSlots = 4;
        static constexpr std::uint32_t kCallsPerFrame = 1024;

        using WorkerCall = std::function<std::optional<std::uint64_t>(std::uint64_t function, std::uint64_t a0, std::uint64_t a1)>;

        struct Ran
        {
            std::uint64_t Frames = 0;
        };

        FrameCalls() = default;
        ~FrameCalls();
        FrameCalls(const FrameCalls&) = delete;
        FrameCalls& operator=(const FrameCalls&) = delete;

        Status Start(std::shared_ptr<Process> process, WorkerCall worker);
        bool Started() const { return m_control != 0; }
        Status Hook(std::uint64_t slot, std::uint64_t expected);
        bool Hooked() const { return m_hooked; }
        void Unhook();
        void Forget();

        std::uint64_t Frames() const;
        std::uint32_t Thread() const;
        bool Pending() const;
        std::size_t Executed() const { return m_executed; }

        std::uint64_t StoreFunction() const { return m_store; }
        std::uint64_t LoadFunction() const { return m_load; }
        std::uint64_t CopyFunction() const { return m_copy; }
        std::uint64_t TextFunction() const { return m_text; }

        FrameBatch Batch() const;
        Result<Ran> Run(FrameBatch& batch, std::chrono::milliseconds timeout, const std::function<bool()>& aborted = {});
        Status RunOnWorker(FrameBatch& batch);

        static Result<std::vector<std::uint8_t>> Code(bool is64, std::uint64_t address, std::uint64_t control, std::vector<std::uint64_t>* labels = nullptr);

    private:
        void Flush();
        Status Post(FrameBatch& batch);
        Status Collect(FrameBatch& batch);
        bool Done() const;

        std::shared_ptr<Process> m_process;
        WorkerCall m_worker;
        std::uint64_t m_code = 0;
        std::uint64_t m_control = 0;
        std::uint64_t m_entries = 0;
        std::uint64_t m_data = 0;
        std::uint64_t m_thunks[kHookSlots] = {};
        std::uint64_t m_direct = 0;
        std::uint64_t m_store = 0;
        std::uint64_t m_load = 0;
        std::uint64_t m_copy = 0;
        std::uint64_t m_text = 0;
        std::uint64_t m_hookSlots[kHookSlots] = {};
        int m_hooksUsed = 0;
        bool m_hooked = false;
        std::uint64_t m_hookSlot = 0;
        std::uint64_t m_hookOriginal = 0;
        std::uint64_t m_hookThunk = 0;
        std::size_t m_executed = 0;
    };
}
