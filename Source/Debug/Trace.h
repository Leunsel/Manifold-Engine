#pragma once

#include "Core/Errors.h"
#include "Debug/BreakAction.h"
#include "Disasm/Disassembler.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Debug
{
    class Debugger;

    struct TraceStep
    {
        std::uint64_t Address = 0;
        Disasm::RegisterValues Registers;
        int Depth = 0;
        Disasm::Flow Kind = Disasm::Flow::None;
        std::uint32_t ThreadId = 0;
    };

    struct TraceRequest
    {
        std::uint64_t Address = 0;
        int Count = 2000;
        bool StepOver = false;
        BreakAction Trigger;
        std::vector<std::uint8_t> StopCode;
        std::string StopText;
    };

    // Break and trace: drives the debugger's own stepping from a background
    // thread, so it works the same with the Windows backend and the in-process
    // one and needs no change to the exception handlers.
    class Tracer
    {
    public:
        explicit Tracer(Debugger& debugger);
        ~Tracer();

        Tracer(const Tracer&) = delete;
        Tracer& operator=(const Tracer&) = delete;

        Status Start(std::shared_ptr<Target::Process> process, const Disasm::Disassembler& disassembler, const TraceRequest& request);
        void Stop();
        bool Running() const { return m_running.load(); }
        bool Waiting() const { return m_waiting.load(); }
        std::uint64_t TriggerAddress() const { return m_address.load(); }
        std::uint64_t Generation() const { return m_generation.load(); }
        std::string StatusText() const;

        std::size_t Count() const;
        std::vector<TraceStep> Steps() const;

    private:
        void Run(TraceRequest request);
        void Finish(const std::string& status);

        Debugger& m_debugger;
        std::shared_ptr<Target::Process> m_process;
        const Disasm::Disassembler* m_disassembler = nullptr;
        std::thread m_thread;
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_waiting{ false };
        std::atomic<bool> m_cancel{ false };
        std::atomic<std::uint64_t> m_generation{ 1 };
        std::atomic<std::uint64_t> m_address{ 0 };

        mutable std::mutex m_lock;
        std::vector<TraceStep> m_steps;
        std::string m_status;
    };
}
