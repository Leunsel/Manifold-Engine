#include "Core/Profiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include <windows.h>

namespace Manifold::Profiler
{
    namespace
    {
        constexpr double kHitchMs = 50.0;

        struct Node
        {
            const char* Name = "";
            int Parent = -1;
            bool Wait = false;
            std::vector<int> Children;
            std::int64_t Ticks = 0;
            int Calls = 0;
            std::array<float, kHistory> Ms{};
            std::array<std::uint16_t, kHistory> CallCounts{};
        };

        struct Counter
        {
            const char* Name = "";
            double Value = 0.0;
            std::array<float, kHistory> Values{};
        };

        struct State
        {
            std::atomic<bool> On{ false };
            bool Wanted = false;
            std::atomic<DWORD> MainThread{ 0 };
            bool Paused = false;
            double TicksToMs = 0.0;
            std::vector<Node> Nodes;
            std::vector<int> Stack;
            std::vector<Counter> Counters;
            std::array<float, kHistory> Work{};
            int Head = 0;
            int Filled = 0;
            std::uint64_t Total = 0;
            std::int64_t Boundary = 0;

            State()
            {
                LARGE_INTEGER frequency{};
                ::QueryPerformanceFrequency(&frequency);
                TicksToMs = 1000.0 / static_cast<double>(frequency.QuadPart > 0 ? frequency.QuadPart : 1);
                Nodes.reserve(160);
                Stack.reserve(64);
                Counters.reserve(32);
                Node root;
                root.Name = "Whole frame";
                Nodes.push_back(std::move(root));
            }
        };

        State& S()
        {
            static State state;
            return state;
        }

        std::int64_t Ticks()
        {
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            return now.QuadPart;
        }

        bool SameName(const char* a, const char* b)
        {
            return a == b || std::strcmp(a, b) == 0;
        }

        int Child(State& s, int parent, const char* name, bool wait)
        {
            for (const int child : s.Nodes[static_cast<std::size_t>(parent)].Children)
            {
                if (SameName(s.Nodes[static_cast<std::size_t>(child)].Name, name))
                    return child;
            }
            Node node;
            node.Name = name;
            node.Parent = parent;
            node.Wait = wait;
            s.Nodes.push_back(std::move(node));
            const int index = static_cast<int>(s.Nodes.size()) - 1;
            s.Nodes[static_cast<std::size_t>(parent)].Children.push_back(index);
            return index;
        }

        int Slot(const State& s, int ago)
        {
            return (s.Head - 1 - ago + kHistory * 2) % kHistory;
        }

        bool Valid(const State& s, int node, int ago)
        {
            return node >= 0 && node < static_cast<int>(s.Nodes.size()) && ago >= 0 && ago < s.Filled;
        }

        void Discard(State& s)
        {
            for (Node& node : s.Nodes)
            {
                node.Ticks = 0;
                node.Calls = 0;
            }
        }

        void ClearHistory(State& s)
        {
            for (Node& node : s.Nodes)
            {
                node.Ms.fill(0.0f);
                node.CallCounts.fill(0);
            }
            for (Counter& counter : s.Counters)
                counter.Values.fill(0.0f);
            Discard(s);
            s.Work.fill(0.0f);
            s.Head = 0;
            s.Filled = 0;
            s.Total = 0;
            s.Boundary = 0;
        }

        void Commit(State& s, std::int64_t period)
        {
            s.Nodes[0].Ticks = period;
            s.Nodes[0].Calls = 1;
            for (Node& node : s.Nodes)
            {
                node.Ms[static_cast<std::size_t>(s.Head)] = static_cast<float>(static_cast<double>(node.Ticks) * s.TicksToMs);
                node.CallCounts[static_cast<std::size_t>(s.Head)] = static_cast<std::uint16_t>(std::min(node.Calls, 65535));
                node.Ticks = 0;
                node.Calls = 0;
            }
            double waiting = 0.0;
            for (const int child : s.Nodes[0].Children)
            {
                if (s.Nodes[static_cast<std::size_t>(child)].Wait)
                    waiting += s.Nodes[static_cast<std::size_t>(child)].Ms[static_cast<std::size_t>(s.Head)];
            }
            s.Work[static_cast<std::size_t>(s.Head)] = static_cast<float>(std::max(0.0, s.Nodes[0].Ms[static_cast<std::size_t>(s.Head)] - waiting));
            for (Counter& counter : s.Counters)
                counter.Values[static_cast<std::size_t>(s.Head)] = static_cast<float>(counter.Value);
            s.Head = (s.Head + 1) % kHistory;
            s.Filled = std::min(s.Filled + 1, kHistory);
            ++s.Total;
        }

