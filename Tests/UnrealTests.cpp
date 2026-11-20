#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/BackgroundWork.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Table/UnrealStructures.h"
#include "Target/Process.h"
#include "Target/Unreal.h"

#include "Target/FakeUnreal.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <cstdio>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        using Fake = FakeUnreal::ImageOf<std::uint64_t>;
        using FakeUnreal::kObjectCount;
        constexpr std::size_t kFakeSlots = sizeof(Fake) / sizeof(std::uint64_t);

        Fake Widen(const FakeUnreal::Image& image)
        {
            std::uintptr_t narrow[kFakeSlots] = {};
            std::uint64_t wide[kFakeSlots] = {};
            std::memcpy(narrow, &image, sizeof(narrow));
            std::copy(std::begin(narrow), std::end(narrow), std::begin(wide));
            Fake fake{};
            std::memcpy(&fake, wide, sizeof(fake));
            return fake;
        }

        Fake ReadFake(const Target::Process& process, std::uint64_t table)
        {
            std::uint64_t wide[kFakeSlots] = {};
            for (std::size_t i = 0; i < kFakeSlots; ++i)
                wide[i] = process.ReadPointer(table + i * static_cast<std::uint64_t>(process.PointerSize())).value_or(0);
            Fake fake{};
            std::memcpy(&fake, wide, sizeof(fake));
            return fake;
        }

        void SyntheticLayout(Engine& engine, const Fake& fake, const FakeUnreal::Offsets& o, const Target::UnrealConfig& config, const char* label)
        {
            using namespace FakeUnreal;
            Target::UnrealRuntime runtime;
            const Status status = runtime.Resolve(engine.Process(), config, true, nullptr);
            CHECK_MSG(status.Ok(), std::string(label) + ": " + status.Text);
            if (!status)
                return;
            const Target::UnrealSummary& summary = runtime.Summary();
            CHECK_MSG(runtime.PointerSize() == o.Pointer, label);
            CHECK_MSG(summary.NamePool == fake.Pool, label);
            CHECK_MSG(summary.Objects == fake.Objects, label);
            CHECK(summary.ObjectCount == kObjectCount);
            CHECK(!summary.LegacyNames);
            const Target::UnrealLayout& l = runtime.Layout();
            CHECK_MSG(l.Index == o.Index && l.Class == o.Class && l.Name == o.Name && l.Outer == o.Outer, label);
            CHECK_MSG(l.Super == o.Super && l.ChildProperties == o.ChildProperties && l.StructSize == o.StructSize, label);
            CHECK_MSG(l.FField && l.FieldClass == o.FieldClass && l.FieldName == o.FieldName && l.Next == o.FieldNext && l.Offset == o.FieldOffset && l.Size == o.FieldSize &&
                l.Extra == o.Extra && l.Component == 4, label);
            CHECK_MSG(l.ItemSize == o.ItemSize && l.ItemOffset == 0, label);

            CHECK(runtime.Type("actor") == fake.Actor);
            CHECK(runtime.Enum("EMyMode") == fake.Mode);
            CHECK(runtime.CppName(fake.Actor) == "AActor");
            CHECK(runtime.CppName(fake.SceneComponent) == "USceneComponent");
            CHECK(runtime.CppName(fake.Vector) == "FVector");
            CHECK(runtime.StructSize(fake.Actor) == kActorSize);
            CHECK(runtime.FullName(fake.ActorInstance) == "/Script/CoreUObject.MyActor");
            CHECK(runtime.NameOf(fake.Engine) == "GameEngine_1");
            CHECK(runtime.IsObject(fake.ActorInstance));
            CHECK(!runtime.IsObject(fake.ActorInstance + 8));

            const auto fields = runtime.Fields(fake.Actor);
            CHECK(fields.size() == 4);
            if (fields.size() == 4)
            {
                CHECK(fields[0].Name == "bHidden" && fields[0].Bitfield && fields[0].Mask == 0x02 && fields[0].Start() == kHidden);
                CHECK(fields[1].Name == "RootComponent" && runtime.TypeText(fields[1]) == "USceneComponent*" && fields[1].Size == o.Pointer);
                CHECK(fields[2].Name == "Health" && runtime.TypeText(fields[2]) == "float");
                CHECK(fields[3].Name == "Tags" && runtime.TypeText(fields[3]) == "TArray<FName>" && fields[3].Size == o.Pointer + 8);
            }
            CHECK(runtime.FindField(fake.Actor, "health").has_value());
            CHECK(!runtime.FindField(fake.Actor, "Nope").has_value());
            const auto location = runtime.FindField(fake.SceneComponent, "RelativeLocation");
            CHECK(location && runtime.TypeText(*location) == "FVector" && location->Inner == fake.Vector);

            const auto entries = runtime.EnumEntries(fake.Mode);
            CHECK_MSG(entries.size() == 3 && entries.back().Name == "EMyMode_MAX" && entries.back().Value == 2, label);

            CHECK(runtime.Engine() == fake.Engine);
            const auto owner = runtime.OwnerOf(fake.ActorInstance + 0x204);
            CHECK_MSG(owner && owner->first == fake.ActorInstance && owner->second == fake.Actor, label);
            const auto odd = runtime.OwnerOf(fake.ActorInstance + 0x207);
            CHECK_MSG(odd && odd->first == fake.ActorInstance, label);
            CHECK(!runtime.OwnerOf(fake.ActorInstance + 0x2F0).has_value());
            const auto whois = runtime.Whois(fake.ActorInstance + kRoot);
            CHECK_MSG(whois && whois->find("RootComponent  USceneComponent*") != std::string::npos, whois.value_or("-"));
            const auto header = runtime.Whois(fake.ActorInstance + static_cast<std::uint64_t>(o.Outer));
            CHECK_MSG(header && header->find("OuterPrivate") != std::string::npos && header->find("+") != std::string::npos, header.value_or("-"));
            int inside = -1;
            CHECK(runtime.HeaderField(o.Pointer, inside) == "ObjectFlags" && inside == 0);
            CHECK(runtime.HeaderField(o.Class + o.Pointer - 1, inside) == "ClassPrivate" && inside == o.Pointer - 1);
            const auto hit = runtime.Locate(fake.SceneInstance + kLocation + 4);
            CHECK(hit && hit->Field && hit->Field->Name == "Y" && hit->Struct == 0);
            const auto start = runtime.Locate(fake.SceneInstance + kLocation);
            CHECK(start && start->Struct == fake.Vector);

            CHECK(runtime.ResolveSymbol("Actor_RootComponent") == static_cast<std::uint64_t>(kRoot));
            CHECK(runtime.ResolveSymbol("SceneComponent_RelativeLocation") == static_cast<std::uint64_t>(kLocation));
            CHECK(runtime.ResolveSymbol("GObjects") == fake.Objects);
            CHECK(runtime.ResolveSymbol("FNamePool") == fake.Pool);
            CHECK(!runtime.ResolveSymbol("Actor_Nope").has_value());

            const auto walk = runtime.Walk(fake.ActorInstance, "A", { "RootComponent", "RelativeLocation" });
            CHECK_MSG(walk.Ok() && walk->Address == fake.SceneInstance + kLocation && walk->Expression == "[A+Actor_RootComponent]+SceneComponent_RelativeLocation", label);
            CHECK(!runtime.Walk(fake.ActorInstance, "A", { "Nope" }).Ok());
            CHECK_MSG(runtime.Follow(fake.ActorInstance, { "RootComponent" }) == fake.SceneInstance, label);

            const auto firstMember = runtime.ResolveSymbol("Vector_X");
            CHECK(firstMember.has_value() && *firstMember == 0);
            const auto firstMemberAgain = runtime.ResolveSymbol("Vector_X");
            CHECK(firstMemberAgain.has_value() && *firstMemberAgain == 0);
            CHECK(!runtime.ResolveSymbol("Vector_W").has_value() && !runtime.ResolveSymbol("Vector_W").has_value());

            CHECK(engine.Process()->ReadPointer(fake.ActorInstance + kTags) == fake.TagData);
            const auto element = runtime.Walk(fake.ActorInstance, "A", { "Tags[1]" });
            CHECK_MSG(element.Ok() && element->Address == fake.TagData + 8 && element->Expression == "[A+Actor_Tags]+8", label);
            CHECK(runtime.NameAt(fake.TagData + 8) == "Brave");
            CHECK(!runtime.Walk(fake.ActorInstance, "A", { "Tags[2]" }).Ok());
            CHECK(!runtime.Walk(fake.ActorInstance, "A", { "Tags[99999999999]" }).Ok());
            CHECK(!runtime.Walk(fake.ActorInstance, "A", { "Tags[1]", "Health" }).Ok());
            CHECK(!runtime.Walk(fake.ActorInstance, "A", { "Health", "RootComponent" }).Ok());

            const auto actorName = runtime.FindName("Actor");
            const auto numbered = runtime.FindName("Actor_5");
            CHECK(actorName.has_value() && numbered.has_value() && *numbered == (*actorName | (6ull << 32)));
            CHECK(!runtime.FindName("Actor_4294967296").has_value());
            CHECK(!runtime.FindName("Actor_99999999999999999999").has_value());
            CHECK(!runtime.FindName("NoSuchNameAnywhere_3").has_value());

            CHECK(runtime.ObjectAt(0).has_value());
            CHECK(!runtime.ObjectAt(kObjectCount).has_value() && !runtime.ObjectAt(0x7FFFFFFF).has_value() && !runtime.ObjectAt(-1).has_value());

            WorkProgress stopped;
            stopped.Cancel();
            Target::UnrealRuntime abandoned;
            const Status search = abandoned.Resolve(engine.Process(), Target::UnrealConfig{}, true, &stopped);
            CHECK_MSG(!search.Ok() && search.Text.find("cancelled") != std::string::npos, search.Text);

            const auto instances = runtime.Instances(fake.Actor, 0);
            CHECK(instances.size() == 1 && instances.front() == fake.ActorInstance);

            const Table::UnrealFill fill = Table::FillUnrealObject(runtime, fake.ActorInstance, true);
            const auto named = [&](const std::string& prefix) { return std::find_if(fill.Fields.begin(), fill.Fields.end(), [&](const Table::StructField& f) { return f.Name.rfind(prefix, 0) == 0; }); };
            CHECK(named("Health") != fill.Fields.end() && named("Health")->Offset == kHealth && named("Health")->Type == Scan::ValueType::Float);
            CHECK(named("VTable") != fill.Fields.end() && named("VTable")->Offset == 0 && named("VTable")->Size == o.Pointer);
            CHECK(named("ObjectFlags") != fill.Fields.end() && named("ObjectFlags")->Offset == o.Pointer);
            CHECK(named("ClassPrivate") != fill.Fields.end() && named("ClassPrivate")->Offset == o.Class && named("ClassPrivate")->Type == Scan::ValueType::Pointer);
            CHECK(named("NamePrivate") != fill.Fields.end() && named("NamePrivate")->CustomType == Target::kUnrealFNameType && named("NamePrivate")->Offset == o.Name);
            CHECK(named("OuterPrivate") != fill.Fields.end() && named("OuterPrivate")->Offset == o.Outer && named("OuterPrivate")->Size == o.Pointer);
            CHECK(named("bHidden") != fill.Fields.end() && named("bHidden")->Type == Scan::ValueType::Binary && named("bHidden")->BitStart == 1);
            CHECK(named("RootComponent") != fill.Fields.end() && named("RootComponent")->Type == Scan::ValueType::Pointer && named("RootComponent")->Size == o.Pointer);
            CHECK(named("Tags.Num") != fill.Fields.end() && named("Tags.Num")->Offset == kTags + o.Pointer);
            CHECK(named("Tags.Max") != fill.Fields.end() && named("Tags.Max")->Offset == kTags + o.Pointer + 4);
            CHECK(named("(not reflected)") != fill.Fields.end() && named("(not reflected)")->Offset == o.Outer + o.Pointer);
            bool overlap = false;
            for (std::size_t i = 1; i < fill.Fields.size(); ++i)
                overlap = overlap || (fill.Fields[i].Type != Scan::ValueType::Binary && fill.Fields[i - 1].Type != Scan::ValueType::Binary &&
                    fill.Fields[i].Offset < fill.Fields[i - 1].Offset + fill.Fields[i - 1].Size);
            CHECK_MSG(!overlap, label);
            CHECK(fill.Arrays.size() == 1);
            const Table::UnrealFill vector = Table::FillFromUnreal(runtime, fake.SceneInstance + kLocation, {}, true);
            CHECK(vector.Handled && vector.Fields.size() == 3 && vector.Fields[2].Name == "Z" && vector.Fields[2].Offset == 8);
            if (!fill.Arrays.empty())
            {
                const Table::UnrealArrayRef array = fill.Arrays.front();
                CHECK(array.Data == fake.TagData && array.Header == fake.ActorInstance + kTags);
                const auto lookup = [&](std::uint64_t data) -> std::optional<std::pair<Target::UnrealField, std::uint64_t>>
                    {
                        if (data != array.Data)
                            return std::nullopt;
                        return std::make_pair(array.Field, array.Header);
                    };
                const Table::UnrealFill items = Table::FillFromUnreal(runtime, array.Data, lookup, true);
                CHECK(items.Handled && items.Fields.size() == 2 && items.Fields[1].Name == "[1]" && items.Fields[1].CustomType == Target::kUnrealFNameType);
                const auto name = Table::UnrealStructureName(runtime, array.Data, false, lookup);
                CHECK_MSG(name && name->first == "TArray<FName>[2]", label);
            }
            const auto name = Table::UnrealStructureName(runtime, fake.ActorInstance + 0x10, true, {});
            CHECK(name && name->first == "Actor" && name->second == fake.ActorInstance);
        }

        void SessionAndLua(Engine& engine, const Fake& fake, const FakeUnreal::Offsets& o)
        {
            using namespace FakeUnreal;
            UnrealSession& session = engine.Unreal();
            session.Config().NamePool = fake.Pool;
            session.Config().Objects = fake.Objects;
            const Status status = session.ResolveNow();
            CHECK_MSG(status.Ok(), status.Text);
            CHECK(session.Ready());
            const Lua::CustomType* fname = engine.Lua().FindCustomType(Target::kUnrealFNameType);
            CHECK(fname != nullptr);
            if (fname)
            {
                std::uint8_t bytes[8] = {};
                CHECK(engine.Process()->ReadExact(fake.ActorInstance + static_cast<std::uint64_t>(o.Name), bytes, 8));
                std::string text;
                CHECK(engine.Lua().CustomToText(*fname, bytes, 0, text) && text == "MyActor");
                std::vector<std::uint8_t> back;
                CHECK(engine.Lua().CustomFromText(*fname, "MyActor", 0, back) && back.size() == 8 && std::memcmp(back.data(), bytes, 8) == 0);
            }
            const Lua::CustomType* fstring = engine.Lua().FindCustomType(Target::kUnrealFStringType);
            CHECK(fstring != nullptr);
            if (fstring)
            {
                std::uint8_t bytes[16] = {};
                const std::int32_t count = 6;
                std::memcpy(bytes, &fake.Text, static_cast<std::size_t>(o.Pointer));
                std::memcpy(bytes + o.Pointer, &count, 4);
                std::memcpy(bytes + o.Pointer + 4, &count, 4);
                std::string text;
                CHECK_MSG(engine.Lua().CustomToText(*fstring, bytes, 0, text) && text == "Hello", text);
                CHECK(fstring->ByteCount() == o.Pointer + 8);
                std::string narrow;
                CHECK_MSG(engine.Lua().CustomToText(*fstring, std::span<const std::uint8_t>(bytes, static_cast<std::size_t>(o.Pointer) + 8), 0, narrow) && narrow == "Hello", narrow);
            }
            const Lua::CustomType* index = engine.Lua().FindCustomType(Target::kUnrealObjectIndexType);
            CHECK(index != nullptr);
            if (index)
            {
                const std::uint8_t bytes[8] = { 16, 0, 0, 0, 0, 0, 0, 0 };
                std::string text;
                CHECK_MSG(engine.Lua().CustomToText(*index, bytes, 0, text) && text == "MyActor (Actor)", text);
            }
            const auto symbol = engine.Evaluate("[" + Hex(fake.ActorInstance) + "+Actor_RootComponent]+SceneComponent_RelativeLocation");
            CHECK(symbol == fake.SceneInstance + kLocation);
            std::string result;
            const std::string code = Format(R"LUA(
                local unreal = manifold.unreal
                local actor = 0x%llX
                local extra = %d
                if unreal.status().state ~= "ready" then return "state " .. unreal.status().state end
                local symbol, offset = unreal.offset("Actor", "RootComponent")
                if symbol ~= "Actor_RootComponent" or offset ~= 0x130 then return "offset " .. tostring(symbol) end
                if unreal.field("Actor", "Health").typeText ~= "float" then return "health" end
                if unreal.cppName("Actor") ~= "AActor" then return "cpp" end
                local vector = unreal.fields("Vector")
                if #vector ~= 3 or vector[2].name ~= "Y" then return "vector" end
                if #unreal.enum("EMyMode") ~= 3 then return "enum" end
                if unreal.engine() == nil then return "engine" end
                local address, expression = unreal.walk(actor, { "RootComponent", "RelativeLocation" }, "A")
                if expression ~= "[A+Actor_RootComponent]+SceneComponent_RelativeLocation" then return "walk " .. tostring(expression) end
                if not unreal.isObject(actor) or unreal.isObject(actor + 8) then return "isObject" end
                if not unreal.claims(actor + 0x200) then return "claims" end
                local object, class = unreal.owner(actor + 0x204)
                if object ~= actor or class ~= unreal.type("Actor") then return "owner" end
                local none, problem = unreal.field("Actor", "Nope")
                if none ~= nil or problem.code ~= "ME-4203" then return "no property" end
                local missing, typeProblem = unreal.fields("AActor")
                if missing ~= nil or typeProblem.code ~= "ME-4202" then return "unknown type" end
                if #unreal.dump("Actor") ~= 5 then return "dump" end
                unreal.configure({ symbols = false })
                if manifold.process.address("Actor_RootComponent") ~= nil then return "symbols still on" end
                unreal.configure({ symbols = true, layout = { extra = extra } })
                if unreal.config().layout.extra ~= extra then return "config layout" end
                unreal.configure({ layout = false })
                if next(unreal.config().layout) ~= nil then return "layout not cleared" end
                return "ok")LUA",
                static_cast<unsigned long long>(fake.ActorInstance), o.Extra);
            const Status lua = engine.Lua().RunWithResult(code, "unreal api", result);
            CHECK_MSG(lua.Ok() && result == "ok", lua.Ok() ? result : lua.Text);
            session.Stop();
            CHECK(session.Stopped() && !session.Ready());
            CHECK(!engine.Evaluate("Actor_RootComponent").has_value());
            session.Config() = Target::UnrealConfig{};
        }

        void AutorunSkip(Engine& engine)
        {
            const std::filesystem::path folder = std::filesystem::temp_directory_path() / ("manifold-ue-skip-" + std::to_string(::GetCurrentProcessId()));
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            const std::filesystem::path old = folder / "Manifold-UE.lua";
            const std::filesystem::path other = folder / "Other.lua";
            {
                std::ofstream(old) << "local RUNTIME_STATE_KEY = \"__MANIFOLD_UE_RUNTIME__\"\nOLD_UE_RAN = true\n";
                std::ofstream(other) << "OTHER_RAN = true\n";
            }
            CHECK(engine.Lua().RunAutorun(folder).Ok());
            std::string result;
            CHECK(engine.Lua().RunWithResult("return tostring(OLD_UE_RAN) .. ' ' .. tostring(OTHER_RAN)", "autorun skip", result).Ok() && result == "nil true");
            std::filesystem::remove(old, ignored);
            std::filesystem::remove(other, ignored);
            std::filesystem::remove(folder, ignored);
        }

        int RunUnrealLive(const std::vector<std::string>& arguments)
        {
            std::string name;
            for (const std::string& argument : arguments)
            {
                if (argument.rfind("--process=", 0) == 0)
                    name = argument.substr(10);
            }
            Settings settings;
            Engine engine(settings);
            engine.Lua().Run("__manifoldHeadless = true", "headless");
            engine.Unreal().Config().AutoStart = false;
            const Status opened = engine.OpenProcessByName(name);
            if (!opened)
            {
                std::printf("%s\n", opened.Text.c_str());
                return 1;
            }
            Target::UnrealRuntime runtime;
            const auto started = std::chrono::steady_clock::now();
            const Status status = runtime.Resolve(engine.Process(), Target::UnrealConfig{}, false, nullptr);
            std::printf("resolve: %s (%.2f s)\n", status.Ok() ? "ok" : status.Text.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            if (!status)
            {
                engine.CloseProcess();
                return 1;
            }
            const Target::UnrealSummary& s = runtime.Summary();
            const Target::UnrealLayout& l = runtime.Layout();
            std::printf("names %s at %llX, objects at %llX (%s), %d objects, %zu types, %s\n", s.LegacyNames ? "GNames" : "FNamePool", static_cast<unsigned long long>(s.NamePool),
                static_cast<unsigned long long>(s.Objects), s.ObjectsSource.c_str(), s.ObjectCount, s.TypeCount, s.Timings.c_str());
            std::printf("layout: item %X+%X index %X class %X name %X outer %X super %X props %X fname %X next %X offset %X size %X extra %X structsize %X children %X/%X flags %X fclass %X %s %d\n",
                l.ItemSize, l.ItemOffset, l.Index, l.Class, l.Name, l.Outer, l.Super, l.ChildProperties, l.FieldName, l.Next, l.Offset, l.Size, l.Extra, l.StructSize,
                l.Children, l.ChildNext, l.FunctionFlags, l.FieldClass, l.FField ? "FField" : "UProperty", l.Component);
            const std::uint64_t actor = runtime.Type("Actor");
            const auto fields = runtime.Fields(actor);
            std::printf("Actor %llX: %zu fields, size %d, cpp %s\n", static_cast<unsigned long long>(actor), fields.size(), runtime.StructSize(actor).value_or(-1), runtime.CppName(actor).c_str());
            for (std::size_t i = 0; i < fields.size() && i < 8; ++i)
                std::printf("  +%04X %-40s %s (%s)\n", fields[i].Start(), runtime.TypeText(fields[i]).c_str(), fields[i].Name.c_str(), fields[i].Owner.c_str());
            const std::uint64_t world = runtime.World();
            const std::uint64_t controller = runtime.Controller();
            const std::uint64_t pawn = runtime.Pawn();
            std::printf("engine %llX %s | world %llX %s | controller %llX %s | pawn %llX %s\n", static_cast<unsigned long long>(runtime.Engine()),
                runtime.FullName(runtime.Engine()).c_str(), static_cast<unsigned long long>(world), runtime.NameOf(world).value_or("-").c_str(),
                static_cast<unsigned long long>(controller), runtime.NameOf(runtime.ClassOf(controller)).value_or("-").c_str(), static_cast<unsigned long long>(pawn),
                runtime.NameOf(runtime.ClassOf(pawn)).value_or("-").c_str());
            if (pawn)
            {
                for (const std::uint64_t offset : { 0x10ull, 0x130ull, 0x200ull })
                    std::printf("whois pawn+%llX: %s\n", static_cast<unsigned long long>(offset), runtime.Whois(pawn + offset).value_or("-").c_str());
                const auto started2 = std::chrono::steady_clock::now();
                const Table::UnrealFill fill = Table::FillUnrealObject(runtime, pawn, true);
                std::printf("pawn dissect: %zu fields, %zu arrays (%.3f s)\n", fill.Fields.size(), fill.Arrays.size(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started2).count());
                for (std::size_t i = 0; i < fill.Fields.size() && i < 12; ++i)
                    std::printf("  +%04X %s%s\n", fill.Fields[i].Offset, fill.Fields[i].Name.c_str(), fill.Fields[i].CustomType.empty() ? "" : ("  [" + fill.Fields[i].CustomType + "]").c_str());
            }
            const auto functions = runtime.Functions(runtime.Type("PlayerController"), false);
            std::printf("PlayerController: %zu own functions", functions.size());
            if (!functions.empty())
                std::printf(", first %s flags %X with %zu parameters", functions.front().Name.c_str(), functions.front().Flags, functions.front().Parameters.size());
            std::printf("\n");
            const std::shared_ptr<const Target::UnrealTypeIndex> index = runtime.Index();
            for (const auto& type : index->List)
            {
                if (type.Kind != Target::UnrealKind::Enum)
                    continue;
                const auto entries = runtime.EnumEntries(type.Object);
                if (entries.size() < 3)
                    continue;
                std::printf("enum %s: %zu entries, first %s = %lld\n", type.Name.c_str(), entries.size(), entries.front().Name.c_str(), static_cast<long long>(entries.front().Value));
                break;
            }
            std::printf("symbol Actor_RootComponent = %s, SceneComponent_RelativeLocation = %s, offline %s\n",
                runtime.ResolveSymbol("Actor_RootComponent") ? Hex(*runtime.ResolveSymbol("Actor_RootComponent")).c_str() : "-",
                runtime.ResolveSymbol("SceneComponent_RelativeLocation") ? Hex(*runtime.ResolveSymbol("SceneComponent_RelativeLocation")).c_str() : "-",
                runtime.IsOffline() ? (*runtime.IsOffline() ? "yes" : "no") : "?");
            engine.CloseProcess();
            return 0;
        }
    }
}

