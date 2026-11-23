#include "Layout/Region.h"

#include "Layout/Audit.h"
#include "Layout/Rules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace Manifold::Layout
{
    namespace
    {
        std::vector<Region*>& Stack()
        {
            static std::vector<Region*> stack;
            return stack;
        }

        int g_quiet = 0;

        ImGuiID MinimumKey(ImGuiID id)
        {
            const int salt = 0x4D696E;
            return ImHashData(&salt, sizeof(salt), id);
        }
    }

    Region::Region(const RegionOptions& options)
    {
        ImGuiContext& g = *GImGui;
        ImGuiWindow* window = g.CurrentWindow;
        m_window = window;
        m_parent = Current();
        m_id = options.Id;
        m_clip = options.Clip;
        m_frameLine = options.FrameLine;
        m_toParent = options.ReportToParent;
        const float x = window->DC.CursorPos.x;
        const float width = options.Width > 0.0f ? options.Width : std::max(0.0f, ImGui::GetContentRegionAvail().x);
        m_left = x;
        m_right = x + width;
        m_top = window->DC.CursorPos.y;
        ImGui::BeginGroup();
        m_workMax = window->WorkRect.Max.x;
        m_contentMax = window->ContentRegionRect.Max.x;
        window->WorkRect.Max.x = m_right;
        window->ContentRegionRect.Max.x = m_right;
        if (m_clip)
        {
            // ∑ a little room beyond the edge, so a focus frame drawn around a
            // widget at the edge stays whole; less than half a gap, so it
            // never reaches the neighbour's content
            const float margin = Pt(2.0f);
            ImGui::PushClipRect(ImVec2(m_left - margin, window->ClipRect.Min.y), ImVec2(m_right + margin, window->ClipRect.Max.y), true);
        }
        Stack().push_back(this);
    }

    Region::~Region()
    {
        End();
    }

    void Region::End()
    {
        if (m_ended)
            return;
        m_ended = true;
        std::vector<Region*>& stack = Stack();
        IM_ASSERT(!stack.empty() && stack.back() == this && "Layout regions must close in the order they opened");
        if (!stack.empty() && stack.back() == this)
            stack.pop_back();
        IM_ASSERT(GImGui->CurrentWindow == m_window && "A layout region must close in the window it opened in");
        if (m_clip)
            ImGui::PopClipRect();
        m_window->WorkRect.Max.x = m_workMax;
        m_window->ContentRegionRect.Max.x = m_contentMax;
        ImGui::EndGroup();
        m_size = ImGui::GetItemRectSize();
        const ImRect drawn(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const float width = m_right - m_left;
        const float natural = m_reported ? std::max(m_natural, m_size.x) : m_size.x;
        const float minimum = m_reported ? std::min(m_minimum, natural) : 0.0f;
        if (m_id != 0)
        {
            ImGuiStorage* storage = ImGui::GetStateStorage();
            storage->SetFloat(m_id, natural);
            storage->SetFloat(MinimumKey(m_id), minimum);
        }
        if (m_size.x > width + 1.0f && Audit::Enabled())
        {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "content is %.0f px wide in a cell of %.0f px", m_size.x, width);
            Audit::Note(Audit::Kind::CellOverflow, drawn, ImRect(m_left, drawn.Min.y, m_right, drawn.Max.y), detail);
        }
        // an auto-sizing window grows to what the content would like
        if (natural > width)
            WantWidth(m_left, natural);
        if (m_toParent && m_parent && m_parent->m_window == m_window)
            m_parent->Report(m_left, m_top, natural, minimum);
    }

    Region* Region::Current()
    {
        const std::vector<Region*>& stack = Stack();
        if (stack.empty() || !GImGui)
            return nullptr;
        Region* top = stack.back();
        return top->m_window == GImGui->CurrentWindow ? top : nullptr;
    }

    void Region::Report(float x, float y, float natural, float minimum)
    {
        if (g_quiet > 0)
            return;
        minimum = std::min(minimum, natural);
        if (std::fabs(y - m_lineY) < 0.5f)
        {
            // beside the item before it: the line needs both, and the gap
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            m_lineNatural += gap + natural;
            m_lineMinimum += gap + minimum;
        }
        else
        {
            // a new line: what is in front of its first item (an indent, a label column) counts, measured now; it does
            // not grow with the region
            const float offset = std::clamp(x - m_left, 0.0f, std::max(0.0f, m_right - m_left));
            m_lineY = y;
            m_lineNatural = offset + natural;
            m_lineMinimum = offset + minimum;
        }
        m_natural = std::max(m_natural, m_lineNatural);
        m_minimum = std::max(m_minimum, m_lineMinimum);
        m_reported = true;
    }

    QuietReports::QuietReports()
    {
        ++g_quiet;
    }

    QuietReports::~QuietReports()
    {
        --g_quiet;
    }

    float Avail()
    {
        return std::max(0.0f, ImGui::GetContentRegionAvail().x);
    }

    void Report(float natural, float minimum)
    {
        if (Region* region = Region::Current())
            region->Report(GImGui->CurrentWindow->DC.CursorPos.x, GImGui->CurrentWindow->DC.CursorPos.y, natural, minimum);
    }

    bool Bounded()
    {
        ImGuiContext& g = *GImGui;
        const ImGuiWindow* window = g.CurrentWindow;
        if (Region::Current())
            return true;
        if (AutoSizedWidth(window) || (window->Flags & ImGuiWindowFlags_HorizontalScrollbar))
            return false;
        if (window->AutoFitFramesX > 0)
            return false;
        if (const ImGuiTable* table = g.CurrentTable; table && table->InnerWindow == window && table->CurrentColumn >= 0)
        {
            const ImGuiTableColumn& column = table->Columns[table->CurrentColumn];
            // ∑ a fixed column fits its content while it is queued for that, or for good when it has no width of its own
            //   and cannot be resized; a fixed column given a width (TableSetupColumn) is bounded like any other
            if ((column.Flags & ImGuiTableColumnFlags_WidthFixed) &&
                (column.AutoFitQueue != 0 || ((column.Flags & ImGuiTableColumnFlags_NoResize) && column.InitStretchWeightOrWidth <= 0.0f)))
                return false;
        }
        return true;
    }

    bool AutoSizedWidth(const ImGuiWindow* window)
    {
        if (window->Flags & ImGuiWindowFlags_ChildWindow)
            return (window->ChildFlags & ImGuiChildFlags_AutoResizeX) != 0;
        return (window->Flags & (ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_Tooltip)) != 0;
    }

    bool FrameLine()
    {
        const Region* region = Region::Current();
        return region && region->FrameLine();
    }

    Measure Measured(ImGuiID id)
    {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        return { storage->GetFloat(id, 0.0f), storage->GetFloat(MinimumKey(id), 0.0f) };
    }

    void WantWidth(float x, float width)
    {
        ImGuiWindow* window = GImGui->CurrentWindow;
        window->DC.IdealMaxPos.x = std::max(window->DC.IdealMaxPos.x, x + width);
    }

    void NewFrame()
    {
        std::vector<Region*>& stack = Stack();
        IM_ASSERT(stack.empty() && "A layout region was left open in the last frame");
        stack.clear();
    }
}
