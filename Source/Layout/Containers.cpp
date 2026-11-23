#include "Layout/Containers.h"

#include "Layout/Audit.h"

#include <algorithm>
#include <cstdio>

namespace Manifold::Layout
{
    Row::Row(const char* id, std::vector<Size> cells, const RowOptions& options) : m_specs(std::move(cells))
    {
        Begin(ImGui::GetID(id), options);
    }

    Row::Row(ImGuiID id, std::vector<Size> cells, const RowOptions& options) : m_specs(std::move(cells))
    {
        Begin(id, options);
    }

    Row::~Row()
    {
        End();
    }

    ImGuiID Row::CellId(int index) const
    {
        return ImHashData(&index, sizeof(index), m_id);
    }

    void Row::Begin(ImGuiID id, const RowOptions& options)
    {
        m_id = id;
        m_options = options;
        m_gap = options.GapPixels >= 0.0f ? options.GapPixels : Layout::Gap(options.Gap);
        std::vector<Measure> measured;
        measured.reserve(m_specs.size());
        for (int i = 0; i < Count(); ++i)
            measured.push_back(Measured(CellId(i)));
        const float available = Avail();
        m_slots = Distribute(available, m_gap, m_specs, measured, Scale(), options.Wrap);
        RegionOptions region;
        region.Width = available;
        region.ReportToParent = false;
        m_region.emplace(region);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        m_left = at.x;
        m_top = at.y;
        m_lineTop = at.y;
        if (!m_slots.Fits && Audit::Enabled())
        {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "the cells need %.0f px, the row has %.0f px", m_slots.Minimum, available);
            Audit::Note(Audit::Kind::RowTooNarrow, ImRect(at.x, at.y, at.x + m_slots.Minimum, at.y + ImGui::GetFrameHeight()),
                ImRect(at.x, at.y, at.x + available, at.y + ImGui::GetFrameHeight()), detail);
        }
    }

    bool Row::BeginCell()
    {
        IM_ASSERT(!m_cell && "Row::BeginCell twice without EndCell");
        if (m_ended || m_next >= Count())
            return false;
        const int index = m_next;
        const int line = m_slots.LineOf[static_cast<std::size_t>(index)];
        if (line != m_line)
        {
            if (m_line >= 0)
                m_lineTop += m_lineHeight + ImGui::GetStyle().ItemSpacing.y;
            m_lineHeight = 0.0f;
            m_line = line;
            const float free = (m_region->Right() - m_left) - m_slots.LineUsed[static_cast<std::size_t>(line)];
            m_x = m_left;
            if (free > 0.0f && m_options.Justify != Align::Start)
                m_x += std::floor(m_options.Justify == Align::End ? free : free * 0.5f);
        }
        ImGui::SetCursorScreenPos(ImVec2(m_x, m_lineTop));
        RegionOptions cell;
        cell.Width = std::max(1.0f, m_slots.Widths[static_cast<std::size_t>(index)]);
        cell.FrameLine = m_options.FrameLine;
        cell.Id = CellId(index);
        cell.ReportToParent = false;
        m_cell.emplace(cell);
        ImGui::PushID(index);
        return true;
    }

    void Row::EndCell()
    {
        if (!m_cell)
            return;
        ImGui::PopID();
        m_cell->End();
        m_lineHeight = std::max(m_lineHeight, m_cell->Size().y);
        m_x += m_slots.Widths[static_cast<std::size_t>(m_next)] + m_gap;
        m_cell.reset();
        ++m_next;
    }

    void Row::Skip()
    {
        Cell([] {});
    }

    void Row::End()
    {
        if (m_ended)
            return;
        EndCell();
        m_ended = true;
        if (m_region)
        {
            m_region->Report(m_left, m_top, m_slots.Natural, m_slots.Minimum);
            m_region->End();
            if (Region* parent = Region::Current())
                parent->Report(m_left, m_top, m_slots.Natural, m_slots.Minimum);
            m_region.reset();
        }
    }

    Stack::Stack(const StackOptions& options)
    {
        RegionOptions region;
        region.Width = options.Width;
        region.Id = options.Id;
        m_region.emplace(region);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Layout::Gap(options.Gap)));
    }

    Stack::~Stack()
    {
        End();
    }

    void Stack::End()
    {
        if (m_ended)
            return;
        m_ended = true;
        ImGui::PopStyleVar();
        m_region.reset();
    }

    RowOptions Toolbar(Align justify)
    {
        RowOptions options;
        options.Gap = Space::S;
        options.Wrap = true;
        options.Justify = justify;
        return options;
    }

    void Spacing(Space space)
    {
        ImGui::Dummy(ImVec2(0.0f, Layout::Gap(space)));
    }
}
