#include "App/WriteHistory.h"

#include "Core/Text.h"
#include "Target/Process.h"

#include <algorithm>

namespace Manifold
{
    namespace
    {
        constexpr std::size_t kCompareBlock = 0x10000;

        bool Overlap(const MemoryWrite& a, const MemoryWrite& b)
        {
            return a.Generation == b.Generation && a.Address < b.Address + b.Written.size() && b.Address < a.Address + a.Written.size();
        }
    }

    Status WriteHistory::Write(const Target::Process& process, std::uint64_t generation, std::uint64_t address, std::span<const std::uint8_t> bytes, std::string source)
    {
        MemoryWrite write;
        write.Generation = generation;
        write.Address = address;
        write.Original.resize(bytes.size());
        if (!process.ReadExact(address, write.Original.data(), write.Original.size()))
            return Status::Fail(ErrorCode::ReadFailed, Hex(address));
        const Status status = process.Write(address, bytes);
        if (!status)
            return status;
        if (m_droppedGeneration != generation)
        {
            m_droppedGeneration = generation;
            m_dropped = 0;
        }
        std::erase_if(m_entries, [&](const MemoryWrite& entry) { return entry.Generation != generation; });
        write.Id = m_nextId++;
        write.Written.assign(bytes.begin(), bytes.end());
        write.Source = std::move(source);
        write.At = std::chrono::system_clock::now();
        m_entries.push_back(std::move(write));
        Trim();
        ++m_revision;
        return status;
    }

    void WriteHistory::Trim()
    {
        std::size_t held = 0;
        for (const MemoryWrite& entry : m_entries)
            held += entry.Original.size() + entry.Written.size();
        while (m_entries.size() > kLimit || (held > kByteLimit && m_entries.size() > 1))
        {
            held -= m_entries.front().Original.size() + m_entries.front().Written.size();
            m_entries.pop_front();
            ++m_dropped;
        }
    }

    void WriteHistory::Amend(int id, std::span<const std::uint8_t> written)
    {
        if (MemoryWrite* write = Entry(id); write && written.size() == write->Written.size())
        {
            write->Written.assign(written.begin(), written.end());
            ++m_revision;
        }
    }

    Status WriteHistory::WriteOrAmend(const Target::Process& process, std::uint64_t generation, int id, std::uint64_t address, std::span<const std::uint8_t> bytes, std::string source)
    {
        MemoryWrite* write = Entry(id);
        if (!write || write->Generation != generation || write->Undone || write->Address != address || write->Written.size() != bytes.size() || BlockedBy(*write))
            return Write(process, generation, address, bytes, std::move(source));
        const Status status = process.Write(address, bytes);
        if (status)
        {
            write->Written.assign(bytes.begin(), bytes.end());
            ++m_revision;
        }
        return status;
    }

    std::optional<int> WriteHistory::BlockedBy(const MemoryWrite& write) const
    {
        const auto self = std::find_if(m_entries.begin(), m_entries.end(), [&](const MemoryWrite& entry) { return entry.Id == write.Id; });
        if (self == m_entries.end())
            return std::nullopt;
        for (auto later = m_entries.end(); later != self + 1;)
        {
            --later;
            if (!later->Undone && Overlap(*later, write))
                return later->Id;
        }
        if (!write.Undone)
            return std::nullopt;
        for (auto earlier = m_entries.begin(); earlier != self; ++earlier)
        {
            if (earlier->Undone && Overlap(*earlier, write))
                return earlier->Id;
        }
        return std::nullopt;
    }

    Status WriteHistory::Undo(const Target::Process& process, std::uint64_t generation, int id)
    {
        MemoryWrite* write = Entry(id);
        if (!write)
            return Status::Fail(ErrorCode::WriteFailed, "an edit that is no longer in the list");
        if (write->Generation != generation)
            return Status::Fail(ErrorCode::WriteFailed, Hex(write->Address) + " (the edit belongs to a process that is no longer open)");
        if (BlockedBy(*write))
            return Status::Fail(ErrorCode::WriteFailed, Hex(write->Address) + " (a later edit overlaps these bytes and has to be undone first)");
        const Status status = process.Write(write->Address, write->Original);
        if (status)
        {
            write->Undone = true;
            ++m_revision;
        }
        return status;
    }

    Status WriteHistory::Redo(const Target::Process& process, std::uint64_t generation, int id)
    {
        MemoryWrite* write = Entry(id);
        if (!write)
            return Status::Fail(ErrorCode::WriteFailed, "an edit that is no longer in the list");
        if (write->Generation != generation)
            return Status::Fail(ErrorCode::WriteFailed, Hex(write->Address) + " (the edit belongs to a process that is no longer open)");
        if (BlockedBy(*write))
            return Status::Fail(ErrorCode::WriteFailed, Hex(write->Address) + " (another edit overlaps these bytes: redo the earlier one or undo the later one first)");
        const Status status = process.Write(write->Address, write->Written);
        if (status)
        {
            write->Undone = false;
            ++m_revision;
        }
        return status;
    }

    std::optional<int> WriteHistory::LastUndoable(std::uint64_t generation) const
    {
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
        {
            if (it->Generation == generation && !it->Undone)
                return it->Id;
        }
        return std::nullopt;
    }

    std::size_t WriteHistory::Undoable(std::uint64_t generation) const
    {
        return static_cast<std::size_t>(std::count_if(m_entries.begin(), m_entries.end(), [&](const MemoryWrite& write) { return write.Generation == generation && !write.Undone; }));
    }

    std::size_t WriteHistory::Dropped(std::uint64_t generation) const
    {
        return generation == m_droppedGeneration ? m_dropped : 0;
    }

    WriteState WriteHistory::StateOf(const Target::Process& process, const MemoryWrite& write) const
    {
        if (write.Undone)
            return WriteState::Undone;
        std::vector<std::uint8_t> now(std::min(write.Written.size(), kCompareBlock));
        for (std::size_t at = 0; at < write.Written.size(); at += kCompareBlock)
        {
            const std::size_t count = std::min(kCompareBlock, write.Written.size() - at);
            if (!process.ReadExact(write.Address + at, now.data(), count))
                return WriteState::Unreadable;
            if (!std::equal(now.begin(), now.begin() + static_cast<std::ptrdiff_t>(count), write.Written.begin() + static_cast<std::ptrdiff_t>(at)))
                return WriteState::ChangedSince;
        }
        return WriteState::InMemory;
    }

    const MemoryWrite* WriteHistory::Find(int id) const
    {
        const auto found = std::find_if(m_entries.begin(), m_entries.end(), [id](const MemoryWrite& write) { return write.Id == id; });
        return found == m_entries.end() ? nullptr : &*found;
    }

    MemoryWrite* WriteHistory::Entry(int id)
    {
        return const_cast<MemoryWrite*>(Find(id));
    }

    void WriteHistory::Clear(std::uint64_t generation)
    {
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const MemoryWrite& write) { return write.Generation == generation; }), m_entries.end());
        if (generation == m_droppedGeneration)
            m_dropped = 0;
        ++m_revision;
    }
}
