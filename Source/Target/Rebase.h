#pragma once

#include "Target/Process.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Manifold::Target
{
    // Where a rebased module is shown: at the image base its PE header asks
    // for (what IDA, Ghidra and x64dbg's RVA view show before the loader moved
    // it), or at a fixed base such as 0 for plain RVAs.
    struct RebaseRule
    {
        bool ImageBase = true;
        std::uint64_t Base = 0;

        bool operator==(const RebaseRule&) const = default;
    };

    struct RebaseEntry
    {
        std::string Module;
        RebaseRule Rule;
    };

    struct RebaseHit
    {
        std::uint64_t Address = 0;
        ModuleInfo Module;
        std::uint64_t ShownBase = 0;
    };

    // Rebasing changes only how addresses are shown and read back: the
    // process, the instructions and everything written stay at the real
    // addresses. Rules are kept by module name, so they hold for the next
    // start of the game, wherever the loader puts the module then.
    class Rebases
    {
    public:
        void Set(std::string_view module, std::optional<RebaseRule> rule);
        std::optional<RebaseRule> Rule(std::string_view module) const;
        std::vector<RebaseEntry> Entries() const;
        bool Any() const { return m_count.load() != 0; }
        std::uint64_t Generation() const { return m_generation.load(); }
        void Forget();

        // The saved form: module name -> "image" or a hex base.
        void Load(const std::map<std::string, std::string>& saved);
        std::map<std::string, std::string> Saved() const;
        static std::string RuleText(const RebaseRule& rule);
        static std::optional<RebaseRule> ParseRule(std::string_view text);

        // The base the module is shown at, when it has a rule.
        std::optional<std::uint64_t> ShownBase(const Process& process, const ModuleInfo& module) const;
        std::optional<std::uint64_t> Shown(const Process& process, std::uint64_t address) const;
        // A shown address back to the real one, when it lies in a rebased
        // module as shown. With overlapping rebased modules the first in
        // the module list wins.
        std::optional<RebaseHit> Runtime(const Process& process, std::uint64_t shown) const;
        std::optional<std::uint64_t> ImageBase(const Process& process, const ModuleInfo& module) const;
        static std::optional<std::uint64_t> ReadImageBase(const Process& process, const ModuleInfo& module);

    private:
        struct Stored
        {
            std::string Module;
            RebaseRule Rule;
        };

        mutable std::mutex m_lock;
        std::map<std::string, Stored> m_rules;
        mutable std::unordered_map<std::uint64_t, std::optional<std::uint64_t>> m_imageBases;
        std::atomic<std::size_t> m_count = 0;
        std::atomic<std::uint64_t> m_generation = 1;
    };
}
