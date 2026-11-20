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

    int RunStructureTests()
    {
        const int before = Totals().Failures;
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
