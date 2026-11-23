#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"
#include "Target/ManagedAgent.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace Manifold::Target
{
    class MonoBridge;
    class Process;
    class FrameBatch;
    struct Il2CppExplorerState;

    // MONO_TYPE_* codes the explorer reads and formats.
    namespace MonoTypeCode
    {
        constexpr int Void = 0x01, Boolean = 0x02, Char = 0x03, I1 = 0x04, U1 = 0x05, I2 = 0x06, U2 = 0x07, I4 = 0x08, U4 = 0x09,
                      I8 = 0x0A, U8 = 0x0B, R4 = 0x0C, R8 = 0x0D, String = 0x0E, Ptr = 0x0F, ByRef = 0x10, ValueType = 0x11,
                      Class = 0x12, Array = 0x14, GenericInst = 0x15, I = 0x18, U = 0x19, FnPtr = 0x1B, Object = 0x1C, SzArray = 0x1D;
    }

    struct MonoTypeInfo
    {
        int Code = 0;               // MONO_TYPE_*; an enum keeps its own code and sets Enum
        int ValueCode = 0;          // what the bytes are: the underlying code of an enum, else Code
        std::uint64_t Class = 0;    // MonoClass* of the type
        std::string Name;           // as the runtime prints it, e.g. UnityEngine.Vector3
        bool ValueType = false;     // stored inline (primitive, struct, enum)
        bool Enum = false;
        int Size = 8;               // bytes the value takes where it is stored
    };

    enum class MonoMemberKind { Field, Property, Method };

    struct MonoMember
    {
        MonoMemberKind Kind = MonoMemberKind::Field;
        std::string Name;
        std::string Declaring;          // the class that declares it
        MonoTypeInfo Type;              // field type, property type, method return type
        std::uint64_t Handle = 0;       // MonoClassField*, MonoProperty* or MonoMethod*
        std::uint64_t Getter = 0;       // property get method
        std::uint64_t Setter = 0;       // property set method
        int Offset = 0;                 // field offset from the object, or from StaticData
        bool Static = false;
        bool Literal = false;           // a const field, shown from LiteralBytes
        std::uint64_t StaticData = 0;   // static storage of the declaring class
        std::uint64_t VTable = 0;       // MonoVTable* of the declaring class (static writes)
        std::vector<std::uint8_t> LiteralBytes;
        std::vector<MonoTypeInfo> Params;
        std::vector<std::string> ParamNames;
        std::string Image;
        std::uint32_t Token = 0;
        std::uint32_t GetterToken = 0;
        std::uint32_t SetterToken = 0;
    };

    struct MonoEnumValue
    {
        std::int64_t Value = 0;
        std::string Name;
    };

    struct MonoClassLayout
    {
        std::uint64_t Klass = 0;
        std::string Name;                   // full name
        std::string Image;
        std::uint32_t TypeToken = 0;
        std::vector<std::string> Chain;     // this class, then its parents
        std::vector<MonoMember> Members;    // own members first, then inherited ones
        bool ValueType = false;
        bool Enum = false;
        bool UnityObject = false;
        bool GameObject = false;
        bool Component = false;
        bool Transform = false;
        bool Behaviour = false;
        int Rank = 0;                       // arrays: the rank, else 0
        MonoTypeInfo Element;               // arrays: the element type
        int ElementSize = 0;
        std::vector<MonoEnumValue> EnumValues;
    };

    // What one value looks like in the inspector.
    struct MonoShown
    {
        std::string Text;
        std::uint64_t Reference = 0;  // the object the value points at, to open it
        bool Null = false;
        bool Editable = false;
        bool Pending = false;         // a layout it needs is still loading
    };

    struct UnityScene
    {
        int Handle = 0;
        int Slot = 0;
        std::string Name;
        int BuildIndex = -1;
        int RootCount = -1;           // -1 until known
        bool Loaded = false;
        bool Active = false;
        bool DontDestroy = false;     // the DontDestroyOnLoad scene, which the scene list leaves out
        bool RootsLoaded = false;
        bool Loading = false;
        std::vector<int> Roots;       // node indices
    };

    struct UnityNode
    {
        std::uint64_t Object = 0;     // the GameObject
        std::uint64_t Transform = 0;
        std::string Name;
        bool Active = false;
        int ChildCount = 0;
        int Parent = -1;              // node index, -1 for a scene root
        int Scene = -1;
        bool ChildrenLoaded = false;
        bool Loading = false;
        std::vector<int> Children;
    };

    struct UnityTree
    {
        std::vector<UnityScene> Scenes;
        std::vector<UnityNode> Nodes;
        bool Read = false;
        std::uint64_t Generation = 0;
    };

    struct UnitySearch
    {
        std::string ClassName;        // e.g. UnityEngine.Camera, or a game class
        std::string NameFilter;
        bool IncludeHidden = false;   // Unity objects: inactive ones and assets too
    };

    struct UnityHit
    {
        std::uint64_t Object = 0;
        std::string Name;
        std::string Class;
    };

    struct UnitySearchResult
    {
        UnitySearch Request;
        std::vector<UnityHit> Hits;
        std::size_t Total = 0;        // before the name filter and the limit
        bool Running = false;
        bool Done = false;
        bool HeapScan = false;        // instances were found by scanning memory for the class
        std::string Error;
        std::uint64_t Generation = 0;
    };

    struct UnityComponent
    {
        std::uint64_t Object = 0;
        std::string Class;
        bool HasEnabled = false;      // a Behaviour
        bool Enabled = false;
    };

    struct UnityTransform
    {
        std::uint64_t Object = 0;
        float LocalPosition[3] = {};
        float LocalEuler[3] = {};
        float LocalScale[3] = { 1.0f, 1.0f, 1.0f };
        float Position[3] = {};
        bool Read = false;
    };

    struct UnityGameObject
    {
        std::uint64_t Object = 0;
        std::string Name;
        std::string Tag;
        std::string Scene;
        int Layer = 0;
        bool ActiveSelf = false;
        bool ActiveInHierarchy = false;
        std::uint64_t Parent = 0;     // parent GameObject
        std::string ParentName;
        UnityTransform Transform;
        std::vector<UnityComponent> Components;
        std::vector<UnityHit> Children;
        int ChildCount = 0;
    };

    // A property value or a method's return value, read on the game's main thread.
    struct MonoReadValue
    {
        bool Read = false;
        std::vector<std::uint8_t> Raw;  // the inline bytes, or the 8-byte reference
        std::string Text;               // a string result, already read
        std::string Error;
    };

    struct UnityInspection
    {
        int Id = 0;
        std::uint64_t Object = 0;       // 0 for a class's static members
        std::uint64_t Klass = 0;
        std::string Title;
        std::shared_ptr<const MonoClassLayout> Layout;
        std::map<int, MonoReadValue> Values;   // member index -> value
        std::optional<UnityGameObject> GameObject;
        std::optional<UnityTransform> Transform;     // a Transform component
        std::uint64_t Owner = 0;        // a component's GameObject
        std::string OwnerName;
        std::optional<bool> Enabled;    // a Behaviour
        std::string ObjectName;         // a UnityEngine.Object's name
        bool FromScan = false;          // found by a memory scan: managed calls stay off
        bool Unheld = false;
        // The object is no longer there: its memory no longer holds an object
        // of Klass (the game destroyed and collected it, e.g. on a scene
        // change). Nothing is called on it or read from it any more.
        bool Gone = false;
        bool Loading = false;
        std::string Error;
        std::uint64_t Generation = 0;
        // The last refresh: wall time, the requests it sent to the agent in
        // how many round trips, and the frames the game ran meanwhile.
        double RefreshMs = 0.0;
        int Calls = 0;
        int RoundTrips = 0;
        int FrameWaits = 0;
    };

    struct UnityStatus
    {
        bool Hooked = false;            // the agent is in the game's frame callback
        std::string Hook;               // which Unity event it hangs on
        std::uint64_t CallbackRuns = 0; // frames the agent has seen
        std::uint32_t MainThread = 0;
        std::string UnityVersion;
        std::vector<std::string> Missing;  // mono_* exports this runtime does not have
        bool Busy = false;
        std::string Task;
        std::string Error;              // the last failure
        std::optional<bool> RunInBackground;
        bool ChangedRunInBackground = false;
        std::uint64_t Generation = 0;
        std::string LastTask;
        std::chrono::steady_clock::time_point HookedSince{};
    };

    struct ExplorerExitNote
    {
        std::string Text;
        bool Crashed = false;
    };

    // The Unity explorer: scenes and their GameObject hierarchy, an object
    // search, and an inspector for any managed object (fields, properties,
    // methods, and for GameObjects their transform and components).
    //
    // Most of the Unity API may only be called on the game's main thread. The
    // managed agent (Managed/ManifoldDumper/Agent.cs, loaded with the dumper's
    // assembly) hangs a managed Tick on a per-frame Unity event and answers
    // requests from a mailbox in its own memory, a few milliseconds per frame
    // at most; the game never waits for Manifold. Objects are named by the
    // agent's slots, members by their metadata tokens, so no pointer from here
    // is ever handed to the runtime as an object. Field values are still read
    // straight from the game's memory. Requests run on one job thread here, so
    // the interface never waits for a frame of the game; it reads the
    // published snapshots instead.
    //
    // An IL2CPP game cannot load a managed assembly, so there the same
    // requests run as batches of il2cpp_runtime_invoke calls from a small
    // native stub (FrameCalls) that a per-frame Unity method's MethodInfo
    // points at; objects are held by GC handles (UnityExplorerIl2Cpp.cpp).
    class UnityExplorer
    {
    public:
        explicit UnityExplorer(MonoBridge& mono);
        ~UnityExplorer();
        UnityExplorer(const UnityExplorer&) = delete;
        UnityExplorer& operator=(const UnityExplorer&) = delete;

        // Ends the job thread, takes the agent out of the frame callback (it
        // restores Application.runInBackground itself) and drops every slot it
        // held. The bridge calls it before its worker stops.
        void Shutdown();

        // Everything here runs code inside the game, so it stays switched off
        // until the user says yes for this Mono session. Reading classes,
        // resolving method addresses and the Structure Dissect lookup do NOT go
        // through here and are unaffected.
        void SetEnabled(bool on) { m_enabled.store(on); }
        bool Enabled() const { return m_enabled.load(); }

        UnityStatus State() const;
        std::uint64_t Generation() const { return m_generation.load(); }
        void Cancel();

        void ReadScenes();
        void ExpandScene(int scene);
        void ExpandNode(int node);
        UnityTree Tree() const;

        void Search(UnitySearch request);
        UnitySearchResult SearchResult() const;

        int Inspect(std::uint64_t object, bool fromScan = false);
        int InspectClass(std::uint64_t klass);
        void Refresh(int id, bool properties);
        void Close(int id);
        std::optional<UnityInspection> Inspection(int id) const;

        void SetActive(std::uint64_t gameObject, bool active, int refresh);
        void SetEnabled(std::uint64_t behaviour, bool enabled, int refresh);
        void SetName(std::uint64_t object, std::string name, int refresh);
        // which: 0 local position, 1 local rotation (euler angles), 2 local scale
        void SetVector(std::uint64_t transform, int which, const float value[3], int refresh);
        void SetMember(int id, int member, std::string text);
        void InvokeMethod(int id, int member, std::vector<std::string> arguments);
        void SetRunInBackground(bool enabled);

        // Host-side reads for drawing, no remote call: a layout loads in the
        // background on first use (nullptr meanwhile), field values come
        // straight from the game's memory.
        std::shared_ptr<const MonoClassLayout> CachedLayout(std::uint64_t klass);
        MonoShown ReadField(const MonoMember& field, std::uint64_t object);
        MonoShown Format(const MonoTypeInfo& type, const std::uint8_t* bytes, std::size_t size, int depth = 0);
        MonoShown Shown(const MonoMember& member, const MonoReadValue& value);
        // The class of the object at this address, read from the game's memory
        // and vetted (IsClass): 0 when the memory does not hold a live object,
        // e.g. one the game destroyed and collected after a scene change.
        std::uint64_t ClassOf(std::uint64_t object) const;
        std::string ClassNameOf(std::uint64_t object);
        std::string ClassName(std::uint64_t klass);
        // Whether a pointer read from the game's memory is a MonoClass*: one the
        // bridge enumerated, one already confirmed, or one whose image field
        // names a loaded assembly image (offsets learned from known classes).
        // Mono dereferences whatever it is given, so garbage handed to it
        // crashes the game inside the runtime.
        bool IsClass(std::uint64_t klass) const;

        // Text to bytes for a value of this type: numbers, true/false, enum
        // names, and structs as "1, 2, 3". Strings and references are not
        // inline values and return nullopt here.
        std::optional<std::vector<std::uint8_t>> Parse(const MonoTypeInfo& type, const std::string& text, std::string* error = nullptr);

        static std::string ShortTypeName(const MonoTypeInfo& type);
        static std::string Signature(const MonoMember& method);
        static ExplorerExitNote ExitNote(const UnityStatus& status, std::string_view process, std::optional<std::uint32_t> exitCode, double seconds);

    private:
        struct AgentArg
        {
            std::uint8_t Kind = 0;
            int Slot = 0;
            std::vector<std::uint8_t> Bytes;
            std::string Text;
        };

        struct PendingValue
        {
            MonoReadValue Value;
            int Slot = 0;
            bool Reference = false;
        };

        // False (and leaves a reason in the status) while the explorer is off,
        // so no request can reach the game by accident.
        bool Allowed();
        void Post(std::string task, std::function<void()> job);
        void Loop();
        void Publish();
        void Fail(const std::string& text);
        bool Aborted() const;

        std::optional<std::uint64_t> Worker(const char* function, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0);
        // Runs a call that allocates or may run managed code (class init, a
        // vtable) on the game's main thread through the agent once it is in
        // the game, never off-thread; before that it falls back to the worker.
        std::optional<std::uint64_t> Exec(const char* function, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0);
        // Learns where MonoClass keeps its image and name from a few enumerated
        // classes (job thread; once per runtime). Until then IsClass accepts
        // enumerated classes only.
        void LearnClassShape();
        std::uint64_t ArgSpace(std::size_t bytes);

        Status EnsureAgent();
        Result<AgentLink::Exchanged> Ask(AgentRequest& request, std::chrono::milliseconds timeout = std::chrono::seconds(15));
        Status Command(AgentRequest& request, const std::string& what);
        void LearnSlots(const std::vector<int>& slots);
        int SlotOf(std::uint64_t object) const;
        std::uint64_t AddressOf(int slot) const;
        void HoldSlots(int group, const std::vector<int>& slots);
        void ReleaseGroup(int group);
        void ForgetSlots();
        std::pair<std::string, std::uint32_t> ClassToken(std::uint64_t klass);
        static void WriteArg(AgentRequest& request, const AgentArg& arg);
        PendingValue ReadValue(AgentCursor& in, const MonoTypeInfo& type);
        void Resolve(PendingValue& pending);

        std::string ReadString(std::uint64_t monoString, std::size_t limit = 4096) const;
        std::vector<std::uint64_t> ReadArray(std::uint64_t monoArray, std::size_t limit, std::size_t* total = nullptr) const;
        template <typename T> std::optional<T> Unbox(std::uint64_t boxed) const;

        std::uint64_t FindClass(std::string_view name);
        std::string WorkerString(const char* function, std::uint64_t handle);
        std::string TypeName(std::uint64_t type);
        MonoTypeInfo TypeInfo(std::uint64_t type);
        std::vector<std::string> ChainOf(std::uint64_t klass);
        std::shared_ptr<const MonoClassLayout> LoadLayout(std::uint64_t klass);
        bool IsUnityObjectClass(std::uint64_t klass);

        void DoReadScenes();
        void DoExpandScene(int scene);
        void DoExpandNode(int node);
        void DoSearch(UnitySearch request);
        void DoInspect(int id, bool properties);
        void DoSetMember(int id, int member, const std::string& text);
        void DoInvokeMethod(int id, int member, const std::vector<std::string>& arguments);
        std::optional<AgentArg> ArgumentFor(const MonoTypeInfo& type, const std::string& text, std::string& error);

        struct Il2Recipe;
        struct Il2Node;
        struct Il2SceneRoots;
        bool Il2() const;
        Status Il2EnsureHook();
        void Il2Shutdown(bool alive);
        std::uint64_t Il2Method(std::string_view className, std::string_view name, std::initializer_list<const char*> parameters);
        std::uint64_t Il2Method(std::string_view className, std::string_view name, std::span<const char* const> parameters);
        std::uint64_t Il2Icall(const std::string& name);
        Status Il2DontDestroyByDifference(std::uint64_t generation, std::vector<std::uint64_t>& items, std::size_t& total);
        Status Il2Run(Il2Recipe& recipe, std::chrono::milliseconds timeout, bool cancellable = true);
        void Il2Keep(Il2Recipe& recipe, int group);
        int Il2Register(std::uint64_t address, std::uint64_t handle, int group);
        void Il2Hold(int group, const std::vector<int>& slots);
        void Il2Release(int group);
        std::string Il2Thrown(std::uint64_t exception);
        std::vector<std::uint64_t> Il2Drain(const char* function, std::uint64_t object);
        void Il2LoadMembers(MonoClassLayout& layout, std::uint64_t klass);
        void Il2RefreshStatics(std::uint64_t klass);
        void Il2ReadScenes();
        void Il2ExpandScene(int scene);
        void Il2ExpandNode(int node);
        Status Il2DontDestroyScene(int& slot, int& handle);
        Status Il2Nodes(const std::vector<std::uint64_t>& gameObjects, std::uint64_t parentTransform, std::size_t children, int group, std::vector<Il2Node>& out);
        Status Il2FindObjects(std::uint64_t klass, bool includeInactive, bool assetsFirst, std::size_t limit, std::vector<std::uint64_t>& items, std::size_t& total);
        Status Il2WalkRoots(bool describe, int group, std::vector<Il2SceneRoots>& scenes);
        void Il2Search(const UnitySearch& request, std::uint64_t klass, UnitySearchResult& result);
        void Il2Inspect(UnityInspection& inspection, bool properties, int group);
        Status Il2Edit(const std::string& what, std::uint64_t method, std::uint64_t self, const std::vector<AgentArg>& arguments, bool valueType = false);
        Status Il2SetRunInBackground(bool enabled);
        bool Il2Fits(const MonoTypeInfo& type, const AgentArg& argument, std::string& error);
        Status Il2SetField(const UnityInspection& inspection, const MonoMember& member, const AgentArg& argument);
        MonoReadValue Il2Invoke(const UnityInspection& inspection, const MonoMember& member, const std::vector<AgentArg>& arguments, int group);

        MonoBridge& m_mono;
        std::unique_ptr<Il2CppExplorerState> m_il2;

        std::thread m_thread;
        mutable std::mutex m_jobLock;
        std::condition_variable m_jobWake;
        std::deque<std::pair<std::string, std::function<void()>>> m_jobs;
        bool m_stopping = false;
        std::atomic<std::uint64_t> m_abort{ 0 };
        std::uint64_t m_jobAbort = 0;

        std::shared_ptr<Process> Proc() const;
        std::size_t PointerBytes() const;

        mutable std::mutex m_stateLock;
        std::shared_ptr<Process> m_process;
        std::map<std::uint64_t, std::string> m_classNames;
        UnityStatus m_status;
        UnityTree m_tree;
        UnitySearchResult m_search;
        std::map<int, UnityInspection> m_inspections;
        std::map<std::uint64_t, std::shared_ptr<const MonoClassLayout>> m_layouts;
        std::map<std::uint64_t, bool> m_layoutQueued;
        int m_nextInspection = 1;
        std::atomic<std::uint64_t> m_generation{ 1 };
        std::map<int, std::uint64_t> m_slotAddress;
        std::map<std::uint64_t, int> m_addressSlot;

        // Job-thread only.
        std::unique_ptr<AgentLink> m_agent;
        bool m_agentReady = false;
        std::uint64_t m_args = 0;                 // argument page in the game (worker calls)
        std::size_t m_argsUsed = 0;
        struct ClassShape
        {
            bool Ready = false;
            bool Failed = false;
            std::uint32_t Image = 0;              // offset of MonoClass::image
            std::uint32_t Name = 0;               // offset of MonoClass::name
        };
        mutable ClassShape m_classShape;          // under m_stateLock
        mutable std::unordered_set<std::uint64_t> m_validClasses;   // confirmed MonoClass*, under m_stateLock
        std::map<std::uint64_t, std::pair<std::string, std::uint32_t>> m_classTokens;
        int m_statTrips = 0;                      // agent round trips of the current job
        int m_statCalls = 0;                      // agent requests of the current job
        int m_statFrames = 0;
        int m_dontDestroyHandle = 0;
        std::atomic<bool> m_enabled{ false };
        std::map<std::uint64_t, MonoTypeInfo> m_types;
        std::map<std::uint64_t, std::vector<std::string>> m_chains;
    };
}