        double Average(const State& s, const std::array<float, kHistory>& values)
        {
            if (s.Filled == 0)
                return 0.0;
            double sum = 0.0;
            for (int ago = 0; ago < s.Filled; ++ago)
                sum += values[static_cast<std::size_t>(Slot(s, ago))];
            return sum / s.Filled;
        }

        double Maximum(const State& s, const std::array<float, kHistory>& values)
        {
            double most = 0.0;
            for (int ago = 0; ago < s.Filled; ++ago)
                most = std::max(most, static_cast<double>(values[static_cast<std::size_t>(Slot(s, ago))]));
            return most;
        }

        void Visit(const State& s, int node, int depth, std::vector<ZoneView>& out)
        {
            const Node& info = s.Nodes[static_cast<std::size_t>(node)];
            ZoneView view;
            view.Node = node;
            view.Parent = info.Parent;
            view.Depth = depth;
            view.Name = info.Name;
            view.Wait = info.Wait;
            view.HasChildren = !info.Children.empty();
            view.AverageMs = Average(s, info.Ms);
            view.MaxMs = Maximum(s, info.Ms);
            double calls = 0.0;
            for (int ago = 0; ago < s.Filled; ++ago)
                calls += info.CallCounts[static_cast<std::size_t>(Slot(s, ago))];
            view.CallsPerFrame = s.Filled ? calls / s.Filled : 0.0;
            double children = 0.0;
            for (const int child : info.Children)
                children += Average(s, s.Nodes[static_cast<std::size_t>(child)].Ms);
            view.SelfMs = std::max(0.0, view.AverageMs - children);
            out.push_back(view);
            for (const int child : info.Children)
                Visit(s, child, depth + 1, out);
        }
    }

    void SetEnabled(bool enabled)
    {
        State& s = S();
        s.Wanted = enabled;
        if (enabled || !s.On.load(std::memory_order_relaxed))
            return;
        s.On.store(false, std::memory_order_relaxed);
        s.Boundary = 0;
        Discard(s);
    }

    bool Enabled()
    {
        return S().On.load(std::memory_order_relaxed);
    }

    void SetPaused(bool paused)
    {
        S().Paused = paused;
    }

    bool Paused()
    {
        return S().Paused;
    }

    void Reset()
    {
        ClearHistory(S());
    }

    void BeginFrame()
    {
        State& s = S();
        if (s.MainThread.load(std::memory_order_relaxed) == 0)
            s.MainThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
        if (s.Wanted && !s.On.load(std::memory_order_relaxed))
        {
            s.On.store(true, std::memory_order_relaxed);
            s.Boundary = 0;
            Discard(s);
        }
        if (!s.On.load(std::memory_order_relaxed))
            return;
        const std::int64_t now = Ticks();
        if (s.Boundary != 0 && !s.Paused)
            Commit(s, now - s.Boundary);
        else
            Discard(s);
        s.Boundary = now;
    }

    void Count(const char* name, double value)
    {
        State& s = S();
        if (!s.On.load(std::memory_order_relaxed) || ::GetCurrentThreadId() != s.MainThread.load(std::memory_order_relaxed))
            return;
        for (Counter& counter : s.Counters)
        {
            if (SameName(counter.Name, name))
            {
                counter.Value = value;
                return;
            }
        }
        Counter counter;
        counter.Name = name;
        counter.Value = value;
        s.Counters.push_back(counter);
    }

    Zone::Zone(const char* name, bool wait) noexcept
    {
        State& s = S();
        if (!s.On.load(std::memory_order_relaxed) || ::GetCurrentThreadId() != s.MainThread.load(std::memory_order_relaxed))
            return;
        const int parent = s.Stack.empty() ? 0 : s.Stack.back();
        m_node = Child(s, parent, name, wait);
        s.Stack.push_back(m_node);
        m_start = Ticks();
    }

    Zone::~Zone()
    {
        if (m_node < 0)
            return;
        const std::int64_t elapsed = Ticks() - m_start;
        State& s = S();
        Node& node = s.Nodes[static_cast<std::size_t>(m_node)];
        node.Ticks += elapsed;
        ++node.Calls;
        if (!s.Stack.empty() && s.Stack.back() == m_node)
        {
            s.Stack.pop_back();
            return;
        }
        const auto found = std::find(s.Stack.rbegin(), s.Stack.rend(), m_node);
        if (found != s.Stack.rend())
            s.Stack.erase(std::next(found).base());
    }

    int Frames()
    {
        return S().Filled;
    }

