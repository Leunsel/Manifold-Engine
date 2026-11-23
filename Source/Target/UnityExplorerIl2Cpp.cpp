#include "Target/UnityExplorer.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Mono.h"
#include "Target/Process.h"
#include "Target/UnityExplorerIl2Cpp.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <thread>

#include <windows.h>

namespace Manifold::Target
{
    using namespace ExplorerLimits;

    namespace
    {
        constexpr auto kHookWait = std::chrono::milliseconds(1500);
        constexpr auto kCallWait = std::chrono::seconds(15);
        constexpr auto kListWait = std::chrono::seconds(30);
        constexpr auto kShutdownWait = std::chrono::milliseconds(1000);
        constexpr std::size_t kNameChars = 128;
        constexpr std::size_t kTextChars = 1024;
        constexpr std::size_t kMaxScenes = 256;
        constexpr std::size_t kMaxComponents = 512;
        constexpr std::size_t kMaxSearched = 20000;
        constexpr std::size_t kMaxValueBytes = 1024;
        constexpr int kMaxMembers = 4096;
        constexpr std::int32_t kProbeValue = 0x5EED1234;
        constexpr std::uint32_t kFieldStatic = 0x10;
        constexpr std::uint32_t kFieldLiteral = 0x40;
        constexpr std::uint32_t kMethodStatic = 0x10;

        constexpr const char* kObjectClass = "UnityEngine.Object";
        constexpr const char* kGameObjectClass = "UnityEngine.GameObject";
        constexpr const char* kComponentClass = "UnityEngine.Component";
        constexpr const char* kTransformClass = "UnityEngine.Transform";
        constexpr const char* kBehaviourClass = "UnityEngine.Behaviour";
        constexpr const char* kSceneClass = "UnityEngine.SceneManagement.Scene";
        constexpr const char* kSceneManagerClass = "UnityEngine.SceneManagement.SceneManager";
        constexpr const char* kApplicationClass = "UnityEngine.Application";
        constexpr const char* kStripped = " (stripped from this build, or another Unity version)";
        // ∑ Six of the owner's IL2CPP builds stripped the managed runInBackground property (GTFO, Moon
        //   Sweepers, ShellShock Live, The Survivalists, Uno, Master Duel); the engine's internal call stays.
        constexpr const char* kGetBackgroundIcall = "UnityEngine.Application::get_runInBackground()";
        constexpr const char* kSetBackgroundIcall = "UnityEngine.Application::set_runInBackground(System.Boolean)";

        // Unity's object finders by version: 6000.6 has no FindObjectsOfType
        // any more and its two-argument FindObjectsByType takes a
        // FindObjectsInactive, where 2021.3 to 6000.0 took a FindObjectsSortMode
        // there. Every shape starts with the Type; the search fills the rest by
        // parameter type. Tried in this order, the first one the build kept wins.
        constexpr const char* kFindInactive = "UnityEngine.FindObjectsInactive";
        constexpr const char* kFindSortMode = "UnityEngine.FindObjectsSortMode";
        struct FinderShape
        {
            const char* Class;
            const char* Method;
            const char* Parameters[3];
            std::size_t Count;
        };
        constexpr FinderShape kActiveFinders[] = {
            { kObjectClass, "FindObjectsOfType", { "System.Type" }, 1 },
            { kObjectClass, "FindObjectsByType", { "System.Type", kFindInactive }, 2 },
            { kObjectClass, "FindObjectsByType", { "System.Type", kFindInactive, kFindSortMode }, 3 },
            { kObjectClass, "FindObjectsByType", { "System.Type", kFindSortMode }, 2 },
            { kObjectClass, "FindObjectsOfType", { "System.Type", "System.Boolean" }, 2 },
        };
        // Resources.FindObjectsOfTypeAll also lists assets; the fallbacks after
        // it find inactive scene objects only.
        constexpr FinderShape kHiddenFinders[] = {
            { "UnityEngine.Resources", "FindObjectsOfTypeAll", { "System.Type" }, 1 },
            { kObjectClass, "FindObjectsByType", { "System.Type", kFindInactive }, 2 },
            { kObjectClass, "FindObjectsByType", { "System.Type", kFindInactive, kFindSortMode }, 3 },
            { kObjectClass, "FindObjectsOfType", { "System.Type", "System.Boolean" }, 2 },
        };
        // The walk over every Transform for scene roots: scene objects first,
        // Resources.FindObjectsOfTypeAll last (its assets have no valid scene
        // and drop out by their handle).
        constexpr FinderShape kSceneFinders[] = {
            { kObjectClass, "FindObjectsByType", { "System.Type", kFindInactive }, 2 },
            { kObjectClass, "FindObjectsByType", { "System.Type", kFindInactive, kFindSortMode }, 3 },
            { kObjectClass, "FindObjectsOfType", { "System.Type", "System.Boolean" }, 2 },
            { "UnityEngine.Resources", "FindObjectsOfTypeAll", { "System.Type" }, 1 },
        };
        constexpr std::size_t kMaxWalked = 100000;
        constexpr const char* kDontDestroyName = "DontDestroyOnLoad";

        struct HookCandidate
        {
            const char* Class;
            const char* Method;
            const char* Label;
        };
        constexpr HookCandidate kHooks[] = {
            { "UnityEngine.UnitySynchronizationContext", "ExecuteTasks", "UnitySynchronizationContext.ExecuteTasks" },
            { "UnityEngine.BeforeRenderHelper", "Invoke", "BeforeRenderHelper.Invoke" },
            { "UnityEngine.Canvas", "SendWillRenderCanvases", "Canvas.SendWillRenderCanvases" },
        };

        constexpr const char* kVectorGetters[] = { "get_localPosition", "get_localEulerAngles", "get_localScale", "get_position" };

        // ∑ Application.unityVersion is stripped in some builds (Dale&Dawson, Master Duel); the player's own
        //   file version carries the same major.minor.patch.
        std::string PlayerVersion(const Process& process)
        {
            std::optional<ModuleInfo> player = process.FindModule("UnityPlayer.dll");
            if (!player)
                player = process.MainModule();
            if (!player || player->Path.empty())
                return {};
            DWORD ignored = 0;
            const DWORD size = ::GetFileVersionInfoSizeW(player->Path.c_str(), &ignored);
            std::vector<std::uint8_t> data(size);
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (size == 0 || !::GetFileVersionInfoW(player->Path.c_str(), 0, size, data.data()) || !::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
                return {};
            const unsigned major = HIWORD(info->dwFileVersionMS);
            if (major < 4)
                return {};
            return std::to_string(major) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." + std::to_string(HIWORD(info->dwFileVersionLS));
        }
    }

    struct UnityExplorer::Il2Node
    {
        std::uint64_t Object = 0;
        std::uint64_t Transform = 0;
        std::string Name;
        bool Active = false;
        int ChildCount = 0;
    };

    // One scene found by the root walk (Il2WalkRoots).
    struct UnityExplorer::Il2SceneRoots
    {
        std::int32_t Handle = 0;
        std::vector<std::uint64_t> GameObjects;
        int Slot = 0;           // the boxed Scene, held when described
        std::string Name;
    };

    struct UnityExplorer::Il2Recipe
    {
        struct Arg
        {
            enum class Kind { Null, Value, Result, Missing };
            Kind What = Kind::Null;
            std::uint64_t Number = 0;
            int Call = -1;

            static Arg Null() { return {}; }
            static Arg Value(std::uint64_t value) { return { Kind::Value, value, -1 }; }
            static Arg Object(std::uint64_t address) { return address ? Arg{ Kind::Value, address, -1 } : Arg{ Kind::Missing, 0, -1 }; }
            static Arg Of(int call, std::uint64_t add = 0) { return call < 0 ? Arg{ Kind::Missing, 0, -1 } : Arg{ Kind::Result, add, call }; }
            bool Missing() const { return What == Kind::Missing; }
            FrameArg Frame() const { return What == Kind::Result ? FrameArg(Number, Call) : FrameArg(Number); }
        };

        struct Invoked
        {
            int Call = -1;
            std::uint64_t Exception = 0;
        };

        struct Kept
        {
            Arg Object;
            int Handle = -1;
            int Group = 0;
            std::uint64_t Address = 0;
            int Slot = 0;
        };

        struct Outcome
        {
            Invoked Call;
            std::uint64_t Cell = 0;
            int Kept = -1;
            std::string Error;
        };

        UnityExplorer& Owner;
        Il2CppExplorerState& State;
        FrameBatch Batch;
        MonoObjectLayout Layout;
        std::size_t Pointer = 8;
        std::vector<Kept> Holds;
        std::vector<int> Strings;

        explicit Il2Recipe(UnityExplorer& owner) : Owner(owner), State(*owner.m_il2), Batch(owner.m_il2->Frame.Batch())
        {
            Layout = MonoLayoutFor(static_cast<int>(owner.PointerBytes()));
            Pointer = static_cast<std::size_t>(Layout.Pointer);
        }

        Invoked Invoke(std::uint64_t method, Arg self, const std::vector<Arg>& parameters = {})
        {
            Invoked out;
            if (!method || self.Missing())
                return out;
            for (const Arg& parameter : parameters)
            {
                if (parameter.Missing())
                    return out;
            }
            std::uint64_t array = 0;
            if (!parameters.empty())
            {
                std::vector<std::uint8_t> cells(parameters.size() * Pointer, 0);
                for (std::size_t i = 0; i < parameters.size(); ++i)
                {
                    if (parameters[i].What == Arg::Kind::Value)
                        std::memcpy(cells.data() + i * Pointer, &parameters[i].Number, Pointer);
                }
                array = Batch.Cell(cells.size(), cells.data());
                if (!array)
                    return out;
                for (std::size_t i = 0; i < parameters.size(); ++i)
                {
                    if (parameters[i].What == Arg::Kind::Result)
                        Batch.Call(State.Frame.StoreFunction(), array + i * Pointer, parameters[i].Frame());
                }
            }
            out.Exception = Batch.Cell(Pointer);
            if (!out.Exception)
                return out;
            out.Call = Batch.Call(State.Invoke, method, self.Frame(), array, out.Exception);
            return out;
        }

        Arg Bytes(const void* data, std::size_t size)
        {
            return Arg::Object(Batch.Cell(size, data));
        }

        Arg Text(const std::string& text)
        {
            if (!State.StringNew)
                return Arg::Of(-1);
            const std::uint64_t cell = Batch.Cell(text.size() + 1, text.c_str());
            if (!cell)
                return Arg::Of(-1);
            const int call = Batch.Call(State.StringNew, cell);
            if (call >= 0)
                Strings.push_back(call);
            return Arg::Of(call);
        }

        Arg Argument(const AgentArg& argument)
        {
            switch (argument.Kind)
            {
            case AgentProtocol::ArgSlot: return Arg::Object(Owner.AddressOf(argument.Slot));
            case AgentProtocol::ArgBytes: return Bytes(argument.Bytes.data(), argument.Bytes.size());
            case AgentProtocol::ArgText: return Text(argument.Text);
            default: return Arg::Value(0);
            }
        }

        Arg This(Arg boxed) const
        {
            if (!State.BoxedThis || boxed.Missing() || boxed.What == Arg::Kind::Null)
                return Arg::Of(-1);
            if (!*State.BoxedThis)
                boxed.Number += Layout.ObjectHeader;
            return boxed;
        }

        int Hold(Arg object, int group = 0)
        {
            if (object.Missing() || object.What == Arg::Kind::Null)
                return -1;
            Kept kept;
            kept.Object = object;
            kept.Group = group;
            kept.Handle = Batch.Call(State.HandleNew, object.Frame(), 0);
            if (kept.Handle < 0)
                return -1;
            Holds.push_back(kept);
            return static_cast<int>(Holds.size()) - 1;
        }

        std::uint64_t Unbox(int call, std::size_t size)
        {
            if (call < 0)
                return 0;
            const std::uint64_t cell = Batch.Cell(size);
            if (cell)
                Batch.Call(State.Frame.CopyFunction(), cell, FrameArg(Layout.ObjectHeader, call), size);
            return cell;
        }

        std::uint64_t Chars(int call, std::size_t capacity)
        {
            if (call < 0)
                return 0;
            const std::uint64_t cell = Batch.Cell(8 + capacity * 2);
            if (cell)
                Batch.Call(State.Frame.TextFunction(), cell, FrameArg(Layout.StringLength, call), capacity);
            return cell;
        }

        int Load(std::uint64_t address)
        {
            return Batch.Call(State.Frame.LoadFunction(), address);
        }

