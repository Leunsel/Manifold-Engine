#include "UI/PatchScanWindow.h"

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
        bool StillOpen(Engine& engine, const std::shared_ptr<Target::Process>& scanned)
        {
            return scanned && engine.Process() == scanned && scanned->Alive();
        }

        std::string ShortBytes(std::span<const std::uint8_t> bytes, std::size_t limit)
        {
            std::string text = HexBytes(bytes.first(std::min(bytes.size(), limit)));
            if (bytes.size() > limit)
                text += " ...";
            return text;
        }

        std::string ListingText(const std::vector<Disasm::Instruction>& listing)
        {
            std::string text;
            for (const Disasm::Instruction& instruction : listing)
            {
                if (!text.empty())
                    text += "\n";
                text += instruction.Length ? instruction.Text() : "??";
            }
            return text;
        }
    }

    PatchScanWindow::PatchScanWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    PatchScanWindow::~PatchScanWindow() = default;

    void PatchScanWindow::Start()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.Running())
            return;
        engine.Process()->RefreshModules();
        std::vector<Target::ModuleInfo> modules;
        if (const Target::ModuleList list = engine.Process()->Modules())
        {
            for (const Target::ModuleInfo& module : *list)
            {
                if (m_windowsModules || !module.System)
                    modules.push_back(module);
            }
        }
        m_scanned = engine.Process();
        m_result.reset();
        m_restore = -1;
        std::shared_ptr<Target::Process> process = m_scanned;
        m_task.Start(engine.Work(), "Looking for patched code", [process, modules = std::move(modules)](Target::PatchScanProgress& progress)
            {
                return Target::ScanForPatches(*process, modules, &progress);
            });
    }

    std::vector<std::size_t> PatchScanWindow::VisibleRows() const
    {
        std::vector<std::size_t> visible;
        if (!m_result)
            return visible;
        visible.reserve(m_result->Patches.size());
        for (std::size_t i = 0; i < m_result->Patches.size(); ++i)
        {
            const Target::CodePatch& patch = m_result->Patches[i];
            if (m_filter.empty() || ContainsNoCase(patch.Module, m_filter) || ContainsNoCase(Hex(patch.Address), m_filter))
                visible.push_back(i);
        }
        return visible;
    }

    Status PatchScanWindow::Restore(const Target::CodePatch& patch)
    {
        Engine& engine = m_workspace.Core();
        return engine.Writes().Write(*m_scanned, engine.ProcessGeneration(), patch.Address, patch.Original, "Restored patch at " + engine.Symbols().ModuleOffset(patch.Address));
    }

    // Restores from the last address down, so a failure leaves the rows that
    // were not written in place, and the erase never moves a row still to do.
    void PatchScanWindow::RestoreAll(const std::vector<std::size_t>& rows)
    {
        Engine& engine = m_workspace.Core();
        if (!m_result || !StillOpen(engine, m_scanned))
            return;
        std::vector<std::size_t> order = rows;
        std::sort(order.rbegin(), order.rend());
        int restored = 0;
        int skipped = 0;
        std::uint64_t bytes = 0;
        std::string firstError;
        for (const std::size_t index : order)
        {
            if (index >= m_result->Patches.size())
                continue;
            const Target::CodePatch patch = m_result->Patches[index];
            if (!patch.Complete())
            {
                ++skipped;
                continue;
            }
            const Status written = Restore(patch);
            if (!written)
            {
                ++skipped;
                if (firstError.empty())
                    firstError = engine.Symbols().ModuleOffset(patch.Address) + ": " + written.Text;
                continue;
            }
            ++restored;
            bytes += patch.Size;
            m_result->Patches.erase(m_result->Patches.begin() + static_cast<std::ptrdiff_t>(index));
        }
        std::string text = "Restored " + std::to_string(restored) + (restored == 1 ? " patch" : " patches") + " (" + Grouped(bytes) + " bytes).";
        if (skipped)
            text += " " + std::to_string(skipped) + " could not be restored" + (firstError.empty() ? " (too large)." : ", first: " + firstError);
        LOG_INFO("Patches", text);
        engine.Notify(skipped ? LogLevel::Warning : LogLevel::Success, text + (restored ? " Tools > Write history can undo them." : ""));
    }

    void PatchScanWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        Engine& engine = m_workspace.Core();
        const bool running = m_task.Running();
        const bool live = m_result && StillOpen(engine, m_scanned);
        if (ImGui::BeginMenu("Scan"))
        {
            if (ImGui::MenuItem(ICON_FA_SEARCH "  Scan again", nullptr, false, !running && engine.HasProcess()))
                Start();
            ImGui::Separator();
            ImGui::MenuItem("Include Windows modules", nullptr, &m_windowsModules);
            ItemTooltip("Also compare the libraries of Windows itself. Overlays and anti-cheat hook these, so expect results there. Takes effect on the next scan.");
            ImGui::EndMenu();
        }
        const std::vector<std::size_t> rows = VisibleRows();
        const std::size_t restorable = static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [&](std::size_t i) { return m_result->Patches[i].Complete(); }));
        if (ImGui::BeginMenu("Restore", live && !rows.empty()))
        {
            const bool filtered = m_result && rows.size() != m_result->Patches.size();
            const std::string label = std::string(ICON_FA_UNDO "  Restore ") + (filtered ? "the " + std::to_string(restorable) + " shown" : "all " + std::to_string(restorable)) + "...";
            if (DangerMenuItem(label.c_str(), nullptr, restorable > 0))
            {
                const std::string text = "Writes the original bytes from the files on disk back over " + std::to_string(restorable) + (restorable == 1 ? " patch" : " patches") +
                                         (filtered ? " that match the filter" : "") + ".\n\nChanges made by active scripts should be undone by turning the scripts off instead: "
                                         "a script that later turns itself off writes over the restored bytes. Each restore can be undone in Tools > Write history.";
                m_workspace.Confirm("Restore patches", text, [this, rows]() { RestoreAll(rows); }, "Restore");
            }
            if (restorable < rows.size())
                ImGui::TextDisabled("%d too large to restore here", static_cast<int>(rows.size() - restorable));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Copy", m_result && !rows.empty()))
        {
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy the list"))
            {
                std::string text;
                for (const std::size_t i : rows)
                {
                    const Target::CodePatch& patch = m_result->Patches[i];
                    text += patch.Module + "+" + Hex(patch.Offset) + "\t" + std::to_string(patch.Size) + "\t" + HexBytes(patch.Current) + "\t" + HexBytes(patch.Original) + "\n";
                }
                ImGui::SetClipboardText(text.c_str());
            }
            ItemTooltip("Module+offset, size, current and original bytes, one patch per line, tab-separated.");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void PatchScanWindow::Collect()
    {
        std::optional<Target::PatchScanResult> collected = m_task.Collect();
        if (!collected)
            return;
        m_result = std::move(collected);
        m_seconds = m_task.Seconds();
        const std::size_t count = m_result->Patches.size();
        LOG_INFO("Patches", std::to_string(count) + (count == 1 ? " patch in " : " patches in ") + std::to_string(m_result->Modules.size()) + " modules");
    }

    std::vector<Disasm::Instruction> PatchScanWindow::Listing(std::uint64_t address, std::span<const std::uint8_t> bytes, std::uint64_t size, int limit) const
    {
        std::vector<Disasm::Instruction> listing;
        Engine& engine = m_workspace.Core();
        std::size_t at = 0;
        while (at < bytes.size() && at < size && static_cast<int>(listing.size()) < limit)
        {
            Disasm::Instruction instruction = engine.Disassembler().Decode(address + at, bytes.data() + at, bytes.size() - at, &engine.Symbols());
            listing.push_back(instruction);
            at += instruction.Length ? instruction.Length : 1;
        }
        return listing;
    }

    void PatchScanWindow::Notes()
    {
        std::vector<const Target::ModulePatchSummary*> notes;
        for (const Target::ModulePatchSummary& summary : m_result->Modules)
        {
            if (summary.TooMany || !summary.Problem || summary.UnreadableBytes)
                notes.push_back(&summary);
        }
        if (notes.empty())
            return;
        const std::size_t shown = m_allNotes ? notes.size() : std::min<std::size_t>(notes.size(), 3);
        for (std::size_t i = 0; i < shown; ++i)
        {
            const Target::ModulePatchSummary& summary = *notes[i];
            std::string text;
            if (!summary.Problem)
                text = summary.Problem.Text;
            else if (summary.TooMany)
                text = Grouped(summary.ChangedBytes) + " bytes differ. The code is probably packed or decrypted at start, so only the first " +
                       std::to_string(Target::kMaxPatchesPerModule) + " places are listed.";
            else
                text = ByteSize(summary.UnreadableBytes) + " of code could not be read.";
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
            ImGui::TextUnformatted(ICON_FA_EXCLAMATION_TRIANGLE);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, Px(6.0f));
            ImGui::TextUnformatted(summary.Module.c_str());
            ImGui::SameLine(0.0f, Px(8.0f));
            ImGui::PushTextWrapPos(0.0f);
            Muted(text);
            ImGui::PopTextWrapPos();
        }
        if (notes.size() > 3)
        {
            if (ImGui::SmallButton(m_allNotes ? "Show fewer" : ("Show " + std::to_string(notes.size() - 3) + " more").c_str()))
                m_allNotes = !m_allNotes;
        }
        ImGui::Spacing();
    }

    void PatchScanWindow::PatchRow(std::size_t index)
    {
        Engine& engine = m_workspace.Core();
        const Target::CodePatch& patch = m_result->Patches[index];
        std::vector<std::uint8_t> now(static_cast<std::size_t>(std::min<std::uint64_t>(patch.Size, 64)) + 16);
        const bool live = StillOpen(engine, m_scanned);
        const std::size_t read = live ? m_scanned->Read(patch.Address, now.data(), now.size()) : 0;
        now.resize(read);
        std::vector<std::uint8_t> was(patch.Original.begin(), patch.Original.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(patch.Original.size(), 64)));
        if (now.size() > was.size())
            was.insert(was.end(), now.begin() + static_cast<std::ptrdiff_t>(was.size()), now.end());
        const std::vector<Disasm::Instruction> nowListing = Listing(patch.Address, now, std::max<std::uint64_t>(patch.Size, 1), 8);
        const std::vector<Disasm::Instruction> wasListing = Listing(patch.Address, was, std::max<std::uint64_t>(patch.Size, 1), 8);

        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextColumn();
        PushMono();
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
        const std::string where = engine.Symbols().ModuleOffset(patch.Address);
        const std::string shown = patch.Module + "+" + Hex(patch.Offset);
        if (ImGui::Selectable(shown.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            m_workspace.OpenMemoryViewer(patch.Address);
        ImGui::PopStyleColor();
        PopMono();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip())
        {
            PushMono();
            ImGui::TextColored(ManifoldTheme::Label, "Now");
            ImGui::TextUnformatted(ListingText(nowListing).c_str());
            ImGui::Spacing();
            ImGui::TextColored(ManifoldTheme::Label, "Was");
            ImGui::TextUnformatted(ListingText(wasListing).c_str());
            PopMono();
            ImGui::EndTooltip();
        }
        if (ImGui::BeginPopupContextItem("##patchmenu"))
        {
            AddressMenuItems(m_workspace, patch.Address, CodeActions);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy current bytes"))
                ImGui::SetClipboardText(HexBytes(patch.Current).c_str());
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy original bytes"))
                ImGui::SetClipboardText(HexBytes(patch.Original).c_str());
            ImGui::Separator();
            ImGui::BeginDisabled(!patch.Complete() || !live);
            if (DangerMenuItem(ICON_FA_UNDO "  Restore the original bytes"))
                m_restore = static_cast<int>(index);
            ImGui::EndDisabled();
            ItemTooltip(!live ? "The scan belongs to a process that is no longer open. Scan again first."
                : patch.Complete() ? "Writes the bytes from the file back. If an active script made this change, turn the script off instead." : "This change is too large to restore here.");
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
        PushMono();
        ImGui::TextUnformatted(std::to_string(patch.Size).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(nowListing.empty() ? "??" : nowListing.front().Text().c_str());
        if (nowListing.size() > 1)
        {
            ImGui::SameLine(0.0f, Px(6.0f));
            ImGui::TextColored(ManifoldTheme::Muted, "+%d", static_cast<int>(nowListing.size() - 1));
        }
        ImGui::TableNextColumn();
        ImGui::TextColored(ManifoldTheme::Muted, "%s", wasListing.empty() ? "??" : wasListing.front().Text().c_str());
        if (wasListing.size() > 1)
        {
            ImGui::SameLine(0.0f, Px(6.0f));
            ImGui::TextColored(ManifoldTheme::Muted, "+%d", static_cast<int>(wasListing.size() - 1));
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(ShortBytes(patch.Current, 12).c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(ManifoldTheme::Muted, "%s", ShortBytes(patch.Original, 12).c_str());
        PopMono();
        ImGui::PopID();
    }

    void PatchScanWindow::Results()
    {
        const std::vector<std::size_t> visible = VisibleRows();
        if (m_result->Patches.empty())
        {
            std::uint64_t code = 0;
            for (const Target::ModulePatchSummary& summary : m_result->Modules)
                code += summary.CodeBytes;
            const std::size_t modules = m_result->Modules.size();
            EmptyState(ICON_FA_CHECK_CIRCLE, "No patches found", "The code of " + std::to_string(modules) + (modules == 1 ? " module" : " modules") + " (" + ByteSize(code) + ") matches the files on disk.", {}, false);
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##patches", 6, flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, Px(48.0f));
            ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Was", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Current bytes", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Original bytes", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    PatchRow(visible[static_cast<std::size_t>(row)]);
            }
            ImGui::EndTable();
        }
    }

    void PatchScanWindow::Draw()
    {
        Collect();
        if (!BeginWindow(ICON_FA_BAND_AID "  Scan for patches###patchscan", ImVec2(920.0f, 560.0f), ImGuiWindowFlags_MenuBar, ImVec2(620.0f, 320.0f)))
            return;
        Engine& engine = m_workspace.Core();
        MenuBar();
        if (!engine.HasProcess() && !m_result && !m_task.Running())
        {
            m_workspace.NoProcessState(ICON_FA_BAND_AID, "Open a process to compare its code with the files on disk.");
            EndToolWindow();
            return;
        }
        const bool running = m_task.Running();
        ImGui::BeginDisabled(running || !engine.HasProcess());
        if (AccentButton(ICON_FA_SEARCH "  Scan"))
            Start();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(200.0f));
        InputTextWithHint("##filter", ICON_FA_SEARCH " Module or address", m_filter);
        ImGui::SameLine(0.0f, Px(12.0f));
        if (running)
        {
            const Target::PatchScanProgress& progress = m_task.State();
            if (TaskStatus("Module " + std::to_string(std::min(progress.Done.load() + 1, std::max(1, progress.Total.load()))) + " of " + std::to_string(progress.Total.load())))
                m_task.Cancel();
        }
        else if (m_result)
        {
            std::uint64_t code = 0;
            for (const Target::ModulePatchSummary& summary : m_result->Modules)
                code += summary.CodeBytes;
            const std::size_t count = m_result->Patches.size();
            ImGui::AlignTextToFramePadding();
            const std::size_t modules = m_result->Modules.size();
            Muted(std::to_string(count) + (count == 1 ? " patch" : " patches") + "  ·  " + std::to_string(modules) + (modules == 1 ? " module  ·  " : " modules  ·  ") +
                  ByteSize(code) + " of code  ·  " + Format("%.2f s", m_seconds));
            if (m_scanned && !StillOpen(engine, m_scanned))
            {
                ImGui::SameLine(0.0f, Px(12.0f));
                ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_EXCLAMATION_TRIANGLE "  From %s (%u), which is no longer open", m_scanned->Name().c_str(), m_scanned->Id());
            }
        }
        ImGui::Spacing();
        if (m_result)
        {
            Notes();
            Results();
        }
        else if (!running)
        {
            if (EmptyState(ICON_FA_BAND_AID, "Scan for patches",
                    "Compares the code of each module in memory with its file on disk and lists every place that differs: hooks, injected jumps and patched bytes.",
                    { { ICON_FA_SEARCH "  Scan", nullptr, true } }, false) == 0)
                Start();
        }
        if (m_restore >= 0 && m_result && static_cast<std::size_t>(m_restore) < m_result->Patches.size() && StillOpen(engine, m_scanned))
        {
            const Target::CodePatch patch = m_result->Patches[static_cast<std::size_t>(m_restore)];
            const Status written = Restore(patch);
            m_workspace.Report(written);
            if (written)
            {
                engine.Notify(LogLevel::Success, "Restored " + std::to_string(patch.Size) + " bytes at " + engine.Symbols().ModuleOffset(patch.Address) + ". Tools > Write history can undo it.");
                m_result->Patches.erase(m_result->Patches.begin() + m_restore);
            }
        }
        m_restore = -1;
        EndToolWindow();
    }
}
