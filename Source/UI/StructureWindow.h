#pragma once

#include "App/Commands.h"
#include "Lua/LuaEngine.h"
#include "UI/AddressActions.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class StructureWindow : public ToolWindow
    {
    public:
        StructureWindow(Workspace& workspace, int number);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show(std::uint64_t address, const std::string& structure = {});
        const std::string& WindowId() const { return m_windowId; }
        bool Placed() const { return m_placed; }

    private:
        struct Column
        {
            AddressField Address;
            std::uint64_t Value = 0;
            bool Valid = false;
        };

        struct Block
        {
            std::vector<std::uint64_t> Bases;
            std::string Path;
            // The field this block hangs under: its block and its place in the
            // parent structure, -1 for the top structure.
            int ParentBlock = -1;
            const Table::StructDefinition* ParentDefinition = nullptr;
            int ParentField = -1;
        };

        struct PointerPath
        {
            std::string Expression;  // [[base+50]+28]+10
            std::vector<std::string> Names;  // _inventory, _possessedItems, _items
            std::uint64_t Address = 0;
        };

        struct Row
        {
            Table::StructDefinition* Definition = nullptr;
            int Field = 0;
            int Depth = 0;
            int Block = 0;
            bool Expanded = false;
            int Twin = 0;
        };

        struct Snapshot
        {
            int Block = 0;
            std::size_t Column = 0;
            std::uint64_t Start = 0;
            std::vector<std::uint8_t> Bytes;
            std::vector<std::uint8_t> Valid;
            bool Whole = false;
        };

        struct PendingDelete
        {
            std::string Structure;
            int Offset = 0;
        };

        void Toolbar();
        void MenuBar();
        static bool IsWindowCommand(Command command);
        void RunCommand(Command command);
        void FieldShortcuts(int pointerSize);
        void RunFieldCommand(Command command, Table::StructDefinition& definition, Table::StructField& field, std::uint64_t address, const std::string& path, int pointerSize);
        void BuildRows(Table::StructDefinition& definition, std::vector<std::uint64_t> bases, const std::string& path, int depth, int parentBlock = -1, int parentField = -1,
            const Table::StructDefinition* parentDefinition = nullptr);
        std::string RowKey(const Row& row) const;
        int AnchorRow() const;
        void SetExpanded(const Row& row, bool expand);
        void KeyboardNavigation();
        PointerPath PathOf(const Row& row) const;
        void Footer();
        int FindEditRow() const;
        void ReadVisible(int first, int last);
        bool ReadField(int block, std::size_t column, std::uint64_t address, std::uint8_t* out, std::size_t size) const;
        void DrawRow(int index, int pointerSize);
        void FieldMenu(Table::StructDefinition& definition, Table::StructField& field, const std::vector<std::uint64_t>& addresses, const std::string& path);
        void AddSelectionToAddressList();
        void DeleteSelection();
        Table::StructDefinition& Guess(const std::string& name, std::uint64_t address, int size);
        void Fill(Table::StructDefinition* definition, std::uint64_t address, int size, bool mono);
        void LookupName(std::uint64_t address, bool containing, std::function<void(std::optional<Lua::StructureLookup>)> done);
        void ExpandPointer(Table::StructDefinition& parent, Table::StructField& field, std::uint64_t target, const std::string& path);
        void PrepareNew(std::uint64_t address);
        std::string FieldText(const Table::StructField& field, std::span<const std::uint8_t> bytes, std::uint64_t address, int pointerSize);
        Status WriteBits(const Table::StructField& field, std::uint64_t address, const std::string& text);
        Table::StructDefinition* Current();

        Workspace& m_workspace;
        Engine& m_engine;
        [[maybe_unused]] int m_number = 1;
        std::string m_title;
        std::string m_windowId;
        bool m_placed = false;
        int m_dockTries = 0;
        std::string m_filter;
        bool m_open = true;
        std::vector<Column> m_columns;
        std::string m_structure;
        std::set<std::string> m_expanded;
        std::vector<Block> m_blocks;
        std::vector<Row> m_rows;
        std::vector<Snapshot> m_snapshots;
        int m_guessSize = 4096;
        bool m_newOpen = false;
        std::string m_newName;
        std::uint64_t m_newFrom = 0;
        std::uint64_t m_newStart = 0;
        std::shared_ptr<int> m_alive = std::make_shared<int>(0);
        std::uint64_t m_lookupSerial = 0;
        bool m_lookupRunning = false;
        std::string m_suggestedName;
        std::set<const Table::StructDefinition*> m_filling;
        std::set<std::string> m_childLookups;
        bool m_renameOpen = false;
        std::string m_renameText;
        std::string m_editPath;
        std::string m_editText;
        std::set<std::string> m_selected;
        std::string m_selectAnchor;
        std::string m_rangeAnchor;
        int m_scrollToRow = -1;
        double m_copiedAt = -10.0;
        std::string m_selectionStructure;
        bool m_editName = false;
        std::string m_addOffset;
        std::string m_pendingStructure;
        int m_pendingOffset = 0;
        bool m_pendingInsert = false;
        std::vector<PendingDelete> m_pendingDeletes;
    };
}
