#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"
#include "Target/Process.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Manifold
{
    class WorkProgress;
}

namespace Manifold::Target
{
    inline constexpr const char* kUnrealFNameType = "UE FName";
    inline constexpr const char* kUnrealFStringType = "UE FString";
    inline constexpr const char* kUnrealObjectIndexType = "UE Object Index";

    struct UnrealPattern
    {
        std::string Bytes;
        int Offset = 0;
        int Length = 0;
        bool Relative = true;
        std::int64_t Add = 0;
        std::string Module;
    };

    struct UnrealConfig
    {
        bool AutoStart = true;
        bool Dissect = true;
        bool Names = true;
        bool Symbols = true;
        bool Types = true;
        std::uint64_t NamePool = 0;
        bool LegacyNames = false;
        std::uint64_t Objects = 0;
        std::map<std::string, int> Layout;
        std::vector<UnrealPattern> NamePatterns;
        std::vector<UnrealPattern> ObjectPatterns;
    };

    struct UnrealLayout
    {
        bool Chunked = true;
        int ObjectsAt = 0;
        int MaxAt = 0x10;
        int NumAt = 0x14;
        int MaxChunksAt = 0x18;
        int NumChunksAt = 0x1C;
        int ItemSize = 0x18;
        int ItemOffset = 0;
        int Index = -1;
        int Class = -1;
        int Name = -1;
        int Outer = -1;
        int Super = -1;
        int ChildProperties = -1;
        int StructSize = -1;
        int Children = -1;
        int ChildNext = -1;
        int FunctionFlags = -1;
        bool FField = true;
        int FieldClass = -1;
        int FieldName = -1;
        int Next = -1;
        int Offset = -1;
        int Size = -1;
        int Component = 4;
        int Extra = -1;
    };

    enum class UnrealKind
    {
        None,
        Class,
        Struct,
        Enum,
        Function
    };

    struct UnrealType
    {
        std::string Name;
        std::uint64_t Object = 0;
        UnrealKind Kind = UnrealKind::None;
    };

    struct UnrealField
    {
        std::uint64_t Address = 0;
        std::string Type;
        std::string Owner;
        std::string Name;
        int Offset = 0;
        int Size = 0;
        int Dim = 1;
        std::uint64_t Flags = 0;
        bool Bitfield = false;
        int ByteOffset = 0;
        std::uint8_t Mask = 0;
        std::uint8_t FieldMask = 0;
        std::uint64_t Inner = 0;
        std::uint64_t MetaClass = 0;
        std::uint64_t InnerProperty = 0;
        std::string InnerType;
        std::uint64_t ValueProperty = 0;
        std::string ValueType;

        int Start() const { return Offset + (Bitfield ? ByteOffset : 0); }
        bool MaskedBit() const { return Bitfield && FieldMask != 0xFF; }
    };

    struct UnrealFunction
    {
        std::string Name;
        std::string Owner;
        std::uint64_t Address = 0;
        std::uint32_t Flags = 0;
        std::vector<UnrealField> Parameters;
    };

    struct UnrealEnumEntry
    {
        std::string Name;
        std::int64_t Value = 0;
    };

    struct UnrealLocation
    {
        std::uint64_t Object = 0;
        std::uint64_t Class = 0;
        std::vector<std::string> Path;
        std::optional<UnrealField> Field;
        int Within = 0;
        std::uint64_t Struct = 0;
    };

    struct UnrealWalk
    {
        std::uint64_t Address = 0;
        std::string Expression;
        std::optional<UnrealField> Field;
    };

    struct UnrealSummary
    {
        std::uint64_t NamePool = 0;
        bool LegacyNames = false;
        std::uint64_t Objects = 0;
        std::string ObjectsSource;
        int ObjectCount = 0;
        std::size_t TypeCount = 0;
        double Seconds = 0.0;
        std::string Timings;
        UnrealLayout Layout;
    };

    class UnrealRuntime
    {
    public:
        static constexpr int kMaxSuperDepth = 64;
        static constexpr int kMaxFields = 8192;

        static bool LooksUnreal(const Process& process);
        Status Resolve(std::shared_ptr<const Process> process, const UnrealConfig& config, bool full, WorkProgress* progress);

        const UnrealSummary& Summary() const { return m_summary; }
        bool Rejected() const { return m_rejected; }
        std::shared_ptr<const Process> ProcessHandle() const { return m_process; }
        std::optional<std::string> WideText(std::uint64_t address, std::size_t length) const { return ReadWide(address, length); }
        const UnrealLayout& Layout() const { return m_layout; }
        std::uint64_t EngineObject() const { return m_engine.load(); }
        const std::vector<UnrealType>& Types() const { return m_typeList; }

