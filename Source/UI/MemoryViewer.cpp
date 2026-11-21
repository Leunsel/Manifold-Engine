#include "UI/MemoryViewer.h"

#include "UI/CodeTools.h"
#include "Asm/Assembler.h"
#include "Asm/MnemonicDocs.h"
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
            m_engine.Options().MemoryHexView = true;
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
        m_engine.Options().MemoryHexView = true;
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
        if (const std::uint64_t writes = m_engine.Writes().Revision(); writes != m_noppedWrites)
        {
            m_noppedWrites = writes;
            if (const std::shared_ptr<Target::Process> process = m_engine.Process())
            {
                std::erase_if(m_nopped, [&](const auto& entry)
                    {
                        std::vector<std::uint8_t> now(entry.second.size());
                        return !process->ReadExact(entry.first, now.data(), now.size()) || std::any_of(now.begin(), now.end(), [](std::uint8_t byte) { return byte != 0x90; });
                    });
            }
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
        Settings& options = m_engine.Options();
        const bool hexShown = options.MemoryHexView;
        const float available = ImGui::GetContentRegionAvail().y;
        const float splitBar = Px(6.0f);
        const float share = std::clamp(static_cast<float>(options.MemoryCodeShare) / 1000.0f, 0.1f, 0.95f);
        float code = std::floor(available * share);
        float hex = available - code - splitBar;
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
        const auto rememberShare = [&]()
            {
                options.MemoryCodeShare = static_cast<int>(std::lround(code / available * 1000.0f));
                m_engine.SaveOptionsSoon();
            };
        const float leftCode = hexShown ? code : available;
        ImGui::BeginChild("##codearea", ImVec2(leftWidth, leftCode), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Disassembly(leftCode);
        ImGui::EndChild();
        const float rightX = ImGui::GetWindowContentRegionMax().x - registersWidth;
        if (showRegisters)
        {
            columnSplitter("##registersplit", codeScreen, hexShown ? code : available);
            ImGui::SetCursorPos(ImVec2(rightX, codeStart.y));
            Registers(registersWidth, code, shown, running);
        }
        if (hexShown)
        {
            ImGui::SetCursorPos(start);
            if (Splitter("##viewsplit", false, splitBar, &code, &hex, Px(120.0f), Px(90.0f)))
                rememberShare();
            ImGui::SetCursorPos(ImVec2(start.x, start.y + code + splitBar));
            const ImVec2 hexStart = ImGui::GetCursorPos();
            const ImVec2 hexScreen = ImGui::GetCursorScreenPos();
            const float hexHeight = ImGui::GetContentRegionAvail().y;
            ImGui::BeginChild("##hexarea", ImVec2(leftWidth, hexHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            HexView(ImGui::GetContentRegionAvail().y);
            ImGui::EndChild();
            if (showRegisters)
            {
                columnSplitter("##stacksplit", hexScreen, hexHeight);
                ImGui::SetCursorPos(ImVec2(rightX, hexStart.y));
                Stack(registersWidth, hexHeight, shown, running);
            }
        }
        else
        {
            m_hexFocus = false;
            if (showRegisters)
            {
                ImGui::SetCursorPos(ImVec2(rightX, start.y));
                if (Splitter("##stacksplit", false, splitBar, &code, &hex, Px(120.0f), Px(90.0f), registersWidth))
                    rememberShare();
                ImGui::SetCursorPos(ImVec2(rightX, start.y + code + splitBar));
                Stack(registersWidth, ImGui::GetContentRegionAvail().y, shown, running);
            }
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
        ExceptionDialog();
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
            item(ICON_FA_CLONE "  New Window", Command::NewViewerWindow);
            ImGui::Separator();
            item(ICON_FA_FILE_EXPORT "  Save Memory Region...", Command::SaveMemoryRegion, process);
            item(ICON_FA_FILE_IMPORT "  Load a File into Memory...", Command::LoadMemoryRegion, process);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_DOWNLOAD "  Load Symbols", nullptr, false, process))
                m_engine.LoadSymbols(false);
            if (ImGui::MenuItem(ICON_FA_GLOBE "  Load Windows Symbols", nullptr, false, process))
                m_engine.LoadSymbols(true);
            if (ImGui::MenuItem(m_engine.MonoPresent() ? (m_engine.Mono().IsIl2Cpp() ? ICON_FA_DICE_D6 "  IL2CPP Runtime Loaded" : ICON_FA_DICE_D6 "  Mono Runtime Loaded")
                                : m_engine.Managed().Loading() ? ICON_FA_DICE_D6 "  Loading the Runtime..." : ICON_FA_DICE_D6 "  Load Mono / IL2CPP Runtime",
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
            item(ICON_FA_LOCATION_ARROW "  Go to Address...", Command::GotoAddress);
            item(ICON_FA_SEARCH "  Find Bytes...", Command::FindMemory);
            if (ImGui::MenuItem(ICON_FA_LIST "  Find All Matching Bytes..."))
                m_workspace.OpenByteSearch();
            item(ICON_FA_SEARCH_LOCATION "  Find Assembly Code...", Command::FindAssembly);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View", process))
        {
            item(ICON_FA_TH_LIST "  Memory Regions", Command::MemoryRegions);
            item(ICON_FA_TAGS "  Modules and Symbols", Command::SymbolsAndModules);
            item(ICON_FA_PROJECT_DIAGRAM "  Referenced Functions", Command::ReferencedFunctions);
            item(ICON_FA_QUOTE_LEFT "  Referenced Strings", Command::ReferencedStrings);
            item(ICON_FA_CODE_BRANCH "  Flow Graph of the Selected Function", Command::FlowGraph);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TH "  Hex View", nullptr, &m_engine.Options().MemoryHexView))
            {
                if (!m_engine.Options().MemoryHexView)
                    m_hexFocus = false;
                m_engine.SaveOptionsSoon();
            }
            ItemTooltip("Off gives the disassembler the whole height. The stack stays beside the registers while the process is stopped.");
            if (ImGui::BeginMenu(ICON_FA_TH "  Hex View Shows", m_engine.Options().MemoryHexView))
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
            ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Follow Breaks", nullptr, &m_followBreaks);
            ItemTooltip("Jump to the instruction when the process stops at a breakpoint.");
            ImGui::MenuItem(ICON_FA_TAGS "  Show Symbols", nullptr, &m_engine.Options().ShowSymbols);
            ItemTooltip("Names in the code: function headers with offsets below them, symbol names in operands, module+offset addresses. Off shows plain addresses.");
            HighlightItems(nullptr);
            ImGui::Separator();
            if (ImGui::BeginMenu(ICON_FA_MICROCHIP "  Registers"))
            {
                RegisterViewItems();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_LAYER_GROUP "  Stack"))
            {
                StackViewItems();
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug", process))
        {
            item(ICON_FA_PLAY "  Run", Command::DebugRun, broken);
            item(ICON_FA_LEVEL_DOWN_ALT "  Step Into", Command::DebugStepInto, broken);
            item(ICON_FA_SHARE "  Step Over", Command::DebugStepOver, broken);
            item(ICON_FA_SIGN_OUT_ALT "  Step Out", Command::DebugRunToReturn, broken);
            ItemTooltip("Runs until the current function returns and stops at its caller, right after the call.");
            item(ICON_FA_PAUSE "  Break", Command::DebugBreak, !broken);
            const bool onException = broken && debugger.Break().Exception.Raised;
            if (ImGui::MenuItem(ICON_FA_FORWARD "  Continue, Exception Handled", nullptr, false, onException))
                debugger.ResumeHandled();
            ItemTooltip("For a stop on an exception: go on at the current registers without giving the exception to the program. Run gives it to the program.");
            if (ImGui::BeginMenu(ICON_FA_EXCLAMATION_TRIANGLE "  Break on Unexpected Exceptions"))
            {
                ExceptionBreakItems();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            item(ICON_FA_CIRCLE "  Toggle Breakpoint", Command::ToggleBreakpoint);
            item(ICON_FA_SLIDERS_H "  Breakpoint with a Condition...", Command::EditBreakpoint);
            item(ICON_FA_MAP_MARKER_ALT "  Set the Instruction Pointer Here", Command::SetInstructionPointer, broken);
            ImGui::Separator();
            item(ICON_FA_LIST "  Breakpoint List", Command::BreakpointList);
            item(ICON_FA_SHOE_PRINTS "  Break and Trace from Here...", Command::BreakAndTrace);
            item(ICON_FA_STREAM "  Thread List", Command::ThreadList, process);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_BUG "  Attach Debugger", nullptr, false, !debugger.Attached()))
                m_workspace.Report(m_engine.EnsureDebugger());
            if (ImGui::MenuItem(ICON_FA_UNLINK "  Detach Debugger", nullptr, false, debugger.Attached()))
                debugger.Detach();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools", process))
        {
            const std::size_t undoable = UndoableEdits();
            const std::string undo = ICON_FA_UNDO_ALT "  Undo the Last Memory Edit" + (undoable == 0 ? std::string() : " (" + std::to_string(undoable) + ")");
            item(undo.c_str(), Command::UndoMemoryEdit, undoable > 0);
            if (ImGui::MenuItem(ICON_FA_HISTORY "  Write History..."))
                m_workspace.OpenWriteHistory();
            ImGui::Separator();
            item(ICON_FA_PLUS_SQUARE "  Allocate Memory...", Command::AllocateMemory);
            item(ICON_FA_FILL_DRIP "  Fill Memory...", Command::FillMemory);
            item(ICON_FA_RANDOM "  Create a Thread...", Command::CreateThread);
            item(ICON_FA_SYRINGE "  Inject a Library...", Command::InjectLibrary);
            ImGui::Separator();
            item(ICON_FA_PROJECT_DIAGRAM "  Dissect Code", Command::DissectCode);
            item(ICON_FA_SITEMAP "  Dissect Data", Command::DissectData);
            item(ICON_FA_BAND_AID "  Scan for Patches", Command::ScanForPatches);
            if (CommandItem(ICON_FA_ROUTE "  Pointer Scan", Command::OpenPointerScan))
                m_workspace.OpenPointerScan();
            ImGui::Separator();
            item(ICON_FA_CODE "  Auto Assembler", Command::ViewerAutoAssemble);
            item(ICON_FA_MOON "  Lua Engine", Command::ViewerLuaEngine);
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

    void MemoryViewer::SetHighlight(const std::string& token)
    {
        const std::string wanted = HighlightToken(token);
        m_highlight = m_highlight == wanted ? std::string() : wanted;
    }

    void MemoryViewer::HighlightItems(const Disasm::Instruction* instruction)
    {
        if (!ImGui::BeginMenu(ICON_FA_HIGHLIGHTER "  Highlight"))
            return;
        if (instruction && instruction->Valid)
        {
            const std::vector<std::string> used = AsmRegisters(instruction->Operands);
            for (const std::string& name : used)
            {
                if (ImGui::MenuItem(name.c_str(), nullptr, m_highlight == name))
                    SetHighlight(name);
            }
            if (used.empty())
                ImGui::TextDisabled("No registers in this instruction");
            ImGui::Separator();
        }
        const bool is64 = !m_engine.Process() || m_engine.Process()->Is64Bit();
        if (ImGui::BeginMenu("Register"))
        {
            const char* const names64[] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
            for (int i = 0; i < (is64 ? 16 : 8); ++i)
            {
                const std::string shown = is64 ? names64[i] : "e" + std::string(names64[i] + 1);
                if (ImGui::MenuItem(shown.c_str(), nullptr, m_highlight == names64[i]))
                    SetHighlight(names64[i]);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Clear Highlight", nullptr, false, !m_highlight.empty()))
            m_highlight.clear();
        ImGui::Separator();
        ImGui::TextDisabled("Middle-click a register or value in the code");
        ImGui::EndMenu();
    }

    void MemoryViewer::ExceptionBreakItems()
    {
        Settings& options = m_engine.Options();
        const auto choose = [&](int mode)
            {
                options.BreakOnException = mode;
                m_engine.SaveOptionsSoon();
                m_workspace.Report(m_engine.ApplyExceptionBreak());
            };
        if (ImGui::MenuItem("Never", nullptr, options.BreakOnException == 0))
            choose(0);
        ItemTooltip("Exceptions go straight to the program (the default).");
        if (ImGui::MenuItem("Always", nullptr, options.BreakOnException == 1))
            choose(1);
        ItemTooltip("Stop at every exception the program raises before its own handlers see it: access violations, C++ and .NET throws, divisions by zero... Mono and Unity games raise many on purpose.");
        std::string region = "In a region...";
        if (options.BreakOnException == 2 && !options.ExceptionRegionStart.empty())
            region = "In " + options.ExceptionRegionStart + (options.ExceptionRegionStop.empty() ? "" : " - " + options.ExceptionRegionStop) + "...";
        if (ImGui::MenuItem(region.c_str(), nullptr, options.BreakOnException == 2))
        {
            m_exceptionStart = options.ExceptionRegionStart;
            m_exceptionStop = options.ExceptionRegionStop;
            if (m_exceptionStart.empty())
            {
                if (const auto module = m_engine.Process()->ModuleAt(m_selected))
                    m_exceptionStart = module->Name;
            }
            m_exceptionError.clear();
            m_exceptionDialog = true;
        }
        ItemTooltip("Stop only at exceptions raised by code in one module or address range, for example the game's own module.");
        ImGui::Separator();
        ImGui::TextDisabled("Debug output and thread names are never stopped at");
    }

    void MemoryViewer::ExceptionDialog()
    {
        if (m_exceptionDialog && !ImGui::IsPopupOpen("##exceptionregion"))
            ImGui::OpenPopup("##exceptionregion");
        if (!BeginModal("##exceptionregion", ICON_FA_EXCLAMATION_TRIANGLE, "Break on Exceptions in a Region", ImVec2(440.0f, 0.0f), &m_exceptionDialog, m_viewport))
            return;
        Muted("A module name stops at exceptions raised anywhere in that module. Or give the first and the last address of the range.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Start (module or address)");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool enter = InputTextWithHint("##exceptionstart", "game.exe or 7FF6A0001000", m_exceptionStart, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextUnformatted("End (empty for the whole module)");
        ImGui::SetNextItemWidth(-1.0f);
        enter = InputTextWithHint("##exceptionstop", "7FF6A0200000", m_exceptionStop, ImGuiInputTextFlags_EnterReturnsTrue) || enter;
        if (!m_exceptionError.empty())
        {
            ImGui::Spacing();
            TextWithError(m_exceptionError, ManifoldTheme::Error);
        }
        const DialogChoice choice = DialogButtons("Break in this region", !Trim(m_exceptionStart).empty(), false);
        if (choice == DialogChoice::Ok || enter)
        {
            Settings& options = m_engine.Options();
            const Settings before = options;
            options.BreakOnException = 2;
            options.ExceptionRegionStart = Trim(m_exceptionStart);
            options.ExceptionRegionStop = Trim(m_exceptionStop);
            const Status applied = m_engine.ApplyExceptionBreak();
            if (applied)
            {
                m_engine.SaveOptionsSoon();
                m_exceptionDialog = false;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                options.BreakOnException = before.BreakOnException;
                options.ExceptionRegionStart = before.ExceptionRegionStart;
                options.ExceptionRegionStop = before.ExceptionRegionStop;
                m_engine.ApplyExceptionBreak();
                m_exceptionError = applied.Text;
            }
        }
        else if (choice == DialogChoice::Cancel)
        {
            m_exceptionDialog = false;
            ImGui::CloseCurrentPopup();
        }
        EndModal();
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
        const bool any = std::any_of(m_bookmarks.begin(), m_bookmarks.end(), [](std::uint64_t bookmark) { return bookmark != 0; });
        if (ImGui::BeginMenu(ICON_FA_TRASH "  Remove", any))
        {
            for (int slot = 0; slot < 10; ++slot)
            {
                const std::uint64_t bookmark = m_bookmarks[static_cast<std::size_t>(slot)];
                if (bookmark && ImGui::MenuItem((std::to_string(slot) + "  " + m_engine.Describe(bookmark) + "##remove").c_str()))
                    RemoveBookmark(slot);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove All Bookmarks"))
                RemoveBookmark(-1);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        const std::string set = "Set a bookmark here with " + ChordText(CurrentBinding(Command::SetBookmark).Chord, true) + "; the same keys on its line remove it";
        SectionLabel(set.c_str());
    }

    void MemoryViewer::RemoveBookmark(int slot)
    {
        if (slot < 0)
        {
            m_bookmarks.fill(0);
            m_engine.Notify(LogLevel::Info, "All bookmarks removed");
            return;
        }
        m_bookmarks[static_cast<std::size_t>(slot)] = 0;
        m_engine.Notify(LogLevel::Info, "Bookmark " + std::to_string(slot) + " removed");
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
        button(ICON_FA_LOCATION_ARROW, "Go to Address", Command::GotoAddress);
        ImGui::SameLine();
        button(ICON_FA_SEARCH, "Find Bytes", Command::FindMemory);
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::BeginDisabled(!broken);
        button(ICON_FA_PLAY, "Run", Command::DebugRun);
        ImGui::SameLine();
        button(ICON_FA_LEVEL_DOWN_ALT, "Step Into", Command::DebugStepInto);
        ImGui::SameLine();
        button(ICON_FA_SHARE, "Step Over", Command::DebugStepOver);
        ImGui::SameLine();
        button(ICON_FA_SIGN_OUT_ALT, "Step Out", Command::DebugRunToReturn);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(broken);
        button(ICON_FA_PAUSE, "Break into the Process", Command::DebugBreak);
        ImGui::EndDisabled();
        ImGui::SameLine();
        button(ICON_FA_CIRCLE, "Toggle Breakpoint", Command::ToggleBreakpoint);
        ImGui::SameLine(0.0f, Px(14.0f));
        button(ICON_FA_CODE, "Auto Assembler", Command::ViewerAutoAssemble);
        ImGui::SameLine();
        button(ICON_FA_SITEMAP, "Dissect Data", Command::DissectData);

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
        const float gutter = Px(26.0f);
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
                    blockStart = m_engine.FunctionStartAt(line.Address);
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
        if (!m_highlight.empty())
        {
            const std::string chip = ICON_FA_HIGHLIGHTER "  " + m_highlight + "  " ICON_FA_TIMES;
            const ImVec2 size = ImGui::CalcTextSize(chip.c_str());
            const ImVec2 chipMin(rightX - size.x - Px(16.0f), frame.y + Px(3.0f));
            const ImVec2 chipMax(rightX - Px(4.0f), origin.y - Px(3.0f));
            ImGui::SetCursorScreenPos(chipMin);
            if (ImGui::InvisibleButton("##highlightchip", ImVec2(chipMax.x - chipMin.x, chipMax.y - chipMin.y)))
                m_highlight.clear();
            const bool chipHovered = ImGui::IsItemHovered();
            drawList->AddRectFilled(chipMin, chipMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, chipHovered ? 0.32f : 0.20f)), Px(4.0f));
            drawList->AddText(ImVec2(chipMin.x + Px(6.0f), std::round((chipMin.y + chipMax.y - size.y) * 0.5f)), ImGui::GetColorU32(ManifoldTheme::Text), chip.c_str());
            if (chipHovered)
                ImGui::SetTooltip("Highlighting %s and the parts of it in the operands. Rows that write it are marked red in the gutter, rows that only read it in the accent colour. Click to stop.",
                    m_highlight.c_str());
        }

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
            const float mouseAt = ImGui::GetIO().MousePos.x;
            if (line.Valid && mouseAt >= textX && mouseAt < commentX && !ImGui::IsAnyMouseDown())
            {
                if (m_explainAddress != line.Address)
                {
                    m_explainAddress = line.Address;
                    m_explainSince = ImGui::GetTime();
                }
                else if (ImGui::GetTime() - m_explainSince > 0.6)
                {
                    ExplainInstruction(line, info.Broken && line.Address == info.Address ? &info.Registers : nullptr);
                }
            }
            else
            {
                m_explainAddress = 0;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            {
                PushMono();
                const float operandStart = textX + charWidth * static_cast<float>(std::max<std::size_t>(line.Mnemonic.size() + 1, 7));
                const std::string token = AsmTokenAt(ImGui::GetFont(), ImGui::GetFontSize(), operandStart, line.Operands, ImGui::GetIO().MousePos.x);
                PopMono();
                if (!token.empty())
                    SetHighlight(token);
                else if (ImGui::GetIO().MousePos.x >= textX)
                    m_highlight.clear();
            }
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
                drawList->AddCircleFilled(ImVec2(origin.x + Px(19.0f), y + rowHeight * 0.5f), std::min(rowHeight * 0.28f, Px(5.0f)), ImGui::GetColorU32(ManifoldTheme::Breakpoint));
            if (isIp)
                drawList->AddText(ImVec2(origin.x + Px(6.0f), textY), ImGui::GetColorU32(ManifoldTheme::Warning), ICON_FA_CARET_RIGHT);
            if (m_nopped.count(line.Address))
                drawList->AddText(ImVec2(origin.x + gutter + Px(2.0f), textY), ImGui::GetColorU32(ManifoldTheme::Warning), ICON_FA_BAN);

            cell(addressX, bytesX, textY, ImGui::GetColorU32(addressInModule[i] ? ManifoldTheme::AddressText : ManifoldTheme::Text), addressTexts[i]);
            cell(bytesX, textX, textY, ImGui::GetColorU32(ManifoldTheme::BytesText), line.Mnemonic == "??" ? std::string("??") : line.BytesText());
            cell(textX, commentX, textY, FlowColor(line), line.Mnemonic);
            const float operandX = textX + charWidth * static_cast<float>(std::max<std::size_t>(line.Mnemonic.size() + 1, 7));
            {
                const ImVec4 operandClip(operandX, textY, std::min(commentX - Px(6.0f), rightX - Px(4.0f)), textY + rowHeight);
                if (operandClip.z > operandClip.x)
                    AsmOperands(drawList, font, fontSize, ImVec2(operandX, textY), operandClip, line.Operands, m_highlight);
            }
            if (const int use = HighlightUse(line.Mnemonic, line.Operands, m_highlight))
            {
                const ImU32 mark = ImGui::GetColorU32(use == 2 ? ManifoldTheme::Changed : ManifoldTheme::Accent);
                drawList->AddRectFilled(ImVec2(origin.x + Px(1.0f), y + Px(2.0f)), ImVec2(origin.x + Px(4.0f), y + rowHeight - Px(2.0f)), mark, Px(1.0f));
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

    void MemoryViewer::ExplainInstruction(const Disasm::Instruction& line, const Disasm::RegisterValues* registers)
    {
        const Disasm::Disassembler& disassembler = m_engine.Disassembler();
        const std::optional<Disasm::InstructionFacts> facts = disassembler.Facts(line);
        std::string summary = Asm::MnemonicSummary(line.Mnemonic);
        std::erase(summary, '`');
        if (!facts && summary.empty())
            return;
        std::optional<std::uint64_t> effective;
        if (registers && line.Memory.Present)
            effective = disassembler.EffectiveAddress(line, *registers);
        ShowTooltip([&]()
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                PushMono();
                ImGui::TextColored(ManifoldTheme::CodeRegister, "%s", line.Mnemonic.c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(line.Operands.c_str());
                PopMono();
                if (!summary.empty())
                    ImGui::TextUnformatted(summary.c_str());
                if (facts)
                {
                    const std::vector<std::string> lines = Disasm::ExplainFacts(*facts);
                    if (!lines.empty())
                    {
                        ImGui::Spacing();
                        for (const std::string& text : lines)
                            ImGui::TextColored(ManifoldTheme::Label, "%s", text.c_str());
                    }
                }
                if (effective)
                    ImGui::TextColored(ManifoldTheme::Accent, "At this break the memory operand is %s", m_engine.Describe(*effective).c_str());
                if (facts && (!facts->Category.empty() || !facts->IsaSet.empty()))
                {
                    ImGui::Spacing();
                    Muted(Lower(facts->Category) + (facts->IsaSet.empty() ? std::string() : "  \xC2\xB7  " + facts->IsaSet));
                }
                ImGui::PopTextWrapPos();
            });
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
        if (set && bookmark && bookmark == m_selected)
        {
            RemoveBookmark(slot);
        }
        else if (set)
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
        if (const auto nopped = m_nopped.find(instruction.Address); nopped != m_nopped.end())
        {
            const Status restored = WriteBytes(instruction.Address, nopped->second, "Restore");
            if (restored)
                m_nopped.erase(nopped);
            m_workspace.Report(restored);
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
        item(ICON_FA_LOCATION_ARROW "  Go to Address", Command::GotoAddress);
        if (instruction.Target)
            item(ICON_FA_ARROW_RIGHT "  Follow", Command::FollowInstruction);
        item(ICON_FA_ARROW_LEFT "  Back", Command::NavigateBack, !m_history.empty());
        if (ImGui::BeginMenu(ICON_FA_BOOKMARK "  Bookmarks"))
        {
            for (int slot = 0; slot < 10; ++slot)
            {
                const bool here = m_bookmarks[static_cast<std::size_t>(slot)] != 0 && m_bookmarks[static_cast<std::size_t>(slot)] == m_selected;
                const std::string label = (here ? "Remove bookmark " : "Set bookmark ") + std::to_string(slot);
                const std::string keys = ChordText((CurrentBinding(Command::SetBookmark).Chord & ImGuiMod_Mask_) | (ImGuiKey_0 + slot));
                if (ImGui::MenuItem(label.c_str(), keys.c_str()))
                    Bookmark(true, slot);
            }
            ImGui::Separator();
            BookmarkItems();
            ImGui::EndMenu();
        }
        HighlightItems(&instruction);
        ImGui::Separator();
        item(ICON_FA_EDIT "  Assemble", Command::AssembleInstruction);
        item(m_nopped.count(instruction.Address) ? ICON_FA_UNDO "  Restore Original Code" : ICON_FA_BAN "  Replace with Code That Does Nothing", Command::NopInstruction);
        item(ICON_FA_UNDO_ALT "  Undo the Last Memory Edit", Command::UndoMemoryEdit, UndoableEdits() > 0);
        if (ImGui::BeginMenu(ICON_FA_MAGIC "  Create Script"))
        {
            m_templates.MenuItems(instruction.Address);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item(ICON_FA_COMMENT "  Comment...", Command::CommentInstruction);
        ImGui::Separator();
        item(ICON_FA_CIRCLE "  Toggle Breakpoint", Command::ToggleBreakpoint);
        item(ICON_FA_SLIDERS_H "  Breakpoint with a Condition or Register Change...", Command::EditBreakpoint);
        item(ICON_FA_SHOE_PRINTS "  Break and Trace from Here...", Command::BreakAndTrace);
        item(ICON_FA_LIST_UL "  Find Out What Addresses This Instruction Accesses", Command::InstructionAccesses, instruction.Memory.Present);
        if (ImGui::MenuItem(ICON_FA_PHONE "  Find Out What Calls This Address"))
            m_workspace.StartWatch(Debug::WatchKind::Calls, instruction.Address, 1);
        {
            const auto dissect = m_engine.CodeDissect();
            const std::size_t references = dissect ? dissect->To(instruction.Address).size() : 0;
            const std::string label = dissect ? ICON_FA_PROJECT_DIAGRAM "  Code That References This Address (" + std::to_string(references) + ")" : ICON_FA_PROJECT_DIAGRAM "  Code That References This Address...";
            item(label.c_str(), Command::CodeReferences);
            ItemTooltip(dissect ? "From the last code dissect" : "Needs a code dissect first; the window offers one");
            if (instruction.Memory.Absolute && dissect)
            {
                const std::uint64_t operand = *instruction.Memory.Absolute;
                const std::string other = ICON_FA_PROJECT_DIAGRAM "  Code That Uses the Operand Address (" + std::to_string(dissect->To(operand).size()) + ")";
                if (ImGui::MenuItem(other.c_str()))
                    m_workspace.OpenCodeDissect(CodeDissectWindow::References, operand);
            }
        }
        FunctionMenu(instruction);
        if (broken)
            item(ICON_FA_MAP_MARKER_ALT "  Set the Instruction Pointer Here", Command::SetInstructionPointer);
        ImGui::Separator();
        if (instruction.Memory.Absolute && ImGui::MenuItem(ICON_FA_TH "  Show the Operand in the Hex View", instruction.Target ? nullptr : ShortcutText(Command::FollowInstruction)))
        {
            m_hexTop = *instruction.Memory.Absolute & ~0xFull;
            m_hexSelected = *instruction.Memory.Absolute;
        }
        item(ICON_FA_PLUS "  Add This Address to the Address List", Command::AddInstructionToList);
        if (ImGui::BeginMenu(ICON_FA_COPY "  Copy"))
        {
            item(m_engine.Options().ShowSymbols ? "Selected instructions with names" : "Selected instructions with module offsets", Command::CopyInstructions);
            if (ImGui::MenuItem(m_engine.Options().ShowSymbols ? "Selected instructions with module offsets" : "Selected instructions with names"))
                CopyInstructions(m_engine.Options().ShowSymbols ? Target::AddressStyle::ModuleOffset : Target::AddressStyle::Name);
            if (ImGui::MenuItem("Selected Instructions with Plain Addresses"))
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
            const std::span<const std::uint8_t> typed(&byte, 1);
            const Status status = m_hexNibble == 0 ? WriteBytes(m_hexSelected, typed, "Hex typing")
                : m_engine.Writes().WriteOrAmend(*m_engine.Process(), m_engine.ProcessGeneration(), m_nibbleWrite, m_hexSelected, typed, "Hex typing");
            if (!status)
            {
                m_workspace.Report(status);
                break;
            }
            if (m_hexNibble == 0)
                m_nibbleWrite = m_engine.Writes().LastId();
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
        item(ICON_FA_ANGLE_DOUBLE_UP "  Go to the Start", Command::GotoFunctionStart, span.has_value());
        item(ICON_FA_ANGLE_DOUBLE_DOWN "  Go to the Last Instruction", Command::GotoFunctionEnd, span.has_value());
        item(ICON_FA_I_CURSOR "  Select the Whole Function", Command::SelectFunction, span.has_value());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Whole Function", nullptr, false, span.has_value()))
        {
            const Target::AddressStyle style = m_engine.Options().ShowSymbols ? Target::AddressStyle::Name : Target::AddressStyle::ModuleOffset;
            ImGui::SetClipboardText(Disasm::CopyText(InstructionsBetween(span->Start, span->Last, true), m_engine.Symbols(), style).c_str());
        }
        ImGui::Separator();
        item(ICON_FA_CODE_BRANCH "  Show the Flow Graph", Command::FlowGraph);
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
        item(ICON_FA_LOCATION_ARROW "  Go to Address", Command::GotoAddress);
        item(ICON_FA_ARROW_RIGHT "  Follow the Pointer", Command::HexFollow);
        item(ICON_FA_ARROW_LEFT "  Back", Command::HexBack, !m_hexHistory.empty());
        if (m_hexDisplay != 0)
            item(ICON_FA_EDIT "  Edit the Value", Command::HexEditValue);
        if (ImGui::MenuItem(ICON_FA_CODE "  Disassemble This Address"))
            Navigate(address);
        item(ICON_FA_SITEMAP "  Dissect Data at This Address", Command::DissectData);
        ImGui::Separator();
        item(ICON_FA_EYE "  Find Out What Accesses This Address", Command::HexAccesses);
        item(ICON_FA_PEN "  Find Out What Writes to This Address", Command::HexWrites);
        item(ICON_FA_CIRCLE "  Break When Written", Command::HexBreakOnWrite);
        item(ICON_FA_CIRCLE "  Break When Read or Written", Command::HexBreakOnAccess);
        ImGui::Separator();
        item(ICON_FA_PLUS "  Add This Address to the Address List", Command::HexAddToList);
        if (ImGui::BeginMenu(ICON_FA_COPY "  Copy"))
        {
            AddressCopyItems(address);
            ImGui::Separator();
            item("16 bytes", Command::HexCopy);
            if (ImGui::MenuItem("16 Bytes as a C Array"))
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
        item(ICON_FA_PASTE "  Paste Bytes", Command::HexPaste);
        item(ICON_FA_UNDO_ALT "  Undo the Last Memory Edit", Command::UndoMemoryEdit, UndoableEdits() > 0);
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
        const bool next = enter || AccentButton("Find Next", Px(100.0f, 0.0f));
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_LIST "  Find All", Px(100.0f, 0.0f)))
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
