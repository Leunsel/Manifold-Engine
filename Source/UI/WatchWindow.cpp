#include "UI/WatchWindow.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <cstring>

namespace Manifold::UI
{
    WatchWindow::WatchWindow(Workspace& workspace, int watchId, Debug::WatchKind kind, std::uint64_t address, int size)
        : m_workspace(workspace), m_engine(workspace.Core()), m_watchId(watchId), m_kind(kind), m_address(address), m_size(size)
    {
        const char* icon = kind == Debug::WatchKind::Writes ? ICON_FA_PEN : kind == Debug::WatchKind::Calls ? ICON_FA_PHONE : kind == Debug::WatchKind::InstructionAccesses ? ICON_FA_LIST_UL : ICON_FA_EYE;
        const char* action = kind == Debug::WatchKind::Writes ? "Writes To" : kind == Debug::WatchKind::Calls ? "Calls" : kind == Debug::WatchKind::InstructionAccesses ? "Addresses This Instruction Accesses" : "Accesses";
        m_title = std::string(icon) + "  Find Out What " + action + " " + m_engine.Describe(address) + "###watch" + std::to_string(watchId);
    }

    WatchWindow::~WatchWindow()
    {
        m_engine.Debugger().RemoveWatch(m_watchId);
    }

    void WatchWindow::Rearm()
    {
        int id = m_watchId;
        if (m_engine.Debugger().StartWatch(m_kind, m_address, m_size, id))
            m_watchId = id;
    }

