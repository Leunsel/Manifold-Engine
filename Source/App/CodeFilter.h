#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"
#include "Target/Process.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Manifold
{
    class Engine;

    namespace Disasm
    {
        struct DissectProgress;
    }

    enum CodeSource : std::uint8_t
    {
        SourceUnwind = 1,
        SourceExport = 2,
        SourceCall = 4,
        SourceBlock = 8
    };

    std::string CodeSourceText(std::uint8_t sources);

    struct CodeCandidate
    {
        std::uint64_t Address = 0;
        std::uint8_t Sources = 0;
        std::uint64_t Function = 0;
    };

    struct CodeFilterOptions
    {
        std::vector<std::string> Modules;
        bool Unwind = true;
        bool Exports = true;
        std::optional<bool> Calls;
    };

    struct CodeModuleCount
    {
        std::string Module;
        std::size_t Unwind = 0;
        std::size_t Exports = 0;
        std::size_t Calls = 0;
        std::size_t Rejected = 0;
        std::size_t Total = 0;
    };

    struct CodeFilterReport
    {
        std::vector<CodeModuleCount> Modules;
        std::size_t Total = 0;
        double Seconds = 0.0;
    };

    enum class FilterRound
    {
        FilterOut,
        KeepOnly,
        Refine
    };

    const char* FilterRoundText(FilterRound round);

    struct RoundRecord
    {
        FilterRound Kind = FilterRound::FilterOut;
        std::size_t Before = 0;
        std::size_t After = 0;
        std::size_t Hits = 0;
        std::size_t Skipped = 0;
        double Seconds = 0.0;
    };

    // Finds code by what it does: every candidate (a function start) gets a
    // one-shot int3; a Filter Out round drops what ran while nothing happened,
    // a Keep Only round keeps only what ran during the action.
    class CodeFilter
    {
    public:
        static constexpr std::size_t kUndoDepth = 32;
        static constexpr std::size_t kMaxRefine = 256;

        explicit CodeFilter(Engine& engine) : m_engine(engine) {}

        static std::vector<Target::ModuleInfo> DefaultModules(const Target::Process& process);

        Result<CodeFilterReport> Prepare(const CodeFilterOptions& options, Disasm::DissectProgress* progress = nullptr);
        Status Start(FilterRound kind);
        Status Stop();
        Status Cancel();
        // Splits every function that is left into its basic blocks (from its
        // flow graph), so the next rounds tell which part of it ran.
        Result<std::size_t> Refine(std::size_t maxFunctions = kMaxRefine);
        bool Undo();
        void Reset();

        bool Prepared() const;
        bool Running() const;
        std::optional<FilterRound> RunningKind() const;
        std::size_t Remaining() const;
        std::vector<CodeCandidate> Candidates() const;
        std::vector<RoundRecord> History() const;
        CodeFilterReport Report() const;
        std::uint64_t LiveHits() const;
        double RoundSeconds() const;

    private:
        Engine& m_engine;
        mutable std::mutex m_lock;
        std::vector<CodeCandidate> m_candidates;
        std::vector<std::vector<CodeCandidate>> m_undo;
        std::vector<RoundRecord> m_history;
        CodeFilterReport m_report;
        bool m_prepared = false;
        bool m_running = false;
        FilterRound m_kind = FilterRound::FilterOut;
        std::size_t m_skipped = 0;
        std::uint64_t m_hitBase = 0;
        std::chrono::steady_clock::time_point m_started;
    };
}
