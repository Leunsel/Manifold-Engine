#include "Target/CrashWatch.h"

#include "Core/Handle.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Process.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr DWORD kCallTimeoutMs = 5000;

        Status Failed(std::string_view detail)
        {
            return Status::Fail(ErrorCode::CrashWatchFailed, detail);
        }

        std::string AgentName(bool is64Bit)
        {
            return is64Bit ? "ManifoldAgent.dll" : "ManifoldAgent32.dll";
        }

        std::filesystem::path AgentPath(bool is64Bit)
        {
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            return std::filesystem::path(self).parent_path() / Utf8ToWide(AgentName(is64Bit));
        }

        const char* ResultText(std::uint32_t result)
        {
            switch (result)
            {
            case Crash::ResultNoMapping:
                return "the library could not map the shared ring";
            case Crash::ResultNoHandler:
                return "the library could not add its exception handler";
            case Crash::ResultUnknownCall:
                return "the library in the process is from another version of the program";
            case Crash::ResultStillInside:
                return "a thread was still inside the library's exception handler";
            default:
                return "the library reported an error";
            }
        }
    }

    CrashWatch::~CrashWatch()
    {
        Leave();
    }

    bool CrashWatch::Attach(std::shared_ptr<Process> process)
    {
        Leave();
        std::lock_guard lock(m_lock);
        m_process = std::move(process);
        if (!m_process || !m_process->Alive() || Process::DryRun())
            return false;
        const std::string name = AgentName(m_process->Is64Bit());
        if (!m_process->FindModule(name))
            return false;
        // ∑ The ring of the earlier session lives in that session's section, which
        //   went away with it: the caller starts a fresh one (in the background).
        const std::uint64_t block = m_process->FindExport(name, "ManifoldCrash");
        Crash::Block shared{};
        return block && m_process->ReadExact(block, &shared, sizeof(shared)) && shared.Magic == Crash::kMagic && shared.Version == Crash::kVersion && shared.Running;
    }

    void CrashWatch::Leave()
    {
        std::lock_guard lock(m_lock);
        ++m_session;
        if (m_running && m_process && m_process->Alive() && !Process::DryRun())
        {
            if (Status stopped = Invoke(*m_process, m_block, m_call, m_serial, Crash::CallStop); !stopped)
                LOG_WARN("Crash", stopped.Text);
        }
        m_running = false;
        Unmap();
        m_process.reset();
        m_modules.reset();
        m_block = 0;
        m_call = 0;
        m_serial = 0;
    }

    void CrashWatch::Unmap()
    {
        if (m_ring)
            ::UnmapViewOfFile(m_ring);
        m_ring = nullptr;
        if (m_mapping)
            ::CloseHandle(m_mapping);
        m_mapping = nullptr;
    }

    Status CrashWatch::Invoke(const Process& process, std::uint64_t block, std::uint64_t call, std::uint32_t& serial, std::uint32_t operation)
    {
        if (!call)
            return Failed("the agent is not loaded");
        serial += 1;
        if (Status written = process.Write(block + offsetof(Crash::Block, Serial), &serial, sizeof(serial)); !written)
            return written;
        if (Status called = process.CreateThread(call, operation, true, kCallTimeoutMs); !called)
            return called;
        std::uint32_t applied = 0;
        std::uint32_t result = 0;
        if (!process.ReadExact(block + offsetof(Crash::Block, Applied), &applied, sizeof(applied)) ||
            !process.ReadExact(block + offsetof(Crash::Block, Result), &result, sizeof(result)))
            return Failed("the state of the library could not be read");
        if (applied != serial)
            return Failed("the library did not take the request");
        if (result != Crash::ResultOk)
            return Failed(ResultText(result));
        return Status::Success();
    }

    // ∑ The slow part (loading the agent, the remote call that starts it: 2.4 s in
    //   Isaac, a few ms in the test target) runs without the lock, so Running and
    //   KeepModules on the main thread never wait for it; the result is taken over
    //   at the end, unless the process was closed or opened anew in between.
    Status CrashWatch::Start()
    {
        std::shared_ptr<Process> process;
        std::uint64_t session = 0;
        {
            std::lock_guard lock(m_lock);
            if (!m_process || !m_process->Alive())
                return Status::Fail(ErrorCode::NoProcess);
            if (Process::DryRun())
                return Failed("a dry run loads nothing into the process");
            if (m_running)
                return Status::Success();
            process = m_process;
            session = m_session;
        }
        const auto began = std::chrono::steady_clock::now();
        const auto since = [&began] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(); };
        bool injected = false;
        const bool is64Bit = process->Is64Bit();
        const std::string name = AgentName(is64Bit);
        if (!process->FindModule(name))
        {
            const std::filesystem::path agent = AgentPath(is64Bit);
            std::error_code ignore;
            if (!std::filesystem::exists(agent, ignore))
                return Status::Fail(ErrorCode::InjectFailed, name + " was not found next to the program");
            if (Status loading = process->InjectLibrary(agent.wstring()); !loading)
                return loading;
            injected = true;
        }
        const std::uint64_t block = process->FindExport(name, "ManifoldCrash");
        const std::uint64_t call = process->FindExport(name, "ManifoldCrashCall");
        Crash::Block shared{};
        if (!block || !call || !process->ReadExact(block, &shared, sizeof(shared)) || shared.Magic != Crash::kMagic || shared.Version != Crash::kVersion)
            return Failed("the agent library in the process is from another version of the program; restart the game to load the new one");
        std::uint32_t serial = shared.Serial;
        const double loaded = since();

        const HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(Crash::Ring)), nullptr);
        if (!mapping)
            return Failed("no shared memory for the ring (" + SystemErrorText(::GetLastError()) + ")");
        auto* ring = static_cast<Crash::Ring*>(::MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Crash::Ring)));
        const auto drop = [&]
            {
                if (ring)
                    ::UnmapViewOfFile(ring);
                ::CloseHandle(mapping);
            };
        if (!ring)
        {
            const DWORD error = ::GetLastError();
            drop();
            return Failed("the ring could not be mapped (" + SystemErrorText(error) + ")");
        }
        std::memset(ring, 0, sizeof(Crash::Ring));
        ring->Version = Crash::kVersion;
        ring->Magic = Crash::kMagic;
        const UniqueHandle target(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, Raw(process->Id())));
        HANDLE remote = nullptr;
        if (!target || !::DuplicateHandle(::GetCurrentProcess(), mapping, target.Get(), &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            const DWORD error = ::GetLastError();
            drop();
            return Failed("the ring cannot be handed to the process (" + SystemErrorText(error) + ")");
        }
        const std::uint64_t value = reinterpret_cast<std::uintptr_t>(remote);
        if (Status written = process->Write(block + offsetof(Crash::Block, Mapping), &value, sizeof(value)); !written)
        {
            drop();
            return written;
        }
        const double handed = since();
        if (Status started = Invoke(*process, block, call, serial, Crash::CallStart); !started)
        {
            drop();
            return started;
        }
        const double total = since();
        {
            std::lock_guard lock(m_lock);
            if (m_session != session || m_process != process)
            {
                drop();
                return Failed("the process was closed while the crash watch started");
            }
            Unmap();
            m_mapping = mapping;
            m_ring = ring;
            m_block = block;
            m_call = call;
            m_serial = serial;
            m_running = true;
            m_modules = process->Modules();
        }
        LOG_INFO("Crash", Format("Watching %s for crashes, to find the script behind one (%.0f ms: %s %.0f, ring %.0f, start call %.0f)", process->Name().c_str(), total,
                              injected ? "loading the agent" : "agent already there", loaded, handed - loaded, total - handed));
        return Status::Success();
    }

    void CrashWatch::KeepModules(ModuleList modules)
    {
        std::lock_guard lock(m_lock);
        if (modules && !modules->empty())
            m_modules = std::move(modules);
    }

    ModuleList CrashWatch::Modules() const
    {
        std::lock_guard lock(m_lock);
        return m_modules;
    }

    bool CrashWatch::Running() const
    {
        std::lock_guard lock(m_lock);
        return m_running;
    }

    CrashCapture CrashWatch::Capture() const
    {
        std::lock_guard lock(m_lock);
        CrashCapture capture;
        capture.Watching = m_running;
        if (!m_ring || m_ring->Magic != Crash::kMagic)
            return capture;
        capture.Is64Bit = m_ring->ProcessId ? m_ring->Is64Bit != 0 : (m_process ? m_process->Is64Bit() : true);
        const std::uint64_t next = m_ring->Next;
        capture.Noted = next;
        const std::uint64_t first = next > Crash::kSlots ? next - Crash::kSlots : 0;
        for (std::uint64_t number = first; number < next; ++number)
        {
            const Crash::Record& slot = m_ring->Slots[number % Crash::kSlots];
            if (slot.Sequence != static_cast<std::uint32_t>(number * 2 + 2))
                continue;
            CrashRecord record;
            record.Number = number;
            record.Code = slot.Code;
            record.Flags = slot.Flags;
            record.ThreadId = slot.ThreadId;
            record.Tick = slot.Tick;
            record.Address = slot.Address;
            record.Info.assign(slot.Info, slot.Info + std::min<std::uint32_t>(slot.InfoCount, 2));
            std::copy(std::begin(slot.Registers), std::end(slot.Registers), record.Registers.begin());
            record.Stack.assign(slot.Stack, slot.Stack + std::min(slot.StackCount, Crash::kStackWords));
            record.Bytes.assign(slot.Bytes, slot.Bytes + std::min(slot.ByteCount, Crash::kCodeBytes));
            capture.Records.push_back(std::move(record));
        }
        return capture;
    }
}
