#include "UI/CodeFilterWindow.h"

#include "App/Engine.h"
#include "App/Sounds.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Table/AddressList.h"
#include "Target/Process.h"
#include "UI/AddressActions.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include <algorithm>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::size_t kFilterLimit = 20000;
        constexpr double kHitsEvery = 0.2;

        bool HasBlocks(const std::vector<CodeCandidate>& candidates)
        {
            return std::any_of(candidates.begin(), candidates.end(), [](const CodeCandidate& candidate) { return candidate.Function != 0; });
        }

        std::size_t Functions(const std::vector<CodeCandidate>& candidates)
        {
            return static_cast<std::size_t>(std::count_if(candidates.begin(), candidates.end(), [](const CodeCandidate& candidate) { return candidate.Function == 0; }));
        }

        std::string Counted(std::size_t count, const char* one, const char* many)
        {
            return Grouped(count) + " " + (count == 1 ? one : many);
        }
    }

    CodeFilterWindow::CodeFilterWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    CodeFilterWindow::~CodeFilterWindow() = default;

    std::string CodeFilterWindow::KeysFor(FilterRound kind) const
    {
        const Settings& settings = m_workspace.Core().Options();
        const std::string& text = kind == FilterRound::FilterOut ? settings.CodeFilterOutKeys : settings.CodeFilterKeepKeys;
        if (!settings.CodeFilterHotkeys || Table::ParseKeysText(text).empty())
            return {};
        return Table::KeysText(Table::ParseKeysText(text));
    }

    void CodeFilterWindow::StartPrepare()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.Running() || engine.Filter().Running())
            return;
        CodeFilterOptions options;
        for (const Target::ModuleInfo& module : CodeFilter::DefaultModules(*engine.Process()))
        {
            if (!m_unchecked.count(module.Name))
                options.Modules.push_back(module.Name);
        }
        if (options.Modules.empty())
        {
            m_error = "Choose at least one module.";
            return;
        }
        options.Unwind = m_unwind;
        options.Exports = m_exports;
        options.Calls = engine.Process()->Is64Bit() ? engine.Options().CodeFilterCalls : true;
        m_error.clear();
        m_process = engine.Process();
        CodeFilter& filter = engine.Filter();
        m_task.Start(engine.Work(), "Collecting functions for the code filter", [&filter, options](Disasm::DissectProgress& progress)
            {
                Outcome outcome;
                Result<CodeFilterReport> report = filter.Prepare(options, &progress);
                if (report)
                    outcome.Report = std::move(report).Value();
                else
                    outcome.Error = report.Error().Text;
                return outcome;
            });
    }

    void CodeFilterWindow::Collect()
    {
        std::optional<Outcome> outcome = m_task.Collect();
        if (!outcome)
            return;
        if (!outcome->Report)
        {
            if (!m_task.State().Cancel)
                m_error = outcome->Error;
            return;
        }
        m_error.clear();
        m_shownRemaining = static_cast<std::size_t>(-1);
        m_selected = -1;
        if (outcome->Report->Total == 0)
            m_error = "No functions were found in the chosen modules.";
    }

    void CodeFilterWindow::Toggle(FilterRound kind)
    {
        Engine& engine = m_workspace.Core();
        CodeFilter& filter = engine.Filter();
        if (!engine.HasProcess() || !filter.Prepared() || m_task.Running())
            return;
        if (filter.Running())
        {
            const auto running = filter.RunningKind();
            const Status stopped = filter.Stop();
            m_shownRemaining = static_cast<std::size_t>(-1);
            if (!stopped)
            {
                engine.Notify(stopped);
                return;
            }
            const std::vector<RoundRecord> history = filter.History();
            if (!history.empty())
            {
                const RoundRecord& last = history.back();
                engine.Notify(LogLevel::Success, Format("%s: %s ran, %s left.", FilterRoundText(running.value_or(kind)), Counted(last.Hits, "function", "functions").c_str(),
                    Grouped(last.After).c_str()));
            }
            Sounds::Play(Sounds::Cue::Success);
            return;
        }
        const Status started = filter.Start(kind);
        if (!started)
        {
            engine.Notify(started);
            Sounds::Play(Sounds::Cue::Error);
            return;
        }
        m_hitsAt = -1.0;
        m_hits = 0;
        m_shownRemaining = static_cast<std::size_t>(-1);
        Sounds::Play(Sounds::Cue::Success);
    }

    void CodeFilterWindow::Refine()
    {
        Engine& engine = m_workspace.Core();
        const Result<std::size_t> refined = engine.Filter().Refine();
        if (!refined)
        {
            engine.Notify(refined.Error());
            return;
        }
        m_shownRemaining = static_cast<std::size_t>(-1);
        engine.Notify(LogLevel::Info, Counted(*refined, "block", "blocks") + " to filter now.");
    }

    // ∑ While the game has the focus, the two key sets start and stop a
    //   round, so the action can be done in the game without switching to
    //   this window. Only while this window is open.
    void CodeFilterWindow::Hotkeys()
    {
        Engine& engine = m_workspace.Core();
        const Settings& settings = engine.Options();
        if (!settings.CodeFilterHotkeys || !engine.HasProcess() || !engine.Filter().Prepared() || m_task.Running())
        {
            m_outDown = false;
            m_keepDown = false;
            return;
        }
        const std::vector<int> out = Table::ParseKeysText(settings.CodeFilterOutKeys);
        const std::vector<int> keep = Table::ParseKeysText(settings.CodeFilterKeepKeys);
        if (Table::UsableHotkeyKeys(out) && Table::KeysPressed(out, m_outDown))
            Toggle(FilterRound::FilterOut);
        if (Table::UsableHotkeyKeys(keep) && Table::KeysPressed(keep, m_keepDown))
            Toggle(FilterRound::KeepOnly);
    }

    void CodeFilterWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        Engine& engine = m_workspace.Core();
        CodeFilter& filter = engine.Filter();
        const bool prepared = filter.Prepared();
        const bool running = filter.Running();
        if (ImGui::BeginMenu("Filter"))
        {
            if (ImGui::MenuItem(ICON_FA_SYNC "  Collect Functions Again", nullptr, false, engine.HasProcess() && !running && !m_task.Running()))
                StartPrepare();
            if (ImGui::MenuItem(ICON_FA_UNDO "  Undo Last Step", nullptr, false, prepared && !running && !filter.History().empty()))
            {
                filter.Undo();
                m_shownRemaining = static_cast<std::size_t>(-1);
            }
            if (ImGui::MenuItem(ICON_FA_SITEMAP "  Refine to Blocks", nullptr, false, prepared && !running && m_functions <= CodeFilter::kMaxRefine && m_functions > 0))
                Refine();
            ItemTooltip("Splits every function that is left into its basic blocks, so the next rounds show which part of it ran.");
            ImGui::Separator();
            if (DangerMenuItem(ICON_FA_ERASER "  Start Over", nullptr, prepared))
            {
                filter.Reset();
                m_shownRemaining = static_cast<std::size_t>(-1);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Copy", prepared && filter.Remaining() > 0))
        {
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy the List"))
            {
                std::string text;
                for (const CodeCandidate& candidate : filter.Candidates())
                    text += engine.Symbols().ModuleOffset(candidate.Address) + "\t" + engine.Describe(candidate.Address) + "\t" + CodeSourceText(candidate.Sources) + "\n";
                ImGui::SetClipboardText(text.c_str());
            }
            ItemTooltip("Module+offset, name and where each address came from, one per line, tab-separated.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Options"))
        {
            Settings& settings = engine.Options();
            bool changed = false;
            changed |= ImGui::MenuItem("Call Targets on 64-Bit", nullptr, &settings.CodeFilterCalls);
            ItemTooltip("Also take the targets of calls as functions in 64-bit modules. That finds small functions the unwind table leaves out, and takes a code dissect "
                        "of each module. 32-bit modules always use them. Takes effect when the functions are collected.");
            changed |= ImGui::MenuItem("Hotkeys", nullptr, &settings.CodeFilterHotkeys);
            ItemTooltip("Start and stop rounds with the keys below while the game has the focus (only while this window is open).");
            ImGui::BeginDisabled(!settings.CodeFilterHotkeys);
            const auto keysField = [&](const char* label, const char* id, std::string& value)
                {
                    Layout::FieldOptions options;
                    options.Placement = Layout::LabelPlacement::Left;
                    options.Width = Px(120.0f);
                    Layout::Field(label, options);
                    changed |= InputTextWithHint(id, "F7, Ctrl+F7, ...", value);
                    if (!value.empty() && !Table::UsableHotkeyKeys(Table::ParseKeysText(value)))
                        Layout::Colored("Not a key set", ManifoldTheme::Warning);
                };
            keysField("Filter Out", "##cfoutkeys", settings.CodeFilterOutKeys);
            keysField("Keep Only", "##cfkeepkeys", settings.CodeFilterKeepKeys);
            ImGui::EndDisabled();
            if (changed)
                engine.SaveOptionsSoon();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void CodeFilterWindow::Setup()
    {
        Engine& engine = m_workspace.Core();
        const bool running = m_task.Running();
        const bool is64Bit = engine.Process()->Is64Bit();
        const std::vector<Target::ModuleInfo> modules = CodeFilter::DefaultModules(*engine.Process());
        const std::size_t chosen = static_cast<std::size_t>(std::count_if(modules.begin(), modules.end(), [&](const Target::ModuleInfo& module) { return !m_unchecked.count(module.Name); }));
        Layout::Wrapped("Finds code by what it does. Every function of the chosen modules gets a one-time breakpoint. Filter Out drops what runs while you do nothing; "
                        "Keep Only keeps what runs while you do the thing once. After a few rounds the code behind the action is left.",
            ManifoldTheme::Muted);
        Layout::Colored(ICON_FA_EXCLAMATION_TRIANGLE "  Games that check their own code or run an anti-cheat notice the breakpoints. Use it in single-player games only.",
            ManifoldTheme::Warning, Layout::Overflow::Wrap);
        Layout::Spacing(Layout::Space::S);
        {
            Layout::Row sources("##cfsources", { Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fit() }, Layout::Toolbar());
            sources.Cell([&] { Layout::Text("Functions From"); });
            sources.Cell([&] {
                ImGui::BeginDisabled(!is64Bit || running);
                bool unwind = is64Bit && m_unwind;
                if (ImGui::Checkbox("Unwind Table", &unwind))
                    m_unwind = unwind;
                ImGui::EndDisabled();
                ItemTooltip(is64Bit ? "The function table of 64-bit modules: exact starts." : "32-bit modules have no unwind table.");
            });
            sources.Cell([&] {
                ImGui::BeginDisabled(running);
                ImGui::Checkbox("Exports", &m_exports);
                ImGui::EndDisabled();
                ItemTooltip("The functions a module exports by name.");
            });
            sources.Cell([&] {
                Settings& settings = engine.Options();
                bool calls = !is64Bit || settings.CodeFilterCalls;
                ImGui::BeginDisabled(!is64Bit || running);
                if (ImGui::Checkbox("Call Targets", &calls))
                {
                    settings.CodeFilterCalls = calls;
                    engine.SaveOptionsSoon();
                }
                ImGui::EndDisabled();
                ItemTooltip(is64Bit ? "Also the targets of calls, checked to start at an instruction: finds small functions the unwind table leaves out. Takes a code dissect."
                                    : "32-bit modules always use the targets of calls, checked to start at an instruction.");
            });
        }
        Layout::Spacing(Layout::Space::S);
        Layout::CardOptions card;
        card.Icon = ICON_FA_LIST;
        card.Title = "Modules";
        const std::string badge = Grouped(chosen) + " of " + Grouped(modules.size()) + " chosen";
        card.Badge = badge;
        card.Height = -Px(48.0f);
        Layout::Card list("##cfmodules", card);
        if (list.Visible())
        {
            ImGui::BeginDisabled(running);
            Layout::Row pick("##cfpick", { Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fit() }, Layout::Toolbar());
            pick.Cell([&] {
                if (Layout::Button("All##cfall", Layout::ButtonKind::Ghost))
                    m_unchecked.clear();
            });
            pick.Cell([&] {
                if (Layout::Button("None##cfnone", Layout::ButtonKind::Ghost))
                {
                    for (const Target::ModuleInfo& module : modules)
                        m_unchecked.insert(module.Name);
                }
            });
            pick.Cell([&] {
                Layout::ButtonStyle style;
                style.Kind = Layout::ButtonKind::Ghost;
                style.Tooltip = "Only the program's own file, without the libraries it loaded.";
                if (Layout::Button("Main Module Only##cfmain", style))
                {
                    const auto main = engine.Process()->MainModule();
                    for (const Target::ModuleInfo& module : modules)
                    {
                        if (main && module.Base == main->Base)
                            m_unchecked.erase(module.Name);
                        else
                            m_unchecked.insert(module.Name);
                    }
                }
            });
            pick.End();
            ImGui::EndDisabled();
        }
        if (list.Visible() && BeginDataTable("##cfmoduletable", { { "Module", 2.0f }, { "Size", 0.6f } }))
        {
            for (std::size_t i = 0; i < modules.size(); ++i)
            {
                const Target::ModuleInfo& module = modules[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Layout::Row name("##cfmodulename", { Layout::Size::Fit(), Layout::Size::Fill() });
                name.Cell([&] {
                    bool on = !m_unchecked.count(module.Name);
                    ImGui::BeginDisabled(running);
                    if (ImGui::Checkbox("##cfmodule", &on))
                    {
                        if (on)
                            m_unchecked.erase(module.Name);
                        else
                            m_unchecked.insert(module.Name);
                    }
                    ImGui::EndDisabled();
                });
                name.Cell([&] { Layout::Text(module.Name); });
                name.End();
                ImGui::TableNextColumn();
                Layout::Muted(ByteSize(module.Size));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        list.End();
        Layout::Row actions("##cfprepare", { Layout::Size::Fit(), Layout::Size::Fill() });
        actions.Cell([&] {
            if (running)
            {
                if (Layout::Button(ICON_FA_TIMES "  Cancel##cfprepare"))
                    m_task.Cancel();
                return;
            }
            Layout::ButtonStyle style;
            style.Kind = Layout::ButtonKind::Accent;
            ImGui::BeginDisabled(chosen == 0);
            if (Layout::Button(ICON_FA_SEARCH "  Collect Functions##cfprepare", style))
                StartPrepare();
            ImGui::EndDisabled();
        });
        actions.Cell([&] {
            if (running)
            {
                const Disasm::DissectProgress& progress = m_task.State();
                const std::uint64_t total = std::max<std::uint64_t>(1, progress.Total.load());
                Layout::Muted(Format("Collecting functions, %d%%", static_cast<int>(std::min<std::uint64_t>(100, progress.Done.load() * 100 / total))));
            }
            else if (!m_error.empty())
                Layout::Colored(m_error, ManifoldTheme::Error);
        });
        actions.End();
    }

    void CodeFilterWindow::Rounds()
    {
        Engine& engine = m_workspace.Core();
        CodeFilter& filter = engine.Filter();
        const bool blocks = m_blocks;
        const std::size_t remaining = filter.Remaining();
        const auto running = filter.RunningKind();
        Layout::CardOptions card;
        card.Icon = ICON_FA_FILTER;
        card.Title = running ? std::string_view(FilterRoundText(*running)) : std::string_view("Rounds");
        const std::string badge = Counted(remaining, blocks ? "candidate" : "function", blocks ? "candidates" : "functions") + " left";
        card.Badge = badge;
        Layout::Card rounds("##cfrounds", card);
        if (rounds.Visible())
        {
            if (running)
            {
                const double now = ImGui::GetTime();
                if (m_hitsAt < 0.0 || now - m_hitsAt >= kHitsEvery)
                {
                    m_hits = filter.LiveHits();
                    m_hitsAt = now;
                }
                const std::string keys = KeysFor(*running);
                Layout::Heading(Format("%s ran so far  ·  %.1f s", Counted(static_cast<std::size_t>(m_hits), "function", "functions").c_str(), filter.RoundSeconds()));
                Layout::Muted(*running == FilterRound::FilterOut ? "Let the game run without doing the thing, then stop the round. What ran is dropped."
                                                                : "Do the thing once in the game, then stop the round. Only what ran is kept.",
                    Layout::Overflow::Wrap);
                Layout::Row bar("##cfrunning", { Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fill() }, Layout::Toolbar());
                bar.Cell([&] {
                    Layout::ButtonStyle style;
                    style.Kind = Layout::ButtonKind::Accent;
                    const std::string label = std::string(ICON_FA_STOP "  Stop Round") + (keys.empty() ? "" : "  (" + keys + ")") + "##cfstop";
                    if (Layout::Button(label, style))
                        Toggle(*running);
                });
                bar.Cell([&] {
                    Layout::ButtonStyle style;
                    style.Tooltip = "Ends the round without changing the list.";
                    if (Layout::Button(ICON_FA_TIMES "  Cancel Round##cfcancel", style))
                    {
                        m_workspace.Report(filter.Cancel());
                        m_shownRemaining = static_cast<std::size_t>(-1);
                    }
                });
                bar.End();
            }
            else
            {
                const std::string outKeys = KeysFor(FilterRound::FilterOut);
                const std::string keepKeys = KeysFor(FilterRound::KeepOnly);
                const std::size_t functions = m_functions;
                Layout::Row bar("##cfidle", { Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fill(), Layout::Size::Fit() }, Layout::Toolbar());
                bar.Cell([&] {
                    Layout::ButtonStyle style;
                    style.Kind = Layout::ButtonKind::Accent;
                    style.Tooltip = "Arms what is left. Let the game run without doing the thing, then stop: everything that ran is dropped.";
                    ImGui::BeginDisabled(remaining == 0);
                    if (Layout::Button(std::string(ICON_FA_PLAY "  Filter Out Running Code") + (outKeys.empty() ? "" : "  (" + outKeys + ")") + "##cfout", style))
                        Toggle(FilterRound::FilterOut);
                    ImGui::EndDisabled();
                });
                bar.Cell([&] {
                    Layout::ButtonStyle style;
                    style.Kind = Layout::ButtonKind::Accent;
                    style.Tooltip = "Arms what is left. Do the thing once, then stop: only what ran is kept.";
                    ImGui::BeginDisabled(remaining == 0);
                    if (Layout::Button(std::string(ICON_FA_CROSSHAIRS "  Keep Only Running Code") + (keepKeys.empty() ? "" : "  (" + keepKeys + ")") + "##cfkeep", style))
                        Toggle(FilterRound::KeepOnly);
                    ImGui::EndDisabled();
                });
                bar.Cell([&] {
                    Layout::ButtonStyle style;
                    style.Tooltip = functions > CodeFilter::kMaxRefine ? "Narrow it down to 256 functions or fewer first."
                                                                       : "Splits every function that is left into its basic blocks, so the next rounds show which part of it ran.";
                    ImGui::BeginDisabled(functions == 0 || functions > CodeFilter::kMaxRefine);
                    if (Layout::Button(ICON_FA_SITEMAP "  Refine to Blocks##cfrefine", style))
                        Refine();
                    ImGui::EndDisabled();
                });
                bar.Skip();
                bar.Cell([&] {
                    ImGui::BeginDisabled(filter.History().empty());
                    if (Layout::IconButton("##cfundo", ICON_FA_UNDO, "Undo the last step"))
                    {
                        filter.Undo();
                        m_shownRemaining = static_cast<std::size_t>(-1);
                    }
                    ImGui::EndDisabled();
                });
                bar.End();
                if (remaining == 0)
                    Layout::Colored("Nothing is left. Undo the last step, or start over with Filter > Start Over.", ManifoldTheme::Warning, Layout::Overflow::Wrap);
            }
        }
        rounds.End();
    }

    void CodeFilterWindow::History()
    {
        CodeFilter& filter = m_workspace.Core().Filter();
        const std::vector<RoundRecord> history = filter.History();
        std::string text = Grouped(filter.Report().Total);
        for (const RoundRecord& record : history)
        {
            const char* mark = record.Kind == FilterRound::FilterOut ? "  " ICON_FA_FILTER " " : record.Kind == FilterRound::KeepOnly ? "  " ICON_FA_CROSSHAIRS " " : "  " ICON_FA_SITEMAP " ";
            text += mark + Grouped(record.After);
        }
        if (history.empty())
            text += "  ·  no rounds yet";
        Layout::Muted(text);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            std::string tip = Grouped(filter.Report().Total) + " collected";
            for (const RoundRecord& record : history)
            {
                tip += "\n" + std::string(FilterRoundText(record.Kind)) + ": " + Grouped(record.Before) + " to " + Grouped(record.After);
                if (record.Kind != FilterRound::Refine)
                    tip += Format(", %s ran in %.1f s", Grouped(record.Hits).c_str(), record.Seconds);
                if (record.Skipped)
                    tip += ", " + Grouped(record.Skipped) + " could not be armed";
            }
            Layout::TextTooltip(tip);
        }
    }

    std::string CodeFilterWindow::Instruction(std::uint64_t address) const
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess())
            return {};
        const Disasm::Instruction instruction = engine.Disassembler().Read(*engine.Process(), address, &engine.Symbols());
        return instruction.Valid ? instruction.Text() : "??";
    }

    void CodeFilterWindow::ResultRow(const CodeCandidate& candidate, int index)
    {
        Engine& engine = m_workspace.Core();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (Layout::RowSelectable("##cfrow", m_selected == index, ImGuiSelectableFlags_AllowDoubleClick))
        {
            m_selected = index;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                m_workspace.RevealInMemoryViewer(candidate.Address, 1, true, true);
        }
        if (ImGui::BeginPopupContextItem("##cfrowmenu"))
        {
            m_selected = index;
            AddressMenuItems(m_workspace, candidate.Address, CodeActions);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_PROJECT_DIAGRAM "  Flow Graph"))
                m_workspace.OpenFlowGraph(candidate.Function ? candidate.Function : candidate.Address);
            if (ImGui::MenuItem(ICON_FA_FINGERPRINT "  Signature Maker"))
                m_workspace.OpenSignatureMaker(candidate.Address, true);
            ImGui::EndPopup();
        }
        Layout::Text(engine.Describe(candidate.Address), { .Color = ManifoldTheme::AddressText, .Font = Fonts::Mono() });
        if (m_blocks)
        {
            ImGui::TableNextColumn();
            if (candidate.Function && candidate.Function != candidate.Address)
                Layout::Text(engine.Describe(candidate.Function) + Format("  +%llX", static_cast<unsigned long long>(candidate.Address - candidate.Function)), { .Font = Fonts::Mono() });
            else
                Layout::Muted("start");
        }
        ImGui::TableNextColumn();
        Layout::Text(Instruction(candidate.Address), { .Font = Fonts::Mono() });
        ImGui::TableNextColumn();
        Layout::Muted(CodeSourceText(candidate.Sources));
    }

    void CodeFilterWindow::Refresh()
    {
        CodeFilter& filter = m_workspace.Core().Filter();
        const std::size_t remaining = filter.Remaining();
        const std::size_t steps = filter.History().size();
        if (remaining == m_shownRemaining && steps == m_shownHistory)
            return;
        m_shown = filter.Candidates();
        m_shownRemaining = remaining;
        m_shownHistory = steps;
        m_blocks = HasBlocks(m_shown);
        m_functions = Functions(m_shown);
        m_rowsValid = false;
        if (m_selected >= static_cast<int>(m_shown.size()))
            m_selected = -1;
    }

    void CodeFilterWindow::Results()
    {
        Engine& engine = m_workspace.Core();
        {
            Layout::Row bar("##cffilterbar", { Layout::Size::Fill() });
            bar.Cell([&] {
                Layout::FieldOptions options;
                options.Placement = Layout::LabelPlacement::None;
                options.MaxWidth = Px(320.0f);
                Layout::Field("Filter", options);
                ImGui::BeginDisabled(m_shown.size() > kFilterLimit);
                InputTextWithHint("##cffilter", m_shown.size() > kFilterLimit ? "filter: fewer than 20 000 left first" : ICON_FA_SEARCH " Module, name or source", m_filter);
                ImGui::EndDisabled();
            });
            bar.End();
        }
        const std::string filter = m_shown.size() <= kFilterLimit ? Trim(m_filter) : std::string();
        if (!m_rowsValid || filter != m_rowsFilter)
        {
            m_rows.clear();
            m_rows.reserve(m_shown.size());
            for (std::size_t i = 0; i < m_shown.size(); ++i)
            {
                if (!filter.empty() && !ContainsNoCase(engine.Describe(m_shown[i].Address), filter) && !ContainsNoCase(CodeSourceText(m_shown[i].Sources), filter))
                    continue;
                m_rows.push_back(static_cast<int>(i));
            }
            m_rowsFilter = filter;
            m_rowsValid = true;
        }
        const std::vector<int>& rows = m_rows;
        if (m_shown.empty())
            return;
        const bool open = m_blocks ? BeginDataTable("##cfblocks", { { "Address", 1.6f }, { "In Function", 1.4f }, { "Instruction", 1.6f }, { "From", 0.8f } }, ImVec2(0.0f, -FLT_MIN))
                                   : BeginDataTable("##cfresults", { { "Address", 1.6f }, { "Instruction", 1.6f }, { "From", 0.8f } }, ImVec2(0.0f, -FLT_MIN));
        if (!open)
            return;
        ClippedRows(static_cast<int>(rows.size()), [&](int row) { ResultRow(m_shown[static_cast<std::size_t>(rows[static_cast<std::size_t>(row)])], rows[static_cast<std::size_t>(row)]); });
        ImGui::EndTable();
    }

    void CodeFilterWindow::Draw()
    {
        Collect();
        Hotkeys();
        if (!m_open)
            return;
        if (m_collectPending && m_workspace.Core().HasProcess() && !m_task.Running() && !m_workspace.Core().Filter().Running())
        {
            m_collectPending = false;
            StartPrepare();
        }
        if (!BeginWindow(ICON_FA_FILTER "  Code Filter###codefilter", ImVec2(880.0f, 620.0f), ImGuiWindowFlags_MenuBar, ImVec2(480.0f, 320.0f)))
            return;
        Engine& engine = m_workspace.Core();
        MenuBar();
        if (!engine.HasProcess() && !m_task.Running())
        {
            m_workspace.NoProcessState(ICON_FA_FILTER, "Open a process to find code by what it does.");
            EndToolWindow();
            return;
        }
        CodeFilter& filter = engine.Filter();
        if (!filter.Prepared() || m_task.Running())
        {
            if (m_task.Running() || engine.HasProcess())
                Setup();
            EndToolWindow();
            return;
        }
        Refresh();
        Rounds();
        Layout::Spacing(Layout::Space::S);
        History();
        Layout::Spacing(Layout::Space::S);
        Results();
        EndToolWindow();
    }
}
