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
#include "Target/FakeUnreal3.h"

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

        using Fake3 = FakeUnreal3::ImageOf<std::uint64_t>;
        constexpr std::size_t kFake3Slots = sizeof(Fake3) / sizeof(std::uint64_t);

        Fake3 Widen3(const FakeUnreal3::Image& image)
        {
            std::uintptr_t narrow[kFake3Slots] = {};
            std::uint64_t wide[kFake3Slots] = {};
            std::memcpy(narrow, &image, sizeof(narrow));
            std::copy(std::begin(narrow), std::end(narrow), std::begin(wide));
            Fake3 fake{};
            std::memcpy(&fake, wide, sizeof(fake));
            return fake;
        }

        Fake3 ReadFake3(const Target::Process& process, std::uint64_t table)
        {
            std::uint64_t wide[kFake3Slots] = {};
            for (std::size_t i = 0; i < kFake3Slots; ++i)
                wide[i] = process.ReadPointer(table + i * static_cast<std::uint64_t>(process.PointerSize())).value_or(0);
            Fake3 fake{};
            std::memcpy(&fake, wide, sizeof(fake));
            return fake;
        }

        const FakeUnreal3::Offsets& Ue3Offsets(int pointer, bool early)
        {
            if (pointer == 8)
                return early ? FakeUnreal3::kEarly64 : FakeUnreal3::kLate64;
            return early ? FakeUnreal3::kEarly32 : FakeUnreal3::kLate32;
        }

        void Ue3Layout(Engine& engine, const Fake3& fake, const FakeUnreal3::Offsets& o, const Target::UnrealConfig& config, const std::string& label)
        {
            using namespace FakeUnreal3;
            Target::UnrealRuntime runtime;
            const Status status = runtime.Resolve(engine.Process(), config, true, nullptr);
            const Target::UnrealLayout& got = runtime.Layout();
            CHECK_MSG(status.Ok(), label + ": " + status.Text + Format(" | item %X index %X class %X name %X outer %X super %X props %X fname %X next %X offset %X size %X",
                got.ItemSize, got.Index, got.Class, got.Name, got.Outer, got.Super, got.ChildProperties, got.FieldName, got.Next, got.Offset, got.Size));
            if (!status)
                return;
            const Target::UnrealSummary& s = runtime.Summary();
            CHECK_MSG(s.NamePool == fake.Names && s.LegacyNames && s.FlatNames, label + ": names at " + Hex(s.NamePool, 0) + ", expected " + Hex(fake.Names, 0));
            CHECK_MSG(s.Objects == fake.Objects && s.ObjectCount == kObjectCount, label + ": objects at " + Hex(s.Objects, 0));
            const Target::UnrealLayout& l = runtime.Layout();
            CHECK_MSG(l.Ue3 && !l.FField && !l.Chunked && l.ItemSize == o.Pointer && l.ItemOffset == 0, label);
            CHECK_MSG(l.Index == o.Index && l.Class == o.Class && l.Name == o.Name && l.Outer == o.Outer,
                Format("%s: index %X class %X name %X outer %X", label.c_str(), l.Index, l.Class, l.Name, l.Outer));
            CHECK_MSG(l.Super == o.Super && l.ChildProperties == o.Children && l.StructSize == o.PropertySize && l.FieldName == o.Name && l.Next == o.Next,
                Format("%s: super %X children %X size %X fname %X next %X", label.c_str(), l.Super, l.ChildProperties, l.StructSize, l.FieldName, l.Next));
            CHECK_MSG(l.Offset == o.Offset && l.Size == o.ArrayDim + 4 && l.Extra == o.Extra && l.Component == 4,
                Format("%s: offset %X size %X extra %X", label.c_str(), l.Offset, l.Size, l.Extra));
            CHECK_MSG(l.Children == o.Children && l.ChildNext == o.Next && l.FunctionFlags == o.FunctionFlags, Format("%s: function flags %X", label.c_str(), l.FunctionFlags));
            CHECK_MSG(l.StructSize16 == (o.Pointer == 4 && !fake.Early), label + ": 16-bit PropertySize");

            CHECK_MSG(runtime.Type("actor") == fake.Actor && runtime.Enum("EMyMode") == fake.Mode && runtime.Type("State") == fake.StateClass, label);
            CHECK_MSG(runtime.CppName(fake.Actor) == "AActor" && runtime.CppName(fake.Pawn) == "APawn" && runtime.CppName(fake.Vector) == "FVector" &&
                runtime.CppName(fake.LocalPlayer) == "ULocalPlayer", label);
            CHECK_MSG(runtime.StructSize(fake.Actor) == kActorSize && runtime.StructSize(fake.Vector) == 12, label);
            CHECK_MSG(runtime.FullName(fake.ActorInstance) == "Engine.MyActor" && runtime.FullName(fake.Vector) == "Core.Object.Vector", label + ": " + runtime.FullName(fake.Vector));
            CHECK_MSG(runtime.NameOf(fake.Engine) == "GameEngine_1" && runtime.NameOf(fake.PlayerInstance) == "LocalPlayer_0", label);
            CHECK_MSG(runtime.NameOf(fake.PawnInstance) == "MyPawn", label + ": wide name " + runtime.NameOf(fake.PawnInstance).value_or("-"));
            CHECK_MSG(runtime.NameText(FakeUnreal3::kNameCount + 100) == std::nullopt, label);
            CHECK_MSG(runtime.IsObject(fake.ActorInstance) && !runtime.IsObject(fake.ActorInstance + 8), label);

            const auto fields = runtime.Fields(fake.Actor);
            CHECK_MSG(fields.size() == 8, label + ": " + std::to_string(fields.size()) + " Actor fields");
            if (fields.size() == 8)
            {
                CHECK_MSG(fields[0].Name == "Outer" && fields[0].Offset == o.Outer && fields[0].Owner == "Object" && runtime.TypeText(fields[0]) == "UObject*", label);
                CHECK_MSG(fields[1].Name == "Owner" && runtime.TypeText(fields[1]) == "AActor*" && fields[1].Offset == kOwner && fields[1].Size == o.Pointer, label);
                CHECK_MSG(fields[2].Name == "Location" && runtime.TypeText(fields[2]) == "FVector" && fields[2].Inner == fake.Vector, label);
                CHECK_MSG(fields[3].Name == "bHidden" && fields[3].Bitfield && fields[3].Mask == 0x04 && fields[3].Start() == kBits, label);
                CHECK_MSG(fields[4].Name == "bCanTeleport" && fields[4].Bitfield && fields[4].Mask == 0x02 && fields[4].Start() == kBits + 1 && fields[4].MaskedBit(), label);
                CHECK_MSG(fields[5].Name == "Health" && runtime.TypeText(fields[5]) == "float" && fields[5].Flags == 1, label);
                CHECK_MSG(fields[6].Name == "Tags" && runtime.TypeText(fields[6]) == "TArray<FName>" && fields[6].Size == o.Pointer + 8, label + ": " + runtime.TypeText(fields[6]));
                CHECK_MSG(fields[7].Name == "WorldInfo" && runtime.TypeText(fields[7]) == "AWorldInfo*", label);
            }
            CHECK_MSG(runtime.Fields(fake.Vector).size() == 3 && runtime.Fields(fake.ObjectClass).size() == 1, label);
            const auto entries = runtime.EnumEntries(fake.Mode);
            CHECK_MSG(entries.size() == 3 && entries[1].Name == "MM_B" && entries[1].Value == 1 && entries.back().Name == "MM_MAX" && entries.back().Value == 2, label);
            const auto own = runtime.Functions(fake.PlayerController, false);
            CHECK_MSG(own.size() == 1 && own.front().Name == "FOV" && own.front().Flags == kFuncExec, label);
            CHECK_MSG(runtime.Functions(fake.Actor, false).size() == 2 && runtime.Functions(fake.Pawn, true).size() == 3, label);

            CHECK_MSG(runtime.Engine() == fake.Engine, label);
            CHECK_MSG(runtime.Controller() == fake.ControllerInstance, label);
            CHECK_MSG(runtime.Pawn() == fake.PawnInstance, label);
            CHECK_MSG(runtime.World() == fake.WorldInfo, label);
            const auto pawns = runtime.Instances(fake.Pawn, 0);
            CHECK_MSG(pawns.size() == 1 && pawns.front() == fake.PawnInstance, label);
            CHECK_MSG(runtime.Instances(fake.Actor, 0).size() == 4, label + ": Default__Actor must not count");

            const auto owner = runtime.Whois(fake.ActorInstance + kOwner);
            CHECK_MSG(owner && owner->find("Owner  AActor*") != std::string::npos, owner.value_or("-"));
            const auto bit = runtime.Whois(fake.ActorInstance + kBits + 1);
            CHECK_MSG(bit && bit->find("bCanTeleport") != std::string::npos, bit.value_or("-"));
            const auto outer = runtime.Whois(fake.ActorInstance + static_cast<std::uint64_t>(o.Outer));
            CHECK_MSG(outer && outer->find(": Outer") != std::string::npos, outer.value_or("-"));
            int inside = -1;
            CHECK_MSG(runtime.HeaderField(0, inside) == "VfTableObject" && runtime.HeaderField(o.Index, inside) == "ObjectInternalInteger", label);

            CHECK_MSG(runtime.ResolveSymbol("Actor_Health") == static_cast<std::uint64_t>(kHealth) && runtime.ResolveSymbol("PlayerController_Pawn") == static_cast<std::uint64_t>(kPawn), label);
            const auto walk = runtime.Walk(fake.ActorInstance, "A", { "Location" });
            CHECK_MSG(walk.Ok() && walk->Address == fake.ActorInstance + kLocation && walk->Expression == "A+Actor_Location", label);
            const auto chain = runtime.Walk(fake.Engine, "E", { "GamePlayers[0]", "Actor", "Pawn" });
            CHECK_MSG(chain.Ok() && chain->Address == fake.ControllerInstance + kPawn, label + ": " + (chain.Ok() ? chain->Expression : chain.Error().Text));
            CHECK_MSG(runtime.NameAt(fake.TagData + 8) == "Brave", label);

            const Table::UnrealFill fill = Table::FillUnrealObject(runtime, fake.ActorInstance, true);
            const auto named = [&](const std::string& prefix) { return std::find_if(fill.Fields.begin(), fill.Fields.end(), [&](const Table::StructField& f) { return f.Name.rfind(prefix, 0) == 0; }); };
            CHECK_MSG(named("VfTableObject") != fill.Fields.end() && named("VfTableObject")->Offset == 0, label);
            CHECK_MSG(named("Outer") != fill.Fields.end() && named("Outer")->Offset == o.Outer && named("Outer")->Size == o.Pointer, label);
            const auto outers = std::count_if(fill.Fields.begin(), fill.Fields.end(), [](const Table::StructField& f) { return f.Name.rfind("Outer", 0) == 0; });
            CHECK_MSG(outers == 1, label + ": Outer is reflected and must not be added again by the header (" + std::to_string(outers) + "x)");
            CHECK_MSG(named("Name") != fill.Fields.end() && named("Name")->CustomType == Target::kUnrealFNameType && named("Name")->Offset == o.Name, label);
            CHECK_MSG(named("ObjectFlags") == fill.Fields.end(), label);
            CHECK_MSG(named("Health") != fill.Fields.end() && named("Health")->Offset == kHealth && named("Health")->Type == Scan::ValueType::Float, label);
            CHECK_MSG(named("bCanTeleport") != fill.Fields.end() && named("bCanTeleport")->Type == Scan::ValueType::Binary && named("bCanTeleport")->Offset == kBits + 1 &&
                named("bCanTeleport")->BitStart == 1, label);
            CHECK_MSG(named("Tags.Num") != fill.Fields.end() && named("Tags.Num")->Offset == kTags + o.Pointer, label);
        }

        void Ue3SessionAndLua(Engine& engine, const Fake3& fake)
        {
            UnrealSession& session = engine.Unreal();
            session.Config().NamePool = fake.Names;
            session.Config().LegacyNames = true;
            session.Config().Objects = fake.Objects;
            const Status status = session.ResolveNow();
            CHECK_MSG(status.Ok(), status.Text);
            CHECK_MSG(session.Ready() && session.StatusText().rfind("UE3 ", 0) == 0, session.StatusText());
            std::string result;
            const std::string code = Format(R"LUA(
                local unreal = manifold.unreal
                if not unreal.status().ue3 or not unreal.layout().ue3 then return "ue3 flag" end
                if unreal.field("Actor", "Health").typeText ~= "float" then return "health" end
                if unreal.cppName("PlayerController") ~= "APlayerController" then return "cpp" end
                if #unreal.enum("EMyMode") ~= 3 then return "enum" end
                if unreal.controller() ~= 0x%llX then return "controller " .. tostring(unreal.controller()) end
                if unreal.pawn() ~= 0x%llX then return "pawn" end
                local symbol, offset = unreal.offset("Actor", "Location")
                if symbol ~= "Actor_Location" or offset ~= 0x110 then return "offset " .. tostring(symbol) end
                return "ok")LUA",
                static_cast<unsigned long long>(fake.ControllerInstance), static_cast<unsigned long long>(fake.PawnInstance));
            const Status lua = engine.Lua().RunWithResult(code, "unreal 3 api", result);
            CHECK_MSG(lua.Ok() && result == "ok", lua.Ok() ? result : lua.Text);
            session.Stop();
            CHECK(session.Stopped() && !session.Ready());
            session.Config() = Target::UnrealConfig{};
        }

        void Ue3Checks(Engine& engine, const Fake3& fake, const FakeUnreal3::Offsets& o, const std::string& label)
        {
            Ue3Layout(engine, fake, o, Target::UnrealConfig{}, label + " (search)");
            Target::UnrealConfig configured;
            configured.NamePool = fake.Names;
            configured.LegacyNames = true;
            configured.Objects = fake.Objects;
            Ue3Layout(engine, fake, o, configured, label + " (configured)");
            Ue3SessionAndLua(engine, fake);
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
            CHECK_MSG(summary.NamePool == fake.Pool, std::string(label) + ": found " + Hex(summary.NamePool, 0) + ", expected " + Hex(fake.Pool, 0));
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

        void VariantLayout(Engine& engine, const FakeUnreal::Variant& variant, const char* label)
        {
            using namespace FakeUnreal;
            Builder builder;
            CHECK_MSG(builder.Build(variant), label);
            const Fake fake = Widen(builder.Fake);
            Target::UnrealRuntime runtime;
            const Status status = runtime.Resolve(engine.Process(), Target::UnrealConfig{}, true, nullptr);
            CHECK_MSG(status.Ok(), std::string(label) + ": " + status.Text);
            if (!status)
                return;
            const int component = variant.Doubles ? 8 : 4;
            const int locationAt = variant.Doubles ? kLocationDouble : kLocation;
            const Target::UnrealLayout& l = runtime.Layout();
            CHECK_MSG(l.Component == component && l.Offset == kNative.FieldOffset && l.Size == kNative.FieldSize && l.Next == kNative.FieldNext, label);
            CHECK_MSG(runtime.Type("Field") == fake.FieldClass, label);
            if (variant.ShadowField)
                CHECK_MSG(builder.ShadowField < fake.FieldClass && runtime.Index()->List.size() > 0, label);
            CHECK_MSG(runtime.StructSize(fake.Vector) == 3 * component, label);
            const auto location = runtime.FindField(fake.SceneComponent, "RelativeLocation");
            CHECK_MSG(location && location->Size == 3 * component && location->Start() == locationAt && location->Inner == fake.Vector, label);
            const Table::UnrealFill vector = Table::FillFromUnreal(runtime, fake.SceneInstance + static_cast<std::uint64_t>(locationAt), {}, true);
            CHECK_MSG(vector.Handled && vector.Fields.size() == 3, label);
            if (vector.Fields.size() == 3)
            {
                const auto z = std::find_if(vector.Fields.begin(), vector.Fields.end(), [](const Table::StructField& f) { return f.Name.rfind("Z", 0) == 0; });
                CHECK_MSG(z != vector.Fields.end() && z->Offset == 2 * component && z->Type == (variant.Doubles ? Scan::ValueType::Double : Scan::ValueType::Float), label);
            }
            const auto entries = runtime.EnumEntries(fake.Mode);
            const std::int64_t last = variant.NarrowEnum || variant.SplitEnum ? 6 : 2;
            CHECK_MSG(entries.size() == 3 && entries.back().Name == (variant.NoMaxEnum ? "EMyMode::C" : "EMyMode_MAX") && entries.back().Value == last && entries[1].Name == "EMyMode::B", label);
            CHECK_MSG(runtime.Engine() == fake.Engine, label);
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
                // What the probes got that far, and the first object headers as pointer-sized words with the name.
                const Target::UnrealLayout& l = runtime.Layout();
                std::printf("so far: item %X+%X index %X class %X name %X outer %X super %X props %X fname %X next %X offset %X size %X, %d objects\n", l.ItemSize, l.ItemOffset,
                    l.Index, l.Class, l.Name, l.Outer, l.Super, l.ChildProperties, l.FieldName, l.Next, l.Offset, l.Size, runtime.Count());
                const int width = engine.Process()->PointerSize();
                for (int i = 0; i < 24 && i < runtime.Count(); ++i)
                {
                    const std::uint64_t object = runtime.ObjectAt(i).value_or(0);
                    std::printf("  [%2d] %llX %-28s", i, static_cast<unsigned long long>(object), runtime.NameOf(object).value_or("-").c_str());
                    for (int at = 0; object && at < 0x70; at += width)
                        std::printf(" %0*llX", width * 2, static_cast<unsigned long long>(engine.Process()->ReadPointer(object + static_cast<std::uint64_t>(at)).value_or(0)));
                    std::printf("\n");
                }
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
            std::size_t enums = 0;
            std::size_t read = 0;
            std::string sample;
            std::vector<std::string> unread;
            for (const auto& type : index->List)
            {
                if (type.Kind != Target::UnrealKind::Enum)
                    continue;
                ++enums;
                const auto entries = runtime.EnumEntries(type.Object);
                if (entries.empty())
                {
                    if (unread.size() < 8)
                        unread.push_back(type.Name);
                    continue;
                }
                ++read;
                if (sample.empty() && entries.size() >= 3)
                    sample = type.Name + ": " + std::to_string(entries.size()) + " entries, first " + entries.front().Name + " = " + std::to_string(entries.front().Value) +
                             ", last " + entries.back().Name + " = " + std::to_string(entries.back().Value);
            }
            std::printf("enums: %zu of %zu read%s%s\n", read, enums, sample.empty() ? "" : ", e.g. ", sample.c_str());
            if (!unread.empty())
                std::printf("enums not read: %s\n", Join(unread, ", ").c_str());
            const auto realUnread = std::find_if(unread.begin(), unread.end(), [](const std::string& name) { return name.rfind("Default__", 0) != 0; });
            if (read == 0 || realUnread != unread.end())
            {
                const std::uint64_t probe = read == 0 ? (runtime.Enum("ETickingGroup") ? runtime.Enum("ETickingGroup") : runtime.Enum("ECollisionChannel")) : runtime.Enum(*realUnread);
                std::printf("enum probe %llX:\n", static_cast<unsigned long long>(probe));
                for (std::uint64_t offset = 0; probe && offset < 0x90; offset += 8)
                {
                    const std::uint64_t value = engine.Process()->ReadPointer(probe + offset).value_or(0);
                    const std::string name = runtime.NameAt(probe + offset).value_or("");
                    std::printf("  +%02llX %016llX  %s\n", static_cast<unsigned long long>(offset), static_cast<unsigned long long>(value), name.size() < 60 ? name.c_str() : "");
                }
                for (std::uint64_t offset = 0x30; probe && offset <= 0x60; offset += 8)
                {
                    const std::uint64_t value = engine.Process()->ReadPointer(probe + offset).value_or(0) & ~std::uint64_t(7);
                    if (value < 0x10000 || value > 0x7FFFFFFFFFFFull)
                        continue;
                    std::printf("  [+%02llX] -> %llX:\n", static_cast<unsigned long long>(offset), static_cast<unsigned long long>(value));
                    for (std::uint64_t at = 0; at < 0x60; at += 8)
                    {
                        const std::uint64_t word = engine.Process()->ReadPointer(value + at).value_or(0);
                        const std::string name = runtime.NameAt(value + at).value_or("");
                        std::printf("      +%02llX %016llX  %s\n", static_cast<unsigned long long>(at), static_cast<unsigned long long>(word), name.size() < 60 ? name.c_str() : "");
                    }
                }
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

            bool Start(const std::filesystem::path& path, const wchar_t* argument = L"--unreal")
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
                std::wstring command = L"\"" + path.wstring() + L"\" " + argument;
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

    namespace
    {
        void SyntheticTarget3(const std::filesystem::path& targetPath, bool early, const std::string& label)
        {
            std::error_code missing;
            if (!std::filesystem::exists(targetPath, missing))
            {
                Skip("the test target " + targetPath.string() + " was not built");
                return;
            }
            UnrealTarget target;
            const bool started = target.Start(targetPath, early ? L"--unreal3-early" : L"--unreal3");
            CHECK_MSG(started, "could not start " + WideToUtf8(targetPath.wstring()) + " for " + label);
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
            const std::string stem = std::filesystem::path(process.Name()).stem().string();
            const std::uint64_t table = engine.Evaluate(stem + ".g_unreal3Fake").value_or(0);
            CHECK_MSG(table != 0, label);
            const Fake3 fake = table ? ReadFake3(process, table) : Fake3{};
            CHECK_MSG(fake.Names != 0 && fake.Objects != 0 && fake.ActorInstance != 0 && (fake.Early != 0) == early, label);
            if (fake.Names && fake.Objects && fake.ActorInstance)
                Ue3Checks(engine, fake, Ue3Offsets(process.PointerSize(), early), label);
            engine.CloseProcess();
            DWORD code = 0;
            ::GetExitCodeProcess(target.Info.hProcess, &code);
            CHECK_MSG(code == STILL_ACTIVE, label + ": the target ended with " + Hex(code, 8));
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
        for (const bool reverse : { false, true })
            VariantLayout(engine, { .Doubles = true, .ReverseChain = reverse, .ShadowField = true, .NarrowEnum = true }, reverse ? "UE5 variant, reversed chain" : "UE5 variant");
        VariantLayout(engine, { .ReverseChain = true }, "float vectors, reversed chain");
        VariantLayout(engine, { .Doubles = true, .SplitEnum = true }, "UE 5.7 split enum arrays");
        VariantLayout(engine, { .Doubles = true, .SplitEnum = true, .NoMaxEnum = true }, "UE 5.7 enum without _MAX");
        VariantLayout(engine, { .NoMaxEnum = true }, "enum without _MAX");
        for (const bool early : { false, true })
        {
            FakeUnreal3::Builder builder;
            CHECK(builder.Build(early));
            Ue3Checks(engine, Widen3(builder.Fake), FakeUnreal3::Native(early), early ? "UE3 early" : "UE3 late");
        }
        {
            // No Actor.Location, and an owner pointer ahead of the class/struct/inner slot.
            FakeUnreal3::Builder builder;
            CHECK(builder.Build(false, true));
            const Fake3 fake = Widen3(builder.Fake);
            const FakeUnreal3::Offsets& o = FakeUnreal3::Native(false);
            Target::UnrealRuntime runtime;
            const Status status = runtime.Resolve(engine.Process(), Target::UnrealConfig{}, true, nullptr);
            CHECK_MSG(status.Ok(), "UE3 without Location: " + status.Text);
            if (status)
            {
                CHECK_MSG(runtime.Layout().Extra == o.Extra, Format("UE3 without Location: extra %X, expected %X", runtime.Layout().Extra, o.Extra));
                const auto location = runtime.FindField(fake.Actor, "WorldLocation");
                CHECK_MSG(location && location->Inner == fake.Vector && runtime.TypeText(*location) == "FVector", "UE3 without Location: WorldLocation");
                const auto tags = runtime.FindField(fake.Actor, "Tags");
                CHECK_MSG(tags && runtime.TypeText(*tags) == "TArray<FName>", "UE3 without Location: Tags");
                const auto owner = runtime.FindField(fake.Actor, "Owner");
                CHECK_MSG(owner && runtime.TypeText(*owner) == "AActor*", "UE3 without Location: Owner");
            }
        }
        engine.CloseProcess();
        SyntheticTarget(target.parent_path() / L"TestTarget32.exe", FakeUnreal::kOffsets32, "32-bit target");
        SyntheticTarget(target.parent_path() / L"TestTarget.exe", FakeUnreal::kOffsets64, "64-bit target");
        for (const bool early : { false, true })
        {
            SyntheticTarget3(target.parent_path() / L"TestTarget32.exe", early, early ? "UE3 early, 32-bit target" : "UE3 late, 32-bit target");
            SyntheticTarget3(target.parent_path() / L"TestTarget.exe", early, early ? "UE3 early, 64-bit target" : "UE3 late, 64-bit target");
        }
        return Totals().Failures - before;
    }

    MANIFOLD_SUITE("unreal", "Unreal Engine", 36, [](const SuiteContext& context) { return RunUnrealTests(context.Target); });
    MANIFOLD_MODE("--unreal-check", [](const std::vector<std::string>& arguments) { return RunUnrealLive(arguments); });
}
