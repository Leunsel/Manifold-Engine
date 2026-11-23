#pragma once

#include "App/CodeFilter.h"
#include "Disasm/CodeDissect.h"
#include "UI/BackgroundTask.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::UI
{
    class CodeFilterWindow : public ToolWindowBase
    {
    public:
        explicit CodeFilterWindow(Workspace& workspace);
        ~CodeFilterWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }
        // Collects the functions of the chosen modules as soon as a process is open.
        void CollectWhenReady() { m_collectPending = true; }

    private:
        struct Outcome
        {
            std::optional<CodeFilterReport> Report;
            std::string Error;
        };

        void Collect();
        void StartPrepare();
        void Toggle(FilterRound kind);
        void Refine();
        void Hotkeys();
        void MenuBar();
        void Setup();
        void Rounds();
        void History();
        void Results();
        void ResultRow(const CodeCandidate& candidate, int index);
        void Refresh();
        std::string Instruction(std::uint64_t address) const;
        std::string KeysFor(FilterRound kind) const;

        Workspace& m_workspace;
        BackgroundTask<Outcome, Disasm::DissectProgress> m_task;
        std::shared_ptr<Target::Process> m_process;
        std::set<std::string> m_unchecked;
        std::string m_error;
        std::string m_filter;
        std::vector<CodeCandidate> m_shown;
        std::vector<int> m_rows;
        std::string m_rowsFilter;
        bool m_rowsValid = false;
        bool m_blocks = false;
        std::size_t m_functions = 0;
        std::size_t m_shownRemaining = static_cast<std::size_t>(-1);
        std::size_t m_shownHistory = static_cast<std::size_t>(-1);
        int m_selected = -1;
        bool m_unwind = true;
        bool m_exports = true;
        bool m_pointers = true;
        bool m_outDown = false;
        bool m_keepDown = false;
        bool m_collectPending = false;
        double m_hitsAt = -1.0;
        std::uint64_t m_hits = 0;
    };
}
