#include "Target/RemoteCall.h"

#include "Core/Text.h"
#include "Target/Process.h"

#include <chrono>
#include <cstring>
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
        constexpr std::uint32_t kRequestOffset = 0x00;
        constexpr std::uint32_t kFuncOffset = 0x08;
        constexpr std::uint32_t kResultOffset = 0x30;
        constexpr std::uint32_t kControlSize = 0x40;
        constexpr std::uint32_t kCollectorOffset = 0x200;
        constexpr std::uint64_t kScratchSize = 0x10000;

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
            e.Label("loop");
            e.Bytes({ 0x48, 0x8B, 0x06 });             // mov rax, [rsi]      request
            e.Bytes({ 0x48, 0x83, 0xF8, 0x02 });       // cmp rax, 2
            e.Jump(0x84, "stop");                      // je stop
            e.Bytes({ 0x48, 0x83, 0xF8, 0x01 });       // cmp rax, 1
            e.Jump(0x85, "loop");                      // jne loop
            e.Bytes({ 0x48, 0x8B, 0x4E, 0x10 });       // mov rcx, [rsi+0x10] a0
            e.Bytes({ 0x48, 0x8B, 0x56, 0x18 });       // mov rdx, [rsi+0x18] a1
            e.Bytes({ 0x4C, 0x8B, 0x46, 0x20 });       // mov r8,  [rsi+0x20] a2
            e.Bytes({ 0x4C, 0x8B, 0x4E, 0x28 });       // mov r9,  [rsi+0x28] a3
            e.Bytes({ 0x48, 0x8B, 0x46, 0x08 });       // mov rax, [rsi+0x08] func
            e.Bytes({ 0xFF, 0xD0 });                   // call rax
            e.Bytes({ 0x48, 0x89, 0x46, 0x30 });       // mov [rsi+0x30], rax result
            e.Bytes({ 0x31, 0xC0 });                   // xor eax, eax
            e.Bytes({ 0x48, 0x89, 0x06 });             // mov [rsi], rax     request = 0
            e.Jump(0xE9, "loop");                      // jmp loop
            e.Label("stop");
            e.Bytes({ 0x48, 0x89, 0xFC });             // mov rsp, rdi
            e.Bytes({ 0x5F });                         // pop rdi
            e.Bytes({ 0x5E });                         // pop rsi
            e.Bytes({ 0x31, 0xC0 });                   // xor eax, eax
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

        Status status;
        m_code = m_process->Allocate(0x1000, PAGE_EXECUTE_READWRITE, 0, status);
        if (!m_code)
            return status;
        m_control = m_process->Allocate(kControlSize, PAGE_READWRITE, 0, status);
        m_data = m_control ? m_process->Allocate(kScratchSize, PAGE_READWRITE, 0, status) : 0;
        if (!m_control || !m_data)
        {
            Stop();
            return status ? Status::Fail(ErrorCode::AllocFailed) : status;
        }

        std::vector<std::uint8_t> worker = WorkerCode();
        std::vector<std::uint8_t> collector = CollectorCode();
        m_collector = m_code + kCollectorOffset;
        std::vector<std::uint8_t> zero(kControlSize, 0);
        if (!m_process->Write(m_code, worker.data(), worker.size()) ||
            !m_process->Write(m_collector, collector.data(), collector.size()) ||
            !m_process->Write(m_control, zero.data(), zero.size()))
        {
            Stop();
            return Status::Fail(ErrorCode::WriteFailed, "could not set up the remote worker");
        }

        if (Status thread = m_process->CreateThread(m_code, m_control, false); !thread)
        {
            Stop();
            return thread;
        }
        m_thread = 1;
        return Status::Success();
    }

    void RemoteCall::Stop()
    {
        if (m_process && m_thread && m_control)
        {
            const std::uint64_t stop = 2;
            m_process->Write(m_control + kRequestOffset, &stop, sizeof(stop));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (m_process)
        {
            if (m_data) m_process->Free(m_data);
            if (m_control) m_process->Free(m_control);
            if (m_code) m_process->Free(m_code);
        }
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
        std::uint64_t block[6] = { 1, function, a0, a1, a2, a3 };
        // Write func and args first (offsets 0x08..0x30), then flip Request to 1 last.
        if (!m_process->Write(m_control + kFuncOffset, &block[1], sizeof(std::uint64_t) * 5))
            return Status::Fail(ErrorCode::WriteFailed);
        const std::uint64_t go = 1;
        if (!m_process->Write(m_control + kRequestOffset, &go, sizeof(go)))
            return Status::Fail(ErrorCode::WriteFailed);
        // Spin (the worker answers in microseconds) and only fall back to
        // sleeping after a grace period, so enumerating tens of thousands of
        // members stays fast; the deadline still guards a hung call.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (std::uint64_t spins = 0;; ++spins)
        {
            std::uint64_t request = 1;
            if (!m_process->ReadExact(m_control + kRequestOffset, &request, sizeof(request)))
                return Status::Fail(ErrorCode::ReadFailed);
            if (request == 0)
                break;
            if (!m_process->Alive())
                return Status::Fail(ErrorCode::DebugTargetExited);
            if (std::chrono::steady_clock::now() > deadline)
                return Status::Fail(ErrorCode::ThreadCreateFailed, "the remote call did not return");
            if (spins < 40000)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!m_process->ReadExact(m_control + kResultOffset, &result, sizeof(result)))
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
