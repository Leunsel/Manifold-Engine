#pragma once

#include "Target/ClrLink.h"
#include "Target/ManagedRuntime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Manifold::Target
{
    // Code ranges of the methods found so far, by start. Ranges never
    // overlap: code the runtime has thrown away and compiled anew (tiered
    // compilation, a collected dynamic method) can be reported over an older
    // range, and the newer one replaces every range it touches, so the range
    // found for an address always contains it.
    class ClrCodeRanges
    {
    public:
        struct Entry
        {
            std::uint64_t Start = 0;
            std::uint64_t End = 0;
            std::string Name;
        };

        // The range that contains the address, nullptr when none does.
        const Entry* Find(std::uint64_t address) const;
        // [start, end) for the method name; an empty range is not kept.
        const Entry* Add(std::uint64_t start, std::uint64_t end, std::string name);
        void Clear() { m_ranges.clear(); }
        std::size_t Size() const { return m_ranges.size(); }

    private:
        std::map<std::uint64_t, Entry> m_ranges;
    };

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
        std::vector<ManagedModule> Modules() override { return m_modules; }

    private:
        using Clock = std::chrono::steady_clock;

        // busy (when given) tells a request that was not sent because the
        // helper was busy with a longer one.
        std::optional<std::vector<std::uint8_t>> Ask(ClrProtocol::Op op, std::uint64_t argument, unsigned timeoutMs, std::string* error = nullptr,
            const std::uint32_t* extra = nullptr, bool* busy = nullptr);
        // Starts the helper again after it ended (a missed deadline, a crash),
        // at most once per backoff period; false while it stays down.
        bool Restart();
        // The members of a class from the helper; with staticsOnly, only the
        // static fields are replaced (their storage may exist by now).
        bool ReadMembers(int classIndex, bool staticsOnly);
        const ClrCodeRanges::Entry* AskCode(std::uint64_t address);
        int AddDescribed(std::uint64_t mt);
        bool InManagedImage(std::uint64_t address) const;

        ClrLink m_link;
        // Restarts: whether attached (Detach stops them), when the next one
        // may run and how long after a failed one. Under m_restartLock.
        std::atomic<bool> m_attached{ false };
        std::mutex m_restartLock;
        Clock::time_point m_nextRestart{};
        std::chrono::seconds m_restartDelay{ 10 };
        // Classes whose members could not be read, by index: when to ask
        // again, so a dead or busy helper is not asked every frame.
        std::unordered_map<int, Clock::time_point> m_membersRetry;
        // Classes with statics the runtime had no storage for yet (a class
        // whose constructor has not run), by index: when they were read.
        std::unordered_map<int, Clock::time_point> m_staticsRead;
        // Per class index: the parent's method table.
        std::vector<std::uint64_t> m_parents;
        // Method tables the helper said are no type, so a stray pointer is
        // not asked about again and again.
        std::unordered_set<std::uint64_t> m_notTypes;
        // Code ranges of methods found so far, and addresses asked about
        // recently that were in no method (code can appear later, as the
        // runtime compiles more, so misses expire).
        ClrCodeRanges m_code;
        std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> m_misses;
        // The images of managed assemblies: precompiled code (ReadyToRun,
        // NGEN) lives inside them, unlike the JIT's.
        std::vector<std::pair<std::uint64_t, std::uint64_t>> m_images;
        std::vector<ManagedModule> m_modules;
    };
}
