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

        // The main-thread channel: a stub at kMainStubOffset and its control
        // block at kMainControlOffset, both in the code page.
        //
        // Want (set by this side) says the host wants the game's main thread;
        // Active (set by the stub) says the stub is serving. Active is a LATCH:
        // it stays set across frames until Want is cleared, so this side can see
        // it with ordinary cross-process reads. The stub itself returns to the
        // game as soon as no call is pending for kMainIdleCycles TSC ticks, so
        // it never holds a frame for long -- but it does NOT drop Active when it
        // does, because a flag that is only up for that sliver of a frame is
        // impossible to catch by polling from another process.
        // Request: 0 nothing pending, 1 call, 2 batch. A batch puts the record
        // array at Func (0x08) and its count at A0 (0x10); the stub counts down
        // in A1 (0x18). Each record is RemoteCall::kMainCallSize bytes:
        //   +0x00 function  +0x08..+0x20 a0..a3  +0x28 result
        //   +0x30 copy offset (u32)  +0x34 copy bytes (u32)  +0x38 slot
        //   +0x40 data (kMainCallData bytes)
        // After each call the stub copies `copy bytes` from result + offset
        // into data (skipped for a null result), still on the main thread.
        constexpr std::uint32_t kMainStubOffset = 0x300;
        constexpr std::uint32_t kMainControlOffset = 0x800;
        constexpr std::uint32_t kMainActiveOffset = 0x38;
        constexpr std::uint32_t kMainRunsOffset = 0x40;
        constexpr std::uint32_t kMainThreadOffset = 0x48;
        constexpr std::uint32_t kMainIdleOffset = 0x50;
        constexpr std::uint32_t kMainWantOffset = 0x60;
        constexpr std::uint32_t kMainControlSize = 0x68;
        // ∑ ~0.5 ms at 3 GHz. Long enough to bridge this side's own work
        // between two requests of one refresh (reading back a few dozen names
        // already took longer than the old 0.13 ms), each such gap otherwise
        // costing a whole frame; short enough that the game barely notices it
        // once at the end of a visit.
        constexpr std::uint64_t kMainIdleCycles = 1500000;

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

        // void stub(void), run by whatever thread the game calls it on. It
        // returns at once unless the host asked to enter; then it serves calls
        // like the worker until a leave request or an idle timeout.
        std::vector<std::uint8_t> MainCode(std::uint64_t control)
        {
            Emitter e;
            e.Bytes({ 0x56, 0x57, 0x53 });             // push rsi; push rdi; push rbx
            e.Bytes({ 0x48, 0x89, 0xE7 });             // mov rdi, rsp
            e.Bytes({ 0x48, 0x83, 0xE4, 0xF0 });       // and rsp, -16
            e.Bytes({ 0x48, 0x83, 0xEC, 0x20 });       // sub rsp, 0x20
            e.Bytes({ 0x48, 0xBE });                   // mov rsi, control
            for (int shift = 0; shift < 64; shift += 8)
                e.Byte(static_cast<std::uint8_t>(control >> shift));
            e.Bytes({ 0xF0, 0x48, 0xFF, 0x46, 0x40 }); // lock inc qword [rsi+0x40]  runs
            e.Bytes({ 0x65, 0x8B, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00 }); // mov eax, gs:[0x48]  thread id
            e.Bytes({ 0x48, 0x89, 0x46, 0x48 });       // mov [rsi+0x48], rax
            e.Bytes({ 0x48, 0x8B, 0x46, 0x60 });       // mov rax, [rsi+0x60]  want
            e.Bytes({ 0x48, 0x85, 0xC0 });             // test rax, rax
            e.Jump(0x84, "clear");                     // jz clear
            e.Bytes({ 0x48, 0xC7, 0x46, 0x38, 0x01, 0x00, 0x00, 0x00 }); // mov qword [rsi+0x38], 1  active (latch)
            e.Label("awake");
            e.Bytes({ 0x0F, 0x31 });                   // rdtsc
            e.Bytes({ 0x48, 0xC1, 0xE2, 0x20 });       // shl rdx, 32
            e.Bytes({ 0x48, 0x09, 0xD0 });             // or rax, rdx
            e.Bytes({ 0x48, 0x89, 0xC3 });             // mov rbx, rax  last activity
            e.Label("loop");
            e.Bytes({ 0x48, 0x8B, 0x06 });             // mov rax, [rsi]
            e.Bytes({ 0x48, 0x83, 0xF8, 0x01 });       // cmp rax, 1
            e.Jump(0x84, "call");                      // je call
            e.Bytes({ 0x48, 0x83, 0xF8, 0x02 });       // cmp rax, 2
            e.Jump(0x84, "batch");                     // je batch
            e.Bytes({ 0xF3, 0x90 });                   // pause
            e.Bytes({ 0x0F, 0x31 });                   // rdtsc
            e.Bytes({ 0x48, 0xC1, 0xE2, 0x20 });       // shl rdx, 32
            e.Bytes({ 0x48, 0x09, 0xD0 });             // or rax, rdx
            e.Bytes({ 0x48, 0x29, 0xD8 });             // sub rax, rbx
            e.Bytes({ 0x48, 0x3B, 0x46, 0x50 });       // cmp rax, [rsi+0x50]  idle limit
            e.Jump(0x82, "loop");                      // jb loop
            // Nothing pending for a while: give the frame back to the game but
            // KEEP Active set, so the next frame resumes serving right away.
            e.Jump(0xE9, "out");                       // jmp out
            e.Label("call");
            e.Bytes({ 0x48, 0x8B, 0x4E, 0x10 });       // mov rcx, [rsi+0x10]
            e.Bytes({ 0x48, 0x8B, 0x56, 0x18 });       // mov rdx, [rsi+0x18]
            e.Bytes({ 0x4C, 0x8B, 0x46, 0x20 });       // mov r8,  [rsi+0x20]
            e.Bytes({ 0x4C, 0x8B, 0x4E, 0x28 });       // mov r9,  [rsi+0x28]
            e.Bytes({ 0x48, 0x8B, 0x46, 0x08 });       // mov rax, [rsi+0x08]
            e.Bytes({ 0xFF, 0xD0 });                   // call rax
            e.Bytes({ 0x48, 0x89, 0x46, 0x30 });       // mov [rsi+0x30], rax
            e.Bytes({ 0xB8, 0x01, 0x00, 0x00, 0x00 }); // mov eax, 1
            e.Bytes({ 0x31, 0xC9 });                   // xor ecx, ecx
            e.Bytes({ 0xF0, 0x48, 0x0F, 0xB1, 0x0E }); // lock cmpxchg [rsi], rcx  call 1 -> 0
            e.Jump(0xE9, "awake");                     // jmp awake
            // ∑ A whole list in one visit: rbx walks the records (callee-saved,
            // and "awake" resets it to a timestamp afterwards); the count lives
            // in the control block so no further register needs saving.
            e.Label("batch");
            e.Bytes({ 0x48, 0x8B, 0x5E, 0x08 });       // mov rbx, [rsi+0x08]  records
            e.Bytes({ 0x48, 0x8B, 0x46, 0x10 });       // mov rax, [rsi+0x10]  count
            e.Bytes({ 0x48, 0x89, 0x46, 0x18 });       // mov [rsi+0x18], rax  remaining
            e.Label("next");
            e.Bytes({ 0x48, 0x83, 0x7E, 0x18, 0x00 }); // cmp qword [rsi+0x18], 0
            e.Jump(0x84, "batchdone");                 // je batchdone
            e.Bytes({ 0x48, 0x8B, 0x4B, 0x08 });       // mov rcx, [rbx+0x08]
            e.Bytes({ 0x48, 0x8B, 0x53, 0x10 });       // mov rdx, [rbx+0x10]
            e.Bytes({ 0x4C, 0x8B, 0x43, 0x18 });       // mov r8,  [rbx+0x18]
            e.Bytes({ 0x4C, 0x8B, 0x4B, 0x20 });       // mov r9,  [rbx+0x20]
            e.Bytes({ 0xFF, 0x13 });                   // call [rbx]
            e.Bytes({ 0x48, 0x89, 0x43, 0x28 });       // mov [rbx+0x28], rax  result
            e.Bytes({ 0x48, 0x85, 0xC0 });             // test rax, rax
            e.Jump(0x84, "skip");                      // jz skip
            e.Bytes({ 0x8B, 0x4B, 0x34 });             // mov ecx, [rbx+0x34]  copy bytes
            e.Bytes({ 0x85, 0xC9 });                   // test ecx, ecx
            e.Jump(0x84, "skip");                      // jz skip
            e.Bytes({ 0x56, 0x57 });                   // push rsi; push rdi
            e.Bytes({ 0x8B, 0x53, 0x30 });             // mov edx, [rbx+0x30]  copy offset
            e.Bytes({ 0x48, 0x8D, 0x34, 0x10 });       // lea rsi, [rax+rdx]
            e.Bytes({ 0x48, 0x8D, 0x7B, 0x40 });       // lea rdi, [rbx+0x40]  data
            e.Bytes({ 0xF3, 0xA4 });                   // rep movsb
            e.Bytes({ 0x5F, 0x5E });                   // pop rdi; pop rsi
            e.Label("skip");
            e.Bytes({ 0x48, 0x81, 0xC3, 0x80, 0x00, 0x00, 0x00 }); // add rbx, 0x80  next record
            e.Bytes({ 0x48, 0xFF, 0x4E, 0x18 });       // dec qword [rsi+0x18]
            e.Jump(0xE9, "next");                      // jmp next
            e.Label("batchdone");
            e.Bytes({ 0xB8, 0x02, 0x00, 0x00, 0x00 }); // mov eax, 2
            e.Bytes({ 0x31, 0xC9 });                   // xor ecx, ecx
            e.Bytes({ 0xF0, 0x48, 0x0F, 0xB1, 0x0E }); // lock cmpxchg [rsi], rcx  batch 2 -> 0
            e.Jump(0xE9, "awake");                     // jmp awake
            e.Label("clear");
            e.Bytes({ 0x48, 0xC7, 0x46, 0x38, 0x00, 0x00, 0x00, 0x00 }); // mov qword [rsi+0x38], 0  active off
            e.Label("out");
            e.Bytes({ 0x48, 0x89, 0xFC });             // mov rsp, rdi
            e.Bytes({ 0x5B, 0x5F, 0x5E });             // pop rbx; pop rdi; pop rsi
            e.Bytes({ 0xC3 });                         // ret
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
        m_control = controlBlock.ValueOr(0);
        m_data = dataBlock.ValueOr(0);
        if (!controlBlock || !dataBlock)
        {
            Stop();
            return controlBlock ? dataBlock.Error() : controlBlock.Error();
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

        std::vector<std::uint8_t> main = MainCode(m_code + kMainControlOffset);
        std::vector<std::uint8_t> mainControl(kMainControlSize, 0);
        std::memcpy(mainControl.data() + kMainIdleOffset, &kMainIdleCycles, sizeof(kMainIdleCycles));
        if (worker.size() > kCollectorOffset || collector.size() > kMainStubOffset - kCollectorOffset || main.size() > kMainControlOffset - kMainStubOffset)
            return fail(Status::Fail(ErrorCode::WriteFailed, "the remote worker code does not fit its page"));
        if (!m_process->Write(m_code, worker.data(), worker.size()) ||
            !m_process->Write(m_collector, collector.data(), collector.size()) ||
            !m_process->Write(m_code + kMainStubOffset, main.data(), main.size()) ||
            !m_process->Write(m_code + kMainControlOffset, mainControl.data(), mainControl.size()) ||
            !m_process->Write(m_control, control.data(), control.size()))
            return fail(Status::Fail(ErrorCode::WriteFailed, "could not set up the remote worker"));

        if (Status thread = m_process->CreateThread(m_code, m_control, false); !thread)
            return fail(thread);
        if (target)
            ::CloseHandle(target);
        m_thread = 1;
        return Status::Success();
    }

    void RemoteCall::Wake() const
    {
        if (m_wake)
            ::SetEvent(m_wake);
    }

    void RemoteCall::Stop()
    {
        if (m_process && m_code && m_thread)
            LeaveMain();
        bool stopped = false;
        if (m_process && m_thread && m_control)
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
        // and scratch addresses were handed to the target as pointers. Three
        // small pages per attach are cheaper than proving each one is no longer
        // reachable. (The Unity detach crash once blamed on freeing these was a
        // GC handle cut to 32 bits, see UnityExplorer::PointerHandles.)
        if (m_wake)
            ::CloseHandle(m_wake);
        m_wake = nullptr;
        m_code = m_control = m_data = m_collector = 0;
        m_dataUsed = 0;
        m_thread = 0;
        m_process.reset();
    }

    Status RemoteCall::Call(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t& result)
    {
        result = 0;
        if (!m_process || !m_thread || !function)
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
        return WaitForResult(m_control, false, result, nullptr);
    }

    Status RemoteCall::WaitForResult(std::uint64_t control, bool main, std::uint64_t& result, bool* notTaken)
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
            // The main stub only drops "inside" while it is not running a call,
            // so a request still pending then was never taken.
            if (main && (spins & 15) == 15 && !InsideMain())
            {
                std::uint64_t again = 1;
                if (m_process->ReadExact(control + kRequestOffset, &again, sizeof(again)) && again == 0)
                    break;
                if (notTaken)
                    *notTaken = true;
                return Status::Fail(ErrorCode::MonoCallFailed, "the game's main thread went on before the call was taken");
            }
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

    std::uint64_t RemoteCall::ReadMain(std::uint32_t offset) const
    {
        std::uint64_t value = 0;
        if (m_process && m_code)
            m_process->ReadExact(m_code + kMainControlOffset + offset, &value, sizeof(value));
        return value;
    }

    bool RemoteCall::InsideMain() const
    {
        return ReadMain(kMainActiveOffset) != 0;
    }

    std::uint64_t RemoteCall::MainRuns() const
    {
        return ReadMain(kMainRunsOffset);
    }

    std::uint32_t RemoteCall::MainThreadId() const
    {
        return static_cast<std::uint32_t>(ReadMain(kMainThreadOffset));
    }

    Status RemoteCall::EnterMain(std::chrono::milliseconds wait)
    {
        if (!m_process || !m_code || !m_thread)
            return Status::Fail(ErrorCode::NoProcess);
        if (InsideMain())
            return Status::Success();
        const std::uint64_t control = m_code + kMainControlOffset;
        const std::uint64_t want = 1;
        if (!m_process->Write(control + kMainWantOffset, &want, sizeof(want)))
            return Status::Fail(ErrorCode::WriteFailed);
        // The stub sets Active on the game's next frame and keeps it set, so
        // this poll cannot miss it however short the stub's own visit is.
        const auto deadline = std::chrono::steady_clock::now() + wait;
        for (int spins = 0;; ++spins)
        {
            if (InsideMain())
                return Status::Success();
            if (!m_process->Alive())
                return Status::Fail(ErrorCode::DebugTargetExited);
            if (std::chrono::steady_clock::now() > deadline || m_interrupt)
                break;
            if (spins < 200)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // The main thread never reached the stub: take the wish back, so it
        // does not start serving later behind our back.
        const std::uint64_t off = 0;
        m_process->Write(control + kMainWantOffset, &off, sizeof(off));
        if (InsideMain())
            return Status::Success();
        return Status::Fail(ErrorCode::MonoCallFailed, "the game's main thread did not run within " + std::to_string(wait.count()) +
            " ms (the game may be paused, loading, or stopped while it is in the background)");
    }

    Status RemoteCall::CallMain(std::uint64_t function, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t& result)
    {
        result = 0;
        if (!m_process || !m_code || !m_thread || !function)
            return Status::Fail(ErrorCode::NoProcess);
        const std::uint64_t block[5] = { function, a0, a1, a2, a3 };
        return SendMain(1, block, 5, result);
    }

    Status RemoteCall::CallMainBatch(std::uint64_t buffer, const std::vector<MainCall>& calls, std::vector<MainResult>& results)
    {
        results.clear();
        if (!m_process || !m_code || !m_thread || !buffer)
            return Status::Fail(ErrorCode::NoProcess);
        if (calls.empty())
            return Status::Success();
        std::vector<std::uint8_t> records(calls.size() * kMainCallSize, 0);
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            const MainCall& call = calls[i];
            if (!call.Function || call.CopyBytes > kMainCallData)
                return Status::Fail(ErrorCode::MonoCallFailed, "a main-thread batch entry has no function or copies more than " + std::to_string(kMainCallData) + " bytes");
            std::uint8_t* record = records.data() + i * kMainCallSize;
            std::memcpy(record + 0x00, &call.Function, 8);
            std::memcpy(record + 0x08, call.Args, sizeof(call.Args));
            std::memcpy(record + 0x30, &call.CopyOffset, 4);
            std::memcpy(record + 0x34, &call.CopyBytes, 4);
            std::memcpy(record + 0x40, call.Data.data(), kMainCallData);
        }
        if (!m_process->Write(buffer, records.data(), records.size()))
            return Status::Fail(ErrorCode::WriteFailed);
        const std::uint64_t block[3] = { buffer, calls.size(), calls.size() };
        std::uint64_t ignored = 0;
        if (Status sent = SendMain(2, block, 3, ignored); !sent)
            return sent;
        // One read brings every result, slot and copied value back.
        if (!m_process->ReadExact(buffer, records.data(), records.size()))
            return Status::Fail(ErrorCode::ReadFailed);
        results.resize(calls.size());
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            const std::uint8_t* record = records.data() + i * kMainCallSize;
            std::memcpy(&results[i].Result, record + 0x28, 8);
            std::memcpy(&results[i].Slot, record + 0x38, 8);
            std::memcpy(results[i].Data.data(), record + 0x40, kMainCallData);
        }
        return Status::Success();
    }

    Status RemoteCall::SendMain(std::uint64_t request, const std::uint64_t* block, std::size_t words, std::uint64_t& result)
    {
        const std::uint64_t control = m_code + kMainControlOffset;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!InsideMain())
            {
                if (Status entered = EnterMain(std::chrono::milliseconds(2000)); !entered)
                    return entered;
            }
            if (!m_process->Write(control + kFuncOffset, block, words * sizeof(std::uint64_t)))
                return Status::Fail(ErrorCode::WriteFailed);
            // A run counter that moved while the request was out means the stub
            // had given the frame back and served it only on the next one.
            const std::uint64_t runs = ReadMain(kMainRunsOffset);
            if (!m_process->Write(control + kRequestOffset, &request, sizeof(request)))
                return Status::Fail(ErrorCode::WriteFailed);
            bool notTaken = false;
            const Status done = WaitForResult(control, true, result, &notTaken);
            if (done && ReadMain(kMainRunsOffset) != runs)
                ++m_frameWaits;
            if (done || !notTaken)
                return done;
            // The stub went idle just before the request: it never ran, so it is
            // safe to enter again and send it once more.
        }
        return Status::Fail(ErrorCode::MonoCallFailed, "the game's main thread kept going on before the call was taken");
    }

    void RemoteCall::LeaveMain()
    {
        if (!m_process || !m_code)
            return;
        const std::uint64_t control = m_code + kMainControlOffset;
        const std::uint64_t off = 0;
        m_process->Write(control + kMainWantOffset, &off, sizeof(off));
        // The stub clears Active on its next run, which is a whole frame away,
        // so give it that long: a stale Active would make the next call wait
        // for a stub that is not serving. A game that is not rendering never
        // gets there at all, hence the bound.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        for (int tries = 0; InsideMain() && std::chrono::steady_clock::now() < deadline; ++tries)
        {
            if (tries < 200)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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
