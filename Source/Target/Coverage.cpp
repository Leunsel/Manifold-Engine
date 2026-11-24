#include "Target/Coverage.h"

#include "Target/AgentLife.h"

#include "Core/Handle.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/CoverageProtocol.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <shared_mutex>
#include <utility>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint32_t kCallTimeoutMs = 15000;
        constexpr std::size_t kTableGrain = 4096;

        Status Failed(std::string_view detail)
        {
            return Status::Fail(ErrorCode::CodeFilterFailed, detail);
        }

        template <class T>
        Status WriteField(const Process& process, std::uint64_t block, std::size_t offset, const T& value)
        {
            return process.Write(block + offset, &value, sizeof(T));
        }

        std::wstring AgentFile(bool is64Bit)
        {
            return is64Bit ? L"ManifoldAgent.dll" : L"ManifoldAgent32.dll";
        }

        const char* ResultText(std::uint32_t result)
        {
            switch (result)
            {
            case Cover::ResultBadTable:
                return "the table of addresses was not valid";
            case Cover::ResultNoHandler:
                return "the library could not add its exception handler";
            case Cover::ResultUnknownCall:
                return "the library in the process is from another version of the program";
            case Cover::ResultStillInside:
                return "a thread was still inside the library's exception handler";
            case Cover::ResultStillArmed:
                return "breakpoints could not be taken out of the code (its protection changed); they stay handled until the process ends";
            default:
                return "the library reported an error";
            }
        }
    }

    // The original bytes of the armed int3s, for every reader of the process,
    // and the check that releases them before a write lands on one.
    class Coverage::Shadow final : public ByteShadow
    {
    public:
        explicit Shadow(Coverage& owner) : m_owner(&owner) {}

        void Set(std::vector<std::pair<std::uint64_t, std::uint8_t>> bytes)
        {
            std::unique_lock lock(m_lock);
            m_bytes = std::move(bytes);
        }

        void Clear()
        {
            std::unique_lock lock(m_lock);
            m_bytes.clear();
        }

        void Detach()
        {
            std::unique_lock lock(m_lock);
            m_bytes.clear();
            m_owner = nullptr;
        }

        void Drop(std::uint64_t begin, std::uint64_t end)
        {
            std::unique_lock lock(m_lock);
            const auto first = std::lower_bound(m_bytes.begin(), m_bytes.end(), begin, [](const auto& entry, std::uint64_t at) { return entry.first < at; });
            auto last = first;
            while (last != m_bytes.end() && last->first < end)
                ++last;
            m_bytes.erase(first, last);
        }

        bool Contains(std::uint64_t address) const
        {
            std::shared_lock lock(m_lock);
            const auto found = std::lower_bound(m_bytes.begin(), m_bytes.end(), address, [](const auto& entry, std::uint64_t at) { return entry.first < at; });
            return found != m_bytes.end() && found->first == address;
        }

        std::size_t Size() const
        {
            std::shared_lock lock(m_lock);
            return m_bytes.size();
        }

        void Show(std::uint64_t address, std::uint8_t* buffer, std::size_t size) const override
        {
            const std::uint64_t end = address + size;
            std::shared_lock lock(m_lock);
            if (m_bytes.empty() || end <= m_bytes.front().first || address > m_bytes.back().first)
                return;
            auto it = std::lower_bound(m_bytes.begin(), m_bytes.end(), address, [](const auto& entry, std::uint64_t at) { return entry.first < at; });
            for (; it != m_bytes.end() && it->first < end; ++it)
                buffer[it->first - address] = it->second;
        }

        void BeforeWrite(std::uint64_t address, std::size_t size) const override
        {
            thread_local bool inside = false;
            if (inside)
                return;
            Coverage* owner = nullptr;
            {
                std::shared_lock lock(m_lock);
                const auto it = std::lower_bound(m_bytes.begin(), m_bytes.end(), address, [](const auto& entry, std::uint64_t at) { return entry.first < at; });
                if (it == m_bytes.end() || it->first >= address + size)
                    return;
                owner = m_owner;
            }
            if (!owner)
                return;
            inside = true;
            if (Status released = owner->ReleaseRange(address, address + size); !released)
                LOG_WARN("CodeFilter", "The code filter's breakpoints under a write at " + Hex(address) + " could not be taken back first: " + released.Text);
            inside = false;
        }

    private:
        Coverage* m_owner = nullptr;
        mutable std::shared_mutex m_lock;
        std::vector<std::pair<std::uint64_t, std::uint8_t>> m_bytes;
    };

    const char* CoverSkipText(CoverSkip skip)
    {
        switch (skip)
        {
        case CoverSkip::None:
            return "";
        case CoverSkip::AlreadyInt3:
            return "already an int3";
        case CoverSkip::Unreadable:
            return "not readable";
        case CoverSkip::Protect:
            return "the page could not be made writable";
        case CoverSkip::System:
            return "system or agent code";
        case CoverSkip::Unsorted:
            return "the table was not sorted";
        case CoverSkip::NotCode:
            return "not executable memory";
        }
        return "";
    }

    Coverage::~Coverage()
    {
        Leave();
    }

    std::string Coverage::AgentName() const
    {
        return WideToUtf8(AgentFile(!m_process || m_process->Is64Bit()));
    }

    std::uint64_t Coverage::BlockAddress() const
    {
        std::lock_guard lock(m_lock);
        return m_block;
    }

    bool Coverage::Usable() const
    {
        return m_process && m_process->Alive();
    }

    bool Coverage::Installed() const
    {
        std::lock_guard lock(m_lock);
        return m_installed;
    }

    bool Coverage::Armed() const
    {
        std::lock_guard lock(m_lock);
        return m_armed;
    }

    void Coverage::Forget()
    {
        if (m_shadow)
        {
            m_shadow->Detach();
            if (m_process)
                m_process->SetShadow(nullptr);
            m_shadow.reset();
            std::lock_guard guard(m_shadowGuard);
            m_shadowSeen.reset();
        }
        m_process.reset();
        m_block = 0;
        m_call = 0;
        m_table = 0;
        m_capacity = 0;
        m_count = 0;
        m_serial = 0;
        m_installed = false;
        m_armed = false;
    }

    void Coverage::Attach(std::shared_ptr<Process> process)
    {
        std::lock_guard lock(m_lock);
        Forget();
        m_process = std::move(process);
        if (!m_process)
            return;
        m_shadow = std::make_shared<Shadow>(*this);
        m_process->SetShadow(m_shadow);
        {
            std::lock_guard guard(m_shadowGuard);
            m_shadowSeen = m_shadow;
        }
        if (!Usable() || Process::DryRun())
            return;
        if (Adopt())
            LOG_INFO("CodeFilter", "The code filter library of an earlier session in " + m_process->Name() + " was taken over; its breakpoints are cleared");
    }

    void Coverage::Leave()
    {
        std::lock_guard lock(m_lock);
        if (m_installed && Usable() && !Process::DryRun())
        {
            if (Status released = ReleaseLocked(); !released)
                LOG_WARN("CodeFilter", released.Text);
        }
        Forget();
    }

    Status Coverage::Call(std::uint32_t operation)
    {
        if (!m_call)
            return Failed("the agent is not loaded");
        m_serial += 1;
        if (Status written = WriteField(*m_process, m_block, offsetof(Cover::Block, Serial), m_serial); !written)
            return written;
        if (Status called = m_process->CreateThread(m_call, operation, true, kCallTimeoutMs); !called)
            return called;
        std::uint32_t applied = 0;
        std::uint32_t result = 0;
        if (!m_process->ReadExact(m_block + offsetof(Cover::Block, Applied), &applied, sizeof(applied)) ||
            !m_process->ReadExact(m_block + offsetof(Cover::Block, Result), &result, sizeof(result)))
            return Failed("the state of the library could not be read");
        if (applied != m_serial)
            return Failed("the library did not take the request");
        if (result != Cover::ResultOk)
            return Failed(ResultText(result));
        return Status::Success();
    }

    Status Coverage::Watch()
    {
        const UniqueHandle duplicator(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, Raw(m_process->Id())));
        HANDLE remote = nullptr;
        if (!duplicator || !::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), duplicator.Get(), &remote, SYNCHRONIZE, FALSE, 0))
            return Failed("the process cannot watch Manifold Engine (" + SystemErrorText(::GetLastError()) + ")");
        const std::uint64_t value = reinterpret_cast<std::uintptr_t>(remote);
        if (Status written = WriteField(*m_process, m_block, offsetof(Cover::Block, HostHandle), value); !written)
            return written;
        return Call(Cover::CallWatch);
    }

    bool Coverage::Adopt()
    {
        const std::string name = AgentName();
        if (!m_process->FindModule(name))
            return false;
        const std::uint64_t block = m_process->FindExport(name, "ManifoldCover");
        const std::uint64_t call = m_process->FindExport(name, "ManifoldCoverCall");
        Cover::Block shared{};
        if (!block || !call || !m_process->ReadExact(block, &shared, sizeof(shared)) || shared.Magic != Cover::kMagic || shared.Version != Cover::kVersion)
            return false;
        if (!shared.Handler && !shared.Active)
            return false;
        m_block = block;
        m_call = call;
        m_serial = shared.Serial;
        m_installed = true;
        if (Status disarmed = Call(Cover::CallDisarm); !disarmed)
            LOG_WARN("CodeFilter", disarmed.Text);
        if (Status watched = Watch(); !watched)
            LOG_WARN("CodeFilter", watched.Text);
        return true;
    }

    Status Coverage::Install()
    {
        const std::string name = AgentName();
        if (Status loaded = LoadAgent(*m_process); !loaded)
            return loaded;
        m_block = m_process->FindExport(name, "ManifoldCover");
        m_call = m_process->FindExport(name, "ManifoldCoverCall");
        Cover::Block shared{};
        if (!m_block || !m_call || !m_process->ReadExact(m_block, &shared, sizeof(shared)) || shared.Magic != Cover::kMagic || shared.Version != Cover::kVersion)
        {
            m_block = 0;
            m_call = 0;
            return Failed("the agent library in the process is from another version of the program; restart the game to load the new one");
        }
        m_serial = shared.Serial;
        m_installed = true;
        if (Status watched = Watch(); !watched)
            LOG_WARN("CodeFilter", watched.Text + ". If Manifold Engine ends unexpectedly, the breakpoints stay until the process ends.");
        return Status::Success();
    }

    Status Coverage::Reserve(std::size_t count)
    {
        if (count <= m_capacity && m_table)
            return Status::Success();
        const std::size_t capacity = (count + kTableGrain - 1) / kTableGrain * kTableGrain;
        const Result<Address> allocated = m_process->Allocate(capacity * sizeof(Cover::Entry), PAGE_READWRITE);
        if (!allocated)
            return allocated.Error();
        if (m_table)
            m_process->Free(m_table);
        m_table = *allocated;
        m_capacity = capacity;
        return Status::Success();
    }

    Status Coverage::Arm(std::span<const std::uint64_t> addresses)
    {
        std::lock_guard lock(m_lock);
        if (!Usable())
            return Status::Fail(ErrorCode::NoProcess);
        if (Process::DryRun())
            return Failed("a dry run does not change code");
        std::vector<std::uint64_t> sorted(addresses.begin(), addresses.end());
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        sorted.erase(std::remove(sorted.begin(), sorted.end(), 0ull), sorted.end());
        if (sorted.size() > Cover::kMaxEntries)
            return Failed(Format("%zu addresses are more than the %u one round can hold", sorted.size(), Cover::kMaxEntries));
        if (!m_installed)
        {
            if (Status installed = Install(); !installed)
                return installed;
        }
        if (m_armed)
        {
            if (Status disarmed = Call(Cover::CallDisarm); !disarmed)
                return disarmed;
            m_armed = false;
            m_shadow->Clear();
        }
        if (Status reserved = Reserve(std::max<std::size_t>(sorted.size(), 1)); !reserved)
            return reserved;
        std::vector<Cover::Entry> table(sorted.size());
        for (std::size_t i = 0; i < sorted.size(); ++i)
        {
            std::memset(&table[i], 0, sizeof(Cover::Entry));
            table[i].Address = sorted[i];
        }
        if (!table.empty())
        {
            if (Status written = m_process->Write(m_table, table.data(), table.size() * sizeof(Cover::Entry)); !written)
                return written;
        }
        const std::uint32_t count = static_cast<std::uint32_t>(table.size());
        if (Status written = WriteField(*m_process, m_block, offsetof(Cover::Block, Table), m_table); !written)
            return written;
        if (Status written = WriteField(*m_process, m_block, offsetof(Cover::Block, Count), count); !written)
            return written;
        m_count = count;
        const Status armed = Call(Cover::CallArm);
        m_armed = count != 0;
        ShowArmed();
        if (!armed)
            return armed;
        return Status::Success();
    }

    Status Coverage::Disarm()
    {
        std::lock_guard lock(m_lock);
        if (!m_installed || !Usable() || !m_armed)
            return Status::Success();
        if (Status disarmed = Call(Cover::CallDisarm); !disarmed)
            return disarmed;
        m_armed = false;
        m_shadow->Clear();
        return Status::Success();
    }

    void Coverage::ShowArmed()
    {
        std::vector<std::pair<std::uint64_t, std::uint8_t>> bytes;
        if (const Result<std::vector<CoverEntry>> entries = EntriesLocked())
        {
            for (const CoverEntry& entry : *entries)
            {
                if (entry.State == CoverState::Armed || entry.State == CoverState::Restoring || entry.State == CoverState::Hit)
                    bytes.emplace_back(entry.Address, entry.Original);
            }
        }
        m_shadow->Set(std::move(bytes));
    }

    Status Coverage::ReleaseRange(std::uint64_t begin, std::uint64_t end)
    {
        std::lock_guard lock(m_lock);
        if (m_installed && m_armed && Usable())
        {
            if (Status written = WriteField(*m_process, m_block, offsetof(Cover::Block, RangeBegin), begin); !written)
                return written;
            if (Status written = WriteField(*m_process, m_block, offsetof(Cover::Block, RangeEnd), end); !written)
                return written;
            if (Status released = Call(Cover::CallReleaseRange); !released)
                return released;
        }
        if (m_shadow)
            m_shadow->Drop(begin, end);
        return Status::Success();
    }

    bool Coverage::Owns(std::uint64_t address) const
    {
        std::shared_ptr<Shadow> shadow;
        {
            std::lock_guard guard(m_shadowGuard);
            shadow = m_shadowSeen;
        }
        return shadow && shadow->Contains(address);
    }

    Status Coverage::ReleaseLocked()
    {
        Status result = Call(Cover::CallRelease);
        if (!result)
        {
            ShowArmed();  // int3s that stayed keep their table and their shadow
            return result;
        }
        m_armed = false;
        m_shadow->Clear();
        const std::uint64_t none = 0;
        const std::uint32_t zero = 0;
        WriteField(*m_process, m_block, offsetof(Cover::Block, Table), none);
        WriteField(*m_process, m_block, offsetof(Cover::Block, Count), zero);
        if (m_table)
            m_process->Free(m_table);
        m_table = 0;
        m_capacity = 0;
        m_count = 0;
        m_installed = false;
        m_block = 0;
        m_call = 0;
        return Status::Success();
    }

    Status Coverage::Release()
    {
        std::lock_guard lock(m_lock);
        if (!m_installed || !Usable() || Process::DryRun())
            return Status::Success();
        return ReleaseLocked();
    }

    Result<std::vector<CoverEntry>> Coverage::EntriesLocked() const
    {
        std::vector<CoverEntry> entries;
        if (!m_installed || !m_table || m_count == 0)
            return entries;
        std::vector<Cover::Entry> raw(m_count);
        if (!m_process->ReadExact(m_table, raw.data(), raw.size() * sizeof(Cover::Entry)))
            return Failed("the table of addresses could not be read");
        entries.reserve(raw.size());
        for (const Cover::Entry& entry : raw)
        {
            CoverEntry out;
            out.Address = entry.Address;
            out.State = static_cast<CoverState>(entry.State);
            out.Skip = static_cast<CoverSkip>(entry.Skip);
            out.ThreadId = entry.ThreadId;
            out.Original = entry.Original;
            entries.push_back(out);
        }
        return entries;
    }

    Result<std::vector<CoverEntry>> Coverage::Entries() const
    {
        std::lock_guard lock(m_lock);
        return EntriesLocked();
    }

    Result<std::vector<std::uint64_t>> Coverage::Hits() const
    {
        std::lock_guard lock(m_lock);
        const Result<std::vector<CoverEntry>> entries = EntriesLocked();
        if (!entries)
            return entries.Error();
        std::vector<std::uint64_t> hits;
        for (const CoverEntry& entry : *entries)
        {
            if (entry.State == CoverState::Hit)
                hits.push_back(entry.Address);
        }
        return hits;
    }

    Result<CoverCounts> Coverage::Counts() const
    {
        std::lock_guard lock(m_lock);
        CoverCounts counts;
        if (!m_installed || !m_block)
            return counts;
        Cover::Block shared{};
        if (!m_process->ReadExact(m_block, &shared, sizeof(shared)))
            return Failed("the state of the library could not be read");
        counts.Hits = shared.Hits;
        counts.Foreign = shared.Foreign;
        counts.Stale = shared.Stale;
        counts.Armed = shared.Armed;
        counts.Skipped = shared.Skipped;
        counts.Active = shared.Active != 0;
        counts.Watching = shared.Watching != 0;
        counts.HostLost = shared.HostLost != 0;
        return counts;
    }
}