        int Count() const;
        std::optional<std::uint64_t> ObjectAt(int index) const;
        bool IsObject(std::uint64_t address) const;
        void Each(const std::function<bool(std::uint64_t object, int index)>& visit) const;

        std::optional<std::string> NameText(std::uint32_t index) const;
        std::optional<std::string> NameFromParts(std::uint32_t index, std::uint32_t number) const;
        std::optional<std::string> NameAt(std::uint64_t address) const;
        std::optional<std::string> NameOf(std::uint64_t object) const;
        std::optional<std::uint64_t> FindName(std::string_view text) const;
        std::uint64_t ClassOf(std::uint64_t object) const;
        std::uint64_t OuterOf(std::uint64_t object) const;
        std::uint64_t SuperOf(std::uint64_t type) const;
        bool HasStructLayout(std::uint64_t type) const;
        std::string FullName(std::uint64_t object) const;
        UnrealKind KindOf(std::uint64_t type) const;

        std::uint64_t Type(std::string_view name) const;
        std::uint64_t Enum(std::string_view name) const;
        std::optional<int> StructSize(std::uint64_t type) const;
        bool IsChildOf(std::uint64_t type, std::uint64_t base) const;
        std::vector<UnrealField> Fields(std::uint64_t type, bool ownOnly = false) const;
        std::optional<UnrealField> FindField(std::uint64_t type, std::string_view name) const;
        std::optional<UnrealField> Describe(std::uint64_t property, const std::string& kind, const std::string& owner) const;
        std::vector<UnrealFunction> Functions(std::uint64_t type, bool inherited) const;
        std::vector<UnrealEnumEntry> EnumEntries(std::uint64_t enumObject) const;
        std::string CppName(std::uint64_t type) const;
        std::string TypeText(const UnrealField& field) const;

        std::vector<std::uint64_t> Instances(std::uint64_t type, std::size_t limit) const;
        std::optional<std::uint64_t> Find(std::string_view nameOrPath) const;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> OwnerOf(std::uint64_t address) const;
        std::optional<UnrealLocation> Locate(std::uint64_t address) const;
        std::optional<std::string> Whois(std::uint64_t address) const;
        std::optional<std::string> HeaderField(int offset, int& inside) const;

        std::uint64_t FindEngine() const;
        std::uint64_t Engine() const;
        std::uint64_t Follow(std::uint64_t object, const std::vector<std::string>& steps) const;
        std::uint64_t World() const;
        std::uint64_t Controller() const;
        std::uint64_t Pawn() const;
        std::optional<bool> IsOffline() const;
        Result<UnrealWalk> Walk(std::uint64_t object, const std::string& root, const std::vector<std::string>& steps) const;

        std::optional<std::uint64_t> ResolveSymbol(std::string_view name) const;
        static std::string OffsetSymbol(const std::string& owner, const std::string& field);

    private:
        struct LiveArray
        {
            std::uint64_t Array = 0;
            UnrealLayout Layout;
            std::vector<std::uint64_t> First;
            std::vector<std::vector<std::uint8_t>> Headers;
        };

        std::optional<std::uint64_t> ReadPointer(std::uint64_t address) const;
        std::uint64_t ReadObject(std::uint64_t address) const;
        std::optional<std::int32_t> ReadInt(std::uint64_t address) const;
        std::optional<std::uint32_t> ReadUInt(std::uint64_t address) const;
        std::optional<std::uint16_t> ReadWord(std::uint64_t address) const;
        std::optional<std::uint64_t> ReadQword(std::uint64_t address) const;
        bool ReadBlock(std::uint64_t address, std::vector<std::uint8_t>& out, std::size_t size) const;
        std::optional<std::string> ReadWide(std::uint64_t address, std::size_t length) const;
        std::optional<std::string> ReadText(std::uint64_t address, std::size_t length) const;
        std::vector<std::uint64_t> FindBytes(const std::string& pattern, std::uint64_t start, std::uint64_t stop, bool writable, std::uint64_t alignment, std::size_t limit) const;
        bool Cancelled() const;
        bool InImage(std::uint64_t address) const;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> WritableSections(const ModuleInfo& image) const;

