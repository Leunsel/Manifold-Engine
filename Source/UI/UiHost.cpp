#include "UI/UiHost.h"

#include "App/Commands.h"
#include "App/Engine.h"
#include "Core/IconNames.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>

namespace Manifold::UI
{
    namespace
    {
        using Lua::UiKind;
        namespace H = Lua::UiHandler;

        struct Context
        {
            int Node = 0;
            int Row = 0;
            std::string Item;
            float X = 0.0f;
            float Y = 0.0f;
        };
        Context g_context;

        int HexDigit(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        std::string Label(const Lua::UiNode& node, std::string_view text)
        {
            return std::string(text) + "##v" + std::to_string(node.Id);
        }

        // the kinds whose label the layout places (left, above or right)
        bool PlacedLabel(UiKind kind)
        {
            return kind == UiKind::Input || kind == UiKind::Number || kind == UiKind::Slider || kind == UiKind::Combo || kind == UiKind::Color || kind == UiKind::Radio;
        }

        // ∑ the kinds that share a row's free width when they have no width of
        // their own (fields, bars, cards, lists, a flexible spacer)
        bool Flexible(const Lua::UiNode& node)
        {
            if (node.Width > 0.0f)
                return false;
            if (node.Width < 0.0f)
                return true; // "fill" in a row: a share of the line
            switch (node.Kind)
            {
            case UiKind::Input:
            case UiKind::Number:
            case UiKind::Slider:
            case UiKind::Combo:
            case UiKind::Color:
            case UiKind::Progress:
            case UiKind::Section:
            case UiKind::Table:
            case UiKind::Tree:
            case UiKind::Canvas:
            case UiKind::Column:
            case UiKind::Spacer:
                return true;
            default:
                return false;
            }
        }

        // ∑ an integer slider formats a 64-bit value: "%d" becomes "%lld", and a
        // float format ("%.1f x") keeps its text around a "%lld"
        std::string IntegerFormat(const std::string& format)
        {
            if (format.empty())
                return "%lld";
            const std::size_t at = format.find('%');
            if (at == std::string::npos)
                return format + " %lld";
            std::size_t end = at + 1;
            while (end < format.size() && std::string_view("-+ #0123456789.lhjzt").find(format[end]) != std::string_view::npos)
                ++end;
            if (end >= format.size())
                return format;
            const char conversion = format[end];
            if (conversion == '%')
                return format;
            if (std::string_view("dioxXuc").find(conversion) == std::string_view::npos && std::string_view("fFeEgGaA").find(conversion) == std::string_view::npos)
                return "%lld";
            const char kind = conversion == 'x' || conversion == 'X' || conversion == 'o' || conversion == 'u' ? conversion : 'd';
            return format.substr(0, at) + "%ll" + kind + format.substr(end + 1);
        }

        Layout::Overflow OverflowOf(const Lua::UiNode& node)
        {
            if (node.Overflow == "wrap" || node.Wrap || node.MaxWidth > 0.0f)
                return Layout::Overflow::Wrap;
            if (node.Overflow == "clip")
                return Layout::Overflow::Clip;
            if (node.Overflow == "visible")
                return Layout::Overflow::Visible;
            return Layout::Overflow::Ellipsis;
        }

        Layout::Align AlignOf(const std::string& align)
        {
            return align == "right" ? Layout::Align::End : align == "center" ? Layout::Align::Center : Layout::Align::Start;
        }

        // a height of 0 is the kind's own, a negative one fills what is left below
        float FillHeight(float points, float fallback)
        {
            if (points > 0.0f)
                return Px(points);
            if (points < 0.0f)
                return std::max(ImGui::GetContentRegionAvail().y, Px(40.0f));
            return fallback;
        }

        std::optional<std::string> FileDialog(const Lua::UiDialog& dialog, void* owner)
        {
            std::wstring filter;
            for (const auto& [description, pattern] : dialog.Filters)
            {
                filter += Utf8ToWide(description);
                filter.push_back(L'\0');
                filter += Utf8ToWide(pattern);
                filter.push_back(L'\0');
            }
            if (filter.empty())
            {
                filter = L"All files";
                filter.push_back(L'\0');
                filter += L"*.*";
                filter.push_back(L'\0');
            }
            filter.push_back(L'\0');
            std::wstring file = Utf8ToWide(dialog.Value);
            file.resize(32768, L'\0');
            const std::wstring title = Utf8ToWide(dialog.Title);
            OPENFILENAMEW request{};
            request.lStructSize = sizeof(request);
            request.hwndOwner = static_cast<HWND>(owner);
            request.lpstrFilter = filter.c_str();
            request.lpstrFile = file.data();
            request.nMaxFile = static_cast<DWORD>(file.size());
            request.lpstrTitle = title.empty() ? nullptr : title.c_str();
            request.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (dialog.Kind == Lua::UiDialogKind::SaveFile ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
            const BOOL chosen = dialog.Kind == Lua::UiDialogKind::SaveFile ? ::GetSaveFileNameW(&request) : ::GetOpenFileNameW(&request);
            if (!chosen)
                return std::nullopt;
            file.resize(std::wcslen(file.c_str()));
            return WideToUtf8(file);
        }
    }

    UiHost::UiHost(Engine& engine, Shell& shell, Lua::UiModel* model) : m_engine(engine), m_shell(shell), m_model(model) {}

    UiHost::~UiHost()
    {
        for (auto& [id, texture] : m_textures)
        {
            if (texture.View)
                static_cast<ID3D11ShaderResourceView*>(texture.View)->Release();
        }
    }

    Lua::UiModel& UiHost::Model()
    {
        return m_model ? *m_model : m_engine.Lua().Ui();
    }

    ImVec4 UiHost::Color(std::string_view name, ImVec4 fallback)
    {
        if (name.empty())
            return fallback;
        if (name.front() == '#' && (name.size() == 7 || name.size() == 9))
        {
            float channels[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            for (std::size_t i = 0; i * 2 + 1 < name.size(); ++i)
            {
                const int high = HexDigit(name[1 + i * 2]);
                const int low = HexDigit(name[2 + i * 2]);
                if (high < 0 || low < 0)
                    return fallback;
                channels[i] = static_cast<float>(high * 16 + low) / 255.0f;
            }
            return ImVec4(channels[0], channels[1], channels[2], channels[3]);
        }
        for (const ManifoldTheme::PaletteColor& entry : ManifoldTheme::kPalette)
        {
            if (EqualsNoCase(entry.Name, name))
                return *entry.Value;
        }
        return fallback;
    }

    std::string UiHost::IconText(std::string_view icon, std::string_view text)
    {
        const std::string_view glyph = IconGlyph(icon);
        if (glyph.empty())
            return std::string(text);
        if (text.empty())
            return std::string(glyph);
        return std::string(glyph) + "  " + std::string(text);
    }

    void UiHost::Post(int target, std::string name, std::vector<Lua::UiValue> values)
    {
        Lua::UiEvent event;
        event.Target = target;
        event.Name = std::move(name);
        event.Values = std::move(values);
        Model().Post(std::move(event));
    }

    void UiHost::Draw()
    {
        Lua::UiModel& model = Model();
        // a field is active only while it is drawn with the keyboard on it
        if (Lua::UiNode* last = model.Find(m_activeField))
            last->Active = false;
        m_activeField = 0;
        CloseModals();
        for (const int id : model.Windows())
        {
            Lua::UiNode* node = model.Find(id);
            if (!node || !node->Open)
                continue;
            if (node->Modal)
            {
                if (std::find(m_modals.begin(), m_modals.end(), id) == m_modals.end())
                    m_modals.push_back(id);
                continue;
            }
            DrawWindow(*node);
        }
        if (!m_modals.empty())
            DrawModal(0);
        DrawDialogs();
        PruneTextures();
    }

    void UiHost::DrawWindow(Lua::UiNode& window)
    {
        const int id = window.Id;
        // ∑ A window with a key keeps the same ImGui name across runs, so ImGui
        // remembers where it was, how big and where it was docked.
        const std::string title = IconText(window.Icon, window.Title.empty() ? std::string("Script") : window.Title) +
                                  (window.Key.empty() ? "###manifold.ui." + std::to_string(id) : "###manifold.ui.key." + window.Key);
        if (window.FocusRequest)
        {
            ImGui::SetNextWindowFocus();
            RequestWindowRaise(title.c_str());
            window.FocusRequest = false;
        }
        bool open = true;
        const ImVec2 size(window.Width > 0.0f ? window.Width : 420.0f, window.Height > 0.0f ? window.Height : 320.0f);
        const std::function<bool(const Lua::UiNode&)> wantsKeys = [&](const Lua::UiNode& node)
            {
                if (node.Kind == Lua::UiKind::Canvas && (node.Handlers & Lua::UiHandler::Key))
                    return true;
                for (const int child : node.Children)
                {
                    if (const Lua::UiNode* found = Model().Find(child); found && wantsKeys(*found))
                        return true;
                }
                return false;
            };
        // ∑ a docked window takes the dock's size, so a fixed or automatic size
        // (and topmost) keeps the window floating, like it keeps its size
        ImGuiWindowFlags flags = window.Dock && !window.Topmost && window.Resizable && !window.AutoSize ? 0 : ImGuiWindowFlags_NoDocking;
        if (wantsKeys(window))
            flags |= ImGuiWindowFlags_NoNavInputs;
        if (!window.Resizable)
            flags |= ImGuiWindowFlags_NoResize;
        if (window.AutoSize)
            flags |= ImGuiWindowFlags_AlwaysAutoResize;
        if (!window.Scrollbar)
            flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ToolWindowExtras extras;
        extras.Maximum = ImVec2(window.MaxWidth, window.MaxHeight);
        extras.Topmost = window.Topmost;
        extras.Placement = window.HasPos ? 3 : window.Position == "center" ? 1 : window.Position == "mouse" ? 2 : 0;
        extras.Position = ImVec2(window.PosX, window.PosY);
        // ∑ never narrower than its content can take (measured last frame):
        // the script's minWidth or more, at most most of the monitor
        const ImVec2 minimum(std::max(window.MinWidth > 0.0f ? window.MinWidth : 160.0f, ContentMinimum(id) / Layout::Scale()), window.MinHeight > 0.0f ? window.MinHeight : 100.0f);
        const bool visible = BeginToolWindow(title.c_str(), &open, size, flags, minimum, false, &extras);
        if (window.Opacity >= 0.0f || window.InactiveOpacity >= 0.0f)
        {
            const Settings& settings = m_engine.Options();
            const float active = window.Opacity >= 0.0f ? window.Opacity : static_cast<float>(settings.ActiveWindowOpacity) / 100.0f;
            const float inactive = window.InactiveOpacity >= 0.0f ? window.InactiveOpacity
                                 : (window.Opacity >= 0.0f ? window.Opacity : static_cast<float>(settings.InactiveWindowOpacity) / 100.0f);
            SetWindowOpacity(active, inactive);
        }
        if (visible)
        {
            // a modal opens over the script window the user works in
            if (m_modals.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                m_modalViewport = ImGui::GetWindowViewport()->ID;
            ImGui::PushID(id);
            DrawRoot(window);
            ImGui::PopID();
        }
        EndToolWindow();
        if (!open)
        {
            if (Lua::UiNode* again = Model().Find(id))
            {
                again->Open = false;
                Model().Touch(*again);
                if (again->Handlers & H::Close)
                    Post(id, "close");
            }
        }
    }

    // ∑ Modal windows are ImGui modal popups. A modal opened while another
    // is up is begun inside it (ImGui stacks popups by nesting), so they are
    // drawn as a chain in the order they opened. A modal the script hides or
    // removes is closed in ImGui's popup stack too, or it would linger there.
    void UiHost::CloseModals()
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        for (std::size_t i = 0; i < m_modals.size(); ++i)
        {
            const Lua::UiNode* node = Model().Find(m_modals[i]);
            if (node && node->Open && node->Modal)
                continue;
            if (const auto known = m_modalIds.find(m_modals[i]); known != m_modalIds.end())
            {
                for (int level = 0; level < g.OpenPopupStack.Size; ++level)
                {
                    if (g.OpenPopupStack[level].PopupId == known->second)
                    {
                        ImGui::ClosePopupToLevel(level, true);
                        break;
                    }
                }
                m_modalIds.erase(known);
            }
            // the ones opened from it go with it
            for (std::size_t j = i + 1; j < m_modals.size(); ++j)
            {
                if (Lua::UiNode* later = Model().Find(m_modals[j]); later && later->Open)
                {
                    later->Open = false;
                    Model().Touch(*later);
                    if (later->Handlers & H::Close)
                        Post(later->Id, "close");
                }
                m_modalIds.erase(m_modals[j]);
            }
            m_modals.resize(i);
            break;
        }
    }

    bool UiHost::DrawModal(std::size_t index)
    {
        if (index >= m_modals.size())
            return false;
        Lua::UiNode* found = Model().Find(m_modals[index]);
        if (!found)
            return false;
        Lua::UiNode& window = *found;
        const int id = window.Id;
        const std::string popup = "manifold.ui.modal." + std::to_string(id);
        const std::string hashed = "###" + popup;
        if (!ImGui::IsPopupOpen(hashed.c_str()))
        {
            ImGui::OpenPopup(hashed.c_str());
            m_modalIds[id] = ImGui::GetID(hashed.c_str());
        }
        if (window.FocusRequest)
            window.FocusRequest = false;
        const auto limit = [](float value, float low, float high) {
            if (high > 0.0f)
                value = std::min(value, high);
            return std::max(value, low);
        };
        const ImVec2 minimum(std::max(window.MinWidth > 0.0f ? window.MinWidth : 160.0f, ContentMinimum(id) / Layout::Scale()), window.MinHeight > 0.0f ? window.MinHeight : 80.0f);
        const ImVec2 size(limit(window.Width > 0.0f ? window.Width : 360.0f, minimum.x, window.MaxWidth), limit(window.Height > 0.0f ? window.Height : 200.0f, minimum.y, window.MaxHeight));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking;
        if (!window.Resizable)
            flags |= ImGuiWindowFlags_NoResize;
        if (window.AutoSize)
            flags |= ImGuiWindowFlags_AlwaysAutoResize;
        if (!window.Scrollbar)
            flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (!window.AutoSize)
            ImGui::SetNextWindowSizeConstraints(Px(minimum.x, minimum.y), ImVec2(window.MaxWidth > 0.0f ? Px(window.MaxWidth) : FLT_MAX, window.MaxHeight > 0.0f ? Px(window.MaxHeight) : FLT_MAX));
        bool open = true;
        const std::string_view glyph = IconGlyph(window.Icon);
        const std::string title = window.Title.empty() ? std::string("Script") : window.Title;
        const ImGuiID viewport = m_modalViewport && ImGui::FindViewportByID(m_modalViewport) ? m_modalViewport : 0;
        const bool visible = BeginModal(popup.c_str(), glyph.empty() ? ICON_FA_WINDOW_RESTORE : std::string(glyph).c_str(), title.c_str(), window.AutoSize ? ImVec2(0.0f, 0.0f) : size, &open, viewport, flags);
        if (visible)
        {
            ImGui::PushID(id);
            DrawRoot(window);
            ImGui::PopID();
            const bool top = index + 1 >= m_modals.size();
            if (top && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive())
                open = false;
            if (open)
                DrawModal(index + 1);
            EndModal();
        }
        if (!open)
        {
            if (Lua::UiNode* again = Model().Find(id))
            {
                again->Open = false;
                Model().Touch(*again);
                if (again->Handlers & H::Close)
                    Post(id, "close");
            }
        }
        return visible;
    }

    void UiHost::DrawContents(int window, UiDesign* design)
    {
        Lua::UiNode* node = Model().Find(window);
        if (!node)
            return;
        UiDesign* const outer = m_design;
        m_design = design;
        m_depth = 0;
        if (design)
            design->Rects.clear();
        const bool picking = design && design->Picking;
        if (picking)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
            ImGui::BeginDisabled();
        }
        ImGui::PushID(window);
        DrawRoot(*node);
        ImGui::PopID();
        if (picking)
        {
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
        }
        m_design = outer;
        PruneTextures();
    }

    float UiHost::ContentMinimum(int window) const
    {
        const auto known = m_contentMinimum.find(window);
        if (known == m_contentMinimum.end() || known->second <= 0.0f)
            return 0.0f;
        const ImGuiStyle& style = ImGui::GetStyle();
        const float decoration = style.WindowPadding.x * 2.0f + style.ScrollbarSize;
        const float cap = ImGui::GetMainViewport()->WorkSize.x * 0.8f;
        return std::min(known->second + decoration, cap);
    }

    // ∑ the window's content is one region: whatever it holds stays inside
    // the window's width, and what it reports is the window's minimum
    void UiHost::DrawRoot(Lua::UiNode& window)
    {
        const int id = window.Id;
        m_rules.Bold = Layout::Defaults().Bold;
        m_rules.Mono = Layout::Defaults().Mono;
        Layout::RulesScope rules(m_rules);
        m_labelPosition = "left";
        m_labelWidth = 0.0f;
        m_labelChosen = false;
        m_inRow = false;
        Layout::Region root;
        DrawChildren(window, false);
        root.End();
        m_contentMinimum[id] = root.Reports().Minimum;
    }

    Layout::Size UiHost::SizeOf(const Lua::UiNode& node) const
    {
        Layout::Size size = node.Width > 0.0f ? Layout::Size::Fixed(node.Width) : Flexible(node) ? Layout::Size::Fill() : Layout::Size::Fit();
        // a field's minWidth and maxWidth are its field's (FieldLayout); the others' their cell's
        if (!PlacedLabel(node.Kind))
        {
            if (node.MinWidth > 0.0f)
                size = size.Min(node.MinWidth);
            if (node.MaxWidth > 0.0f)
                size = size.Max(node.MaxWidth);
        }
        return size;
    }

    void UiHost::DrawChildren(const Lua::UiNode& node, bool row)
    {
        const bool gap = node.Gap >= 0.0f;
        if (gap && !row)
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Px(node.Gap)));
        std::vector<int> shown;
        for (const int child : node.Children)
        {
            const Lua::UiNode* item = Model().Find(child);
            if (item && (item->Visible || m_design))
                shown.push_back(child);
        }
        // ∑ labels: the window's and each container's labelPosition is the
        // default for the fields inside; left labels of one container share a
        // column (at most the rules' share of the line), so the fields line
        // up; a row puts each label beside its field
        const std::string outerPosition = m_labelPosition;
        const float outerLabelWidth = m_labelWidth;
        const bool outerChosen = m_labelChosen;
        if (!node.LabelPosition.empty())
        {
            m_labelPosition = node.LabelPosition;
            m_labelChosen = true;
        }
        m_labelWidth = 0.0f;
        if (!row)
        {
            std::vector<std::string_view> labels;
            for (const int child : shown)
            {
                const Lua::UiNode* item = Model().Find(child);
                if (item && PlacedLabel(item->Kind) && !item->Label.empty() && (item->LabelPosition.empty() ? m_labelPosition : item->LabelPosition) == "left")
                    labels.push_back(item->Label);
            }
            if (!labels.empty())
                m_labelWidth = Layout::LabelColumn(labels);
            // ∑ too narrow for labels beside the fields: unless a window or
            // container asked for left labels, they go above, so no field is
            // squeezed until its value is cut off (a field's own wins)
            if (!m_labelChosen && m_labelPosition == "left" && m_labelWidth > 0.0f
                && Layout::Avail() - m_labelWidth - ImGui::GetStyle().ItemInnerSpacing.x * 2.0f < Layout::Pt(Layout::Current().NarrowField))
            {
                m_labelPosition = "above";
                m_labelWidth = 0.0f;
            }
        }
        const bool outerRow = m_inRow;
        if (row)
        {
            // ∑ a row: the layout's row shares the line (fields, bars, cards,
            // lists and spacers without a width of their own fill it, the
            // others get what they measured), every widget stays in its cell;
            // a wrapping row moves what does not fit to the next line
            std::vector<Layout::Size> cells;
            for (const int child : shown)
            {
                if (const Lua::UiNode* item = Model().Find(child))
                    cells.push_back(SizeOf(*item));
                else
                    cells.push_back(Layout::Size::Fit());
            }
            Layout::RowOptions options;
            options.Wrap = node.Wrap;
            if (gap)
                options.GapPixels = Px(node.Gap);
            options.Justify = node.Align == "right" ? Layout::Align::End : node.Align == "center" ? Layout::Align::Center : Layout::Align::Start;
            Layout::Row line("##row", std::move(cells), options);
            for (const int child : shown)
            {
                line.Cell([&] {
                    Lua::UiNode* item = Model().Find(child);
                    if (!item)
                        return;
                    m_inRow = true;
                    DrawNode(*item);
                });
            }
            line.End();
        }
        else
        {
            for (const int child : shown)
            {
                Lua::UiNode* item = Model().Find(child);
                if (!item)
                    continue;
                m_inRow = false;
                DrawNode(*item);
            }
        }
        m_inRow = outerRow;
        m_labelPosition = outerPosition;
        m_labelWidth = outerLabelWidth;
        m_labelChosen = outerChosen;
        if (gap && !row)
            ImGui::PopStyleVar();
        if (shown.empty() && m_design && node.Kind != UiKind::Window)
            DropZone(row);
    }

