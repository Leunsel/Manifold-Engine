#include "Debug/Debugger.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Debug/VehProtocol.h"
#include "Syscalls/Syscall.h"
#include "Target/Process.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>

#include <windows.h>

namespace Manifold::Debug
{
    namespace
    {
        constexpr DWORD kWx86Breakpoint = 0x4000001F;
        constexpr DWORD kWx86SingleStep = 0x4000001E;
        constexpr std::uint64_t kTrapFlag = 0x100;
        constexpr std::uint64_t kResumeFlag = 0x10000;
        constexpr std::size_t kMaxWatchEntries = 4096;
        constexpr std::uint64_t kStaleHardwareMilliseconds = 5000;
        constexpr std::uint64_t kHiddenCheckMilliseconds = 15;
        constexpr int kThreadHideFromDebugger = 0x11;

        using QueryThread = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

        QueryThread NtQueryThread()
        {
            static const QueryThread query = reinterpret_cast<QueryThread>(
                reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
            return query;
        }

        bool HiddenFromDebugger(HANDLE thread)
        {
            const QueryThread query = NtQueryThread();
            BOOLEAN hidden = FALSE;
            return query && query(thread, kThreadHideFromDebugger, &hidden, sizeof(hidden), nullptr) >= 0 && hidden;
        }

        // Thread suspend/resume and the 64-bit context read/write go through
        // the direct system call when its service number resolved, and fall
        // back to the matching Win32 API when it did not. The Wow64 context
        // for a 32-bit target stays on Win32: it does not map cleanly to a
        // single documented syscall from a 64-bit host.
        bool SuspendThreadDirect(HANDLE thread)
        {
            if (SuspendThreadSysCall == 0)
                return ::SuspendThread(thread) != static_cast<DWORD>(-1);
            return NtSuspendThread_direct(thread, nullptr) >= 0;
        }

        bool ResumeThreadDirect(HANDLE thread)
        {
            if (ResumeThreadSysCall == 0)
                return ::ResumeThread(thread) != static_cast<DWORD>(-1);
            return NtResumeThread_direct(thread, nullptr) >= 0;
        }

        bool GetContextDirect(HANDLE thread, CONTEXT& ctx)
        {
            if (GetContextSysCall == 0)
                return ::GetThreadContext(thread, &ctx) != FALSE;
            return NtGetContextThread_direct(thread, &ctx) >= 0;
        }

        bool SetContextDirect(HANDLE thread, const CONTEXT& ctx)
        {
            if (SetContextSysCall == 0)
                return ::SetThreadContext(thread, &ctx) != FALSE;
            return NtSetContextThread_direct(thread, const_cast<CONTEXT*>(&ctx)) >= 0;
        }

        bool StillRunning(HANDLE thread)
        {
            return ::WaitForSingleObject(thread, 0) == WAIT_TIMEOUT;
        }

        std::uint64_t TebOf(HANDLE thread)
        {
            struct Basic
            {
                LONG ExitStatus;
                PVOID TebBaseAddress;
                PVOID UniqueProcess;
                PVOID UniqueThread;
                ULONG_PTR AffinityMask;
                LONG Priority;
                LONG BasePriority;
            } basic{};
            const QueryThread query = NtQueryThread();
            if (!query || query(thread, 0, &basic, sizeof(basic), nullptr) < 0)
                return 0;
            return reinterpret_cast<std::uint64_t>(basic.TebBaseAddress);
        }

        class ContextHolder
        {
        public:
            explicit ContextHolder(bool is64Bit) : m_is64(is64Bit) {}

            enum class Scope
            {
                Full,
                DebugRegisters,
                Control
            };

            bool Read(HANDLE thread, Scope scope = Scope::Full)
            {
                m_thread = thread;
                if (m_is64)
                {
                    std::memset(&m_native, 0, sizeof(m_native));
                    m_native.ContextFlags = scope == Scope::DebugRegisters ? CONTEXT_DEBUG_REGISTERS : scope == Scope::Control ? CONTEXT_CONTROL : CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
                    return GetContextDirect(thread, m_native);
                }
                std::memset(&m_wow, 0, sizeof(m_wow));
                m_wow.ContextFlags = scope == Scope::DebugRegisters ? WOW64_CONTEXT_DEBUG_REGISTERS : scope == Scope::Control ? WOW64_CONTEXT_CONTROL :
                                     WOW64_CONTEXT_FULL | WOW64_CONTEXT_DEBUG_REGISTERS;
                return ::Wow64GetThreadContext(thread, &m_wow) != FALSE;
            }

            bool Write() const
            {
                if (m_is64)
                    return SetContextDirect(m_thread, m_native);
                return ::Wow64SetThreadContext(m_thread, &m_wow) != FALSE;
            }

            std::uint64_t Ip() const { return m_is64 ? m_native.Rip : m_wow.Eip; }
            void SetIp(std::uint64_t value)
            {
                if (m_is64)
                    m_native.Rip = value;
                else
                    m_wow.Eip = static_cast<DWORD>(value);
            }
            std::uint64_t Sp() const { return m_is64 ? m_native.Rsp : m_wow.Esp; }
            std::uint64_t Flags() const { return m_is64 ? m_native.EFlags : m_wow.EFlags; }
            void SetFlags(std::uint64_t value)
            {
                if (m_is64)
                    m_native.EFlags = static_cast<DWORD>(value);
                else
                    m_wow.EFlags = static_cast<DWORD>(value);
            }
            std::uint64_t Dr6() const { return m_is64 ? m_native.Dr6 : m_wow.Dr6; }
            std::uint64_t Dr7() const { return m_is64 ? m_native.Dr7 : m_wow.Dr7; }
            void SetDebugRegisters(const std::uint64_t (&address)[4], std::uint64_t dr7)
            {
                if (m_is64)
                {
                    m_native.Dr0 = address[0];
                    m_native.Dr1 = address[1];
                    m_native.Dr2 = address[2];
                    m_native.Dr3 = address[3];
                    m_native.Dr7 = dr7;
                }
                else
                {
                    m_wow.Dr0 = static_cast<DWORD>(address[0]);
                    m_wow.Dr1 = static_cast<DWORD>(address[1]);
                    m_wow.Dr2 = static_cast<DWORD>(address[2]);
                    m_wow.Dr3 = static_cast<DWORD>(address[3]);
                    m_wow.Dr7 = static_cast<DWORD>(dr7);
                }
            }
            void ClearDr6()
            {
                if (m_is64)
                    m_native.Dr6 = 0;
                else
                    m_wow.Dr6 = 0;
            }

            Disasm::RegisterValues Values() const
            {
                Disasm::RegisterValues r;
                if (m_is64)
                {
                    r.Rax = m_native.Rax; r.Rbx = m_native.Rbx; r.Rcx = m_native.Rcx; r.Rdx = m_native.Rdx;
                    r.Rsi = m_native.Rsi; r.Rdi = m_native.Rdi; r.Rbp = m_native.Rbp; r.Rsp = m_native.Rsp;
                    r.R8 = m_native.R8; r.R9 = m_native.R9; r.R10 = m_native.R10; r.R11 = m_native.R11;
                    r.R12 = m_native.R12; r.R13 = m_native.R13; r.R14 = m_native.R14; r.R15 = m_native.R15;
                    r.Rip = m_native.Rip;
                    r.Flags = m_native.EFlags;
                    r.GsBase = TebOf(m_thread);
                }
                else
                {
                    r.Rax = m_wow.Eax; r.Rbx = m_wow.Ebx; r.Rcx = m_wow.Ecx; r.Rdx = m_wow.Edx;
                    r.Rsi = m_wow.Esi; r.Rdi = m_wow.Edi; r.Rbp = m_wow.Ebp; r.Rsp = m_wow.Esp;
                    r.Rip = m_wow.Eip;
                    r.Flags = m_wow.EFlags;
                    const std::uint64_t teb = TebOf(m_thread);
                    r.FsBase = teb ? teb + 0x2000 : 0;
                }
                return r;
            }

            void Apply(const Disasm::RegisterValues& r)
            {
                if (m_is64)
                {
                    m_native.Rax = r.Rax; m_native.Rbx = r.Rbx; m_native.Rcx = r.Rcx; m_native.Rdx = r.Rdx;
                    m_native.Rsi = r.Rsi; m_native.Rdi = r.Rdi; m_native.Rbp = r.Rbp; m_native.Rsp = r.Rsp;
                    m_native.R8 = r.R8; m_native.R9 = r.R9; m_native.R10 = r.R10; m_native.R11 = r.R11;
                    m_native.R12 = r.R12; m_native.R13 = r.R13; m_native.R14 = r.R14; m_native.R15 = r.R15;
                    m_native.Rip = r.Rip;
                    m_native.EFlags = static_cast<DWORD>(r.Flags);
                }
                else
                {
                    m_wow.Eax = static_cast<DWORD>(r.Rax); m_wow.Ebx = static_cast<DWORD>(r.Rbx);
                    m_wow.Ecx = static_cast<DWORD>(r.Rcx); m_wow.Edx = static_cast<DWORD>(r.Rdx);
                    m_wow.Esi = static_cast<DWORD>(r.Rsi); m_wow.Edi = static_cast<DWORD>(r.Rdi);
                    m_wow.Ebp = static_cast<DWORD>(r.Rbp); m_wow.Esp = static_cast<DWORD>(r.Rsp);
                    m_wow.Eip = static_cast<DWORD>(r.Rip);
                    m_wow.EFlags = static_cast<DWORD>(r.Flags);
                }
            }

        private:
            bool m_is64 = true;
            HANDLE m_thread = nullptr;
            alignas(16) CONTEXT m_native{};
            WOW64_CONTEXT m_wow{};
        };

        std::uint64_t LengthBits(int size)
        {
            switch (size)
            {
            case 2: return 1;
            case 8: return 2;
            case 4: return 3;
            default: return 0;
            }
        }

        std::uint64_t TypeBits(BreakpointType type)
        {
            switch (type)
            {
            case BreakpointType::Write: return 1;
            case BreakpointType::Access: return 3;
            default: return 0;
            }
        }

        std::string ErrorText(DWORD error)
        {
            return "Windows error " + std::to_string(error);
        }
    }

