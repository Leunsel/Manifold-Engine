#pragma once

#include "Target/ManagedRuntime.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace Manifold::Target
{
    class MonoRuntime final : public RuntimeBackend
    {
    public:
        using RuntimeBackend::RuntimeBackend;

        ManagedRuntime Kind() const override { return ManagedRuntime::Mono; }
        Status Attach(const std::string& module) override;
        void Detach() override;
        Status Enumerate(WorkProgress* progress, bool starting) override;
        void EnsureMembers(int classIndex) override;
        std::optional<std::string> ResolveHead(std::uint64_t header, bool allowRuntimeFallback) override;
        std::optional<std::string> MethodNameAt(std::uint64_t address) override;
        std::optional<std::string> MethodStartNameAt(std::uint64_t address) override;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> MethodRange(std::uint64_t address) override;
        std::size_t MethodsSharingCode(std::uint64_t) override { return 0; }
        bool CompilesOnDemand() const override { return true; }
        std::optional<std::uint64_t> Compile(std::uint64_t method) override;

    private:
        struct JitRange
        {
            std::uint64_t Start = 0;
            std::uint64_t End = 0;
            std::string Name;
        };

        const std::string* FindJitName(std::uint64_t address) const;
        void RememberJitRange(std::uint64_t start, std::uint64_t end, std::string name);
        void RememberJitMiss(std::uint64_t address);

        // Reverse-resolution cache for MethodNameAt. Resolved method code ranges
        // sorted by Start (non-overlapping), plus the addresses already known not
        // to be Mono code, so the hot per-line lookup almost never hits the runtime.
        std::vector<JitRange> m_jitRanges;
        std::unordered_set<std::uint64_t> m_jitMisses;
    };
}
