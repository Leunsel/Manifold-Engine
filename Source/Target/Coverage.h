#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"
#include "Target/Process.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace Manifold::Target
{
    enum class CoverState : std::uint32_t
    {
        Idle = 0,
        Armed = 1,
        Restoring = 2,
        Hit = 3,
        Released = 4,
        Skipped = 5,
        Releasing = 6
    };

    enum class CoverSkip : std::uint32_t
    {
        None = 0,
        AlreadyInt3 = 1,
        Unreadable = 2,
        Protect = 3,
        System = 4,
        Unsorted = 5,
        NotCode = 6
    };

    struct CoverEntry
    {
        std::uint64_t Address = 0;
        CoverState State = CoverState::Idle;
        CoverSkip Skip = CoverSkip::None;
        std::uint32_t ThreadId = 0;
        std::uint8_t Original = 0;
    };

    struct CoverCounts
    {
        std::uint64_t Hits = 0;
        std::uint64_t Foreign = 0;
        std::uint64_t Stale = 0;
        std::uint32_t Armed = 0;
        std::uint32_t Skipped = 0;
        bool Active = false;
        bool Watching = false;
        bool HostLost = false;
    };

    const char* CoverSkipText(CoverSkip skip);

    // The code filter's part in the process: one-shot int3 breakpoints the
    // agent library sets, and takes back when a thread runs into one. The
    // host only hands over the addresses; it never writes the int3s itself.
    class Coverage
    {
    public:
        Coverage() = default;
        ~Coverage();

        Coverage(const Coverage&) = delete;
        Coverage& operator=(const Coverage&) = delete;

        void Attach(std::shared_ptr<Process> process);
        void Leave();

        Status Arm(std::span<const std::uint64_t> addresses);
        Status Disarm();
        Status Release();

        bool Installed() const;
        bool Armed() const;
        Result<std::vector<CoverEntry>> Entries() const;
        Result<std::vector<std::uint64_t>> Hits() const;
        Result<CoverCounts> Counts() const;
        std::string AgentName() const;
        std::uint64_t BlockAddress() const;

    private:
        Status Install();
        Status Watch();
        Status Call(std::uint32_t operation);
        Status Reserve(std::size_t count);
        Status ReleaseLocked();
        Result<std::vector<CoverEntry>> EntriesLocked() const;
        bool Adopt();
        bool Usable() const;
        void DropAgent();
        void Forget();

        mutable std::mutex m_lock;
        std::shared_ptr<Process> m_process;
        std::uint64_t m_block = 0;
        std::uint64_t m_call = 0;
        std::uint64_t m_table = 0;
        std::size_t m_capacity = 0;
        std::uint32_t m_count = 0;
        std::uint32_t m_serial = 0;
        bool m_installed = false;
        bool m_armed = false;
    };
}