    std::uint64_t FrameCount()
    {
        return S().Total;
    }

    int RootNode()
    {
        return 0;
    }

    double FrameMs(int ago)
    {
        return ZoneMs(0, ago);
    }

    double WorkMs(int ago)
    {
        const State& s = S();
        if (!Valid(s, 0, ago))
            return 0.0;
        return s.Work[static_cast<std::size_t>(Slot(s, ago))];
    }

    double ZoneMs(int node, int ago)
    {
        const State& s = S();
        if (!Valid(s, node, ago))
            return 0.0;
        return s.Nodes[static_cast<std::size_t>(node)].Ms[static_cast<std::size_t>(Slot(s, ago))];
    }

    double ZoneCalls(int node, int ago)
    {
        const State& s = S();
        if (!Valid(s, node, ago))
            return 0.0;
        return s.Nodes[static_cast<std::size_t>(node)].CallCounts[static_cast<std::size_t>(Slot(s, ago))];
    }

    void History(int node, std::vector<float>& out)
    {
        const State& s = S();
        out.clear();
        if (node < 0 || node >= static_cast<int>(s.Nodes.size()))
            return;
        out.resize(static_cast<std::size_t>(s.Filled));
        for (int i = 0; i < s.Filled; ++i)
            out[static_cast<std::size_t>(i)] = s.Nodes[static_cast<std::size_t>(node)].Ms[static_cast<std::size_t>(Slot(s, s.Filled - 1 - i))];
    }

    void WorkHistory(std::vector<float>& out)
    {
        const State& s = S();
        out.resize(static_cast<std::size_t>(s.Filled));
        for (int i = 0; i < s.Filled; ++i)
            out[static_cast<std::size_t>(i)] = s.Work[static_cast<std::size_t>(Slot(s, s.Filled - 1 - i))];
    }

    FrameSummary Summary()
    {
        const State& s = S();
        FrameSummary summary;
        summary.Frames = s.Filled;
        if (s.Filled == 0)
            return summary;
        std::vector<float> work(static_cast<std::size_t>(s.Filled));
        double period = 0.0;
        double total = 0.0;
        for (int ago = 0; ago < s.Filled; ++ago)
        {
            const std::size_t slot = static_cast<std::size_t>(Slot(s, ago));
            const double frame = s.Nodes[0].Ms[slot];
            period += frame;
            summary.PeriodMax = std::max(summary.PeriodMax, frame);
            work[static_cast<std::size_t>(ago)] = s.Work[slot];
            total += s.Work[slot];
            if (s.Work[slot] > kHitchMs)
                ++summary.Hitches;
        }
        summary.PeriodMs = period / s.Filled;
        summary.WorkMs = total / s.Filled;
        summary.FramesPerSecond = summary.PeriodMs > 0.0 ? 1000.0 / summary.PeriodMs : 0.0;
        std::sort(work.begin(), work.end());
        const auto percentile = [&](double p) {
            const std::size_t at = static_cast<std::size_t>(std::min<double>(static_cast<double>(work.size() - 1), p * static_cast<double>(work.size() - 1) + 0.5));
            return static_cast<double>(work[at]);
        };
        summary.WorkP50 = percentile(0.50);
        summary.WorkP95 = percentile(0.95);
        summary.WorkP99 = percentile(0.99);
        summary.WorkMax = work.back();
        return summary;
    }

    std::vector<ZoneView> Tree()
    {
        std::vector<ZoneView> out;
        const State& s = S();
        out.reserve(s.Nodes.size());
        Visit(s, 0, 0, out);
        return out;
    }

    std::vector<int> TopLevel()
    {
        return S().Nodes[0].Children;
    }

    std::vector<CounterView> Counters()
    {
        const State& s = S();
        std::vector<CounterView> out;
        for (const Counter& counter : s.Counters)
        {
            CounterView view;
            view.Name = counter.Name;
            view.Last = s.Filled ? counter.Values[static_cast<std::size_t>(Slot(s, 0))] : counter.Value;
            view.Average = Average(s, counter.Values);
            view.Max = Maximum(s, counter.Values);
            out.push_back(view);
        }
        return out;
    }

    const char* NodeName(int node)
    {
        const State& s = S();
        return node >= 0 && node < static_cast<int>(s.Nodes.size()) ? s.Nodes[static_cast<std::size_t>(node)].Name : "";
    }

    bool NodeWaits(int node)
    {
        const State& s = S();
        return node >= 0 && node < static_cast<int>(s.Nodes.size()) && s.Nodes[static_cast<std::size_t>(node)].Wait;
    }
}
