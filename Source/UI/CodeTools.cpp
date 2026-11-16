#include "UI/Windows.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/AddressActions.h"
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

    }

    void ModuleScopeCombo(const char* id, Engine& engine, ModuleScope& scope, float width, bool memory)
    {
        ImGui::SetNextItemWidth(width);
        const std::string preview = ScopeName(engine, scope);
        if (!ImGui::BeginCombo(id, preview.c_str()))
            return;
        if (memory)
        {
            if (ImGui::Selectable("All memory", scope.Kind == kScopeAllMemory))
                scope = { kScopeAllMemory, {} };
            if (ImGui::Selectable("Executable memory", scope.Kind == kScopeExecutable))
                scope = { kScopeExecutable, {} };
            ImGui::Separator();
        }
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
        if (!engine.HasProcess() || scope.Kind < 0)
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
        case kScopeAllMemory: return "All memory";
        case kScopeExecutable: return "Executable memory";
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

    CodeDissectWindow::~CodeDissectWindow() = default;

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
        if (!engine.HasProcess() || m_task.Running())
            return;
        engine.Process()->RefreshModules();
        std::vector<Target::ModuleInfo> modules = ScopeModules(engine, m_scope);
        if (modules.empty())
        {
            engine.Notify(LogLevel::Warning, "No module matches the chosen scope.");
            return;
        }
        m_generation = engine.ProcessGeneration();
        std::shared_ptr<Target::Process> process = engine.Process();
        m_task.Start([process, modules = std::move(modules)](Disasm::DissectProgress& progress)
            {
                return Disasm::DissectCode(*process, modules, &progress);
            });
    }

    void CodeDissectWindow::Collect()
    {
        std::optional<std::shared_ptr<Disasm::DissectResult>> collected = m_task.Collect();
        if (!collected || !*collected)
            return;
        std::shared_ptr<Disasm::DissectResult> result = std::move(*collected);
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
        const bool running = m_task.Running();
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
            const Disasm::DissectProgress& progress = m_task.State();
            const std::uint64_t total = std::max<std::uint64_t>(1, progress.Total.load());
            const int phase = progress.Phase.load();
            if (TaskStatus(phase == 0 ? Format("Reading code  %d%%", static_cast<int>(progress.Done.load() * 100 / total)) : phase == 1 ? std::string("Sorting references") : std::string("Looking for strings")))
                m_task.Cancel();
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
        ImGui::TextColored(ManifoldTheme::AddressText, "%s", AddressText(engine, m_selected).c_str());
        PopMono();
        ImGui::SameLine(0.0f, Px(8.0f));
        Muted(refs.empty() ? std::string("no code uses this address") : std::to_string(refs.size()) + (refs.size() == 1 ? " reference" : " references"));
        if (refs.empty())
            return;
        if (!BeginDataTable("##refs", { { "From", 1.2f }, { "Kind", 1.0f, 120.0f }, { "Instruction", 2.0f } }))
            return;
        ClippedRows(static_cast<int>(refs.size()), [&](int row)
            {
                const Disasm::CodeRef& ref = refs[static_cast<std::size_t>(row)];
                const std::uint64_t from = ref.From();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                AddressCell(m_workspace, from, false, CodeActions);
                ImGui::TableNextColumn();
                Muted(Disasm::RefKindName(ref.Kind()));
                ImGui::TableNextColumn();
                PushMono();
                if (engine.HasProcess())
                    ImGui::TextUnformatted(engine.Disassembler().Read(*engine.Process(), from, &engine.Symbols()).Text().c_str());
                PopMono();
            });
        ImGui::EndTable();
    }

    void CodeDissectWindow::StringsTab(const Disasm::DissectResult& result, float height)
    {
        Engine& engine = m_workspace.Core();
        ImGui::SetNextItemWidth(Px(260.0f));
        InputTextWithHint("##stringfilter", ICON_FA_SEARCH " Text", m_filter);
        const std::string key = "s:" + m_filter;
        const auto current = engine.CodeDissect();
        if (m_visibleFor != current || m_visibleFilter != key)
        {
            m_visible.clear();
            for (std::size_t i = 0; i < result.Strings.size(); ++i)
            {
                if (m_filter.empty() || ContainsNoCase(result.Strings[i].Text, m_filter))
                    m_visible.push_back(i);
            }
            m_visibleFor = current;
            m_visibleFilter = key;
        }
        if (result.Strings.empty())
        {
            Muted("No referenced strings were found in this code.");
            return;
        }
        if (BeginDataTable("##strings", { { "Address", 1.0f, 200.0f }, { "Refs", 1.0f, 50.0f }, { "Text", 1.0f } }, ImVec2(0.0f, height)))
        {
            ClippedRows(static_cast<int>(m_visible.size()), [&](int row)
                {
                    const Disasm::ReferencedString& entry = result.Strings[m_visible[static_cast<std::size_t>(row)]];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const auto copyText = [&entry]()
                        {
                            if (ImGui::MenuItem(ICON_FA_COPY "  Copy text"))
                                ImGui::SetClipboardText(entry.Text.c_str());
                        };
                    if (AddressCell(m_workspace, entry.Address, m_selected == entry.Address, DataActions, false, copyText))
                        m_selected = entry.Address;
                    ImGui::TableNextColumn();
                    Muted(std::to_string(entry.References));
                    ImGui::TableNextColumn();
                    if (entry.Wide)
                    {
                        ImGui::TextColored(ManifoldTheme::Muted, "L");
                        ImGui::SameLine(0.0f, Px(4.0f));
                    }
                    ImGui::TextUnformatted(Shown(entry.Text, 300).c_str());
                });
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
        const auto current = engine.CodeDissect();
        if (m_visibleFor != current || m_visibleFilter != key)
        {
            m_visible.clear();
            for (std::size_t index : m_byCalls)
            {
                const std::uint64_t address = result.Functions[index].Address;
                if (m_filter.empty() || ContainsNoCase(Hex(address), m_filter) || ContainsNoCase(engine.Describe(address), m_filter))
                    m_visible.push_back(index);
            }
            m_visibleFor = current;
            m_visibleFilter = key;
        }
        if (BeginDataTable("##functions", { { "Calls", 1.0f, 60.0f }, { "Function", 1.0f } }, ImVec2(0.0f, height)))
        {
            ClippedRows(static_cast<int>(m_visible.size()), [&](int row)
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
                        AddressMenuItems(m_workspace, entry.Address, CodeActions);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    PushMono();
                    ImGui::TextColored(ManifoldTheme::AddressText, "%s", engine.Describe(entry.Address).c_str());
                    PopMono();
                });
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
        if (!BeginWindow(ICON_FA_PROJECT_DIAGRAM "  Code dissect###codedissect", ImVec2(940.0f, 640.0f), 0, ImVec2(640.0f, 400.0f)))
            return;
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() && !m_task.Running())
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
            if (!m_task.Running() && EmptyState(ICON_FA_PROJECT_DIAGRAM, "Dissect code",
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

    AsmSearchWindow::~AsmSearchWindow() = default;

    void AsmSearchWindow::Start()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.Running())
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
        m_result.reset();
        m_searched = ScopeName(engine, m_scope);
        std::shared_ptr<Target::Process> process = m_process;
        const Disasm::Disassembler* disassembler = &engine.Disassembler();
        const Target::Symbols* symbols = &engine.Symbols();
        m_task.Start([process, disassembler, symbols, spans = std::move(spans), pattern = *pattern](Disasm::AsmSearchProgress& progress)
            {
                return Disasm::SearchAssembly(*process, *disassembler, symbols, spans, pattern, &progress);
            });
    }

    void AsmSearchWindow::Collect()
    {
        std::optional<Disasm::AsmSearchResult> collected = m_task.Collect();
        if (!collected)
            return;
        m_result = std::move(collected);
        LOG_INFO("Assembly", std::to_string(m_result->Matches.size()) + " matches in " + Grouped(m_result->Instructions) + " instructions");
    }

    void AsmSearchWindow::Results()
    {
        if (m_result->Matches.empty())
        {
            EmptyState(ICON_FA_SEARCH, "Nothing found", "No instruction in " + m_searched + " matches the pattern.", {}, false);
            return;
        }
        if (!BeginDataTable("##asmmatches", { { "Address", 1.0f }, { "Code", 2.2f } }))
            return;
        ClippedRows(static_cast<int>(m_result->Matches.size()), [&](int row)
            {
                const Disasm::AsmMatch& match = m_result->Matches[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                AddressCell(m_workspace, match.Address, false, CodeActions);
                ImGui::TableNextColumn();
                PushMono();
                ImGui::TextUnformatted(Join(match.Lines, "  ;  ").c_str());
                PopMono();
            });
        ImGui::EndTable();
    }

    void AsmSearchWindow::Draw()
    {
        Collect();
        if (!BeginWindow(ICON_FA_SEARCH_LOCATION "  Find assembly code###asmsearch", ImVec2(860.0f, 580.0f), 0, ImVec2(560.0f, 360.0f)))
            return;
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() && !m_task.Running() && !m_result)
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
        const bool running = m_task.Running();
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
            const Disasm::AsmSearchProgress& progress = m_task.State();
            const std::uint64_t total = std::max<std::uint64_t>(1, progress.Total.load());
            if (TaskStatus(Format("Searching  %d%%", static_cast<int>(progress.Done.load() * 100 / total))))
                m_task.Cancel();
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
