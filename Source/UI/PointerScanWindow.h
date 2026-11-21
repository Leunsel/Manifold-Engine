#pragma once

#include "Scan/PointerScan.h"
#include "Scan/ValueType.h"
#include "UI/BackgroundTask.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class PointerScanWindow : public ToolWindowBase
    {
    public:
        explicit PointerScanWindow(Workspace& workspace);
        ~PointerScanWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }
        void Prepare(std::uint64_t address);

    private:
        enum class Job
        {
            Scan,
            Rescan,
            Order,
            Load
        };

        struct Outcome
        {
            Job Kind = Job::Scan;
            std::shared_ptr<Scan::PointerResults> Results;
            Scan::PointerScanStats Stats;
            Status Error;
            std::optional<std::vector<std::uint32_t>> Order;
            Scan::PointerOrder OrderKind = Scan::PointerOrder::Found;
            std::uint64_t Before = 0;
            std::string File;
            double Seconds = 0.0;
        };

        void Start();
        void Rescan();
        void Sort(Scan::PointerOrder order);
        void Open();
        void Save();
        void Collect();
        void MenuBar();
        void Inputs();
        void RescanRow();
        void Status();
        void Results();
        void RefreshBases();
        bool PathAt(int row, Scan::PointerPath& path) const;
        std::optional<std::uint64_t> Resolve(const Scan::PointerPath& path) const;
        void AddToList(const Scan::PointerPath& path);
        void CopyAll() const;
        std::string ProgressText() const;

        Workspace& m_workspace;
        std::string m_address;
        std::string m_maxOffset = "1000";
        int m_level = 5;
        bool m_aligned = true;
        std::uint64_t m_maxResults = 2'000'000;
        Scan::PointerMemory m_memory;
        std::string m_error;
        bool m_focus = true;

        bool m_rescanByValue = false;
        std::string m_rescanAddress;
        std::string m_rescanValue;
        Scan::ValueType m_rescanType = Scan::ValueType::FourBytes;
        std::string m_rescanError;

        BackgroundTask<Outcome, Scan::PointerProgress> m_task;
        Job m_running = Job::Scan;
        std::shared_ptr<Scan::PointerResults> m_results;
        std::vector<std::uint32_t> m_order;
        Scan::PointerOrder m_orderKind = Scan::PointerOrder::Found;
        Scan::PointerScanStats m_stats;
        std::string m_summary;
        bool m_scanned = false;
        Scan::ValueType m_valueType = Scan::ValueType::FourBytes;
        std::vector<std::uint64_t> m_bases;
        std::uint64_t m_basesGeneration = ~0ull;
        double m_basesAt = -10.0;
        int m_selected = -1;
    };
}