        Outcome InvokeFor(std::uint64_t method, Arg self, const std::vector<Arg>& parameters, const MonoTypeInfo& type)
        {
            Outcome out;
            const auto process = Owner.Proc();
            if (!method || !process || !process->ReadPointer(method).value_or(0))
            {
                out.Error = "has no code of its own (abstract, generic or stripped)";
                return out;
            }
            const std::size_t size = static_cast<std::size_t>(std::max(1, type.Size));
            if (type.Code != MonoTypeCode::Void && type.ValueType && size > kMaxValueBytes)
            {
                out.Error = "its value is too large to read";
                return out;
            }
            out.Call = Invoke(method, self, parameters);
            if (out.Call.Call < 0)
            {
                out.Error = "could not be called from here";
                return out;
            }
            if (type.Code == MonoTypeCode::Void)
                return out;
            if (type.ValueType)
                out.Cell = Unbox(out.Call.Call, size);
            else if (type.Code == MonoTypeCode::String)
                out.Cell = Chars(out.Call.Call, kTextChars);
            else
                out.Kept = Hold(Arg::Of(out.Call.Call));
            return out;
        }

        struct Flag
        {
            Invoked Call;
            std::uint64_t Cell = 0;
            int Raw = -1;
        };

        Flag ReadBackground()
        {
            Flag out;
            if (const std::uint64_t get = Owner.Il2Method(kApplicationClass, "get_runInBackground", {}))
            {
                out.Call = Invoke(get, Arg::Null());
                out.Cell = Unbox(out.Call.Call, 1);
            }
            else if (const std::uint64_t icall = Owner.Il2Icall(kGetBackgroundIcall))
            {
                out.Raw = Batch.Call(icall);
            }
            return out;
        }

        std::optional<bool> Value(const Flag& flag) const
        {
            if (flag.Raw >= 0)
                return (Batch.Result(flag.Raw) & 0xFF) != 0;
            if (Returned(flag.Call))
                return Number<std::uint8_t>(flag.Cell) != 0;
            return std::nullopt;
        }

        bool CanSetBackground()
        {
            return Owner.Il2Method(kApplicationClass, "set_runInBackground", { "System.Boolean" }) || Owner.Il2Icall(kSetBackgroundIcall);
        }

        Invoked SetBackground(bool value)
        {
            const std::uint8_t byte = value ? 1 : 0;
            if (const std::uint64_t set = Owner.Il2Method(kApplicationClass, "set_runInBackground", { "System.Boolean" }))
                return Invoke(set, Arg::Null(), { Bytes(&byte, 1) });
            Invoked out;
            if (const std::uint64_t icall = Owner.Il2Icall(kSetBackgroundIcall))
                out.Call = Batch.Call(icall, static_cast<std::uint64_t>(byte));
            return out;
        }

        std::uint64_t Result(int call) const { return Batch.Result(call); }
        std::uint64_t Exception(const Invoked& invoked) const { return invoked.Exception ? Batch.Pointer(invoked.Exception) : 0; }
        bool Returned(const Invoked& invoked) const { return invoked.Call >= 0 && Batch.Result(invoked.Call) != 0; }
        std::uint64_t Address(int kept) const { return kept >= 0 && kept < static_cast<int>(Holds.size()) ? Holds[static_cast<std::size_t>(kept)].Address : 0; }

        template <typename T> T Number(std::uint64_t cell, T fallback = {}) const
        {
            return cell ? Batch.Value<T>(cell) : fallback;
        }

        std::string TextAt(std::uint64_t cell) const
        {
            if (!cell)
                return {};
            const std::uint32_t copied = Batch.Value<std::uint32_t>(cell);
            const std::uint32_t total = Batch.Value<std::uint32_t>(cell + 4);
            std::wstring wide(std::min<std::size_t>(copied, 0x10000), L'\0');
            if (!wide.empty() && !Batch.Read(cell + 8, wide.data(), wide.size() * 2))
                return {};
            std::string text = WideToUtf8(wide);
            if (total > copied)
                text += "...";
            return text;
        }

        bool StringsMade() const
        {
            return std::all_of(Strings.begin(), Strings.end(), [this](int call) { return Batch.Result(call) != 0; });
        }

        MonoReadValue Read(const Outcome& outcome, const MonoTypeInfo& type) const
        {
            MonoReadValue value;
            if (!outcome.Error.empty())
            {
                value.Error = outcome.Error;
                return value;
            }
            if (const std::uint64_t thrown = Exception(outcome.Call))
            {
                value.Error = Owner.Il2Thrown(thrown);
                return value;
            }
            value.Read = true;
            if (type.Code == MonoTypeCode::Void)
                return value;
            const std::uint64_t result = Batch.Result(outcome.Call.Call);
            if (type.ValueType)
            {
                value.Raw.assign(static_cast<std::size_t>(std::max(1, type.Size)), 0);
                if (result && outcome.Cell)
                    Batch.Read(outcome.Cell, value.Raw.data(), value.Raw.size());
                return value;
            }
            value.Raw.resize(Pointer);
            std::memcpy(value.Raw.data(), &result, Pointer);
            if (type.Code == MonoTypeCode::String && result)
                value.Text = TextAt(outcome.Cell);
            return value;
        }
    };

    // ---------------------------------------------------------------------
    // The frame stub and held objects

    std::uint64_t UnityExplorer::Il2Method(std::string_view className, std::string_view name, std::initializer_list<const char*> parameters)
    {
        return Il2Method(className, name, std::span<const char* const>(parameters.begin(), parameters.size()));
    }

    std::uint64_t UnityExplorer::Il2Method(std::string_view className, std::string_view name, std::span<const char* const> parameters)
    {
        std::string key = std::string(className) + "::" + std::string(name) + "(";
        for (const char* parameter : parameters)
            key += std::string(parameter) + ",";
        key += ")";
        if (const auto found = m_il2->Methods.find(key); found != m_il2->Methods.end())
            return found->second;
        std::uint64_t handle = 0;
        if (const auto details = m_mono.ClassDetails(className))
        {
            for (const MonoMethod& method : details->Methods)
            {
                if (method.Name != name || !method.Address)
                    continue;
                if (method.ParamCount >= 0 && method.ParamCount != static_cast<int>(parameters.size()))
                    continue;
                if (Worker("il2cpp_method_is_generic", method.Handle).value_or(0) & 0xFF)
                    continue;
                bool matches = true;
                std::uint64_t index = 0;
                for (const char* wanted : parameters)
                {
                    const auto type = Worker("il2cpp_method_get_param", method.Handle, index++);
                    if (!type)
                        break;
                    if (!*type || TypeInfo(*type).Name != wanted)
                    {
                        matches = false;
                        break;
                    }
                }
                if (matches)
                {
                    handle = method.Handle;
                    break;
                }
            }
        }
        if (!handle)
            LOG_INFO("Unity", "This game's build has no " + key);
        m_il2->Methods[key] = handle;
        return handle;
    }

    std::uint64_t UnityExplorer::Il2Icall(const std::string& name)
    {
        if (const auto found = m_il2->Icalls.find(name); found != m_il2->Icalls.end())
            return found->second;
        std::uint64_t address = 0;
        {
            std::lock_guard bridge(m_mono.m_lock);
            if (m_mono.m_present && m_mono.Fn("il2cpp_resolve_icall"))
            {
                m_mono.m_call.ResetScratch();
                if (const std::uint64_t text = m_mono.m_call.Scratch(name.size() + 1))
                {
                    m_mono.m_process->Write(text, name.c_str(), name.size() + 1);
                    address = m_mono.CallFn("il2cpp_resolve_icall", text).value_or(0);
                }
                m_mono.m_call.ResetScratch();
            }
        }
        if (address)
            LOG_INFO("Unity", "Using the engine's internal call " + name + ", the managed method is stripped");
        m_il2->Icalls[name] = address;
        return address;
    }

    Status UnityExplorer::Il2EnsureHook()
    {
        using Arg = Il2Recipe::Arg;
        if (!m_enabled.load())
            return Status::Fail(ErrorCode::MonoCallFailed, "the scene explorer is switched off");
        if (!Il2())
            return Status::Fail(ErrorCode::MonoCallFailed, "the scene explorer's frame stub needs an IL2CPP runtime");
        if (!m_il2)
            m_il2 = std::make_unique<Il2CppExplorerState>();
        Il2CppExplorerState& state = *m_il2;
        if (state.Ready && state.Frame.Hooked())
            return Status::Success();
        const auto process = Proc();
        if (!process)
            return Status::Fail(ErrorCode::NoProcess);

        std::uint64_t moduleBase = 0;
        std::uint64_t moduleEnd = 0;
        std::vector<std::string> missing;
        {
            std::lock_guard bridge(m_mono.m_lock);
            if (!m_mono.m_present)
                return Status::Fail(ErrorCode::NoProcess);
            moduleBase = m_mono.m_moduleBase;
            moduleEnd = m_mono.m_moduleEnd;
            const auto resolve = [&](const char* name)
                {
                    const std::uint64_t address = m_mono.Fn(name);
                    if (!address)
                        missing.emplace_back(name);
                    return address;
                };
            state.Invoke = resolve("il2cpp_runtime_invoke");
            state.HandleNew = resolve("il2cpp_gchandle_new");
            state.HandleFree = resolve("il2cpp_gchandle_free");
            state.StringNew = resolve("il2cpp_string_new");
            state.TypeGetObject = resolve("il2cpp_type_get_object");
            state.ObjectNew = resolve("il2cpp_object_new");
            state.ValueBox = resolve("il2cpp_value_box");
            state.FieldSet = resolve("il2cpp_field_set_value");
            state.FieldStaticSet = resolve("il2cpp_field_static_set_value");
        }
        if (!missing.empty())
            LOG_WARN("Unity", "This IL2CPP runtime does not export: " + Join(missing, ", "));
        {
            std::lock_guard lock(m_stateLock);
            m_status.Missing = missing;
        }
        if (!state.Invoke || !state.HandleNew || !state.HandleFree)
            return Status::Fail(ErrorCode::MonoCallFailed, "this IL2CPP runtime does not export il2cpp_runtime_invoke and the GC handle functions the explorer needs");

        const Status started = state.Frame.Start(process, [this](std::uint64_t function, std::uint64_t a0, std::uint64_t a1) -> std::optional<std::uint64_t>
            {
                std::lock_guard bridge(m_mono.m_lock);
                std::uint64_t result = 0;
                if (!m_mono.m_present || !m_mono.m_call.Call(function, a0, a1, result))
                    return std::nullopt;
                return result;
            });
        if (!started)
            return started;

        std::vector<std::string> tried;
        for (const HookCandidate& candidate : kHooks)
        {
            if (Aborted())
                return Status::Fail(ErrorCode::MonoCallFailed, "cancelled");
            const std::uint64_t method = Il2Method(candidate.Class, candidate.Method, {});
            if (!method)
                continue;
            const std::uint64_t code = process->ReadPointer(method).value_or(0);
            if (code < moduleBase || code >= moduleEnd)
                continue;
            if (const Status hooked = state.Frame.Hook(method, code); !hooked)
            {
                LOG_WARN("Unity", std::string(candidate.Label) + ": " + hooked.Text);
                continue;
            }
            tried.emplace_back(candidate.Label);
            const std::uint64_t before = state.Frame.Frames();
            const auto deadline = std::chrono::steady_clock::now() + kHookWait;
            bool alive = false;
            while (!alive && std::chrono::steady_clock::now() < deadline && !Aborted() && process->Alive())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                alive = state.Frame.Frames() > before;
            }
            if (alive)
            {
                state.Hook = candidate.Label;
                break;
            }
            state.Frame.Unhook();
        }
        if (!state.Frame.Hooked())
        {
            if (tried.empty())
                return Status::Fail(ErrorCode::MonoCallFailed, "this game has none of the per-frame Unity methods the explorer can sit on (UnitySynchronizationContext.ExecuteTasks, "
                                                               "BeforeRenderHelper.Invoke, Canvas.SendWillRenderCanvases)");
            return Status::Fail(ErrorCode::MonoCallFailed, "the game's main thread did not come through " + Join(tried, ", ") +
                                                               ". Bring the game to the foreground so it runs, then try again.");
        }