namespace Manifold::Tests
{
    namespace
    {
        struct UnrealTarget
        {
            PROCESS_INFORMATION Info{};

            UnrealTarget() = default;
            UnrealTarget(const UnrealTarget&) = delete;
            UnrealTarget& operator=(const UnrealTarget&) = delete;

            ~UnrealTarget()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }

            bool Start(const std::filesystem::path& path)
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
                std::wstring command = L"\"" + path.wstring() + L"\" --unreal";
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

        void SyntheticTarget(const std::filesystem::path& targetPath, const FakeUnreal::Offsets& o, const char* label)
        {
            std::error_code missing;
            if (!std::filesystem::exists(targetPath, missing))
            {
                Skip("the test target " + targetPath.string() + " was not built");
                return;
            }
            UnrealTarget target;
            const bool started = target.Start(targetPath);
            CHECK_MSG(started, "could not start " + WideToUtf8(targetPath.wstring()) + " --unreal");
            if (!started)
                return;
            Settings settings;
            Engine engine(settings);
            engine.Lua().Run("__manifoldHeadless = true", "headless");
            engine.Unreal().Config().AutoStart = false;
            const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return;
            const Target::Process& process = *engine.Process();
            CHECK_MSG(process.PointerSize() == o.Pointer, label);
            const std::string stem = std::filesystem::path(process.Name()).stem().string();
            const std::uint64_t table = engine.Evaluate(stem + ".g_unrealFake").value_or(0);
            CHECK_MSG(table != 0, label);
            const Fake fake = ReadFake(process, table);
            CHECK_MSG(fake.Pool != 0 && fake.Objects != 0 && fake.ActorInstance != 0, label);
            if (fake.Pool && fake.Objects && fake.ActorInstance)
            {
                CHECK_MSG(fake.ActorInstance % 8 == static_cast<std::uint64_t>(o.ObjectBias), label);
                SyntheticLayout(engine, fake, o, Target::UnrealConfig{}, label);
                Target::UnrealConfig configured;
                configured.NamePool = fake.Pool;
                configured.Objects = fake.Objects;
                configured.Layout["extra"] = o.Extra;
                SyntheticLayout(engine, fake, o, configured, label);
                Target::UnrealConfig pattern;
                pattern.NamePatterns.push_back({ .Bytes = "4D 46 55 45 50 4F 4F 4C", .Offset = 8, .Relative = false });
                pattern.ObjectPatterns.push_back({ .Bytes = "4D 46 55 45 4F 42 4A 53", .Offset = 8, .Relative = false });
                Target::UnrealRuntime byPattern;
                const Status found = byPattern.Resolve(engine.Process(), pattern, true, nullptr);
                CHECK_MSG(found.Ok() && byPattern.Summary().ObjectsSource == "pattern" && byPattern.Summary().NamePool == fake.Pool && byPattern.Summary().Objects == fake.Objects,
                    std::string(label) + ": " + (found.Ok() ? byPattern.Summary().ObjectsSource : found.Text));
                SessionAndLua(engine, fake, o);
            }
            engine.CloseProcess();
            DWORD code = 0;
            ::GetExitCodeProcess(target.Info.hProcess, &code);
            CHECK_MSG(code == STILL_ACTIVE, std::string(label) + ": the target ended with " + Hex(code, 8));
        }
    }

