#include "UI/Windows.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>

namespace Manifold::UI
{
    namespace
    {
        std::string Shown(std::string_view text, std::size_t limit)
        {
            std::string out;
            for (char c : text)
            {
                if (out.size() >= limit)
                {
                    out += " ...";
                    break;
                }
                if (c == '\n')
                    out += "\\n";
                else if (c == '\r')
                    out += "\\r";
                else if (c == '\t')
                    out += "\\t";
                else
                    out += c;
            }
            return out;
        }

        std::string Where(Engine& engine, std::uint64_t address)
        {
            if (engine.HasProcess())
            {
                if (const auto module = engine.Process()->ModuleAt(address))
                    return module->Name + "+" + Hex(address - module->Base);
            }
            return Hex(address);
        }
    }

    void ModuleScopeCombo(const char* id, Engine& engine, ModuleScope& scope, float width)
    {
        ImGui::SetNextItemWidth(width);
        const std::string preview = ScopeName(engine, scope);
        if (!ImGui::BeginCombo(id, preview.c_str()))
            return;
        const auto main = engine.HasProcess() ? engine.Process()->MainModule() : std::nullopt;
        if (ImGui::Selectable(main ? ("Main module  ·  " + main->Name).c_str() : "Main module", scope.Kind == 0))
            scope = { 0, {} };
        if (ImGui::Selectable("Program modules (no Windows libraries)", scope.Kind == 1))
            scope = { 1, {} };
        if (ImGui::Selectable("All modules", scope.Kind == 2))
            scope = { 2, {} };
        if (engine.HasProcess())
        {
            if (const Target::ModuleList list = engine.Process()->Modules())
            {
                ImGui::Separator();
                for (const Target::ModuleInfo& module : *list)
                {
                    ImGui::PushID(module.Name.c_str());
                    if (ImGui::Selectable(module.Name.c_str(), scope.Kind == 3 && EqualsNoCase(scope.Module, module.Name)))
                        scope = { 3, module.Name };
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - Px(60.0f));
                    Muted(ByteSize(module.Size));
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndCombo();
    }

    std::vector<Target::ModuleInfo> ScopeModules(Engine& engine, const ModuleScope& scope)
    {
        std::vector<Target::ModuleInfo> modules;
        if (!engine.HasProcess())
            return modules;
        if (scope.Kind == 0)
        {
            if (const auto main = engine.Process()->MainModule())
                modules.push_back(*main);
            return modules;
        }
        if (const Target::ModuleList list = engine.Process()->Modules())
        {
            for (const Target::ModuleInfo& module : *list)
            {
                if (scope.Kind == 2 || (scope.Kind == 1 && !module.System) || (scope.Kind == 3 && EqualsNoCase(scope.Module, module.Name)))
                    modules.push_back(module);
            }
        }
        return modules;
    }

    std::string ScopeName(Engine& engine, const ModuleScope& scope)
    {
        switch (scope.Kind)
        {
        case 0:
        {
            const auto main = engine.HasProcess() ? engine.Process()->MainModule() : std::nullopt;
            return main ? "Main module  ·  " + main->Name : "Main module";
        }
        case 1: return "Program modules";
        case 2: return "All modules";
        default: return scope.Module;
        }
    }

    CodeDissectWindow::CodeDissectWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    CodeDissectWindow::~CodeDissectWindow()
    {
        if (m_task.valid())
        {
            m_progress->Cancel = true;
            m_task.wait();
        }
    }

    void CodeDissectWindow::Show(int tab, std::uint64_t address)
    {
        m_open = true;
        m_bringToFront = true;
        if (tab >= 0)
            m_wantTab = tab;
        if (address)
        {
            m_selected = address;
            m_referenceText = m_workspace.Core().Symbols().ModuleOffset(address);
        }
    }

    void CodeDissectWindow::Start()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.valid())
            return;
        engine.Process()->RefreshModules();
        std::vector<Target::ModuleInfo> modules = ScopeModules(engine, m_scope);
        if (modules.empty())
        {
            engine.Notify(LogLevel::Warning, "No module matches the chosen scope.");
            return;
        }
        m_progress = std::make_unique<Disasm::DissectProgress>();
        m_generation = engine.ProcessGeneration();
        Disasm::DissectProgress* progress = m_progress.get();
        std::shared_ptr<Target::Process> process = engine.Process();
        m_task = std::async(std::launch::async, [process, modules = std::move(modules), progress]()
            {
                return Disasm::DissectCode(*process, modules, progress);
            });
    }

