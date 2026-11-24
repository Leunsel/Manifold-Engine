#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Manifold::Scan
{
    // ∑ Scan results stay in RAM while a budget shared by every scan of the
    //   program allows it, and only the rest goes to a file in the scan folder.
    //   Measured 2026-11-24 (1 GB target, 268M results): the temporary files
    //   reached the SSD in full (9 GB per run) although 43 GB were free; RAM made
    //   large next scans 3-4x faster than an NVMe and about 200x faster than a
    //   hard disk.
    namespace MemoryBudget
    {
        inline constexpr int kAuto = -1;      // up to half the RAM, while enough stays free
        inline constexpr int kDiskOnly = 0;   // every result goes to the scan files

        // megabytes, or kAuto / kDiskOnly; blocks already taken stay where they are
        void SetLimit(int megabytes);
        int Limit();
        // what the limit comes to now (kAuto: AutoBytes)
        std::uint64_t LimitBytes();
        // half the RAM
        std::uint64_t AutoBytes();
        std::uint64_t Used();
        // A block of RAM for scan results: false when the limit, or the memory the
        // system keeps free (2 GB or an eighth of the RAM), does not allow it.
        bool Reserve(std::uint64_t bytes);
        void Release(std::uint64_t bytes);
    }

    // Data written once from the front and read anywhere: RAM blocks first, a
    // temporary file (deleted when closed) for what does not fit.
    class ScanStore
    {
    public:
        static constexpr std::size_t kBlock = 16u * 1024 * 1024;

        ScanStore() = default;
        ~ScanStore();
        ScanStore(const ScanStore&) = delete;
        ScanStore& operator=(const ScanStore&) = delete;

        bool Append(const void* data, std::size_t size);
        bool Flush();
        bool ReadAt(std::uint64_t offset, void* buffer, std::size_t size) const;

        std::uint64_t Size() const { return m_ramBytes + m_fileBytes + m_buffer.size(); }
        std::uint64_t MemoryBytes() const { return m_ramBytes; }
        std::uint64_t DiskBytes() const { return m_fileBytes + m_buffer.size(); }

    private:
        bool ToMemory(const std::uint8_t*& bytes, std::size_t& size);
        bool OpenFile();
        bool WriteToFile(const void* data, std::size_t size);

        std::vector<std::unique_ptr<std::uint8_t[]>> m_blocks;
        std::uint64_t m_ramBytes = 0;
        std::uint64_t m_reserved = 0;
        bool m_spilled = false;   // once one block was refused, the rest goes to the file, so offsets stay in order
        void* m_file = nullptr;   // HANDLE
        std::vector<std::uint8_t> m_buffer;
        std::uint64_t m_fileBytes = 0;
    };
}
