#pragma once

#include "Core/Errors.h"

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
#include <string>
#include <thread>
#include <vector>

namespace Manifold::Target
{
    class MonoBridge;
    class Process;

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
        bool Loading = false;
        std::string Error;
        std::uint64_t Generation = 0;
    };

    struct UnityStatus
    {
        bool Hooked = false;            // the per-frame callback is in the game
        std::string Hook;               // which Unity event it hangs on
        std::uint64_t CallbackRuns = 0; // times the game has called the stub
        std::uint32_t MainThread = 0;
        std::string UnityVersion;
        std::vector<std::string> Missing;  // mono_* exports this runtime does not have
        bool Busy = false;
        std::string Task;
        std::string Error;              // the last failure
        std::optional<bool> RunInBackground;
        bool ChangedRunInBackground = false;
        std::uint64_t Generation = 0;
    };

    // The Unity explorer: scenes and their GameObject hierarchy, an object
    // search, and an inspector for any managed object (fields, properties,
    // methods, and for GameObjects their transform and components), all
    // through the bridge's remote-call worker without a DLL.
    //
    // Most of the Unity API may only be called on the game's main thread, so
    // the first request hangs the worker's main-thread stub on a per-frame
    // Unity event (Application.onBeforeRender, or Canvas.willRenderCanvases on
    // old versions) through Marshal.GetDelegateForFunctionPointer. Unity calls
    // then run between two frames, a few milliseconds per frame at most.
    // Requests run on one job thread here, so the interface never waits for a
    // frame of the game; it reads the published snapshots instead.
    class UnityExplorer
    {
    public:
        explicit UnityExplorer(MonoBridge& mono);
        ~UnityExplorer();
        UnityExplorer(const UnityExplorer&) = delete;
        UnityExplorer& operator=(const UnityExplorer&) = delete;

        // Ends the job thread, takes the callback out of the game, restores
        // Application.runInBackground when it was changed here, and frees the
        // pins. The bridge calls it before its worker stops.
        void Shutdown();

        // Everything here runs code inside the game, which can crash it (see
        // the note in the .cpp), so it stays switched off until the user says
        // yes for this Mono session. Reading classes, resolving method
        // addresses and the Structure Dissect lookup do NOT go through here and
        // are unaffected.
        void SetEnabled(bool on) { m_enabled.store(on); }
        bool Enabled() const { return m_enabled.load(); }

        // Diagnostics: stop the bootstrap early to find out which step a game
        // dislikes. 2 = create the delegate but do not register it, 3 = also
        // register it but never enter the main thread, 4 (default) = normal.
        void SetProbeLevel(int level) { m_probeLevel = level; }
        // Diagnostics: run without taking any GC handle, to tell whether the
        // handles themselves are what a game cannot survive.
        void SetPinning(bool on) { m_pinning = on; }

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
        std::uint64_t ClassOf(std::uint64_t object) const;
        std::string ClassNameOf(std::uint64_t object);
        std::string ClassName(std::uint64_t klass);

        // Text to bytes for a value of this type: numbers, true/false, enum
        // names, and structs as "1, 2, 3". Strings and references are not
        // inline values and return nullopt here.
        std::optional<std::vector<std::uint8_t>> Parse(const MonoTypeInfo& type, const std::string& text, std::string* error = nullptr);

        static std::string ShortTypeName(const MonoTypeInfo& type);
        static std::string Signature(const MonoMember& method);

    private:
        struct Arg
        {
            std::uint64_t Pointer = 0;           // a reference argument (object or null)
            std::vector<std::uint8_t> Bytes;      // a value argument, passed by address
            static Arg Ref(std::uint64_t object) { Arg a; a.Pointer = object; return a; }
            static Arg Raw(std::vector<std::uint8_t> bytes) { Arg a; a.Bytes = std::move(bytes); return a; }
            template <typename T> static Arg Value(const T& value)
            {
                Arg a;
                a.Bytes.resize(sizeof(T));
                std::memcpy(a.Bytes.data(), &value, sizeof(T));
                return a;
            }
        };

        struct Invoked
        {
            bool Ok = false;
            std::uint64_t Result = 0;   // the returned object (boxed for value types)
            std::string Error;
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
        std::optional<std::uint64_t> Main(const char* function, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0);
        // Runs a call that allocates or may run managed code (class init, a Type
        // object, a string) on the game's main thread once the callback is up,
        // never off-thread, where a static constructor or GC would crash it.
        std::optional<std::uint64_t> Exec(const char* function, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0);
        void YieldFrame();
        Invoked Invoke(bool main, std::uint64_t method, const Arg& self, const std::vector<Arg>& params);
        std::string ExceptionText(bool main, std::uint64_t exception);
        std::uint64_t ArgSpace(std::size_t bytes);

        std::uint64_t FindClass(std::string_view name);
        std::uint64_t Method(std::string_view klass, std::string_view name, int params, std::string_view firstParam = {});
        std::uint64_t TypeObject(std::uint64_t klass);
        std::uint32_t Pin(std::uint64_t object);
        void Unpin(std::vector<std::uint32_t>& pins);
        std::string WorkerString(const char* function, std::uint64_t handle);
        std::string TypeName(std::uint64_t type);
        MonoTypeInfo TypeInfo(std::uint64_t type);
        std::vector<std::string> ChainOf(std::uint64_t klass);
        std::shared_ptr<const MonoClassLayout> LoadLayout(std::uint64_t klass);
        bool IsUnityObjectClass(std::uint64_t klass);

        std::string ReadString(std::uint64_t monoString, std::size_t limit = 4096) const;
        std::vector<std::uint64_t> ReadArray(std::uint64_t monoArray, std::size_t limit, std::size_t* total = nullptr) const;
        template <typename T> std::optional<T> Unbox(std::uint64_t boxed) const;
        std::optional<std::string> CallString(bool main, std::uint64_t method, const Arg& self);
        template <typename T> std::optional<T> CallValue(bool main, std::uint64_t method, const Arg& self, const std::vector<Arg>& params = {});
        std::uint64_t CallObject(bool main, std::uint64_t method, const Arg& self, const std::vector<Arg>& params = {});
        std::uint64_t NewString(const std::string& text);

        Status EnsureHook();
        void Unhook();
        int ResolveDontDestroyScene();
        void DoReadScenes();
        void DoExpandScene(int scene);
        void DoExpandNode(int node);
        bool ReadNode(std::uint64_t gameObject, std::uint64_t knownTransform, UnityNode& node);
        void DoSearch(UnitySearch request);
        void DoInspect(int id, bool properties);
        void ReadGameObject(std::uint64_t gameObject, UnityGameObject& info, std::vector<std::uint32_t>& pins);
        bool ReadTransform(std::uint64_t transform, UnityTransform& info);
        void ReadProperties(UnityInspection& inspection, std::vector<std::uint32_t>& pins);
        MonoReadValue ReadResult(const MonoTypeInfo& type, const Invoked& call, std::vector<std::uint32_t>& pins);
        void DoSetMember(int id, int member, const std::string& text);
        void DoInvokeMethod(int id, int member, const std::vector<std::string>& arguments);
        std::optional<Arg> ArgumentFor(const MonoTypeInfo& type, const std::string& text, std::string& error);

        MonoBridge& m_mono;

        std::thread m_thread;
        mutable std::mutex m_jobLock;
        std::condition_variable m_jobWake;
        std::deque<std::pair<std::string, std::function<void()>>> m_jobs;
        bool m_stopping = false;
        std::atomic<std::uint64_t> m_abort{ 0 };
        std::uint64_t m_jobAbort = 0;

        std::shared_ptr<Process> Proc() const;

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

        // Job-thread only.
        std::uint64_t m_args = 0;                 // argument page in the game
        std::size_t m_argsUsed = 0;
        std::chrono::steady_clock::time_point m_visitStart{};
        bool m_visiting = false;
        std::uint64_t m_delegate = 0;
        std::uint32_t m_delegatePin = 0;
        std::uint64_t m_unhook = 0;               // remove_* method of the event
        bool m_hooked = false;
        int m_dontDestroyHandle = 0;
        std::atomic<bool> m_enabled{ false };
        int m_probeLevel = 4;
        // OFF by default, and it must stay that way: taking GC handles through
        // mono_gchandle_new/_free from this side corrupted the target's handle
        // table and killed Unity 6 games a few seconds later (a bisection
        // against CloverPit pinned it to exactly these two calls -- creating
        // managed objects and the callback delegate are both fine without
        // them). Newer Mono moved to pointer-sized handles (..._v2), so the
        // legacy 32-bit handle this code took back is not something
        // mono_gchandle_free can be trusted with here.
        //
        // Nothing needs them: a registered delegate is rooted by the event's
        // subscriber list, scene objects by their scene, assets by Unity. Only
        // values handed back by a call (strings, boxed results) are unrooted,
        // and those are read immediately.
        bool m_pinning = false;
        std::optional<bool> m_originalRunInBackground;
        std::map<std::string, std::uint64_t> m_methods;
        std::map<std::uint64_t, std::uint64_t> m_typeObjects;
        std::map<std::uint64_t, MonoTypeInfo> m_types;
        std::map<std::uint64_t, std::vector<std::string>> m_chains;
        std::vector<std::uint32_t> m_treePins;
        std::vector<std::uint32_t> m_searchPins;
        std::vector<std::uint32_t> m_keepPins;    // type objects
        std::map<int, std::vector<std::uint32_t>> m_inspectionPins;
    };
}
