#pragma once

#include "Core/Errors.h"
#include "Scan/ValueType.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Manifold::Target
{
    class Process;
    struct RegionInfo;
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

    enum class ScanKind
    {
        Single,
        All,
        Grouped,
        Custom
    };

    enum class CompareTo
    {
        Previous,
        First,
        Saved
    };

    inline constexpr ValueType kAllTypes[] = { ValueType::Byte, ValueType::TwoBytes, ValueType::FourBytes, ValueType::EightBytes, ValueType::Float, ValueType::Double };
    inline constexpr std::uint32_t kAllTypesDefault = 0x3F;
    inline constexpr int kGroupMaxBytes = 4096;

    std::uint32_t AllTypeBit(ValueType type);

    struct GroupElement
    {
        ValueType Type = ValueType::FourBytes;
        int Size = 4;
        int Offset = 0;
        bool Any = false;
        bool Skip = false;
        bool Unicode = false;
        bool Signed = false;
        std::int64_t Low = 0;
        std::int64_t High = 0;
        double LowFloat = 0.0;
        double HighFloat = 0.0;
        std::vector<std::uint8_t> Text;
    };

    struct GroupSpec
    {
        std::vector<GroupElement> Elements;
        int BlockSize = 0;
        int Within = 0;

        bool Matches(const std::uint8_t* block, std::vector<int>* offsets = nullptr) const;
        std::vector<int> Offsets(std::span<const std::uint8_t> block) const;
        std::string Describe(std::span<const std::uint8_t> block, int pointerSize) const;
    };

    std::optional<GroupSpec> ParseGroup(std::string_view text, bool hex, int pointerSize, std::string& error);

    struct ScanOptions
    {
        ScanKind Kind = ScanKind::Single;
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
        bool SkipNoCache = true;
        bool SkipWriteCombine = true;
        int Alignment = 4;
        bool PauseProcess = false;
        int Threads = 0;
        std::uint32_t AllTypes = kAllTypesDefault;
        std::string CustomName;
        int CustomSize = 0;
        std::vector<std::uint8_t> CustomBytes;
        int BlockSize = 0;
        CompareTo Against = CompareTo::Previous;
        std::string SavedName;
        int MemoryLimitMb = -1;  // RAM for the results of all scans: -1 Auto, 0 disk only (MemoryBudget in Scan/ScanStore.h)
    };

    // Where the results of a scan are kept.
    struct ScanStorage
    {
        std::uint64_t MemoryBytes = 0;
        std::uint64_t DiskBytes = 0;
    };

    const char* CompareName(Compare compare);
    bool CompareNeedsValue(Compare compare);
    bool CompareNeedsTwoValues(Compare compare);
    bool CompareAllowedFirst(Compare compare);
    bool CompareAllowedNext(Compare compare);
    bool CompareAvailable(ScanKind kind, Compare compare);
    bool CompareUsesPrevious(Compare compare);

    struct ScanRecord
    {
        std::uint64_t Address = 0;
        ValueType Type = ValueType::FourBytes;
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
        // the current results, and with the kept ones (Undo, saved results) everything this scanner holds
        ScanStorage Storage(bool everything = false) const;
        bool IsSnapshot() const;
        bool Record(std::uint64_t index, ScanRecord& out) const;
        ValueType Type() const;
        ScanKind Kind() const;
        int ValueSize() const;
        ValueFormat Format() const;
        std::shared_ptr<const GroupSpec> Group() const;
        std::string CustomName() const;
        const ScanOptions& Options() const { return m_options; }

        bool CanUndo() const { return m_undo != nullptr; }
        void Undo();
        void Reset();
        int ScanNumber() const { return m_scanNumber; }
        double LastSeconds() const { return m_lastSeconds; }

        bool SaveResults(const std::string& name);
        std::vector<std::string> SavedNames() const;
        bool HasSaved(std::string_view name) const;

    private:
        void Run(std::shared_ptr<Target::Process> process, ScanOptions options, std::shared_ptr<const ResultSet> previous, std::shared_ptr<const ResultSet> saved);

        std::shared_ptr<const ResultSet> m_current;
        std::shared_ptr<const ResultSet> m_undo;
        std::shared_ptr<const ResultSet> m_finished;
        std::map<std::string, std::shared_ptr<const ResultSet>> m_saved;
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

    struct PatternFilter
    {
        Tristate Writable = Tristate::Any;
        Tristate Executable = Tristate::Any;
        Tristate CopyOnWrite = Tristate::Any;
        bool IncludeReadOnly = true;
        std::uint64_t Divisor = 0;
        std::uint64_t Modulo = 0;
        std::uint64_t Remainder = 0;

        bool Accepts(const Target::RegionInfo& region) const;
        bool Accepts(std::uint64_t address) const;
    };

    std::vector<std::uint64_t> FindPattern(const Target::Process& process, const Pattern& pattern, std::uint64_t start, std::uint64_t stop, bool executableOnly,
        std::size_t limit, bool includeReadOnly = true);
    struct PatternProgress
    {
        std::atomic<bool> Cancel{ false };
        std::atomic<std::uint64_t> Done{ 0 };
        std::atomic<std::uint64_t> Total{ 0 };
        std::function<bool()> Stop;

        bool Stopped() const { return Cancel || (Stop && Stop()); }
    };

    std::vector<std::uint64_t> FindPattern(const Target::Process& process, const Pattern& pattern, std::uint64_t start, std::uint64_t stop, const PatternFilter& filter,
        std::size_t limit, PatternProgress* progress = nullptr);
}
