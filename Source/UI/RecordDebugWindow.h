#pragma once

#include "App/ActivationRunner.h"
#include "Table/RecordCheck.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Manifold::UI
{
    // Address List Debug: the open table as a list for table authors and Manifold developers. Every record with its ID, address
    // chain, raw bytes, flags and problems; an inspector for the selected one; a table check (broken dropdown links, Lua that
    // looks for records that aren't there, scripts that don't assemble, addresses that don't resolve); the trace of every switch
    // on or off; and the address list's internal state.
    class RecordDebugWindow : public ToolWindowBase
    {
    public:
        explicit RecordDebugWindow(Workspace& workspace) : m_workspace(workspace) {}
        void Draw() override;
        // Selects the record and shows it in the Records tab.
        void Focus(int recordId);
        // Shows a tab by name: records, check, trace or internals. False for another name.
        bool ShowPage(std::string_view page);

    private:
        enum Tab
        {
            RecordsTab,
            CheckTab,
            TraceTab,
            InternalsTab
        };

        struct Row
        {
            int Id = 0;
            int Depth = 0;
        };

        struct Problems
        {
            int Errors = 0;
            int Warnings = 0;
            int Notes = 0;
        };

        void MenuBar();
        void TableChanged();
        void UpdateStaticIssues(bool force);
        Problems ProblemsOf(int recordId) const;
        std::vector<const Table::RecordIssue*> IssuesOf(int recordId) const;
        // Counts the issues per record once for each new check result (m_issuesSerial), not per record and frame.
        void IndexIssues() const;
        void IssuesChanged() { ++m_issuesSerial; }

        void RecordsPage();
        void BuildRows();
        void RecordTable(float height);
        void RecordRow(Table::MemoryRecord& record, int depth);
        void RecordMenu(Table::MemoryRecord& record);
        void Inspector();
        void InspectorBody(Table::MemoryRecord& record);
        std::string Report(Table::MemoryRecord& record);
        void SelectRecord(int id, bool scroll);

        void CheckPage();
        void StartRuntimeCheck();
        void StepRuntimeCheck();
        void CheckRecordAtRuntime(Table::MemoryRecord& record);
        std::string CheckReport() const;

        void TracePage();
        void TraceTable(float height, const std::vector<const ActivationTraceEntry*>& entries);
        void TraceDetails(const ActivationTraceEntry& entry);
        std::string TraceReport(const std::vector<const ActivationTraceEntry*>& entries) const;

        void InternalsPage();

        Workspace& m_workspace;
        int m_tab = RecordsTab;
        int m_requestTab = -1;
        std::uint64_t m_tableIdentity = 0;

        int m_selected = -1;
        // ∑ A record asked for by Focus before it exists: --open=recorddebug:<ID> runs before the table is loaded, and loading
        //   a table resets the selection. It is selected once the table has it.
        int m_pendingFocus = -1;
        int m_scrollTo = -1;
        bool m_followMain = true;
        int m_mainSeen = -1;
        std::string m_filter;
        bool m_onlyProblems = false;
        bool m_showHidden = true;
        std::set<int> m_collapsed;
        std::vector<Row> m_rows;
        // ∑ What m_rows was built from: it is built again only when one of these changes, and at most every kRowsRecheck
        //   seconds besides (an address error or a hidden child's visibility can change without a table change).
        struct RowsKey
        {
            std::uint64_t Generation = 0;
            std::uint64_t Identity = 0;
            std::uint64_t Issues = 0;
            std::string Filter;
            bool OnlyProblems = false;
            bool ShowHidden = false;
            std::set<int> Collapsed;
            bool operator==(const RowsKey&) const = default;
        };
        RowsKey m_rowsKey;
        bool m_rowsBuilt = false;
        double m_rowsAt = -10.0;
        float m_inspectorWidth = 360.0f;

        std::vector<Table::RecordIssue> m_static;
        std::vector<Table::RecordReference> m_references;
        std::uint64_t m_staticGeneration = 0;
        double m_staticAt = -10.0;

        std::vector<Table::RecordIssue> m_runtime;
        std::uint64_t m_issuesSerial = 1;
        mutable std::uint64_t m_indexedSerial = 0;
        mutable std::unordered_map<int, Problems> m_problems;
        std::vector<int> m_checkQueue;
        std::size_t m_checkNext = 0;
        bool m_checkRunning = false;
        bool m_checkAssemble = true;
        bool m_checkRan = false;
        double m_checkStarted = 0.0;
        double m_checkSeconds = 0.0;
        int m_checkLevel = 0;
        std::string m_checkFilter;

        std::uint64_t m_traceSelected = 0;
        bool m_traceOnlySelected = false;
        bool m_traceOnlyFailures = false;
        std::string m_traceFilter;
        float m_traceDetailHeight = 200.0f;
    };
}
