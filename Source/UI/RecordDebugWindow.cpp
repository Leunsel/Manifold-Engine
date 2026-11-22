#include "UI/RecordDebugWindow.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "Table/RecordPath.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace Manifold::UI
{
    namespace
    {
        using Table::IssueLevel;
        using Table::MemoryRecord;
        using Table::RecordIssue;
        using Table::RecordKind;

        constexpr double kStaticRecheck = 0.5;
        constexpr double kRowsRecheck = 0.25;
        constexpr double kCheckBudget = 0.006;
        constexpr const char* kRecordEvents[] = { "onactivate", "ondeactivate", "onvaluechanged", "ongetdisplayvalue", "ondestroy" };

        ImVec4 LevelTint(IssueLevel level)
        {
            switch (level)
            {
            case IssueLevel::Error: return ManifoldTheme::Error;
            case IssueLevel::Warning: return ManifoldTheme::Warning;
            default: return ManifoldTheme::Accent;
            }
        }

        const char* LevelGlyph(IssueLevel level)
        {
            switch (level)
            {
            case IssueLevel::Error: return ICON_FA_TIMES_CIRCLE;
            case IssueLevel::Warning: return ICON_FA_EXCLAMATION_TRIANGLE;
            default: return ICON_FA_INFO_CIRCLE;
            }
        }

        const char* KindName(RecordKind kind)
        {
            switch (kind)
            {
            case RecordKind::Script: return "Script";
            case RecordKind::Group: return "Group";
            default: return "Value";
            }
        }

        const char* KindIcon(RecordKind kind)
        {
            switch (kind)
            {
            case RecordKind::Script: return ICON_FA_CODE;
            case RecordKind::Group: return ICON_FA_FOLDER;
            default: return ICON_FA_MEMORY;
            }
        }

        std::string TypeOf(const MemoryRecord& record)
        {
            if (record.Kind != RecordKind::Value)
                return "";
            std::string type;
            if (record.Type == Scan::ValueType::String)
                type = std::string(record.Unicode ? "Unicode[" : "String[") + std::to_string(record.Length) + "]";
            else if (record.Type == Scan::ValueType::ByteArray)
                type = "AOB[" + std::to_string(record.Length) + "]";
            else if (record.Type == Scan::ValueType::Binary)
                type = "Binary " + std::to_string(record.BitStart) + ":" + std::to_string(record.BitLength);
            else
                type = Scan::TypeName(record.Type);
            if (!record.CustomTypeName.empty())
                type = record.CustomTypeName + " (custom, stored as " + type + ")";
            return type;
        }

        std::string Name(const MemoryRecord& record)
        {
            return record.Description.empty() ? "(no description)" : record.Description;
        }

        std::string Ago(double now, double at)
        {
            if (at < 0.0)
                return "never";
            const double seconds = std::max(0.0, now - at);
            if (seconds < 1.0)
                return Format("%.0f ms ago", seconds * 1000.0);
            if (seconds < 120.0)
                return Format("%.1f s ago", seconds);
            return Format("%.0f min ago", seconds / 60.0);
        }

        std::string Took(double seconds)
        {
            if (seconds <= 0.0)
                return "-";
            if (seconds < 0.001)
                return Format("%.0f µs", seconds * 1e6);
            if (seconds < 1.0)
                return Format("%.1f ms", seconds * 1000.0);
            return Format("%.2f s", seconds);
        }

        std::string ShortBytes(const std::vector<std::uint8_t>& bytes, std::size_t most)
        {
            if (bytes.empty())
                return "";
            const std::size_t shown = std::min(bytes.size(), most);
            std::string text = HexBytes(std::span<const std::uint8_t>(bytes.data(), shown));
            if (bytes.size() > shown)
                text += Format(" … (%zu bytes)", bytes.size());
            return text;
        }

        // The trace keeps only a prefix of each write; WriteSizes has the full sizes.
        std::size_t TracedWriteSize(const ActivationTraceEntry& entry, std::size_t index)
        {
            return index < entry.WriteSizes.size() ? entry.WriteSizes[index] : entry.Writes[index].Bytes.size();
        }

        std::string TracedBytes(const ActivationTraceEntry& entry, std::size_t index, std::size_t most)
        {
            const std::vector<std::uint8_t>& bytes = entry.Writes[index].Bytes;
            const std::size_t full = TracedWriteSize(entry, index);
            const std::size_t shown = std::min(bytes.size(), most);
            std::string text = HexBytes(std::span<const std::uint8_t>(bytes.data(), shown));
            if (full > shown)
                text += Format(" … (%zu bytes)", full);
            return text;
        }

        const char* OutcomeName(ActivationTraceEntry::Outcome outcome)
        {
            switch (outcome)
            {
            case ActivationTraceEntry::Outcome::Failed: return "Failed";
            case ActivationTraceEntry::Outcome::Vetoed: return "Stopped by Lua";
            default: return "Done";
            }
        }

        ImVec4 OutcomeTint(ActivationTraceEntry::Outcome outcome)
        {
            switch (outcome)
            {
            case ActivationTraceEntry::Outcome::Failed: return ManifoldTheme::Error;
            case ActivationTraceEntry::Outcome::Vetoed: return ManifoldTheme::Warning;
            default: return ManifoldTheme::Success;
            }
        }

        std::vector<std::string> OptionNames(const Table::RecordOptions& options)
        {
            std::vector<std::string> names;
            if (options.HideChildren)
                names.push_back("hide children while off");
            if (options.AlwaysHideChildren)
                names.push_back("always hide children");
            if (options.ActivateChildren)
                names.push_back("activate children");
            if (options.DeactivateChildren)
                names.push_back("deactivate children");
            if (options.RecursiveSetValue)
                names.push_back("set value of children");
            if (options.AllowManualExpand)
                names.push_back("manual collapse");
            if (options.AsyncScript)
                names.push_back("async script");
            return names;
        }

        std::vector<std::string> FlagsOf(Engine& engine, const MemoryRecord& record)
        {
            std::vector<std::string> flags;
            if (record.Active)
            {
                if (record.Kind == RecordKind::Value)
                    flags.push_back(record.Freeze == Table::FreezeMode::AllowIncrease ? "frozen (may rise)" : record.Freeze == Table::FreezeMode::AllowDecrease ? "frozen (may fall)" : "frozen");
                else
                    flags.push_back("on");
            }
            if (engine.ActivationPending(record.Id))
                flags.push_back("queued");
            if (record.Async)
                flags.push_back("async");
            if (record.DontSave)
                flags.push_back("not saved");
            if (record.AddressGroupHeader)
                flags.push_back("address header");
            if (!record.Hotkeys.empty())
                flags.push_back(Format("%zu %s", record.Hotkeys.size(), record.Hotkeys.size() == 1 ? "hotkey" : "hotkeys"));
            if (!record.DropDownLink.empty())
                flags.push_back("list → " + Table::DropDownLinkTarget(record));
            else if (!record.DropDown.empty())
                flags.push_back(Format("list of %zu", record.DropDown.size()));
            if (!record.CustomTypeName.empty())
                flags.push_back("custom type");
            if (!record.LuaOnActivate.empty() || !record.LuaOnDeactivate.empty())
                flags.push_back("lua");
            if (record.ShowHex)
                flags.push_back("hex");
            for (std::string& option : OptionNames(record.Options))
                flags.push_back(std::move(option));
            return flags;
        }

        bool BeginProperties(const char* id)
        {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
                return false;
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, Px(118.0f));
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            return true;
        }

        void Property(const char* name, std::string_view value, ImVec4 color = ManifoldTheme::Text, bool mono = false)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
            ImGui::TextUnformatted(name);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (mono)
                PushMono();
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(value.data(), value.data() + value.size());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (mono)
                PopMono();
            if (ImGui::BeginPopupContextItem(name))
            {
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy"))
                    ImGui::SetClipboardText(std::string(value).c_str());
                ImGui::EndPopup();
            }
        }

        bool Section(const char* title, bool open = true)
        {
            ImGui::Spacing();
            return ImGui::CollapsingHeader(title, open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        }

        std::string StepLabel(AddressStep::Kind kind)
        {
            switch (kind)
            {
            case AddressStep::Kind::Parent: return "Parent";
            case AddressStep::Kind::Relative: return "Relative";
            case AddressStep::Kind::Read: return "Read";
            case AddressStep::Kind::Offset: return "Offset";
            default: return "Base";
            }
        }
    }

    void RecordDebugWindow::Focus(int recordId)
    {
        Show();
        if (recordId < 0)
            return;
        m_requestTab = RecordsTab;
        m_pendingFocus = recordId;
        // ∑ Opening on a record means "this one": a filter that hides it would look like the record is missing.
        m_filter.clear();
        m_onlyProblems = false;
    }

    bool RecordDebugWindow::ShowPage(std::string_view page)
    {
        constexpr std::pair<std::string_view, int> kPages[] = { { "records", RecordsTab }, { "check", CheckTab }, { "trace", TraceTab }, { "internals", InternalsTab } };
        for (const auto& [name, tab] : kPages)
        {
            if (EqualsNoCase(page, name))
            {
                Show();
                m_requestTab = tab;
                return true;
            }
        }
        return false;
    }

    void RecordDebugWindow::SelectRecord(int id, bool scroll)
    {
        m_selected = id;
        if (!scroll)
            return;
        m_scrollTo = id;
        Table::AddressList& list = m_workspace.Core().Table();
        if (Table::MemoryRecord* record = list.FindById(id))
        {
            for (Table::MemoryRecord* parent = record->Parent; parent; parent = parent->Parent)
                m_collapsed.erase(parent->Id);
        }
    }

    void RecordDebugWindow::TableChanged()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        if (list.Identity() == m_tableIdentity)
            return;
        m_tableIdentity = list.Identity();
        m_selected = -1;
        m_collapsed.clear();
        m_static.clear();
        m_references.clear();
        m_staticGeneration = 0;
        m_runtime.clear();
        IssuesChanged();
        m_checkQueue.clear();
        m_checkRunning = false;
        m_checkRan = false;
        m_traceSelected = 0;
    }

    void RecordDebugWindow::UpdateStaticIssues(bool force)
    {
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        if (!force && (list.Generation() == m_staticGeneration || engine.Now() - m_staticAt < kStaticRecheck))
            return;
        Table::CheckHooks hooks;
        Lua::LuaEngine& lua = engine.Lua();
        hooks.CustomTypeExists = [&lua](std::string_view name) { return lua.FindCustomType(name) != nullptr; };
        m_static = Table::CheckRecords(list, hooks);
        IssuesChanged();
        m_references = Table::FindRecordReferences(list);
        m_staticGeneration = list.Generation();
        m_staticAt = engine.Now();
    }

    std::vector<const RecordIssue*> RecordDebugWindow::IssuesOf(int recordId) const
    {
        std::vector<const RecordIssue*> issues;
        for (const RecordIssue& issue : m_static)
        {
            if (issue.RecordId == recordId)
                issues.push_back(&issue);
        }
        for (const RecordIssue& issue : m_runtime)
        {
            if (issue.RecordId == recordId)
                issues.push_back(&issue);
        }
        return issues;
    }

    void RecordDebugWindow::IndexIssues() const
    {
        if (m_indexedSerial == m_issuesSerial)
            return;
        m_problems.clear();
        for (const std::vector<RecordIssue>* issues : { &m_static, &m_runtime })
        {
            for (const RecordIssue& issue : *issues)
            {
                Problems& problems = m_problems[issue.RecordId];
                if (issue.Level == IssueLevel::Error)
                    ++problems.Errors;
                else if (issue.Level == IssueLevel::Warning)
                    ++problems.Warnings;
                else
                    ++problems.Notes;
            }
        }
        m_indexedSerial = m_issuesSerial;
    }

    RecordDebugWindow::Problems RecordDebugWindow::ProblemsOf(int recordId) const
    {
        IndexIssues();
        const auto indexed = m_problems.find(recordId);
        Problems problems = indexed == m_problems.end() ? Problems{} : indexed->second;
        if (const Table::MemoryRecord* record = m_workspace.Core().Table().FindById(recordId); record && !record->LastError.empty())
            ++problems.Errors;
        return problems;
    }

    void RecordDebugWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_STETHOSCOPE "  Address List Debug###recorddebug", ImVec2(1360.0f, 680.0f), ImGuiWindowFlags_MenuBar, ImVec2(620.0f, 300.0f)))
            return;
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        TableChanged();
        UpdateStaticIssues(false);
        if (m_checkRunning)
            StepRuntimeCheck();

        if (m_followMain)
        {
            const std::vector<int>& selection = list.Selection();
            const int main = selection.empty() ? -1 : selection.back();
            if (main != m_mainSeen)
            {
                m_mainSeen = main;
                if (main >= 0 && list.FindById(main))
                    SelectRecord(main, true);
            }
        }
        if (m_pendingFocus >= 0 && list.FindById(m_pendingFocus))
        {
            SelectRecord(m_pendingFocus, true);
            m_mainSeen = list.Selection().empty() ? -1 : list.Selection().back();
            m_pendingFocus = -1;
        }
        if (m_selected >= 0 && !list.FindById(m_selected))
            m_selected = -1;

        MenuBar();

        int errors = 0;
        int warnings = 0;
        for (const std::vector<RecordIssue>* issues : { &m_static, &m_runtime })
        {
            for (const RecordIssue& issue : *issues)
            {
                errors += issue.Level == IssueLevel::Error;
                warnings += issue.Level == IssueLevel::Warning;
            }
        }

        if (ImGui::BeginTabBar("##debugtabs"))
        {
            const auto tab = [&](const char* label, int id)
                {
                    ImGuiTabItemFlags flags = 0;
                    if (m_requestTab == id)
                        flags |= ImGuiTabItemFlags_SetSelected;
                    const bool open = ImGui::BeginTabItem(label, nullptr, flags);
                    if (open)
                        m_tab = id;
                    return open;
                };
            if (tab(ICON_FA_LIST "  Records###records", RecordsTab))
            {
                RecordsPage();
                ImGui::EndTabItem();
            }
            const std::string checkLabel = std::string(ICON_FA_CLIPBOARD_CHECK "  Table Check") +
                (errors || warnings ? Format(" (%d)", errors + warnings) : std::string()) + "###check";
            if (errors)
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Error);
            const bool checkOpen = tab(checkLabel.c_str(), CheckTab);
            if (errors)
                ImGui::PopStyleColor();
            if (checkOpen)
            {
                CheckPage();
                ImGui::EndTabItem();
            }
            const std::string traceLabel = std::string(ICON_FA_HISTORY "  Activation Trace") +
                (engine.Activation().Trace().empty() ? std::string() : Format(" (%zu)", engine.Activation().Trace().size())) + "###trace";
            if (tab(traceLabel.c_str(), TraceTab))
            {
                TracePage();
                ImGui::EndTabItem();
            }
            if (tab(ICON_FA_MICROCHIP "  Internals###internals", InternalsTab))
            {
                InternalsPage();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        m_requestTab = -1;
        EndToolWindow();
    }

    void RecordDebugWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        Engine& engine = m_workspace.Core();
        Table::MemoryRecord* selected = m_selected >= 0 ? engine.Table().FindById(m_selected) : nullptr;
        if (ImGui::BeginMenu("Records"))
        {
            if (ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Show in the Address List", nullptr, false, selected != nullptr))
                m_workspace.RevealRecord(selected->Id);
            if (ImGui::MenuItem(ICON_FA_SLIDERS_H "  Edit Record...", nullptr, false, selected != nullptr))
                m_workspace.OpenRecordEditor(selected->Id);
            if (ImGui::MenuItem(ICON_FA_CODE "  Edit Script", nullptr, false, selected && selected->Kind == RecordKind::Script))
                m_workspace.OpenScript(selected->Script, selected->Id);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy Report", "Ctrl+C", false, selected != nullptr))
                ImGui::SetClipboardText(Report(*selected).c_str());
            ItemTooltip("Everything the inspector shows about the record, as text.");
            ImGui::Separator();
            ImGui::MenuItem("Follow the Address List Selection", nullptr, &m_followMain);
            ItemTooltip("Selecting a record in the address list selects it here too.");
            ImGui::MenuItem("Only Records with Problems", nullptr, &m_onlyProblems);
            ImGui::MenuItem("Show Hidden Children", nullptr, &m_showHidden);
            ItemTooltip("Also list children the address list hides (hide children while off, always hide).");
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CARET_DOWN "  Expand All"))
                m_collapsed.clear();
            if (ImGui::MenuItem(ICON_FA_CARET_RIGHT "  Collapse All"))
                engine.Table().ForEach([this](Table::MemoryRecord& record) { if (!record.Children.empty()) m_collapsed.insert(record.Id); });
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Check"))
        {
            if (ImGui::MenuItem(ICON_FA_PLAY "  Run Check with the Process", nullptr, false, engine.HasProcess() && !m_checkRunning))
            {
                m_requestTab = CheckTab;
                StartRuntimeCheck();
            }
            ImGui::MenuItem("Assemble Scripts (Check Only)", nullptr, &m_checkAssemble);
            ItemTooltip("The check assembles each script's [ENABLE] part (or [DISABLE] when it is on) without writing anything, like the script editor's Check.");
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy Problems"))
                ImGui::SetClipboardText(CheckReport().c_str());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Trace"))
        {
            ImGui::MenuItem("Only the Selected Record", nullptr, &m_traceOnlySelected);
            ImGui::MenuItem("Only Failures", nullptr, &m_traceOnlyFailures);
            ImGui::Separator();
            if (DangerMenuItem(ICON_FA_TRASH "  Clear Trace", nullptr, !engine.Activation().Trace().empty()))
            {
                engine.Activation().ClearTrace();
                m_traceSelected = 0;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // ---------------------------------------------------------------- Records

    void RecordDebugWindow::BuildRows()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        const double now = m_workspace.Core().Now();
        RowsKey key{ list.Generation(), list.Identity(), m_issuesSerial, m_filter, m_onlyProblems, m_showHidden, m_collapsed };
        if (m_rowsBuilt && key == m_rowsKey && now - m_rowsAt < kRowsRecheck)
            return;
        m_rowsKey = std::move(key);
        m_rowsBuilt = true;
        m_rowsAt = now;
        m_rows.clear();
        const bool filtering = !m_filter.empty() || m_onlyProblems;
        const auto idText = ParseDecimal(m_filter);
        const auto matches = [&](Table::MemoryRecord& record)
            {
                if (m_onlyProblems)
                {
                    const Problems problems = ProblemsOf(record.Id);
                    if (!problems.Errors && !problems.Warnings && !problems.Notes && record.AddressError.empty())
                        return false;
                }
                if (m_filter.empty())
                    return true;
                return (idText && *idText == record.Id) || ContainsNoCase(record.Description, m_filter) ||
                       ContainsNoCase(record.Address, m_filter);
            };
        const std::function<void(Table::MemoryRecord&, int)> visit = [&](Table::MemoryRecord& record, int depth)
            {
                if (matches(record))
                    m_rows.push_back({ record.Id, depth });
                if (!filtering && m_collapsed.count(record.Id))
                    return;
                if (!filtering && !m_showHidden && !record.ChildrenVisible())
                    return;
                for (const auto& child : record.Children)
                    visit(*child, depth + 1);
            };
        for (const auto& root : list.Roots())
            visit(*root, 0);
    }

    void RecordDebugWindow::RecordsPage()
    {
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        ImGui::SetNextItemWidth(std::min(Px(300.0f), ImGui::GetContentRegionAvail().x));
        InputTextWithHint("##recordfilter", ICON_FA_SEARCH "  Filter by description, ID or address", m_filter);
        ImGui::SameLine();
        ImGui::Checkbox("Only Problems", &m_onlyProblems);
        BuildRows();
        ImGui::SameLine();
        Muted(m_rows.size() == list.Count() ? Format("%zu records", list.Count()) : Format("%zu of %zu records", m_rows.size(), list.Count()));

        if (list.Count() == 0)
        {
            EmptyState(ICON_FA_STETHOSCOPE, "The table is empty",
                "Open a table or add records; this list then shows each record's ID, address chain, bytes, flags and problems.", {}, false);
            return;
        }

        const float gap = Px(6.0f);
        const ImVec2 origin = ImGui::GetCursorPos();
        const float total = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetContentRegionAvail().y;
        const float minList = Px(320.0f);
        const float minSide = Px(260.0f);
        m_inspectorWidth = std::clamp(m_inspectorWidth, minSide, std::max(minSide, total - minList - gap));
        float side = m_inspectorWidth;
        float left = total - side - gap;

        ImGui::BeginChild("##recordlist", ImVec2(left, height));
        RecordTable(ImGui::GetContentRegionAvail().y);
        ImGui::EndChild();

        ImGui::SetCursorPos(origin);
        if (Splitter("##inspectorsplit", true, gap, &left, &side, minList, minSide, height))
            m_inspectorWidth = side;
        ImGui::SetCursorPos(ImVec2(origin.x + total - side, origin.y));
        ImGui::BeginChild("##inspector", ImVec2(side, height), ImGuiChildFlags_Borders);
        Inspector();
        ImGui::EndChild();

        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
                             !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (focused && !m_rows.empty())
        {
            const ImGuiIO& io = ImGui::GetIO();
            const auto at = std::find_if(m_rows.begin(), m_rows.end(), [this](const Row& row) { return row.Id == m_selected; });
            const int index = at == m_rows.end() ? -1 : static_cast<int>(at - m_rows.begin());
            const int last = static_cast<int>(m_rows.size()) - 1;
            int next = index;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                next = std::min(last, index + 1);
            else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                next = std::max(0, index - 1);
            else if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
                next = 0;
            else if (ImGui::IsKeyPressed(ImGuiKey_End, false))
                next = last;
            if (next != index && next >= 0)
                SelectRecord(m_rows[static_cast<std::size_t>(next)].Id, true);
            if (Table::MemoryRecord* record = m_selected >= 0 ? list.FindById(m_selected) : nullptr)
            {
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
                    ImGui::SetClipboardText(Report(*record).c_str());
                else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && !record->Children.empty())
                    m_collapsed.erase(record->Id);
                else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && !record->Children.empty())
                    m_collapsed.insert(record->Id);
                else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
                    m_workspace.RevealRecord(record->Id);
            }
        }
    }

    void RecordDebugWindow::RecordTable(float height)
    {
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##debugrecords", 9, flags, ImVec2(0.0f, height)))
            return;
        // ∑ Kind is the icon before the description already and Refreshed is for refresh questions, so both start hidden;
        //   the header's context menu shows them.
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, Px(52.0f));
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 2.2f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 0.55f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Resolved", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Value and Bytes", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Refreshed", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 0.7f);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Problems", ImGuiTableColumnFlags_WidthFixed, Px(64.0f));
        ImGui::TableHeadersRow();
        int scrollIndex = -1;
        if (m_scrollTo >= 0)
        {
            for (std::size_t i = 0; i < m_rows.size(); ++i)
            {
                if (m_rows[i].Id == m_scrollTo)
                    scrollIndex = static_cast<int>(i);
            }
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()));
        if (scrollIndex >= 0)
            clipper.IncludeItemByIndex(scrollIndex);
        PushMono();
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Row row = m_rows[static_cast<std::size_t>(i)];
                Table::MemoryRecord* record = list.FindById(row.Id);
                if (record && engine.NeedsValue(*record))
                {
                    engine.RefreshRecord(row.Id);
                    record = list.FindById(row.Id);
                }
                if (!record)
                    continue;
                ImGui::PushID(row.Id);
                RecordRow(*record, row.Depth);
                ImGui::PopID();
            }
        }
        PopMono();
        if (scrollIndex < 0)
            m_scrollTo = -1;
        ImGui::EndTable();
    }

    void RecordDebugWindow::RecordRow(Table::MemoryRecord& record, int depth)
    {
        Engine& engine = m_workspace.Core();
        const double now = engine.Now();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool selected = record.Id == m_selected;
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap))
        {
            SelectRecord(record.Id, false);
            m_pendingFocus = -1;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                m_workspace.RevealRecord(record.Id);
        }
        if (m_scrollTo == record.Id)
        {
            ImGui::SetScrollHereY(0.4f);
            m_scrollTo = -1;
        }
        if (ImGui::BeginPopupContextItem("##debugrecordmenu"))
        {
            SelectRecord(record.Id, false);
            RecordMenu(record);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(record.FileId >= 0 && record.FileId != record.Id ? ManifoldTheme::Warning : ManifoldTheme::Label, "%d", record.Id);
        if (record.FileId >= 0 && record.FileId != record.Id)
            ItemTooltip(Format("ID %d in the table file", record.FileId).c_str());

        ImGui::TableNextColumn();
        const float indent = Px(14.0f) * static_cast<float>(depth);
        if (indent > 0.0f)
            ImGui::Indent(indent);
        if (!record.Children.empty())
        {
            const bool collapsed = m_collapsed.count(record.Id) > 0 && m_filter.empty() && !m_onlyProblems;
            ImGui::TextColored(ManifoldTheme::Label, "%s", collapsed ? ICON_FA_CARET_RIGHT : ICON_FA_CARET_DOWN);
            if (ImGui::IsItemClicked())
            {
                if (collapsed)
                    m_collapsed.erase(record.Id);
                else
                    m_collapsed.insert(record.Id);
            }
            ImGui::SameLine(0.0f, Px(4.0f));
        }
        const bool hidden = record.Parent && !record.Parent->ChildrenVisible();
        ImGui::TextColored(ManifoldTheme::Muted, "%s", KindIcon(record.Kind));
        ImGui::SameLine(0.0f, Px(5.0f));
        const ImVec4 color = hidden ? ManifoldTheme::Muted : record.HasColor ? RecordColor(record.Color) : ManifoldTheme::Text;
        TextFitted(Name(record), ImGui::GetContentRegionAvail().x, record.Description.empty() ? ManifoldTheme::Muted : color);
        if (hidden)
            ItemTooltip("The address list hides this record (its parent hides children).");
        if (indent > 0.0f)
            ImGui::Unindent(indent);

        ImGui::TableNextColumn();
        Muted(KindName(record.Kind));

        ImGui::TableNextColumn();
        const bool hasAddress = record.Kind == RecordKind::Value || (record.Kind == RecordKind::Group && record.AddressGroupHeader);
        if (hasAddress)
        {
            std::string written = record.Address;
            for (const std::string& offset : record.Offsets)
                written += " → " + (offset.empty() ? std::string("0") : offset);
            TextFitted(written, ImGui::GetContentRegionAvail().x, ManifoldTheme::AddressText);
            if (record.IsPointer())
                ItemTooltip(("[" + record.Address + "] + " + Join(record.Offsets, " + ")).c_str());
        }

        ImGui::TableNextColumn();
        if (hasAddress)
        {
            if (record.Kind == RecordKind::Group)
                engine.ResolveRecord(record);
            if (!engine.HasProcess())
                Muted("-");
            else if (record.AddressOk)
                TextFitted(engine.Tables().AddressHex(record.ResolvedAddress), ImGui::GetContentRegionAvail().x, ManifoldTheme::Text);
            else
            {
                ImGui::TextColored(ManifoldTheme::Error, ICON_FA_UNLINK " unresolved");
                if (!record.AddressError.empty())
                    ErrorTooltip(record.AddressError);
            }
        }

        ImGui::TableNextColumn();
        if (record.Kind == RecordKind::Value)
        {
            const bool fresh = now - record.ChangedAt < 0.6;
            ImGui::TextColored(record.ValueText == "??" ? ManifoldTheme::Muted : fresh ? ManifoldTheme::Changed : ManifoldTheme::Text, "%s", record.ValueText.c_str());
            if (!record.LastBytes.empty())
            {
                ImGui::SameLine();
                TextFitted(ShortBytes(record.LastBytes, 8), ImGui::GetContentRegionAvail().x, ManifoldTheme::BytesText);
            }
        }
        else if (record.Kind == RecordKind::Script)
        {
            Muted(record.Active ? "enabled" : "disabled");
            if (!record.ScriptMemory.Allocations.empty())
            {
                ImGui::SameLine();
                Muted(Format("· %zu alloc", record.ScriptMemory.Allocations.size()));
            }
        }
        else if (!record.Children.empty())
            Muted(Format("%zu children", record.Children.size()));

        ImGui::TableNextColumn();
        if (record.Kind == RecordKind::Value)
            Muted(record.RefreshedAt < 0.0 ? "never" : Took(now - record.RefreshedAt));

        ImGui::TableNextColumn();
        const std::vector<std::string> flags = FlagsOf(engine, record);
        if (!flags.empty())
        {
            TextFitted(Join(flags, " · "), ImGui::GetContentRegionAvail().x, record.Active ? ManifoldTheme::Success : ManifoldTheme::Muted);
            if (ImGui::IsItemHovered())
                ShowTooltip([&]()
                    {
                        for (const std::string& flag : flags)
                            ImGui::BulletText("%s", flag.c_str());
                    });
        }

        ImGui::TableNextColumn();
        const Problems problems = ProblemsOf(record.Id);
        if (problems.Errors || problems.Warnings || problems.Notes)
        {
            const IssueLevel worst = problems.Errors ? IssueLevel::Error : problems.Warnings ? IssueLevel::Warning : IssueLevel::Info;
            ImGui::TextColored(LevelTint(worst), "%s %d", LevelGlyph(worst), problems.Errors + problems.Warnings + problems.Notes);
            if (ImGui::IsItemHovered())
                ShowTooltip([&]()
                    {
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
                        if (!record.LastError.empty())
                            ImGui::TextColored(ManifoldTheme::Error, ICON_FA_TIMES_CIRCLE "  Last switch failed: %s", record.LastError.c_str());
                        for (const RecordIssue* issue : IssuesOf(record.Id))
                            ImGui::TextColored(LevelTint(issue->Level), "%s  %s", LevelGlyph(issue->Level), issue->Text.c_str());
                        ImGui::PopTextWrapPos();
                    });
        }
    }

    void RecordDebugWindow::RecordMenu(Table::MemoryRecord& record)
    {
        ImGui::TextDisabled("%s (ID %d)", Name(record).c_str(), record.Id);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Show in the Address List", "Enter"))
            m_workspace.RevealRecord(record.Id);
        if (ImGui::MenuItem(ICON_FA_SLIDERS_H "  Edit Record..."))
            m_workspace.OpenRecordEditor(record.Id);
        if (record.Kind == RecordKind::Script && ImGui::MenuItem(ICON_FA_CODE "  Edit Script"))
            m_workspace.OpenScript(record.Script, record.Id);
        if (record.Kind == RecordKind::Value && record.AddressOk && ImGui::MenuItem(ICON_FA_MEMORY "  Browse This Memory Region"))
            m_workspace.RevealInMemoryViewer(record.ResolvedAddress, static_cast<std::uint64_t>(std::max(1, record.Size(8))), false, true);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy Report", "Ctrl+C"))
            ImGui::SetClipboardText(Report(record).c_str());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy ID"))
            ImGui::SetClipboardText(std::to_string(record.Id).c_str());
        if (record.Kind == RecordKind::Value && record.AddressOk && ImGui::MenuItem(ICON_FA_COPY "  Copy Resolved Address"))
            ImGui::SetClipboardText(Hex(record.ResolvedAddress).c_str());
        if (ImGui::MenuItem(ICON_FA_HISTORY "  Show Its Activation Trace"))
        {
            m_traceOnlySelected = true;
            m_requestTab = TraceTab;
        }
    }

    void RecordDebugWindow::Inspector()
    {
        Engine& engine = m_workspace.Core();
        Table::MemoryRecord* record = m_selected >= 0 ? engine.Table().FindById(m_selected) : nullptr;
        if (!record)
        {
            EmptyState(ICON_FA_STETHOSCOPE, "No record selected", "Pick a record in the list or in the address list to see everything about it here.", {}, false);
            return;
        }
        InspectorBody(*record);
    }

    void RecordDebugWindow::InspectorBody(Table::MemoryRecord& record)
    {
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        const double now = engine.Now();

        ImGui::TextColored(ManifoldTheme::Muted, "%s", KindIcon(record.Kind));
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(Name(record).c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("ID %d  ·  %s", record.Id, KindName(record.Kind));
        if (ImGui::SmallButton(ICON_FA_CROSSHAIRS "  Show"))
            m_workspace.RevealRecord(record.Id);
        ItemTooltip("Select it in the address list.");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_SLIDERS_H "  Edit"))
            m_workspace.OpenRecordEditor(record.Id);
        if (record.Kind == RecordKind::Script)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_CODE "  Script"))
                m_workspace.OpenScript(record.Script, record.Id);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_COPY "  Copy Report"))
            ImGui::SetClipboardText(Report(record).c_str());

        const std::vector<const RecordIssue*> issues = IssuesOf(record.Id);
        if (!issues.empty() || !record.LastError.empty())
        {
            const std::string title = Format(ICON_FA_EXCLAMATION_TRIANGLE "  Problems (%zu)###problems", issues.size() + (record.LastError.empty() ? 0 : 1));
            if (Section(title.c_str()))
            {
                ImGui::PushTextWrapPos(0.0f);
                if (!record.LastError.empty())
                    ImGui::TextColored(ManifoldTheme::Error, ICON_FA_TIMES_CIRCLE "  Last switch failed: %s", record.LastError.c_str());
                for (const RecordIssue* issue : issues)
                    ImGui::TextColored(LevelTint(issue->Level), "%s  %s", LevelGlyph(issue->Level), issue->Text.c_str());
                ImGui::PopTextWrapPos();
            }
        }

        if (Section("Identity") && BeginProperties("##identity"))
        {
            Property("ID", std::to_string(record.Id), ManifoldTheme::Text, true);
            if (record.FileId >= 0)
                Property("ID in the file", std::to_string(record.FileId), record.FileId == record.Id ? ManifoldTheme::Muted : ManifoldTheme::Warning, true);
            int depth = 0;
            for (const Table::MemoryRecord* at = record.Parent; at; at = at->Parent)
                ++depth;
            Property("Parent", record.Parent ? Format("%s (ID %d)", Name(*record.Parent).c_str(), record.Parent->Id) : std::string("(top level)"));
            {
                const std::vector<std::string> path = Table::RecordPath(record);
                const std::size_t same = Table::FindByPath(list, path).size();
                Property("Path", Table::PathText(path) + (same > 1 ? Format("  (%zu records have it)", same) : std::string()), same > 1 ? ManifoldTheme::Warning : ManifoldTheme::Text);
                std::string lua = "manifold.record.find({ ";
                for (std::size_t i = 0; i < path.size(); ++i)
                {
                    std::string quoted = "\"";
                    for (const char c : path[i])
                        quoted += c == '"' || c == '\\' ? std::string("\\") + c : c == '\n' ? std::string("\\n") : std::string(1, c);
                    lua += (i ? ", " : "") + quoted + "\"";
                }
                lua += " })";
                if (ImGui::BeginPopupContextItem("##pathmenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy as Lua"))
                        ImGui::SetClipboardText(lua.c_str());
                    ImGui::EndPopup();
                }
                ItemTooltip("Right-click: Copy as Lua, the manifold.record.find call that finds this record by its path.");
            }
            const auto& siblings = list.SiblingsOf(record);
            Property("Position", Format("%d of %zu, depth %d", list.IndexOf(record) + 1, siblings.size(), depth));
            if (!record.Children.empty())
            {
                std::size_t descendants = 0;
                const std::function<void(const Table::MemoryRecord&)> count = [&](const Table::MemoryRecord& node)
                    {
                        for (const auto& child : node.Children)
                        {
                            ++descendants;
                            count(*child);
                        }
                    };
                count(record);
                Property("Children", Format("%zu (%zu below in all)", record.Children.size(), descendants));
            }
            if (record.Kind == RecordKind::Value)
                Property("Type", TypeOf(record));
            std::string state = record.Active ? (record.Kind == RecordKind::Value ? "frozen" : "on") : "off";
            if (engine.ActivationPending(record.Id))
                state += ", switch queued";
            Property("State", state, record.Active ? ManifoldTheme::Success : ManifoldTheme::Text);
            const std::vector<std::string> options = OptionNames(record.Options);
            if (!options.empty())
                Property("Options", Join(options, ", "));
            const std::vector<std::string> flags = FlagsOf(engine, record);
            if (!flags.empty())
                Property("Flags", Join(flags, ", "));
            if (record.HasColor)
                Property("Color", Hex(record.Color, 6), RecordColor(record.Color), true);
            if (!record.Comment.empty())
                Property("Comment", record.Comment);
            ImGui::EndTable();
        }

        const bool hasAddress = record.Kind == RecordKind::Value || (record.Kind == RecordKind::Group && record.AddressGroupHeader);
        if (hasAddress && Section("Address"))
        {
            if (BeginProperties("##address"))
            {
                Property("Written", record.Address.empty() ? std::string("(empty)") : record.Address, ManifoldTheme::AddressText, true);
                for (std::size_t i = 0; i < record.Offsets.size(); ++i)
                    Property(Format("Offset %zu", i + 1).c_str(), record.Offsets[i].empty() ? std::string("(empty = 0)") : record.Offsets[i], ManifoldTheme::AddressText, true);
                ImGui::EndTable();
            }
            const std::vector<AddressStep> steps = engine.Tables().ExplainAddress(record);
            if (!steps.empty() && ImGui::BeginTable("##chain", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthFixed, Px(64.0f));
                ImGui::TableSetupColumn("Expression", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();
                PushMono();
                for (const AddressStep& step : steps)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    Muted(StepLabel(step.What));
                    ImGui::TableNextColumn();
                    TextFitted(step.Text, ImGui::GetContentRegionAvail().x, ManifoldTheme::AddressText);
                    ImGui::TableNextColumn();
                    if (step.Ok)
                    {
                        TextFitted(Hex(step.Value), ImGui::GetContentRegionAvail().x, ManifoldTheme::Text);
                        const std::string described = engine.Describe(step.Value);
                        if (described != Hex(step.Value))
                            ItemTooltip(described.c_str());
                    }
                    else
                    {
                        ImGui::TextColored(ManifoldTheme::Error, ICON_FA_TIMES_CIRCLE " failed");
                        ErrorTooltip(step.Error);
                    }
                }
                PopMono();
                ImGui::EndTable();
                const AddressStep& last = steps.back();
                ImGui::PushTextWrapPos(0.0f);
                if (!last.Ok)
                    ImGui::TextColored(ManifoldTheme::Error, "%s", last.Error.c_str());
                else
                {
                    const std::string described = engine.Describe(last.Value);
                    ImGui::TextColored(ManifoldTheme::Success, ICON_FA_CHECK_CIRCLE "  %s%s", Hex(last.Value).c_str(), described != Hex(last.Value) ? (" = " + described).c_str() : "");
                }
                ImGui::PopTextWrapPos();
            }
        }

        if (record.Kind == RecordKind::Value && Section("Value") && BeginProperties("##value"))
        {
            Property("Shown", record.ValueText.empty() ? std::string("(not read yet)") : record.ValueText);
            Property("Bytes", record.LastBytes.empty() ? std::string("(none)") : ShortBytes(record.LastBytes, 64), ManifoldTheme::BytesText, true);
            if (record.Active && !record.FrozenBytes.empty())
                Property("Frozen bytes", ShortBytes(record.FrozenBytes, 64), ManifoldTheme::BytesText, true);
            Property("Size", Format("%d bytes", record.Size(engine.Process() ? engine.Process()->PointerSize() : 8)));
            Property("Refreshed", Ago(now, record.RefreshedAt));
            Property("Changed", record.ChangedAt < 0.0 ? std::string("not since loading") : Ago(now, record.ChangedAt));
            ImGui::EndTable();
        }

        if (record.Kind == RecordKind::Script && Section("Script"))
        {
            const std::vector<std::string> lines = SplitLines(record.Script);
            if (BeginProperties("##script"))
            {
                Property("Lines", std::to_string(lines.size()));
                Property("Sections", std::string(Lower(record.Script).find("[enable]") != std::string::npos ? "[ENABLE] " : "") +
                    (Lower(record.Script).find("[disable]") != std::string::npos ? "[DISABLE]" : ""));
                Property("Registered", record.ScriptMemory.Registered.empty() ? std::string("(none)") : Join(record.ScriptMemory.Registered, ", "), ManifoldTheme::CodeLabel, true);
                ImGui::EndTable();
            }
            if (!record.ScriptMemory.Allocations.empty() && ImGui::BeginTable("##allocs", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Allocation", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                ImGui::TableSetupColumn("Global", ImGuiTableColumnFlags_WidthFixed, Px(48.0f));
                ImGui::TableHeadersRow();
                PushMono();
                for (const Asm::AaAllocation& allocation : record.ScriptMemory.Allocations)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    TextFitted(allocation.Name, ImGui::GetContentRegionAvail().x, ManifoldTheme::CodeLabel);
                    ImGui::TableNextColumn();
                    TextFitted(Hex(allocation.Address), ImGui::GetContentRegionAvail().x, ManifoldTheme::AddressText);
                    ImGui::TableNextColumn();
                    Muted(Hex(allocation.Size));
                    ImGui::TableNextColumn();
                    Muted(allocation.Global ? "yes" : "");
                }
                PopMono();
                ImGui::EndTable();
            }
            if (!record.ScriptMemory.Values.empty() && ImGui::TreeNode(Format("Labels and Scan Results (%zu)###values", record.ScriptMemory.Values.size()).c_str()))
            {
                PushMono();
                for (const auto& [name, value] : record.ScriptMemory.Values)
                    ImGui::Text("%s = %s", name.c_str(), Hex(value).c_str());
                PopMono();
                ImGui::TreePop();
            }
        }

        if (Section("Links and References"))
        {
            const std::vector<Table::MemoryRecord*> linking = Table::RecordsLinkingTo(list, record);
            std::vector<const Table::RecordReference*> lookingHere;
            std::vector<const Table::RecordReference*> looksFor;
            for (const Table::RecordReference& reference : m_references)
            {
                if (reference.TargetId == record.Id)
                    lookingHere.push_back(&reference);
                if (reference.FromRecord == record.Id)
                    looksFor.push_back(&reference);
            }
            if (BeginProperties("##links"))
            {
                if (!record.DropDownLink.empty())
                {
                    const std::string target = Table::DropDownLinkTarget(record);
                    const Table::MemoryRecord* linked = list.FindByDescription(target);
                    Property("Dropdown list", linked ? Format("linked to \"%s\" (ID %d, %zu entries)", target.c_str(), linked->Id, linked->DropDown.size())
                                                     : Format("linked to \"%s\", which doesn't exist", target.c_str()),
                        linked ? ManifoldTheme::Text : ManifoldTheme::Error);
                }
                else
                    Property("Dropdown list", record.DropDown.empty() ? std::string("(none)") : Format("%zu entries of its own", record.DropDown.size()));
                const int named = record.Description.empty() ? 0 : list.CountByDescription(record.Description);
                if (named > 1)
                    Property("Same name", Format("%d records are called this; getMemoryRecordByDescription finds the first, manifold.record.find fails (ME-6011)", named), ManifoldTheme::Warning);
                std::vector<std::string> names;
                for (const Table::MemoryRecord* other : linking)
                    names.push_back(Format("%s (ID %d)", Name(*other).c_str(), other->Id));
                Property("Lists linking here", names.empty() ? std::string("(none)") : Join(names, ", "));
                std::vector<std::string> events;
                for (const char* event : kRecordEvents)
                {
                    if (engine.Lua().HasRecordEvent(record.Id, event))
                        events.push_back(event);
                }
                if (!record.LuaOnActivate.empty())
                    events.push_back("On Activate code");
                if (!record.LuaOnDeactivate.empty())
                    events.push_back("On Deactivate code");
                Property("Lua events", events.empty() ? std::string("(none)") : Join(events, ", "));
                ImGui::EndTable();
            }
            const auto referenceList = [&](const char* title, const std::vector<const Table::RecordReference*>& references, bool own)
                {
                    if (references.empty())
                        return;
                    ImGui::Spacing();
                    SectionLabel(title);
                    ImGui::PushTextWrapPos(0.0f);
                    for (const Table::RecordReference* reference : references)
                    {
                        const std::string where = reference->FromRecord == Table::kTableLevel ? reference->Where
                                                  : reference->FromRecord == record.Id ? reference->Where
                                                  : [&]()
                                                    {
                                                        const Table::MemoryRecord* from = list.FindById(reference->FromRecord);
                                                        return (from ? Name(*from) : std::string("?")) + ", " + reference->Where;
                                                    }();
                        const std::string target = reference->ById ? "ID " + reference->Target : "\"" + reference->Target + "\"";
                        const bool missing = reference->TargetId < 0;
                        ImGui::TextColored(missing ? ManifoldTheme::Warning : ManifoldTheme::Text, "%s  %s(%s)  ·  %s, line %d%s", missing ? ICON_FA_UNLINK : ICON_FA_LINK,
                            reference->Function.c_str(), target.c_str(), where.c_str(), reference->Line, own && missing ? "  ·  not in the table" : "");
                    }
                    ImGui::PopTextWrapPos();
                };
            referenceList("Lua that looks this record up", lookingHere, false);
            referenceList("Records its Lua code looks up", looksFor, true);
        }

        if (!record.Hotkeys.empty() && Section("Hotkeys"))
        {
            if (ImGui::BeginTable("##hotkeys", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, Px(34.0f));
                ImGui::TableHeadersRow();
                for (const Table::Hotkey& hotkey : record.Hotkeys)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool held = engine.Tables().HotkeyHeld(record, hotkey);
                    ImGui::TextColored(Table::UsableHotkeyKeys(hotkey.Keys) ? ManifoldTheme::Text : ManifoldTheme::Error, "%s", Table::KeysText(hotkey.Keys).c_str());
                    if (held)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_BAN);
                        ItemTooltip("Held until the table's Lua script is accepted.");
                    }
                    if (!hotkey.Description.empty())
                        ItemTooltip(hotkey.Description.c_str());
                    ImGui::TableNextColumn();
                    Muted(Table::HotkeyActionName(hotkey.Action));
                    ImGui::TableNextColumn();
                    TextFitted(hotkey.Value, ImGui::GetContentRegionAvail().x, ManifoldTheme::Text);
                    ImGui::TableNextColumn();
                    Muted(std::to_string(hotkey.Id));
                }
                ImGui::EndTable();
            }
        }

        std::vector<const ActivationTraceEntry*> history;
        const auto& trace = engine.Activation().Trace();
        for (auto it = trace.rbegin(); it != trace.rend() && history.size() < 8; ++it)
        {
            if (it->RecordId == record.Id)
                history.push_back(&*it);
        }
        if (!history.empty() && Section("Recent Switches"))
        {
            for (const ActivationTraceEntry* entry : history)
            {
                ImGui::TextColored(OutcomeTint(entry->Result), "%s", entry->Active ? ICON_FA_TOGGLE_ON : ICON_FA_TOGGLE_OFF);
                ImGui::SameLine();
                ImGui::Text("%s  %s, %s", entry->Active ? "On" : "Off", OutcomeName(entry->Result), Took(entry->Seconds).c_str());
                ImGui::SameLine();
                Muted(Ago(now, entry->At));
                if (!entry->Error.empty())
                {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ManifoldTheme::Error, "    %s", entry->Error.c_str());
                    ImGui::PopTextWrapPos();
                }
            }
            if (ImGui::SmallButton(ICON_FA_HISTORY "  Show in the Trace"))
            {
                m_traceOnlySelected = true;
                m_requestTab = TraceTab;
            }
        }
    }

    std::string RecordDebugWindow::Report(Table::MemoryRecord& record)
    {
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        std::string text = Format("%s \"%s\" (ID %d)\n", KindName(record.Kind), record.Description.c_str(), record.Id);
        if (record.FileId >= 0 && record.FileId != record.Id)
            text += Format("  ID in the file: %d\n", record.FileId);
        text += "  Parent: " + (record.Parent ? Format("\"%s\" (ID %d)", record.Parent->Description.c_str(), record.Parent->Id) : std::string("top level")) + "\n";
        text += "  Path: " + Table::PathText(Table::RecordPath(record)) + "\n";
        if (record.Kind == RecordKind::Value)
            text += "  Type: " + TypeOf(record) + "\n";
        text += std::string("  State: ") + (record.Active ? "on" : "off") + "\n";
        const std::vector<std::string> flags = FlagsOf(engine, record);
        if (!flags.empty())
            text += "  Flags: " + Join(flags, ", ") + "\n";
        const std::vector<std::string> options = OptionNames(record.Options);
        if (!options.empty())
            text += "  Options: " + Join(options, ", ") + "\n";
        if (record.Kind == RecordKind::Value || record.AddressGroupHeader)
        {
            text += "  Address: " + record.Address + "\n";
            for (std::size_t i = 0; i < record.Offsets.size(); ++i)
                text += Format("  Offset %zu: %s\n", i + 1, record.Offsets[i].c_str());
            for (const AddressStep& step : engine.Tables().ExplainAddress(record))
                text += "    " + StepLabel(step.What) + " " + step.Text + " -> " + (step.Ok ? Hex(step.Value) : "failed: " + step.Error) + "\n";
        }
        if (record.Kind == RecordKind::Value)
        {
            text += "  Value: " + record.ValueText + "\n";
            if (!record.LastBytes.empty())
                text += "  Bytes: " + HexBytes(record.LastBytes) + "\n";
        }
        if (record.Kind == RecordKind::Script)
        {
            for (const Asm::AaAllocation& allocation : record.ScriptMemory.Allocations)
                text += Format("  Allocation %s at %s, size %s\n", allocation.Name.c_str(), Hex(allocation.Address).c_str(), Hex(allocation.Size).c_str());
            if (!record.ScriptMemory.Registered.empty())
                text += "  Registered: " + Join(record.ScriptMemory.Registered, ", ") + "\n";
        }
        if (!record.DropDownLink.empty())
        {
            const Table::MemoryRecord* linked = list.FindByDescription(Table::DropDownLinkTarget(record));
            text += "  Dropdown link: " + Table::DropDownLinkTarget(record) + (linked ? Format(" (ID %d)", linked->Id) : std::string(" (missing)")) + "\n";
        }
        for (const Table::Hotkey& hotkey : record.Hotkeys)
            text += Format("  Hotkey %s: %s %s\n", Table::KeysText(hotkey.Keys).c_str(), Table::HotkeyActionName(hotkey.Action), hotkey.Value.c_str());
        if (!record.LastError.empty())
            text += "  Last error: " + record.LastError + "\n";
        for (const RecordIssue* issue : IssuesOf(record.Id))
            text += std::string("  ") + Table::IssueLevelName(issue->Level) + " (" + issue->Check + "): " + issue->Text + "\n";
        return text;
    }

    // ---------------------------------------------------------------- Table check

    void RecordDebugWindow::StartRuntimeCheck()
    {
        Engine& engine = m_workspace.Core();
        m_runtime.clear();
        IssuesChanged();
        m_checkQueue.clear();
        m_checkNext = 0;
        for (const Table::MemoryRecord* record : engine.Table().Flatten())
        {
            if (record->Kind == RecordKind::Value || record->AddressGroupHeader || (record->Kind == RecordKind::Script && m_checkAssemble))
                m_checkQueue.push_back(record->Id);
        }
        m_checkRunning = true;
        m_checkRan = true;
        m_checkStarted = engine.Now();
        UpdateStaticIssues(true);
    }

    void RecordDebugWindow::StepRuntimeCheck()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess())
        {
            m_checkRunning = false;
            m_runtime.push_back({ Table::kTableLevel, IssueLevel::Info, "check", "The process closed during the check; it stopped there." });
            IssuesChanged();
            return;
        }
        const double deadline = engine.Now() + kCheckBudget;
        do
        {
            if (m_checkNext >= m_checkQueue.size())
            {
                m_checkRunning = false;
                m_checkSeconds = engine.Now() - m_checkStarted;
                return;
            }
            if (Table::MemoryRecord* record = engine.Table().FindById(m_checkQueue[m_checkNext++]))
                CheckRecordAtRuntime(*record);
        } while (engine.Now() < deadline);
    }

    void RecordDebugWindow::CheckRecordAtRuntime(Table::MemoryRecord& record)
    {
        Engine& engine = m_workspace.Core();
        const auto add = [&](IssueLevel level, const char* check, std::string text)
            {
                m_runtime.push_back({ record.Id, level, check, std::move(text) });
                IssuesChanged();
            };
        if (record.Kind == RecordKind::Value || record.AddressGroupHeader)
        {
            if (TrimView(record.Address).empty())
                return;
            const std::vector<AddressStep> steps = engine.Tables().ExplainAddress(record);
            if (steps.empty())
                return;
            const AddressStep& last = steps.back();
            if (!last.Ok)
            {
                std::string where = StepLabel(last.What) + " " + last.Text;
                if (steps.size() > 1)
                    where = Format("step %zu (%s)", steps.size(), where.c_str());
                add(IssueLevel::Error, "address", "The address doesn't resolve at " + where + ": " + last.Error);
                return;
            }
            if (record.Kind == RecordKind::Value && engine.ReadRecord(record).empty())
                add(IssueLevel::Warning, "address", "The address resolves to " + Hex(last.Value) + ", but nothing can be read there.");
            return;
        }
        if (record.Kind != RecordKind::Script || TrimView(record.Script).empty())
            return;
        const std::string lower = Lower(record.Script);
        if (lower.find("[enable]") == std::string::npos && lower.find("[disable]") == std::string::npos)
            return;
        const Asm::AaSection section = record.Active ? Asm::AaSection::Disable : Asm::AaSection::Enable;
        const char* part = record.Active ? "[DISABLE]" : "[ENABLE]";
        Asm::AaMemory memory = record.ScriptMemory;
        // ∑ Lua always held back here, trusted table or not: a check over the whole table must not run its {$lua} blocks and
        //   custom commands (they may write memory, switch records or delete them). The editor's Check keeps running them.
        const ScriptCheck check = engine.CheckScript(record.Script, section, memory, true);
        const std::string heldBack = check.LuaHeldBack ? Format(" %d Lua %s not run: this check runs no Lua.", check.LuaHeldBack, check.LuaHeldBack == 1 ? "part was" : "parts were") : std::string();
        if (!check.Result.Ok())
        {
            const std::string line = check.Result.Line > 0 ? Format(" (line %d)", check.Result.Line) : std::string();
            add(check.LuaHeldBack ? IssueLevel::Info : IssueLevel::Error, "assemble", std::string("The ") + part + " part doesn't assemble" + line + ": " + check.Result.Error.Text + heldBack);
        }
        else if (check.LuaHeldBack)
            add(IssueLevel::Info, "assemble", std::string("The ") + part + " part assembles, checked without its Lua." + heldBack);
    }

    std::string RecordDebugWindow::CheckReport() const
    {
        Table::AddressList& list = m_workspace.Core().Table();
        std::string text;
        for (const std::vector<RecordIssue>* issues : { &m_static, &m_runtime })
        {
            for (const RecordIssue& issue : *issues)
            {
                const Table::MemoryRecord* record = issue.RecordId == Table::kTableLevel ? nullptr : list.FindById(issue.RecordId);
                const std::string who = issue.RecordId == Table::kTableLevel ? std::string("Table") : Format("\"%s\" (ID %d)", record ? record->Description.c_str() : "?", issue.RecordId);
                text += std::string(Table::IssueLevelName(issue.Level)) + "\t" + who + "\t" + issue.Check + "\t" + issue.Text + "\n";
            }
        }
        return text;
    }

    void RecordDebugWindow::CheckPage()
    {
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        UpdateStaticIssues(false);

        ImGui::BeginDisabled(!engine.HasProcess() || m_checkRunning);
        if (AccentButton(ICON_FA_PLAY "  Run Check with the Process"))
            StartRuntimeCheck();
        ImGui::EndDisabled();
        if (!engine.HasProcess())
            ItemTooltip("Open a process to also check that addresses resolve and scripts assemble. The checks of the table itself run all the time.");
        ImGui::SameLine();
        ImGui::Checkbox("Assemble Scripts", &m_checkAssemble);
        ItemTooltip("Assemble each script's [ENABLE] part (or [DISABLE] when it is on) without writing anything, like the script editor's Check. "
                    "Scripts with AOB scans take a moment each.");
        ImGui::SameLine();
        if (m_checkRunning)
        {
            const float done = m_checkQueue.empty() ? 1.0f : static_cast<float>(m_checkNext) / static_cast<float>(m_checkQueue.size());
            ImGui::ProgressBar(done, ImVec2(Px(160.0f), 0.0f), Format("%zu / %zu", m_checkNext, m_checkQueue.size()).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_STOP "  Stop"))
                m_checkRunning = false;
        }
        else if (m_checkRan)
            Muted(Format("Checked with the process in %s", Took(m_checkSeconds).c_str()));
        else
            Muted("Shown: the checks of the table itself");

        ImGui::SetNextItemWidth(Px(240.0f));
        InputTextWithHint("##checkfilter", ICON_FA_SEARCH "  Filter problems", m_checkFilter);
        ImGui::SameLine();
        Segmented("##checklevel", &m_checkLevel, { "All", "Errors", "Warnings", "Notes" }, Px(300.0f));

        std::unordered_map<int, int> order;
        {
            int index = 0;
            for (const Table::MemoryRecord* record : list.Flatten())
                order[record->Id] = index++;
        }
        std::vector<const RecordIssue*> shown;
        int counts[3] = {};
        for (const std::vector<RecordIssue>* issues : { &m_static, &m_runtime })
        {
            for (const RecordIssue& issue : *issues)
            {
                ++counts[static_cast<int>(issue.Level)];
                if (m_checkLevel != 0 && static_cast<int>(issue.Level) != m_checkLevel - 1)
                    continue;
                if (!m_checkFilter.empty())
                {
                    const Table::MemoryRecord* record = list.FindById(issue.RecordId);
                    if (!ContainsNoCase(issue.Text, m_checkFilter) && !ContainsNoCase(issue.Check, m_checkFilter) &&
                        (!record || !ContainsNoCase(record->Description, m_checkFilter)))
                        continue;
                }
                shown.push_back(&issue);
            }
        }
        std::stable_sort(shown.begin(), shown.end(), [&](const RecordIssue* a, const RecordIssue* b)
            {
                const auto rank = [&](int id) { const auto found = order.find(id); return id == Table::kTableLevel ? -1 : found == order.end() ? 1 << 30 : found->second; };
                return rank(a->RecordId) < rank(b->RecordId);
            });
        ImGui::SameLine();
        Muted(Format("%d %s  ·  %d %s  ·  %d %s", counts[0], counts[0] == 1 ? "error" : "errors", counts[1], counts[1] == 1 ? "warning" : "warnings",
            counts[2], counts[2] == 1 ? "note" : "notes"));

        if (counts[0] + counts[1] + counts[2] == 0)
        {
            EmptyState(ICON_FA_CHECK_CIRCLE, "No problems found",
                engine.HasProcess() ? (m_checkRan ? "The table and its records look fine, also with the process." : "The table looks fine. Run the check with the process to also try addresses and scripts.")
                                    : "The table looks fine. With a process open the check also tries addresses and scripts.",
                {}, false);
            return;
        }
        if (!BeginDataTable("##problems", { { "", 0.0f, 26.0f }, { "Record", 1.3f }, { "Check", 0.55f }, { "Problem", 3.6f } }, ImVec2(0.0f, 0.0f), ImGuiTableFlags_SizingStretchProp))
            return;
        ClippedRows(static_cast<int>(shown.size()), [&](int index)
            {
                const RecordIssue& issue = *shown[static_cast<std::size_t>(index)];
                Table::MemoryRecord* record = issue.RecordId == Table::kTableLevel ? nullptr : list.FindById(issue.RecordId);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable("##issue", record && record->Id == m_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap) && record)
                {
                    SelectRecord(record->Id, true);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        m_requestTab = RecordsTab;
                }
                if (ImGui::BeginPopupContextItem("##issuemenu"))
                {
                    if (record && ImGui::MenuItem(ICON_FA_STETHOSCOPE "  Inspect the Record"))
                    {
                        SelectRecord(record->Id, true);
                        m_requestTab = RecordsTab;
                    }
                    if (record && ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Show in the Address List"))
                        m_workspace.RevealRecord(record->Id);
                    if (record && ImGui::MenuItem(ICON_FA_SLIDERS_H "  Edit Record..."))
                        m_workspace.OpenRecordEditor(record->Id);
                    if (record && record->Kind == RecordKind::Script && ImGui::MenuItem(ICON_FA_CODE "  Edit Script"))
                        m_workspace.OpenScript(record->Script, record->Id);
                    ImGui::Separator();
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Problem"))
                        ImGui::SetClipboardText(issue.Text.c_str());
                    ImGui::EndPopup();
                }
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(LevelTint(issue.Level), "%s", LevelGlyph(issue.Level));
                ImGui::TableNextColumn();
                if (record)
                    TextFitted(Format("%d  %s", record->Id, Name(*record).c_str()), ImGui::GetContentRegionAvail().x, ManifoldTheme::Text);
                else
                    TextFitted(issue.RecordId == Table::kTableLevel ? "(the table)" : "(removed record)", ImGui::GetContentRegionAvail().x, ManifoldTheme::Label);
                ImGui::TableNextColumn();
                Muted(issue.Check);
                ImGui::TableNextColumn();
                TextFitted(issue.Text, ImGui::GetContentRegionAvail().x, issue.Level == IssueLevel::Info ? ManifoldTheme::Text : LevelTint(issue.Level));
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(issue.Text.c_str()).x > ImGui::GetColumnWidth())
                    ShowTooltip([&]() { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f); ImGui::TextUnformatted(issue.Text.c_str()); ImGui::PopTextWrapPos(); });
            });
        ImGui::EndTable();
    }

    // ---------------------------------------------------------------- Activation trace

    std::string RecordDebugWindow::TraceReport(const std::vector<const ActivationTraceEntry*>& entries) const
    {
        std::string text;
        for (const ActivationTraceEntry* entry : entries)
        {
            text += Format("#%llu\t\"%s\" (ID %d)\t%s\t%s\t%s\twaited %s\t%zu writes, %zu bytes", static_cast<unsigned long long>(entry->Serial), entry->Description.c_str(), entry->RecordId,
                entry->Active ? "on" : "off", OutcomeName(entry->Result), Took(entry->Seconds).c_str(), Took(entry->Waited).c_str(), entry->WriteCount, entry->BytesWritten);
            if (!entry->Error.empty())
                text += "\t" + entry->Error + (entry->ErrorLine > 0 ? Format(" (line %d)", entry->ErrorLine) : std::string());
            text += "\n";
            for (std::size_t i = 0; i < entry->Writes.size(); ++i)
            {
                const Asm::AaWrite& write = entry->Writes[i];
                text += Format("    write %s: %s (line %d)\n", Hex(write.Address).c_str(), TracedBytes(*entry, i, 32).c_str(), write.Line);
            }
            for (const Asm::AaAllocation& allocation : entry->Allocated)
                text += Format("    alloc %s at %s, size %s\n", allocation.Name.c_str(), Hex(allocation.Address).c_str(), Hex(allocation.Size).c_str());
            for (const Asm::AaAllocation& allocation : entry->Released)
                text += Format("    dealloc %s at %s\n", allocation.Name.c_str(), Hex(allocation.Address).c_str());
            if (!entry->Registered.empty())
                text += "    registered " + Join(entry->Registered, ", ") + "\n";
            if (!entry->Unregistered.empty())
                text += "    unregistered " + Join(entry->Unregistered, ", ") + "\n";
        }
        return text;
    }

    void RecordDebugWindow::TracePage()
    {
        Engine& engine = m_workspace.Core();
        const auto& trace = engine.Activation().Trace();
        ImGui::SetNextItemWidth(Px(240.0f));
        InputTextWithHint("##tracefilter", ICON_FA_SEARCH "  Filter by record", m_traceFilter);
        ImGui::SameLine();
        ImGui::Checkbox("Only the Selected Record", &m_traceOnlySelected);
        ImGui::SameLine();
        ImGui::Checkbox("Only Failures", &m_traceOnlyFailures);

        std::vector<const ActivationTraceEntry*> entries;
        for (auto it = trace.rbegin(); it != trace.rend(); ++it)
        {
            if (m_traceOnlySelected && it->RecordId != m_selected)
                continue;
            if (m_traceOnlyFailures && it->Result == ActivationTraceEntry::Outcome::Done)
                continue;
            if (!m_traceFilter.empty() && !ContainsNoCase(it->Description, m_traceFilter) && std::to_string(it->RecordId) != m_traceFilter)
                continue;
            entries.push_back(&*it);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_COPY "  Copy"))
            ImGui::SetClipboardText(TraceReport(entries).c_str());
        ImGui::SameLine();
        Muted(entries.size() == trace.size() ? Format("%zu switches (the last %zu are kept)", trace.size(), ActivationRunner::kTraceLength)
                                             : Format("%zu of %zu switches", entries.size(), trace.size()));

        if (trace.empty())
        {
            EmptyState(ICON_FA_HISTORY, "Nothing switched yet",
                "Every time a record is switched on or off (by hand, by a hotkey, by Lua or with its group) it shows up here: how long it took, what the script wrote and allocated, and why it failed.",
                {}, false);
            return;
        }
        const ActivationTraceEntry* selected = nullptr;
        for (const ActivationTraceEntry* entry : entries)
        {
            if (entry->Serial == m_traceSelected)
                selected = entry;
        }
        const float gap = Px(6.0f);
        const float available = ImGui::GetContentRegionAvail().y;
        if (!selected)
        {
            TraceTable(available, entries);
            return;
        }
        const float minTable = Px(90.0f);
        const float minDetails = Px(90.0f);
        m_traceDetailHeight = std::clamp(m_traceDetailHeight, minDetails, std::max(minDetails, available - minTable - gap));
        float top = available - m_traceDetailHeight - gap;
        float bottom = m_traceDetailHeight;
        const ImVec2 start = ImGui::GetCursorPos();
        TraceTable(top, entries);
        ImGui::SetCursorPos(start);
        if (Splitter("##tracesplit", false, gap, &top, &bottom, minTable, minDetails))
            m_traceDetailHeight = bottom;
        ImGui::SetCursorPos(ImVec2(start.x, start.y + top + gap));
        ImGui::BeginChild("##tracedetails", ImVec2(0.0f, m_traceDetailHeight), ImGuiChildFlags_Borders);
        TraceDetails(*selected);
        ImGui::EndChild();
    }

    void RecordDebugWindow::TraceTable(float height, const std::vector<const ActivationTraceEntry*>& entries)
    {
        Engine& engine = m_workspace.Core();
        const double now = engine.Now();
        Table::AddressList& list = engine.Table();
        if (!BeginDataTable("##trace", { { "When", 0.7f }, { "Record", 1.8f }, { "Switch", 0.4f }, { "Result", 0.8f }, { "Took", 0.5f }, { "Waited", 0.5f },
                { "Writes", 0.6f }, { "Started By", 1.0f } }, ImVec2(0.0f, height), ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingStretchProp))
            return;
        ClippedRows(static_cast<int>(entries.size()), [&](int index)
            {
                const ActivationTraceEntry& entry = *entries[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable("##traceentry", entry.Serial == m_traceSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    m_traceSelected = entry.Serial == m_traceSelected ? 0 : entry.Serial;
                    if (list.FindById(entry.RecordId))
                        m_selected = entry.RecordId;
                }
                ImGui::SameLine(0.0f, 0.0f);
                Muted(Ago(now, entry.At));
                ImGui::TableNextColumn();
                TextFitted(Format("%d  %s", entry.RecordId, entry.Description.empty() ? "(no description)" : entry.Description.c_str()), ImGui::GetContentRegionAvail().x,
                    list.FindById(entry.RecordId) ? ManifoldTheme::Text : ManifoldTheme::Muted);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.Active ? "On" : "Off");
                ImGui::TableNextColumn();
                ImGui::TextColored(OutcomeTint(entry.Result), "%s", OutcomeName(entry.Result));
                if (!entry.Error.empty())
                    ErrorTooltip(entry.Error);
                ImGui::TableNextColumn();
                Muted(Took(entry.Seconds));
                if (entry.Async)
                    ItemTooltip("Ran on the activation thread (asynchronous mode).");
                ImGui::TableNextColumn();
                Muted(entry.Async ? Took(entry.Waited) : "-");
                ImGui::TableNextColumn();
                if (entry.Kind == RecordKind::Script)
                    Muted(entry.WriteCount ? Format("%zu · %zu B", entry.WriteCount, entry.BytesWritten) : std::string("none"));
                ImGui::TableNextColumn();
                std::string by = entry.FromScript ? "Lua" : "hand or hotkey";
                if (entry.ViaParent >= 0)
                {
                    const Table::MemoryRecord* parent = list.FindById(entry.ViaParent);
                    by = "with " + (parent ? Name(*parent) : Format("ID %d", entry.ViaParent));
                }
                Muted(by);
            });
        ImGui::EndTable();
    }

    void RecordDebugWindow::TraceDetails(const ActivationTraceEntry& entry)
    {
        Engine& engine = m_workspace.Core();
        ImGui::Text("%s  \"%s\" (ID %d) switched %s", entry.Active ? ICON_FA_TOGGLE_ON : ICON_FA_TOGGLE_OFF, entry.Description.c_str(), entry.RecordId, entry.Active ? "on" : "off");
        ImGui::SameLine();
        ImGui::TextColored(OutcomeTint(entry.Result), "%s", OutcomeName(entry.Result));
        ImGui::SameLine();
        Muted(Format("#%llu  ·  took %s%s", static_cast<unsigned long long>(entry.Serial), Took(entry.Seconds).c_str(),
            entry.Async ? (", waited " + Took(entry.Waited)).c_str() : ", synchronous"));
        if (engine.Table().FindById(entry.RecordId))
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_STETHOSCOPE "  Inspect"))
            {
                SelectRecord(entry.RecordId, true);
                m_requestTab = RecordsTab;
            }
        }
        if (!entry.Error.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ManifoldTheme::Error, "%s%s", entry.Error.c_str(), entry.ErrorLine > 0 ? Format("  (line %d)", entry.ErrorLine).c_str() : "");
            ImGui::PopTextWrapPos();
        }
        if (entry.Result == ActivationTraceEntry::Outcome::Vetoed)
            Muted("onMemRecPreExecute or the record's OnActivate event returned false, so nothing ran.");
        if (!entry.Writes.empty() && ImGui::BeginTable("##writes", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, Px(44.0f));
            ImGui::TableHeadersRow();
            PushMono();
            for (std::size_t i = 0; i < entry.Writes.size(); ++i)
            {
                const Asm::AaWrite& write = entry.Writes[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                TextFitted(engine.Describe(write.Address), ImGui::GetContentRegionAvail().x, ManifoldTheme::AddressText);
                ItemTooltip(Hex(write.Address).c_str());
                ImGui::TableNextColumn();
                Muted(std::to_string(TracedWriteSize(entry, i)));
                ImGui::TableNextColumn();
                TextFitted(TracedBytes(entry, i, 24), ImGui::GetContentRegionAvail().x, ManifoldTheme::BytesText);
                ImGui::TableNextColumn();
                Muted(write.Line > 0 ? std::to_string(write.Line) : std::string());
            }
            PopMono();
            ImGui::EndTable();
            if (entry.WriteCount > entry.Writes.size())
                Muted(Format("and %zu more writes", entry.WriteCount - entry.Writes.size()));
        }
        if (!(entry.Allocated.empty() && entry.Released.empty() && entry.Registered.empty() && entry.Unregistered.empty() && entry.Notes.empty()) && BeginProperties("##traceprops"))
        {
            const auto allocations = [](const std::vector<Asm::AaAllocation>& list)
                {
                    std::vector<std::string> parts;
                    for (const Asm::AaAllocation& allocation : list)
                        parts.push_back(allocation.Name + " " + Hex(allocation.Address) + " (" + Hex(allocation.Size) + ")");
                    return Join(parts, ", ");
                };
            if (!entry.Allocated.empty())
                Property("Allocated", allocations(entry.Allocated), ManifoldTheme::Text, true);
            if (!entry.Released.empty())
                Property("Released", allocations(entry.Released), ManifoldTheme::Text, true);
            if (!entry.Registered.empty())
                Property("Registered", Join(entry.Registered, ", "), ManifoldTheme::CodeLabel, true);
            if (!entry.Unregistered.empty())
                Property("Unregistered", Join(entry.Unregistered, ", "), ManifoldTheme::CodeLabel, true);
            for (const std::string& note : entry.Notes)
                Property("Note", note);
            ImGui::EndTable();
        }
    }

    // ---------------------------------------------------------------- Internals

    void RecordDebugWindow::InternalsPage()
    {
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        TableRuntime& tables = engine.Tables();
        ActivationRunner& activation = engine.Activation();
        const std::vector<Table::MemoryRecord*> records = list.Flatten();
        int values = 0, scripts = 0, groups = 0, active = 0, frozen = 0, failed = 0, unresolved = 0, maxDepth = 0, highest = -1, renumbered = 0;
        for (const Table::MemoryRecord* record : records)
        {
            values += record->Kind == RecordKind::Value;
            scripts += record->Kind == RecordKind::Script;
            groups += record->Kind == RecordKind::Group;
            active += record->Active;
            frozen += record->Active && record->Kind == RecordKind::Value;
            failed += !record->LastError.empty();
            unresolved += record->Kind == RecordKind::Value && !record->AddressOk && !record->AddressError.empty();
            highest = std::max(highest, record->Id);
            renumbered += record->FileId >= 0 && record->FileId != record->Id;
            int depth = 0;
            for (const Table::MemoryRecord* at = record->Parent; at; at = at->Parent)
                ++depth;
            maxDepth = std::max(maxDepth, depth);
        }
        ImGui::BeginChild("##internals");
        if (Section("Table") && BeginProperties("##tableprops"))
        {
            Property("File", list.FilePath.empty() ? std::string("(not saved)") : list.FilePath, ManifoldTheme::Text, true);
            Property("Records", Format("%zu: %d values, %d scripts, %d groups", records.size(), values, scripts, groups));
            Property("Highest ID", highest < 0 ? std::string("-") : std::to_string(highest), ManifoldTheme::Text, true);
            Property("Renumbered", std::to_string(renumbered), renumbered ? ManifoldTheme::Warning : ManifoldTheme::Text);
            Property("Deepest nesting", std::to_string(maxDepth));
            Property("Active", Format("%d (%d values frozen)", active, frozen));
            Property("Failed last switch", std::to_string(failed), failed ? ManifoldTheme::Error : ManifoldTheme::Text);
            Property("Unresolved", std::to_string(unresolved), unresolved ? ManifoldTheme::Warning : ManifoldTheme::Text);
            Property("Selected", Format("%zu", list.Selection().size()));
            Property("Generation", std::to_string(list.Generation()), ManifoldTheme::Text, true);
            Property("Identity", std::to_string(list.Identity()), ManifoldTheme::Text, true);
            Property("Unsaved changes", list.Dirty() ? "yes" : "no");
            Property("Table files", std::to_string(list.Files.size()));
            Property("Lua script", list.TableLuaScript.empty() ? std::string("(none)") : Format("%zu lines", SplitLines(list.TableLuaScript).size()));
            Property("Scripts trusted", tables.ScriptsTrusted() ? "yes" : "no, hotkeys and Lua are held");
            Property("Table script waiting", tables.TableScriptWaiting() ? "yes, for the prompt" : "no");
            ImGui::EndTable();
        }
        if (Section("Refresh") && BeginProperties("##refreshprops"))
        {
            Property("Interval", Format("%d ms", engine.Options().UpdateIntervalMs));
            Property("Last refresh", Ago(engine.Now(), tables.LastValueRefresh() < 0.0 ? -1.0 : tables.LastValueRefresh()));
            Property("Refreshed records", tables.VisibleKnown() ? Format("%zu drawn by the address list (plus the ones Lua watches with onValueChanged)", tables.VisibleRecords().size())
                                                                 : std::string("all values (the address list hasn't drawn yet)"));
            Property("Process", engine.HasProcess() ? Format("%s-bit", engine.Process() && engine.Process()->Is64Bit() ? "64" : "32") : std::string("none"));
            ImGui::EndTable();
        }
        if (Section("Activation"))
        {
            const ActivationProgress progress = activation.Progress();
            if (BeginProperties("##activationprops"))
            {
                Property("Mode", activation.Async() ? "asynchronous (one script at a time on a worker thread)" : "synchronous (on the UI thread)");
                Property("Working", activation.Working() ? "yes" : "no");
                Property("Progress", progress.Total ? Format("%d of %d%s", progress.Done, progress.Total, progress.Current.empty() ? "" : (", now " + progress.Current).c_str()) : std::string("idle"));
                Property("Table Lua script", progress.TableScript ? Format("running for %.1f s", progress.TableScriptSeconds) : std::string("not running"));
                Property("Trace", Format("%zu of %zu kept, %llu in all", activation.Trace().size(), ActivationRunner::kTraceLength, static_cast<unsigned long long>(activation.TraceSerial())));
                ImGui::EndTable();
            }
            const std::vector<QueuedActivation> queue = activation.Queue();
            if (queue.empty())
                Muted("The queue is empty.");
            else if (ImGui::BeginTable("##queue", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Record", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                ImGui::TableSetupColumn("Waiting", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableHeadersRow();
                for (const QueuedActivation& job : queue)
                {
                    const Table::MemoryRecord* record = list.FindById(job.RecordId);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    TextFitted(Format("%d  %s", job.RecordId, record ? Name(*record).c_str() : "(removed)"), ImGui::GetContentRegionAvail().x, ManifoldTheme::Text);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(job.Active ? "On" : "Off");
                    ImGui::TableNextColumn();
                    ImGui::TextColored(job.Running ? ManifoldTheme::Accent : ManifoldTheme::Muted, "%s", job.Running ? "running" : job.FromScript ? "queued by Lua" : "queued");
                    ImGui::TableNextColumn();
                    Muted(job.QueuedAt > 0.0 ? Took(engine.Now() - job.QueuedAt) : std::string("-"));
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }
}