    void WatchWindow::Draw()
    {
        if (!m_open)
            return;
        if (!BeginToolWindow(m_title.c_str(), &m_open, ImVec2(720.0f, 460.0f), ImGuiWindowFlags_MenuBar, ImVec2(520.0f, 340.0f)))
        {
            EndToolWindow();
            return;
        }
        Debug::Debugger& debugger = m_engine.Debugger();
        const bool known = debugger.CopyWatch(m_watchId, m_copy);
        const bool running = known && m_copy.Running && debugger.Attached();
        const bool byAddress = m_kind == Debug::WatchKind::InstructionAccesses;
        if (m_selected >= static_cast<int>(m_copy.Entries.size()))
            m_selected = -1;
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("Watch"))
            {
                if (ImGui::MenuItem(ICON_FA_STOP "  Stop", nullptr, false, running))
                    debugger.StopWatch(m_watchId);
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy the List", nullptr, false, !m_copy.Entries.empty()))
                    CopyList();
                ItemTooltip(byAddress ? "Count and address of every row, one per line, tab-separated." : "Count, address and instruction of every row, one per line, tab-separated.");
                if (ImGui::MenuItem(ICON_FA_ERASER "  Clear the List", nullptr, false, !m_copy.Entries.empty()))
                {
                    debugger.ClearWatch(m_watchId);
                    m_selected = -1;
                }
                ItemTooltip("Forget the rows and counts so far. The watch keeps running.");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(byAddress ? "Address" : "Instruction", m_selected >= 0))
            {
                if (m_selected >= 0)
                    EntryMenu(m_copy.Entries[static_cast<std::size_t>(m_selected)]);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        if (running)
        {
            if (DangerButton(ICON_FA_STOP "  Stop", Px(90.0f, 0.0f)))
                debugger.StopWatch(m_watchId);
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::Button(ICON_FA_STOP "  Stopped", Px(90.0f, 0.0f));
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Muted(Grouped(m_copy.Hits) + " hits  ·  " + std::to_string(m_copy.Entries.size()) + (m_kind == Debug::WatchKind::InstructionAccesses ? " addresses" : " instructions") +
              (m_copy.Slot >= 0 ? "  ·  debug register " + std::to_string(m_copy.Slot) : "  ·  software breakpoint"));
        if (const int hidden = debugger.HiddenThreads(); hidden > 0 && m_copy.Slot >= 0)
        {
            const bool mainHidden = debugger.MainThreadHidden();
            const bool onWindows = !debugger.UsingVeh();
            ImGui::Spacing();
            if (mainHidden && onWindows && m_engine.Options().UseInProcessForHiddenThreads)
            {
                // The user asked Manifold to make the move itself; do it quietly.
                m_workspace.UseVehForWatches();
            }
            else if (mainHidden && onWindows)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
                ImGui::TextWrapped("%s", (std::string(ICON_FA_LIGHTBULB "  ")
                    + "The main thread hides from the Windows debugger, so what it touches doesn't show up here. "
                      "The in-process debugger can follow hidden threads - switch this watch over to catch them.").c_str());
                ImGui::PopStyleColor();
                if (ImGui::Button(ICON_FA_EXCHANGE_ALT "  Switch to the In-Process Debugger"))
                    m_workspace.UseVehForWatches();
                ItemTooltip("Re-runs this watch through the in-process (VEH) debugger, which sees threads that hide from the Windows debugger. It attaches from inside the process.");
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                ImGui::TextWrapped("%s", (std::string(ICON_FA_INFO_CIRCLE "  ") + (hidden == 1 ? "1 thread hides" : std::to_string(hidden) + " threads hide")
                    + " from the Windows debugger, so some accesses aren't listed here.").c_str());
                ImGui::PopStyleColor();
            }
        }

        const float detailHeight = m_selected >= 0 ? Px(150.0f) : 0.0f;
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        PushMono();
        if (ImGui::BeginTable("##entries", 3, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y - detailHeight)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
            ImGui::TableSetupColumn(byAddress ? "Address" : m_kind == Debug::WatchKind::Calls ? "Caller" : "Instruction", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn(byAddress ? "Value" : "Code", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(m_copy.Entries.size()); ++i)
            {
                const Debug::WatchEntry& entry = m_copy.Entries[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(Grouped(entry.Count).c_str(), m_selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        if (byAddress)
                            m_workspace.OpenMemoryViewer(entry.Accessed, true);
                        else
                            m_workspace.OpenMemoryViewer(entry.Instruction);
                    }
                }
                if (ImGui::BeginPopupContextItem("##entrymenu"))
                {
                    m_selected = i;
                    PopMono();
                    EntryMenu(entry);
                    PushMono();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                const std::uint64_t shown = byAddress ? entry.Accessed : entry.Instruction;
                ImGui::TextColored(ManifoldTheme::AddressText, "%s", m_engine.Describe(shown).c_str());
                ImGui::TableNextColumn();
                if (byAddress)
                {
                    std::uint8_t bytes[8]{};
                    if (m_engine.HasProcess() && m_engine.Process()->ReadExact(entry.Accessed, bytes, sizeof(bytes)))
                    {
                        std::uint32_t value = 0;
                        std::memcpy(&value, bytes, 4);
                        float asFloat = 0.0f;
                        std::memcpy(&asFloat, bytes, 4);
                        ImGui::Text("%u  (%s)  %s", value, Hex(value, 8).c_str(), Scan::FormatValue(Scan::ValueType::Float, std::span<const std::uint8_t>(bytes, 4), {}).c_str());
                    }
                    else
                        Muted("??");
                }
                else
                {
                    auto text = m_text.find(entry.Instruction);
                    if (text == m_text.end() && m_engine.HasProcess())
                    {
                        const Disasm::Instruction instruction = m_engine.Disassembler().Read(*m_engine.Process(), entry.Instruction, &m_engine.Symbols());
                        text = m_text.emplace(entry.Instruction, instruction.Text()).first;
                    }
                    const std::string instructionText = text == m_text.end() ? std::string("??") : text->second;
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    const std::size_t space = instructionText.find(' ');
                    const std::string mnemonic = instructionText.substr(0, space);
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    ImFont* font = ImGui::GetFont();
                    const float fontSize = ImGui::GetFontSize();
                    const ImVec4 clip(pos.x, pos.y, pos.x + ImGui::GetContentRegionAvail().x, pos.y + ImGui::GetTextLineHeight());
                    draw->AddText(font, fontSize, pos, ImGui::GetColorU32(instructionText == "??" ? ManifoldTheme::Muted : ManifoldTheme::CodeKeyword), mnemonic.c_str(), nullptr, 0.0f, &clip);
                    if (space != std::string::npos)
                        AsmOperands(draw, font, fontSize, ImVec2(pos.x + ImGui::CalcTextSize((mnemonic + " ").c_str()).x, pos.y), clip, std::string_view(instructionText).substr(space + 1));
                    ImGui::Dummy(ImVec2(ImGui::CalcTextSize(instructionText.c_str()).x, ImGui::GetTextLineHeight()));
                }
            }
            ImGui::EndTable();
        }
        PopMono();

        if (m_selected >= 0 && m_selected < static_cast<int>(m_copy.Entries.size()))
        {
            const Debug::WatchEntry& entry = m_copy.Entries[static_cast<std::size_t>(m_selected)];
            BeginCard("##details", nullptr, ImVec2(0.0f, 0.0f));
            PushMono();
            const Disasm::RegisterValues& r = entry.Registers;
            const bool is64 = !m_engine.HasProcess() || m_engine.Process()->Is64Bit();
            std::vector<std::string> lines = Disasm::RegisterLines(r, is64);
            lines.back() += Format("  thread %u", entry.ThreadId);
            for (const std::string& line : lines)
                ImGui::TextUnformatted(line.c_str());
            PopMono();
            EndCard();
        }
        EndToolWindow();
    }

    void WatchWindow::EntryMenu(const Debug::WatchEntry& entry)
    {
        if (ImGui::MenuItem(ICON_FA_CODE "  Show in the Disassembler"))
            m_workspace.OpenMemoryViewer(entry.Instruction);
        if (m_kind == Debug::WatchKind::InstructionAccesses)
        {
            if (ImGui::MenuItem(ICON_FA_TH "  Show the Address in the Hex View"))
                m_workspace.OpenMemoryViewer(entry.Accessed, true);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_PLUS "  Add the Address to the List"))
                m_workspace.AddAddress(entry.Accessed, Scan::ValueType::FourBytes);
            if (ImGui::MenuItem(ICON_FA_SITEMAP "  Dissect"))
                m_workspace.OpenStructure(entry.Accessed);
        }
        else
        {
            ImGui::Separator();
            if (m_kind != Debug::WatchKind::Calls && ImGui::MenuItem(ICON_FA_LIST_UL "  Find Out What Addresses This Instruction Accesses"))
                m_workspace.StartWatch(Debug::WatchKind::InstructionAccesses, entry.Instruction, 1);
            if (ImGui::MenuItem(ICON_FA_MAGIC "  AOB Injection Script"))
            {
                m_workspace.LastCodeAddress = entry.Instruction;
                m_workspace.OpenMemoryViewer(entry.Instruction);
            }
            ItemTooltip("Opens the instruction in the memory viewer; right click it there and pick Create script.");
        }
        ImGui::Separator();
        const std::string address = m_engine.Describe(m_kind == Debug::WatchKind::InstructionAccesses ? entry.Accessed : entry.Instruction);
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Address", address.c_str()))
            ImGui::SetClipboardText(address.c_str());
    }

    void WatchWindow::CopyList() const
    {
        const bool byAddress = m_kind == Debug::WatchKind::InstructionAccesses;
        std::string text;
        for (const Debug::WatchEntry& entry : m_copy.Entries)
        {
            text += std::to_string(entry.Count) + "\t" + m_engine.Describe(byAddress ? entry.Accessed : entry.Instruction);
            if (!byAddress)
            {
                const auto found = m_text.find(entry.Instruction);
                if (found != m_text.end())
                    text += "\t" + found->second;
            }
            text += "\n";
        }
        ImGui::SetClipboardText(text.c_str());
    }
}
