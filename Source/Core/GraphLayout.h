#pragma once

#include <span>
#include <vector>

namespace Manifold::Graph
{
    struct Size
    {
        float Width = 0.0f;
        float Height = 0.0f;
    };

    struct Point
    {
        float X = 0.0f;
        float Y = 0.0f;
    };

    struct Edge
    {
        int From = 0;
        int To = 0;
    };

    struct Spacing
    {
        float Column = 36.0f;
        float Layer = 56.0f;
        float Lane = 9.0f;
    };

    struct Result
    {
        std::vector<Point> Positions;
        std::vector<int> Layers;
        std::vector<std::vector<Point>> Routes;
        std::vector<bool> Back;
        float Width = 0.0f;
        float Height = 0.0f;
    };

    Result LayeredLayout(std::span<const Size> sizes, std::span<const Edge> edges, int root, const Spacing& spacing = {});
}
