#include "TestKit.h"

#include "Core/Text.h"
#include "Scan/Scanner.h"
#include "Scan/ValueType.h"
#include "Target/Process.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

// ∑ Scan workers read past each 2 MiB chunk by the value or pattern length - 1, so a match that crosses
//   the boundary is found. The buffers had a fixed margin (+256 in FindPattern, kGroupMaxBytes + 64 in
//   a first scan, kWindowBytes + kGroupMaxBytes + 64 in a next scan) and longer values wrote past them.
//   The workers now skip a read that would not fit, so an undersized buffer shows up here as a missed
//   match at the boundary rather than as a silent heap overwrite.
namespace Manifold::Tests
{
    namespace
    {
        using namespace Manifold::Scan;

        constexpr std::size_t kMb = 1024 * 1024;
        constexpr std::size_t kChunk = 2 * kMb;  // kChunkBytes in Scan/Scanner.cpp
        constexpr std::size_t kBlock = 6 * kMb;

        struct Block
        {
            std::uint8_t* Data = nullptr;

            Block() { Data = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, kBlock, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)); }
            ~Block()
            {
                if (Data)
                    ::VirtualFree(Data, 0, MEM_RELEASE);
            }
            Block(const Block&) = delete;
            Block& operator=(const Block&) = delete;