    void UiHost::DropZone(bool row)
    {
        const float width = row ? Px(160.0f) : std::max(Px(120.0f), ImGui::GetContentRegionAvail().x);
        ImGui::Dummy(ImVec2(width, Px(34.0f)));
    }

    void UiHost::DrawRow(Lua::UiNode& node)
    {
        DrawChildren(node, true);
    }

    void UiHost::DrawGrid(Lua::UiNode& node)
    {
        const int columns = std::clamp(node.GridColumns, 1, 16);
        const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : 0.0f, 0.0f);
        if (!ImGui::BeginTable("##grid", columns, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX, size))
            return;
        // ∑ label columns fit their text: the first one, and in an even grid
        // ("label | field | label | field") every second one
        for (int c = 0; c < columns; ++c)
        {
            const bool label = columns > 1 && (c == 0 || (columns % 2 == 0 && c % 2 == 0));
            ImGui::TableSetupColumn(nullptr, label ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch);
        }
        const bool outer = m_inGrid;
        const float outerLabelWidth = m_labelWidth;
        m_labelWidth = 0.0f; // a cell's field keeps its label beside it
        if (m_design && node.Children.empty())
        {
            ImGui::TableNextColumn();
            DropZone(false);
        }
        const std::vector<int> children = node.Children;
        for (const int child : children)
        {
            Lua::UiNode* item = Model().Find(child);
            if (!item)
                continue;
            ImGui::TableNextColumn();
            // a hidden widget keeps its cell, so the cells after it stay in their columns
            if (!item->Visible && !m_design)
                continue;
            m_inGrid = true;
            DrawNode(*item);
        }
        m_inGrid = outer;
        m_labelWidth = outerLabelWidth;
        ImGui::EndTable();
    }

    void UiHost::DrawNode(Lua::UiNode& node)
    {
        if (!m_design)
        {
            DrawNodeBody(node);
            return;
        }
        const bool dim = !node.Visible || (!node.Enabled && m_design->Picking);
        if (dim)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (node.Visible ? 0.5f : 0.35f));
        ++m_depth;
        const float outerField = m_fieldWidth;
        m_fieldWidth = 0.0f;
        ImGui::BeginGroup();
        DrawNodeBody(node);
        ImGui::EndGroup();
        const float field = m_fieldWidth;
        m_fieldWidth = outerField;
        --m_depth;
        if (dim)
            ImGui::PopStyleVar();
        m_design->Rects.push_back({ node.Id, m_depth, ImGui::GetItemRectMin(), ImGui::GetItemRectMax() });
        m_design->Rects.back().FieldWidth = field;
    }

    void UiHost::DrawNodeBody(Lua::UiNode& node)
    {
        ImGui::PushID(node.Id);
        ImGui::BeginDisabled(!node.Enabled);
        const bool inGrid = m_inGrid;
        m_cell = inGrid;
        m_inGrid = false;
        // one group around the widget, so its tooltip covers all of it (a
        // switch and its text, a field and its label, every radio button)
        const bool tooltip = !node.Tooltip.empty();
        if (tooltip)
            ImGui::BeginGroup();
        switch (node.Kind)
        {
        case UiKind::Row:
            DrawRow(node);
            break;
        case UiKind::Grid:
            DrawGrid(node);
            break;
        case UiKind::Column:
        case UiKind::Tab:
        {
            // a column keeps its content within its width (its cell's in a row)
            const float width = node.Width > 0.0f ? Px(node.Width) : 0.0f;
            if (node.Scroll)
            {
                Layout::Report(width > 0.0f ? width : Layout::Pt(160.0f), Layout::Pt(Layout::Current().MinCard.x));
                const ImVec2 size(width, FillHeight(node.Height, Px(200.0f)));
                ImGui::BeginChild("##scroll", size, ImGuiChildFlags_None);
                DrawChildren(node, false);
                ImGui::EndChild();
                break;
            }
            Layout::RegionOptions options;
            options.Width = width;
            Layout::Region column(options);
            DrawChildren(node, false);
            column.End();
            break;
        }
        case UiKind::Section:
            DrawSection(node);
            break;
        case UiKind::Tabs:
            DrawTabs(node);
            break;
        case UiKind::Split:
            DrawSplit(node);
            break;
        case UiKind::Text:
        case UiKind::Heading:
        {
            // ∑ text and headings: a colour, a size, bold, an alignment, and
            // what happens when the place is too narrow (overflow: "..." by
            // default, wrap with wrap or maxWidth); text beside a field (in a
            // row or a cell) sits on the field's line
            const bool heading = node.Kind == UiKind::Heading;
            const std::string text = heading ? IconText(node.Icon, node.Text) : node.Text;
            Layout::TextStyle style;
            style.Color = Color(node.Color, ManifoldTheme::Text);
            style.Font = heading || node.Bold ? m_rules.Bold : node.Mono ? m_rules.Mono : nullptr;
            style.FontSize = node.FontSize > 0.0f ? Px(node.FontSize) : 0.0f;
            style.Mode = heading && OverflowOf(node) == Layout::Overflow::Wrap ? Layout::Overflow::Ellipsis : OverflowOf(node);
            style.Justify = AlignOf(node.Align);
            style.MaxWidth = node.MaxWidth > 0.0f ? Px(node.MaxWidth) : 0.0f;
            style.FrameLine = m_cell || m_inRow;
            style.Tooltip = node.Tooltip.empty();
            Layout::Text(text, style);
            break;
        }
        case UiKind::Separator:
            if (node.Text.empty())
                ImGui::Separator();
            else
                ImGui::SeparatorText(node.Text.c_str());
            break;
        case UiKind::Spacer:
            // in a row a spacer without a width is a filling cell: what follows goes to the right
            ImGui::Dummy(ImVec2(m_inRow && node.Width <= 0.0f ? 1.0f : Px(node.Width > 0.0f ? node.Width : 1.0f), Px(node.Height > 0.0f ? node.Height : 8.0f)));
            break;
        case UiKind::Button:
        {
            // an icon without text makes an icon button
            std::string caption = node.Text.empty() && !node.Icon.empty() ? IconText(node.Icon, "") : IconText(node.Icon, node.Text.empty() ? std::string("Button") : node.Text);
            if (!node.Hotkey.empty() && node.Confirm.empty())
                caption += "  (" + node.Hotkey + ")";
            const std::string label = Label(node, caption);
            Layout::ButtonStyle look;
            look.Kind = node.Style == "accent" ? Layout::ButtonKind::Accent : node.Style == "danger" ? Layout::ButtonKind::Danger : Layout::ButtonKind::Normal;
            look.Width = node.Width > 0.0f ? Layout::Size::Fixed(node.Width) : (node.Width < 0.0f || m_cell) ? Layout::Size::Fill() : Layout::Size::Fit();
            if (node.MinWidth > 0.0f)
                look.Width = look.Width.Min(node.MinWidth);
            if (node.MaxWidth > 0.0f)
                look.Width = look.Width.Max(node.MaxWidth);
            look.Height = node.Height > 0.0f ? Px(node.Height) : 0.0f;
            look.AutoTooltip = node.Tooltip.empty();
            bool clicked = Layout::Button(label, look);
            if (!node.Shortcut.empty() && !m_design)
            {
                const ImGuiKeyChord chord = ParseChord(node.Shortcut);
                if (chord != 0)
                {
                    if (node.Enabled && ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused))
                        clicked = true;
                    if (node.Tooltip.empty())
                        ItemTooltip(node.Shortcut.c_str());
                }
            }
            if (clicked && !node.Confirm.empty())
            {
                ImGui::OpenPopup("##confirm");
                clicked = false;
            }
            if (ImGui::BeginPopup("##confirm"))
            {
                Layout::TextStyle question;
                question.Mode = Layout::Overflow::Wrap;
                question.MaxWidth = Px(320.0f);
                Layout::Text(node.Confirm, question);
                ImGui::Spacing();
                const std::string yes = IconText(node.Icon, node.Text.empty() ? std::string("OK") : node.Text) + "##yes";
                if (Layout::Button(yes, node.Style == "danger" ? Layout::ButtonKind::Danger : Layout::ButtonKind::Accent))
                {
                    clicked = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (Layout::Button("Cancel##no") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            if (clicked && (node.Handlers & H::Click))
                Post(node.Id, "click");
            break;
        }
        case UiKind::Progress:
            Layout::Report(node.Width > 0.0f ? Px(node.Width) : Layout::Pt(160.0f), node.Width > 0.0f ? Px(node.Width) : Layout::Pt(60.0f));
            ImGui::ProgressBar(node.Fraction, ImVec2(node.Width > 0.0f ? Px(node.Width) : -FLT_MIN, node.Height > 0.0f ? Px(node.Height) : 0.0f), node.Text.empty() ? nullptr : node.Text.c_str());
            break;
        case UiKind::Image:
            DrawImage(node);
            break;
        case UiKind::Canvas:
            DrawCanvas(node);
            break;
        case UiKind::Table:
            DrawTable(node);
            break;
        case UiKind::Tree:
        {
            Layout::Report(node.Width > 0.0f ? Px(node.Width) : Layout::Pt(240.0f), node.Width > 0.0f ? Px(node.Width) : Layout::Pt(Layout::Current().MinCard.x));
            const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : 0.0f, FillHeight(node.Height, Px(200.0f)));
            m_treePopup = ImGui::GetID("##context");
            ImGui::BeginChild("##tree", size, ImGuiChildFlags_Borders);
            DrawTree(node, node.Tree);
            ImGui::EndChild();
            int choice = 0;
            ContextMenu(node, "##context", choice);
            break;
        }
        default:
            DrawValue(node);
            break;
        }
        if (tooltip)
            ImGui::EndGroup();
        m_inGrid = inGrid;
        ImGui::EndDisabled();
        if (tooltip)
            ItemTooltip(node.Tooltip.c_str());
        ImGui::PopID();
    }

    void UiHost::DrawSection(Lua::UiNode& node)
    {
        const std::string title = node.Title.empty() ? node.Text : node.Title;
        if (node.Collapsible)
        {
            const std::string header = IconText(node.Icon, title.empty() ? std::string("Section") : title) + (node.Badge.empty() ? std::string() : "  ·  " + node.Badge) + "###section";
            // a script's update{ open = } folds or unfolds it; the user's folding is kept in open
            if (node.OpenChanged)
            {
                ImGui::SetNextItemOpen(node.Open, ImGuiCond_Always);
                node.OpenChanged = false;
            }
            const bool open = ImGui::CollapsingHeader(header.c_str(), node.Open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            if (!m_design)
                node.Open = open;
            if (open)
            {
                ImGui::Indent(Px(6.0f));
                DrawChildren(node, false);
                ImGui::Unindent(Px(6.0f));
            }
            return;
        }
        // ∑ a card of the layout's: padding, clipping, a title line whose
        // badge goes first when it is short, never below the card minimum
        const std::string glyph(IconGlyph(node.Icon));
        Layout::CardOptions options;
        options.Icon = glyph.empty() ? nullptr : glyph.c_str();
        options.Title = title;
        options.Badge = node.Badge;
        options.Width = node.Width > 0.0f ? Px(node.Width) : 0.0f;
        options.Height = node.Height > 0.0f ? Px(node.Height) : node.Height < 0.0f ? -FLT_MIN : 0.0f;
        Layout::Card card("##section", options);
        DrawChildren(node, false);
        card.End();
    }

    void UiHost::DrawTabs(Lua::UiNode& node)
    {
        if (m_design && node.Children.empty())
        {
            DropZone(false);
            return;
        }
        if (!ImGui::BeginTabBar("##tabs"))
            return;
        const std::vector<int> children = node.Children;
        for (const int child : children)
        {
            Lua::UiNode* tab = Model().Find(child);
            if (!tab || tab->Kind != UiKind::Tab || !tab->Visible)
                continue;
            const std::string label = IconText(tab->Icon, tab->Title.empty() ? tab->Text : tab->Title) + "###tab" + std::to_string(tab->Id);
            const ImGuiTabItemFlags tabFlags = m_design && m_design->ForceTab == tab->Id ? ImGuiTabItemFlags_SetSelected : 0;
            if (m_design && m_design->ForceTab == tab->Id)
                m_design->ForceTab = 0;
            // a tab's own options: disabled, a tooltip on its header, a scrolling page
            ImGui::BeginDisabled(!tab->Enabled);
            const bool selected = ImGui::BeginTabItem(label.c_str(), nullptr, tabFlags);
            ImGui::EndDisabled();
            if (!tab->Tooltip.empty())
                ItemTooltip(tab->Tooltip.c_str());
            if (selected)
            {
                ImGui::PushID(tab->Id);
                ImGui::BeginDisabled(!tab->Enabled);
                if (tab->Scroll)
                {
                    ImGui::BeginChild("##tabscroll", ImVec2(0.0f, FillHeight(tab->Height, Px(200.0f))), ImGuiChildFlags_None);
                    DrawChildren(*tab, false);
                    ImGui::EndChild();
                }
                else
                    DrawChildren(*tab, false);
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    void UiHost::DrawSplit(Lua::UiNode& node)
    {
        std::vector<Lua::UiNode*> parts;
        for (const int child : node.Children)
        {
            if (Lua::UiNode* item = Model().Find(child); item && item->Visible && parts.size() < 2)
                parts.push_back(item);
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float gap = Px(6.0f);
        const float height = node.Height > 0.0f ? Px(node.Height) : std::max(avail.y, Px(120.0f));
        const float width = node.Width > 0.0f ? Px(node.Width) : avail.x;
        Layout::Report(width, Layout::Pt(80.0f));
        const float total = (node.Vertical ? height : width) - gap;
        const float first = std::max(Px(20.0f), total * node.Ratio);
        const float second = std::max(Px(20.0f), total - first);
        const ImVec2 firstSize = node.Vertical ? ImVec2(width, first) : ImVec2(first, height);
        const ImVec2 secondSize = node.Vertical ? ImVec2(width, second) : ImVec2(second, height);
        ImGui::BeginChild("##first", firstSize, ImGuiChildFlags_None);
        if (!parts.empty())
            DrawNode(*parts[0]);
        ImGui::EndChild();
        if (!node.Vertical)
            ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##bar", node.Vertical ? ImVec2(width, gap) : ImVec2(gap, height));
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || active)
        {
            ImGui::SetMouseCursor(node.Vertical ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, active ? 0.6f : 0.3f)));
        }
        if (active && total > 0.0f)
        {
            const float delta = node.Vertical ? ImGui::GetIO().MouseDelta.y : ImGui::GetIO().MouseDelta.x;
            node.Ratio = std::clamp(node.Ratio + delta / total, 0.05f, 0.95f);
            if (delta != 0.0f)
                Model().Touch(node);
        }
        if (!node.Vertical)
            ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##second", secondSize, ImGuiChildFlags_None);
        if (parts.size() > 1)
            DrawNode(*parts[1]);
        ImGui::EndChild();
    }

    // ∑ A field and its label share the space they have (Layout::Field):
    // "left" draws the label in the container's label column and the field
    // fills the rest of the line, "above" puts the label over a field as wide
    // as the line, "right" (ImGui's way) leaves room for the label after the
    // field. Without a label the field fills the line. In a row the line is
    // the widget's cell. A width of the widget's own wins; minWidth and
    // maxWidth clamp the field, which never gets below the rules' minimum.
    // reserve keeps room after the field (its key caps). Returns the width.
    float UiHost::FieldLayout(const Lua::UiNode& node, const std::string& side, float reserve)
    {
        Layout::FieldOptions options;
        options.Placement = node.Label.empty() ? Layout::LabelPlacement::None
                          : side == "above"    ? Layout::LabelPlacement::Above
                          : side == "right"    ? Layout::LabelPlacement::Right
                                               : Layout::LabelPlacement::Left;
        options.LabelColumn = m_labelWidth;
        options.Width = node.Width > 0.0f ? Px(node.Width) : 0.0f;
        options.MinWidth = node.MinWidth > 0.0f ? Px(node.MinWidth) : 0.0f;
        options.MaxWidth = node.MaxWidth > 0.0f ? Px(node.MaxWidth) : 0.0f;
        options.Reserve = reserve;
        return Layout::Field(node.Label, options).Width;
    }

    void UiHost::DrawValue(Lua::UiNode& node)
    {
        const std::string side = node.LabelPosition.empty() ? m_labelPosition : node.LabelPosition;
        std::string label = Label(node, node.Label);
        float fieldWidth = 0.0f;
        const bool cycles = node.Kind == UiKind::Combo;
        if (PlacedLabel(node.Kind) && node.Kind != UiKind::Radio)
        {
            // ∑ the keys that move it sit after the field, inside its line
            float caps = 0.0f;
            for (const auto& [key, sign] : { std::pair{ &node.HotkeyLess, cycles ? "< " : "- " }, std::pair{ &node.HotkeyMore, cycles ? "> " : "+ " } })
            {
                if (!key->empty())
                    caps += Layout::KeyCapWidth(sign + *key) + ImGui::GetStyle().ItemInnerSpacing.x;
            }
            fieldWidth = FieldLayout(node, side, caps);
            m_fieldWidth = fieldWidth;
            if (side == "left" || side == "above" || node.Label.empty())
                label = Label(node, ""); // the label is drawn already
        }
        else if (node.Kind == UiKind::Checkbox && m_labelWidth > 0.0f && !m_inRow)
        {
            // a checkbox among left-labelled fields lines up with the fields
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m_labelWidth + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f);
        }
        if (node.Focus && !node.Focused && !m_design)
        {
            ImGui::SetKeyboardFocusHere();
            node.Focused = true;
        }
        switch (node.Kind)
        {
        case UiKind::Checkbox:
        {
            bool value = node.Bool;
            bool flipped = false;
            if (node.Style == "switch")
            {
                // a switch with its label beside it; a click on the label flips it too
                flipped = ToggleSwitch("##switch", &value);
                const std::size_t hidden = label.find("##");
                const std::string shown = hidden == std::string::npos ? label : label.substr(0, hidden);
                if (!shown.empty())
                {
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    Layout::TextStyle text;
                    text.FrameLine = true;
                    text.Tooltip = node.Tooltip.empty();
                    Layout::Text(shown, text);
                    if (ImGui::IsItemClicked())
                    {
                        value = !value;
                        flipped = true;
                    }
                }
            }
            else
                flipped = ImGui::Checkbox(label.c_str(), &value);
            if (!node.Hotkey.empty())
            {
                // ∑ the key beside the box, like a trainer shows it
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x * 2.0f);
                const std::string hint = "Hotkey " + node.Hotkey + ": works from anywhere while this window is open";
                Layout::KeyCap(node.Hotkey, node.Tooltip.empty() ? hint.c_str() : nullptr);
            }
            if (flipped)
            {
                node.Bool = value;
                Model().Touch(node);
                if (node.Handlers & H::Change)
                    Post(node.Id, "change", { value });
            }
            break;
        }
        case UiKind::Input:
        {
            const std::string before = node.String;
            ImGuiInputTextFlags flags = node.Password ? ImGuiInputTextFlags_Password : 0;
            if (node.ReadOnly)
                flags |= ImGuiInputTextFlags_ReadOnly;
            bool entered = false;
            if (node.Multiline)
                InputMultiline(label.c_str(), node.String, ImVec2(fieldWidth > 0.0f ? fieldWidth : -FLT_MIN, FillHeight(node.Height, Px(80.0f))), flags);
            else
            {
                flags |= ImGuiInputTextFlags_EnterReturnsTrue;
                entered = node.Hint.empty() ? InputText(label.c_str(), node.String, flags) : InputTextWithHint(label.c_str(), node.Hint.c_str(), node.String, flags);
            }
            if (ImGui::IsItemActive())
            {
                node.Active = true;
                m_activeField = node.Id;
            }
            if (node.MaxLength > 0 && node.String != before)
            {
                std::size_t at = 0;
                int count = 0;
                while (at < node.String.size() && count < node.MaxLength)
                {
                    const unsigned char lead = static_cast<unsigned char>(node.String[at]);
                    at += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                    ++count;
                }
                if (at < node.String.size())
                    node.String.resize(at);
            }
            if (node.String != before)
            {
                Model().Touch(node);
                if (node.Handlers & H::Change)
                    Post(node.Id, "change", { node.String });
            }
            if (entered && (node.Handlers & H::Submit))
                Post(node.Id, "submit", { node.String });
            break;
        }
        case UiKind::Number:
        {
            // ∑ typing 250 must not report 2 and 25 first (a bound record would
            // write each into the game): keystrokes wait until the field is
            // left or Enter is pressed; the +/- buttons report at once
            const bool stepped = node.Integer || node.Hex || node.Step > 0.0;
            ImGuiID textId = 0;
            if (stepped)
            {
                ImGui::PushID(label.c_str());
                textId = ImGui::GetID("");
                ImGui::PopID();
            }
            else
                textId = ImGui::GetID(label.c_str());
            bool changed = false;
            if (node.Integer || node.Hex)
            {
                std::int64_t value = Lua::UiToInteger(node.Number);
                const std::int64_t step = node.Step > 0.0 ? std::clamp<std::int64_t>(Lua::UiToInteger(std::round(node.Step)), 1, 1000000000000000) : 1; // fast = step * 10 stays in range
                const std::int64_t fast = step * 10;
                changed = ImGui::InputScalar(label.c_str(), ImGuiDataType_S64, &value, &step, &fast, node.Hex ? "%llX" : "%lld", node.Hex ? ImGuiInputTextFlags_CharsHexadecimal : 0);
                if (changed)
                    node.Number = static_cast<double>(value);
            }
            else
            {
                double value = node.Number;
                const double step = node.Step > 0.0 ? node.Step : 0.0;
                changed = ImGui::InputDouble(label.c_str(), &value, step, step * 10.0, node.Format.empty() ? "%g" : node.Format.c_str());
                if (changed)
                    node.Number = value;
            }
            const bool typing = ImGui::GetActiveID() == textId && textId != 0;
            if (typing)
            {
                node.Active = true;
                m_activeField = node.Id;
            }
            if (changed)
            {
                node.Number = Lua::UiClampValue(node, node.Number);
                Model().Touch(node);
                if (typing)
                    node.PendingChange = true;
                else if (node.Handlers & H::Change)
                    Post(node.Id, "change", { node.Number });
            }
            if (!typing && node.PendingChange)
            {
                node.PendingChange = false;
                if (node.Handlers & H::Change)
                    Post(node.Id, "change", { node.Number });
            }
            break;
        }
        case UiKind::Slider:
        {
            bool changed = false;
            const auto [lowLimit, highLimit] = Lua::UiValueRange(node); // 0..100 or 0..1 without min and max
            if (node.Integer)
            {
                std::int64_t value = Lua::UiToInteger(node.Number);
                const std::int64_t low = Lua::UiToInteger(lowLimit);
                const std::int64_t high = Lua::UiToInteger(highLimit);
                const std::string format = IntegerFormat(node.Format);
                changed = ImGui::SliderScalar(label.c_str(), ImGuiDataType_S64, &value, &low, &high, format.c_str(), ImGuiSliderFlags_AlwaysClamp);
                if (changed)
                    node.Number = Lua::UiSnapValue(node, static_cast<double>(value));
            }
            else
            {
                double value = node.Number;
                const double low = lowLimit;
                const double high = highLimit;
                changed = ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &value, &low, &high, node.Format.empty() ? "%.2f" : node.Format.c_str(), ImGuiSliderFlags_AlwaysClamp);
                if (changed)
                    node.Number = Lua::UiSnapValue(node, value);
            }
            if (changed)
            {
                Model().Touch(node);
                if (node.Handlers & H::Change)
                    Post(node.Id, "change", { node.Number });
            }
            break;
        }
        case UiKind::Combo:
        {
            if (node.Multi)
            {
                // ∑ several choices: a box per item, the list stays open while
                // ticking; the preview names the chosen ones
                std::string preview;
                for (const int item : node.Chosen)
                {
                    if (item >= 1 && item <= static_cast<int>(node.Items.size()))
                        preview += (preview.empty() ? "" : ", ") + node.Items[static_cast<std::size_t>(item - 1)];
                }
                if (ImGui::BeginCombo(label.c_str(), preview.empty() ? (node.Hint.empty() ? "None" : node.Hint.c_str()) : preview.c_str()))
                {
                    for (int i = 0; i < static_cast<int>(node.Items.size()); ++i)
                    {
                        ImGui::PushID(i);
                        bool on = std::find(node.Chosen.begin(), node.Chosen.end(), i + 1) != node.Chosen.end();
                        if (ImGui::Checkbox(node.Items[static_cast<std::size_t>(i)].c_str(), &on))
                        {
                            if (on)
                                node.Chosen.push_back(i + 1);
                            else
                                std::erase(node.Chosen, i + 1);
                            std::sort(node.Chosen.begin(), node.Chosen.end());
                            Model().Touch(node);
                            if (node.Handlers & H::Change)
                            {
                                // the list travels as text; the Lua side turns it into a table
                                std::string list;
                                for (const int item : node.Chosen)
                                    list += (list.empty() ? "" : ",") + std::to_string(item);
                                Post(node.Id, "change", { list });
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            const char* preview = node.Selected > 0 && node.Selected <= static_cast<int>(node.Items.size()) ? node.Items[static_cast<std::size_t>(node.Selected - 1)].c_str() : node.Hint.c_str();
            if (ImGui::BeginCombo(label.c_str(), preview))
            {
                for (int i = 0; i < static_cast<int>(node.Items.size()); ++i)
                {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(node.Items[static_cast<std::size_t>(i)].c_str(), node.Selected == i + 1))
                    {
                        node.Selected = i + 1;
                        Model().Touch(node);
                        if (node.Handlers & H::Change)
                            Post(node.Id, "change", { static_cast<double>(i + 1) });
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case UiKind::Radio:
        {
            const bool left = !node.Label.empty() && side == "left";
            if (left)
            {
                const float start = ImGui::GetCursorPosX();
                const float column = std::min(std::max(m_labelWidth, ImGui::CalcTextSize(node.Label.c_str()).x), std::floor(Layout::Avail() * Layout::Current().LabelColumnShare));
                Layout::TextStyle text;
                text.MaxWidth = std::max(1.0f, column);
                text.FrameLine = true;
                Layout::Text(node.Label, text);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::SetCursorPosX(start + column + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f);
                ImGui::BeginGroup();
            }
            else if (!node.Label.empty() && (side == "above" || !node.Horizontal))
                Layout::Text(node.Label);
            for (int i = 0; i < static_cast<int>(node.Items.size()); ++i)
            {
                ImGui::PushID(i);
                if (node.Horizontal && i > 0)
                    ImGui::SameLine();
                if (ImGui::RadioButton(node.Items[static_cast<std::size_t>(i)].c_str(), node.Selected == i + 1) && node.Selected != i + 1)
                {
                    node.Selected = i + 1;
                    Model().Touch(node);
                    if (node.Handlers & H::Change)
                        Post(node.Id, "change", { static_cast<double>(i + 1) });
                }
                ImGui::PopID();
            }
            if (left)
                ImGui::EndGroup();
            else if (node.Horizontal && !node.Label.empty() && side == "right")
            {
                ImGui::SameLine();
                Layout::Text(node.Label);
            }
            break;
        }
        case UiKind::Color:
        {
            const ImVec4 current = Color(node.String, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            float channels[4] = { current.x, current.y, current.z, current.w };
            const ImGuiColorEditFlags flags = node.Alpha ? ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf : 0;
            const bool changed = node.Alpha ? ImGui::ColorEdit4(label.c_str(), channels, flags) : ImGui::ColorEdit3(label.c_str(), channels, flags);
            if (changed)
            {
                const auto byte = [](float value) { return static_cast<unsigned>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
                char text[16];
                if (node.Alpha)
                    std::snprintf(text, sizeof(text), "#%02X%02X%02X%02X", byte(channels[0]), byte(channels[1]), byte(channels[2]), byte(channels[3]));
                else
                    std::snprintf(text, sizeof(text), "#%02X%02X%02X", byte(channels[0]), byte(channels[1]), byte(channels[2]));
                node.String = text;
                Model().Touch(node);
                if (node.Handlers & H::Change)
                    Post(node.Id, "change", { node.String });
            }
            break;
        }
        default:
            Layout::Text(node.Text);
            break;
        }
        if (node.Kind != UiKind::Radio && (!node.HotkeyLess.empty() || !node.HotkeyMore.empty()))
        {
            // the field reported them with itself (its reserve)
            Layout::QuietReports quiet;
            const std::pair<const std::string*, const char*> caps[] = { { &node.HotkeyLess, cycles ? "< " : "- " }, { &node.HotkeyMore, cycles ? "> " : "+ " } };
            for (const auto& [key, sign] : caps)
            {
                if (key->empty())
                    continue;
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                const std::string hint = std::string(key == &node.HotkeyMore ? (cycles ? "Next item: " : "Up: ") : (cycles ? "Previous item: " : "Down: ")) + *key + ", from anywhere while this window is open";
                Layout::KeyCap(sign + *key, node.Tooltip.empty() ? hint.c_str() : nullptr);
            }
        }
    }

    bool UiHost::ContextMenu(Lua::UiNode& node, const char* id, int& choice)
    {
        choice = 0;
        if (!ImGui::BeginPopup(id))
            return false;
        for (int i = 0; i < static_cast<int>(node.Menu.size()); ++i)
        {
            const std::string& text = node.Menu[static_cast<std::size_t>(i)];
            if (text == "-")
            {
                ImGui::Separator();
                continue;
            }
            ImGui::PushID(i);
            if (ImGui::MenuItem(text.c_str()))
                choice = i + 1;
            ImGui::PopID();
        }
        ImGui::EndPopup();
        if (choice && g_context.Node == node.Id && (node.Handlers & H::Menu))
        {
            const std::string& text = node.Menu[static_cast<std::size_t>(choice - 1)];
            if (node.Kind == UiKind::Table)
                Post(node.Id, "menu", { static_cast<double>(choice), text, static_cast<double>(g_context.Row) });
            else if (node.Kind == UiKind::Tree)
                Post(node.Id, "menu", { static_cast<double>(choice), text, g_context.Item });
            else
                Post(node.Id, "menu", { static_cast<double>(choice), text, static_cast<double>(g_context.X), static_cast<double>(g_context.Y) });
        }
        return true;
    }

    void UiHost::DrawTable(Lua::UiNode& node)
    {
        std::size_t columns = node.Columns.size();
        for (const auto& row : node.Rows)
            columns = std::max(columns, row.size());
        columns = std::clamp<std::size_t>(columns, 1, 64);
        Layout::Report(node.Width > 0.0f ? Px(node.Width) : Layout::Pt(240.0f), node.Width > 0.0f ? Px(node.Width) : Layout::Pt(Layout::Current().MinCard.x));
        const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : 0.0f, FillHeight(node.Height, Px(200.0f)));
        const ImGuiID popup = ImGui::GetID("##context");
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##table", static_cast<int>(columns), flags, size))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            bool titled = false;
            for (std::size_t c = 0; c < columns; ++c)
            {
                const Lua::UiColumn column = c < node.Columns.size() ? node.Columns[c] : Lua::UiColumn{};
                titled = titled || !column.Title.empty();
                ImGui::TableSetupColumn(column.Title.c_str(), column.Width > 0.0f ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch, column.Width > 0.0f ? Px(column.Width) : 0.0f);
            }
            if (titled)
                ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(node.Rows.size()));
            while (clipper.Step())
            {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
                {
                    const std::vector<std::string>& row = node.Rows[static_cast<std::size_t>(r)];
                    ImGui::TableNextRow();
                    for (std::size_t c = 0; c < columns; ++c)
                    {
                        ImGui::TableSetColumnIndex(static_cast<int>(c));
                        const std::string& cell = c < row.size() ? row[c] : std::string();
                        // ∑ a cell's text ends in "..." in a column too narrow for it
                        Layout::TextStyle text;
                        text.Justify = c < node.Columns.size() ? AlignOf(node.Columns[c].Align) : Layout::Align::Start;
                        if (c == 0)
                        {
                            ImGui::PushID(r);
                            const bool selected = node.Selected == r + 1;
                            const bool clicked = Layout::RowSelectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick);
                            const bool rightClicked = !node.Menu.empty() && ImGui::IsItemClicked(ImGuiMouseButton_Right);
                            if (clicked)
                            {
                                if (node.Selected != r + 1)
                                {
                                    node.Selected = r + 1;
                                    Model().Touch(node);
                                    if (node.Handlers & H::Select)
                                        Post(node.Id, "select", { static_cast<double>(r + 1) });
                                }
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && (node.Handlers & H::Activate))
                                    Post(node.Id, "activate", { static_cast<double>(r + 1) });
                            }
                            if (rightClicked)
                            {
                                node.Selected = r + 1;
                                g_context = { node.Id, r + 1, {}, 0.0f, 0.0f };
                                ImGui::OpenPopup(popup);
                            }
                            Layout::Text(cell, text);
                            ImGui::PopID();
                        }
                        else
                            Layout::Text(cell, text);
                    }
                }
            }
            ImGui::EndTable();
        }
        int choice = 0;
        ContextMenu(node, "##context", choice);
    }

    void UiHost::DrawTree(Lua::UiNode& node, const std::vector<Lua::UiTreeItem>& items)
    {
        for (const Lua::UiTreeItem& item : items)
        {
            ImGui::PushID(item.Id.c_str());
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (item.Children.empty())
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (item.Id == node.SelectedId)
                flags |= ImGuiTreeNodeFlags_Selected;
            if (item.Open)
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            const bool open = Layout::TreeNode("##item", IconText(item.Icon, item.Text), flags);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
            {
                if (node.SelectedId != item.Id)
                {
                    node.SelectedId = item.Id;
                    Model().Touch(node);
                    if (node.Handlers & H::Select)
                        Post(node.Id, "select", { item.Id });
                }
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && (node.Handlers & H::Activate))
                    Post(node.Id, "activate", { item.Id });
            }
            if (!node.Menu.empty() && ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                node.SelectedId = item.Id;
                g_context = { node.Id, 0, item.Id, 0.0f, 0.0f };
                ImGui::OpenPopup(m_treePopup);
            }
            if (open && !item.Children.empty())
            {
                DrawTree(node, item.Children);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    void UiHost::DrawCanvas(Lua::UiNode& node)
    {
        CanvasState& state = m_canvases[node.Id];
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        Layout::Report(node.Width > 0.0f ? Px(node.Width) : Layout::Pt(160.0f), node.Width > 0.0f ? Px(node.Width) : Layout::Pt(40.0f));
        const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : std::max(avail.x, Px(40.0f)), FillHeight(node.Height, Px(200.0f)));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImGuiID popup = ImGui::GetID("##context");
        ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(origin.x + size.x, origin.y + size.y);
        draw->PushClipRect(origin, end, true);
        if (!node.Background.empty())
            draw->AddRectFilled(origin, end, ImGui::GetColorU32(Color(node.Background, ManifoldTheme::Surface)));
        const float unit = Px(1.0f) * node.Scale;
        const auto screen = [&](float x, float y) { return ImVec2(origin.x + (x - node.ViewX) * unit, origin.y + (y - node.ViewY) * unit); };
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouseCanvas((io.MousePos.x - origin.x) / unit + node.ViewX, (io.MousePos.y - origin.y) / unit + node.ViewY);
        std::vector<ImVec2> points;
        for (const Lua::UiDraw& item : node.Draws)
        {
            const ImU32 color = ImGui::GetColorU32(Color(item.Color, ManifoldTheme::Text));
            const float thickness = std::max(0.5f, item.Thickness) * Px(1.0f);
            switch (item.Kind)
            {
            case Lua::UiDrawKind::Line:
                draw->AddLine(screen(item.X, item.Y), screen(item.X2, item.Y2), color, thickness);
                break;
            case Lua::UiDrawKind::Rect:
                if (item.Filled)
                    draw->AddRectFilled(screen(item.X, item.Y), screen(item.X2, item.Y2), color, item.Rounding * unit);
                else
                    draw->AddRect(screen(item.X, item.Y), screen(item.X2, item.Y2), color, item.Rounding * unit, 0, thickness);
                break;
            case Lua::UiDrawKind::Circle:
                if (item.Filled)
                    draw->AddCircleFilled(screen(item.X, item.Y), item.Radius * unit, color);
                else
                    draw->AddCircle(screen(item.X, item.Y), item.Radius * unit, color, 0, thickness);
                break;
            case Lua::UiDrawKind::Polygon:
                points.clear();
                for (std::size_t i = 0; i + 1 < item.Points.size(); i += 2)
                    points.push_back(screen(item.Points[i], item.Points[i + 1]));
                if (points.size() >= 3)
                {
                    if (item.Filled)
                        draw->AddConcavePolyFilled(points.data(), static_cast<int>(points.size()), color);
                    else
                        draw->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_Closed, thickness);
                }
                break;
            case Lua::UiDrawKind::Text:
            {
                const float fontSize = (item.Size > 0.0f ? item.Size * Px(1.0f) : ImGui::GetFontSize());
                draw->AddText(ImGui::GetFont(), fontSize, screen(item.X, item.Y), color, item.Text.c_str());
                break;
            }
            case Lua::UiDrawKind::Image:
            {
                int width = 0;
                int height = 0;
                if (void* texture = TextureOf(item.Image, width, height))
                {
                    const float x2 = item.X2 > item.X ? item.X2 : item.X + static_cast<float>(width);
                    const float y2 = item.Y2 > item.Y ? item.Y2 : item.Y + static_cast<float>(height);
                    draw->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture))), screen(item.X, item.Y), screen(x2, y2));
                }
                break;
            }
            }
        }
        draw->PopClipRect();

        bool viewChanged = node.ViewChanged;
        node.ViewChanged = false;
        if (node.Zoom && hovered && io.MouseWheel != 0.0f)
        {
            const ImVec2 before = mouseCanvas;
            node.Scale = std::clamp(node.Scale * std::pow(1.15f, io.MouseWheel), 0.05f, 64.0f);
            const float scaled = Px(1.0f) * node.Scale;
            node.ViewX = before.x - (io.MousePos.x - origin.x) / scaled;
            node.ViewY = before.y - (io.MousePos.y - origin.y) / scaled;
            viewChanged = true;
        }
        if (ImGui::IsItemActivated())
        {
            state.Press = io.MousePos;
            state.Moved = false;
        }
        if (active)
        {
            const float dx = io.MousePos.x - state.Press.x;
            const float dy = io.MousePos.y - state.Press.y;
            if (dx * dx + dy * dy > Px(3.0f) * Px(3.0f))
                state.Moved = true;
            if (node.Zoom && state.Moved && (io.MouseDown[ImGuiMouseButton_Left] || io.MouseDown[ImGuiMouseButton_Middle]) && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
            {
                node.ViewX -= io.MouseDelta.x / unit;
                node.ViewY -= io.MouseDelta.y / unit;
                viewChanged = true;
            }
        }
        if (hovered && !state.Moved)
        {
            for (int button = 0; button < 3; ++button)
            {
                if (!ImGui::IsMouseReleased(button))
                    continue;
                if (button == ImGuiMouseButton_Right && !node.Menu.empty())
                {
                    g_context = { node.Id, 0, {}, mouseCanvas.x, mouseCanvas.y };
                    ImGui::OpenPopup(popup);
                }
                else if (node.Handlers & H::Click)
                    Post(node.Id, "click", { static_cast<double>(mouseCanvas.x), static_cast<double>(mouseCanvas.y), static_cast<double>(button + 1) });
            }
        }
        if (hovered && (node.Handlers & H::Hover) && (io.MousePos.x != state.LastHover.x || io.MousePos.y != state.LastHover.y))
        {
            state.LastHover = io.MousePos;
            Post(node.Id, "hover", { static_cast<double>(mouseCanvas.x), static_cast<double>(mouseCanvas.y) });
        }
        if (viewChanged && (node.Handlers & H::View))
            Post(node.Id, "view", { static_cast<double>(node.ViewX), static_cast<double>(node.ViewY), static_cast<double>(node.Scale) });
        if ((node.Handlers & H::Key) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput)
        {
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart; ++key)
            {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), true))
                    Post(node.Id, "key", { std::string(ImGui::GetKeyName(static_cast<ImGuiKey>(key))) });
            }
        }
        int choice = 0;
        ContextMenu(node, "##context", choice);
    }

    void UiHost::DrawImage(const Lua::UiNode& node)
    {
        int width = 0;
        int height = 0;
        void* texture = TextureOf(node.ImageId, width, height);
        const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : Px(static_cast<float>(width)), node.Height > 0.0f ? Px(node.Height) : Px(static_cast<float>(height)));
        if (!texture)
        {
            ImGui::Dummy(ImVec2(std::max(size.x, Px(16.0f)), std::max(size.y, Px(16.0f))));
            return;
        }
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture))), size);
    }

    void* UiHost::TextureOf(int image, int& width, int& height)
    {
        const Lua::UiImage* source = Model().FindImage(image);
        if (const auto found = m_textures.find(image); found != m_textures.end())
        {
            if (source && source->Path == found->second.Path && source->Width == found->second.Width && source->Height == found->second.Height)
            {
                width = found->second.Width;
                height = found->second.Height;
                return found->second.View;
            }
            if (found->second.View)
                static_cast<ID3D11ShaderResourceView*>(found->second.View)->Release();
            m_textures.erase(found);
        }
        ID3D11Device* device = static_cast<ID3D11Device*>(m_shell.Device());
        if (!source || !device || source->Rgba.empty())
            return nullptr;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(source->Width);
        description.Height = static_cast<UINT>(source->Height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = source->Rgba.data();
        data.SysMemPitch = description.Width * 4;
        ID3D11Texture2D* texture = nullptr;
        if (FAILED(device->CreateTexture2D(&description, &data, &texture)) || !texture)
            return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView* resource = nullptr;
        const HRESULT made = device->CreateShaderResourceView(texture, &view, &resource);
        texture->Release();
        if (FAILED(made) || !resource)
            return nullptr;
        m_textures[image] = { resource, source->Width, source->Height, source->Path };
        width = source->Width;
        height = source->Height;
        return resource;
    }

    void UiHost::PruneTextures()
    {
        const std::uint64_t generation = Model().Generation();
        if (generation == m_textureGeneration)
            return;
        m_textureGeneration = generation;
        for (auto it = m_textures.begin(); it != m_textures.end();)
        {
            if (Model().FindImage(it->first))
            {
                ++it;
                continue;
            }
            if (it->second.View)
                static_cast<ID3D11ShaderResourceView*>(it->second.View)->Release();
            it = m_textures.erase(it);
        }
        for (auto it = m_canvases.begin(); it != m_canvases.end();)
            it = Model().Find(it->first) ? std::next(it) : m_canvases.erase(it);
    }

    void UiHost::DrawDialogs()
    {
        const std::vector<Lua::UiDialog>& dialogs = Model().Dialogs();
        if (dialogs.empty())
        {
            m_openDialog = 0;
            return;
        }
        const Lua::UiDialog dialog = dialogs.front();
        const auto finish = [&](std::vector<Lua::UiValue> values)
            {
                Post(dialog.Id, "dialog", std::move(values));
                Model().RemoveDialog(dialog.Id);
                m_openDialog = 0;
            };
        if (dialog.Kind == Lua::UiDialogKind::OpenFile || dialog.Kind == Lua::UiDialogKind::SaveFile)
        {
            const std::optional<std::string> path = FileDialog(dialog, m_shell.NativeWindow());
            finish({ path ? Lua::UiValue(*path) : Lua::UiValue(std::monostate{}) });
            return;
        }
        const std::string id = "manifold.ui.dialog" + std::to_string(dialog.Id);
        if (m_openDialog != dialog.Id)
        {
            m_openDialog = dialog.Id;
            m_dialogText = dialog.Value;
            m_dialogChoice = 0;
        }
        if (!ImGui::IsPopupOpen(id.c_str()))
            ImGui::OpenPopup(id.c_str());
        const char* icon = dialog.Kind == Lua::UiDialogKind::Input ? ICON_FA_KEYBOARD : dialog.Kind == Lua::UiDialogKind::Choose ? ICON_FA_LIST : ICON_FA_QUESTION_CIRCLE;
        if (!BeginModal(id.c_str(), icon, dialog.Title.empty() ? "Script" : dialog.Title.c_str(), ImVec2(420.0f, 0.0f)))
            return;
        if (!dialog.Text.empty())
        {
            Layout::TextStyle text;
            text.Mode = Layout::Overflow::Wrap;
            text.MaxWidth = Px(400.0f);
            Layout::Text(dialog.Text, text);
            ImGui::Spacing();
        }
        bool accept = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        if (dialog.Kind == Lua::UiDialogKind::Input)
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            InputText("##value", m_dialogText);
        }
        else if (dialog.Kind == Lua::UiDialogKind::Choose)
        {
            if (ImGui::BeginListBox("##choices", ImVec2(-FLT_MIN, Px(180.0f))))
            {
                for (int i = 0; i < static_cast<int>(dialog.Items.size()); ++i)
                {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(dialog.Items[static_cast<std::size_t>(i)].c_str(), m_dialogChoice == i + 1, ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        m_dialogChoice = i + 1;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            accept = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
        }
        ImGui::Spacing();
        const char* yes = dialog.Kind == Lua::UiDialogKind::Confirm ? "Yes" : "OK";
        const char* no = dialog.Kind == Lua::UiDialogKind::Confirm ? "No" : "Cancel";
        bool pressedYes = accept;
        bool pressedNo = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        {
            Layout::Row buttons("##answer", { Layout::Size::Fit().Min(100.0f), Layout::Size::Fit().Min(100.0f) });
            Layout::ButtonStyle wide;
            wide.Width = Layout::Size::Fill();
            buttons.Cell([&] {
                wide.Kind = dialog.Danger ? Layout::ButtonKind::Danger : Layout::ButtonKind::Accent;
                pressedYes = Layout::Button(yes, wide) || pressedYes;
            });
            buttons.Cell([&] {
                wide.Kind = Layout::ButtonKind::Normal;
                pressedNo = Layout::Button(no, wide) || pressedNo;
            });
        }
        if (pressedYes && !(dialog.Kind == Lua::UiDialogKind::Choose && m_dialogChoice == 0))
        {
            ImGui::CloseCurrentPopup();
            if (dialog.Kind == Lua::UiDialogKind::Confirm)
                finish({ true });
            else if (dialog.Kind == Lua::UiDialogKind::Input)
                finish({ m_dialogText });
            else
                finish({ static_cast<double>(m_dialogChoice), dialog.Items[static_cast<std::size_t>(m_dialogChoice - 1)] });
        }
        else if (pressedNo)
        {
            ImGui::CloseCurrentPopup();
            if (dialog.Kind == Lua::UiDialogKind::Confirm)
                finish({ false });
            else
                finish({ std::monostate{} });
        }
        EndModal();
    }

    bool UiHost::HasMenu()
    {
        return !Model().Slots(Lua::UiSlotKind::Menu).empty();
    }

    void UiHost::MenuItems(const std::vector<Lua::UiSlot>& slots, const std::string& prefix)
    {
        std::vector<std::string> groups;
        for (const Lua::UiSlot& slot : slots)
        {
            if (slot.Path.rfind(prefix, 0) != 0)
                continue;
            const std::string rest = slot.Path.substr(prefix.size());
            const std::size_t slash = rest.find('/');
            if (slash == std::string::npos)
            {
                if (rest == "-")
                {
                    ImGui::Separator();
                    continue;
                }
                const std::string label = IconText(slot.Icon, slot.Text) + "##slot" + std::to_string(slot.Id);
                if (ImGui::MenuItem(label.c_str(), nullptr, slot.Checkable && slot.Checked, slot.Enabled))
                    Post(slot.Id, "click");
                continue;
            }
            const std::string group = rest.substr(0, slash);
            if (std::find(groups.begin(), groups.end(), group) != groups.end())
                continue;
            groups.push_back(group);
            if (ImGui::BeginMenu(group.c_str()))
            {
                MenuItems(slots, prefix + group + "/");
                ImGui::EndMenu();
            }
        }
    }

    void UiHost::DrawMenuItems()
    {
        const std::vector<Lua::UiSlot> slots = Model().Slots(Lua::UiSlotKind::Menu);
        if (!slots.empty())
            MenuItems(slots, "");
    }

    float UiHost::ToolbarWidth()
    {
        float width = 0.0f;
        const ImGuiStyle& style = ImGui::GetStyle();
        for (const Lua::UiSlot& slot : Model().Slots(Lua::UiSlotKind::Toolbar))
        {
            if (width > 0.0f)
                width += style.ItemSpacing.x;
            width += ImGui::CalcTextSize(IconText(slot.Icon, slot.Text).c_str()).x + style.FramePadding.x * 2.0f;
        }
        return width;
    }

    void UiHost::DrawToolbar(float right, float y)
    {
        const std::vector<Lua::UiSlot> slots = Model().Slots(Lua::UiSlotKind::Toolbar);
        if (slots.empty())
            return;
        float x = right - ToolbarWidth();
        const ImGuiStyle& style = ImGui::GetStyle();
        for (const Lua::UiSlot& slot : slots)
        {
            const std::string text = IconText(slot.Icon, slot.Text);
            ImGui::SetCursorPos(ImVec2(x, y));
            ImGui::BeginDisabled(!slot.Enabled);
            if (Layout::Button(text + "##slot" + std::to_string(slot.Id)))
                Post(slot.Id, "click");
            ImGui::EndDisabled();
            if (!slot.Tooltip.empty())
                ItemTooltip(slot.Tooltip.c_str());
            x += ImGui::CalcTextSize(text.c_str()).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
        }
    }

    float UiHost::StatusWidth(float gap)
    {
        float width = 0.0f;
        for (const Lua::UiSlot& slot : Model().Slots(Lua::UiSlotKind::Status))
            width += ImGui::CalcTextSize(IconText(slot.Icon, slot.Text).c_str()).x + gap;
        return width;
    }

    void UiHost::DrawStatus(float& x, float y, float gap)
    {
        for (const Lua::UiSlot& slot : Model().Slots(Lua::UiSlotKind::Status))
        {
            const std::string text = IconText(slot.Icon, slot.Text);
            ImGui::SetCursorPos(ImVec2(x, y));
            Layout::Colored(text, Color(slot.Color, ManifoldTheme::Muted), Layout::Overflow::Visible);
            if (ImGui::IsItemClicked())
                Post(slot.Id, "click");
            if (!slot.Tooltip.empty())
                ItemTooltip(slot.Tooltip.c_str());
            x += ImGui::CalcTextSize(text.c_str()).x + gap;
        }
    }
}