        std::optional<std::string> PoolEntry(std::uint64_t entry) const;
        std::optional<std::string> LegacyEntry(std::uint64_t entry, int indexAt, int nameAt) const;
        std::uint64_t NamePoolAmong(const std::vector<std::uint64_t>& slots) const;
        std::uint64_t FindNamePool() const;
        bool IsNameEntryArray(std::uint64_t chunks, int nameAt);
        std::uint64_t FindNameEntryArray();
        std::uint64_t PatternTarget(const UnrealPattern& pattern) const;
        bool CheckNamePool(std::uint64_t pool, bool legacy);

        bool Plausible(const std::uint8_t* data, const UnrealLayout& layout) const;
        bool ProbeItems(std::uint64_t array, UnrealLayout& layout) const;
        bool FirstObjects(std::uint64_t array, LiveArray& live) const;
        std::optional<std::string> HeaderName(const LiveArray& live, int i, int offset) const;
        std::optional<LiveArray> LiveLayout(std::uint64_t array, const UnrealLayout& layout) const;
        std::optional<LiveArray> ScanSection(std::uint64_t from, std::uint64_t size, bool generic) const;
        std::optional<LiveArray> ExportedObjectArray() const;
        std::optional<LiveArray> CheckObjects(std::uint64_t array) const;
        std::optional<std::pair<LiveArray, std::string>> FindObjectArray() const;

        bool ResolveClassAndOuter(const LiveArray& live);
        void IndexTypes();
        bool ProbeSuper();
        bool ProbePropertyFields(std::uint64_t x, std::uint64_t y, std::uint64_t z);
        bool ProbeChain();
        void ProbeStructSize();
        void ProbeExtra();
        void ProbeFunctions();
        void ApplyOverrides();
        std::optional<int> Override(const char* key) const;

        std::uint64_t ItemAddress(int index) const;
        std::uint64_t Chunk(int index) const;
        bool IsFunction(std::uint64_t object) const;
        std::optional<std::string> TypeOf(std::uint64_t property, const std::vector<std::uint8_t>* block) const;
        std::uint64_t FindRawField(std::uint64_t type, std::string_view name) const;
        std::uint64_t FieldIn(std::uint64_t type, std::string_view name) const;
        std::vector<UnrealField> OwnFields(std::uint64_t type) const;
        std::optional<UnrealField> DescribeWith(std::uint64_t property, const std::string& kind, const std::string& owner, const std::vector<std::uint8_t>* block) const;
        std::optional<std::pair<std::uint64_t, std::string>> InnerProperty(std::uint64_t property, int slot, const std::vector<std::uint8_t>* block) const;
        std::string PropertyText(std::uint64_t property, const std::string& kind) const;
        bool Descend(std::uint64_t type, int offset, std::vector<std::string>& path, int depth, UnrealLocation& found) const;

        std::shared_ptr<const Process> m_process;
        WorkProgress* m_progress = nullptr;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> m_images;
        ModuleInfo m_main;
        bool m_full = false;
        bool m_rejected = false;
        UnrealLayout m_layout;
        UnrealSummary m_summary;
        std::uint64_t m_pool = 0;
        bool m_legacyNames = false;
        int m_legacyIndexAt = 0;
        int m_legacyNameAt = 0;
        std::uint64_t m_array = 0;
        int m_fieldBlock = 0;
        std::map<std::string, int> m_overrides;
        std::unordered_map<std::string, std::uint64_t> m_types;
        std::unordered_map<std::string, std::uint64_t> m_enums;
        std::vector<UnrealType> m_typeList;
        mutable std::atomic<std::uint64_t> m_engine{ 0 };

        mutable std::mutex m_lock;
        mutable std::unordered_map<std::uint32_t, std::string> m_names;
        mutable std::unordered_map<std::uint32_t, std::uint64_t> m_blocks;
        mutable std::unordered_map<int, std::uint64_t> m_chunks;
        mutable std::unordered_map<std::uint64_t, UnrealKind> m_kinds;
        mutable std::unordered_map<std::uint64_t, std::string> m_fieldTypes;
        mutable std::unordered_map<std::uint64_t, std::vector<UnrealField>> m_own;
        mutable std::unordered_map<std::uint64_t, std::vector<UnrealField>> m_merged;
        mutable std::unordered_map<std::uint64_t, std::string> m_cpp;
        mutable std::unordered_map<std::uint64_t, std::vector<UnrealEnumEntry>> m_enumEntries;
        mutable std::unordered_map<std::uint64_t, bool> m_vtables;
        mutable std::unordered_map<std::string, std::optional<std::uint64_t>> m_symbols;
    };

    const char* UnrealKindName(UnrealKind kind);
    bool IsObjectProperty(std::string_view kind);
}
