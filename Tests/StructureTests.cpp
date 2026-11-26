#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Scan/StaticPointers.h"
#include "Table/AddressList.h"
#include "Table/CheatTable.h"
#include "Table/ManagedStructures.h"
#include "Table/Structures.h"
#include "Target/Process.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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

    namespace
    {
        Table::StructField MakeField(int offset, std::string name, Scan::ValueType type, int size)
        {
            Table::StructField field;
            field.Offset = offset;
            field.Name = std::move(name);
            field.Type = type;
            field.Size = size;
            return field;
        }

        Table::StructField MakeBits(int offset, std::string name, int start, int length)
        {
            Table::StructField field = MakeField(offset, std::move(name), Scan::ValueType::Binary, Table::BitFieldSize(start, length));
            field.BitStart = start;
            field.BitLength = length;
            return field;
        }

        std::vector<std::string> Names(const Table::StructDefinition& definition)
        {
            std::vector<std::string> names;
            for (const Table::StructField& field : definition.Fields)
                names.push_back(field.Name);
            return names;
        }

        void OffsetParsing()
        {
            CHECK(Table::ParseFieldOffset("1C") == 0x1C);
            CHECK(Table::ParseFieldOffset(" +0x1c ") == 0x1C);
            CHECK(Table::ParseFieldOffset("-8") == -8);
            CHECK(Table::ParseFieldOffset("0") == 0);
            CHECK(Table::ParseFieldOffset("10000000") == Table::kMaxFieldOffset);
            CHECK(!Table::ParseFieldOffset("10000001"));
            CHECK(!Table::ParseFieldOffset("-10000001"));
            CHECK(!Table::ParseFieldOffset(""));
            CHECK(!Table::ParseFieldOffset("+"));
            CHECK(!Table::ParseFieldOffset("+-8"));
            CHECK(!Table::ParseFieldOffset("12G"));
            CHECK(!Table::ParseFieldOffset("FFFFFFFFFFFFFFFF"));
            CHECK_EQ(Table::FieldOffsetText(0x48), std::string("+0048"));
            CHECK_EQ(Table::FieldOffsetText(-8), std::string("-0008"));
        }

        void OverlapRules()
        {
            const Table::StructField dword = MakeField(0x10, "Dword", Scan::ValueType::FourBytes, 4);
            CHECK(Table::FieldsOverlap(dword, MakeField(0x13, "Byte", Scan::ValueType::Byte, 1), 8));
            CHECK(!Table::FieldsOverlap(dword, MakeField(0x14, "Next", Scan::ValueType::Byte, 1), 8));
            CHECK(!Table::FieldsOverlap(dword, MakeField(0x0C, "Before", Scan::ValueType::FourBytes, 4), 8));
            // A pointer is as wide as the target's pointers, whatever its stored size.
            const Table::StructField pointer = MakeField(0x20, "Pointer", Scan::ValueType::Pointer, 8);
            CHECK(!Table::FieldsOverlap(pointer, MakeField(0x24, "After", Scan::ValueType::FourBytes, 4), 4));
            CHECK(Table::FieldsOverlap(pointer, MakeField(0x24, "After", Scan::ValueType::FourBytes, 4), 8));
            // Bit fields share a byte without overlapping; a byte over them overlaps both.
            const Table::StructField low = MakeBits(0x30, "Low", 0, 3);
            const Table::StructField high = MakeBits(0x30, "High", 3, 5);
            CHECK(!Table::FieldsOverlap(low, high, 8));
            CHECK(Table::FieldsOverlap(low, MakeBits(0x30, "Mid", 2, 2), 8));
            CHECK(Table::FieldsOverlap(high, MakeField(0x30, "Flags", Scan::ValueType::Byte, 1), 8));

            Table::StructDefinition definition;
            definition.Fields = { dword, MakeField(0x14, "B", Scan::ValueType::FourBytes, 4), MakeField(0x18, "C", Scan::ValueType::FourBytes, 4) };
            const auto found = definition.Overlapping(MakeField(0x12, "Probe", Scan::ValueType::FourBytes, 4), 8);
            CHECK(found == (std::vector<std::size_t>{ 0, 1 }));
            CHECK(definition.Overlapping(definition.Fields[1], 8, 1).empty());
        }

        void MovingFields()
        {
            Table::StructDefinition definition;
            definition.Fields = { MakeField(0, "A", Scan::ValueType::FourBytes, 4), MakeField(4, "B", Scan::ValueType::FourBytes, 4), MakeField(8, "C", Scan::ValueType::FourBytes, 4),
                MakeField(0x10, "D", Scan::ValueType::EightBytes, 8) };

            // Into a gap: the definition is sorted again and the result says where the field went.
            Table::FieldMove moved = Table::MoveField(definition, 0, 0xC, Table::FieldOverlap::Keep, 8);
            CHECK(Names(definition) == (std::vector<std::string>{ "B", "C", "A", "D" }));
            CHECK(moved.Index == 2 && moved.Twin == 0 && moved.Replaced == 0);

            // Over D with Replace: D goes.
            moved = Table::MoveField(definition, 0, 0x14, Table::FieldOverlap::Replace, 8);
            CHECK(Names(definition) == (std::vector<std::string>{ "C", "A", "B" }));
            CHECK(moved.Index == 2 && moved.Replaced == 1 && definition.Fields[2].Offset == 0x14);

            // Onto C with Keep Both: a twin after the field that was there first.
            moved = Table::MoveField(definition, 2, 8, Table::FieldOverlap::Keep, 8);
            CHECK(Names(definition) == (std::vector<std::string>{ "C", "B", "A" }));
            CHECK(moved.Index == 1 && moved.Twin == 1 && moved.Replaced == 0);

            // Backwards over several, with Replace: the index counts the fields removed before it.
            definition.Fields = { MakeField(0, "A", Scan::ValueType::Byte, 1), MakeField(1, "B", Scan::ValueType::Byte, 1), MakeField(2, "C", Scan::ValueType::Byte, 1),
                MakeField(8, "Wide", Scan::ValueType::FourBytes, 4) };
            moved = Table::MoveField(definition, 3, 0, Table::FieldOverlap::Replace, 8);
            CHECK(Names(definition) == (std::vector<std::string>{ "Wide" }));
            CHECK(moved.Index == 0 && moved.Replaced == 3);

            // Out of range is clamped by the model; the window refuses it before.
            Table::MoveField(definition, 0, Table::kMaxFieldOffset + 100, Table::FieldOverlap::Keep, 8);
            CHECK_EQ(definition.Fields[0].Offset, Table::kMaxFieldOffset);
            Table::MoveField(definition, 5, 0, Table::FieldOverlap::Keep, 8);
            CHECK_EQ(definition.Fields.size(), 1u);
        }

        void PastingFields()
        {
            Table::StructDefinition definition;
            definition.Fields = { MakeField(0, "A", Scan::ValueType::FourBytes, 4), MakeField(0x40, "Guess 40", Scan::ValueType::FourBytes, 4),
                MakeField(0x48, "Guess 48", Scan::ValueType::FourBytes, 4), MakeField(0x50, "Keep", Scan::ValueType::FourBytes, 4) };
            // Copied out of order (as a selection may be), with a gap: the distances stay.
            const std::vector<Table::StructField> copied = { MakeField(0x18, "Y", Scan::ValueType::Float, 4), MakeField(0x10, "X", Scan::ValueType::Float, 4),
                MakeField(0x1C, "Z", Scan::ValueType::Float, 4) };
            const auto pasted = Table::PasteFields(definition, copied, 0x40, 8);
            CHECK(pasted && pasted->Added == 3 && pasted->Replaced == 2 && pasted->First == 0x40);
            CHECK(Names(definition) == (std::vector<std::string>{ "A", "X", "Y", "Z", "Keep" }));
            CHECK(definition.Fields[1].Offset == 0x40 && definition.Fields[2].Offset == 0x48 && definition.Fields[3].Offset == 0x4C);

            // At their own offsets.
            Table::StructDefinition other;
            const auto inPlace = Table::PasteFields(other, copied, std::nullopt, 8);
            CHECK(inPlace && inPlace->First == 0x10 && other.Fields.size() == 3 && other.Fields[0].Offset == 0x10 && other.Fields[2].Offset == 0x1C);

            // Pasted bit fields keep their neighbours in the same byte.
            Table::StructDefinition flags;
            flags.Fields = { MakeBits(0, "Low", 0, 4) };
            CHECK(Table::PasteFields(flags, { MakeBits(0, "High", 4, 4) }, std::nullopt, 8)->Replaced == 0);
            CHECK_EQ(flags.Fields.size(), 2u);

            // Beyond the limit: nothing changes.
            const std::vector<Table::StructField> before = definition.Fields;
            CHECK(!Table::PasteFields(definition, copied, Table::kMaxFieldOffset - 4, 8));
            CHECK(!Table::PasteFields(definition, copied, -Table::kMaxFieldOffset - 1, 8));
            CHECK(definition.Fields == before);
            CHECK(Table::PasteFields(definition, {}, 0, 8)->Added == 0);
        }

        void ClipboardFormat()
        {
            Table::StructureStore store;
            Table::StructDefinition& vector = store.Add("Vector3");
            vector.Fields = { MakeField(0, "X", Scan::ValueType::Float, 4), MakeField(4, "Y", Scan::ValueType::Float, 4), MakeField(8, "Z", Scan::ValueType::Float, 4) };
            Table::StructDefinition& loop = store.Add("Loop");
            loop.Fields = { MakeField(0, "Self", Scan::ValueType::Pointer, 8) };
            loop.Fields[0].ChildStructure = "Loop";

            std::vector<Table::StructField> fields = { MakeField(0x10, "Health", Scan::ValueType::Float, 4), MakeField(0x20, "Position", Scan::ValueType::ByteArray, 12),
                MakeField(0x30, "Name", Scan::ValueType::String, 32), MakeBits(0x50, "Flag", 3, 2), MakeField(0x58, "Next", Scan::ValueType::Pointer, 8),
                MakeField(0x60, "Money", Scan::ValueType::ByteArray, 4) };
            fields[1].ChildStructure = "Vector3";
            fields[2].Unicode = true;
            fields[3].Display = Table::FieldDisplay::Binary;
            fields[4].ChildStructure = "Loop";
            fields[4].Display = Table::FieldDisplay::Hex;
            fields[5].CustomType = "Money";

            const std::string text = Table::FieldsToXml(store, fields);
            // Cheat Engine offers Paste only for text that starts like this.
            CHECK_MSG(text.rfind("<Elements", 0) == 0, text.substr(0, 40));
            CHECK(text.find("Vartype=\"Unicode String\"") != std::string::npos);
            CHECK(text.find("Customtype=\"Money\"") != std::string::npos);
            CHECK(text.find("<Structure Name=\"Vector3\"") != std::string::npos);

            const auto read = Table::FieldsFromXml(text);
            CHECK(read.has_value());
            if (read)
            {
                CHECK(read->Fields == fields);
                CHECK_EQ(read->Structures.size(), 2u);
                bool vectorBack = false;
                for (const auto& child : read->Structures)
                    vectorBack = vectorBack || (child->Name == "Vector3" && child->Fields == vector.Fields);
                CHECK(vectorBack);
            }

            // What Cheat Engine 7.5 puts on the clipboard: no header, an empty description left out, a local child structure inside.
            const char* cheatEngine =
                "<Elements>\r\n"
                "  <Element Offset=\"8\" OffsetHex=\"00000008\" Vartype=\"Pointer\" Bytesize=\"8\" DisplayMethod=\"unsigned integer\" ChildStruct=\"Inner\" Nested=\"1\">\r\n"
                "    <Structure Name=\"Inner\" AutoFill=\"0\" AutoCreate=\"1\" DefaultHex=\"0\" AutoDestroy=\"0\" DoNotSaveLocal=\"0\" RLECompression=\"1\" AutoCreateStructsize=\"4096\">\r\n"
                "      <Elements>\r\n"
                "        <Element Offset=\"0\" OffsetHex=\"00000000\" Description=\"Count\" Vartype=\"4 Bytes\" Bytesize=\"4\" DisplayMethod=\"signed integer\"/>\r\n"
                "      </Elements>\r\n"
                "    </Structure>\r\n"
                "  </Element>\r\n"
                "  <Element Offset=\"16\" OffsetHex=\"00000010\" Description=\"Gold\" Vartype=\"Custom\" Customtype=\"Money\" Bytesize=\"4\" DisplayMethod=\"hexadecimal\"/>\r\n"
                "</Elements>\r\n";
            const auto fromCe = Table::FieldsFromXml(cheatEngine);
            CHECK(fromCe.has_value());
            if (fromCe)
            {
                CHECK_EQ(fromCe->Fields.size(), 2u);
                CHECK(fromCe->Fields[0].Offset == 8 && fromCe->Fields[0].Name.empty() && fromCe->Fields[0].Type == Scan::ValueType::Pointer && fromCe->Fields[0].ChildStructure == "Inner");
                CHECK(fromCe->Fields[1].CustomType == "Money" && fromCe->Fields[1].Display == Table::FieldDisplay::Hex && fromCe->Fields[1].Name == "Gold");
                CHECK(fromCe->Structures.size() == 1 && fromCe->Structures[0]->Name == "Inner" && fromCe->Structures[0]->Fields.size() == 1 &&
                      fromCe->Structures[0]->Fields[0].Display == Table::FieldDisplay::Signed);
            }

            CHECK(!Table::FieldsFromXml(""));
            CHECK(!Table::FieldsFromXml("just text"));
            CHECK(!Table::FieldsFromXml("<Elements></Elements>"));
            CHECK(!Table::FieldsFromXml("<CheatTable><CheatEntries/></CheatTable>"));
        }

        void UndoHistory()
        {
            Table::StructureStore store;
            Table::StructDefinition& definition = store.Add("Player");
            definition.Fields = { MakeField(0, "A", Scan::ValueType::FourBytes, 4) };
            Table::StructureHistory history;
            CHECK(!history.Undo(store) && !history.Redo(store));

            const auto edit = [&](const char* label, auto change, std::string merge = {}, std::uint64_t group = 0)
                {
                    std::vector<Table::StructField> before = definition.Fields;
                    change();
                    history.Record(definition.Name, std::move(before), definition.Fields, label, std::move(merge), group);
                };
            edit("Rename Field", [&]() { definition.Fields[0].Name = "Health"; });
            edit("Change Offset", [&]() { Table::MoveField(definition, 0, 8, Table::FieldOverlap::Keep, 8); });
            edit("Nothing", []() {});
            CHECK_EQ(history.UndoCount(), 2u);
            CHECK_EQ(history.UndoLabel(), std::string("Change Offset"));

            auto undone = history.Undo(store);
            CHECK(undone && undone->Label == "Change Offset" && !undone->Missing);
            CHECK(definition.Fields[0].Offset == 0 && definition.Fields[0].Name == "Health");
            CHECK(history.CanRedo() && history.RedoLabel() == "Change Offset");
            history.Undo(store);
            CHECK_EQ(definition.Fields[0].Name, std::string("A"));
            history.Redo(store);
            history.Redo(store);
            CHECK(definition.Fields[0].Offset == 8 && definition.Fields[0].Name == "Health" && !history.CanRedo());

            // A new edit drops what could be redone.
            history.Undo(store);
            edit("Rename Field", [&]() { definition.Fields[0].Name = "Armor"; });
            CHECK(!history.CanRedo());

            // Typing a length digit by digit is one step; back to the start it is none.
            const std::size_t steps = history.UndoCount();
            edit("Change Length", [&]() { definition.Fields[0].Size = 1; }, "length A");
            edit("Change Length", [&]() { definition.Fields[0].Size = 12; }, "length A");
            CHECK_EQ(history.UndoCount(), steps + 1);
            history.Undo(store);
            CHECK_EQ(definition.Fields[0].Size, 4);
            edit("Change Length", [&]() { definition.Fields[0].Size = 5; }, "length A");
            edit("Change Length", [&]() { definition.Fields[0].Size = 4; }, "length A");
            CHECK_EQ(history.UndoCount(), steps);

            // A group of steps over two structures is undone and redone at once.
            Table::StructDefinition& other = store.Add("Enemy");
            other.Fields = { MakeField(0, "E", Scan::ValueType::FourBytes, 4) };
            {
                std::vector<Table::StructField> before = definition.Fields;
                definition.Fields.clear();
                history.Record("Player", std::move(before), definition.Fields, "Delete Fields", {}, 7);
                before = other.Fields;
                other.Fields.clear();
                history.Record("Enemy", std::move(before), other.Fields, "Delete Fields", {}, 7);
            }
            history.Undo(store);
            CHECK(definition.Fields.size() == 1 && other.Fields.size() == 1);
            history.Redo(store);
            CHECK(definition.Fields.empty() && other.Fields.empty());
            history.Undo(store);

            // A renamed structure keeps its steps, and fields pointing to it follow.
            history.Renamed("Player", "Hero");
            definition.Name = "Hero";
            edit("Rename Field", [&]() { definition.Fields[0].Name = "Shield"; });
            CHECK(history.Undo(store) && definition.Fields[0].Name == "Armor");

            // A structure that is gone: the step is dropped and said so.
            edit("Rename Field", [&]() { definition.Fields[0].Name = "Gone"; });
            store.Remove(definition);
            auto missing = history.Undo(store);
            CHECK(missing && missing->Missing && missing->Structure == "Hero");

            // The stack has a limit.
            Table::StructDefinition& many = store.Add("Many");
            Table::StructureHistory bounded;
            for (std::size_t i = 0; i < Table::StructureHistory::kLimit + 20; ++i)
            {
                std::vector<Table::StructField> before = many.Fields;
                many.Fields.push_back(MakeField(static_cast<int>(i) * 4, "F", Scan::ValueType::FourBytes, 4));
                bounded.Record("Many", std::move(before), many.Fields, "Add Field");
            }
            CHECK_EQ(bounded.UndoCount(), Table::StructureHistory::kLimit);

            Table::StructDefinition& wide = store.Add("Wide");
            const std::size_t fields = Table::StructureHistory::kLimit + 50;
            for (std::size_t i = 0; i < fields; ++i)
                wide.Fields.push_back(MakeField(static_cast<int>(i) * 4, "F", Scan::ValueType::FourBytes, 4));
            Table::StructureHistory large;
            for (std::size_t i = 0; i < fields; ++i)
            {
                std::vector<Table::StructField> before = wide.Fields;
                wide.Fields.erase(wide.Fields.begin());
                large.Record("Wide", std::move(before), wide.Fields, "Delete Fields", {}, 9);
            }
            CHECK(large.Undo(store));
            CHECK_MSG(wide.Fields.size() == fields, std::to_string(wide.Fields.size()) + " fields after the undo");
            CHECK(!large.CanUndo());
        }
    }

    int RunStructureTests()
    {
        const int before = Totals().Failures;
        FieldAtAddress();
        MatchPointerBlocks();
        ManagedPointerWidth();
        EmbeddedToAddressList();
        OffsetParsing();
        OverlapRules();
        MovingFields();
        PastingFields();
        ClipboardFormat();
        UndoHistory();
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
