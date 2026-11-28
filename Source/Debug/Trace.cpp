#include "Debug/Trace.h"

#include "Core/MainThread.h"
#include "Core/Text.h"
#include "Debug/Debugger.h"
#include "Target/Process.h"

#include <algorithm>
#include <chrono>

namespace Manifold::Debug
{
    Tracer::Tracer(Debugger& debugger) : m_debugger(debugger) {}

    Tracer::~Tracer()
    {
        Stop();
    }

    void Tracer::Stop()
    {
        m_cancel = true;
        if (m_thread.joinable())
            m_thread.join();
        m_cancel = false;
    }

    Status Tracer::Start(std::shared_ptr<Target::Process> process, const Disasm::Disassembler& disassembler, const TraceRequest& request)
    {
        Stop();
        if (!m_debugger.Attached())
            return Status::Fail(ErrorCode::DebugTraceFailed, "the debugger is not attached");
        if (m_debugger.HasBreakpoint(request.Address))
            return Status::Fail(ErrorCode::DebugTraceFailed, Hex(request.Address) + " already has a breakpoint");
        if (m_debugger.Broken())
            return Status::Fail(ErrorCode::DebugTraceFailed, "the process is stopped at a breakpoint; let it run first");
        m_process = std::move(process);
        m_disassembler = &disassembler;
        {
            std::lock_guard lock(m_lock);
            m_steps.clear();
            m_status = "waiting for " + Hex(request.Address);
        }
        m_address = request.Address;
        ++m_generation;
        m_running = true;
        m_waiting = true;
        m_cancel = false;
        m_thread = std::thread(&Tracer::Run, this, request);
        return Status::Success();
    }

    void Tracer::Finish(const std::string& status)
    {
        m_debugger.RemoveBreakpoint(m_address.load());
        const BreakInfo held = m_debugger.Break();
        if (held.Broken)
        {
            m_debugger.Resume(ResumeMode::Continue);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (m_debugger.Attached() && std::chrono::steady_clock::now() < until)
            {
                const BreakInfo now = m_debugger.Break();
                if (!now.Broken || now.Serial != held.Serial)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        {
            std::lock_guard lock(m_lock);
            m_status = status;
        }
        m_waiting = false;
        m_running = false;
        ++m_generation;
    }

    void Tracer::Run(TraceRequest request)
    {
        SetThreadName(L"Tracer");
        BreakAction action = request.Trigger;
        action.Break = true;
        // ∑ Read before the breakpoint is set: a hit that comes while SetBreakpoint returns would have this number already,
        //   and the trace would wait for ever beside the stopped process.
        std::uint64_t lastSerial = m_debugger.Break().Serial;
        const Status set = m_debugger.SetBreakpoint(request.Address, BreakpointType::Execute, 1, false, action);
        if (!set)
        {
            Finish(set.Text);
            return;
        }

        const auto waitForBreak = [&]() -> bool
            {
                while (!m_cancel && m_debugger.Attached() && m_process->Alive())
                {
                    const BreakInfo info = m_debugger.Break();
                    if (info.Broken && info.Serial != lastSerial)
                    {
                        lastSerial = info.Serial;
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                return false;
            };

        if (!waitForBreak())
        {
            Finish(m_cancel ? "stopped" : "the process ended before " + Hex(request.Address) + " was reached");
            return;
        }
        m_waiting = false;
        action.ThreadId = m_debugger.Break().ThreadId;
        m_debugger.SetBreakpointAction(request.Address, action);
        {
            std::lock_guard lock(m_lock);
            m_status = "tracing";
        }
        ++m_generation;

        int depth = 0;
        std::string ending = "done";
        for (int taken = 0; taken < request.Count; ++taken)
        {
            if (m_cancel)
            {
                ending = "stopped";
                break;
            }
            const BreakInfo info = m_debugger.Break();
            if (!info.Broken)
            {
                ending = "the process ran on";
                break;
            }
            const Disasm::Instruction instruction = m_disassembler->Read(*m_process, info.Address, nullptr);
            TraceStep step;
            step.Address = info.Address;
            step.Registers = info.Registers;
            step.Depth = depth;
            step.Kind = instruction.Kind;
            step.ThreadId = info.ThreadId;
            {
                std::lock_guard lock(m_lock);
                m_steps.push_back(step);
            }
            ++m_generation;

            if (!request.StopCode.empty() && ConditionHolds({ {}, {}, true, 0, request.StopCode, {} }, info.Registers, info.ThreadId, *m_process))
            {
                ending = "stop condition held";
                break;
            }

            const bool follow = !request.StepOver;
            if (follow && instruction.Kind == Disasm::Flow::Call && instruction.Valid)
                ++depth;
            else if (instruction.Kind == Disasm::Flow::Return && depth > 0)
                --depth;

            m_debugger.Resume(request.StepOver ? ResumeMode::StepOver : ResumeMode::StepInto);
            if (!waitForBreak())
            {
                ending = m_cancel ? "stopped" : "the process ended";
                break;
            }
        }
        if (static_cast<int>(Count()) >= request.Count)
            ending = "reached " + std::to_string(request.Count) + " steps";
        Finish(ending);
    }

    std::size_t Tracer::Count() const
    {
        std::lock_guard lock(m_lock);
        return m_steps.size();
    }

    std::vector<TraceStep> Tracer::Steps() const
    {
        std::lock_guard lock(m_lock);
        return m_steps;
    }

    std::string Tracer::StatusText() const
    {
        std::lock_guard lock(m_lock);
        return m_status;
    }
}
