#pragma once

#include "Target/ClrLink.h"
#include "Target/ManagedRuntime.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Manifold::Target
{
    // The .NET side of MonoBridge: .NET Framework and .NET (CoreCLR) games.
    // Unlike Mono and IL2CPP nothing runs in the game; the helper process
    // (ClrLink) reads it through the runtime's data access library. A class
    // handle here is the type's MethodTable, the pointer every object starts
    // with, so naming an object is one read and a table lookup.
    class ClrRuntime final : public RuntimeBackend
    {
    public:
        using RuntimeBackend::RuntimeBackend;

        ManagedRuntime Kind() const override { return ManagedRuntime::Clr; }
        Status Attach(const std::string& module) override;
        void Detach() override;
        Status Enumerate(WorkProgress* progress, bool starting) override;
        void EnsureMembers(int classIndex) override;
        std::optional<std::string> ResolveHead(std::uint64_t header, bool allowDescribe) override;
        std::optional<std::string> MethodNameAt(std::uint64_t address) override;
        std::optional<std::string> MethodStartNameAt(std::uint64_t address) override;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> MethodRange(std::uint64_t address) override;
        std::size_t MethodsSharingCode(std::uint64_t address) override;
        bool CompilesOnDemand() const override { return true; }
        // Not a compile: the address of the method's code if the runtime has
        // compiled it by now (the helper cannot make it do so).
        std::optional<std::uint64_t> Compile(std::uint64_t method) override;
        std::uint64_t ManagedClass(std::string_view space, std::string_view name) override;
        std::optional<int> ParentIndex(int classIndex) override;
        std::optional<std::uint64_t> StaticAddress(int classIndex, std::string_view field) override;
        std::string Description() const override;
        Result<ManagedHeap> ReadHeap() override;
        Result<ManagedInstances> Instances(std::uint64_t klass, std::uint32_t limit) override;
        std::optional<ManagedObject> ObjectAt(std::uint64_t address) override;

    private:
        struct CodeEntry
        {
            std::uint64_t End = 0;
            std::string Name;
        };

        std::optional<std::vector<std::uint8_t>> Ask(ClrProtocol::Op op, std::uint64_t argument, unsigned timeoutMs, std::string* error = nullptr,
            const std::uint32_t* extra = nullptr);
        const std::pair<const std::uint64_t, CodeEntry>* FindCode(std::uint64_t address);
        bool AskCode(std::uint64_t address);
        int AddDescribed(std::uint64_t mt);
        bool InManagedImage(std::uint64_t address) const;

        ClrLink m_link;
        // Per class index: the parent's method table.
        std::vector<std::uint64_t> m_parents;
        // Method tables the helper said are no type, so a stray pointer is
        // not asked about again and again.
        std::unordered_set<std::uint64_t> m_notTypes;
        // Code ranges of methods found so far, by start, and addresses asked
        // about recently that were in no method (code can appear later, as
        // the runtime compiles more, so misses expire).
        std::map<std::uint64_t, CodeEntry> m_code;
        std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> m_misses;
        // The images of managed assemblies: precompiled code (ReadyToRun,
        // NGEN) lives inside them, unlike the JIT's.
        std::vector<std::pair<std::uint64_t, std::uint64_t>> m_images;
    };
}