        Il2Recipe recipe(*this);
        std::uint64_t boxedCell = 0;
        std::uint64_t rawCell = 0;
        const std::uint64_t int32 = FindClass("System.Int32");
        const std::uint64_t hash = Il2Method("System.Int32", "GetHashCode", {});
        if (int32 && hash && state.ValueBox)
        {
            const Arg value = recipe.Bytes(&kProbeValue, sizeof(kProbeValue));
            const int box = recipe.Batch.Call(state.ValueBox, int32, value.Number);
            boxedCell = recipe.Unbox(recipe.Invoke(hash, Arg::Of(box)).Call, 4);
            rawCell = recipe.Unbox(recipe.Invoke(hash, Arg::Of(box, recipe.Layout.ObjectHeader)).Call, 4);
        }
        // ∑ Int32.GetHashCode is virtual; before metadata v29 a virtual struct method may sit behind an adjustor
        //   thunk where the explorer's own Scene methods do not. Scene.get_handle is the kind actually used,
        //   checked against the handle unboxed from GetActiveScene, and wins when it decides.
        std::uint64_t sceneCell = 0;
        std::uint64_t sceneBoxedCell = 0;
        std::uint64_t sceneRawCell = 0;
        const std::uint64_t getActive = Il2Method(kSceneManagerClass, "GetActiveScene", {});
        const std::uint64_t getHandle = Il2Method(kSceneClass, "get_handle", {});
        if (getActive && getHandle)
        {
            const Il2Recipe::Invoked active = recipe.Invoke(getActive, Arg::Null());
            sceneCell = recipe.Unbox(active.Call, 4);
            sceneBoxedCell = recipe.Unbox(recipe.Invoke(getHandle, Arg::Of(active.Call)).Call, 4);
            sceneRawCell = recipe.Unbox(recipe.Invoke(getHandle, Arg::Of(active.Call, recipe.Layout.ObjectHeader)).Call, 4);
        }
        const std::uint64_t versionCell = recipe.Chars(recipe.Invoke(Il2Method(kApplicationClass, "get_unityVersion", {}), Arg::Null()).Call, 64);
        const Il2Recipe::Flag background = recipe.ReadBackground();
        if (const Status ran = Il2Run(recipe, std::chrono::seconds(5)); !ran)
        {
            state.Frame.Unhook();
            return Status::Fail(ErrorCode::MonoCallFailed, "the frame stub did not answer its first request: " + ran.Text);
        }
        const std::int32_t sceneHandle = recipe.Number<std::int32_t>(sceneCell);
        const bool sceneBoxed = sceneHandle != 0 && recipe.Number<std::int32_t>(sceneBoxedCell) == sceneHandle;
        const bool sceneRaw = sceneHandle != 0 && recipe.Number<std::int32_t>(sceneRawCell) == sceneHandle;
        if (sceneBoxed != sceneRaw)
            state.BoxedThis = sceneBoxed;
        else if (recipe.Number<std::int32_t>(boxedCell) == kProbeValue)
            state.BoxedThis = true;
        else if (recipe.Number<std::int32_t>(rawCell) == kProbeValue)
            state.BoxedThis = false;
        else
        {
            state.BoxedThis.reset();
            LOG_WARN("Unity", "Could not find out how this IL2CPP version passes a struct to its own methods; scenes cannot be read");
        }
        state.Ready = true;
        std::string version = recipe.TextAt(versionCell);
        if (version.empty())
            version = PlayerVersion(*process);
        {
            std::lock_guard lock(m_stateLock);
            if (!m_status.Hooked)
                m_status.HookedSince = std::chrono::steady_clock::now();
            m_status.Hooked = true;
            m_status.Hook = state.Hook;
            m_status.UnityVersion = version;
            if (const auto running = recipe.Value(background))
                m_status.RunInBackground = *running;
            m_status.CallbackRuns = state.Frame.Frames();
            m_status.MainThread = state.Frame.Thread();
            m_status.Error.clear();
        }
        LOG_INFO("Unity", "The frame stub answers from " + state.Hook + " (Unity " + version + ", structs passed " +
                              (state.BoxedThis ? (*state.BoxedThis ? "boxed" : "as data") : "in an unknown way") + ")");
        Publish();
        return Status::Success();
    }

    Status UnityExplorer::Il2Run(Il2Recipe& recipe, std::chrono::milliseconds timeout, bool cancellable)
    {
        Il2CppExplorerState& state = *m_il2;
        std::size_t freed = 0;
        while (freed < state.Surplus.size() && recipe.Batch.Fits(1, 0))
        {
            recipe.Batch.Call(state.HandleFree, state.Surplus[state.Surplus.size() - 1 - freed]);
            ++freed;
        }
        if (recipe.Batch.Overflowed())
            return Status::Fail(ErrorCode::MonoCallFailed, "the request does not fit one batch");
        const std::size_t calls = recipe.Batch.Calls();
        const Result<FrameCalls::Ran> ran = state.Frame.Run(recipe.Batch, timeout, cancellable ? std::function<bool()>([this] { return Aborted(); }) : std::function<bool()>());
        ++m_statTrips;
        m_statCalls += static_cast<int>(calls);
        const std::size_t executed = state.Frame.Executed();
        const std::size_t released = ran ? freed : std::min(freed, executed > calls - freed ? executed - (calls - freed) : 0);
        state.Surplus.resize(state.Surplus.size() - released);
        {
            std::lock_guard lock(m_stateLock);
            m_status.CallbackRuns = state.Frame.Frames();
            m_status.MainThread = state.Frame.Thread();
        }
        if (!ran)
        {
            if (executed > 0)
            {
                for (Il2Recipe::Kept& kept : recipe.Holds)
                    kept.Group = kStoppedGroup;
                Il2Keep(recipe, kStoppedGroup);
                for (Il2Recipe::Kept& kept : recipe.Holds)
                    kept = {};
                Il2Release(kStoppedGroup);
            }
            return ran.Error();
        }
        m_statFrames += static_cast<int>(ran->Frames);
        return Status::Success();
    }

    void UnityExplorer::Il2Keep(Il2Recipe& recipe, int group)
    {
        for (Il2Recipe::Kept& kept : recipe.Holds)
        {
            const std::uint64_t handle = recipe.Result(kept.Handle);
            if (!handle)
                continue;
            const std::uint64_t address = kept.Object.What == Il2Recipe::Arg::Kind::Value ? kept.Object.Number : recipe.Result(kept.Object.Call) + kept.Object.Number;
            if (!address)
                continue;
            kept.Address = address;
            kept.Slot = Il2Register(address, handle, kept.Group ? kept.Group : group);
        }
    }

    int UnityExplorer::Il2Register(std::uint64_t address, std::uint64_t handle, int group)
    {
        Il2CppExplorerState& state = *m_il2;
        if (const int known = SlotOf(address))
        {
            if (const auto found = state.Slots.find(known); found != state.Slots.end())
            {
                state.Surplus.push_back(handle);
                if (std::find(found->second.Groups.begin(), found->second.Groups.end(), group) == found->second.Groups.end())
                    found->second.Groups.push_back(group);
                return known;
            }
        }
        const int slot = state.NextSlot++;
        Il2CppHeld held;
        held.Handle = handle;
        held.Address = address;
        held.Groups.push_back(group);
        state.Slots.emplace(slot, std::move(held));
        std::lock_guard lock(m_stateLock);
        m_slotAddress[slot] = address;
        m_addressSlot[address] = slot;
        return slot;
    }

    void UnityExplorer::Il2Hold(int group, const std::vector<int>& slots)
    {
        if (!m_il2)
            return;
        for (const int slot : slots)
        {
            const auto found = m_il2->Slots.find(slot);
            if (found != m_il2->Slots.end() && std::find(found->second.Groups.begin(), found->second.Groups.end(), group) == found->second.Groups.end())
                found->second.Groups.push_back(group);
        }
    }

    void UnityExplorer::Il2Release(int group)
    {
        if (!m_il2)
            return;
        Il2CppExplorerState& state = *m_il2;
        std::lock_guard lock(m_stateLock);
        for (auto it = state.Slots.begin(); it != state.Slots.end();)
        {
            std::vector<int>& groups = it->second.Groups;
            const auto member = std::find(groups.begin(), groups.end(), group);
            if (member == groups.end())
            {
                ++it;
                continue;
            }
            groups.erase(member);
            if (!groups.empty())
            {
                ++it;
                continue;
            }
            state.Surplus.push_back(it->second.Handle);
            if (const auto back = m_addressSlot.find(it->second.Address); back != m_addressSlot.end() && back->second == it->first)
                m_addressSlot.erase(back);
            m_slotAddress.erase(it->first);
            if (state.DontDestroySlot == it->first)
                state.DontDestroySlot = 0;
            it = state.Slots.erase(it);
        }
    }

    std::string UnityExplorer::Il2Thrown(std::uint64_t exception)
    {
        const std::string name = ClassNameOf(exception);
        return name.empty() ? std::string("threw an exception") : "threw " + name;
    }

    void UnityExplorer::Il2Shutdown(bool alive)
    {
        if (!m_il2)
            return;
        Il2CppExplorerState& state = *m_il2;
        if (!alive || !state.Frame.Started())
        {
            state.Frame.Forget();
            m_il2.reset();
            return;
        }
        std::vector<std::uint64_t> handles = std::move(state.Surplus);
        state.Surplus.clear();
        for (const auto& [slot, held] : state.Slots)
            handles.push_back(held.Handle);
        state.Slots.clear();
        const bool hooked = state.Frame.Hooked();
        bool onFrame = hooked;
        bool restored = !state.OriginalRunInBackground;
        std::size_t next = 0;
        while (onFrame && (next < handles.size() || !restored))
        {
            Il2Recipe recipe(*this);
            if (!restored)
                recipe.SetBackground(*state.OriginalRunInBackground);
            const std::size_t before = recipe.Batch.Calls();
            std::size_t planned = next;
            while (planned < handles.size() && recipe.Batch.Fits(1, 0))
                recipe.Batch.Call(state.HandleFree, handles[planned++]);
            if (!Il2Run(recipe, kShutdownWait, false))
            {
                // The main thread may have run part of the batch before it stopped answering;
                // those handles are free already and must not be freed again below.
                const std::size_t executed = state.Frame.Executed();
                if (executed >= before)
                {
                    restored = true;
                    next += std::min(executed - before, planned - next);
                }
                onFrame = false;
                break;
            }
            restored = true;
            next = planned;
        }
        if (!restored)
            LOG_WARN("Unity", "The game's main thread did not answer, so Application.runInBackground stays as the explorer set it");
        state.Frame.Unhook();
        while (next < handles.size())
        {
            FrameBatch batch = state.Frame.Batch();
            std::size_t planned = next;
            while (planned < handles.size() && batch.Fits(1, 0))
                batch.Call(state.HandleFree, handles[planned++]);
            if (!state.Frame.RunOnWorker(batch))
            {
                LOG_WARN("Unity", Manifold::Format("%zu objects stay held in the game: the helper could not release them", handles.size() - next));
                break;
            }
            next = planned;
        }
        m_il2.reset();
    }

    // ---------------------------------------------------------------------
    // Class members

    std::vector<std::uint64_t> UnityExplorer::Il2Drain(const char* function, std::uint64_t object)
    {
        std::vector<std::uint64_t> out;
        std::vector<RemoteCall::DrainItem> items;
        {
            std::lock_guard bridge(m_mono.m_lock);
            const std::uint64_t address = m_mono.m_present ? m_mono.Fn(function) : 0;
            if (!address || !m_mono.m_call.Drain(address, object, items, RemoteCall::kNoPeek, 0, static_cast<std::size_t>(kMaxMembers)))
                return out;
        }
        out.reserve(items.size());
        for (const RemoteCall::DrainItem& item : items)
            out.push_back(item.Item);
        return out;
    }

