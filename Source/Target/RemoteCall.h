#pragma once

#include "Core/CheckedMutex.h"
#include "Core/Errors.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;

    // Calls functions inside a target without injecting a DLL: a small
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
        bool Running() const { return m_running; }
        void SetTimeout(std::chrono::milliseconds timeout) { m_timeout = timeout; }
        void SetGuard(const CheckedMutex* guard) { m_guard = guard; }

        // Call function(a0..a3) in the target and return its result.
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t& result);
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t& result) { return Call(function, a0, 0, 0, 0, result); }
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t& result) { return Call(function, a0, a1, 0, 0, result); }

        // A code address in the target that, called as void(item, user), appends
        // item to the array the user pointer names: { u64 count; u64 cap; u64 items[cap]; }.
        std::uint64_t CollectorAddress() const { return m_collector; }

        // Walks an iterator API of the form item = function(object, &iter)
        // (mono_class_get_methods, il2cpp_class_get_fields, ...) inside the
        // target until it returns null, gathering a whole batch of items per
        // round trip instead of one call each. With a peek offset the pointer
        // at item + offset is read next to each item, in the target; with a
        // peek function, peek(item) is called instead (it wins over the offset).
        struct DrainItem
        {
            std::uint64_t Item = 0;
            std::uint64_t Peek = 0;
        };
        static constexpr std::uint32_t kNoPeek = 0xFFFFFFFFu;
        Status Drain(std::uint64_t function, std::uint64_t object, std::vector<DrainItem>& items, std::uint32_t peekOffset = kNoPeek,
            std::uint64_t peekFunction = 0, std::size_t limit = 65536);

        // A fresh, zeroed region in the shared scratch page (for iterators,
        // collector arrays and out-parameters). Returns 0 when the arena is full.
        std::uint64_t Scratch(std::size_t bytes);
        void ResetScratch();

        std::string ReadCString(std::uint64_t address, std::size_t limit = 512) const;

    private:
        void Wake() const;
        Status WaitForResult(std::uint64_t control, std::uint64_t& result);

        std::shared_ptr<Process> m_process;
        void* m_wake = nullptr;          // local event; the worker waits on its duplicate
        std::uint64_t m_code = 0;      // executable page: worker + collector
        std::uint64_t m_control = 0;    // control block for the worker
        std::uint64_t m_data = 0;       // scratch arena
        std::uint64_t m_dataUsed = 0;
        std::uint64_t m_collector = 0;
        std::uint64_t m_drain = 0;
        std::uint64_t m_drainBlock = 0;  // the drain's batch block
        bool m_running = false;
        std::chrono::milliseconds m_timeout{ 5000 };
        const CheckedMutex* m_guard = nullptr;
    };
}
