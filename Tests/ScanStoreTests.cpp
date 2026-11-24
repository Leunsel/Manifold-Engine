#include "TestKit.h"

#include "Core/Text.h"
#include "Scan/ScanStore.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        using namespace Manifold::Scan;

        constexpr std::uint64_t kMb = 1024 * 1024;

        std::vector<std::uint8_t> Pattern(std::size_t size, std::uint32_t seed)
        {
            std::vector<std::uint8_t> bytes(size);
            std::uint32_t state = seed;
            for (std::uint8_t& byte : bytes)
            {
                state = state * 1664525u + 1013904223u;
                byte = static_cast<std::uint8_t>(state >> 24);
            }
            return bytes;
        }

        // Writes in odd pieces, then reads back at places that cross blocks and the RAM/file border.
        bool RoundTrip(ScanStore& store, const std::vector<std::uint8_t>& data)
        {
            std::size_t at = 0;
            std::size_t piece = 1;
            while (at < data.size())
            {
                const std::size_t take = std::min(piece, data.size() - at);
                if (!store.Append(data.data() + at, take))
                    return false;
                at += take;
                piece = piece * 7 % 3000001 + 13;  // 1 byte up to ~3 MB
            }
            if (!store.Flush() || store.Size() != data.size())
                return false;
            std::vector<std::uint8_t> back(data.size());
            if (!store.ReadAt(0, back.data(), back.size()) || back != data)
                return false;
            for (const std::uint64_t offset : { std::uint64_t{ 0 }, ScanStore::kBlock - 3, ScanStore::kBlock * 2 - 1, store.MemoryBytes() - 5, data.size() - 9 })
            {
                if (offset >= data.size())
                    continue;
                std::uint8_t window[9] = {};
                const std::size_t size = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(window), data.size() - offset));
                if (!store.ReadAt(offset, window, size) || std::memcmp(window, data.data() + offset, size) != 0)
                    return false;
            }
            std::uint8_t past = 0;
            return !store.ReadAt(data.size(), &past, 1);
        }

        void CheckStore()
        {
            const int before = MemoryBudget::Limit();
            const std::uint64_t used = MemoryBudget::Used();
            const std::vector<std::uint8_t> data = Pattern(40 * kMb + 12345, 7);

            MemoryBudget::SetLimit(MemoryBudget::kDiskOnly);
            {
                ScanStore store;
                CHECK(RoundTrip(store, data));
                CHECK(store.MemoryBytes() == 0 && store.DiskBytes() == data.size());
                CHECK(MemoryBudget::Used() == used);
            }

            MemoryBudget::SetLimit(static_cast<int>((used + 32 * kMb) / kMb));  // two blocks, the rest spills
            {
                ScanStore store;
                CHECK(RoundTrip(store, data));
                CHECK_MSG(store.MemoryBytes() == 32 * kMb && store.DiskBytes() == data.size() - 32 * kMb, Format("%llu in RAM", store.MemoryBytes()));
                CHECK(MemoryBudget::Used() == used + 32 * kMb);
                ScanStore second;  // the budget is shared: nothing left for another store
                const std::uint8_t byte = 1;
                CHECK(second.Append(&byte, 1) && second.MemoryBytes() == 0 && second.DiskBytes() == 1);
            }
            CHECK(MemoryBudget::Used() == used);  // given back when the stores went

            MemoryBudget::SetLimit(MemoryBudget::kAuto);
            {
                ScanStore store;
                CHECK(RoundTrip(store, data));
                CHECK_MSG(store.DiskBytes() == 0 && store.MemoryBytes() == data.size(), Format("%llu on disk", store.DiskBytes()));
            }
            CHECK(MemoryBudget::Used() == used);
            CHECK(MemoryBudget::AutoBytes() > 0 && MemoryBudget::LimitBytes() == MemoryBudget::AutoBytes());
            MemoryBudget::SetLimit(-5);
            CHECK(MemoryBudget::Limit() == MemoryBudget::kAuto);
            MemoryBudget::SetLimit(before);
        }

        bool Wait(Scanner& scanner)
        {
            for (int i = 0; i < 1200; ++i)
            {
                if (scanner.Poll())
                    return scanner.LastError().Ok();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        }

        struct Outcome
        {
            std::vector<std::uint64_t> Counts;
            std::uint64_t Hash = 1469598103934665603ull;
            ScanStorage Storage;
            bool Ok = true;
        };

        // Unknown, unchanged, exact and undo over a still buffer of this process, under one limit.
        Outcome ScanUnder(const std::shared_ptr<Target::Process>& process, std::uint64_t base, std::uint64_t size, int limitMb)
        {
            Outcome outcome;
            Scanner scanner;
            ScanOptions options;
            options.Type = ValueType::FourBytes;
            options.Mode = Compare::Unknown;
            options.Start = base;
            options.Stop = base + size;
            options.Alignment = 4;
            options.Threads = 4;
            options.MemoryLimitMb = limitMb;
            const auto step = [&](Compare mode, std::string value, bool first)
                {
                    options.Mode = mode;
                    options.Value = std::move(value);
                    const Status started = scanner.Start(process, options, first);
                    if (!started || !Wait(scanner))
                    {
                        outcome.Ok = false;
                        return;
                    }
                    outcome.Counts.push_back(scanner.Count());
                    ScanRecord record;
                    for (std::uint64_t i = 0; i < scanner.Count(); i += 997)
                    {
                        if (!scanner.Record(i, record))
                        {
                            outcome.Ok = false;
                            return;
                        }
                        for (const std::uint8_t byte : record.Previous)
                            outcome.Hash = (outcome.Hash ^ byte) * 1099511628211ull;
                        outcome.Hash = (outcome.Hash ^ record.Address) * 1099511628211ull;
                    }
                };
            step(Compare::Unknown, {}, true);
            // ∑ Measured here: later sets find the budget taken by this one, which
            //   stays for Undo, and go to disk under a small limit.
            outcome.Storage = scanner.Storage(true);
            step(Compare::Unchanged, {}, false);
            step(Compare::Unchanged, {}, false);
            step(Compare::Exact, "100", false);
            scanner.Undo();
            outcome.Counts.push_back(scanner.Count());
            return outcome;
        }

        void CheckScans()
        {
            const int before = MemoryBudget::Limit();
            const std::uint64_t size = 24 * kMb;
            auto* buffer = static_cast<std::uint32_t*>(::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            CHECK(buffer != nullptr);
            if (!buffer)
                return;
            std::uint32_t state = 99;
            for (std::uint64_t i = 0; i < size / 4; ++i)
            {
                state = state * 1664525u + 1013904223u;
                buffer[i] = i % 8 == 0 ? 100 : state;
            }
            Result<std::shared_ptr<Target::Process>> process = Target::Process::Open(MakeProcessId(::GetCurrentProcessId()));
            CHECK_MSG(process.Ok(), process.Error().Text);
            if (process)
            {
                const std::uint64_t base = reinterpret_cast<std::uint64_t>(buffer);
                const Outcome disk = ScanUnder(*process, base, size, MemoryBudget::kDiskOnly);
                const Outcome split = ScanUnder(*process, base, size, 48);  // the first results fit, the next ones spill
                const Outcome memory = ScanUnder(*process, base, size, MemoryBudget::kAuto);
                CHECK(disk.Ok && split.Ok && memory.Ok);
                CHECK_MSG(disk.Counts.size() == 5 && disk.Counts[0] == size / 4 && disk.Counts[1] == size / 4 && disk.Counts[3] == size / 32 && disk.Counts[4] == size / 4,
                    Format("%zu steps", disk.Counts.size()));
                CHECK(disk.Counts == split.Counts && disk.Counts == memory.Counts);
                CHECK(disk.Hash == split.Hash && disk.Hash == memory.Hash);
                CHECK(disk.Storage.MemoryBytes == 0 && disk.Storage.DiskBytes > 0);
                CHECK_MSG(split.Storage.MemoryBytes > 0 && split.Storage.MemoryBytes <= 48 * kMb && split.Storage.DiskBytes > 0,
                    Format("%llu in RAM, %llu on disk", split.Storage.MemoryBytes, split.Storage.DiskBytes));
                CHECK_MSG(memory.Storage.DiskBytes == 0 && memory.Storage.MemoryBytes > 0, Format("%llu on disk", memory.Storage.DiskBytes));
            }
            CHECK(MemoryBudget::Used() == 0);  // every scanner gave its blocks back
            ::VirtualFree(buffer, 0, MEM_RELEASE);
            MemoryBudget::SetLimit(before);
        }

        int RunScanStoreTests()
        {
            const int before = Totals().Failures;
            CheckStore();
            CheckScans();
            return Totals().Failures - before;
        }
    }

    MANIFOLD_SUITE("scanstore", "Scan results in RAM and on disk", 131, [](const SuiteContext&) { return RunScanStoreTests(); });
}
