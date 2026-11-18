#pragma once

#include "Core/Types.h"
#include "Target/ManagedDumper.h"
#include "Target/ManagedRuntime.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
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
        void ResolveValueTypes(int classIndex) override;
        std::optional<std::string> ResolveHead(std::uint64_t header, bool allowRuntimeFallback) override;
        std::optional<std::string> MethodNameAt(std::uint64_t address) override;
        std::optional<std::string> MethodStartNameAt(std::uint64_t address) override;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> MethodRange(std::uint64_t address) override;
        std::size_t MethodsSharingCode(std::uint64_t) override { return 0; }
        bool CompilesOnDemand() const override { return true; }
        std::optional<std::uint64_t> Compile(std::uint64_t method) override;
        std::uint64_t ManagedClass(std::string_view space, std::string_view name) override;

    private:
        using ImageList = std::vector<std::pair<std::uint64_t, std::string>>;

        struct DumperHandles
        {
            std::uint64_t Class = 0;
            std::uint64_t VTable = 0;
            std::uint64_t Begin = 0;
            std::uint64_t Release = 0;
            std::uint64_t State = 0;
            std::uint64_t Types = 0;
            std::uint64_t Cancel = 0;
            std::uint64_t Data = 0;
            std::uint64_t Size = 0;
        };

        ImageList ReadImages();
        Status LoadManagedImage(const ImageList& images);
        std::optional<std::vector<MonoClass>> DumpWithAgent(const ImageList& images, std::int64_t total, WorkProgress* progress);
        Status LoadDumper(const ImageList& images);
        Result<ManagedDump> RunDumper(std::int64_t total, WorkProgress* progress);
        bool CheckDump(const ImageList& images, ManagedDump& dump);
        void NameNestedTypes(ManagedDump& dump, WorkProgress* progress);
        std::optional<std::uint64_t> InvokeStatic(std::uint64_t method);
        std::optional<std::int64_t> ReadStatic(std::uint64_t field, std::size_t bytes);
        bool WriteStatic(std::uint64_t field, std::int32_t value);
        void StopDumper();

        DumperHandles m_dumper;
        std::uint64_t m_managedImage = 0;
        bool m_dumperPending = false;
        bool m_dumperBroken = false;

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
