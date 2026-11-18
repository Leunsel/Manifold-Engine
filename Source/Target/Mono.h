#pragma once

#include "Core/Errors.h"
#include "Target/ManagedRuntime.h"
#include "Target/RemoteCall.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Manifold
{
    class WorkProgress;
}

namespace Manifold::Target
{
    class Process;
    class UnityExplorer;

    struct MonoMethod
    {
        std::string Name;
        std::uint64_t Handle = 0;   // MonoMethod* / MethodInfo* in the target
        std::uint64_t Address = 0;  // the code: JIT-compiled on first resolve (Mono), known up front (IL2CPP)
        int ParamCount = -1;        // read for IL2CPP; -1 when not read
    };

    struct MonoField
    {
        std::string Name;
        int Offset = 0;
        int TypeCode = 0;  // MONO_TYPE_* of the field (IL2CPP's Il2CppTypeEnum uses the same codes)
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
        std::uint64_t Handle = 0;   // MonoClass* / Il2CppClass* in the target
        bool MembersLoaded = false; // methods and fields are read on demand
        std::vector<MonoMethod> Methods;
        std::vector<MonoField> Fields;
        std::string Parent;         // IL2CPP: the base class, read with the members
        bool Generic = false;       // IL2CPP: an open generic definition, which has no field offsets
        bool Discovered = false;    // IL2CPP: named from its shape in memory, never handed to the runtime

        std::string FullName() const { return Namespace.empty() ? Name : Namespace + "." + Name; }
    };

    // Reads the managed runtime of a Unity game, Mono or IL2CPP: resolves
    // Class:Method to code addresses and lists classes, methods and field
    // offsets. Everything runs through the RemoteCall worker, so no native
    // DLL is injected; the one worker thread is attached to the runtime's
    // domain once. On Mono it loads the small managed dumper into the root
    // domain to read every class in one pass, and falls back to the C API
    // when that fails. The Unity explorer on top is Mono only.
    class MonoBridge
    {
    public:
        MonoBridge();
        ~MonoBridge();
        MonoBridge(const MonoBridge&) = delete;
        MonoBridge& operator=(const MonoBridge&) = delete;

        Status Start(std::shared_ptr<Process> process, WorkProgress* progress = nullptr);
        // Ends everything in the game: the Unity explorer takes its callback
        // out, the worker thread ends, the class list is dropped.
        void Stop();
        bool Present() const { return m_present.load(); }

        // Which runtime Start attached to; None while nothing is attached.
        ManagedRuntime Runtime() const { return m_runtime.load(); }
        bool IsIl2Cpp() const { return m_runtime.load() == ManagedRuntime::Il2Cpp; }
        static const char* RuntimeName(ManagedRuntime runtime);

        // The scene and object explorer and the inspector on top of this
        // bridge (Mono only). Created on first use; it starts nothing in the
        // game until a scene or object request needs the game's main thread.
        UnityExplorer& Unity();

        // Whether one of the known runtime DLLs (a Mono DLL or IL2CPP's
        // GameAssembly.dll) is in the target's module list, and which. Only
        // looks at the cached list, so it is cheap enough to ask every frame;
        // Start also finds a renamed or embedded runtime by its exports.
        static bool HasRuntime(const Process& process);
        static ManagedRuntime DetectRuntime(const Process& process);
        const std::string& Module() const { return m_module; }

        Status Refresh(WorkProgress* progress = nullptr);
        void SetManagedDumper(bool use) { m_useDumper = use; }
        bool ManagedDumperUsed() const { return m_readByDumper.load(); }
        std::uint64_t Generation() const { return m_generation.load(); }

        // The class list is shallow: Namespace/Name/Image/Handle are filled, but
        // Methods and Fields load only when asked for (a real game has far too
        // many members to walk up front).
        std::vector<MonoClass> Classes() const;
        std::size_t ClassCount() const;
        // Class names may be "Class", "Namespace.Class" or carry the image in
        // front, "Assembly-CSharp:Namespace.Class".
        std::vector<std::string> MethodsOf(std::string_view className);
        std::vector<MonoField> FieldsOf(std::string_view className);
        // One class with its members loaded: methods with handles (and for
        // IL2CPP their code addresses and parameter counts), fields, parent.
        std::optional<MonoClass> ClassDetails(std::string_view className);

        // The class of the object whose header is exactly at an address, or
        // nullopt when it is not a known managed object (checked against the
        // enumerated classes, so a random address does not pass).
        std::optional<std::string> ClassNameAt(std::uint64_t address);

