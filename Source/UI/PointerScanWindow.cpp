#include "UI/PointerScanWindow.h"

#include "App/Engine.h"
#include "Core/Paths.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"
#include "UI/AddressActions.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>

namespace Manifold::UI
{
    namespace
    {
        constexpr int kShownRows = 500000;
        constexpr std::size_t kCopiedPaths = 20000;
        constexpr double kBasesSeconds = 1.0;
        constexpr std::uint64_t kResultLimits[] = { 100'000, 1'000'000, 2'000'000, 10'000'000 };
        const std::vector<FileFilter> kPathFilters = { { L"Pointer paths (*.mptr)", L"*.mptr" }, { L"All files", L"*.*" } };

        void WrappedMuted(std::string_view text)
        {
            ImGui::PushTextWrapPos(0.0f);
            Muted(text);
            ImGui::PopTextWrapPos();
        }

        double Since(std::chrono::steady_clock::time_point started)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        }

        // ∑ The main scanner's Rounded mode: a value typed with n decimals matches within half of 10^-n, so the value as the window shows it is found again.
        double RoundingStep(Scan::ValueType type, std::string_view text)
        {
            text = TrimView(text);
            if (!Scan::IsFloating(type) || text.empty() || StartsWithNoCase(text, "0x") || text.front() == '$')
                return 0.0;
            int decimals = 0;
            if (const std::size_t dot = text.find_first_of(".,"); dot != std::string_view::npos)
            {
                for (std::size_t index = dot + 1; index < text.size() && text[index] >= '0' && text[index] <= '9'; ++index)
                    ++decimals;
            }
            return std::pow(10.0, -decimals);
        }

        const char* OrderName(Scan::PointerOrder order)
        {
            switch (order)
            {
            case Scan::PointerOrder::Level: return "Fewest levels first";
            case Scan::PointerOrder::LastOffset: return "Smallest last offset first";
            case Scan::PointerOrder::Base: return "By base address";
            default: return "As found";
            }
        }
    }

    PointerScanWindow::PointerScanWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    PointerScanWindow::~PointerScanWindow() = default;

    void PointerScanWindow::Prepare(std::uint64_t address)
    {
        Show();
        if (address)
        {
            m_address = Hex(address);
            m_rescanAddress = m_address;
        }
        m_error.clear();
        m_focus = address == 0;
    }

