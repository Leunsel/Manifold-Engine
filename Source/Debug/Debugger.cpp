#include "Debug/Debugger.h"

#include "Core/Handle.h"
#include "Core/MainThread.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Debug/VehProtocol.h"
#include "Syscalls/Syscall.h"
#include "Target/Process.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
        constexpr std::uint64_t kMaxReturnSteps = 2000000;
        constexpr std::uint64_t kStaleHardwareMilliseconds = 5000;
        constexpr std::uint64_t kAttachSettleMilliseconds = 2000;
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
                                     WOW64_CONTEXT_FULL | WOW64_CONTEXT_DEBUG_REGISTERS | WOW64_CONTEXT_EXTENDED_REGISTERS;
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

            VectorRegisters Vectors() const
            {
                VectorRegisters v;
                if (m_is64)
                {
                    if ((m_native.ContextFlags & CONTEXT_FLOATING_POINT) != CONTEXT_FLOATING_POINT)
                        return v;
                    for (int i = 0; i < 16; ++i)
                        std::memcpy(v.Xmm[static_cast<std::size_t>(i)].data(), &m_native.FltSave.XmmRegisters[i], 16);
                    v.Mxcsr = m_native.MxCsr;
                    v.Count = 16;
                    return v;
                }
                if ((m_wow.ContextFlags & WOW64_CONTEXT_EXTENDED_REGISTERS) != WOW64_CONTEXT_EXTENDED_REGISTERS)
                    return v;
                for (int i = 0; i < 8; ++i)
                    std::memcpy(v.Xmm[static_cast<std::size_t>(i)].data(), m_wow.ExtendedRegisters + kFxsaveXmm + i * 16, 16);
                std::memcpy(&v.Mxcsr, m_wow.ExtendedRegisters + kFxsaveMxcsr, sizeof(v.Mxcsr));
                v.Count = 8;
                return v;
            }

            void ApplyVectors(const VectorRegisters& v)
            {
                if (m_is64)
                {
                    if ((m_native.ContextFlags & CONTEXT_FLOATING_POINT) != CONTEXT_FLOATING_POINT)
                        return;
                    for (int i = 0; i < 16; ++i)
                        std::memcpy(&m_native.FltSave.XmmRegisters[i], v.Xmm[static_cast<std::size_t>(i)].data(), 16);
                    m_native.MxCsr = v.Mxcsr;
                    m_native.FltSave.MxCsr = v.Mxcsr;
                    return;
                }
                if ((m_wow.ContextFlags & WOW64_CONTEXT_EXTENDED_REGISTERS) != WOW64_CONTEXT_EXTENDED_REGISTERS)
                    return;
                for (int i = 0; i < 8; ++i)
                    std::memcpy(m_wow.ExtendedRegisters + kFxsaveXmm + i * 16, v.Xmm[static_cast<std::size_t>(i)].data(), 16);
                std::memcpy(m_wow.ExtendedRegisters + kFxsaveMxcsr, &v.Mxcsr, sizeof(v.Mxcsr));
            }

            bool HasFpuArea() const
            {
                return m_is64 ? (m_native.ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT
                              : (m_wow.ContextFlags & WOW64_CONTEXT_EXTENDED_REGISTERS) == WOW64_CONTEXT_EXTENDED_REGISTERS;
            }

            FpuRegisters Fpu() const
            {
                FpuRegisters f;
                if (!HasFpuArea())
                    return f;
                const std::uint8_t* area = m_is64 ? reinterpret_cast<const std::uint8_t*>(&m_native.FltSave) : m_wow.ExtendedRegisters;
                std::memcpy(f.Area.data(), area, FpuRegisters::kBytes);
                f.Valid = true;
                return f;
            }

            void ApplyFpu(const FpuRegisters& f)
            {
                if (!HasFpuArea() || !f.Valid)
                    return;
                std::uint8_t* area = m_is64 ? reinterpret_cast<std::uint8_t*>(&m_native.FltSave) : m_wow.ExtendedRegisters;
                std::memcpy(area, f.Area.data(), 24);
                std::memcpy(area + 32, f.Area.data() + 32, FpuRegisters::kBytes - 32);
            }

        private:
            static constexpr int kFxsaveMxcsr = 24;
            static constexpr int kFxsaveXmm = 160;
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

        bool Touches(const Breakpoint& breakpoint, std::uint64_t first, std::uint64_t last, bool written)
        {
            if (!breakpoint.Hardware || breakpoint.Type == BreakpointType::Execute || (breakpoint.Type == BreakpointType::Write && !written))
                return false;
            return first < breakpoint.Address + static_cast<std::uint64_t>(breakpoint.Size) && breakpoint.Address < last;
        }

        Disasm::RegisterValues FromAgent(const Veh::Registers& raw, bool is64Bit)
        {
            Disasm::RegisterValues r;
            r.Rax = raw.Rax; r.Rbx = raw.Rbx; r.Rcx = raw.Rcx; r.Rdx = raw.Rdx;
            r.Rsi = raw.Rsi; r.Rdi = raw.Rdi; r.Rbp = raw.Rbp; r.Rsp = raw.Rsp;
            r.R8 = raw.R8; r.R9 = raw.R9; r.R10 = raw.R10; r.R11 = raw.R11;
            r.R12 = raw.R12; r.R13 = raw.R13; r.R14 = raw.R14; r.R15 = raw.R15;
            r.Rip = raw.Rip;
            r.Flags = raw.Flags;
            if (is64Bit)
                r.GsBase = raw.GsBase;
            else
                r.FsBase = raw.GsBase;
            return r;
        }

        Veh::Registers ToAgent(const Disasm::RegisterValues& r, bool is64Bit)
        {
            Veh::Registers raw{};
            raw.Rax = r.Rax; raw.Rbx = r.Rbx; raw.Rcx = r.Rcx; raw.Rdx = r.Rdx;
            raw.Rsi = r.Rsi; raw.Rdi = r.Rdi; raw.Rbp = r.Rbp; raw.Rsp = r.Rsp;
            raw.R8 = r.R8; raw.R9 = r.R9; raw.R10 = r.R10; raw.R11 = r.R11;
            raw.R12 = r.R12; raw.R13 = r.R13; raw.R14 = r.R14; raw.R15 = r.R15;
            raw.Rip = r.Rip;
            raw.Flags = r.Flags;
            raw.GsBase = is64Bit ? r.GsBase : r.FsBase;
            return raw;
        }
    }

    struct Debugger::ThreadState
    {
        HANDLE Handle = nullptr;
        UniqueHandle Owned;
        std::uint32_t Id = 0;
        std::uint64_t Reinsert = 0;
        std::uint64_t StepInstruction = 0;
        std::uint64_t StepFirst = 0;
        std::uint64_t StepLast = 0;
        bool StepWrites = false;
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

    std::uint16_t FpuRegisters::Control() const
    {
        return static_cast<std::uint16_t>(Area[0] | (Area[1] << 8));
    }

    std::uint16_t FpuRegisters::Status() const
    {
        return static_cast<std::uint16_t>(Area[2] | (Area[3] << 8));
    }

    std::uint8_t FpuRegisters::Tags() const
    {
        return Area[4];
    }

    std::uint16_t FpuRegisters::Opcode() const
    {
        return static_cast<std::uint16_t>(Area[6] | (Area[7] << 8));
    }

    void FpuRegisters::SetControl(std::uint16_t value)
    {
        Area[0] = static_cast<std::uint8_t>(value);
        Area[1] = static_cast<std::uint8_t>(value >> 8);
    }

    void FpuRegisters::SetStatus(std::uint16_t value)
    {
        Area[2] = static_cast<std::uint8_t>(value);
        Area[3] = static_cast<std::uint8_t>(value >> 8);
    }

    void FpuRegisters::SetTags(std::uint8_t value)
    {
        Area[4] = value;
    }

    double ExtendedToDouble(const std::uint8_t* tenBytes)
    {
        std::uint64_t mantissa = 0;
        std::memcpy(&mantissa, tenBytes, 8);
        const unsigned top = static_cast<unsigned>(tenBytes[8] | (tenBytes[9] << 8));
        const bool negative = (top & 0x8000) != 0;
        const int exponent = static_cast<int>(top & 0x7FFF);
        double value = 0.0;
        if (exponent == 0x7FFF)
            value = (mantissa << 1) == 0 ? HUGE_VAL : std::nan("");
        else if (exponent != 0 || mantissa != 0)
            value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
        return negative ? -value : value;
    }

    void DoubleToExtended(double value, std::uint8_t* tenBytes)
    {
        std::uint64_t mantissa = 0;
        unsigned top = std::signbit(value) ? 0x8000u : 0u;
        if (std::isnan(value))
        {
            mantissa = 0xC000000000000000ull;
            top |= 0x7FFF;
        }
        else if (std::isinf(value))
        {
            mantissa = 0x8000000000000000ull;
            top |= 0x7FFF;
        }
        else if (value != 0.0)
        {
            int exponent = 0;
            const double fraction = std::frexp(std::fabs(value), &exponent);
            mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
            top |= static_cast<unsigned>(exponent - 1 + 16383) & 0x7FFF;
        }
        std::memcpy(tenBytes, &mantissa, 8);
        tenBytes[8] = static_cast<std::uint8_t>(top);
        tenBytes[9] = static_cast<std::uint8_t>(top >> 8);
    }

    std::string ExtendedText(const std::uint8_t* tenBytes)
    {
        const double value = ExtendedToDouble(tenBytes);
        if (std::isnan(value))
            return "NaN";
        if (std::isinf(value))
            return value < 0 ? "-Inf" : "+Inf";
        return Format("%.17g", value);
    }

    std::string ExceptionName(std::uint32_t code)
    {
        switch (code)
        {
        case 0xC0000005: return "Access violation";
        case 0xC0000006: return "In-page error";
        case 0xC000001D: return "Illegal instruction";
        case 0xC0000025: return "Non-continuable exception";
        case 0xC000008C: return "Array bounds exceeded";
        case 0xC000008D: return "Float denormal operand";
        case 0xC000008E: return "Float divide by zero";
        case 0xC000008F: return "Float inexact result";
        case 0xC0000090: return "Float invalid operation";
        case 0xC0000091: return "Float overflow";
        case 0xC0000092: return "Float stack check";
        case 0xC0000093: return "Float underflow";
        case 0xC0000094: return "Integer divide by zero";
        case 0xC0000095: return "Integer overflow";
        case 0xC0000096: return "Privileged instruction";
        case 0xC00000FD: return "Stack overflow";
        case 0xC0000374: return "Heap corruption";
        case 0xC0000409: return "Stack buffer overrun";
        case 0xC0000420: return "Assertion failure";
        case 0x80000001: return "Guard page";
        case 0x80000002: return "Misaligned data";
        case 0x80000003: return "Breakpoint instruction";
        case 0x4000001F: return "Breakpoint instruction";
        case 0xE06D7363: return "C++ exception";
        case 0xE0434352: return ".NET exception";
        case 0xE0434F4D: return ".NET exception";
        default: return "Exception " + Hex(code, 8);
        }
    }

    std::string DescribeException(const BreakException& exception)
    {
        std::string text = ExceptionName(exception.Code);
        if ((exception.Code == 0xC0000005 || exception.Code == 0xC0000006) && exception.ParamCount >= 2)
        {
            const std::uint64_t kind = exception.Params[0];
            text += kind == 0 ? " reading " : kind == 1 ? " writing " : kind == 8 ? " executing " : " at ";
            text += Hex(exception.Params[1]);
        }
        if (!exception.FirstChance)
            text += ", the program did not handle it";
        return text;
    }

    bool UnexpectedException(std::uint32_t code)
    {
        switch (code)
        {
        case 0x40010006:
        case 0x4001000A:
        case 0x406D1388:
        case 0x40010005:
        case 0x40010008:
        case 0x80000004:
        case 0x4000001E:
            return false;
        default:
            return true;
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
        LOG_SUCCESS("Debugger", "Attached to " + m_process->Name() + " (" + ToText(m_process->Id()) + ")");
        return status;
    }

    void Debugger::Detach()
    {
        m_stepping = false;
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
            if (!breakpoint.WatchId || !Touches(breakpoint, first, last, instruction.Memory.Written))
                continue;
            auto watch = m_watches.find(breakpoint.WatchId);
            if (watch != m_watches.end())
                RecordHit(watch->second, *previous, breakpoint.Address, threadId, registers);
        }
    }

    void Debugger::PlanStepAccess(ThreadState& thread, const Disasm::RegisterValues& registers)
    {
        thread.StepInstruction = 0;
        if (!thread.Armed || thread.Hidden)
            return;
        if (std::none_of(m_breakpoints.begin(), m_breakpoints.end(), [](const auto& item) { return item.second.Hardware && item.second.Type != BreakpointType::Execute; }))
            return;
        std::uint8_t bytes[16]{};
        const std::size_t got = m_process->Read(registers.Rip, bytes, sizeof(bytes));
        if (got == 0)
            return;
        UnpatchLocked(registers.Rip, bytes, got);
        const Disasm::Instruction instruction = m_disassembler->Decode(registers.Rip, bytes, got, nullptr);
        if (!instruction.Valid || instruction.IsNop || !instruction.Memory.Present || (!instruction.Memory.Read && !instruction.Memory.Written))
            return;
        const auto accessed = m_disassembler->EffectiveAddress(instruction, registers);
        if (!accessed)
            return;
        thread.StepInstruction = registers.Rip;
        thread.StepFirst = *accessed;
        thread.StepLast = *accessed + static_cast<std::uint64_t>(std::max(1, instruction.Memory.Size));
        thread.StepWrites = instruction.Memory.Written;
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

    Status Debugger::SetBreakpoint(std::uint64_t address, BreakpointType type, int size, bool hardware, BreakAction action)
    {
        if (type != BreakpointType::Execute)
        {
            if (size != 1 && size != 2 && size != 4 && size != 8)
                size = size > 4 ? 8 : size > 2 ? 4 : size > 1 ? 2 : 1;
            if (!m_is64Bit && size == 8)
                size = 4;
            if (address % static_cast<std::uint64_t>(size) != 0)
                return Status::Fail(ErrorCode::DebugBadSize, std::to_string(size) + " at " + Hex(address));
        }
        else
        {
            size = 1;
        }
        if (m_backend == Backend::Veh)
            return VehSetBreakpoint(address, type, size, action);
        if (type != BreakpointType::Execute)
            hardware = true;
        return Post([this, address, type, size, hardware, action]() -> Status
            {
                Breakpoint breakpoint;
                breakpoint.Address = address;
                breakpoint.Type = type;
                breakpoint.Size = size;
                breakpoint.Hardware = hardware || (m_preferHardware && TakeSlot() >= 0);
                breakpoint.Action = action;
                return AddBreakpointInternal(breakpoint);
            });
    }

    Status Debugger::SetBreakpointAction(std::uint64_t address, BreakAction action)
    {
        if (m_backend == Backend::Veh)
        {
            std::lock_guard lock(m_lock);
            auto found = m_breakpoints.find(address);
            if (found == m_breakpoints.end() || found->second.WatchId || found->second.Temporary)
                return Status::Fail(ErrorCode::DebugBreakpointFailed, Hex(address) + " has no breakpoint");
            found->second.Action = action;
            VehWriteAction(found->second.Slot, &found->second.Action, 0, true);
            ++m_generation;
            return Status::Success();
        }
        return Post([this, address, action]() -> Status
            {
                auto found = m_breakpoints.find(address);
                if (found == m_breakpoints.end() || found->second.WatchId || found->second.Temporary)
                    return Status::Fail(ErrorCode::DebugBreakpointFailed, Hex(address) + " has no breakpoint");
                found->second.Action = action;
                ++m_generation;
                return Status::Success();
            });
    }

    Debugger::HitOutcome Debugger::RunBreakAction(Breakpoint& breakpoint, Disasm::RegisterValues& registers, std::uint32_t threadId, bool& changed)
    {
        changed = false;
        if (breakpoint.Action.Plain())
        {
            ++breakpoint.Hits;
            return HitOutcome::Stop;
        }
        if (!ConditionHolds(breakpoint.Action, registers, threadId, *m_process))
            return HitOutcome::Skip;
        ++breakpoint.Hits;
        changed = ApplyChanges(breakpoint.Action, registers, threadId, *m_process);
        return breakpoint.Action.Break ? HitOutcome::Stop : HitOutcome::Run;
    }

    void Debugger::DataHit(Breakpoint& breakpoint, std::uint64_t instruction, std::uint32_t threadId, Disasm::RegisterValues& registers, bool& changed, bool& enterBreak,
        std::string& reason, std::uint64_t& breakpointAddress)
    {
        if (breakpoint.WatchId)
        {
            auto watch = m_watches.find(breakpoint.WatchId);
            if (watch != m_watches.end())
                RecordHit(watch->second, instruction, breakpoint.Address, threadId, registers);
            return;
        }
        bool applied = false;
        const HitOutcome outcome = RunBreakAction(breakpoint, registers, threadId, applied);
        changed = changed || applied;
        if (outcome == HitOutcome::Stop && !enterBreak)
        {
            enterBreak = true;
            reason = std::string(BreakpointTypeName(breakpoint.Type)) + " breakpoint on " + Hex(breakpoint.Address);
            breakpointAddress = breakpoint.Address;
        }
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
            m_stepping = mode != ResumeMode::Continue;
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

    void Debugger::SetVectors(const VectorRegisters& vectors)
    {
        std::lock_guard lock(m_lock);
        if (!m_break.Broken || m_break.Vectors.Count == 0)
            return;
        if (m_backend == Backend::Veh)
        {
            if (!m_vehBlock || !m_process)
                return;
            Veh::Vectors raw{};
            for (int i = 0; i < 16; ++i)
                std::memcpy(raw.Xmm[i], vectors.Xmm[static_cast<std::size_t>(i)].data(), 16);
            raw.Mxcsr = vectors.Mxcsr;
            raw.Count = static_cast<std::uint32_t>(m_break.Vectors.Count);
            m_process->Write(m_vehBlock + offsetof(Veh::Block, NewXmm), &raw, sizeof(raw));
            const std::uint32_t dirty = 1;
            m_process->Write(m_vehBlock + offsetof(Veh::Block, XmmDirty), &dirty, sizeof(dirty));
        }
        else
        {
            m_newVectors = vectors;
            m_newVectors.Count = m_break.Vectors.Count;
            m_vectorsChanged = true;
        }
        const int count = m_break.Vectors.Count;
        m_break.Vectors = vectors;
        m_break.Vectors.Count = count;
        ++m_generation;
    }

    void Debugger::SetFpu(const FpuRegisters& fpu)
    {
        std::lock_guard lock(m_lock);
        if (!m_break.Broken || !m_break.Fpu.Valid)
            return;
        if (m_backend == Backend::Veh)
        {
            if (!m_vehBlock || !m_process)
                return;
            m_process->Write(m_vehBlock + offsetof(Veh::Block, NewFpu), fpu.Area.data(), FpuRegisters::kBytes);
            const std::uint32_t dirty = 1;
            m_process->Write(m_vehBlock + offsetof(Veh::Block, FpuDirty), &dirty, sizeof(dirty));
        }
        else
        {
            m_newFpu = fpu;
            m_newFpu.Valid = true;
            m_fpuChanged = true;
        }
        m_break.Fpu = fpu;
        m_break.Fpu.Valid = true;
        ++m_generation;
    }

    void Debugger::ResumeHandled()
    {
        {
            std::lock_guard lock(m_lock);
            if (!m_break.Broken || !m_break.Exception.Raised)
                return;
            if (m_backend == Backend::Veh)
            {
                if (m_vehBlock && m_process)
                {
                    const std::uint32_t one = 1;
                    m_process->Write(m_vehBlock + offsetof(Veh::Block, ExceptionHandled), &one, sizeof(one));
                }
            }
            else
            {
                m_exceptionHandled = true;
            }
        }
        Resume(ResumeMode::Continue);
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

    std::optional<Debugger::HitPlace> Debugger::ExecuteHitPlace(WatchKind kind, std::uint64_t at, const Disasm::RegisterValues& registers) const
    {
        if (kind == WatchKind::Calls)
        {
            const auto returnAddress = m_process->ReadPointer(registers.Rsp);
            if (!returnAddress)
                return std::nullopt;
            return HitPlace{ PreviousInstruction(*returnAddress).value_or(*returnAddress), *returnAddress };
        }
        const Disasm::Instruction instruction = m_disassembler->Read(*m_process, at, nullptr);
        return HitPlace{ at, m_disassembler->EffectiveAddress(instruction, registers).value_or(0) };
    }

    bool Debugger::AnyHardware() const
    {
        return std::any_of(m_breakpoints.begin(), m_breakpoints.end(), [](const auto& item) { return item.second.Hardware; });
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
        m_stepping = false;
        m_break.Broken = true;
        m_break.ThreadId = threadId;
        m_break.Address = context.Ip();
        m_break.BreakpointAddress = breakpointAddress;
        m_break.Registers = context.Values();
        m_break.Vectors = context.Vectors();
        m_break.Fpu = context.Fpu();
        m_break.Exception = m_cause;
        m_cause = {};
        m_break.Reason = reason;
        m_break.Is64Bit = m_is64Bit;
        m_break.Serial = ++m_breakSerial;
        m_resumeRequested = false;
        m_registersChanged = false;
        m_vectorsChanged = false;
        m_fpuChanged = false;
        m_exceptionHandled = false;
        ++m_generation;
        LOG_INFO("Debugger", reason + " at " + Hex(m_break.Address) + " in thread " + std::to_string(threadId));
    }

    std::uint32_t Debugger::Wow64BreakThread(std::uint32_t breakIn) const
    {
        const auto runs32 = [this](std::uint32_t id)
            {
                const auto found = m_threads.find(id);
                if (found == m_threads.end() || !StillRunning(found->second->Handle))
                    return false;
                ContextHolder context(false);
                return context.Read(found->second->Handle, ContextHolder::Scope::Control) && context.Ip() != 0;
            };
        if (m_mainThread != breakIn && runs32(m_mainThread))
            return m_mainThread;
        for (const auto& [id, thread] : m_threads)
        {
            if (id != breakIn && runs32(id))
                return id;
        }
        return breakIn;
    }

    unsigned long Debugger::HandleException(void* rawEvent, bool& enterBreak, std::string& reason, std::uint64_t& breakpointAddress, std::uint32_t& breakThread)
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
            state->Owned = UniqueHandle(handle);
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
                        if (const auto place = ExecuteHitPlace(watch->second.Kind, at, values))
                            RecordHit(watch->second, place->Instruction, place->Accessed, event.dwThreadId, values);
                    }
                    thread.Reinsert = at;
                    PlanStepAccess(thread, context.Values());
                    context.SetFlags(context.Flags() | kTrapFlag);
                    context.Write();
                    return DBG_CONTINUE;
                }
                if (breakpoint.Temporary)
                {
                    if ((breakpoint.OwnerThread && breakpoint.OwnerThread != event.dwThreadId) || (breakpoint.AboveStack && context.Values().Rsp <= breakpoint.AboveStack))
                    {
                        thread.Reinsert = at;
                        PlanStepAccess(thread, context.Values());
                        context.SetFlags(context.Flags() | kTrapFlag);
                        context.Write();
                        return DBG_CONTINUE;
                    }
                    const bool stepOut = breakpoint.AboveStack != 0;
                    m_breakpoints.erase(found);
                    context.Write();
                    enterBreak = true;
                    reason = stepOut ? "Returned to the caller" : "Stepped over";
                    return DBG_CONTINUE;
                }
                Disasm::RegisterValues values = context.Values();
                bool changed = false;
                const HitOutcome outcome = RunBreakAction(breakpoint, values, event.dwThreadId, changed);
                if (changed)
                    context.Apply(values);
                thread.Reinsert = at;
                if (outcome != HitOutcome::Stop)
                {
                    PlanStepAccess(thread, values);
                    context.SetFlags(context.Flags() | kTrapFlag);
                    context.Write();
                    return DBG_CONTINUE;
                }
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
                if (!m_is64Bit)
                    breakThread = Wow64BreakThread(event.dwThreadId);
                return DBG_CONTINUE;
            }
        }
        else if (code == EXCEPTION_SINGLE_STEP || code == kWx86SingleStep)
        {
            ContextHolder context(m_is64Bit);
            if (!context.Read(thread.Handle))
            {
                const bool hardwareActive = AnyHardware();
                if (!hardwareActive && ::GetTickCount64() - m_lastHardwareTick >= kStaleHardwareMilliseconds && !thread.Stepping && !thread.Reinsert)
                    return DBG_EXCEPTION_NOT_HANDLED;
                LOG_WARN("Debugger", Format("Could not read thread %u at a single step (%s); the step is kept from the process", static_cast<unsigned>(event.dwThreadId),
                    SystemErrorText(::GetLastError()).c_str()));
                return DBG_CONTINUE;
            }
            const std::uint64_t dr6 = thread.Reinsert && m_simulateMissingStepBits ? context.Dr6() & ~0xFull : context.Dr6();
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
                Breakpoint& breakpoint = found->second;
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
                            if (const auto place = ExecuteHitPlace(watch->second.Kind, context.Ip(), values))
                                RecordHit(watch->second, place->Instruction, place->Accessed, event.dwThreadId, values);
                        }
                    }
                    else
                    {
                        Disasm::RegisterValues current = values;
                        bool changed = false;
                        const HitOutcome outcome = RunBreakAction(breakpoint, current, event.dwThreadId, changed);
                        if (changed)
                        {
                            context.Apply(current);
                            context.SetFlags(context.Flags() | kResumeFlag);
                        }
                        if (outcome == HitOutcome::Stop && !enterBreak)
                        {
                            enterBreak = true;
                            reason = "Hardware breakpoint";
                            breakpointAddress = breakpoint.Address;
                            thread.ResumeFlag = true;
                        }
                    }
                }
                else
                {
                    Disasm::RegisterValues current = values;
                    bool changed = false;
                    DataHit(breakpoint, PreviousInstruction(context.Ip()).value_or(context.Ip()), event.dwThreadId, current, changed, enterBreak, reason, breakpointAddress);
                    if (changed)
                    {
                        context.Apply(current);
                        write = true;
                    }
                }
            }

            if (thread.Reinsert)
            {
                if (thread.StepInstruction && thread.Armed && !thread.Hidden)
                {
                    Disasm::RegisterValues current = context.Values();
                    bool changed = false;
                    for (auto& [address, breakpoint] : m_breakpoints)
                    {
                        if (breakpoint.Slot < 0 || breakpoint.Slot >= 4 || (dr6 & (1ull << breakpoint.Slot)) != 0)
                            continue;
                        if (Touches(breakpoint, thread.StepFirst, thread.StepLast, thread.StepWrites))
                            DataHit(breakpoint, thread.StepInstruction, event.dwThreadId, current, changed, enterBreak, reason, breakpointAddress);
                    }
                    if (changed)
                        context.Apply(current);
                }
                thread.StepInstruction = 0;
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
                    if (thread.ReturnPending || ++thread.Steps > kMaxReturnSteps)
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
                const bool hardwareActive = AnyHardware();
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
        if (BreaksOnException(code, at))
        {
            m_cause = {};
            m_cause.Raised = true;
            m_cause.Code = code;
            m_cause.FirstChance = event.u.Exception.dwFirstChance != 0;
            m_cause.ParamCount = static_cast<int>(std::min<DWORD>(record.NumberParameters, 2));
            for (int i = 0; i < m_cause.ParamCount; ++i)
                m_cause.Params[i] = record.ExceptionInformation[i];
            enterBreak = true;
            reason = DescribeException(m_cause);
            breakpointAddress = 0;
        }
        return DBG_EXCEPTION_NOT_HANDLED;
    }

    bool Debugger::BreaksOnException(std::uint32_t code, std::uint64_t address) const
    {
        const ExceptionBreak mode = m_exceptionMode.load();
        if (mode == ExceptionBreak::Never || !UnexpectedException(code))
            return false;
        if (mode == ExceptionBreak::Always)
            return true;
        return address >= m_exceptionStart.load() && address < m_exceptionStop.load();
    }

    void Debugger::SetExceptionBreak(ExceptionBreak mode, std::uint64_t start, std::uint64_t stop)
    {
        m_exceptionStart = start;
        m_exceptionStop = stop;
        m_exceptionMode = mode;
        if (m_backend == Backend::Veh)
        {
            std::lock_guard lock(m_lock);
            VehWriteExceptionBreak();
        }
    }

    void Debugger::VehWriteExceptionBreak()
    {
        if (!m_vehBlock || !m_process)
            return;
        const std::uint64_t range[2] = { m_exceptionStart.load(), m_exceptionStop.load() };
        m_process->Write(m_vehBlock + offsetof(Veh::Block, ExceptionStart), range, sizeof(range));
        const ExceptionBreak mode = m_exceptionMode.load();
        const std::uint32_t raw = mode == ExceptionBreak::Always ? Veh::ExceptionAlways : mode == ExceptionBreak::InRegion ? Veh::ExceptionInRegion : Veh::ExceptionNever;
        m_process->Write(m_vehBlock + offsetof(Veh::Block, ExceptionMode), &raw, sizeof(raw));
    }

    void Debugger::Loop(std::shared_ptr<std::promise<Status>> attached)
    {
        SetThreadName(L"Debugger");
        const std::uint32_t id = Raw(m_process->Id());
        if (!::DebugActiveProcess(id))
        {
            attached->set_value(Status::Fail(ErrorCode::DebugAttachFailed, SystemErrorText(::GetLastError())));
            return;
        }
        ::DebugSetProcessKillOnExit(FALSE);
        m_attached = true;
        ++m_generation;

        const std::uint64_t settleUntil = ::GetTickCount64() + kAttachSettleMilliseconds;
        bool exited = false;
        DEBUG_EVENT event{};
        while (!m_stop)
        {
            if (attached && (m_initialBreakSeen || ::GetTickCount64() >= settleUntil))
            {
                attached->set_value(Status::Success());
                attached.reset();
            }
            {
                std::unique_lock lock(m_lock);
                RunTasks();
                if (AnyHardware())
                    CheckHiddenThreads(false);
            }
            if (!::WaitForDebugEventEx(&event, 10))
                continue;

            DWORD status = DBG_CONTINUE;
            bool enterBreak = false;
            std::string reason;
            std::uint64_t breakpointAddress = 0;
            std::uint32_t breakThread = event.dwThreadId;
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
                if (AnyHardware())
                    ApplyHardware(event.dwThreadId);
                break;
            }
            case CREATE_THREAD_DEBUG_EVENT:
            {
                auto state = std::make_unique<ThreadState>();
                state->Handle = event.u.CreateThread.hThread;
                state->Id = event.dwThreadId;
                m_threads[event.dwThreadId] = std::move(state);
                if (AnyHardware())
                    ApplyHardware(event.dwThreadId);
                break;
            }
            case EXIT_THREAD_DEBUG_EVENT:
                m_threads.erase(event.dwThreadId);
                if (event.dwThreadId == m_break.ThreadId)
                    m_stepping = false;
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (event.u.LoadDll.hFile)
                    ::CloseHandle(event.u.LoadDll.hFile);
                break;
            case EXCEPTION_DEBUG_EVENT:
                status = HandleException(&event, enterBreak, reason, breakpointAddress, breakThread);
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
                m_stepping = false;
                LOG_WARN("Debugger", ErrorMessage(ErrorCode::DebugTargetExited) + " Exit code " + Hex(event.u.ExitProcess.dwExitCode, 8) + ".");
                break;
            default:
                break;
            }

            if (enterBreak && !m_stop)
            {
                EnterBreak(breakThread, reason, breakpointAddress);
                while (m_break.Broken && !m_resumeRequested && !m_stop)
                {
                    m_wake.wait_for(lock, std::chrono::milliseconds(50));
                    RunTasks();
                }
                auto threadFound = m_threads.find(m_break.ThreadId);
                if (threadFound != m_threads.end() && m_break.Broken)
                {
                    ThreadState& thread = *threadFound->second;
                    ContextHolder context(m_is64Bit);
                    if (context.Read(thread.Handle))
                    {
                        if (m_registersChanged)
                            context.Apply(m_newRegisters);
                        if (m_vectorsChanged)
                            context.ApplyVectors(m_newVectors);
                        if (m_fpuChanged)
                            context.ApplyFpu(m_newFpu);
                        std::uint64_t flags = context.Flags();
                        if (thread.ResumeFlag)
                        {
                            flags |= kResumeFlag;
                            thread.ResumeFlag = false;
                        }
                        ResumeMode mode = m_stop ? ResumeMode::Continue : m_resumeMode;
                        if (mode == ResumeMode::RunToReturn)
                        {
                            const Disasm::Instruction current = m_disassembler->Read(*m_process, context.Ip(), nullptr);
                            if (current.Valid && current.Kind == Disasm::Flow::Return)
                                mode = ResumeMode::StepInto;
                            else if (const auto slot = ReturnSlot(context.Values()))
                            {
                                Breakpoint temporary;
                                temporary.Address = m_process->ReadPointer(*slot).value_or(0);
                                temporary.Temporary = true;
                                temporary.OwnerThread = thread.Id;
                                temporary.AboveStack = *slot;
                                if (temporary.Address && !m_breakpoints.count(temporary.Address) && InsertSoftware(temporary))
                                {
                                    m_breakpoints[temporary.Address] = temporary;
                                    mode = ResumeMode::Continue;
                                }
                            }
                        }
                        if (mode == ResumeMode::StepOver)
                        {
                            const Disasm::Instruction current = m_disassembler->Read(*m_process, context.Ip(), nullptr);
                            if (current.Kind == Disasm::Flow::Call && current.Valid)
                            {
                                Breakpoint temporary;
                                temporary.Address = current.Next();
                                temporary.Temporary = true;
                                temporary.OwnerThread = thread.Id;
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
                        {
                            flags |= kTrapFlag;
                            PlanStepAccess(thread, context.Values());
                        }
                        context.SetFlags(flags);
                        context.Write();
                    }
                }
                if (m_break.Exception.Raised && (m_registersChanged || m_vectorsChanged || m_fpuChanged || m_exceptionHandled))
                    status = DBG_CONTINUE;
                m_break.Broken = false;
                m_break.Exception = {};
                m_registersChanged = false;
                m_vectorsChanged = false;
                m_fpuChanged = false;
                m_exceptionHandled = false;
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
            LOG_ERROR("Debugger", ErrorMessage(ErrorCode::DebugDetachFailed, SystemErrorText(::GetLastError())));
        else
            LOG_INFO("Debugger", "Detached");
        if (attached)
            attached->set_value(exited ? Status::Fail(ErrorCode::DebugTargetExited) : Status::Success());
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
        return (std::filesystem::path(self).parent_path() / AgentName()).wstring();
    }

    std::wstring Debugger::AgentName() const
    {
        return m_is64Bit ? L"ManifoldAgent.dll" : L"ManifoldAgent32.dll";
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
        m_process->Write(slotAddress, shared.get(), offsetof(Veh::Watch, Entries));
        VehBump();
    }

    void Debugger::VehWriteAction(int slot, const BreakAction* action, std::uint32_t threadId, bool keepCounts)
    {
        if (!m_vehBlock || !m_process || slot < 0 || slot >= Veh::kMaxWatches)
            return;
        auto shared = std::make_unique<Veh::Action>();
        std::memset(shared.get(), 0, sizeof(Veh::Action));
        shared->Break = action ? (action->Break ? 1u : 0u) : 1u;
        shared->ThreadId = threadId ? threadId : (action ? action->ThreadId : 0u);
        if (action)
        {
            const std::size_t length = std::min<std::size_t>(action->ConditionCode.size(), static_cast<std::size_t>(Program::kMaxCode));
            std::memcpy(shared->Condition, action->ConditionCode.data(), length);
            shared->ConditionLength = static_cast<std::uint32_t>(length);
            const std::size_t count = std::min<std::size_t>(action->ChangeCode.size(), static_cast<std::size_t>(Program::kMaxChanges));
            for (std::size_t i = 0; i < count; ++i)
            {
                const RegisterChange& change = action->ChangeCode[i];
                Program::Change& out = shared->Changes[i];
                out.Target = change.Target;
                out.Part = change.Part;
                const std::size_t codeLength = std::min<std::size_t>(change.Code.size(), static_cast<std::size_t>(Program::kMaxCode));
                out.Length = static_cast<std::uint8_t>(codeLength);
                std::memcpy(out.Code, change.Code.data(), codeLength);
            }
            shared->ChangeCount = static_cast<std::uint32_t>(count);
        }
        const std::uint64_t actionAddress = m_vehBlock + offsetof(Veh::Block, Actions) + static_cast<std::uint64_t>(slot) * sizeof(Veh::Action);
        if (!keepCounts)
        {
            m_process->Write(actionAddress, shared.get(), sizeof(Veh::Action));
            return;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(shared.get());
        m_process->Write(actionAddress, bytes, offsetof(Veh::Action, Hits));
        m_process->Write(actionAddress + offsetof(Veh::Action, Condition), bytes + offsetof(Veh::Action, Condition), sizeof(Veh::Action) - offsetof(Veh::Action, Condition));
    }

    Status Debugger::VehSetBreakpoint(std::uint64_t address, BreakpointType type, int size, const BreakAction& action)
    {
        std::lock_guard lock(m_lock);
        if (!m_attached || !m_vehBlock)
            return Status::Fail(ErrorCode::DebugNotAttached);
        if (m_breakpoints.count(address))
            return Status::Fail(ErrorCode::DebugBreakpointFailed, Hex(address) + " has a breakpoint already");
        const int slot = VehFreeSlot();
        if (slot < 0)
            return Status::Fail(ErrorCode::DebugNoFreeSlot);
        const std::uint32_t mode = type == BreakpointType::Write ? Veh::ModeBreakWrite : type == BreakpointType::Access ? Veh::ModeBreakAccess : Veh::ModeBreak;
        Breakpoint breakpoint;
        breakpoint.Address = address;
        breakpoint.Type = type;
        breakpoint.Size = size;
        breakpoint.Hardware = true;
        breakpoint.Slot = slot;
        breakpoint.Inserted = false;
        breakpoint.Action = action;
        VehWriteAction(slot, &breakpoint.Action, 0, false);
        VehWriteSlot(slot, mode, address, size, true);
        m_breakpoints[address] = breakpoint;
        ++m_generation;
        LOG_INFO("Debugger", std::string(type == BreakpointType::Execute ? "Breakpoint" : std::string(BreakpointTypeName(type)) + " breakpoint") + " at " + Hex(address) +
            " (in-process, debug register " + std::to_string(slot) + ")");
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
        m_vehStepOut = false;
        bool keep = false;
        if (mode != ResumeMode::Continue)
        {
            const Disasm::Instruction current = m_disassembler->Read(*m_process, m_break.Address, nullptr);
            keep = current.Valid && current.Kind != Disasm::Flow::Interrupt && current.Mnemonic != "syscall" && current.Mnemonic != "sysenter" &&
                   (mode == ResumeMode::StepInto || (mode == ResumeMode::StepOver && current.Kind != Disasm::Flow::Call) ||
                       (mode == ResumeMode::RunToReturn && current.Kind == Disasm::Flow::Return));
            if (mode == ResumeMode::RunToReturn && current.Kind != Disasm::Flow::Return)
            {
                if (const auto slot = ReturnSlot(m_break.Registers))
                {
                    const std::uint64_t target = m_process->ReadPointer(*slot).value_or(0);
                    const int free = VehFreeSlot();
                    if (target && free >= 0)
                    {
                        BreakAction action;
                        action.Condition = Format("%s > 0x%llX", m_is64Bit ? "RSP" : "ESP", static_cast<unsigned long long>(*slot));
                        const SymbolResolver none = [](std::string_view) { return std::optional<std::uint64_t>(); };
                        if (CompileAction(action, m_is64Bit, none))
                        {
                            m_vehTempSlots.push_back(free);
                            VehWriteAction(free, &action, m_break.ThreadId, false);
                            VehWriteSlot(free, Veh::ModeBreak, target, 1, true);
                            m_vehStepOut = true;
                        }
                    }
                }
            }
            std::vector<std::uint64_t> targets = m_vehStepOut ? std::vector<std::uint64_t>{} : VehStepTargets(current, m_break.Registers, mode);
            for (std::uint64_t target : targets)
            {
                if (!target || target == m_break.Address)
                    continue;
                const int slot = VehFreeSlot();
                if (slot < 0)
                    break;
                m_vehTempSlots.push_back(slot);
                VehWriteAction(slot, nullptr, m_break.ThreadId, false);
                VehWriteSlot(slot, Veh::ModeBreak, target, 1, true);
            }
            if (m_vehTempSlots.empty())
            {
                const int slot = VehFreeSlot();
                if (slot >= 0)
                {
                    m_vehTempSlots.push_back(slot);
                    VehWriteAction(slot, nullptr, m_break.ThreadId, false);
                    VehWriteSlot(slot, Veh::ModeBreak, current.Next(), 1, true);
                }
            }
        }
        m_stepping = mode != ResumeMode::Continue;
        m_break.Broken = false;
        ++m_generation;
        const std::uint32_t keepThreads = keep ? 1u : 0u;
        m_process->Write(m_vehBlock + offsetof(Veh::Block, KeepSuspended), &keepThreads, sizeof(keepThreads));
        const std::uint32_t cmd = Veh::ResumeContinue;
        m_process->Write(m_vehBlock + offsetof(Veh::Block, ResumeCmd), &cmd, sizeof(cmd));
        VehWakeAgent();
    }

    void Debugger::VehWakeAgent()
    {
        if (m_vehResumeEvent)
            ::SetEvent(static_cast<HANDLE>(m_vehResumeEvent));
    }

    // The innermost return address on the stack: the first slot from RSP up
    // whose value follows a call instruction in executable memory. Frames
    // without unwind data (allocated code, 32-bit) have no better source, and
    // a stale value can only make the step out stop early, never run away,
    // because the temporary breakpoint also waits for RSP above the slot.
    std::optional<std::uint64_t> Debugger::ReturnSlot(const Disasm::RegisterValues& registers) const
    {
        constexpr std::size_t kScan = 0x4000;
        const int pointer = m_is64Bit ? 8 : 4;
        std::vector<std::uint8_t> stack(kScan);
        const std::size_t got = m_process->Read(registers.Rsp, stack.data(), stack.size());
        std::uint64_t regionStart = 0;
        std::uint64_t regionEnd = 0;
        bool executable = false;
        for (std::size_t at = 0; at + static_cast<std::size_t>(pointer) <= got; at += static_cast<std::size_t>(pointer))
        {
            std::uint64_t value = 0;
            std::memcpy(&value, stack.data() + at, static_cast<std::size_t>(pointer));
            if (value < 0x10000)
                continue;
            if (value < regionStart || value >= regionEnd)
            {
                const auto region = m_process->RegionAt(value);
                if (!region)
                {
                    regionStart = regionEnd = 0;
                    continue;
                }
                regionStart = region->Base;
                regionEnd = region->End();
                executable = region->Committed() && region->Executable();
            }
            if (!executable)
                continue;
            std::uint8_t bytes[7] = {};
            if (m_process->Read(value - 7, bytes, sizeof(bytes)) != sizeof(bytes))
                continue;
            UnpatchLocked(value - 7, bytes, sizeof(bytes));
            for (const int length : { 5, 6, 2, 3, 7, 4 })
            {
                const Disasm::Instruction call = m_disassembler->Decode(value - static_cast<std::uint64_t>(length), bytes + (7 - length), static_cast<std::size_t>(length), nullptr);
                if (call.Valid && call.Kind == Disasm::Flow::Call && call.Length == length)
                    return registers.Rsp + at;
            }
        }
        return std::nullopt;
    }

    void Debugger::VehSetRegisters(const Disasm::RegisterValues& registers)
    {
        std::lock_guard lock(m_lock);
        if (!m_break.Broken || !m_vehBlock || !m_process)
            return;
        const Veh::Registers regs = ToAgent(registers, m_is64Bit);
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
        struct { std::uint32_t Broken, ResumeCmd, BrokenThread, RegsDirty, BreakNow, BreakSlot; std::uint64_t BreakSerial, BreakAddress; Veh::Registers Regs; } head{};
        if (!m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, Broken), &head, sizeof(head)))
            return;
        std::lock_guard lock(m_lock);
        if (head.Broken && head.BreakSerial != m_vehBreakSerial)
        {
            m_vehBreakSerial = head.BreakSerial;
            std::string reason = "Breakpoint";
            std::uint64_t breakpointAddress = head.BreakAddress;
            const int breakSlot = static_cast<int>(head.BreakSlot);
            BreakException exception;
            if (head.BreakSlot == Veh::kSlotBreakNow)
                reason = "Break";
            else if (head.BreakSlot == Veh::kSlotException)
            {
                struct
                {
                    std::uint32_t Code;
                    std::uint32_t Count;
                    std::uint64_t Params[2];
                } cause{};
                if (m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, BreakCode), &cause, sizeof(cause)))
                {
                    exception.Raised = true;
                    exception.Code = cause.Code;
                    exception.ParamCount = static_cast<int>(std::min<std::uint32_t>(cause.Count, 2));
                    exception.Params[0] = cause.Params[0];
                    exception.Params[1] = cause.Params[1];
                }
                reason = DescribeException(exception);
                breakpointAddress = 0;
            }
            else if (std::find(m_vehTempSlots.begin(), m_vehTempSlots.end(), breakSlot) != m_vehTempSlots.end())
                reason = m_vehStepOut ? "Returned to the caller" : "Step";
            else
            {
                for (const auto& [address, breakpoint] : m_breakpoints)
                {
                    if (breakpoint.Slot != breakSlot || breakpoint.WatchId)
                        continue;
                    breakpointAddress = breakpoint.Address;
                    if (breakpoint.Type != BreakpointType::Execute)
                        reason = std::string(BreakpointTypeName(breakpoint.Type)) + " breakpoint on " + Hex(breakpoint.Address);
                    break;
                }
            }
            VehClearTempSlots();
            const Disasm::RegisterValues regs = FromAgent(head.Regs, m_is64Bit);
            m_break = {};
            Veh::Vectors vectors{};
            if (m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, BreakXmm), &vectors, sizeof(vectors)) && vectors.Count > 0 && vectors.Count <= 16)
            {
                for (std::uint32_t i = 0; i < vectors.Count; ++i)
                    std::memcpy(m_break.Vectors.Xmm[i].data(), vectors.Xmm[i], 16);
                m_break.Vectors.Mxcsr = vectors.Mxcsr;
                m_break.Vectors.Count = static_cast<int>(vectors.Count);
            }
            std::uint32_t fpuValid = 0;
            if (m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, FpuValid), &fpuValid, sizeof(fpuValid)) && fpuValid &&
                m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, BreakFpu), m_break.Fpu.Area.data(), FpuRegisters::kBytes))
                m_break.Fpu.Valid = true;
            m_break.Exception = exception;
            m_stepping = false;
            m_break.Broken = true;
            m_break.ThreadId = head.BrokenThread;
            m_break.Address = head.BreakAddress;
            m_break.BreakpointAddress = breakpointAddress;
            m_break.Registers = regs;
            m_break.Reason = reason;
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
        const std::wstring agent = AgentPath();
        const std::string agentName = WideToUtf8(AgentName());
        std::error_code ignore;
        if (!std::filesystem::exists(agent, ignore))
            return Status::Fail(ErrorCode::InjectFailed, agentName + " was not found next to the program");

        const Result<Address> allocated = m_process->Allocate(sizeof(Veh::Block), PAGE_READWRITE);
        if (!allocated)
            return allocated.Error();
        const std::uint64_t block = *allocated;
        auto shared = std::make_unique<Veh::Block>();
        std::memset(shared.get(), 0, sizeof(Veh::Block));
        shared->Magic = Veh::kMagic;
        shared->Version = Veh::kVersion;
        if (Status written = m_process->Write(block, shared.get(), sizeof(Veh::Block)); !written)
        {
            m_process->Free(block);
            return written;
        }
        HANDLE breakEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE resumeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (HANDLE duplicator = ::OpenProcess(PROCESS_DUP_HANDLE, FALSE, Raw(m_process->Id())))
        {
            HANDLE remoteBreak = nullptr;
            HANDLE remoteResume = nullptr;
            if (breakEvent && resumeEvent && ::DuplicateHandle(::GetCurrentProcess(), breakEvent, duplicator, &remoteBreak, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
                ::DuplicateHandle(::GetCurrentProcess(), resumeEvent, duplicator, &remoteResume, 0, FALSE, DUPLICATE_SAME_ACCESS))
            {
                const std::uint64_t remote[2] = { reinterpret_cast<std::uintptr_t>(remoteBreak), reinterpret_cast<std::uintptr_t>(remoteResume) };
                m_process->Write(block + offsetof(Veh::Block, BreakEvent), remote, sizeof(remote));
            }
            ::CloseHandle(duplicator);
        }
        const auto dropEvents = [&]()
            {
                if (breakEvent)
                    ::CloseHandle(breakEvent);
                if (resumeEvent)
                    ::CloseHandle(resumeEvent);
                breakEvent = resumeEvent = nullptr;
            };
        const bool loadedBefore = m_process->FindModule(agentName).has_value();
        if (Status injected = m_process->InjectLibrary(agent); !injected)
        {
            dropEvents();
            m_process->Free(block);
            return injected;
        }
        const std::uint64_t start = m_process->FindExport(agentName, "ManifoldAgentStart");
        if (!start)
        {
            dropEvents();
            m_process->Free(block);
            return Status::Fail(ErrorCode::InjectFailed, "ManifoldAgentStart was not found in the agent");
        }
        if (Status thread = m_process->CreateThread(start, block, true); !thread)
        {
            dropEvents();
            if (thread.Code != ErrorCode::ThreadTimedOut)
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
            dropEvents();
            std::uint32_t lastError = 0;
            m_process->ReadExact(block + offsetof(Veh::Block, LastError), &lastError, sizeof(lastError));
            m_process->Free(block);
            if (loadedBefore && !lastError)
                return Status::Fail(ErrorCode::DebugAgentVersion);
            return Status::Fail(ErrorCode::InjectFailed, "the agent did not start" + (lastError ? " (" + SystemErrorText(lastError) + ")" : std::string()));
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
            m_vehStepOut = false;
            m_vehBreakEvent = breakEvent;
            m_vehResumeEvent = resumeEvent;
            VehWriteExceptionBreak();
        }
        m_vehStop = false;
        m_attached = true;
        ++m_generation;
        m_vehThread = std::thread(&Debugger::VehPollLoop, this);
        LOG_SUCCESS("Debugger", "In-process debugger running in " + m_process->Name() + " (" + ToText(m_process->Id()) + ")");
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
            VehWakeAgent();
            std::uint32_t running = 1;
            for (int tries = 0; tries < 400 && running && m_process->Alive(); ++tries)
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
        for (void** event : { &m_vehBreakEvent, &m_vehResumeEvent })
        {
            if (*event)
                ::CloseHandle(static_cast<HANDLE>(*event));
            *event = nullptr;
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
        SetThreadName(L"Debugger agent poll");
        struct Target
        {
            int Id;
            int Slot;
            WatchKind Kind;
            std::uint64_t Address;
        };
        auto shared = std::make_unique<Veh::Watch>();
        std::uint64_t beat = 0;
        bool lostReported = false;
        while (!m_vehStop)
        {
            if (m_vehBreakEvent)
                ::WaitForSingleObject(static_cast<HANDLE>(m_vehBreakEvent), 30);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (!m_process || !m_process->Alive())
            {
                std::lock_guard lock(m_lock);
                for (auto& [id, watch] : m_watches)
                    watch.Running = false;
                continue;
            }
            ++beat;
            if (!m_beatPaused)
                m_process->Write(m_vehBlock + offsetof(Veh::Block, HostBeat), &beat, sizeof(beat));
            if (beat % 32 == 0 && !lostReported)
            {
                std::uint32_t lost = 0;
                if (m_process->ReadExact(m_vehBlock + offsetof(Veh::Block, HostLost), &lost, sizeof(lost)) && lost)
                {
                    lostReported = true;
                    LOG_WARN("Debugger", "The in-process debugger stopped itself because Manifold did not answer for "
                        + std::to_string(Veh::kHostSilenceMs / 1000) + " seconds. Detach and attach again.");
                    std::lock_guard lock(m_lock);
                    for (auto& [id, watch] : m_watches)
                        watch.Running = false;
                    ++m_generation;
                }
            }
            VehCheckBreak();
            std::vector<std::pair<std::uint64_t, int>> counted;
            {
                std::lock_guard lock(m_lock);
                for (const auto& [address, breakpoint] : m_breakpoints)
                {
                    if (!breakpoint.WatchId && breakpoint.Slot >= 0 && breakpoint.Slot < Veh::kMaxWatches)
                        counted.emplace_back(address, breakpoint.Slot);
                }
            }
            for (const auto& [address, slot] : counted)
            {
                std::uint64_t hits = 0;
                const std::uint64_t actionAddress = m_vehBlock + offsetof(Veh::Block, Actions) + static_cast<std::uint64_t>(slot) * sizeof(Veh::Action);
                if (!m_process->ReadExact(actionAddress + offsetof(Veh::Action, Hits), &hits, sizeof(hits)))
                    continue;
                std::lock_guard lock(m_lock);
                auto found = m_breakpoints.find(address);
                if (found != m_breakpoints.end() && found->second.Slot == slot)
                    found->second.Hits = hits;
            }
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
                    const Disasm::RegisterValues regs = FromAgent(raw.Regs, m_is64Bit);
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
                    else if (target.Kind == WatchKind::Calls && raw.Accessed)
                    {
                        entry.Instruction = PreviousInstruction(raw.Accessed).value_or(raw.Accessed);
                        entry.Accessed = raw.Accessed;
                    }
                    else
                    {
                        const HitPlace place = ExecuteHitPlace(target.Kind, raw.Instruction, regs).value_or(HitPlace{ raw.Instruction, 0 });
                        entry.Instruction = place.Instruction;
                        entry.Accessed = place.Accessed;
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
