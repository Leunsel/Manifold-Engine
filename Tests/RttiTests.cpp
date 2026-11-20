#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Table/RttiStructures.h"
#include "Target/Process.h"
#include "Target/Rtti.h"
#include "Target/RttiMsvc.h"

#include "Target/FakeRtti.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        using Fake = FakeRtti::ImageOf<std::uint64_t>;
        using Target::RttiRuntime;
        constexpr std::size_t kFakeSlots = sizeof(Fake) / sizeof(std::uint64_t);

        Fake Widen(const FakeRtti::Image& image)
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

        bool Has(const std::vector<std::uint64_t>& list, std::uint64_t value)
        {
            return std::find(list.begin(), list.end(), value) != list.end();
        }

        const Table::StructField* FieldAt(const std::vector<Table::StructField>& fields, std::uint64_t offset)
        {
            for (const Table::StructField& field : fields)
            {
                if (field.Offset == static_cast<int>(offset))
                    return &field;
            }
            return nullptr;
        }

        std::string NameAt(const std::vector<Table::StructField>& fields, std::uint64_t offset)
        {
            const Table::StructField* field = FieldAt(fields, offset);
            return field ? field->Name : std::string("(no field)");
        }

        void Names()
        {
            using Target::MsvcDemangle;
            using Target::MsvcMangle;
            CHECK_EQ(MsvcDemangle(".?AVPlayer@Game@@"), "Game::Player");
            CHECK_EQ(MsvcDemangle(".?AUVec3@@"), "Vec3");
            CHECK_EQ(MsvcDemangle(".?ATValue@Json@@"), "Json::Value");
            CHECK_EQ(MsvcDemangle(".?AW4Color@Gfx@@"), "Gfx::Color");
            CHECK_EQ(MsvcDemangle(".?AV?$vector@H@std@@"), "std::vector<int>");
            CHECK_EQ(MsvcDemangle(".?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"),
                "std::basic_string<char, std::char_traits<char>, std::allocator<char>>");
            CHECK_EQ(MsvcDemangle(".?AVHidden@?A0x12345678@@"), "`anonymous namespace'::Hidden");
            CHECK_EQ(MsvcDemangle(".?AV?$Holder@PEAVWidget@Ui@@@@"), "Holder<Ui::Widget*>");
            CHECK_EQ(MsvcDemangle(".?AV?$Ref@AEBUData@@@@"), "Ref<const Data&>");
            CHECK_EQ(MsvcDemangle(".?AV?$Array@H$0BA@@@"), "Array<int, 16>");
            CHECK_EQ(MsvcDemangle(".?AV?$N@$03@@"), "N<4>");
            CHECK_EQ(MsvcDemangle(".?AV?$N@$0?3@@"), "N<-4>");
            CHECK_EQ(MsvcDemangle(".?AV?$Fn@P6AHH@Z@@"), "Fn<int (*)(int)>");
            CHECK_EQ(MsvcDemangle(".?AV?$M@$$QEAH@@"), "M<int&&>");
            CHECK_EQ(MsvcDemangle(".?AV?$M@_N_K_W@@"), "M<bool, unsigned __int64, wchar_t>");
            CHECK_EQ(MsvcDemangle(".?AV?$M@$$T@@"), "M<std::nullptr_t>");
            CHECK_EQ(MsvcDemangle(".?AV?$Pack@$$V@@"), "Pack<>");
            CHECK_EQ(MsvcDemangle(".?AV<lambda_1>@?1??main@@YAHXZ@"), "main()::<lambda_1>");
            CHECK_EQ(MsvcDemangle(".?AVA@B@1@"), "B::B::A");
            CHECK_EQ(MsvcDemangle(".?AV"), ".?AV");
            CHECK_EQ(MsvcDemangle(".?AVBroken"), ".?AVBroken");
            CHECK_EQ(MsvcDemangle(".?AXfoo@@"), ".?AXfoo@@");
            CHECK_EQ(MsvcDemangle(".?AV?$M@P8C@@AEXXZ@@"), ".?AV?$M@P8C@@AEXXZ@@");
            CHECK_EQ(MsvcDemangle(".?AVA@9@"), ".?AVA@9@");
            CHECK_EQ(MsvcDemangle(".?AVTail@@x"), ".?AVTail@@x");
            CHECK_EQ(MsvcDemangle("plain"), "plain");
            CHECK_EQ(MsvcDemangle(""), "");
            std::string deep = ".?AV";
            for (int i = 0; i < 1500; ++i)
                deep += "?$T@PEAV";
            CHECK_EQ(MsvcDemangle(deep), deep);
            CHECK(Target::IsMsvcTypeName(".?AVPlayer@Game@@") && !Target::IsMsvcTypeName("?AVPlayer@@") && !Target::IsMsvcTypeName(".?AV Player@@"));

            const auto both = MsvcMangle("Game::Player");
            CHECK(both.size() == 2 && both[0] == ".?AVPlayer@Game@@" && both[1] == ".?AUPlayer@Game@@");
            const auto repeated = MsvcMangle("A::A");
            CHECK(repeated.size() == 2 && repeated[0] == ".?AVA@0@");
            const auto decorated = MsvcMangle(".?AVX@@");
            CHECK(decorated.size() == 1 && decorated[0] == ".?AVX@@");
            CHECK(MsvcMangle("std::vector<int>").empty() && MsvcMangle("").empty() && MsvcMangle("a:::b").empty() && MsvcMangle("::").empty());
            for (const char* name : { "Game::Player", "A::A", "One::Two::Three::Four", "Solo" })
                CHECK_EQ(MsvcDemangle(MsvcMangle(name).front()), name);
        }

        void Synthetic(Engine& engine, const Fake& f, const char* label)
        {
            const std::shared_ptr<Target::Process> process = engine.Process();
            const auto pointer = static_cast<std::uint64_t>(process->PointerSize());
            RttiRuntime runtime(process);

            const auto base = runtime.ObjectAt(f.Base);
            CHECK_MSG(base && base->Class->Name == "Synth::Base" && base->Start == f.Base && base->Class->Decorated == ".?AVBase@Synth@@" && base->Via == 0, label);
            CHECK_MSG(base && base->VTable == f.TableBase && base->Class->Abi == Target::RttiAbi::Msvc, label);
            const auto derived = runtime.ObjectAt(f.Derived);
            CHECK_MSG(derived && derived->Class->Name == "Synth::Derived", label);
            if (derived)
            {
                const auto bases = runtime.Bases(*derived->Class);
                CHECK_MSG(bases->size() == 2 && (*bases)[0].Name == "Synth::Derived" && (*bases)[0].Depth == 0 && (*bases)[1].Name == "Synth::Base" && (*bases)[1].Depth == 1 &&
                    !(*bases)[1].Virtual, label);
            }
            const auto second = runtime.ObjectAt(f.MultiSecond);
            CHECK_MSG(second && second->Class->Name == "Synth::Multi" && second->Start == f.Multi && second->Offset == 2 * pointer, label);
            if (second)
            {
                CHECK_EQ(runtime.SymbolFor(*second->Class), "Synth::Multi::vftable@Synth::Second");
                const auto bases = runtime.Bases(*second->Class);
                CHECK_MSG(bases->size() == 3 && (*bases)[2].Name == "Synth::Second" && (*bases)[2].Decorated == ".?AUSecond@Synth@@" && (*bases)[2].Depth == 1 &&
                    (*bases)[2].Offset == static_cast<std::int32_t>(2 * pointer) && (*bases)[1].Depth == 1, label);
                CHECK(runtime.BaseAt(*second->Class, static_cast<std::int64_t>(2 * pointer)) == std::string("Synth::Second"));
                CHECK(!runtime.BaseAt(*second->Class, 4).has_value());
            }
            const auto primary = runtime.ObjectAt(f.Multi);
            CHECK_MSG(primary && runtime.SymbolFor(*primary->Class) == "Synth::Multi::vftable", label);

            const auto virt = runtime.ObjectAt(f.Virt);
            CHECK_MSG(virt && virt->Class->Name == "Synth::Virt" && virt->Start == f.Virt && virt->Via == f.VirtPart && virt->Class->VirtualInheritance(), label);
            const auto part = runtime.ObjectAt(f.VirtPart);
            CHECK_MSG(part && part->Start == f.Virt && part->Via == 0, label);
            if (part)
            {
                const auto bases = runtime.Bases(*part->Class);
                CHECK(bases->size() == 2 && (*bases)[1].Virtual && (*bases)[1].Name == "Synth::VBase");
                if (bases->size() == 2)
                {
                    CHECK(runtime.BaseOffset((*bases)[1], f.Virt) == static_cast<std::int64_t>(2 * pointer));
                    CHECK(!runtime.BaseOffset((*bases)[1], std::nullopt).has_value());
                }
                CHECK_EQ(runtime.SymbolFor(*part->Class), "Synth::Virt::vftable@" + Hex(2 * pointer));
                CHECK(runtime.VirtualBaseTable(process->ReadPointer(f.Virt).value_or(0)) == static_cast<std::int32_t>(2 * pointer));
            }

            for (const auto& [address, what] : { std::pair<std::uint64_t, const char*>{ f.BadSignature, "signature" }, { f.BadSelf, "self reference" }, { f.BadCount, "no bases" },
                     { f.BadCountHuge, "too many bases" }, { f.BadName, "name" } })
            {
                CHECK_MSG(!runtime.ObjectAt(address).has_value(), std::string(label) + ": a locator with a bad " + what + " was taken");
                const std::uint64_t table = process->ReadPointer(address).value_or(0);
                CHECK_MSG(table != 0 && !runtime.TableClass(table) && !runtime.Locator(process->ReadPointer(table - pointer).value_or(0)), what);
            }
            CHECK(!runtime.ObjectAt(f.Plain).has_value() && !runtime.ObjectAt(f.Base + 1).has_value() && !runtime.ObjectAt(0).has_value());
            CHECK(runtime.TableClass(f.TableBase) && runtime.TableClass(f.TableBase)->Name == "Synth::Base");

            const auto plain = runtime.Owner(f.Plain);
            CHECK_MSG(plain && plain->Start == f.Base && plain->Offset == f.Plain - f.Base && plain->Class->Name == "Synth::Base", label);
            CHECK(!runtime.Owner(f.Plain, 16).has_value());
            const auto inMember = runtime.Owner(f.Owner + 3 * pointer + 1);
            CHECK_MSG(inMember && inMember->Class->Name == "Synth::Part" && inMember->Start == f.OwnerPart && inMember->Offset == pointer + 1, label);
            const auto inOwner = runtime.Owner(f.Owner + pointer);
            CHECK_MSG(inOwner && inOwner->Class->Name == "Synth::Owner" && inOwner->Offset == pointer, label);
            const auto inMulti = runtime.Locate(f.Multi + 3 * pointer);
            CHECK_MSG(inMulti && inMulti->Class->Name == "Synth::Multi" && inMulti->Start == f.Multi && inMulti->Offset == 3 * pointer, label);
            const auto whois = runtime.Whois(f.MultiSecond + pointer);
            CHECK_MSG(whois && whois->find("is Synth::Multi+" + Hex(3 * pointer)) != std::string::npos && whois->find("Object start: " + Hex(f.Multi)) != std::string::npos &&
                whois->find("Decorated name: .?AVMulti@Synth@@") != std::string::npos && whois->find("    Synth::Second  +" + Hex(2 * pointer)) != std::string::npos &&
                whois->find("Defined in: " + process->Name()) != std::string::npos, whois.value_or("-"));
            const auto whoisVirt = runtime.Whois(f.Virt);
            CHECK_MSG(whoisVirt && whoisVirt->find(" is a Synth::Virt") != std::string::npos && whoisVirt->find("Synth::VBase (virtual)  +" + Hex(2 * pointer)) != std::string::npos,
                whoisVirt.value_or("-"));

            RttiRuntime fresh(process);
            CHECK(fresh.Classes().empty());
            int stops = 0;
            CHECK(!fresh.Instances("Synth::Base", 100, [&stops] { ++stops; return true; }).has_value() && stops > 0);
            const auto multi = fresh.Primary("Synth::Multi");
            CHECK_MSG(multi && multi->first == f.TableMulti, label);
            const auto byBase = fresh.Primary("Synth::Multi", "Synth::Second");
            CHECK_MSG(byBase && byBase->first == f.TableMultiSecond, label);
            const auto byOffset = fresh.Primary("Synth::Multi", Lower(Hex(2 * pointer)));
            CHECK_MSG(byOffset && byOffset->first == f.TableMultiSecond, label);
            CHECK(!fresh.Primary("Synth::Multi", "Synth::Nope").has_value());
            const auto tables = fresh.Tables("Synth::Multi");
            CHECK_MSG(tables.size() == 2 && tables[0].Address == f.TableMulti && tables[0].Offset == 0 && tables[0].Symbol == "Synth::Multi::vftable" &&
                tables[1].Address == f.TableMultiSecond && tables[1].Symbol == "Synth::Multi::vftable@Synth::Second" && tables[1].Module == process->Name(), label);
            CHECK_MSG(fresh.Tables("Synth::Base").size() == 1, std::string(label) + ": the rejected locators count as tables of Synth::Base");
            CHECK(!fresh.Primary("Synth::Second").has_value() && !fresh.Primary("Synth::Nope").has_value() && !fresh.Primary("Synth::Nope").has_value());
            CHECK(!fresh.Primary("").has_value() && !fresh.Primary("Synth::Base<int>").has_value());
            const auto decorated = fresh.Primary(".?AVPart@Synth@@");
            CHECK_MSG(decorated && decorated->first == f.TablePart, label);
            const std::vector<std::string> classes = fresh.Classes();
            CHECK(std::find(classes.begin(), classes.end(), "Synth::Part") != classes.end() && std::find(classes.begin(), classes.end(), ".?AVPart@Synth@@") == classes.end());
            CHECK(std::is_sorted(classes.begin(), classes.end()));
            const auto instances = fresh.Instances("Synth::Base");
            CHECK_MSG(instances && Has(*instances, f.Base) && !Has(*instances, f.Derived), label);
            const auto multis = fresh.Instances("Synth::Multi");
            CHECK_MSG(multis && Has(*multis, f.Multi) && !Has(*multis, f.MultiSecond), label);
            const auto one = fresh.Instances("Synth::Base", 1);
            CHECK(one && one->size() == 1);
            CHECK(!fresh.Instances("Synth::Nope").has_value());
            CHECK(fresh.ResolveSymbol("Synth::Multi::vftable@Synth::Second") == f.TableMultiSecond && fresh.ResolveSymbol("Synth::Base::vftable") == f.TableBase);
            CHECK(!fresh.ResolveSymbol("Synth::Base").has_value() && !fresh.ResolveSymbol("Synth::Nope::vftable").has_value() && !fresh.ResolveSymbol("::vftable").has_value() &&
                !fresh.ResolveSymbol("Synth::Base::vftable@").has_value());
            const Target::RttiCounts counts = fresh.Counts();
            CHECK(counts.Modules > 0 && counts.Classes >= 3 && counts.Tables >= counts.Classes);
            CHECK(fresh.StatusText().find(pointer == 8 ? "64-bit process " : "32-bit process ") == 0);
        }

        void Real(Engine& engine, const Fake& f, const char* label)
        {
            const std::shared_ptr<Target::Process> process = engine.Process();
            RttiRuntime runtime(process);
            const auto dog = runtime.ObjectAt(f.Dog);
            CHECK_MSG(dog && dog->Class->Name == "FakeRtti::Zoo::Dog" && dog->Class->Decorated == ".?AUDog@Zoo@FakeRtti@@" && dog->Start == f.Dog, label);
            const auto walker = runtime.ObjectAt(f.DuckWalker);
            CHECK_MSG(walker && walker->Class->Name == "FakeRtti::Zoo::Duck" && walker->Start == f.Duck && walker->Class->Decorated == ".?AVDuck@Zoo@FakeRtti@@", label);
            if (walker)
            {
                CHECK_EQ(runtime.SymbolFor(*walker->Class), "FakeRtti::Zoo::Duck::vftable@FakeRtti::Zoo::Walker");
                const auto bases = runtime.Bases(*walker->Class);
                CHECK_MSG(bases->size() == 3 && (*bases)[0].Name == "FakeRtti::Zoo::Duck" && (*bases)[1].Name == "FakeRtti::Zoo::Animal" && (*bases)[1].Offset == 0 &&
                    (*bases)[2].Name == "FakeRtti::Zoo::Walker" && (*bases)[2].Offset == static_cast<std::int32_t>(f.DuckWalker - f.Duck) && (*bases)[2].Depth == 1, label);
            }
            const auto swimmer = runtime.ObjectAt(f.Swimmer);
            CHECK_MSG(swimmer && swimmer->Class->Name == "FakeRtti::Zoo::Swimmer" && swimmer->Via == 0 && runtime.HasVirtualBases(*swimmer->Class), label);
            const auto swimmerBase = runtime.ObjectAt(f.SwimmerAnimal);
            CHECK_MSG(swimmerBase && swimmerBase->Start == f.Swimmer && swimmerBase->Class->Name == "FakeRtti::Zoo::Swimmer", label);
            if (swimmer)
            {
                const auto bases = runtime.Bases(*swimmer->Class);
                CHECK_MSG(bases->size() == 2 && (*bases)[1].Virtual && runtime.BaseOffset((*bases)[1], f.Swimmer) == static_cast<std::int64_t>(f.SwimmerAnimal - f.Swimmer), label);
            }
            const auto fish = runtime.ObjectAt(f.Fish);
            CHECK_MSG(fish && fish->Class->Name == "FakeRtti::Zoo::Fish" && fish->Start == f.Fish && fish->Via == f.FishAnimal, label);
            CHECK_MSG(fish && !fish->Class->VirtualInheritance() && runtime.HasVirtualBases(*fish->Class), label);
            CHECK(!runtime.ObjectAt(f.Fish + process->PointerSize()).has_value());
            const auto cage = runtime.ObjectAt(f.Cage);
            CHECK_MSG(cage && cage->Class->Name == "FakeRtti::Zoo::Cage<int>", cage ? cage->Class->Name : std::string(label));
            const auto after = runtime.Owner(f.KeeperAfter);
            CHECK_MSG(after && after->Class->Name == "FakeRtti::Zoo::Dog" && after->Start == f.KeeperPet, label);

            RttiRuntime fresh(process);
            const auto dogs = fresh.Instances("FakeRtti::Zoo::Dog");
            CHECK_MSG(dogs && Has(*dogs, f.Dog) && Has(*dogs, f.KeeperPet) && !Has(*dogs, f.Duck), label);
            const auto duck = fresh.Primary("FakeRtti::Zoo::Duck", "FakeRtti::Zoo::Walker");
            CHECK_MSG(duck && duck->first == process->ReadPointer(f.DuckWalker).value_or(1), label);
            CHECK(fresh.Tables("FakeRtti::Zoo::Duck").size() == 2 && fresh.Tables("FakeRtti::Zoo::Swimmer").size() == 2);
        }

        void Dissect(Engine& engine, const Fake& f, const char* label)
        {
            const std::shared_ptr<Target::Process> process = engine.Process();
            const auto pointer = static_cast<std::uint64_t>(process->PointerSize());
            RttiRuntime runtime(process);
            const std::set<std::string> none;

            Table::RttiFill owner = Table::FillFromRtti(runtime, f.Owner, 256, none);
            CHECK_MSG(owner.Handled && owner.Object && NameAt(owner.Fields, 0) == "vftable", label);
            const Table::StructField* member = FieldAt(owner.Fields, 2 * pointer);
            CHECK_MSG(member && member->Type == Scan::ValueType::ByteArray && member->ChildStructure == "Synth::Part" && member->Name == "Synth::Part" &&
                member->Size == Table::kRttiMemberLimit, label);
            CHECK(std::none_of(owner.Fields.begin(), owner.Fields.end(), [&](const Table::StructField& field)
                {
                    return field.Offset > static_cast<int>(2 * pointer) && field.Offset < static_cast<int>(2 * pointer) + Table::kRttiMemberLimit;
                }));
            CHECK_MSG(owner.Children.size() == 1 && owner.Children[0].Name == "Synth::Part" && NameAt(owner.Children[0].Fields, 0) == "vftable" &&
                owner.Children[0].Size() == Table::kRttiMemberLimit, label);
            CHECK(owner.Fields.back().Offset + owner.Fields.back().Size == Table::kRttiGuessSize);

            const std::set<std::string> known{ "Synth::Part" };
            const Table::RttiFill reused = Table::FillFromRtti(runtime, f.Owner, 256, known);
            const Table::StructField* again = FieldAt(reused.Fields, 2 * pointer);
            CHECK_MSG(reused.Handled && reused.Children.empty() && again && again->ChildStructure == "Synth::Part", label);

            const Table::RttiFill derived = Table::FillFromRtti(runtime, f.Derived, 256, none);
            CHECK_MSG(derived.Handled && NameAt(derived.Fields, 0) == "vftable" && NameAt(derived.Fields, pointer) == "Synth::Base*" &&
                NameAt(derived.Fields, 2 * pointer) == "Synth::Second*" && NameAt(derived.Fields, 3 * pointer) == "Synth::Multi*" && derived.Children.empty(),
                std::string(label) + ": " + NameAt(derived.Fields, pointer) + ", " + NameAt(derived.Fields, 2 * pointer) + ", " + NameAt(derived.Fields, 3 * pointer));
            const Table::RttiFill multi = Table::FillFromRtti(runtime, f.Multi, 256, none);
            CHECK_MSG(multi.Handled && NameAt(multi.Fields, 0) == "vftable" && NameAt(multi.Fields, 2 * pointer) == "vftable (Synth::Second)" && multi.Children.empty(),
                NameAt(multi.Fields, 2 * pointer));
            const Table::RttiFill virt = Table::FillFromRtti(runtime, f.Virt, 256, none);
            CHECK_MSG(virt.Handled && NameAt(virt.Fields, 0) == "vbtable" && NameAt(virt.Fields, 2 * pointer) == "vftable (Synth::VBase)",
                NameAt(virt.Fields, 0) + ", " + NameAt(virt.Fields, 2 * pointer));
            const Table::RttiFill table = Table::FillFromRtti(runtime, f.TableBase, 256, none);
            CHECK_MSG(table.Handled && !table.Object && table.Labelled == 4 && NameAt(table.Fields, 0) == "virtual 0" && NameAt(table.Fields, 3 * pointer) == "virtual 3" &&
                NameAt(table.Fields, 4 * pointer).rfind("Pointer", 0) == 0, NameAt(table.Fields, 4 * pointer));
            CHECK(!Table::FillFromRtti(runtime, f.MultiSecond, 256, none).Handled);
            CHECK(!Table::FillFromRtti(runtime, f.Plain, 256, none).Handled);
            CHECK(!Table::FillFromRtti(runtime, f.BadSignature, 256, none).Handled);

            const Table::RttiFill swimmer = Table::FillFromRtti(runtime, f.Swimmer, 256, none);
            const auto swimmerObject = runtime.ObjectAt(f.Swimmer);
            const std::uint64_t tablePointer = swimmerObject ? static_cast<std::uint64_t>((*runtime.Bases(*swimmerObject->Class))[1].TablePointerAt) : 0;
            CHECK_MSG(swimmer.Handled && tablePointer != 0 && NameAt(swimmer.Fields, 0) == "vftable" && NameAt(swimmer.Fields, tablePointer) == "vbtable" &&
                NameAt(swimmer.Fields, f.SwimmerAnimal - f.Swimmer) == "vftable (FakeRtti::Zoo::Animal)",
                NameAt(swimmer.Fields, tablePointer) + ", " + NameAt(swimmer.Fields, f.SwimmerAnimal - f.Swimmer));
            const Table::RttiFill keeper = Table::FillFromRtti(runtime, f.Keeper, 256, none);
            const Table::StructField* pet = FieldAt(keeper.Fields, f.KeeperPet - f.Keeper);
            CHECK_MSG(keeper.Handled && pet && pet->Type == Scan::ValueType::ByteArray && pet->ChildStructure == "FakeRtti::Zoo::Dog" && pet->Name == "FakeRtti::Zoo::Dog" &&
                keeper.Children.size() == 1 && NameAt(keeper.Children[0].Fields, 0) == "vftable", label);

            const auto name = Table::RttiStructureName(runtime, f.MultiSecond);
            CHECK_MSG(name && name->Name == "Synth::Multi" && name->Start == f.Multi && name->Object, label);
            const auto tableName = Table::RttiStructureName(runtime, f.TableMultiSecond);
            CHECK_MSG(tableName && tableName->Name == "Synth::Multi::vftable@Synth::Second" && tableName->Start == f.TableMultiSecond && !tableName->Object, label);
            CHECK(!Table::RttiStructureName(runtime, f.Plain).has_value());

            Table::StructureStore store;
            Table::StructDefinition& target = store.Add("Target");
            store.Add("Synth::Part").Fields.push_back(Table::StructField{ 0, "kept" });
            Table::ApplyRttiFill(store, target, Table::FillFromRtti(runtime, f.Owner, 256, none));
            CHECK(store.Items.size() == 2 && store.Find("Synth::Part")->Fields.size() == 1 && store.Find("Synth::Part")->Fields[0].Name == "kept" && !target.Fields.empty());
            Table::ApplyRttiFill(store, target, Table::FillFromRtti(runtime, f.Keeper, 256, none));
            CHECK(store.Items.size() == 3 && store.Find("FakeRtti::Zoo::Dog") != nullptr);
        }

        void SessionAndLua(Engine& engine, Settings& settings, const Fake& f, const char* label)
        {
            const std::shared_ptr<Target::Process> process = engine.Process();
            const auto pointer = static_cast<std::uint64_t>(process->PointerSize());
            RttiSession& session = engine.Rtti();
            CHECK(session.Dissects() && session.Runtime() != nullptr && session.Runtime() == session.Runtime());
            session.Reset();
            CHECK(!engine.Symbols().FindRegistered("Synth::Multi::vftable").has_value());
            CHECK_MSG(engine.Evaluate("Synth::Multi::vftable@Synth::Second") == f.TableMultiSecond, label);
            CHECK_MSG(engine.Symbols().FindRegistered("Synth::Multi::vftable") == f.TableMulti, label);
            CHECK(engine.Symbols().LabelOnly(f.TableMulti));
            CHECK(engine.Evaluate("Synth::Multi::vftable+8") == f.TableMulti + 8);
            CHECK(!engine.Evaluate("Synth::Nope::vftable").has_value());
            CHECK(session.Names(f.Dog) && session.Names(f.TableBase) && !session.Names(f.Plain));
            session.Reset();
            CHECK(!engine.Symbols().FindRegistered("Synth::Multi::vftable").has_value() && session.Runtime()->Classes().empty());

            const auto empty = process->Allocate(0x10000, PAGE_READWRITE);
            CHECK(empty.Ok());
            const std::uint64_t nothing = empty.Ok() ? *empty + 0x8000 : 0;

            std::string result;
            const std::string locals = "local dog, duck, walker, fish, keeper, pet, keeperSize, multi, second, virt, owner, plain, nothing, tableSecond, badName, P = 0x" + Hex(f.Dog) +
                ", 0x" + Hex(f.Duck) + ", 0x" + Hex(f.DuckWalker) + ", 0x" + Hex(f.Fish) + ", 0x" + Hex(f.Keeper) + ", 0x" + Hex(f.KeeperPet) + ", " + std::to_string(f.KeeperSize) +
                ", 0x" + Hex(f.Multi) + ", 0x" + Hex(f.MultiSecond) + ", 0x" + Hex(f.Virt) + ", 0x" + Hex(f.Owner) + ", 0x" + Hex(f.Plain) + ", 0x" + Hex(nothing) + ", 0x" +
                Hex(f.TableMultiSecond) + ", 0x" + Hex(f.BadName) + ", " + std::to_string(pointer) + "\n";
            const auto lua = [&](const char* name, const std::string& code)
                {
                    const Status status = engine.Lua().RunWithResult(locals + code, name, result);
                    CHECK_MSG(status.Ok() && result == "ok", std::string(label) + " " + name + ": " + (status.Ok() ? result : status.Text));
                };

            lua("rtti lookups", R"LUA(
                local r = manifold.rtti
                if r.className(dog) ~= "FakeRtti::Zoo::Dog" then return "className " .. tostring(r.className(dog)) end
                if r.className(string.format("%X", dog)) ~= "FakeRtti::Zoo::Dog" then return "className of an expression" end
                local missing, problem = r.className(plain)
                if missing ~= nil or problem ~= nil then return "className of plain memory" end
                if r.className(badName) ~= nil then return "a bad locator has a class" end
                local o = r.object(walker)
                if not o or o.class ~= "FakeRtti::Zoo::Duck" or o.start ~= duck or o.address ~= walker or o.offset ~= walker - duck or o.viaVirtualBase then return "object" end
                if o.decorated ~= ".?AVDuck@Zoo@FakeRtti@@" or o.vtable ~= manifold.memory.read(walker, "pointer") or o.module ~= manifold.process.info().name then return "object fields" end
                local viaBase = r.object(fish)
                if not viaBase or not viaBase.viaVirtualBase or viaBase.start ~= fish or viaBase.class ~= "FakeRtti::Zoo::Fish" then return "virtual base object" end
                if r.object(plain) ~= nil then return "object of plain memory" end
                local owned = r.owner(pet + P + 1)
                if not owned or owned.class ~= "FakeRtti::Zoo::Dog" or owned.start ~= pet or owned.offset ~= P + 1 then return "owner" end
                if r.owner(plain, 8) ~= nil then return "owner window" end
                local text = r.whois(second + P)
                if not text or not text:find("is Synth::Multi+", 1, true) or not text:find("Synth::Second  +", 1, true) then return "whois " .. tostring(text) end
                local none, why = r.whois(nothing)
                if none ~= nil or not manifold.error.is(why) or why.code ~= "ME-4301" then return "whois of nothing" end
                local bases = r.bases("Synth::Multi")
                if not bases or #bases ~= 3 or bases[1].name ~= "Synth::Multi" or bases[1].depth ~= 0 or bases[3].name ~= "Synth::Second" or bases[3].offset ~= 2 * P or bases[3].virtual then return "bases by name" end
                if bases[3].decorated ~= ".?AUSecond@Synth@@" or bases[3].depth ~= 1 then return "base fields" end
                local virtual = r.bases(virt)
                if not virtual or #virtual ~= 2 or not virtual[2].virtual or virtual[2].offset ~= 2 * P then return "bases of an object" end
                local byName = r.bases("Synth::Virt")
                if not byName or byName[2].offset ~= nil then return "a virtual base has an offset without an object" end
                none, why = r.bases("Synth::Nope")
                if none ~= nil or why.code ~= "ME-4302" then return "bases of an unknown class" end
                none, why = r.bases(nothing)
                if none ~= nil or why.code ~= "ME-4301" then return "bases of nothing" end
                if r.vtable("Synth::Multi", "Synth::Second") ~= tableSecond or r.vtable("Synth::Multi") ~= manifold.memory.read(multi, "pointer") then return "vtable" end
                none, why = r.vtable("Synth::Nope")
                if none ~= nil or why ~= nil then return "vtable of an unknown class" end
                local tables = r.vtables("Synth::Multi")
                if not tables or #tables ~= 2 or tables[2].address ~= tableSecond or tables[2].symbol ~= "Synth::Multi::vftable@Synth::Second" or tables[1].offset ~= 0 or tables[2].offset ~= 2 * P then return "vtables" end
                none, why = r.vtables("Synth::Nope")
                if none ~= nil or why.code ~= "ME-4302" then return "vtables of an unknown class" end
                local found = {}
                for _, address in ipairs(r.instances("FakeRtti::Zoo::Dog")) do found[address] = true end
                if not found[dog] or not found[pet] then return "instances" end
                if #r.instances("FakeRtti::Zoo::Dog", 1) ~= 1 then return "instances limit" end
                none, why = r.instances("Synth::Nope")
                if none ~= nil or why.code ~= "ME-4302" then return "instances of an unknown class" end
                local seen = {}
                for _, name in ipairs(r.classes()) do seen[name] = true end
                if not seen["Synth::Multi"] or not seen["FakeRtti::Zoo::Dog"] or seen["Synth::Nope"] then return "classes" end
                if r.demangle(".?AV?$vector@H@std@@") ~= "std::vector<int>" or r.demangle("x") ~= "x" then return "demangle" end
                local names = r.mangle("Game::Player")
                if not names or names[1] ~= ".?AVPlayer@Game@@" or names[2] ~= ".?AUPlayer@Game@@" or r.mangle("a<b>") ~= nil then return "mangle" end
                local status = r.status()
                if status.dissect ~= true or status.is64Bit ~= (P == 8) or status.classes < 3 or status.vtables < status.classes or status.modules < 1 then return "status" end
                if status.process ~= manifold.process.info().id or not status.text:find("classes and", 1, true) then return "status text" end
                if not r.claims(dog) or not r.claims(tableSecond) or r.claims(plain) or r.claims({}) then return "claims" end
                if manifold.process.address("Synth::Multi::vftable@Synth::Second") ~= tableSecond then return "symbol" end
                if getRTTIClassName(dog) ~= "FakeRtti::Zoo::Dog" or getRTTIClassName(plain) ~= nil then return "getRTTIClassName" end
                return "ok")LUA");

            lua("rtti label", R"LUA(
                local member = pet - keeper
                local s = createStructure("EmbedProbe")
                s.autoGuess(keeper, 0, keeperSize)
                local labelled = manifold.rtti.label(s, keeper)
                if type(labelled) ~= "number" or labelled < 1 then return "nothing labelled" end
                local found
                for i = 0, s.Count - 1 do
                  local e = s.getElement(i)
                  if e.Offset == member then found = e end
                  if e.Offset > member and e.Offset < keeperSize then return "a field inside the member stayed: " .. string.format("%X", e.Offset) end
                end
                if s.getElement(0).Name ~= "vftable" then return "field 0 is " .. s.getElement(0).Name end
                if not found then return "no field at the member" end
                if found.Vartype == vtPointer then return "the member is still a pointer" end
                local child = found.ChildStruct
                if not child then return "no child structure" end
                if child.Name ~= "FakeRtti::Zoo::Dog" then return "child is " .. child.Name end
                if found.Bytesize ~= keeperSize - member then return "member size " .. found.Bytesize end
                local first = child.getElement(0)
                if not first or first.Name ~= "vftable" then return "child field 0 is " .. tostring(first and first.Name) end
                local t = createStructure("TableProbe")
                t.autoGuess(tableSecond, 0, 4 * P)
                if manifold.rtti.label(t, tableSecond) ~= 4 or t.getElement(0).Name ~= "virtual 0" then return "table labels" end
                local p = createStructure("PlainProbe")
                p.autoGuess(plain, 0, 64)
                if manifold.rtti.label(p, plain) ~= 0 then return "plain memory was labelled" end
                local none, why = manifold.rtti.label(1, keeper)
                if none ~= nil or why.code ~= "ME-7007" then return "label without a structure" end
                s.destroy() t.destroy() p.destroy()
                return "ok")LUA");

            lua("rtti dissect", R"LUA(
                local function structure(name)
                  for i = 0, getStructureCount() - 1 do
                    if getStructure(i).Name == name then return getStructure(i) end
                  end
                end
                if structure("Synth::Owner") then return "the structure exists before the dissect" end
                if manifold.rtti.dissect(owner + P) ~= true then return "dissect" end
                local made = structure("Synth::Owner")
                if not made or made.getElement(0).Name ~= "vftable" then return "no labelled structure" end
                if not structure("Synth::Part") then return "no member structure" end
                local count = getStructureCount()
                if manifold.rtti.dissect(owner) ~= true or getStructureCount() ~= count then return "a second dissect made another structure" end
                local none, why = manifold.rtti.dissect(nothing)
                if none ~= nil or why.code ~= "ME-4301" then return "dissect of nothing" end
                return "ok")LUA");

            lua("rtti alias", R"LUA(
                local R = ManifoldRTTI
                if type(R) ~= "table" or type(R.VERSION) ~= "string" then return "no ManifoldRTTI" end
                for _, name in ipairs({ "demangle", "mangle", "className", "object", "owner", "whois", "bases", "vtable", "vtables", "instances", "dissect", "label", "classes", "status", "reset" }) do
                  if type(R[name]) ~= "function" then return "ManifoldRTTI." .. name end
                end
                if R.className(dog) ~= "FakeRtti::Zoo::Dog" or R.className(plain) ~= nil or R.className(nil) ~= nil then return "className" end
                local o = R.object(walker)
                if not o or o.start ~= duck or o.class ~= "FakeRtti::Zoo::Duck" or o.viaVirtualBase ~= false then return "object" end
                if R.object(plain) ~= nil or R.owner(plain, 8) ~= nil or R.owner(pet + P).start ~= pet then return "owner" end
                if select("#", R.whois(nothing)) ~= 1 or R.whois(nothing) ~= nil or not R.whois(dog):find("is a FakeRtti::Zoo::Dog", 1, true) then return "whois" end
                if R.bases("Synth::Nope") ~= nil or #R.bases("Synth::Multi") ~= 3 or #R.bases(virt) ~= 2 then return "bases" end
                if R.vtable("Synth::Multi", "Synth::Second") ~= tableSecond or R.vtable("Synth::Nope") ~= nil then return "vtable" end
                if R.vtables("Synth::Nope") ~= nil or #R.vtables("Synth::Multi") ~= 2 then return "vtables" end
                if R.instances("Synth::Nope") ~= nil or #R.instances("Synth::Multi", 1.0) ~= 1 then return "instances" end
                if R.demangle(".?AVPlayer@Game@@") ~= "Game::Player" or R.demangle(5) ~= nil or R.mangle("Game::Player")[2] ~= ".?AUPlayer@Game@@" then return "names" end
                if type(R.status()) ~= "string" or not R.status():find("-bit process", 1, true) then return "status" end
                local ok, message = pcall(R.dissect, nothing)
                if ok or not tostring(message):find("no object with run-time type information at", 1, true) then return "dissect error" end
                local s = createStructure("AliasProbe")
                s.autoGuess(multi, 0, 4 * P)
                if R.label(s, multi) ~= 2 or s.getElement(0).Name ~= "vftable" then return "label" end
                s.destroy()
                local listed = false
                for _, name in ipairs(R.classes()) do listed = listed or name == "Synth::Multi" end
                if not listed then return "classes" end
                R.reset()
                if #R.classes() ~= 0 then return "reset" end
                local doc = manifold.doc.entry(R.className)
                if not doc or doc.path ~= "ManifoldRTTI.className" then return "documentation" end
                return "ok")LUA");

            lua("rtti coexistence", R"LUA(
                local r = manifold.rtti
                local before = rawget(_G, "ManifoldGodot")
                ManifoldGodot = { claims = function(address) return address == dog end }
                local claimed, other = r.claims(dog), r.claims(duck)
                ManifoldGodot = { claims = function() error("broken") end }
                local raised = r.claims(dog)
                ManifoldGodot = before
                if claimed then return "RTTI answers for an address Godot claims" end
                if not other then return "RTTI lost an address nobody claims" end
                if not raised then return "an error in claims counted as a claim" end
                return "ok")LUA");
            engine.Lua().Run("ManifoldGodot = { claims = function(address) return address == 0x" + Hex(f.Dog) + " end }", "claims");
            CHECK(session.OthersClaim(f.Dog) && !session.OthersClaim(f.Duck));
            engine.Lua().Run("ManifoldGodot = nil", "claims");
            CHECK(!session.OthersClaim(f.Dog));

            settings.StructureRtti = false;
            CHECK(!session.Dissects() && !session.Enabled());
            lua("rtti off", R"LUA(
                if manifold.rtti.claims(dog) or manifold.rtti.status().dissect then return "the stage is still on" end
                if manifold.rtti.className(dog) ~= "FakeRtti::Zoo::Dog" then return "the lookups went with the stage" end
                return "ok")LUA");
            settings.StructureRtti = true;
            if (empty.Ok())
                process->Free(*empty);
        }

        void WithoutProcess(Engine& engine)
        {
            std::string result;
            const Status status = engine.Lua().RunWithResult(R"LUA(
                local r = manifold.rtti
                local value, problem = r.className(0x10000)
                if value ~= nil or not problem or problem.code ~= "ME-1005" then return "className" end
                value, problem = r.instances("Synth::Base")
                if value ~= nil or problem.code ~= "ME-1005" then return "instances" end
                value, problem = r.classes()
                if value ~= nil or problem.code ~= "ME-1005" then return "classes" end
                if r.claims(0x10000) or r.status().text ~= "no process" or r.reset() ~= true then return "status" end
                if r.demangle(".?AVPlayer@Game@@") ~= "Game::Player" or r.mangle("A")[1] ~= ".?AVA@@" then return "names" end
                local ok, message = pcall(ManifoldRTTI.className, 0x10000)
                if ok or not tostring(message):find("no process is opened", 1, true) then return "alias " .. tostring(message) end
                if ManifoldRTTI.status() ~= "no process" or ManifoldRTTI.demangle(".?AVPlayer@Game@@") ~= "Game::Player" then return "alias without a process" end
                if getRTTIClassName(0x10000) ~= nil then return "getRTTIClassName" end
                return "ok")LUA", "rtti without a process", result);
            CHECK_MSG(status.Ok() && result == "ok", status.Ok() ? result : status.Text);
            CHECK(engine.Rtti().Runtime() == nullptr && engine.Rtti().StatusText() == "no process" && !engine.Rtti().Dissects());
        }

        void AutorunSkip(Engine& engine)
        {
            const std::filesystem::path folder = std::filesystem::temp_directory_path() / ("manifold-rtti-skip-" + std::to_string(::GetCurrentProcessId()));
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            const std::filesystem::path old = folder / "Manifold-RTTI.lua";
            const std::filesystem::path other = folder / "Other.lua";
            {
                std::ofstream(old) << "local RUNTIME_STATE_KEY = \"__MANIFOLD_RTTI_RUNTIME__\"\nOLD_RTTI_RAN = true\nManifoldRTTI = {}\n";
                std::ofstream(other) << "OTHER_RTTI_RAN = true\n";
            }
            CHECK(engine.Lua().RunAutorun(folder).Ok());
            std::string result;
            CHECK(engine.Lua().RunWithResult("return tostring(OLD_RTTI_RAN) .. ' ' .. tostring(OTHER_RTTI_RAN) .. ' ' .. type(ManifoldRTTI.className)", "autorun skip", result).Ok() &&
                result == "nil true function");
            std::filesystem::remove(old, ignored);
            std::filesystem::remove(other, ignored);
            std::filesystem::remove(folder, ignored);
        }

        struct RttiTarget
        {
            PROCESS_INFORMATION Info{};

            RttiTarget() = default;
            RttiTarget(const RttiTarget&) = delete;
            RttiTarget& operator=(const RttiTarget&) = delete;

            ~RttiTarget()
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
                std::wstring command = L"\"" + path.wstring() + L"\" --rtti";
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

        void All(Engine& engine, Settings& settings, const Fake& fake, const char* label)
        {
            Synthetic(engine, fake, label);
            Real(engine, fake, label);
            Dissect(engine, fake, label);
            SessionAndLua(engine, settings, fake, label);
        }

        void InTarget(const std::filesystem::path& targetPath, int pointer, const char* label)
        {
            std::error_code missing;
            if (!std::filesystem::exists(targetPath, missing))
            {
                Skip("the test target " + targetPath.string() + " was not built");
                return;
            }
            RttiTarget target;
            const bool started = target.Start(targetPath);
            CHECK_MSG(started, "could not start " + WideToUtf8(targetPath.wstring()) + " --rtti");
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
            CHECK_MSG(process.PointerSize() == pointer, label);
            const std::string stem = std::filesystem::path(process.Name()).stem().string();
            const std::uint64_t table = engine.Evaluate(stem + ".g_rttiFake").value_or(0);
            CHECK_MSG(table != 0, label);
            const Fake fake = table ? ReadFake(process, table) : Fake{};
            CHECK_MSG(fake.Dog != 0 && fake.Base != 0 && fake.TableBase != 0, label);
            if (fake.Dog && fake.Base && fake.TableBase)
                All(engine, settings, fake, label);
            engine.CloseProcess();
            DWORD code = 0;
            ::GetExitCodeProcess(target.Info.hProcess, &code);
            CHECK_MSG(code == STILL_ACTIVE, std::string(label) + ": the target ended with " + Hex(code, 8));
        }
    }

    int RunRttiTests(const std::filesystem::path& target)
    {
        const int before = Totals().Failures;
        Names();
        {
            Settings settings;
            Engine engine(settings);
            engine.Lua().Run("__manifoldHeadless = true", "headless");
            engine.Unreal().Config().AutoStart = false;
            WithoutProcess(engine);
            AutorunSkip(engine);
            const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (opened)
            {
                FakeRtti::Builder builder;
                CHECK(builder.Build());
                All(engine, settings, Widen(builder.Fake), "own process");
                engine.CloseProcess();
                WithoutProcess(engine);
            }
        }
        InTarget(target.parent_path() / L"TestTarget32.exe", 4, "32-bit target");
        InTarget(target.parent_path() / L"TestTarget.exe", 8, "64-bit target");
        return Totals().Failures - before;
    }

    MANIFOLD_SUITE("rtti", "C++ run-time type information", 37, [](const SuiteContext& context) { return RunRttiTests(context.Target); });
}
