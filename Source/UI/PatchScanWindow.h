#pragma once

#include "Target/PatchScan.h"
#include "UI/BackgroundTask.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class PatchScanWindow : public ToolWindowBase
    {
    public:
        explicit PatchScanWindow(Workspace& workspace);
        ~PatchScanWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }

    private:
        void Start();
        void Collect();
        void MenuBar();
        std::vector<std::size_t> VisibleRows() const;
        void RestoreAll(const std::vector<std::size_t>& rows);
        Status Restore(const Target::CodePatch& patch);
        void Notes();
        void Results();
        void PatchRow(std::size_t index);
        std::vector<Disasm::Instruction> Listing(std::uint64_t address, std::span<const std::uint8_t> bytes, std::uint64_t size, int limit) const;

        Workspace& m_workspace;
        bool m_windowsModules = false;
        bool m_allNotes = false;
        std::string m_filter;
        std::shared_ptr<Target::Process> m_scanned;
        BackgroundTask<Target::PatchScanResult, Target::PatchScanProgress> m_task;
        std::optional<Target::PatchScanResult> m_result;
        double m_seconds = 0.0;
        int m_restore = -1;
    };
}
