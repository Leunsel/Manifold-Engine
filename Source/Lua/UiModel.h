#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Manifold::Lua
{
    enum class UiKind : std::uint8_t
    {
        Window,
        Row,
        Column,
        Section,
        Tabs,
        Tab,
        Split,
        Grid,
        Text,
        Heading,
        Separator,
        Spacer,
        Button,
        Checkbox,
        Input,
        Number,
        Slider,
        Combo,
        Radio,
        Color,
        Table,
        Tree,
        Progress,
        Image,
        Canvas,
    };

    std::string_view UiKindName(UiKind kind);
    std::optional<UiKind> UiKindFromName(std::string_view name);
    bool UiIsContainer(UiKind kind);

    struct UiColumn
    {
        std::string Title;
        float Width = 0.0f;
        std::string Align;
    };

    struct UiTreeItem
    {
        std::string Id;
        std::string Text;
        std::string Icon;
        bool Open = false;
        std::vector<UiTreeItem> Children;
    };

    enum class UiDrawKind : std::uint8_t
    {
        Line,
        Rect,
        Circle,
        Polygon,
        Text,
        Image,
    };

    struct UiDraw
    {
        UiDrawKind Kind = UiDrawKind::Line;
        float X = 0.0f;
        float Y = 0.0f;
        float X2 = 0.0f;
        float Y2 = 0.0f;
        float Radius = 0.0f;
        float Thickness = 1.0f;
        float Rounding = 0.0f;
        float Size = 0.0f;
        bool Filled = false;
        std::string Color;
        std::string Text;
        std::vector<float> Points;
        int Image = 0;
    };

    struct UiNode
    {
        int Id = 0;
        UiKind Kind = UiKind::Text;
        int Parent = 0;
        std::vector<int> Children;
        std::string Owner;
        std::uint64_t Revision = 1;

        std::string Text;
        std::string Label;
        std::string Tooltip;
        std::string Icon;
        std::string Color;
        std::string Badge;
        std::string Hint;
        std::string Format;
        std::string Style;
        bool Enabled = true;
        bool Visible = true;
        bool Wrap = false;
        bool Mono = false;
        float Width = 0.0f;
        float Height = 0.0f;

        bool Bool = false;
        double Number = 0.0;
        double Min = 0.0;
        double Max = 0.0;
        double Step = 0.0;
        bool HasMin = false;
        bool HasMax = false;
        bool Integer = false;
        bool Hex = false;
        std::string String;
        bool Multiline = false;
        bool Password = false;
        int Selected = 0;
        bool Multi = false;        // combo: several items can be chosen (Chosen, not Selected)
        std::vector<int> Chosen;   // combo with Multi: the chosen items' numbers, ascending
        std::vector<std::string> Items;
        std::vector<UiColumn> Columns;
        std::vector<std::vector<std::string>> Rows;
        std::vector<UiTreeItem> Tree;
        std::string SelectedId;
        std::vector<std::string> Menu;
        bool Vertical = false;
        bool Horizontal = false;
        float Ratio = 0.5f;
        bool Collapsible = false;
        int GridColumns = 2;
        std::string Align;
        bool Alpha = false;

        std::vector<UiDraw> Draws;
        std::string Background;
        bool Zoom = false;
        float ViewX = 0.0f;
        float ViewY = 0.0f;
        float Scale = 1.0f;
        bool ViewChanged = false;
        int ImageId = 0;
        std::string Source;
        float Fraction = 0.0f;

        std::string Title;
        bool Open = true;
        bool FocusRequest = false;
        bool Dock = true;
        float MinWidth = 0.0f;
        float MinHeight = 0.0f;
        float MaxWidth = 0.0f;
        float MaxHeight = 0.0f;
        bool Resizable = true;
        bool AutoSize = false;
        bool Scrollbar = true;
        bool Topmost = false;
        bool Modal = false;
        std::string Position;
        float PosX = 0.0f;
        float PosY = 0.0f;
        bool HasPos = false;
        std::string Key;
        std::string Shortcut;
        std::string Confirm;
        bool ReadOnly = false;
        int MaxLength = 0;
        bool Focus = false;
        bool Focused = false;
        float Gap = -1.0f;
        bool Scroll = false;
        float Opacity = -1.0f;          // below 0: the user's setting
        float InactiveOpacity = -1.0f;  // below 0: Opacity, else the user's setting
        double Interval = 0.0;
        double NextUpdate = 0.0;
        bool Updates = false;
        bool Binds = false;        // window: widgets bound to memory records, refreshed every kBindInterval
        double NextBind = 0.0;
        std::uint32_t Handlers = 0;
    };

    // seconds between two refreshes of a window's record-bound widgets
    inline constexpr double kBindInterval = 0.1;

    namespace UiHandler
    {
        constexpr std::uint32_t Click = 1u << 0;
        constexpr std::uint32_t Change = 1u << 1;
        constexpr std::uint32_t Select = 1u << 2;
        constexpr std::uint32_t Activate = 1u << 3;
        constexpr std::uint32_t Menu = 1u << 4;
        constexpr std::uint32_t Hover = 1u << 5;
        constexpr std::uint32_t Submit = 1u << 6;
        constexpr std::uint32_t Close = 1u << 7;
        constexpr std::uint32_t View = 1u << 8;
        constexpr std::uint32_t Key = 1u << 9;
    }

    struct UiImage
    {
        int Id = 0;
        int Width = 0;
        int Height = 0;
        std::vector<std::uint8_t> Rgba;
        std::string Owner;
        std::string Path;
    };

    enum class UiSlotKind : std::uint8_t
    {
        Menu,
        Toolbar,
        Status,
    };

    struct UiSlot
    {
        int Id = 0;
        UiSlotKind Kind = UiSlotKind::Menu;
        std::string Path;
        std::string Text;
        std::string Icon;
        std::string Tooltip;
        std::string Color;
        bool Enabled = true;
        bool Checked = false;
        bool Checkable = false;
        std::string Owner;
        std::uint64_t Order = 0;
    };

    enum class UiDialogKind : std::uint8_t
    {
        Confirm,
        Input,
        Choose,
        OpenFile,
        SaveFile,
    };

    struct UiDialog
    {
        int Id = 0;
        UiDialogKind Kind = UiDialogKind::Confirm;
        std::string Title;
        std::string Text;
        std::string Value;
        std::vector<std::string> Items;
        std::vector<std::pair<std::string, std::string>> Filters;
        std::string Owner;
        bool Danger = false;
    };

    using UiValue = std::variant<std::monostate, bool, double, std::string>;

    struct UiWatch
    {
        int Window = 0;
        std::string Path;
        std::uint64_t Stamp = 0;
    };

    struct UiEvent
    {
        int Target = 0;
        std::string Name;
        std::vector<UiValue> Values;
    };

    class UiModel
    {
    public:
        int Create(UiKind kind, int parent, std::string owner);
        UiNode* Find(int id);
        const UiNode* Find(int id) const;
        bool Remove(int id);
        void Clear(int id);
        void Touch(UiNode& node);
        void Changed() { ++m_generation; }
        std::vector<int> Windows() const;
        std::vector<int> Owned(std::string_view owner) const;
        std::size_t NodeCount() const { return m_nodes.size(); }

        int AddImage(UiImage image);
        const UiImage* FindImage(int id) const;
        bool RemoveImage(int id);

        int AddSlot(UiSlot slot);
        UiSlot* FindSlot(int id);
        bool RemoveSlot(int id);
        std::vector<UiSlot> Slots(UiSlotKind kind) const;

        int AddDialog(UiDialog dialog);
        bool RemoveDialog(int id);
        const std::vector<UiDialog>& Dialogs() const { return m_dialogs; }

        void Post(UiEvent event);
        std::vector<UiEvent> TakeEvents();
        bool HasEvents() const { return !m_events.empty(); }
        double NextUpdate() const;

        void ForgetOwner(std::string_view owner);
        void Reset();

        void Watch(int window, std::string path);
        void Unwatch(int window);
        int Watching(std::string_view path) const;
        void PollWatches(double now);
        static std::uint64_t FileStamp(std::string_view path);
        std::uint64_t Generation() const { return m_generation; }

    private:
        void RemoveTree(int id);

        std::map<int, UiNode> m_nodes;
        std::vector<int> m_windows;
        std::map<int, UiImage> m_images;
        std::map<int, UiSlot> m_slots;
        std::vector<UiDialog> m_dialogs;
        std::vector<UiEvent> m_events;
        std::vector<UiWatch> m_watches;
        double m_nextPoll = 0.0;
        int m_next = 1;
        std::uint64_t m_order = 0;
        std::uint64_t m_generation = 1;
    };
}
