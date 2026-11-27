#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Table/ManagedStructures.h"
#include "Target/FrameCalls.h"
#include "Target/Mono.h"
#include "Target/Process.h"
#include "Target/RemoteCall.h"
#include "Target/UnityExplorer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    void SettleMono(Engine& engine, int milliseconds);
    void CheckFrozenWorker(Engine& engine, const std::string& className, std::size_t methods, std::size_t fields);

    namespace
    {
        // TestTarget.exe --il2cpp: its il2cpp_* exports answer and its fake
        // Mono has no root domain, so the bridge has to pick IL2CPP by itself.
        struct Il2CppTarget
        {
            PROCESS_INFORMATION Info{};

            ~Il2CppTarget()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }

            bool Start(const std::filesystem::path& path, const std::wstring& extra = L"")
            {
                SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
                HANDLE read = nullptr;
                HANDLE write = nullptr;
                if (!::CreatePipe(&read, &write, &security, 0))
                    return false;
                ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW startup{ sizeof(startup) };
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdOutput = write;
                startup.hStdError = write;
                std::wstring command = L"\"" + path.wstring() + L"\" --il2cpp" + extra;
                const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &Info);
                ::CloseHandle(write);
                std::string text;
                char buffer[512];
                DWORD got = 0;
                while (started && text.find("shoot at") == std::string::npos)
                {
                    if (!::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
                        break;
                    text.append(buffer, got);
                }
                ::CloseHandle(read);
                return started && text.find("shoot at") != std::string::npos;
            }
        };

        std::uint64_t ReadValue(const Target::Process& process, std::uint64_t address)
        {
            return process.ReadPointer(address).value_or(0);
        }

        std::int32_t ReadInt(const Target::Process& process, std::uint64_t address)
        {
            std::int32_t value = 0;
            process.ReadExact(address, &value, sizeof(value));
            return value;
        }

        const Target::MonoMethod* Method(const Target::MonoClass& info, const std::string& name, int params)
        {
            for (const Target::MonoMethod& method : info.Methods)
            {
                if (method.Name == name && (params < 0 || method.ParamCount == params))
                    return &method;
            }
            return nullptr;
        }

        void CheckFieldStall(Engine& engine)
        {
            const Target::Process& process = *engine.Process();
            const std::string stem = std::filesystem::path(process.Name()).stem().string();
            const std::uint64_t flag = engine.Evaluate(stem + ".g_il2cppFieldStall").value_or(0);
            const std::int32_t stall = 1;
            CHECK(flag && process.Write(flag, &stall, sizeof(stall)).Ok());
            engine.Mono().ClassDetails("Game.Player");
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            CHECK_EQ(ReadInt(process, flag), 0);
            const std::optional<Target::MonoClass> after = engine.Mono().ClassDetails("Game.Player");
            std::string names;
            if (after)
            {
                for (const Target::MonoField& field : after->Fields)
                    names += (names.empty() ? "" : ",") + field.Name + Format("@%X", field.Offset);
            }
            CHECK_MSG(after && names == "health@18,ammo@1C,speed@20", "Game.Player after a field read that stalled: " + names);
        }

        void CheckMethodStall(Engine& engine)
        {
            const Target::Process& process = *engine.Process();
            const std::string stem = std::filesystem::path(process.Name()).stem().string();
            const std::uint64_t flag = engine.Evaluate(stem + ".g_il2cppMethodStall").value_or(0);
            const std::int32_t stall = 1;
            CHECK(flag && process.Write(flag, &stall, sizeof(stall)).Ok());
            engine.Mono().ClassDetails("System.Collections.Generic.List`1");
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            CHECK_EQ(ReadInt(process, flag), 0);
            const std::optional<Target::MonoClass> after = engine.Mono().ClassDetails("System.Collections.Generic.List`1");
            CHECK_MSG(after && after->Generic && after->Methods.size() == 1 && after->Methods[0].Name == "Add" && after->Methods[0].ParamCount == 1,
                Format("List`1 after a method read that stalled: %zu methods, %d parameters", after ? after->Methods.size() : 0,
                    after && !after->Methods.empty() ? after->Methods[0].ParamCount : -1));
        }
    }

    namespace
    {
        bool Until(const std::function<bool()>& done, int milliseconds = 10000)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (done())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return done();
        }

        bool Act(Target::UnityExplorer& unity, const std::function<void()>& action, int milliseconds = 15000)
        {
            const std::uint64_t before = unity.Generation();
            action();
            if (!Until([&] { return unity.Generation() != before; }, milliseconds))
                return false;
            return Until([&] { return !unity.State().Busy; }, milliseconds);
        }

        int NodeNamed(const Target::UnityTree& tree, const std::string& name)
        {
            for (std::size_t i = 0; i < tree.Nodes.size(); ++i)
            {
                if (tree.Nodes[i].Name == name)
                    return static_cast<int>(i);
            }
            return -1;
        }

        int MemberNamed(const Target::MonoClassLayout& layout, Target::MonoMemberKind kind, const std::string& name)
        {
            for (std::size_t i = 0; i < layout.Members.size(); ++i)
            {
                if (layout.Members[i].Kind == kind && layout.Members[i].Name == name)
                    return static_cast<int>(i);
            }
            return -1;
        }

        Target::MonoShown FieldShown(Target::UnityExplorer& unity, const Target::MonoClassLayout& layout, const std::string& name, std::uint64_t object)
        {
            const int index = MemberNamed(layout, Target::MonoMemberKind::Field, name);
            if (index < 0)
                return { "no field " + name };
            Target::MonoShown shown;
            Until([&]
                {
                    shown = unity.ReadField(layout.Members[static_cast<std::size_t>(index)], object);
                    return !shown.Pending;
                }, 5000);
            return shown;
        }

        Target::MonoShown ValueShown(Target::UnityExplorer& unity, const Target::UnityInspection& inspection, Target::MonoMemberKind kind, const std::string& name)
        {
            const int index = inspection.Layout ? MemberNamed(*inspection.Layout, kind, name) : -1;
            if (index < 0)
                return { "no member " + name };
            const auto found = inspection.Values.find(index);
            if (found == inspection.Values.end())
                return { "no value for " + name };
            Target::MonoShown shown;
            Until([&]
                {
                    shown = unity.Shown(inspection.Layout->Members[static_cast<std::size_t>(index)], found->second);
                    return !shown.Pending;
                }, 5000);
            return shown;
        }

        std::optional<Target::UnityInspection> Inspected(Target::UnityExplorer& unity, std::uint64_t object)
        {
            int id = 0;
            Act(unity, [&] { id = unity.Inspect(object); });
            Until([&] { const auto got = unity.Inspection(id); return got && !got->Loading; });
            return unity.Inspection(id);
        }

        bool Near(const float* actual, float x, float y, float z)
        {
            return std::fabs(actual[0] - x) < 0.001f && std::fabs(actual[1] - y) < 0.001f && std::fabs(actual[2] - z) < 0.001f;
        }
    }

    // The scene explorer on IL2CPP, against the fake scene of
    // TestTarget.exe --il2cpp --il2cpp-scene: the frame stub on a per-frame
    // method, scenes, the hierarchy, the inspector, edits, the search and
    // taking everything out again. The target counts calls that arrive off
    // its main thread and GC handles that are wrong or left over.
    void CheckIl2CppScene(Engine& engine, std::uint32_t mainThread, bool alt)
    {
        const Target::Process& process = *engine.Process();
        const std::string stem = std::filesystem::path(process.Name()).stem().string();
        const auto counter = [&](const char* name) { return ReadInt(process, engine.Evaluate(stem + "." + name).value_or(0)); };
        const auto pause = [&](std::int32_t paused)
            {
                CHECK(process.Write(engine.Evaluate(stem + ".g_il2cppPaused").value_or(0), &paused, sizeof(paused)).Ok());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            };
        const Status loaded = engine.Managed().Ensure();
        CHECK_MSG(loaded.Ok(), loaded.Text);
        if (!loaded)
            return;
        Target::MonoBridge& bridge = engine.Mono();
        CHECK(bridge.IsIl2Cpp());
        Target::UnityExplorer& unity = bridge.Unity();
        const char* const hookClass = alt ? "UnityEngine.BeforeRenderHelper" : "UnityEngine.UnitySynchronizationContext";
        const char* const hookMethod = alt ? "Invoke" : "ExecuteTasks";
        const char* const frameCounter = alt ? "g_il2cppRenders" : "g_il2cppFrames";
        std::uint64_t hookSlot = 0;
        std::uint64_t hookCode = 0;
        if (const auto details = bridge.ClassDetails(hookClass))
        {
            if (const Target::MonoMethod* method = Method(*details, hookMethod, 0))
            {
                hookSlot = method->Handle;
                hookCode = method->Address;
            }
        }
        CHECK(hookSlot != 0 && hookCode != 0);
        CHECK_EQ(ReadValue(process, hookSlot), hookCode);

        unity.SetEnabled(true);
        CHECK(Act(unity, [&] { unity.ReadScenes(); }));
        Target::UnityStatus status = unity.State();
        CHECK_MSG(status.Error.empty(), status.Error);
        CHECK(status.Hooked);
        CHECK_EQ(status.Hook, std::string(alt ? "BeforeRenderHelper.Invoke" : "UnitySynchronizationContext.ExecuteTasks"));
        CHECK_EQ(status.UnityVersion, std::string("2021.3.99f1"));
        CHECK_EQ(status.MainThread, mainThread);
        CHECK(status.CallbackRuns > 0);
        CHECK(status.RunInBackground.has_value() && !*status.RunInBackground);
        CHECK(status.Missing.empty());
        CHECK(ReadValue(process, hookSlot) != hookCode);
        const std::int32_t framesBefore = counter(frameCounter);
        CHECK(Until([&] { return counter(frameCounter) > framesBefore + 3; }, 3000));

        Target::UnityTree tree = unity.Tree();
        CHECK(tree.Read);
        CHECK_EQ(tree.Scenes.size(), 3u);
        if (tree.Scenes.size() != 3)
            return;
        // The alternative fake has Unity 6000.6's stripped scene API (no
        // SceneManager.GetSceneAt, no Scene.GetRootGameObjects): the scenes
        // come from the walk over all Transforms, without build indices.
        CHECK(tree.Scenes[0].Name == "Main" && tree.Scenes[0].Handle == 0x10 && tree.Scenes[0].Active && tree.Scenes[0].BuildIndex == (alt ? -1 : 0) &&
              tree.Scenes[0].RootCount == 3 && tree.Scenes[0].Loaded);
        CHECK(tree.Scenes[1].Name == "Additive" && tree.Scenes[1].Handle == 0x20 && !tree.Scenes[1].Active && tree.Scenes[1].BuildIndex == (alt ? -1 : 1) &&
              tree.Scenes[1].RootCount == 2);
        CHECK(tree.Scenes[2].DontDestroy);

        CHECK(Act(unity, [&] { unity.ExpandScene(0); }));
        tree = unity.Tree();
        CHECK_MSG(unity.State().Error.empty(), unity.State().Error);
        CHECK(tree.Scenes[0].RootsLoaded);
        CHECK_EQ(tree.Scenes[0].Roots.size(), 3u);
        const int player = NodeNamed(tree, "Player");
        const int enemies = NodeNamed(tree, "Enemies");
        const int ui = NodeNamed(tree, "UI");
        CHECK(player >= 0 && enemies >= 0 && ui >= 0);
        if (player < 0 || enemies < 0 || ui < 0)
            return;
        const Target::UnityNode playerNode = tree.Nodes[static_cast<std::size_t>(player)];
        const Target::UnityNode enemiesNode = tree.Nodes[static_cast<std::size_t>(enemies)];
        CHECK(playerNode.Active && playerNode.ChildCount == 2 && playerNode.Object != 0 && playerNode.Transform != 0 && playerNode.Scene == 0 && playerNode.Parent == -1);
        CHECK(!enemiesNode.Active && enemiesNode.ChildCount == 2);
        CHECK_EQ(unity.ClassNameOf(playerNode.Object), std::string("UnityEngine.GameObject"));
        CHECK_EQ(unity.ClassNameOf(playerNode.Transform), std::string("UnityEngine.Transform"));
        if (alt)
        {
            // This target has no GameObject.GetComponents(Type), like a
            // stripped build; GetComponentsInternal lists the components.
            if (const auto inspection = Inspected(unity, playerNode.Object))
            {
                CHECK_MSG(inspection->Error.empty(), inspection->Error);
                CHECK(inspection->GameObject && inspection->GameObject->Components.size() == 2 && inspection->GameObject->Components[1].Class == "Game.Health");
            }
            // The walk serves the other scenes too, DontDestroyOnLoad by the
            // handle of its probe object.
            CHECK(Act(unity, [&] { unity.ExpandScene(1); }));
            CHECK(Act(unity, [&] { unity.ExpandScene(2); }));
            tree = unity.Tree();
            CHECK_MSG(unity.State().Error.empty(), unity.State().Error);
            CHECK(tree.Scenes[1].RootsLoaded && tree.Scenes[1].Roots.size() == 2u && NodeNamed(tree, "Crowd") >= 0);
            CHECK(tree.Scenes[2].RootsLoaded && tree.Scenes[2].Handle == 0x30 && tree.Scenes[2].Roots.size() == 1u && NodeNamed(tree, "Services") >= 0);
            CHECK_EQ(counter("g_il2cppProbesLive"), 0);
            // The search through FindObjectsByType(Type, FindObjectsInactive):
            // Exclude leaves the 3 inactive Transforms out, Include (no
            // Resources.FindObjectsOfTypeAll here) has them.
            for (const bool hidden : { false, true })
            {
                unity.Search({ "UnityEngine.Transform", "", hidden });
                Until([&] { return unity.SearchResult().Done; }, 15000);
                const Target::UnitySearchResult found = unity.SearchResult();
                CHECK_MSG(found.Error.empty(), found.Error);
                CHECK_EQ(found.Hits.size(), hidden ? 3012u : 3009u);
                CHECK_EQ(counter("g_il2cppFinder"), hidden ? 13 : 3);
            }
            // Shut down while the game's main thread does not come by: the
            // helper thread gives the GC handles back instead.
            CHECK_EQ(counter("g_il2cppOffThread"), 0);
            CHECK_EQ(counter("g_il2cppHandleErrors"), 0);
            CHECK(counter("g_il2cppHandlesLive") >= 8);
            pause(1);
            engine.Managed().Stop();
            CHECK_EQ(counter("g_il2cppHandlesLive"), 0);
            CHECK_EQ(counter("g_il2cppHandleErrors"), 0);
            CHECK_EQ(ReadValue(process, hookSlot), hookCode);
            pause(0);
            return;
        }

        CHECK(Act(unity, [&] { unity.ExpandNode(player); }));
        CHECK(Act(unity, [&] { unity.ExpandNode(enemies); }));
        CHECK(Act(unity, [&] { unity.ExpandNode(ui); }));
        tree = unity.Tree();
        const int weapon = NodeNamed(tree, "Weapon");
        const int camera = NodeNamed(tree, "Camera");
        const int enemyA = NodeNamed(tree, "Enemy A");
        CHECK(weapon >= 0 && camera >= 0 && enemyA >= 0 && NodeNamed(tree, "Enemy B") >= 0);
        CHECK(NodeNamed(tree, "Gr\xC3\xB6\xC3\x9F" "e") >= 0);
        if (weapon < 0 || camera < 0 || enemyA < 0)
            return;
        CHECK(tree.Nodes[static_cast<std::size_t>(player)].ChildrenLoaded);
        CHECK_EQ(tree.Nodes[static_cast<std::size_t>(player)].Children.size(), 2u);
        CHECK(tree.Nodes[static_cast<std::size_t>(weapon)].Parent == player && tree.Nodes[static_cast<std::size_t>(weapon)].ChildCount == 1);
        CHECK_EQ(tree.Nodes[static_cast<std::size_t>(camera)].ChildCount, 0);
        const Target::UnityNode weaponNode = tree.Nodes[static_cast<std::size_t>(weapon)];
        const Target::UnityNode enemyNode = tree.Nodes[static_cast<std::size_t>(enemyA)];
        CHECK(Act(unity, [&] { unity.ExpandNode(weapon); }));
        tree = unity.Tree();
        const int muzzle = NodeNamed(tree, "Muzzle");
        CHECK(muzzle >= 0 && tree.Nodes[static_cast<std::size_t>(muzzle)].Parent == weapon);

        // The DontDestroyOnLoad scene is found through a probe object, once.
        CHECK(Act(unity, [&] { unity.ExpandScene(2); }));
        tree = unity.Tree();
        CHECK_MSG(unity.State().Error.empty(), unity.State().Error);
        CHECK(tree.Scenes[2].RootsLoaded && tree.Scenes[2].Handle == 0x30);
        CHECK(NodeNamed(tree, "Services") >= 0);
        CHECK_EQ(counter("g_il2cppProbesMade"), 1);
        CHECK_EQ(counter("g_il2cppProbesLive"), 0);

        // A long list takes several batches and many frames.
        CHECK(Act(unity, [&] { unity.ExpandScene(1); }));
        tree = unity.Tree();
        const int crowd = NodeNamed(tree, "Crowd");
        CHECK(crowd >= 0 && tree.Nodes[static_cast<std::size_t>(std::max(0, crowd))].ChildCount == 3000);
        if (crowd >= 0)
        {
            const std::uint64_t framesAtStart = unity.State().CallbackRuns;
            CHECK(Act(unity, [&] { unity.ExpandNode(crowd); }, 30000));
            tree = unity.Tree();
            const Target::UnityNode& crowdNode = tree.Nodes[static_cast<std::size_t>(crowd)];
            CHECK_MSG(unity.State().Error.empty(), unity.State().Error);
            CHECK(crowdNode.ChildrenLoaded);
            CHECK_EQ(crowdNode.Children.size(), 3000u);
            if (crowdNode.Children.size() == 3000)
            {
                CHECK_EQ(tree.Nodes[static_cast<std::size_t>(crowdNode.Children.front())].Name, std::string("Unit 0"));
                CHECK_EQ(tree.Nodes[static_cast<std::size_t>(crowdNode.Children.back())].Name, std::string("Unit 2999"));
                CHECK(tree.Nodes[static_cast<std::size_t>(crowdNode.Children[1234])].Object != 0);
            }
            CHECK(unity.State().CallbackRuns >= framesAtStart + 20);
        }

        // A GameObject in the inspector.
        std::uint64_t health = 0;
        if (const auto inspection = Inspected(unity, playerNode.Object))
        {
            CHECK_MSG(inspection->Error.empty(), inspection->Error);
            CHECK(!inspection->Unheld && !inspection->FromScan);
            CHECK(inspection->Layout && inspection->Layout->Name == "UnityEngine.GameObject" && inspection->Layout->GameObject && inspection->Layout->UnityObject);
            CHECK(inspection->GameObject.has_value());
            if (inspection->GameObject)
            {
                const Target::UnityGameObject& info = *inspection->GameObject;
                CHECK(info.Name == "Player" && info.Tag == "Player" && info.Layer == 3 && info.ActiveSelf && info.ActiveInHierarchy && info.Scene == "Main");
                CHECK(info.Parent == 0 && info.ParentName.empty());
                CHECK(info.Transform.Read && info.Transform.Object == playerNode.Transform);
                CHECK(Near(info.Transform.LocalPosition, 1.0f, 2.0f, 3.0f) && Near(info.Transform.LocalScale, 1.0f, 1.0f, 1.0f) && Near(info.Transform.Position, 1.0f, 2.0f, 3.0f));
                CHECK_EQ(info.ChildCount, 2);
                CHECK(info.Children.size() == 2 && info.Children[0].Name == "Weapon" && info.Children[0].Object == weaponNode.Object && info.Children[1].Name == "Camera");
                CHECK_EQ(info.Components.size(), 2u);
                if (info.Components.size() == 2)
                {
                    CHECK(info.Components[0].Class == "UnityEngine.Transform" && info.Components[0].Object == playerNode.Transform && !info.Components[0].HasEnabled);
                    CHECK(info.Components[1].Class == "Game.Health" && info.Components[1].HasEnabled && info.Components[1].Enabled);
                    health = info.Components[1].Object;
                }
            }
            CHECK_EQ(ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "tag").Text, std::string("\"Player\""));
            CHECK_EQ(ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "layer").Text, std::string("3"));
            CHECK(inspection->RoundTrips >= 2 && inspection->Calls > 10);
        }
        else
        {
            CHECK_MSG(false, "the player was not inspected");
        }
        if (const auto inspection = Inspected(unity, weaponNode.Object))
        {
            CHECK(inspection->GameObject && inspection->GameObject->Parent == playerNode.Object && inspection->GameObject->ParentName == "Player");
            CHECK(inspection->GameObject && Near(inspection->GameObject->Transform.Position, 1.5f, 2.0f, 3.0f));
        }
        CHECK(health != 0);
        if (!health)
            return;

        // A component: fields from memory, properties and methods through the
        // frame stub, reference values held so they can be opened.
        int id = 0;
        Act(unity, [&] { id = unity.Inspect(health); });
        Until([&] { const auto got = unity.Inspection(id); return got && !got->Loading; });
        std::optional<Target::UnityInspection> inspection = unity.Inspection(id);
        CHECK(inspection.has_value() && inspection->Layout != nullptr);
        if (!inspection || !inspection->Layout)
            return;
        const std::shared_ptr<const Target::MonoClassLayout> layout = inspection->Layout;
        CHECK_MSG(inspection->Error.empty(), inspection->Error);
        CHECK(layout->Name == "Game.Health" && layout->Behaviour && layout->Component && layout->UnityObject && !layout->GameObject);
        CHECK(layout->Chain.size() >= 5 && layout->Chain[1] == "UnityEngine.MonoBehaviour");
        CHECK(inspection->ObjectName == "Player" && inspection->Owner == playerNode.Object && inspection->OwnerName == "Player");
        CHECK(inspection->Enabled.has_value() && *inspection->Enabled);
        CHECK_EQ(FieldShown(unity, *layout, "hp", health).Text, std::string("100"));
        CHECK_EQ(FieldShown(unity, *layout, "regen", health).Text, std::string("1.5"));
        CHECK_EQ(FieldShown(unity, *layout, "label", health).Text, std::string("\"hero\""));
        CHECK_EQ(FieldShown(unity, *layout, "spawn", health).Text, std::string("(4.0, 5.0, 6.0)"));
        CHECK_EQ(FieldShown(unity, *layout, "mode", health).Text, std::string("Fight (1)"));
        CHECK_EQ(FieldShown(unity, *layout, "s_count", health).Text, std::string("7"));
        CHECK_EQ(FieldShown(unity, *layout, "MaxHp", health).Text, std::string("const"));
        const Target::MonoShown target = FieldShown(unity, *layout, "target", health);
        CHECK(target.Reference == enemyNode.Object && target.Text == "GameObject");
        CHECK_EQ(ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "Hp").Text, std::string("100"));
        CHECK_EQ(ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "Label").Text, std::string("\"hero\""));
        CHECK_EQ(ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "Target").Reference, enemyNode.Object);
        CHECK_EQ(ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "enabled").Text, std::string("true"));
        CHECK_EQ(ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "name").Text, std::string("\"Player\""));
        CHECK_EQ(ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "gameObject").Reference, playerNode.Object);
        const std::string broken = ValueShown(unity, *inspection, Target::MonoMemberKind::Property, "Broken").Text;
        CHECK_MSG(broken.find("System.InvalidOperationException") != std::string::npos, broken);
        const int tryRead = MemberNamed(*layout, Target::MonoMemberKind::Method, "TryRead");
        CHECK(tryRead >= 0 && layout->Members[static_cast<std::size_t>(tryRead)].Params.size() == 1 &&
              layout->Members[static_cast<std::size_t>(tryRead)].Params[0].Code == Target::MonoTypeCode::ByRef);
        const int heal = MemberNamed(*layout, Target::MonoMemberKind::Method, "Heal");
        CHECK(heal >= 0 && Target::UnityExplorer::Signature(layout->Members[static_cast<std::size_t>(std::max(0, heal))]) == "(int amount)");
        CHECK(MemberNamed(*layout, Target::MonoMemberKind::Method, "get_Hp") < 0);

        const auto refreshed = [&]
            {
                Until([&] { const auto got = unity.Inspection(id); return got && !got->Loading; });
                inspection = unity.Inspection(id);
                return inspection.has_value();
            };
        const auto setMember = [&](Target::MonoMemberKind kind, const char* name, const std::string& text)
            {
                const int index = MemberNamed(*layout, kind, name);
                CHECK_MSG(index >= 0, name);
                Act(unity, [&] { unity.SetMember(id, index, text); });
                refreshed();
            };
        const auto invoke = [&](const char* name, std::vector<std::string> arguments)
            {
                const int index = MemberNamed(*layout, Target::MonoMemberKind::Method, name);
                CHECK_MSG(index >= 0, name);
                Act(unity, [&] { unity.InvokeMethod(id, index, std::move(arguments)); });
                refreshed();
                Target::MonoReadValue value;
                if (inspection)
                {
                    if (const auto found = inspection->Values.find(index); found != inspection->Values.end())
                        value = found->second;
                }
                return index >= 0 ? std::make_pair(unity.Shown(layout->Members[static_cast<std::size_t>(index)], value), value) : std::make_pair(Target::MonoShown{}, value);
            };
        setMember(Target::MonoMemberKind::Field, "hp", "250");
        CHECK_EQ(FieldShown(unity, *layout, "hp", health).Text, std::string("250"));
        setMember(Target::MonoMemberKind::Property, "Hp", "300");
        CHECK_MSG(unity.State().Error.empty(), unity.State().Error);
        CHECK_EQ(FieldShown(unity, *layout, "hp", health).Text, std::string("300"));
        setMember(Target::MonoMemberKind::Field, "label", "\"boss\"");
        CHECK_MSG(unity.State().Error.empty(), unity.State().Error);
        CHECK_EQ(FieldShown(unity, *layout, "label", health).Text, std::string("\"boss\""));
        setMember(Target::MonoMemberKind::Property, "Label", "\"chief\"");
        CHECK_EQ(FieldShown(unity, *layout, "label", health).Text, std::string("\"chief\""));
        setMember(Target::MonoMemberKind::Field, "target", Hex(playerNode.Object));
        CHECK_MSG(unity.State().Error.empty(), unity.State().Error);
        CHECK_EQ(FieldShown(unity, *layout, "target", health).Reference, playerNode.Object);
        setMember(Target::MonoMemberKind::Field, "target", Hex(health));
        CHECK_MSG(unity.State().Error.find("not a UnityEngine.GameObject") != std::string::npos, unity.State().Error);
        CHECK_EQ(FieldShown(unity, *layout, "target", health).Reference, playerNode.Object);
        setMember(Target::MonoMemberKind::Property, "Target", Hex(enemyNode.Object));
        CHECK_EQ(FieldShown(unity, *layout, "target", health).Reference, enemyNode.Object);
        setMember(Target::MonoMemberKind::Field, "target", "null");
        CHECK(FieldShown(unity, *layout, "target", health).Null);

        CHECK_EQ(invoke("Heal", { "5" }).first.Text, std::string("305"));
        CHECK_EQ(FieldShown(unity, *layout, "hp", health).Text, std::string("305"));
        CHECK_EQ(invoke("Describe", { "x" }).first.Text, std::string("\"x:chief\""));
        CHECK_EQ(invoke("Count", {}).first.Text, std::string("7"));
        const std::string exploded = invoke("Explode", {}).second.Error;
        CHECK_MSG(exploded.find("System.InvalidOperationException") != std::string::npos, exploded);
        const std::string byRef = invoke("TryRead", { "1" }).second.Error;
        CHECK_MSG(byRef.find("ref or out") != std::string::npos, byRef);

        // Edits through Unity's own methods.
        Act(unity, [&] { unity.SetEnabled(health, false, id); });
        refreshed();
        CHECK(inspection && inspection->Enabled.has_value() && !*inspection->Enabled);
        Act(unity, [&] { unity.SetActive(enemiesNode.Object, true, 0); });
        CHECK(unity.Tree().Nodes[static_cast<std::size_t>(enemies)].Active);
        if (const auto enemy = Inspected(unity, enemyNode.Object))
        {
            CHECK(!enemy->Unheld);
            CHECK(enemy->GameObject && enemy->GameObject->Name == "Enemy A" && enemy->GameObject->ActiveInHierarchy);
        }
        Act(unity, [&] { unity.SetActive(enemiesNode.Object, false, 0); });
        Act(unity, [&] { unity.SetName(playerNode.Object, "Hero", 0); });
        CHECK_EQ(unity.Tree().Nodes[static_cast<std::size_t>(player)].Name, std::string("Hero"));
        const float moved[3] = { 7.0f, 8.0f, 9.0f };
        Act(unity, [&] { unity.SetVector(playerNode.Transform, 0, moved, 0); });
        if (const auto again = Inspected(unity, playerNode.Object))
            CHECK(again->GameObject && again->GameObject->Name == "Hero" && Near(again->GameObject->Transform.LocalPosition, 7.0f, 8.0f, 9.0f));
        Act(unity, [&] { unity.SetRunInBackground(true); });
        status = unity.State();
        CHECK(status.RunInBackground.has_value() && *status.RunInBackground && status.ChangedRunInBackground);
        CHECK_EQ(counter("g_il2cppRunInBackground"), 1);

        // The search: Unity objects through Unity, other classes by their
        // class pointer in memory.
        const auto search = [&](const std::string& klass, const std::string& filter, bool hidden)
            {
                unity.Search({ klass, filter, hidden });
                Until([&] { return unity.SearchResult().Done; }, 15000);
                return unity.SearchResult();
            };
        Target::UnitySearchResult found = search("UnityEngine.Transform", "", false);
        CHECK_MSG(found.Error.empty(), found.Error);
        CHECK(found.Total == 3009 && found.Hits.size() == 3009 && !found.HeapScan);
        CHECK(std::all_of(found.Hits.begin(), found.Hits.end(), [](const Target::UnityHit& hit) { return hit.Class == "UnityEngine.Transform" && hit.Object != 0; }));
        CHECK_EQ(counter("g_il2cppFinder"), 1);
        found = search("Transform", "", true);
        CHECK_EQ(found.Hits.size(), 3012u);
        CHECK_EQ(counter("g_il2cppFinder"), 12);
        found = search("UnityEngine.Transform", "enemy", true);
        CHECK(found.Total == 3012 && found.Hits.size() == 2);
        found = search("Game.Health", "", false);
        CHECK(found.Hits.size() == 1 && found.Hits[0].Object == health && found.Hits[0].Name == "Hero");
        found = search("System.Type", "", false);
        CHECK_MSG(found.Error.empty(), found.Error);
        CHECK(found.HeapScan && found.Total >= 2 && !found.Hits.empty());

        // Nothing ran off the game's main thread, every GC handle was a real
        // one, and Shutdown gives all of them back, sets runInBackground
        // back and takes the stub off the method again.
        CHECK_EQ(counter("g_il2cppOffThread"), 0);
        CHECK_EQ(counter("g_il2cppHandleErrors"), 0);
        CHECK(counter("g_il2cppHandlesLive") > 10);
        CHECK(counter("g_il2cppInvokes") > 100);

        // A game whose main thread stops coming by: the request can be
        // stopped, nothing of it runs later, and the next one works again.
        pause(1);
        const std::int32_t invokesPaused = counter("g_il2cppInvokes");
        unity.ExpandNode(camera);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(unity.State().Busy);
        unity.Cancel();
        CHECK(Until([&] { return !unity.State().Busy; }, 5000));
        CHECK_MSG(unity.State().Error.find("cancelled") != std::string::npos, unity.State().Error);
        CHECK(!unity.Tree().Nodes[static_cast<std::size_t>(camera)].ChildrenLoaded);
        pause(0);
        CHECK_EQ(counter("g_il2cppInvokes"), invokesPaused);
        CHECK(Act(unity, [&] { unity.ExpandNode(camera); }));
        CHECK(unity.Tree().Nodes[static_cast<std::size_t>(camera)].ChildrenLoaded);
        CHECK(counter("g_il2cppInvokes") > invokesPaused);
        CHECK_EQ(counter("g_il2cppOffThread"), 0);
        engine.Managed().Stop();
        CHECK_EQ(counter("g_il2cppHandlesLive"), 0);
        CHECK_EQ(counter("g_il2cppHandleErrors"), 0);
        CHECK_EQ(counter("g_il2cppRunInBackground"), 0);
        CHECK_EQ(ReadValue(process, hookSlot), hookCode);
        const std::int32_t framesAfter = counter(frameCounter);
        CHECK(Until([&] { return counter(frameCounter) > framesAfter + 3; }, 3000));

        // A second session in the same game works like the first.
        const Status again = engine.Managed().Ensure();
        CHECK_MSG(again.Ok(), again.Text);
        if (!again)
            return;
        Target::UnityExplorer& second = engine.Mono().Unity();
        second.SetEnabled(true);
        CHECK(Act(second, [&] { second.ReadScenes(); }));
        CHECK_MSG(second.State().Error.empty(), second.State().Error);
        CHECK(second.State().Hooked && second.Tree().Scenes.size() == 3);
        CHECK(Act(second, [&] { second.ExpandScene(0); }));
        CHECK(NodeNamed(second.Tree(), "Hero") >= 0);
        engine.Managed().Stop();
        CHECK_EQ(counter("g_il2cppHandlesLive"), 0);
        CHECK_EQ(counter("g_il2cppOffThread"), 0);
        CHECK_EQ(ReadValue(process, hookSlot), hookCode);
    }

    void CheckIl2Cpp(Engine& engine)
    {
        const Target::Process& process = *engine.Process();
        const std::string module = process.Name();
        const std::string stem = std::filesystem::path(module).stem().string();
        const auto main = process.MainModule();
        CHECK(main.has_value());
        if (!main)
            return;
        const auto inModule = [&](std::uint64_t address) { return address >= main->Base && address < main->Base + main->Size; };
        const std::uint64_t player = engine.Evaluate(stem + ".g_il2cppPlayer").value_or(0);
        CHECK(player != 0);

        // The drain stub on its own: one call walks a whole iterator, with the
        // 8 bytes at an offset of each item or a peek function next to it.
        {
            Target::RemoteCall call;
            const Status started = call.Start(engine.Process());
            CHECK_MSG(started.Ok(), started.Text);
            const std::uint64_t methods = process.FindExport(module, "il2cpp_class_get_methods");
            const std::uint64_t fields = process.FindExport(module, "il2cpp_class_get_fields");
            const std::uint64_t pointer = process.FindExport(module, "fake_method_pointer");
            const std::uint64_t klass = ReadValue(process, player);
            std::vector<Target::RemoteCall::DrainItem> byOffset;
            std::vector<Target::RemoteCall::DrainItem> byFunction;
            std::vector<Target::RemoteCall::DrainItem> plain;
            CHECK(started && call.Drain(methods, klass, byOffset, 0).Ok());
            CHECK(started && call.Drain(methods, klass, byFunction, Target::RemoteCall::kNoPeek, pointer).Ok());
            CHECK(started && call.Drain(fields, klass, plain).Ok());
            CHECK_EQ(byOffset.size(), 4u);
            CHECK_EQ(byFunction.size(), 4u);
            for (std::size_t i = 0; i < std::min(byOffset.size(), byFunction.size()); ++i)
            {
                CHECK(byOffset[i].Item == byFunction[i].Item);
                CHECK(byOffset[i].Peek != 0 && byOffset[i].Peek == byFunction[i].Peek && inModule(byOffset[i].Peek));
                CHECK_EQ(ReadValue(process, byOffset[i].Item), byOffset[i].Peek);
            }
            CHECK_EQ(plain.size(), 4u);
            CHECK(std::all_of(plain.begin(), plain.end(), [](const Target::RemoteCall::DrainItem& item) { return item.Item != 0 && item.Peek == 0; }));
            std::vector<Target::RemoteCall::DrainItem> limited;
            CHECK(started && call.Drain(methods, klass, limited, 0, 0, 2).Ok());
            CHECK_EQ(limited.size(), 2u);
            call.Stop();
        }

        const std::int32_t detachedBefore = ReadInt(process, engine.Evaluate(stem + ".g_il2cppDetached").value_or(0));
        const Status loaded = engine.Managed().Ensure();
        CHECK_MSG(loaded.Ok(), loaded.Text);
        if (!loaded)
            return;
        Target::MonoBridge& bridge = engine.Mono();
        CHECK(bridge.IsIl2Cpp());
        CHECK(bridge.Runtime() == Target::ManagedRuntime::Il2Cpp);
        CHECK_EQ(bridge.Module(), module);
        CHECK(ReadInt(process, engine.Evaluate(stem + ".g_il2cppAttached").value_or(0)) >= 1);

        // Classes: "<Module>" is left out, image names lose their ".dll".
        const std::vector<Target::MonoClass> classes = bridge.Classes();
        CHECK_EQ(classes.size(), 5u);
        const auto found = std::find_if(classes.begin(), classes.end(), [](const Target::MonoClass& c) { return c.FullName() == "Game.Player"; });
        CHECK(found != classes.end());
        if (found != classes.end())
            CHECK_EQ(found->Image, std::string("Assembly-CSharp"));
        CHECK(std::none_of(classes.begin(), classes.end(), [](const Target::MonoClass& c) { return c.Name == "<Module>"; }));

        // Members: parents, parameter counts, overloads, code addresses
        // (MethodInfo::methodPointer at offset 0), instance fields only.
        std::uint64_t take = 0;
        std::uint64_t getter = 0;
        if (const auto details = bridge.ClassDetails("Game.Player"))
        {
            CHECK_EQ(details->Parent, std::string("Game.Actor"));
            CHECK(!details->Generic);
            CHECK_EQ(details->Methods.size(), 4u);
            const Target::MonoMethod* takeDamage = Method(*details, "TakeDamage", 1);
            const Target::MonoMethod* heal = Method(*details, "Heal", 1);
            const Target::MonoMethod* healMore = Method(*details, "Heal", 2);
            const Target::MonoMethod* health = Method(*details, "get_Health", 0);
            CHECK(takeDamage && heal && healMore && health);
            if (takeDamage && heal && healMore && health)
            {
                take = takeDamage->Address;
                getter = health->Address;
                CHECK(inModule(take) && inModule(heal->Address) && inModule(healMore->Address) && inModule(getter));
                CHECK(heal->Address != healMore->Address);
                CHECK_EQ(ReadValue(process, takeDamage->Handle), take);
            }
            CHECK_EQ(details->Fields.size(), 3u);
            if (details->Fields.size() == 3)
            {
                CHECK(details->Fields[0].Name == "health" && details->Fields[0].Offset == 0x18 && details->Fields[0].TypeCode == 0x08);
                CHECK(details->Fields[1].Name == "ammo" && details->Fields[1].Offset == 0x1C && details->Fields[1].TypeCode == 0x08);
                CHECK(details->Fields[2].Name == "speed" && details->Fields[2].Offset == 0x20 && details->Fields[2].TypeCode == 0x0C);
            }
        }
        else
        {
            CHECK_MSG(false, "Game.Player has no details");
        }
        if (const auto list = bridge.ClassDetails("List`1"))
        {
            CHECK(list->Generic);
            CHECK(list->Fields.empty());
            CHECK_EQ(list->Parent, std::string("System.Object"));
            const Target::MonoMethod* add = Method(*list, "Add", 1);
            CHECK(add && add->Address == 0);
        }
        else
        {
            CHECK_MSG(false, "List`1 has no details");
        }
        if (const auto manager = bridge.ClassDetails("GameManager"))
        {
            const Target::MonoMethod* level = Method(*manager, "get_Level", 0);
            CHECK(level && level->Address == getter && getter != 0);
            const Target::MonoMethod* onEvent = Method(*manager, "OnEvent", 1);
            CHECK(onEvent && onEvent->Address == 0);
        }
        const std::vector<std::string> names = bridge.MethodsOf("Player");
        CHECK(names.size() == 4 && names[0] == "TakeDamage" && names[1] == "Heal" && names[3] == "get_Health");
        const std::vector<Target::MonoField> inherited = bridge.InstanceFieldsOf("Game.Player");
        CHECK_EQ(inherited.size(), 4u);
        CHECK(inherited.size() == 4 && inherited[0].Name == "health" && inherited[3].Name == "id" && inherited[3].Offset == 0x10);
        CHECK_EQ(bridge.InstanceFieldsOf("Game.Actor").size(), 1u);
        CHECK(bridge.InstanceFieldsOf("Nope").empty());
        CHECK(bridge.FieldOffset("Player", "ammo") == 0x1C);
        CHECK(bridge.FieldOffset("Game.Player", "speed") == 0x20);
        CHECK(!bridge.FieldOffset("Player", "s_count").has_value());

        // Name -> address, also with the image in front, and in expressions
        // where the dash of Assembly-CSharp is part of the name.
        CHECK(take != 0 && bridge.AddressOf("Player:TakeDamage") == take);
        CHECK(take != 0 && bridge.AddressOf("Assembly-CSharp:Player:TakeDamage") == take);
        CHECK(take != 0 && bridge.AddressOf("Assembly-CSharp.dll:Game.Player:TakeDamage") == take);
        CHECK(!bridge.AddressOf("mscorlib:Player:TakeDamage").has_value());
        CHECK(!bridge.AddressOf("GameManager:OnEvent").has_value());
        CHECK(!bridge.AddressOf("Nope:Missing").has_value());
        Status error;
        CHECK_MSG(engine.Evaluate("Player:TakeDamage", &error) == take, error.Text);
        CHECK_MSG(engine.Evaluate("Assembly-CSharp:Player:TakeDamage", &error) == take, error.Text);
        CHECK_MSG(engine.Evaluate("Assembly-CSharp:Game.Player:TakeDamage+4", &error) == take + 4, error.Text);
        CHECK_MSG(engine.Evaluate("\"Assembly-CSharp:Player:TakeDamage\"+4", &error) == take + 4, error.Text);
        CHECK(engine.Evaluate(stem + ".exe+10-4") == main->Base + 0xC);

        // Address -> name, from the sorted table of method code, both in the
        // bridge and through the symbol provider (it beats a nearby export).
        if (take)
        {
            const auto atStart = bridge.MethodNameAt(take);
            CHECK_MSG(atStart && *atStart == "Game.Player:TakeDamage", atStart ? *atStart : "nil");
            const auto inside = bridge.MethodNameAt(take + 1);
            CHECK_MSG(inside && *inside == "Game.Player:TakeDamage", inside ? *inside : "nil");
            CHECK(bridge.MethodStartNameAt(take) == std::optional<std::string>("Game.Player:TakeDamage"));
            CHECK(!bridge.MethodStartNameAt(take + 1).has_value());
            const auto range = bridge.MethodRange(take + 1);
            CHECK(range && range->first == take && range->second > take + 1);
            CHECK_EQ(engine.Describe(take), std::string("Game.Player:TakeDamage"));
            CHECK_EQ(engine.Describe(take + 1), std::string("Game.Player:TakeDamage+1"));
            CHECK_EQ(bridge.MethodsSharingCode(take), 1u);
        }
        if (getter)
        {
            CHECK_EQ(bridge.MethodsSharingCode(getter), 2u);
            const auto shared = bridge.MethodNameAt(getter);
            CHECK_MSG(shared && *shared == "Game.Player:get_Health", shared ? *shared : "nil");
        }
        CHECK(!bridge.MethodNameAt(0).has_value());
        CHECK(!bridge.MethodNameAt(main->Base).has_value());
        CHECK(!bridge.MethodNameAt(main->Base + main->Size + 0x1000).has_value());
        const std::uint64_t domainGet = process.FindExport(module, "il2cpp_domain_get");
        CHECK_EQ(engine.Describe(domainGet), stem + ".il2cpp_domain_get");

        // Structure Dissect's lookups: the object points straight at its
        // class; a generic instance no image lists is named by its shape;
        // anything else is left alone.
        if (player)
        {
            CHECK(bridge.ClassNameAt(player) == std::optional<std::string>("Game.Player"));
            const auto inside = bridge.ClassAt(player + 0x18);
            CHECK_MSG(inside && inside->Base == player && inside->Name == "Game.Player", inside ? inside->Name + " at " + Hex(inside->Base) : "nil");
            CHECK(!bridge.ClassAt(player + 0x100000).has_value());
            CHECK(!bridge.ClassNameAt(player + 8).has_value());
        }
        const std::size_t before = bridge.ClassCount();
        const std::uint64_t list = engine.Evaluate(stem + ".g_il2cppList").value_or(0);
        const auto listName = bridge.ClassNameAt(list);
        CHECK_MSG(listName && *listName == "System.Collections.Generic.List<System.Int32>", listName ? *listName : "nil");
        CHECK_EQ(bridge.ClassCount(), before + 1);
        // The open definition keeps its name and has no offsets; the object's
        // own class (found by its pointer, not its name) has them.
        CHECK(bridge.FieldsOf("System.Collections.Generic.List`1").empty());
        const std::vector<Target::MonoField> listFields = bridge.InstanceFieldsAt(list);
        CHECK_EQ(listFields.size(), 3u);
        if (listFields.size() == 3)
        {
            CHECK(listFields[0].Name == "_items" && listFields[0].Offset == 0x10);
            CHECK(listFields[1].Name == "_size" && listFields[1].Offset == 0x18 && listFields[1].TypeCode == 0x08);
        }
        CHECK(!bridge.ArrayAt(list).has_value());
        if (player)
        {
            CHECK(!bridge.ArrayAt(player).has_value());
            CHECK_EQ(bridge.InstanceFieldsAt(player).size(), 4u);  // health, ammo, speed + Actor.id
        }

        // Arrays, as Structure Dissect lays them out: header, then [0], [1], ...
        const int pointerSize = process.PointerSize();
        const auto ints = bridge.ArrayAt(engine.Evaluate(stem + ".g_il2cppInts").value_or(0));
        CHECK(ints.has_value());
        if (ints)
        {
            CHECK_EQ(ints->Length, 3u);
            CHECK_EQ(ints->ElementSize, 4);
            CHECK_EQ(ints->DataOffset, 4 * pointerSize);
            CHECK_EQ(ints->Element.TypeCode, 0x08);
            CHECK_MSG(Table::ManagedArrayName(*ints) == "System.Int32[3]", Table::ManagedArrayName(*ints));
            Table::StructDefinition layout;
            Table::FillFromManaged(layout, Table::ManagedArrayFields(*ints, pointerSize, 1024), 0, {}, pointerSize);
            CHECK_EQ(layout.Fields.size(), 5u);
            if (layout.Fields.size() == 5)
            {
                CHECK(layout.Fields[0].Name == "bounds" && layout.Fields[0].Type == Scan::ValueType::Pointer && layout.Fields[0].Size == pointerSize);
                CHECK(layout.Fields[1].Name == "max_length" && layout.Fields[1].Offset == 3 * pointerSize);
                CHECK(layout.Fields[3].Name == "[1]" && layout.Fields[3].Offset == 4 * pointerSize + 4 && layout.Fields[3].Type == Scan::ValueType::FourBytes);
            }
            CHECK_EQ(Table::ManagedArrayFields(*ints, pointerSize, 2).size(), 4u);
        }
        const auto players = bridge.ArrayAt(engine.Evaluate(stem + ".g_il2cppPlayers").value_or(0));
        CHECK(players && players->Length == 2 && players->ElementSize == pointerSize && players->Element.TypeCode == 0x12);
        const auto vecs = bridge.ArrayAt(engine.Evaluate(stem + ".g_il2cppVecs").value_or(0));
        CHECK(vecs.has_value());
        if (vecs)
        {
            CHECK_EQ(vecs->ElementSize, 8);
            CHECK_MSG(vecs->Element.TypeName == "Game.Vec2" && vecs->Element.ValueSize == 8, vecs->Element.TypeName);
            const std::vector<Target::MonoField> elements = Table::ManagedArrayFields(*vecs, pointerSize, 1024);
            Table::ManagedLayouts nested;
            Table::CollectManagedLayouts(bridge, elements, nested);
            const auto vec = nested.find("Game.Vec2");
            CHECK(vec != nested.end() && vec->second.size() == 2);
            Table::StructDefinition layout;
            Table::FillFromManaged(layout, elements, 0, nested, pointerSize);
            CHECK(layout.Fields.size() == 4 && layout.Fields[3].Name == "[1]" && layout.Fields[3].ChildStructure == "Game.Vec2" && layout.Fields[3].Size == 8);
        }
        // IntPtr holds a native pointer (UnityEngine.Object.m_CachedPtr).
        {
            int size = 0;
            Table::FieldDisplay display = Table::FieldDisplay::Unsigned;
            // ∑ Pointer sized in a 32-bit game too: Terraria's dissect read two neighbouring references as one 8-byte value.
            CHECK(Table::ManagedFieldType(0x18, pointerSize, size, display) == Scan::ValueType::Pointer && size == pointerSize);
            CHECK(Table::ManagedFieldType(0x12, 4, size, display) == Scan::ValueType::Pointer && size == 4);
            CHECK(Table::ManagedFieldType(0x12, 8, size, display) == Scan::ValueType::Pointer && size == 8);
        }
        CHECK(!bridge.ClassNameAt(engine.Evaluate(stem + ".g_il2cppWild").value_or(0)).has_value());
        CHECK(!bridge.ClassNameAt(engine.Evaluate(stem + ".g_il2cppNotClass").value_or(0)).has_value());
        CHECK(!bridge.ClassNameAt(engine.Evaluate(stem + ".g_il2cppStray").value_or(0)).has_value());

        // This target has no Unity classes, so the explorer finds no
        // per-frame method to sit on and says so; nothing stays in the game.
        Target::UnityExplorer& unity = bridge.Unity();
        unity.SetEnabled(true);
        Act(unity, [&] { unity.ReadScenes(); });
        CHECK_MSG(unity.State().Error.find("per-frame Unity methods") != std::string::npos, unity.State().Error);
        CHECK(!unity.State().Hooked);
        CHECK(!unity.Tree().Read);
        unity.SetEnabled(false);

        // The mono_* Lua functions serve IL2CPP too, as in Cheat Engine.
        std::string luaResult;
        const Status lua = engine.Lua().RunWithResult(
            "local methods = mono_class_getMethods('Actor') "
            "return tostring(mono_isPresent()) .. ',' .. tostring(mono_isIl2Cpp()) .. ',' .. tostring(#mono_enumClasses()) .. ',' .. "
            "tostring(#mono_class_getMethods('Game.Player')) .. ',' .. tostring(methods and #methods) .. ',' .. "
            "string.format('%X', mono_getAddress('Player', 'TakeDamage') or 0) .. ',' .. tostring(mono_class_getFieldOffset('Game.Player', 'speed')) .. ',' .. "
            "string.format('%X', getAddress('Assembly-CSharp:Player:TakeDamage'))",
            "il2cpp lua", luaResult);
        CHECK_MSG(lua.Ok(), lua.Text);
        if (lua && take)
            CHECK_EQ(luaResult, "true,true," + std::to_string(bridge.ClassCount()) + ",4,0," + Hex(take) + ",32," + Hex(take));

        // Loading in the background, a reload (which drops classes found only
        // by their shape) and Shutdown, which takes the worker off IL2CPP's
        // thread list.
        engine.Managed().Stop();
        CHECK(!engine.MonoPresent());
        CHECK(!bridge.IsIl2Cpp());
        CHECK_EQ(ReadInt(process, engine.Evaluate(stem + ".g_il2cppDetached").value_or(0)), detachedBefore + 1);
        CHECK(!engine.Evaluate("Player:TakeDamage").has_value());
        engine.Managed().Load(true);
        CHECK(engine.Managed().Loading());
        SettleMono(engine, 5000);
        CHECK(!engine.Managed().Loading());
        CHECK(engine.MonoPresent() && bridge.IsIl2Cpp());
        CHECK_EQ(bridge.ClassCount(), 5u);
        CHECK(take != 0 && engine.Evaluate("Player:TakeDamage") == take);
        bridge.ClassNameAt(list);
        const std::uint64_t generation = bridge.Generation();
        engine.Managed().Reload(true);
        SettleMono(engine, 5000);
        CHECK(bridge.Generation() != generation);
        CHECK_EQ(bridge.ClassCount(), 5u);
        CHECK(take != 0 && bridge.MethodNameAt(take) == std::optional<std::string>("Game.Player:TakeDamage"));
        engine.Managed().Reload(true);
        SettleMono(engine, 5000);
        CheckFrozenWorker(engine, "Game.Player", 4, 3);
        engine.Managed().Reload(true);
        SettleMono(engine, 5000);
        CheckFieldStall(engine);
        CheckMethodStall(engine);
        engine.Managed().Stop();
    }

    namespace
    {
        void CheckFrameStub()
        {
            for (const bool is64 : { true, false })
            {
                std::vector<std::uint64_t> labels;
                const Result<std::vector<std::uint8_t>> code = Target::FrameCalls::Code(is64, 0x20000000, 0x30000000, &labels);
                CHECK_MSG(code.Ok(), code ? std::string() : code.Error().Text);
                if (!code)
                    continue;
                CHECK(code->size() > 100 && code->size() < 0x1000);
                CHECK_EQ(labels.size(), static_cast<std::size_t>(Target::FrameCalls::kHookSlots) + 5);
                CHECK(std::all_of(labels.begin(), labels.end(), [&](std::uint64_t label) { return label >= 0x20000000 && label < 0x20000000 + code->size(); }));
                CHECK(labels.size() > 1 && labels[0] == 0x20000000 && labels[1] > labels[0]);
            }
        }

        void CheckIl2CppSceneTarget(const std::filesystem::path& targetPath, bool alt)
        {
            Il2CppTarget target;
            const bool started = target.Start(targetPath, alt ? L" --il2cpp-scene --il2cpp-alt" : L" --il2cpp-scene");
            CHECK_MSG(started, "could not start " + WideToUtf8(targetPath.wstring()) + " --il2cpp --il2cpp-scene");
            if (!started)
                return;
            {
                Settings settings;
                Engine engine(settings);
                const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
                CHECK_MSG(opened.Ok(), opened.Text);
                if (opened)
                    CheckIl2CppScene(engine, target.Info.dwThreadId, alt);
                engine.CloseProcess();
            }
            DWORD code = 0;
            ::GetExitCodeProcess(target.Info.hProcess, &code);
            CHECK_MSG(code == STILL_ACTIVE, "the IL2CPP scene target ended with " + Hex(static_cast<std::uint32_t>(code), 8));
        }
    }

    void CheckIl2CppTarget(const std::filesystem::path& targetPath)
    {
        CheckFrameStub();
        CheckIl2CppSceneTarget(targetPath, false);
        CheckIl2CppSceneTarget(targetPath, true);
        Il2CppTarget target;
        const bool started = target.Start(targetPath);
        CHECK_MSG(started, "could not start " + WideToUtf8(targetPath.wstring()) + " --il2cpp");
        if (!started)
            return;
        Settings settings;
        Engine engine(settings);
        const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
        CHECK_MSG(opened.Ok(), opened.Text);
        if (opened)
            CheckIl2Cpp(engine);
        engine.CloseProcess();
    }

    int RunIl2CppCheck(const std::vector<std::string>& arguments)
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        const int before = Totals().Failures;
        const bool target32 = std::find(arguments.begin(), arguments.end(), "--target32") != arguments.end();
        CheckIl2CppTarget(std::filesystem::path(self).parent_path() / (target32 ? L"TestTarget32.exe" : L"TestTarget.exe"));
        const int failures = Totals().Failures - before;
        std::printf("il2cpp check: %d checks, %d failed\n", Totals().Checks, failures);
        return failures == 0 ? 0 : 1;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_MODE("--il2cpp-check", &RunIl2CppCheck);
}