    void UnityExplorer::Il2LoadMembers(MonoClassLayout& layout, std::uint64_t klass)
    {
        const auto process = Proc();
        if (!process)
            return;
        const std::size_t pointer = static_cast<std::size_t>(process->PointerSize());
        const std::size_t mark = m_argsUsed;
        const std::uint64_t literal = ArgSpace(64);
        const auto flagsOf = [&](std::uint64_t method)
            {
                return static_cast<std::uint32_t>(Worker("il2cpp_method_get_flags", method, 0).value_or(0) & 0xFFFFFFFFu);
            };
        const auto parameter = [&](std::uint64_t method, int index)
            {
                MonoTypeInfo info;
                if (const auto type = Worker("il2cpp_method_get_param", method, static_cast<std::uint64_t>(index)); type && *type)
                    info = TypeInfo(*type);
                if (!info.Name.empty() && info.Name.back() == '&')
                {
                    info.Code = MonoTypeCode::ByRef;
                    info.ValueCode = MonoTypeCode::ByRef;
                    info.ValueType = false;
                    info.Enum = false;
                    info.Size = static_cast<int>(pointer);
                }
                return info;
            };
        std::uint64_t current = klass;
        for (int depth = 0; current && depth < 48; ++depth, current = Worker("mono_class_get_parent", current).value_or(0))
        {
            const std::string declaring = ClassName(current);
            if (declaring == "System.Object" || declaring == "System.ValueType" || declaring == "System.Enum")
                break;
            const std::uint64_t staticData = Worker("il2cpp_class_get_static_field_data", current).value_or(0);

            for (const std::uint64_t field : Il2Drain("il2cpp_class_get_fields", current))
            {
                MonoMember member;
                member.Kind = MonoMemberKind::Field;
                member.Handle = field;
                member.Declaring = declaring;
                member.Name = WorkerString("il2cpp_field_get_name", field);
                const std::uint32_t flags = static_cast<std::uint32_t>(Worker("il2cpp_field_get_flags", field).value_or(0) & 0xFFFFFFFFu);
                member.Static = (flags & kFieldStatic) != 0;
                member.Literal = (flags & kFieldLiteral) != 0;
                member.Offset = static_cast<std::int32_t>(Worker("il2cpp_field_get_offset", field).value_or(0) & 0xFFFFFFFFu);
                if (const auto type = Worker("il2cpp_field_get_type", field); type && *type)
                    member.Type = TypeInfo(*type);
                if (member.Static)
                {
                    member.StaticData = staticData;
                    member.VTable = current;
                }
                if (layout.Enum && depth == 0)
                {
                    if (member.Literal && literal && member.Type.Size <= 8 && m_mono.Fn("il2cpp_field_static_get_value"))
                    {
                        const std::uint8_t clear[8] = {};
                        process->Write(literal, clear, sizeof(clear));
                        Worker("il2cpp_field_static_get_value", field, literal);
                        member.LiteralBytes.resize(static_cast<std::size_t>(std::max(1, member.Type.Size)));
                        process->ReadExact(literal, member.LiteralBytes.data(), member.LiteralBytes.size());
                        std::int64_t value = 0;
                        std::memcpy(&value, member.LiteralBytes.data(), member.LiteralBytes.size());
                        const int size = static_cast<int>(member.LiteralBytes.size());
                        if (size < 8 && (member.Type.ValueCode == MonoTypeCode::I1 || member.Type.ValueCode == MonoTypeCode::I2 || member.Type.ValueCode == MonoTypeCode::I4))
                            value = (value << (64 - size * 8)) >> (64 - size * 8);
                        layout.EnumValues.push_back({ value, member.Name });
                    }
                    if (!member.Static && member.Name == "value__")
                        continue;
                }
                layout.Members.push_back(std::move(member));
            }

            std::vector<std::string> propertyNames;
            for (const std::uint64_t property : Il2Drain("il2cpp_class_get_properties", current))
            {
                MonoMember member;
                member.Kind = MonoMemberKind::Property;
                member.Handle = property;
                member.Declaring = declaring;
                member.Name = WorkerString("il2cpp_property_get_name", property);
                member.Getter = Worker("il2cpp_property_get_get_method", property).value_or(0);
                member.Setter = Worker("il2cpp_property_get_set_method", property).value_or(0);
                propertyNames.push_back(member.Name);
                const std::uint64_t accessor = member.Getter ? member.Getter : member.Setter;
                if (!accessor)
                    continue;
                const int count = static_cast<int>(Worker("il2cpp_method_get_param_count", accessor).value_or(0) & 0xFFFFu);
                if (member.Getter)
                {
                    if (count != 0)
                        continue;
                    if (const auto type = Worker("il2cpp_method_get_return_type", accessor); type && *type)
                        member.Type = TypeInfo(*type);
                }
                else
                {
                    if (count != 1)
                        continue;
                    member.Type = parameter(accessor, 0);
                }
                member.Static = (flagsOf(accessor) & kMethodStatic) != 0;
                layout.Members.push_back(std::move(member));
            }

            for (const std::uint64_t method : Il2Drain("il2cpp_class_get_methods", current))
            {
                MonoMember member;
                member.Kind = MonoMemberKind::Method;
                member.Handle = method;
                member.Declaring = declaring;
                member.Name = WorkerString("il2cpp_method_get_name", method);
                if (member.Name == ".ctor" || member.Name == ".cctor")
                    continue;
                if (member.Name.size() > 4 && (member.Name.rfind("get_", 0) == 0 || member.Name.rfind("set_", 0) == 0) &&
                    std::find(propertyNames.begin(), propertyNames.end(), member.Name.substr(4)) != propertyNames.end())
                    continue;
                member.Static = (flagsOf(method) & kMethodStatic) != 0;
                if (const auto type = Worker("il2cpp_method_get_return_type", method); type && *type)
                    member.Type = TypeInfo(*type);
                const int count = static_cast<int>(Worker("il2cpp_method_get_param_count", method).value_or(0) & 0xFFFFu);
                for (int p = 0; p < count && p < 64; ++p)
                {
                    member.Params.push_back(parameter(method, p));
                    const auto name = Worker("il2cpp_method_get_param_name", method, static_cast<std::uint64_t>(p));
                    member.ParamNames.push_back(name && *name ? m_mono.m_call.ReadCString(*name, 128) : std::string());
                }
                layout.Members.push_back(std::move(member));
            }
        }
        m_argsUsed = mark;
    }

    void UnityExplorer::Il2RefreshStatics(std::uint64_t klass)
    {
        std::shared_ptr<const MonoClassLayout> layout;
        {
            std::lock_guard lock(m_stateLock);
            if (const auto found = m_layouts.find(klass); found != m_layouts.end())
                layout = found->second;
        }
        if (!layout)
            return;
        std::vector<std::uint64_t> waiting;
        for (const MonoMember& member : layout->Members)
        {
            if (member.Kind == MonoMemberKind::Field && member.Static && !member.Literal && !member.StaticData && member.VTable &&
                std::find(waiting.begin(), waiting.end(), member.VTable) == waiting.end())
                waiting.push_back(member.VTable);
        }
        for (const std::uint64_t declaring : waiting)
        {
            if (!Worker("il2cpp_class_get_static_field_data", declaring).value_or(0))
                continue;
            std::lock_guard lock(m_stateLock);
            m_layouts.erase(klass);
            return;
        }
    }

    // ---------------------------------------------------------------------
    // Scenes

    Status UnityExplorer::Il2Nodes(const std::vector<std::uint64_t>& gameObjects, std::uint64_t parentTransform, std::size_t children, int group, std::vector<Il2Node>& out)
    {
        using Arg = Il2Recipe::Arg;
        const std::uint64_t getName = Il2Method(kObjectClass, "get_name", {});
        const std::uint64_t getTransform = Il2Method(kGameObjectClass, "get_transform", {});
        const std::uint64_t getActive = Il2Method(kGameObjectClass, "get_activeSelf", {});
        const std::uint64_t getChildCount = Il2Method(kTransformClass, "get_childCount", {});
        const std::uint64_t getChild = Il2Method(kTransformClass, "GetChild", { "System.Int32" });
        const std::uint64_t getGameObject = Il2Method(kComponentClass, "get_gameObject", {});
        if (!getTransform)
            return Status::Fail(ErrorCode::MonoCallFailed, std::string("this game's build has no GameObject.transform") + kStripped);
        if (parentTransform && (!getChild || !getGameObject))
            return Status::Fail(ErrorCode::MonoCallFailed, std::string("this game's build has no Transform.GetChild or Component.gameObject") + kStripped);
        struct Plan
        {
            int Object = -1;
            int Transform = -1;
            Il2Recipe::Invoked Active;
            std::uint64_t Name = 0;
            std::uint64_t ActiveCell = 0;
            std::uint64_t Children = 0;
        };
        const std::size_t count = parentTransform ? children : gameObjects.size();
        std::size_t next = 0;
        while (next < count)
        {
            if (Aborted())
                return Status::Fail(ErrorCode::MonoCallFailed, "cancelled");
            Il2Recipe recipe(*this);
            std::vector<Plan> plans;
            while (next < count && recipe.Batch.Fits(16, 768))
            {
                const std::size_t index = next++;
                Arg gameObject = Arg::Of(-1);
                Arg transform = Arg::Of(-1);
                if (parentTransform)
                {
                    const std::int32_t child = static_cast<std::int32_t>(index);
                    transform = Arg::Of(recipe.Invoke(getChild, Arg::Object(parentTransform), { recipe.Bytes(&child, sizeof(child)) }).Call);
                    gameObject = Arg::Of(recipe.Invoke(getGameObject, transform).Call);
                }
                else
                {
                    gameObject = Arg::Object(gameObjects[index]);
                    transform = Arg::Of(recipe.Invoke(getTransform, gameObject).Call);
                }
                Plan plan;
                plan.Object = recipe.Hold(gameObject);
                plan.Transform = recipe.Hold(transform);
                plan.Name = recipe.Chars(recipe.Invoke(getName, gameObject).Call, kNameChars);
                plan.Active = recipe.Invoke(getActive, gameObject);
                plan.ActiveCell = recipe.Unbox(plan.Active.Call, 1);
                plan.Children = recipe.Unbox(recipe.Invoke(getChildCount, transform).Call, 4);
                plans.push_back(plan);
            }
            if (const Status ran = Il2Run(recipe, kListWait); !ran)
                return ran;
            Il2Keep(recipe, group);
            for (const Plan& plan : plans)
            {
                Il2Node node;
                node.Object = recipe.Address(plan.Object);
                node.Transform = recipe.Address(plan.Transform);
                node.Name = recipe.TextAt(plan.Name);
                node.Active = recipe.Returned(plan.Active) ? recipe.Number<std::uint8_t>(plan.ActiveCell) != 0 : true;
                node.ChildCount = std::max(0, recipe.Number<std::int32_t>(plan.Children));
                if (node.Object)
                    out.push_back(std::move(node));
            }
        }
        return Status::Success();
    }

    // The scenes of a build that stripped SceneManager.sceneCount/GetSceneAt or
    // Scene.GetRootGameObjects (Unity 6000.6 drops them when the game itself
    // never calls them): every Transform without a parent is a root, and its
    // GameObject's scene says where it belongs. Roots without a valid scene
    // (assets, HideAndDontSave objects) drop out; scenes come sorted by
    // handle. With describe, each scene's boxed Scene is held in group and its
    // name read.
    Status UnityExplorer::Il2WalkRoots(bool describe, int group, std::vector<Il2SceneRoots>& scenes)
    {
        using Arg = Il2Recipe::Arg;
        scenes.clear();
        const std::uint64_t transformClass = FindClass(kTransformClass);
        const std::uint64_t getParent = Il2Method(kTransformClass, "get_parent", {});
        const std::uint64_t getGameObject = Il2Method(kComponentClass, "get_gameObject", {});
        const std::uint64_t getScene = Il2Method(kGameObjectClass, "get_scene", {});
        const std::uint64_t getName = Il2Method(kSceneClass, "get_name", {});
        if (!transformClass || !getParent || !getGameObject || (!getScene && describe))
            return Status::Fail(ErrorCode::MonoCallFailed, std::string("this game's build has none of SceneManager.GetSceneAt, Scene.GetRootGameObjects, "
                                                                       "and one of Transform.parent, Component.gameObject or GameObject.scene is missing too") + kStripped);
        // ∑ Without GameObject.scene the roots cannot be told apart by scene, and Resources.FindObjectsOfTypeAll
        //   would mix in prefabs, so only the active scene objects are walked; they all land under handle 0.
        std::size_t total = 0;
        std::vector<std::uint64_t> transforms;
        const Status found = Il2FindObjects(transformClass, getScene != 0, false, kMaxWalked, transforms, total);
        const auto done = [&](Status status)
            {
                Il2Release(kScratchGroup);
                return status;
            };
        if (!found)
            return done(found);
        if (total > transforms.size())
            LOG_WARN("Unity", "The scene walk looked at the first " + std::to_string(transforms.size()) + " of " + std::to_string(total) + " Transforms only");

        // Which Transforms have no parent.
        std::vector<std::uint64_t> roots;
        std::size_t next = 0;
        while (next < transforms.size())
        {
            if (Aborted())
                return done(Status::Fail(ErrorCode::MonoCallFailed, "cancelled"));
            Il2Recipe recipe(*this);
            std::vector<std::pair<std::uint64_t, Il2Recipe::Invoked>> plans;
            while (next < transforms.size() && recipe.Batch.Fits(4, 64))
            {
                const std::uint64_t transform = transforms[next++];
                if (transform)
                    plans.emplace_back(transform, recipe.Invoke(getParent, Arg::Object(transform)));
            }
            if (const Status ran = Il2Run(recipe, kListWait); !ran)
                return done(ran);
            for (const auto& [transform, parent] : plans)
            {
                if (parent.Call >= 0 && !recipe.Exception(parent) && !recipe.Result(parent.Call))
                    roots.push_back(transform);
            }
        }

        // Each root's GameObject and scene handle.
        std::map<std::int32_t, Il2SceneRoots> byHandle;
        next = 0;
        while (next < roots.size())
        {
            if (Aborted())
                return done(Status::Fail(ErrorCode::MonoCallFailed, "cancelled"));
            Il2Recipe recipe(*this);
            struct Plan
            {
                Il2Recipe::Invoked GameObject;
                Il2Recipe::Invoked Scene;
                std::uint64_t Handle = 0;
            };
            std::vector<Plan> plans;
            while (next < roots.size() && recipe.Batch.Fits(8, 128))
            {
                Plan plan;
                plan.GameObject = recipe.Invoke(getGameObject, Arg::Object(roots[next++]));
                if (getScene)
                {
                    plan.Scene = recipe.Invoke(getScene, Arg::Of(plan.GameObject.Call));
                    plan.Handle = recipe.Unbox(plan.Scene.Call, 4);
                }
                plans.push_back(plan);
            }
            if (const Status ran = Il2Run(recipe, kListWait); !ran)
                return done(ran);
            for (const Plan& plan : plans)
            {
                const std::uint64_t gameObject = recipe.Returned(plan.GameObject) ? recipe.Result(plan.GameObject.Call) : 0;
                if (!gameObject || (getScene && (!recipe.Returned(plan.Scene) || recipe.Exception(plan.Scene))))
                    continue;
                const std::int32_t handle = getScene ? recipe.Number<std::int32_t>(plan.Handle) : 0;
                if (handle == 0 && getScene)
                    continue;
                Il2SceneRoots& scene = byHandle[handle];
                scene.Handle = handle;
                scene.GameObjects.push_back(gameObject);
            }
        }
        for (auto& [handle, scene] : byHandle)
            scenes.push_back(std::move(scene));
        if (!describe || scenes.empty())
            return done(Status::Success());

        // One boxed Scene per scene, through its first root, and its name.
        Il2Recipe recipe(*this);
        std::vector<std::pair<int, std::uint64_t>> plans;
        for (const Il2SceneRoots& scene : scenes)
        {
            const Il2Recipe::Invoked boxed = recipe.Invoke(getScene, Arg::Object(scene.GameObjects.front()));
            const int kept = recipe.Hold(Arg::Of(boxed.Call));
            const std::uint64_t name = getName ? recipe.Chars(recipe.Invoke(getName, recipe.This(Arg::Of(boxed.Call))).Call, kNameChars) : 0;
            plans.emplace_back(kept, name);
        }
        if (const Status ran = Il2Run(recipe, kCallWait); !ran)
            return done(ran);
        Il2Keep(recipe, group);
        for (std::size_t i = 0; i < scenes.size(); ++i)
        {
            const auto [kept, name] = plans[i];
            scenes[i].Slot = kept >= 0 ? recipe.Holds[static_cast<std::size_t>(kept)].Slot : 0;
            scenes[i].Name = recipe.TextAt(name);
        }
        return done(Status::Success());
    }