    struct Debugger::ThreadState
    {
        HANDLE Handle = nullptr;
        std::uint32_t Id = 0;
        std::uint64_t Reinsert = 0;
        bool Stepping = false;
        bool UntilReturn = false;
        bool ReturnPending = false;
        bool ResumeFlag = false;
        bool Hidden = false;
        bool Armed = false;
        std::uint64_t Steps = 0;
    };

    const char* WatchKindName(WatchKind kind)
    {
        switch (kind)
        {
        case WatchKind::Accesses: return "accesses";
        case WatchKind::Writes: return "writes to";
        case WatchKind::InstructionAccesses: return "addresses this instruction accesses";
        case WatchKind::Calls: return "calls";
        }
        return "?";
    }

    const char* BreakpointTypeName(BreakpointType type)
    {
        switch (type)
        {
        case BreakpointType::Write: return "Write";
        case BreakpointType::Access: return "Access";
        default: return "Execute";
        }
    }

    Debugger::Debugger() = default;

    Debugger::~Debugger()
    {
        Detach();
    }

    Status Debugger::Attach(std::shared_ptr<Target::Process> process)
    {
        if (m_attached)
            return Status::Fail(ErrorCode::DebugAlreadyAttached);
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        if (m_backend == Backend::Veh)
        {
            m_process = std::move(process);
            m_is64Bit = m_process->Is64Bit();
            m_disassembler = std::make_unique<Disasm::Disassembler>(m_is64Bit);
            return VehAttach();
        }
        if (m_thread.joinable())
            m_thread.join();
        m_process = std::move(process);
        m_is64Bit = m_process->Is64Bit();
        m_disassembler = std::make_unique<Disasm::Disassembler>(m_is64Bit);
        m_stop = false;
        m_initialBreakSeen = false;
        auto attached = std::make_shared<std::promise<Status>>();
        std::future<Status> result = attached->get_future();
        m_thread = std::thread(&Debugger::Loop, this, attached);
        Status status = result.get();
        if (!status)
        {
            if (m_thread.joinable())
                m_thread.join();
            m_process.reset();
            LOG_ERROR("Debugger", status.Text);
            return status;
        }
        LOG_SUCCESS("Debugger", "Attached to " + m_process->Name() + " (" + std::to_string(m_process->Id()) + ")");
        return status;
    }

    void Debugger::Detach()
    {
        if (m_backend == Backend::Veh)
        {
            VehDetach();
            return;
        }
        if (!m_thread.joinable())
            return;
        {
            std::lock_guard lock(m_lock);
            m_stop = true;
            m_resumeRequested = true;
            m_resumeMode = ResumeMode::Continue;
        }
        m_wake.notify_all();
        m_thread.join();
        std::lock_guard lock(m_lock);
        m_breakpoints.clear();
        for (auto& [id, watch] : m_watches)
            watch.Running = false;
        m_threads.clear();
        m_hiddenThreads = 0;
        m_mainThreadHidden = false;
        m_break = {};
        ++m_generation;
        m_process.reset();
    }

    Status Debugger::Post(std::function<Status()> work, unsigned timeoutMilliseconds)
    {
        if (!m_attached)
            return Status::Fail(ErrorCode::DebugNotAttached);
        if (m_backend == Backend::Veh)
            return Status::Fail(ErrorCode::DebugBreakpointFailed,
                "the in-process debugger only finds what accesses, writes or calls an address; code breakpoints and stepping need the normal debugger");
        if (std::this_thread::get_id() == m_thread.get_id())
            return work();
        auto done = std::make_shared<std::promise<Status>>();
        std::future<Status> result = done->get_future();
        {
            std::lock_guard lock(m_lock);
            m_tasks.push_back({ std::move(work), done });
        }
        m_wake.notify_all();
        if (result.wait_for(std::chrono::milliseconds(timeoutMilliseconds)) != std::future_status::ready)
            return Status::Fail(ErrorCode::DebugBreakpointFailed, "the debugger did not answer");
        return result.get();
    }

    void Debugger::RunTasks()
    {
        while (!m_tasks.empty())
        {
            Task task = std::move(m_tasks.front());
            m_tasks.pop_front();
            const Status status = task.Work();
            task.Done->set_value(status);
        }
    }

    int Debugger::TakeSlot() const
    {
        bool used[4] = {};
        for (const auto& [address, breakpoint] : m_breakpoints)
        {
            if (breakpoint.Hardware && breakpoint.Slot >= 0 && breakpoint.Slot < 4)
                used[breakpoint.Slot] = true;
        }
        for (int slot = 0; slot < 4; ++slot)
        {
            if (!used[slot])
                return slot;
        }
        return -1;
    }

    int Debugger::FreeHardwareSlots() const
    {
        std::lock_guard lock(m_lock);
        int used = 0;
        for (const auto& [address, breakpoint] : m_breakpoints)
            used += breakpoint.Hardware ? 1 : 0;
        return 4 - used;
    }

    void Debugger::AttributeLostHit(std::uint64_t ip, std::uint32_t threadId, const Disasm::RegisterValues& registers)
    {
        const auto previous = PreviousInstruction(ip);
        if (!previous)
            return;
        std::uint8_t bytes[16]{};
        if (m_process->Read(*previous, bytes, sizeof(bytes)) == 0)
            return;
        UnpatchLocked(*previous, bytes, sizeof(bytes));
        const Disasm::Instruction instruction = m_disassembler->Decode(*previous, bytes, sizeof(bytes), nullptr);
        if (!instruction.Memory.Present)
            return;
        const auto accessed = m_disassembler->EffectiveAddress(instruction, registers);
        if (!accessed)
            return;
        const std::uint64_t first = *accessed;
        const std::uint64_t last = first + static_cast<std::uint64_t>(std::max(1, instruction.Memory.Size));
        for (auto& [address, breakpoint] : m_breakpoints)
        {
            if (!breakpoint.Hardware || breakpoint.Type == BreakpointType::Execute || !breakpoint.WatchId)
                continue;
            if (breakpoint.Type == BreakpointType::Write && !instruction.Memory.Written)
                continue;
            if (first < breakpoint.Address + static_cast<std::uint64_t>(breakpoint.Size) && breakpoint.Address < last)
            {
                auto watch = m_watches.find(breakpoint.WatchId);
                if (watch != m_watches.end())
                    RecordHit(watch->second, *previous, breakpoint.Address, threadId, registers);
            }
        }
    }

    Status Debugger::ApplyHardware(std::uint32_t onlyThread)
    {
        m_lastHardwareTick = ::GetTickCount64();
        std::uint64_t addresses[4] = {};
        std::uint64_t dr7 = 0;
        for (const auto& [address, breakpoint] : m_breakpoints)
        {
            if (!breakpoint.Hardware || breakpoint.Slot < 0)
                continue;
            const int slot = breakpoint.Slot;
            addresses[slot] = breakpoint.Address;
            dr7 |= 1ull << (slot * 2);
            dr7 |= TypeBits(breakpoint.Type) << (16 + slot * 4);
            dr7 |= (breakpoint.Type == BreakpointType::Execute ? 0 : LengthBits(breakpoint.Size)) << (18 + slot * 4);
        }
        Status result;
        for (auto& [id, thread] : m_threads)
        {
            if (onlyThread && id != onlyThread)
                continue;
            if (thread->Hidden)
                continue;
            if (dr7 && HiddenFromDebugger(thread->Handle))
            {
                LeaveOut(*thread);
                continue;
            }
            const bool suspended = SuspendThreadDirect(thread->Handle);
            ContextHolder context(m_is64Bit);
            if (context.Read(thread->Handle, ContextHolder::Scope::DebugRegisters))
            {
                context.SetDebugRegisters(addresses, dr7);
                if (context.Write())
                    thread->Armed = dr7 != 0;
                else
                    result = Status::Fail(ErrorCode::DebugContextFailed, std::to_string(id));
            }
            if (suspended)
                ResumeThreadDirect(thread->Handle);
            if (thread->Armed && HiddenFromDebugger(thread->Handle))
                LeaveOut(*thread);
        }
        CountHidden();
        if (!result)
            LOG_DEBUG("Debugger", result.Text);
        return Status::Success();
    }

