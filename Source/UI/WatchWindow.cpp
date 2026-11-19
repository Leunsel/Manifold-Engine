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
        m_title = std::string(icon) + "  Find out what " + Debug::WatchKindName(kind) + " " + m_engine.Describe(address) + "###watch" + std::to_string(watchId);
    }

    WatchWindow::~WatchWindow()
    {
        m_engine.Debugger().RemoveWatch(m_watchId);
    }

    void WatchWindow::Draw()
    {
        if (!m_open)
            return;
        if (!BeginToolWindow(m_title.c_str(), &m_open, ImVec2(720.0f, 460.0f), 0, ImVec2(520.0f, 340.0f)))
        {
            EndToolWindow();
            return;
        }
        Debug::Debugger& debugger = m_engine.Debugger();
        const bool known = debugger.CopyWatch(m_watchId, m_copy);
        const bool running = known && m_copy.Running && debugger.Attached();
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
        if (ImGui::Button(ICON_FA_ERASER "  Clear"))
        {
            debugger.ClearWatch(m_watchId);
            m_selected = -1;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Muted(Grouped(m_copy.Hits) + " hits  ·  " + std::to_string(m_copy.Entries.size()) + (m_kind == Debug::WatchKind::InstructionAccesses ? " addresses" : " instructions") +
              (m_copy.Slot >= 0 ? "  ·  debug register " + std::to_string(m_copy.Slot) : "  ·  software breakpoint"));
        if (const int hidden = debugger.HiddenThreads(); hidden > 0 && m_copy.Slot >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
            ImGui::TextWrapped("%s", (std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + (hidden == 1 ? "1 thread hides" : std::to_string(hidden) + " threads hide") +
                                      " from debuggers" + (debugger.MainThreadHidden() ? ", the main thread among them" : "") +
                                      ". Accesses from hidden threads are not listed here; the game keeps running.").c_str());
            ImGui::PopStyleColor();
        }

        const float detailHeight = m_selected >= 0 ? Px(150.0f) : 0.0f;
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        const bool byAddress = m_kind == Debug::WatchKind::InstructionAccesses;
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
            if (ImGui::SmallButton(ICON_FA_CODE "  Show in disassembler"))
                m_workspace.OpenMemoryViewer(entry.Instruction);
            ImGui::SameLine();
            if (byAddress)
            {
                if (ImGui::SmallButton(ICON_FA_PLUS "  Add address to the list"))
                    m_workspace.AddAddress(entry.Accessed, Scan::ValueType::FourBytes);
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_SITEMAP "  Dissect"))
                    m_workspace.OpenStructure(entry.Accessed);
            }
            else
            {
                if (m_kind != Debug::WatchKind::Calls && ImGui::SmallButton(ICON_FA_LIST_UL "  What addresses does it access?"))
                    m_workspace.StartWatch(Debug::WatchKind::InstructionAccesses, entry.Instruction, 1);
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_MAGIC "  AOB injection script"))
                {
                    m_workspace.LastCodeAddress = entry.Instruction;
                    m_workspace.OpenMemoryViewer(entry.Instruction);
                }
                ItemTooltip("Opens the instruction in the memory viewer; right click it there and pick Create script.");
            }
            PushMono();
            const Disasm::RegisterValues& r = entry.Registers;
            const bool is64 = !m_engine.HasProcess() || m_engine.Process()->Is64Bit();
            const int digits = is64 ? 16 : 8;
            ImGui::Text("RAX %s  RBX %s  RCX %s  RDX %s", Hex(r.Rax, digits).c_str(), Hex(r.Rbx, digits).c_str(), Hex(r.Rcx, digits).c_str(), Hex(r.Rdx, digits).c_str());
            ImGui::Text("RSI %s  RDI %s  RBP %s  RSP %s", Hex(r.Rsi, digits).c_str(), Hex(r.Rdi, digits).c_str(), Hex(r.Rbp, digits).c_str(), Hex(r.Rsp, digits).c_str());
            if (is64)
            {
                ImGui::Text("R8  %s  R9  %s  R10 %s  R11 %s", Hex(r.R8, 16).c_str(), Hex(r.R9, 16).c_str(), Hex(r.R10, 16).c_str(), Hex(r.R11, 16).c_str());
                ImGui::Text("R12 %s  R13 %s  R14 %s  R15 %s", Hex(r.R12, 16).c_str(), Hex(r.R13, 16).c_str(), Hex(r.R14, 16).c_str(), Hex(r.R15, 16).c_str());
            }
            ImGui::Text("RIP %s  thread %u", Hex(r.Rip, digits).c_str(), entry.ThreadId);
            PopMono();
            EndCard();
        }
        EndToolWindow();
    }
}