    // ∑ Scene.get_name is stripped in some builds (The Survivalists, Master Duel); the static
    //   Scene.GetNameInternal(int) or, failing that, the engine's internal call behind it still answers.
    std::map<std::int32_t, std::string> UnityExplorer::Il2SceneNames(const std::vector<std::int32_t>& handles)
    {
        using Arg = Il2Recipe::Arg;
        std::map<std::int32_t, std::string> names;
        if (handles.empty())
            return names;
        const std::uint64_t method = Il2Method(kSceneClass, "GetNameInternal", { "System.Int32" });
        const std::uint64_t icall = method ? 0 : Il2Icall("UnityEngine.SceneManagement.Scene::GetNameInternal(System.Int32)");
        if (!method && !icall)
            return names;
        Il2Recipe recipe(*this);
        std::vector<std::pair<std::int32_t, std::uint64_t>> cells;
        for (const std::int32_t handle : handles)
        {
            if (!handle)
                continue;
            const int call = method ? recipe.Invoke(method, Arg::Null(), { recipe.Bytes(&handle, sizeof(handle)) }).Call
                                    : recipe.Batch.Call(icall, static_cast<std::uint64_t>(static_cast<std::uint32_t>(handle)));
            cells.emplace_back(handle, recipe.Chars(call, kNameChars));
        }
        if (!Il2Run(recipe, kCallWait))
            return names;
        for (const auto& [handle, cell] : cells)
        {
            if (std::string name = recipe.TextAt(cell); !name.empty())
                names[handle] = std::move(name);
        }
        return names;
    }