    void Debugger::LeaveOut(ThreadState& thread)
    {
        thread.Hidden = true;
        if (thread.Armed)
        {
            const bool suspended = SuspendThreadDirect(thread.Handle);
            ContextHolder context(m_is64Bit);
            if (context.Read(thread.Handle, ContextHolder::Scope::DebugRegisters))
            {
                const std::uint64_t none[4] = {};
                context.SetDebugRegisters(none, 0);
                context.Write();
            }
            if (suspended)
                ResumeThreadDirect(thread.Handle);
            thread.Armed = false;
        }
        LOG_WARN("Debugger", Format("Thread %u%s hides itself from debuggers. Hardware breakpoints leave it out, so its accesses are not seen.", static_cast<unsigned>(thread.Id),
            thread.Id == m_mainThread ? " (the main thread)" : ""));
    }

    void Debugger::CheckHiddenThreads(bool force)
    {
        const std::uint64_t now = ::GetTickCount64();
        if (!force && now - m_lastHiddenCheck < kHiddenCheckMilliseconds)
            return;
        m_lastHiddenCheck = now;
        bool changed = false;
        for (auto& [id, thread] : m_threads)
        {
            if (!thread->Hidden && thread->Armed && HiddenFromDebugger(thread->Handle))
            {
                LeaveOut(*thread);
                changed = true;
            }
        }
        if (changed || force)
            CountHidden();
    }

    void Debugger::CountHidden()
    {
        int hidden = 0;
        bool main = false;
        for (const auto& [id, thread] : m_threads)
        {
            if (!thread->Hidden || !StillRunning(thread->Handle))
                continue;
            ++hidden;
            main = main || id == m_mainThread;
        }
        if (hidden != m_hiddenThreads.load())
            ++m_generation;
        m_hiddenThreads = hidden;
        m_mainThreadHidden = main;
    }

    Status Debugger::InsertSoftware(Breakpoint& breakpoint)
    {
        if (breakpoint.Inserted)
            return Status::Success();
        std::uint8_t original = 0;
        if (!m_process->ReadExact(breakpoint.Address, &original, 1))
            return Status::Fail(ErrorCode::DebugBreakpointFailed, Hex(breakpoint.Address));
        if (original == 0xCC)
            return Status::Fail(ErrorCode::DebugBreakpointFailed, Hex(breakpoint.Address) + " holds an int3 already");
        const std::uint8_t trap = 0xCC;
        if (!m_process->Write(breakpoint.Address, &trap, 1))
            return Status::Fail(ErrorCode::DebugBreakpointFailed, Hex(breakpoint.Address));
        breakpoint.Original = original;
        breakpoint.Inserted = true;
        return Status::Success();
    }

    void Debugger::RemoveSoftware(Breakpoint& breakpoint)
    {
        if (!breakpoint.Inserted)
            return;
        m_process->Write(breakpoint.Address, &breakpoint.Original, 1);
        breakpoint.Inserted = false;
    }

    Status Debugger::AddBreakpointInternal(Breakpoint breakpoint)
    {
        if (m_breakpoints.count(breakpoint.Address))
            return Status::Fail(ErrorCode::DebugBreakpointFailed, Hex(breakpoint.Address) + " has a breakpoint already");
        if (breakpoint.Hardware)
        {
            breakpoint.Slot = TakeSlot();
            if (breakpoint.Slot < 0)
                return Status::Fail(ErrorCode::DebugNoFreeSlot);
            m_breakpoints[breakpoint.Address] = breakpoint;
            ApplyHardware();
        }
        else
        {
            Status inserted = InsertSoftware(breakpoint);
            if (!inserted)
                return inserted;
            m_breakpoints[breakpoint.Address] = breakpoint;
        }
        ++m_generation;
        return Status::Success();
    }

    void Debugger::RemoveBreakpointInternal(std::uint64_t address)
    {
        auto found = m_breakpoints.find(address);
        if (found == m_breakpoints.end())
            return;
        const bool hardware = found->second.Hardware;
        RemoveSoftware(found->second);
        m_breakpoints.erase(found);
        if (hardware)
            ApplyHardware();
        ++m_generation;
    }

    Status Debugger::StartWatch(WatchKind kind, std::uint64_t address, int size, int& id)
    {
        if (kind == WatchKind::Accesses || kind == WatchKind::Writes)
        {
            if (size != 1 && size != 2 && size != 4 && size != 8)
                size = size > 4 ? 8 : size > 2 ? 4 : size > 1 ? 2 : 1;
            if (!m_is64Bit && size == 8)
                size = 4;
            if (address % static_cast<std::uint64_t>(size) != 0)
                return Status::Fail(ErrorCode::DebugBadSize, std::to_string(size) + " at " + Hex(address));
        }
        if (m_backend == Backend::Veh)
            return VehStartWatch(kind, address, size, id);
        auto newId = std::make_shared<int>(0);
        const Status status = Post([this, newId, kind, address, size]() -> Status
            {
                Watch watch;
                watch.Id = m_nextWatchId++;
                watch.Kind = kind;
                watch.Address = address;
                watch.Size = size;
                Breakpoint breakpoint;
                breakpoint.Address = address;
                breakpoint.WatchId = watch.Id;
                breakpoint.Size = size;
                if (kind == WatchKind::Accesses || kind == WatchKind::Writes)
                {
                    breakpoint.Type = kind == WatchKind::Writes ? BreakpointType::Write : BreakpointType::Access;
                    breakpoint.Hardware = true;
                }
                else
                {
                    breakpoint.Type = BreakpointType::Execute;
                    breakpoint.Hardware = m_preferHardware && TakeSlot() >= 0;
                }
                Status added = AddBreakpointInternal(breakpoint);
                if (!added)
                    return added;
                watch.Running = true;
                watch.Slot = m_breakpoints[address].Slot;
                m_watches[watch.Id] = std::move(watch);
                *newId = m_nextWatchId - 1;
                return Status::Success();
            });
        if (status)
        {
            id = *newId;
            LOG_INFO("Debugger", Format("Watching what %s %s", WatchKindName(kind), Hex(address).c_str()));
        }
        return status;
    }

    void Debugger::StopWatch(int id)
    {
        if (m_backend == Backend::Veh)
        {
            VehSetActive(id, false);
            return;
        }
        Post([this, id]() -> Status
            {
                auto found = m_watches.find(id);
                if (found == m_watches.end() || !found->second.Running)
                    return Status::Success();
                found->second.Running = false;
                RemoveBreakpointInternal(found->second.Address);
                ++found->second.Generation;
                return Status::Success();
            });
        std::lock_guard lock(m_lock);
        if (auto found = m_watches.find(id); found != m_watches.end())
            found->second.Running = false;
    }

    void Debugger::RemoveWatch(int id)
    {
        StopWatch(id);
        std::lock_guard lock(m_lock);
        m_watches.erase(id);
        ++m_generation;
    }

    void Debugger::ClearWatch(int id)
    {
        std::lock_guard lock(m_lock);
        if (auto found = m_watches.find(id); found != m_watches.end())
        {
            found->second.Entries.clear();
            found->second.Hits = 0;
            ++found->second.Generation;
            if (m_backend == Backend::Veh && m_vehBlock && found->second.Slot >= 0 && found->second.Slot < Veh::kMaxWatches)
            {
                const std::uint64_t slotAddress = m_vehBlock + offsetof(Veh::Block, Watches) + static_cast<std::uint64_t>(found->second.Slot) * sizeof(Veh::Watch);
                std::uint32_t clear = 0;
                if (m_process->ReadExact(slotAddress + offsetof(Veh::Watch, Clear), &clear, sizeof(clear)))
                {
                    clear += 1;
                    m_process->Write(slotAddress + offsetof(Veh::Watch, Clear), &clear, sizeof(clear));
                }
            }
        }
    }

    bool Debugger::CopyWatch(int id, Watch& out) const
    {
        std::lock_guard lock(m_lock);
        const auto found = m_watches.find(id);
        if (found == m_watches.end())
            return false;
        if (out.Id == id && out.Generation == found->second.Generation)
            return true;
        out = found->second;
        return true;
    }

