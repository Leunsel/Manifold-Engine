#include "Target/Unreal.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"

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
        constexpr std::uint64_t kOwnerWindow = 0x10000;
        constexpr std::uint64_t kOwnerBlock = 0x1000;

        bool IsPtr(std::uint64_t value)
        {
            return value >= kPointerMin && value <= kPointerMax && (value & 7) == 0;
        }

        bool EndsWith(std::string_view text, std::string_view tail)
        {
            return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
        }

        template <class T>
        std::optional<T> From(const std::vector<std::uint8_t>* block, int offset)
        {
            if (!block || offset < 0 || static_cast<std::size_t>(offset) + sizeof(T) > block->size())
                return std::nullopt;
            T value{};
            std::memcpy(&value, block->data() + offset, sizeof(T));
            return value;
        }

        const std::unordered_set<std::string_view> kInnerAtExtra = { "ObjectProperty", "ObjectPtrProperty", "ClassProperty", "ClassPtrProperty", "WeakObjectProperty",
            "LazyObjectProperty", "SoftObjectProperty", "SoftClassProperty", "StructProperty", "ByteProperty", "InterfaceProperty", "DelegateProperty",
            "MulticastDelegateProperty", "MulticastInlineDelegateProperty", "MulticastSparseDelegateProperty" };
        const std::unordered_map<std::string_view, std::string_view> kScalarText = { { "FloatProperty", "float" }, { "DoubleProperty", "double" },
            { "IntProperty", "int32" }, { "Int8Property", "int8" }, { "Int16Property", "int16" }, { "Int64Property", "int64" }, { "UInt16Property", "uint16" },
            { "UInt32Property", "uint32" }, { "UInt64Property", "uint64" }, { "NameProperty", "FName" }, { "StrProperty", "FString" }, { "TextProperty", "FText" },
            { "BoolProperty", "bool" }, { "FieldPathProperty", "TFieldPath" }, { "Utf8StrProperty", "FUtf8String" }, { "AnsiStrProperty", "FAnsiString" } };

        class Stages
        {
        public:
            Stages() : m_started(std::chrono::steady_clock::now()), m_mark(m_started) {}

            void Mark(const char* name)
            {
                const auto now = std::chrono::steady_clock::now();
                if (!m_text.empty())
                    m_text += ", ";
                m_text += Format("%s %.2f s", name, std::chrono::duration<double>(now - m_mark).count());
                m_mark = now;
            }

            double Seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count(); }
            const std::string& Text() const { return m_text; }

        private:
            std::chrono::steady_clock::time_point m_started;
            std::chrono::steady_clock::time_point m_mark;
            std::string m_text;
        };

        const std::vector<std::string> kControllerPath = { "GameViewport", "World", "OwningGameInstance", "LocalPlayers[0]", "PlayerController" };
        constexpr std::int64_t kMaxWalkIndex = 0x00FFFFFF;

        void SortFields(std::vector<UnrealField>& list)
        {
            std::stable_sort(list.begin(), list.end(), [](const UnrealField& a, const UnrealField& b)
                {
                    if (a.Start() != b.Start())
                        return a.Start() < b.Start();
                    return a.Mask < b.Mask;
                });
        }

        std::pair<std::string, std::optional<int>> SplitIndex(const std::string& step)
        {
            const std::size_t open = step.rfind('[');
            if (open == std::string::npos || step.back() != ']' || open + 2 > step.size() - 1)
                return { step, std::nullopt };
            const std::string digits = step.substr(open + 1, step.size() - open - 2);
            if (digits.empty() || digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return { step, std::nullopt };
            const auto index = ParseDecimal(digits);
            if (!index || *index < 0 || *index > kMaxWalkIndex)
                return { step, std::nullopt };
            return { step.substr(0, open), static_cast<int>(*index) };
        }

        struct ProgressScope
        {
            ProgressScope(WorkProgress*& slot, WorkProgress* progress) : Slot(slot) { Slot = progress; }
            ~ProgressScope() { Slot = nullptr; }
            ProgressScope(const ProgressScope&) = delete;
            ProgressScope& operator=(const ProgressScope&) = delete;
            WorkProgress*& Slot;
        };
    }

    Status UnrealRuntime::Resolve(std::shared_ptr<const Process> process, const UnrealConfig& config, bool full, WorkProgress* progress)
    {
        const auto fail = [](std::string message) { return Status::Fail(ErrorCode::UnrealNotResolved, message); };
        const auto cancelled = [&]() { return progress && progress->Cancelled(); };
        const auto step = [&](const char* text)
            {
                if (progress)
                    progress->SetText(text);
            };
        Stages stages;
        const ProgressScope scope(m_progress, progress);
        m_process = std::move(process);
        m_full = full;
        m_overrides = config.Layout;
        m_layout = UnrealLayout{};
        if (!m_process)
            return Status::Fail(ErrorCode::NoProcess);
        m_images.clear();
        if (const auto modules = m_process->Modules())
        {
            for (const ModuleInfo& module : *modules)
                m_images.push_back({ module.Base, module.Base + module.Size });
        }
        std::sort(m_images.begin(), m_images.end());
        const auto main = m_process->MainModule();
        if (!main)
            return fail("the process has no main module");
        m_main = *main;
        m_rejected = false;
        if (!full && !LooksUnreal(*m_process))
        {
            m_rejected = true;
            return fail("not an Unreal Engine process (no CoreUObject in " + m_main.Name + ")");
        }
        if (!m_process->Is64Bit())
            return fail("only 64-bit games are supported");

        step("Finding the name table");
        m_pool = 0;
        m_legacyNames = config.LegacyNames;
        if (config.NamePool)
        {
            if (!CheckNamePool(config.NamePool, config.LegacyNames))
                return fail("the configured name table at " + Hex(config.NamePool) + (config.LegacyNames ? " is no TNameEntryArray" : " is no FNamePool"));
            m_pool = config.NamePool;
        }
        for (const UnrealPattern& pattern : config.NamePatterns)
        {
            if (m_pool)
                break;
            const std::uint64_t target = PatternTarget(pattern);
            if (target && CheckNamePool(target, config.LegacyNames))
                m_pool = target;
        }
        if (!m_pool)
        {
            m_legacyNames = false;
            m_pool = FindNamePool();
        }
        if (!m_pool && !cancelled())
        {
            m_pool = FindNameEntryArray();
            m_legacyNames = m_pool != 0;
        }
        if (cancelled())
            return fail("cancelled");
        if (!m_pool)
            return fail("name table not found: neither FNamePool (UE 4.23+) nor TNameEntryArray (UE 4.22 and older)");
        stages.Mark("names");

        step("Finding the object array");
        std::optional<LiveArray> live;
        std::string source;
        if (config.Objects)
        {
            live = CheckObjects(config.Objects);
            if (!live)
                return fail("the configured object array at " + Hex(config.Objects) + " is no GUObjectArray");
            source = "configured";
        }
        for (const UnrealPattern& pattern : config.ObjectPatterns)
        {
            if (live)
                break;
            if (const std::uint64_t target = PatternTarget(pattern))
            {
                live = CheckObjects(target);
                source = "pattern";
            }
        }
        if (!live)
        {
            if (auto found = FindObjectArray())
            {
                live = std::move(found->first);
                source = found->second;
            }
        }
        if (cancelled())
            return fail("cancelled");
        if (!live)
            return fail("object array (GObjects) not found");
        m_array = live->Array;
        m_layout = live->Layout;
        ApplyOverrides();
        if (!ResolveClassAndOuter(*live))
            return fail("UObject class and outer offsets not found");
        stages.Mark("objects");

        step("Reading the types");
        IndexTypes();
        stages.Mark("types");
        if (cancelled())
            return fail("cancelled");
        for (const char* required : { "Object", "Field", "Struct", "Class" })
        {
            if (!Type(required))
                return fail(std::string("core class '") + required + "' not found among " + std::to_string(m_typeList.size()) + " types");
        }
        step("Reading the reflection layout");
        if (!ProbeSuper())
            return fail("UStruct::SuperStruct offset not found");
        if (!ProbeChain())
        {
            const bool given = Override("childProperties") && Override("fieldName") && Override("next") && Override("offset") && Override("size");
            if (!given)
                return fail("property chain of ScriptStruct 'Vector' not found");
            ApplyOverrides();
        }
        ProbeStructSize();
        ProbeExtra();
        ProbeFunctions();
        ApplyOverrides();
        const int typeSlot = m_layout.FField ? m_layout.FieldClass : m_layout.Class;
        m_fieldBlock = std::max({ m_layout.FieldName + 8, m_layout.Next + 8, m_layout.Offset + 4, m_layout.Size + 12, typeSlot + 8 });
        if (m_layout.Extra >= 0)
            m_fieldBlock = std::max(m_fieldBlock, m_layout.Extra + 24);
        stages.Mark("layout");

        step("Finding the engine");
        m_engine = FindEngine();
        stages.Mark("engine");

        m_summary = UnrealSummary{};
        m_summary.NamePool = m_pool;
        m_summary.LegacyNames = m_legacyNames;
        m_summary.Objects = m_array;
        m_summary.ObjectsSource = source;
        m_summary.ObjectCount = Count();
        m_summary.TypeCount = m_typeList.size();
        m_summary.Seconds = stages.Seconds();
        m_summary.Timings = stages.Text();
        m_summary.Layout = m_layout;
        const UnrealLayout& l = m_layout;
        LOG_INFO("Unreal", Format("%s at %s (%s) | GObjects at %s (%s) | %d objects, %zu types | %.1f s (%s)", m_legacyNames ? "GNames" : "FNamePool",
            Hex(m_pool).c_str(), m_legacyNames ? "pre-4.23" : "4.23+", Hex(m_array).c_str(), source.c_str(), m_summary.ObjectCount, m_summary.TypeCount,
            m_summary.Seconds, m_summary.Timings.c_str()));
        LOG_INFO("Unreal", Format("UObject index %X class %X name %X outer %X | Super %X Properties %X Name %X Next %X Offset %X Size %X Extra %s | %s fields, %s vectors",
            l.Index, l.Class, l.Name, l.Outer, l.Super, l.ChildProperties, l.FieldName, l.Next, l.Offset, l.Size, l.Extra >= 0 ? Hex(static_cast<std::uint64_t>(l.Extra)).c_str() : "-",
            l.FField ? "FField" : "UProperty", l.Component == 8 ? "double" : "float"));
        return Status::Success();
    }

    std::optional<std::string> UnrealRuntime::TypeOf(std::uint64_t property, const std::vector<std::uint8_t>* block) const
    {
        const int slot = m_layout.FField ? m_layout.FieldClass : m_layout.Class;
        if (slot < 0)
            return std::nullopt;
        const auto value = block ? From<std::uint64_t>(block, slot) : ReadPointer(property + static_cast<std::uint64_t>(slot));
        if (!value || !*value)
            return std::nullopt;
        const std::uint64_t typeClass = *value;
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_fieldTypes.find(typeClass); it != m_fieldTypes.end())
                return it->second;
        }
        const auto name = m_layout.FField ? NameAt(typeClass) : NameOf(typeClass);
        if (name)
        {
            std::lock_guard lock(m_lock);
            m_fieldTypes[typeClass] = *name;
        }
        return name;
    }

    std::optional<std::pair<std::uint64_t, std::string>> UnrealRuntime::InnerProperty(std::uint64_t property, int slot, const std::vector<std::uint8_t>* block) const
    {
        const auto value = block ? From<std::uint64_t>(block, slot) : ReadPointer(property + static_cast<std::uint64_t>(slot));
        if (!value || !IsPtr(*value))
            return std::nullopt;
        const auto kind = TypeOf(*value, nullptr);
        if (!kind || !EndsWith(*kind, "Property"))
            return std::nullopt;
        return std::make_pair(*value, *kind);
    }

    std::optional<UnrealField> UnrealRuntime::Describe(std::uint64_t property, const std::string& kind, const std::string& owner) const
    {
        std::vector<std::uint8_t> block;
        const bool read = m_fieldBlock > 0 && ReadBlock(property, block, static_cast<std::size_t>(m_fieldBlock));
        const std::string type = kind.empty() ? TypeOf(property, read ? &block : nullptr).value_or("") : kind;
        return DescribeWith(property, type, owner, read ? &block : nullptr);
    }

    std::optional<UnrealField> UnrealRuntime::DescribeWith(std::uint64_t property, const std::string& kind, const std::string& owner, const std::vector<std::uint8_t>* block) const
    {
        if (!property || m_layout.FieldName < 0)
            return std::nullopt;
        const auto readInt = [&](int offset) -> std::optional<std::int32_t>
            {
                if (block)
                    return From<std::int32_t>(block, offset);
                return ReadInt(property + static_cast<std::uint64_t>(offset));
            };
        const auto readObject = [&](int offset) -> std::uint64_t
            {
                const auto value = block ? From<std::uint64_t>(block, offset) : ReadPointer(property + static_cast<std::uint64_t>(offset));
                return value && IsPtr(*value) ? *value : 0;
            };
        UnrealField info;
        info.Address = property;
        info.Type = kind;
        info.Owner = owner;
        if (block)
        {
            const auto index = From<std::uint32_t>(block, m_layout.FieldName);
            const auto number = From<std::uint32_t>(block, m_layout.FieldName + 4);
            info.Name = index && number ? NameFromParts(*index, *number).value_or("?") : "?";
        }
        else
        {
            info.Name = NameAt(property + static_cast<std::uint64_t>(m_layout.FieldName)).value_or("?");
        }
        info.Offset = readInt(m_layout.Offset).value_or(0);
        info.Size = readInt(m_layout.Size).value_or(0);
        info.Dim = readInt(m_layout.Size - 4).value_or(1);
        if (block)
            info.Flags = From<std::uint64_t>(block, m_layout.Size + 4).value_or(0);
        else
            info.Flags = ReadQword(property + static_cast<std::uint64_t>(m_layout.Size + 4)).value_or(0);
        if (info.Dim < 1 || info.Dim > 0x10000)
            info.Dim = 1;
        const int extra = m_layout.Extra;
        if (extra < 0)
            return info;
        if (kind == "BoolProperty")
        {
            std::uint8_t bits[4] = {};
            bool got = false;
            if (block && static_cast<std::size_t>(extra) + 4 <= block->size())
            {
                std::memcpy(bits, block->data() + extra, 4);
                got = true;
            }
            else
            {
                got = m_process->ReadExact(property + static_cast<std::uint64_t>(extra), bits, 4);
            }
            const int fieldSize = bits[0];
            if (got && (fieldSize == 1 || fieldSize == 2 || fieldSize == 4 || fieldSize == 8) && bits[1] < fieldSize && bits[2] != 0)
            {
                info.Bitfield = true;
                info.ByteOffset = bits[1];
                info.Mask = bits[2];
                info.FieldMask = bits[3];
            }
        }
        else if (kind == "EnumProperty")
        {
            info.Inner = readObject(extra + 8);
            if (const auto underlying = InnerProperty(property, extra, block))
            {
                info.InnerProperty = underlying->first;
                info.InnerType = underlying->second;
            }
        }
        else if (kind == "ArrayProperty" || kind == "SetProperty")
        {
            auto inner = InnerProperty(property, extra, block);
            if (!inner)
                inner = InnerProperty(property, extra + 8, block);
            if (inner)
            {
                info.InnerProperty = inner->first;
                info.InnerType = inner->second;
            }
        }
        else if (kind == "MapProperty")
        {
            int slot = extra;
            auto inner = InnerProperty(property, slot, block);
            if (!inner)
            {
                slot = extra + 8;
                inner = InnerProperty(property, slot, block);
            }
            if (inner)
            {
                info.InnerProperty = inner->first;
                info.InnerType = inner->second;
                if (const auto value = InnerProperty(property, slot + 8, block))
                {
                    info.ValueProperty = value->first;
                    info.ValueType = value->second;
                }
            }
        }
        else if (kInnerAtExtra.count(kind))
        {
            info.Inner = readObject(extra);
            if (kind == "ClassProperty" || kind == "ClassPtrProperty" || kind == "SoftClassProperty")
                info.MetaClass = readObject(extra + 8);
        }
        return info;
    }

    std::vector<UnrealField> UnrealRuntime::OwnFields(std::uint64_t type) const
    {
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_own.find(type); it != m_own.end())
                return it->second;
        }
        std::vector<UnrealField> out;
        if (!IsPtr(type) || m_layout.ChildProperties < 0)
            return out;
        const std::string owner = NameOf(type).value_or("?");
        std::uint64_t field = ReadObject(type + static_cast<std::uint64_t>(m_layout.ChildProperties));
        std::vector<std::uint8_t> block;
        for (int guard = 0; field && guard < kMaxFields; ++guard)
        {
            const bool read = m_fieldBlock > 0 && ReadBlock(field, block, static_cast<std::size_t>(m_fieldBlock));
            const std::vector<std::uint8_t>* data = read ? &block : nullptr;
            const auto kind = TypeOf(field, data);
            if (kind && EndsWith(*kind, "Property"))
            {
                if (auto info = DescribeWith(field, *kind, owner, data))
                    out.push_back(std::move(*info));
            }
            const auto next = data ? From<std::uint64_t>(data, m_layout.Next) : ReadPointer(field + static_cast<std::uint64_t>(m_layout.Next));
            field = next && IsPtr(*next) ? *next : 0;
        }
        std::lock_guard lock(m_lock);
        m_own[type] = out;
        return out;
    }

    std::vector<UnrealField> UnrealRuntime::Fields(std::uint64_t type, bool ownOnly) const
    {
        if (!IsPtr(type))
            return {};
        if (ownOnly)
        {
            std::vector<UnrealField> own = OwnFields(type);
            SortFields(own);
            return own;
        }
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_merged.find(type); it != m_merged.end())
                return it->second;
        }
        std::vector<UnrealField> out;
        std::uint64_t current = type;
        for (int depth = 0; depth < kMaxSuperDepth && IsPtr(current); ++depth)
        {
            for (UnrealField& field : OwnFields(current))
                out.push_back(std::move(field));
            current = SuperOf(current);
        }
        SortFields(out);
        std::lock_guard lock(m_lock);
        m_merged[type] = out;
        return out;
    }

    std::optional<UnrealField> UnrealRuntime::FindField(std::uint64_t type, std::string_view name) const
    {
        for (UnrealField& field : Fields(type))
        {
            if (EqualsNoCase(field.Name, name))
                return std::move(field);
        }
        return std::nullopt;
    }

    std::string UnrealRuntime::CppName(std::uint64_t type) const
    {
        if (!IsPtr(type))
            return "?";
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_cpp.find(type); it != m_cpp.end())
                return it->second;
        }
        const std::string name = NameOf(type).value_or("?");
        std::string text;
        switch (KindOf(ClassOf(type)))
        {
        case UnrealKind::Struct: text = "F" + name; break;
        case UnrealKind::Enum: text = name; break;
        case UnrealKind::Class: text = (IsChildOf(type, Type("Actor")) ? "A" : "U") + name; break;
        default: text = name; break;
        }
        std::lock_guard lock(m_lock);
        m_cpp[type] = text;
        return text;
    }

    std::string UnrealRuntime::PropertyText(std::uint64_t property, const std::string& kind) const
    {
        if (!property)
            return "?";
        const auto info = Describe(property, kind, {});
        return info ? TypeText(*info) : "?";
    }

    std::string UnrealRuntime::TypeText(const UnrealField& f) const
    {
        const std::string& kind = f.Type;
        if (const auto it = kScalarText.find(kind); it != kScalarText.end())
            return std::string(it->second);
        const auto inner = [&](const char* fallback) { return f.Inner ? CppName(f.Inner) : std::string(fallback); };
        if (kind == "ByteProperty")
            return inner("uint8");
        if (kind == "EnumProperty")
            return inner("enum");
        if (IsObjectProperty(kind))
        {
            if ((kind == "ClassProperty" || kind == "ClassPtrProperty") && f.MetaClass)
                return "TSubclassOf<" + CppName(f.MetaClass) + ">";
            return inner("UObject") + "*";
        }
        if (kind == "WeakObjectProperty")
            return "TWeakObjectPtr<" + inner("UObject") + ">";
        if (kind == "LazyObjectProperty")
            return "TLazyObjectPtr<" + inner("UObject") + ">";
        if (kind == "SoftObjectProperty")
            return "TSoftObjectPtr<" + inner("UObject") + ">";
        if (kind == "SoftClassProperty")
            return "TSoftClassPtr<" + (f.MetaClass ? CppName(f.MetaClass) : std::string("UObject")) + ">";
        if (kind == "InterfaceProperty")
            return "TScriptInterface<" + inner("IInterface") + ">";
        if (kind == "StructProperty")
            return inner("struct");
        if (kind == "ArrayProperty")
            return "TArray<" + PropertyText(f.InnerProperty, f.InnerType) + ">";
        if (kind == "SetProperty")
            return "TSet<" + PropertyText(f.InnerProperty, f.InnerType) + ">";
        if (kind == "MapProperty")
            return "TMap<" + PropertyText(f.InnerProperty, f.InnerType) + ", " + PropertyText(f.ValueProperty, f.ValueType) + ">";
        if (kind == "DelegateProperty")
            return "FScriptDelegate";
        if (kind.find("MulticastDelegate") != std::string::npos)
            return "FMulticastDelegate";
        if (EndsWith(kind, "Property"))
            return kind.substr(0, kind.size() - 8);
        return kind.empty() ? "?" : kind;
    }

    std::vector<UnrealEnumEntry> UnrealRuntime::EnumEntries(std::uint64_t enumObject) const
    {
        if (!IsPtr(enumObject))
            return {};
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_enumEntries.find(enumObject); it != m_enumEntries.end())
                return it->second;
        }
        std::vector<UnrealEnumEntry> entries;
        for (std::uint64_t offset = 0x28; offset <= 0x80; offset += 8)
        {
            const std::uint64_t data = ReadObject(enumObject + offset);
            const auto count = ReadInt(enumObject + offset + 8);
            const auto max = ReadInt(enumObject + offset + 0xC);
            if (!data || !count || !max || *count <= 0 || *count > 4096 || *max < *count)
                continue;
            const std::string last = Upper(NameAt(data + static_cast<std::uint64_t>(*count - 1) * 16).value_or(""));
            if (!EndsWith(last, "MAX"))
                continue;
            for (int i = 0; i < *count; ++i)
            {
                const std::uint64_t entry = data + static_cast<std::uint64_t>(i) * 16;
                entries.push_back({ NameAt(entry).value_or("?"), static_cast<std::int64_t>(ReadQword(entry + 8).value_or(0)) });
            }
            break;
        }
        std::lock_guard lock(m_lock);
        m_enumEntries[enumObject] = entries;
        return entries;
    }

    std::vector<UnrealFunction> UnrealRuntime::Functions(std::uint64_t type, bool inherited) const
    {
        std::vector<UnrealFunction> out;
        if (m_layout.Children < 0)
            return out;
        std::uint64_t current = type;
        for (int depth = 0; depth < kMaxSuperDepth && IsPtr(current); ++depth)
        {
            const std::string owner = NameOf(current).value_or("?");
            std::uint64_t field = ReadObject(current + static_cast<std::uint64_t>(m_layout.Children));
            for (int guard = 0; field && guard < kMaxFields; ++guard)
            {
                if (IsFunction(field))
                {
                    UnrealFunction function;
                    function.Name = NameOf(field).value_or("?");
                    function.Owner = owner;
                    function.Address = field;
                    function.Flags = m_layout.FunctionFlags >= 0 ? ReadUInt(field + static_cast<std::uint64_t>(m_layout.FunctionFlags)).value_or(0) : 0;
                    function.Parameters = OwnFields(field);
                    out.push_back(std::move(function));
                }
                field = ReadObject(field + static_cast<std::uint64_t>(m_layout.ChildNext));
            }
            if (!inherited)
                break;
            current = SuperOf(current);
        }
        return out;
    }

    std::string UnrealRuntime::FullName(std::uint64_t object) const
    {
        std::vector<std::string> parts;
        std::uint64_t current = object;
        for (int depth = 0; depth < 32 && IsPtr(current); ++depth)
        {
            parts.insert(parts.begin(), NameOf(current).value_or("?"));
            current = OuterOf(current);
        }
        return Join(parts, ".");
    }

    std::vector<std::uint64_t> UnrealRuntime::Instances(std::uint64_t type, std::size_t limit) const
    {
        std::vector<std::uint64_t> out;
        if (!IsPtr(type))
            return out;
        std::unordered_map<std::uint64_t, bool> seen;
        Each([&](std::uint64_t object, int)
            {
                const std::uint64_t meta = ClassOf(object);
                if (!meta)
                    return false;
                auto it = seen.find(meta);
                if (it == seen.end())
                    it = seen.emplace(meta, IsChildOf(meta, type)).first;
                if (!it->second)
                    return false;
                if (StartsWithNoCase(NameOf(object).value_or(""), "Default__"))
                    return false;
                out.push_back(object);
                return limit && out.size() >= limit;
            });
        return out;
    }

    std::optional<std::uint64_t> UnrealRuntime::Find(std::string_view nameOrPath) const
    {
        std::optional<std::uint64_t> found;
        const bool path = nameOrPath.find('.') != std::string_view::npos;
        Each([&](std::uint64_t object, int)
            {
                if (NameOf(object) == nameOrPath || (path && FullName(object) == nameOrPath))
                {
                    found = object;
                    return true;
                }
                return false;
            });
        return found;
    }

    std::optional<std::pair<std::uint64_t, std::uint64_t>> UnrealRuntime::OwnerOf(std::uint64_t address) const
    {
        if (m_layout.Index < 0)
            return std::nullopt;
        const std::uint64_t start = address - address % 8;
        if (!IsPtr(start))
            return std::nullopt;
        const std::uint64_t stop = std::max(start > kOwnerWindow ? start - kOwnerWindow : 0, kPointerMin);
        const int count = Count();
        std::unordered_map<int, std::uint64_t> slotObjects;
        std::vector<std::uint8_t> slots;
        const auto check = [&](std::uint64_t candidate) -> std::optional<std::pair<std::uint64_t, std::uint64_t>>
            {
                const std::uint64_t klass = ClassOf(candidate);
                const auto size = StructSize(klass);
                if (size && address < candidate + static_cast<std::uint64_t>(*size))
                    return std::make_pair(candidate, klass);
                return std::nullopt;
            };
        for (std::uint64_t top = start; top >= stop;)
        {
            const std::uint64_t bottom = std::max(top >= kOwnerBlock - 8 ? top - kOwnerBlock + 8 : 0, stop);
            const bool read = ReadBlock(bottom + static_cast<std::uint64_t>(m_layout.Index), slots, static_cast<std::size_t>(top - bottom + 4));
            for (std::uint64_t candidate = top;; candidate -= 8)
            {
                std::optional<std::int32_t> index;
                if (read)
                {
                    std::int32_t value = 0;
                    std::memcpy(&value, slots.data() + (candidate - bottom), 4);
                    index = value;
                }
                else
                {
                    index = ReadInt(candidate + static_cast<std::uint64_t>(m_layout.Index));
                }
                if (index && *index >= 0 && *index < count)
                {
                    auto it = slotObjects.find(*index);
                    if (it == slotObjects.end())
                        it = slotObjects.emplace(*index, ObjectAt(*index).value_or(0)).first;
                    if (it->second == candidate)
                        return check(candidate);
                }
                if (candidate <= bottom || candidate < 8)
                    break;
            }
            if (bottom < 8 + stop)
                break;
            top = bottom - 8;
        }
        std::uint64_t best = 0;
        Each([&](std::uint64_t object, int)
            {
                if (object <= address && object > best)
                    best = object;
                return false;
            });
        if (best)
            return check(best);
        return std::nullopt;
    }

    std::optional<std::string> UnrealRuntime::HeaderField(int offset, int& inside) const
    {
        struct Slot
        {
            int At;
            int Width;
            const char* Name;
        };
        std::vector<Slot> slots = { { 0, 8, "VTable" }, { m_layout.Index, 4, "InternalIndex" }, { m_layout.Class, 8, "ClassPrivate" }, { m_layout.Name, 8, "NamePrivate" },
            { m_layout.Outer, 8, "OuterPrivate" } };
        if (m_layout.Index == 0x0C)
            slots.push_back({ 0x08, 4, "ObjectFlags" });
        for (const Slot& slot : slots)
        {
            if (slot.At >= 0 && offset >= slot.At && offset < slot.At + slot.Width)
            {
                inside = offset - slot.At;
                return std::string(slot.Name);
            }
        }
        return std::nullopt;
    }

    bool UnrealRuntime::Descend(std::uint64_t type, int offset, std::vector<std::string>& path, int depth, UnrealLocation& found) const
    {
        for (const UnrealField& f : Fields(type))
        {
            const int elementSize = std::max(1, f.Size);
            const int total = elementSize * std::max(1, f.Dim);
            const int start = f.Start();
            const int width = f.Bitfield ? 1 : total;
            if (offset < start || offset >= start + width)
                continue;
            const int index = f.Bitfield ? 0 : (offset - f.Offset) / elementSize;
            const int within = f.Bitfield ? 0 : offset - f.Offset - index * elementSize;
            std::string segment = f.Name + (f.Dim > 1 ? "[" + std::to_string(index) + "]" : "");
            if (f.MaskedBit())
                segment += Format(" (bit mask %02X)", f.Mask);
            path.push_back(segment);
            if (f.Type == "StructProperty" && f.Inner && depth < 12)
            {
                if (within == 0 && !found.Struct)
                    found.Struct = f.Inner;
                return Descend(f.Inner, within, path, depth + 1, found);
            }
            found.Field = f;
            found.Within = within;
            return true;
        }
        path.push_back(Format("+%X (not reflected)", offset));
        found.Within = offset;
        return false;
    }

    std::optional<UnrealLocation> UnrealRuntime::Locate(std::uint64_t address) const
    {
        const auto owner = OwnerOf(address);
        if (!owner)
            return std::nullopt;
        UnrealLocation hit;
        hit.Object = owner->first;
        hit.Class = owner->second;
        int inside = 0;
        const int offset = static_cast<int>(address - hit.Object);
        if (const auto header = HeaderField(offset, inside))
            hit.Path.push_back(*header + (inside > 0 ? Format(" +%X", inside) : std::string()));
        else if (address != hit.Object)
            Descend(hit.Class, offset, hit.Path, 0, hit);
        return hit;
    }

    std::optional<std::string> UnrealRuntime::Whois(std::uint64_t address) const
    {
        const auto hit = Locate(address);
        if (!hit)
            return std::nullopt;
        std::string text = Format("%s (%s) +%X", NameOf(hit->Object).value_or("?").c_str(), CppName(hit->Class).c_str(), static_cast<unsigned>(address - hit->Object));
        if (!hit->Path.empty())
            text += ": " + Join(hit->Path, ".");
        if (hit->Field)
            text += "  " + TypeText(*hit->Field) + (hit->Within > 0 ? Format(" +%X", hit->Within) : std::string());
        return text;
    }

    std::uint64_t UnrealRuntime::FindEngine() const
    {
        for (const char* name : { "GameEngine", "Engine" })
        {
            const auto found = Instances(Type(name), 1);
            if (!found.empty())
                return found.front();
        }
        return 0;
    }

    std::uint64_t UnrealRuntime::Engine() const
    {
        const std::uint64_t known = m_engine.load();
        if (known && IsObject(known))
            return known;
        const std::uint64_t found = FindEngine();
        m_engine = found;
        return found;
    }

    std::uint64_t UnrealRuntime::Follow(std::uint64_t object, const std::vector<std::string>& steps) const
    {
        std::uint64_t current = object;
        for (const std::string& step : steps)
        {
            if (!IsPtr(current))
                return 0;
            const auto [name, index] = SplitIndex(step);
            const auto f = FindField(ClassOf(current), name);
            if (!f)
                return 0;
            std::uint64_t slot = current + static_cast<std::uint64_t>(f->Offset);
            if (f->Type == "ArrayProperty" && index)
            {
                const std::uint64_t data = ReadPointer(slot).value_or(0);
                slot = data ? data + static_cast<std::uint64_t>(*index) * 8 : 0;
            }
            current = slot ? ReadPointer(slot).value_or(0) : 0;
        }
        return IsPtr(current) ? current : 0;
    }

    std::uint64_t UnrealRuntime::World() const
    {
        return Follow(Engine(), { "GameViewport", "World" });
    }

    std::uint64_t UnrealRuntime::Controller() const
    {
        return Follow(Engine(), kControllerPath);
    }

    std::uint64_t UnrealRuntime::Pawn() const
    {
        const std::uint64_t controller = Controller();
        if (!controller)
            return 0;
        if (const std::uint64_t pawn = Follow(controller, { "AcknowledgedPawn" }))
            return pawn;
        return Follow(controller, { "Pawn" });
    }

    std::optional<bool> UnrealRuntime::IsOffline() const
    {
        const std::uint64_t world = World();
        const auto netDriver = world ? FindField(ClassOf(world), "NetDriver") : std::nullopt;
        if (!netDriver)
            return std::nullopt;
        const auto driver = ReadPointer(world + static_cast<std::uint64_t>(netDriver->Offset));
        if (!driver)
            return std::nullopt;
        if (*driver == 0)
            return true;
        const auto server = FindField(ClassOf(*driver), "ServerConnection");
        const auto clients = FindField(ClassOf(*driver), "ClientConnections");
        if (!server || !clients)
            return std::nullopt;
        return ReadPointer(*driver + static_cast<std::uint64_t>(server->Offset)) == 0 && ReadInt(*driver + static_cast<std::uint64_t>(clients->Offset) + 8).value_or(0) == 0;
    }

    std::string UnrealRuntime::OffsetSymbol(const std::string& owner, const std::string& field)
    {
        std::string text = owner + "_" + field;
        for (char& c : text)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                c = '_';
        }
        return text;
    }

    Result<UnrealWalk> UnrealRuntime::Walk(std::uint64_t object, const std::string& root, const std::vector<std::string>& steps) const
    {
        UnrealWalk walk;
        walk.Address = object;
        walk.Expression = root;
        std::uint64_t type = ClassOf(object);
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            const auto [name, index] = SplitIndex(steps[i]);
            const auto f = FindField(type, name);
            if (!f)
                return Status::Fail(ErrorCode::UnrealNoProperty, NameOf(type).value_or("?") + "." + steps[i]);
            walk.Field = f;
            walk.Address += static_cast<std::uint64_t>(f->Start());
            walk.Expression += "+" + OffsetSymbol(f->Owner, f->Name);
            bool isObject = IsObjectProperty(f->Type);
            bool isStruct = f->Type == "StructProperty";
            if (isStruct)
            {
                type = f->Inner;
            }
            else if (f->Type == "ArrayProperty" && index)
            {
                const auto data = ReadPointer(walk.Address);
                const auto count = ReadInt(walk.Address + 8);
                if (!data || !*data || !count)
                    return Status::Fail(ErrorCode::ReadFailed, Hex(walk.Address));
                if (*index >= *count)
                    return Status::Fail(ErrorCode::UnrealNoProperty, NameOf(type).value_or("?") + "." + steps[i] + " (the array has " + std::to_string(std::max(*count, 0)) + " elements)");
                const auto element = f->InnerProperty ? Describe(f->InnerProperty, f->InnerType, f->Owner) : std::nullopt;
                const std::uint64_t stride = element && element->Size > 0 ? static_cast<std::uint64_t>(element->Size) : 8;
                const std::uint64_t offset = static_cast<std::uint64_t>(*index) * stride;
                walk.Address = *data + offset;
                walk.Expression = Format("[%s]+%llX", walk.Expression.c_str(), static_cast<unsigned long long>(offset));
                isObject = !element || IsObjectProperty(element->Type);
                isStruct = element && element->Type == "StructProperty";
                if (isStruct)
                    type = element->Inner;
            }
            if (i + 1 < steps.size() && !isObject && !isStruct)
                return Status::Fail(ErrorCode::UnrealNoProperty, NameOf(type).value_or("?") + "." + steps[i] + " (a " + f->Type + " has no properties)");
            if (isObject && i + 1 < steps.size())
            {
                const auto next = ReadPointer(walk.Address);
                if (!next || !IsPtr(*next))
                    return Status::Fail(ErrorCode::ReadFailed, Hex(walk.Address));
                walk.Address = *next;
                walk.Expression = "[" + walk.Expression + "]";
                type = ClassOf(walk.Address);
            }
        }
        return walk;
    }

    std::optional<std::uint64_t> UnrealRuntime::ResolveSymbol(std::string_view name) const
    {
        if (name == "GObjects")
            return m_array;
        if (name == "FNamePool" || name == "GNames")
            return m_pool;
        if (name == "GameEngine")
        {
            const std::uint64_t engine = Engine();
            return engine ? std::optional<std::uint64_t>(engine) : std::nullopt;
        }
        if (name.size() > 256 || name.find('_', 1) == std::string_view::npos)
            return std::nullopt;
        const std::string key(name);
        {
            std::lock_guard lock(m_lock);
            if (const auto it = m_symbols.find(key); it != m_symbols.end())
                return it->second;
        }
        std::optional<std::uint64_t> value;
        for (std::size_t cut = name.find('_', 1); cut != std::string_view::npos; cut = name.find('_', cut + 1))
        {
            const std::uint64_t type = Type(name.substr(0, cut));
            if (!type)
                continue;
            if (const auto f = FindField(type, name.substr(cut + 1)))
            {
                value = static_cast<std::uint64_t>(f->Start());
                break;
            }
        }
        std::lock_guard lock(m_lock);
        m_symbols[key] = value;
        return value;
    }
}
