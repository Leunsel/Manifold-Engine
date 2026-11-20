#include "UI/FlowGraphWindow.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Target/CodeMap.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>

namespace Manifold::UI
{
    namespace
    {
        constexpr float kMinZoom = 0.12f;
        constexpr float kMaxZoom = 2.5f;
        constexpr float kTextZoom = 0.5f;
        constexpr std::size_t kMermaidLines = 14;

        ImU32 EdgeColor(Disasm::EdgeKind kind, float alpha)
        {
            switch (kind)
            {
            case Disasm::EdgeKind::Taken: return ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Success, alpha));
            case Disasm::EdgeKind::NotTaken: return ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Error, alpha));
            default: return ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::FlowCall, alpha));
            }
        }

        ImU32 MnemonicColor(const Disasm::Instruction& line)
        {
            switch (line.Kind)
            {
            case Disasm::Flow::Jump:
            case Disasm::Flow::ConditionalJump: return ImGui::GetColorU32(ManifoldTheme::FlowJump);
            case Disasm::Flow::Call: return ImGui::GetColorU32(ManifoldTheme::FlowCall);
            case Disasm::Flow::Return: return ImGui::GetColorU32(ManifoldTheme::FlowReturn);
            case Disasm::Flow::Interrupt: return ImGui::GetColorU32(ManifoldTheme::Muted);
            default: break;
            }
            return ImGui::GetColorU32(line.IsNop || !line.Valid ? ManifoldTheme::Muted : ManifoldTheme::CodeKeyword);
        }

        void DashedLine(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 color, float thickness, float dash)
        {
            const float length = std::hypot(b.x - a.x, b.y - a.y);
            if (length <= 0.0f)
                return;
            const ImVec2 step((b.x - a.x) / length, (b.y - a.y) / length);
            for (float at = 0.0f; at < length; at += dash * 2.0f)
            {
                const float to = std::min(length, at + dash);
                draw->AddLine(ImVec2(a.x + step.x * at, a.y + step.y * at), ImVec2(a.x + step.x * to, a.y + step.y * to), color, thickness);
            }
        }

        std::string MermaidText(std::string text)
        {
            text = ReplaceAll(text, "\"", "#quot;");
            text = ReplaceAll(text, "<", "#lt;");
            return ReplaceAll(text, ">", "#gt;");
        }
    }

    void FlowGraphWindow::ShowFunction(std::uint64_t address, bool remember)
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess())
            return;
        const std::uint64_t entry = FunctionStart(address);
        if (remember && m_built && entry != m_entry)
            m_history.push_back(m_entry);
        m_selected = address;
        m_followed = address;
        Build(entry);
        m_fit = true;
        m_centerBlock = m_graph.BlockAt(address);
        Show();
    }

    std::uint64_t FlowGraphWindow::FunctionStart(std::uint64_t address) const
    {
        if (const auto span = m_workspace.Core().FunctionAround(address))
            return span->Start;
        return address;
    }

    void FlowGraphWindow::Build(std::uint64_t entry)
    {
        Engine& engine = m_workspace.Core();
        m_entry = entry;
        m_generation = engine.ProcessGeneration();
        m_writes = engine.Writes().Revision();
        const Target::CodeMap* map = engine.CodeMap();
        const Target::Symbols& symbols = engine.Symbols();
        const auto other = [&](std::uint64_t target, Disasm::EdgeKind)
        {
            return engine.FunctionStartAt(target);
        };
        m_graph = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), entry, &symbols, map, other);
        m_loops = static_cast<int>(std::count_if(m_graph.Blocks.begin(), m_graph.Blocks.end(), [](const Disasm::FlowBlock& block) { return block.LoopHead; }));
        m_built = true;
        Measure();
    }

    void FlowGraphWindow::Measure()
    {
        ImFont* font = Fonts::Mono();
        PushMono();
        m_fontSize = std::round(ImGui::GetFontSize());
        PopMono();
        m_lineHeight = std::round(m_fontSize * 1.3f);
        m_padding = std::round(m_fontSize * 0.6f);
        const auto measure = [&](std::string_view text) { return font->CalcTextSizeA(m_fontSize, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x; };
        m_offsetWidth = measure("+00000 ");
        m_mnemonicWidth = measure("cmovne  ");
        m_sizes.clear();
        for (std::size_t b = 0; b < m_graph.Blocks.size(); ++b)
        {
            const Disasm::FlowBlock& block = m_graph.Blocks[b];
            float width = measure(BlockTitle(static_cast<int>(b))) + m_fontSize * 3.0f;
            for (const Disasm::Instruction& line : block.Lines)
                width = std::max(width, m_offsetWidth + std::max(m_mnemonicWidth, measure(line.Mnemonic + " ")) + measure(line.Operands));
            const std::size_t extra = block.Exits.size() + (block.Indirect || block.Cases ? 1u : 0u) + (block.Truncated ? 1u : 0u);
            for (const std::uint64_t exit : block.Exits)
                width = std::max(width, measure("-> " + m_workspace.Core().Describe(exit)));
            const float height = m_lineHeight * static_cast<float>(1 + block.Lines.size() + extra) + m_padding * 2.0f;
            m_sizes.push_back({ width + m_padding * 2.0f, height });
        }
        std::vector<Layout::Edge> edges;
        edges.reserve(m_graph.Edges.size());
        for (const Disasm::FlowEdge& edge : m_graph.Edges)
            edges.push_back({ edge.From, edge.To });
        Layout::Spacing spacing;
        spacing.Column = m_fontSize * 3.0f;
        spacing.Layer = m_fontSize * 4.0f;
        spacing.Lane = m_fontSize * 0.7f;
        m_layout = Layout::LayeredLayout(m_sizes, edges, 0, spacing);
    }

    std::string FlowGraphWindow::BlockTitle(int block) const
    {
        const Disasm::FlowBlock& data = m_graph.Blocks[static_cast<std::size_t>(block)];
        if (block == 0)
            return m_workspace.Core().Describe(data.Start);
        return "+" + Hex(data.Start - m_graph.Entry);
    }

    void FlowGraphWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_CODE_BRANCH "  Flow graph###flowgraph", ImVec2(900.0f, 640.0f), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar,
                ImVec2(420.0f, 300.0f)))
            return;
        Engine& engine = m_workspace.Core();
        MenuBar();
        if (!engine.HasProcess())
        {
            m_built = false;
            m_history.clear();
            m_workspace.NoProcessState(ICON_FA_CODE_BRANCH, "Open a process, then pick an instruction and open its flow graph.");
            EndToolWindow();
            return;
        }
        if (m_built && m_generation != engine.ProcessGeneration())
        {
            m_built = false;
            m_history.clear();
        }
        if (!m_built)
        {
            const std::uint64_t start = engine.SelectedCodeAddress;
            if (!start)
            {
                EmptyState(ICON_FA_CODE_BRANCH, "No function picked yet", "Right-click an instruction in the memory viewer and choose Show the flow graph.", {}, false);
                EndToolWindow();
                return;
            }
            ShowFunction(start, false);
        }
        if (m_writes != engine.Writes().Revision())
            Build(m_entry);
        if (m_follow && engine.SelectedCodeAddress && engine.SelectedCodeAddress != m_followed)
        {
            m_followed = engine.SelectedCodeAddress;
            m_selected = m_followed;
            if (m_graph.BlockAt(m_followed) < 0)
                ShowFunction(m_followed);
            else
                m_centerBlock = m_graph.BlockAt(m_followed);
        }
        const bool back = Pressed(Command::FlowGraphBack);
        if (Pressed(Command::FlowGraphFit))
            m_fit = true;
        if (Pressed(Command::FlowGraphActualSize))
            m_actualSize = true;
        if (back)
            GoBack();
        Toolbar();
        Canvas();
        EndToolWindow();
    }

    void FlowGraphWindow::Toolbar()
    {
        Engine& engine = m_workspace.Core();
        {
            const GhostButtons ghost;
            ImGui::BeginDisabled(m_history.empty());
            if (IconButton(ICON_FA_ARROW_LEFT "##flowback", WithKeys("Back to the function before", Command::FlowGraphBack).c_str()))
                GoBack();
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        ImGui::TextUnformatted(engine.Describe(m_entry).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        std::string stats = Format("%zu blocks  ·  %d instructions", m_graph.Blocks.size(), m_graph.Instructions);
        if (m_loops > 0)
            stats += Format("  ·  %d %s", m_loops, m_loops == 1 ? "loop" : "loops");
        Muted(stats);
        if (m_graph.Truncated)
        {
            ImGui::SameLine();
            ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_EXCLAMATION_TRIANGLE);
            ItemTooltip("The function is larger than the graph follows (6000 instructions or 800 blocks), so some paths end early.");
        }
    }

    void FlowGraphWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        Engine& engine = m_workspace.Core();
        const bool shown = engine.HasProcess() && m_built;
        if (ImGui::BeginMenu("Graph"))
        {
            if (CommandItem(ICON_FA_ARROW_LEFT "  Back to the function before", Command::FlowGraphBack, false, !m_history.empty()))
                GoBack();
            if (ImGui::MenuItem(ICON_FA_SYNC "  Read the function again", nullptr, false, shown))
                Build(m_entry);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy as Mermaid", nullptr, false, shown))
            {
                ImGui::SetClipboardText(Mermaid().c_str());
                engine.Notify(LogLevel::Info, "Copied the flow graph as Mermaid text");
            }
            ItemTooltip("A Mermaid flowchart of the graph, for notes, issues and wikis.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            if (CommandItem(ICON_FA_EXPAND "  Fit the graph into the window", Command::FlowGraphFit, false, shown))
                m_fit = true;
            if (CommandItem(ICON_FA_SEARCH "  Actual size", Command::FlowGraphActualSize, false, shown))
                m_actualSize = true;
            ImGui::Separator();
            ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Follow the disassembler", nullptr, &m_follow);
            ItemTooltip("Shows the function of the instruction selected in the memory viewer, and moves to it as the selection changes.");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void FlowGraphWindow::Canvas()
    {
        Engine& engine = m_workspace.Core();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x < 32.0f || size.y < 32.0f)
            return;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Page);
        ImGui::BeginChild("##flowcanvas", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleColor();
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 area = ImGui::GetContentRegionAvail();
        const ImVec2 max(min.x + area.x, min.y + area.y);
        if (m_actualSize)
        {
            const ImVec2 center(m_pan.x + area.x * 0.5f / m_zoom, m_pan.y + area.y * 0.5f / m_zoom);
            SetZoom(1.0f);
            m_pan = ImVec2(center.x - area.x * 0.5f / m_zoom, center.y - area.y * 0.5f / m_zoom);
            m_actualSize = false;
        }
        if (m_fit)
        {
            Fit(area);
            m_fit = false;
            if (m_centerBlock >= 0 && m_zoom < kTextZoom)
            {
                SetZoom(0.8f);
                CenterOn(m_centerBlock, area);
            }
            m_centerBlock = -1;
        }
        else if (m_centerBlock >= 0)
        {
            CenterOn(m_centerBlock, area);
            m_centerBlock = -1;
        }

        ImGui::InvisibleButton("##flowinput", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const ImGuiIO& io = ImGui::GetIO();
        const auto toWorld = [&](ImVec2 screen) { return ImVec2(m_pan.x + (screen.x - min.x) / m_zoom, m_pan.y + (screen.y - min.y) / m_zoom); };
        const auto toScreen = [&](float x, float y) { return ImVec2(std::floor(min.x + (x - m_pan.x) * m_zoom), std::floor(min.y + (y - m_pan.y) * m_zoom)); };

        const bool minimap = m_layout.Width * m_zoom > area.x || m_layout.Height * m_zoom > area.y;
        const bool minimapHovered = minimap && hovered && io.MousePos.x > max.x - Px(190.0f) && io.MousePos.y > max.y - Px(130.0f);
        if (hovered && io.MouseWheel != 0.0f && !minimapHovered)
        {
            const ImVec2 anchor = toWorld(io.MousePos);
            SetZoom(m_zoomWanted * std::pow(1.15f, io.MouseWheel));
            m_pan = ImVec2(anchor.x - (io.MousePos.x - min.x) / m_zoom, anchor.y - (io.MousePos.y - min.y) / m_zoom);
        }
        if (ImGui::IsItemActive() && !minimapHovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 3.0f)))
        {
            m_dragging = true;
            m_pan = ImVec2(m_pan.x - io.MouseDelta.x / m_zoom, m_pan.y - io.MouseDelta.y / m_zoom);
        }
        const Hit hit = hovered && !minimapHovered ? HitTest(toWorld(io.MousePos)) : Hit{};
        m_hoverBlock = hit.Block;
        if (hovered && !minimapHovered && !m_dragging)
        {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && hit.Line >= 0 && !m_skipRelease)
            {
                const Disasm::Instruction& line = m_graph.Blocks[static_cast<std::size_t>(hit.Block)].Lines[static_cast<std::size_t>(hit.Line)];
                m_selected = line.Address;
                m_followed = line.Address;
                Activate(line, false);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hit.Line >= 0)
            {
                const Disasm::Instruction line = m_graph.Blocks[static_cast<std::size_t>(hit.Block)].Lines[static_cast<std::size_t>(hit.Line)];
                const std::uint64_t entry = m_entry;
                Activate(line, true);
                m_skipRelease = m_entry != entry;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && hit.Line >= 0)
            {
                m_menuAddress = m_graph.Blocks[static_cast<std::size_t>(hit.Block)].Lines[static_cast<std::size_t>(hit.Line)].Address;
                ImGui::OpenPopup("##flowline");
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            m_dragging = false;
            m_skipRelease = false;
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, max, true);
        const float dot = std::max(1.0f, 1.2f * m_zoom);
        const float grid = m_fontSize * 2.5f;
        if (grid * m_zoom > 10.0f)
        {
            const ImU32 dotColor = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.06f));
            const float startX = std::floor(m_pan.x / grid) * grid;
            const float startY = std::floor(m_pan.y / grid) * grid;
            for (float y = startY; y < m_pan.y + area.y / m_zoom; y += grid)
            {
                for (float x = startX; x < m_pan.x + area.x / m_zoom; x += grid)
                    draw->AddCircleFilled(toScreen(x, y), dot, dotColor, 4);
            }
        }

        const float thickness = std::max(1.0f, std::round(1.6f * m_zoom));
        for (int pass = 0; pass < 2; ++pass)
        {
            for (std::size_t e = 0; e < m_graph.Edges.size(); ++e)
            {
                const Disasm::FlowEdge& edge = m_graph.Edges[e];
                const bool lit = m_hoverBlock >= 0 && (edge.From == m_hoverBlock || edge.To == m_hoverBlock);
                if ((pass == 1) != lit)
                    continue;
                const std::vector<Layout::Point>& route = m_layout.Routes[e];
                if (route.size() < 2)
                    continue;
                const float alpha = m_hoverBlock >= 0 && !lit ? 0.35f : 0.9f;
                const ImU32 color = EdgeColor(edge.Kind, alpha);
                const float width = lit ? thickness + 1.0f : thickness;
                for (std::size_t p = 1; p < route.size(); ++p)
                {
                    const ImVec2 a = toScreen(route[p - 1].X, route[p - 1].Y);
                    const ImVec2 b = toScreen(route[p].X, route[p].Y);
                    if (edge.Back)
                        DashedLine(draw, a, b, color, width, std::max(3.0f, std::round(5.0f * m_zoom)));
                    else
                        draw->AddLine(a, b, color, width);
                }
                const ImVec2 tip = toScreen(route.back().X, route.back().Y);
                const float arrow = std::max(4.0f, std::round(6.0f * m_zoom));
                draw->AddTriangleFilled(ImVec2(tip.x - arrow * 0.6f, tip.y - arrow), ImVec2(tip.x + arrow * 0.6f, tip.y - arrow), tip, color);
            }
        }

        ImFont* font = Fonts::Mono();
        const float textSize = std::round(m_fontSize * m_zoom);
        const bool text = m_zoom >= kTextZoom;
        const Debug::Debugger& debugger = engine.Debugger();
        const Debug::BreakInfo broken = debugger.Broken() ? debugger.Break() : Debug::BreakInfo{};
        if (broken.Broken && broken.Serial != m_breakSerial)
        {
            m_breakSerial = broken.Serial;
            if (m_follow && m_graph.BlockAt(broken.Address) >= 0)
                m_centerBlock = m_graph.BlockAt(broken.Address);
        }
        const std::vector<Debug::Breakpoint> breakpoints = debugger.Attached() ? debugger.Breakpoints() : std::vector<Debug::Breakpoint>{};
        const auto hasBreakpoint = [&](std::uint64_t address)
            {
                return std::any_of(breakpoints.begin(), breakpoints.end(), [&](const Debug::Breakpoint& breakpoint) { return breakpoint.Address == address; });
            };
        const float rounding = std::max(2.0f, 5.0f * m_zoom);
        for (std::size_t b = 0; b < m_graph.Blocks.size(); ++b)
        {
            const Disasm::FlowBlock& block = m_graph.Blocks[b];
            const Layout::Point at = m_layout.Positions[b];
            const ImVec2 topLeft = toScreen(at.X, at.Y);
            const ImVec2 bottomRight = toScreen(at.X + m_sizes[b].Width, at.Y + m_sizes[b].Height);
            if (bottomRight.x < min.x || bottomRight.y < min.y || topLeft.x > max.x || topLeft.y > max.y)
                continue;
            const bool entry = b == 0;
            const bool lit = static_cast<int>(b) == m_hoverBlock;
            draw->AddRectFilled(topLeft, bottomRight, ImGui::GetColorU32(ManifoldTheme::Card), rounding);
            const ImVec4 border = entry ? ManifoldTheme::Accent : block.LoopHead ? ManifoldTheme::Warning : lit ? ManifoldTheme::Label : ManifoldTheme::CardBorder;
            draw->AddRect(topLeft, bottomRight, ImGui::GetColorU32(border), rounding, 0, entry || lit ? std::max(1.0f, 1.8f * m_zoom) : 1.0f);
            const float header = toScreen(at.X, at.Y + m_lineHeight + m_padding).y - topLeft.y;
            draw->AddRectFilled(topLeft, ImVec2(bottomRight.x, topLeft.y + header), ImGui::GetColorU32(ManifoldTheme::WithAlpha(entry ? ManifoldTheme::Accent : ManifoldTheme::Text, entry ? 0.14f : 0.05f)), rounding,
                ImDrawFlags_RoundCornersTop);
            if (!text)
            {
                for (std::size_t l = 0; l < block.Lines.size(); l += 2)
                {
                    const float y = at.Y + m_padding + m_lineHeight * static_cast<float>(l + 1) + m_lineHeight * 0.3f;
                    const float length = std::min(m_sizes[b].Width - m_padding * 2.0f, m_offsetWidth + m_mnemonicWidth + static_cast<float>(block.Lines[l].Operands.size()) * m_fontSize * 0.55f);
                    draw->AddRectFilled(toScreen(at.X + m_padding, y), toScreen(at.X + m_padding + length, y + m_lineHeight * 0.35f), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.12f)));
                }
                continue;
            }
            const ImVec4 clip(topLeft.x, topLeft.y, bottomRight.x, bottomRight.y);
            const std::string title = BlockTitle(static_cast<int>(b));
            draw->AddText(font, textSize, toScreen(at.X + m_padding, at.Y + m_padding * 0.5f + (m_lineHeight - m_fontSize) * 0.5f), ImGui::GetColorU32(entry ? ManifoldTheme::Accent : ManifoldTheme::AddressText), title.c_str(), nullptr, 0.0f, &clip);
            if (block.LoopHead)
            {
                const char* badge = "loop";
                const float badgeWidth = font->CalcTextSizeA(m_fontSize, FLT_MAX, 0.0f, badge).x;
                draw->AddText(font, textSize, toScreen(at.X + m_sizes[b].Width - m_padding - badgeWidth, at.Y + m_padding * 0.5f + (m_lineHeight - m_fontSize) * 0.5f), ImGui::GetColorU32(ManifoldTheme::Warning), badge, nullptr, 0.0f, &clip);
            }
            float y = at.Y + m_padding + m_lineHeight;
            for (std::size_t l = 0; l < block.Lines.size(); ++l)
            {
                const Disasm::Instruction& line = block.Lines[l];
                const ImVec2 rowMin = toScreen(at.X + 1.0f, y);
                const ImVec2 rowMax = toScreen(at.X + m_sizes[b].Width - 1.0f, y + m_lineHeight);
                if (broken.Broken && broken.Address == line.Address)
                    draw->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.28f)));
                else if (line.Address == m_selected)
                    draw->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ManifoldTheme::CurrentLine));
                else if (hit.Block == static_cast<int>(b) && hit.Line == static_cast<int>(l))
                    draw->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.06f)));
                const float textY = y + (m_lineHeight - m_fontSize) * 0.5f;
                if (hasBreakpoint(line.Address))
                    draw->AddCircleFilled(toScreen(at.X + m_padding * 0.5f, y + m_lineHeight * 0.5f), std::max(2.0f, m_fontSize * 0.25f * m_zoom), ImGui::GetColorU32(ManifoldTheme::Breakpoint));
                const std::string offset = "+" + Hex(line.Address - m_graph.Entry);
                draw->AddText(font, textSize, toScreen(at.X + m_padding, textY), ImGui::GetColorU32(ManifoldTheme::Muted), offset.c_str(), nullptr, 0.0f, &clip);
                draw->AddText(font, textSize, toScreen(at.X + m_padding + m_offsetWidth, textY), MnemonicColor(line), line.Mnemonic.c_str(), nullptr, 0.0f, &clip);
                const float mnemonic = std::max(m_mnemonicWidth, font->CalcTextSizeA(m_fontSize, FLT_MAX, 0.0f, (line.Mnemonic + " ").c_str()).x);
                AsmOperands(draw, font, textSize, toScreen(at.X + m_padding + m_offsetWidth + mnemonic, textY), clip, line.Operands);
                y += m_lineHeight;
            }
            const auto note = [&](const std::string& textLine, const ImVec4& color)
                {
                    draw->AddText(font, textSize, toScreen(at.X + m_padding, y + (m_lineHeight - m_fontSize) * 0.5f), ImGui::GetColorU32(color), textLine.c_str(), nullptr, 0.0f, &clip);
                    y += m_lineHeight;
                };
            for (const std::uint64_t exit : block.Exits)
                note("-> " + engine.Describe(exit), ManifoldTheme::FlowJump);
            if (block.Indirect)
                note("-> indirect jump", ManifoldTheme::Muted);
            else if (block.Cases)
                note("-> jump table, " + std::to_string(block.Cases) + (block.Cases == 1 ? " target" : " targets"), ManifoldTheme::Muted);
            if (block.Truncated)
                note("... the code ends here", ManifoldTheme::Warning);
        }
        draw->PopClipRect();

        if (ImGui::BeginPopup("##flowline"))
        {
            std::optional<Disasm::Instruction> chosen;
            if (const int index = m_graph.BlockAt(m_menuAddress); index >= 0)
            {
                for (const Disasm::Instruction& line : m_graph.Blocks[static_cast<std::size_t>(index)].Lines)
                {
                    if (line.Address == m_menuAddress)
                        chosen = line;
                }
            }
            if (chosen)
                LineMenu(*chosen);
            ImGui::EndPopup();
        }
        if (minimap)
            Minimap(min, max);
        ImGui::EndChild();
    }

    void FlowGraphWindow::Minimap(ImVec2 canvasMin, ImVec2 canvasMax)
    {
        if (m_layout.Width <= 0.0f || m_layout.Height <= 0.0f)
            return;
        const float boxWidth = Px(180.0f);
        const float boxHeight = Px(120.0f);
        const ImVec2 boxMax(canvasMax.x - Px(8.0f), canvasMax.y - Px(8.0f));
        const ImVec2 boxMin(boxMax.x - boxWidth, boxMax.y - boxHeight);
        const ImVec2 area(canvasMax.x - canvasMin.x, canvasMax.y - canvasMin.y);
        const float viewWidth = area.x / m_zoom;
        const float viewHeight = area.y / m_zoom;
        const float scale = std::min((boxWidth - Px(8.0f)) / m_layout.Width, (boxHeight - Px(8.0f)) / m_layout.Height);
        const ImVec2 origin(boxMin.x + (boxWidth - m_layout.Width * scale) * 0.5f, boxMin.y + (boxHeight - m_layout.Height * scale) * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Card, 0.92f)), Px(4.0f));
        draw->AddRect(boxMin, boxMax, ImGui::GetColorU32(ManifoldTheme::CardBorder), Px(4.0f));
        for (std::size_t b = 0; b < m_graph.Blocks.size(); ++b)
        {
            const Layout::Point at = m_layout.Positions[b];
            draw->AddRectFilled(ImVec2(origin.x + at.X * scale, origin.y + at.Y * scale), ImVec2(origin.x + (at.X + m_sizes[b].Width) * scale, origin.y + (at.Y + m_sizes[b].Height) * scale),
                ImGui::GetColorU32(b == 0 ? ManifoldTheme::Accent : ManifoldTheme::WithAlpha(ManifoldTheme::Label, 0.55f)));
        }
        const ImVec2 viewMin(origin.x + m_pan.x * scale, origin.y + m_pan.y * scale);
        const ImVec2 viewMax(viewMin.x + viewWidth * scale, viewMin.y + viewHeight * scale);
        draw->PushClipRect(boxMin, boxMax, true);
        draw->AddRect(viewMin, viewMax, ImGui::GetColorU32(ManifoldTheme::Accent), 0.0f, 0, 1.5f);
        draw->PopClipRect();
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseHoveringRect(boxMin, boxMax) && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsWindowHovered())
        {
            const float worldX = (io.MousePos.x - origin.x) / scale;
            const float worldY = (io.MousePos.y - origin.y) / scale;
            m_pan = ImVec2(worldX - viewWidth * 0.5f, worldY - viewHeight * 0.5f);
        }
    }

    void FlowGraphWindow::LineMenu(const Disasm::Instruction& line)
    {
        Engine& engine = m_workspace.Core();
        ImGui::TextDisabled("%s", engine.Describe(line.Address).c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CODE "  Show in the disassembler"))
            m_workspace.RevealInMemoryViewer(line.Address, line.Length, true, true);
        if (line.Kind == Disasm::Flow::Call && line.Target && ImGui::MenuItem(ICON_FA_CODE_BRANCH "  Open the called function's graph"))
            ShowFunction(*line.Target);
        if ((line.Kind == Disasm::Flow::Jump || line.Kind == Disasm::Flow::ConditionalJump) && line.Target)
        {
            if (m_graph.BlockAt(*line.Target) >= 0)
            {
                if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  Go to the jump target"))
                {
                    m_selected = *line.Target;
                    m_centerBlock = m_graph.BlockAt(*line.Target);
                }
            }
            else if (ImGui::MenuItem(ICON_FA_CODE_BRANCH "  Open the jump target's graph"))
                ShowFunction(*line.Target);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the address", engine.Describe(line.Address).c_str()))
            ImGui::SetClipboardText(engine.Describe(line.Address).c_str());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the instruction"))
            ImGui::SetClipboardText(line.Text().c_str());
    }

    void FlowGraphWindow::GoBack()
    {
        if (m_history.empty())
            return;
        const std::uint64_t previous = m_history.back();
        m_history.pop_back();
        ShowFunction(previous, false);
    }

    void FlowGraphWindow::Activate(const Disasm::Instruction& line, bool open)
    {
        if (!open)
        {
            m_workspace.RevealInMemoryViewer(line.Address, line.Length, true, false);
            return;
        }
        if (line.Kind == Disasm::Flow::Call && line.Target)
        {
            ShowFunction(*line.Target);
            return;
        }
        if ((line.Kind == Disasm::Flow::Jump || line.Kind == Disasm::Flow::ConditionalJump) && line.Target)
        {
            if (m_graph.BlockAt(*line.Target) < 0)
            {
                ShowFunction(*line.Target);
                return;
            }
            m_selected = *line.Target;
            m_centerBlock = m_graph.BlockAt(*line.Target);
            return;
        }
        m_workspace.RevealInMemoryViewer(line.Address, line.Length, true, true);
    }

    FlowGraphWindow::Hit FlowGraphWindow::HitTest(ImVec2 world) const
    {
        for (std::size_t b = 0; b < m_graph.Blocks.size(); ++b)
        {
            const Layout::Point at = m_layout.Positions[b];
            if (world.x < at.X || world.y < at.Y || world.x > at.X + m_sizes[b].Width || world.y > at.Y + m_sizes[b].Height)
                continue;
            const float row = (world.y - at.Y - m_padding - m_lineHeight) / m_lineHeight;
            const int line = row >= 0.0f ? static_cast<int>(row) : -1;
            return { static_cast<int>(b), line >= 0 && line < static_cast<int>(m_graph.Blocks[b].Lines.size()) ? line : -1 };
        }
        return {};
    }

    void FlowGraphWindow::Fit(ImVec2 canvas)
    {
        if (m_layout.Width <= 0.0f || m_layout.Height <= 0.0f)
            return;
        const float margin = m_fontSize * 2.0f;
        SetZoom(std::min({ canvas.x / (m_layout.Width + margin * 2.0f), canvas.y / (m_layout.Height + margin * 2.0f), 1.0f }));
        m_pan = ImVec2(m_layout.Width * 0.5f - canvas.x * 0.5f / m_zoom, -margin);
        if (m_layout.Height * m_zoom < canvas.y)
            m_pan.y = m_layout.Height * 0.5f - canvas.y * 0.5f / m_zoom;
    }

    void FlowGraphWindow::SetZoom(float zoom)
    {
        m_zoomWanted = std::clamp(zoom, kMinZoom, kMaxZoom);
        const float pixels = std::round(m_fontSize * m_zoomWanted);
        m_zoom = pixels >= m_fontSize * kTextZoom ? std::min(pixels, std::floor(m_fontSize * kMaxZoom)) / m_fontSize : m_zoomWanted;
    }

    void FlowGraphWindow::CenterOn(int block, ImVec2 canvas)
    {
        if (block < 0 || static_cast<std::size_t>(block) >= m_sizes.size())
            return;
        const Layout::Point at = m_layout.Positions[static_cast<std::size_t>(block)];
        const float x = at.X + m_sizes[static_cast<std::size_t>(block)].Width * 0.5f;
        m_pan = ImVec2(x - canvas.x * 0.5f / m_zoom, at.Y - canvas.y * 0.15f / m_zoom);
    }

    std::string FlowGraphWindow::Mermaid() const
    {
        std::string text = "flowchart TD\n";
        for (std::size_t b = 0; b < m_graph.Blocks.size(); ++b)
        {
            const Disasm::FlowBlock& block = m_graph.Blocks[b];
            std::string label = MermaidText(BlockTitle(static_cast<int>(b)));
            for (std::size_t l = 0; l < block.Lines.size() && l < kMermaidLines; ++l)
                label += "<br/>" + MermaidText(block.Lines[l].Text());
            if (block.Lines.size() > kMermaidLines)
                label += "<br/>... " + std::to_string(block.Lines.size() - kMermaidLines) + " more";
            text += "    B" + std::to_string(b) + "[\"" + label + "\"]\n";
        }
        for (const Disasm::FlowEdge& edge : m_graph.Edges)
        {
            const char* label = edge.Kind == Disasm::EdgeKind::Taken ? "|taken|" : edge.Kind == Disasm::EdgeKind::NotTaken ? "|not taken|" : "";
            text += "    B" + std::to_string(edge.From) + (edge.Back ? " -.->" : " -->") + label + " B" + std::to_string(edge.To) + "\n";
        }
        return text;
    }
}