            std::uint64_t At(std::size_t offset) const { return reinterpret_cast<std::uint64_t>(Data) + offset; }
        };

        void Fill(Block& block, std::uint32_t seed, bool letters)
        {
            std::uint32_t state = seed;
            for (std::size_t i = 0; i < kBlock; ++i)
            {
                state = state * 1664525u + 1013904223u;
                const auto byte = static_cast<std::uint8_t>(state >> 24);
                block.Data[i] = letters ? static_cast<std::uint8_t>('a' + byte % 26) : byte;
            }
        }

        Pattern Exact(const Block& block, std::size_t offset, std::size_t length)
        {
            Pattern pattern;
            pattern.Bytes.assign(block.Data + offset, block.Data + offset + length);
            pattern.Mask.assign(length, 0xFF);
            return pattern;
        }

        std::string Hex(const Block& block, std::size_t offset, std::size_t length)
        {
            std::string text;
            text.reserve(length * 3);
            char digits[4];
            for (std::size_t i = 0; i < length; ++i)
            {
                std::snprintf(digits, sizeof(digits), i ? " %02X" : "%02X", block.Data[offset + i]);
                text += digits;
            }
            return text;
        }

        bool Wait(Scanner& scanner)
        {
            for (int i = 0; i < 3000; ++i)
            {
                if (scanner.Poll())
                    return scanner.LastError().Ok();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        }

        // One hit, at the planted offset, for a pattern taken from the block itself.
        void FindOnce(const Target::Process& process, const Block& block, std::size_t offset, std::size_t length, const char* where)
        {
            const std::vector<std::uint64_t> hits = FindPattern(process, Exact(block, offset, length), block.At(0), block.At(kBlock), PatternFilter{}, 4);
            CHECK_MSG(hits.size() == 1 && hits.front() == block.At(offset),
                Format("%zu-byte pattern %s at +0x%zX: %zu hits", length, where, offset, hits.size()));
        }

        void PatternReads(const Target::Process& process)
        {
            Block block;
            CHECK(block.Data != nullptr);
            if (!block.Data)
                return;
            Fill(block, 7, false);

            // The reported case: a full first chunk with more than 2 MiB + 256 left in the region, pattern
            // over 257 bytes. The match is well inside the chunk; the overlong read came before matching.
            for (const std::size_t length : { std::size_t(258), std::size_t(300), std::size_t(5000) })
                FindOnce(process, block, 4096, length, "inside the first chunk");

            // A match that crosses a chunk boundary, short and long. The long ones need the chunk's read to
            // run length - 1 bytes into the next chunk, which only fits a buffer sized for it.
            for (const std::size_t length : { std::size_t(8), std::size_t(256), std::size_t(257), std::size_t(300), std::size_t(70000), 3 * kMb })
                FindOnce(process, block, kChunk - 100, length, "across the first boundary");
            FindOnce(process, block, 2 * kChunk - 1, 300, "one byte before the second boundary");

            // At the very end of the region the read is clamped to the region, not past it.
            FindOnce(process, block, kBlock - 300, 300, "ending at the region end");
            FindOnce(process, block, kBlock - 8, 8, "ending at the region end");
        }

        ScanOptions Over(const Block& block)
        {
            ScanOptions options;
            options.Mode = Compare::Exact;
            options.Start = block.At(0);
            options.Stop = block.At(kBlock);
            options.Alignment = 1;
            options.Threads = 4;
            return options;
        }

        // First scan for one long value across the first boundary, then next scans that re-read it.
        void ValueScan(const std::shared_ptr<Target::Process>& process, Block& block, ScanOptions options, std::size_t offset, std::size_t length, const char* what)
        {
            Scanner scanner;
            const bool first = scanner.Start(process, options, true) && Wait(scanner);
            CHECK_MSG(first, Format("%s of %zu bytes: first scan failed", what, length));
            if (!first)
                return;
            ScanRecord record;
            CHECK_MSG(scanner.Count() == 1 && scanner.Record(0, record) && record.Address == block.At(offset),
                Format("%s of %zu bytes across the boundary: %llu results", what, length, static_cast<unsigned long long>(scanner.Count())));

            // The next scan reads each result into a window that must hold the whole value (over 64 KiB here).
            const bool same = scanner.Start(process, options, false) && Wait(scanner);
            CHECK_MSG(same && scanner.Count() == 1, Format("%s of %zu bytes: next scan kept %llu", what, length, static_cast<unsigned long long>(scanner.Count())));

            const std::size_t last = offset + length - 1;
            const std::uint8_t keep = block.Data[last];
            block.Data[last] = static_cast<std::uint8_t>(keep ^ 0x01);
            const bool changed = scanner.Start(process, options, false) && Wait(scanner);
            CHECK_MSG(changed && scanner.Count() == 0, Format("%s of %zu bytes: last byte changed, %llu left", what, length, static_cast<unsigned long long>(scanner.Count())));
            block.Data[last] = keep;
        }

        void ValueReads(const std::shared_ptr<Target::Process>& process)
        {
            Block block;
            CHECK(block.Data != nullptr);
            if (!block.Data)
                return;
            const std::size_t offset = kChunk - 100;

            Fill(block, 11, false);
            for (const std::size_t length : { std::size_t(300), std::size_t(5000), std::size_t(70000) })
            {
                ScanOptions options = Over(block);
                options.Type = ValueType::ByteArray;
                options.Value = Hex(block, offset, length);
                ValueScan(process, block, options, offset, length, "byte array");
            }

            Fill(block, 13, true);
            for (const std::size_t length : { std::size_t(5000), std::size_t(70000) })
            {
                ScanOptions options = Over(block);
                options.Type = ValueType::String;
                options.Value.assign(reinterpret_cast<const char*>(block.Data + offset), length);
                ValueScan(process, block, options, offset, length, "string");
            }
        }

        struct Pages
        {
            static constexpr std::size_t kPage = 4096;
            std::uint8_t* Data = nullptr;

            Pages() { Data = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 3 * kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)); }
            ~Pages()
            {
                if (Data)
                    ::VirtualFree(Data, 0, MEM_RELEASE);
            }
            Pages(const Pages&) = delete;
            Pages& operator=(const Pages&) = delete;

            std::uint64_t At(std::size_t offset) const { return reinterpret_cast<std::uint64_t>(Data) + offset; }
            bool Middle(DWORD protect)
            {
                DWORD old = 0;
                return ::VirtualProtect(Data + kPage, kPage, protect, &old) != FALSE;
            }
        };

        std::uint64_t NextCount(const std::shared_ptr<Target::Process>& process, Scanner& scanner, ScanOptions options, Compare mode)
        {
            options.Mode = mode;
            return scanner.Start(process, options, false) && Wait(scanner) ? scanner.Count() : ~0ull;
        }

        void UnreadablePages(const std::shared_ptr<Target::Process>& process)
        {
            Pages pages;
            CHECK(pages.Data != nullptr);
            if (!pages.Data)
                return;
            const std::int32_t value = 1234;
            for (std::size_t page = 0; page < 3; ++page)
                std::memcpy(pages.Data + page * Pages::kPage + 0x100, &value, sizeof(value));
            ScanOptions options;
            options.Type = ValueType::FourBytes;
            options.Value = "1234";
            options.Start = pages.At(0);
            options.Stop = pages.At(3 * Pages::kPage);
            options.Alignment = 4;
            Scanner changed;
            Scanner unchanged;
            const bool first = changed.Start(process, options, true) && Wait(changed) && unchanged.Start(process, options, true) && Wait(unchanged);
            CHECK_MSG(first && changed.Count() == 3 && unchanged.Count() == 3, Format("exact 1234 over three pages: %llu results", static_cast<unsigned long long>(changed.Count())));
            CHECK(pages.Middle(PAGE_NOACCESS));
            const std::uint64_t kept = NextCount(process, changed, options, Compare::Changed);
            ScanRecord record;
            const bool middle = kept == 1 && changed.Record(0, record) && record.Address == pages.At(Pages::kPage + 0x100);
            CHECK_MSG(kept == 0, Format("next scan Changed with the middle page unreadable: %llu kept%s", static_cast<unsigned long long>(kept), middle ? ", the one on the middle page" : ""));
            const std::uint64_t same = NextCount(process, unchanged, options, Compare::Unchanged);
            CHECK_MSG(same == 2, Format("next scan Unchanged with the middle page unreadable: %llu kept", static_cast<unsigned long long>(same)));
            CHECK(pages.Middle(PAGE_READWRITE));

            std::memset(pages.Data, 0x11, 3 * Pages::kPage);
            options.Mode = Compare::Unknown;
            options.Value.clear();
            const bool snapshot = changed.Start(process, options, true) && Wait(changed) && unchanged.Start(process, options, true) && Wait(unchanged);
            CHECK_MSG(snapshot && changed.IsSnapshot() && unchanged.IsSnapshot(), "unknown initial value over three pages");
            CHECK(pages.Middle(PAGE_NOACCESS));
            const std::uint64_t snapshotKept = NextCount(process, changed, options, Compare::Changed);
            CHECK_MSG(snapshotKept == 0, Format("next scan Changed from a snapshot with the middle page unreadable: %llu kept", static_cast<unsigned long long>(snapshotKept)));
            const std::uint64_t snapshotSame = NextCount(process, unchanged, options, Compare::Unchanged);
            CHECK_MSG(snapshotSame == 2 * Pages::kPage / 4, Format("next scan Unchanged from a snapshot with the middle page unreadable: %llu kept", static_cast<unsigned long long>(snapshotSame)));
            CHECK(pages.Middle(PAGE_READWRITE));
        }

        // Changed, Unchanged and Same as first after an eight-byte array scan compare all eight bytes, not four.
        void ByteArrayNextScanWidth(const std::shared_ptr<Target::Process>& process)
        {
            Pages pages;
            CHECK(pages.Data != nullptr);
            if (!pages.Data)
                return;
            const std::uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            ScanOptions options;
            options.Type = ValueType::ByteArray;
            options.Value = "01 02 03 04 05 06 07 08";
            options.Start = pages.At(0);
            options.Stop = pages.At(3 * Pages::kPage);
            const std::pair<Compare, std::uint64_t> cases[] = { { Compare::Unchanged, 0 }, { Compare::Changed, 1 }, { Compare::SameAsFirst, 0 } };
            for (const auto& [mode, expected] : cases)
            {
                std::memcpy(pages.Data + 0x100, bytes, sizeof(bytes));
                Scanner scanner;
                options.Mode = Compare::Exact;
                const bool first = scanner.Start(process, options, true) && Wait(scanner);
                CHECK_MSG(first && scanner.Count() == 1, Format("exact eight-byte array: %llu results", static_cast<unsigned long long>(scanner.Count())));
                pages.Data[0x107] = 9;
                const std::uint64_t kept = NextCount(process, scanner, options, mode);
                CHECK_MSG(kept == expected, Format("next scan %s after byte eight changed: %llu kept, %llu expected", CompareName(mode), static_cast<unsigned long long>(kept),
                                                   static_cast<unsigned long long>(expected)));
            }
        }

        int RunScanBufferTests()
        {
            const int before = Totals().Failures;
            auto opened = Target::Process::Open(MakeProcessId(::GetCurrentProcessId()));
            CHECK_MSG(opened.Ok(), opened.Error().Text);
            if (!opened.Ok())
                return Totals().Failures - before;
            const std::shared_ptr<Target::Process> process = *opened;
            PatternReads(*process);
            ValueReads(process);
            UnreadablePages(process);
            ByteArrayNextScanWidth(process);
            return Totals().Failures - before;
        }
    }

    MANIFOLD_SUITE("scanbuffers", "Scan read buffers fit long patterns", 135, [](const SuiteContext&) { return RunScanBufferTests(); });
}