    void UnityExplorer::Il2ReadScenes()
    {
        using Arg = Il2Recipe::Arg;
        {
            std::lock_guard state(m_stateLock);
            m_status.Error.clear();
        }
        if (Status ready = Il2EnsureHook(); !ready)
            return Fail(ready.Text);
        Il2Release(kTreeGroup);
        const std::uint64_t getCount = Il2Method(kSceneManagerClass, "get_sceneCount", {});
        const std::uint64_t getAt = Il2Method(kSceneManagerClass, "GetSceneAt", { "System.Int32" });
        const std::uint64_t getActive = Il2Method(kSceneManagerClass, "GetActiveScene", {});
        if (!m_il2->BoxedThis)
            return Fail("Could not read the scenes: Manifold could not find out how this IL2CPP version passes a struct to its own methods");
        UnityTree tree;
        tree.Read = true;
        if (!getCount || !getAt)
        {
            // A stripped build: the scenes come from the root walk.
            std::int32_t active = 0;
            if (getActive)
            {
                Il2Recipe first(*this);
                const std::uint64_t activeCell = first.Unbox(first.Invoke(getActive, Arg::Null()).Call, 4);
                if (const Status ran = Il2Run(first, kCallWait); !ran)
                    return Fail("Could not read the scenes: " + ran.Text);
                active = first.Number<std::int32_t>(activeCell);
            }
            if (!m_dontDestroyHandle)
            {
                int slot = 0;
                int handle = 0;
                Il2DontDestroyScene(slot, handle);
            }
            std::vector<Il2SceneRoots> found;
            if (const Status walked = Il2WalkRoots(true, kTreeGroup, found); !walked)
                return Fail("Could not read the scenes: " + walked.Text);
            std::vector<std::int32_t> nameless;
            for (const Il2SceneRoots& read : found)
            {
                if (read.Name.empty())
                    nameless.push_back(read.Handle);
            }
            const std::map<std::int32_t, std::string> named = Il2SceneNames(nameless);
            for (Il2SceneRoots& read : found)
            {
                if (const auto it = named.find(read.Handle); read.Name.empty() && it != named.end())
                    read.Name = it->second;
            }
            for (Il2SceneRoots& read : found)
            {
                // DontDestroyOnLoad has its own entry below.
                if (read.Name == kDontDestroyName || (m_dontDestroyHandle && read.Handle == m_dontDestroyHandle))
                    continue;
                UnityScene scene;
                scene.Slot = read.Slot;
                scene.Handle = read.Handle;
                scene.Name = read.Name.empty() ? "Scene #" + std::to_string(read.Handle) : std::move(read.Name);
                scene.BuildIndex = -1;
                scene.RootCount = static_cast<int>(read.GameObjects.size());
                scene.Loaded = true;
                scene.Active = scene.Handle == active;
                if (scene.Slot)
                    tree.Scenes.push_back(std::move(scene));
            }
        }
        else
        {
            Il2Recipe first(*this);
            const std::uint64_t countCell = first.Unbox(first.Invoke(getCount, Arg::Null()).Call, 4);
            const std::uint64_t activeCell = first.Unbox(first.Invoke(getActive, Arg::Null()).Call, 4);
            if (const Status ran = Il2Run(first, kCallWait); !ran)
                return Fail("Could not read the scenes: " + ran.Text);
            const std::size_t count = static_cast<std::size_t>(std::clamp(first.Number<std::int32_t>(countCell), 0, static_cast<std::int32_t>(kMaxScenes)));
            const std::int32_t active = first.Number<std::int32_t>(activeCell);

            const std::uint64_t getName = Il2Method(kSceneClass, "get_name", {});
            const std::uint64_t getBuildIndex = Il2Method(kSceneClass, "get_buildIndex", {});
            const std::uint64_t getRootCount = Il2Method(kSceneClass, "get_rootCount", {});
            const std::uint64_t getLoaded = Il2Method(kSceneClass, "get_isLoaded", {});
            struct Plan
            {
                int Scene = -1;
                std::uint64_t Handle = 0;
                std::uint64_t Name = 0;
                std::uint64_t BuildIndex = 0;
                std::uint64_t RootCount = 0;
                std::uint64_t Loaded = 0;
            };
            std::vector<Plan> plans;
            Il2Recipe second(*this);
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::int32_t index = static_cast<std::int32_t>(i);
                const Arg scene = Arg::Of(second.Invoke(getAt, Arg::Null(), { second.Bytes(&index, sizeof(index)) }).Call);
                const Arg self = second.This(scene);
                Plan plan;
                plan.Scene = second.Hold(scene);
                plan.Handle = second.Unbox(scene.Call, 4);
                plan.Name = second.Chars(second.Invoke(getName, self).Call, kNameChars);
                plan.BuildIndex = second.Unbox(second.Invoke(getBuildIndex, self).Call, 4);
                plan.RootCount = second.Unbox(second.Invoke(getRootCount, self).Call, 4);
                plan.Loaded = second.Unbox(second.Invoke(getLoaded, self).Call, 1);
                plans.push_back(plan);
            }
            if (!plans.empty())
            {
                if (const Status ran = Il2Run(second, kCallWait); !ran)
                    return Fail("Could not read the scenes: " + ran.Text);
                Il2Keep(second, kTreeGroup);
            }
            for (const Plan& plan : plans)
            {
                UnityScene scene;
                scene.Slot = plan.Scene >= 0 ? second.Holds[static_cast<std::size_t>(plan.Scene)].Slot : 0;
                scene.Handle = second.Number<std::int32_t>(plan.Handle);
                scene.Name = second.TextAt(plan.Name);
                scene.BuildIndex = second.Number<std::int32_t>(plan.BuildIndex, -1);
                scene.RootCount = second.Number<std::int32_t>(plan.RootCount, -1);
                scene.Loaded = plan.Loaded ? second.Number<std::uint8_t>(plan.Loaded) != 0 : true;
                scene.Active = scene.Handle == active;
                if (scene.Slot)
                    tree.Scenes.push_back(std::move(scene));
            }
            std::vector<std::int32_t> nameless;
            for (const UnityScene& scene : tree.Scenes)
            {
                if (scene.Name.empty())
                    nameless.push_back(scene.Handle);
            }
            const std::map<std::int32_t, std::string> named = Il2SceneNames(nameless);
            for (UnityScene& scene : tree.Scenes)
            {
                if (!scene.Name.empty())
                    continue;
                const auto it = named.find(scene.Handle);
                scene.Name = it != named.end() ? it->second : "Scene #" + std::to_string(scene.Handle);
            }
        }
        UnityScene dontDestroy;
        dontDestroy.Name = "DontDestroyOnLoad";
        dontDestroy.DontDestroy = true;
        dontDestroy.Handle = m_dontDestroyHandle;
        dontDestroy.Loaded = true;
        tree.Scenes.push_back(std::move(dontDestroy));
        {
            std::lock_guard state(m_stateLock);
            tree.Generation = m_tree.Generation + 1;
            m_tree = std::move(tree);
        }
        Publish();
    }

    Status UnityExplorer::Il2DontDestroyScene(int& slot, int& handle)
    {
        using Arg = Il2Recipe::Arg;
        Il2CppExplorerState& state = *m_il2;
        if (state.DontDestroySlot && AddressOf(state.DontDestroySlot))
        {
            slot = state.DontDestroySlot;
            handle = m_dontDestroyHandle;
            return Status::Success();
        }
        const std::uint64_t klass = FindClass(kGameObjectClass);
        const std::uint64_t create = Il2Method(kGameObjectClass, ".ctor", { "System.String" });
        const std::uint64_t keep = Il2Method(kObjectClass, "DontDestroyOnLoad", { kObjectClass });
        const std::uint64_t getScene = Il2Method(kGameObjectClass, "get_scene", {});
        std::uint64_t destroy = Il2Method(kObjectClass, "DestroyImmediate", { kObjectClass });
        if (!destroy)
            destroy = Il2Method(kObjectClass, "Destroy", { kObjectClass });
        if (!klass || !create || !keep || !getScene || !destroy || !state.ObjectNew || !state.StringNew)
            return Status::Fail(ErrorCode::MonoCallFailed, "The DontDestroyOnLoad objects cannot be listed: this game's build lacks one of GameObject(string), "
                                                           "Object.DontDestroyOnLoad, GameObject.scene and Object.DestroyImmediate");
        Il2Recipe recipe(*this);
        const int probe = recipe.Batch.Call(state.ObjectNew, klass);
        const int held = recipe.Hold(Arg::Of(probe), kScratchGroup);
        recipe.Invoke(create, Arg::Of(probe), { recipe.Text("Manifold probe") });
        recipe.Invoke(keep, Arg::Null(), { Arg::Of(probe) });
        const Il2Recipe::Invoked scene = recipe.Invoke(getScene, Arg::Of(probe));
        const int kept = recipe.Hold(Arg::Of(scene.Call), kPermanentGroup);
        const std::uint64_t handleCell = recipe.Unbox(scene.Call, 4);
        recipe.Invoke(destroy, Arg::Null(), { Arg::Of(probe) });
        const Status ran = Il2Run(recipe, kCallWait);
        if (ran)
            Il2Keep(recipe, kPermanentGroup);
        Il2Release(kScratchGroup);
        if (!ran)
            return ran;
        if (held < 0 || kept < 0 || !recipe.Holds[static_cast<std::size_t>(kept)].Slot)
            return Status::Fail(ErrorCode::MonoCallFailed, "The DontDestroyOnLoad scene could not be read" +
                                                           (recipe.Exception(scene) ? ": GameObject.scene " + Il2Thrown(recipe.Exception(scene)) : std::string()));
        state.DontDestroySlot = recipe.Holds[static_cast<std::size_t>(kept)].Slot;
        m_dontDestroyHandle = recipe.Number<std::int32_t>(handleCell);
        slot = state.DontDestroySlot;
        handle = m_dontDestroyHandle;
        return Status::Success();
    }

    Status UnityExplorer::Il2DontDestroyByDifference(std::uint64_t generation, std::vector<std::uint64_t>& items, std::size_t& total)
    {
        using Arg = Il2Recipe::Arg;
        std::vector<int> sceneSlots;
        {
            std::lock_guard state(m_stateLock);
            if (m_tree.Generation != generation)
                return Status::Fail(ErrorCode::MonoCallFailed, "the scenes were read again meanwhile");
            for (const UnityScene& scene : m_tree.Scenes)
            {
                if (!scene.DontDestroy && scene.Slot)
                    sceneSlots.push_back(scene.Slot);
            }
        }
        std::vector<Il2SceneRoots> found;
        if (const Status walked = Il2WalkRoots(false, kScratchGroup, found); !walked)
            return walked;
        const std::uint64_t getRoots = Il2Method(kSceneClass, "GetRootGameObjects", {});
        std::set<std::uint64_t> inScenes;
        for (const int sceneSlot : sceneSlots)
        {
            const std::uint64_t sceneAddress = AddressOf(sceneSlot);
            if (!sceneAddress)
                continue;
            Il2Recipe recipe(*this);
            const Il2Recipe::Invoked roots = recipe.Invoke(getRoots, recipe.This(Arg::Object(sceneAddress)));
            const int array = recipe.Hold(Arg::Of(roots.Call), kScratchGroup);
            if (const Status ran = Il2Run(recipe, kListWait); !ran)
                return ran;
            Il2Keep(recipe, kScratchGroup);
            if (recipe.Exception(roots))
                continue;
            for (const std::uint64_t root : ReadArray(recipe.Address(array), kMaxWalked, nullptr))
                inScenes.insert(root);
        }
        items.clear();
        for (const Il2SceneRoots& group : found)
        {
            for (const std::uint64_t gameObject : group.GameObjects)
            {
                if (!inScenes.count(gameObject))
                    items.push_back(gameObject);
            }
        }
        total = items.size();
        if (items.size() > kMaxChildren)
            items.resize(kMaxChildren);
        return Status::Success();
    }

    void UnityExplorer::Il2ExpandScene(int sceneIndex)
    {
        using Arg = Il2Recipe::Arg;
        UnityScene scene;
        std::uint64_t generation = 0;
        {
            std::lock_guard state(m_stateLock);
            if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_tree.Scenes.size()))
                return;
            scene = m_tree.Scenes[static_cast<std::size_t>(sceneIndex)];
            generation = m_tree.Generation;
        }
        const auto finish = [&](const std::string& error)
            {
                {
                    std::lock_guard state(m_stateLock);
                    if (m_tree.Generation == generation && sceneIndex < static_cast<int>(m_tree.Scenes.size()))
                    {
                        m_tree.Scenes[static_cast<std::size_t>(sceneIndex)].Loading = false;
                        m_tree.Scenes[static_cast<std::size_t>(sceneIndex)].RootsLoaded = error.empty();
                    }
                }
                if (!error.empty())
                    Fail(error);
                else
                    Publish();
            };
        if (Status ready = Il2EnsureHook(); !ready)
            return finish(ready.Text);
        int slot = scene.Slot;
        int handle = scene.Handle;
        std::size_t total = 0;
        std::vector<std::uint64_t> items;
        const std::uint64_t getRootList = Il2Method(kSceneClass, "GetRootGameObjects", {});
        const bool byDifference = scene.DontDestroy && !Il2Method(kGameObjectClass, "get_scene", {}) && getRootList;
        if (scene.DontDestroy && !byDifference)
        {
            if (const Status probed = Il2DontDestroyScene(slot, handle); !probed)
                return finish(probed.Text);
        }
        const std::uint64_t address = byDifference ? 0 : AddressOf(slot);
        if (!address && !byDifference)
            return finish("Manifold no longer holds this scene; read the scenes again");
        if (byDifference)
        {
            // ∑ No GameObject.scene (GTFO, Golf With Your Friends): the DontDestroyOnLoad roots are the
            //   parentless GameObjects that no loaded scene lists among its own roots.
            if (const Status listed = Il2DontDestroyByDifference(generation, items, total); !listed)
                return finish("Could not read the DontDestroyOnLoad objects: " + listed.Text);
        }
        else if (getRootList)
        {
            Il2Recipe first(*this);
            const Il2Recipe::Invoked roots = first.Invoke(getRootList, first.This(Arg::Object(address)));
            const int array = first.Hold(Arg::Of(roots.Call), kScratchGroup);
            if (const Status ran = Il2Run(first, kListWait); !ran)
                return finish("Could not read the scene's objects: " + ran.Text);
            Il2Keep(first, kScratchGroup);
            if (const std::uint64_t thrown = first.Exception(roots))
            {
                Il2Release(kScratchGroup);
                return finish("Could not read the scene's objects: Scene.GetRootGameObjects " + Il2Thrown(thrown));
            }
            items = ReadArray(first.Address(array), kMaxChildren, &total);
        }
        else
        {
            // A stripped build: this scene's roots from the walk over all Transforms.
            std::vector<Il2SceneRoots> found;
            if (const Status walked = Il2WalkRoots(false, kScratchGroup, found); !walked)
                return finish("Could not read the scene's objects: " + walked.Text);
            for (Il2SceneRoots& read : found)
            {
                if (read.Handle == handle)
                {
                    total = read.GameObjects.size();
                    if (read.GameObjects.size() > kMaxChildren)
                        read.GameObjects.resize(kMaxChildren);
                    items = std::move(read.GameObjects);
                }
            }
        }
        std::vector<Il2Node> nodes;
        const Status listed = Il2Nodes(items, 0, 0, kTreeGroup, nodes);
        Il2Release(kScratchGroup);
        if (!listed)
            return finish("Could not read the scene's objects: " + listed.Text);
        {
            std::lock_guard state(m_stateLock);
            if (m_tree.Generation != generation)
                return;
            UnityScene& target = m_tree.Scenes[static_cast<std::size_t>(sceneIndex)];
            target.Slot = slot;
            target.Handle = handle;
            target.RootCount = static_cast<int>(total);
            for (Il2Node& read : nodes)
            {
                if (m_tree.Nodes.size() >= kMaxNodes)
                    break;
                UnityNode node;
                node.Object = read.Object;
                node.Transform = read.Transform;
                node.Name = read.Name.empty() ? std::string("?") : std::move(read.Name);
                node.Active = read.Active;
                node.ChildCount = read.ChildCount;
                node.Scene = sceneIndex;
                target.Roots.push_back(static_cast<int>(m_tree.Nodes.size()));
                m_tree.Nodes.push_back(std::move(node));
            }
        }
        finish({});
    }

    void UnityExplorer::Il2ExpandNode(int nodeIndex)
    {
        using Arg = Il2Recipe::Arg;
        UnityNode parent;
        std::uint64_t generation = 0;
        {
            std::lock_guard state(m_stateLock);
            if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_tree.Nodes.size()))
                return;
            parent = m_tree.Nodes[static_cast<std::size_t>(nodeIndex)];
            generation = m_tree.Generation;
        }
        std::string error;
        std::vector<Il2Node> nodes;
        if (Status ready = Il2EnsureHook(); !ready)
            error = ready.Text;
        else if (!SlotOf(parent.Transform))
            error = "Manifold no longer holds this object; read the scenes again";
        else
        {
            Il2Recipe first(*this);
            const Il2Recipe::Invoked counted = first.Invoke(Il2Method(kTransformClass, "get_childCount", {}), Arg::Object(parent.Transform));
            const std::uint64_t countCell = first.Unbox(counted.Call, 4);
            if (const Status ran = Il2Run(first, kCallWait); !ran)
                error = "Could not read the child objects: " + ran.Text;
            else if (const std::uint64_t thrown = first.Exception(counted))
                error = "Could not read the child objects: Transform.childCount " + Il2Thrown(thrown);
            else if (!first.Returned(counted))
                error = std::string("Could not read the child objects: this game's build has no Transform.childCount") + kStripped;
            else
            {
                const std::size_t count = static_cast<std::size_t>(std::clamp<std::int64_t>(first.Number<std::int32_t>(countCell), 0, static_cast<std::int64_t>(kMaxChildren)));
                if (const Status listed = Il2Nodes({}, parent.Transform, count, kTreeGroup, nodes); !listed)
                    error = "Could not read the child objects: " + listed.Text;
            }
        }
        {
            std::lock_guard state(m_stateLock);
            if (m_tree.Generation == generation && nodeIndex < static_cast<int>(m_tree.Nodes.size()))
            {
                m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].Loading = false;
                m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].ChildrenLoaded = error.empty();
                for (Il2Node& read : nodes)
                {
                    if (m_tree.Nodes.size() >= kMaxNodes)
                        break;
                    UnityNode node;
                    node.Object = read.Object;
                    node.Transform = read.Transform;
                    node.Name = read.Name.empty() ? std::string("?") : std::move(read.Name);
                    node.Active = read.Active;
                    node.ChildCount = read.ChildCount;
                    node.Parent = nodeIndex;
                    node.Scene = parent.Scene;
                    m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].Children.push_back(static_cast<int>(m_tree.Nodes.size()));
                    m_tree.Nodes.push_back(std::move(node));
                }
                if (error.empty())
                    m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].ChildCount = static_cast<int>(m_tree.Nodes[static_cast<std::size_t>(nodeIndex)].Children.size());
            }
        }
        if (!error.empty())
            Fail(error);
        else
            Publish();
    }

    // ---------------------------------------------------------------------
    // Search

    // Calls the first object finder this build kept (see kActiveFinders) for
    // klass; the array stays held in kScratchGroup until the caller releases it.
    // assetsFirst prefers Resources.FindObjectsOfTypeAll for inactive objects.
    Status UnityExplorer::Il2FindObjects(std::uint64_t klass, bool includeInactive, bool assetsFirst, std::size_t limit, std::vector<std::uint64_t>& items, std::size_t& total)
    {
        using Arg = Il2Recipe::Arg;
        Il2CppExplorerState& state = *m_il2;
        const std::span<const FinderShape> shapes = !includeInactive ? std::span<const FinderShape>(kActiveFinders)
                                                    : assetsFirst  ? std::span<const FinderShape>(kHiddenFinders)
                                                                   : std::span<const FinderShape>(kSceneFinders);
        const FinderShape* shape = nullptr;
        std::uint64_t find = 0;
        for (const FinderShape& candidate : shapes)
        {
            find = Il2Method(candidate.Class, candidate.Method, std::span<const char* const>(candidate.Parameters, candidate.Count));
            if (find)
            {
                shape = &candidate;
                break;
            }
        }
        // ∑ GTFO (2019.4) kept no inactive finder at all; the engine's internal call behind
        //   Resources.FindObjectsOfTypeAll is registered whatever the managed side stripped.
        const std::uint64_t icall = !find && includeInactive ? Il2Icall("UnityEngine.Resources::FindObjectsOfTypeAll(System.Type)") : 0;
        const std::uint64_t type = Worker("il2cpp_class_get_type", klass).value_or(0);
        if ((!find && !icall) || !type || !state.TypeGetObject)
            return Status::Fail(ErrorCode::MonoCallFailed, std::string("this game's build has neither ") +
                                                           (includeInactive && assetsFirst ? "Resources.FindObjectsOfTypeAll" : "Object.FindObjectsOfType") +
                                                           " nor Object.FindObjectsByType" + kStripped);
        if (includeInactive && assetsFirst && shape && shape != &shapes.front())
            LOG_INFO("Unity", "This game's build has no Resources.FindObjectsOfTypeAll; the search lists inactive objects through Object.FindObjectsByType, but no assets");
        Il2Recipe first(*this);
        const int typeObject = first.Batch.Call(state.TypeGetObject, type);
        // FindObjectsInactive.Include = 1 / Exclude = 0, FindObjectsSortMode.None = 0.
        const std::int32_t inactive = includeInactive ? 1 : 0;
        const std::int32_t unsorted = 0;
        std::vector<Arg> arguments{ Arg::Of(typeObject) };
        for (std::size_t i = 1; shape && i < shape->Count; ++i)
        {
            const std::string_view parameter = shape->Parameters[i];
            if (parameter == kFindInactive)
                arguments.push_back(first.Bytes(&inactive, sizeof(inactive)));
            else if (parameter == kFindSortMode)
                arguments.push_back(first.Bytes(&unsorted, sizeof(unsorted)));
            else
                arguments.push_back(first.Bytes(&includeInactive, 1));
        }
        Il2Recipe::Invoked found;
        if (find)
            found = first.Invoke(find, Arg::Null(), arguments);
        else
            found.Call = first.Batch.Call(icall, Arg::Of(typeObject).Frame());
        const int array = first.Hold(Arg::Of(found.Call), kScratchGroup);
        if (const Status ran = Il2Run(first, kListWait); !ran)
            return ran;
        Il2Keep(first, kScratchGroup);
        if (const std::uint64_t thrown = first.Exception(found))
            return Status::Fail(ErrorCode::MonoCallFailed, std::string(shape->Class).substr(std::string_view("UnityEngine.").size()) + "." + shape->Method + " " + Il2Thrown(thrown));
        items = ReadArray(first.Address(array), limit, &total);
        return Status::Success();
    }

    void UnityExplorer::Il2Search(const UnitySearch& request, std::uint64_t klass, UnitySearchResult& result)
    {
        using Arg = Il2Recipe::Arg;
        if (Status ready = Il2EnsureHook(); !ready)
        {
            result.Error = ready.Text;
            return;
        }
        Il2Release(kSearchGroup);
        std::size_t total = 0;
        std::vector<std::uint64_t> items;
        if (const Status found = Il2FindObjects(klass, request.IncludeHidden, true, kMaxSearched, items, total); !found)
        {
            Il2Release(kScratchGroup);
            result.Error = "The search failed: " + found.Text;
            return;
        }
        result.Total = total;
        const std::string filter = Lower(Trim(request.NameFilter));
        const std::uint64_t getName = Il2Method(kObjectClass, "get_name", {});
        std::vector<std::pair<std::uint64_t, std::string>> hits;
        std::size_t next = 0;
        while (next < items.size() && hits.size() < kMaxHits && !Aborted())
        {
            Il2Recipe recipe(*this);
            std::vector<std::pair<std::uint64_t, std::uint64_t>> plans;
            while (next < items.size() && recipe.Batch.Fits(4, 512))
            {
                const std::uint64_t item = items[next++];
                if (item)
                    plans.emplace_back(item, recipe.Chars(recipe.Invoke(getName, Arg::Object(item)).Call, kNameChars));
            }
            if (const Status ran = Il2Run(recipe, kListWait); !ran)
            {
                result.Error = "The search failed: " + ran.Text;
                break;
            }
            for (const auto& [item, cell] : plans)
            {
                std::string name = recipe.TextAt(cell);
                if (!filter.empty() && Lower(name).find(filter) == std::string::npos)
                    continue;
                if (hits.size() < kMaxHits)
                    hits.emplace_back(item, std::move(name));
            }
        }
        next = 0;
        while (next < hits.size() && result.Error.empty())
        {
            Il2Recipe recipe(*this);
            while (next < hits.size() && recipe.Batch.Fits(1, 0))
                recipe.Hold(Arg::Object(hits[next++].first));
            if (const Status ran = Il2Run(recipe, kListWait); !ran)
            {
                result.Error = "The search failed: " + ran.Text;
                break;
            }
            Il2Keep(recipe, kSearchGroup);
        }
        Il2Release(kScratchGroup);
        for (auto& [item, name] : hits)
        {
            if (!SlotOf(item))
                continue;
            UnityHit hit;
            hit.Object = item;
            hit.Name = name.empty() ? std::string("?") : std::move(name);
            hit.Class = ClassNameOf(item);
            result.Hits.push_back(std::move(hit));
        }
    }

    // ---------------------------------------------------------------------
    // Inspector and edits

    void UnityExplorer::Il2Inspect(UnityInspection& inspection, bool properties, int group)
    {
        using Arg = Il2Recipe::Arg;
        const MonoClassLayout& layout = *inspection.Layout;
        if (Status ready = Il2EnsureHook(); !ready)
        {
            inspection.Error = ready.Text;
            return;
        }
        const auto process = Proc();
        if (!process)
            return;
        Il2CppExplorerState& state = *m_il2;
        const MonoObjectLayout objects = MonoLayoutFor(process->PointerSize());
        const std::size_t pointer = static_cast<std::size_t>(objects.Pointer);
        const std::uint64_t object = inspection.Object;
        const std::uint64_t getName = Il2Method(kObjectClass, "get_name", {});
        const std::uint64_t getGameObject = Il2Method(kComponentClass, "get_gameObject", {});
        const std::uint64_t getEnabled = Il2Method(kBehaviourClass, "get_enabled", {});

        Il2Recipe recipe(*this);
        const Arg self = Arg::Object(object);
        const Arg selfThis = layout.ValueType ? recipe.This(self) : self;

        struct Vectors
        {
            Il2Recipe::Invoked Call[4];
            std::uint64_t Cell[4] = {};
        };
        const auto planVectors = [&](Il2Recipe& into, Arg transform)
            {
                Vectors vectors;
                for (int i = 0; i < 4; ++i)
                {
                    vectors.Call[i] = into.Invoke(Il2Method(kTransformClass, kVectorGetters[i], {}), transform);
                    vectors.Cell[i] = into.Unbox(vectors.Call[i].Call, sizeof(float) * 3);
                }
                return vectors;
            };
        const auto readVectors = [&](const Il2Recipe& from, const Vectors& vectors, UnityTransform& out)
            {
                float* const targets[] = { out.LocalPosition, out.LocalEuler, out.LocalScale, out.Position };
                for (int i = 0; i < 4; ++i)
                {
                    if (from.Returned(vectors.Call[i]))
                        from.Batch.Read(vectors.Cell[i], targets[i], sizeof(float) * 3);
                }
                out.Read = from.Returned(vectors.Call[0]);
            };

        Il2Recipe::Invoked named;
        std::uint64_t nameCell = 0;
        std::uint64_t tagCell = 0;
        std::uint64_t layerCell = 0;
        std::uint64_t activeSelfCell = 0;
        std::uint64_t activeHierarchyCell = 0;
        std::uint64_t sceneNameCell = 0;
        std::uint64_t sceneHandleCell = 0;
        std::uint64_t parentNameCell = 0;
        std::uint64_t childCountCell = 0;
        std::uint64_t ownerNameCell = 0;
        std::uint64_t enabledCell = 0;
        Il2Recipe::Invoked enabledCall;
        int keptTransform = -1;
        int keptParent = -1;
        int keptComponents = -1;
        int keptOwner = -1;
        bool componentsAsked = false;
        Vectors vectors;
        const bool unity = layout.UnityObject && object;
        if (unity)
        {
            named = recipe.Invoke(getName, self);
            nameCell = recipe.Chars(named.Call, kNameChars);
            if (layout.GameObject)
            {
                tagCell = recipe.Chars(recipe.Invoke(Il2Method(kGameObjectClass, "get_tag", {}), self).Call, kNameChars);
                layerCell = recipe.Unbox(recipe.Invoke(Il2Method(kGameObjectClass, "get_layer", {}), self).Call, 4);
                activeSelfCell = recipe.Unbox(recipe.Invoke(Il2Method(kGameObjectClass, "get_activeSelf", {}), self).Call, 1);
                activeHierarchyCell = recipe.Unbox(recipe.Invoke(Il2Method(kGameObjectClass, "get_activeInHierarchy", {}), self).Call, 1);
                const Arg scene = Arg::Of(recipe.Invoke(Il2Method(kGameObjectClass, "get_scene", {}), self).Call);
                sceneNameCell = recipe.Chars(recipe.Invoke(Il2Method(kSceneClass, "get_name", {}), recipe.This(scene)).Call, kNameChars);
                sceneHandleCell = recipe.Unbox(scene.Call, 4);
                const Arg transform = Arg::Of(recipe.Invoke(Il2Method(kGameObjectClass, "get_transform", {}), self).Call);
                keptTransform = recipe.Hold(transform);
                vectors = planVectors(recipe, transform);
                const Arg parent = Arg::Of(recipe.Invoke(Il2Method(kTransformClass, "get_parent", {}), transform).Call);
                const Arg parentObject = Arg::Of(recipe.Invoke(getGameObject, parent).Call);
                keptParent = recipe.Hold(parentObject);
                parentNameCell = recipe.Chars(recipe.Invoke(getName, parentObject).Call, kNameChars);
                childCountCell = recipe.Unbox(recipe.Invoke(Il2Method(kTransformClass, "get_childCount", {}), transform).Call, 4);
                const std::uint64_t getComponents = Il2Method(kGameObjectClass, "GetComponents", { "System.Type" });
                const std::uint64_t componentClass = FindClass(kComponentClass);
                const std::uint64_t componentType = componentClass ? Worker("il2cpp_class_get_type", componentClass).value_or(0) : 0;
                const std::uint64_t getComponentsInternal = getComponents ? 0 : Il2Method(kGameObjectClass, "GetComponentsInternal",
                    { "System.Type", "System.Boolean", "System.Boolean", "System.Boolean", "System.Boolean", "System.Object" });
                if ((getComponents || getComponentsInternal) && componentType && state.TypeGetObject)
                {
                    componentsAsked = true;
                    const int typeObject = recipe.Batch.Call(state.TypeGetObject, componentType);
                    const std::uint8_t no = 0;
                    const std::uint8_t yes = 1;
                    const Il2Recipe::Invoked listed = getComponents
                        ? recipe.Invoke(getComponents, self, { Arg::Of(typeObject) })
                        : recipe.Invoke(getComponentsInternal, self, { Arg::Of(typeObject), recipe.Bytes(&no, 1), recipe.Bytes(&no, 1), recipe.Bytes(&yes, 1),
                              recipe.Bytes(&no, 1), Arg::Value(0) });
                    keptComponents = recipe.Hold(Arg::Of(listed.Call), kScratchGroup);
                }
            }
            else
            {
                if (layout.Component)
                {
                    const Arg owner = Arg::Of(recipe.Invoke(getGameObject, self).Call);
                    keptOwner = recipe.Hold(owner);
                    ownerNameCell = recipe.Chars(recipe.Invoke(getName, owner).Call, kNameChars);
                }
                if (layout.Behaviour)
                {
                    enabledCall = recipe.Invoke(getEnabled, self);
                    enabledCell = recipe.Unbox(enabledCall.Call, 1);
                }
                if (layout.Transform)
                    vectors = planVectors(recipe, self);
            }
        }

        std::vector<std::pair<std::size_t, Il2Recipe::Outcome>> getters;
        if (properties)
        {
            for (std::size_t i = 0; i < layout.Members.size(); ++i)
            {
                const MonoMember& member = layout.Members[i];
                if (member.Kind != MonoMemberKind::Property || !member.Getter || (!member.Static && !object))
                    continue;
                Il2Recipe::Outcome outcome;
                if (!recipe.Batch.Fits(8, 4096))
                    outcome.Error = "not read: this class has too many properties for one request";
                else
                    outcome = recipe.InvokeFor(member.Getter, member.Static ? Arg::Null() : selfThis, {}, member.Type);
                getters.emplace_back(i, std::move(outcome));
            }
        }
        for (const MonoMember& member : layout.Members)
        {
            if (member.Kind != MonoMemberKind::Field || member.Literal || member.Type.ValueType || member.Type.Code == MonoTypeCode::String)
                continue;
            std::uint64_t address = 0;
            if (member.Static)
            {
                if (!member.StaticData || member.Offset < 0)
                    continue;
                address = member.StaticData + static_cast<std::uint64_t>(member.Offset);
            }
            else if (object && member.Offset >= 0)
                address = object + static_cast<std::uint64_t>(member.Offset);
            if (!address || !recipe.Batch.Fits(2, 0))
                continue;
            recipe.Hold(Arg::Of(recipe.Load(address)));
        }
        if (layout.Rank > 0 && object && !layout.Element.ValueType)
        {
            const std::uint64_t length = std::min<std::uint64_t>(static_cast<std::uint64_t>(process->ReadValue<std::uint32_t>(object + objects.ArrayLength).value_or(0)), kMaxShownElements);
            for (std::uint64_t i = 0; i < length && recipe.Batch.Fits(2, 0); ++i)
                recipe.Hold(Arg::Of(recipe.Load(object + objects.ArrayData + i * pointer)));
        }
        if (recipe.Batch.Calls() == 0)
            return;
        if (const Status ran = Il2Run(recipe, kCallWait); !ran)
        {
            inspection.Error = ran.Text;
            return;
        }
        Il2Keep(recipe, group);

        if (!getters.empty())
        {
            std::map<int, MonoReadValue> kept;
            for (const auto& [index, value] : inspection.Values)
            {
                if (index >= 0 && index < static_cast<int>(layout.Members.size()) && layout.Members[static_cast<std::size_t>(index)].Kind == MonoMemberKind::Method)
                    kept.emplace(index, value);
            }
            for (const auto& [index, outcome] : getters)
                kept[static_cast<int>(index)] = recipe.Read(outcome, layout.Members[index].Type);
            inspection.Values = std::move(kept);
        }
        if (!unity)
            return;
        if (const std::uint64_t thrown = recipe.Exception(named))
            inspection.Error = "Object.name " + Il2Thrown(thrown) + ": the game may have destroyed this Unity object";
        inspection.ObjectName = recipe.TextAt(nameCell);
        if (!layout.GameObject)
        {
            inspection.Owner = recipe.Address(keptOwner);
            inspection.OwnerName = inspection.Owner ? recipe.TextAt(ownerNameCell) : std::string();
            if (recipe.Returned(enabledCall))
                inspection.Enabled = recipe.Number<std::uint8_t>(enabledCell) != 0;
            UnityTransform read;
            readVectors(recipe, vectors, read);
            if (read.Read)
            {
                read.Object = object;
                inspection.Transform = read;
            }
            return;
        }

        UnityGameObject info;
        info.Object = object;
        info.Name = inspection.ObjectName;
        info.Tag = recipe.TextAt(tagCell);
        info.Layer = recipe.Number<std::int32_t>(layerCell);
        info.ActiveSelf = recipe.Number<std::uint8_t>(activeSelfCell) != 0;
        info.ActiveInHierarchy = recipe.Number<std::uint8_t>(activeHierarchyCell) != 0;
        info.Scene = recipe.TextAt(sceneNameCell);
        if (const std::int32_t handle = recipe.Number<std::int32_t>(sceneHandleCell); info.Scene.empty() && handle)
        {
            const std::map<std::int32_t, std::string> named = Il2SceneNames({ handle });
            const auto it = named.find(handle);
            info.Scene = it != named.end() ? it->second : "Scene #" + std::to_string(handle);
        }
        info.Transform.Object = recipe.Address(keptTransform);
        readVectors(recipe, vectors, info.Transform);
        info.Parent = recipe.Address(keptParent);
        if (info.Parent)
        {
            info.ParentName = recipe.TextAt(parentNameCell);
            if (info.ParentName.empty())
                info.ParentName = "?";
        }
        info.ChildCount = std::max(0, recipe.Number<std::int32_t>(childCountCell));
        if (!componentsAsked && inspection.Error.empty())
            inspection.Error = std::string("The components are not listed: this game's build has neither GameObject.GetComponents(Type) nor GetComponentsInternal") + kStripped;

        const std::vector<std::uint64_t> components = ReadArray(recipe.Address(keptComponents), kMaxComponents);
        const std::uint64_t getChild = Il2Method(kTransformClass, "GetChild", { "System.Int32" });
        struct ChildPlan
        {
            int Kept = -1;
            std::uint64_t Name = 0;
        };
        struct ComponentPlan
        {
            std::uint64_t Object = 0;
            int Kept = -1;
            bool Behaviour = false;
            Il2Recipe::Invoked Enabled;
            std::uint64_t EnabledCell = 0;
        };
        std::vector<ChildPlan> childPlans;
        std::vector<ComponentPlan> componentPlans;
        Il2Recipe second(*this);
        const std::size_t shown = std::min<std::size_t>(static_cast<std::size_t>(info.ChildCount), kMaxShownChildren);
        for (std::size_t i = 0; i < shown && info.Transform.Object && second.Batch.Fits(8, 512); ++i)
        {
            const std::int32_t index = static_cast<std::int32_t>(i);
            const Arg child = Arg::Of(second.Invoke(getChild, Arg::Object(info.Transform.Object), { second.Bytes(&index, sizeof(index)) }).Call);
            const Arg childObject = Arg::Of(second.Invoke(getGameObject, child).Call);
            ChildPlan plan;
            plan.Kept = second.Hold(childObject);
            plan.Name = second.Chars(second.Invoke(getName, childObject).Call, kNameChars);
            childPlans.push_back(plan);
        }
        for (const std::uint64_t component : components)
        {
            if (!component || !second.Batch.Fits(4, 64))
                continue;
            ComponentPlan plan;
            plan.Object = component;
            plan.Kept = second.Hold(Arg::Object(component));
            const std::vector<std::string> chain = ChainOf(ClassOf(component));
            plan.Behaviour = std::find(chain.begin(), chain.end(), kBehaviourClass) != chain.end();
            if (plan.Behaviour)
            {
                plan.Enabled = second.Invoke(getEnabled, Arg::Object(component));
                plan.EnabledCell = second.Unbox(plan.Enabled.Call, 1);
            }
            componentPlans.push_back(plan);
        }
        if (second.Batch.Calls() > 0)
        {
            if (const Status ran = Il2Run(second, kCallWait); !ran)
                inspection.Error = ran.Text;
            else
            {
                Il2Keep(second, group);
                for (const ChildPlan& plan : childPlans)
                {
                    UnityHit hit;
                    hit.Object = second.Address(plan.Kept);
                    hit.Name = second.TextAt(plan.Name);
                    hit.Class = "GameObject";
                    if (hit.Name.empty())
                        hit.Name = "?";
                    if (hit.Object)
                        info.Children.push_back(std::move(hit));
                }
                for (const ComponentPlan& plan : componentPlans)
                {
                    if (!second.Address(plan.Kept))
                        continue;
                    UnityComponent entry;
                    entry.Object = plan.Object;
                    entry.Class = ClassNameOf(plan.Object);
                    entry.HasEnabled = plan.Behaviour;
                    entry.Enabled = plan.Behaviour && second.Returned(plan.Enabled) && second.Number<std::uint8_t>(plan.EnabledCell) != 0;
                    info.Components.push_back(std::move(entry));
                }
            }
        }
        Il2Release(kScratchGroup);
        inspection.GameObject = std::move(info);
    }

    Status UnityExplorer::Il2Edit(const std::string& what, std::uint64_t method, std::uint64_t self, const std::vector<AgentArg>& arguments, bool valueType)
    {
        using Arg = Il2Recipe::Arg;
        if (!method)
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": this game's build does not have the method" + kStripped);
        Il2Recipe recipe(*this);
        std::vector<Arg> parameters;
        for (const AgentArg& argument : arguments)
            parameters.push_back(recipe.Argument(argument));
        // A method of a value type takes the unboxed struct as this, like the getters in Il2Inspect.
        const Arg target = !self ? Arg::Null() : valueType ? recipe.This(Arg::Object(self)) : Arg::Object(self);
        const Il2Recipe::Invoked invoked = recipe.Invoke(method, target, parameters);
        if (invoked.Call < 0)
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": the call could not be put together");
        if (const Status ran = Il2Run(recipe, kCallWait); !ran)
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": " + ran.Text);
        if (!recipe.StringsMade())
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": the game could not make the string");
        if (const std::uint64_t thrown = recipe.Exception(invoked))
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": it " + Il2Thrown(thrown));
        return Status::Success();
    }

    Status UnityExplorer::Il2SetRunInBackground(bool enabled)
    {
        if (Status ready = Il2EnsureHook(); !ready)
            return ready;
        Il2CppExplorerState& state = *m_il2;
        Il2Recipe recipe(*this);
        if (!recipe.CanSetBackground())
            return Status::Fail(ErrorCode::MonoCallFailed, std::string("Could not change Application.runInBackground: this game's build has no such property") + kStripped);
        const Il2Recipe::Flag before = recipe.ReadBackground();
        const Il2Recipe::Invoked changed = recipe.SetBackground(enabled);
        const Il2Recipe::Flag after = recipe.ReadBackground();
        if (const Status ran = Il2Run(recipe, std::chrono::seconds(5)); !ran)
            return Status::Fail(ErrorCode::MonoCallFailed, "Bring the game to the foreground once so this can run on its main thread, then try again (" + ran.Text + ")");
        if (const std::uint64_t thrown = recipe.Exception(changed))
            return Status::Fail(ErrorCode::MonoCallFailed, "Could not change Application.runInBackground: it " + Il2Thrown(thrown));
        const bool was = recipe.Value(before).value_or(false);
        const bool now = recipe.Value(after).value_or(enabled);
        if (!state.OriginalRunInBackground)
            state.OriginalRunInBackground = was;
        if (*state.OriginalRunInBackground == now)
            state.OriginalRunInBackground.reset();
        std::lock_guard lock(m_stateLock);
        m_status.RunInBackground = now;
        m_status.ChangedRunInBackground = state.OriginalRunInBackground.has_value();
        return Status::Success();
    }

    bool UnityExplorer::Il2Fits(const MonoTypeInfo& type, const AgentArg& argument, std::string& error)
    {
        if (type.Code == MonoTypeCode::ByRef)
        {
            error = "a ref or out value cannot be passed from here";
            return false;
        }
        if (argument.Kind != AgentProtocol::ArgSlot)
            return true;
        const std::uint64_t address = AddressOf(argument.Slot);
        if (type.Name == "System.Object")
            return true;
        const std::vector<std::string> chain = ChainOf(ClassOf(address));
        if (std::find(chain.begin(), chain.end(), type.Name) != chain.end())
            return true;
        error = "the object at " + Hex(address) + " is a " + (chain.empty() ? std::string("?") : chain.front()) + ", not a " + type.Name +
                " (a value for an interface or a generic type cannot be checked from here)";
        return false;
    }

    Status UnityExplorer::Il2SetField(const UnityInspection& inspection, const MonoMember& member, const AgentArg& argument)
    {
        using Arg = Il2Recipe::Arg;
        Il2CppExplorerState& state = *m_il2;
        const std::string what = "Setting " + member.Name + " failed";
        std::string error;
        if (!Il2Fits(member.Type, argument, error))
            return Status::Fail(ErrorCode::MonoCallFailed, member.Name + ": " + error);
        const std::uint64_t function = member.Static ? state.FieldStaticSet : state.FieldSet;
        if (!function)
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": this IL2CPP runtime does not export il2cpp_field_set_value");
        if (member.Static && !member.StaticData)
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": its class is not set up yet");
        Il2Recipe recipe(*this);
        const Arg value = recipe.Argument(argument);
        if (value.Missing())
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": the value could not be put together");
        const int call = member.Static ? recipe.Batch.Call(function, member.Handle, value.Frame()) : recipe.Batch.Call(function, inspection.Object, member.Handle, value.Frame());
        if (call < 0)
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": the call could not be put together");
        if (const Status ran = Il2Run(recipe, kCallWait); !ran)
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": " + ran.Text);
        if (!recipe.StringsMade())
            return Status::Fail(ErrorCode::MonoCallFailed, what + ": the game could not make the string");
        return Status::Success();
    }

    MonoReadValue UnityExplorer::Il2Invoke(const UnityInspection& inspection, const MonoMember& member, const std::vector<AgentArg>& arguments, int group)
    {
        using Arg = Il2Recipe::Arg;
        MonoReadValue value;
        if (Status ready = Il2EnsureHook(); !ready)
        {
            value.Error = ready.Text;
            return value;
        }
        if (Worker("il2cpp_method_is_generic", member.Handle).value_or(0) & 0xFF)
        {
            value.Error = member.Name + " is generic and cannot be called without type arguments";
            return value;
        }
        for (std::size_t i = 0; i < member.Params.size() && i < arguments.size(); ++i)
        {
            std::string error;
            if (!Il2Fits(member.Params[i], arguments[i], error))
            {
                value.Error = (i < member.ParamNames.size() && !member.ParamNames[i].empty() ? member.ParamNames[i] : "argument " + std::to_string(i + 1)) + ": " + error;
                return value;
            }
        }
        Il2Recipe recipe(*this);
        const bool boxed = inspection.Layout && inspection.Layout->ValueType;
        const Arg self = member.Static ? Arg::Null() : boxed ? recipe.This(Arg::Object(inspection.Object)) : Arg::Object(inspection.Object);
        std::vector<Arg> parameters;
        for (const AgentArg& argument : arguments)
            parameters.push_back(recipe.Argument(argument));
        const Il2Recipe::Outcome outcome = recipe.InvokeFor(member.Handle, self, parameters, member.Type);
        if (outcome.Error.empty())
        {
            if (const Status ran = Il2Run(recipe, kListWait); !ran)
            {
                value.Error = ran.Text;
                return value;
            }
            if (!recipe.StringsMade())
            {
                value.Error = "the game could not make the string";
                return value;
            }
            Il2Keep(recipe, group);
            LOG_INFO("Unity", "Called " + member.Declaring + ":" + member.Name);
        }
        return recipe.Read(outcome, member.Type);
    }
}
