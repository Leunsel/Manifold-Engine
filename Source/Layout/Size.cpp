#include "Layout/Size.h"

#include <algorithm>
#include <cfloat>

namespace Manifold::Layout
{
    namespace
    {
        struct Want
        {
            float Low = 0.0f;
            float Want = 0.0f;
            float High = FLT_MAX;
            float Natural = 0.0f;
            float Weight = 0.0f; // fill cells only
        };

        Want WantOf(const Size& spec, const Measure& measure, float scale)
        {
            Want want;
            const float minimum = std::max(0.0f, spec.MinPoints * scale);
            want.High = spec.MaxPoints > 0.0f ? std::max(minimum, spec.MaxPoints * scale) : FLT_MAX;
            const float contentLow = measure.Minimum > 0.0f ? measure.Minimum : measure.Natural;
            switch (spec.Kind)
            {
            case Size::Mode::Fixed:
                want.Want = std::clamp(spec.Value * scale, minimum, want.High);
                want.Low = std::clamp(contentLow > 0.0f ? contentLow : want.Want, minimum, want.Want);
                want.Natural = want.Want;
                want.High = want.Want;
                break;
            case Size::Mode::Fit:
                want.Want = std::clamp(std::max(measure.Natural, measure.Minimum), minimum, want.High);
                want.Low = std::clamp(contentLow, minimum, want.Want);
                want.Natural = want.Want;
                want.High = want.Want;
                break;
            case Size::Mode::Fill:
                want.Low = std::min(std::max(minimum, measure.Minimum), want.High);
                want.Want = want.Low;
                want.Natural = std::clamp(measure.Natural, want.Low, want.High);
                want.Weight = spec.Value > 0.0f ? spec.Value : 1.0f;
                break;
            }
            return want;
        }

        // one line of cells [first, last): grows fill cells or shrinks toward the minimums
        bool PlaceLine(const std::vector<Want>& wants, std::size_t first, std::size_t last, float available, float gap, std::vector<float>& widths)
        {
            const float space = available - gap * static_cast<float>(last - first - 1);
            float sumWant = 0.0f;
            float sumLow = 0.0f;
            for (std::size_t i = first; i < last; ++i)
            {
                widths[i] = wants[i].Want;
                sumWant += wants[i].Want;
                sumLow += wants[i].Low;
            }
            if (sumWant <= space)
            {
                // ∑ water-filling: share the rest by weight; a cell at its
                // maximum drops out and its share goes to the others
                float extra = space - sumWant;
                for (int round = 0; round < 8 && extra > 0.01f; ++round)
                {
                    float weights = 0.0f;
                    for (std::size_t i = first; i < last; ++i)
                    {
                        if (wants[i].Weight > 0.0f && widths[i] < wants[i].High)
                            weights += wants[i].Weight;
                    }
                    if (weights <= 0.0f)
                        break;
                    float given = 0.0f;
                    for (std::size_t i = first; i < last; ++i)
                    {
                        if (wants[i].Weight <= 0.0f || widths[i] >= wants[i].High)
                            continue;
                        const float share = std::min(extra * wants[i].Weight / weights, wants[i].High - widths[i]);
                        widths[i] += share;
                        given += share;
                    }
                    extra -= given;
                }
                return true;
            }
            const float deficit = sumWant - space;
            const float shrinkable = sumWant - sumLow;
            if (shrinkable >= deficit && shrinkable > 0.0f)
            {
                for (std::size_t i = first; i < last; ++i)
                    widths[i] = wants[i].Want - (wants[i].Want - wants[i].Low) * deficit / shrinkable;
                return true;
            }
            for (std::size_t i = first; i < last; ++i)
                widths[i] = wants[i].Low;
            return false;
        }
    }

    Slots Distribute(float available, float gap, std::span<const Size> specs, std::span<const Measure> measured, float scale, bool wrap)
    {
        Slots slots;
        const std::size_t count = specs.size();
        slots.Widths.assign(count, 0.0f);
        slots.LineOf.assign(count, 0);
        if (count == 0)
            return slots;
        std::vector<Want> wants;
        wants.reserve(count);
        float sumWant = 0.0f;
        float sumLow = 0.0f;
        float widest = 0.0f;
        for (std::size_t i = 0; i < count; ++i)
        {
            wants.push_back(WantOf(specs[i], i < measured.size() ? measured[i] : Measure{}, scale));
            sumWant += wants.back().Want;
            sumLow += wants.back().Low;
            slots.Natural += wants.back().Natural;
            widest = std::max(widest, wants.back().Low);
        }
        const float gaps = gap * static_cast<float>(count - 1);
        slots.Natural += gaps;
        slots.Minimum = wrap ? widest : sumLow + gaps;
        if (available <= 0.0f)
            available = slots.Natural;

        // ∑ a wrapping row keeps its cells at their own size and breaks the
        // line before it would have to shrink them; a row that does not wrap
        // shrinks them, down to their minimums
        if (!wrap || sumWant + gaps <= available)
        {
            slots.Fits = PlaceLine(wants, 0, count, available, gap, slots.Widths);
            float used = gaps;
            for (const float width : slots.Widths)
                used += width;
            slots.LineUsed.push_back(used);
            return slots;
        }
        std::size_t first = 0;
        int line = 0;
        while (first < count)
        {
            std::size_t last = first + 1;
            float taken = wants[first].Want;
            while (last < count && taken + gap + wants[last].Want <= available)
            {
                taken += gap + wants[last].Want;
                ++last;
            }
            if (!PlaceLine(wants, first, last, available, gap, slots.Widths))
                slots.Fits = false;
            float used = gap * static_cast<float>(last - first - 1);
            for (std::size_t i = first; i < last; ++i)
            {
                slots.LineOf[i] = line;
                used += slots.Widths[i];
            }
            slots.LineUsed.push_back(used);
            first = last;
            ++line;
        }
        return slots;
    }
}
