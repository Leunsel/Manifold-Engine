#pragma once

#include "Core/Errors.h"

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
        void LeaveMain();
        // While set, EnterMain gives up at once instead of waiting for a frame.
        void InterruptMain(bool interrupt) { m_interrupt = interrupt; }
        bool InsideMain() const;
        std::uint64_t MainRuns() const;
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

        std::shared_ptr<Process> m_process;
        void* m_wake = nullptr;          // local event; the worker waits on its duplicate
        std::uint64_t m_code = 0;      // executable page: worker + collector
        std::uint64_t m_control = 0;    // control block for the worker
        std::uint64_t m_data = 0;       // scratch arena
        std::uint64_t m_dataUsed = 0;
        std::uint64_t m_collector = 0;
        std::uint64_t m_thread = 0;     // target thread handle (as a value)
        std::chrono::milliseconds m_timeout{ 5000 };
        std::atomic<bool> m_interrupt{ false };
    };
}
