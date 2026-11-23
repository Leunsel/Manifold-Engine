#pragma once

#include "Core/Errors.h"
#include "Table/AddressList.h"
#include "Table/CheatTable.h"
#include "Table/Structures.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold
{
    class Engine;

    struct RecordAddressText
    {
        std::string Text;
        std::string Resolved;
        std::string Tooltip;
    };

    // One step of how a record's address comes about, for the Address List Debug window: the base expression or the parent's
    // address, then for each pointer offset the read and the added offset. A step that fails has Ok false and ends the chain.
    struct AddressStep
    {
        enum class Kind
        {
            Base,
            Parent,
            Relative,
            Read,
            Offset
        };
        Kind What = Kind::Base;
        std::string Text;
        std::uint64_t Value = 0;
        bool Ok = true;
        std::string Error;
    };

    enum class SaveKind
    {
        Written,
        Writing,  // the text is being written in the background; LastSave says Written or Failed once it is done
        UpToDate,
        Queued,
        Failed
    };

    inline constexpr double kSaveCooldownSeconds = 1.0;

    class TableRuntime
    {
    public:
        explicit TableRuntime(Engine& engine) : m_engine(engine) {}

        TableRuntime(const TableRuntime&) = delete;
        TableRuntime& operator=(const TableRuntime&) = delete;

        Table::AddressList& Table() { return m_table; }
        Table::StructureStore& Structures() { return m_structures; }

        void ResolveRecord(Table::MemoryRecord& record);
        // The same walk as ResolveRecord, step by step; it reads the process but leaves the record as it is (a parent may be resolved).
        std::vector<AddressStep> ExplainAddress(Table::MemoryRecord& record);
        std::vector<std::uint8_t> ReadRecord(Table::MemoryRecord& record);
        std::string AddressHex(std::uint64_t address) const;
        RecordAddressText AddressTextOf(const Table::MemoryRecord& record) const;
        std::string FormatRecord(const Table::MemoryRecord& record, std::span<const std::uint8_t> bytes) const;
        const std::vector<Table::DropDownEntry>& DropDownOf(const Table::MemoryRecord& record) const;
        void ApplyHotkey(Table::MemoryRecord& record, const Table::Hotkey& hotkey);
        Status WriteRecord(Table::MemoryRecord& record, std::string_view text, bool recursive = true);
        Status AddToRecord(Table::MemoryRecord& record, double delta);
        void RefreshValues(bool force);
        void RefreshRecord(int id);
        bool NeedsValue(const Table::MemoryRecord& record) const;
        void SetVisibleRecords(std::vector<int> ids);
        // The records the address list drew last frame and so refreshes; empty and not known before it drew once.
        const std::vector<int>& VisibleRecords() const { return m_visibleRecords; }
        bool VisibleKnown() const { return m_visibleKnown; }
        double LastValueRefresh() const { return m_lastValues; }
        void UpdateFreezes();
        void UpdateHotkeys();
        // Deletes the record and everything below it. Refused while one of them is being switched or waits in the activation
        // queue; the active ones are switched off first, deepest first, and a script that stays on keeps the whole tree in the
        // table (its changes would stay in the game). Every removed record gets its ondestroy event.
        Status RemoveRecord(int recordId, bool fromScript = false);

        Status LoadTable(const std::filesystem::path& path, bool merge);
        Status RestoreTable(const std::filesystem::path& path);
        // Saves now. A table whose text and file are as the last save left them is not written again (UpToDate); the
        // table is then clean all the same. toast also shows the outcome as a notification; either way it is one log entry.
        Status SaveTable(const std::filesystem::path& path, bool toast = false);
        // Ctrl+S: like SaveTable with a toast, but the file is written in the background (Writing), and a request within
        // kSaveCooldownSeconds of the last save to the same file, or while a write runs, is queued and runs when both are over
        // (TickSaves), with the table as it is then. Quit, a new or opened table and a save to another file finish the
        // running write and the queued save first (FlushPendingSave).
        SaveKind RequestSave(const std::filesystem::path& path, double now);
        void TickSaves(double now);
        void FlushPendingSave();
        // Waits for a write running in the background and takes its result; nothing queued is started.
        void FinishWrite();
        bool SavePending() const { return m_pendingSave.has_value() || m_writeId != 0; }
        SaveKind LastSave() const { return m_lastSaveKind; }
        std::uint64_t TableWrites() const { return m_tableWrites; }
        void NewTable();
        std::vector<Table::TableSymbol> TableSymbols() const;
        bool TableScriptWaiting() const { return m_tableScriptWaiting; }
        void AnswerTableScript(bool run);
        bool HotkeyHeld(const Table::MemoryRecord& record, const Table::Hotkey& hotkey) const;
        void NoteActivationByHand(const Table::MemoryRecord& record);
        bool ScriptsTrusted() const { return m_scriptsTrusted; }

    private:
        void RefreshRecord(int id, double now);
        SaveKind SaveNow(const std::filesystem::path& path, double now, bool toast, bool background, Status* failure = nullptr);

        // ∑ What the last save left on the disk: the hash and size of the text and the file's write time and size right
        //   after it. A save whose text hashes the same, to a file still stamped the same, has nothing to write; any change
        //   to the file from outside (CE saving over it, a copy) changes the stamp and the save writes again.
        struct SavedFile
        {
            std::wstring Path;
            std::size_t Hash = 0;
            std::size_t Size = 0;
            std::uint64_t DiskTime = 0;
            std::uint64_t DiskSize = 0;
        };
        std::optional<SavedFile> m_savedFile;
        double m_lastSaveAt = -1.0;
        std::optional<std::filesystem::path> m_pendingSave;
        double m_pendingDue = 0.0;
        SaveKind m_lastSaveKind = SaveKind::Failed;
        std::uint64_t m_tableWrites = 0;
        int m_writeId = 0;
        std::wstring m_writingKey;

        Engine& m_engine;
        Table::AddressList m_table;
        Table::StructureStore m_structures;
        std::vector<Table::TableSymbol> m_tableSymbols;
        bool m_tableScriptWaiting = false;
        bool m_scriptsTrusted = true;
        bool m_hotkeyHoldNoted = false;
        std::string m_pendingTableScript;
        double m_lastValues = -1.0;
        double m_lastFreeze = -1.0;
        std::vector<int> m_visibleRecords;
        bool m_visibleKnown = false;
    };
}
