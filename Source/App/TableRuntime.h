#pragma once

#include "Core/Errors.h"
#include "Table/AddressList.h"
#include "Table/CheatTable.h"
#include "Table/Structures.h"

#include <cstdint>
#include <filesystem>
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

        Status LoadTable(const std::filesystem::path& path, bool merge);
        Status RestoreTable(const std::filesystem::path& path);
        Status SaveTable(const std::filesystem::path& path);
        void NewTable();
        std::vector<Table::TableSymbol> TableSymbols() const;
        bool TableScriptWaiting() const { return m_tableScriptWaiting; }
        void AnswerTableScript(bool run);
        bool HotkeyHeld(const Table::MemoryRecord& record, const Table::Hotkey& hotkey) const;
        void NoteActivationByHand(const Table::MemoryRecord& record);
        bool ScriptsTrusted() const { return m_scriptsTrusted; }

    private:
        void RefreshRecord(int id, double now);

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