    std::vector<int> Debugger::WatchIds() const
    {
        std::lock_guard lock(m_lock);
        std::vector<int> ids;
        for (const auto& [id, watch] : m_watches)
            ids.push_back(id);
        return ids;
    }

    Status Debugger::SetBreakpoint(std::uint64_t address, BreakpointType type, int size, bool hardware)
    {
        if (m_backend == Backend::Veh)
        {
            if (type != BreakpointType::Execute)
                return Status::Fail(ErrorCode::DebugBreakpointFailed,
                    "the in-process debugger breaks on code only; use find out what writes or accesses for data");
            return VehSetBreakpoint(address);
        }
        if (type != BreakpointType::Execute)
            hardware = true;
        return Post([=, this]() -> Status
            {
                Breakpoint breakpoint;
                breakpoint.Address = address;
                breakpoint.Type = type;
                breakpoint.Size = size;
                breakpoint.Hardware = hardware || (m_preferHardware && TakeSlot() >= 0);
                return AddBreakpointInternal(breakpoint);
            });
    }

    Status Debugger::RemoveBreakpoint(std::uint64_t address)
    {
        if (m_backend == Backend::Veh)
        {
            VehRemoveBreakpoint(address);
            return Status::Success();
        }
        return Post([=, this]() -> Status
            {
                RemoveBreakpointInternal(address);
                return Status::Success();
            });
    }

    Status Debugger::ToggleBreakpoint(std::uint64_t address)
    {
        if (HasBreakpoint(address))
            return RemoveBreakpoint(address);
        return SetBreakpoint(address, BreakpointType::Execute, 1, false);
    }

    bool Debugger::HasBreakpoint(std::uint64_t address) const
    {
        std::lock_guard lock(m_lock);
        const auto found = m_breakpoints.find(address);
        return found != m_breakpoints.end() && found->second.WatchId == 0 && !found->second.Temporary;
    }

    std::vector<Breakpoint> Debugger::Breakpoints() const
    {
        std::lock_guard lock(m_lock);
        std::vector<Breakpoint> list;
        for (const auto& [address, breakpoint] : m_breakpoints)
        {
            if (breakpoint.WatchId == 0 && !breakpoint.Temporary)
                list.push_back(breakpoint);
        }
        return list;
    }

    bool Debugger::Broken() const
    {
        std::lock_guard lock(m_lock);
        return m_break.Broken;
    }

    BreakInfo Debugger::Break() const
    {
        std::lock_guard lock(m_lock);
        return m_break;
    }

    void Debugger::Resume(ResumeMode mode)
    {
        if (m_backend == Backend::Veh)
        {
            VehResume(mode);
            return;
        }
        {
            std::lock_guard lock(m_lock);
            if (!m_break.Broken)
                return;
            m_resumeRequested = true;
            m_resumeMode = mode;
        }
        m_wake.notify_all();
    }

    void Debugger::SetRegisters(const Disasm::RegisterValues& registers)
    {
        if (m_backend == Backend::Veh)
        {
            VehSetRegisters(registers);
            return;
        }
        std::lock_guard lock(m_lock);
        if (!m_break.Broken)
            return;
        m_newRegisters = registers;
        m_registersChanged = true;
        m_break.Registers = registers;
        m_break.Address = registers.Rip;
        ++m_generation;
    }

    Status Debugger::BreakNow()
    {
        if (!m_attached)
            return Status::Fail(ErrorCode::DebugNotAttached);
        if (m_backend == Backend::Veh)
        {
            if (m_vehBlock && m_process)
            {
                const std::uint32_t one = 1;
                m_process->Write(m_vehBlock + offsetof(Veh::Block, BreakNow), &one, sizeof(one));
            }
            return Status::Success();
        }
        {
            std::lock_guard lock(m_lock);
            m_breakRequested = true;
        }
        if (!::DebugBreakProcess(m_process->Handle()))
            return Status::Fail(ErrorCode::DebugBreakpointFailed, "DebugBreakProcess");
        return Status::Success();
    }

    void Debugger::Unpatch(std::uint64_t address, std::uint8_t* buffer, std::size_t size) const
    {
        std::lock_guard lock(m_lock);
        UnpatchLocked(address, buffer, size);
    }

    void Debugger::UnpatchLocked(std::uint64_t address, std::uint8_t* buffer, std::size_t size) const
    {
        for (const auto& [at, breakpoint] : m_breakpoints)
        {
            if (breakpoint.Inserted && !breakpoint.Hardware && at >= address && at < address + size)
                buffer[at - address] = breakpoint.Original;
        }
    }

    std::optional<std::uint64_t> Debugger::PreviousInstruction(std::uint64_t address) const
    {
        constexpr std::uint64_t kLookBack = 48;
        if (address < kLookBack)
            return std::nullopt;
        std::uint8_t buffer[kLookBack + 16]{};
        const std::uint64_t start = address - kLookBack;
        if (m_process->Read(start, buffer, sizeof(buffer)) == 0)
            return std::nullopt;
        UnpatchLocked(start, buffer, sizeof(buffer));
        return m_disassembler->PreviousStart(buffer, sizeof(buffer), start, address);
    }

    void Debugger::RecordHit(Watch& watch, std::uint64_t instruction, std::uint64_t accessed, std::uint32_t threadId, const Disasm::RegisterValues& registers)
    {
        ++watch.Hits;
        ++watch.Generation;
        const bool byAccessed = watch.Kind == WatchKind::InstructionAccesses;
        for (WatchEntry& entry : watch.Entries)
        {
            if ((byAccessed ? entry.Accessed == accessed : entry.Instruction == instruction))
            {
                ++entry.Count;
                entry.Registers = registers;
                entry.ThreadId = threadId;
                entry.Accessed = accessed;
                return;
            }
        }
        if (watch.Entries.size() >= kMaxWatchEntries)
            return;
        WatchEntry entry;
        entry.Instruction = instruction;
        entry.Accessed = accessed;
        entry.Count = 1;
        entry.ThreadId = threadId;
        entry.Registers = registers;
        watch.Entries.push_back(entry);
        ++m_generation;
    }

    void Debugger::EnterBreak(std::uint32_t threadId, const std::string& reason, std::uint64_t breakpointAddress)
    {
        auto threadFound = m_threads.find(threadId);
        if (threadFound == m_threads.end())
            return;
        ThreadState& thread = *threadFound->second;
        ContextHolder context(m_is64Bit);
        if (!context.Read(thread.Handle))
            return;
        m_break.Broken = true;
        m_break.ThreadId = threadId;
        m_break.Address = context.Ip();
        m_break.BreakpointAddress = breakpointAddress;
        m_break.Registers = context.Values();
        m_break.Reason = reason;
        m_break.Is64Bit = m_is64Bit;
        m_break.Serial = ++m_breakSerial;
        m_resumeRequested = false;
        m_registersChanged = false;
        ++m_generation;
        LOG_INFO("Debugger", reason + " at " + Hex(m_break.Address) + " in thread " + std::to_string(threadId));
    }

