#pragma once

#include "Asm/AutoAssembler.h"
#include "Scan/ValueType.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Manifold::Table
{
    enum class RecordKind
    {
        Value,
        Script,
        Group
    };

    enum class FreezeMode
    {
        Normal,
        AllowIncrease,
        AllowDecrease
    };

    struct RecordOptions
    {
        bool HideChildren = false;
        bool ActivateChildren = false;
        bool DeactivateChildren = false;
        bool RecursiveSetValue = false;
        bool AllowManualExpand = false;
        bool ManualExpand = false;
        bool AlwaysHideChildren = false;
        bool AsyncScript = false;
    };

    enum class HotkeyAction
    {
        Toggle,
        ToggleAllowIncrease,
        ToggleAllowDecrease,
        Activate,
        Deactivate,
        SetValue,
        Increase,
        Decrease
    };

    struct Hotkey
    {
        int Id = 0;
        std::vector<int> Keys;
        HotkeyAction Action = HotkeyAction::Toggle;
        std::string Value;
        std::string Description;
        bool WasDown = false;
    };

    struct DropDownEntry
    {
        std::string Value;
        std::string Text;
    };

    constexpr int kMaxRecordLength = 4096;
    constexpr int kMaxRecordDepth = 32;
    constexpr int kMaxRecordId = 0x3FFFFFFF;
    constexpr int kMaxHotkeyKeys = 5;

    struct MemoryRecord
    {
        int Id = 0;
        std::string Description;
        RecordKind Kind = RecordKind::Value;
        Scan::ValueType Type = Scan::ValueType::FourBytes;
        int Length = 10;
        bool Unicode = false;
        bool ZeroTerminate = true;
        int BitStart = 0;
        int BitLength = 1;
        bool ShowHex = false;
        bool ShowSigned = true;
        std::string Address;
        std::vector<std::string> Offsets;
        std::string Script;
        std::string Comment;
        std::uint32_t Color = 0;
        bool HasColor = false;
        RecordOptions Options;
        FreezeMode Freeze = FreezeMode::Normal;
        std::vector<Hotkey> Hotkeys;
        std::vector<DropDownEntry> DropDown;
        std::string DropDownLink;
        bool DropDownOnly = false;
        bool DropDownDescriptionOnly = false;
        bool Expanded = true;
        bool AddressGroupHeader = false;
        bool DontSave = false;
        bool Async = false;
        std::string CustomTypeName;

        bool Active = false;
        std::vector<std::uint8_t> FrozenBytes;
        std::string LuaOnActivate;
        std::string LuaOnDeactivate;

        std::uint64_t ResolvedAddress = 0;
        bool AddressOk = false;
        std::string AddressError;
        std::string ValueText;
        std::vector<std::uint8_t> LastBytes;
        bool ValueChanged = false;
        double ChangedAt = -10.0;
        double RefreshedAt = -1.0;
        Asm::AaMemory ScriptMemory;
        std::string LastError;

        MemoryRecord* Parent = nullptr;
        std::vector<std::unique_ptr<MemoryRecord>> Children;

        Scan::ValueFormat Format(int pointerSize) const;
        int Size(int pointerSize) const;
        void SetLength(std::int64_t length);
        void SetBits(std::int64_t start, std::int64_t length);
        bool IsPointer() const { return !Offsets.empty(); }
        bool ChildrenVisible() const;
        bool IsDescendantOf(const MemoryRecord* other) const;
        std::unique_ptr<MemoryRecord> Clone(const std::function<int()>& nextId) const;
    };

    struct TableFile
    {
        std::string Name;
        std::shared_ptr<std::vector<std::uint8_t>> Data = std::make_shared<std::vector<std::uint8_t>>();
        std::shared_ptr<std::uint64_t> Version = std::make_shared<std::uint64_t>(0);
    };

    // ∑ Two spellings of <Files> exist. CE 7.5 and older name each element after the file (<name Encoding="Ascii85">) and read only the tag,
    //   so names that are no XML name can't be stored. CE 7.6 writes <File0 Name="name" Encoding="Ascii85">, <File1 ...> and reads the
    //   Name attribute with the tag as fallback. Both are read; the values match Settings::TableFileNames.
    enum class FileNaming
    {
        AsLoaded,
        Tag,
        Attribute
    };

    class AddressList
    {
    public:
        AddressList();

        std::vector<std::unique_ptr<MemoryRecord>>& Roots() { return m_roots; }
        const std::vector<std::unique_ptr<MemoryRecord>>& Roots() const { return m_roots; }

        MemoryRecord& Add(std::unique_ptr<MemoryRecord> record, MemoryRecord* parent = nullptr, int index = -1);
        MemoryRecord& AddValue(std::string description, std::string address, Scan::ValueType type, MemoryRecord* parent = nullptr);
        MemoryRecord& AddScript(std::string description, std::string script, MemoryRecord* parent = nullptr);
        MemoryRecord& AddGroup(std::string description, MemoryRecord* parent = nullptr);
        std::unique_ptr<MemoryRecord> Take(MemoryRecord& record);
        void Remove(MemoryRecord& record);
        bool Move(MemoryRecord& record, MemoryRecord* newParent, int index);
        void Clear();

        MemoryRecord* FindById(int id);
        MemoryRecord* FindByDescription(std::string_view description);
        void ForEach(const std::function<void(MemoryRecord&)>& visit);
        void ForEachVisible(const std::function<void(MemoryRecord&, int depth)>& visit);
        std::vector<MemoryRecord*> Flatten();
        std::size_t Count() const;
        int IndexOf(const MemoryRecord& record) const;
        std::vector<std::unique_ptr<MemoryRecord>>& SiblingsOf(MemoryRecord& record);

        int NextId() { return m_nextId++; }
        void NoteId(int id);
        std::uint64_t Generation() const { return m_generation; }
        void Touch() { ++m_generation; m_dirty = true; }
        void Refresh() { ++m_generation; }
        bool Dirty() const { return m_dirty; }
        void SetClean() { m_dirty = false; }
        // ∑ Changes only when the whole table is replaced (new table, open, restore), not on edits like Generation. Work that started
        //   on one table checks it before it lands. SessionBackup::TableReplaced bumps it, which TableRuntime::NewTable calls after Clear.
        std::uint64_t Identity() const { return m_identity; }
        void NewIdentity() { ++m_identity; }

        std::vector<int>& Selection() { return m_selection; }
        bool Selected(int id) const;
        void Select(int id, bool add);
        void ClearSelection() { m_selection.clear(); }

        struct DisasmNote
        {
            std::string Comment;
            std::string Header;
        };
        std::map<std::string, DisasmNote> DisasmNotes;

        std::string TableLuaScript;
        std::string TableComment;
        std::string FilePath;
        std::vector<TableFile> Files;
        std::vector<std::string> ExtraXml;
        int LoadedVersion = 0;
        FileNaming LoadedFileNaming = FileNaming::Tag;

        TableFile* FindFile(std::string_view name);
        TableFile& AddFile(std::string name);
        bool RemoveFile(std::string_view name);

    private:
        void IndexTree(MemoryRecord& record);
        void ClaimTree(MemoryRecord& record);
        void ForgetTree(const MemoryRecord& record);
        void IndexDescriptions();

        std::vector<std::unique_ptr<MemoryRecord>> m_roots;
        std::unordered_map<int, MemoryRecord*> m_byId;
        std::unordered_map<std::string, MemoryRecord*> m_byDescription;
        std::uint64_t m_describedAt = 0;
        std::vector<int> m_selection;
        int m_nextId = 0;
        std::uint64_t m_generation = 1;
        std::uint64_t m_identity = 1;
        bool m_dirty = false;
    };

    const char* HotkeyActionName(HotkeyAction action);
    bool UsableHotkeyKeys(const std::vector<int>& keys);
    bool KeysPressed(const std::vector<int>& keys, bool& wasDown);
    std::string KeysText(const std::vector<int>& keys);

    std::string DropDownText(const std::vector<DropDownEntry>& entries);
    std::vector<DropDownEntry> ParseDropDown(std::string_view text);
    std::string OptionsText(const RecordOptions& options);
    void ParseOptionsText(RecordOptions& options, std::string_view text);
}
