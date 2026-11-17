#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Manifold
{
    class Engine;
}

namespace Manifold::Diagnostics
{
    struct Adapter
    {
        std::string Name;
        std::uint32_t Vendor = 0;
        std::uint64_t VideoMemory = 0;
        std::uint64_t Luid = 0;
        bool Software = false;
    };

    struct GraphicsInfo
    {
        bool Known = false;
        Adapter Device;
        std::vector<Adapter> Adapters;
        std::string Driver;
        std::string FeatureLevel;
        bool Warp = false;
        std::string SwapEffect;
        std::string Choice;
        bool ChosenByWindows = false;
        void* Window = nullptr;
    };

    void SetGraphics(GraphicsInfo info);
    const GraphicsInfo& Graphics();
    std::uint64_t DisplayAdapterLuid(void* window);
    std::string WindowsGraphicsPreference();

    enum class Level
    {
        Ok,
        Info,
        Warning,
        Problem
    };

    enum class Action
    {
        None,
        RestartElevated,
        OpenCrashFolder,
        OpenSettings,
        OpenLog,
        TurnOnProfiler
    };

    struct Check
    {
        Level Severity = Level::Ok;
        std::string Area;
        std::string Title;
        std::string Detail;
        std::string Fix;
        Action Act = Action::None;
    };

    struct InfoRow
    {
        std::string Key;
        std::string Value;
    };

    struct InfoGroup
    {
        std::string Title;
        std::vector<InfoRow> Rows;
    };

    struct ProcessSample
    {
        bool Valid = false;
        double CpuOfOneCore = 0.0;
        double CpuOfSystem = 0.0;
        std::uint64_t PrivateBytes = 0;
        std::uint64_t PeakPrivateBytes = 0;
        std::uint64_t WorkingSet = 0;
        std::uint64_t PeakWorkingSet = 0;
        unsigned long Handles = 0;
        unsigned long GdiObjects = 0;
        unsigned long UserObjects = 0;
        int Threads = 0;
        double UptimeSeconds = 0.0;
    };

    struct ThreadSample
    {
        unsigned long Id = 0;
        std::string Start;
        std::string Description;
        double CpuOfOneCore = 0.0;
        double CpuSeconds = 0.0;
        int Priority = 0;
        bool Main = false;
    };

    void Sample(bool threads);
    const ProcessSample& LastSample();
    const std::vector<ThreadSample>& LastThreads();

    const char* LevelName(Level level);
    std::string Bytes(std::uint64_t bytes);
    std::string Milliseconds(double ms);

    std::vector<InfoGroup> CollectInfo(Engine& engine);
    std::vector<Check> RunChecks(Engine& engine);
    std::string Report(Engine& engine, const std::vector<InfoGroup>& extra);
}
