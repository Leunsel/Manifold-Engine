#pragma once

#include <cstdint>
#include <vector>

namespace Manifold::Profiler
{
    inline constexpr int kHistory = 240;

    struct ZoneView
    {
        int Node = 0;
        int Parent = -1;
        int Depth = 0;
        const char* Name = "";
        bool Wait = false;
        bool HasChildren = false;
        double AverageMs = 0.0;
        double SelfMs = 0.0;
        double MaxMs = 0.0;
        double CallsPerFrame = 0.0;
    };

    struct CounterView
    {
        const char* Name = "";
        double Last = 0.0;
        double Average = 0.0;
        double Max = 0.0;
    };

    struct FrameSummary
    {
        int Frames = 0;
        double PeriodMs = 0.0;
        double WorkMs = 0.0;
        double WorkP50 = 0.0;
        double WorkP95 = 0.0;
        double WorkP99 = 0.0;
        double WorkMax = 0.0;
        double PeriodMax = 0.0;
        int Hitches = 0;
        double FramesPerSecond = 0.0;
    };

    void SetEnabled(bool enabled);
    bool Enabled();
    void SetPaused(bool paused);
    bool Paused();
    void Reset();
    void BeginFrame();
    void Count(const char* name, double value);

    class Zone
    {
    public:
        explicit Zone(const char* name, bool wait = false) noexcept;
        ~Zone();
        Zone(const Zone&) = delete;
        Zone& operator=(const Zone&) = delete;

    private:
        int m_node = -1;
        std::int64_t m_start = 0;
    };

    int Frames();
    std::uint64_t FrameCount();
    int RootNode();
    double FrameMs(int ago);
    double WorkMs(int ago);
    double ZoneMs(int node, int ago);
    double ZoneCalls(int node, int ago);
    void History(int node, std::vector<float>& out);
    void WorkHistory(std::vector<float>& out);
    FrameSummary Summary();
    std::vector<ZoneView> Tree();
    std::vector<int> TopLevel();
    std::vector<CounterView> Counters();
    const char* NodeName(int node);
    bool NodeWaits(int node);
}

#define MANIFOLD_PROFILE_JOIN2(a, b) a##b
#define MANIFOLD_PROFILE_JOIN(a, b) MANIFOLD_PROFILE_JOIN2(a, b)
#define PROFILE_ZONE(name) ::Manifold::Profiler::Zone MANIFOLD_PROFILE_JOIN(profileZone, __LINE__)(name)
#define PROFILE_WAIT(name) ::Manifold::Profiler::Zone MANIFOLD_PROFILE_JOIN(profileZone, __LINE__)(name, true)