    int RunUnrealTests(const std::filesystem::path& target)
    {
        const int before = Totals().Failures;
        Settings settings;
        Engine engine(settings);
        engine.Lua().Run("__manifoldHeadless = true", "headless");
        engine.Unreal().Config().AutoStart = false;
        AutorunSkip(engine);
        const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
        CHECK_MSG(opened.Ok(), opened.Text);
        if (!opened)
            return Totals().Failures - before;
        {
            FakeUnreal::Builder builder;
            CHECK(builder.Build());
            const Fake fake = Widen(builder.Fake);
            const FakeUnreal::Offsets& o = FakeUnreal::kNative;
            SyntheticLayout(engine, fake, o, Target::UnrealConfig{}, "search");
            Target::UnrealConfig configured;
            configured.NamePool = fake.Pool;
            configured.Objects = fake.Objects;
            configured.Layout["extra"] = o.Extra;
            SyntheticLayout(engine, fake, o, configured, "configured");
            {
                Target::UnrealRuntime late;
                CHECK(late.Resolve(engine.Process(), configured, true, nullptr).Ok());
                const std::shared_ptr<const Target::UnrealTypeIndex> first = late.Index();
                CHECK(!late.RefreshTypes());
                CHECK(late.Index() == first);
                CHECK(late.Type("LateBlueprint_C") == 0);
                CHECK(!late.ResolveSymbol("LateBlueprint_C"));
                const std::uint64_t loaded = builder.LoadClass(1500, "LateBlueprint_C");
                CHECK(late.Type("LateBlueprint_C") == 0);
                CHECK(late.RefreshTypes());
                CHECK(late.Type("lateblueprint_c") == loaded);
                CHECK(late.Index() != first && late.Index()->List.size() == first->List.size() + 1);
                CHECK(first->Types.count("lateblueprint_c") == 0);
                CHECK(late.Type("Actor") == fake.Actor && late.Enum("EMyMode") == fake.Mode);
                CHECK(!late.RefreshTypes());
            }
            Target::UnrealConfig wrong;
            wrong.NamePool = fake.Objects;
            Target::UnrealRuntime runtime;
            const Status refused = runtime.Resolve(engine.Process(), wrong, true, nullptr);
            CHECK(!refused.Ok() && refused.Code == ErrorCode::UnrealNotResolved && refused.Text.find("configured name table") != std::string::npos);
            Target::UnrealRuntime gated;
            const Status rejected = gated.Resolve(engine.Process(), Target::UnrealConfig{}, false, nullptr);
            CHECK(!rejected.Ok() && gated.Rejected());
            SessionAndLua(engine, fake, o);
        }
        engine.CloseProcess();
        SyntheticTarget(target.parent_path() / L"TestTarget32.exe", FakeUnreal::kOffsets32, "32-bit target");
        SyntheticTarget(target.parent_path() / L"TestTarget.exe", FakeUnreal::kOffsets64, "64-bit target");
        return Totals().Failures - before;
    }

    MANIFOLD_SUITE("unreal", "Unreal Engine", 36, [](const SuiteContext& context) { return RunUnrealTests(context.Target); });
    MANIFOLD_MODE("--unreal-check", [](const std::vector<std::string>& arguments) { return RunUnrealLive(arguments); });
}
