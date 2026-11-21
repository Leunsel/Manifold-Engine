#include "UI/UiHost.h"

#include "App/Commands.h"
#include "App/Engine.h"
#include "Core/IconNames.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

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
        for (const int id : model.Windows())
        {
            if (Lua::UiNode* node = model.Find(id); node && node->Open)
                DrawWindow(*node);
        }
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
        ImGuiWindowFlags flags = window.Dock && !window.Topmost ? 0 : ImGuiWindowFlags_NoDocking;
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
        const ImVec2 minimum(window.MinWidth > 0.0f ? window.MinWidth : 160.0f, window.MinHeight > 0.0f ? window.MinHeight : 100.0f);
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
            ImGui::PushID(id);
            DrawChildren(window, false);
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
        DrawChildren(*node, false);
        ImGui::PopID();
        if (picking)
        {
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
        }
        m_design = outer;
        PruneTextures();
    }

    void UiHost::DrawChildren(const Lua::UiNode& node, bool row)
    {
        bool first = true;
        const bool gap = node.Gap >= 0.0f;
        if (gap && !row)
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Px(node.Gap)));
        const std::vector<int> children = node.Children;
        for (const int child : children)
        {
            Lua::UiNode* item = Model().Find(child);
            if (!item || (!item->Visible && !m_design))
                continue;
            if (row && !first)
                ImGui::SameLine(0.0f, gap ? Px(node.Gap) : -1.0f);
            first = false;
            DrawNode(*item);
        }
        if (gap && !row)
            ImGui::PopStyleVar();
        if (first && m_design && node.Kind != UiKind::Window)
            DropZone(row);
    }

    void UiHost::DropZone(bool row)
    {
        const float width = row ? Px(160.0f) : std::max(Px(120.0f), ImGui::GetContentRegionAvail().x);
        ImGui::Dummy(ImVec2(width, Px(34.0f)));
    }

    void UiHost::DrawRow(Lua::UiNode& node)
    {
        if (node.Align == "right" || node.Align == "center")
        {
            const auto known = m_rowWidths.find(node.Id);
            const float free = ImGui::GetContentRegionAvail().x - (known == m_rowWidths.end() ? 0.0f : known->second);
            if (free > 0.0f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (node.Align == "right" ? free : free * 0.5f));
        }
        ImGui::BeginGroup();
        DrawChildren(node, true);
        ImGui::EndGroup();
        m_rowWidths[node.Id] = ImGui::GetItemRectSize().x;
    }

    void UiHost::DrawGrid(Lua::UiNode& node)
    {
        const int columns = std::clamp(node.GridColumns, 1, 16);
        const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : 0.0f, 0.0f);
        if (!ImGui::BeginTable("##grid", columns, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX, size))
            return;
        for (int c = 0; c < columns; ++c)
            ImGui::TableSetupColumn(nullptr, c == 0 && columns > 1 ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch);
        const bool outer = m_inGrid;
        if (m_design && node.Children.empty())
        {
            ImGui::TableNextColumn();
            DropZone(false);
        }
        const std::vector<int> children = node.Children;
        for (const int child : children)
        {
            Lua::UiNode* item = Model().Find(child);
            if (!item || (!item->Visible && !m_design))
                continue;
            ImGui::TableNextColumn();
            m_inGrid = true;
            DrawNode(*item);
        }
        m_inGrid = outer;
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
        ImGui::BeginGroup();
        DrawNodeBody(node);
        ImGui::EndGroup();
        --m_depth;
        if (dim)
            ImGui::PopStyleVar();
        m_design->Rects.push_back({ node.Id, m_depth, ImGui::GetItemRectMin(), ImGui::GetItemRectMax() });
    }

    void UiHost::DrawNodeBody(Lua::UiNode& node)
    {
        ImGui::PushID(node.Id);
        ImGui::BeginDisabled(!node.Enabled);
        const bool inGrid = m_inGrid;
        m_cell = inGrid;
        m_inGrid = false;
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
            if (node.Scroll)
            {
                const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : 0.0f, node.Height > 0.0f ? Px(node.Height) : Px(200.0f));
                ImGui::BeginChild("##scroll", size, ImGuiChildFlags_None);
                DrawChildren(node, false);
                ImGui::EndChild();
                break;
            }
            ImGui::BeginGroup();
            DrawChildren(node, false);
            ImGui::EndGroup();
            break;
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
        {
            const ImVec4 color = Color(node.Color, ManifoldTheme::Text);
            if (m_cell)
                ImGui::AlignTextToFramePadding();
            if (node.Mono)
                PushMono();
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (node.MaxWidth > 0.0f)
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(node.MaxWidth));
                ImGui::TextUnformatted(node.Text.c_str());
                ImGui::PopTextWrapPos();
            }
            else if (node.Wrap)
                ImGui::TextWrapped("%s", node.Text.c_str());
            else
                ImGui::TextUnformatted(node.Text.c_str());
            ImGui::PopStyleColor();
            if (node.Mono)
                PopMono();
            break;
        }
        case UiKind::Heading:
            if (m_cell)
                ImGui::AlignTextToFramePadding();
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::TextUnformatted(IconText(node.Icon, node.Text).c_str());
            ImGui::PopFont();
            break;
        case UiKind::Separator:
            if (node.Text.empty())
                ImGui::Separator();
            else
                ImGui::SeparatorText(node.Text.c_str());
            break;
        case UiKind::Spacer:
            ImGui::Dummy(ImVec2(Px(node.Width > 0.0f ? node.Width : 1.0f), Px(node.Height > 0.0f ? node.Height : 8.0f)));
            break;
        case UiKind::Button:
        {
            const std::string label = Label(node, IconText(node.Icon, node.Text.empty() ? std::string("Button") : node.Text));
            ImVec2 size(node.Width > 0.0f ? Px(node.Width) : (node.Width < 0.0f || m_cell ? -FLT_MIN : 0.0f), node.Height > 0.0f ? Px(node.Height) : 0.0f);
            if (node.MinWidth > 0.0f || node.MaxWidth > 0.0f)
            {
                const ImGuiStyle& style = ImGui::GetStyle();
                float width = size.x > 0.0f ? size.x : size.x < 0.0f ? ImGui::GetContentRegionAvail().x : ImGui::CalcTextSize(label.c_str(), nullptr, true).x + style.FramePadding.x * 2.0f;
                if (node.MinWidth > 0.0f)
                    width = std::max(width, Px(node.MinWidth));
                if (node.MaxWidth > 0.0f)
                    width = std::min(width, Px(node.MaxWidth));
                size.x = width;
            }
            bool clicked = false;
            if (node.Style == "accent")
                clicked = AccentButton(label.c_str(), size);
            else if (node.Style == "danger")
                clicked = DangerButton(label.c_str(), size);
            else
                clicked = ImGui::Button(label.c_str(), size);
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
                ImGui::PushTextWrapPos(Px(320.0f));
                ImGui::TextUnformatted(node.Confirm.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
                const std::string yes = IconText(node.Icon, node.Text.empty() ? std::string("OK") : node.Text) + "##yes";
                if (node.Style == "danger" ? DangerButton(yes.c_str()) : AccentButton(yes.c_str()))
                {
                    clicked = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##no") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            if (clicked && (node.Handlers & H::Click))
                Post(node.Id, "click");
            break;
        }
        case UiKind::Progress:
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
            const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : 0.0f, node.Height > 0.0f ? Px(node.Height) : Px(200.0f));
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
        m_inGrid = inGrid;
        ImGui::EndDisabled();
        if (!node.Tooltip.empty())
            ItemTooltip(node.Tooltip.c_str());
        ImGui::PopID();
    }

    void UiHost::DrawSection(Lua::UiNode& node)
    {
        const std::string title = node.Title.empty() ? node.Text : node.Title;
        if (node.Collapsible)
        {
            const std::string header = IconText(node.Icon, title.empty() ? std::string("Section") : title) + (node.Badge.empty() ? std::string() : "  ·  " + node.Badge) + "###section";
            if (ImGui::CollapsingHeader(header.c_str(), node.Open ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            {
                ImGui::Indent(Px(6.0f));
                DrawChildren(node, false);
                ImGui::Unindent(Px(6.0f));
            }
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Card);
        ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::CardBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(10.0f, 8.0f));
        const float height = node.Height > 0.0f ? Px(node.Height) : 0.0f;
        ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
        if (height <= 0.0f)
            flags |= ImGuiChildFlags_AutoResizeY;
        ImGui::BeginChild("##section", ImVec2(node.Width > 0.0f ? Px(node.Width) : 0.0f, height), flags);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (!title.empty() || !node.Badge.empty())
        {
            const std::string glyph(IconGlyph(node.Icon));
            CardTitle(glyph.empty() ? nullptr : glyph.c_str(), title.c_str(), node.Badge);
        }
        DrawChildren(node, false);
        ImGui::EndChild();
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
            if (ImGui::BeginTabItem(label.c_str(), nullptr, tabFlags))
            {
                ImGui::PushID(tab->Id);
                DrawChildren(*tab, false);
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
        }
        if (!node.Vertical)
            ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##second", secondSize, ImGuiChildFlags_None);
        if (parts.size() > 1)
            DrawNode(*parts[1]);
        ImGui::EndChild();
    }

    void UiHost::DrawValue(Lua::UiNode& node)
    {
        if (node.MinWidth > 0.0f || node.MaxWidth > 0.0f)
        {
            float width = node.Width > 0.0f ? Px(node.Width) : (node.Width < 0.0f || (m_cell && node.Label.empty())) ? ImGui::GetContentRegionAvail().x : ImGui::CalcItemWidth();
            if (node.MinWidth > 0.0f)
                width = std::max(width, Px(node.MinWidth));
            if (node.MaxWidth > 0.0f)
                width = std::min(width, Px(node.MaxWidth));
            ImGui::SetNextItemWidth(width);
        }
        else if (node.Width > 0.0f)
            ImGui::SetNextItemWidth(Px(node.Width));
        else if (node.Width < 0.0f || (m_cell && node.Label.empty()))
            ImGui::SetNextItemWidth(-FLT_MIN);
        if (node.Focus && !node.Focused && !m_design)
        {
            ImGui::SetKeyboardFocusHere();
            node.Focused = true;
        }
        const std::string label = Label(node, node.Label);
        switch (node.Kind)
        {
        case UiKind::Checkbox:
        {
            bool value = node.Bool;
            if (ImGui::Checkbox(label.c_str(), &value))
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
                InputMultiline(label.c_str(), node.String, ImVec2(node.Width > 0.0f ? Px(node.Width) : -FLT_MIN, node.Height > 0.0f ? Px(node.Height) : Px(80.0f)), flags);
            else
            {
                flags |= ImGuiInputTextFlags_EnterReturnsTrue;
                entered = node.Hint.empty() ? InputText(label.c_str(), node.String, flags) : InputTextWithHint(label.c_str(), node.Hint.c_str(), node.String, flags);
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
            bool changed = false;
            if (node.Integer || node.Hex)
            {
                std::int64_t value = static_cast<std::int64_t>(node.Number);
                const std::int64_t step = node.Step > 0.0 ? static_cast<std::int64_t>(node.Step) : 1;
                const std::int64_t fast = step * 10;
                changed = ImGui::InputScalar(label.c_str(), ImGuiDataType_S64, &value, &step, &fast, node.Hex ? "%llX" : "%lld", node.Hex ? ImGuiInputTextFlags_CharsHexadecimal : 0);
                if (changed)
                    node.Number = static_cast<double>(value);
            }
            else
            {
                double value = node.Number;
                const double step = node.Step > 0.0 ? node.Step : 0.0;
                changed = ImGui::InputDouble(label.c_str(), &value, step, step * 10.0, node.Format.empty() ? "%.3f" : node.Format.c_str());
                if (changed)
                    node.Number = value;
            }
            if (changed)
            {
                if (node.HasMin)
                    node.Number = std::max(node.Number, node.Min);
                if (node.HasMax)
                    node.Number = std::min(node.Number, node.Max);
                Model().Touch(node);
                if (node.Handlers & H::Change)
                    Post(node.Id, "change", { node.Number });
            }
            break;
        }
        case UiKind::Slider:
        {
            bool changed = false;
            if (node.Integer)
            {
                std::int64_t value = static_cast<std::int64_t>(node.Number);
                const std::int64_t low = node.HasMin ? static_cast<std::int64_t>(node.Min) : 0;
                const std::int64_t high = node.HasMax ? static_cast<std::int64_t>(node.Max) : 100;
                changed = ImGui::SliderScalar(label.c_str(), ImGuiDataType_S64, &value, &low, &high, node.Format.empty() ? "%lld" : node.Format.c_str());
                if (changed)
                    node.Number = static_cast<double>(value);
            }
            else
            {
                double value = node.Number;
                const double low = node.HasMin ? node.Min : 0.0;
                const double high = node.HasMax ? node.Max : 1.0;
                changed = ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &value, &low, &high, node.Format.empty() ? "%.3f" : node.Format.c_str());
                if (changed)
                    node.Number = value;
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
            const char* preview = node.Selected > 0 && node.Selected <= static_cast<int>(node.Items.size()) ? node.Items[static_cast<std::size_t>(node.Selected - 1)].c_str() : "";
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
            if (!node.Horizontal && !node.Label.empty())
                ImGui::TextUnformatted(node.Label.c_str());
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
            if (node.Horizontal && !node.Label.empty())
            {
                ImGui::SameLine();
                ImGui::TextUnformatted(node.Label.c_str());
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
            ImGui::TextUnformatted(node.Text.c_str());
            break;
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
        const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : 0.0f, node.Height > 0.0f ? Px(node.Height) : Px(200.0f));
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
                        const bool right = c < node.Columns.size() && node.Columns[c].Align == "right";
                        if (right)
                        {
                            const float width = ImGui::CalcTextSize(cell.c_str()).x;
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
                        }
                        if (c == 0)
                        {
                            ImGui::PushID(r);
                            const bool selected = node.Selected == r + 1;
                            if (ImGui::Selectable((cell + "##row").c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
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
                            if (!node.Menu.empty() && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                            {
                                node.Selected = r + 1;
                                g_context = { node.Id, r + 1, {}, 0.0f, 0.0f };
                                ImGui::OpenPopup(popup);
                            }
                            ImGui::PopID();
                        }
                        else
                            ImGui::TextUnformatted(cell.c_str());
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
            const bool open = ImGui::TreeNodeEx("##item", flags, "%s", IconText(item.Icon, item.Text).c_str());
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
        const ImVec2 size(node.Width > 0.0f ? Px(node.Width) : std::max(avail.x, Px(40.0f)), node.Height > 0.0f ? Px(node.Height) : Px(200.0f));
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
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(dialog.Text.c_str());
            ImGui::PopTextWrapPos();
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
        const bool pressedYes = (dialog.Danger ? DangerButton(yes, Px(100.0f, 0.0f)) : AccentButton(yes, Px(100.0f, 0.0f))) || accept;
        ImGui::SameLine();
        const bool pressedNo = ImGui::Button(no, Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
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
            if (ImGui::Button((text + "##slot" + std::to_string(slot.Id)).c_str()))
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
            ImGui::PushStyleColor(ImGuiCol_Text, Color(slot.Color, ManifoldTheme::Muted));
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked())
                Post(slot.Id, "click");
            if (!slot.Tooltip.empty())
                ItemTooltip(slot.Tooltip.c_str());
            x += ImGui::CalcTextSize(text.c_str()).x + gap;
        }
    }
}