    unsigned long Debugger::HandleException(void* rawEvent, bool& enterBreak, std::string& reason, std::uint64_t& breakpointAddress)
    {
        const DEBUG_EVENT& event = *static_cast<const DEBUG_EVENT*>(rawEvent);
        const EXCEPTION_RECORD& record = event.u.Exception.ExceptionRecord;
        const DWORD code = record.ExceptionCode;
        const std::uint64_t at = reinterpret_cast<std::uint64_t>(record.ExceptionAddress);

        auto threadFound = m_threads.find(event.dwThreadId);
        if (threadFound == m_threads.end())
        {
            HANDLE handle = ::OpenThread(THREAD_ALL_ACCESS, FALSE, event.dwThreadId);
            if (!handle)
                return DBG_EXCEPTION_NOT_HANDLED;
            auto state = std::make_unique<ThreadState>();
            state->Handle = handle;
            state->Id = event.dwThreadId;
            threadFound = m_threads.emplace(event.dwThreadId, std::move(state)).first;
        }
        ThreadState& thread = *threadFound->second;

        if (code == EXCEPTION_BREAKPOINT || code == kWx86Breakpoint)
        {
            auto found = m_breakpoints.find(at);
            if (found != m_breakpoints.end() && !found->second.Hardware && found->second.Inserted)
            {
                Breakpoint& breakpoint = found->second;
                ContextHolder context(m_is64Bit);
                if (!context.Read(thread.Handle))
                    return DBG_CONTINUE;
                context.SetIp(at);
                RemoveSoftware(breakpoint);
                if (breakpoint.WatchId)
                {
                    auto watch = m_watches.find(breakpoint.WatchId);
                    if (watch != m_watches.end())
                    {
                        const Disasm::RegisterValues values = context.Values();
                        if (watch->second.Kind == WatchKind::Calls)
                        {
                            const auto returnAddress = m_process->ReadPointer(values.Rsp);
                            if (returnAddress)
                            {
                                const auto caller = PreviousInstruction(*returnAddress);
                                RecordHit(watch->second, caller.value_or(*returnAddress), *returnAddress, event.dwThreadId, values);
                            }
                        }
                        else
                        {
                            const Disasm::Instruction instruction = m_disassembler->Read(*m_process, at, nullptr);
                            const auto accessed = m_disassembler->EffectiveAddress(instruction, values);
                            RecordHit(watch->second, at, accessed.value_or(0), event.dwThreadId, values);
                        }
                    }
                    thread.Reinsert = at;
                    context.SetFlags(context.Flags() | kTrapFlag);
                    context.Write();
                    return DBG_CONTINUE;
                }
                if (breakpoint.Temporary)
                {
                    m_breakpoints.erase(found);
                    context.Write();
                    enterBreak = true;
                    reason = "Stepped over";
                    return DBG_CONTINUE;
                }
                thread.Reinsert = at;
                context.Write();
                enterBreak = true;
                reason = "Breakpoint";
                breakpointAddress = at;
                return DBG_CONTINUE;
            }
            if (!m_initialBreakSeen)
            {
                m_initialBreakSeen = true;
                return DBG_CONTINUE;
            }
            std::uint8_t current = 0xCC;
            if (m_process->ReadExact(at, &current, 1) && current != 0xCC)
            {
                ContextHolder context(m_is64Bit);
                if (context.Read(thread.Handle))
                {
                    context.SetIp(at);
                    context.Write();
                }
                LOG_DEBUG("Debugger", "Breakpoint at " + Hex(at) + " was already removed; the thread runs on");
                return DBG_CONTINUE;
            }
            if (m_breakRequested)
            {
                m_breakRequested = false;
                enterBreak = true;
                reason = "Break";
                return DBG_CONTINUE;
            }
            return DBG_EXCEPTION_NOT_HANDLED;
        }

        if (code == EXCEPTION_SINGLE_STEP || code == kWx86SingleStep)
        {
            ContextHolder context(m_is64Bit);
            if (!context.Read(thread.Handle))
            {
                const bool hardwareActive = std::any_of(m_breakpoints.begin(), m_breakpoints.end(), [](const auto& item) { return item.second.Hardware; });
                if (!hardwareActive && ::GetTickCount64() - m_lastHardwareTick >= kStaleHardwareMilliseconds && !thread.Stepping && !thread.Reinsert)
                    return DBG_EXCEPTION_NOT_HANDLED;
                LOG_WARN("Debugger", Format("Could not read thread %u at a single step (%s); the step is kept from the process", static_cast<unsigned>(event.dwThreadId),
                    ErrorText(::GetLastError()).c_str()));
                return DBG_CONTINUE;
            }
            const std::uint64_t dr6 = context.Dr6();
            bool handled = false;
            bool write = false;

            for (int slot = 0; slot < 4; ++slot)
            {
                if (!(dr6 & (1ull << slot)))
                    continue;
                handled = true;
                auto found = std::find_if(m_breakpoints.begin(), m_breakpoints.end(), [&](const auto& item) { return item.second.Hardware && item.second.Slot == slot; });
                if (found == m_breakpoints.end())
                    continue;
                const Breakpoint& breakpoint = found->second;
                const Disasm::RegisterValues values = context.Values();
                if (breakpoint.Type == BreakpointType::Execute)
                {
                    context.SetFlags(context.Flags() | kResumeFlag);
                    write = true;
                    if (breakpoint.WatchId)
                    {
                        auto watch = m_watches.find(breakpoint.WatchId);
                        if (watch != m_watches.end())
                        {
                            if (watch->second.Kind == WatchKind::Calls)
                            {
                                if (const auto returnAddress = m_process->ReadPointer(values.Rsp))
                                {
                                    const auto caller = PreviousInstruction(*returnAddress);
                                    RecordHit(watch->second, caller.value_or(*returnAddress), *returnAddress, event.dwThreadId, values);
                                }
                            }
                            else
                            {
                                const Disasm::Instruction instruction = m_disassembler->Read(*m_process, context.Ip(), nullptr);
                                const auto accessed = m_disassembler->EffectiveAddress(instruction, values);
                                RecordHit(watch->second, context.Ip(), accessed.value_or(0), event.dwThreadId, values);
                            }
                        }
                    }
                    else
                    {
                        enterBreak = true;
                        reason = "Hardware breakpoint";
                        breakpointAddress = breakpoint.Address;
                        thread.ResumeFlag = true;
                    }
                }
                else
                {
                    const auto instruction = PreviousInstruction(context.Ip());
                    if (breakpoint.WatchId)
                    {
                        auto watch = m_watches.find(breakpoint.WatchId);
                        if (watch != m_watches.end())
                            RecordHit(watch->second, instruction.value_or(context.Ip()), breakpoint.Address, event.dwThreadId, values);
                    }
                    else
                    {
                        enterBreak = true;
                        reason = std::string(BreakpointTypeName(breakpoint.Type)) + " breakpoint on " + Hex(breakpoint.Address);
                        breakpointAddress = breakpoint.Address;
                    }
                }
            }

            if (thread.Reinsert)
            {
                auto found = m_breakpoints.find(thread.Reinsert);
                if (found != m_breakpoints.end() && !found->second.Hardware)
                    InsertSoftware(found->second);
                thread.Reinsert = 0;
                if (!thread.Stepping)
                    context.SetFlags(context.Flags() & ~kTrapFlag);
                handled = true;
                write = true;
            }

            if (thread.Stepping)
            {
                handled = true;
                if (thread.UntilReturn)
                {
                    if (thread.ReturnPending || ++thread.Steps > 2000000)
                    {
                        thread.Stepping = false;
                        thread.UntilReturn = false;
                        thread.ReturnPending = false;
                        enterBreak = true;
                        reason = "Returned";
                    }
                    else
                    {
                        const Disasm::Instruction next = m_disassembler->Read(*m_process, context.Ip(), nullptr);
                        thread.ReturnPending = next.Kind == Disasm::Flow::Return;
                        context.SetFlags(context.Flags() | kTrapFlag);
                        write = true;
                    }
                }
                else
                {
                    thread.Stepping = false;
                    enterBreak = true;
                    reason = "Step";
                }
            }

            if (handled)
            {
                context.ClearDr6();
                write = true;
            }
            if (!handled)
            {
                const bool hardwareActive = std::any_of(m_breakpoints.begin(), m_breakpoints.end(), [](const auto& item) { return item.second.Hardware; });
                if (hardwareActive || ::GetTickCount64() - m_lastHardwareTick < kStaleHardwareMilliseconds)
                {
                    handled = true;
                    AttributeLostHit(context.Ip(), event.dwThreadId, context.Values());
                    context.ClearDr6();
                    write = true;
                }
                else
                {
                    LOG_DEBUG("Debugger", Format("Single step at %s is not the debugger's (dr6 %llX); the process handles it", Hex(context.Ip()).c_str(),
                        static_cast<unsigned long long>(dr6)));
                }
            }
            if (write)
                context.Write();
            return handled ? DBG_CONTINUE : DBG_EXCEPTION_NOT_HANDLED;
        }

        if (event.u.Exception.dwFirstChance == 0)
            LOG_WARN("Debugger", "Second chance exception " + Hex(code, 8) + " at " + Hex(at));
        return DBG_EXCEPTION_NOT_HANDLED;
    }

