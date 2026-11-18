#pragma once

#include "Core/Errors.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Manifold
{
    class WorkProgress;
}

namespace Manifold::Target
{
    class MonoBridge;

    enum class ManagedRuntime
    {
        None,
        Mono,
        Il2Cpp
    };

    class RuntimeBackend
    {
    public:
        explicit RuntimeBackend(MonoBridge& bridge) : m_bridge(bridge) {}
        virtual ~RuntimeBackend() = default;

        RuntimeBackend(const RuntimeBackend&) = delete;
        RuntimeBackend& operator=(const RuntimeBackend&) = delete;

        virtual ManagedRuntime Kind() const = 0;
        virtual Status Attach(const std::string& module) = 0;
        virtual void Detach() = 0;
        virtual Status Enumerate(WorkProgress* progress, bool starting) = 0;
        virtual void EnsureMembers(int classIndex) = 0;
        virtual std::optional<std::string> ResolveHead(std::uint64_t header, bool allowFallback) = 0;
        virtual std::optional<std::string> MethodNameAt(std::uint64_t address) = 0;
        virtual std::optional<std::string> MethodStartNameAt(std::uint64_t address) = 0;
        virtual std::optional<std::pair<std::uint64_t, std::uint64_t>> MethodRange(std::uint64_t address) = 0;
        virtual std::size_t MethodsSharingCode(std::uint64_t address) = 0;
        virtual bool CompilesOnDemand() const = 0;
        virtual std::optional<std::uint64_t> Compile(std::uint64_t method) = 0;
        virtual std::uint64_t ManagedClass(std::string_view, std::string_view) { return 0; }

    protected:
        MonoBridge& m_bridge;
    };
}
