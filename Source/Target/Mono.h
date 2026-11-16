#pragma once

#include "Core/Errors.h"
#include "Target/RemoteCall.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Manifold::Target
{
    class Process;

    struct MonoMethod
    {
        std::string Name;
        std::uint64_t Handle = 0;   // MonoMethod* in the target
        std::uint64_t Address = 0;  // JIT code, filled on first resolve
    };

    struct MonoField
    {
        std::string Name;
        int Offset = 0;
        int TypeCode = 0;  // MONO_TYPE_* of the field
    };

    struct MonoInstance
    {
        std::uint64_t Base = 0;   // the object header (where the structure starts)
        std::string Name;         // its class
    };

    struct MonoClass
    {
        std::string Image;
        std::string Namespace;
        std::string Name;
        std::uint64_t Handle = 0;   // MonoClass* in the target
        bool MembersLoaded = false; // methods and fields are read on demand
        std::vector<MonoMethod> Methods;
        std::vector<MonoField> Fields;

        std::string FullName() const { return Namespace.empty() ? Name : Namespace + "." + Name; }
    };

    // Reads the Mono runtime of a target: resolves Class:Method to JIT addresses
    // and lists classes, methods and field offsets. Everything runs through the
    // RemoteCall worker, so no DLL is injected; the one worker thread is attached
    // to the Mono root domain once.
    class MonoBridge
    {
    public:
        Status Start(std::shared_ptr<Process> process);
        void Stop();
        bool Present() const { return m_present; }

        // Whether one of the known Mono runtime DLLs is in the target's module
        // list. Only looks at the cached list, so it is cheap enough to ask
        // every frame; Start also finds a renamed or embedded runtime.
        static bool HasRuntime(const Process& process);
        const std::string& Module() const { return m_module; }

        Status Refresh();
        std::uint64_t Generation() const { return m_generation; }

        // The class list is shallow: Namespace/Name/Image/Handle are filled, but
        // Methods and Fields load only when asked for (a real game has far too
        // many members to walk up front).
        std::vector<MonoClass> Classes() const;
        std::size_t ClassCount() const;
        std::vector<std::string> MethodsOf(std::string_view className);
        std::vector<MonoField> FieldsOf(std::string_view className);

        // The class of the object whose header is exactly at an address, or
        // nullopt when it is not a known Mono object (checked against the
        // enumerated classes, so a random address does not pass).
        std::optional<std::string> ClassNameAt(std::uint64_t address);

        // Like ClassNameAt but also finds the object when the address points
        // inside it, not at its header: it scans back a little for the enclosing
        // object and returns that header, so dissecting a field address still
        // lands on the right class the way Cheat Engine does.
        std::optional<MonoInstance> ClassAt(std::uint64_t address);

        // The JIT-compiled Mono method whose code contains an address, as
        // "Namespace.Class:Method" (or "Class:Method" without a namespace), or
        // nullopt when the address is not inside a Mono method. Safe for ANY
        // address: mono_jit_info_table_find reports null off the JIT code, unlike
        // mono_object_get_class which would dereference a wild pointer. Resolved
        // ranges and a bounded set of misses are cached so the disassembler can
        // ask per line every frame without a remote call each time. This is the
        // reverse of AddressOf and names Mono code the memory viewer would
        // otherwise show as a raw address (JIT code lives outside every module).
        std::optional<std::string> MethodNameAt(std::uint64_t address);

        // The method name only when the address is exactly its first instruction,
        // so the disassembler can draw a header at each method start without
        // labelling every line inside it.
        std::optional<std::string> MethodStartNameAt(std::uint64_t address);

        // The [start, end) code range of the method containing an address, so a
        // listing can show offsets from the method start and stop at its end.
        std::optional<std::pair<std::uint64_t, std::uint64_t>> MethodRange(std::uint64_t address);

        // "Class:Method" or "Namespace.Class:Method"; compiles the method on the
        // first call and caches the address. Returns nullopt when not Mono or not found.
        std::optional<std::uint64_t> AddressOf(std::string_view name);
        std::optional<int> FieldOffset(std::string_view className, std::string_view fieldName);

    private:
        std::uint64_t Fn(const char* name) const;
        std::optional<std::uint64_t> CallFn(const char* name, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0);
        std::string CallString(const char* name, std::uint64_t handle);
        int FindClass(std::string_view name) const;
        void EnsureMembers(int classIndex);
        std::optional<std::string> ResolveHead(std::uint64_t header, bool allowRuntimeFallback);
        const std::string* FindJitName(std::uint64_t address) const;
        void RememberJitRange(std::uint64_t start, std::uint64_t end, std::string name);
        void RememberJitMiss(std::uint64_t address);

        std::shared_ptr<Process> m_process;
        mutable std::recursive_mutex m_lock;
        RemoteCall m_call;
        bool m_present = false;
        std::string m_module;
        std::uint64_t m_domain = 0;
        std::map<std::string, std::uint64_t> m_functions;
        std::vector<MonoClass> m_classes;
        std::map<std::string, int> m_classIndex;  // name and full name -> class
        std::map<std::uint64_t, int> m_handleIndex;  // MonoClass* -> class
        std::uint64_t m_handleMin = 0;  // span of known MonoClass* pointers, to
        std::uint64_t m_handleMax = 0;  // vet a klass before asking the runtime
        std::uint64_t m_generation = 0;

        // Reverse-resolution cache for MethodNameAt. Resolved method code ranges
        // sorted by Start (non-overlapping), plus the addresses already known not
        // to be Mono code, so the hot per-line lookup almost never hits the runtime.
        struct JitRange
        {
            std::uint64_t Start = 0;
            std::uint64_t End = 0;
            std::string Name;
        };
        std::vector<JitRange> m_jitRanges;
        std::unordered_set<std::uint64_t> m_jitMisses;
    };
}