    void CodeDissectWindow::Collect()
    {
        if (!m_task.valid() || m_task.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        std::shared_ptr<Disasm::DissectResult> result = m_task.get();
        Engine& engine = m_workspace.Core();
        if (result->Cancelled)
        {
            engine.Notify(LogLevel::Info, "Code dissect cancelled");
            return;
        }
        if (engine.ProcessGeneration() != m_generation)
            return;
        LOG_INFO("Dissect", Grouped(result->Stats.Instructions) + " instructions, " + Grouped(result->Refs.size()) + " references, " +
                            std::to_string(result->Strings.size()) + " strings in " + Format("%.2f s", result->Stats.Seconds));
        engine.SetCodeDissect(std::move(result));
    }

    void CodeDissectWindow::Toolbar()
    {
        Engine& engine = m_workspace.Core();
        const bool running = m_task.valid();
        ImGui::BeginDisabled(running || !engine.HasProcess());
        ModuleScopeCombo("##dissectscope", engine, m_scope, Px(280.0f));
        ImGui::SameLine();
        if (AccentButton(ICON_FA_PLAY "  Dissect"))
            Start();
        ImGui::EndDisabled();
        ItemTooltip("Reads the code of the chosen modules and records every call, jump and data reference");
        ImGui::SameLine(0.0f, Px(12.0f));
        if (running)
        {
            Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
            ImGui::SameLine(0.0f, Px(6.0f));
            ImGui::AlignTextToFramePadding();
            const std::uint64_t total = std::max<std::uint64_t>(1, m_progress->Total.load());
            const int phase = m_progress->Phase.load();
            Muted(phase == 0 ? Format("Reading code  %d%%", static_cast<int>(m_progress->Done.load() * 100 / total)) : phase == 1 ? std::string("Sorting references") : std::string("Looking for strings"));
            ImGui::SameLine(0.0f, Px(8.0f));
            if (ImGui::Button("Cancel"))
                m_progress->Cancel = true;
            return;
        }
        const auto result = engine.CodeDissect();
        if (!result)
            return;
        const Disasm::DissectStats& stats = result->Stats;
        const std::string scope = result->Modules.size() == 1 ? result->Modules.front().Name : std::to_string(result->Modules.size()) + " modules";
        ImGui::AlignTextToFramePadding();
        Muted(scope + "  ·  " + Grouped(stats.Instructions) + " instructions  ·  " + Grouped(stats.Calls) + " calls  ·  " + Grouped(stats.Jumps + stats.Conditionals) + " jumps  ·  " +
              Grouped(stats.Reads + stats.Writes + stats.Addresses) + " data references  ·  " + Format("%.2f s", stats.Seconds));
    }

    void CodeDissectWindow::ReferenceList(const Disasm::DissectResult& result)
    {
        Engine& engine = m_workspace.Core();
        if (!m_selected)
        {
            Muted("Select an entry to see the code that uses it.");
            return;
        }
        const std::span<const Disasm::CodeRef> refs = result.To(m_selected);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_LINK);
        ImGui::SameLine(0.0f, Px(6.0f));
        PushMono();
        ImGui::TextColored(ManifoldTheme::AddressText, "%s", Where(engine, m_selected).c_str());
        PopMono();
        ImGui::SameLine(0.0f, Px(8.0f));
        Muted(refs.empty() ? std::string("no code uses this address") : std::to_string(refs.size()) + (refs.size() == 1 ? " reference" : " references"));
        if (refs.empty())
            return;
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##refs", 3, flags))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(refs.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const Disasm::CodeRef& ref = refs[static_cast<std::size_t>(row)];
                const std::uint64_t from = ref.From();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                PushMono();
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
                if (ImGui::Selectable(Where(engine, from).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    m_workspace.OpenMemoryViewer(from);
                ImGui::PopStyleColor();
                PopMono();
                if (ImGui::BeginPopupContextItem("##refmenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_CODE "  Show in disassembler"))
                        m_workspace.OpenMemoryViewer(from);
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy address"))
                        ImGui::SetClipboardText(engine.Symbols().ModuleOffset(from).c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                Muted(Disasm::RefKindName(ref.Kind()));
                ImGui::TableNextColumn();
                PushMono();
                if (engine.HasProcess())
                    ImGui::TextUnformatted(engine.Disassembler().Read(*engine.Process(), from, &engine.Symbols()).Text().c_str());
                PopMono();
            }
        }
        ImGui::EndTable();
    }

    void CodeDissectWindow::StringsTab(const Disasm::DissectResult& result, float height)
    {
        Engine& engine = m_workspace.Core();
        ImGui::SetNextItemWidth(Px(260.0f));
        InputTextWithHint("##stringfilter", ICON_FA_SEARCH " Text", m_filter);
        const std::string key = "s:" + m_filter;
        if (m_visibleFor != &result || m_visibleFilter != key)
        {
            m_visible.clear();
            for (std::size_t i = 0; i < result.Strings.size(); ++i)
            {
                if (m_filter.empty() || ContainsNoCase(result.Strings[i].Text, m_filter))
                    m_visible.push_back(i);
            }
            m_visibleFor = &result;
            m_visibleFilter = key;
        }
        if (result.Strings.empty())
        {
            Muted("No referenced strings were found in this code.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##strings", 3, flags, ImVec2(0.0f, height)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, Px(200.0f));
            ImGui::TableSetupColumn("Refs", ImGuiTableColumnFlags_WidthFixed, Px(50.0f));
            ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_visible.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const Disasm::ReferencedString& entry = result.Strings[m_visible[static_cast<std::size_t>(row)]];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(row);
                    PushMono();
                    ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
                    if (ImGui::Selectable(Where(engine, entry.Address).c_str(), m_selected == entry.Address, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        m_selected = entry.Address;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            m_workspace.OpenMemoryViewer(entry.Address, true);
                    }
                    ImGui::PopStyleColor();
                    PopMono();
                    if (ImGui::BeginPopupContextItem("##stringmenu"))
                    {
                        if (ImGui::MenuItem(ICON_FA_TH "  Show in hex view"))
                            m_workspace.OpenMemoryViewer(entry.Address, true);
                        if (ImGui::MenuItem(ICON_FA_COPY "  Copy text"))
                            ImGui::SetClipboardText(entry.Text.c_str());
                        if (ImGui::MenuItem(ICON_FA_COPY "  Copy address"))
                            ImGui::SetClipboardText(engine.Symbols().ModuleOffset(entry.Address).c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    Muted(std::to_string(entry.References));
                    ImGui::TableNextColumn();
                    if (entry.Wide)
                    {
                        ImGui::TextColored(ManifoldTheme::Muted, "L");
                        ImGui::SameLine(0.0f, Px(4.0f));
                    }
                    ImGui::TextUnformatted(Shown(entry.Text, 300).c_str());
                }
            }
            ImGui::EndTable();
        }
    }

    void CodeDissectWindow::FunctionsTab(const Disasm::DissectResult& result, float height)
    {
        Engine& engine = m_workspace.Core();
        if (m_sorted.get() != &result)
        {
            m_byCalls.resize(result.Functions.size());
            for (std::size_t i = 0; i < m_byCalls.size(); ++i)
                m_byCalls[i] = i;
            std::stable_sort(m_byCalls.begin(), m_byCalls.end(), [&](std::size_t a, std::size_t b) { return result.Functions[a].Calls > result.Functions[b].Calls; });
            m_sorted = engine.CodeDissect();
        }
        ImGui::SetNextItemWidth(Px(260.0f));
        InputTextWithHint("##functionfilter", ICON_FA_SEARCH " Name or address", m_filter);
        const std::string key = "f:" + m_filter;
        if (m_visibleFor != &result || m_visibleFilter != key)
        {
            m_visible.clear();
            for (std::size_t index : m_byCalls)
            {
                const std::uint64_t address = result.Functions[index].Address;
                if (m_filter.empty() || ContainsNoCase(Hex(address), m_filter) || ContainsNoCase(engine.Describe(address), m_filter))
                    m_visible.push_back(index);
            }
            m_visibleFor = &result;
            m_visibleFilter = key;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##functions", 2, flags, ImVec2(0.0f, height)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, Px(60.0f));
            ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_visible.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const Disasm::ReferencedFunction& entry = result.Functions[m_visible[static_cast<std::size_t>(row)]];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(row);
                    if (ImGui::Selectable(std::to_string(entry.Calls).c_str(), m_selected == entry.Address, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        m_selected = entry.Address;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            m_workspace.OpenMemoryViewer(entry.Address);
                    }
                    if (ImGui::BeginPopupContextItem("##functionmenu"))
                    {
                        if (ImGui::MenuItem(ICON_FA_CODE "  Show in disassembler"))
                            m_workspace.OpenMemoryViewer(entry.Address);
                        if (ImGui::MenuItem(ICON_FA_COPY "  Copy address"))
                            ImGui::SetClipboardText(engine.Symbols().ModuleOffset(entry.Address).c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    PushMono();
                    ImGui::TextColored(ManifoldTheme::AddressText, "%s", engine.Describe(entry.Address).c_str());
                    PopMono();
                }
            }
            ImGui::EndTable();
        }
    }

    void CodeDissectWindow::ReferencesTab(const Disasm::DissectResult& result, float height)
    {
        Engine& engine = m_workspace.Core();
        (void)result;
        (void)height;
        ImGui::SetNextItemWidth(Px(320.0f));
        const bool enter = InputTextWithHint("##refaddress", "Address, e.g. game.exe+1234 or a symbol", m_referenceText, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SEARCH "  Show") || enter)
        {
            Status error;
            if (const auto address = engine.Evaluate(m_referenceText, &error))
                m_selected = *address;
            else
                m_workspace.Report(error);
        }
    }

    void CodeDissectWindow::Draw()
    {
        Collect();
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_PROJECT_DIAGRAM "  Code dissect###codedissect", &m_open, ImVec2(940.0f, 640.0f), 0, ImVec2(640.0f, 400.0f)))
        {
            EndToolWindow();
            return;
        }
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() && !m_task.valid())
        {
            m_workspace.NoProcessState(ICON_FA_PROJECT_DIAGRAM, "Open a process to dissect its code.");
            EndToolWindow();
            return;
        }
        Toolbar();
        ImGui::Spacing();
        const auto result = engine.CodeDissect();
        if (!result)
        {
            if (!m_task.valid() && EmptyState(ICON_FA_PROJECT_DIAGRAM, "Dissect code",
                    "Reads the code of the chosen modules and records every call, jump and data reference. Referenced strings, called functions and who uses an address all come from it.",
                    { { ICON_FA_PLAY "  Dissect", nullptr, true } }, false) == 0)
                Start();
            EndToolWindow();
            return;
        }
        if (ImGui::BeginTabBar("##dissecttabs"))
        {
            const auto tab = [&](const char* label, int index)
                {
                    const ImGuiTabItemFlags flags = m_wantTab == index ? ImGuiTabItemFlags_SetSelected : 0;
                    const bool open = ImGui::BeginTabItem(label, nullptr, flags);
                    if (open && m_tab != index)
                    {
                        m_tab = index;
                        m_filter.clear();
                    }
                    return open;
                };
            const float bottom = ImGui::GetContentRegionAvail().y;
            const float listHeight = std::floor(bottom * 0.56f);
            const std::string stringsLabel = "Strings  " + Grouped(result->Strings.size()) + "###strings";
            const std::string functionsLabel = "Functions  " + Grouped(result->Functions.size()) + "###functions";
            if (tab(stringsLabel.c_str(), Strings))
            {
                StringsTab(*result, listHeight);
                ImGui::Spacing();
                ReferenceList(*result);
                ImGui::EndTabItem();
            }
            if (tab(functionsLabel.c_str(), Functions))
            {
                FunctionsTab(*result, listHeight);
                ImGui::Spacing();
                ReferenceList(*result);
                ImGui::EndTabItem();
            }
            if (tab("References###references", References))
            {
                ReferencesTab(*result, listHeight);
                ImGui::Spacing();
                ReferenceList(*result);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        m_wantTab = -1;
        EndToolWindow();
    }

    AsmSearchWindow::AsmSearchWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    AsmSearchWindow::~AsmSearchWindow()
    {
        if (m_task.valid())
        {
            m_progress->Cancel = true;
            m_task.wait();
        }
    }

    void AsmSearchWindow::Start()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.valid())
            return;
        const auto pattern = Disasm::ParseAsmPattern(m_pattern);
        if (!pattern)
        {
            engine.Notify(LogLevel::Warning, "Enter an instruction to search for.");
            return;
        }
        engine.Process()->RefreshModules();
        const std::vector<Target::ModuleInfo> modules = ScopeModules(engine, m_scope);
        std::vector<Disasm::CodeSpan> spans = Disasm::ExecutableSpans(*engine.Process(), modules);
        m_process = engine.Process();
        m_progress = std::make_unique<Disasm::AsmSearchProgress>();
        m_result.reset();
        m_searched = ScopeName(engine, m_scope);
        Disasm::AsmSearchProgress* progress = m_progress.get();
        std::shared_ptr<Target::Process> process = m_process;
        const Disasm::Disassembler* disassembler = &engine.Disassembler();
        const Target::Symbols* symbols = &engine.Symbols();
        m_task = std::async(std::launch::async, [process, disassembler, symbols, spans = std::move(spans), pattern = *pattern, progress]()
            {
                return Disasm::SearchAssembly(*process, *disassembler, symbols, spans, pattern, progress);
            });
    }

    void AsmSearchWindow::Collect()
    {
        if (!m_task.valid() || m_task.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        m_result = m_task.get();
        LOG_INFO("Assembly", std::to_string(m_result->Matches.size()) + " matches in " + Grouped(m_result->Instructions) + " instructions");
    }

    void AsmSearchWindow::Results()
    {
        Engine& engine = m_workspace.Core();
        if (m_result->Matches.empty())
        {
            EmptyState(ICON_FA_SEARCH, "Nothing found", "No instruction in " + m_searched + " matches the pattern.", {}, false);
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##asmmatches", 2, flags))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_result->Matches.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const Disasm::AsmMatch& match = m_result->Matches[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                PushMono();
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
                if (ImGui::Selectable(Where(engine, match.Address).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    m_workspace.OpenMemoryViewer(match.Address);
                ImGui::PopStyleColor();
                PopMono();
                if (ImGui::BeginPopupContextItem("##matchmenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_CODE "  Show in disassembler"))
                        m_workspace.OpenMemoryViewer(match.Address);
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy address"))
                        ImGui::SetClipboardText(engine.Symbols().ModuleOffset(match.Address).c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                PushMono();
                ImGui::TextUnformatted(Join(match.Lines, "  ;  ").c_str());
                PopMono();
            }
        }
        ImGui::EndTable();
    }

    void AsmSearchWindow::Draw()
    {
        Collect();
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_SEARCH_LOCATION "  Find assembly code###asmsearch", &m_open, ImVec2(860.0f, 580.0f), 0, ImVec2(560.0f, 360.0f)))
        {
            EndToolWindow();
            return;
        }
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() && !m_task.valid() && !m_result)
        {
            m_workspace.NoProcessState(ICON_FA_SEARCH_LOCATION, "Open a process to search its code.");
            EndToolWindow();
            return;
        }
        Muted("One instruction per line, or separated by ;   * matches anything, ? one character.   Example: mov [rcx+*],eax");
        if (m_focus)
        {
            ImGui::SetKeyboardFocusHere();
            m_focus = false;
        }
        InputMultiline("##asmpattern", m_pattern, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f));
        const bool shortcut = ImGui::IsItemFocused() && ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        const bool running = m_task.valid();
        ImGui::BeginDisabled(running || !engine.HasProcess());
        ModuleScopeCombo("##asmscope", engine, m_scope, Px(280.0f));
        ImGui::SameLine();
        if (AccentButton(ICON_FA_SEARCH "  Search") || (shortcut && !running))
            Start();
        ImGui::EndDisabled();
        ItemTooltip("Ctrl+Enter in the pattern searches too");
        ImGui::SameLine(0.0f, Px(12.0f));
        if (running)
        {
            Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
            ImGui::SameLine(0.0f, Px(6.0f));
            ImGui::AlignTextToFramePadding();
            const std::uint64_t total = std::max<std::uint64_t>(1, m_progress->Total.load());
            Muted(Format("Searching  %d%%", static_cast<int>(m_progress->Done.load() * 100 / total)));
            ImGui::SameLine(0.0f, Px(8.0f));
            if (ImGui::Button("Cancel"))
                m_progress->Cancel = true;
        }
        else if (m_result)
        {
            ImGui::AlignTextToFramePadding();
            const std::size_t count = m_result->Matches.size();
            Muted(Grouped(count) + (count == 1 ? " match" : " matches") + (m_result->Truncated ? " (stopped at the limit)" : m_result->Cancelled ? " (cancelled)" : "") +
                  "  ·  " + Grouped(m_result->Instructions) + " instructions  ·  " + Format("%.2f s", m_result->Seconds));
        }
        ImGui::Spacing();
        if (m_result)
            Results();
        EndToolWindow();
    }
}
