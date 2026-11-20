#pragma once

#include "App/Commands.h"
#include "Lua/LuaEngine.h"
#include "UI/ProcessIcon.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class MainWindow
    {
    public:
        explicit MainWindow(Workspace& workspace);
        void Draw();
        void FocusScanValue() { m_focusValue = true; }

    private:
        void TitleBar();
        float TitleTools(float left, float right, float height);
        float ProcessChip(float left, float right, float y, float height);
        void ProcessChipMenu();
        void ProcessItems();
        void ScanArea(float height);
        void ResultsCard(float width, float height);
        void ScanCard(float width, float height);
        void AddressListCard(float height);
        void StatusBar();
        void RecordRow(Table::MemoryRecord& record, int depth, int& row, bool match = true);
        void BuildRows();
        bool RowMatches(const Table::MemoryRecord& record) const;
        void ListKeys();
        void SelectRow(int index, bool extend);
        void StartInlineEdit(Table::MemoryRecord& record);
        void InlineValue(Table::MemoryRecord& record);
        void ProgressLine(float right);
        std::function<void()> StickyHeader();
        void DropOnRecord(Table::MemoryRecord& record, int depth);
        std::vector<Table::MemoryRecord*> DraggedRecords(int draggedId);
        void RecordMenu(Table::MemoryRecord& record);
        void ValueDialog();
        bool ValueChoices(const std::vector<Table::DropDownEntry>& entries);
        void AddAddressDialog();
        void DescriptionDialog();
        void DoFirstScan();
        void DoNextScan();
        void NewScan();
        Scan::ScanOptions CurrentOptions() const;
        std::vector<Table::MemoryRecord*> SelectedRecords();
        void SaveOptions();
        void OpenValueDialog(Table::MemoryRecord& record);
        void MainShortcuts();
        bool CanRun(Command command);
        void Run(Command command);
        bool Item(const char* label, Command command, bool selected = false);
        void ToggleCompactMode();
        void ToggleReleaseMode();
        void OpenAddDialog();
        void AddScriptRecord();
        void ResultMenu(std::uint64_t index);
        void RunResultCommand(Command command, std::uint64_t index);
        void ResultValueDialog();
        void SaveResultsDialog();
        void RefreshScriptMenu();
        void ScriptMenuInto(const char* merge, int& opened, int& clicked);
        void ScriptMenuShortcuts();
        std::string ResultText(const Scan::ScanRecord& record, std::span<const std::uint8_t> bytes);
        void AddResult(const Scan::ScanRecord& record);
        Status FillCustom(Scan::ScanOptions& options, const std::string& name) const;
        std::string ScanTypeLabel() const;
        Table::MemoryRecord* FocusRecord();
        void WriteValues(const std::vector<int>& ids, const std::string& text);
        void UndoValueChange();
        void DeleteSelectedRecords(bool ask);
        void RunRecordCommand(Command command, Table::MemoryRecord* record);
        void DetectMono();
        void UnityMenu();
        void UnrealMenu();
        void RttiMenu();

        struct ValueUndo
        {
            int Id = -1;
            std::string Text;
        };

        Workspace& m_workspace;
        Engine& m_engine;
        bool m_release = false;
        float m_menusWidth = 0.0f;
        std::vector<std::filesystem::path> m_backupVersions;
        IconCache m_processIcon;
        float m_chipHover = 0.0f;
        double m_chipConnectedAt = -1.0;
        std::uint32_t m_chipProcess = 0;
        std::vector<ValueUndo> m_valueUndo;
        struct ResultTarget
        {
            std::uint64_t Address = 0;
            Scan::ValueType Type = Scan::ValueType::FourBytes;
        };

        bool m_resultValueDialog = false;
        std::string m_resultValueText;
        std::vector<ResultTarget> m_resultValueTargets;
        bool m_saveResultsDialog = false;
        std::string m_saveResultsName;
        std::vector<Lua::ScriptMenuItem> m_scriptMenu;
        std::uint64_t m_scriptMenuGeneration = 0;
        bool m_monoDetected = false;
        std::uint64_t m_monoGeneration = ~0ull;
        double m_monoCheckAt = 0.0;
        double m_monoRefreshAt = 0.0;

        int m_typeIndex = 2;
        Scan::ScanKind m_scanKind = Scan::ScanKind::Single;
        std::string m_customName;
        Scan::CompareTo m_compareTo = Scan::CompareTo::Previous;
        std::string m_savedName;
        int m_compareIndex = 0;
        std::string m_value;
        std::string m_value2;
        bool m_hex = false;
        bool m_unicode = false;
        bool m_caseSensitive = true;
        int m_floatMode = 0;
        int m_writable = 1;
        int m_executable = 2;
        int m_copyOnWrite = 0;
        bool m_fastScan = true;
        int m_alignment = 4;
        bool m_pause = false;
        bool m_private = true;
        bool m_image = true;
        bool m_mapped = false;
        std::string m_start = "0";
        std::string m_stop = "7FFFFFFFFFFF";
        bool m_focusValue = false;
        float m_topHeight = 0.0f;
        float m_resultsShare = 0.58f;
        float m_stackShare = 0.42f;

        std::set<std::uint64_t> m_selectedResults;
        std::uint64_t m_lastClickedResult = 0;
        std::map<std::uint64_t, std::string> m_liveValues;
        double m_liveAt = -1.0;
        int m_liveScan = -1;

        int m_lastClickedRecord = -1;

        bool m_valueDialog = false;
        std::string m_valueText;
        std::vector<int> m_valueTargets;
        std::string m_valueSearch;
        std::string m_valueCurrent;
        std::string m_valueFiltered;
        std::vector<int> m_valueVisible;
        std::size_t m_valueEntries = 0;
        int m_valueCursor = -1;
        int m_valueScroll = 0;
        float m_valueColumn = 0.0f;
        bool m_addDialog = false;
        std::string m_addAddress;
        std::string m_addDescription;
        int m_addType = 2;
        bool m_descriptionDialog = false;
        std::string m_descriptionText;
        int m_descriptionTarget = 0;
        std::string m_tableFilter;
        struct ListRow
        {
            int Id = 0;
            int Depth = 0;
            bool Match = true;
        };
        struct RowSpot
        {
            int Id = 0;
            float Top = 0.0f;
        };
        std::vector<ListRow> m_rows;
        std::vector<RowSpot> m_spots;
        int m_anchorRecord = -1;
        int m_scrollToRecord = -1;
        int m_inlineEdit = -1;
        std::string m_inlineText;
        std::string m_inlineOriginal;
        std::vector<int> m_inlineTargets;
        bool m_inlineFocus = false;
        int m_inlineFrames = 0;
        bool m_filterActiveOnly = false;
        int m_filterKind = 0;
        std::string m_recentChecked;
        bool m_recentExists = false;
        double m_recentCheckedAt = -100.0;
    };
}
