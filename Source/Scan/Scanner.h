#pragma once

#include "Core/Errors.h"
#include "Scan/ValueType.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Scan
{
    enum class Compare
    {
        Exact,
        Bigger,
        Smaller,
        Between,
        Unknown,
        Increased,
        IncreasedBy,
        Decreased,
        DecreasedBy,
        Changed,
        Unchanged,
        SameAsFirst
    };

    enum class Tristate
    {
        No,
        Yes,
        Any
    };

    enum class FloatMode
    {
        Rounded,
        Truncated,
        Exact
    };

    struct ScanOptions
    {
        ValueType Type = ValueType::FourBytes;
        Compare Mode = Compare::Exact;
        std::string Value;
        std::string Value2;
        bool Hex = false;
        bool Unicode = false;
        bool CaseSensitive = true;
        FloatMode Floats = FloatMode::Rounded;
        std::uint64_t Start = 0;
        std::uint64_t Stop = 0x7FFFFFFFFFFFull;
        Tristate Writable = Tristate::Yes;
        Tristate Executable = Tristate::Any;
        Tristate CopyOnWrite = Tristate::No;
        bool Private = true;
        bool Image = true;
        bool Mapped = false;
        int Alignment = 4;
        bool PauseProcess = false;
        int Threads = 0;
    };

    const char* CompareName(Compare compare);
    bool CompareNeedsValue(Compare compare);
    bool CompareNeedsTwoValues(Compare compare);
    bool CompareAllowedFirst(Compare compare);
    bool CompareAllowedNext(Compare compare);

    struct ScanRecord
    {
        std::uint64_t Address = 0;
        std::vector<std::uint8_t> Previous;
        std::vector<std::uint8_t> First;
    };

    class ResultSet;

    class Scanner
    {
    public:
        Scanner();
        ~Scanner();

        Scanner(const Scanner&) = delete;
        Scanner& operator=(const Scanner&) = delete;

        Status Start(std::shared_ptr<Target::Process> process, const ScanOptions& options, bool firstScan);
        void Cancel();
        bool Busy() const { return m_busy.load(); }
        double Progress() const;
        bool Poll();
        const Status& LastError() const { return m_lastError; }

        bool HasResults() const { return m_current != nullptr; }
        std::uint64_t Count() const;
        bool IsSnapshot() const;
        bool Record(std::uint64_t index, ScanRecord& out) const;
        ValueType Type() const;
        int ValueSize() const;
        ValueFormat Format() const;
        const ScanOptions& Options() const { return m_options; }

        bool CanUndo() const { return m_undo != nullptr; }
        void Undo();
        void Reset();
        int ScanNumber() const { return m_scanNumber; }
        double LastSeconds() const { return m_lastSeconds; }

    private:
        void Run(std::shared_ptr<Target::Process> process, ScanOptions options, std::shared_ptr<const ResultSet> previous);

        std::shared_ptr<const ResultSet> m_current;
        std::shared_ptr<const ResultSet> m_undo;
        std::shared_ptr<const ResultSet> m_finished;
        Status m_finishedError;
        bool m_finishedReady = false;
        std::mutex m_lock;
        std::thread m_thread;
        std::atomic<bool> m_busy{ false };
        std::atomic<bool> m_cancel{ false };
        std::atomic<std::uint64_t> m_done{ 0 };
        std::atomic<std::uint64_t> m_total{ 1 };
        ScanOptions m_options;
        Status m_lastError;
        int m_scanNumber = 0;
        double m_lastSeconds = 0.0;
        double m_startedAt = 0.0;
    };

    std::vector<std::uint64_t> FindPattern(const Target::Process& process, const Pattern& pattern, std::uint64_t start, std::uint64_t stop, bool executableOnly,
        std::size_t limit, bool includeReadOnly = true);
}
