#pragma once

#include "Lua/UiModel.h"

#include "imgui.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Manifold
{
    class Engine;
}

namespace Manifold::UI
{
    class Shell;

    struct UiDesign
    {
        struct Rect
        {
            int Node = 0;
            int Depth = 0;
            ImVec2 Min;
            ImVec2 Max;
        };

        bool Picking = false;
        int ForceTab = 0;
        std::vector<Rect> Rects;
    };

    class UiHost
    {
    public:
        UiHost(Engine& engine, Shell& shell, Lua::UiModel* model = nullptr);
        ~UiHost();
        UiHost(const UiHost&) = delete;
        UiHost& operator=(const UiHost&) = delete;

        void Draw();
        void DrawContents(int window, UiDesign* design);
        bool HasMenu();
        void DrawMenuItems();
        float ToolbarWidth();
        void DrawToolbar(float right, float y);
        float StatusWidth(float gap);
        void DrawStatus(float& x, float y, float gap);

        static ImVec4 Color(std::string_view name, ImVec4 fallback);
        static std::string IconText(std::string_view icon, std::string_view text);

    private:
        struct Texture
        {
            void* View = nullptr;
            int Width = 0;
            int Height = 0;
            std::string Path;
        };

        struct CanvasState
        {
            bool Dragging = false;
            bool Moved = false;
            ImVec2 Press;
            ImVec2 LastHover = ImVec2(-1.0e9f, -1.0e9f);
        };

        Lua::UiModel& Model();
        void DrawWindow(Lua::UiNode& window);
        bool DrawModal(std::size_t index);
        void CloseModals();
        void DrawChildren(const Lua::UiNode& node, bool row);
        void DrawNode(Lua::UiNode& node);
        void DrawSection(Lua::UiNode& node);
        void DrawTabs(Lua::UiNode& node);
        void DrawSplit(Lua::UiNode& node);
        void DrawGrid(Lua::UiNode& node);
        void DrawRow(Lua::UiNode& node);
        void DrawValue(Lua::UiNode& node);
        void DrawNodeBody(Lua::UiNode& node);
        void DropZone(bool row);
        void DrawTable(Lua::UiNode& node);
        void DrawTree(Lua::UiNode& node, const std::vector<Lua::UiTreeItem>& items);
        void DrawCanvas(Lua::UiNode& node);
        void DrawImage(const Lua::UiNode& node);
        void DrawDialogs();
        bool ContextMenu(Lua::UiNode& node, const char* id, int& choice);
        void Post(int target, std::string name, std::vector<Lua::UiValue> values = {});
        void* TextureOf(int image, int& width, int& height);
        void PruneTextures();
        void MenuItems(const std::vector<Lua::UiSlot>& slots, const std::string& prefix);

        Engine& m_engine;
        Shell& m_shell;
        Lua::UiModel* m_model = nullptr;
        UiDesign* m_design = nullptr;
        int m_depth = 0;
        bool m_inGrid = false;
        bool m_cell = false;
        std::map<int, float> m_rowWidths;
        // ∑ layout of fields: where labels go (inherited from the window and its
        // containers), the aligned label column of the current container, the
        // share of a row the widget being drawn gets, and the width a slotted
        // container's content stays within
        std::string m_labelPosition = "left";
        float m_labelWidth = 0.0f;
        bool m_labelChosen = false; // a window or container set labelPosition
        float m_rowSlot = 0.0f;
        bool m_inRow = false;
        float m_limit = 0.0f;
        float m_limitStart = 0.0f;
        std::map<int, float> m_itemWidths;
        float Avail() const;
        float SlotWidth(const Lua::UiNode& node) const;
        float FieldLayout(const Lua::UiNode& node, const std::string& side);
        std::map<int, Texture> m_textures;
        std::map<int, CanvasState> m_canvases;
        std::uint64_t m_textureGeneration = 0;
        int m_openDialog = 0;
        std::vector<int> m_modals;
        std::map<int, ImGuiID> m_modalIds;
        ImGuiID m_modalViewport = 0;
        std::string m_dialogText;
        int m_dialogChoice = 0;
        ImGuiID m_treePopup = 0;
    };
}
