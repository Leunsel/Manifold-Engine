#include "Core/GraphLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace Manifold::Graph
{
    namespace
    {
        struct Segment
        {
            int Channel = 0;
            std::size_t Edge = 0;
            int Part = 0;
            float Sort = 0.0f;
        };
    }

    Result LayeredLayout(std::span<const Size> sizes, std::span<const Edge> edges, int root, const Spacing& spacing)
    {
        Result result;
        const std::size_t count = sizes.size();
        result.Positions.assign(count, {});
        result.Layers.assign(count, 0);
        result.Routes.assign(edges.size(), {});
        result.Back.assign(edges.size(), false);
        if (count == 0)
            return result;
        if (root < 0 || static_cast<std::size_t>(root) >= count)
            root = 0;

        std::vector<std::vector<std::size_t>> out(count);
        for (std::size_t e = 0; e < edges.size(); ++e)
        {
            if (edges[e].From >= 0 && static_cast<std::size_t>(edges[e].From) < count && edges[e].To >= 0 && static_cast<std::size_t>(edges[e].To) < count)
                out[static_cast<std::size_t>(edges[e].From)].push_back(e);
        }
        std::vector<int> state(count, 0);
        std::vector<std::size_t> starts(count);
        std::iota(starts.begin(), starts.end(), 0);
        std::rotate(starts.begin(), starts.begin() + root, starts.begin() + root + 1);
        for (const std::size_t first : starts)
        {
            if (state[first])
                continue;
            std::vector<std::pair<std::size_t, std::size_t>> stack{ { first, 0 } };
            state[first] = 1;
            while (!stack.empty())
            {
                const std::size_t node = stack.back().first;
                if (stack.back().second < out[node].size())
                {
                    const std::size_t e = out[node][stack.back().second++];
                    const std::size_t to = static_cast<std::size_t>(edges[e].To);
                    if (state[to] == 1)
                        result.Back[e] = true;
                    else if (state[to] == 0)
                    {
                        state[to] = 1;
                        stack.push_back({ to, 0 });
                    }
                    continue;
                }
                state[node] = 2;
                stack.pop_back();
            }
        }

        std::vector<int> indegree(count, 0);
        for (std::size_t e = 0; e < edges.size(); ++e)
        {
            if (!result.Back[e] && edges[e].From != edges[e].To)
                ++indegree[static_cast<std::size_t>(edges[e].To)];
        }
        std::vector<std::size_t> ready;
        for (std::size_t v = 0; v < count; ++v)
        {
            if (indegree[v] == 0)
                ready.push_back(v);
        }
        std::vector<int>& layer = result.Layers;
        for (std::size_t at = 0; at < ready.size(); ++at)
        {
            const std::size_t node = ready[at];
            for (const std::size_t e : out[node])
            {
                if (result.Back[e] || edges[e].From == edges[e].To)
                    continue;
                const std::size_t to = static_cast<std::size_t>(edges[e].To);
                layer[to] = std::max(layer[to], layer[node] + 1);
                if (--indegree[to] == 0)
                    ready.push_back(to);
            }
        }
        const int layerCount = *std::max_element(layer.begin(), layer.end()) + 1;

        std::vector<float> width(count);
        std::vector<int> nodeLayer(layer.begin(), layer.end());
        for (std::size_t v = 0; v < count; ++v)
            width[v] = sizes[v].Width;
        std::vector<std::vector<std::size_t>> chains(edges.size());
        std::vector<std::vector<std::size_t>> up(count);
        std::vector<std::vector<std::size_t>> down(count);
        const auto link = [&](std::size_t from, std::size_t to)
            {
                down[from].push_back(to);
                up[to].push_back(from);
            };
        for (std::size_t e = 0; e < edges.size(); ++e)
        {
            if (result.Back[e] || edges[e].From == edges[e].To)
                continue;
            const std::size_t from = static_cast<std::size_t>(edges[e].From);
            const std::size_t to = static_cast<std::size_t>(edges[e].To);
            std::size_t previous = from;
            for (int l = layer[from] + 1; l < layer[to]; ++l)
            {
                const std::size_t dummy = width.size();
                width.push_back(spacing.Lane);
                nodeLayer.push_back(l);
                up.emplace_back();
                down.emplace_back();
                chains[e].push_back(dummy);
                link(previous, dummy);
                previous = dummy;
            }
            link(previous, to);
        }
        const std::size_t total = width.size();

        std::vector<std::vector<std::size_t>> rows(static_cast<std::size_t>(layerCount));
        for (std::size_t v = 0; v < total; ++v)
            rows[static_cast<std::size_t>(nodeLayer[v])].push_back(v);
        std::vector<float> order(total, 0.0f);
        const auto number = [&](std::vector<std::size_t>& row)
            {
                for (std::size_t i = 0; i < row.size(); ++i)
                    order[row[i]] = static_cast<float>(i);
            };
        for (auto& row : rows)
            number(row);
        const auto sortBy = [&](std::vector<std::size_t>& row, const std::vector<std::vector<std::size_t>>& neighbours)
            {
                std::vector<std::pair<float, std::size_t>> keyed;
                keyed.reserve(row.size());
                for (const std::size_t v : row)
                {
                    float key = order[v];
                    if (!neighbours[v].empty())
                    {
                        float sum = 0.0f;
                        for (const std::size_t n : neighbours[v])
                            sum += order[n];
                        key = sum / static_cast<float>(neighbours[v].size());
                    }
                    keyed.push_back({ key, v });
                }
                std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                for (std::size_t i = 0; i < row.size(); ++i)
                    row[i] = keyed[i].second;
                number(row);
            };
        for (int sweep = 0; sweep < 6; ++sweep)
        {
            for (int l = 1; l < layerCount; ++l)
                sortBy(rows[static_cast<std::size_t>(l)], up);
            for (int l = layerCount - 2; l >= 0; --l)
                sortBy(rows[static_cast<std::size_t>(l)], down);
        }

        const auto gapBetween = [&](std::size_t a, std::size_t b)
            {
                return (a >= count || b >= count) ? spacing.Lane * 1.5f : spacing.Column;
            };
        std::vector<float> x(total, 0.0f);
        for (const auto& row : rows)
        {
            float at = 0.0f;
            for (std::size_t i = 0; i < row.size(); ++i)
            {
                if (i > 0)
                    at += gapBetween(row[i - 1], row[i]);
                x[row[i]] = at;
                at += width[row[i]];
            }
        }
        const auto align = [&](const std::vector<std::size_t>& row, const std::vector<std::vector<std::size_t>>& neighbours)
            {
                const std::size_t n = row.size();
                if (n == 0)
                    return;
                std::vector<float> desired(n);
                for (std::size_t i = 0; i < n; ++i)
                {
                    const std::size_t v = row[i];
                    if (neighbours[v].empty())
                    {
                        desired[i] = x[v];
                        continue;
                    }
                    float sum = 0.0f;
                    for (const std::size_t u : neighbours[v])
                        sum += x[u] + width[u] * 0.5f;
                    desired[i] = sum / static_cast<float>(neighbours[v].size()) - width[v] * 0.5f;
                }
                std::vector<float> left(n);
                std::vector<float> right(n);
                for (std::size_t i = 0; i < n; ++i)
                    left[i] = i == 0 ? desired[i] : std::max(desired[i], left[i - 1] + width[row[i - 1]] + gapBetween(row[i - 1], row[i]));
                for (std::size_t i = n; i-- > 0;)
                    right[i] = i + 1 == n ? desired[i] : std::min(desired[i], right[i + 1] - gapBetween(row[i], row[i + 1]) - width[row[i]]);
                for (std::size_t i = 0; i < n; ++i)
                    x[row[i]] = (left[i] + right[i]) * 0.5f;
            };
        for (int pass = 0; pass < 4; ++pass)
        {
            for (int l = layerCount - 2; l >= 0; --l)
                align(rows[static_cast<std::size_t>(l)], down);
            for (int l = 1; l < layerCount; ++l)
                align(rows[static_cast<std::size_t>(l)], up);
        }

        std::vector<float> top(static_cast<std::size_t>(layerCount), 0.0f);
        std::vector<float> bottom(static_cast<std::size_t>(layerCount), 0.0f);
        std::vector<float> rightmost(static_cast<std::size_t>(layerCount), std::numeric_limits<float>::lowest());
        float y = 0.0f;
        for (int l = 0; l < layerCount; ++l)
        {
            float tallest = 0.0f;
            for (const std::size_t v : rows[static_cast<std::size_t>(l)])
            {
                if (v < count)
                    tallest = std::max(tallest, sizes[v].Height);
                rightmost[static_cast<std::size_t>(l)] = std::max(rightmost[static_cast<std::size_t>(l)], x[v] + width[v]);
            }
            top[static_cast<std::size_t>(l)] = y;
            bottom[static_cast<std::size_t>(l)] = y + tallest;
            y += tallest + spacing.Layer;
        }
        for (std::size_t v = 0; v < count; ++v)
            result.Positions[v] = { x[v], top[static_cast<std::size_t>(layer[v])] };

        std::vector<std::vector<std::size_t>> outPorts(count);
        std::vector<std::vector<std::size_t>> inPorts(count);
        const auto firstHop = [&](std::size_t e) { return chains[e].empty() ? static_cast<std::size_t>(edges[e].To) : chains[e].front(); };
        const auto lastHop = [&](std::size_t e) { return chains[e].empty() ? static_cast<std::size_t>(edges[e].From) : chains[e].back(); };
        for (std::size_t e = 0; e < edges.size(); ++e)
        {
            if (edges[e].From < 0 || static_cast<std::size_t>(edges[e].From) >= count || edges[e].To < 0 || static_cast<std::size_t>(edges[e].To) >= count)
                continue;
            outPorts[static_cast<std::size_t>(edges[e].From)].push_back(e);
            inPorts[static_cast<std::size_t>(edges[e].To)].push_back(e);
        }
        std::vector<float> outX(edges.size(), 0.0f);
        std::vector<float> inX(edges.size(), 0.0f);
        const float far = std::numeric_limits<float>::max() * 0.5f;
        for (std::size_t v = 0; v < count; ++v)
        {
            std::sort(outPorts[v].begin(), outPorts[v].end(), [&](std::size_t a, std::size_t b)
                {
                    const float ka = result.Back[a] || edges[a].From == edges[a].To ? far : x[firstHop(a)];
                    const float kb = result.Back[b] || edges[b].From == edges[b].To ? far : x[firstHop(b)];
                    return ka < kb;
                });
            for (std::size_t i = 0; i < outPorts[v].size(); ++i)
                outX[outPorts[v][i]] = x[v] + width[v] * static_cast<float>(i + 1) / static_cast<float>(outPorts[v].size() + 1);
            std::sort(inPorts[v].begin(), inPorts[v].end(), [&](std::size_t a, std::size_t b)
                {
                    const float ka = result.Back[a] || edges[a].From == edges[a].To ? far : x[lastHop(a)];
                    const float kb = result.Back[b] || edges[b].From == edges[b].To ? far : x[lastHop(b)];
                    return ka < kb;
                });
            for (std::size_t i = 0; i < inPorts[v].size(); ++i)
                inX[inPorts[v][i]] = x[v] + width[v] * static_cast<float>(i + 1) / static_cast<float>(inPorts[v].size() + 1);
        }

        std::vector<std::size_t> backEdges;
        for (std::size_t e = 0; e < edges.size(); ++e)
        {
            if (result.Back[e] || edges[e].From == edges[e].To)
                backEdges.push_back(e);
        }
        std::sort(backEdges.begin(), backEdges.end(), [&](std::size_t a, std::size_t b)
            {
                const int spanA = layer[static_cast<std::size_t>(edges[a].From)] - layer[static_cast<std::size_t>(edges[a].To)];
                const int spanB = layer[static_cast<std::size_t>(edges[b].From)] - layer[static_cast<std::size_t>(edges[b].To)];
                return spanA < spanB;
            });
        std::vector<float> sideX(edges.size(), 0.0f);
        for (std::size_t i = 0; i < backEdges.size(); ++i)
        {
            const std::size_t e = backEdges[i];
            const int from = layer[static_cast<std::size_t>(edges[e].To)];
            const int to = layer[static_cast<std::size_t>(edges[e].From)];
            float edge = std::numeric_limits<float>::lowest();
            for (int l = from; l <= to; ++l)
                edge = std::max(edge, rightmost[static_cast<std::size_t>(l)]);
            sideX[e] = edge + spacing.Column * 0.5f + spacing.Lane * static_cast<float>(i);
        }

        std::vector<Segment> segments;
        for (std::size_t e = 0; e < edges.size(); ++e)
        {
            if (edges[e].From < 0 || static_cast<std::size_t>(edges[e].From) >= count || edges[e].To < 0 || static_cast<std::size_t>(edges[e].To) >= count)
                continue;
            const int from = layer[static_cast<std::size_t>(edges[e].From)];
            const int to = layer[static_cast<std::size_t>(edges[e].To)];
            if (result.Back[e] || edges[e].From == edges[e].To)
            {
                segments.push_back({ from, e, 0, outX[e] });
                segments.push_back({ to - 1, e, 1, inX[e] });
                continue;
            }
            float previous = outX[e];
            for (int l = from; l < to; ++l)
            {
                const std::size_t hop = static_cast<std::size_t>(l - from);
                const float next = hop < chains[e].size() ? x[chains[e][hop]] + width[chains[e][hop]] * 0.5f : inX[e];
                segments.push_back({ l, e, static_cast<int>(hop), std::min(previous, next) });
                previous = next;
            }
        }
        std::map<int, std::vector<std::size_t>> byChannel;
        for (std::size_t s = 0; s < segments.size(); ++s)
            byChannel[segments[s].Channel].push_back(s);
        std::map<std::pair<std::size_t, int>, float> laneY;
        for (auto& [channel, list] : byChannel)
        {
            std::sort(list.begin(), list.end(), [&](std::size_t a, std::size_t b) { return segments[a].Sort < segments[b].Sort; });
            const float floor = channel >= 0 ? bottom[static_cast<std::size_t>(channel)] : -spacing.Layer;
            const float ceiling = channel + 1 < layerCount ? top[static_cast<std::size_t>(channel + 1)] : floor + spacing.Layer;
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                const Segment& segment = segments[list[i]];
                laneY[{ segment.Edge, segment.Part }] = floor + (ceiling - floor) * static_cast<float>(i + 1) / static_cast<float>(list.size() + 1);
            }
        }

        for (std::size_t e = 0; e < edges.size(); ++e)
        {
            if (edges[e].From < 0 || static_cast<std::size_t>(edges[e].From) >= count || edges[e].To < 0 || static_cast<std::size_t>(edges[e].To) >= count)
                continue;
            const std::size_t from = static_cast<std::size_t>(edges[e].From);
            const std::size_t to = static_cast<std::size_t>(edges[e].To);
            std::vector<Point>& route = result.Routes[e];
            const float start = result.Positions[from].Y + sizes[from].Height;
            const float end = result.Positions[to].Y;
            if (result.Back[e] || from == to)
            {
                const float below = laneY[{ e, 0 }];
                const float above = laneY[{ e, 1 }];
                route = { { outX[e], start }, { outX[e], below }, { sideX[e], below }, { sideX[e], above }, { inX[e], above }, { inX[e], end } };
                continue;
            }
            route.push_back({ outX[e], start });
            float atX = outX[e];
            const int first = layer[from];
            for (int l = first; l < layer[to]; ++l)
            {
                const std::size_t hop = static_cast<std::size_t>(l - first);
                const float lane = laneY[{ e, static_cast<int>(hop) }];
                const bool dummy = hop < chains[e].size();
                const float next = dummy ? x[chains[e][hop]] + width[chains[e][hop]] * 0.5f : inX[e];
                route.push_back({ atX, lane });
                route.push_back({ next, lane });
                if (dummy)
                    route.push_back({ next, bottom[static_cast<std::size_t>(l + 1)] });
                atX = next;
            }
            route.push_back({ inX[e], end });
        }

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();
        for (std::size_t v = 0; v < count; ++v)
        {
            minX = std::min(minX, result.Positions[v].X);
            minY = std::min(minY, result.Positions[v].Y);
            maxX = std::max(maxX, result.Positions[v].X + sizes[v].Width);
            maxY = std::max(maxY, result.Positions[v].Y + sizes[v].Height);
        }
        for (const auto& route : result.Routes)
        {
            for (const Point& point : route)
            {
                minX = std::min(minX, point.X);
                minY = std::min(minY, point.Y);
                maxX = std::max(maxX, point.X);
                maxY = std::max(maxY, point.Y);
            }
        }
        for (Point& position : result.Positions)
        {
            position.X -= minX;
            position.Y -= minY;
        }
        for (auto& route : result.Routes)
        {
            for (Point& point : route)
            {
                point.X -= minX;
                point.Y -= minY;
            }
        }
        result.Width = maxX - minX;
        result.Height = maxY - minY;
        return result;
    }
}
