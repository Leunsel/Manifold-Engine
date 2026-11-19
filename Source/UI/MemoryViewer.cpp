#include "UI/MemoryViewer.h"

#include "UI/CodeTools.h"
#include "Asm/Assembler.h"
#include "Core/Text.h"
#include "Core/RuntimeLog.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/ScriptUi.h"
#include "UI/Shortcuts.h"
#include "UI/Templates.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

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

        std::string UndoNote()
        {
            const std::string keys = ShortcutLabel(Command::UndoMemoryEdit);
            return keys.empty() ? std::string(".") : ". Undo with " + keys + ".";
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

        struct FlagHelp
        {
            const char* Title;
            const char* Meaning;
            const char* Readers;
        };

        const FlagHelp kFlagHelp[] = {
            { "Carry", "1 when an unsigned addition carried out of the top bit, or a subtraction or comparison had to borrow because the first value was below the second. Shifts and rotates put the bit they move out here.",
              "jb/jc, jae/jnc, jbe, ja, adc, sbb, setb, cmovb" },
            { "Parity", "1 when the lowest byte of the result has an even number of set bits. Compilers check it after float comparisons (ucomiss, comisd) to catch NaN.",
              "jp/jpe, jnp/jpo, setp, cmovp" },
            { "Auxiliary carry", "1 when bit 3 carried into bit 4 or borrowed from it. Only the decimal (BCD) adjustments use it.", "daa, das, aaa, aas" },
            { "Zero", "1 when the result was zero: cmp of two equal values, test of a zero value, or a count that reached zero.", "je/jz, jne/jnz, jbe, ja, jle, jg, sete, cmove" },
            { "Sign", "Copy of the top bit of the result: 1 when the result is negative as a signed number.", "js, jns, jl, jge, jle, jg, sets, cmovs" },
            { "Trap", "When 1, the processor stops the thread after every instruction. The debugger sets it itself for single steps; switching it on here makes the thread stop again after its next instruction.",
              "" },
            { "Direction", "Direction of the string instructions movs, stos, cmps, scas and lods: 0 counts up, 1 counts down. Code expects 0 outside of such loops.",
              "rep movs, rep stos, cmps, scas, lods" },
            { "Overflow", "1 when a signed result did not fit, for example when adding two large positive numbers gave a negative one.", "jo, jno, jl, jge, jle, jg, seto, cmovo" },
        };

        const std::vector<const char*> kVectorFormats = { "Float", "Double", "Int", "Hex" };

        int VectorLanes(int format)
        {
            return format == 1 ? 2 : format == 3 ? 1 : 4;
        }

        std::string NumberText(double value, int digits)
        {
            if (std::isnan(value))
                return "nan";
            if (std::isinf(value))
                return value < 0 ? "-inf" : "inf";
            return Format("%.*g", digits, value);
        }

        std::string LaneText(const std::array<std::uint8_t, 16>& xmm, int format, int lane, int digits)
        {
            switch (format)
            {
            case 0:
            {
                float value = 0.0f;
                std::memcpy(&value, xmm.data() + lane * 4, sizeof(value));
                return NumberText(value, digits);
            }
            case 1:
            {
                double value = 0.0;
                std::memcpy(&value, xmm.data() + lane * 8, sizeof(value));
                return NumberText(value, digits);
            }
            case 2:
            {
                std::int32_t value = 0;
                std::memcpy(&value, xmm.data() + lane * 4, sizeof(value));
                return std::to_string(value);
            }
            default:
            {
                std::uint64_t low = 0;
                std::uint64_t high = 0;
                std::memcpy(&low, xmm.data(), 8);
                std::memcpy(&high, xmm.data() + 8, 8);
                return Hex(high, 16) + " " + Hex(low, 16);
            }
            }
        }

        std::string VectorText(const std::array<std::uint8_t, 16>& xmm, int format)
        {
            std::string text;
            for (int lane = 0; lane < VectorLanes(format); ++lane)
            {
                if (lane)
                    text += ' ';
                text += LaneText(xmm, format, lane, format == 1 ? 17 : 9);
            }
            return text;
        }

        std::optional<std::array<std::uint8_t, 16>> ParseVector(std::string_view text, int format)
        {
            std::string cleaned(text);
            for (char& c : cleaned)
            {
                if (c == ',' || c == ';' || c == '\t')
                    c = ' ';
            }
            std::array<std::uint8_t, 16> xmm{};
            if (format == 3)
            {
                std::string digits;
                for (const char c : cleaned)
                {
                    if (c != ' ')
                        digits += c;
                }
                if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
                    digits.erase(0, 2);
                if (digits.empty() || digits.size() > 32)
                    return std::nullopt;
                digits.insert(0, 32 - digits.size(), '0');
                const auto high = ParseHex(digits.substr(0, 16));
                const auto low = ParseHex(digits.substr(16));
                if (!high || !low)
                    return std::nullopt;
                std::memcpy(xmm.data(), &*low, 8);
                std::memcpy(xmm.data() + 8, &*high, 8);
                return xmm;
            }
            const std::vector<std::string> parts = Split(cleaned, ' ');
            if (static_cast<int>(parts.size()) != VectorLanes(format))
                return std::nullopt;
            for (int lane = 0; lane < VectorLanes(format); ++lane)
            {
                const std::string& part = parts[static_cast<std::size_t>(lane)];
                if (format == 2)
                {
                    const bool hex = part.size() > 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X');
                    std::int64_t value = 0;
                    if (hex)
                    {
                        const auto parsed = ParseHex(part.substr(2));
                        if (!parsed || *parsed > 0xFFFFFFFFull)
                            return std::nullopt;
                        value = static_cast<std::int64_t>(*parsed);
                    }
                    else
                    {
                        const auto parsed = ParseDecimal(part);
                        if (!parsed || *parsed < INT32_MIN || *parsed > static_cast<std::int64_t>(UINT32_MAX))
                            return std::nullopt;
                        value = *parsed;
                    }
                    const std::uint32_t bits = static_cast<std::uint32_t>(value);
                    std::memcpy(xmm.data() + lane * 4, &bits, 4);
                    continue;
                }
                const auto parsed = ParseDouble(part);
                if (!parsed)
                    return std::nullopt;
                if (format == 0)
                {
                    const float value = static_cast<float>(*parsed);
                    std::memcpy(xmm.data() + lane * 4, &value, 4);
                }
                else
                {
                    std::memcpy(xmm.data() + lane * 8, &*parsed, 8);
                }
            }
            return xmm;
        }

        void ClippedText(ImDrawList* draw, ImVec2 at, float width, ImU32 color, const std::string& text)
        {
            draw->PushClipRect(at, ImVec2(at.x + width, at.y + ImGui::GetTextLineHeight() + 2.0f), true);
            draw->AddText(at, color, text.c_str());
            draw->PopClipRect();
        }

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

        std::uint64_t WithoutPadding(const Target::Process& process, std::uint64_t start, std::uint64_t end)
        {
            if (end <= start + 1)
                return end;
            std::uint8_t tail[64]{};
            const std::size_t size = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(tail), end - start - 1));
            if (process.Read(end - size, tail, size) != size)
                return end;
            std::size_t padding = 0;
            while (padding < size && tail[size - 1 - padding] == 0xCC)
                ++padding;
            return end - padding;
        }
    }

    MemoryViewer::MemoryViewer(Workspace& workspace, int number) : m_workspace(workspace), m_engine(workspace.Core()), m_templates(workspace), m_number(number)
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

    void MemoryViewer::Select(std::uint64_t address)
    {
        m_open = true;
        if (address == 0)
            return;
        Navigate(address);
    }

    void MemoryViewer::ShowHex(std::uint64_t address, std::uint64_t start, std::uint64_t stop)
    {
        m_open = true;
        if (address)
        {
            m_hexTop = address & ~0xFull;
            m_hexSelected = address;
            m_hexMarkStart = 0;
            m_hexMarkStop = 0;
        }
        if (start && stop >= start)
        {
            m_hexMarkStart = start;
            m_hexMarkStop = stop;
        }
    }

    void MemoryViewer::Raise()
    {
        m_open = true;
        m_bringToFront = true;
    }

    void MemoryViewer::PublishSelection()
    {
        m_workspace.ActiveViewer = m_number;
        m_workspace.LastCodeAddress = m_selected;
        m_engine.SelectedCodeAddress = m_selected;
        if (m_hexSelected)
            m_engine.SelectedHexAddress = m_hexSelected;
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
        m_top = m_engine.HasProcess() ? m_engine.Disassembler().BackUp(*m_engine.Process(), address, 4, m_engine.CodeMap()) : address;
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
        if (const std::shared_ptr<Target::Process> process = m_engine.Process(); m_noppedProcess.lock() != process)
        {
            m_nopped.clear();
            m_noppedProcess = process;
        }
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
        if (!BeginToolWindow(m_title.c_str(), &m_open, ImVec2(980.0f, 680.0f), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar, ImVec2(700.0f, 400.0f)))
        {
            EndToolWindow();
            return;
        }
        m_viewport = ImGui::GetWindowViewport()->ID;
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            PublishSelection();
        MenuBar();
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
        if (info.Broken)
            m_heldBreak = info;
        else if (!debugger.Attached() || !debugger.Stepping())
            m_heldBreak = {};
        const bool running = !info.Broken && m_heldBreak.Broken;
        const Debug::BreakInfo shown = info.Broken ? info : m_heldBreak;
        const bool showRegisters = shown.Broken && m_engine.Options().ShowRegistersOnBreak;
        const float totalWidth = ImGui::GetContentRegionAvail().x;
        const float gap = Px(6.0f);
        if (m_registersWidth <= 0.0f)
            m_registersWidth = Px(330.0f);
        m_registersWidth = std::clamp(m_registersWidth, Px(220.0f), std::max(Px(220.0f), totalWidth - Px(300.0f) - gap));
        const float registersWidth = showRegisters ? m_registersWidth : 0.0f;
        const ImVec2 codeStart = ImGui::GetCursorPos();
        const ImVec2 codeScreen = ImGui::GetCursorScreenPos();
        const float leftWidth = totalWidth - registersWidth - (showRegisters ? gap : 0.0f);
        const auto columnSplitter = [&](const char* id, ImVec2 top, float barHeight)
            {
                ImGuiWindow* window = ImGui::GetCurrentWindow();
                float first = leftWidth;
                float second = registersWidth;
                const ImRect bar(ImVec2(top.x + first, top.y), ImVec2(top.x + first + gap, top.y + barHeight));
                ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0, 0, 0, 0));
                if (ImGui::SplitterBehavior(bar, window->GetID(id), ImGuiAxis_X, &first, &second, Px(300.0f), Px(220.0f), 4.0f, 0.04f))
                    m_registersWidth = second;
                ImGui::PopStyleColor();
            };
        ImGui::BeginChild("##codearea", ImVec2(leftWidth, code), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Disassembly(code);
        ImGui::EndChild();
        if (showRegisters)
        {
            columnSplitter("##registersplit", codeScreen, code);
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - registersWidth, codeStart.y));
            Registers(registersWidth, code, shown, running);
        }
        ImGui::SetCursorPos(ImVec2(start.x, start.y + code));
        if (Splitter("##viewsplit", false, Px(6.0f), &code, &hex, Px(120.0f), Px(90.0f)))
            m_codeShare = code / available;
        ImGui::SetCursorPos(ImVec2(start.x, start.y + code + Px(6.0f)));
        const ImVec2 hexStart = ImGui::GetCursorPos();
        const ImVec2 hexScreen = ImGui::GetCursorScreenPos();
        const float hexHeight = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("##hexarea", ImVec2(leftWidth, hexHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        HexView(ImGui::GetContentRegionAvail().y);
        ImGui::EndChild();
        if (showRegisters)
        {
            columnSplitter("##stacksplit", hexScreen, hexHeight);
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - registersWidth, hexStart.y));
            Stack(registersWidth, hexHeight, shown, running);
        }
        Shortcuts();

        GotoDialog();
        CommentDialog();
        AssembleDialog();
        FindDialog();
        AllocateDialog();
        InjectDialog();
        RegionDialog();
        ThreadDialog();
        m_templates.Draw();
        if (const auto generated = m_templates.TakeText())
            m_workspace.OpenScript(*generated);
        EndToolWindow();
    }

    void MemoryViewer::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        const bool process = m_engine.HasProcess();
        Debug::Debugger& debugger = m_engine.Debugger();
        const bool broken = debugger.Broken();
        const auto item = [&](const char* label, Command command, bool enabled = true, bool selected = false)
            {
                if (CommandItem(label, command, selected, enabled))
                    RunCommand(command);
            };
        if (ImGui::BeginMenu("File"))
        {
            item(ICON_FA_CLONE "  New window", Command::NewViewerWindow);
            ImGui::Separator();
            item(ICON_FA_FILE_EXPORT "  Save memory region...", Command::SaveMemoryRegion, process);
            item(ICON_FA_FILE_IMPORT "  Load a file into memory...", Command::LoadMemoryRegion, process);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_DOWNLOAD "  Load symbols", nullptr, false, process))
                m_engine.LoadSymbols(false);
            if (ImGui::MenuItem(ICON_FA_GLOBE "  Load Windows symbols", nullptr, false, process))
                m_engine.LoadSymbols(true);
            if (ImGui::MenuItem(m_engine.MonoPresent() ? (m_engine.Mono().IsIl2Cpp() ? ICON_FA_DICE_D6 "  IL2CPP runtime loaded" : ICON_FA_DICE_D6 "  Mono runtime loaded")
                                : m_engine.Managed().Loading() ? ICON_FA_DICE_D6 "  Loading the runtime..." : ICON_FA_DICE_D6 "  Load Mono / IL2CPP runtime",
                    nullptr, false, process && !m_engine.MonoPresent() && !m_engine.Managed().Loading()))
                m_engine.Managed().Load();
            ItemTooltip("Reads the classes and methods of a Unity game (Mono or IL2CPP) so method names show here and resolve in addresses.");
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TIMES "  Close"))
                m_open = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Search", process))
        {
            item(ICON_FA_LOCATION_ARROW "  Go to address...", Command::GotoAddress);
            item(ICON_FA_SEARCH "  Find bytes...", Command::FindMemory);
            if (ImGui::MenuItem(ICON_FA_LIST "  Find all matching bytes..."))
                m_workspace.OpenByteSearch();
            item(ICON_FA_SEARCH_LOCATION "  Find assembly code...", Command::FindAssembly);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View", process))
        {
            item(ICON_FA_TH_LIST "  Memory regions", Command::MemoryRegions);
            item(ICON_FA_TAGS "  Modules and symbols", Command::SymbolsAndModules);
            item(ICON_FA_PROJECT_DIAGRAM "  Referenced functions", Command::ReferencedFunctions);
            item(ICON_FA_QUOTE_LEFT "  Referenced strings", Command::ReferencedStrings);
            item(ICON_FA_CODE_BRANCH "  Flow graph of the selected function", Command::FlowGraph);
            ImGui::Separator();
            if (ImGui::BeginMenu(ICON_FA_TH "  Hex view shows"))
            {
                const Command modes[] = { Command::HexBytes, Command::HexTwoBytes, Command::HexFourBytes, Command::HexEightBytes, Command::HexFourBytesDecimal, Command::HexFloat, Command::HexDouble };
                for (int i = 0; i < static_cast<int>(std::size(modes)); ++i)
                    item(kHexModes[i], modes[i], true, m_hexDisplay == i);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_BOOKMARK "  Bookmarks"))
            {
                BookmarkItems();
                ImGui::EndMenu();
            }
            ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Follow breaks", nullptr, &m_followBreaks);
            ImGui::MenuItem(ICON_FA_TAGS "  Show symbols", nullptr, &m_engine.Options().ShowSymbols);
            ItemTooltip("Names in the code: function headers with offsets below them, symbol names in operands, module+offset addresses. Off shows plain addresses.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug", process))
        {
            item(ICON_FA_PLAY "  Run", Command::DebugRun, broken);
            item(ICON_FA_LEVEL_DOWN_ALT "  Step into", Command::DebugStepInto, broken);
            item(ICON_FA_SHARE "  Step over", Command::DebugStepOver, broken);
            item(ICON_FA_SIGN_OUT_ALT "  Run until return", Command::DebugRunToReturn, broken);
            item(ICON_FA_PAUSE "  Break", Command::DebugBreak, !broken);
            ImGui::Separator();
            item(ICON_FA_CIRCLE "  Toggle breakpoint", Command::ToggleBreakpoint);
            item(ICON_FA_SLIDERS_H "  Breakpoint with a condition...", Command::EditBreakpoint);
            item(ICON_FA_MAP_MARKER_ALT "  Set the instruction pointer here", Command::SetInstructionPointer, broken);
            ImGui::Separator();
            item(ICON_FA_LIST "  Breakpoint list", Command::BreakpointList);
            item(ICON_FA_SHOE_PRINTS "  Break and trace from here...", Command::BreakAndTrace);
            item(ICON_FA_STREAM "  Thread list", Command::ThreadList, process);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_BUG "  Attach debugger", nullptr, false, !debugger.Attached()))
                m_workspace.Report(m_engine.EnsureDebugger());
            if (ImGui::MenuItem(ICON_FA_UNLINK "  Detach debugger", nullptr, false, debugger.Attached()))
                debugger.Detach();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools", process))
        {
            const std::size_t undoable = UndoableEdits();
            const std::string undo = ICON_FA_UNDO_ALT "  Undo the last memory edit" + (undoable == 0 ? std::string() : " (" + std::to_string(undoable) + ")");
            item(undo.c_str(), Command::UndoMemoryEdit, undoable > 0);
            if (ImGui::MenuItem(ICON_FA_HISTORY "  Write history..."))
                m_workspace.OpenWriteHistory();
            ImGui::Separator();
            item(ICON_FA_PLUS_SQUARE "  Allocate memory...", Command::AllocateMemory);
            item(ICON_FA_FILL_DRIP "  Fill memory...", Command::FillMemory);
            item(ICON_FA_RANDOM "  Create a thread...", Command::CreateThread);
            item(ICON_FA_SYRINGE "  Inject a library...", Command::InjectLibrary);
            ImGui::Separator();
            item(ICON_FA_PROJECT_DIAGRAM "  Dissect code", Command::DissectCode);
            item(ICON_FA_SITEMAP "  Dissect data", Command::DissectData);
            item(ICON_FA_BAND_AID "  Scan for patches", Command::ScanForPatches);
            ImGui::Separator();
            item(ICON_FA_CODE "  Auto Assembler", Command::ViewerAutoAssemble);
            item(ICON_FA_MOON "  Lua engine", Command::ViewerLuaEngine);
            ImGui::EndMenu();
        }
        ScriptMenuBar();
        ImGui::EndMenuBar();
    }

    void MemoryViewer::ScriptMenuBar()
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        if (lua.MenuGeneration() != m_scriptMenuGeneration)
        {
            m_scriptMenuGeneration = lua.MenuGeneration();
            m_scriptMenu = lua.MenuItems(Lua::ScriptMenu::MemoryViewMenu);
        }
        const ScriptMenuClicks clicks = DrawScriptMenuRoots(m_scriptMenu, "##script");
        if (clicks.Opened || clicks.Clicked)
            PublishSelection();
        if (clicks.Opened)
            lua.ClickMenuItem(clicks.Opened);
        if (clicks.Clicked)
            lua.ClickMenuItem(clicks.Clicked);
    }

    void MemoryViewer::ScriptShortcuts(std::vector<ImGuiKeyChord>& taken)
    {
        const bool code = m_focusCode;
        const bool hex = !m_focusCode && m_hexFocus;
        std::vector<ImGuiKeyChord> cheatEngine;
        for (const CommandInfo& info : AllCommands())
        {
            const bool active = info.Scope == CommandScope::MemoryViewer || (info.Scope == CommandScope::Disassembler && code) || (info.Scope == CommandScope::HexView && hex);
            if (!active || !info.FromCheatEngine || info.Digits)
                continue;
            const Binding binding = CurrentBinding(info.Id);
            cheatEngine.push_back(binding.Chord);
            if (binding.Alternate)
                cheatEngine.push_back(binding.Alternate);
        }
        const int clicked = ScriptShortcutPressed(m_scriptMenu, cheatEngine, CommandScope::MemoryViewer, &taken, "memory viewer");
        if (clicked)
        {
            PublishSelection();
            m_engine.Lua().ClickMenuItem(clicked);
        }
    }

    void MemoryViewer::BookmarkItems()
    {
        for (int slot = 0; slot < 10; ++slot)
        {
            const std::uint64_t bookmark = m_bookmarks[static_cast<std::size_t>(slot)];
            const std::string label = std::to_string(slot) + "  " + (bookmark ? m_engine.Describe(bookmark) : std::string("not set"));
            const std::string keys = ChordText((CurrentBinding(Command::GotoBookmark).Chord & ImGuiMod_Mask_) | (ImGuiKey_0 + slot));
            if (ImGui::MenuItem(label.c_str(), keys.c_str(), false, bookmark != 0))
                Bookmark(false, slot);
        }
        ImGui::Separator();
        const std::string set = "Set a bookmark here with " + ChordText(CurrentBinding(Command::SetBookmark).Chord, true);
        SectionLabel(set.c_str());
    }

    void MemoryViewer::Toolbar()
    {
        Debug::Debugger& debugger = m_engine.Debugger();
        const bool broken = debugger.Broken();
        const auto button = [&](const char* icon, const char* tooltip, Command command)
            {
                if (IconButton(icon, WithKeys(tooltip, command).c_str()))
                    RunCommand(command);
            };
        ImGui::BeginDisabled(m_history.empty());
        button(ICON_FA_ARROW_LEFT, "Back", Command::NavigateBack);
        ImGui::EndDisabled();
        ImGui::SameLine();
        button(ICON_FA_LOCATION_ARROW, "Go to address", Command::GotoAddress);
        ImGui::SameLine();
        button(ICON_FA_SEARCH, "Find bytes", Command::FindMemory);
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::BeginDisabled(!broken);
        button(ICON_FA_PLAY, "Run", Command::DebugRun);
        ImGui::SameLine();
        button(ICON_FA_LEVEL_DOWN_ALT, "Step into", Command::DebugStepInto);
        ImGui::SameLine();
        button(ICON_FA_SHARE, "Step over", Command::DebugStepOver);
        ImGui::SameLine();
        button(ICON_FA_SIGN_OUT_ALT, "Run until return", Command::DebugRunToReturn);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(broken);
        button(ICON_FA_PAUSE, "Break into the process", Command::DebugBreak);
        ImGui::EndDisabled();
        ImGui::SameLine();
        button(ICON_FA_CIRCLE, "Toggle breakpoint on the selected instruction", Command::ToggleBreakpoint);
        ImGui::SameLine(0.0f, Px(14.0f));
        button(ICON_FA_CODE, "Auto Assembler", Command::ViewerAutoAssemble);
        ImGui::SameLine();
        button(ICON_FA_SITEMAP, "Dissect data at the hex view address", Command::DissectData);
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
        const Debug::BreakInfo info = ShownBreak();
        const bool showSymbols = m_engine.Options().ShowSymbols;
        const Target::Symbols* symbols = showSymbols ? &m_engine.Symbols() : nullptr;

        PushMono();
        const float rowHeight = std::floor(ImGui::GetTextLineHeight() + Px(3.0f));
        const float charWidth = ImGui::CalcTextSize("0").x;
        PopMono();
        const float headerHeight = std::floor(ImGui::GetTextLineHeight() + Px(8.0f));
        const float bodyHeight = std::max(rowHeight, height - headerHeight);

        // With symbols shown, a line where a symbol starts (a registered symbol,
        // an export, or a Mono method) gets a header, and the lines after it show
        // their offset from that start ("+0", "+3", ...) until the function ends.
        // Registered symbols and exports have no known end, so their offsets stop
        // after kSymbolReach bytes; Mono methods stop at their JIT code end.
        // When the listing starts inside a function, its header stays pinned as
        // the first row, so the function context does not scroll away.
        constexpr std::uint64_t kSymbolReach = 0x1000;
        std::uint64_t functionStart = 0;
        std::uint64_t functionEnd = 0;
        std::string functionName;
        if (showSymbols && m_top)
        {
            if (const auto symbol = m_engine.Symbols().Containing(m_top, kSymbolReach); symbol && m_top < SymbolEnd(symbol->first))
            {
                functionStart = symbol->first;
                functionEnd = SymbolEnd(symbol->first);
                functionName = symbol->second;
            }
            if (m_engine.MonoPresent())
            {
                if (const auto range = m_engine.Mono().MethodRange(m_top); range && range->first > functionStart)
                {
                    if (auto name = m_engine.Mono().MethodStartNameAt(range->first))
                    {
                        functionStart = range->first;
                        functionEnd = WithoutPadding(process, range->first, range->second);
                        functionName = std::move(*name);
                    }
                }
            }
        }
        const bool sticky = functionStart && functionStart < m_top && !functionName.empty();
        const float stickyHeight = sticky ? rowHeight : 0.0f;
        m_visibleLines = std::max(4, static_cast<int>((bodyHeight - stickyHeight) / rowHeight));
        {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(m_visibleLines + 1) * 16 + 16);
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
            if (m_selected > m_top && m_selected - m_top < buffer.size())
            {
                const std::size_t sel = static_cast<std::size_t>(m_selected - m_top);
                m_lines.clear();
                m_lines.reserve(static_cast<std::size_t>(m_visibleLines + 1));
                for (const Disasm::Instruction& line : disassembler.DecodeMany(buffer, readable, m_top, m_visibleLines + 1, symbols, m_engine.CodeMap()))
                {
                    if (line.Address + (line.Length ? line.Length : 1) > m_selected)
                        break;
                    m_lines.push_back(line);
                }
                const std::vector<std::uint8_t> sub(buffer.begin() + static_cast<std::ptrdiff_t>(sel), buffer.end());
                const std::vector<bool> subReadable(readable.begin() + static_cast<std::ptrdiff_t>(sel), readable.end());
                const int remaining = std::max(1, m_visibleLines + 1 - static_cast<int>(m_lines.size()));
                for (const Disasm::Instruction& line : disassembler.DecodeMany(sub, subReadable, m_selected, remaining, symbols, m_engine.CodeMap()))
                    m_lines.push_back(line);
            }
            else
            {
                m_lines = disassembler.DecodeMany(buffer, readable, m_top, m_visibleLines + 1, symbols, m_engine.CodeMap());
            }
        }

        const ImVec2 frame = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(frame, ImVec2(frame.x + width, frame.y + height), ImGui::GetColorU32(ManifoldTheme::Card), Px(4.0f));
        const ImVec2 origin(frame.x, frame.y + headerHeight);
        const float rightX = frame.x + width;
        const float gutter = Px(18.0f);
        const float arrows = Px(26.0f);

        const ImVec2 linesOrigin(origin.x, origin.y + stickyHeight);
        const std::uint64_t stickyStart = functionStart;
        std::vector<std::string> addressTexts;
        std::vector<bool> addressInModule;
        std::vector<std::optional<std::string>> headers;
        std::vector<bool> blockStarts;
        addressTexts.reserve(m_lines.size());
        addressInModule.reserve(m_lines.size());
        headers.reserve(m_lines.size());
        blockStarts.reserve(m_lines.size());
        float widestAddress = 0.0f;
        PushMono();
        for (const Disasm::Instruction& line : m_lines)
        {
            std::optional<std::string> header;
            bool blockStart = false;
            if (showSymbols)
            {
                header = m_engine.Symbols().ExactName(line.Address);
                if (header)
                {
                    blockStart = !m_engine.Symbols().LabelOnly(line.Address);
                    if (blockStart)
                    {
                        functionStart = line.Address;
                        functionEnd = std::max(SymbolEnd(line.Address), line.Next());
                    }
                }
                else if (m_engine.MonoPresent())
                {
                    header = m_engine.Mono().MethodStartNameAt(line.Address);
                    if (header)
                    {
                        blockStart = true;
                        const auto range = m_engine.Mono().MethodRange(line.Address);
                        functionStart = line.Address;
                        functionEnd = range ? WithoutPadding(process, line.Address, range->second) : line.Address + kSymbolReach;
                    }
                }
            }
            std::string text;
            bool symbolic = false;
            if (!showSymbols)
            {
                text = Hex(line.Address, process.Is64Bit() ? 12 : 8);
            }
            else if (functionStart && line.Address >= functionStart && line.Address < functionEnd)
            {
                text = "+" + Hex(line.Address - functionStart);
                symbolic = true;
            }
            else
            {
                const std::string address = m_engine.Symbols().ModuleOffset(line.Address);
                symbolic = address.front() == '"';
                text = symbolic ? ReplaceAll(address, "\"", "") : Hex(line.Address, process.Is64Bit() ? 12 : 8);
            }
            headers.push_back(std::move(header));
            blockStarts.push_back(blockStart);
            addressTexts.push_back(std::move(text));
            addressInModule.push_back(symbolic);
            widestAddress = std::max(widestAddress, ImGui::CalcTextSize(addressTexts.back().c_str()).x);
        }
        PopMono();
        if (m_addressProcess != Raw(process.Id()))
        {
            m_addressProcess = Raw(process.Id());
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
        const float mouseY = ImGui::GetIO().MousePos.y;
        const bool stickyHovered = hovered && sticky && mouseY < linesOrigin.y;
        const int hoveredRow = hovered && mouseY >= linesOrigin.y ? static_cast<int>((mouseY - linesOrigin.y) / rowHeight) : -1;
        if (stickyHovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Go to the start of %s", functionName.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                m_focusCode = true;
                Navigate(stickyStart);
            }
        }
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
                m_top = disassembler.BackUp(process, m_top, steps, m_engine.CodeMap());
            else if (-steps < static_cast<int>(m_lines.size()))
                m_top = m_lines[static_cast<std::size_t>(-steps)].Address;
        }

        const std::uint64_t selectionLow = std::min(m_selected, m_selectionEnd);
        const std::uint64_t selectionHigh = std::max(m_selected, m_selectionEnd);
        const auto breakpoints = debugger.Breakpoints();
        drawList->PushClipRect(linesOrigin, ImVec2(origin.x + width, origin.y + bodyHeight), true);
        for (std::size_t i = 0; i < m_lines.size(); ++i)
        {
            const Disasm::Instruction& line = m_lines[i];
            const float y = linesOrigin.y + static_cast<float>(i) * rowHeight;
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
                drawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.03f)));

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

            const std::optional<std::string>& methodHeader = headers[i];
            if (methodHeader && blockStarts[i] && i != 0)
                drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), ImGui::GetColorU32(ManifoldTheme::RowSeparator));

            std::string comment;
            ImU32 commentColor = ImGui::GetColorU32(ManifoldTheme::Muted);
            const auto note = m_engine.Table().DisasmNotes.find(m_engine.Symbols().ModuleOffset(line.Address));
            const bool userComment = note != m_engine.Table().DisasmNotes.end() && !note->second.Comment.empty();
            if (userComment)
            {
                comment = note->second.Comment;
                commentColor = ImGui::GetColorU32(ManifoldTheme::CodeComment);
            }
            else if (methodHeader)
            {
                comment = *methodHeader;
                commentColor = ImGui::GetColorU32(ManifoldTheme::Accent);
            }
            else if (line.Memory.Present && line.Memory.Absolute && !line.InlineBytes)
            {
                std::uint8_t value[8]{};
                const int size = std::clamp(line.Memory.Size, 1, 8);
                if (process.ReadExact(*line.Memory.Absolute, value, static_cast<std::size_t>(size)))
                {
                    std::uint64_t number = 0;
                    std::memcpy(&number, value, static_cast<std::size_t>(size));
                    comment = "[" + m_engine.Symbols().RegisteredName(*line.Memory.Absolute).value_or(Hex(*line.Memory.Absolute)) + "]=" + Hex(number);
                }
            }
            else if (info.Broken && isIp && line.Memory.Present)
            {
                if (const auto effective = disassembler.EffectiveAddress(line, info.Registers))
                    comment = "-> " + m_engine.Describe(*effective);
            }
            if (!comment.empty())
                cell(commentX, rightX + Px(4.0f), textY, commentColor, comment);

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
                    const float y2 = linesOrigin.y + static_cast<float>(j) * rowHeight + rowHeight * 0.5f;
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
        if (sticky)
        {
            const ImVec2 stickyMin(origin.x, origin.y);
            const ImVec2 stickyMax(origin.x + width, linesOrigin.y);
            drawList->AddRectFilled(stickyMin, stickyMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, stickyHovered ? 0.16f : 0.08f)));
            drawList->AddLine(ImVec2(stickyMin.x, stickyMax.y - 1.0f), ImVec2(stickyMax.x, stickyMax.y - 1.0f), ImGui::GetColorU32(ManifoldTheme::RowSeparator));
            const float textY = origin.y + Px(1.5f);
            drawList->AddText(ImVec2(origin.x + gutter + Px(2.0f), textY), ImGui::GetColorU32(ManifoldTheme::Accent), ICON_FA_LEVEL_UP_ALT);
            cell(addressX, bytesX, textY, ImGui::GetColorU32(ManifoldTheme::Muted), Hex(stickyStart, process.Is64Bit() ? 12 : 8));
            cell(bytesX, rightX + Px(4.0f), textY, ImGui::GetColorU32(ManifoldTheme::Accent), functionName);
        }
        PopMono();

        if (hoveredRow >= 0 && hoveredRow < static_cast<int>(m_lines.size()) && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && hovered)
        {
            PublishSelection();
            m_engine.Lua().MenuOpening(Lua::ScriptMenu::MemoryViewPopup);
            m_scriptPopup = m_engine.Lua().MenuItems(Lua::ScriptMenu::MemoryViewPopup);
            ImGui::OpenPopup("##codemenu");
        }
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
        const auto ensureVisible = [&]()
            {
                const bool visible = !m_lines.empty() && m_selected >= m_lines.front().Address && static_cast<int>(m_lines.size()) > 2 &&
                                     m_selected < m_lines[std::min<std::size_t>(m_lines.size() - 2, static_cast<std::size_t>(m_visibleLines - 1))].Address;
                if (!visible)
                    m_top = disassembler.BackUp(process, m_selected, std::max(1, m_visibleLines / 3), m_engine.CodeMap());
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
            m_selected = disassembler.BackUp(process, m_selected, 1, m_engine.CodeMap());
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
            m_top = disassembler.BackUp(process, m_top, std::max(1, m_visibleLines - 1), m_engine.CodeMap());
            m_selected = m_selectionEnd = m_top;
        }
    }

    void MemoryViewer::Shortcuts()
    {
        const bool code = m_focusCode;
        const bool hex = !m_focusCode && m_hexFocus;
        std::vector<ImGuiKeyChord> scripted;
        ScriptShortcuts(scripted);
        const auto byScript = [&](const Binding& binding)
            {
                return std::find(scripted.begin(), scripted.end(), binding.Chord) != scripted.end() ||
                       (binding.Alternate && std::find(scripted.begin(), scripted.end(), binding.Alternate) != scripted.end());
            };
        std::vector<ImGuiKeyChord> claimed;
        for (const CommandInfo& info : AllCommands())
        {
            if (!(info.Scope == CommandScope::Disassembler && code) && !(info.Scope == CommandScope::HexView && hex))
                continue;
            const Binding binding = CurrentBinding(info.Id);
            if (!info.FromCheatEngine && byScript(binding))
                continue;
            claimed.push_back(binding.Chord);
            if (binding.Alternate)
                claimed.push_back(binding.Alternate);
            if (info.Digits)
            {
                const int digit = PressedDigit(info.Id);
                if (digit >= 0)
                    Bookmark(info.Id == Command::SetBookmark, digit);
                continue;
            }
            if (Pressed(info.Id))
                RunCommand(info.Id);
        }
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope != CommandScope::MemoryViewer || std::find(claimed.begin(), claimed.end(), CurrentBinding(info.Id).Chord) != claimed.end())
                continue;
            if (!info.FromCheatEngine && byScript(CurrentBinding(info.Id)))
                continue;
            if (Pressed(info.Id))
                RunCommand(info.Id);
        }
    }

    void MemoryViewer::Bookmark(bool set, int slot)
    {
        std::uint64_t& bookmark = m_bookmarks[static_cast<std::size_t>(slot)];
        if (set)
        {
            bookmark = m_selected;
            m_engine.Notify(LogLevel::Info, "Bookmark " + std::to_string(slot) + ": " + m_engine.Describe(m_selected));
        }
        else if (bookmark)
            Navigate(bookmark);
        else
            m_engine.Notify(LogLevel::Info, "Bookmark " + std::to_string(slot) + " is not set. Set it with " + ChordText((CurrentBinding(Command::SetBookmark).Chord & ImGuiMod_Mask_) | (ImGuiKey_0 + slot)) + ".");
    }

    Status MemoryViewer::WriteBytes(std::uint64_t address, std::span<const std::uint8_t> bytes, std::string source)
    {
        return m_engine.Writes().Write(*m_engine.Process(), m_engine.ProcessGeneration(), address, bytes, std::move(source));
    }

    std::size_t MemoryViewer::UndoableEdits() const
    {
        return m_engine.Writes().Undoable(m_engine.ProcessGeneration());
    }

    void MemoryViewer::UndoEdit()
    {
        WriteHistory& history = m_engine.Writes();
        const auto id = history.LastUndoable(m_engine.ProcessGeneration());
        const MemoryWrite* write = id ? history.Find(*id) : nullptr;
        if (!write || !m_engine.HasProcess())
        {
            m_engine.Notify(LogLevel::Info, "There is no memory edit to undo.");
            return;
        }
        const std::uint64_t address = write->Address;
        const std::size_t size = write->Original.size();
        const Status status = history.Undo(*m_engine.Process(), m_engine.ProcessGeneration(), *id);
        if (status)
        {
            m_nopped.erase(address);
            m_engine.Notify(LogLevel::Info, "Restored " + std::to_string(size) + (size == 1 ? " byte at " : " bytes at ") + m_engine.Describe(address));
        }
        m_workspace.Report(status);
    }

    Scan::ValueType MemoryViewer::HexValueType() const
    {
        const int size = HexValueSize();
        if (m_hexDisplay == 5)
            return Scan::ValueType::Float;
        if (m_hexDisplay == 6)
            return Scan::ValueType::Double;
        return size == 1 ? Scan::ValueType::Byte : size == 2 ? Scan::ValueType::TwoBytes : size == 8 ? Scan::ValueType::EightBytes : Scan::ValueType::FourBytes;
    }

    void MemoryViewer::RunCommand(Command command)
    {
        if (command == Command::NewViewerWindow)
        {
            m_workspace.OpenMemoryViewer(m_selected, false, true);
            return;
        }
        if (!m_engine.HasProcess())
            return;
        const Target::Process& process = *m_engine.Process();
        const Disasm::Disassembler& disassembler = m_engine.Disassembler();
        Debug::Debugger& debugger = m_engine.Debugger();
        const std::uint64_t hexAddress = m_hexSelected ? m_hexSelected : m_hexTop;
        const int watchSize = HexValueSize() == 1 ? 4 : HexValueSize();
        const auto resume = [&](Debug::ResumeMode mode)
            {
                if (debugger.Broken())
                    debugger.Resume(mode);
            };
        switch (command)
        {
        case Command::SaveMemoryRegion: OpenRegionDialog(RegionSave); break;
        case Command::LoadMemoryRegion: OpenRegionDialog(RegionLoad); break;
        case Command::FillMemory: OpenRegionDialog(RegionFill); break;
        case Command::GotoAddress:
            m_gotoOpen = true;
            m_gotoHex = !m_focusCode && m_hexFocus;
            m_gotoText = Hex(m_gotoHex ? hexAddress : m_selected);
            break;
        case Command::FindMemory: m_findOpen = true; break;
        case Command::FindAssembly: m_workspace.OpenAsmSearch(); break;
        case Command::UndoMemoryEdit: UndoEdit(); break;
        case Command::MemoryRegions: m_workspace.OpenRegions(); break;
        case Command::ReferencedFunctions: m_workspace.OpenCodeDissect(CodeDissectWindow::Functions); break;
        case Command::ReferencedStrings: m_workspace.OpenCodeDissect(CodeDissectWindow::Strings); break;
        case Command::SymbolsAndModules: m_workspace.OpenSymbols(); break;
        case Command::DissectCode: m_workspace.OpenCodeDissect(); break;
        case Command::DissectData: m_workspace.OpenStructure(hexAddress); break;
        case Command::ViewerAutoAssemble: m_workspace.OpenScript(Asm::CheatTableTemplate()); break;
        case Command::ViewerLuaEngine: m_workspace.OpenLua(); break;
        case Command::InjectLibrary: m_injectOpen = true; break;
        case Command::AllocateMemory: m_allocateOpen = true; break;
        case Command::CreateThread:
            m_threadOpen = true;
            m_threadAddress = Hex(m_selected);
            break;
        case Command::ScanForPatches: m_workspace.OpenPatchScan(); break;
        case Command::DebugRun: resume(Debug::ResumeMode::Continue); break;
        case Command::DebugStepInto: resume(Debug::ResumeMode::StepInto); break;
        case Command::DebugStepOver: resume(Debug::ResumeMode::StepOver); break;
        case Command::DebugRunToReturn: resume(Debug::ResumeMode::RunToReturn); break;
        case Command::DebugBreak:
            if (!debugger.Broken())
            {
                Status status = m_engine.EnsureDebugger();
                if (status)
                    status = debugger.BreakNow();
                m_workspace.Report(status);
            }
            break;
        case Command::ToggleBreakpoint:
        {
            Status status = m_engine.EnsureDebugger();
            if (status)
                status = debugger.ToggleBreakpoint(m_selected);
            m_workspace.Report(status);
            break;
        }
        case Command::SetInstructionPointer:
            if (debugger.Broken())
            {
                Disasm::RegisterValues registers = debugger.Break().Registers;
                registers.Rip = m_selected;
                debugger.SetRegisters(registers);
            }
            break;
        case Command::BreakpointList: m_workspace.OpenBreakpoints(); break;
        case Command::ThreadList: m_workspace.OpenThreadList(); break;
        case Command::EditBreakpoint: m_workspace.OpenBreakpointEditor(m_selected, Debug::BreakpointType::Execute, 1, m_viewport); break;
        case Command::BreakAndTrace:
        {
            Status status = m_engine.EnsureDebugger();
            if (status)
                m_workspace.OpenTrace(m_selected, m_viewport);
            else
                m_workspace.Report(status);
            break;
        }
        case Command::CommentInstruction:
        {
            m_commentAddress = m_selected;
            m_commentHeader = false;
            const std::string key = m_engine.Symbols().ModuleOffset(m_selected);
            const auto found = m_engine.Table().DisasmNotes.find(key);
            m_commentText = found == m_engine.Table().DisasmNotes.end() ? std::string() : found->second.Comment;
            m_commentOpen = true;
            break;
        }
        case Command::FollowInstruction:
        {
            const Disasm::Instruction instruction = disassembler.Read(process, m_selected, nullptr);
            if (instruction.Target)
                Navigate(*instruction.Target);
            else if (instruction.Memory.Absolute)
            {
                m_hexTop = *instruction.Memory.Absolute & ~0xFull;
                m_hexSelected = *instruction.Memory.Absolute;
            }
            break;
        }
        case Command::NavigateBack:
            if (!m_history.empty())
            {
                const std::uint64_t back = m_history.back();
                m_history.pop_back();
                Navigate(back, false);
            }
            break;
        case Command::AssembleInstruction:
        {
            const Disasm::Instruction instruction = disassembler.Read(process, m_selected, nullptr);
            m_assembleOpen = true;
            m_assembleAddress = m_selected;
            m_assembleText = instruction.Valid ? instruction.Text() : "";
            m_assembleError.clear();
            break;
        }
        case Command::NopInstruction: Nop(disassembler.Read(process, m_selected, nullptr)); break;
        case Command::CopyInstructions:
            CopyInstructions(m_engine.Options().ShowSymbols ? Target::AddressStyle::Name : Target::AddressStyle::ModuleOffset);
            break;
        case Command::CopyInstructionBytes:
        {
            std::string text;
            for (const Disasm::Instruction& instruction : SelectedInstructions(false))
                text += instruction.BytesText() + "\n";
            ImGui::SetClipboardText(text.c_str());
            break;
        }
        case Command::InstructionAccesses:
            if (disassembler.Read(process, m_selected, nullptr).Memory.Present)
                m_workspace.StartWatch(Debug::WatchKind::InstructionAccesses, m_selected, 1);
            else
                m_engine.Notify(LogLevel::Info, "The selected instruction does not access memory.");
            break;
        case Command::CodeReferences: m_workspace.OpenCodeDissect(CodeDissectWindow::References, m_selected); break;
        case Command::FlowGraph: m_workspace.OpenFlowGraph(m_selected); break;
        case Command::GotoFunctionStart:
        case Command::GotoFunctionEnd:
        case Command::SelectFunction: JumpToFunction(command); break;
        case Command::AddInstructionToList:
            m_workspace.AddAddress(m_selected, Scan::ValueType::ByteArray, disassembler.Read(process, m_selected, &m_engine.Symbols()).Text());
            break;
        case Command::HexFollow:
            if (const auto target = process.ReadPointer(hexAddress); target && process.RegionAt(*target))
            {
                m_hexHistory.push_back(hexAddress);
                m_hexTop = *target & ~0xFull;
                m_hexSelected = *target;
            }
            else
                m_engine.Notify(LogLevel::Info, "The value at " + Hex(hexAddress) + " does not point into readable memory.");
            break;
        case Command::HexBack:
            if (!m_hexHistory.empty())
            {
                m_hexSelected = m_hexHistory.back();
                m_hexTop = m_hexSelected & ~0xFull;
                m_hexHistory.pop_back();
            }
            break;
        case Command::HexEditValue:
            if (m_hexDisplay != 0)
                m_hexEditing = true;
            break;
        case Command::HexCopy:
        {
            std::uint8_t bytes[16]{};
            if (process.ReadExact(hexAddress, bytes, sizeof(bytes)))
                ImGui::SetClipboardText(HexBytes(bytes).c_str());
            break;
        }
        case Command::HexPaste:
        {
            const char* clipboard = ImGui::GetClipboardText();
            const auto bytes = clipboard ? ParseHexBytes(clipboard) : std::nullopt;
            if (!bytes || bytes->empty())
                m_engine.Notify(LogLevel::Warning, "The clipboard holds no hex bytes, like \"48 8B 05\".");
            else
                m_workspace.Report(WriteBytes(hexAddress, *bytes, "Paste"));
            break;
        }
        case Command::HexAccesses: m_workspace.StartWatch(Debug::WatchKind::Accesses, hexAddress, watchSize); break;
        case Command::HexWrites: m_workspace.StartWatch(Debug::WatchKind::Writes, hexAddress, watchSize); break;
        case Command::HexBreakOnWrite:
        case Command::HexBreakOnAccess:
        {
            Status status = m_engine.EnsureDebugger();
            if (status)
                status = debugger.SetBreakpoint(hexAddress, command == Command::HexBreakOnAccess ? Debug::BreakpointType::Access : Debug::BreakpointType::Write, watchSize, true);
            m_workspace.Report(status);
            break;
        }
        case Command::HexAddToList: m_workspace.AddAddress(hexAddress, HexValueType()); break;
        case Command::HexBytes: m_hexDisplay = 0; break;
        case Command::HexTwoBytes: m_hexDisplay = 1; break;
        case Command::HexFourBytes: m_hexDisplay = 2; break;
        case Command::HexEightBytes: m_hexDisplay = 3; break;
        case Command::HexFourBytesDecimal: m_hexDisplay = 4; break;
        case Command::HexFloat: m_hexDisplay = 5; break;
        case Command::HexDouble: m_hexDisplay = 6; break;
        default: break;
        }
    }

    void MemoryViewer::OpenRegionDialog(int mode)
    {
        m_regionMode = mode;
        m_regionOpen = true;
        m_regionError.clear();
        const std::uint64_t address = mode == RegionFill || m_hexFocus ? (m_hexSelected ? m_hexSelected : m_hexTop) : m_selected;
        m_regionAddress = Hex(address);
    }

    void MemoryViewer::RegionDialog()
    {
        if (m_regionOpen)
        {
            ImGui::OpenPopup("##region");
            m_regionOpen = false;
        }
        if (!ImGui::BeginPopup("##region"))
            return;
        const Target::Process& process = *m_engine.Process();
        static const char* const kTitles[] = { "Save a memory region to a file", "Load a file into memory", "Fill memory" };
        ImGui::TextUnformatted(kTitles[m_regionMode]);
        const float label = Px(110.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Address");
        ImGui::SameLine(label);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Px(260.0f));
        bool enter = InputText("##regionaddress", m_regionAddress, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (m_regionMode != RegionLoad)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Size (hex)");
            ImGui::SameLine(label);
            ImGui::SetNextItemWidth(Px(260.0f));
            enter = InputText("##regionsize", m_regionSize, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsHexadecimal) || enter;
        }
        if (m_regionMode == RegionFill)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Bytes");
            ImGui::SameLine(label);
            ImGui::SetNextItemWidth(Px(260.0f));
            enter = InputTextWithHint("##regionfill", "90 or CC or 00 01 02", m_regionFill, ImGuiInputTextFlags_EnterReturnsTrue) || enter;
            Muted("The bytes repeat until the size is filled.");
        }
        if (!m_regionError.empty())
            TextWithError(m_regionError, ManifoldTheme::Error);
        static const char* const kButtons[] = { ICON_FA_FILE_EXPORT "  Save...", ICON_FA_FILE_IMPORT "  Load...", ICON_FA_FILL_DRIP "  Fill" };
        if (AccentButton(kButtons[m_regionMode], Px(110.0f, 0.0f)) || enter)
        {
            Status error;
            const auto address = m_engine.Evaluate(m_regionAddress, &error);
            const std::uint64_t size = ParseHex(m_regionSize).value_or(0);
            constexpr std::uint64_t kLimit = 64ull << 20;
            if (!address)
                m_regionError = error.Text;
            else if (m_regionMode != RegionLoad && (size == 0 || size > kLimit))
                m_regionError = "The size must be between 1 and 4000000 (64 MB).";
            else if (m_regionMode == RegionSave)
            {
                std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
                std::size_t missing = 0;
                for (std::uint64_t at = 0; at < size;)
                {
                    const std::uint64_t chunk = std::min<std::uint64_t>(size - at, 0x1000 - ((*address + at) & 0xFFF));
                    if (!process.ReadExact(*address + at, data.data() + at, static_cast<std::size_t>(chunk)))
                        missing += static_cast<std::size_t>(chunk);
                    at += chunk;
                }
                if (const auto path = m_workspace.SaveFileDialog({ { L"Binary files (*.bin)", L"*.bin" }, { L"All files", L"*.*" } }, "memory_" + Hex(*address) + ".bin", L"bin"))
                {
                    std::ofstream file(*path, std::ios::binary | std::ios::trunc);
                    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
                    if (!file)
                        m_regionError = "Could not write " + WideToUtf8(path->wstring()) + ".";
                    else
                    {
                        m_engine.Notify(missing ? LogLevel::Warning : LogLevel::Success, "Saved " + ByteSize(size) + " from " + Hex(*address) + " to " + WideToUtf8(path->filename().wstring()) +
                            (missing ? " (" + ByteSize(missing) + " could not be read and are zero)" : std::string()));
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            else if (m_regionMode == RegionLoad)
            {
                if (const auto path = m_workspace.OpenFileDialog({ { L"Binary files (*.bin)", L"*.bin" }, { L"All files", L"*.*" } }))
                {
                    std::error_code ignored;
                    const std::uint64_t length = std::filesystem::file_size(*path, ignored);
                    std::ifstream file(*path, std::ios::binary);
                    if (!file || length == 0 || length > kLimit)
                        m_regionError = "The file must exist and hold between 1 byte and 64 MB.";
                    else
                    {
                        std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
                        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
                        const Status status = WriteBytes(*address, data, "Load file");
                        m_workspace.Report(status);
                        if (status)
                        {
                            m_engine.Notify(LogLevel::Success, "Loaded " + ByteSize(length) + " to " + m_engine.Describe(*address) + UndoNote());
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
            else
            {
                const auto pattern = ParseHexBytes(m_regionFill);
                if (!pattern || pattern->empty())
                    m_regionError = "Type the bytes as hex, like 90 or 00 FF.";
                else
                {
                    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
                    for (std::size_t i = 0; i < data.size(); ++i)
                        data[i] = (*pattern)[i % pattern->size()];
                    const Status status = WriteBytes(*address, data, "Fill");
                    m_workspace.Report(status);
                    if (status)
                    {
                        m_engine.Notify(LogLevel::Success, "Filled " + ByteSize(size) + " at " + m_engine.Describe(*address) + UndoNote());
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(90.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void MemoryViewer::ThreadDialog()
    {
        if (m_threadOpen)
        {
            ImGui::OpenPopup("##thread");
            m_threadOpen = false;
            m_threadError.clear();
        }
        if (!ImGui::BeginPopup("##thread"))
            return;
        ImGui::TextUnformatted("Create a thread in the process");
        const float label = Px(110.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Start address");
        ImGui::SameLine(label);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Px(260.0f));
        bool enter = InputText("##threadaddress", m_threadAddress, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Parameter");
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(Px(260.0f));
        enter = InputText("##threadparameter", m_threadParameter, ImGuiInputTextFlags_EnterReturnsTrue) || enter;
        if (!m_threadError.empty())
            TextWithError(m_threadError, ManifoldTheme::Error);
        if (AccentButton(ICON_FA_PLAY "  Create", Px(110.0f, 0.0f)) || enter)
        {
            Status error;
            const auto start = m_engine.Evaluate(m_threadAddress, &error);
            const auto parameter = start ? m_engine.Evaluate(m_threadParameter.empty() ? "0" : m_threadParameter, &error) : std::nullopt;
            if (!start || !parameter)
                m_threadError = error.Text;
            else
            {
                const Status status = m_engine.Process()->CreateThread(*start, *parameter, false);
                m_workspace.Report(status);
                if (status)
                {
                    m_engine.Notify(LogLevel::Success, "Started a thread at " + m_engine.Describe(*start) + ".");
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(90.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
        const Status status = WriteBytes(instruction.Address, nops, "NOP");
        if (status)
            m_nopped[instruction.Address] = std::move(original);
        m_workspace.Report(status);
    }

    void MemoryViewer::CodeMenu(const Disasm::Instruction& instruction)
    {
        Debug::Debugger& debugger = m_engine.Debugger();
        const bool broken = debugger.Broken();
        const auto item = [&](const char* label, Command command, bool enabled = true)
            {
                if (CommandItem(label, command, false, enabled))
                    RunCommand(command);
            };
        ImGui::TextDisabled("%s", m_engine.Describe(instruction.Address).c_str());
        ImGui::Separator();
        item(ICON_FA_LOCATION_ARROW "  Go to address", Command::GotoAddress);
        if (instruction.Target)
            item(ICON_FA_ARROW_RIGHT "  Follow", Command::FollowInstruction);
        item(ICON_FA_ARROW_LEFT "  Back", Command::NavigateBack, !m_history.empty());
        if (ImGui::BeginMenu(ICON_FA_BOOKMARK "  Bookmarks"))
        {
            for (int slot = 0; slot < 10; ++slot)
            {
                const std::string label = "Set bookmark " + std::to_string(slot);
                const std::string keys = ChordText((CurrentBinding(Command::SetBookmark).Chord & ImGuiMod_Mask_) | (ImGuiKey_0 + slot));
                if (ImGui::MenuItem(label.c_str(), keys.c_str()))
                    Bookmark(true, slot);
            }
            ImGui::Separator();
            BookmarkItems();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item(ICON_FA_EDIT "  Assemble", Command::AssembleInstruction);
        item(m_nopped.count(instruction.Address) ? ICON_FA_UNDO "  Restore original code" : ICON_FA_BAN "  Replace with code that does nothing", Command::NopInstruction);
        item(ICON_FA_UNDO_ALT "  Undo the last memory edit", Command::UndoMemoryEdit, UndoableEdits() > 0);
        if (ImGui::BeginMenu(ICON_FA_MAGIC "  Create script"))
        {
            m_templates.MenuItems(instruction.Address);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item(ICON_FA_COMMENT "  Comment...", Command::CommentInstruction);
        ImGui::Separator();
        item(ICON_FA_CIRCLE "  Toggle breakpoint", Command::ToggleBreakpoint);
        item(ICON_FA_SLIDERS_H "  Breakpoint with a condition or register change...", Command::EditBreakpoint);
        item(ICON_FA_SHOE_PRINTS "  Break and trace from here...", Command::BreakAndTrace);
        item(ICON_FA_LIST_UL "  Find out what addresses this instruction accesses", Command::InstructionAccesses, instruction.Memory.Present);
        if (ImGui::MenuItem(ICON_FA_PHONE "  Find out what calls this address"))
            m_workspace.StartWatch(Debug::WatchKind::Calls, instruction.Address, 1);
        {
            const auto dissect = m_engine.CodeDissect();
            const std::size_t references = dissect ? dissect->To(instruction.Address).size() : 0;
            const std::string label = dissect ? ICON_FA_PROJECT_DIAGRAM "  Code that references this address (" + std::to_string(references) + ")" : ICON_FA_PROJECT_DIAGRAM "  Code that references this address...";
            item(label.c_str(), Command::CodeReferences);
            ItemTooltip(dissect ? "From the last code dissect" : "Needs a code dissect first; the window offers one");
            if (instruction.Memory.Absolute && dissect)
            {
                const std::uint64_t operand = *instruction.Memory.Absolute;
                const std::string other = ICON_FA_PROJECT_DIAGRAM "  Code that uses the operand address (" + std::to_string(dissect->To(operand).size()) + ")";
                if (ImGui::MenuItem(other.c_str()))
                    m_workspace.OpenCodeDissect(CodeDissectWindow::References, operand);
            }
        }
        FunctionMenu(instruction);
        if (broken)
            item(ICON_FA_MAP_MARKER_ALT "  Set the instruction pointer here", Command::SetInstructionPointer);
        ImGui::Separator();
        if (instruction.Memory.Absolute && ImGui::MenuItem(ICON_FA_TH "  Show the operand in the hex view", instruction.Target ? nullptr : ShortcutText(Command::FollowInstruction)))
        {
            m_hexTop = *instruction.Memory.Absolute & ~0xFull;
            m_hexSelected = *instruction.Memory.Absolute;
        }
        item(ICON_FA_PLUS "  Add this address to the address list", Command::AddInstructionToList);
        if (ImGui::BeginMenu(ICON_FA_COPY "  Copy"))
        {
            item(m_engine.Options().ShowSymbols ? "Selected instructions with names" : "Selected instructions with module offsets", Command::CopyInstructions);
            if (ImGui::MenuItem(m_engine.Options().ShowSymbols ? "Selected instructions with module offsets" : "Selected instructions with names"))
                CopyInstructions(m_engine.Options().ShowSymbols ? Target::AddressStyle::ModuleOffset : Target::AddressStyle::Name);
            if (ImGui::MenuItem("Selected instructions with plain addresses"))
                CopyInstructions(Target::AddressStyle::Plain);
            item("Bytes only", Command::CopyInstructionBytes);
            ImGui::Separator();
            AddressCopyItems(instruction.Address);
            if (ImGui::MenuItem("Bytes"))
                ImGui::SetClipboardText(instruction.BytesText().c_str());
            if (ImGui::MenuItem("Instruction"))
                ImGui::SetClipboardText(instruction.Text().c_str());
            ImGui::EndMenu();
        }
        if (!m_scriptPopup.empty())
        {
            ImGui::Separator();
            if (const int clicked = DrawScriptMenuItems(m_scriptPopup))
            {
                PublishSelection();
                m_engine.Lua().ClickMenuItem(clicked);
            }
        }
    }

    bool MemoryViewer::RegisterLine(const std::string& id, std::string_view name, const std::string& editText, float rowHeight, float nameWidth, std::string& edited,
        ImVec2& valueMin, ImVec2& valueMax)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float line = ImGui::GetTextLineHeight();
        const float right = start.x + ImGui::GetContentRegionAvail().x;
        valueMin = valueMax = start;
        if (m_registerEdit == id)
        {
            const float frame = ImGui::GetFrameHeight();
            const float height = std::max(rowHeight, frame);
            draw->AddText(ImVec2(start.x + Px(4.0f), std::round(start.y + (height - line) * 0.5f)), ImGui::GetColorU32(ManifoldTheme::CodeRegister),
                std::string(name).c_str());
            ImGui::SetCursorScreenPos(ImVec2(start.x + nameWidth, start.y + (height - frame) * 0.5f));
            ImGui::SetNextItemWidth(right - (start.x + nameWidth));
            if (m_registerEditFocus)
            {
                ImGui::SetKeyboardFocusHere();
                m_registerEditFocus = false;
            }
            const bool enter = InputText("##edit", m_registerEditText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            bool committed = false;
            if (enter)
            {
                edited = m_registerEditText;
                committed = true;
                m_registerEdit.clear();
            }
            else if (ImGui::IsItemDeactivated() || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                m_registerEdit.clear();
            }
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            return committed;
        }
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight)) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            !m_registersRunning)
        {
            m_registerEdit = id;
            m_registerEditText = editText;
            m_registerEditFocus = true;
        }
        const float textY = std::round(start.y + (rowHeight - line) * 0.5f);
        draw->AddText(ImVec2(start.x + Px(4.0f), textY), ImGui::GetColorU32(ManifoldTheme::CodeRegister), std::string(name).c_str());
        valueMin = ImVec2(start.x + nameWidth, textY);
        valueMax = ImVec2(right - Px(2.0f), textY + line);
        return false;
    }

    Debug::BreakInfo MemoryViewer::ShownBreak() const
    {
        const Debug::BreakInfo live = m_engine.Debugger().Break();
        if (!live.Broken && m_heldBreak.Broken && m_engine.Debugger().Stepping())
            return m_heldBreak;
        return live;
    }

    void MemoryViewer::Registers(float width, float height, const Debug::BreakInfo& info, bool running)
    {
        Debug::Debugger& debugger = m_engine.Debugger();
        m_registersRunning = running;
        if (running)
            m_registerEdit.clear();
        if (info.Serial != m_registerSerial)
        {
            m_havePreviousRegisters = m_registerSerial != 0;
            m_previousRegisters = m_lastRegisters;
            m_previousVectors = m_lastVectors;
            m_registerSerial = info.Serial;
            m_registerEdit.clear();
        }
        m_lastRegisters = info.Registers;
        m_lastVectors = info.Vectors;

        if (info.Serial != m_conditionSerial || info.Registers.Rip != m_conditionAddress)
        {
            m_conditionSerial = info.Serial;
            m_conditionAddress = info.Registers.Rip;
            std::uint8_t bytes[16] = {};
            const std::size_t got = m_engine.Process()->Read(info.Registers.Rip, bytes, sizeof(bytes));
            debugger.Unpatch(info.Registers.Rip, bytes, sizeof(bytes));
            m_conditionInstruction = got ? m_engine.Disassembler().Decode(info.Registers.Rip, bytes, got, nullptr) : Disasm::Instruction{};
        }
        const Disasm::ConditionInfo condition = m_conditionInstruction.Valid ? Disasm::EvaluateCondition(m_conditionInstruction.Mnemonic, info.Registers, info.Is64Bit)
                                                                            : Disasm::ConditionInfo{};

        BeginCard("##registers", nullptr, ImVec2(width, height));
        CardTitle(ICON_FA_MICROCHIP, "Registers", "thread " + std::to_string(info.ThreadId) + (running ? "  ·  stepping..." : ""));
        PushMono();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float line = ImGui::GetTextLineHeight();
        const float rowHeight = std::round(line + Px(5.0f));
        const float nameWidth = ImGui::CalcTextSize("XMM15").x + Px(14.0f);
        const ImU32 textColor = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 changedColor = ImGui::GetColorU32(ManifoldTheme::Changed);
        const ImU32 mutedColor = ImGui::GetColorU32(ManifoldTheme::Muted);
        const int digits = info.Is64Bit ? 16 : 8;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));

        Disasm::RegisterValues values = info.Registers;
        bool changed = false;
        for (const RegisterRow& row : kRegisterRows)
        {
            if (row.Only64 && !info.Is64Bit)
                continue;
            std::string name = row.Name;
            if (!info.Is64Bit)
                name[0] = 'E';
            const std::uint64_t value = values.*row.Field;
            const std::string text = Hex(value, digits);
            ImGui::PushID(row.Name);
            std::string edited;
            ImVec2 valueMin;
            ImVec2 valueMax;
            if (RegisterLine(row.Name, name, text, rowHeight, nameWidth, edited, valueMin, valueMax))
            {
                if (const auto parsed = ParseHex(edited))
                {
                    values.*row.Field = info.Is64Bit ? *parsed : (*parsed & 0xFFFFFFFFull);
                    changed = true;
                }
                else
                {
                    m_engine.Notify(LogLevel::Error, "\"" + edited + "\" is not a hex number.");
                }
            }
            if (valueMax.x > valueMin.x)
            {
                if (condition.UsesCounter && row.Field == &Disasm::RegisterValues::Rcx)
                    draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ManifoldTheme::Accent), ImGui::GetStyle().FrameRounding);
                const bool differs = m_havePreviousRegisters && m_previousRegisters.*row.Field != value;
                draw->AddText(valueMin, differs ? changedColor : textColor, text.c_str());
                const std::string described = m_engine.Describe(value);
                const float used = ImGui::CalcTextSize(text.c_str()).x + Px(10.0f);
                if (described != Hex(value) && valueMax.x - valueMin.x > used + Px(30.0f))
                    ClippedText(draw, ImVec2(valueMin.x + used, valueMin.y), valueMax.x - valueMin.x - used, ImGui::GetColorU32(ManifoldTheme::AddressText), described);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s = %s", name.c_str(), Hex(value).c_str());
                    ImGui::TextColored(ManifoldTheme::Muted, "%llu  ·  %lld", static_cast<unsigned long long>(value), static_cast<long long>(value));
                    if (described != Hex(value))
                        ImGui::TextColored(ManifoldTheme::AddressText, "%s", described.c_str());
                    if (m_havePreviousRegisters && m_previousRegisters.*row.Field != value)
                        ImGui::TextColored(ManifoldTheme::Changed, "was %s", Hex(m_previousRegisters.*row.Field, digits).c_str());
                    if (condition.UsesCounter && row.Field == &Disasm::RegisterValues::Rcx)
                        ImGui::TextColored(ManifoldTheme::Accent, "%s at %s reads this register", m_conditionInstruction.Mnemonic.c_str(), info.Is64Bit ? "RIP" : "EIP");
                    ImGui::TextColored(ManifoldTheme::Muted, "%s", running ? "The thread is running a step" : "Double-click to change it");
                    ImGui::EndTooltip();
                }
            }
            if (ImGui::BeginPopupContextItem("##regmenu"))
            {
                if (ImGui::MenuItem("Change value", nullptr, false, !running))
                {
                    m_registerEdit = row.Name;
                    m_registerEditText = text;
                    m_registerEditFocus = true;
                }
                if (ImGui::MenuItem("Show in hex view"))
                {
                    m_hexTop = value & ~0xFull;
                    m_hexSelected = value;
                }
                if (ImGui::MenuItem("Show in disassembler"))
                    Navigate(value);
                if (ImGui::MenuItem("Copy"))
                    ImGui::SetClipboardText(Hex(value).c_str());
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
        SectionLabel("Flags");
        if (condition.Kind != Disasm::ConditionKind::None)
        {
            const char* verdict = condition.Kind == Disasm::ConditionKind::Move ? (condition.Taken ? "moves" : "does not move")
                                : condition.Kind == Disasm::ConditionKind::Set  ? (condition.Taken ? "sets 1" : "sets 0")
                                                                                : (condition.Taken ? "jumps" : "does not jump");
            const std::string hint = m_conditionInstruction.Mnemonic + " " + verdict;
            const float hintWidth = ImGui::CalcTextSize(hint.c_str()).x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - hintWidth - Px(2.0f)));
            ImGui::TextColored(condition.Taken ? ManifoldTheme::Accent : ManifoldTheme::Muted, "%s", hint.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("%s at %s", m_conditionInstruction.Text().c_str(), info.Is64Bit ? "RIP" : "EIP");
                ImGui::TextColored(ManifoldTheme::Muted, "%s", condition.UsesCounter ? "It depends on RCX; the flags it reads are outlined." : "The flags it reads are outlined.");
                ImGui::EndTooltip();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, Px(2.0f)));
        {
            const float available = ImGui::GetContentRegionAvail().x;
            const float spacing = Px(4.0f);
            const float cell = std::floor((available - spacing * 3.0f) / 4.0f);
            const float cellHeight = std::round(line + Px(6.0f));
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            for (int i = 0; i < static_cast<int>(std::size(kFlags)); ++i)
            {
                const FlagBit& flag = kFlags[i];
                const ImVec2 min(origin.x + static_cast<float>(i % 4) * (cell + spacing), origin.y + static_cast<float>(i / 4) * (cellHeight + spacing));
                const ImVec2 max(min.x + cell, min.y + cellHeight);
                ImGui::SetCursorScreenPos(min);
                ImGui::PushID(flag.Name);
                const bool clicked = ImGui::InvisibleButton("##flag", ImVec2(cell, cellHeight));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool set = (values.Flags >> flag.Bit) & 1;
                const bool differs = m_havePreviousRegisters && (((m_previousRegisters.Flags ^ values.Flags) >> flag.Bit) & 1);
                const bool read = (condition.FlagsRead >> flag.Bit) & 1u;
                const ImVec4 fill = set ? ManifoldTheme::WithAlpha(ManifoldTheme::Accent, hovered ? 0.34f : 0.24f) : ImGui::GetStyleColorVec4(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
                draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), ImGui::GetStyle().FrameRounding);
                if (read)
                    draw->AddRect(min, max, ImGui::GetColorU32(ManifoldTheme::Accent), ImGui::GetStyle().FrameRounding, 0, Px(1.5f));
                const std::string label = std::string(flag.Name) + " " + (set ? "1" : "0");
                const ImVec2 size = ImGui::CalcTextSize(label.c_str());
                draw->AddText(ImVec2(std::round(min.x + (cell - size.x) * 0.5f), std::round(min.y + (cellHeight - size.y) * 0.5f)),
                    differs ? changedColor : set ? textColor : mutedColor, label.c_str());
                if (hovered)
                {
                    const FlagHelp& help = kFlagHelp[i];
                    PopMono();
                    ImGui::BeginTooltip();
                    ImGui::Text("%s  ·  %s", flag.Name, help.Title);
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
                    ImGui::TextUnformatted(help.Meaning);
                    if (help.Readers[0])
                        ImGui::TextColored(ManifoldTheme::Muted, "Read by %s", help.Readers);
                    ImGui::PopTextWrapPos();
                    ImGui::Separator();
                    if (differs)
                        ImGui::TextColored(ManifoldTheme::Changed, "Now %d, was %d at the last stop", set ? 1 : 0, set ? 0 : 1);
                    else
                        ImGui::Text("Now %d", set ? 1 : 0);
                    if (read)
                        ImGui::TextColored(ManifoldTheme::Accent, "%s at %s reads it: %s", m_conditionInstruction.Mnemonic.c_str(), info.Is64Bit ? "RIP" : "EIP",
                            condition.Taken ? (condition.Kind == Disasm::ConditionKind::Jump ? "it jumps" : condition.Kind == Disasm::ConditionKind::Move ? "it moves" : "it sets 1")
                                            : (condition.Kind == Disasm::ConditionKind::Jump ? "it does not jump" : condition.Kind == Disasm::ConditionKind::Move ? "it does not move" : "it sets 0"));
                    ImGui::TextColored(ManifoldTheme::Muted, "%s", running ? "The thread is running a step" : "Click to switch it");
                    ImGui::EndTooltip();
                    PushMono();
                }
                if (clicked && !running)
                {
                    values.Flags ^= 1ull << flag.Bit;
                    changed = true;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (cellHeight + spacing) * 2.0f));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
        if (changed && !running)
            debugger.SetRegisters(values);

        ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
        const Debug::VectorRegisters& vectors = info.Vectors;
        if (vectors.Count > 0)
        {
            SectionLabel("XMM");
            PopMono();
            const float segmentWidth = std::min(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("XMM").x - Px(16.0f), Px(220.0f));
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - segmentWidth);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (ImGui::GetFrameHeight() - line) * 0.5f);
            if (Segmented("##vectorformat", &m_vectorFormat, kVectorFormats, segmentWidth))
                m_registerEdit.clear();
            PushMono();
            ImGui::Dummy(ImVec2(0.0f, Px(2.0f)));
            Debug::VectorRegisters newVectors = vectors;
            bool vectorsChanged = false;
            const int lanes = VectorLanes(m_vectorFormat);
            for (int i = 0; i <= vectors.Count; ++i)
            {
                const bool mxcsr = i == vectors.Count;
                const std::string name = mxcsr ? "MXCSR" : "XMM" + std::to_string(i);
                const std::string id = "##" + name;
                const auto& xmm = vectors.Xmm[static_cast<std::size_t>(mxcsr ? 0 : i)];
                const std::string editText = mxcsr ? Hex(vectors.Mxcsr, 8) : VectorText(xmm, m_vectorFormat);
                ImGui::PushID(name.c_str());
                std::string edited;
                ImVec2 valueMin;
                ImVec2 valueMax;
                if (RegisterLine(id, name, editText, rowHeight, nameWidth, edited, valueMin, valueMax))
                {
                    if (mxcsr)
                    {
                        if (const auto parsed = ParseHex(edited); parsed && *parsed <= 0xFFFFFFFFull)
                        {
                            newVectors.Mxcsr = static_cast<std::uint32_t>(*parsed);
                            vectorsChanged = true;
                        }
                        else
                        {
                            m_engine.Notify(LogLevel::Error, "\"" + edited + "\" is not a 32-bit hex number.");
                        }
                    }
                    else if (const auto parsed = ParseVector(edited, m_vectorFormat))
                    {
                        newVectors.Xmm[static_cast<std::size_t>(i)] = *parsed;
                        vectorsChanged = true;
                    }
                    else
                    {
                        m_engine.Notify(LogLevel::Error, "\"" + edited + "\" does not give " + std::to_string(lanes) + (lanes == 1 ? " 128-bit hex value." : " " + std::string(kVectorFormats[static_cast<std::size_t>(m_vectorFormat)]) + " values."));
                    }
                }
                if (valueMax.x > valueMin.x)
                {
                    const bool differs = m_havePreviousRegisters && m_previousVectors.Count == vectors.Count && (mxcsr ? m_previousVectors.Mxcsr != vectors.Mxcsr : m_previousVectors.Xmm[static_cast<std::size_t>(i)] != xmm);
                    const ImU32 color = differs ? changedColor : textColor;
                    if (mxcsr)
                    {
                        draw->AddText(valueMin, color, Hex(vectors.Mxcsr, 8).c_str());
                    }
                    else
                    {
                        const float laneWidth = std::floor((valueMax.x - valueMin.x) / static_cast<float>(lanes));
                        for (int lane = 0; lane < lanes; ++lane)
                        {
                            std::string text = LaneText(xmm, m_vectorFormat, lane, m_vectorFormat == 1 ? 10 : 7);
                            for (int fewer = m_vectorFormat == 1 ? 8 : 5; fewer >= 2 && m_vectorFormat < 2 && ImGui::CalcTextSize(text.c_str()).x > laneWidth - Px(8.0f); fewer -= 2)
                                text = LaneText(xmm, m_vectorFormat, lane, fewer);
                            ClippedText(draw, ImVec2(valueMin.x + laneWidth * static_cast<float>(lane), valueMin.y), laneWidth - Px(6.0f), color, text);
                        }
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        if (mxcsr)
                        {
                            ImGui::Text("MXCSR = %s", Hex(vectors.Mxcsr, 8).c_str());
                        }
                        else
                        {
                            ImGui::Text("%s", name.c_str());
                            for (int format = 0; format < static_cast<int>(kVectorFormats.size()); ++format)
                            {
                                std::string all;
                                for (int lane = 0; lane < VectorLanes(format); ++lane)
                                    all += (lane ? "   " : "") + LaneText(xmm, format, lane, format == 1 ? 17 : 9);
                                ImGui::TextColored(ManifoldTheme::Muted, "%-6s", kVectorFormats[static_cast<std::size_t>(format)]);
                                ImGui::SameLine();
                                ImGui::TextUnformatted(all.c_str());
                            }
                            ImGui::TextColored(ManifoldTheme::Muted, "Lane 0 first. Double-click to change it.");
                        }
                        ImGui::EndTooltip();
                    }
                }
                if (ImGui::BeginPopupContextItem("##xmmmenu"))
                {
                    if (ImGui::MenuItem("Change value", nullptr, false, !running))
                    {
                        m_registerEdit = id;
                        m_registerEditText = editText;
                        m_registerEditFocus = true;
                    }
                    if (ImGui::MenuItem("Copy"))
                        ImGui::SetClipboardText(editText.c_str());
                    if (!mxcsr && ImGui::MenuItem("Set to zero", nullptr, false, !running))
                    {
                        newVectors.Xmm[static_cast<std::size_t>(i)] = {};
                        vectorsChanged = true;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (vectorsChanged && !running)
                debugger.SetVectors(newVectors);
            ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
        }
        else if (debugger.UsingVeh())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ManifoldTheme::Muted, "XMM registers come from the in-process debugger. The one loaded into this process is older and does not send them; they appear after the process restarts.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
        }

        ImGui::PopStyleVar();
        PopMono();
        EndCard();
    }

    void MemoryViewer::Stack(float width, float height, const Debug::BreakInfo& info, bool running)
    {
        const Disasm::RegisterValues& values = info.Registers;
        const int pointer = info.Is64Bit ? 8 : 4;
        BeginCard("##stack", nullptr, ImVec2(width, height));
        CardTitle(ICON_FA_LAYER_GROUP, "Stack", std::string(info.Is64Bit ? "RSP " : "ESP ") + Hex(values.Rsp, pointer * 2) + (running ? "  ·  stepping..." : ""));
        PushMono();
        constexpr int kEntries = 512;
        std::vector<std::uint8_t> data(static_cast<std::size_t>(kEntries * pointer));
        const std::size_t got = m_engine.Process()->Read(values.Rsp, data.data(), data.size());
        const int entries = static_cast<int>(got / static_cast<std::size_t>(pointer));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float line = ImGui::GetTextLineHeight();
        const float rowHeight = std::round(line + Px(5.0f));
        const float offsetWidth = ImGui::CalcTextSize("+0000").x + Px(14.0f);
        const ImU32 textColor = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 mutedColor = ImGui::GetColorU32(ManifoldTheme::Muted);
        const ImU32 registerColor = ImGui::GetColorU32(ManifoldTheme::CodeRegister);
        const ImU32 symbolColor = ImGui::GetColorU32(ManifoldTheme::AddressText);
        if (entries == 0)
            ImGui::TextColored(ManifoldTheme::Muted, "The stack at %s cannot be read.", Hex(values.Rsp).c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGuiListClipper clipper;
        clipper.Begin(entries, rowHeight);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const std::uint64_t at = values.Rsp + static_cast<std::uint64_t>(i * pointer);
                std::uint64_t value = 0;
                std::memcpy(&value, data.data() + static_cast<std::size_t>(i * pointer), static_cast<std::size_t>(pointer));
                ImGui::PushID(i);
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const float right = start.x + ImGui::GetContentRegionAvail().x;
                if (ImGui::Selectable("##slot", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight)) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    Navigate(value);
                const float textY = std::round(start.y + (rowHeight - line) * 0.5f);
                const bool frame = at == values.Rbp;
                const std::string offset = frame ? std::string(info.Is64Bit ? "RBP" : "EBP") : Format("+%X", i * pointer);
                draw->AddText(ImVec2(start.x + Px(4.0f), textY), frame ? registerColor : mutedColor, offset.c_str());
                const std::string text = Hex(value, pointer * 2);
                draw->AddText(ImVec2(start.x + offsetWidth, textY), textColor, text.c_str());
                const std::string described = m_engine.Describe(value);
                const bool named = described != Hex(value);
                const float used = offsetWidth + ImGui::CalcTextSize(text.c_str()).x + Px(10.0f);
                if (named && right - (start.x + used) > Px(30.0f))
                    ClippedText(draw, ImVec2(start.x + used, textY), right - (start.x + used), symbolColor, described);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(ManifoldTheme::Muted, "%s  (%s+%X)", Hex(at).c_str(), info.Is64Bit ? "RSP" : "ESP", i * pointer);
                    ImGui::TextUnformatted(Hex(value).c_str());
                    if (named)
                        ImGui::TextColored(ManifoldTheme::AddressText, "%s", described.c_str());
                    ImGui::TextColored(ManifoldTheme::Muted, "Double-click to show the value in the disassembler");
                    ImGui::EndTooltip();
                }
                if (ImGui::BeginPopupContextItem("##stackmenu"))
                {
                    if (ImGui::MenuItem("Show value in disassembler"))
                        Navigate(value);
                    if (ImGui::MenuItem("Show value in hex view"))
                    {
                        m_hexTop = value & ~0xFull;
                        m_hexSelected = value;
                    }
                    if (ImGui::MenuItem("Show this slot in hex view"))
                    {
                        m_hexTop = at & ~0xFull;
                        m_hexSelected = at;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy value"))
                        ImGui::SetClipboardText(Hex(value).c_str());
                    if (ImGui::MenuItem("Copy address"))
                        ImGui::SetClipboardText(Hex(at).c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopStyleVar();
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
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    m_hexMarkStart = m_hexMarkStop = 0;
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
                const bool marked = m_hexMarkStart && address + static_cast<std::uint64_t>(valueSize) > m_hexMarkStart && address <= m_hexMarkStop;
                if (marked && !selected)
                    drawList->AddRectFilled(ImVec2(x - Px(2.0f), y - Px(1.0f)), ImVec2(x + cellWidth - charWidth * 0.5f, y + rowHeight - Px(1.0f)),
                        ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.16f)), Px(2.0f));
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
                    m_workspace.Report(WriteBytes(m_hexSelected, *bytes, "Value"));
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
        if (m_hexDisplay != 0 || io.KeyCtrl || io.KeyAlt)
            return;
        for (const ImWchar character : io.InputQueueCharacters)
        {
            const int digit = HexValue(static_cast<char>(character));
            if (digit < 0)
                continue;
            std::uint8_t byte = 0;
            if (!m_engine.Process()->ReadExact(m_hexSelected, &byte, 1))
                break;
            byte = m_hexNibble == 0 ? static_cast<std::uint8_t>((byte & 0x0F) | (digit << 4)) : static_cast<std::uint8_t>((byte & 0xF0) | digit);
            const Status status = m_hexNibble == 0 ? WriteBytes(m_hexSelected, std::span<const std::uint8_t>(&byte, 1), "Hex typing") : m_engine.Process()->Write(m_hexSelected, &byte, 1);
            if (!status)
            {
                m_workspace.Report(status);
                break;
            }
            if (m_hexNibble == 0)
                m_nibbleWrite = m_engine.Writes().LastId();
            else
                m_engine.Writes().Amend(m_nibbleWrite, std::span<const std::uint8_t>(&byte, 1));
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

    std::vector<Disasm::Instruction> MemoryViewer::SelectedInstructions(bool names) const
    {
        std::vector<Disasm::Instruction> lines;
        if (!m_engine.HasProcess())
            return lines;
        return InstructionsBetween(std::min(m_selected, m_selectionEnd), std::max(m_selected, m_selectionEnd), names);
    }

    std::vector<Disasm::Instruction> MemoryViewer::InstructionsBetween(std::uint64_t first, std::uint64_t last, bool names) const
    {
        constexpr std::size_t kMaxLines = 100000;
        std::vector<Disasm::Instruction> lines;
        if (!m_engine.HasProcess())
            return lines;
        std::uint64_t at = first;
        while (at <= last && lines.size() < kMaxLines)
        {
            const std::vector<Disasm::Instruction> chunk = m_engine.Disassembler().ReadMany(*m_engine.Process(), at, 512, names ? &m_engine.Symbols() : nullptr);
            if (chunk.empty())
                break;
            for (const Disasm::Instruction& instruction : chunk)
            {
                if (instruction.Address > last)
                    return lines;
                lines.push_back(instruction);
                at = instruction.Next();
            }
        }
        return lines;
    }

    std::optional<Disasm::FunctionSpan> MemoryViewer::FunctionOf(std::uint64_t address)
    {
        const std::uint64_t generation = m_engine.ProcessGeneration();
        const std::uint64_t writes = m_engine.Writes().Revision();
        if (!m_function.Valid || m_function.Address != address || m_function.Generation != generation || m_function.Writes != writes)
        {
            m_function.Span = m_engine.FunctionAround(address);
            m_function.Address = address;
            m_function.Generation = generation;
            m_function.Writes = writes;
            m_function.Valid = true;
        }
        return m_function.Span;
    }

    std::string MemoryViewer::NoFunctionReason(std::uint64_t address) const
    {
        if (const Target::CodeMap* map = m_engine.CodeMap())
        {
            if (const auto section = map->SectionAt(address); section && !section->Code)
                return "It lies outside the module's code sections, in its headers or data.";
        }
        return "There is no unwind data or managed method here, and nothing above it within 64 KB both looks like a start and leads here.";
    }

    std::uint64_t MemoryViewer::SymbolEnd(std::uint64_t start)
    {
        const std::uint64_t generation = m_engine.ProcessGeneration();
        const std::uint64_t symbols = m_engine.Symbols().Generation();
        const std::uint64_t writes = m_engine.Writes().Revision();
        const double now = m_engine.Now();
        SymbolEndCache& cache = m_symbolEnds;
        if (cache.Generation != generation || cache.Symbols != symbols || cache.Writes != writes || now - cache.Stamp > 1.0)
        {
            cache.Ends.clear();
            cache.Generation = generation;
            cache.Symbols = symbols;
            cache.Writes = writes;
            cache.Stamp = now;
        }
        const auto [entry, added] = cache.Ends.try_emplace(start, start);
        if (added)
            entry->second = m_engine.SymbolEnd(start);
        return entry->second;
    }

    void MemoryViewer::JumpToFunction(Command command)
    {
        const std::optional<Disasm::FunctionSpan> span = FunctionOf(m_selected);
        if (!span)
        {
            m_engine.Notify(LogLevel::Info, "No function start found for " + m_engine.Describe(m_selected) + ". " + NoFunctionReason(m_selected));
            return;
        }
        if (command == Command::GotoFunctionEnd)
            Navigate(span->Last);
        else
            Navigate(span->Start);
        if (command == Command::SelectFunction)
            m_selectionEnd = span->Last;
        if (span->Source == Disasm::FunctionSource::Padding || span->Source == Disasm::FunctionSource::Guess)
            m_engine.Notify(LogLevel::Info, "Function start " + m_engine.Describe(span->Start) + " " + Disasm::FunctionSourceText(span->Source) + ".");
    }

    void MemoryViewer::FunctionMenu(const Disasm::Instruction& instruction)
    {
        if (!ImGui::BeginMenu(ICON_FA_CODE_BRANCH "  Function"))
            return;
        const auto item = [&](const char* label, Command command, bool enabled = true)
            {
                if (CommandItem(label, command, false, enabled))
                    RunCommand(command);
            };
        const std::optional<Disasm::FunctionSpan> span = FunctionOf(instruction.Address);
        if (span)
        {
            ImGui::TextDisabled("%s, %s bytes", m_engine.Describe(span->Start).c_str(), Hex(span->End - span->Start).c_str());
            ItemTooltip((std::string("The start is ") + Disasm::FunctionSourceText(span->Source) + ".").c_str());
        }
        else
        {
            ImGui::TextDisabled("No function start found here");
            ItemTooltip(NoFunctionReason(instruction.Address).c_str());
        }
        ImGui::Separator();
        item(ICON_FA_ANGLE_DOUBLE_UP "  Go to the start", Command::GotoFunctionStart, span.has_value());
        item(ICON_FA_ANGLE_DOUBLE_DOWN "  Go to the last instruction", Command::GotoFunctionEnd, span.has_value());
        item(ICON_FA_I_CURSOR "  Select the whole function", Command::SelectFunction, span.has_value());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the whole function", nullptr, false, span.has_value()))
        {
            const Target::AddressStyle style = m_engine.Options().ShowSymbols ? Target::AddressStyle::Name : Target::AddressStyle::ModuleOffset;
            ImGui::SetClipboardText(Disasm::CopyText(InstructionsBetween(span->Start, span->Last, true), m_engine.Symbols(), style).c_str());
        }
        ImGui::Separator();
        item(ICON_FA_CODE_BRANCH "  Show the flow graph", Command::FlowGraph);
        ImGui::EndMenu();
    }

    void MemoryViewer::CopyInstructions(Target::AddressStyle style)
    {
        const std::vector<Disasm::Instruction> lines = SelectedInstructions(style != Target::AddressStyle::Plain);
        ImGui::SetClipboardText(Disasm::CopyText(lines, m_engine.Symbols(), style).c_str());
    }

    void MemoryViewer::AddressCopyItems(std::uint64_t address)
    {
        const auto copy = [](const char* label, const std::string& value)
            {
                const std::string preview = value.size() > 40 ? value.substr(0, 37) + "..." : value;
                if (ImGui::MenuItem(label, preview.c_str()))
                    ImGui::SetClipboardText(value.c_str());
            };
        const Target::Symbols& symbols = m_engine.Symbols();
        copy("Address", symbols.AddressText(address, Target::AddressStyle::Plain));
        copy("Name", symbols.AddressText(address, Target::AddressStyle::Name));
        copy("Module + offset", symbols.AddressText(address, Target::AddressStyle::ModuleOffset));
    }

    void MemoryViewer::HexMenu(std::uint64_t address)
    {
        const auto item = [&](const char* label, Command command, bool enabled = true)
            {
                if (CommandItem(label, command, false, enabled))
                    RunCommand(command);
            };
        ImGui::TextDisabled("%s", m_engine.Describe(address).c_str());
        ImGui::Separator();
        item(ICON_FA_LOCATION_ARROW "  Go to address", Command::GotoAddress);
        item(ICON_FA_ARROW_RIGHT "  Follow the pointer", Command::HexFollow);
        item(ICON_FA_ARROW_LEFT "  Back", Command::HexBack, !m_hexHistory.empty());
        if (m_hexDisplay != 0)
            item(ICON_FA_EDIT "  Edit the value", Command::HexEditValue);
        if (ImGui::MenuItem(ICON_FA_CODE "  Disassemble this address"))
            Navigate(address);
        item(ICON_FA_SITEMAP "  Dissect data at this address", Command::DissectData);
        ImGui::Separator();
        item(ICON_FA_EYE "  Find out what accesses this address", Command::HexAccesses);
        item(ICON_FA_PEN "  Find out what writes to this address", Command::HexWrites);
        item(ICON_FA_CIRCLE "  Break when written", Command::HexBreakOnWrite);
        item(ICON_FA_CIRCLE "  Break when read or written", Command::HexBreakOnAccess);
        ImGui::Separator();
        item(ICON_FA_PLUS "  Add this address to the address list", Command::HexAddToList);
        if (ImGui::BeginMenu(ICON_FA_COPY "  Copy"))
        {
            AddressCopyItems(address);
            ImGui::Separator();
            item("16 bytes", Command::HexCopy);
            if (ImGui::MenuItem("16 bytes as a C array"))
            {
                std::uint8_t bytes[16]{};
                if (m_engine.Process()->ReadExact(address, bytes, sizeof(bytes)))
                {
                    std::string array = "{ ";
                    for (std::size_t i = 0; i < sizeof(bytes); ++i)
                        array += (i ? ", 0x" : "0x") + Hex(bytes[i], 2);
                    ImGui::SetClipboardText((array + " }").c_str());
                }
            }
            ImGui::EndMenu();
        }
        item(ICON_FA_PASTE "  Paste bytes", Command::HexPaste);
        item(ICON_FA_UNDO_ALT "  Undo the last memory edit", Command::UndoMemoryEdit, UndoableEdits() > 0);
        if (ImGui::BeginMenu(ICON_FA_TH "  Show"))
        {
            const Command modes[] = { Command::HexBytes, Command::HexTwoBytes, Command::HexFourBytes, Command::HexEightBytes, Command::HexFourBytesDecimal, Command::HexFloat, Command::HexDouble };
            for (int i = 0; i < static_cast<int>(std::size(modes)); ++i)
            {
                if (CommandItem(kHexModes[i], modes[i], m_hexDisplay == i))
                    RunCommand(modes[i]);
            }
            ImGui::EndMenu();
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

    void MemoryViewer::CommentDialog()
    {
        if (m_commentOpen && !ImGui::IsPopupOpen("##comment"))
            ImGui::OpenPopup("##comment");
        if (!ImGui::BeginPopup("##comment"))
        {
            m_commentOpen = false;
            return;
        }
        ImGui::TextUnformatted(m_commentHeader ? "Header above" : "Comment at");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_engine.Describe(m_commentAddress).c_str());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Px(360.0f));
        const bool enter = InputText("##commenttext", m_commentText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const DialogChoice choice = DialogButtons("Save", true, false);
        if (enter || choice == DialogChoice::Ok)
        {
            const std::string key = m_engine.Symbols().ModuleOffset(m_commentAddress);
            Table::AddressList::DisasmNote& note = m_engine.Table().DisasmNotes[key];
            (m_commentHeader ? note.Header : note.Comment) = Trim(m_commentText);
            if (note.Comment.empty() && note.Header.empty())
                m_engine.Table().DisasmNotes.erase(key);
            m_engine.Table().Touch();
            m_commentOpen = false;
            ImGui::CloseCurrentPopup();
        }
        else if (choice == DialogChoice::Cancel)
        {
            m_commentOpen = false;
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
                const Status status = WriteBytes(m_assembleAddress, bytes, "Assemble");
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
        ImGui::TextUnformatted("Find bytes");
        Muted("Find next searches forward from the hex view, Find all lists every match.");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Px(360.0f));
        const bool enter = InputTextWithHint("##pattern", "48 8B ?? 10 or 'text'", m_findText, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_findError.empty())
            TextWithError(m_findError, ManifoldTheme::Error);
        const bool next = enter || AccentButton("Find next", Px(100.0f, 0.0f));
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_LIST "  Find all", Px(100.0f, 0.0f)))
        {
            if (BytePattern(m_findText))
            {
                m_workspace.OpenByteSearch(Trim(m_findText));
                ImGui::CloseCurrentPopup();
            }
            else
            {
                m_findError = ErrorMessage(ErrorCode::ScanBadPattern, Trim(m_findText));
            }
        }
        if (next)
        {
            const std::string text = Trim(m_findText);
            const std::optional<Scan::Pattern> pattern = BytePattern(text);
            if (!pattern)
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
            const Result<Address> allocated = m_engine.Process()->Allocate(static_cast<std::size_t>(ParseHex(m_allocateSize).value_or(0x1000)), 0x40);
            const Status status = allocated.ToStatus();
            if (allocated)
            {
                const Address memory = *allocated;
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
