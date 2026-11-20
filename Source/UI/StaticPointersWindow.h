#pragma once

#include "Scan/StaticPointers.h"
#include "UI/BackgroundTask.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::UI
{
    class StaticPointersWindow : public ToolWindowBase
    {
    public:
        explicit StaticPointersWindow(Workspace& workspace);
        ~StaticPointersWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }
        void Search(std::uint64_t address);

    private:
        struct Outcome
        {
            std::vector<Scan::StaticPointer> Hits;
            bool Truncated = false;
            bool Cancelled = false;
            double Seconds = 0.0;
        };

        void Start();
        void Collect();
        void Results();
        void AddToList(const Scan::StaticPointer& hit);
        std::string PathText(const Scan::StaticPointer& hit) const;

        Workspace& m_workspace;
        std::string m_address;
        std::string m_maxOffset = "1000";
        bool m_system = false;
        std::string m_error;
        std::uint64_t m_target = 0;
        std::shared_ptr<Target::Process> m_process;
        BackgroundTask<Outcome, Scan::PatternProgress> m_task;
        std::optional<Outcome> m_result;
        int m_selected = -1;
    };
}
