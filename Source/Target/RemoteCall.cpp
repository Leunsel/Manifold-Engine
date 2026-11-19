#include "Target/RemoteCall.h"

#include "Core/Text.h"
#include "Target/Process.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        // Control block the worker polls. Request: 0 idle, 1 call, 2 stop; the
        // worker writes Result and sets Request back to 0 when a call returns.
        // Stopped is written last when the worker leaves: 1 after a stop
        // request, 2 when the host process was gone (also its exit code).
        // Idle, it waits on the wake event and the host process handle, both
        // duplicated into the target (WaitAny), or naps with Sleep(1) when
        // those could not be set up.
        constexpr std::uint32_t kRequestOffset = 0x00;
        constexpr std::uint32_t kFuncOffset = 0x08;
        constexpr std::uint32_t kResultOffset = 0x30;
        constexpr std::uint32_t kStoppedOffset = 0x38;
        constexpr std::uint32_t kSleepOffset = 0x40;
        constexpr std::uint32_t kWaitOffset = 0x48;        // WaitForMultipleObjects
        constexpr std::uint32_t kWakeOffset = 0x50;        // handles[0]: wake event
        constexpr std::uint32_t kHostOffset = 0x58;        // handles[1]: host process
        constexpr std::uint32_t kCloseOffset = 0x60;       // CloseHandle
        constexpr std::uint32_t kControlSize = 0x70;       // 0x68: exit reason while leaving
        constexpr std::uint32_t kSpinBeforeSleep = 100000;
        constexpr std::uint32_t kWaitMilliseconds = 200;
        constexpr std::uint32_t kCollectorOffset = 0x200;
        constexpr std::uint64_t kScratchSize = 0x10000;

        // The iterator drain: u64 drain(fn, object, block), run by the worker.
        // Block: +0x00 count (out), +0x08 cap, +0x10 iterator slot (in/out, so
        // a full batch continues on the next call), +0x18 peek offset (or ~0),
        // +0x20 done (out), +0x28 peek function (or 0); 16-byte entries
        // { item, peek } from +0x30.
        constexpr std::uint32_t kDrainOffset = 0x900;
        constexpr std::uint32_t kDrainHeader = 0x30;
        constexpr std::uint32_t kDrainBatch = 1024;

        class Emitter
        {
        public:
            void Byte(std::uint8_t value) { m_bytes.push_back(value); }
            void Bytes(std::initializer_list<std::uint8_t> values) { m_bytes.insert(m_bytes.end(), values); }
            void Label(const std::string& name) { m_labels[name] = m_bytes.size(); }

            // jcc (0F 8x) / jmp (E9) with a rel32 filled in once labels are known.
            void Jump(std::uint8_t op, const std::string& target)
            {
                if (op == 0xE9)
                    Byte(0xE9);
                else
                    Bytes({ 0x0F, op });
                m_fixups.push_back({ m_bytes.size(), target });
                Bytes({ 0, 0, 0, 0 });
            }

            std::vector<std::uint8_t> Resolve()
            {
                for (const auto& fixup : m_fixups)
                {
                    const std::int64_t from = static_cast<std::int64_t>(fixup.At) + 4;
                    const std::int64_t to = static_cast<std::int64_t>(m_labels.at(fixup.Target));
                    const std::int32_t rel = static_cast<std::int32_t>(to - from);
                    std::memcpy(&m_bytes[fixup.At], &rel, 4);
                }
                return m_bytes;
            }

        private:
            struct Fixup { std::size_t At; std::string Target; };
            std::vector<std::uint8_t> m_bytes;
            std::map<std::string, std::size_t> m_labels;
            std::vector<Fixup> m_fixups;
        };

        std::vector<std::uint8_t> WorkerCode()
        {
            Emitter e;
            e.Bytes({ 0x56 });                         // push rsi
            e.Bytes({ 0x57 });                         // push rdi
            e.Bytes({ 0x48, 0x89, 0xCE });             // mov rsi, rcx   (control)
            e.Bytes({ 0x48, 0x89, 0xE7 });             // mov rdi, rsp   (save)
            e.Bytes({ 0x48, 0x83, 0xE4, 0xF0 });       // and rsp, -16
            e.Bytes({ 0x48, 0x83, 0xEC, 0x20 });       // sub rsp, 0x20  (shadow)
            e.Label("awake");
            e.Bytes({ 0x41, 0xBB });                   // mov r11d, spin budget
            for (int shift = 0; shift < 32; shift += 8)
                e.Byte(static_cast<std::uint8_t>(kSpinBeforeSleep >> shift));
            e.Label("loop");
            e.Bytes({ 0x48, 0x8B, 0x06 });             // mov rax, [rsi]      request
            e.Bytes({ 0x48, 0x83, 0xF8, 0x02 });       // cmp rax, 2
            e.Jump(0x84, "stop");                      // je stop
            e.Bytes({ 0x48, 0x83, 0xF8, 0x01 });       // cmp rax, 1
            e.Jump(0x84, "call");                      // je call
            e.Bytes({ 0x45, 0x85, 0xDB });             // test r11d, r11d
            e.Jump(0x84, "doze");                      // jz doze
            e.Bytes({ 0x41, 0xFF, 0xCB });             // dec r11d
            e.Bytes({ 0xF3, 0x90 });                   // pause
            e.Jump(0xE9, "loop");                      // jmp loop
            e.Label("doze");
            e.Bytes({ 0x48, 0x8B, 0x46, 0x48 });       // mov rax, [rsi+0x48] WaitForMultipleObjects
            e.Bytes({ 0x48, 0x85, 0xC0 });             // test rax, rax
            e.Jump(0x84, "nap");                       // jz nap
            e.Bytes({ 0xB9, 0x02, 0x00, 0x00, 0x00 }); // mov ecx, 2           count
            e.Bytes({ 0x48, 0x8D, 0x56, 0x50 });       // lea rdx, [rsi+0x50]  handles
            e.Bytes({ 0x45, 0x31, 0xC0 });             // xor r8d, r8d         any
            e.Bytes({ 0x41, 0xB9 });                   // mov r9d, timeout
            for (int shift = 0; shift < 32; shift += 8)
                e.Byte(static_cast<std::uint8_t>(kWaitMilliseconds >> shift));
            e.Bytes({ 0xFF, 0xD0 });                   // call rax
            e.Bytes({ 0x83, 0xF8, 0x01 });             // cmp eax, 1           host process signalled
            e.Jump(0x84, "gone");                      // je gone
            e.Bytes({ 0x85, 0xC0 });                   // test eax, eax        woken
            e.Jump(0x84, "dozed");                     // jz dozed
            e.Bytes({ 0x3D, 0x02, 0x01, 0x00, 0x00 }); // cmp eax, 0x102       timeout
            e.Jump(0x84, "dozed");                     // je dozed
            e.Bytes({ 0x48, 0xC7, 0x46, 0x48, 0x00, 0x00, 0x00, 0x00 }); // mov qword [rsi+0x48], 0  (the wait failed: nap)
            e.Label("dozed");
            e.Bytes({ 0x45, 0x31, 0xDB });             // xor r11d, r11d
            e.Jump(0xE9, "loop");                      // jmp loop
            e.Label("nap");
            e.Bytes({ 0x48, 0x8B, 0x46, 0x40 });       // mov rax, [rsi+0x40] Sleep
            e.Bytes({ 0x48, 0x85, 0xC0 });             // test rax, rax
            e.Jump(0x84, "awake");                     // jz awake
            e.Bytes({ 0xB9, 0x01, 0x00, 0x00, 0x00 }); // mov ecx, 1
            e.Bytes({ 0xFF, 0xD0 });                   // call rax
            e.Bytes({ 0x45, 0x31, 0xDB });             // xor r11d, r11d
            e.Jump(0xE9, "loop");                      // jmp loop
            e.Label("call");
            e.Bytes({ 0x48, 0x8B, 0x4E, 0x10 });       // mov rcx, [rsi+0x10] a0
            e.Bytes({ 0x48, 0x8B, 0x56, 0x18 });       // mov rdx, [rsi+0x18] a1
            e.Bytes({ 0x4C, 0x8B, 0x46, 0x20 });       // mov r8,  [rsi+0x20] a2
            e.Bytes({ 0x4C, 0x8B, 0x4E, 0x28 });       // mov r9,  [rsi+0x28] a3
            e.Bytes({ 0x48, 0x8B, 0x46, 0x08 });       // mov rax, [rsi+0x08] func
            e.Bytes({ 0xFF, 0xD0 });                   // call rax
            e.Bytes({ 0x48, 0x89, 0x46, 0x30 });       // mov [rsi+0x30], rax result
            e.Bytes({ 0xB8, 0x01, 0x00, 0x00, 0x00 }); // mov eax, 1
            e.Bytes({ 0x31, 0xC9 });                   // xor ecx, ecx
            e.Bytes({ 0xF0, 0x48, 0x0F, 0xB1, 0x0E }); // lock cmpxchg [rsi], rcx  request 1 -> 0, a stop sent meanwhile stays
            e.Jump(0xE9, "awake");                     // jmp awake
            e.Label("gone");
            e.Bytes({ 0xB8, 0x02, 0x00, 0x00, 0x00 }); // mov eax, 2           host gone
            e.Jump(0xE9, "leave");                     // jmp leave
            e.Label("stop");
            e.Bytes({ 0xB8, 0x01, 0x00, 0x00, 0x00 }); // mov eax, 1           stop request
            e.Label("leave");
            e.Bytes({ 0x48, 0x89, 0x46, 0x68 });       // mov [rsi+0x68], rax  reason
            e.Bytes({ 0x48, 0x8B, 0x46, 0x60 });       // mov rax, [rsi+0x60]  CloseHandle
            e.Bytes({ 0x48, 0x85, 0xC0 });             // test rax, rax
            e.Jump(0x84, "done");                      // jz done
            e.Bytes({ 0x48, 0x8B, 0x4E, 0x50 });       // mov rcx, [rsi+0x50]  wake event
            e.Bytes({ 0x48, 0x85, 0xC9 });             // test rcx, rcx
            e.Jump(0x84, "closehost");                 // jz closehost
            e.Bytes({ 0xFF, 0x56, 0x60 });             // call [rsi+0x60]
            e.Label("closehost");
            e.Bytes({ 0x48, 0x8B, 0x4E, 0x58 });       // mov rcx, [rsi+0x58]  host process
            e.Bytes({ 0x48, 0x85, 0xC9 });             // test rcx, rcx
            e.Jump(0x84, "done");                      // jz done
            e.Bytes({ 0xFF, 0x56, 0x60 });             // call [rsi+0x60]
            e.Label("done");
            e.Bytes({ 0x48, 0x8B, 0x46, 0x68 });       // mov rax, [rsi+0x68]
            e.Bytes({ 0x48, 0x89, 0x46, 0x38 });       // mov [rsi+0x38], rax  Stopped (last access)
            e.Bytes({ 0x48, 0x89, 0xFC });             // mov rsp, rdi
            e.Bytes({ 0x5F });                         // pop rdi
            e.Bytes({ 0x5E });                         // pop rsi
            e.Bytes({ 0xC3 });                         // ret                  exit code = reason
            return e.Resolve();
        }

        // A kernel32 export the worker may call. System DLLs share their base
        // across the processes of one boot, so the local address is trusted
        // when the module it lies in (kernel32, or kernelbase for a forwarded
        // export) sits at the same base in the target.
        std::uint64_t SystemExport(const Process& process, const char* name)
        {
            const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
            const FARPROC local = kernel ? ::GetProcAddress(kernel, name) : nullptr;
            HMODULE owner = nullptr;
            if (!local || !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(local), &owner))
                return 0;
            wchar_t path[MAX_PATH] = L"";
            if (!::GetModuleFileNameW(owner, path, MAX_PATH))
                return 0;
            const auto remote = process.FindModule(WideToUtf8(std::filesystem::path(path).filename().wstring()));
            if (!remote || remote->Base != reinterpret_cast<std::uint64_t>(owner))
                return 0;
            return reinterpret_cast<std::uint64_t>(local);
        }

        void CloseRemoteHandle(HANDLE target, std::uint64_t handle)
        {
            if (target && handle)
                ::DuplicateHandle(target, reinterpret_cast<HANDLE>(handle), nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
        }

        std::vector<std::uint8_t> DrainCode()
        {
            Emitter e;
            e.Bytes({ 0x53, 0x56, 0x57 });             // push rbx; push rsi; push rdi
            e.Bytes({ 0x41, 0x54, 0x41, 0x55 });       // push r12; push r13
            e.Bytes({ 0x55 });                         // push rbp
            e.Bytes({ 0x48, 0x89, 0xE5 });             // mov rbp, rsp
            e.Bytes({ 0x48, 0x83, 0xE4, 0xF0 });       // and rsp, -16
            e.Bytes({ 0x48, 0x83, 0xEC, 0x20 });       // sub rsp, 0x20
            e.Bytes({ 0x49, 0x89, 0xCC });             // mov r12, rcx  function
            e.Bytes({ 0x49, 0x89, 0xD5 });             // mov r13, rdx  object
            e.Bytes({ 0x4C, 0x89, 0xC3 });             // mov rbx, r8   block
            e.Label("loop");
            e.Bytes({ 0x48, 0x8B, 0x03 });             // mov rax, [rbx]      count
            e.Bytes({ 0x48, 0x3B, 0x43, 0x08 });       // cmp rax, [rbx+8]    cap
            e.Jump(0x83, "full");                      // jae full
            e.Bytes({ 0x4C, 0x89, 0xE9 });             // mov rcx, r13
            e.Bytes({ 0x48, 0x8D, 0x53, 0x10 });       // lea rdx, [rbx+0x10] iterator
            e.Bytes({ 0x41, 0xFF, 0xD4 });             // call r12
            e.Bytes({ 0x48, 0x85, 0xC0 });             // test rax, rax
            e.Jump(0x84, "end");                       // jz end
            e.Bytes({ 0x48, 0x8B, 0x3B });             // mov rdi, [rbx]
            e.Bytes({ 0x48, 0xC1, 0xE7, 0x04 });       // shl rdi, 4
            e.Bytes({ 0x48, 0x8D, 0x7C, 0x3B, 0x30 }); // lea rdi, [rbx+rdi+0x30]  entry
            e.Bytes({ 0x48, 0x89, 0x07 });             // mov [rdi], rax
            e.Bytes({ 0x48, 0xC7, 0x47, 0x08, 0x00, 0x00, 0x00, 0x00 }); // mov qword [rdi+8], 0
            e.Bytes({ 0x48, 0x8B, 0x73, 0x28 });       // mov rsi, [rbx+0x28] peek function
            e.Bytes({ 0x48, 0x85, 0xF6 });             // test rsi, rsi
            e.Jump(0x84, "read");                      // jz read
            e.Bytes({ 0x48, 0x89, 0xC1 });             // mov rcx, rax
            e.Bytes({ 0xFF, 0xD6 });                   // call rsi
            e.Bytes({ 0x48, 0x89, 0x47, 0x08 });       // mov [rdi+8], rax
            e.Jump(0xE9, "stored");                    // jmp stored
            e.Label("read");
            e.Bytes({ 0x48, 0x8B, 0x73, 0x18 });       // mov rsi, [rbx+0x18] peek offset
            e.Bytes({ 0x48, 0x83, 0xFE, 0xFF });       // cmp rsi, -1
            e.Jump(0x84, "stored");                    // je stored
            e.Bytes({ 0x48, 0x8B, 0x34, 0x30 });       // mov rsi, [rax+rsi]
            e.Bytes({ 0x48, 0x89, 0x77, 0x08 });       // mov [rdi+8], rsi
            e.Label("stored");
            e.Bytes({ 0x48, 0xFF, 0x03 });             // inc qword [rbx]
            e.Jump(0xE9, "loop");                      // jmp loop
            e.Label("end");
            e.Bytes({ 0x48, 0xC7, 0x43, 0x20, 0x01, 0x00, 0x00, 0x00 }); // mov qword [rbx+0x20], 1  done
            e.Label("full");
            e.Bytes({ 0x48, 0x8B, 0x03 });             // mov rax, [rbx]
            e.Bytes({ 0x48, 0x89, 0xEC });             // mov rsp, rbp
            e.Bytes({ 0x5D });                         // pop rbp
            e.Bytes({ 0x41, 0x5D, 0x41, 0x5C });       // pop r13; pop r12
            e.Bytes({ 0x5F, 0x5E, 0x5B });             // pop rdi; pop rsi; pop rbx
            e.Bytes({ 0xC3 });                         // ret
            return e.Resolve();
        }

        std::vector<std::uint8_t> CollectorCode()
        {
            // void collector(rcx = item, rdx = user); user -> { u64 count, u64 cap, u64 items[] }
            return {
                0x48, 0x8B, 0x02,                      // mov rax, [rdx]        count
                0x4C, 0x8B, 0x42, 0x08,                // mov r8,  [rdx+8]      cap
                0x4C, 0x39, 0xC0,                      // cmp rax, r8
                0x73, 0x08,                            // jae +8 (ret)
                0x48, 0x89, 0x4C, 0xC2, 0x10,          // mov [rdx+rax*8+0x10], rcx
                0x48, 0xFF, 0xC0,                      // inc rax
                0x48, 0x89, 0x02,                      // mov [rdx], rax
                0xC3                                   // ret
            };
        }
    }

    RemoteCall::~RemoteCall()
    {
        Stop();
    }

    Status RemoteCall::Start(std::shared_ptr<Process> process)
    {
        Stop();
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        if (!process->Is64Bit())
            return Status::Fail(ErrorCode::ThreadCreateFailed, "the remote-call worker supports 64-bit targets only");
        m_process = std::move(process);

        const Result<Address> codeBlock = m_process->Allocate(0x1000, PAGE_EXECUTE_READWRITE);
        if (!codeBlock)
            return codeBlock.Error();
        m_code = *codeBlock;
        const Result<Address> controlBlock = m_process->Allocate(kControlSize, PAGE_READWRITE);
        const Result<Address> dataBlock = controlBlock ? m_process->Allocate(kScratchSize, PAGE_READWRITE) : Result<Address>(controlBlock.Error());
        const Result<Address> drainBlock = dataBlock ? m_process->Allocate(kDrainHeader + kDrainBatch * sizeof(DrainItem), PAGE_READWRITE) : Result<Address>(dataBlock.Error());
        m_control = controlBlock.ValueOr(0);
        m_data = dataBlock.ValueOr(0);
        m_drainBlock = drainBlock.ValueOr(0);
        if (!controlBlock || !dataBlock || !drainBlock)
        {
            Stop();
            return drainBlock.Error();
        }

        std::vector<std::uint8_t> worker = WorkerCode();
        std::vector<std::uint8_t> collector = CollectorCode();
        m_collector = m_code + kCollectorOffset;
        std::vector<std::uint8_t> control(kControlSize, 0);
        const auto put = [&](std::uint32_t offset, std::uint64_t value) { std::memcpy(control.data() + offset, &value, sizeof(value)); };
        put(kSleepOffset, SystemExport(*m_process, "Sleep"));

        // The wake event lets an idle worker sleep without losing the fast
        // round trip; the host handle lets it leave by itself when this
        // process ends without Stop (crash, killed harness). Both are given to
        // the target with SYNCHRONIZE only and closed by the worker as it ends.
        HANDLE target = Process::DryRun() ? nullptr : ::OpenProcess(PROCESS_DUP_HANDLE, FALSE, Raw(m_process->Id()));
        std::uint64_t remoteWake = 0;
        std::uint64_t remoteHost = 0;
        const std::uint64_t wait = SystemExport(*m_process, "WaitForMultipleObjects");
        const std::uint64_t close = SystemExport(*m_process, "CloseHandle");
        if (target && wait && close)
        {
            m_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE wake = nullptr;
            HANDLE host = nullptr;
            if (m_wake && ::DuplicateHandle(::GetCurrentProcess(), m_wake, target, &wake, SYNCHRONIZE, FALSE, 0))
                remoteWake = reinterpret_cast<std::uint64_t>(wake);
            if (remoteWake && ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), target, &host, SYNCHRONIZE, FALSE, 0))
                remoteHost = reinterpret_cast<std::uint64_t>(host);
            if (remoteWake && remoteHost)
            {
                put(kWaitOffset, wait);
                put(kWakeOffset, remoteWake);
                put(kHostOffset, remoteHost);
                put(kCloseOffset, close);
            }
            else
            {
                CloseRemoteHandle(target, remoteWake);
                remoteWake = 0;
                if (m_wake)
                    ::CloseHandle(m_wake);
                m_wake = nullptr;
            }
        }
        const auto fail = [&](Status failure)
            {
                CloseRemoteHandle(target, remoteWake);
                CloseRemoteHandle(target, remoteHost);
                if (target)
                    ::CloseHandle(target);
                Stop();
                return failure;
            };

        std::vector<std::uint8_t> drain = DrainCode();
        if (worker.size() > kCollectorOffset || collector.size() > kDrainOffset - kCollectorOffset || kDrainOffset + drain.size() > 0x1000)
            return fail(Status::Fail(ErrorCode::WriteFailed, "the remote worker code does not fit its page"));
        m_drain = m_code + kDrainOffset;
        if (!m_process->Write(m_code, worker.data(), worker.size()) ||
            !m_process->Write(m_collector, collector.data(), collector.size()) ||
            !m_process->Write(m_drain, drain.data(), drain.size()) ||
            !m_process->Write(m_control, control.data(), control.size()))
            return fail(Status::Fail(ErrorCode::WriteFailed, "could not set up the remote worker"));

        if (Status thread = m_process->CreateThread(m_code, m_control, false); !thread)
            return fail(thread);
        if (target)
            ::CloseHandle(target);
        m_running = true;
        return Status::Success();
    }

    void RemoteCall::Wake() const
    {
        if (m_wake)
            ::SetEvent(m_wake);
    }

    void RemoteCall::Stop()
    {
        bool stopped = false;
        if (m_process && m_running && m_control)
        {
            const std::uint64_t stop = 2;
            m_process->Write(m_control + kRequestOffset, &stop, sizeof(stop));
            Wake();
            // Wait until the worker has left its loop (it writes Stopped right
            // before returning), so a call in flight finishes before this side
            // drops its handles and stops reading the block.
            for (int tries = 0; tries < 100 && !stopped; ++tries)
            {
                std::uint64_t value = 0;
                if (!m_process->ReadExact(m_control + kStoppedOffset, &value, sizeof(value)))
                    break;
                stopped = value != 0;
                if (!stopped)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        // Nothing here is freed, on purpose. The code page still holds the
        // worker's last instructions (it pops and returns after it wrote
        // Stopped), rsi holds the control block for the worker's whole life,
        // and scratch addresses were handed to the target as pointers. Four
        // small pages per attach are cheaper than proving each one is no longer
        // reachable. (The Unity detach crash once blamed on freeing these was a
        // GC handle cut to 32 bits, see UnityExplorer::PointerHandles.)
        if (m_wake)
            ::CloseHandle(m_wake);
        m_wake = nullptr;
        m_code = m_control = m_data = m_collector = m_drain = m_drainBlock = 0;
        m_dataUsed = 0;
        m_running = false;
        m_process.reset();
    }

    Status RemoteCall::Call(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t& result)
    {
        result = 0;
        if (!m_process || !m_running || !function)
            return Status::Fail(ErrorCode::NoProcess);
        std::uint64_t pending = 0;
        if (!m_process->ReadExact(m_control + kRequestOffset, &pending, sizeof(pending)))
            return Status::Fail(ErrorCode::ReadFailed);
        if (pending != 0)
            return Status::Fail(ErrorCode::MonoCallFailed, "the previous call has not returned yet");
        std::uint64_t block[6] = { 1, function, a0, a1, a2, a3 };
        // Write func and args first (offsets 0x08..0x30), then flip Request to 1 last.
        if (!m_process->Write(m_control + kFuncOffset, &block[1], sizeof(std::uint64_t) * 5))
            return Status::Fail(ErrorCode::WriteFailed);
        const std::uint64_t go = 1;
        if (!m_process->Write(m_control + kRequestOffset, &go, sizeof(go)))
            return Status::Fail(ErrorCode::WriteFailed);
        // A spinning worker picks the request up by itself and the event just
        // stays set; a waiting one wakes at once instead of after a timer tick.
        Wake();
        return WaitForResult(m_control, result);
    }

    Status RemoteCall::WaitForResult(std::uint64_t control, std::uint64_t& result)
    {
        // Spin (the worker answers in microseconds) and only fall back to
        // sleeping after a grace period, so enumerating tens of thousands of
        // members stays fast; the deadline still guards a hung call.
        const auto deadline = std::chrono::steady_clock::now() + m_timeout;
        for (std::uint64_t spins = 0;; ++spins)
        {
            std::uint64_t request = 1;
            if (!m_process->ReadExact(control + kRequestOffset, &request, sizeof(request)))
                return Status::Fail(ErrorCode::ReadFailed);
            if (request == 0)
                break;
            if (!m_process->Alive())
                return Status::Fail(ErrorCode::DebugTargetExited);
            if (std::chrono::steady_clock::now() > deadline)
                return Status::Fail(ErrorCode::MonoCallFailed, "the call did not return within " + std::to_string(m_timeout.count()) + " ms");
            if (spins < 40000)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!m_process->ReadExact(control + kResultOffset, &result, sizeof(result)))
            return Status::Fail(ErrorCode::ReadFailed);
        return Status::Success();
    }

    std::uint64_t RemoteCall::Scratch(std::size_t bytes)
    {
        if (!m_process || !m_data)
            return 0;
        const std::uint64_t aligned = (bytes + 15) & ~std::uint64_t(15);
        if (m_dataUsed + aligned > kScratchSize)
            return 0;
        const std::uint64_t at = m_data + m_dataUsed;
        std::vector<std::uint8_t> zero(static_cast<std::size_t>(aligned), 0);
        if (!m_process->Write(at, zero.data(), zero.size()))
            return 0;
        m_dataUsed += aligned;
        return at;
    }

    void RemoteCall::ResetScratch()
    {
        m_dataUsed = 0;
    }

    Status RemoteCall::Drain(std::uint64_t function, std::uint64_t object, std::vector<DrainItem>& items, std::uint32_t peekOffset, std::uint64_t peekFunction, std::size_t limit)
    {
        items.clear();
        if (!m_process || !m_running || !m_drain || !m_drainBlock || !function)
            return Status::Fail(ErrorCode::NoProcess);
        // Its own block, apart from the scratch arena: only the header is
        // written per walk, and the caller's scratch regions stay as they are.
        const std::uint64_t block = m_drainBlock;
        const std::uint64_t header[6] = { 0, kDrainBatch, 0, peekOffset == kNoPeek ? ~std::uint64_t(0) : peekOffset, 0, peekFunction };
        Status status = m_process->Write(block, header, sizeof(header)) ? Status::Success() : Status::Fail(ErrorCode::WriteFailed);
        std::vector<DrainItem> batch;
        while (status && items.size() < limit)
        {
            std::uint64_t count = 0;
            if (status = Call(m_drain, function, object, block, 0, count); !status)
                break;
            std::uint64_t state[6] = {};
            if (!m_process->ReadExact(block, state, sizeof(state)))
            {
                status = Status::Fail(ErrorCode::ReadFailed);
                break;
            }
            count = std::min<std::uint64_t>(state[0], kDrainBatch);
            batch.resize(static_cast<std::size_t>(count));
            if (count && !m_process->ReadExact(block + kDrainHeader, batch.data(), batch.size() * sizeof(DrainItem)))
            {
                status = Status::Fail(ErrorCode::ReadFailed);
                break;
            }
            items.insert(items.end(), batch.begin(), batch.end());
            if (state[4] != 0 || count == 0)
                break;
            const std::uint64_t zero = 0;
            if (!m_process->Write(block, &zero, sizeof(zero)))
                status = Status::Fail(ErrorCode::WriteFailed);
        }
        if (items.size() > limit)
            items.resize(limit);
        return status;
    }

    std::string RemoteCall::ReadCString(std::uint64_t address, std::size_t limit) const
    {
        if (!m_process || !address)
            return {};
        std::string out;
        std::uint8_t buffer[64];
        while (out.size() < limit)
        {
            const std::size_t want = std::min(sizeof(buffer), limit - out.size());
            const std::size_t got = m_process->Read(address + out.size(), buffer, want);
            if (got == 0)
                break;
            const auto* end = static_cast<const std::uint8_t*>(std::memchr(buffer, 0, got));
            out.append(reinterpret_cast<const char*>(buffer), end ? static_cast<std::size_t>(end - buffer) : got);
            if (end || got < want)
                break;
        }
        return out;
    }
}
