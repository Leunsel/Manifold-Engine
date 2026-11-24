#include "Target/CrashWatch.h"

#include "Core/Handle.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Process.h"

#include <algorithm>
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

    void CrashWatch::Attach(std::shared_ptr<Process> process)
    {
        Leave();
        bool adopt = false;
        {
            std::lock_guard lock(m_lock);
            m_process = std::move(process);
            if (!m_process || !m_process->Alive() || Process::DryRun())
                return;
            const std::string name = AgentName(m_process->Is64Bit());
            if (!m_process->FindModule(name))
                return;
            const std::uint64_t block = m_process->FindExport(name, "ManifoldCrash");
            Crash::Block shared{};
            adopt = block && m_process->ReadExact(block, &shared, sizeof(shared)) && shared.Magic == Crash::kMagic && shared.Version == Crash::kVersion && shared.Running;
        }
        // ∑ The ring of the earlier session lives in that session's section,
        //   which went away with it: a fresh one takes its place.
        if (adopt)
        {
            if (Status started = Start(); started)
                LOG_INFO("Crash", "The crash watch of an earlier session in " + m_process->Name() + " was taken over");
            else
                LOG_WARN("Crash", started.Text);
        }
    }

    void CrashWatch::Leave()
    {
        std::lock_guard lock(m_lock);
        if (m_running && m_process && m_process->Alive() && !Process::DryRun())
        {
            if (Status stopped = Call(Crash::CallStop); !stopped)
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

    Status CrashWatch::MapRing()
    {
        Unmap();
        const HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(Crash::Ring)), nullptr);
        if (!mapping)
            return Failed("no shared memory for the ring (" + SystemErrorText(::GetLastError()) + ")");
        void* view = ::MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Crash::Ring));
        if (!view)
        {
            const DWORD error = ::GetLastError();
            ::CloseHandle(mapping);
            return Failed("the ring could not be mapped (" + SystemErrorText(error) + ")");
        }
        m_mapping = mapping;
        m_ring = static_cast<Crash::Ring*>(view);
        std::memset(m_ring, 0, sizeof(Crash::Ring));
        m_ring->Version = Crash::kVersion;
        m_ring->Magic = Crash::kMagic;
        return Status::Success();
    }

    Status CrashWatch::Call(std::uint32_t operation)
    {
        if (!m_call)
            return Failed("the agent is not loaded");
        m_serial += 1;
        if (Status written = m_process->Write(m_block + offsetof(Crash::Block, Serial), &m_serial, sizeof(m_serial)); !written)
            return written;
        if (Status called = m_process->CreateThread(m_call, operation, true, kCallTimeoutMs); !called)
            return called;
        std::uint32_t applied = 0;
        std::uint32_t result = 0;
        if (!m_process->ReadExact(m_block + offsetof(Crash::Block, Applied), &applied, sizeof(applied)) ||
            !m_process->ReadExact(m_block + offsetof(Crash::Block, Result), &result, sizeof(result)))
            return Failed("the state of the library could not be read");
        if (applied != m_serial)
            return Failed("the library did not take the request");
        if (result != Crash::ResultOk)
            return Failed(ResultText(result));
        return Status::Success();
    }

    Status CrashWatch::Start()
    {
        std::lock_guard lock(m_lock);
        if (!m_process || !m_process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        if (Process::DryRun())
            return Failed("a dry run loads nothing into the process");
        const bool is64Bit = m_process->Is64Bit();
        const std::string name = AgentName(is64Bit);
        if (!m_process->FindModule(name))
        {
            const std::filesystem::path agent = AgentPath(is64Bit);
            std::error_code ignore;
            if (!std::filesystem::exists(agent, ignore))
                return Status::Fail(ErrorCode::InjectFailed, name + " was not found next to the program");
            if (Status injected = m_process->InjectLibrary(agent.wstring()); !injected)
                return injected;
        }
        m_block = m_process->FindExport(name, "ManifoldCrash");
        m_call = m_process->FindExport(name, "ManifoldCrashCall");
        Crash::Block shared{};
        if (!m_block || !m_call || !m_process->ReadExact(m_block, &shared, sizeof(shared)) || shared.Magic != Crash::kMagic || shared.Version != Crash::kVersion)
        {
            m_block = 0;
            m_call = 0;
            return Failed("the agent library in the process is from another version of the program; restart the game to load the new one");
        }
        m_serial = shared.Serial;
        if (Status mapped = MapRing(); !mapped)
            return mapped;
        const UniqueHandle target(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, Raw(m_process->Id())));
        HANDLE remote = nullptr;
        if (!target || !::DuplicateHandle(::GetCurrentProcess(), m_mapping, target.Get(), &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            const DWORD error = ::GetLastError();
            Unmap();
            return Failed("the ring cannot be handed to the process (" + SystemErrorText(error) + ")");
        }
        const std::uint64_t value = reinterpret_cast<std::uintptr_t>(remote);
        if (Status written = m_process->Write(m_block + offsetof(Crash::Block, Mapping), &value, sizeof(value)); !written)
        {
            Unmap();
            return written;
        }
        if (Status started = Call(Crash::CallStart); !started)
        {
            Unmap();
            return started;
        }
        m_running = true;
        m_modules = m_process->Modules();
        LOG_INFO("Crash", "Watching " + m_process->Name() + " for crashes, to find the script behind one");
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
