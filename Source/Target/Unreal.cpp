#include "Target/Unreal.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Scan/Scanner.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint64_t kPointerMin = 0x10000;
        constexpr std::uint64_t kPointerMax = 0x7FFFFFFFFFFF;
        constexpr std::uint64_t kNameBlocksAt = 0x10;
        constexpr std::uint32_t kNameMaxBlocks = 8192;
        constexpr std::size_t kNameMaxLength = 1024;
        constexpr std::size_t kNamePeek = 0x3E;
        constexpr std::uint32_t kLegacyPerChunk = 0x4000;
        constexpr int kObjectsPerChunk = 0x10000;
        constexpr int kMinObjects = 0x800;
        constexpr int kMaxObjects = 0x10000000;
        constexpr std::uint64_t kSectionStep = 0x10000;
        constexpr int kRawBlockObjects = 0x1000;
        constexpr std::uint32_t kFuncExec = 0x200;
        constexpr std::uint32_t kFuncNative = 0x400;
        constexpr std::uint32_t kFuncEvent = 0x800;
        constexpr std::uint32_t kFuncStatic = 0x2000;
        constexpr std::uint32_t kFuncBlueprintEvent = 0x08000000;
        constexpr const char* kCoreUObjectUtf16 = "2F 00 53 00 63 00 72 00 69 00 70 00 74 00 2F 00 43 00 6F 00 72 00 65 00 55 00 4F 00 62 00 6A 00 65 00 63 00 74 00";
        constexpr const char* kNoneByteProperty = "4E 6F 6E 65 ?? ?? 42 79 74 65 50 72 6F 70 65 72 74 79";

        bool IsPtr(std::uint64_t value)
        {
            return value >= kPointerMin && value <= kPointerMax && (value & 7) == 0;
        }

        bool EndsWith(std::string_view text, std::string_view tail)
        {
            return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
        }

        bool IsPropertyKind(std::string_view kind)
        {
            return EndsWith(kind, "Property");
        }

        bool Overlaps(int a, int aWidth, int b, int bWidth)
        {
            return a < b + bWidth && b < a + aWidth;
        }

        std::string PointerPattern(std::uint64_t value)
        {
            std::string text;
            for (int i = 0; i < 8; ++i)
            {
                if (i)
                    text += ' ';
                text += Hex((value >> (i * 8)) & 0xFF, 2);
            }
            return text;
        }

        template <class T>
        T Get(const std::uint8_t* data, std::size_t offset)
        {
            T value{};
            std::memcpy(&value, data + offset, sizeof(T));
            return value;
        }

        template <class T>
        std::optional<T> From(const std::vector<std::uint8_t>* block, int offset)
        {
            if (!block || offset < 0 || static_cast<std::size_t>(offset) + sizeof(T) > block->size())
                return std::nullopt;
            return Get<T>(block->data(), static_cast<std::size_t>(offset));
        }

        UnrealLayout ArrayLayout(bool chunked, int objects, int max, int num, int maxChunks, int numChunks)
        {
            UnrealLayout layout;
            layout.Chunked = chunked;
            layout.ObjectsAt = objects;
            layout.MaxAt = max;
            layout.NumAt = num;
            layout.MaxChunksAt = maxChunks;
            layout.NumChunksAt = numChunks;
            return layout;
        }

        const UnrealLayout kArrayLayouts[] = {
            ArrayLayout(true, 0x00, 0x10, 0x14, 0x18, 0x1C),
            ArrayLayout(true, 0x10, 0x00, 0x04, 0x08, 0x0C),
            ArrayLayout(true, 0x18, 0x10, 0x00, 0x14, 0x20),
            ArrayLayout(true, 0x18, 0x00, 0x14, 0x10, 0x04),
            ArrayLayout(false, 0x00, 0x08, 0x0C, 0, 0),
        };

        const int kItemSizes[] = { 0x18, 0x10, 0x20, 0x28, 0x30 };

        const std::unordered_set<std::string_view> kObjectKinds = { "ObjectProperty", "ObjectPtrProperty", "ClassProperty", "ClassPtrProperty" };
    }

    const char* UnrealKindName(UnrealKind kind)
    {
        switch (kind)
        {
        case UnrealKind::Class: return "class";
        case UnrealKind::Struct: return "struct";
        case UnrealKind::Enum: return "enum";
        case UnrealKind::Function: return "function";
        default: return "";
        }
    }

    bool IsObjectProperty(std::string_view kind)
    {
        return kObjectKinds.count(kind) > 0;
    }

    std::optional<std::uint64_t> UnrealRuntime::ReadPointer(std::uint64_t address) const
    {
        if (!address || !m_process)
            return std::nullopt;
        return m_process->ReadValue<std::uint64_t>(address);
    }

    std::uint64_t UnrealRuntime::ReadObject(std::uint64_t address) const
    {
        const auto value = ReadPointer(address);
        return value && IsPtr(*value) ? *value : 0;
    }

    std::optional<std::int32_t> UnrealRuntime::ReadInt(std::uint64_t address) const
    {
        if (!address || !m_process)
            return std::nullopt;
        return m_process->ReadValue<std::int32_t>(address);
    }

    std::optional<std::uint32_t> UnrealRuntime::ReadUInt(std::uint64_t address) const
    {
        if (!address || !m_process)
            return std::nullopt;
        return m_process->ReadValue<std::uint32_t>(address);
    }

    std::optional<std::uint16_t> UnrealRuntime::ReadWord(std::uint64_t address) const
    {
        if (!address || !m_process)
            return std::nullopt;
        return m_process->ReadValue<std::uint16_t>(address);
    }

    std::optional<std::uint64_t> UnrealRuntime::ReadQword(std::uint64_t address) const
    {
        return ReadPointer(address);
    }

    bool UnrealRuntime::ReadBlock(std::uint64_t address, std::vector<std::uint8_t>& out, std::size_t size) const
    {
        out.resize(size);
        if (!address || !m_process || !size)
            return false;
        return m_process->ReadExact(address, out.data(), size);
    }

    std::optional<std::string> UnrealRuntime::ReadWide(std::uint64_t address, std::size_t length) const
    {
        if (!address || !length || !m_process)
            return std::nullopt;
        std::wstring text(length, L'\0');
        const std::size_t got = m_process->Read(address, text.data(), length * 2) / 2;
        if (got == 0)
            return std::nullopt;
        text.resize(got);
        const std::size_t end = text.find(L'\0');
        if (end != std::wstring::npos)
            text.resize(end);
        return WideToUtf8(text);
    }

    std::optional<std::string> UnrealRuntime::ReadText(std::uint64_t address, std::size_t length) const
    {
        if (!address || !length || !m_process)
            return std::nullopt;
        std::string text(length, '\0');
        const std::size_t got = m_process->Read(address, text.data(), length);
        if (got == 0)
            return std::nullopt;
        text.resize(got);
        const std::size_t end = text.find('\0');
        if (end != std::string::npos)
            text.resize(end);
        return text;
    }

    std::vector<std::uint64_t> UnrealRuntime::FindBytes(const std::string& text, std::uint64_t start, std::uint64_t stop, bool writable, std::uint64_t alignment,
        std::size_t limit) const
    {
        const auto pattern = Scan::ParsePattern(text);
        if (!pattern || !m_process || Cancelled())
            return {};
        Scan::PatternFilter filter;
        if (writable)
            filter.Writable = Scan::Tristate::Yes;
        filter.Divisor = alignment > 1 ? alignment : 0;
        Scan::PatternProgress progress;
        progress.Stop = [this] { return Cancelled(); };
        std::vector<std::uint64_t> hits = Scan::FindPattern(*m_process, *pattern, start, stop, filter, limit * 4 + 16, &progress);
        if (Cancelled())
            return {};
        std::sort(hits.begin(), hits.end());
        if (hits.size() > limit)
            hits.resize(limit);
        return hits;
    }

    bool UnrealRuntime::Cancelled() const
    {
        return m_progress && m_progress->Cancelled();
    }

    bool UnrealRuntime::InImage(std::uint64_t address) const
    {
        auto it = std::upper_bound(m_images.begin(), m_images.end(), address, [](std::uint64_t value, const auto& image) { return value < image.first; });
        if (it == m_images.begin())
            return false;
        --it;
        return address < it->second;
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> UnrealRuntime::WritableSections(const ModuleInfo& image) const
    {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        const auto peOffset = ReadUInt(image.Base + 0x3C);
        if (!peOffset)
            return out;
        const std::uint64_t pe = image.Base + *peOffset;
        const auto count = ReadWord(pe + 6);
        const auto optional = ReadWord(pe + 0x14);
        if (!count || !optional)
            return out;
        const std::uint64_t first = pe + 0x18 + *optional;
        for (std::uint16_t i = 0; i < *count; ++i)
        {
            const std::uint64_t section = first + static_cast<std::uint64_t>(i) * 0x28;
            const auto size = ReadUInt(section + 8);
            const auto rva = ReadUInt(section + 0xC);
            const auto flags = ReadUInt(section + 0x24);
            if (!size || !rva || !flags || *size == 0)
                continue;
            const bool writable = (*flags & 0x80000000u) != 0;
            const bool executable = (*flags & 0x20000000u) != 0;
            if (writable && !executable)
                out.push_back({ image.Base + *rva, *size });
        }
        return out;
    }

    bool UnrealRuntime::LooksUnreal(const Process& process)
    {
        const auto main = process.MainModule();
        if (!main)
            return false;
        const std::string name = main->Name;
        const std::string path = Lower(WideToUtf8(main->Path));
        if (name.find("-Win64-") != std::string::npos || name.find("-Win32-") != std::string::npos || name.find("-WinGDK-") != std::string::npos ||
            name.find("Shipping") != std::string::npos || name.find("UE4") != std::string::npos)
            return true;
        if (path.find("\\binaries\\win64\\") != std::string::npos || path.find("\\binaries\\win32\\") != std::string::npos ||
            path.find("\\binaries\\wingdk\\") != std::string::npos)
            return true;
        if (const auto modules = process.Modules())
        {
            for (const ModuleInfo& module : *modules)
            {
                if (module.Name.find("-CoreUObject") != std::string::npos)
                    return true;
            }
        }
        const auto pattern = Scan::ParsePattern(kCoreUObjectUtf16);
        if (!pattern)
            return false;
        Scan::PatternFilter filter;
        return Scan::FindPattern(process, *pattern, main->Base, main->Base + main->Size, filter, 2).size() == 1;
    }

    std::optional<std::string> UnrealRuntime::PoolEntry(std::uint64_t entry) const
    {
        std::uint8_t head[2 + kNamePeek] = {};
        const std::size_t got = m_process->Read(entry, head, sizeof(head));
        std::uint16_t header = 0;
        if (got >= 2)
            header = Get<std::uint16_t>(head, 0);
        else if (const auto word = ReadWord(entry))
            header = *word;
        else
            return std::nullopt;
        const std::size_t length = header >> 6;
        const bool wide = (header & 1) != 0;
        if (length == 0 || length > kNameMaxLength)
            return std::nullopt;
        if (wide)
            return ReadWide(entry + 2, length);
        if (got >= 2 + length)
            return std::string(reinterpret_cast<const char*>(head + 2), length);
        std::string text(length, '\0');
        if (!m_process->ReadExact(entry + 2, text.data(), length))
            return std::nullopt;
        return text;
    }

    std::optional<std::string> UnrealRuntime::LegacyEntry(std::uint64_t entry, int indexAt, int nameAt) const
    {
        if (!entry)
            return std::nullopt;
        const auto index = ReadUInt(entry + static_cast<std::uint64_t>(indexAt));
        if (!index)
            return std::nullopt;
        const auto text = (*index & 1) ? ReadWide(entry + static_cast<std::uint64_t>(nameAt), kNameMaxLength) : ReadText(entry + static_cast<std::uint64_t>(nameAt), kNameMaxLength);
        if (!text || text->empty())
            return std::nullopt;
        return text;
    }

    std::optional<std::string> UnrealRuntime::NameText(std::uint32_t index) const
    {
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_names.find(index); it != m_names.end())
                return it->second;
        }
        if (!m_pool)
            return std::nullopt;
        std::optional<std::string> text;
        if (!m_legacyNames)
        {
            const std::uint32_t block = index >> 16;
            if (block >= kNameMaxBlocks)
                return std::nullopt;
            std::uint64_t base = 0;
            {
                std::lock_guard lock(m_lock);
                if (const auto it = m_blocks.find(block); it != m_blocks.end())
                    base = it->second;
            }
            if (!base)
            {
                const auto value = ReadPointer(m_pool + kNameBlocksAt + static_cast<std::uint64_t>(block) * 8);
                if (!value || *value < kPointerMin || *value > kPointerMax)
                    return std::nullopt;
                base = *value;
                std::lock_guard lock(m_lock);
                m_blocks[block] = base;
            }
            text = PoolEntry(base + static_cast<std::uint64_t>(index & 0xFFFF) * 2);
        }
        else
        {
            const std::uint32_t chunkIndex = index / kLegacyPerChunk;
            std::uint64_t chunk = 0;
            {
                std::lock_guard lock(m_lock);
                if (const auto it = m_blocks.find(chunkIndex); it != m_blocks.end())
                    chunk = it->second;
            }
            if (!chunk)
            {
                chunk = ReadObject(m_pool + static_cast<std::uint64_t>(chunkIndex) * 8);
                if (!chunk)
                    return std::nullopt;
                std::lock_guard lock(m_lock);
                m_blocks[chunkIndex] = chunk;
            }
            const auto entry = ReadPointer(chunk + static_cast<std::uint64_t>(index % kLegacyPerChunk) * 8);
            if (entry)
                text = LegacyEntry(*entry, m_legacyIndexAt, m_legacyNameAt);
        }
        if (text)
        {
            std::lock_guard lock(m_lock);
            m_names[index] = *text;
        }
        return text;
    }

    std::optional<std::string> UnrealRuntime::NameFromParts(std::uint32_t index, std::uint32_t number) const
    {
        auto text = NameText(index);
        if (!text)
            return std::nullopt;
        if (number > 0)
            *text += "_" + std::to_string(number - 1);
        return text;
    }

    std::optional<std::string> UnrealRuntime::NameAt(std::uint64_t address) const
    {
        const auto value = ReadQword(address);
        if (!value)
            return std::nullopt;
        return NameFromParts(static_cast<std::uint32_t>(*value & 0xFFFFFFFF), static_cast<std::uint32_t>(*value >> 32));
    }

    std::optional<std::string> UnrealRuntime::NameOf(std::uint64_t object) const
    {
        if (!object || m_layout.Name < 0)
            return std::nullopt;
        return NameAt(object + static_cast<std::uint64_t>(m_layout.Name));
    }

    std::optional<std::uint64_t> UnrealRuntime::FindName(std::string_view text) const
    {
        std::string base;
        std::uint32_t number = 0;
        const std::size_t cut = text.rfind('_');
        if (cut != std::string_view::npos && cut + 1 < text.size() && std::all_of(text.begin() + static_cast<std::ptrdiff_t>(cut) + 1, text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        {
            const auto suffix = cut + 11 >= text.size() ? ParseDecimal(text.substr(cut + 1)) : std::nullopt;
            if (suffix && *suffix >= 0 && *suffix < 0xFFFFFFFFll)
            {
                base = std::string(text.substr(0, cut));
                number = static_cast<std::uint32_t>(*suffix) + 1;
            }
        }
        std::optional<std::uint64_t> numbered;
        std::lock_guard lock(m_lock);
        for (const auto& [index, name] : m_names)
        {
            if (name == text)
                return static_cast<std::uint64_t>(index);
            if (!base.empty() && name == base && (!numbered || index < (*numbered & 0xFFFFFFFFull)))
                numbered = static_cast<std::uint64_t>(index) | (static_cast<std::uint64_t>(number) << 32);
        }
        return numbered;
    }

    std::uint64_t UnrealRuntime::NamePoolAmong(const std::vector<std::uint64_t>& slots) const
    {
        for (const std::uint64_t slot : slots)
        {
            const std::uint64_t pool = slot - kNameBlocksAt;
            if (!InImage(slot))
                continue;
            const auto current = ReadUInt(pool + 8);
            const auto cursor = ReadUInt(pool + 0xC);
            if (current && cursor && *current < kNameMaxBlocks && *cursor < 0x20000)
                return pool;
        }
        return 0;
    }

    std::uint64_t UnrealRuntime::FindNamePool() const
    {
        for (const std::uint64_t text : FindBytes(kNoneByteProperty, 0, ~0ull, true, 2, 64))
        {
            const std::uint64_t block = text - 2;
            const auto header = ReadWord(block);
            if (!header || (*header >> 6) != 4)
                continue;
            const std::string pattern = PointerPattern(block);
            std::uint64_t pool = 0;
            if (m_main.Size)
                pool = NamePoolAmong(FindBytes(pattern, m_main.Base, m_main.Base + m_main.Size, true, 8, 64));
            if (!pool)
                pool = NamePoolAmong(FindBytes(pattern, 0, ~0ull, true, 8, 64));
            if (pool)
                return pool;
        }
        return 0;
    }

    bool UnrealRuntime::IsNameEntryArray(std::uint64_t chunks, int nameAt)
    {
        const std::uint64_t chunk = ReadObject(chunks);
        if (!chunk)
            return false;
        std::uint64_t entries[3] = {};
        for (int i = 0; i < 3; ++i)
        {
            entries[i] = ReadObject(chunk + static_cast<std::uint64_t>(i) * 8);
            if (!entries[i])
                return false;
        }
        for (int indexAt = 0; indexAt <= nameAt - 4; indexAt += 4)
        {
            bool matches = true;
            for (int slot = 0; slot < 3 && matches; ++slot)
                matches = ReadUInt(entries[slot] + static_cast<std::uint64_t>(indexAt)) == static_cast<std::uint32_t>(slot << 1);
            if (matches && LegacyEntry(entries[1], indexAt, nameAt) == "ByteProperty" && LegacyEntry(entries[2], indexAt, nameAt) == "IntProperty")
            {
                m_legacyIndexAt = indexAt;
                m_legacyNameAt = nameAt;
                return true;
            }
        }
        return false;
    }

    std::uint64_t UnrealRuntime::FindNameEntryArray()
    {
        for (const int nameAt : { 0x10, 0x0C, 0x08 })
        {
            std::string pattern = "00 00 00 00 ";
            for (int i = 0; i < nameAt - 4; ++i)
                pattern += "?? ";
            pattern += "4E 6F 6E 65 00";
            for (const std::uint64_t entry : FindBytes(pattern, 0, ~0ull, true, 4, 32))
            {
                for (const std::uint64_t slot : FindBytes(PointerPattern(entry), 0, ~0ull, true, 8, 8))
                {
                    for (const std::uint64_t chunks : FindBytes(PointerPattern(slot), 0, ~0ull, true, 8, 8))
                    {
                        if (IsNameEntryArray(chunks, nameAt))
                            return chunks;
                    }
                }
            }
        }
        return 0;
    }

    std::uint64_t UnrealRuntime::PatternTarget(const UnrealPattern& pattern) const
    {
        std::optional<ModuleInfo> module = pattern.Module.empty() ? std::optional<ModuleInfo>(m_main) : m_process->FindModule(pattern.Module);
        if (!module || !module->Size)
            return 0;
        for (const std::uint64_t hit : FindBytes(pattern.Bytes, module->Base, module->Base + module->Size, false, 0, 4))
        {
            std::uint64_t target = 0;
            if (pattern.Relative)
            {
                const auto displacement = ReadInt(hit + static_cast<std::uint64_t>(pattern.Offset));
                if (!displacement)
                    continue;
                const int length = pattern.Length > 0 ? pattern.Length : pattern.Offset + 4;
                target = hit + static_cast<std::uint64_t>(length) + static_cast<std::uint64_t>(static_cast<std::int64_t>(*displacement));
            }
            else
            {
                const auto pointer = ReadPointer(hit + static_cast<std::uint64_t>(pattern.Offset));
                if (!pointer)
                    continue;
                target = *pointer;
            }
            target = static_cast<std::uint64_t>(static_cast<std::int64_t>(target) + pattern.Add);
            if (target)
                return target;
        }
        return 0;
    }

    bool UnrealRuntime::CheckNamePool(std::uint64_t pool, bool legacy)
    {
        if (!legacy)
        {
            const std::uint64_t block = ReadObject(pool + kNameBlocksAt);
            if (!block)
                return false;
            const auto header = ReadWord(block);
            const auto text = ReadText(block + 2, 4);
            return header && (*header >> 6) == 4 && text == "None";
        }
        for (const int nameAt : { 0x10, 0x0C, 0x08 })
        {
            if (IsNameEntryArray(pool, nameAt))
                return true;
        }
        return false;
    }

    bool UnrealRuntime::Plausible(const std::uint8_t* data, const UnrealLayout& layout) const
    {
        const std::uint64_t objects = Get<std::uint64_t>(data, static_cast<std::size_t>(layout.ObjectsAt));
        if (!IsPtr(objects) || InImage(objects))
            return false;
        const std::int32_t max = Get<std::int32_t>(data, static_cast<std::size_t>(layout.MaxAt));
        const std::int32_t num = Get<std::int32_t>(data, static_cast<std::size_t>(layout.NumAt));
        if (num <= kMinObjects || max < num || max > kMaxObjects)
            return false;
        if (!layout.Chunked)
            return true;
        const std::int32_t maxChunks = Get<std::int32_t>(data, static_cast<std::size_t>(layout.MaxChunksAt));
        const std::int32_t numChunks = Get<std::int32_t>(data, static_cast<std::size_t>(layout.NumChunksAt));
        if (numChunks < 1 || maxChunks < numChunks || maxChunks > kMaxObjects / kObjectsPerChunk + 1)
            return false;
        return static_cast<std::int64_t>(numChunks) * kObjectsPerChunk >= num && static_cast<std::int64_t>(maxChunks) * kObjectsPerChunk >= max;
    }

    bool UnrealRuntime::ProbeItems(std::uint64_t array, UnrealLayout& layout) const
    {
        const std::uint64_t objects = ReadPointer(array + static_cast<std::uint64_t>(layout.ObjectsAt)).value_or(0);
        const std::uint64_t base = layout.Chunked ? ReadPointer(objects).value_or(0) : objects;
        if (!IsPtr(base))
            return false;
        std::vector<std::uint8_t> raw;
        if (!ReadBlock(base, raw, 64 * 0x30))
            return false;
        std::unordered_map<std::uint64_t, bool> cache;
        const auto vtableIn = [&](std::uint64_t object)
            {
                if (!IsPtr(object))
                    return false;
                if (const auto it = cache.find(object); it != cache.end())
                    return it->second;
                const auto vtable = ReadPointer(object);
                const bool known = vtable && InImage(*vtable);
                cache[object] = known;
                return known;
            };
        for (const int size : kItemSizes)
        {
            for (int offset = 0; offset <= std::min(28, size - 8); offset += 4)
            {
                int good = 0;
                std::unordered_set<std::uint64_t> seen;
                for (int i = 0; i < 64; ++i)
                {
                    const std::uint64_t object = Get<std::uint64_t>(raw.data(), static_cast<std::size_t>(i * size + offset));
                    if (!seen.count(object) && vtableIn(object))
                    {
                        ++good;
                        seen.insert(object);
                    }
                }
                if (good >= 56)
                {
                    layout.ItemSize = size;
                    layout.ItemOffset = offset;
                    return true;
                }
            }
        }
        return false;
    }

    bool UnrealRuntime::FirstObjects(std::uint64_t array, LiveArray& live) const
    {
        const std::uint64_t objects = ReadPointer(array + static_cast<std::uint64_t>(live.Layout.ObjectsAt)).value_or(0);
        const std::uint64_t base = live.Layout.Chunked ? ReadPointer(objects).value_or(0) : objects;
        std::vector<std::uint8_t> raw;
        if (!base || !ReadBlock(base, raw, static_cast<std::size_t>(64 * live.Layout.ItemSize)))
            return false;
        live.First.assign(64, 0);
        live.Headers.assign(64, {});
        for (int i = 0; i < 64; ++i)
        {
            const std::uint64_t object = Get<std::uint64_t>(raw.data(), static_cast<std::size_t>(i * live.Layout.ItemSize + live.Layout.ItemOffset));
            live.First[static_cast<std::size_t>(i)] = object;
            if (IsPtr(object) && !ReadBlock(object, live.Headers[static_cast<std::size_t>(i)], 0x48))
                live.Headers[static_cast<std::size_t>(i)].clear();
            if (!IsPtr(object))
                live.Headers[static_cast<std::size_t>(i)].clear();
        }
        return true;
    }

    std::optional<std::string> UnrealRuntime::HeaderName(const LiveArray& live, int i, int offset) const
    {
        const auto& header = live.Headers[static_cast<std::size_t>(i)];
        if (header.size() < static_cast<std::size_t>(offset) + 8)
            return std::nullopt;
        return NameFromParts(Get<std::uint32_t>(header.data(), static_cast<std::size_t>(offset)), Get<std::uint32_t>(header.data(), static_cast<std::size_t>(offset) + 4));
    }

    std::optional<UnrealRuntime::LiveArray> UnrealRuntime::LiveLayout(std::uint64_t array, const UnrealLayout& layout) const
    {
        LiveArray live;
        live.Array = array;
        live.Layout = layout;
        if (const auto size = Override("itemSize"))
        {
            live.Layout.ItemSize = *size;
            live.Layout.ItemOffset = Override("itemOffset").value_or(0);
        }
        else if (!ProbeItems(array, live.Layout))
        {
            return std::nullopt;
        }
        if (!FirstObjects(array, live))
            return std::nullopt;
        if (const auto index = Override("index"))
            live.Layout.Index = *index;
        for (const int offset : { 0x0C, 0x08, 0x10, 0x14, 0x04, 0x18, 0x1C, 0x20 })
        {
            if (live.Layout.Index >= 0)
                break;
            int hits = 0;
            for (int i = 0; i < 64; ++i)
            {
                const auto& header = live.Headers[static_cast<std::size_t>(i)];
                if (header.size() >= static_cast<std::size_t>(offset) + 4 && Get<std::int32_t>(header.data(), static_cast<std::size_t>(offset)) == i)
                    ++hits;
            }
            if (hits >= 58)
            {
                live.Layout.Index = offset;
                break;
            }
        }
        if (live.Layout.Index < 0)
            return std::nullopt;
        std::vector<int> candidates{ 0x18 };
        for (int offset = 0x08; offset <= 0x40; offset += 4)
            candidates.push_back(offset);
        if (const auto name = Override("name"))
            candidates = { *name };
        for (const int offset : candidates)
        {
            if (Overlaps(offset, 8, live.Layout.Index, 4))
                continue;
            int decoded = 0;
            bool anchor = false;
            std::unordered_set<std::string> seen;
            for (int i = 0; i < 64; ++i)
            {
                const auto name = HeaderName(live, i, offset);
                if (!name)
                    continue;
                ++decoded;
                anchor = anchor || *name == "/Script/CoreUObject" || *name == "Object";
                seen.insert(*name);
            }
            if ((anchor && decoded >= 58 && static_cast<int>(seen.size()) * 4 >= decoded * 3) || Override("name"))
            {
                live.Layout.Name = offset;
                return live;
            }
        }
        return std::nullopt;
    }

    std::optional<UnrealRuntime::LiveArray> UnrealRuntime::ScanSection(std::uint64_t from, std::uint64_t size, bool generic) const
    {
        const std::uint64_t stop = from + size;
        std::vector<std::uint8_t> data;
        for (std::uint64_t at = from; at + 0x28 <= stop; at += kSectionStep)
        {
            const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(kSectionStep + 0x40, stop - at));
            data.resize(length);
            const std::size_t got = m_process->Read(at, data.data(), length);
            if (got < 0x28)
                continue;
            const std::size_t last = std::min<std::size_t>(static_cast<std::size_t>(kSectionStep), got - 0x28);
            for (std::size_t pos = (8 - (at % 8)) % 8; pos <= last; pos += 8)
            {
                if (generic)
                {
                    for (const UnrealLayout& layout : kArrayLayouts)
                    {
                        if (Plausible(data.data() + pos, layout))
                        {
                            if (auto live = LiveLayout(at + pos, layout))
                                return live;
                        }
                    }
                }
                else if (Plausible(data.data() + pos, kArrayLayouts[0]))
                {
                    if (auto live = LiveLayout(at + pos, kArrayLayouts[0]))
                        return live;
                }
            }
        }
        return std::nullopt;
    }

    std::optional<UnrealRuntime::LiveArray> UnrealRuntime::CheckObjects(std::uint64_t array) const
    {
        std::vector<std::uint8_t> data;
        for (std::uint64_t offset = 0; offset <= 0x50; offset += 8)
        {
            if (!ReadBlock(array + offset, data, 0x28))
                continue;
            for (const UnrealLayout& layout : kArrayLayouts)
            {
                if (Plausible(data.data(), layout))
                {
                    if (auto live = LiveLayout(array + offset, layout))
                        return live;
                }
            }
        }
        return std::nullopt;
    }

    std::optional<UnrealRuntime::LiveArray> UnrealRuntime::ExportedObjectArray() const
    {
        const auto modules = m_process->Modules();
        if (!modules)
            return std::nullopt;
        for (const ModuleInfo& module : *modules)
        {
            if (module.Name.find("-CoreUObject") == std::string::npos)
                continue;
            for (const char* symbol : { "?GUObjectArray@@3VFUObjectArray@@A", "GUObjectArray" })
            {
                const std::uint64_t address = m_process->FindExport(module.Name, symbol);
                if (!address)
                    continue;
                if (auto live = CheckObjects(address))
                    return live;
            }
        }
        return std::nullopt;
    }

    std::optional<std::pair<UnrealRuntime::LiveArray, std::string>> UnrealRuntime::FindObjectArray() const
    {
        if (auto live = ExportedObjectArray())
            return std::make_pair(std::move(*live), std::string("export"));
        std::vector<ModuleInfo> images;
        if (const auto modules = m_process->Modules())
        {
            for (const ModuleInfo& module : *modules)
            {
                if (module.Base == m_main.Base || module.Name.find("-CoreUObject") != std::string::npos || module.Name.find("-Core-") != std::string::npos)
                    images.push_back(module);
            }
        }
        for (const bool generic : { false, true })
        {
            for (const ModuleInfo& image : images)
            {
                for (const auto& [from, size] : WritableSections(image))
                {
                    if (auto live = ScanSection(from, size, generic))
                        return std::make_pair(std::move(*live), std::string(generic ? "data scan (generic)" : "data scan"));
                }
            }
        }
        return std::nullopt;
    }

    int UnrealRuntime::Count() const
    {
        const auto count = ReadInt(m_array + static_cast<std::uint64_t>(m_layout.NumAt));
        if (!count || *count < 0 || *count > kMaxObjects)
            return 0;
        return *count;
    }

    std::uint64_t UnrealRuntime::Chunk(int index) const
    {
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_chunks.find(index); it != m_chunks.end())
                return it->second;
        }
        const std::uint64_t table = ReadPointer(m_array + static_cast<std::uint64_t>(m_layout.ObjectsAt)).value_or(0);
        const std::uint64_t chunk = table ? ReadObject(table + static_cast<std::uint64_t>(index) * 8) : 0;
        if (chunk)
        {
            std::lock_guard lock(m_lock);
            m_chunks[index] = chunk;
        }
        return chunk;
    }

    std::uint64_t UnrealRuntime::ItemAddress(int index) const
    {
        if (m_layout.Chunked)
        {
            const std::uint64_t chunk = Chunk(index / kObjectsPerChunk);
            return chunk ? chunk + static_cast<std::uint64_t>(index % kObjectsPerChunk) * static_cast<std::uint64_t>(m_layout.ItemSize) : 0;
        }
        const std::uint64_t items = ReadPointer(m_array + static_cast<std::uint64_t>(m_layout.ObjectsAt)).value_or(0);
        return items ? items + static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(m_layout.ItemSize) : 0;
    }

    std::optional<std::uint64_t> UnrealRuntime::ObjectAt(int index) const
    {
        if (index < 0 || !m_array || index >= Count())
            return std::nullopt;
        const std::uint64_t item = ItemAddress(index);
        const std::uint64_t object = item ? ReadObject(item + static_cast<std::uint64_t>(m_layout.ItemOffset)) : 0;
        if (!object)
            return std::nullopt;
        return object;
    }

    void UnrealRuntime::Each(const std::function<bool(std::uint64_t object, int index)>& visit) const
    {
        const int count = Count();
        std::vector<std::uint8_t> raw;
        for (int first = 0; first < count; first += kRawBlockObjects)
        {
            const int n = std::min(kRawBlockObjects, count - first);
            const std::uint64_t base = ItemAddress(first);
            if (!base || !ReadBlock(base, raw, static_cast<std::size_t>(n) * static_cast<std::size_t>(m_layout.ItemSize)))
                continue;
            for (int i = 0; i < n; ++i)
            {
                const std::uint64_t object = Get<std::uint64_t>(raw.data(), static_cast<std::size_t>(i) * static_cast<std::size_t>(m_layout.ItemSize) + static_cast<std::size_t>(m_layout.ItemOffset));
                if (IsPtr(object) && visit(object, first + i))
                    return;
            }
        }
    }

    bool UnrealRuntime::IsObject(std::uint64_t address) const
    {
        if (!IsPtr(address) || m_layout.Index < 0)
            return false;
        const auto index = ReadInt(address + static_cast<std::uint64_t>(m_layout.Index));
        if (!index || *index < 0 || *index >= Count())
            return false;
        return ObjectAt(*index) == address;
    }

    std::uint64_t UnrealRuntime::ClassOf(std::uint64_t object) const
    {
        return object && m_layout.Class >= 0 ? ReadObject(object + static_cast<std::uint64_t>(m_layout.Class)) : 0;
    }

    std::uint64_t UnrealRuntime::OuterOf(std::uint64_t object) const
    {
        return object && m_layout.Outer >= 0 ? ReadObject(object + static_cast<std::uint64_t>(m_layout.Outer)) : 0;
    }

    std::uint64_t UnrealRuntime::SuperOf(std::uint64_t type) const
    {
        return IsPtr(type) && m_layout.Super >= 0 ? ReadObject(type + static_cast<std::uint64_t>(m_layout.Super)) : 0;
    }

    bool UnrealRuntime::HasStructLayout(std::uint64_t type) const
    {
        const UnrealKind kind = KindOf(ClassOf(type));
        return kind == UnrealKind::Class || kind == UnrealKind::Struct || kind == UnrealKind::Function;
    }

    bool UnrealRuntime::ResolveClassAndOuter(const LiveArray& live)
    {
        std::uint64_t objectClass = 0;
        std::uint64_t package = 0;
        for (int i = 0; i < 64; ++i)
        {
            const auto name = HeaderName(live, i, m_layout.Name);
            if (name == "Object" && !objectClass)
                objectClass = live.First[static_cast<std::size_t>(i)];
            if (name == "/Script/CoreUObject" && !package)
                package = live.First[static_cast<std::size_t>(i)];
        }
        if (!objectClass || !package)
            return false;
        if (m_layout.Class < 0)
        {
            for (const int offset : { 0x10, 0x08, 0x18, 0x20, 0x28, 0x30, 0x38 })
            {
                if (Overlaps(offset, 8, m_layout.Name, 8) || Overlaps(offset, 8, m_layout.Index, 4))
                    continue;
                const std::uint64_t meta = ReadObject(objectClass + static_cast<std::uint64_t>(offset));
                const std::uint64_t packageMeta = ReadObject(package + static_cast<std::uint64_t>(offset));
                if (meta && packageMeta && NameAt(meta + static_cast<std::uint64_t>(m_layout.Name)) == "Class" &&
                    NameAt(packageMeta + static_cast<std::uint64_t>(m_layout.Name)) == "Package")
                {
                    m_layout.Class = offset;
                    break;
                }
            }
        }
        if (m_layout.Class < 0)
            return false;
        if (m_layout.Outer < 0)
        {
            for (const int offset : { 0x20, 0x28, 0x18, 0x10, 0x30, 0x38, 0x40 })
            {
                if (!Overlaps(offset, 8, m_layout.Name, 8) && offset != m_layout.Class && ReadPointer(objectClass + static_cast<std::uint64_t>(offset)) == package)
                {
                    m_layout.Outer = offset;
                    break;
                }
            }
        }
        return m_layout.Outer >= 0;
    }

    UnrealKind UnrealRuntime::KindOf(std::uint64_t meta) const
    {
        if (!meta)
            return UnrealKind::None;
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_kinds.find(meta); it != m_kinds.end())
                return it->second;
        }
        const std::string name = Lower(NameOf(meta).value_or(""));
        UnrealKind kind = UnrealKind::None;
        if (name == "class" || name == "dynamicclass" || EndsWith(name, "generatedclass"))
            kind = UnrealKind::Class;
        else if (name == "scriptstruct" || name == "userdefinedstruct")
            kind = UnrealKind::Struct;
        else if (name == "enum" || name == "userdefinedenum")
            kind = UnrealKind::Enum;
        else if (name == "function" || name == "delegatefunction" || name == "sparsedelegatefunction")
            kind = UnrealKind::Function;
        std::lock_guard lock(m_lock);
        m_kinds[meta] = kind;
        return kind;
    }

    void UnrealRuntime::IndexTypes()
    {
        m_types.clear();
        m_enums.clear();
        m_typeList.clear();
        const int first = std::min(m_layout.Class, m_layout.Name);
        const std::size_t span = static_cast<std::size_t>(std::max(m_layout.Class, m_layout.Name) + 8 - first);
        std::vector<std::pair<std::uint64_t, int>> objects;
        objects.reserve(static_cast<std::size_t>(Count()));
        Each([&](std::uint64_t object, int index)
            {
                objects.push_back({ object, index });
                return false;
            });
        std::sort(objects.begin(), objects.end());
        constexpr std::uint64_t kPage = 0x1000;
        std::vector<std::uint8_t> page(kPage + span);
        std::uint64_t pageBase = 0;
        std::size_t pageSize = 0;
        for (const auto& [object, index] : objects)
        {
            const std::uint64_t at = object + static_cast<std::uint64_t>(first);
            if (!(pageSize && at >= pageBase && at + span <= pageBase + pageSize))
            {
                pageBase = at & ~(kPage - 1);
                pageSize = m_process->Read(pageBase, page.data(), page.size());
            }
            std::uint64_t meta = 0;
            std::optional<std::string> name;
            if (pageSize && at >= pageBase && at + span <= pageBase + pageSize)
            {
                const std::uint8_t* header = page.data() + (at - pageBase);
                meta = Get<std::uint64_t>(header, static_cast<std::size_t>(m_layout.Class - first));
                const std::size_t nameAt = static_cast<std::size_t>(m_layout.Name - first);
                if (IsPtr(meta))
                {
                    const UnrealKind kind = KindOf(meta);
                    if (kind == UnrealKind::Class || kind == UnrealKind::Struct || kind == UnrealKind::Enum)
                        name = NameFromParts(Get<std::uint32_t>(header, nameAt), Get<std::uint32_t>(header, nameAt + 4));
                }
            }
            else
            {
                meta = ClassOf(object);
                const UnrealKind kind = KindOf(meta);
                if (kind == UnrealKind::Class || kind == UnrealKind::Struct || kind == UnrealKind::Enum)
                    name = NameOf(object);
            }
            if (!name || !IsPtr(meta))
                continue;
            const UnrealKind kind = KindOf(meta);
            const std::string key = Lower(*name);
            if (kind == UnrealKind::Enum)
                m_enums.emplace(key, object);
            else
                m_types.emplace(key, object);
            m_typeList.push_back({ *name, object, kind });
        }
        std::sort(m_typeList.begin(), m_typeList.end(), [](const UnrealType& a, const UnrealType& b) { return Lower(a.Name) < Lower(b.Name); });
    }

    std::uint64_t UnrealRuntime::Type(std::string_view name) const
    {
        if (const auto it = m_types.find(Lower(name)); it != m_types.end())
            return it->second;
        return 0;
    }

    std::uint64_t UnrealRuntime::Enum(std::string_view name) const
    {
        if (const auto it = m_enums.find(Lower(name)); it != m_enums.end())
            return it->second;
        return 0;
    }

    bool UnrealRuntime::ProbeSuper()
    {
        const std::uint64_t structType = Type("Struct");
        const std::uint64_t field = Type("Field");
        const std::uint64_t object = Type("Object");
        const std::uint64_t classType = Type("Class");
        if (!structType || !field || !object || !classType)
            return false;
        if (m_layout.Super >= 0)
            return true;
        for (int offset = m_layout.Outer + 8; offset <= 0x80; offset += 8)
        {
            const auto at = static_cast<std::uint64_t>(offset);
            if (ReadPointer(structType + at) == field && ReadPointer(field + at) == object && ReadPointer(classType + at) == structType)
            {
                m_layout.Super = offset;
                return true;
            }
        }
        return false;
    }

    bool UnrealRuntime::ProbePropertyFields(std::uint64_t x, std::uint64_t y, std::uint64_t z)
    {
        for (int offset = 0x20; offset <= 0x90; offset += 4)
        {
            if (const auto preset = Override("offset"); preset && offset != *preset)
                continue;
            const auto at = static_cast<std::uint64_t>(offset);
            const auto a = ReadInt(x + at);
            const auto b = ReadInt(y + at);
            const auto c = ReadInt(z + at);
            if (a == 0 && b && (*b == 4 || *b == 8) && c == *b * 2)
            {
                m_layout.Offset = offset;
                m_layout.Component = *b;
                if (m_layout.Size < 0)
                {
                    for (int size = 0x24; size <= offset - 4; size += 4)
                    {
                        const auto s = static_cast<std::uint64_t>(size);
                        if (ReadInt(x + s) == *b && ReadInt(y + s) == *b && ReadInt(z + s) == *b && ReadInt(x + s - 4) == 1)
                        {
                            m_layout.Size = size;
                            break;
                        }
                    }
                }
                return m_layout.Size >= 0;
            }
        }
        return false;
    }

    bool UnrealRuntime::ProbeChain()
    {
        const std::uint64_t vector = Type("Vector");
        if (!vector)
            return false;
        for (int list = m_layout.Super + 8; list <= m_layout.Super + 0x48; list += 8)
        {
            if (m_layout.ChildProperties >= 0 && list != m_layout.ChildProperties)
                continue;
            const std::uint64_t x = ReadObject(vector + static_cast<std::uint64_t>(list));
            if (!x)
                continue;
            for (int name = 8; name <= 0x40; name += 4)
            {
                if (const auto preset = Override("fieldName"); preset && name != *preset)
                    continue;
                if (NameAt(x + static_cast<std::uint64_t>(name)) != "X")
                    continue;
                for (int next = 8; next <= 0x48; next += 8)
                {
                    if (const auto preset = Override("next"); preset && next != *preset)
                        continue;
                    if (next + 8 > name && next < name + 8)
                        continue;
                    const std::uint64_t y = ReadObject(x + static_cast<std::uint64_t>(next));
                    const std::uint64_t z = y && NameAt(y + static_cast<std::uint64_t>(name)) == "Y" ? ReadObject(y + static_cast<std::uint64_t>(next)) : 0;
                    if (!z || NameAt(z + static_cast<std::uint64_t>(name)) != "Z")
                        continue;
                    m_layout.ChildProperties = list;
                    m_layout.FieldName = name;
                    m_layout.Next = next;
                    const std::uint64_t meta = ClassOf(x);
                    m_layout.FField = !(name == m_layout.Name && meta && IsPropertyKind(NameOf(meta).value_or("")));
                    if (const auto preset = Override("fField"))
                        m_layout.FField = *preset != 0;
                    if (m_layout.FField && m_layout.FieldClass < 0)
                    {
                        for (int slot = 8; slot <= name - 8; slot += 8)
                        {
                            const std::uint64_t fieldClass = ReadObject(x + static_cast<std::uint64_t>(slot));
                            if (fieldClass && IsPropertyKind(NameAt(fieldClass).value_or("")))
                            {
                                m_layout.FieldClass = slot;
                                break;
                            }
                        }
                        if (m_layout.FieldClass < 0)
                            return false;
                    }
                    return ProbePropertyFields(x, y, z);
                }
            }
        }
        return false;
    }

    void UnrealRuntime::ProbeStructSize()
    {
        if (m_layout.StructSize >= 0)
            return;
        const int c = m_layout.Component;
        const std::pair<const char*, int> known[] = { { "Vector", 3 * c }, { "Vector2D", 2 * c }, { "LinearColor", 16 }, { "IntPoint", 8 } };
        for (int offset = m_layout.ChildProperties + 8; offset <= m_layout.ChildProperties + 0x40; offset += 4)
        {
            bool ok = true;
            for (const auto& [name, size] : known)
            {
                const std::uint64_t type = Type(name);
                ok = ok && type && ReadInt(type + static_cast<std::uint64_t>(offset)) == size;
            }
            if (ok)
            {
                m_layout.StructSize = offset;
                return;
            }
        }
    }

    std::optional<int> UnrealRuntime::StructSize(std::uint64_t type) const
    {
        if (!type || m_layout.StructSize < 0)
            return std::nullopt;
        const auto size = ReadInt(type + static_cast<std::uint64_t>(m_layout.StructSize));
        if (!size || *size <= 0 || *size >= 0x1000000)
            return std::nullopt;
        return *size;
    }

    std::uint64_t UnrealRuntime::FindRawField(std::uint64_t type, std::string_view wanted) const
    {
        std::uint64_t field = ReadObject(type + static_cast<std::uint64_t>(m_layout.ChildProperties));
        for (int guard = 0; field && guard < kMaxFields; ++guard)
        {
            if (EqualsNoCase(NameAt(field + static_cast<std::uint64_t>(m_layout.FieldName)).value_or(""), wanted))
                return field;
            field = ReadObject(field + static_cast<std::uint64_t>(m_layout.Next));
        }
        return 0;
    }

    std::uint64_t UnrealRuntime::FieldIn(std::uint64_t type, std::string_view wanted) const
    {
        for (int depth = 0; depth < kMaxSuperDepth && IsPtr(type); ++depth)
        {
            if (const std::uint64_t field = FindRawField(type, wanted))
                return field;
            type = SuperOf(type);
        }
        return 0;
    }

    void UnrealRuntime::ProbeExtra()
    {
        if (m_layout.Extra >= 0)
            return;
        const std::uint64_t scene = Type("SceneComponent");
        const std::uint64_t vector = Type("Vector");
        const std::uint64_t root = FieldIn(Type("Actor"), "RootComponent");
        const std::uint64_t location = FieldIn(scene, "RelativeLocation");
        if (!root || !location)
            return;
        for (int offset = (m_layout.Offset + 4 + 7) & ~7; offset <= 0x100; offset += 8)
        {
            if (ReadPointer(root + static_cast<std::uint64_t>(offset)) == scene && ReadPointer(location + static_cast<std::uint64_t>(offset)) == vector)
            {
                m_layout.Extra = offset;
                return;
            }
        }
    }

    bool UnrealRuntime::IsChildOf(std::uint64_t type, std::uint64_t base) const
    {
        for (int depth = 0; depth < kMaxSuperDepth; ++depth)
        {
            if (!IsPtr(type))
                return false;
            if (type == base)
                return true;
            type = SuperOf(type);
        }
        return false;
    }

    bool UnrealRuntime::IsFunction(std::uint64_t object) const
    {
        return IsPtr(object) && KindOf(ClassOf(object)) == UnrealKind::Function;
    }

    void UnrealRuntime::ProbeFunctions()
    {
        std::vector<std::uint64_t> samples;
        for (const char* name : { "Actor", "Pawn", "PlayerController", "GameplayStatics", "KismetSystemLibrary" })
        {
            if (const std::uint64_t type = Type(name))
                samples.push_back(type);
        }
        const auto countFunctions = [&](std::uint64_t owner, std::uint64_t head, int next)
            {
                int functions = 0;
                int guard = 0;
                for (std::uint64_t field = head; field && guard < kMaxFields; ++guard)
                {
                    if (OuterOf(field) != owner)
                        return -1;
                    if (IsFunction(field))
                        ++functions;
                    else if (m_layout.FField)
                        return -1;
                    field = ReadObject(field + static_cast<std::uint64_t>(next));
                }
                return guard < kMaxFields ? functions : -1;
            };
        if (m_layout.Children < 0)
        {
            if (!m_layout.FField)
            {
                m_layout.Children = m_layout.ChildProperties;
                m_layout.ChildNext = m_layout.Next;
            }
            else
            {
                for (int children = 0x28; children <= 0x100 && m_layout.Children < 0; children += 8)
                {
                    if (children == m_layout.ChildProperties || children == m_layout.Super)
                        continue;
                    for (int next = 0x28; next <= 0x40; next += 8)
                    {
                        int chains = 0;
                        bool foreign = false;
                        for (const std::uint64_t type : samples)
                        {
                            const int n = countFunctions(type, ReadObject(type + static_cast<std::uint64_t>(children)), next);
                            foreign = foreign || n < 0;
                            if (n >= 2)
                                ++chains;
                        }
                        if (!foreign && chains >= 2)
                        {
                            m_layout.Children = children;
                            m_layout.ChildNext = next;
                            break;
                        }
                    }
                }
            }
        }
        if (m_layout.Children < 0 || m_layout.FunctionFlags >= 0)
            return;
        const auto findFunction = [&](const char* owner, const char* name)
            {
                const std::uint64_t type = Type(owner);
                std::uint64_t field = type ? ReadObject(type + static_cast<std::uint64_t>(m_layout.Children)) : 0;
                for (int guard = 0; field && guard < kMaxFields; ++guard)
                {
                    if (EqualsNoCase(NameOf(field).value_or(""), name) && IsFunction(field))
                        return field;
                    field = ReadObject(field + static_cast<std::uint64_t>(m_layout.ChildNext));
                }
                return std::uint64_t{ 0 };
            };
        struct Expectation
        {
            std::uint64_t Function;
            std::uint32_t Set;
            std::uint32_t Clear;
        };
        std::vector<Expectation> expectations;
        const auto expect = [&](const char* owner, const char* name, std::uint32_t set, std::uint32_t clear)
            {
                const std::uint64_t function = findFunction(owner, name);
                if (function)
                    expectations.push_back({ function, set, clear });
                return function != 0;
            };
        if (!expect("PlayerController", "EnableCheats", kFuncExec | kFuncNative, kFuncStatic))
            expect("PlayerController", "FOV", kFuncExec | kFuncNative, kFuncStatic);
        expect("KismetSystemLibrary", "ExecuteConsoleCommand", kFuncStatic | kFuncNative, kFuncExec);
        expect("GameplayStatics", "SpawnObject", kFuncStatic | kFuncNative, kFuncExec);
        expect("Actor", "ReceiveBeginPlay", kFuncEvent | kFuncBlueprintEvent, kFuncNative);
        if (expectations.size() < 3)
            return;
        for (int offset = m_layout.StructSize >= 0 ? m_layout.StructSize + 4 : 0x40; offset <= 0x200; offset += 4)
        {
            bool all = true;
            for (const Expectation& e : expectations)
            {
                const auto flags = ReadUInt(e.Function + static_cast<std::uint64_t>(offset));
                all = all && flags && (*flags & e.Set) == e.Set && (*flags & e.Clear) == 0;
            }
            if (all)
            {
                m_layout.FunctionFlags = offset;
                return;
            }
        }
    }

    std::optional<int> UnrealRuntime::Override(const char* key) const
    {
        if (const auto it = m_overrides.find(key); it != m_overrides.end())
            return it->second;
        return std::nullopt;
    }

    void UnrealRuntime::ApplyOverrides()
    {
        const std::pair<const char*, int*> fields[] = { { "index", &m_layout.Index }, { "class", &m_layout.Class }, { "name", &m_layout.Name },
            { "outer", &m_layout.Outer }, { "super", &m_layout.Super }, { "childProperties", &m_layout.ChildProperties }, { "structSize", &m_layout.StructSize },
            { "children", &m_layout.Children }, { "childNext", &m_layout.ChildNext }, { "functionFlags", &m_layout.FunctionFlags },
            { "fieldClass", &m_layout.FieldClass }, { "fieldName", &m_layout.FieldName }, { "next", &m_layout.Next }, { "offset", &m_layout.Offset },
            { "size", &m_layout.Size }, { "extra", &m_layout.Extra }, { "itemSize", &m_layout.ItemSize }, { "itemOffset", &m_layout.ItemOffset } };
        for (const auto& [key, target] : fields)
        {
            if (const auto value = Override(key))
                *target = *value;
        }
        if (const auto value = Override("fField"))
            m_layout.FField = *value != 0;
    }
}
