#pragma once

#include "Core/GraphLayout.h"
#include "Disasm/FlowGraph.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class FlowGraphWindow : public ToolWindowBase
    {
    public:
        explicit FlowGraphWindow(Workspace& workspace) : m_workspace(workspace) {}
        void Draw() override;
        void ShowFunction(std::uint64_t address, bool remember = true);

    private:
        struct Hit
        {
            int Block = -1;
            int Line = -1;
        };

        std::uint64_t FunctionStart(std::uint64_t address) const;
        void Build(std::uint64_t entry);
        void Measure();
        void MenuBar();
        void Toolbar();
        void Canvas();
        void Minimap(ImVec2 canvasMin, ImVec2 canvasMax);
        void LineMenu(const Disasm::Instruction& line);
        void Fit(ImVec2 canvas);
        void SetZoom(float zoom);
        void CenterOn(int block, ImVec2 canvas);
        void Activate(const Disasm::Instruction& line, bool open);
        void GoBack();
        Hit HitTest(ImVec2 world) const;
        std::string BlockTitle(int block) const;
        std::string Mermaid() const;

        Workspace& m_workspace;
        Disasm::FlowGraph m_graph;
        Layout::Result m_layout;
        std::vector<Layout::Size> m_sizes;
        std::vector<std::uint64_t> m_history;
        std::uint64_t m_entry = 0;
        std::uint64_t m_generation = 0;
        std::uint64_t m_writes = 0;
        std::uint64_t m_selected = 0;
        std::uint64_t m_followed = 0;
        std::uint64_t m_breakSerial = 0;
        int m_loops = 0;
        int m_hoverBlock = -1;
        int m_centerBlock = -1;
        bool m_follow = false;
        bool m_fit = false;
        bool m_actualSize = false;
        bool m_skipRelease = false;
        bool m_built = false;
        bool m_dragging = false;
        ImVec2 m_pan{ 0.0f, 0.0f };
        float m_zoom = 1.0f;
        float m_zoomWanted = 1.0f;
        float m_fontSize = 13.0f;
        float m_lineHeight = 16.0f;
        float m_padding = 8.0f;
        float m_offsetWidth = 40.0f;
        float m_mnemonicWidth = 60.0f;
        std::uint64_t m_menuAddress = 0;
    };
}
