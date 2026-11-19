#include "App/WriteHistory.h"

#include "Target/Process.h"

#include <algorithm>

namespace Manifold
{
    Status WriteHistory::Write(const Target::Process& process, std::uint64_t generation, std::uint64_t address, std::span<const std::uint8_t> bytes, std::string source)
    {
        MemoryWrite write;
        write.Generation = generation;
        write.Address = address;
        write.Original.resize(bytes.size());
        const bool saved = process.ReadExact(address, write.Original.data(), write.Original.size());
        const Status status = process.Write(address, bytes);
        if (!status || !saved)
            return status;
        write.Id = m_nextId++;
        write.Written.assign(bytes.begin(), bytes.end());
        write.Source = std::move(source);
        write.At = std::chrono::system_clock::now();
        m_entries.push_back(std::move(write));
        while (m_entries.size() > kLimit)
            m_entries.pop_front();
        ++m_revision;
        return status;
    }

    void WriteHistory::Amend(int id, std::span<const std::uint8_t> written)
    {
        if (MemoryWrite* write = Entry(id); write && written.size() == write->Written.size())
        {
            write->Written.assign(written.begin(), written.end());
            ++m_revision;
        }
    }

    Status WriteHistory::Undo(const Target::Process& process, std::uint64_t generation, int id)
    {
        MemoryWrite* write = Entry(id);
        if (!write || write->Generation != generation)
            return Status::Fail(ErrorCode::WriteFailed, "the edit belongs to a process that is no longer open");
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
        if (!write || write->Generation != generation)
            return Status::Fail(ErrorCode::WriteFailed, "the edit belongs to a process that is no longer open");
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

    WriteState WriteHistory::StateOf(const Target::Process& process, const MemoryWrite& write) const
    {
        if (write.Undone)
            return WriteState::Undone;
        std::vector<std::uint8_t> now(write.Written.size());
        if (!process.ReadExact(write.Address, now.data(), now.size()))
            return WriteState::Unreadable;
        return now == write.Written ? WriteState::InMemory : WriteState::ChangedSince;
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
        ++m_revision;
    }
}
