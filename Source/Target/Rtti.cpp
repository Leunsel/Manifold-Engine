#include "Target/Rtti.h"

#include "Core/Text.h"
#include "Scan/Scanner.h"
#include "Target/RttiMsvc.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint64_t kImageReloadMs = 2000;
        constexpr std::size_t kCacheLimit = 250000;
        constexpr std::uint64_t kLowest = 0x10000;

        template <class T>
        T At(const std::uint8_t* data, std::size_t offset)
        {
            T value{};
            std::memcpy(&value, data + offset, sizeof(T));
            return value;
        }

        void Append(std::vector<std::uint8_t>& bytes, std::uint64_t value, int size)
        {
            for (int i = 0; i < size; ++i)
                bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }

    RttiRuntime::RttiRuntime(std::shared_ptr<Process> process, TableSeen seen)
        : m_process(std::move(process)), m_seen(std::move(seen)), m_pointer(m_process->PointerSize()), m_x64(m_process->Is64Bit())
    {
        m_images = std::make_shared<std::vector<Image>>();
        ReloadImages();
    }

    void RttiRuntime::ReloadImages()
    {
        m_process->RefreshModules();
        auto list = std::make_shared<std::vector<Image>>();
        std::optional<Image> main;
        const auto executable = m_process->MainModule();
        if (const ModuleList modules = m_process->Modules())
        {
            for (const ModuleInfo& module : *modules)
            {
                if (!module.Base || !module.Size)
                    continue;
                Image image{ module.Name, module.Base, module.Base + module.Size, module.System };
                if (executable && executable->Base == module.Base)
                    main = image;
                if (!main && list->empty())
                    main = image;
                list->push_back(std::move(image));
            }
        }
        if (executable)
        {
            const auto found = std::find_if(list->begin(), list->end(), [&](const Image& image) { return image.From == executable->Base; });
            if (found != list->end())
                main = *found;
        }
        std::sort(list->begin(), list->end(), [](const Image& a, const Image& b) { return a.From < b.From; });
        std::lock_guard lock(m_lock);
        // ∑ Every cache below is keyed by addresses in the images. When one of them
        //   went away or moved (a module unloaded, a level's DLL loaded anew), those
        //   addresses may now hold another module's data, so all of it is read again;
        //   images that were only added keep what is known.
        const bool changed = m_images && std::any_of(m_images->begin(), m_images->end(), [&](const Image& old)
            {
                const Image* now = FindImage(*list, old.From);
                return !now || now->From != old.From || now->To != old.To || now->Name != old.Name;
            });
        if (changed)
        {
            m_locators.clear();
            m_descriptors.clear();
            m_bases.clear();
            m_classes.clear();
            m_symbols.clear();
        }
        m_images = std::move(list);
        m_main = std::move(main);
        m_imagesAt = ::GetTickCount64();
        m_tables.clear();
        m_misses.clear();
    }

    RttiRuntime::ImageList RttiRuntime::Images() const
    {
        std::lock_guard lock(m_lock);
        return m_images;
    }

    const RttiRuntime::Image* RttiRuntime::FindImage(const std::vector<Image>& images, std::uint64_t address)
    {
        auto it = std::upper_bound(images.begin(), images.end(), address, [](std::uint64_t value, const Image& image) { return value < image.From; });
        if (it == images.begin())
            return nullptr;
        --it;
        return address < it->To ? &*it : nullptr;
    }

    bool RttiRuntime::InImage(std::uint64_t address) const
    {
        const ImageList images = Images();
        return FindImage(*images, address) != nullptr;
    }

    std::optional<RttiRuntime::Image> RttiRuntime::ImageOf(std::uint64_t address, bool quick)
    {
        {
            const ImageList images = Images();
            if (const Image* image = FindImage(*images, address))
                return *image;
        }
        if (quick)
            return std::nullopt;
        {
            std::lock_guard lock(m_lock);
            if (::GetTickCount64() - m_imagesAt < kImageReloadMs)
                return std::nullopt;
        }
        ReloadImages();
        const ImageList images = Images();
        if (const Image* image = FindImage(*images, address))
            return *image;
        return std::nullopt;
    }

    std::optional<std::uint64_t> RttiRuntime::ReadPointer(std::uint64_t address) const
    {
        if (address < kLowest)
            return std::nullopt;
        const auto value = m_process->ReadPointer(address);
        if (!value || *value == 0)
            return std::nullopt;
        return value;
    }

    std::optional<std::uint32_t> RttiRuntime::ReadU32(std::uint64_t address) const
    {
        if (address < kLowest)
            return std::nullopt;
        return m_process->ReadValue<std::uint32_t>(address);
    }

    std::optional<std::int32_t> RttiRuntime::ReadI32(std::uint64_t address) const
    {
        if (address < kLowest)
            return std::nullopt;
        return m_process->ReadValue<std::int32_t>(address);
    }

    std::uint64_t RttiRuntime::Absolute(std::uint64_t imageBase, std::uint32_t reference) const
    {
        return m_x64 ? imageBase + reference : reference;
    }

    std::string RttiRuntime::TypeName(std::uint64_t descriptor)
    {
        std::lock_guard lock(m_lock);
        if (const auto found = m_descriptors.find(descriptor); found != m_descriptors.end())
            return found->second;
        std::string name;
        if (descriptor >= kLowest)
        {
            const std::uint64_t at = descriptor + 2 * static_cast<std::uint64_t>(m_pointer);
            for (const std::size_t size : { std::size_t{ 128 }, kNameLimit })
            {
                std::string raw(size, '\0');
                const std::size_t got = m_process->Read(at, raw.data(), raw.size());
                raw.resize(got);
                const std::size_t end = raw.find('\0');
                if (end != std::string::npos)
                {
                    name = raw.substr(0, end);
                    break;
                }
                if (got < size)
                    break;
            }
        }
        if (!IsMsvcTypeName(name))
            name.clear();
        if (m_descriptors.size() >= kCacheLimit)
            m_descriptors.clear();
        m_descriptors[descriptor] = name;
        return name;
    }

    std::shared_ptr<RttiRuntime::Record> RttiRuntime::RecordOf(const std::string& name)
    {
        std::shared_ptr<Record>& record = m_classes[name];
        if (!record)
        {
            record = std::make_shared<Record>();
            record->Name = name;
        }
        return record;
    }

    RttiClassPtr RttiRuntime::Locator(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (const auto found = m_locators.find(address); found != m_locators.end())
            return found->second;
        if (m_locators.size() >= kCacheLimit)
            m_locators.clear();
        m_locators[address] = nullptr;
        const ImageList images = m_images;
        const Image* image = FindImage(*images, address);
        if (!image || address % 4 != 0)
            return nullptr;
        std::uint8_t raw[24]{};
        if (!m_process->ReadExact(address, raw, m_x64 ? 24 : 20))
            return nullptr;
        const auto signature = At<std::uint32_t>(raw, 0);
        const auto offset = At<std::uint32_t>(raw, 4);
        if (signature != (m_x64 ? 1u : 0u) || offset >= 0x1000000)
            return nullptr;
        if (m_x64 && At<std::uint32_t>(raw, 20) != address - image->From)
            return nullptr;
        const std::uint64_t descriptor = Absolute(image->From, At<std::uint32_t>(raw, 12));
        const std::uint64_t hierarchy = Absolute(image->From, At<std::uint32_t>(raw, 16));
        const Image* descriptorImage = FindImage(*images, descriptor);
        if (!descriptorImage || !FindImage(*images, hierarchy))
            return nullptr;
        std::uint8_t header[12]{};
        if (!m_process->ReadExact(hierarchy, header, sizeof(header)))
            return nullptr;
        const auto count = At<std::uint32_t>(header, 8);
        if (At<std::uint32_t>(header, 0) != 0 || count < 1 || count > kMaxBases)
            return nullptr;
        const std::string decorated = TypeName(descriptor);
        if (decorated.empty())
            return nullptr;
        auto type = std::make_shared<RttiClass>();
        type->Locator = address;
        type->TypeDescriptor = descriptor;
        type->Hierarchy = hierarchy;
        type->ImageBase = image->From;
        type->Offset = offset;
        type->ConstructorOffset = At<std::uint32_t>(raw, 8);
        type->Attributes = At<std::uint32_t>(header, 4);
        type->Decorated = decorated;
        type->Name = MsvcDemangle(decorated);
        type->Module = image->Name;
        m_locators[address] = type;
        const std::shared_ptr<Record> record = RecordOf(type->Name);
        if (record->Decorated.empty())
            record->Decorated = decorated;
        if (!record->TypeDescriptor)
            record->TypeDescriptor = descriptor;
        if (!record->Home)
            record->Home = *descriptorImage;
        return type;
    }

    RttiBases RttiRuntime::Bases(const RttiClass& type)
    {
        std::lock_guard lock(m_lock);
        if (const auto found = m_bases.find(type.Locator); found != m_bases.end())
            return found->second;
        auto list = std::make_shared<std::vector<RttiBase>>();
        const std::uint32_t count = std::min(ReadU32(type.Hierarchy + 8).value_or(0), kMaxBases);
        const std::uint64_t array = Absolute(type.ImageBase, ReadU32(type.Hierarchy + 12).value_or(0));
        std::vector<std::int64_t> open;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uint64_t descriptor = Absolute(type.ImageBase, ReadU32(array + 4ull * i).value_or(0));
            RttiBase base;
            std::uint8_t raw[24]{};
            if (descriptor >= kLowest && m_process->ReadExact(descriptor, raw, sizeof(raw)))
            {
                base.Decorated = TypeName(Absolute(type.ImageBase, At<std::uint32_t>(raw, 0)));
                base.Contained = At<std::uint32_t>(raw, 4);
                base.Offset = At<std::int32_t>(raw, 8);
                base.TablePointerAt = At<std::int32_t>(raw, 12);
                base.TableSlot = At<std::int32_t>(raw, 16);
                base.Attributes = At<std::uint32_t>(raw, 20);
            }
            base.Name = base.Decorated.empty() ? "?" : MsvcDemangle(base.Decorated);
            base.Virtual = base.TablePointerAt != -1;
            while (!open.empty() && open.back() <= 0)
                open.pop_back();
            base.Depth = static_cast<int>(open.size());
            for (std::int64_t& left : open)
                --left;
            if (base.Contained > 0)
                open.push_back(base.Contained);
            list->push_back(std::move(base));
        }
        if (m_bases.size() >= kCacheLimit)
            m_bases.clear();
        m_bases[type.Locator] = list;
        return list;
    }

    std::optional<std::int64_t> RttiRuntime::BaseOffset(const RttiBase& base, std::optional<std::uint64_t> start) const
    {
        if (!base.Virtual)
            return base.Offset;
        if (!start)
            return std::nullopt;
        const auto table = ReadPointer(*start + static_cast<std::int64_t>(base.TablePointerAt));
        if (!table)
            return std::nullopt;
        const auto delta = ReadI32(*table + static_cast<std::int64_t>(base.TableSlot));
        if (!delta)
            return std::nullopt;
        return static_cast<std::int64_t>(base.TablePointerAt) + *delta + base.Offset;
    }

    std::optional<std::string> RttiRuntime::BaseAt(const RttiClass& type, std::int64_t offset, std::optional<std::uint64_t> start)
    {
        const RttiBases bases = Bases(type);
        for (std::size_t i = 1; i < bases->size(); ++i)
        {
            const RttiBase& base = (*bases)[i];
            if ((!base.Virtual || start) && BaseOffset(base, start) == offset)
                return base.Name;
        }
        return std::nullopt;
    }

    bool RttiRuntime::HasVirtualBases(const RttiClass& type)
    {
        if (type.VirtualInheritance())
            return true;
        const RttiBases bases = Bases(type);
        return std::any_of(bases->begin(), bases->end(), [](const RttiBase& base) { return base.Virtual; });
    }

    bool RttiRuntime::StartsWithBaseTable(const RttiClass& type, std::uint64_t start, std::uint64_t address)
    {
        const RttiBases bases = Bases(type);
        return std::any_of(bases->begin(), bases->end(), [&](const RttiBase& base)
            {
                return base.Virtual && start + static_cast<std::int64_t>(base.TablePointerAt) == address && BaseOffset(base, start) == static_cast<std::int64_t>(type.Offset);
            });
    }

    std::string RttiRuntime::SymbolFor(const RttiClass& type)
    {
        if (type.Offset == 0)
            return type.Name + "::vftable";
        return type.Name + "::vftable@" + BaseAt(type, type.Offset).value_or(Hex(type.Offset));
    }

    RttiClassPtr RttiRuntime::TableClass(std::uint64_t table, bool quick)
    {
        if (!table)
            return nullptr;
        {
            std::lock_guard lock(m_lock);
            if (const auto found = m_tables.find(table); found != m_tables.end())
                return found->second;
        }
        RttiClassPtr type;
        if (table % static_cast<std::uint64_t>(m_pointer) == 0 && ImageOf(table, quick))
        {
            const auto locator = ReadPointer(table - static_cast<std::uint64_t>(m_pointer));
            if (locator && InImage(*locator))
                type = Locator(*locator);
            const std::vector<RegionInfo> region = type ? m_process->Regions(table - static_cast<std::uint64_t>(m_pointer), table) : std::vector<RegionInfo>{};
            if (!region.empty() && region.front().Executable() && !region.front().Writable())
                type = nullptr;
        }
        std::string symbol;
        {
            std::lock_guard lock(m_lock);
            if (m_tables.size() >= kCacheLimit)
                m_tables.clear();
            m_tables[table] = type;
            if (!type)
                return nullptr;
            RecordOf(type->Name)->Tables[table] = type;
            symbol = SymbolFor(*type);
            const auto known = m_symbols.find(symbol);
            if (known != m_symbols.end() && known->second == table)
                return type;
            m_symbols[symbol] = table;
        }
        if (m_seen)
            m_seen(symbol, table);
        return type;
    }

    std::optional<std::int32_t> RttiRuntime::VirtualBaseTable(std::uint64_t value) const
    {
        if (!value || !InImage(value))
            return std::nullopt;
        std::uint8_t raw[8]{};
        if (!m_process->ReadExact(value, raw, sizeof(raw)))
            return std::nullopt;
        const auto top = At<std::int32_t>(raw, 0);
        const auto first = At<std::int32_t>(raw, 4);
        if (top <= 0 && top >= -0x1000 && first > 0 && first < 0x100000)
            return first;
        return std::nullopt;
    }

    std::optional<RttiObject> RttiRuntime::ObjectAt(std::uint64_t address, bool quick)
    {
        if (address < kLowest || address % static_cast<std::uint64_t>(m_pointer) != 0)
            return std::nullopt;
        const auto table = ReadPointer(address);
        if (!table)
            return std::nullopt;
        if (const RttiClassPtr type = TableClass(*table, quick))
        {
            RttiObject object;
            object.Address = address;
            object.Start = address - type->Offset;
            object.VTable = *table;
            object.Offset = type->Offset;
            object.Class = type;
            return object;
        }
        const auto first = VirtualBaseTable(*table);
        if (!first)
            return std::nullopt;
        const std::uint64_t part = address + static_cast<std::uint64_t>(*first);
        const auto partTable = ReadPointer(part);
        if (!partTable)
            return std::nullopt;
        const RttiClassPtr type = TableClass(*partTable, true);
        if (!type)
            return std::nullopt;
        const std::uint64_t start = part - type->Offset;
        if (start > address || (!type->VirtualInheritance() && !StartsWithBaseTable(*type, start, address)))
            return std::nullopt;
        RttiObject object;
        object.Address = address;
        object.Start = start;
        object.VTable = *partTable;
        object.Via = part;
        object.Offset = address - start;
        object.Class = type;
        return object;
    }

    std::optional<RttiObject> RttiRuntime::Owner(std::uint64_t address, std::uint64_t window)
    {
        const auto pointer = static_cast<std::uint64_t>(m_pointer);
        const std::uint64_t top = address - address % pointer;
        if (top < kLowest)
            return std::nullopt;
        const std::uint64_t floor = top - kLowest > window ? top - window : kLowest;
        std::uint64_t page = top & ~0xFFFull;
        std::uint64_t position = top;
        std::vector<std::uint8_t> raw(0x1000);
        while (position >= floor)
        {
            if (!m_process->ReadExact(page, raw.data(), raw.size()))
                return std::nullopt;
            const ImageList images = Images();
            while (position >= page && position >= floor)
            {
                std::uint64_t value = 0;
                std::memcpy(&value, raw.data() + (position - page), pointer);
                if (FindImage(*images, value))
                {
                    if (auto hit = ObjectAt(position, true); hit && hit->Start <= address)
                    {
                        hit->Offset = address - hit->Start;
                        return hit;
                    }
                }
                position -= pointer;
            }
            page -= 0x1000;
        }
        return std::nullopt;
    }

    std::optional<RttiObject> RttiRuntime::Locate(std::uint64_t address)
    {
        if (auto hit = ObjectAt(address))
        {
            hit->Offset = address - hit->Start;
            return hit;
        }
        return Owner(address);
    }

    std::string RttiRuntime::ModuleOffset(std::uint64_t address) const
    {
        const ImageList images = Images();
        const Image* image = FindImage(*images, address);
        if (!image)
            return Hex(address);
        return image->Name + "+" + Hex(address - image->From);
    }

    std::optional<std::string> RttiRuntime::Whois(std::uint64_t address)
    {
        const auto hit = Locate(address);
        if (!hit)
            return std::nullopt;
        const RttiClass& type = *hit->Class;
        std::string text;
        if (address == hit->Start)
        {
            text = Hex(address) + " is a " + type.Name + "\n";
        }
        else
        {
            text = Hex(address) + " is " + type.Name + "+" + Hex(address - hit->Start) + "\n";
            text += "Object start: " + Hex(hit->Start) + "\n";
        }
        text += "Decorated name: " + type.Decorated + "\n";
        text += "Virtual function table: " + Hex(hit->VTable) + " (" + ModuleOffset(hit->VTable) + ")\n";
        text += "Defined in: " + type.Module + "\n";
        text += "Class and base classes:";
        for (const RttiBase& base : *Bases(type))
        {
            const auto offset = BaseOffset(base, hit->Start);
            text += "\n  " + std::string(static_cast<std::size_t>(base.Depth) * 2, ' ') + base.Name + (base.Virtual ? " (virtual)" : "") + "  +" +
                (offset ? Hex(static_cast<std::uint64_t>(*offset)) : std::string("?"));
        }
        return text;
    }

    std::vector<std::uint64_t> RttiRuntime::ScanImage(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& mask, const Image& image, std::uint64_t alignment,
        std::size_t limit, const Stop& stop) const
    {
        Scan::Pattern pattern;
        pattern.Bytes = bytes;
        pattern.Mask = mask;
        Scan::PatternFilter filter;
        filter.Divisor = alignment > 1 ? alignment : 0;
        Scan::PatternProgress progress;
        if (stop)
            progress.Stop = stop;
        std::vector<std::uint64_t> hits = Scan::FindPattern(*m_process, pattern, image.From, image.To, filter, limit, &progress);
        std::erase_if(hits, [&](std::uint64_t hit) { return hit < image.From || hit >= image.To; });
        std::sort(hits.begin(), hits.end());
        return hits;
    }

    std::shared_ptr<RttiRuntime::Record> RttiRuntime::FindRecord(std::string_view text, const Stop& stop)
    {
        const std::string name(text);
        if (name.empty())
            return nullptr;
        std::vector<std::string> candidates;
        std::uint64_t knownDescriptor = 0;
        std::vector<Image> images;
        {
            std::lock_guard lock(m_lock);
            const auto found = m_classes.find(name);
            const std::shared_ptr<Record> record = found != m_classes.end() ? found->second : nullptr;
            if (record && record->Scanned && !record->Tables.empty())
                return record;
            if (m_misses.count(name))
                return nullptr;
            if (record && !record->Decorated.empty())
                candidates = { record->Decorated };
            else
                candidates = MsvcMangle(name);
            if (candidates.empty())
            {
                m_misses.insert(name);
                return nullptr;
            }
            if (record && record->Home)
            {
                images.push_back(*record->Home);
                knownDescriptor = record->TypeDescriptor;
            }
            else
            {
                if (m_main)
                    images.push_back(*m_main);
                for (const Image& image : *m_images)
                {
                    if (!image.System && (!m_main || image.From != m_main->From))
                        images.push_back(image);
                }
            }
        }
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint8_t> mask;
        const std::string_view first = candidates.front();
        for (std::size_t i = 0; i < first.size(); ++i)
        {
            const bool either = candidates.size() == 2 && i == 3;
            bytes.push_back(either ? std::uint8_t{ 0 } : static_cast<std::uint8_t>(first[i]));
            mask.push_back(either ? 0x00 : 0xFF);
        }
        bytes.push_back(0);
        mask.push_back(0xFF);
        const auto pointer = static_cast<std::uint64_t>(m_pointer);
        std::string key = name;
        for (const Image& image : images)
        {
            if (stop && stop())
                return nullptr;
            std::vector<std::uint64_t> descriptors;
            if (knownDescriptor && knownDescriptor >= image.From && knownDescriptor < image.To)
            {
                descriptors.push_back(knownDescriptor);
            }
            else
            {
                for (const std::uint64_t hit : ScanImage(bytes, mask, image, 0, 16, stop))
                {
                    const std::uint64_t descriptor = hit - 2 * pointer;
                    const std::string decorated = TypeName(descriptor);
                    if (!decorated.empty() && std::find(candidates.begin(), candidates.end(), decorated) != candidates.end())
                        descriptors.push_back(descriptor);
                }
            }
            for (const std::uint64_t descriptor : descriptors)
            {
                const std::uint64_t reference = m_x64 ? descriptor - image.From : descriptor;
                if (reference > 0xFFFFFFFFull)
                    continue;
                std::vector<std::uint8_t> wanted;
                Append(wanted, reference, 4);
                for (const std::uint64_t hit : ScanImage(wanted, std::vector<std::uint8_t>(4, 0xFF), image, 4, 4096, stop))
                {
                    const RttiClassPtr type = Locator(hit - 12);
                    if (!type || type->TypeDescriptor != descriptor)
                        continue;
                    key = type->Name;
                    std::vector<std::uint8_t> locator;
                    Append(locator, type->Locator, m_pointer);
                    for (const std::uint64_t at : ScanImage(locator, std::vector<std::uint8_t>(locator.size(), 0xFF), image, pointer, 64, stop))
                        TableClass(at + pointer, true);
                }
            }
            if (stop && stop())
                return nullptr;
            std::lock_guard lock(m_lock);
            const auto found = m_classes.find(key);
            if (found != m_classes.end() && !found->second->Tables.empty())
            {
                const std::shared_ptr<Record> record = found->second;
                record->Scanned = true;
                if (key != name)
                    m_classes[name] = record;
                return record;
            }
        }
        std::lock_guard lock(m_lock);
        m_misses.insert(name);
        return nullptr;
    }

    bool RttiRuntime::FindClass(std::string_view name, const Stop& stop)
    {
        return FindRecord(name, stop) != nullptr;
    }

    std::pair<std::uint64_t, RttiClassPtr> RttiRuntime::PrimaryOf(const Record& record, std::string_view which)
    {
        std::map<std::uint64_t, RttiClassPtr> tables;
        {
            std::lock_guard lock(m_lock);
            tables = record.Tables;
        }
        std::pair<std::uint64_t, RttiClassPtr> best{ 0, nullptr };
        std::string upper(which);
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        for (const auto& [table, type] : tables)
        {
            if (!which.empty())
            {
                if (Hex(type->Offset) == upper || BaseAt(*type, type->Offset) == std::string(which))
                    return { table, type };
            }
            else if (!best.second || type->Offset < best.second->Offset)
            {
                best = { table, type };
            }
        }
        return best;
    }

    std::optional<std::pair<std::uint64_t, RttiClassPtr>> RttiRuntime::Primary(std::string_view name, std::string_view which, const Stop& stop)
    {
        const std::shared_ptr<Record> record = FindRecord(name, stop);
        if (!record)
            return std::nullopt;
        const auto best = PrimaryOf(*record, which);
        if (!best.second)
            return std::nullopt;
        return best;
    }

    std::vector<RttiTable> RttiRuntime::Tables(std::string_view name, const Stop& stop)
    {
        std::vector<RttiTable> list;
        const std::shared_ptr<Record> record = FindRecord(name, stop);
        if (!record)
            return list;
        std::map<std::uint64_t, RttiClassPtr> tables;
        {
            std::lock_guard lock(m_lock);
            tables = record->Tables;
        }
        for (const auto& [table, type] : tables)
            list.push_back({ table, type->Offset, SymbolFor(*type), type->Module });
        std::stable_sort(list.begin(), list.end(), [](const RttiTable& a, const RttiTable& b) { return a.Offset < b.Offset; });
        return list;
    }

    std::optional<std::vector<std::uint64_t>> RttiRuntime::Instances(std::string_view name, std::size_t limit, const Stop& stop)
    {
        const auto primary = Primary(name, {}, stop);
        if (!primary)
            return std::nullopt;
        Scan::Pattern pattern;
        Append(pattern.Bytes, primary->first, m_pointer);
        pattern.Mask.assign(pattern.Bytes.size(), 0xFF);
        Scan::PatternFilter filter;
        filter.Writable = Scan::Tristate::Yes;
        filter.Divisor = static_cast<std::uint64_t>(m_pointer);
        Scan::PatternProgress progress;
        if (stop)
            progress.Stop = stop;
        std::vector<std::uint64_t> hits = Scan::FindPattern(*m_process, pattern, 0, ~0ull, filter, std::clamp<std::size_t>(limit, 1, kScanLimit), &progress);
        std::sort(hits.begin(), hits.end());
        for (std::uint64_t& hit : hits)
            hit -= primary->second->Offset;
        return hits;
    }

    std::vector<std::string> RttiRuntime::Classes() const
    {
        std::vector<std::string> list;
        std::lock_guard lock(m_lock);
        for (const auto& [name, record] : m_classes)
        {
            if (record->Name == name && !record->Tables.empty())
                list.push_back(name);
        }
        return list;
    }

    RttiCounts RttiRuntime::Counts() const
    {
        RttiCounts counts;
        std::lock_guard lock(m_lock);
        counts.Modules = m_images->size();
        for (const auto& [name, record] : m_classes)
        {
            if (record->Name != name || record->Tables.empty())
                continue;
            ++counts.Classes;
            counts.Tables += record->Tables.size();
        }
        return counts;
    }

    std::string RttiRuntime::StatusText() const
    {
        const RttiCounts counts = Counts();
        return Format("%d-bit process %u: %zu modules, %zu classes and %zu virtual function tables seen", m_x64 ? 64 : 32, Raw(m_process->Id()), counts.Modules,
            counts.Classes, counts.Tables);
    }

    std::optional<std::uint64_t> RttiRuntime::ResolveSymbol(std::string_view token)
    {
        static constexpr std::string_view kSuffix = "::vftable";
        if (token.size() > 512 || token.find(kSuffix) == std::string_view::npos)
            return std::nullopt;
        std::string_view name;
        std::string_view which;
        if (const std::size_t at = token.find("::vftable@"); at != std::string_view::npos && at + kSuffix.size() + 1 < token.size())
        {
            name = token.substr(0, at);
            which = token.substr(at + kSuffix.size() + 1);
        }
        else if (token.size() > kSuffix.size() && token.substr(token.size() - kSuffix.size()) == kSuffix)
        {
            name = token.substr(0, token.size() - kSuffix.size());
        }
        if (name.empty())
            return std::nullopt;
        const auto primary = Primary(name, which);
        if (!primary)
            return std::nullopt;
        return primary->first;
    }
}