    void Debugger::Loop(std::shared_ptr<std::promise<Status>> attached)
    {
        const std::uint32_t id = m_process->Id();
        if (!::DebugActiveProcess(id))
        {
            attached->set_value(Status::Fail(ErrorCode::DebugAttachFailed, ErrorText(::GetLastError())));
            return;
        }
        ::DebugSetProcessKillOnExit(FALSE);
        m_attached = true;
        ++m_generation;
        attached->set_value(Status::Success());

        bool exited = false;
        DEBUG_EVENT event{};
        while (!m_stop)
        {
            {
                std::unique_lock lock(m_lock);
                RunTasks();
                if (std::any_of(m_breakpoints.begin(), m_breakpoints.end(), [](const auto& item) { return item.second.Hardware; }))
                    CheckHiddenThreads(false);
            }
            if (!::WaitForDebugEventEx(&event, 10))
                continue;

            DWORD status = DBG_CONTINUE;
            bool enterBreak = false;
            std::string reason;
            std::uint64_t breakpointAddress = 0;
            std::unique_lock lock(m_lock);
            switch (event.dwDebugEventCode)
            {
            case CREATE_PROCESS_DEBUG_EVENT:
            {
                if (event.u.CreateProcessInfo.hFile)
                    ::CloseHandle(event.u.CreateProcessInfo.hFile);
                auto state = std::make_unique<ThreadState>();
                state->Handle = event.u.CreateProcessInfo.hThread;
                state->Id = event.dwThreadId;
                m_mainThread = event.dwThreadId;
                m_threads[event.dwThreadId] = std::move(state);
                break;
            }
            case CREATE_THREAD_DEBUG_EVENT:
            {
                auto state = std::make_unique<ThreadState>();
                state->Handle = event.u.CreateThread.hThread;
                state->Id = event.dwThreadId;
                m_threads[event.dwThreadId] = std::move(state);
                if (std::any_of(m_breakpoints.begin(), m_breakpoints.end(), [](const auto& item) { return item.second.Hardware; }))
                    ApplyHardware(event.dwThreadId);
                break;
            }
            case EXIT_THREAD_DEBUG_EVENT:
                m_threads.erase(event.dwThreadId);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (event.u.LoadDll.hFile)
                    ::CloseHandle(event.u.LoadDll.hFile);
                break;
            case EXCEPTION_DEBUG_EVENT:
                status = HandleException(&event, enterBreak, reason, breakpointAddress);
                if (status == DBG_EXCEPTION_NOT_HANDLED)
                {
                    const EXCEPTION_RECORD& record = event.u.Exception.ExceptionRecord;
                    LOG_DEBUG("Debugger", Format("Exception %08X at %s in thread %u passed to the process (%s chance)", static_cast<unsigned>(record.ExceptionCode),
                        Hex(reinterpret_cast<std::uint64_t>(record.ExceptionAddress)).c_str(), static_cast<unsigned>(event.dwThreadId), event.u.Exception.dwFirstChance ? "first" : "second"));
                }
                break;
            case OUTPUT_DEBUG_STRING_EVENT:
                if (m_logDebugStrings)
                {
                    const auto& info = event.u.DebugString;
                    const std::size_t length = std::min<std::size_t>(info.nDebugStringLength, 4096);
                    if (info.fUnicode)
                    {
                        std::wstring text(length, L'\0');
                        if (m_process->Read(reinterpret_cast<std::uint64_t>(info.lpDebugStringData), text.data(), length * 2))
                            LOG_INFO("Target", Trim(WideToUtf8(text.c_str())));
                    }
                    else
                    {
                        std::string text(length, '\0');
                        if (m_process->Read(reinterpret_cast<std::uint64_t>(info.lpDebugStringData), text.data(), length))
                            LOG_INFO("Target", Trim(text.c_str()));
                    }
                }
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                exited = true;
                m_stop = true;
                LOG_WARN("Debugger", ErrorMessage(ErrorCode::DebugTargetExited) + " Exit code " + Hex(event.u.ExitProcess.dwExitCode, 8) + ".");
                break;
            default:
                break;
            }

            if (enterBreak && !m_stop)
            {
                EnterBreak(event.dwThreadId, reason, breakpointAddress);
                while (m_break.Broken && !m_resumeRequested && !m_stop)
                {
                    m_wake.wait_for(lock, std::chrono::milliseconds(50));
                    RunTasks();
                }
                auto threadFound = m_threads.find(event.dwThreadId);
                if (threadFound != m_threads.end() && m_break.Broken)
                {
                    ThreadState& thread = *threadFound->second;
                    ContextHolder context(m_is64Bit);
                    if (context.Read(thread.Handle))
                    {
                        if (m_registersChanged)
                            context.Apply(m_newRegisters);
                        std::uint64_t flags = context.Flags();
                        if (thread.ResumeFlag)
                        {
                            flags |= kResumeFlag;
                            thread.ResumeFlag = false;
                        }
                        ResumeMode mode = m_stop ? ResumeMode::Continue : m_resumeMode;
                        if (mode == ResumeMode::StepOver)
                        {
                            const Disasm::Instruction current = m_disassembler->Read(*m_process, context.Ip(), nullptr);
                            if (current.Kind == Disasm::Flow::Call && current.Valid)
                            {
                                Breakpoint temporary;
                                temporary.Address = current.Next();
                                temporary.Temporary = true;
                                if (!m_breakpoints.count(temporary.Address) && InsertSoftware(temporary))
                                    m_breakpoints[temporary.Address] = temporary;
                                else
                                    mode = ResumeMode::StepInto;
                            }
                            else
                            {
                                mode = ResumeMode::StepInto;
                            }
                        }
                        if (mode == ResumeMode::StepInto || mode == ResumeMode::RunToReturn)
                        {
                            thread.Stepping = true;
                            thread.UntilReturn = mode == ResumeMode::RunToReturn;
                            thread.ReturnPending = false;
                            thread.Steps = 0;
                            flags |= kTrapFlag;
                        }
                        if (thread.Reinsert)
                            flags |= kTrapFlag;
                        context.SetFlags(flags);
                        context.Write();
                    }
                }
                m_break.Broken = false;
                m_registersChanged = false;
                ++m_generation;
            }
            lock.unlock();
            ::ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
        }

        std::unique_lock lock(m_lock);
        RunTasks();
        if (!exited)
            CleanUp();
        for (auto& [address, breakpoint] : m_breakpoints)
            breakpoint.Inserted = false;
        m_attached = false;
        ++m_generation;
        lock.unlock();
        if (!exited && !::DebugActiveProcessStop(id))
            LOG_ERROR("Debugger", ErrorMessage(ErrorCode::DebugDetachFailed, ErrorText(::GetLastError())));
        else
            LOG_INFO("Debugger", "Detached");
    }

    void Debugger::CleanUp()
    {
        for (auto& [address, breakpoint] : m_breakpoints)
            RemoveSoftware(breakpoint);
        std::map<std::uint64_t, Breakpoint> kept;
        m_breakpoints.swap(kept);
        ApplyHardware();
        m_breakpoints.swap(kept);
        for (auto& [id, thread] : m_threads)
        {
            if (thread->Reinsert || thread->Stepping)
            {
                ContextHolder context(m_is64Bit);
                if (context.Read(thread->Handle, ContextHolder::Scope::Control))
                {
                    context.SetFlags(context.Flags() & ~kTrapFlag);
                    context.Write();
                }
            }
        }
    }

    std::wstring Debugger::AgentPath() const
    {
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        return (std::filesystem::path(self).parent_path() / L"ManifoldAgent.dll").wstring();
    }

    int Debugger::VehFreeSlot() const
    {
        bool used[Veh::kMaxWatches] = {};
        for (const auto& [id, watch] : m_watches)
        {
            if (watch.Running && watch.Slot >= 0 && watch.Slot < Veh::kMaxWatches)
                used[watch.Slot] = true;
        }
        for (const auto& [address, breakpoint] : m_breakpoints)
        {
            if (breakpoint.Slot >= 0 && breakpoint.Slot < Veh::kMaxWatches)
                used[breakpoint.Slot] = true;
        }
        for (int slot : m_vehTempSlots)
        {
            if (slot >= 0 && slot < Veh::kMaxWatches)
                used[slot] = true;
        }
        for (int slot = 0; slot < Veh::kMaxWatches; ++slot)
        {
            if (!used[slot])
                return slot;
        }
        return -1;
    }

    void Debugger::VehWriteSlot(int slot, std::uint32_t mode, std::uint64_t address, int size, bool active)
    {
        if (!m_vehBlock || !m_process)
            return;
        auto shared = std::make_unique<Veh::Watch>();
        std::memset(shared.get(), 0, sizeof(Veh::Watch));
        shared->Active = active ? 1u : 0u;
        shared->Mode = mode;
        shared->Address = address;
        shared->Size = static_cast<std::uint32_t>(size);
        shared->Slot = static_cast<std::uint32_t>(slot);
        const std::uint64_t slotAddress = m_vehBlock + offsetof(Veh::Block, Watches) + static_cast<std::uint64_t>(slot) * sizeof(Veh::Watch);
        m_process->Write(slotAddress, shared.get(), sizeof(Veh::Watch));
        VehBump();
    }

    Status Debugger::VehSetBreakpoint(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_attached || !m_vehBlock)
            return Status::Fail(ErrorCode::DebugNotAttached);
        if (m_breakpoints.count(address))
            return Status::Success();
        const int slot = VehFreeSlot();
        if (slot < 0)
            return Status::Fail(ErrorCode::DebugNoFreeSlot);
        VehWriteSlot(slot, Veh::ModeBreak, address, 1, true);
        Breakpoint breakpoint;
        breakpoint.Address = address;
        breakpoint.Type = BreakpointType::Execute;
        breakpoint.Hardware = true;
        breakpoint.Slot = slot;
        breakpoint.Inserted = false;
        m_breakpoints[address] = breakpoint;
        ++m_generation;
        LOG_INFO("Debugger", "Breakpoint at " + Hex(address) + " (in-process, debug register " + std::to_string(slot) + ")");
        return Status::Success();
    }

