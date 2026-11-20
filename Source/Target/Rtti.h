#pragma once

#include "Target/Process.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Manifold::Target
{
    enum class RttiAbi
    {
        Msvc
    };

    struct RttiBase
    {
        std::string Name;
        std::string Decorated;
        int Depth = 0;
        std::uint32_t Contained = 0;
        std::int32_t Offset = 0;
        std::int32_t TablePointerAt = -1;
        std::int32_t TableSlot = 0;
        std::uint32_t Attributes = 0;
        bool Virtual = false;
    };

    struct RttiClass
    {
        RttiAbi Abi = RttiAbi::Msvc;
        std::uint64_t Locator = 0;
        std::uint64_t TypeDescriptor = 0;
        std::uint64_t Hierarchy = 0;
        std::uint64_t ImageBase = 0;
        std::uint32_t Offset = 0;
        std::uint32_t ConstructorOffset = 0;
        std::uint32_t Attributes = 0;
        std::string Name;
        std::string Decorated;
        std::string Module;

        bool VirtualInheritance() const { return (Attributes & 2) != 0; }
    };

    using RttiClassPtr = std::shared_ptr<const RttiClass>;
    using RttiBases = std::shared_ptr<const std::vector<RttiBase>>;

    struct RttiObject
    {
        std::uint64_t Address = 0;
        std::uint64_t Start = 0;
        std::uint64_t VTable = 0;
        std::uint64_t Via = 0;
        std::uint64_t Offset = 0;
        RttiClassPtr Class;
    };

    struct RttiTable
    {
        std::uint64_t Address = 0;
        std::uint32_t Offset = 0;
        std::string Symbol;
        std::string Module;
    };

    struct RttiCounts
    {
        std::size_t Modules = 0;
        std::size_t Classes = 0;
        std::size_t Tables = 0;
    };

    class RttiRuntime
    {
    public:
        using Stop = std::function<bool()>;
        using TableSeen = std::function<void(const std::string& symbol, std::uint64_t table)>;

        static constexpr std::uint64_t kOwnerWindow = 0x4000;
        static constexpr std::size_t kInstanceLimit = 100000;
        static constexpr std::size_t kScanLimit = 1000000;
        static constexpr std::size_t kNameLimit = 2048;
        static constexpr std::uint32_t kMaxBases = 512;
        static constexpr int kMaxVirtuals = 2048;

        RttiRuntime(std::shared_ptr<Process> process, TableSeen seen = {});

        RttiRuntime(const RttiRuntime&) = delete;
        RttiRuntime& operator=(const RttiRuntime&) = delete;

        const std::shared_ptr<Process>& ProcessHandle() const { return m_process; }
        int PointerSize() const { return m_pointer; }
        void ReloadImages();

        RttiClassPtr Locator(std::uint64_t address);
        RttiClassPtr TableClass(std::uint64_t table, bool quick = false);
        std::optional<RttiObject> ObjectAt(std::uint64_t address, bool quick = false);
        std::optional<RttiObject> Owner(std::uint64_t address, std::uint64_t window = kOwnerWindow);
        std::optional<RttiObject> Locate(std::uint64_t address);
        std::optional<std::string> Whois(std::uint64_t address);

        RttiBases Bases(const RttiClass& type);
        std::optional<std::int64_t> BaseOffset(const RttiBase& base, std::optional<std::uint64_t> start) const;
        std::optional<std::string> BaseAt(const RttiClass& type, std::int64_t offset, std::optional<std::uint64_t> start = std::nullopt);
        std::string SymbolFor(const RttiClass& type);
        bool HasVirtualBases(const RttiClass& type);
        std::optional<std::int32_t> VirtualBaseTable(std::uint64_t value) const;
        bool InImage(std::uint64_t address) const;
        std::optional<std::uint64_t> ReadPointer(std::uint64_t address) const;

        bool FindClass(std::string_view name, const Stop& stop = {});
        std::optional<std::pair<std::uint64_t, RttiClassPtr>> Primary(std::string_view name, std::string_view which = {}, const Stop& stop = {});
        std::vector<RttiTable> Tables(std::string_view name, const Stop& stop = {});
        std::optional<std::vector<std::uint64_t>> Instances(std::string_view name, std::size_t limit = kInstanceLimit, const Stop& stop = {});
        std::vector<std::string> Classes() const;
        RttiCounts Counts() const;
        std::string StatusText() const;
        std::optional<std::uint64_t> ResolveSymbol(std::string_view token);

    private:
        struct Image
        {
            std::string Name;
            std::uint64_t From = 0;
            std::uint64_t To = 0;
            bool System = false;
        };

        struct Record
        {
            std::string Name;
            std::string Decorated;
            std::uint64_t TypeDescriptor = 0;
            std::optional<Image> Home;
            std::map<std::uint64_t, RttiClassPtr> Tables;
            bool Scanned = false;
        };

        using ImageList = std::shared_ptr<const std::vector<Image>>;

        ImageList Images() const;
        static const Image* FindImage(const std::vector<Image>& images, std::uint64_t address);
        std::optional<Image> ImageOf(std::uint64_t address, bool quick);
        std::optional<std::uint32_t> ReadU32(std::uint64_t address) const;
        std::optional<std::int32_t> ReadI32(std::uint64_t address) const;
        std::string TypeName(std::uint64_t descriptor);
        std::uint64_t Absolute(std::uint64_t imageBase, std::uint32_t reference) const;
        std::shared_ptr<Record> RecordOf(const std::string& name);
        std::shared_ptr<Record> FindRecord(std::string_view name, const Stop& stop);
        std::pair<std::uint64_t, RttiClassPtr> PrimaryOf(const Record& record, std::string_view which);
        std::vector<std::uint64_t> ScanImage(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& mask, const Image& image, std::uint64_t alignment,
            std::size_t limit, const Stop& stop) const;
        std::string ModuleOffset(std::uint64_t address) const;
        bool StartsWithBaseTable(const RttiClass& type, std::uint64_t start, std::uint64_t address);

        std::shared_ptr<Process> m_process;
        TableSeen m_seen;
        int m_pointer = 8;
        bool m_x64 = true;

        mutable std::recursive_mutex m_lock;
        ImageList m_images;
        std::optional<Image> m_main;
        std::uint64_t m_imagesAt = 0;
        std::unordered_map<std::uint64_t, RttiClassPtr> m_locators;
        std::unordered_map<std::uint64_t, std::string> m_descriptors;
        std::unordered_map<std::uint64_t, RttiClassPtr> m_tables;
        std::unordered_map<std::uint64_t, RttiBases> m_bases;
        std::map<std::string, std::shared_ptr<Record>> m_classes;
        std::unordered_set<std::string> m_misses;
        std::unordered_map<std::string, std::uint64_t> m_symbols;
    };
}
