#pragma once

#include "Core/Errors.h"

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
    // such as mono_assembly_foreach gather their items into an array.
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

        // Call function(a0..a3) in the target and return its 64-bit result.
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t& result);
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t& result) { return Call(function, a0, 0, 0, 0, result); }
        Status Call(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t& result) { return Call(function, a0, a1, 0, 0, result); }

        // A code address in the target that, called as void(item, user), appends
        // item to the array the user pointer names: { u64 count; u64 cap; u64 items[cap]; }.
        std::uint64_t CollectorAddress() const { return m_collector; }

        // A fresh, zeroed region in the shared scratch page (for iterators,
        // collector arrays and out-parameters). Returns 0 when the arena is full.
        std::uint64_t Scratch(std::size_t bytes);
        void ResetScratch();

        std::string ReadCString(std::uint64_t address, std::size_t limit = 512) const;

    private:
        std::shared_ptr<Process> m_process;
        std::uint64_t m_code = 0;      // executable page: worker + collector
        std::uint64_t m_control = 0;    // control block for the worker
        std::uint64_t m_data = 0;       // scratch arena
        std::uint64_t m_dataUsed = 0;
        std::uint64_t m_collector = 0;
        std::uint64_t m_thread = 0;     // target thread handle (as a value)
    };
}
