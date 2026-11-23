#pragma once

#include <span>
#include <vector>

namespace Manifold::Layout
{
    // ∑ How a cell of a row asks for width. Fixed: that many points. Fit: what
    // its content needs (measured in the last frame). Fill: a share of what
    // is left, by weight. Min and Max (points) bound all three; a Fit cell
    // shrinks down to its content's own minimum before anything overlaps.
    struct Size
    {
        enum class Mode
        {
            Fixed,
            Fit,
            Fill
        };

        Mode Kind = Mode::Fit;
        float Value = 0.0f;
        float MinPoints = 0.0f;
        float MaxPoints = 0.0f;

        static Size Fixed(float points) { return { Mode::Fixed, points, 0.0f, 0.0f }; }
        static Size Fit() { return { Mode::Fit, 0.0f, 0.0f, 0.0f }; }
        static Size Fill(float weight = 1.0f) { return { Mode::Fill, weight > 0.0f ? weight : 1.0f, 0.0f, 0.0f }; }
        Size Min(float points) const
        {
            Size size = *this;
            size.MinPoints = points;
            return size;
        }
        Size Max(float points) const
        {
            Size size = *this;
            size.MaxPoints = points;
            return size;
        }
    };

    // what a cell's content reported in the last frame, in pixels
    struct Measure
    {
        float Natural = 0.0f;
        float Minimum = 0.0f;
    };

    struct Slots
    {
        std::vector<float> Widths;     // pixels per cell
        std::vector<int> LineOf;       // the line each cell is on (0 unless wrapping)
        std::vector<float> LineUsed;   // the width each line takes, gaps included
        float Natural = 0.0f;          // what the row would like on one line
        float Minimum = 0.0f;          // the least it can take: one line, or the widest cell when wrapping
        bool Fits = true;              // false: even the minimums are wider than the room
    };

    // Pure: no ImGui. scale turns points into pixels.
    Slots Distribute(float available, float gap, std::span<const Size> specs, std::span<const Measure> measured, float scale, bool wrap);
}