    void Debugger::VehRemoveBreakpoint(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        auto found = m_breakpoints.find(address);
        if (found == m_breakpoints.end())
            return;
        const int slot = found->second.Slot;
        if (slot >= 0 && slot < Veh::kMaxWatches)
            VehWriteSlot(slot, Veh::ModeBreak, 0, 1, false);
        m_breakpoints.erase(found);
        ++m_generation;
    }

    std::vector<std::uint64_t> Debugger::VehStepTargets(const Disasm::Instruction& ins, const Disasm::RegisterValues& regs, ResumeMode mode) const
    {
        std::vector<std::uint64_t> out;
        const auto readPointer = [&](std::uint64_t address) -> std::uint64_t { return m_process->ReadPointer(address).value_or(0); };
        switch (ins.Kind)
        {
        case Disasm::Flow::Return:
            out.push_back(readPointer(regs.Rsp));
            break;
        case Disasm::Flow::Call:
            if (mode == ResumeMode::StepOver || mode == ResumeMode::RunToReturn)
                out.push_back(ins.Next());
            else if (ins.Target)
                out.push_back(*ins.Target);
            else
            {
                if (ins.Memory.Present)
                    if (const auto address = m_disassembler->EffectiveAddress(ins, regs))
                        out.push_back(readPointer(*address));
                out.push_back(ins.Next());
            }
            break;
        case Disasm::Flow::Jump:
            if (ins.Target)
                out.push_back(*ins.Target);
            else if (ins.Memory.Present)
            {
                if (const auto address = m_disassembler->EffectiveAddress(ins, regs))
                    out.push_back(readPointer(*address));
            }
            else
                out.push_back(ins.Next());
            break;
        case Disasm::Flow::ConditionalJump:
            out.push_back(ins.Next());
            if (ins.Target)
                out.push_back(*ins.Target);
            break;
        default:
            out.push_back(ins.Next());
            break;
        }
        return out;
    }

    void Debugger::VehClearTempSlots()
    {
        for (int slot : m_vehTempSlots)
            if (slot >= 0 && slot < Veh::kMaxWatches)
                VehWriteSlot(slot, Veh::ModeBreak, 0, 1, false);
        m_vehTempSlots.clear();
    }

    void Debugger::VehResume(ResumeMode mode)
    {
        std::lock_guard lock(m_lock);
        if (!m_break.Broken || !m_vehBlock || !m_process)
            return;
        VehClearTempSlots();
        if (mode != ResumeMode::Continue)
        {
            const Disasm::Instruction current = m_disassembler->Read(*m_process, m_break.Address, nullptr);
            std::vector<std::uint64_t> targets = VehStepTargets(current, m_break.Registers, mode);
            for (std::uint64_t target : targets)
            {
                if (!target || target == m_break.Address)
                    continue;
                const int slot = VehFreeSlot();
                if (slot < 0)
                    break;
                m_vehTempSlots.push_back(slot);
                VehWriteSlot(slot, Veh::ModeBreak, target, 1, true);
            }
            if (m_vehTempSlots.empty())
            {
                const int slot = VehFreeSlot();
                if (slot >= 0)
                {
                    m_vehTempSlots.push_back(slot);
                    VehWriteSlot(slot, Veh::ModeBreak, current.Next(), 1, true);
                }
            }
        }
        m_break.Broken = false;
        ++m_generation;
        const std::uint32_t cmd = Veh::ResumeContinue;
        m_process->Write(m_vehBlock + offsetof(Veh::Block, ResumeCmd), &cmd, sizeof(cmd));
    }

    void Debugger::VehSetRegisters(const Disasm::RegisterValues& registers)
    {
        std::lock_guard lock(m_lock);
        if (!m_break.Broken || !m_vehBlock || !m_process)
            return;
        Veh::Registers regs{};
        regs.Rax = registers.Rax; regs.Rbx = registers.Rbx; regs.Rcx = registers.Rcx; regs.Rdx = registers.Rdx;
        regs.Rsi = registers.Rsi; regs.Rdi = registers.Rdi; regs.Rbp = registers.Rbp; regs.Rsp = registers.Rsp;
        regs.R8 = registers.R8; regs.R9 = registers.R9; regs.R10 = registers.R10; regs.R11 = registers.R11;
        regs.R12 = registers.R12; regs.R13 = registers.R13; regs.R14 = registers.R14; regs.R15 = registers.R15;
        regs.Rip = registers.Rip; regs.Flags = registers.Flags; regs.GsBase = registers.GsBase;
        m_process->Write(m_vehBlock + offsetof(Veh::Block, NewRegs), &regs, sizeof(regs));
        const std::uint32_t dirty = 1;
        m_process->Write(m_vehBlock + offsetof(Veh::Block, RegsDirty), &dirty, sizeof(dirty));
        m_break.Registers = registers;
        m_break.Address = registers.Rip;
        ++m_generation;
    }

