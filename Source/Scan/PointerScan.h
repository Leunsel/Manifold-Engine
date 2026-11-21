#pragma once

#include "Core/Errors.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Scan
{
    inline constexpr int kPointerMaxLevel = 12;
    inline constexpr std::uint32_t kPointerMaxOffset = 0xFFFFFF;
    inline constexpr std::uint64_t kPointerMemoryLimit = 64ull * 1024 * 1024;
    inline constexpr std::uint64_t kPointerSortLimit = 8'000'000;

    struct PointerModule
    {
        std::string Name;
        std::uint64_t Base = 0;
        std::uint64_t Size = 0;
    };

    struct PointerRegion
    {
        std::uint64_t Base = 0;
        std::uint64_t Size = 0;
        int Module = -1;
    };

    class PointerSource
    {
    public:
        virtual ~PointerSource() = default;
        virtual int PointerSize() const = 0;
        virtual std::vector<PointerModule> Modules() const = 0;
        virtual std::vector<PointerRegion> Regions() const = 0;
        virtual std::size_t Read(std::uint64_t address, void* buffer, std::size_t size) const = 0;
    };

    struct PointerMemory
    {
        bool ReadOnly = false;
        bool Mapped = false;
        bool SystemModules = false;
    };

    class ProcessPointerSource final : public PointerSource
    {
    public:
        ProcessPointerSource(std::shared_ptr<const Target::Process> process, const PointerMemory& memory);

        int PointerSize() const override;
        std::vector<PointerModule> Modules() const override { return m_modules; }
        std::vector<PointerRegion> Regions() const override;
        std::size_t Read(std::uint64_t address, void* buffer, std::size_t size) const override;

    private:
        struct Span
        {
            std::uint64_t Base = 0;
            std::uint64_t End = 0;
            int Module = -1;
        };

        std::shared_ptr<const Target::Process> m_process;
        PointerMemory m_memory;
        std::vector<PointerModule> m_modules;
        std::vector<Span> m_spans;
    };

    enum class PointerStage
    {
        Idle,
        Reading,
        Sorting,
        Searching,
        Rescanning,
        Ordering
    };

    struct PointerProgress
    {
        std::atomic<bool> Cancel{ false };
        std::atomic<int> Stage{ 0 };
        std::atomic<std::uint64_t> Done{ 0 };
        std::atomic<std::uint64_t> Total{ 0 };
        std::atomic<std::uint64_t> Pointers{ 0 };
        std::atomic<std::uint64_t> Results{ 0 };
        std::atomic<int> Level{ 0 };

        void Enter(PointerStage stage, std::uint64_t total)
        {
            Done = 0;
            Total = total;
            Stage = static_cast<int>(stage);
        }
    };

    struct PointerPath
    {
        std::uint32_t ModuleOffset = 0;
        std::uint16_t Module = 0;
        std::uint8_t Count = 0;
        std::uint32_t Offsets[kPointerMaxLevel] = {};
    };

    template <class ReadPointer>
    std::optional<std::uint64_t> ResolvePointerPath(std::uint64_t moduleBase, const PointerPath& path, ReadPointer&& read)
    {
        std::uint64_t address = moduleBase + path.ModuleOffset;
        for (int index = 0; index < path.Count; ++index)
        {
            const std::optional<std::uint64_t> pointer = read(address);
            if (!pointer)
                return std::nullopt;
            address = *pointer + path.Offsets[index];
        }
        return address;
    }

    class PointerResults
    {
    public:
        PointerResults();
        ~PointerResults();

        PointerResults(const PointerResults&) = delete;
        PointerResults& operator=(const PointerResults&) = delete;

        void Begin(int pointerSize, int levels, std::vector<std::string> modules, std::uint64_t memoryLimit = kPointerMemoryLimit);
        int PointerSize() const { return m_pointerSize; }
        int Levels() const { return m_levels; }
        std::size_t RecordSize() const { return m_recordSize; }
        const std::vector<std::string>& Modules() const { return m_modules; }
        std::uint64_t Target() const { return m_target; }
        void SetTarget(std::uint64_t target) { m_target = target; }
        std::uint64_t Count() const { return m_count.load(); }
        std::uint64_t MemoryBytes() const;
        std::uint64_t SpilledBytes() const;
        bool Failed() const { return m_failed.load(); }

        bool Append(const std::uint8_t* records, std::size_t count);
        std::size_t ReadRaw(std::uint64_t index, std::size_t count, std::uint8_t* out) const;
        bool Read(std::uint64_t index, PointerPath& out) const;
        void Decode(const std::uint8_t* record, PointerPath& out) const;
        void Encode(const PointerPath& path, std::uint8_t* record) const;
        std::string BaseText(const PointerPath& path) const;
        std::string PathText(const PointerPath& path) const;

        Status Save(const std::filesystem::path& path) const;
        Status Load(const std::filesystem::path& path, std::uint64_t memoryLimit = kPointerMemoryLimit);

    private:
        bool Spill(const std::uint8_t* data, std::size_t size);

        int m_pointerSize = 8;
        int m_levels = 1;
        std::size_t m_recordSize = 12;
        std::size_t m_perSlab = 1;
        std::uint64_t m_memoryRecords = 0;
        std::uint64_t m_target = 0;
        std::vector<std::string> m_modules;
        std::vector<std::unique_ptr<std::uint8_t[]>> m_slabs;
        std::uint64_t m_inMemory = 0;
        std::uint64_t m_inFile = 0;
        std::atomic<std::uint64_t> m_count{ 0 };
        std::atomic<bool> m_failed{ false };
        void* m_file = nullptr;
        mutable std::mutex m_lock;
        mutable std::vector<std::uint8_t> m_window;
        mutable std::uint64_t m_windowFirst = 0;
        mutable std::size_t m_windowCount = 0;
    };

    enum class PointerOrder
    {
        Found,
        Level,
        LastOffset,
        Base
    };

    std::optional<std::vector<std::uint32_t>> SortPointerResults(const PointerResults& results, PointerOrder order, PointerProgress* progress = nullptr);

    struct PointerScanOptions
    {
        std::uint64_t Target = 0;
        int MaxLevel = 5;
        std::uint32_t MaxOffset = 0x1000;
        bool Aligned = true;
        std::uint64_t MaxResults = 2'000'000;
        int Threads = 0;
        std::uint64_t MemoryLimit = kPointerMemoryLimit;
    };

    struct PointerScanStats
    {
        double MapSeconds = 0.0;
        double SearchSeconds = 0.0;
        std::uint64_t ScannedBytes = 0;
        std::uint64_t Pointers = 0;
        std::uint64_t Nodes = 0;
        std::uint64_t Results = 0;
        std::uint64_t MapBytes = 0;
        std::uint64_t SearchBytes = 0;
        int Levels = 0;
        bool Truncated = false;
        bool Cancelled = false;
        bool TargetOutside = false;
    };

    class PointerMap
    {
    public:
        PointerMap();
        ~PointerMap();

        PointerMap(const PointerMap&) = delete;
        PointerMap& operator=(const PointerMap&) = delete;

        Status Build(const PointerSource& source, bool aligned, int threads = 0, PointerProgress* progress = nullptr);
        Status Search(const PointerScanOptions& options, PointerResults& results, PointerProgress* progress = nullptr, PointerScanStats* stats = nullptr);

        int PointerSize() const;
        std::uint64_t Count() const;
        std::uint64_t ScannedBytes() const;
        std::uint64_t MemoryBytes() const;
        bool Cancelled() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    Status ScanPointers(const PointerSource& source, const PointerScanOptions& options, PointerResults& results, PointerProgress* progress = nullptr, PointerScanStats* stats = nullptr);

    struct PointerRescan
    {
        std::optional<std::uint64_t> Address;
        std::vector<std::uint8_t> Value;
        int Threads = 0;
        std::uint64_t MemoryLimit = kPointerMemoryLimit;
    };

    Status RescanPointers(const PointerSource& source, const PointerResults& from, const PointerRescan& rescan, PointerResults& out, PointerProgress* progress = nullptr);
}