        // Like ClassNameAt but also finds the object when the address points
        // inside it, not at its header: it scans back a little for the enclosing
        // object and returns that header, so dissecting a field address still
        // lands on the right class the way Cheat Engine does.
        std::optional<MonoInstance> ClassAt(std::uint64_t address);

        // The managed method whose code contains an address, as
        // "Namespace.Class:Method" (or "Class:Method" without a namespace), or
        // nullopt when the address is not inside one. Safe for ANY address:
        // Mono asks mono_jit_info_table_find, which reports null off the JIT
        // code (unlike mono_object_get_class, which would dereference a wild
        // pointer); IL2CPP looks the address up in a sorted table of every
        // method's code that Start built. Resolved Mono ranges and a bounded
        // set of misses are cached so the disassembler can ask per line every
        // frame without a remote call each time. This is the reverse of
        // AddressOf and names managed code the memory viewer would otherwise
        // show as a raw address or a module offset.
        std::optional<std::string> MethodNameAt(std::uint64_t address);

        // The method name only when the address is exactly its first instruction,
        // so the disassembler can draw a header at each method start without
        // labelling every line inside it.
        std::optional<std::string> MethodStartNameAt(std::uint64_t address);

        // The [start, end) code range of the method containing an address, so a
        // listing can show offsets from the method start and stop at its end.
        std::optional<std::pair<std::uint64_t, std::uint64_t>> MethodRange(std::uint64_t address);

        // How many methods share code with the method at an address (IL2CPP
        // folds identical method bodies into one); 1 for a method of its own,
        // 0 when the address is no IL2CPP method start.
        std::size_t MethodsSharingCode(std::uint64_t address);

        // While this answers true (the debugger holds the target at a break, so
        // the worker thread is frozen too), address lookups answer from the cache
        // only instead of waiting for a remote call that cannot finish.
        void SetPauseCheck(std::function<bool()> paused) { m_paused = std::move(paused); }

        // "Class:Method", "Namespace.Class:Method" or "Image:Class:Method";
        // compiles the method on the first call (Mono) and caches the address.
        // Returns nullopt when no runtime is attached or the method is unknown.
        std::optional<std::uint64_t> AddressOf(std::string_view name);
        std::optional<int> FieldOffset(std::string_view className, std::string_view fieldName);

    private:
        friend class UnityExplorer;
        friend class MonoRuntime;
        friend class Il2CppRuntime;
        struct Candidate
        {
            std::string Module;
            ManagedRuntime Runtime = ManagedRuntime::None;
        };

        std::uint64_t Fn(const char* name) const;
        std::optional<std::uint64_t> CallFn(const char* name, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0);
        std::string CallString(const char* name, std::uint64_t handle);
        int FindClass(std::string_view name) const;
        static std::vector<Candidate> FindRuntimes(const Process& process, WorkProgress* progress);
        Status AttachRuntime(const Candidate& candidate);
        Status Enumerate(WorkProgress* progress, bool starting);
        void EnsureMembers(int classIndex);
        std::optional<std::string> ResolveHead(std::uint64_t header, bool allowRuntimeFallback);

        std::shared_ptr<Process> m_process;
        std::function<bool()> m_paused;
        mutable std::recursive_mutex m_lock;
        RemoteCall m_call;
        std::unique_ptr<RuntimeBackend> m_backend;
        std::atomic<bool> m_present{ false };
        std::atomic<ManagedRuntime> m_runtime{ ManagedRuntime::None };
        std::string m_module;
        std::uint64_t m_moduleBase = 0;  // the runtime module's range: IL2CPP's code lives in it
        std::uint64_t m_moduleEnd = 0;
        std::uint64_t m_domain = 0;
        std::map<std::string, std::uint64_t> m_functions;
        std::vector<MonoClass> m_classes;
        std::map<std::string, int> m_classIndex;  // name and full name -> class
        std::map<std::uint64_t, int> m_handleIndex;  // MonoClass* -> class
        std::uint64_t m_handleMin = 0;  // span of known MonoClass* pointers, to
        std::uint64_t m_handleMax = 0;  // vet a klass before asking the runtime
        std::unordered_set<std::uint64_t> m_images;  // MonoImage* of every loaded assembly
        std::atomic<std::uint64_t> m_generation{ 0 };
        std::atomic<bool> m_useDumper{ true };
        std::atomic<bool> m_readByDumper{ false };

        std::mutex m_unityLock;
        std::unique_ptr<UnityExplorer> m_unity;
    };
}
