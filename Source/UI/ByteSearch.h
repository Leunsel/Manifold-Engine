#pragma once

#include "UI/BackgroundTask.h"
#include "UI/CodeTools.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class ByteSearchWindow : public ToolWindowBase
    {
    public:
        explicit ByteSearchWindow(Workspace& workspace);
        ~ByteSearchWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }
        void Search(const std::string& pattern);

    private:
        struct Hit
        {
            std::uint64_t Address = 0;
            bool Code = false;
        };

        struct Outcome
        {
            std::vector<Hit> Hits;
            std::size_t Length = 0;
            bool Truncated = false;
            bool Cancelled = false;
            double Seconds = 0.0;
        };

        void Start();
        void Collect();
        void Keys();
        void Results();
        void Reveal(int row, bool raise);

        Workspace& m_workspace;
        bool m_focus = true;
        ModuleScope m_scope;
        std::string m_pattern;
        std::string m_error;
        std::string m_searched;
        std::string m_searchedPattern;
        std::shared_ptr<Target::Process> m_process;
        BackgroundTask<Outcome, Scan::PatternProgress> m_task;
        std::optional<Outcome> m_result;
        int m_selected = -1;
        bool m_scrollToSelected = false;
    };
}
