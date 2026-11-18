#pragma once

#include "Core/Errors.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;

    // Calls functions inside a 64-bit target without injecting a DLL: a small
    // position-independent worker is written into an executable page and run on
    // one persistent remote thread, so every call shares the same thread (it
    // can be attached to a runtime once, e.g. mono_thread_attach). Manifold
    // drives it through a control block in a second page and reads results and
    // out-data back with ReadProcessMemory. A collector stub lets callback APIs
    // such as mono_assembly_foreach gather their items into an array. Between
    // calls the worker spins briefly, then waits on an event Call sets. Stop
    // ends it with exit code 1; when this process ends without Stop (crash,
    // killed), the worker sees it through a handle of this process and ends by
    // itself with exit code 2.
    class RemoteCall
    {
    public:
        RemoteCall() = default;
        ~RemoteCall();

        RemoteCall(const RemoteCall&) = delete;
        RemoteCall& operator=(const RemoteCall&) = delete;

        Status Start(std::shared_ptr<Process> process);
        void Stop();
        bool Running() const { return m_thread != 0; }
        void SetTimeout(std::chrono::milliseconds timeout) { m_timeout = timeout; }

        // Call function(a0..a3) in the target and return its 64-bit result.
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t& result);
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t& result) { return Call(function, a0, 0, 0, 0, result); }
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t& result) { return Call(function, a0, a1, 0, 0, result); }

        // A code address in the target that, called as void(item, user), appends
        // item to the array the user pointer names: { u64 count; u64 cap; u64 items[cap]; }.
        std::uint64_t CollectorAddress() const { return m_collector; }

        // Walks an iterator API of the form item = function(object, &iter)
        // (mono_class_get_methods, il2cpp_class_get_fields, ...) inside the
        // target until it returns null, gathering a whole batch of items per
        // round trip instead of one call each. With a peek offset the 8 bytes
        // at item + offset are read next to each item, in the target; with a
        // peek function, peek(item) is called instead (it wins over the offset).
        struct DrainItem
        {
            std::uint64_t Item = 0;
            std::uint64_t Peek = 0;
        };
        static constexpr std::uint32_t kNoPeek = 0xFFFFFFFFu;
        Status Drain(std::uint64_t function, std::uint64_t object, std::vector<DrainItem>& items, std::uint32_t peekOffset = kNoPeek,
            std::uint64_t peekFunction = 0, std::size_t limit = 65536);

        // A second channel that runs calls on whatever thread calls MainStub()
        // as void(): a game registers it as a per-frame callback, so the calls
        // run on its main thread between frames. The stub returns at once
        // unless EnterMain asked for it; then it holds the thread and serves
        // CallMain requests until LeaveMain, or by itself once no request came
        // for a few dozen milliseconds (so a host that dies or stalls never
        // freezes the game). Its control block lives in the code page, which is
        // never freed, so a stub still registered after Stop keeps returning.
        std::uint64_t MainStub() const { return m_code ? m_code + 0x300 : 0; }
        Status EnterMain(std::chrono::milliseconds wait);
        Status CallMain(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t& result);

        // A list of calls the stub runs back to back in ONE main-thread visit:
        // one write sends them, one read brings every result back, so the game
        // waits for the calls themselves instead of a cross-process round trip
        // per call. The records live in target memory the caller owns
        // (`buffer`, calls.size() * kMainCallSize bytes, read-write).
        static constexpr std::size_t kMainCallSize = 0x80;
        static constexpr std::uint32_t kMainCallData = 64;
        struct MainCall
        {
            std::uint64_t Function = 0;
            std::uint64_t Args[4] = {};
            // After the call returns non-null, CopyBytes bytes at result +
            // CopyOffset are copied into Data, still on the main thread (e.g.
            // a boxed value, read before the garbage collector can take it).
            std::uint32_t CopyOffset = 0;
            std::uint32_t CopyBytes = 0;
            // Written into the record before the call; an argument may point
            // here (see MainCallData), e.g. a params array and its values.
            std::array<std::uint8_t, kMainCallData> Data{};
        };
        struct MainResult
        {
            std::uint64_t Result = 0;
            std::uint64_t Slot = 0;     // the record's own 8-byte slot, zero before the call
            std::array<std::uint8_t, kMainCallData> Data{};
        };
        // Addresses inside the record of call `index`, to pass as arguments.
        static std::uint64_t MainCallSlot(std::uint64_t buffer, std::size_t index) { return buffer + index * kMainCallSize + 0x38; }
        static std::uint64_t MainCallData(std::uint64_t buffer, std::size_t index) { return buffer + index * kMainCallSize + 0x40; }
        Status CallMainBatch(std::uint64_t buffer, const std::vector<MainCall>& calls, std::vector<MainResult>& results);
        void LeaveMain();
        // While set, EnterMain gives up at once instead of waiting for a frame.
        void InterruptMain(bool interrupt) { m_interrupt = interrupt; }
        bool InsideMain() const;
        std::uint64_t MainRuns() const;
        // Main-thread requests that had to wait for the game's next frame
        // because the stub had already returned (a running total).
        std::uint64_t FrameWaits() const { return m_frameWaits; }
        std::uint32_t MainThreadId() const;

        // A fresh, zeroed region in the shared scratch page (for iterators,
        // collector arrays and out-parameters). Returns 0 when the arena is full.
        std::uint64_t Scratch(std::size_t bytes);
        void ResetScratch();

        std::string ReadCString(std::uint64_t address, std::size_t limit = 512) const;

    private:
        void Wake() const;
        std::uint64_t ReadMain(std::uint32_t offset) const;
        Status WaitForResult(std::uint64_t control, bool main, std::uint64_t& result, bool* notTaken);
        Status SendMain(std::uint64_t request, const std::uint64_t* block, std::size_t words, std::uint64_t& result);

        std::shared_ptr<Process> m_process;
        void* m_wake = nullptr;          // local event; the worker waits on its duplicate
        std::uint64_t m_code = 0;      // executable page: worker + collector
        std::uint64_t m_control = 0;    // control block for the worker
        std::uint64_t m_data = 0;       // scratch arena
        std::uint64_t m_dataUsed = 0;
        std::uint64_t m_collector = 0;
        std::uint64_t m_drain = 0;
        std::uint64_t m_drainBlock = 0;  // the drain's batch block
        std::uint64_t m_thread = 0;     // target thread handle (as a value)
        std::chrono::milliseconds m_timeout{ 5000 };
        std::uint64_t m_frameWaits = 0;
        std::atomic<bool> m_interrupt{ false };
    };
}