    void Debugger::VehCheckBreak()
    {
        if (!m_vehBlock || !m_process)
            return;
        struct { std::uint32_t Broken, ResumeCmd, BrokenThread, RegsDirty, BreakNow, Pad3; std::uint64_t BreakSerial, BreakAddress; Veh::Registers Regs; } head{};
        if (!m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, Broken), &head, sizeof(head)))
            return;
        std::lock_guard lock(m_lock);
        if (head.Broken && head.BreakSerial != m_vehBreakSerial)
        {
            m_vehBreakSerial = head.BreakSerial;
            VehClearTempSlots();
            Disasm::RegisterValues regs;
            regs.Rax = head.Regs.Rax; regs.Rbx = head.Regs.Rbx; regs.Rcx = head.Regs.Rcx; regs.Rdx = head.Regs.Rdx;
            regs.Rsi = head.Regs.Rsi; regs.Rdi = head.Regs.Rdi; regs.Rbp = head.Regs.Rbp; regs.Rsp = head.Regs.Rsp;
            regs.R8 = head.Regs.R8; regs.R9 = head.Regs.R9; regs.R10 = head.Regs.R10; regs.R11 = head.Regs.R11;
            regs.R12 = head.Regs.R12; regs.R13 = head.Regs.R13; regs.R14 = head.Regs.R14; regs.R15 = head.Regs.R15;
            regs.Rip = head.Regs.Rip; regs.Flags = head.Regs.Flags; regs.GsBase = head.Regs.GsBase;
            m_break = {};
            m_break.Broken = true;
            m_break.ThreadId = head.BrokenThread;
            m_break.Address = head.BreakAddress;
            m_break.BreakpointAddress = head.BreakAddress;
            m_break.Registers = regs;
            m_break.Reason = "Breakpoint";
            m_break.Serial = ++m_breakSerial;
            m_break.Is64Bit = m_is64Bit;
            ++m_generation;
        }
        else if (!head.Broken && m_break.Broken)
        {
            m_break.Broken = false;
            ++m_generation;
        }
    }

    Status Debugger::VehAttach()
    {
        if (!m_is64Bit)
            return Status::Fail(ErrorCode::DebugAttachFailed, "the in-process debugger supports 64-bit targets only");
        const std::wstring agent = AgentPath();
        std::error_code ignore;
        if (!std::filesystem::exists(agent, ignore))
            return Status::Fail(ErrorCode::InjectFailed, "ManifoldAgent.dll was not found next to the program");

        Status status;
        const std::uint64_t block = m_process->Allocate(sizeof(Veh::Block), PAGE_READWRITE, 0, status);
        if (!block)
            return status;
        auto shared = std::make_unique<Veh::Block>();
        std::memset(shared.get(), 0, sizeof(Veh::Block));
        shared->Magic = Veh::kMagic;
        shared->Version = Veh::kVersion;
        if (Status written = m_process->Write(block, shared.get(), sizeof(Veh::Block)); !written)
        {
            m_process->Free(block);
            return written;
        }
        if (Status injected = m_process->InjectLibrary(agent); !injected)
        {
            m_process->Free(block);
            return injected;
        }
        const std::uint64_t start = m_process->FindExport("ManifoldAgent.dll", "ManifoldAgentStart");
        if (!start)
        {
            m_process->Free(block);
            return Status::Fail(ErrorCode::InjectFailed, "ManifoldAgentStart was not found in the agent");
        }
        if (Status thread = m_process->CreateThread(start, block, true); !thread)
        {
            m_process->Free(block);
            return thread;
        }
        std::uint32_t running = 0;
        for (int tries = 0; tries < 50 && !running; ++tries)
        {
            if (!m_process->ReadExact(block + offsetof(Veh::Block, Running), &running, sizeof(running)))
                break;
            if (!running)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!running)
        {
            std::uint32_t lastError = 0;
            m_process->ReadExact(block + offsetof(Veh::Block, LastError), &lastError, sizeof(lastError));
            m_process->Free(block);
            return Status::Fail(ErrorCode::InjectFailed, "the agent did not start" + (lastError ? " (" + ErrorText(lastError) + ")" : std::string()));
        }
        {
            std::lock_guard lock(m_lock);
            m_vehBlock = block;
            m_watches.clear();
            m_breakpoints.clear();
            m_break = {};
            m_vehBreakSerial = 0;
            m_vehTempSlots.clear();
            m_hiddenThreads = 0;
            m_mainThreadHidden = false;
        }
        m_vehStop = false;
        m_attached = true;
        ++m_generation;
        m_vehThread = std::thread(&Debugger::VehPollLoop, this);
        LOG_SUCCESS("Debugger", "In-process debugger running in " + m_process->Name() + " (" + std::to_string(m_process->Id()) + ")");
        return Status::Success();
    }

    void Debugger::VehDetach()
    {
        if (!m_attached)
            return;
        m_vehStop = true;
        if (m_vehThread.joinable())
            m_vehThread.join();
        if (m_vehBlock && m_process)
        {
            const std::uint32_t stop = 1;
            m_process->Write(m_vehBlock + offsetof(Veh::Block, Stop), &stop, sizeof(stop));
            std::uint32_t running = 1;
            for (int tries = 0; tries < 100 && running; ++tries)
            {
                if (!m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, Running), &running, sizeof(running)))
                {
                    running = 0;
                    break;
                }
                if (running)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!running)
                m_process->Free(m_vehBlock);
        }
        {
            std::lock_guard lock(m_lock);
            m_vehBlock = 0;
            for (auto& [id, watch] : m_watches)
                watch.Running = false;
            m_breakpoints.clear();
            m_break = {};
            m_vehTempSlots.clear();
            m_attached = false;
            ++m_generation;
        }
        m_process.reset();
        LOG_INFO("Debugger", "Detached");
    }

    Status Debugger::VehStartWatch(WatchKind kind, std::uint64_t address, int size, int& id)
    {
        std::lock_guard lock(m_lock);
        if (!m_attached || !m_vehBlock)
            return Status::Fail(ErrorCode::DebugNotAttached);
        const int slot = VehFreeSlot();
        if (slot < 0)
            return Status::Fail(ErrorCode::DebugNoFreeSlot);
        auto shared = std::make_unique<Veh::Watch>();
        std::memset(shared.get(), 0, sizeof(Veh::Watch));
        shared->Active = 1;
        shared->Mode = static_cast<std::uint32_t>(kind);
        shared->Address = address;
        shared->Size = static_cast<std::uint32_t>(size);
        shared->Slot = static_cast<std::uint32_t>(slot);
        const std::uint64_t slotAddress = m_vehBlock + offsetof(Veh::Block, Watches) + static_cast<std::uint64_t>(slot) * sizeof(Veh::Watch);
        if (Status written = m_process->Write(slotAddress, shared.get(), sizeof(Veh::Watch)); !written)
            return written;
        VehBump();
        Watch watch;
        watch.Id = m_nextWatchId++;
        watch.Kind = kind;
        watch.Address = address;
        watch.Size = size;
        watch.Running = true;
        watch.Slot = slot;
        id = watch.Id;
        m_watches[watch.Id] = std::move(watch);
        ++m_generation;
        LOG_INFO("Debugger", Format("Watching what %s %s (in-process, debug register %d)", WatchKindName(kind), Hex(address).c_str(), slot));
        return Status::Success();
    }

    void Debugger::VehBump()
    {
        if (!m_vehBlock || !m_process)
            return;
        std::uint32_t command = 0;
        if (m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, Command), &command, sizeof(command)))
        {
            command += 1;
            m_process->Write(m_vehBlock + offsetof(Veh::Block, Command), &command, sizeof(command));
        }
    }

    void Debugger::VehSetActive(int id, bool active)
    {
        std::lock_guard lock(m_lock);
        auto found = m_watches.find(id);
        if (found == m_watches.end())
            return;
        if (m_vehBlock && m_process && found->second.Slot >= 0 && found->second.Slot < Veh::kMaxWatches)
        {
            const std::uint64_t slotAddress = m_vehBlock + offsetof(Veh::Block, Watches) + static_cast<std::uint64_t>(found->second.Slot) * sizeof(Veh::Watch);
            std::uint32_t value = active ? 1u : 0u;
            m_process->Write(slotAddress + offsetof(Veh::Watch, Active), &value, sizeof(value));
            VehBump();
        }
        if (!active)
        {
            found->second.Running = false;
            ++found->second.Generation;
            ++m_generation;
        }
    }

    void Debugger::VehPollLoop()
    {
        struct Target
        {
            int Id;
            int Slot;
            WatchKind Kind;
            std::uint64_t Address;
        };
        auto shared = std::make_unique<Veh::Watch>();
        while (!m_vehStop)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (!m_process || !m_process->Alive())
            {
                std::lock_guard lock(m_lock);
                for (auto& [id, watch] : m_watches)
                    watch.Running = false;
                continue;
            }
            VehCheckBreak();
            std::vector<Target> targets;
            {
                std::lock_guard lock(m_lock);
                for (const auto& [id, watch] : m_watches)
                {
                    if (watch.Running && watch.Slot >= 0 && watch.Slot < Veh::kMaxWatches)
                        targets.push_back({ id, watch.Slot, watch.Kind, watch.Address });
                }
            }
            for (const Target& target : targets)
            {
                if (m_vehStop)
                    break;
                const std::uint64_t slotAddress = m_vehBlock + offsetof(Veh::Block, Watches) + static_cast<std::uint64_t>(target.Slot) * sizeof(Veh::Watch);
                const std::size_t head = offsetof(Veh::Watch, Entries);
                if (!m_process->ReadExact(slotAddress, shared.get(), head))
                    continue;
                std::uint64_t entryCount = shared->EntryCount;
                if (entryCount > static_cast<std::uint64_t>(Veh::kMaxEntries))
                    entryCount = Veh::kMaxEntries;
                if (entryCount > 0 && !m_process->ReadExact(slotAddress + head, shared->Entries, static_cast<std::size_t>(entryCount) * sizeof(Veh::Entry)))
                    continue;
                std::uint64_t hits = 0;
                std::vector<WatchEntry> entries;
                entries.reserve(static_cast<std::size_t>(entryCount));
                for (std::uint64_t i = 0; i < entryCount; ++i)
                {
                    const Veh::Entry& raw = shared->Entries[i];
                    Disasm::RegisterValues regs;
                    regs.Rax = raw.Regs.Rax; regs.Rbx = raw.Regs.Rbx; regs.Rcx = raw.Regs.Rcx; regs.Rdx = raw.Regs.Rdx;
                    regs.Rsi = raw.Regs.Rsi; regs.Rdi = raw.Regs.Rdi; regs.Rbp = raw.Regs.Rbp; regs.Rsp = raw.Regs.Rsp;
                    regs.R8 = raw.Regs.R8; regs.R9 = raw.Regs.R9; regs.R10 = raw.Regs.R10; regs.R11 = raw.Regs.R11;
                    regs.R12 = raw.Regs.R12; regs.R13 = raw.Regs.R13; regs.R14 = raw.Regs.R14; regs.R15 = raw.Regs.R15;
                    regs.Rip = raw.Regs.Rip; regs.Flags = raw.Regs.Flags; regs.GsBase = raw.Regs.GsBase;
                    WatchEntry entry;
                    entry.Count = raw.Count;
                    entry.ThreadId = raw.ThreadId;
                    entry.Registers = regs;
                    if (target.Kind == WatchKind::Accesses || target.Kind == WatchKind::Writes)
                    {
                        const auto previous = PreviousInstruction(raw.Instruction);
                        entry.Instruction = previous.value_or(raw.Instruction);
                        entry.Accessed = target.Address;
                    }
                    else if (target.Kind == WatchKind::Calls)
                    {
                        const auto ret = m_process->ReadPointer(regs.Rsp);
                        const auto caller = ret ? PreviousInstruction(*ret) : std::nullopt;
                        entry.Instruction = caller.value_or(ret.value_or(raw.Instruction));
                        entry.Accessed = ret.value_or(0);
                    }
                    else
                    {
                        entry.Instruction = raw.Instruction;
                        const Disasm::Instruction decoded = m_disassembler->Read(*m_process, raw.Instruction, nullptr);
                        const auto accessed = m_disassembler->EffectiveAddress(decoded, regs);
                        entry.Accessed = accessed.value_or(0);
                    }
                    hits += raw.Count;
                    entries.push_back(entry);
                }
                std::lock_guard lock(m_lock);
                auto found = m_watches.find(target.Id);
                if (found == m_watches.end() || !found->second.Running)
                    continue;
                if (found->second.Hits != hits || found->second.Entries.size() != entries.size())
                    ++found->second.Generation;
                found->second.Hits = hits;
                found->second.Entries = std::move(entries);
            }
        }
    }
}