    void PointerScanWindow::Start()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.Running())
            return;
        Manifold::Status error;
        const auto target = engine.Evaluate(Trim(m_address), &error);
        if (!target)
        {
            m_error = error.Text.empty() ? ErrorMessage(ErrorCode::ExprUnknownSymbol, Trim(m_address)) : error.Text;
            return;
        }
        const auto maxOffset = ParseHex(Trim(m_maxOffset));
        if (!maxOffset || *maxOffset > Scan::kPointerMaxOffset)
        {
            m_error = "The largest offset must be a hex number up to " + Hex(Scan::kPointerMaxOffset) + ".";
            return;
        }
        m_error.clear();
        engine.Process()->RefreshModules();
        Scan::PointerScanOptions options;
        options.Target = *target;
        options.MaxLevel = std::clamp(m_level, 1, Scan::kPointerMaxLevel);
        options.MaxOffset = static_cast<std::uint32_t>(*maxOffset);
        options.Aligned = m_aligned;
        options.MaxResults = m_maxResults;
        const std::shared_ptr<Target::Process> process = engine.Process();
        const Scan::PointerMemory memory = m_memory;
        m_running = Job::Scan;
        m_rescanAddress = Hex(*target);
        m_task.Start(engine.Work(), "Scanning for pointer paths", [process, memory, options](Scan::PointerProgress& progress)
            {
                const auto started = std::chrono::steady_clock::now();
                Outcome outcome;
                outcome.Kind = Job::Scan;
                outcome.Results = std::make_shared<Scan::PointerResults>();
                const Scan::ProcessPointerSource source(process, memory);
                outcome.Error = Scan::ScanPointers(source, options, *outcome.Results, &progress, &outcome.Stats);
                outcome.Seconds = Since(started);
                return outcome;
            });
    }

    void PointerScanWindow::Rescan()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.Running() || !m_results)
            return;
        Scan::PointerRescan rescan;
        if (m_rescanByValue)
        {
            Scan::ValueFormat format;
            format.PointerSize = engine.Process()->PointerSize();
            const auto bytes = Scan::ParseValue(m_rescanType, Trim(m_rescanValue), format);
            if (!bytes || bytes->empty())
            {
                m_rescanError = ErrorMessage(ErrorCode::ScanBadValue, Trim(m_rescanValue));
                return;
            }
            rescan.Value = *bytes;
            rescan.Type = m_rescanType;
            rescan.Step = RoundingStep(m_rescanType, m_rescanValue);
        }
        else
        {
            Manifold::Status error;
            const auto address = engine.Evaluate(Trim(m_rescanAddress), &error);
            if (!address)
            {
                m_rescanError = error.Text.empty() ? ErrorMessage(ErrorCode::ExprUnknownSymbol, Trim(m_rescanAddress)) : error.Text;
                return;
            }
            rescan.Address = *address;
        }
        m_rescanError.clear();
        engine.Process()->RefreshModules();
        const std::shared_ptr<Target::Process> process = engine.Process();
        const std::shared_ptr<Scan::PointerResults> from = m_results;
        m_running = Job::Rescan;
        m_task.Start(engine.Work(), "Checking pointer paths", [process, from, rescan](Scan::PointerProgress& progress)
            {
                const auto started = std::chrono::steady_clock::now();
                Outcome outcome;
                outcome.Kind = Job::Rescan;
                outcome.Before = from->Count();
                outcome.Results = std::make_shared<Scan::PointerResults>();
                const Scan::ProcessPointerSource source(process, { true, true, true });
                outcome.Error = Scan::RescanPointers(source, *from, rescan, *outcome.Results, &progress);
                outcome.Stats.Cancelled = progress.Cancel;
                outcome.Stats.Results = outcome.Results->Count();
                outcome.Seconds = Since(started);
                return outcome;
            });
    }

    void PointerScanWindow::Sort(Scan::PointerOrder order)
    {
        if (m_task.Running() || !m_results)
            return;
        if (order == Scan::PointerOrder::Found)
        {
            m_order.clear();
            m_orderKind = order;
            m_selected = -1;
            return;
        }
        const std::shared_ptr<Scan::PointerResults> results = m_results;
        m_running = Job::Order;
        m_task.Start(m_workspace.Core().Work(), "Sorting pointer paths", [results, order](Scan::PointerProgress& progress)
            {
                Outcome outcome;
                outcome.Kind = Job::Order;
                outcome.Results = results;
                outcome.OrderKind = order;
                outcome.Order = Scan::SortPointerResults(*results, order, &progress);
                return outcome;
            });
    }

    void PointerScanWindow::Open()
    {
        if (m_task.Running())
            return;
        std::error_code ignored;
        std::filesystem::create_directories(Paths::TablesFolder(), ignored);
        const auto path = m_workspace.OpenFileDialog(kPathFilters, Paths::TablesFolder());
        if (!path)
            return;
        m_running = Job::Load;
        m_task.Start(m_workspace.Core().Work(), "Loading pointer paths", [file = *path](Scan::PointerProgress&)
            {
                Outcome outcome;
                outcome.Kind = Job::Load;
                outcome.File = WideToUtf8(file.filename().wstring());
                outcome.Results = std::make_shared<Scan::PointerResults>();
                outcome.Error = outcome.Results->Load(file);
                outcome.Stats.Results = outcome.Results->Count();
                return outcome;
            });
    }

    void PointerScanWindow::Save()
    {
        Engine& engine = m_workspace.Core();
        if (m_task.Running() || !m_results || m_results->Count() == 0)
            return;
        std::error_code ignored;
        std::filesystem::create_directories(Paths::TablesFolder(), ignored);
        std::string name = engine.HasProcess() ? engine.Process()->Name() : std::string("pointers");
        if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
            name.resize(dot);
        const auto path = m_workspace.SaveFileDialog(kPathFilters, WideToUtf8((Paths::TablesFolder() / Utf8ToWide(name + ".mptr")).wstring()), L"mptr");
        if (!path)
            return;
        const std::shared_ptr<Scan::PointerResults> results = m_results;
        m_running = Job::Save;
        m_task.Start(engine.Work(), "Saving pointer paths", [results, file = *path](Scan::PointerProgress&)
            {
                Outcome outcome;
                outcome.Kind = Job::Save;
                outcome.File = WideToUtf8(file.filename().wstring());
                outcome.Results = results;
                outcome.Error = results->Save(file);
                outcome.Stats.Results = results->Count();
                return outcome;
            });
    }

    void PointerScanWindow::Collect()
    {
        std::optional<Outcome> outcome = m_task.Collect();
        if (!outcome)
            return;
        Engine& engine = m_workspace.Core();
        if (!outcome->Error)
        {
            m_workspace.Report(outcome->Error);
            if (outcome->Kind != Job::Scan)
                return;
        }
        if (outcome->Kind == Job::Order)
        {
            if (outcome->Results != m_results)
                return;
            if (outcome->Order)
            {
                m_order = std::move(*outcome->Order);
                m_orderKind = outcome->OrderKind;
                m_selected = -1;
            }
            return;
        }
        if (outcome->Kind == Job::Save)
        {
            engine.Notify(LogLevel::Success, "Saved " + Grouped(outcome->Stats.Results) + " pointer paths to " + outcome->File);
            return;
        }
        if (outcome->Kind == Job::Rescan && m_results && (outcome->Stats.Cancelled || outcome->Stats.Results == 0))
        {
            // ∑ An empty or partial set would replace the only copy of the old paths, and with no paths Rescan and Save are off, so nothing could bring them back.
            const std::uint64_t kept = m_results->Count();
            m_summary = (outcome->Stats.Cancelled ? std::string("Rescan cancelled") : "No path of " + Grouped(outcome->Before) + " leads there now") + ", the previous " + Grouped(kept) +
                        (kept == 1 ? " path is" : " paths are") + " kept  ·  " + Format("%.2f s", outcome->Seconds);
            engine.Notify(outcome->Stats.Cancelled ? LogLevel::Info : LogLevel::Warning, m_summary);
            return;
        }
        m_results = std::move(outcome->Results);
        m_order.clear();
        m_orderKind = Scan::PointerOrder::Found;
        m_selected = -1;
        m_basesGeneration = ~0ull;
        m_scanned = true;
        const std::uint64_t count = m_results->Count();
        const std::string paths = Grouped(count) + (count == 1 ? " path" : " paths");
        if (outcome->Kind == Job::Scan)
        {
            m_stats = outcome->Stats;
            m_summary = paths + (m_stats.Truncated ? " (stopped at the limit)" : m_stats.Cancelled ? " (cancelled)" : "") + " to " + Hex(m_results->Target()) + "  ·  " + Grouped(m_stats.Pointers) +
                        " pointers in " + ByteSize(m_stats.ScannedBytes) + "  ·  read " + Format("%.2f s", m_stats.MapSeconds) + ", search " + Format("%.2f s", m_stats.SearchSeconds) + "  ·  " +
                        ByteSize(m_stats.MapBytes + m_stats.SearchBytes) + " used";
            LOG_INFO("Scan", "Pointer scan: " + m_summary);
        }
        else if (outcome->Kind == Job::Rescan)
        {
            m_stats = {};
            m_summary = paths + " of " + Grouped(outcome->Before) + " still lead there  ·  " + Format("%.2f s", outcome->Seconds);
            LOG_INFO("Scan", "Pointer rescan: " + m_summary);
        }
        else
        {
            m_stats = {};
            m_summary = paths + " from " + outcome->File + (m_results->Target() ? ", found for " + Hex(m_results->Target()) : "");
            if (m_results->Target())
                m_rescanAddress = Hex(m_results->Target());
            engine.Notify(LogLevel::Success, "Loaded " + m_summary);
        }
    }

    void PointerScanWindow::RefreshBases()
    {
        Engine& engine = m_workspace.Core();
        if (!m_results)
            return;
        const std::uint64_t generation = engine.ProcessGeneration() * 1000003ull + engine.Symbols().Generation();
        const double now = engine.Now();
        if (generation == m_basesGeneration && now - m_basesAt < kBasesSeconds)
            return;
        m_basesGeneration = generation;
        m_basesAt = now;
        m_bases.assign(m_results->Modules().size(), 0);
        if (!engine.HasProcess())
            return;
        for (std::size_t index = 0; index < m_bases.size(); ++index)
        {
            if (const auto module = engine.Process()->FindModule(m_results->Modules()[index]))
                m_bases[index] = module->Base;
        }
    }

    bool PointerScanWindow::PathAt(int row, Scan::PointerPath& path) const
    {
        if (!m_results || row < 0)
            return false;
        const std::uint64_t index = m_order.empty() ? static_cast<std::uint64_t>(row) : static_cast<std::size_t>(row) < m_order.size() ? m_order[static_cast<std::size_t>(row)] : ~0ull;
        return m_results->Read(index, path);
    }

    std::optional<std::uint64_t> PointerScanWindow::Resolve(const Scan::PointerPath& path) const
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || path.Module >= m_bases.size() || m_bases[path.Module] == 0)
            return std::nullopt;
        const Target::Process& process = *engine.Process();
        const int pointerSize = m_results->PointerSize();
        return Scan::ResolvePointerPath(m_bases[path.Module], path, [&](std::uint64_t address) -> std::optional<std::uint64_t>
            {
                std::uint64_t value = 0;
                if (!process.ReadExact(address, &value, static_cast<std::size_t>(pointerSize)))
                    return std::nullopt;
                return value;
            });
    }

    void PointerScanWindow::AddToList(const Scan::PointerPath& path)
    {
        Engine& engine = m_workspace.Core();
        Table::MemoryRecord& record = engine.Table().AddValue("Pointer path " + m_results->BaseText(path), m_results->BaseText(path), m_valueType);
        for (int index = 0; index < path.Count; ++index)
            record.Offsets.push_back(Hex(path.Offsets[index]));
        engine.RefreshValues(true);
    }

    void PointerScanWindow::CopyAll() const
    {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(m_results->Count(), kCopiedPaths));
        std::string text;
        Scan::PointerPath path;
        for (std::size_t row = 0; row < count; ++row)
        {
            if (PathAt(static_cast<int>(row), path))
                text += m_results->PathText(path) + "\r\n";
        }
        ImGui::SetClipboardText(text.c_str());
    }

    std::string PointerScanWindow::ProgressText() const
    {
        const Scan::PointerProgress& progress = m_task.State();
        const std::uint64_t total = std::max<std::uint64_t>(1, progress.Total.load());
        const int percent = static_cast<int>(std::min<std::uint64_t>(100, progress.Done.load() * 100 / total));
        switch (static_cast<Scan::PointerStage>(progress.Stage.load()))
        {
        case Scan::PointerStage::Reading: return "Reading memory" + Format("  %d%%", percent) + "  ·  " + Grouped(progress.Pointers.load()) + " pointers";
        case Scan::PointerStage::Sorting: return "Sorting " + Grouped(progress.Pointers.load()) + " pointers";
        case Scan::PointerStage::Searching: return "Searching level " + std::to_string(progress.Level.load()) + "  ·  " + Grouped(progress.Results.load()) + " paths";
        case Scan::PointerStage::Rescanning: return "Checking paths" + Format("  %d%%", percent) + "  ·  " + Grouped(progress.Results.load()) + " still lead there";
        case Scan::PointerStage::Ordering: return "Sorting paths";
        default: return m_running == Job::Load ? "Loading paths" : m_running == Job::Save ? "Saving paths" : "Starting";
        }
    }

    void PointerScanWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        Engine& engine = m_workspace.Core();
        const bool running = m_task.Running();
        const bool process = engine.HasProcess();
        const bool results = m_results && m_results->Count() > 0;
        if (ImGui::BeginMenu("Scan"))
        {
            if (ImGui::MenuItem(ICON_FA_SEARCH "  Scan", nullptr, false, !running && process))
                Start();
            if (ImGui::MenuItem(ICON_FA_FILTER "  Rescan", nullptr, false, !running && process && results))
                Rescan();
            if (ImGui::MenuItem(ICON_FA_STOP "  Stop", nullptr, false, running))
                m_task.Cancel();
            ImGui::Separator();
            ImGui::MenuItem("Offsets Are Multiples of the Pointer Size", nullptr, &m_aligned);
            ItemTooltip("Follows only pointers to addresses that are a multiple of 8 (4 in a 32-bit process), which is where objects start. The last offset follows the address itself. "
                        "Turn it off for packed structures. Takes effect on the next scan.");
            if (ImGui::BeginMenu("Most Paths"))
            {
                for (const std::uint64_t limit : kResultLimits)
                {
                    if (ImGui::MenuItem(Grouped(limit).c_str(), nullptr, m_maxResults == limit))
                        m_maxResults = limit;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Memory"))
        {
            ImGui::MenuItem("Read-Only Memory", nullptr, &m_memory.ReadOnly);
            ItemTooltip("Also follows pointers that lie in read-only memory. Game objects are writable, so this mostly adds time and memory.");
            ImGui::MenuItem("Mapped Memory", nullptr, &m_memory.Mapped);
            ItemTooltip("Also reads memory-mapped files and shared sections.");
            ImGui::MenuItem("Windows Modules as Bases", nullptr, &m_memory.SystemModules);
            ItemTooltip("Lets a path start in a library of Windows itself. Off, their memory is left out altogether.");
            ImGui::Separator();
            SectionLabel("Takes effect on the next scan");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Paths"))
        {
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open...", nullptr, false, !running))
                Open();
            if (ImGui::MenuItem(ICON_FA_SAVE "  Save...", nullptr, false, !running && results))
                Save();
            ImGui::Separator();
            const bool sortable = results && !running && m_results->Count() <= Scan::kPointerSortLimit;
            if (ImGui::BeginMenu(ICON_FA_SORT_AMOUNT_DOWN "  Order", results && !running))
            {
                for (const Scan::PointerOrder order : { Scan::PointerOrder::Found, Scan::PointerOrder::Level, Scan::PointerOrder::LastOffset, Scan::PointerOrder::Base })
                {
                    if (ImGui::MenuItem(OrderName(order), nullptr, m_orderKind == order, sortable || order == Scan::PointerOrder::Found))
                        Sort(order);
                }
                if (!sortable)
                    ImGui::TextDisabled("Rescan first: more than %s paths", Grouped(Scan::kPointerSortLimit).c_str());
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Value Shown As"))
            {
                for (const Scan::ValueType type : Scan::kAllTypes)
                {
                    if (ImGui::MenuItem(Scan::TypeName(type), nullptr, m_valueType == type))
                        m_valueType = type;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_LIST "  Copy All Paths", nullptr, false, results))
                CopyAll();
            if (results && m_results->Count() > kCopiedPaths)
                ImGui::TextDisabled("The first %s", Grouped(kCopiedPaths).c_str());
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void PointerScanWindow::Inputs()
    {
        Engine& engine = m_workspace.Core();
        const bool running = m_task.Running();
        bool enter = false;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Address");
        ImGui::SameLine();
        if (m_focus)
        {
            ImGui::SetKeyboardFocusHere();
            m_focus = false;
        }
        ImGui::SetNextItemWidth(Px(220.0f));
        enter |= InputTextWithHint("##pointeraddress", "address or expression", m_address, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::TextUnformatted("Levels");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(90.0f));
        ImGui::InputInt("##pointerlevels", &m_level, 1, 1);
        m_level = std::clamp(m_level, 1, Scan::kPointerMaxLevel);
        ItemTooltip("The most pointers a path may follow. Each level more reaches further and takes longer.");
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::TextUnformatted("Largest offset");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(90.0f));
        enter |= InputTextWithHint("##pointeroffset", "1000", m_maxOffset, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsHexadecimal);
        ItemTooltip("How far into a structure a value may lie, in hex.");
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::BeginDisabled(running || !engine.HasProcess());
        if (AccentButton(ICON_FA_SEARCH "  Scan") || (enter && !running))
        {
            Start();
            if (enter && m_error.empty())
                ImGui::ClearActiveID();
        }
        ImGui::EndDisabled();
        if (!m_error.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            TextWithError(m_error, ManifoldTheme::Error);
            ImGui::PopTextWrapPos();
        }
    }

    void PointerScanWindow::RescanRow()
    {
        Engine& engine = m_workspace.Core();
        if (!m_results || m_results->Count() == 0)
            return;
        const bool running = m_task.Running();
        bool enter = false;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Keep paths that lead to");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(110.0f));
        if (ImGui::BeginCombo("##rescanby", m_rescanByValue ? "the value" : "the address"))
        {
            if (ImGui::Selectable("the address", !m_rescanByValue))
                m_rescanByValue = false;
            if (ImGui::Selectable("the value", m_rescanByValue))
                m_rescanByValue = true;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(200.0f));
        if (m_rescanByValue)
        {
            enter |= InputTextWithHint("##rescanvalue", "value", m_rescanValue, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(110.0f));
            if (ImGui::BeginCombo("##rescantype", Scan::TypeName(m_rescanType)))
            {
                for (const Scan::ValueType type : Scan::kAllTypes)
                {
                    if (ImGui::Selectable(Scan::TypeName(type), m_rescanType == type))
                        m_rescanType = type;
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            enter |= InputTextWithHint("##rescanaddress", "where the value is now", m_rescanAddress, ImGuiInputTextFlags_EnterReturnsTrue);
        }
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::BeginDisabled(running || !engine.HasProcess());
        if (ImGui::Button(ICON_FA_FILTER "  Rescan") || (enter && !running))
        {
            Rescan();
            if (enter && m_rescanError.empty())
                ImGui::ClearActiveID();
        }
        ImGui::EndDisabled();
        ItemTooltip("After the game started again, find the value once more and rescan with its new address: the paths that still lead to it are the ones to keep.");
        if (!m_rescanError.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            TextWithError(m_rescanError, ManifoldTheme::Error);
            ImGui::PopTextWrapPos();
        }
    }

    void PointerScanWindow::Status()
    {
        if (m_task.Running())
        {
            if (TaskStatus(ProgressText(), m_running != Job::Load && m_running != Job::Order && m_running != Job::Save))
                m_task.Cancel();
            return;
        }
        if (!m_summary.empty())
        {
            std::string text = m_summary;
            if (m_orderKind != Scan::PointerOrder::Found)
                text += std::string("  ·  ") + OrderName(m_orderKind);
            if (m_results && m_results->SpilledBytes() > 0)
                text += "  ·  " + ByteSize(m_results->SpilledBytes()) + " of the paths are kept in a file";
            WrappedMuted(text);
        }
    }

    void PointerScanWindow::Results()
    {
        Engine& engine = m_workspace.Core();
        const std::uint64_t count = m_results->Count();
        if (count == 0)
        {
            const std::string target = Hex(m_results->Target());
            if (m_stats.TargetOutside)
                EmptyState(ICON_FA_ROUTE, "The address was not read", target + " is not in the memory this scan read. If it lies in a read-only section, turn on Read-only memory in the Memory menu.", {}, false);
            else if (m_stats.Cancelled)
                EmptyState(ICON_FA_ROUTE, "Cancelled", "The scan was stopped before it found a path.", {}, false);
            else
                EmptyState(ICON_FA_ROUTE, "No pointer paths", "No path leads there. For a new scan, try more levels or a larger offset.", {}, false);
            return;
        }
        RefreshBases();
        const int rows = static_cast<int>(std::min<std::uint64_t>(count, kShownRows));
        if (count > static_cast<std::uint64_t>(rows))
            WrappedMuted("Showing the first " + Grouped(static_cast<std::uint64_t>(rows)) + ". Rescan to narrow the paths down.");
        if (!BeginDataTable("##pointerpaths", { { "Base address", 1.3f }, { "Offsets", 1.3f }, { "Points to", 0.9f }, { "Value", 0.8f } }))
            return;
        Scan::ValueFormat format;
        format.PointerSize = m_results->PointerSize();
        const int valueSize = std::max(1, Scan::ValueSize(m_valueType, format));
        ImGuiListClipper clipper;
        clipper.Begin(rows);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                Scan::PointerPath path;
                if (!PathAt(row, path))
                    continue;
                const std::optional<std::uint64_t> address = Resolve(path);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                PushMono();
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
                if (ImGui::Selectable(m_results->BaseText(path).c_str(), row == m_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selected = row;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        AddToList(path);
                }
                ImGui::PopStyleColor();
                PopMono();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s\nDouble-click adds it to the address list.", m_results->PathText(path).c_str());
                if (ImGui::BeginPopupContextItem("##pointerpathmenu"))
                {
                    m_selected = row;
                    if (ImGui::MenuItem(ICON_FA_PLUS "  Add the Path to the Address List"))
                        AddToList(path);
                    if (ImGui::MenuItem(ICON_FA_MEMORY "  Show Where It Points in the Hex View", nullptr, false, address.has_value()))
                        m_workspace.OpenMemoryViewer(*address, true);
                    if (ImGui::MenuItem(ICON_FA_SITEMAP "  Dissect the Structure It Points Into", nullptr, false, address.has_value() && path.Count > 0))
                        m_workspace.OpenStructure(*address - path.Offsets[path.Count - 1]);
                    if (ImGui::MenuItem(ICON_FA_CODE "  Show the Base Address in the Hex View", nullptr, false, path.Module < m_bases.size() && m_bases[path.Module] != 0))
                        m_workspace.OpenMemoryViewer(m_bases[path.Module] + path.ModuleOffset, true);
                    ImGui::Separator();
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Path"))
                        ImGui::SetClipboardText(m_results->PathText(path).c_str());
                    if (ImGui::MenuItem(ICON_FA_LIST "  Copy All Paths"))
                        CopyAll();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                PushMono();
                std::string offsets;
                for (int index = 0; index < path.Count; ++index)
                    offsets += (index ? "  " : "") + Hex(path.Offsets[index]);
                ImGui::TextUnformatted(offsets.c_str());
                ImGui::TableNextColumn();
                if (!address)
                    ImGui::TextDisabled(path.Module < m_bases.size() && m_bases[path.Module] ? "broken" : "module not loaded");
                else if (m_results->Target() && *address != m_results->Target())
                    TextWithError(Hex(*address), ManifoldTheme::Warning);
                else
                    ImGui::TextUnformatted(Hex(*address).c_str());
                ImGui::TableNextColumn();
                std::uint8_t bytes[8] = {};
                if (address && engine.Process()->ReadExact(*address, bytes, static_cast<std::size_t>(std::min(valueSize, 8))))
                    ImGui::TextUnformatted(Scan::FormatValue(m_valueType, std::span<const std::uint8_t>(bytes, static_cast<std::size_t>(std::min(valueSize, 8))), format).c_str());
                else
                    ImGui::TextDisabled("??");
                PopMono();
            }
        }
        ImGui::PopItemFlag();
        ImGui::EndTable();
    }

    void PointerScanWindow::Draw()
    {
        Collect();
        if (!BeginWindow(ICON_FA_ROUTE "  Pointer Scan###pointerscan", ImVec2(860.0f, 580.0f), ImGuiWindowFlags_MenuBar, ImVec2(720.0f, 320.0f)))
            return;
        Engine& engine = m_workspace.Core();
        MenuBar();
        if (!engine.HasProcess() && !m_task.Running() && !m_results)
        {
            m_workspace.NoProcessState(ICON_FA_ROUTE, "Open a process to scan for pointer paths, or open saved paths from the Paths menu.");
            EndToolWindow();
            return;
        }
        Inputs();
        RescanRow();
        Status();
        ImGui::Spacing();
        if (m_results && m_scanned && !m_task.Running())
            Results();
        else if (m_results && m_task.Running() && (m_running == Job::Order || m_running == Job::Save))
            Results();
        else if (!m_scanned && !m_task.Running())
            WrappedMuted("Finds chains of pointers that lead from a module to an address, so the value can be found again after the game starts anew: static address, then one offset per pointer. "
                  "Reads the whole process once, so it takes memory in proportion to the pointers the game holds. For one level only, Find static pointers is quicker.");
        EndToolWindow();
    }
}
