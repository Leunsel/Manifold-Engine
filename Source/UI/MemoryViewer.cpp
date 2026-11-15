#include "UI/Windows.h"

#include "Asm/Assembler.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Templates.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Manifold::UI
{
    namespace
    {
        constexpr const char* kHexModes[] = { "Bytes", "2 Bytes (hex)", "4 Bytes (hex)", "8 Bytes (hex)", "4 Bytes", "Float", "Double" };

        ImU32 FlowColor(const Disasm::Instruction& instruction)
        {
            switch (instruction.Kind)
            {
            case Disasm::Flow::Jump:
            case Disasm::Flow::ConditionalJump: return ImGui::GetColorU32(ManifoldTheme::FlowJump);
            case Disasm::Flow::Call: return ImGui::GetColorU32(ManifoldTheme::FlowCall);
            case Disasm::Flow::Return: return ImGui::GetColorU32(ManifoldTheme::FlowReturn);
            case Disasm::Flow::Interrupt: return ImGui::GetColorU32(ManifoldTheme::Muted);
            default: break;
            }
            if (instruction.IsNop || !instruction.Valid)
                return ImGui::GetColorU32(ManifoldTheme::Muted);
            return ImGui::GetColorU32(ManifoldTheme::CodeKeyword);
        }

        struct RegisterRow
        {
            const char* Name;
            std::uint64_t Disasm::RegisterValues::*Field;
            bool Only64;
        };

        const RegisterRow kRegisterRows[] = {
            { "RAX", &Disasm::RegisterValues::Rax, false }, { "RBX", &Disasm::RegisterValues::Rbx, false }, { "RCX", &Disasm::RegisterValues::Rcx, false },
            { "RDX", &Disasm::RegisterValues::Rdx, false }, { "RSI", &Disasm::RegisterValues::Rsi, false }, { "RDI", &Disasm::RegisterValues::Rdi, false },
            { "RBP", &Disasm::RegisterValues::Rbp, false }, { "RSP", &Disasm::RegisterValues::Rsp, false }, { "R8", &Disasm::RegisterValues::R8, true },
            { "R9", &Disasm::RegisterValues::R9, true }, { "R10", &Disasm::RegisterValues::R10, true }, { "R11", &Disasm::RegisterValues::R11, true },
            { "R12", &Disasm::RegisterValues::R12, true }, { "R13", &Disasm::RegisterValues::R13, true }, { "R14", &Disasm::RegisterValues::R14, true },
            { "R15", &Disasm::RegisterValues::R15, true }, { "RIP", &Disasm::RegisterValues::Rip, false },
        };

        struct FlagBit
        {
            const char* Name;
            int Bit;
        };

        const FlagBit kFlags[] = { { "CF", 0 }, { "PF", 2 }, { "AF", 4 }, { "ZF", 6 }, { "SF", 7 }, { "TF", 8 }, { "DF", 10 }, { "OF", 11 } };

        void DrawSkeleton(ImVec2 min, ImVec2 max)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(min, max, true);
            const float rounding = Px(2.0f);
            const float line = ImGui::GetTextLineHeight();
            const float bar = std::round(line * 0.55f);
            const float rowHeight = std::round(line + Px(7.0f));
            const auto block = [&](float x, float y, float width, ImVec4 color, float alpha) {
                const float top = std::round(y + (rowHeight - bar) * 0.5f);
                draw->AddRectFilled(ImVec2(std::round(x), top), ImVec2(std::round(x + width), top + bar), ImGui::GetColorU32(ManifoldTheme::WithAlpha(color, alpha)), rounding);
            };

            const float left = min.x + Px(10.0f);
            const float bytesX = left + Px(118.0f);
            const float opcodeX = left + Px(262.0f);
            const float commentX = left + Px(560.0f);
            const float header = ImGui::GetFrameHeight();
            draw->AddRectFilled(min, ImVec2(max.x, min.y + header), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.025f)), Px(4.0f));
            const float headerY = min.y + (header - rowHeight) * 0.5f;
            block(left, headerY, Px(52.0f), ManifoldTheme::Muted, 0.16f);
            block(bytesX, headerY, Px(38.0f), ManifoldTheme::Muted, 0.16f);
            block(opcodeX, headerY, Px(46.0f), ManifoldTheme::Muted, 0.16f);
            block(commentX, headerY, Px(54.0f), ManifoldTheme::Muted, 0.16f);

            const float split = std::round(min.y + (max.y - min.y) * 0.60f);
            float y = min.y + header + Px(4.0f);
            for (unsigned int row = 0; y + rowHeight <= split; ++row, y += rowHeight)
            {
                const unsigned int seed = SkeletonNoise(row + 7);
                if (row == 4)
                    draw->AddRectFilled(ImVec2(min.x, y), ImVec2(max.x, y + rowHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.05f)));
                block(left, y, Px(86.0f), ManifoldTheme::AddressText, 0.10f);
                const unsigned int bytes = 2 + seed % 5;
                for (unsigned int i = 0; i < bytes; ++i)
                    block(bytesX + static_cast<float>(i) * Px(19.0f), y, Px(14.0f), ManifoldTheme::BytesText, 0.09f);
                const bool call = seed % 7 == 0;
                const bool jump = !call && seed % 5 == 0;
                const ImVec4 mnemonic = call ? ManifoldTheme::FlowCall : (jump ? ManifoldTheme::FlowJump : ManifoldTheme::CodeKeyword);
                const float mnemonicWidth = Px(26.0f + static_cast<float>((seed >> 4) % 3) * 8.0f);
                block(opcodeX, y, mnemonicWidth, mnemonic, 0.13f);
                const float operandWidth = Px(60.0f + static_cast<float>((seed >> 8) % 9) * 16.0f);
                block(opcodeX + mnemonicWidth + Px(10.0f), y, operandWidth, ManifoldTheme::Text, 0.06f);
                if ((seed >> 12) % 4 == 0)
                    block(commentX, y, Px(70.0f + static_cast<float>((seed >> 16) % 5) * 18.0f), ManifoldTheme::CodeComment, 0.09f);
            }

            draw->AddLine(ImVec2(min.x, split + Px(4.0f)), ImVec2(max.x, split + Px(4.0f)), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.06f)), Px(1.0f));
            y = split + Px(12.0f);
            const float cell = Px(14.0f);
            const float cellStep = Px(21.0f);
            const float hexX = left + Px(118.0f);
            const float asciiX = hexX + cellStep * 16.0f + Px(16.0f);
            for (unsigned int row = 0; y < max.y; ++row, y += rowHeight)
            {
                const unsigned int seed = SkeletonNoise(row + 101);
                block(left, y, Px(86.0f), ManifoldTheme::AddressText, 0.10f);
                for (unsigned int i = 0; i < 16; ++i)
                {
                    const bool zero = (SkeletonNoise(seed + i) % 3) == 0;
                    block(hexX + static_cast<float>(i) * cellStep + (i >= 8 ? Px(6.0f) : 0.0f), y, cell, ManifoldTheme::BytesText, zero ? 0.05f : 0.09f);
                }
                block(asciiX, y, Px(120.0f), ManifoldTheme::Text, 0.05f);
            }

            const ImVec4 page = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
            const float fade = std::round(min.y + (max.y - min.y) * 0.35f);
            draw->AddRectFilledMultiColor(ImVec2(min.x, fade), max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(page, 0.0f)), ImGui::GetColorU32(ManifoldTheme::WithAlpha(page, 0.0f)),
                ImGui::GetColorU32(ManifoldTheme::WithAlpha(page, 0.92f)), ImGui::GetColorU32(ManifoldTheme::WithAlpha(page, 0.92f)));
            draw->PopClipRect();
        }
    }

    MemoryViewer::MemoryViewer(Workspace& workspace, int number) : m_workspace(workspace), m_engine(workspace.Core()), m_number(number)
    {
        m_title = NextWindowTitle(ICON_FA_MEMORY "  Memory Viewer", number);
        m_followBreaks = number == 1;
    }

    void MemoryViewer::Show(std::uint64_t address, bool hexView)
    {
        m_open = true;
        m_bringToFront = true;
        if (address == 0)
            return;
        if (hexView)
        {
            m_hexTop = address & ~0xFull;
            m_hexSelected = address;
            m_hexFocus = true;
        }
        else
        {
            Navigate(address);
            m_hexTop = address & ~0xFull;
        }
    }

    void MemoryViewer::Navigate(std::uint64_t address, bool remember)
    {
        if (remember && m_selected && m_selected != address)
        {
            m_history.push_back(m_selected);
            if (m_history.size() > 100)
                m_history.erase(m_history.begin());
        }
        m_selected = address;
        m_selectionEnd = address;
        m_top = m_engine.HasProcess() ? m_engine.Disassembler().BackUp(*m_engine.Process(), address, 4) : address;
        m_workspace.LastCodeAddress = address;
    }

    int MemoryViewer::HexValueSize() const
    {
        switch (m_hexDisplay)
        {
        case 1: return 2;
        case 2: return 4;
        case 3: return 8;
        case 4: return 4;
        case 5: return 4;
        case 6: return 8;
        default: return 1;
        }
    }

    void MemoryViewer::Draw()
    {
        if (!m_open)
            return;
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (m_workspace.LastCodeAddress == m_selected)
        {
            m_engine.SelectedCodeAddress = m_selected;
            if (m_hexSelected)
                m_engine.SelectedHexAddress = m_hexSelected;
        }
        Debug::Debugger& debugger = m_engine.Debugger();
        const Debug::BreakInfo info = debugger.Break();
        if (m_followBreaks && info.Broken && info.Serial != m_lastBreak)
        {
            m_lastBreak = info.Serial;
            Navigate(info.Address);
            ImGui::SetNextWindowFocus();
        }
        if (!BeginToolWindow(m_title.c_str(), &m_open, ImVec2(980.0f, 680.0f), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(700.0f, 400.0f)))
        {
            EndToolWindow();
            return;
        }
        if (!m_engine.HasProcess())
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            DrawSkeleton(origin, ImVec2(origin.x + available.x, origin.y + available.y));
            m_workspace.NoProcessState(ICON_FA_MEMORY, "Open a process to read its code and memory here, set breakpoints and step through it.");
            EndToolWindow();
            return;
        }
        if (m_top == 0)
        {
            if (const auto main = m_engine.Process()->MainModule())
                Navigate(main->Base + 0x1000, false);
            m_hexTop = m_top & ~0xFull;
        }

        Toolbar();
        const float available = ImGui::GetContentRegionAvail().y;
        float code = std::floor(available * m_codeShare);
        float hex = available - code - Px(6.0f);
        const ImVec2 start = ImGui::GetCursorPos();
        const bool showRegisters = info.Broken && m_engine.Options().ShowRegistersOnBreak;
        const float registersWidth = showRegisters ? Px(250.0f) : 0.0f;
        const ImVec2 codeStart = ImGui::GetCursorPos();
        ImGui::BeginChild("##codearea", ImVec2(ImGui::GetContentRegionAvail().x - registersWidth - (showRegisters ? Px(6.0f) : 0.0f), code), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Disassembly(code);
        ImGui::EndChild();
        if (showRegisters)
        {
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - registersWidth, codeStart.y));
            Registers(registersWidth, code);
        }
        ImGui::SetCursorPos(ImVec2(start.x, start.y + code));
        if (Splitter("##viewsplit", false, Px(6.0f), &code, &hex, Px(120.0f), Px(90.0f)))
            m_codeShare = code / available;
        ImGui::SetCursorPos(ImVec2(start.x, start.y + code + Px(6.0f)));
        HexView(ImGui::GetContentRegionAvail().y);

        GotoDialog();
        AssembleDialog();
        FindDialog();
        AllocateDialog();
        InjectDialog();
        EndToolWindow();
    }

    void MemoryViewer::Toolbar()
    {
        Debug::Debugger& debugger = m_engine.Debugger();
        const bool broken = debugger.Broken();
        if (IconButton(ICON_FA_ARROW_LEFT, "Back (Backspace)") && !m_history.empty())
        {
            const std::uint64_t back = m_history.back();
            m_history.pop_back();
            Navigate(back, false);
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_LOCATION_ARROW, "Go to address (Ctrl+G)"))
        {
            m_gotoOpen = true;
            m_gotoHex = false;
            m_gotoText = Hex(m_selected);
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_SEARCH, "Find bytes (Ctrl+F)"))
            m_findOpen = true;
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::BeginDisabled(!broken);
        if (IconButton(ICON_FA_PLAY, "Run (F9)"))
            debugger.Resume(Debug::ResumeMode::Continue);
        ImGui::SameLine();
        if (IconButton(ICON_FA_LEVEL_DOWN_ALT, "Step into (F7)"))
            debugger.Resume(Debug::ResumeMode::StepInto);
        ImGui::SameLine();
        if (IconButton(ICON_FA_SHARE, "Step over (F8)"))
            debugger.Resume(Debug::ResumeMode::StepOver);
        ImGui::SameLine();
        if (IconButton(ICON_FA_SIGN_OUT_ALT, "Run until return (Shift+F8)"))
            debugger.Resume(Debug::ResumeMode::RunToReturn);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!debugger.Attached() || broken);
        if (IconButton(ICON_FA_PAUSE, "Break into the process"))
            m_workspace.Report(debugger.BreakNow());
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (IconButton(ICON_FA_CIRCLE, "Toggle breakpoint on the selected instruction (F5)"))
        {
            Status status = m_engine.EnsureDebugger();
            if (status)
                status = debugger.ToggleBreakpoint(m_selected);
            m_workspace.Report(status);
        }
        ImGui::SameLine(0.0f, Px(14.0f));
        if (IconButton(ICON_FA_CODE, "Auto Assembler (Ctrl+A)"))
            m_workspace.OpenScript(Asm::CheatTableTemplate());
        ImGui::SameLine();
        if (IconButton(ICON_FA_SITEMAP, "Dissect data at the hex view address"))
            m_workspace.OpenStructure(m_hexSelected ? m_hexSelected : m_hexTop);
        ImGui::SameLine();
        if (IconButton(ICON_FA_PUZZLE_PIECE, "Allocate memory or inject a library"))
            ImGui::OpenPopup("##toolsmenu");
        if (ImGui::BeginPopup("##toolsmenu"))
        {
            if (ImGui::MenuItem(ICON_FA_PLUS_SQUARE "  Allocate memory..."))
                m_allocateOpen = true;
            if (ImGui::MenuItem(ICON_FA_SYRINGE "  Inject library..."))
                m_injectOpen = true;
            if (ImGui::MenuItem(ICON_FA_TH_LIST "  Memory regions"))
                m_workspace.OpenRegions();
            if (ImGui::MenuItem(ICON_FA_CLONE "  New memory viewer window"))
                m_workspace.OpenMemoryViewer(m_selected, false, true);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::Checkbox("Follow breaks", &m_followBreaks);
        ItemTooltip("Jump to the instruction when the process stops at a breakpoint");

        const auto region = m_engine.Process()->RegionAt(m_selected);
        std::string where = m_engine.Describe(m_selected);
        if (region)
            where += "  ·  " + Target::ProtectText(region->Protect) + "  ·  " + Target::RegionTypeText(region->Type);
        if (broken)
            where = ICON_FA_PAUSE_CIRCLE "  " + debugger.Break().Reason + "  ·  " + where;
        const float width = ImGui::CalcTextSize(where.c_str()).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + Px(10.0f), ImGui::GetWindowContentRegionMax().x - width));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, broken ? ManifoldTheme::Warning : ManifoldTheme::Muted);
        ImGui::TextUnformatted(where.c_str());
        ImGui::PopStyleColor();
    }

    void MemoryViewer::Disassembly(float height)
    {
        const Target::Process& process = *m_engine.Process();
        const Disasm::Disassembler& disassembler = m_engine.Disassembler();
        Debug::Debugger& debugger = m_engine.Debugger();
        const Debug::BreakInfo info = debugger.Break();

        PushMono();
        const float rowHeight = std::floor(ImGui::GetTextLineHeight() + Px(3.0f));
        const float charWidth = ImGui::CalcTextSize("0").x;
        PopMono();
        const float headerHeight = std::floor(ImGui::GetTextLineHeight() + Px(8.0f));
        const float bodyHeight = std::max(rowHeight, height - headerHeight);
        m_visibleLines = std::max(4, static_cast<int>(bodyHeight / rowHeight));
        {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(m_visibleLines + 1) * 15 + 16);
            std::vector<bool> readable(buffer.size(), false);
            for (std::size_t page = 0; page < buffer.size();)
            {
                const std::uint64_t at = m_top + page;
                const std::size_t chunk = std::min<std::size_t>(buffer.size() - page, 0x1000 - static_cast<std::size_t>(at & 0xFFF));
                if (process.ReadExact(at, buffer.data() + page, chunk))
                    std::fill(readable.begin() + static_cast<std::ptrdiff_t>(page), readable.begin() + static_cast<std::ptrdiff_t>(page + chunk), true);
                page += chunk;
            }
            if (debugger.Attached())
                debugger.Unpatch(m_top, buffer.data(), buffer.size());
            m_lines = disassembler.DecodeMany(buffer, readable, m_top, m_visibleLines + 1, &m_engine.Symbols());
        }

        const ImVec2 frame = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(frame, ImVec2(frame.x + width, frame.y + height), ImGui::GetColorU32(ManifoldTheme::Card), Px(4.0f));
        const ImVec2 origin(frame.x, frame.y + headerHeight);
        const float rightX = frame.x + width;
        const float gutter = Px(18.0f);
        const float arrows = Px(26.0f);

        std::vector<std::string> addressTexts;
        std::vector<bool> addressInModule;
        addressTexts.reserve(m_lines.size());
        addressInModule.reserve(m_lines.size());
        float widestAddress = 0.0f;
        PushMono();
        for (const Disasm::Instruction& line : m_lines)
        {
            const std::string address = m_engine.Symbols().ModuleOffset(line.Address);
            const bool inModule = address.front() == '"';
            addressTexts.push_back(inModule ? ReplaceAll(address, "\"", "") : Hex(line.Address, process.Is64Bit() ? 12 : 8));
            addressInModule.push_back(inModule);
            widestAddress = std::max(widestAddress, ImGui::CalcTextSize(addressTexts.back().c_str()).x);
        }
        PopMono();
        if (m_addressProcess != process.Id())
        {
            m_addressProcess = process.Id();
            if (m_addressAuto)
                m_addressWidth = 0.0f;
        }
        const float minimumWidth = charWidth * 4.0f;
        if (m_addressAuto)
            m_addressWidth = std::max({ m_addressWidth, widestAddress + charWidth * 2.0f, charWidth * 14.0f });
        if (m_bytesWidth <= 0.0f)
            m_bytesWidth = charWidth * 24.0f;
        if (m_opcodeWidth <= 0.0f)
            m_opcodeWidth = charWidth * 44.0f;
        const float addressX = frame.x + gutter + arrows;
        const float bytesX = addressX + m_addressWidth;
        const float textX = bytesX + m_bytesWidth;
        const float commentX = textX + m_opcodeWidth;

        drawList->AddLine(ImVec2(frame.x, origin.y - 1.0f), ImVec2(rightX, origin.y - 1.0f), ImGui::GetColorU32(ManifoldTheme::RowSeparator));
        const float titleY = frame.y + std::floor((headerHeight - ImGui::GetTextLineHeight()) * 0.5f);
        const ImU32 titleColor = ImGui::GetColorU32(ManifoldTheme::Muted);
        const ImVec4 headerClip(frame.x, frame.y, rightX, origin.y);
        const struct
        {
            const char* Title;
            float Left;
            float Right;
        } titles[] = { { "Address", addressX, bytesX }, { "Bytes", bytesX, textX }, { "Opcode", textX, commentX }, { "Comment", commentX, rightX } };
        for (const auto& title : titles)
        {
            const ImVec4 clip(title.Left, headerClip.y, std::min(title.Right - Px(4.0f), rightX), headerClip.w);
            if (clip.z > clip.x)
                drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(title.Left, titleY), titleColor, title.Title, nullptr, 0.0f, &clip);
        }
        const auto separator = [&](const char* id, float x, float& columnWidth, bool address)
            {
                if (x >= rightX - Px(4.0f))
                    return;
                ImGui::SetCursorScreenPos(ImVec2(x - Px(5.0f), frame.y));
                ImGui::InvisibleButton(id, ImVec2(Px(10.0f), headerHeight));
                const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
                if (hot)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsItemActivated())
                    m_dragStartWidth = columnWidth;
                if (ImGui::IsItemActive())
                {
                    columnWidth = std::max(minimumWidth, m_dragStartWidth + ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x);
                    if (address)
                        m_addressAuto = false;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    columnWidth = 0.0f;
                    if (address)
                        m_addressAuto = true;
                }
                ItemTooltip(address ? "Drag to resize, double click to fit the addresses" : "Drag to resize, double click for the default width");
                const ImU32 color = ImGui::GetColorU32(hot ? ManifoldTheme::Accent : ManifoldTheme::RowSeparator);
                drawList->AddLine(ImVec2(x - Px(4.0f), frame.y + Px(4.0f)), ImVec2(x - Px(4.0f), origin.y - Px(4.0f)), color, hot ? Px(2.0f) : 1.0f);
            };
        separator("##addresscolumn", bytesX, m_addressWidth, true);
        separator("##bytescolumn", textX, m_bytesWidth, false);
        separator("##opcodecolumn", commentX, m_opcodeWidth, false);

        PushMono();
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        const auto cell = [&](float left, float right, float y, ImU32 color, const std::string& text)
            {
                const ImVec4 clip(left, y, std::min(right - Px(6.0f), rightX - Px(4.0f)), y + rowHeight);
                if (clip.z > clip.x)
                    drawList->AddText(font, fontSize, ImVec2(left, y), color, text.data(), text.data() + text.size(), 0.0f, &clip);
            };

        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##code", ImVec2(width, bodyHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive() || ImGui::IsItemClicked(ImGuiMouseButton_Right))
            m_focusCode = true;
        if (ImGui::IsWindowFocused() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered)
            m_focusCode = false;
        const int hoveredRow = hovered ? static_cast<int>((ImGui::GetIO().MousePos.y - origin.y) / rowHeight) : -1;
        if (hovered && hoveredRow >= 0 && hoveredRow < static_cast<int>(m_lines.size()))
        {
            const Disasm::Instruction& line = m_lines[static_cast<std::size_t>(hoveredRow)];
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                if (ImGui::GetIO().KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    m_selectionEnd = line.Address;
                else if (!(ImGui::IsMouseClicked(ImGuiMouseButton_Right) && line.Address >= std::min(m_selected, m_selectionEnd) && line.Address <= std::max(m_selected, m_selectionEnd)))
                {
                    m_selected = line.Address;
                    m_selectionEnd = line.Address;
                }
                m_workspace.LastCodeAddress = m_selected;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                const float mouseX = ImGui::GetIO().MousePos.x;
                if (mouseX < origin.x + gutter)
                {
                    Status status = m_engine.EnsureDebugger();
                    if (status)
                        status = debugger.ToggleBreakpoint(line.Address);
                    m_workspace.Report(status);
                }
                else if (mouseX >= textX && mouseX < commentX && line.Target && (line.Kind == Disasm::Flow::Jump || line.Kind == Disasm::Flow::Call || line.Kind == Disasm::Flow::ConditionalJump))
                    Navigate(*line.Target);
                else
                {
                    m_assembleOpen = true;
                    m_assembleAddress = line.Address;
                    m_assembleText = line.Valid ? line.Text() : "";
                    m_assembleError.clear();
                }
            }
        }
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
        {
            const int steps = static_cast<int>(std::round(ImGui::GetIO().MouseWheel * 3.0f));
            if (steps > 0)
                m_top = disassembler.BackUp(process, m_top, steps);
            else if (-steps < static_cast<int>(m_lines.size()))
                m_top = m_lines[static_cast<std::size_t>(-steps)].Address;
        }

        const std::uint64_t selectionLow = std::min(m_selected, m_selectionEnd);
        const std::uint64_t selectionHigh = std::max(m_selected, m_selectionEnd);
        const auto breakpoints = debugger.Breakpoints();
        drawList->PushClipRect(origin, ImVec2(origin.x + width, origin.y + bodyHeight), true);
        for (std::size_t i = 0; i < m_lines.size(); ++i)
        {
            const Disasm::Instruction& line = m_lines[i];
            const float y = origin.y + static_cast<float>(i) * rowHeight;
            if (y > origin.y + bodyHeight)
                break;
            const ImVec2 rowMin(origin.x, y);
            const ImVec2 rowMax(origin.x + width, y + rowHeight);
            const bool isIp = info.Broken && line.Address == info.Address;
            if (isIp)
                drawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.22f)));
            else if (line.Address >= selectionLow && line.Address <= selectionHigh)
                drawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ManifoldTheme::CurrentLine));
            else if (static_cast<int>(i) == hoveredRow)
                drawList->AddRectFilled(rowMin, rowMax, IM_COL32(255, 255, 255, 8));

            const bool hasBreakpoint = std::any_of(breakpoints.begin(), breakpoints.end(), [&](const Debug::Breakpoint& breakpoint) { return breakpoint.Address == line.Address; });
            const float textY = y + Px(1.5f);
            if (hasBreakpoint)
                drawList->AddCircleFilled(ImVec2(origin.x + gutter * 0.55f, y + rowHeight * 0.5f), rowHeight * 0.28f, ImGui::GetColorU32(ManifoldTheme::Breakpoint));
            if (isIp)
                drawList->AddText(ImVec2(origin.x + Px(3.0f), textY), ImGui::GetColorU32(ManifoldTheme::Warning), ICON_FA_CARET_RIGHT);
            if (m_nopped.count(line.Address))
                drawList->AddText(ImVec2(origin.x + gutter + Px(2.0f), textY), ImGui::GetColorU32(ManifoldTheme::Warning), ICON_FA_BAN);

            cell(addressX, bytesX, textY, ImGui::GetColorU32(addressInModule[i] ? ManifoldTheme::AddressText : ManifoldTheme::Text), addressTexts[i]);
            cell(bytesX, textX, textY, ImGui::GetColorU32(ManifoldTheme::BytesText), line.Mnemonic == "??" ? std::string("??") : line.BytesText());
            cell(textX, commentX, textY, FlowColor(line), line.Mnemonic);
            const float operandX = textX + charWidth * static_cast<float>(std::max<std::size_t>(line.Mnemonic.size() + 1, 7));
            {
                const ImVec4 operandClip(operandX, textY, std::min(commentX - Px(6.0f), rightX - Px(4.0f)), textY + rowHeight);
                if (operandClip.z > operandClip.x)
                    AsmOperands(drawList, font, fontSize, ImVec2(operandX, textY), operandClip, line.Operands);
            }

            std::string comment;
            if (const auto name = m_engine.Symbols().ExactName(line.Address))
                comment = *name + ":";
            else if (line.Memory.Present && line.Memory.Absolute)
            {
                std::uint8_t value[8]{};
                const int size = std::clamp(line.Memory.Size, 1, 8);
                if (process.ReadExact(*line.Memory.Absolute, value, static_cast<std::size_t>(size)))
                {
                    std::uint64_t number = 0;
                    std::memcpy(&number, value, static_cast<std::size_t>(size));
                    comment = "[" + Hex(*line.Memory.Absolute) + "]=" + Hex(number);
                }
            }
            else if (info.Broken && isIp && line.Memory.Present)
            {
                if (const auto effective = disassembler.EffectiveAddress(line, info.Registers))
                    comment = "-> " + m_engine.Describe(*effective);
            }
            if (!comment.empty())
                cell(commentX, rightX + Px(4.0f), textY, ImGui::GetColorU32(ManifoldTheme::Muted), comment);

            if (line.Target && (line.Kind == Disasm::Flow::Jump || line.Kind == Disasm::Flow::ConditionalJump))
            {
                for (std::size_t j = 0; j < m_lines.size(); ++j)
                {
                    if (m_lines[j].Address != *line.Target)
                        continue;
                    const bool focused = line.Address >= selectionLow && line.Address <= selectionHigh;
                    const float depth = Px(6.0f) + static_cast<float>((i * 7 + j * 3) % 4) * Px(4.0f);
                    const float x = addressX - depth;
                    const float y1 = y + rowHeight * 0.5f;
                    const float y2 = origin.y + static_cast<float>(j) * rowHeight + rowHeight * 0.5f;
                    const ImU32 color = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::FlowJump, focused ? 0.95f : 0.35f));
                    const float thickness = focused ? Px(1.6f) : Px(1.0f);
                    drawList->AddLine(ImVec2(addressX - Px(2.0f), y1), ImVec2(x, y1), color, thickness);
                    drawList->AddLine(ImVec2(x, y1), ImVec2(x, y2), color, thickness);
                    drawList->AddLine(ImVec2(x, y2), ImVec2(addressX - Px(2.0f), y2), color, thickness);
                    drawList->AddTriangleFilled(ImVec2(addressX - Px(1.0f), y2), ImVec2(addressX - Px(5.0f), y2 - Px(3.0f)), ImVec2(addressX - Px(5.0f), y2 + Px(3.0f)), color);
                    break;
                }
            }
        }
        drawList->PopClipRect();
        PopMono();

        if (hoveredRow >= 0 && hoveredRow < static_cast<int>(m_lines.size()) && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && hovered)
            ImGui::OpenPopup("##codemenu");
        if (ImGui::BeginPopup("##codemenu"))
        {
            const Disasm::Instruction selected = disassembler.Read(process, m_selected, &m_engine.Symbols());
            CodeMenu(selected);
            ImGui::EndPopup();
        }
        if (m_focusCode && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            HandleCodeKeys();
    }

    void MemoryViewer::HandleCodeKeys()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            return;
        const Target::Process& process = *m_engine.Process();
        const Disasm::Disassembler& disassembler = m_engine.Disassembler();
        Debug::Debugger& debugger = m_engine.Debugger();
        const auto ensureVisible = [&]()
            {
                const bool visible = !m_lines.empty() && m_selected >= m_lines.front().Address && static_cast<int>(m_lines.size()) > 2 &&
                                     m_selected < m_lines[std::min<std::size_t>(m_lines.size() - 2, static_cast<std::size_t>(m_visibleLines - 1))].Address;
                if (!visible)
                    m_top = disassembler.BackUp(process, m_selected, std::max(1, m_visibleLines / 3));
            };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        {
            m_selected = disassembler.Read(process, m_selected, nullptr).Next();
            if (!io.KeyShift)
                m_selectionEnd = m_selected;
            ensureVisible();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        {
            const auto previous = disassembler.PreviousStart(process, m_selected);
            m_selected = previous.value_or(m_selected - 1);
            if (!io.KeyShift)
                m_selectionEnd = m_selected;
            ensureVisible();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && m_lines.size() > 2)
        {
            m_top = m_lines[m_lines.size() - 2].Address;
            m_selected = m_selectionEnd = m_top;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
        {
            m_top = disassembler.BackUp(process, m_top, std::max(1, m_visibleLines - 1));
            m_selected = m_selectionEnd = m_top;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
        {
            const Disasm::Instruction instruction = disassembler.Read(process, m_selected, nullptr);
            if (instruction.Target)
                Navigate(*instruction.Target);
            else if (instruction.Memory.Absolute)
            {
                m_hexTop = *instruction.Memory.Absolute & ~0xFull;
                m_hexSelected = *instruction.Memory.Absolute;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && !m_history.empty())
        {
            const std::uint64_t back = m_history.back();
            m_history.pop_back();
            Navigate(back, false);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false))
        {
            const Disasm::Instruction instruction = disassembler.Read(process, m_selected, nullptr);
            m_assembleOpen = true;
            m_assembleAddress = m_selected;
            m_assembleText = instruction.Valid ? instruction.Text() : "";
            m_assembleError.clear();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
        {
            m_gotoOpen = true;
            m_gotoHex = false;
            m_gotoText = Hex(m_selected);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
            m_findOpen = true;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
            m_workspace.OpenScript(Asm::CheatTableTemplate());
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
        {
            std::string text;
            for (const Disasm::Instruction& instruction : disassembler.ReadMany(process, std::min(m_selected, m_selectionEnd), 512, &m_engine.Symbols()))
            {
                if (instruction.Address > std::max(m_selected, m_selectionEnd))
                    break;
                text += m_engine.Symbols().ModuleOffset(instruction.Address) + " - " + instruction.BytesText() + " - " + instruction.Text() + "\n";
            }
            ImGui::SetClipboardText(text.c_str());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
        {
            Status status = m_engine.EnsureDebugger();
            if (status)
                status = debugger.ToggleBreakpoint(m_selected);
            m_workspace.Report(status);
        }
        if (debugger.Broken())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F9, false))
                debugger.Resume(Debug::ResumeMode::Continue);
            if (ImGui::IsKeyPressed(ImGuiKey_F7, false))
                debugger.Resume(Debug::ResumeMode::StepInto);
            if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
                debugger.Resume(io.KeyShift ? Debug::ResumeMode::RunToReturn : Debug::ResumeMode::StepOver);
        }
    }

    void MemoryViewer::Nop(const Disasm::Instruction& instruction)
    {
        const Target::Process& process = *m_engine.Process();
        if (m_nopped.count(instruction.Address))
        {
            m_workspace.Report(process.Write(instruction.Address, m_nopped[instruction.Address]));
            m_nopped.erase(instruction.Address);
            return;
        }
        std::vector<std::uint8_t> original(instruction.Bytes.begin(), instruction.Bytes.begin() + instruction.Length);
        std::vector<std::uint8_t> nops(original.size(), 0x90);
        const Status status = process.Write(instruction.Address, nops);
        if (status)
            m_nopped[instruction.Address] = std::move(original);
        m_workspace.Report(status);
    }

    void MemoryViewer::CodeMenu(const Disasm::Instruction& instruction)
    {
        Debug::Debugger& debugger = m_engine.Debugger();
        const bool broken = debugger.Broken();
        ImGui::TextDisabled("%s", m_engine.Describe(instruction.Address).c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_LOCATION_ARROW "  Go to address", "Ctrl+G"))
        {
            m_gotoOpen = true;
            m_gotoHex = false;
            m_gotoText = Hex(instruction.Address);
        }
        if (instruction.Target && ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Follow", "Space"))
            Navigate(*instruction.Target);
        if (ImGui::MenuItem(ICON_FA_ARROW_LEFT "  Back", "Backspace", false, !m_history.empty()))
        {
            const std::uint64_t back = m_history.back();
            m_history.pop_back();
            Navigate(back, false);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_EDIT "  Assemble", "Enter"))
        {
            m_assembleOpen = true;
            m_assembleAddress = instruction.Address;
            m_assembleText = instruction.Valid ? instruction.Text() : "";
            m_assembleError.clear();
        }
        if (ImGui::MenuItem(m_nopped.count(instruction.Address) ? ICON_FA_UNDO "  Restore original code" : ICON_FA_BAN "  Replace with code that does nothing"))
            Nop(instruction);
        if (ImGui::BeginMenu(ICON_FA_MAGIC "  Create script"))
        {
            for (int kind = 0; kind < 3; ++kind)
            {
                if (ImGui::MenuItem(kTemplateNames[kind]))
                {
                    std::string error;
                    if (const auto script = MakeTemplate(m_engine, static_cast<TemplateKind>(kind), instruction.Address, "INJECT", error))
                        m_workspace.OpenScript(*script);
                    else
                        m_workspace.Report(Status::Fail(ErrorCode::AsmSyntax, error));
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CIRCLE "  Toggle breakpoint", "F5"))
        {
            Status status = m_engine.EnsureDebugger();
            if (status)
                status = debugger.ToggleBreakpoint(instruction.Address);
            m_workspace.Report(status);
        }
        if (ImGui::MenuItem(ICON_FA_LIST_UL "  Find out what addresses this instruction accesses", nullptr, false, instruction.Memory.Present))
            m_workspace.StartWatch(Debug::WatchKind::InstructionAccesses, instruction.Address, 1);
        if (ImGui::MenuItem(ICON_FA_PHONE "  Find out what calls this address"))
            m_workspace.StartWatch(Debug::WatchKind::Calls, instruction.Address, 1);
        if (broken && ImGui::MenuItem(ICON_FA_MAP_MARKER_ALT "  Set the instruction pointer here"))
        {
            Disasm::RegisterValues registers = debugger.Break().Registers;
            registers.Rip = instruction.Address;
            debugger.SetRegisters(registers);
        }
        ImGui::Separator();
        if (instruction.Memory.Absolute && ImGui::MenuItem(ICON_FA_TH "  Show the operand in the hex view"))
        {
            m_hexTop = *instruction.Memory.Absolute & ~0xFull;
            m_hexSelected = *instruction.Memory.Absolute;
        }
        if (ImGui::MenuItem(ICON_FA_PLUS "  Add this address to the address list"))
            m_workspace.AddAddress(instruction.Address, Scan::ValueType::ByteArray, instruction.Text());
        if (ImGui::BeginMenu(ICON_FA_COPY "  Copy"))
        {
            if (ImGui::MenuItem("Address"))
                ImGui::SetClipboardText(Hex(instruction.Address).c_str());
            if (ImGui::MenuItem("Symbolic address"))
                ImGui::SetClipboardText(m_engine.Symbols().ModuleOffset(instruction.Address).c_str());
            if (ImGui::MenuItem("Bytes"))
                ImGui::SetClipboardText(instruction.BytesText().c_str());
            if (ImGui::MenuItem("Instruction"))
                ImGui::SetClipboardText(instruction.Text().c_str());
            ImGui::EndMenu();
        }
    }

    void MemoryViewer::Registers(float width, float height)
    {
        Debug::Debugger& debugger = m_engine.Debugger();
        Debug::BreakInfo info = debugger.Break();
        BeginCard("##registers", nullptr, ImVec2(width, height));
        CardTitle(ICON_FA_MICROCHIP, "Registers", "thread " + std::to_string(info.ThreadId));
        PushMono();
        bool changed = false;
        Disasm::RegisterValues values = info.Registers;
        for (const RegisterRow& row : kRegisterRows)
        {
            if (row.Only64 && !info.Is64Bit)
                continue;
            std::string name = row.Name;
            if (!info.Is64Bit)
                name[0] = 'E';
            ImGui::PushID(row.Name);
            ImGui::TextColored(ManifoldTheme::CodeRegister, "%-3s", name.c_str());
            ImGui::SameLine();
            std::string text = Hex(values.*row.Field, info.Is64Bit ? 16 : 8);
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("0000000000000000").x + Px(10.0f));
            if (InputText("##value", text, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (const auto parsed = ParseHex(text))
                {
                    values.*row.Field = *parsed;
                    changed = true;
                }
            }
            if (ImGui::BeginPopupContextItem("##regmenu"))
            {
                if (ImGui::MenuItem("Show in hex view"))
                {
                    m_hexTop = (values.*row.Field) & ~0xFull;
                    m_hexSelected = values.*row.Field;
                }
                if (ImGui::MenuItem("Show in disassembler"))
                    Navigate(values.*row.Field);
                if (ImGui::MenuItem("Copy"))
                    ImGui::SetClipboardText(Hex(values.*row.Field).c_str());
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        for (const FlagBit& flag : kFlags)
        {
            bool set = (values.Flags >> flag.Bit) & 1;
            if (ImGui::Checkbox(flag.Name, &set))
            {
                values.Flags ^= 1ull << flag.Bit;
                changed = true;
            }
            if (&flag != &kFlags[3] && &flag != &kFlags[7])
                ImGui::SameLine();
        }
        if (changed)
            debugger.SetRegisters(values);
        ImGui::Spacing();
        SectionLabel("Stack");
        const int pointer = info.Is64Bit ? 8 : 4;
        for (int i = 0; i < 24; ++i)
        {
            const std::uint64_t at = values.Rsp + static_cast<std::uint64_t>(i * pointer);
            const auto value = m_engine.Process()->ReadPointer(at);
            if (!value)
                break;
            ImGui::TextColored(ManifoldTheme::Muted, "+%02X", i * pointer);
            ImGui::SameLine();
            ImGui::TextUnformatted(Hex(*value, pointer * 2).c_str());
            const std::string described = m_engine.Describe(*value);
            if (described != Hex(*value))
            {
                ImGui::SameLine();
                ImGui::TextColored(ManifoldTheme::AddressText, "%s", described.c_str());
            }
        }
        PopMono();
        EndCard();
    }

    void MemoryViewer::HexView(float height)
    {
        const Target::Process& process = *m_engine.Process();
        PushMono();
        const float rowHeight = std::floor(ImGui::GetTextLineHeight() + Px(2.0f));
        const float headerHeight = ImGui::GetFrameHeight() + Px(4.0f);
        ImGui::SetNextItemWidth(Px(130.0f));
        ImGui::PopFont();
        if (ImGui::BeginCombo("##hexmode", kHexModes[m_hexDisplay]))
        {
            for (int i = 0; i < static_cast<int>(std::size(kHexModes)); ++i)
            {
                if (ImGui::Selectable(kHexModes[i], i == m_hexDisplay))
                    m_hexDisplay = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const std::uint64_t focus = m_hexSelected ? m_hexSelected : m_hexTop;
        const auto region = process.RegionAt(focus);
        std::string info = m_engine.Describe(focus);
        if (region)
            info += "  ·  " + Target::ProtectText(region->Protect) + "  ·  " + Target::RegionTypeText(region->Type) + "  ·  base " + Hex(region->AllocationBase);
        ImGui::AlignTextToFramePadding();
        Muted(info);
        PushMono();

        const int rows = std::max(1, static_cast<int>((height - headerHeight) / rowHeight));
        const std::size_t bytesShown = static_cast<std::size_t>(rows * m_hexColumns);
        std::vector<std::uint8_t> data(bytesShown);
        std::vector<bool> readable(bytesShown, false);
        for (std::size_t page = 0; page < bytesShown;)
        {
            const std::uint64_t at = m_hexTop + page;
            const std::size_t chunk = std::min<std::size_t>(bytesShown - page, 0x1000 - static_cast<std::size_t>(at & 0xFFF));
            if (process.ReadExact(at, data.data() + page, chunk))
                std::fill(readable.begin() + static_cast<std::ptrdiff_t>(page), readable.begin() + static_cast<std::ptrdiff_t>(page + chunk), true);
            page += chunk;
        }
        const double now = m_engine.Now();
        if (m_hexPreviousTop != m_hexTop || m_hexPrevious.size() != data.size())
        {
            m_hexPrevious = data;
            m_hexPreviousTop = m_hexTop;
            m_hexChangedAt.assign(data.size(), -10.0);
        }
        for (std::size_t i = 0; i < data.size(); ++i)
        {
            if (readable[i] && data[i] != m_hexPrevious[i])
                m_hexChangedAt[i] = now;
        }
        m_hexPrevious = data;

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float viewHeight = static_cast<float>(rows) * rowHeight;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + viewHeight), ImGui::GetColorU32(ManifoldTheme::Card), Px(4.0f));
        const float charWidth = ImGui::CalcTextSize("0").x;
        const float addressX = origin.x + Px(8.0f);
        const float dataX = addressX + charWidth * 18.0f;
        const int valueSize = HexValueSize();
        const int perRow = m_hexColumns / valueSize;
        const float cellWidth = m_hexDisplay == 0 ? charWidth * 3.0f : charWidth * static_cast<float>(m_hexDisplay >= 4 ? 14 : valueSize * 2 + 1);
        const float asciiX = dataX + cellWidth * static_cast<float>(perRow) + charWidth * 2.0f;

        ImGui::InvisibleButton("##hex", ImVec2(width, viewHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated() || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            m_hexFocus = true;
            m_focusCode = false;
        }
        if (hovered)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int row = static_cast<int>((mouse.y - origin.y) / rowHeight);
            int column = -1;
            if (mouse.x >= dataX && mouse.x < dataX + cellWidth * static_cast<float>(perRow))
                column = static_cast<int>((mouse.x - dataX) / cellWidth) * valueSize;
            else if (mouse.x >= asciiX && mouse.x < asciiX + charWidth * static_cast<float>(m_hexColumns))
                column = static_cast<int>((mouse.x - asciiX) / charWidth);
            if (column >= 0 && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
            {
                m_hexSelected = m_hexTop + static_cast<std::uint64_t>(row * m_hexColumns + column);
                m_hexNibble = 0;
            }
            if (ImGui::GetIO().MouseWheel != 0.0f)
            {
                const std::int64_t delta = -static_cast<std::int64_t>(std::round(ImGui::GetIO().MouseWheel * 3.0f)) * m_hexColumns;
                m_hexTop = static_cast<std::uint64_t>(static_cast<std::int64_t>(m_hexTop) + delta);
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##hexmenu");
            if (column >= 0 && m_hexDisplay != 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                m_hexEditing = true;
        }

        drawList->PushClipRect(origin, ImVec2(origin.x + width, origin.y + viewHeight), true);
        const ImU32 textColor = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 mutedColor = ImGui::GetColorU32(ManifoldTheme::Muted);
        for (int row = 0; row < rows; ++row)
        {
            const float y = origin.y + static_cast<float>(row) * rowHeight + Px(1.0f);
            const std::uint64_t rowAddress = m_hexTop + static_cast<std::uint64_t>(row * m_hexColumns);
            drawList->AddText(ImVec2(addressX, y), ImGui::GetColorU32(ManifoldTheme::AddressText), Hex(rowAddress, process.Is64Bit() ? 12 : 8).c_str());
            for (int cell = 0; cell < perRow; ++cell)
            {
                const std::size_t index = static_cast<std::size_t>(row * m_hexColumns + cell * valueSize);
                const std::uint64_t address = rowAddress + static_cast<std::uint64_t>(cell * valueSize);
                const float x = dataX + static_cast<float>(cell) * cellWidth;
                const bool selected = m_hexSelected >= address && m_hexSelected < address + static_cast<std::uint64_t>(valueSize);
                if (selected)
                    drawList->AddRectFilled(ImVec2(x - Px(2.0f), y - Px(1.0f)), ImVec2(x + cellWidth - charWidth * 0.5f, y + rowHeight - Px(1.0f)),
                        ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, m_hexFocus ? 0.35f : 0.18f)), Px(2.0f));
                bool cellReadable = true;
                bool changed = false;
                for (int b = 0; b < valueSize; ++b)
                {
                    cellReadable = cellReadable && readable[index + static_cast<std::size_t>(b)];
                    changed = changed || now - m_hexChangedAt[index + static_cast<std::size_t>(b)] < 1.0;
                }
                std::string text;
                if (!cellReadable)
                    text = std::string(static_cast<std::size_t>(m_hexDisplay == 0 ? 2 : valueSize * 2), '?');
                else
                {
                    const std::span<const std::uint8_t> bytes(data.data() + index, static_cast<std::size_t>(valueSize));
                    switch (m_hexDisplay)
                    {
                    case 0: text = Hex(data[index], 2); break;
                    case 1: case 2: case 3:
                    {
                        std::uint64_t value = 0;
                        std::memcpy(&value, bytes.data(), bytes.size());
                        text = Hex(value, valueSize * 2);
                        break;
                    }
                    case 4: text = Scan::FormatValue(Scan::ValueType::FourBytes, bytes, {}); break;
                    case 5: text = Scan::FormatValue(Scan::ValueType::Float, bytes, {}); break;
                    case 6: text = Scan::FormatValue(Scan::ValueType::Double, bytes, {}); break;
                    default: break;
                    }
                    if (text.size() > 13)
                        text = text.substr(0, 12) + "~";
                }
                const ImU32 color = !cellReadable ? mutedColor : changed ? ImGui::GetColorU32(ManifoldTheme::Changed) : (m_hexDisplay == 0 && data[index] == 0 ? mutedColor : textColor);
                drawList->AddText(ImVec2(x, y), color, text.c_str());
            }
            std::string ascii;
            for (int b = 0; b < m_hexColumns; ++b)
            {
                const std::size_t index = static_cast<std::size_t>(row * m_hexColumns + b);
                const std::uint8_t c = data[index];
                ascii += !readable[index] ? '?' : (c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
            }
            drawList->AddText(ImVec2(asciiX, y), mutedColor, ascii.c_str());
            if (m_hexSelected >= rowAddress && m_hexSelected < rowAddress + static_cast<std::uint64_t>(m_hexColumns))
            {
                const float x = asciiX + charWidth * static_cast<float>(m_hexSelected - rowAddress);
                drawList->AddRect(ImVec2(x, y - Px(1.0f)), ImVec2(x + charWidth, y + rowHeight - Px(1.0f)), ImGui::GetColorU32(ManifoldTheme::Accent));
            }
        }
        drawList->PopClipRect();
        PopMono();

        if (ImGui::BeginPopup("##hexmenu"))
        {
            HexMenu(m_hexSelected);
            ImGui::EndPopup();
        }
        if (m_hexEditing)
        {
            ImGui::OpenPopup("##hexvalue");
            m_hexEditing = false;
            m_valueBuffer.clear();
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(valueSize));
            if (process.ReadExact(m_hexSelected, bytes.data(), bytes.size()))
            {
                const Scan::ValueType type = m_hexDisplay == 5 ? Scan::ValueType::Float : m_hexDisplay == 6 ? Scan::ValueType::Double :
                                             valueSize == 2 ? Scan::ValueType::TwoBytes : valueSize == 8 ? Scan::ValueType::EightBytes : Scan::ValueType::FourBytes;
                Scan::ValueFormat format;
                format.Hex = m_hexDisplay >= 1 && m_hexDisplay <= 3;
                m_valueBuffer = Scan::FormatValue(type, bytes, format);
            }
        }
        if (ImGui::BeginPopup("##hexvalue"))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(Px(200.0f));
            if (InputText("##edit", m_valueBuffer, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                const Scan::ValueType type = m_hexDisplay == 5 ? Scan::ValueType::Float : m_hexDisplay == 6 ? Scan::ValueType::Double :
                                             valueSize == 2 ? Scan::ValueType::TwoBytes : valueSize == 8 ? Scan::ValueType::EightBytes : Scan::ValueType::FourBytes;
                Scan::ValueFormat format;
                format.Hex = m_hexDisplay >= 1 && m_hexDisplay <= 3;
                if (const auto bytes = Scan::ParseValue(type, m_valueBuffer, format))
                    m_workspace.Report(process.Write(m_hexSelected, *bytes));
                else
                    m_workspace.Report(Status::Fail(ErrorCode::ScanBadValue, m_valueBuffer));
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (m_hexFocus && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            HandleHexKeys();
    }

    void MemoryViewer::HandleHexKeys()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            return;
        const int step = HexValueSize();
        const int rows = 8;
        const auto keep = [&]()
            {
                if (m_hexSelected < m_hexTop)
                    m_hexTop = m_hexSelected & ~static_cast<std::uint64_t>(m_hexColumns - 1);
                if (m_hexSelected >= m_hexTop + static_cast<std::uint64_t>(m_hexColumns * 12))
                    m_hexTop = (m_hexSelected & ~static_cast<std::uint64_t>(m_hexColumns - 1)) - static_cast<std::uint64_t>(m_hexColumns * 6);
            };
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { m_hexSelected += static_cast<std::uint64_t>(step); m_hexNibble = 0; keep(); }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { m_hexSelected -= static_cast<std::uint64_t>(step); m_hexNibble = 0; keep(); }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { m_hexSelected += static_cast<std::uint64_t>(m_hexColumns); m_hexNibble = 0; keep(); }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { m_hexSelected -= static_cast<std::uint64_t>(m_hexColumns); m_hexNibble = 0; keep(); }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) { m_hexTop += static_cast<std::uint64_t>(m_hexColumns * rows); m_hexSelected += static_cast<std::uint64_t>(m_hexColumns * rows); }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) { m_hexTop -= static_cast<std::uint64_t>(m_hexColumns * rows); m_hexSelected -= static_cast<std::uint64_t>(m_hexColumns * rows); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
        {
            m_gotoOpen = true;
            m_gotoHex = true;
            m_gotoText = Hex(m_hexSelected);
        }
        if (m_hexDisplay != 0 || io.KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false))
                m_hexEditing = true;
            return;
        }
        for (const ImWchar character : io.InputQueueCharacters)
        {
            const int digit = HexValue(static_cast<char>(character));
            if (digit < 0)
                continue;
            std::uint8_t byte = 0;
            if (!m_engine.Process()->ReadExact(m_hexSelected, &byte, 1))
                break;
            byte = m_hexNibble == 0 ? static_cast<std::uint8_t>((byte & 0x0F) | (digit << 4)) : static_cast<std::uint8_t>((byte & 0xF0) | digit);
            const Status status = m_engine.Process()->Write(m_hexSelected, &byte, 1);
            if (!status)
            {
                m_workspace.Report(status);
                break;
            }
            if (m_hexNibble == 0)
                m_hexNibble = 1;
            else
            {
                m_hexNibble = 0;
                ++m_hexSelected;
                keep();
            }
        }
    }

    void MemoryViewer::HexMenu(std::uint64_t address)
    {
        const int size = HexValueSize();
        ImGui::TextDisabled("%s", m_engine.Describe(address).c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_LOCATION_ARROW "  Go to address", "Ctrl+G"))
        {
            m_gotoOpen = true;
            m_gotoHex = true;
            m_gotoText = Hex(address);
        }
        if (ImGui::MenuItem(ICON_FA_CODE "  Disassemble this address"))
            Navigate(address);
        if (ImGui::MenuItem(ICON_FA_SITEMAP "  Dissect data at this address"))
            m_workspace.OpenStructure(address);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_EYE "  Find out what accesses this address"))
            m_workspace.StartWatch(Debug::WatchKind::Accesses, address, std::max(1, size == 1 ? 4 : size));
        if (ImGui::MenuItem(ICON_FA_PEN "  Find out what writes to this address"))
            m_workspace.StartWatch(Debug::WatchKind::Writes, address, std::max(1, size == 1 ? 4 : size));
        if (ImGui::MenuItem(ICON_FA_CIRCLE "  Break when written"))
        {
            Status status = m_engine.EnsureDebugger();
            if (status)
                status = m_engine.Debugger().SetBreakpoint(address, Debug::BreakpointType::Write, size == 1 ? 4 : size, true);
            m_workspace.Report(status);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_PLUS "  Add this address to the address list"))
        {
            const Scan::ValueType type = m_hexDisplay == 5 ? Scan::ValueType::Float : m_hexDisplay == 6 ? Scan::ValueType::Double :
                                         size == 2 ? Scan::ValueType::TwoBytes : size == 8 ? Scan::ValueType::EightBytes : size == 1 ? Scan::ValueType::Byte : Scan::ValueType::FourBytes;
            m_workspace.AddAddress(address, type);
        }
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy address"))
            ImGui::SetClipboardText(Hex(address).c_str());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy 16 bytes"))
        {
            std::uint8_t bytes[16]{};
            if (m_engine.Process()->ReadExact(address, bytes, sizeof(bytes)))
                ImGui::SetClipboardText(HexBytes(bytes).c_str());
        }
    }

    void MemoryViewer::GotoDialog()
    {
        if (m_gotoOpen)
        {
            ImGui::OpenPopup("##goto");
            m_gotoOpen = false;
            m_gotoError.clear();
        }
        if (!ImGui::BeginPopup("##goto"))
            return;
        ImGui::TextUnformatted(m_gotoHex ? "Go to address in the hex view" : "Go to address");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Px(320.0f));
        const bool enter = InputText("##gotoaddress", m_gotoText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (!m_gotoError.empty())
            TextWithError(m_gotoError, ManifoldTheme::Error);
        if (enter || ImGui::Button("Go", Px(80.0f, 0.0f)))
        {
            Status error;
            if (const auto address = m_engine.Evaluate(m_gotoText, &error))
            {
                if (m_gotoHex)
                {
                    m_hexTop = *address & ~0xFull;
                    m_hexSelected = *address;
                }
                else
                    Navigate(*address);
                ImGui::CloseCurrentPopup();
            }
            else
                m_gotoError = error.Text;
        }
        ImGui::EndPopup();
    }

    void MemoryViewer::AssembleDialog()
    {
        if (m_assembleOpen)
        {
            ImGui::OpenPopup("##assemble");
            m_assembleOpen = false;
        }
        if (!ImGui::BeginPopup("##assemble"))
            return;
        const Target::Process& process = *m_engine.Process();
        ImGui::Text("Assemble at %s", m_engine.Describe(m_assembleAddress).c_str());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        PushMono();
        ImGui::SetNextItemWidth(Px(420.0f));
        const bool enter = InputText("##instruction", m_assembleText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        PopMono();
        Asm::AssembleOptions options;
        options.Is64Bit = process.Is64Bit();
        options.Address = m_assembleAddress;
        options.DefaultToDword = true;
        options.Resolve = [this](std::string_view name) -> Asm::SymbolValue
            {
                if (const auto value = m_engine.Symbols().Resolve(name))
                    return { Asm::SymbolState::Known, *value };
                return {};
            };
        const Asm::AssembleResult result = Asm::Assemble(m_assembleText, options);
        const Disasm::Instruction old = m_engine.Disassembler().Read(process, m_assembleAddress, nullptr);
        if (result.Ok)
        {
            const Disasm::Instruction written = m_engine.Disassembler().Decode(m_assembleAddress, result.Bytes.data(), result.Bytes.size(), nullptr);
            PushMono();
            ImGui::TextColored(ManifoldTheme::BytesText, "%s", HexBytes(result.Bytes).c_str());
            ImGui::SameLine(0.0f, Px(10.0f));
            ImGui::TextUnformatted(written.Text().c_str());
            PopMono();
            Muted(std::to_string(result.Bytes.size()) + (result.Bytes.size() == 1 ? " byte" : " bytes") + ", replaces " + std::to_string(old.Length));
            ImGui::Checkbox("Fill the rest of the replaced instructions with NOPs", &m_assembleNopFill);
        }
        else if (!TrimView(m_assembleText).empty())
            ImGui::TextColored(ManifoldTheme::Error, "%s", result.Message.c_str());
        ImGui::BeginDisabled(!result.Ok);
        if (enter || AccentButton("Write", Px(90.0f, 0.0f)))
        {
            if (result.Ok)
            {
                std::vector<std::uint8_t> bytes = result.Bytes;
                if (m_assembleNopFill)
                {
                    std::uint64_t covered = m_assembleAddress;
                    while (covered < m_assembleAddress + bytes.size())
                        covered = m_engine.Disassembler().Read(process, covered, nullptr).Next();
                    bytes.resize(static_cast<std::size_t>(covered - m_assembleAddress), 0x90);
                }
                const Status status = process.Write(m_assembleAddress, bytes);
                m_workspace.Report(status);
                if (status)
                {
                    LOG_INFO("Memory Viewer", "Assembled \"" + m_assembleText + "\" at " + Hex(m_assembleAddress));
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(90.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void MemoryViewer::FindDialog()
    {
        if (m_findOpen)
        {
            ImGui::OpenPopup("##find");
            m_findOpen = false;
            m_findError.clear();
        }
        if (!ImGui::BeginPopup("##find"))
            return;
        ImGui::TextUnformatted("Find bytes (searches forward from the hex view)");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Px(360.0f));
        const bool enter = InputTextWithHint("##pattern", "48 8B ?? 10 or 'text'", m_findText, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_findError.empty())
            TextWithError(m_findError, ManifoldTheme::Error);
        if (enter || AccentButton("Find next", Px(100.0f, 0.0f)))
        {
            std::optional<Scan::Pattern> pattern;
            const std::string text = Trim(m_findText);
            if (text.size() >= 2 && (text.front() == '\'' || text.front() == '"') && text.back() == text.front())
            {
                Scan::Pattern bytes;
                for (char c : text.substr(1, text.size() - 2))
                {
                    bytes.Bytes.push_back(static_cast<std::uint8_t>(c));
                    bytes.Mask.push_back(0xFF);
                }
                pattern = bytes;
            }
            else
                pattern = Scan::ParsePattern(text);
            if (!pattern || pattern->Empty())
                m_findError = ErrorMessage(ErrorCode::ScanBadPattern, text);
            else
            {
                const std::uint64_t from = (m_hexSelected ? m_hexSelected : m_hexTop) + 1;
                const auto hits = Scan::FindPattern(*m_engine.Process(), *pattern, from, m_engine.Process()->HighestAddress(), false, 1);
                if (hits.empty())
                    m_findError = "Not found after " + Hex(from) + ".";
                else
                {
                    m_hexTop = hits.front() & ~0xFull;
                    m_hexSelected = hits.front();
                    if (m_engine.Process()->RegionAt(hits.front()).value_or(Target::RegionInfo{}).Executable())
                        Navigate(hits.front());
                    m_findError.clear();
                }
            }
        }
        ImGui::EndPopup();
    }

    void MemoryViewer::AllocateDialog()
    {
        if (m_allocateOpen)
        {
            ImGui::OpenPopup("##allocate");
            m_allocateOpen = false;
        }
        if (!ImGui::BeginPopup("##allocate"))
            return;
        ImGui::TextUnformatted("Allocate memory in the process (hex size)");
        ImGui::SetNextItemWidth(Px(160.0f));
        InputText("##size", m_allocateSize, ImGuiInputTextFlags_CharsHexadecimal);
        if (AccentButton("Allocate", Px(100.0f, 0.0f)))
        {
            Status status;
            const std::uint64_t memory = m_engine.Process()->Allocate(static_cast<std::size_t>(ParseHex(m_allocateSize).value_or(0x1000)), 0x40, 0, status);
            if (memory)
            {
                m_hexTop = memory;
                m_hexSelected = memory;
                m_engine.Notify(LogLevel::Success, "Allocated " + m_allocateSize + " bytes at " + Hex(memory));
                ImGui::SetClipboardText(Hex(memory).c_str());
            }
            m_workspace.Report(status);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void MemoryViewer::InjectDialog()
    {
        if (m_injectOpen)
        {
            m_injectOpen = false;
            if (const auto path = m_workspace.OpenFileDialog({ { L"Libraries (*.dll)", L"*.dll" } }))
            {
                const Status status = m_engine.Process()->InjectLibrary(path->wstring());
                if (status)
                    m_engine.Notify(LogLevel::Success, "Injected " + WideToUtf8(path->filename().wstring()));
                m_workspace.Report(status);
            }
        }
    }
}
