#pragma once

#include "Layout/Region.h"
#include "Layout/Rules.h"
#include "Layout/Size.h"

#include <optional>
#include <utility>
#include <vector>

namespace Manifold::Layout
{
    enum class Align
    {
        Start,
        Center,
        End
    };

    struct RowOptions
    {
        Space Gap = Space::M;
        float GapPixels = -1.0f;    // overrides Gap when 0 or more
        bool Wrap = false;          // cells that do not fit go to the next line instead of shrinking
        Align Justify = Align::Start;
        bool FrameLine = true;      // text in a cell sits on the line of framed widgets beside it
    };

    // ∑ A row of cells side by side. The widths come from Distribute (what
    // each cell asked for, what its content measured last frame), every cell
    // is a region, and the whole row is one, so nothing in a cell reaches its
    // neighbour or past the row. Usage:
    //
    //   Layout::Row row("find", { Size::Fill(), Size::Fit(), Size::Fit() });
    //   row.Cell([&] { InputText(...); });
    //   row.Cell([&] { Layout::Button("Find"); });
    //   row.Cell([&] { Layout::Button("Close"); });
    class Row
    {
    public:
        Row(const char* id, std::vector<Size> cells, const RowOptions& options = {});
        Row(ImGuiID id, std::vector<Size> cells, const RowOptions& options = {});
        ~Row();
        Row(const Row&) = delete;
        Row& operator=(const Row&) = delete;

        template <class Draw>
        void Cell(Draw&& draw)
        {
            if (BeginCell())
                draw();
            EndCell();
        }

        bool BeginCell();
        void EndCell();
        void Skip();                // leaves the next cell empty
        void End();
        int Count() const { return static_cast<int>(m_specs.size()); }
        int Next() const { return m_next; }
        const Slots& Widths() const { return m_slots; }
        float Gap() const { return m_gap; }

    private:
        void Begin(ImGuiID id, const RowOptions& options);
        ImGuiID CellId(int index) const;

        ImGuiID m_id = 0;
        RowOptions m_options;
        std::vector<Size> m_specs;
        Slots m_slots;
        std::optional<Region> m_region;
        std::optional<Region> m_cell;
        float m_gap = 0.0f;
        float m_left = 0.0f;
        float m_top = 0.0f;
        float m_lineTop = 0.0f;
        float m_lineHeight = 0.0f;
        float m_x = 0.0f;
        int m_line = -1;
        int m_next = 0;
        bool m_ended = false;
    };

    struct StackOptions
    {
        Space Gap = Space::M;
        float Width = 0.0f;   // pixels; 0: what is available
        ImGuiID Id = 0;
    };

    // ∑ Content one below the other with the same gap; a region of its own,
    // so what is inside stays within the stack's width.
    class Stack
    {
    public:
        explicit Stack(const StackOptions& options = {});
        ~Stack();
        Stack(const Stack&) = delete;
        Stack& operator=(const Stack&) = delete;
        void End();

    private:
        std::optional<Region> m_region;
        bool m_ended = false;
    };

    // the row options of a toolbar: small gaps, wraps when narrow
    RowOptions Toolbar(Align justify = Align::Start);
    void Spacing(Space space = Space::M);
}
