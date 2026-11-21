#include "TestKit.h"

#include "Core/Text.h"
#include "Scan/StaticPointers.h"
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
            CHECK(!Table::FilterAddress("game.exe+10").has_value());
            CHECK(!Table::FilterAddress("12345678123456789").has_value());
            CHECK(!Table::FilterAddress("").has_value());
        }
    }

    int RunStructureTests()
    {
        const int before = Totals().Failures;
        FieldAtAddress();
        MatchPointerBlocks();
        auto opened = Target::Process::Open(MakeProcessId(::GetCurrentProcessId()));
        CHECK(opened.Ok());
        if (opened.Ok())
        {
            std::shared_ptr<Target::Process> process = *opened;
            process->RefreshModules();
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
