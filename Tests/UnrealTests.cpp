#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/BackgroundWork.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Table/UnrealStructures.h"
#include "Target/Process.h"
#include "Target/Unreal.h"

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
        struct FakePool
        {
            std::uint64_t Lock;
            std::uint32_t CurrentBlock;
            std::uint32_t Cursor;
            std::uint64_t Blocks[8192];
        };

        struct FakeArray
        {
            std::uint64_t Objects;
            std::uint64_t PreAllocated;
            std::int32_t Max;
            std::int32_t Num;
            std::int32_t MaxChunks;
            std::int32_t NumChunks;
        };

        FakePool g_pool;
        FakeArray g_objects;
        const std::uint64_t g_vtable[4] = { 0x11, 0x22, 0x33, 0x44 };

        constexpr int kObjectCount = 2100;
        constexpr std::size_t kObjectStride = 0x300;
        constexpr std::size_t kFieldStride = 0xA0;

        class FakeUnreal
        {
        public:
            ~FakeUnreal()
            {
                for (void* block : { m_names, m_objects, m_items, m_fields })
                {
                    if (block)
                        ::VirtualFree(block, 0, MEM_RELEASE);
                }
                std::memset(&g_pool, 0, sizeof(g_pool));
                std::memset(&g_objects, 0, sizeof(g_objects));
            }

            bool Build()
            {
                m_names = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                m_objects = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, kObjectCount * kObjectStride, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                m_items = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x10000 * 0x18 + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                m_fields = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (!m_names || !m_objects || !m_items || !m_fields)
                    return false;
                Name("None");
                Name("ByteProperty");
                Name("IntProperty");

                const auto object = [&](int index, const char* name, std::uint32_t number = 0) { return Object(index, Name(name), number); };
                Package = object(0, "/Script/CoreUObject");
                ObjectClass = object(1, "Object");
                FieldClass = object(2, "Field");
                StructClass = object(3, "Struct");
                ClassClass = object(4, "Class");
                PackageClass = object(5, "Package");
                ScriptStructClass = object(6, "ScriptStruct");
                EnumClass = object(7, "Enum");
                Vector = object(8, "Vector");
                Vector2D = object(9, "Vector2D");
                LinearColor = object(10, "LinearColor");
                IntPoint = object(11, "IntPoint");
                Actor = object(12, "Actor");
                SceneComponent = object(13, "SceneComponent");
                GameEngineClass = object(14, "GameEngine");
                Mode = object(15, "EMyMode");
                ActorInstance = object(16, "MyActor");
                SceneInstance = object(17, "Root");
                Engine = object(18, "GameEngine", 2);
                const std::uint32_t filler = Name("Filler");
                for (int i = 19; i < kObjectCount; ++i)
                    Object(i, filler, static_cast<std::uint32_t>(i));

                for (int i = 0; i < kObjectCount; ++i)
                {
                    const std::uint64_t at = Address(i);
                    std::uint64_t meta = ObjectClass;
                    if (at == Package)
                        meta = PackageClass;
                    else if (at >= ObjectClass && at <= EnumClass)
                        meta = ClassClass;
                    else if (at >= Vector && at <= IntPoint)
                        meta = ScriptStructClass;
                    else if (at == Actor || at == SceneComponent || at == GameEngineClass)
                        meta = ClassClass;
                    else if (at == Mode)
                        meta = EnumClass;
                    else if (at == ActorInstance)
                        meta = Actor;
                    else if (at == SceneInstance)
                        meta = SceneComponent;
                    else if (at == Engine)
                        meta = GameEngineClass;
                    Put<std::uint64_t>(at, 0x10, meta);
                    Put<std::uint64_t>(at, 0x20, at == Package ? 0 : Package);
                }
                Super(FieldClass, ObjectClass);
                Super(StructClass, FieldClass);
                Super(ClassClass, StructClass);
                Super(Actor, ObjectClass);
                Super(SceneComponent, ObjectClass);
                Super(GameEngineClass, ObjectClass);
                Size(Vector, 12);
                Size(Vector2D, 8);
                Size(LinearColor, 16);
                Size(IntPoint, 8);
                Size(Actor, 0x220);
                Size(SceneComponent, 0x200);
                Size(GameEngineClass, 0x100);
                Size(ObjectClass, 0x28);

                const std::uint64_t floatClass = FieldClassNamed("FloatProperty");
                const std::uint64_t objectClass = FieldClassNamed("ObjectProperty");
                const std::uint64_t boolClass = FieldClassNamed("BoolProperty");
                const std::uint64_t arrayClass = FieldClassNamed("ArrayProperty");
                const std::uint64_t nameClass = FieldClassNamed("NameProperty");
                const std::uint64_t structClass = FieldClassNamed("StructProperty");

                const std::uint64_t z = Field(floatClass, Vector, "Z", 8, 4, 0);
                const std::uint64_t y = Field(floatClass, Vector, "Y", 4, 4, z);
                const std::uint64_t x = Field(floatClass, Vector, "X", 0, 4, y);
                Put<std::uint64_t>(Vector, 0x50, x);

                const std::uint64_t location = Field(structClass, SceneComponent, "RelativeLocation", 0x11C, 12, 0);
                Put<std::uint64_t>(location, 0x78, Vector);
                Put<std::uint64_t>(SceneComponent, 0x50, location);

                const std::uint64_t inner = Field(nameClass, Actor, "Tags", 0, 8, 0);
                const std::uint64_t tags = Field(arrayClass, Actor, "Tags", 0x208, 0x10, 0);
                Put<std::uint64_t>(tags, 0x78, inner);
                const std::uint64_t hidden = Field(boolClass, Actor, "bHidden", 0x58, 1, tags);
                const std::uint8_t bits[4] = { 1, 0, 0x02, 0x02 };
                std::memcpy(reinterpret_cast<void*>(hidden + 0x78), bits, 4);
                const std::uint64_t root = Field(objectClass, Actor, "RootComponent", 0x130, 8, hidden);
                Put<std::uint64_t>(root, 0x78, SceneComponent);
                const std::uint64_t health = Field(floatClass, Actor, "Health", 0x200, 4, root);
                Put<std::uint64_t>(Actor, 0x50, health);

                std::uint8_t* entries = m_fields + 0xF000;
                const char* names[] = { "EMyMode::A", "EMyMode::B", "EMyMode_MAX" };
                for (int i = 0; i < 3; ++i)
                {
                    Put<std::uint32_t>(reinterpret_cast<std::uint64_t>(entries), static_cast<std::size_t>(i) * 16, Name(names[i]));
                    Put<std::int64_t>(reinterpret_cast<std::uint64_t>(entries), static_cast<std::size_t>(i) * 16 + 8, i);
                }
                Put<std::uint64_t>(Mode, 0x40, reinterpret_cast<std::uint64_t>(entries));
                Put<std::int32_t>(Mode, 0x48, 3);
                Put<std::int32_t>(Mode, 0x4C, 3);

                Put<std::uint64_t>(ActorInstance, 0x130, SceneInstance);
                Put<float>(ActorInstance, 0x200, 75.0f);
                Put<std::uint8_t>(ActorInstance, 0x58, 0x02);
                std::uint8_t* tagData = m_fields + 0xF100;
                Put<std::uint32_t>(reinterpret_cast<std::uint64_t>(tagData), 0, Name("Hero"));
                Put<std::uint32_t>(reinterpret_cast<std::uint64_t>(tagData), 8, Name("Brave"));
                Put<std::uint64_t>(ActorInstance, 0x208, reinterpret_cast<std::uint64_t>(tagData));
                Put<std::int32_t>(ActorInstance, 0x210, 2);
                Put<std::int32_t>(ActorInstance, 0x214, 2);
                Put<float>(SceneInstance, 0x11C, 1.0f);
                Put<float>(SceneInstance, 0x120, 2.0f);
                Put<float>(SceneInstance, 0x124, 3.0f);

                const std::uint64_t chunkTable = reinterpret_cast<std::uint64_t>(m_items) + 0x10000 * 0x18;
                Put<std::uint64_t>(chunkTable, 0, reinterpret_cast<std::uint64_t>(m_items));
                for (int i = 0; i < kObjectCount; ++i)
                    Put<std::uint64_t>(reinterpret_cast<std::uint64_t>(m_items), static_cast<std::size_t>(i) * 0x18, Address(i));
                g_objects = { chunkTable, 0, 0x10000, kObjectCount, 1, 1 };
                g_pool.Blocks[0] = reinterpret_cast<std::uint64_t>(m_names);
                g_pool.Cursor = static_cast<std::uint32_t>(m_used / 2);
                return true;
            }

            std::uint32_t Name(std::string_view text)
            {
                if (const auto it = m_known.find(std::string(text)); it != m_known.end())
                    return it->second;
                const std::uint32_t index = static_cast<std::uint32_t>(m_used / 2);
                const std::uint16_t header = static_cast<std::uint16_t>(text.size() << 6);
                std::memcpy(m_names + m_used, &header, 2);
                std::memcpy(m_names + m_used + 2, text.data(), text.size());
                m_used += (2 + text.size() + 1) & ~std::size_t(1);
                m_known[std::string(text)] = index;
                return index;
            }

            std::uint64_t Address(int index) const { return reinterpret_cast<std::uint64_t>(m_objects) + static_cast<std::uint64_t>(index) * kObjectStride; }

            std::uint64_t Package = 0, ObjectClass = 0, FieldClass = 0, StructClass = 0, ClassClass = 0, PackageClass = 0, ScriptStructClass = 0, EnumClass = 0;
            std::uint64_t Vector = 0, Vector2D = 0, LinearColor = 0, IntPoint = 0, Actor = 0, SceneComponent = 0, GameEngineClass = 0, Mode = 0;
            std::uint64_t ActorInstance = 0, SceneInstance = 0, Engine = 0;

        private:
            template <class T>
            static void Put(std::uint64_t base, std::size_t offset, T value)
            {
                std::memcpy(reinterpret_cast<void*>(base + offset), &value, sizeof(T));
            }

            std::uint64_t Object(int index, std::uint32_t name, std::uint32_t number)
            {
                const std::uint64_t at = Address(index);
                Put<std::uint64_t>(at, 0, reinterpret_cast<std::uint64_t>(&g_vtable[0]));
                Put<std::int32_t>(at, 0x0C, index);
                Put<std::uint32_t>(at, 0x18, name);
                Put<std::uint32_t>(at, 0x1C, number);
                return at;
            }

            void Super(std::uint64_t type, std::uint64_t parent) { Put<std::uint64_t>(type, 0x40, parent); }
            void Size(std::uint64_t type, std::int32_t size) { Put<std::int32_t>(type, 0x58, size); }

            std::uint64_t FieldClassNamed(const char* name)
            {
                const std::uint64_t at = reinterpret_cast<std::uint64_t>(m_fields) + m_fieldUsed;
                m_fieldUsed += kFieldStride;
                Put<std::uint32_t>(at, 0, Name(name));
                return at;
            }

            std::uint64_t Field(std::uint64_t fieldClass, std::uint64_t owner, const char* name, std::int32_t offset, std::int32_t size, std::uint64_t next)
            {
                const std::uint64_t at = reinterpret_cast<std::uint64_t>(m_fields) + m_fieldUsed;
                m_fieldUsed += kFieldStride;
                Put<std::uint64_t>(at, 0x08, fieldClass);
                Put<std::uint64_t>(at, 0x10, owner);
                Put<std::uint64_t>(at, 0x20, next);
                Put<std::uint32_t>(at, 0x28, Name(name));
                Put<std::int32_t>(at, 0x38, 1);
                Put<std::int32_t>(at, 0x3C, size);
                Put<std::int32_t>(at, 0x4C, offset);
                return at;
            }

            std::uint8_t* m_names = nullptr;
            std::uint8_t* m_objects = nullptr;
            std::uint8_t* m_items = nullptr;
            std::uint8_t* m_fields = nullptr;
            std::size_t m_used = 0;
            std::size_t m_fieldUsed = 0;
            std::map<std::string, std::uint32_t> m_known;
        };

        void SyntheticLayout(Engine& engine, const FakeUnreal& fake, const Target::UnrealConfig& config, const char* label)
        {
            Target::UnrealRuntime runtime;
            const Status status = runtime.Resolve(engine.Process(), config, true, nullptr);
            CHECK_MSG(status.Ok(), std::string(label) + ": " + status.Text);
            if (!status)
                return;
            const Target::UnrealSummary& summary = runtime.Summary();
            CHECK(summary.NamePool == reinterpret_cast<std::uint64_t>(&g_pool));
            CHECK(summary.Objects == reinterpret_cast<std::uint64_t>(&g_objects));
            CHECK(summary.ObjectCount == kObjectCount);
            CHECK(!summary.LegacyNames);
            const Target::UnrealLayout& l = runtime.Layout();
            CHECK_MSG(l.Index == 0x0C && l.Class == 0x10 && l.Name == 0x18 && l.Outer == 0x20, label);
            CHECK_MSG(l.Super == 0x40 && l.ChildProperties == 0x50 && l.StructSize == 0x58, label);
            CHECK_MSG(l.FField && l.FieldClass == 0x08 && l.FieldName == 0x28 && l.Next == 0x20 && l.Offset == 0x4C && l.Size == 0x3C && l.Extra == 0x78 && l.Component == 4, label);
            CHECK(l.ItemSize == 0x18 && l.ItemOffset == 0);

            CHECK(runtime.Type("actor") == fake.Actor);
            CHECK(runtime.Enum("EMyMode") == fake.Mode);
            CHECK(runtime.CppName(fake.Actor) == "AActor");
            CHECK(runtime.CppName(fake.SceneComponent) == "USceneComponent");
            CHECK(runtime.CppName(fake.Vector) == "FVector");
            CHECK(runtime.StructSize(fake.Actor) == 0x220);
            CHECK(runtime.FullName(fake.ActorInstance) == "/Script/CoreUObject.MyActor");
            CHECK(runtime.NameOf(fake.Engine) == "GameEngine_1");
            CHECK(runtime.IsObject(fake.ActorInstance));
            CHECK(!runtime.IsObject(fake.ActorInstance + 8));

            const auto fields = runtime.Fields(fake.Actor);
            CHECK(fields.size() == 4);
            if (fields.size() == 4)
            {
                CHECK(fields[0].Name == "bHidden" && fields[0].Bitfield && fields[0].Mask == 0x02 && fields[0].Start() == 0x58);
                CHECK(fields[1].Name == "RootComponent" && runtime.TypeText(fields[1]) == "USceneComponent*");
                CHECK(fields[2].Name == "Health" && runtime.TypeText(fields[2]) == "float");
                CHECK(fields[3].Name == "Tags" && runtime.TypeText(fields[3]) == "TArray<FName>");
            }
            CHECK(runtime.FindField(fake.Actor, "health").has_value());
            CHECK(!runtime.FindField(fake.Actor, "Nope").has_value());
            const auto location = runtime.FindField(fake.SceneComponent, "RelativeLocation");
            CHECK(location && runtime.TypeText(*location) == "FVector" && location->Inner == fake.Vector);

            const auto entries = runtime.EnumEntries(fake.Mode);
            CHECK(entries.size() == 3 && entries.back().Name == "EMyMode_MAX" && entries.back().Value == 2);

            CHECK(runtime.Engine() == fake.Engine);
            const auto owner = runtime.OwnerOf(fake.ActorInstance + 0x204);
            CHECK(owner && owner->first == fake.ActorInstance && owner->second == fake.Actor);
            CHECK(!runtime.OwnerOf(fake.ActorInstance + 0x2F0).has_value());
            const auto whois = runtime.Whois(fake.ActorInstance + 0x130);
            CHECK_MSG(whois && whois->find("RootComponent  USceneComponent*") != std::string::npos, whois.value_or("-"));
            const auto hit = runtime.Locate(fake.SceneInstance + 0x120);
            CHECK(hit && hit->Field && hit->Field->Name == "Y" && hit->Struct == 0);
            const auto start = runtime.Locate(fake.SceneInstance + 0x11C);
            CHECK(start && start->Struct == fake.Vector);

            CHECK(runtime.ResolveSymbol("Actor_RootComponent") == 0x130);
            CHECK(runtime.ResolveSymbol("SceneComponent_RelativeLocation") == 0x11C);
            CHECK(runtime.ResolveSymbol("GObjects") == reinterpret_cast<std::uint64_t>(&g_objects));
            CHECK(!runtime.ResolveSymbol("Actor_Nope").has_value());

            const auto walk = runtime.Walk(fake.ActorInstance, "A", { "RootComponent", "RelativeLocation" });
            CHECK(walk.Ok() && walk->Address == fake.SceneInstance + 0x11C && walk->Expression == "[A+Actor_RootComponent]+SceneComponent_RelativeLocation");
            CHECK(!runtime.Walk(fake.ActorInstance, "A", { "Nope" }).Ok());

            const auto firstMember = runtime.ResolveSymbol("Vector_X");
            CHECK(firstMember.has_value() && *firstMember == 0);
            const auto firstMemberAgain = runtime.ResolveSymbol("Vector_X");
            CHECK(firstMemberAgain.has_value() && *firstMemberAgain == 0);
            CHECK(!runtime.ResolveSymbol("Vector_W").has_value() && !runtime.ResolveSymbol("Vector_W").has_value());

            std::uint64_t tagData = 0;
            std::memcpy(&tagData, reinterpret_cast<const void*>(fake.ActorInstance + 0x208), sizeof(tagData));
            const auto element = runtime.Walk(fake.ActorInstance, "A", { "Tags[1]" });
            CHECK(element.Ok() && element->Address == tagData + 8 && element->Expression == "[A+Actor_Tags]+8");
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
            CHECK(named("Health") != fill.Fields.end() && named("Health")->Offset == 0x200 && named("Health")->Type == Scan::ValueType::Float);
            CHECK(named("NamePrivate") != fill.Fields.end() && named("NamePrivate")->CustomType == Target::kUnrealFNameType);
            CHECK(named("bHidden") != fill.Fields.end() && named("bHidden")->Type == Scan::ValueType::Binary && named("bHidden")->BitStart == 1);
            CHECK(named("RootComponent") != fill.Fields.end() && named("RootComponent")->Type == Scan::ValueType::Pointer);
            CHECK(named("(not reflected)") != fill.Fields.end());
            CHECK(fill.Arrays.size() == 1);
            const Table::UnrealFill vector = Table::FillFromUnreal(runtime, fake.SceneInstance + 0x11C, {}, true);
            CHECK(vector.Handled && vector.Fields.size() == 3 && vector.Fields[2].Name == "Z" && vector.Fields[2].Offset == 8);
            if (!fill.Arrays.empty())
            {
                const Table::UnrealArrayRef array = fill.Arrays.front();
                const auto lookup = [&](std::uint64_t data) -> std::optional<std::pair<Target::UnrealField, std::uint64_t>>
                    {
                        if (data != array.Data)
                            return std::nullopt;
                        return std::make_pair(array.Field, array.Header);
                    };
                const Table::UnrealFill items = Table::FillFromUnreal(runtime, array.Data, lookup, true);
                CHECK(items.Handled && items.Fields.size() == 2 && items.Fields[1].Name == "[1]" && items.Fields[1].CustomType == Target::kUnrealFNameType);
                const auto name = Table::UnrealStructureName(runtime, array.Data, false, lookup);
                CHECK(name && name->first == "TArray<FName>[2]");
            }
            const auto name = Table::UnrealStructureName(runtime, fake.ActorInstance + 0x10, true, {});
            CHECK(name && name->first == "Actor" && name->second == fake.ActorInstance);
        }

        void SessionAndLua(Engine& engine, const FakeUnreal& fake)
        {
            UnrealSession& session = engine.Unreal();
            session.Config().NamePool = reinterpret_cast<std::uint64_t>(&g_pool);
            session.Config().Objects = reinterpret_cast<std::uint64_t>(&g_objects);
            const Status status = session.ResolveNow();
            CHECK_MSG(status.Ok(), status.Text);
            CHECK(session.Ready());
            const Lua::CustomType* fname = engine.Lua().FindCustomType(Target::kUnrealFNameType);
            CHECK(fname != nullptr);
            if (fname)
            {
                std::uint8_t bytes[8] = {};
                std::memcpy(bytes, reinterpret_cast<const void*>(fake.ActorInstance + 0x18), 8);
                std::string text;
                CHECK(engine.Lua().CustomToText(*fname, bytes, 0, text) && text == "MyActor");
                std::vector<std::uint8_t> back;
                CHECK(engine.Lua().CustomFromText(*fname, "MyActor", 0, back) && back.size() == 8 && std::memcmp(back.data(), bytes, 8) == 0);
            }
            const auto symbol = engine.Evaluate("[" + Hex(fake.ActorInstance) + "+Actor_RootComponent]+SceneComponent_RelativeLocation");
            CHECK(symbol == fake.SceneInstance + 0x11C);
            std::string result;
            const std::string code = Format(R"LUA(
                local unreal = manifold.unreal
                local actor = 0x%llX
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
                unreal.configure({ symbols = true, layout = { extra = 0x78 } })
                if unreal.config().layout.extra ~= 0x78 then return "config layout" end
                unreal.configure({ layout = false })
                if next(unreal.config().layout) ~= nil then return "layout not cleared" end
                return "ok")LUA",
                static_cast<unsigned long long>(fake.ActorInstance));
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
            for (const auto& type : runtime.Types())
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
    int RunUnrealTests()
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
            FakeUnreal fake;
            CHECK(fake.Build());
            SyntheticLayout(engine, fake, Target::UnrealConfig{}, "search");
            Target::UnrealConfig configured;
            configured.NamePool = reinterpret_cast<std::uint64_t>(&g_pool);
            configured.Objects = reinterpret_cast<std::uint64_t>(&g_objects);
            configured.Layout["extra"] = 0x78;
            SyntheticLayout(engine, fake, configured, "configured");
            Target::UnrealConfig wrong;
            wrong.NamePool = reinterpret_cast<std::uint64_t>(&g_objects);
            Target::UnrealRuntime runtime;
            const Status refused = runtime.Resolve(engine.Process(), wrong, true, nullptr);
            CHECK(!refused.Ok() && refused.Code == ErrorCode::UnrealNotResolved && refused.Text.find("configured name table") != std::string::npos);
            Target::UnrealRuntime gated;
            const Status rejected = gated.Resolve(engine.Process(), Target::UnrealConfig{}, false, nullptr);
            CHECK(!rejected.Ok() && gated.Rejected());
            SessionAndLua(engine, fake);
        }
        engine.CloseProcess();
        return Totals().Failures - before;
    }

    MANIFOLD_SUITE("unreal", "Unreal Engine", 36, [](const SuiteContext&) { return RunUnrealTests(); });
    MANIFOLD_MODE("--unreal-check", [](const std::vector<std::string>& arguments) { return RunUnrealLive(arguments); });
}
