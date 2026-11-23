#include "UI/ByteSearch.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Disasm/Signature.h"
#include "Generated/IconsFontAwesome5.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"
#include "UI/AddressActions.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <chrono>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::size_t kMaxHits = 100000;
    }

    std::optional<Scan::Pattern> BytePattern(std::string_view input)
    {
        const std::string text = Trim(input);
        if (text.size() >= 2 && (text.front() == '\'' || text.front() == '"') && text.back() == text.front())
        {
            Scan::Pattern bytes;
            for (const char c : text.substr(1, text.size() - 2))
            {
                bytes.Bytes.push_back(static_cast<std::uint8_t>(c));
                bytes.Mask.push_back(0xFF);
            }
            if (bytes.Empty())
                return std::nullopt;
            return bytes;
        }
        auto pattern = Scan::ParsePattern(text);
        if (pattern && !pattern->Empty())
            return pattern;
        std::string error;
        if (auto signature = Disasm::ParseSignature(text, error))
            return std::move(signature->Pattern);
        return std::nullopt;
    }

    ByteSearchWindow::ByteSearchWindow(Workspace& workspace) : m_workspace(workspace)
    {
        m_scope.Kind = kScopeAllMemory;
    }

    ByteSearchWindow::~ByteSearchWindow() = default;

    void ByteSearchWindow::Search(const std::string& pattern)
    {
        Show();
        m_pattern = pattern;
        m_focus = false;
        Start();
    }

    void ByteSearchWindow::Start()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.Running())
            return;
        const auto pattern = BytePattern(m_pattern);
        if (!pattern)
        {
            m_error = ErrorMessage(ErrorCode::ScanBadPattern, Trim(m_pattern));
            return;
        }
        m_error.clear();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        Scan::PatternFilter filter;
        if (m_scope.Kind == kScopeAllMemory || m_scope.Kind == kScopeExecutable)
        {
            ranges.emplace_back(0, ~0ull);
            if (m_scope.Kind == kScopeExecutable)
                filter.Executable = Scan::Tristate::Yes;
        }
        else
        {
            engine.Process()->RefreshModules();
            for (const Target::ModuleInfo& module : ScopeModules(engine, m_scope))
                ranges.emplace_back(module.Base, module.Base + module.Size);
        }
        m_process = engine.Process();
        m_searched = ScopeName(engine, m_scope);
        m_searchedPattern = Trim(m_pattern);
        m_result.reset();
        m_selected = -1;
        std::shared_ptr<Target::Process> process = m_process;
        m_task.Start(engine.Work(), "Searching for bytes", [process, pattern = *pattern, ranges = std::move(ranges), filter](Scan::PatternProgress& progress)
            {
                const auto started = std::chrono::steady_clock::now();
                Outcome outcome;
                outcome.Length = pattern.Bytes.size();
                std::vector<std::uint64_t> addresses;
                for (const auto& [start, stop] : ranges)
                {
                    const auto hits = Scan::FindPattern(*process, pattern, start, stop, filter, kMaxHits + 1 - addresses.size(), &progress);
                    addresses.insert(addresses.end(), hits.begin(), hits.end());
                    if (addresses.size() > kMaxHits || progress.Cancel)
                        break;
                }
                outcome.Cancelled = progress.Cancel;
                std::sort(addresses.begin(), addresses.end());
                addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
                if (addresses.size() > kMaxHits)
                {
                    addresses.resize(kMaxHits);
                    outcome.Truncated = true;
                }
                const std::vector<Target::RegionInfo> regions = addresses.empty() ? std::vector<Target::RegionInfo>{} : process->Regions(addresses.front(), addresses.back() + 1);
                std::size_t region = 0;
                outcome.Hits.reserve(addresses.size());
                for (const std::uint64_t address : addresses)
                {
                    while (region < regions.size() && regions[region].End() <= address)
                        ++region;
                    const bool code = region < regions.size() && regions[region].Base <= address && regions[region].Executable();
                    outcome.Hits.push_back({ address, code });
                }
                outcome.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                return outcome;
            });
    }

    void ByteSearchWindow::Collect()
    {
        std::optional<Outcome> outcome = m_task.Collect();
        if (!outcome)
            return;
        m_result = std::move(outcome);
        LOG_INFO("Scan", Grouped(m_result->Hits.size()) + " matches for " + m_searchedPattern + " in " + m_searched + (m_result->Cancelled ? " (cancelled)" : ""));
        if (m_result->Hits.size() == 1)
        {
            m_selected = 0;
            Reveal(0, false);
        }
    }

    void ByteSearchWindow::Reveal(int row, bool raise)
    {
        if (!m_result || row < 0 || row >= static_cast<int>(m_result->Hits.size()))
            return;
        const Hit& hit = m_result->Hits[static_cast<std::size_t>(row)];
        m_workspace.RevealInMemoryViewer(hit.Address, m_result->Length, hit.Code, raise);
    }

    void ByteSearchWindow::Keys()
    {
        if (!m_result || m_result->Hits.empty() || ImGui::GetIO().WantTextInput || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            return;
        const int count = static_cast<int>(m_result->Hits.size());
        const int page = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().y / ImGui::GetFrameHeightWithSpacing()) - 1);
        int next = m_selected;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            next = m_selected < 0 ? 0 : m_selected + 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            next = m_selected < 0 ? 0 : m_selected - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
            next = m_selected < 0 ? 0 : m_selected + page;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            next = m_selected < 0 ? 0 : m_selected - page;
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
            next = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End, false))
            next = count - 1;
        next = std::clamp(next, 0, count - 1);
        if (next != m_selected)
        {
            m_selected = next;
            m_scrollToSelected = true;
            Reveal(m_selected, false);
        }
        if (m_selected >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
            Reveal(m_selected, true);
    }

    void ByteSearchWindow::Results()
    {
        Engine& engine = m_workspace.Core();
        if (m_result->Hits.empty())
        {
            EmptyState(ICON_FA_SEARCH, "Nothing found", "No bytes in " + m_searched + " match " + m_searchedPattern + ".", {}, false);
            return;
        }
        Keys();
        if (!BeginDataTable("##bytehits", { { "Address", 1.2f }, { "Bytes", 1.3f }, { "Code", 1.5f } }))
            return;
        const std::size_t shown = std::min<std::size_t>(m_result->Length, 16);
        std::vector<std::uint8_t> bytes(shown);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_result->Hits.size()));
        if (m_scrollToSelected && m_selected >= 0)
            clipper.IncludeItemByIndex(m_selected);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const Hit& hit = m_result->Hits[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                PushMono();
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
                if (ImGui::Selectable(AddressText(engine, hit.Address).c_str(), row == m_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selected = row;
                    Reveal(row, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                }
                ImGui::PopStyleColor();
                PopMono();
                if (m_scrollToSelected && row == m_selected)
                {
                    ImGui::SetScrollHereY();
                    m_scrollToSelected = false;
                }
                if (ImGui::BeginPopupContextItem("##hitmenu"))
                {
                    m_selected = row;
                    if (ImGui::MenuItem(ICON_FA_CODE "  Show in the Memory Viewer"))
                        Reveal(row, true);
                    ImGui::Separator();
                    AddressMenuItems(m_workspace, hit.Address, hit.Code ? (CopyAddress | CopyModuleOffset) : (DissectHere | WatchAccesses | WatchWrites | AddToList | CopyAddress | CopyModuleOffset));
                    if (ImGui::MenuItem(ICON_FA_LIST "  Copy All Addresses"))
                    {
                        std::string text;
                        for (const Hit& each : m_result->Hits)
                            text += engine.Symbols().ModuleOffset(each.Address) + "\r\n";
                        ImGui::SetClipboardText(text.c_str());
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                PushMono();
                if (m_process && m_process->ReadExact(hit.Address, bytes.data(), bytes.size()))
                {
                    std::string text;
                    for (const std::uint8_t byte : bytes)
                        text += (text.empty() ? "" : " ") + Hex(byte, 2);
                    if (m_result->Length > shown)
                        text += " ...";
                    ImGui::TextUnformatted(text.c_str());
                }
                else
                {
                    ImGui::TextDisabled("unreadable");
                }
                ImGui::TableNextColumn();
                if (hit.Code && engine.HasProcess() && m_process == engine.Process())
                    ImGui::TextUnformatted(engine.Disassembler().Read(*m_process, hit.Address, &engine.Symbols()).Text().c_str());
                PopMono();
            }
        }
        ImGui::PopItemFlag();
        ImGui::EndTable();
    }

    void ByteSearchWindow::Draw()
    {
        Collect();
        if (!BeginWindow(ICON_FA_SEARCH "  Find Bytes###bytesearch", ImVec2(780.0f, 540.0f), 0, ImVec2(480.0f, 300.0f)))
            return;
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() && !m_task.Running() && !m_result)
        {
            m_workspace.NoProcessState(ICON_FA_SEARCH, "Open a process to search its memory.");
            EndToolWindow();
            return;
        }
        Muted("Hex bytes with ?? for any byte and 8? for half of one, or 'text' in quotes.   Example: 48 8B ?? ?? 89");
        if (m_focus)
        {
            ImGui::SetKeyboardFocusHere();
            m_focus = false;
        }
        const bool running = m_task.Running();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = InputTextWithHint("##bytepattern", "48 8B ?? 10 or 'text'", m_pattern, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_error.empty())
            TextWithError(m_error, ManifoldTheme::Error);
        ImGui::BeginDisabled(running || !engine.HasProcess());
        ModuleScopeCombo("##bytescope", engine, m_scope, Px(280.0f), true);
        ImGui::SameLine();
        if (AccentButton(ICON_FA_SEARCH "  Find All") || (enter && !running))
        {
            Start();
            if (enter && m_error.empty())
                ImGui::ClearActiveID();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, Px(12.0f));
        if (running)
        {
            const Scan::PatternProgress& progress = m_task.State();
            const std::uint64_t total = std::max<std::uint64_t>(1, progress.Total.load());
            if (TaskStatus("Searching " + m_searched + Format("  %d%%", static_cast<int>(std::min<std::uint64_t>(100, progress.Done.load() * 100 / total)))))
                m_task.Cancel();
        }
        else if (m_result)
        {
            ImGui::AlignTextToFramePadding();
            const std::size_t count = m_result->Hits.size();
            Muted(Grouped(count) + (count == 1 ? " match" : " matches") + (m_result->Truncated ? " (stopped at the limit)" : m_result->Cancelled ? " (cancelled)" : "") + "  ·  " + m_searchedPattern + "  ·  " +
                  m_searched + "  ·  " + Format("%.2f s", m_result->Seconds));
        }
        ImGui::Spacing();
        if (m_result)
            Results();
        EndToolWindow();
    }
}
