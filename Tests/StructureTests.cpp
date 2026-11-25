#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Scan/StaticPointers.h"
#include "Table/AddressList.h"
#include "Table/ManagedStructures.h"
#include "Table/Structures.h"
#include "Target/Process.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        struct alignas(16) GuessSample
        {
            char Text[8] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H' };
            void* Pointer = nullptr;
            std::uint32_t Number = 1234;
            float Ratio = 1.5f;
        };

        GuessSample g_guessSample;
        std::uint8_t* g_staticPointer = nullptr;

        const Table::StructField* FieldAt(const Table::StructDefinition& definition, int offset)
        {
            for (const Table::StructField& field : definition.Fields)
                if (field.Offset == offset)
                    return &field;
            return nullptr;
        }

        void MatchPointerBlocks()
        {
            std::vector<std::uint8_t> bytes(64, 0);
            const std::uint64_t inside = 0x7FF612340010;
            const std::uint64_t outside = 0x7FF612350000;
            std::memcpy(bytes.data() + 8, &inside, 8);
            std::memcpy(bytes.data() + 24, &outside, 8);
            std::memcpy(bytes.data() + 33, &inside, 8);
            std::vector<std::uint64_t> found;
            Scan::MatchPointers(bytes, 0x1000, 8, 0x7FF612340000, 0x7FF612340100, found);
            CHECK(found.size() == 1 && found.front() == 0x1008);

            found.clear();
            Scan::MatchPointers(std::span<const std::uint8_t>(bytes.data() + 4, 60), 0x1004, 8, 0x7FF612340000, 0x7FF612340100, found);
            CHECK(found.size() == 1 && found.front() == 0x1008);

            std::vector<std::uint8_t> small(16, 0);
            const std::uint32_t value = 0x00401020;
            std::memcpy(small.data() + 4, &value, 4);
            found.clear();
            Scan::MatchPointers(small, 0x2000, 4, 0x00401000, 0x00401020, found);
            CHECK(found.size() == 1 && found.front() == 0x2004);
        }

        void GuessOwnMemory(const Target::Process& process)
        {
            g_guessSample.Pointer = &g_staticPointer;
            const std::uint64_t base = reinterpret_cast<std::uint64_t>(&g_guessSample);
            Table::StructDefinition definition;
            Table::GuessFields(process, definition, base, 0, static_cast<int>(sizeof(GuessSample)));
            const Table::StructField* text = FieldAt(definition, 0);
            CHECK(text && text->Type == Scan::ValueType::String && text->Size <= 8);
            const Table::StructField* pointer = FieldAt(definition, 8);
            CHECK_MSG(pointer && pointer->Type == Scan::ValueType::Pointer, "the pointer after a string is a pointer");

            Table::StructDefinition shifted;
            Table::GuessFields(process, shifted, base + 4, 0, static_cast<int>(sizeof(GuessSample)) - 4);
            const Table::StructField* moved = FieldAt(shifted, 4);
            CHECK_MSG(moved && moved->Type == Scan::ValueType::Pointer, "alignment follows the real address");
        }

        void FindOwnStaticPointer(const Target::Process& process)
        {
            auto object = std::make_unique<std::uint8_t[]>(0x200);
            g_staticPointer = object.get();
            const std::uint64_t target = reinterpret_cast<std::uint64_t>(object.get()) + 0x30;
            Scan::StaticPointerSearch search;
            search.Target = target;
            search.MaxOffset = 0x100;
            Scan::PatternProgress progress;
            bool truncated = true;
            const std::vector<Scan::StaticPointer> hits = Scan::FindStaticPointers(process, search, &progress, &truncated);
            const std::uint64_t slot = reinterpret_cast<std::uint64_t>(&g_staticPointer);
            const auto hit = std::find_if(hits.begin(), hits.end(), [&](const Scan::StaticPointer& each) { return each.Address == slot; });
            CHECK_MSG(hit != hits.end(), Grouped(hits.size()) + " hits, none at the test's own global");
            if (hit != hits.end())
            {
                CHECK(hit->Offset(target) == 0x30);
                CHECK(!hit->Module.empty() && hit->ModuleBase != 0);
            }
            CHECK(!truncated);
            CHECK(progress.Done.load() > 0 && progress.Done.load() <= progress.Total.load());

            search.MaxOffset = 0x10;
            const std::vector<Scan::StaticPointer> narrow = Scan::FindStaticPointers(process, search);
            CHECK(std::none_of(narrow.begin(), narrow.end(), [&](const Scan::StaticPointer& each) { return each.Address == slot; }));
            g_staticPointer = nullptr;
        }
    }

    namespace
    {
        void FieldAtAddress()
        {
            // Top structure at 0x10000: an int at +0, an embedded 16-byte
            // structure at +8 with a float at +4 inside it, a pointer at +18.
            const std::vector<Table::FieldSpan> spans{
                { 0x10000, 4, 0 }, { 0x10008, 16, 0 }, { 0x1000C, 4, 1 }, { 0x10018, 8, 0 }, { 0, 4, 0 }, { 0x20000, 0, 2 },
            };
            CHECK_EQ(Table::ContainingField(spans, 0x10000), 0);
            CHECK_EQ(Table::ContainingField(spans, 0x10003), 0);
            CHECK_EQ(Table::ContainingField(spans, 0x10004), -1);
            CHECK_EQ(Table::ContainingField(spans, 0x10008), 1);
            CHECK_EQ(Table::ContainingField(spans, 0x1000D), 2);
            CHECK_EQ(Table::ContainingField(spans, 0x10017), 1);
            CHECK_EQ(Table::ContainingField(spans, 0x1001F), 3);
            CHECK_EQ(Table::ContainingField(spans, 0x10020), -1);
            CHECK_EQ(Table::ContainingField(spans, 0x2), -1);
            // A size of 0 still holds its first byte.
            CHECK_EQ(Table::ContainingField(spans, 0x20000), 5);
            // Of equal spans the deeper one wins.
            CHECK_EQ(Table::ContainingField({ { 0x500000, 8, 0 }, { 0x500000, 8, 1 } }, 0x500004), 1);

            CHECK(Table::FilterAddress("7FF6A0001000") == std::optional<std::uint64_t>(0x7FF6A0001000ull));
            CHECK(Table::FilterAddress(" 0x10000 ") == std::optional<std::uint64_t>(0x10000));
            CHECK(!Table::FilterAddress("FFFF").has_value());
            CHECK(!Table::FilterAddress("+1A0000").has_value());
            CHECK(!Table::FilterAddress("health").has_value());
            CHECK(!Table::FilterAddress("added").has_value());
            CHECK(!Table::FilterAddress("FACADE").has_value());
            CHECK(Table::FilterAddress("0xFACADE") == std::optional<std::uint64_t>(0xFACADE));
            CHECK(Table::FilterAddress("DEAD0") == std::optional<std::uint64_t>(0xDEAD0));
            CHECK(!Table::FilterAddress("game.exe+10").has_value());
            CHECK(!Table::FilterAddress("12345678123456789").has_value());
            CHECK(!Table::FilterAddress("").has_value());
        }
    }

    namespace
    {
        // ∑ The pointer rescan in Crab read 8 bytes in the last page of the address space (a changing sentinel plus
        //   an offset). The page loop's end wrapped to 0 and the zero fill ran up to 4 KB past the 8-byte buffer.
        //   Nothing there is readable, so each read must fail and leave every byte after the buffer as it was.
        void ReadAtTopOfAddressSpace(const Target::Process& process)
        {
            struct Guarded
            {
                std::uint64_t Value = 0x1122334455667788ull;
                std::uint8_t After[0x1000];
            } guarded{};
            std::fill(std::begin(guarded.After), std::end(guarded.After), std::uint8_t(0xA5));
            const auto afterIntact = [&] { return std::all_of(std::begin(guarded.After), std::end(guarded.After), [](std::uint8_t b) { return b == 0xA5; }); };
            for (const std::uint64_t address : { 0xFFFFFFFFFFFFF010ull, 0xFFFFFFFFFFFFFFF0ull, 0xFFFFFFFFFFFFFFF8ull, 0xFFFFFFFFFFFFFFFCull, 0xFFFFFFFFFFFFFFFFull, 0xFFFF800000000FFCull })
            {
                CHECK(process.Read(address, &guarded.Value, sizeof(guarded.Value)) == 0);
                CHECK(!process.ReadExact(address, &guarded.Value, sizeof(guarded.Value)));
                CHECK(afterIntact());
            }
        }
    }

    namespace
    {
        // ∑ Terraria (32-bit .NET): references and IntPtr took 8 bytes, so the dissect showed two neighbouring
        //   references as one value. They are as wide as the target's pointers; a Vector2 stays embedded.
        void ManagedPointerWidth()
        {
            std::vector<Target::MonoField> fields(5);
            fields[0].Name = "whoAmI"; fields[0].Offset = 4; fields[0].TypeCode = 0x08;
            fields[1].Name = "position"; fields[1].Offset = 0x20; fields[1].TypeCode = 0x11; fields[1].TypeHandle = 0x1234; fields[1].TypeName = "Microsoft.Xna.Framework.Vector2"; fields[1].ValueSize = 8;
            fields[2].Name = "creativeTracker"; fields[2].Offset = 0x70; fields[2].TypeCode = 0x12;
            fields[3].Name = "DpadRadial"; fields[3].Offset = 0x74; fields[3].TypeCode = 0x12;
            fields[4].Name = "handle"; fields[4].Offset = 0x78; fields[4].TypeCode = 0x18;
            for (const int pointerSize : { 4, 8 })
            {
                // A value type's field offsets count the boxed object's header (two pointers) in.
                Table::ManagedLayouts nested;
                std::vector<Target::MonoField>& inner = nested["Microsoft.Xna.Framework.Vector2"];
                inner.resize(2);
                inner[0].Name = "X";
                inner[0].Offset = 2 * pointerSize;
                inner[0].TypeCode = 0x0c;
                inner[1].Name = "Y";
                inner[1].Offset = 2 * pointerSize + 4;
                inner[1].TypeCode = 0x0c;
                Table::StructDefinition player;
                Table::FillFromManaged(player, fields, 0, nested, pointerSize);
                CHECK_EQ(player.Fields.size(), 5u);
                if (player.Fields.size() != 5)
                    continue;
                CHECK(player.Fields[1].ChildStructure == "Microsoft.Xna.Framework.Vector2" && player.Fields[1].Size == 8 && player.Fields[1].Type != Scan::ValueType::Pointer);
                for (const std::size_t index : { std::size_t(2), std::size_t(3), std::size_t(4) })
                    CHECK_MSG(player.Fields[index].Type == Scan::ValueType::Pointer && player.Fields[index].Size == pointerSize,
                        player.Fields[index].Name + " has " + std::to_string(player.Fields[index].Size) + " bytes with " + std::to_string(pointerSize) + "-byte pointers");
                Table::StructureStore store;
                store.Add("Terraria.Player");
                Table::AddManagedLayouts(store, nested, 2 * pointerSize, "Terraria.Player", pointerSize);
                const Table::StructDefinition* vector = store.Find("Microsoft.Xna.Framework.Vector2");
                CHECK(vector && vector->Fields.size() == 2 && vector->Fields[1].Offset == 4 && vector->Fields[1].Type == Scan::ValueType::Float);
            }
        }

        struct Mover
        {
            std::uint32_t Id = 7;
            float Unused = 0.0f;
            struct { float X = 34676.0f; float Y = 4422.0f; } Position;
            struct { float X = -1.5f; float Y = 2.5f; } Velocity;
            void* Owner = nullptr;
        };

        Mover g_mover;

        // ∑ Terraria: "add the structure to the address list" made position and velocity one AOB record of
        //   8 bytes each. An embedded structure becomes a group that counts from its offset, with X and Y.
        void EmbeddedToAddressList()
        {
            Settings settings;
            Engine engine(settings);
            CHECK(engine.OpenProcess(MakeProcessId(::GetCurrentProcessId())).Ok());
            Table::StructDefinition& vector = engine.Structures().Add("Vector2");
            vector.Fields.push_back({ 0, "X", Scan::ValueType::Float, 4 });
            vector.Fields.push_back({ 4, "Y", Scan::ValueType::Float, 4 });
            Table::StructDefinition& looped = engine.Structures().Add("Looped");
            looped.Fields.push_back({ 0, "Self", Scan::ValueType::ByteArray, 8 });
            looped.Fields.back().ChildStructure = "Looped";
            std::vector<Table::StructField> fields;
            fields.push_back({ 0, "Id", Scan::ValueType::FourBytes, 4 });
            fields.push_back({ 8, "Position", Scan::ValueType::ByteArray, 8 });
            fields.back().ChildStructure = "Vector2";
            fields.push_back({ 0x10, "Velocity", Scan::ValueType::ByteArray, 8 });
            fields.back().ChildStructure = "Vector2";
            fields.push_back({ 0x18, "Owner", Scan::ValueType::Pointer, 8 });
            fields.back().ChildStructure = "Vector2";
            fields.push_back({ 0, "Loop", Scan::ValueType::ByteArray, 8 });
            fields.back().ChildStructure = "Looped";

            Table::AddressList& table = engine.Table();
            Table::MemoryRecord& group = table.AddGroup("Mover");
            group.AddressGroupHeader = true;
            group.Address = Hex(reinterpret_cast<std::uint64_t>(&g_mover));
            std::vector<Table::MemoryRecord*> added;
            for (const Table::StructField& field : fields)
                added.push_back(&Table::AddFieldRecords(table, engine.Structures(), field, Table::RelativeFieldAddress(field.Offset), &group, 0, 4));
            engine.RefreshValues(true);

            Table::MemoryRecord& position = *added[1];
            CHECK(position.Kind == Table::RecordKind::Group && position.AddressGroupHeader && position.Address == "+8");
            CHECK_EQ(position.Children.size(), 2u);
            if (position.Children.size() == 2)
            {
                const Table::MemoryRecord& y = *position.Children[1];
                CHECK(y.Description == "Y" && y.Type == Scan::ValueType::Float && y.Address == "+4");
                CHECK_MSG(y.AddressOk && y.ResolvedAddress == reinterpret_cast<std::uint64_t>(&g_mover.Position.Y), y.AddressError);
            }
            Table::MemoryRecord& velocity = *added[2];
            CHECK(velocity.Kind == Table::RecordKind::Group && velocity.Children.size() == 2);
            if (velocity.Children.size() == 2)
                CHECK(velocity.Children[0]->AddressOk && velocity.Children[0]->ResolvedAddress == reinterpret_cast<std::uint64_t>(&g_mover.Velocity.X));
            CHECK(added[3]->Kind == Table::RecordKind::Value && added[3]->Type == Scan::ValueType::Pointer);
            // A structure that embeds itself stops at the depth limit.
            int depth = 0;
            for (const Table::MemoryRecord* at = added[4]; at && at->Kind == Table::RecordKind::Group; at = at->Children.empty() ? nullptr : at->Children.front().get())
                ++depth;
            CHECK_EQ(depth, 4);
        }
    }

    int RunStructureTests()
    {
        const int before = Totals().Failures;
        FieldAtAddress();
        MatchPointerBlocks();
        ManagedPointerWidth();
        EmbeddedToAddressList();
        auto opened = Target::Process::Open(MakeProcessId(::GetCurrentProcessId()));
        CHECK(opened.Ok());
        if (opened.Ok())
        {
            std::shared_ptr<Target::Process> process = *opened;
            process->RefreshModules();
            ReadAtTopOfAddressSpace(*process);
            GuessOwnMemory(*process);
            FindOwnStaticPointer(*process);
        }
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("structures", "Structures and static pointers", 25, [](const SuiteContext&) { return RunStructureTests(); });
}
