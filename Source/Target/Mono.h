#pragma once

#include "Core/Errors.h"
#include "Target/RemoteCall.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

        // The class of the object at an address, or nullopt when it is not a
        // known Mono object (checked against the enumerated classes, so a random
        // address does not pass). This is what makes Structure Dissect name a
        // Mono instance automatically.
        std::optional<std::string> ClassNameAt(std::uint64_t address);

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
    };
}
