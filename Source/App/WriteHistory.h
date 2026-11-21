#pragma once

#include "Core/Errors.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Manifold
{
    namespace Target
    {
        class Process;
    }

    struct MemoryWrite
    {
        int Id = 0;
        std::uint64_t Generation = 0;
        std::uint64_t Address = 0;
        std::vector<std::uint8_t> Original;
        std::vector<std::uint8_t> Written;
        std::string Source;
        std::chrono::system_clock::time_point At;
        bool Undone = false;
    };

    enum class WriteState
    {
        InMemory,
        Undone,
        ChangedSince,
        Unreadable
    };

    // How the history reads and writes around a debugger's software breakpoints. Without it the
    // history would record an int3 as the original byte and wipe inserted int3s with its writes.
    struct WriteGuard
    {
        std::function<bool(const Target::Process&, std::uint64_t, std::uint8_t*, std::size_t)> Read;
        std::function<Status(const Target::Process&, std::uint64_t, std::span<const std::uint8_t>)> Write;
    };

    class WriteHistory
    {
    public:
        static constexpr std::size_t kLimit = 500;
        static constexpr std::size_t kByteLimit = 256u * 1024u * 1024u;

        Status Write(const Target::Process& process, std::uint64_t generation, std::uint64_t address, std::span<const std::uint8_t> bytes, std::string source);
        void Amend(int id, std::span<const std::uint8_t> written);
        Status WriteOrAmend(const Target::Process& process, std::uint64_t generation, int id, std::uint64_t address, std::span<const std::uint8_t> bytes, std::string source);
        Status Undo(const Target::Process& process, std::uint64_t generation, int id);
        Status Redo(const Target::Process& process, std::uint64_t generation, int id);
        std::optional<int> BlockedBy(const MemoryWrite& write) const;
        std::optional<int> LastUndoable(std::uint64_t generation) const;
        std::size_t Undoable(std::uint64_t generation) const;
        std::size_t Dropped(std::uint64_t generation) const;
        WriteState StateOf(const Target::Process& process, const MemoryWrite& write) const;
        const MemoryWrite* Find(int id) const;
        const std::deque<MemoryWrite>& Entries() const { return m_entries; }
        int LastId() const { return m_nextId - 1; }
        std::uint64_t Revision() const { return m_revision; }
        void Clear(std::uint64_t generation);
        void SetGuard(WriteGuard guard) { m_guard = std::move(guard); }

    private:
        Status Put(const Target::Process& process, std::uint64_t address, std::span<const std::uint8_t> bytes) const;
        bool ReadClean(const Target::Process& process, std::uint64_t address, std::uint8_t* data, std::size_t size) const;
        MemoryWrite* Entry(int id);
        void Trim();

        std::deque<MemoryWrite> m_entries;
        int m_nextId = 1;
        std::uint64_t m_revision = 0;
        std::uint64_t m_droppedGeneration = 0;
        std::size_t m_dropped = 0;
        WriteGuard m_guard;
    };
}
