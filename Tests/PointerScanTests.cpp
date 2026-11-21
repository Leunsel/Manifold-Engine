#include "TestKit.h"

#include "Core/Paths.h"
#include "Core/Text.h"
#include "Scan/PointerScan.h"
#include "Target/Process.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        class Image final : public Scan::PointerSource
        {
        public:
            explicit Image(int pointerSize) : m_pointerSize(pointerSize) {}

            int AddModule(const std::string& name, std::uint64_t base, std::uint64_t size)
            {
                m_modules.push_back({ name, base, size });
                return static_cast<int>(m_modules.size()) - 1;
            }

            void AddBlock(std::uint64_t base, std::uint64_t size, int module, bool sparse = false)
            {
                Block block;
                block.Base = base;
                block.Size = size;
                block.Module = module;
                if (!sparse)
                    block.Bytes.assign(static_cast<std::size_t>(size), 0);
                m_blocks.push_back(std::move(block));
            }

            void Raw(std::uint64_t address, const void* data, std::size_t size)
            {
                for (Block& block : m_blocks)
                {
                    if (address < block.Base || address + size > block.Base + block.Size)
                        continue;
                    if (block.Bytes.empty())
                    {
                        const auto* bytes = static_cast<const std::uint8_t*>(data);
                        for (std::size_t index = 0; index < size; ++index)
                            block.Sparse[address + index] = bytes[index];
                    }
                    else
                    {
                        std::memcpy(block.Bytes.data() + (address - block.Base), data, size);
                    }
                    return;
                }
                CHECK_MSG(false, "the image has no memory at " + Hex(address));
            }

            void Point(std::uint64_t address, std::uint64_t value)
            {
                Raw(address, &value, static_cast<std::size_t>(m_pointerSize));
            }

            int PointerSize() const override { return m_pointerSize; }
            std::vector<Scan::PointerModule> Modules() const override { return m_modules; }

            std::vector<Scan::PointerRegion> Regions() const override
            {
                std::vector<Scan::PointerRegion> regions;
                for (const Block& block : m_blocks)
                    regions.push_back({ block.Base, block.Size, block.Module });
                return regions;
            }

            std::size_t Read(std::uint64_t address, void* buffer, std::size_t size) const override
            {
                for (const Block& block : m_blocks)
                {
                    if (address < block.Base || address >= block.Base + block.Size)
                        continue;
                    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(size, block.Base + block.Size - address));
                    if (!block.Bytes.empty())
                    {
                        std::memcpy(buffer, block.Bytes.data() + (address - block.Base), take);
                        return take;
                    }
                    std::memset(buffer, 0, take);
                    for (auto at = block.Sparse.lower_bound(address); at != block.Sparse.end() && at->first < address + take; ++at)
                        static_cast<std::uint8_t*>(buffer)[at->first - address] = at->second;
                    return take;
                }
                return 0;
            }

        private:
            struct Block
            {
                std::uint64_t Base = 0;
                std::uint64_t Size = 0;
                int Module = -1;
                std::vector<std::uint8_t> Bytes;
                std::map<std::uint64_t, std::uint8_t> Sparse;
            };

            int m_pointerSize;
            std::vector<Scan::PointerModule> m_modules;
            std::vector<Block> m_blocks;
        };

        struct World
        {
            std::unique_ptr<Image> Memory;
            std::uint64_t Game = 0;
            std::uint64_t Engine = 0;
            std::uint64_t Heap = 0;
            std::uint64_t Other = 0;
            std::uint64_t Player = 0;
            std::uint64_t Target = 0;
        };

        World BuildWorld(int pointerSize, bool restarted)
        {
            World world;
            world.Memory = std::make_unique<Image>(pointerSize);
            Image& image = *world.Memory;
            const bool wide = pointerSize == 8;
            world.Game = wide ? (restarted ? 0x7FF6A0000000ull : 0x7FF600000000ull) : (restarted ? 0x00A00000ull : 0x00400000ull);
            world.Engine = wide ? (restarted ? 0x7FF7B0000000ull : 0x7FF700000000ull) : (restarted ? 0x11000000ull : 0x10000000ull);
            world.Heap = wide ? (restarted ? 0x2C000000000ull : 0x1A000000000ull) : (restarted ? 0x02800000ull : 0x02000000ull);
            world.Other = wide ? (restarted ? 0x2D000000000ull : 0x1B000000000ull) : (restarted ? 0x03800000ull : 0x03000000ull);
            const std::uint64_t shift = restarted ? 0x10000 : 0;
            const int game = image.AddModule("game.exe", world.Game, 0x10000);
            const int engine = image.AddModule("engine.dll", world.Engine, 0x10000);
            image.AddBlock(world.Game + 0x1000, 0x1000, game);
            image.AddBlock(world.Engine + 0x2000, 0x1000, engine);
            image.AddBlock(world.Heap, 0x40000, -1);
            image.AddBlock(world.Other, 0x40000, -1);

            const std::uint64_t heap = world.Heap + shift;
            const std::uint64_t worldObject = heap + 0x1000;
            const std::uint64_t manager = heap + 0x2000;
            const std::uint64_t player = heap + 0x3000;
            const std::uint64_t list = heap + 0xC000;
            const std::uint64_t first = world.Other + shift + 0x1000;
            const std::uint64_t second = world.Other + shift + 0x3000;
            world.Player = player;
            world.Target = player + 0x34;

            image.Point(world.Game + 0x1010, worldObject);
            image.Point(worldObject + 0x18, manager);
            image.Point(manager + 0x40, player);
            image.Point(world.Engine + 0x2008, player);
            image.Point(first + 0x10, restarted ? 0 : player);
            image.Point(first + 0x8, second);
            image.Point(second + 0x8, first);
            image.Point(world.Game + 0x1030, first);
            image.Point(world.Game + 0x1040, player + 1);
            image.Point(world.Game + 0x1050, player - 0x108);
            image.Point(world.Game + 0x1058, restarted ? heap + 0x20000 : player - 0x100);
            image.Point(world.Engine + 0x2020, list);
            image.Point(list + 0x10, player);
            image.Point(list + 0x20, restarted ? 0 : player);
            std::uint64_t link = player;
            std::uint64_t slot = heap + 0xA008;
            for (int index = 0; index < 6; ++index)
            {
                image.Point(slot, link);
                link = slot & ~0xFFFull;
                slot = link - 0x1000;
            }
            image.Point(world.Engine + 0x2010, heap + 0x5000);
            image.Raw(world.Game + 0x1021, &player, static_cast<std::size_t>(pointerSize));
            const std::uint32_t health = 1000;
            image.Raw(world.Target, &health, sizeof(health));
            const std::uint32_t noise[] = { 7, 0x3F800000, 0xFFFFFFFF, 123456 };
            image.Raw(heap + 0x3040, noise, sizeof(noise));
            return world;
        }

        std::multiset<std::string> Paths(const Scan::PointerResults& results)
        {
            std::multiset<std::string> paths;
            for (std::uint64_t index = 0; index < results.Count(); ++index)
            {
                Scan::PointerPath path;
                if (results.Read(index, path))
                    paths.insert(results.PathText(path));
            }
            return paths;
        }

        std::string Listed(const std::multiset<std::string>& paths)
        {
            std::string text;
            for (const std::string& path : paths)
                text += path + "  ";
            return text;
        }

        std::string Path(const char* module, std::uint64_t offset, std::initializer_list<std::uint32_t> offsets)
        {
            std::string text = "\"" + std::string(module) + "\"+" + Hex(offset);
            for (const std::uint32_t each : offsets)
                text = "[" + text + "]+" + Hex(each);
            return text;
        }

        std::multiset<std::string> KnownPaths(bool deep, bool unaligned)
        {
            std::multiset<std::string> paths = {
                Path("engine.dll", 0x2008, { 0x34 }),
                Path("game.exe", 0x1058, { 0x134 }),
                Path("game.exe", 0x1030, { 0x10, 0x34 }),
                Path("engine.dll", 0x2020, { 0x10, 0x34 }),
                Path("engine.dll", 0x2020, { 0x20, 0x34 }),
                Path("game.exe", 0x1010, { 0x18, 0x40, 0x34 }),
                Path("game.exe", 0x1030, { 0x8, 0x8, 0x10, 0x34 }),
            };
            if (deep)
                paths.insert(Path("engine.dll", 0x2010, { 0, 0, 0, 0, 0, 0x8, 0x34 }));
            if (unaligned)
                paths.insert(Path("game.exe", 0x1040, { 0x33 }));
            return paths;
        }

        Scan::PointerScanOptions Options(const World& world, int level)
        {
            Scan::PointerScanOptions options;
            options.Target = world.Target;
            options.MaxLevel = level;
            options.MaxOffset = 0x134;
            return options;
        }

        void KnownChains(int pointerSize)
        {
            const std::string label = pointerSize == 8 ? "64-bit image" : "32-bit image";
            const World world = BuildWorld(pointerSize, false);
            for (const int threads : { 1, 4 })
            {
                Scan::PointerScanOptions options = Options(world, 5);
                options.Threads = threads;
                Scan::PointerResults results;
                Scan::PointerScanStats stats;
                Scan::PointerProgress progress;
                const Status status = Scan::ScanPointers(*world.Memory, options, results, &progress, &stats);
                CHECK_MSG(status.Ok(), status.Text);
                const std::multiset<std::string> found = Paths(results);
                CHECK_MSG(found == KnownPaths(false, false), label + ": " + Listed(found));
                CHECK_EQ(results.PointerSize(), pointerSize);
                CHECK_EQ(results.Levels(), 5);
                CHECK_EQ(stats.Results, results.Count());
                CHECK(!stats.Truncated && !stats.Cancelled && !stats.TargetOutside);
                CHECK(stats.Pointers >= 18 && stats.Pointers < 64);
                CHECK_EQ(stats.ScannedBytes, 0x82000ull);
                CHECK_EQ(progress.Results.load(), results.Count());
            }

            Scan::PointerResults deep;
            CHECK(Scan::ScanPointers(*world.Memory, Options(world, 7), deep).Ok());
            const std::multiset<std::string> deepPaths = Paths(deep);
            CHECK_MSG(deepPaths == KnownPaths(true, false), label + ", seven levels: " + Listed(deepPaths));

            Scan::PointerScanOptions loose = Options(world, 5);
            loose.Aligned = false;
            Scan::PointerResults unaligned;
            CHECK(Scan::ScanPointers(*world.Memory, loose, unaligned).Ok());
            const std::multiset<std::string> loosePaths = Paths(unaligned);
            CHECK_MSG(loosePaths == KnownPaths(false, true), label + ", any offset: " + Listed(loosePaths));

            Scan::PointerScanOptions shallow = Options(world, 1);
            Scan::PointerResults direct;
            CHECK(Scan::ScanPointers(*world.Memory, shallow, direct).Ok());
            CHECK(Paths(direct) == (std::multiset<std::string>{ Path("engine.dll", 0x2008, { 0x34 }), Path("game.exe", 0x1058, { 0x134 }) }));

            Scan::PointerScanOptions narrow = Options(world, 5);
            narrow.MaxOffset = 0x40;
            Scan::PointerResults close;
            CHECK(Scan::ScanPointers(*world.Memory, narrow, close).Ok());
            const std::multiset<std::string> closePaths = Paths(close);
            CHECK_MSG(closePaths == (std::multiset<std::string>{ Path("engine.dll", 0x2008, { 0x34 }), Path("game.exe", 0x1030, { 0x10, 0x34 }), Path("engine.dll", 0x2020, { 0x10, 0x34 }),
                                         Path("engine.dll", 0x2020, { 0x20, 0x34 }), Path("game.exe", 0x1010, { 0x18, 0x40, 0x34 }), Path("game.exe", 0x1030, { 0x8, 0x8, 0x10, 0x34 }) }),
                label + ", offsets up to 40: " + Listed(closePaths));

            Scan::PointerScanOptions outside = Options(world, 5);
            outside.Target = world.Heap + 0x900000;
            Scan::PointerResults none;
            Scan::PointerScanStats stats;
            CHECK(Scan::ScanPointers(*world.Memory, outside, none, nullptr, &stats).Ok());
            CHECK(none.Count() == 0 && stats.TargetOutside);

            for (std::uint64_t index = 0; index < deep.Count(); ++index)
            {
                Scan::PointerPath path;
                CHECK(deep.Read(index, path));
                const std::uint64_t base = path.Module == 0 ? world.Game : world.Engine;
                const auto resolved = Scan::ResolvePointerPath(base, path, [&](std::uint64_t address) -> std::optional<std::uint64_t>
                    {
                        std::uint64_t value = 0;
                        if (world.Memory->Read(address, &value, static_cast<std::size_t>(pointerSize)) != static_cast<std::size_t>(pointerSize))
                            return std::nullopt;
                        return value;
                    });
                CHECK_MSG(resolved && *resolved == world.Target, deep.PathText(path));
            }
        }

        void RescanAfterRestart(int pointerSize)
        {
            const std::string label = pointerSize == 8 ? "64-bit rescan" : "32-bit rescan";
            const World before = BuildWorld(pointerSize, false);
            const World after = BuildWorld(pointerSize, true);
            Scan::PointerResults results;
            CHECK(Scan::ScanPointers(*before.Memory, Options(before, 5), results).Ok());
            CHECK_EQ(results.Count(), 7u);

            const std::multiset<std::string> survivors = { Path("engine.dll", 0x2008, { 0x34 }), Path("engine.dll", 0x2020, { 0x10, 0x34 }), Path("game.exe", 0x1010, { 0x18, 0x40, 0x34 }) };
            Scan::PointerRescan byAddress;
            byAddress.Address = after.Target;
            Scan::PointerResults kept;
            Scan::PointerProgress progress;
            CHECK(Scan::RescanPointers(*after.Memory, results, byAddress, kept, &progress).Ok());
            const std::multiset<std::string> keptPaths = Paths(kept);
            CHECK_MSG(keptPaths == survivors, label + " by address: " + Listed(keptPaths));
            CHECK_EQ(kept.Target(), after.Target);
            CHECK_EQ(progress.Done.load(), 7u);
            CHECK_EQ(progress.Results.load(), 3u);

            Scan::PointerRescan byValue;
            const std::uint32_t health = 1000;
            byValue.Value.assign(reinterpret_cast<const std::uint8_t*>(&health), reinterpret_cast<const std::uint8_t*>(&health) + 4);
            Scan::PointerResults valued;
            CHECK(Scan::RescanPointers(*after.Memory, results, byValue, valued).Ok());
            const std::multiset<std::string> valuedPaths = Paths(valued);
            CHECK_MSG(valuedPaths == survivors, label + " by value: " + Listed(valuedPaths));
            CHECK_EQ(valued.Target(), 0u);

            Scan::PointerRescan same;
            same.Address = before.Target;
            Scan::PointerResults unchanged;
            CHECK(Scan::RescanPointers(*before.Memory, results, same, unchanged).Ok());
            CHECK(Paths(unchanged) == KnownPaths(false, false));

            Scan::PointerRescan wrong;
            wrong.Address = after.Target + 4;
            Scan::PointerResults empty;
            CHECK(Scan::RescanPointers(*after.Memory, results, wrong, empty).Ok());
            CHECK_EQ(empty.Count(), 0u);

            Image unloaded(pointerSize);
            unloaded.AddModule("engine.dll", after.Engine, 0x10000);
            Scan::PointerResults partial;
            CHECK(Scan::RescanPointers(unloaded, results, same, partial).Ok());
            CHECK_EQ(partial.Count(), 0u);
        }

        void RescanByRoundedValue(int pointerSize)
        {
            const std::string label = pointerSize == 8 ? "64-bit rounded rescan" : "32-bit rounded rescan";
            const World before = BuildWorld(pointerSize, false);
            World after = BuildWorld(pointerSize, true);
            Scan::PointerResults results;
            CHECK(Scan::ScanPointers(*before.Memory, Options(before, 5), results).Ok());
            const std::multiset<std::string> survivors = { Path("engine.dll", 0x2008, { 0x34 }), Path("engine.dll", 0x2020, { 0x10, 0x34 }), Path("game.exe", 0x1010, { 0x18, 0x40, 0x34 }) };
            const auto rescanFor = [&](Scan::ValueType type, double typed, double step)
                {
                    Scan::PointerRescan rescan;
                    rescan.Type = type;
                    rescan.Step = step;
                    if (type == Scan::ValueType::Float)
                    {
                        const float value = static_cast<float>(typed);
                        rescan.Value.assign(reinterpret_cast<const std::uint8_t*>(&value), reinterpret_cast<const std::uint8_t*>(&value) + sizeof(value));
                    }
                    else
                    {
                        rescan.Value.assign(reinterpret_cast<const std::uint8_t*>(&typed), reinterpret_cast<const std::uint8_t*>(&typed) + sizeof(typed));
                    }
                    Scan::PointerResults kept;
                    CHECK(Scan::RescanPointers(*after.Memory, results, rescan, kept).Ok());
                    CHECK_EQ(kept.Target(), 0u);
                    return Paths(kept);
                };

            // The window shows this Float as 123.457 (%.6g).
            const float health = 123.456789f;
            after.Memory->Raw(after.Target, &health, sizeof(health));
            const std::multiset<std::string> shown = rescanFor(Scan::ValueType::Float, 123.457, 0.001);
            CHECK_MSG(shown == survivors, label + ", Float as shown: " + Listed(shown));
            CHECK_MSG(rescanFor(Scan::ValueType::Float, 123.0, 1.0) == survivors, label + ", Float typed without decimals");
            CHECK_EQ(rescanFor(Scan::ValueType::Float, 123.457, 0.0).size(), 0u);
            CHECK_EQ(rescanFor(Scan::ValueType::Float, 123.4, 0.1).size(), 0u);
            CHECK_EQ(rescanFor(Scan::ValueType::Float, 124.0, 0.001).size(), 0u);

            const double distance = 98765.4321098765;
            after.Memory->Raw(after.Target, &distance, sizeof(distance));
            const std::multiset<std::string> wide = rescanFor(Scan::ValueType::Double, 98765.43211, 0.00001);
            CHECK_MSG(wide == survivors, label + ", Double as shown: " + Listed(wide));
            CHECK_EQ(rescanFor(Scan::ValueType::Double, 98765.5, 0.1).size(), 0u);

            const float broken = std::numeric_limits<float>::quiet_NaN();
            after.Memory->Raw(after.Target, &broken, sizeof(broken));
            CHECK_EQ(rescanFor(Scan::ValueType::Float, std::numeric_limits<double>::quiet_NaN(), 1.0).size(), 0u);
            // 123 matched the three paths above while the target held a number; a NaN there matches nothing.
            CHECK_EQ(rescanFor(Scan::ValueType::Float, 123.0, 1.0).size(), 0u);
        }

        World BuildWide(int pointerSize, int slots)
        {
            World world;
            world.Memory = std::make_unique<Image>(pointerSize);
            Image& image = *world.Memory;
            world.Game = pointerSize == 8 ? 0x7FF600000000ull : 0x00400000ull;
            world.Heap = pointerSize == 8 ? 0x1A000000000ull : 0x02000000ull;
            const int game = image.AddModule("game.exe", world.Game, 0x100000);
            image.AddBlock(world.Game + 0x1000, static_cast<std::uint64_t>(slots) * 8, game);
            image.AddBlock(world.Heap, 0x10000, -1);
            world.Player = world.Heap + 0x3000;
            world.Target = world.Player + 0x34;
            for (int index = 0; index < slots; ++index)
                image.Point(world.Game + 0x1000 + static_cast<std::uint64_t>(index) * 8, world.Player + static_cast<std::uint64_t>(index % 4) * 8);
            return world;
        }

        void SpillSaveSort()
        {
            constexpr int kSlots = 3000;
            const World world = BuildWide(8, kSlots);
            Scan::PointerScanOptions options = Options(world, 3);
            Scan::PointerResults whole;
            CHECK(Scan::ScanPointers(*world.Memory, options, whole).Ok());
            CHECK_EQ(whole.Count(), static_cast<std::uint64_t>(kSlots));
            CHECK_EQ(whole.SpilledBytes(), 0u);

            options.MemoryLimit = 16 * 1024;
            Scan::PointerResults spilled;
            Scan::PointerScanStats stats;
            CHECK(Scan::ScanPointers(*world.Memory, options, spilled, nullptr, &stats).Ok());
            CHECK_EQ(spilled.Count(), static_cast<std::uint64_t>(kSlots));
            CHECK(spilled.SpilledBytes() > 0);
            CHECK_EQ(spilled.SpilledBytes() + (16 * 1024 / spilled.RecordSize()) * spilled.RecordSize(), kSlots * spilled.RecordSize());
            const std::multiset<std::string> all = Paths(whole);
            CHECK(Paths(spilled) == all);
            CHECK_EQ(all.size(), static_cast<std::size_t>(kSlots));
            CHECK_EQ(std::set<std::string>(all.begin(), all.end()).size(), static_cast<std::size_t>(kSlots));
            Scan::PointerPath path;
            CHECK(spilled.Read(kSlots - 1, path) && spilled.Read(0, path) && spilled.Read(kSlots / 2, path));
            CHECK(!spilled.Read(kSlots, path));

            std::error_code ignored;
            const std::filesystem::path file = Paths::DataFolder() / L"pointer-scan-test.mptr";
            const Status saved = spilled.Save(file);
            CHECK_MSG(saved.Ok(), saved.Text);
            Scan::PointerResults loaded;
            const Status read = loaded.Load(file, 32 * 1024);
            CHECK_MSG(read.Ok(), read.Text);
            CHECK(Paths(loaded) == all);
            CHECK_EQ(loaded.Target(), world.Target);
            CHECK_EQ(loaded.Levels(), 3);
            CHECK(loaded.SpilledBytes() > 0);
            CHECK(loaded.Modules() == spilled.Modules());

            const std::uintmax_t size = std::filesystem::file_size(file, ignored);
            std::filesystem::resize_file(file, size - 5, ignored);
            Scan::PointerResults cut;
            CHECK_EQ(cut.Load(file).Code, ErrorCode::PointerFileUnreadable);
            CHECK_EQ(cut.Count(), 0u);
            std::filesystem::resize_file(file, 3, ignored);
            CHECK_EQ(cut.Load(file).Code, ErrorCode::PointerFileUnreadable);
            std::filesystem::remove(file, ignored);
            CHECK_EQ(cut.Load(file).Code, ErrorCode::PointerFileUnreadable);

            Scan::PointerRescan rescan;
            rescan.Address = world.Target;
            rescan.MemoryLimit = 8 * 1024;
            Scan::PointerResults narrowed;
            CHECK(Scan::RescanPointers(*world.Memory, spilled, rescan, narrowed).Ok());
            CHECK_EQ(narrowed.Count(), static_cast<std::uint64_t>(kSlots));
            CHECK(narrowed.SpilledBytes() > 0);
            rescan.Address = world.Target + 8;
            CHECK(Scan::RescanPointers(*world.Memory, spilled, rescan, narrowed).Ok());
            CHECK_EQ(narrowed.Count(), 0u);

            const auto byOffset = Scan::SortPointerResults(spilled, Scan::PointerOrder::LastOffset);
            CHECK(byOffset && byOffset->size() == kSlots);
            const auto byBase = Scan::SortPointerResults(spilled, Scan::PointerOrder::Base);
            CHECK(byBase && byBase->size() == kSlots);
            if (byOffset && byBase)
            {
                bool offsetsRise = true;
                bool basesRise = true;
                Scan::PointerPath last;
                Scan::PointerPath each;
                for (std::size_t index = 1; index < byOffset->size(); ++index)
                {
                    spilled.Read((*byOffset)[index - 1], last);
                    spilled.Read((*byOffset)[index], each);
                    offsetsRise = offsetsRise && last.Offsets[0] <= each.Offsets[0];
                    spilled.Read((*byBase)[index - 1], last);
                    spilled.Read((*byBase)[index], each);
                    basesRise = basesRise && last.ModuleOffset < each.ModuleOffset;
                }
                CHECK(offsetsRise);
                CHECK(basesRise);
                std::vector<std::uint32_t> seen = *byBase;
                std::sort(seen.begin(), seen.end());
                CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end() && seen.front() == 0 && seen.back() == kSlots - 1);
            }

            const World mixed = BuildWorld(8, false);
            Scan::PointerResults paths;
            CHECK(Scan::ScanPointers(*mixed.Memory, Options(mixed, 7), paths).Ok());
            const auto byLevel = Scan::SortPointerResults(paths, Scan::PointerOrder::Level);
            CHECK(byLevel && byLevel->size() == 8);
            if (byLevel && byLevel->size() == 8)
            {
                std::vector<std::string> ordered;
                for (const std::uint32_t index : *byLevel)
                {
                    paths.Read(index, path);
                    ordered.push_back(paths.PathText(path));
                }
                CHECK(ordered == (std::vector<std::string>{ Path("engine.dll", 0x2008, { 0x34 }), Path("game.exe", 0x1058, { 0x134 }), Path("game.exe", 0x1030, { 0x10, 0x34 }),
                                     Path("engine.dll", 0x2020, { 0x10, 0x34 }), Path("engine.dll", 0x2020, { 0x20, 0x34 }), Path("game.exe", 0x1010, { 0x18, 0x40, 0x34 }),
                                     Path("game.exe", 0x1030, { 0x8, 0x8, 0x10, 0x34 }), Path("engine.dll", 0x2010, { 0, 0, 0, 0, 0, 0x8, 0x34 }) }));
            }

            options = Options(world, 3);
            options.MaxResults = 100;
            Scan::PointerResults limited;
            CHECK(Scan::ScanPointers(*world.Memory, options, limited, nullptr, &stats).Ok());
            CHECK_EQ(limited.Count(), 100u);
            CHECK(stats.Truncated);
        }

        void Cancelled()
        {
            const World world = BuildWide(8, 3000);
            Scan::PointerProgress progress;
            progress.Cancel = true;
            Scan::PointerResults results;
            Scan::PointerScanStats stats;
            CHECK(Scan::ScanPointers(*world.Memory, Options(world, 3), results, &progress, &stats).Ok());
            CHECK_EQ(results.Count(), 0u);
            CHECK(stats.Cancelled);

            Scan::PointerMap map;
            CHECK(map.Build(*world.Memory, true).Ok());
            CHECK_EQ(map.Count(), 3000u);
            CHECK(map.MemoryBytes() >= 3000 * 8 && map.MemoryBytes() <= 4 * 1024 * 1024);
            Scan::PointerProgress later;
            later.Cancel = true;
            CHECK(map.Search(Options(world, 3), results, &later, &stats).Ok());
            CHECK(stats.Cancelled);
            CHECK_EQ(results.Count(), 0u);
            Scan::PointerProgress again;
            CHECK(map.Search(Options(world, 3), results, &again, &stats).Ok());
            CHECK_EQ(results.Count(), 3000u);
            CHECK(map.Search(Options(world, 3), results, &again, &stats).Ok());
            CHECK_EQ(results.Count(), 3000u);

            Scan::PointerResults full;
            CHECK(Scan::ScanPointers(*world.Memory, Options(world, 3), full).Ok());
            Scan::PointerProgress stopped;
            stopped.Cancel = true;
            Scan::PointerRescan rescan;
            rescan.Address = world.Target;
            Scan::PointerResults kept;
            CHECK(Scan::RescanPointers(*world.Memory, full, rescan, kept, &stopped).Ok());
            CHECK_EQ(kept.Count(), 0u);
        }

        void BeyondFourGigabytes()
        {
            Image image(8);
            const std::uint64_t game = 0x7FF600000000ull;
            const std::uint64_t large = 0x10000000000ull;
            const std::uint64_t largeSize = 0x108000000ull;
            const std::uint64_t second = 0x20000000000ull;
            const int module = image.AddModule("game.exe", game, 0x10000);
            image.AddBlock(game + 0x1000, 0x1000, module);
            image.AddBlock(large, largeSize, -1, true);
            image.AddBlock(second, 0x10000, -1);
            const std::uint64_t player = second + 0x3000;
            const std::uint64_t manager = large + 0x100000;
            const std::uint64_t distant = large + 0x107FF0000ull;
            const std::uint64_t edge = large + 0xFFFFF000ull;
            image.Point(game + 0x1010, distant);
            image.Point(distant + 0x20, manager);
            image.Point(manager + 0x8, player);
            image.Point(game + 0x1018, edge);
            image.Point(edge + 0x10, distant + 0x100);
            image.Point(distant + 0x108, player);
            Scan::PointerScanOptions options;
            options.Target = player + 0x34;
            options.MaxLevel = 4;
            options.MaxOffset = 0x200;
            Scan::PointerResults results;
            Scan::PointerScanStats stats;
            const Status status = Scan::ScanPointers(image, options, results, nullptr, &stats);
            CHECK_MSG(status.Ok(), status.Text);
            CHECK_EQ(stats.Pointers, 6u);
            CHECK(stats.ScannedBytes > 0x100000000ull);
            const std::multiset<std::string> found = Paths(results);
            CHECK_MSG(found == (std::multiset<std::string>{ Path("game.exe", 0x1010, { 0x20, 0x8, 0x34 }), Path("game.exe", 0x1010, { 0x108, 0x34 }), Path("game.exe", 0x1018, { 0x10, 0x8, 0x34 }) }),
                "beyond 4 GB: " + Listed(found));
        }

        void ParallelMatchesSerial()
        {
            Image image(8);
            const std::uint64_t game = 0x7FF600000000ull;
            const std::uint64_t heap = 0x1A000000000ull;
            const std::uint64_t heapSize = 0x200000;
            const int module = image.AddModule("game.exe", game, 0x10000);
            image.AddBlock(game + 0x1000, 0x1000, module);
            image.AddBlock(heap, heapSize, -1);
            std::uint64_t seed = 0x2545F4914F6CDD1Dull;
            const auto next = [&seed]()
                {
                    seed ^= seed << 13;
                    seed ^= seed >> 7;
                    seed ^= seed << 17;
                    return seed;
                };
            for (std::uint64_t slot = 0; slot < heapSize; slot += 8)
            {
                const std::uint64_t roll = next();
                if (roll % 2)
                    image.Point(heap + slot, heap + ((roll >> 8) % heapSize & ~7ull));
            }
            for (std::uint64_t slot = 0; slot < 0x1000; slot += 8)
                image.Point(game + 0x1000 + slot, heap + ((next() >> 8) % heapSize & ~7ull));
            Scan::PointerScanOptions options;
            options.Target = heap + 0x100034;
            options.MaxLevel = 5;
            options.MaxOffset = 0x100;
            options.Threads = 1;
            Scan::PointerResults serial;
            Scan::PointerScanStats serialStats;
            CHECK(Scan::ScanPointers(image, options, serial, nullptr, &serialStats).Ok());
            options.Threads = 6;
            Scan::PointerResults parallel;
            Scan::PointerScanStats parallelStats;
            CHECK(Scan::ScanPointers(image, options, parallel, nullptr, &parallelStats).Ok());
            CHECK(serialStats.Pointers > 100000 && serialStats.Nodes > 50000);
            CHECK_EQ(parallelStats.Pointers, serialStats.Pointers);
            CHECK_EQ(parallelStats.Nodes, serialStats.Nodes);
            CHECK(serial.Count() > 100);
            CHECK_EQ(parallel.Count(), serial.Count());
            CHECK(Paths(parallel) == Paths(serial));
            bool resolves = true;
            for (std::uint64_t index = 0; index < parallel.Count(); ++index)
            {
                Scan::PointerPath path;
                parallel.Read(index, path);
                const auto address = Scan::ResolvePointerPath(game, path, [&](std::uint64_t at) -> std::optional<std::uint64_t>
                    {
                        std::uint64_t value = 0;
                        if (image.Read(at, &value, 8) != 8)
                            return std::nullopt;
                        return value;
                    });
                resolves = resolves && address && *address == options.Target && path.Count <= 5;
            }
            CHECK(resolves);
        }

        struct Program
        {
            PROCESS_INFORMATION Info{};
            std::uint64_t World = 0;
            std::uint64_t Hero = 0;
            std::uint64_t Health = 0;

            Program() = default;
            Program(const Program&) = delete;
            Program& operator=(const Program&) = delete;

            ~Program()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }
        };

        bool Launch(const std::filesystem::path& path, Program& program)
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
            std::wstring command = L"\"" + path.wstring() + L"\"";
            const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &program.Info);
            ::CloseHandle(write);
            if (!started)
            {
                ::CloseHandle(read);
                return false;
            }
            std::string text;
            char buffer[512];
            DWORD got = 0;
            while (text.find("shoot at") == std::string::npos || text.find('\n', text.find("shoot at")) == std::string::npos)
            {
                if (!::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
                    break;
                text.append(buffer, got);
            }
            ::CloseHandle(read);
            const auto take = [&](const char* label) -> std::uint64_t
                {
                    const std::size_t at = text.find(label);
                    if (at == std::string::npos)
                        return 0;
                    const std::size_t start = at + std::strlen(label);
                    std::size_t end = start;
                    while (end < text.size() && IsHexDigit(text[end]))
                        ++end;
                    return ParseHex(text.substr(start, end - start)).value_or(0);
                };
            program.World = take("g_world at ");
            program.Hero = take("hero at ");
            program.Health = take("health at ");
            return program.World && program.Hero && program.Health;
        }

        void LiveTarget(const std::filesystem::path& path, int pointerSize)
        {
            const std::string name = WideToUtf8(path.filename().wstring());
            if (!std::filesystem::exists(path))
            {
                Skip(name + " is not beside the tests; the live pointer scan was not checked");
                return;
            }
            Program program;
            CHECK_MSG(Launch(path, program), name + " did not start");
            if (!program.Info.hProcess || !program.Health)
                return;
            auto opened = Target::Process::Open(MakeProcessId(program.Info.dwProcessId));
            CHECK(opened.Ok());
            if (!opened.Ok())
                return;
            const std::shared_ptr<Target::Process> process = *opened;
            process->RefreshModules();
            CHECK_EQ(process->PointerSize(), pointerSize);
            const auto module = process->FindModule(name);
            CHECK(module.has_value());
            if (!module)
                return;
            const std::string known = Path(name.c_str(), program.World - module->Base, { static_cast<std::uint32_t>(pointerSize), 0 });

            Scan::ProcessPointerSource source(process, {});
            Scan::PointerScanOptions options;
            options.Target = program.Health;
            options.MaxLevel = 3;
            Scan::PointerResults results;
            Scan::PointerScanStats stats;
            Scan::PointerProgress progress;
            const Status status = Scan::ScanPointers(source, options, results, &progress, &stats);
            CHECK_MSG(status.Ok(), status.Text);
            const std::multiset<std::string> found = Paths(results);
            CHECK_MSG(found.count(known) == 1, name + ": " + Grouped(found.size()) + " paths, " + known + " is not among them");
            CHECK(stats.Pointers > 0 && stats.ScannedBytes > 0 && !stats.TargetOutside);
            CHECK_EQ(progress.Done.load() > 0, true);
            std::printf("  %s: %s pointers in %s, %s paths, map %.0f ms, search %.0f ms\n", name.c_str(), Grouped(stats.Pointers).c_str(), ByteSize(stats.ScannedBytes).c_str(),
                Grouped(results.Count()).c_str(), stats.MapSeconds * 1000.0, stats.SearchSeconds * 1000.0);
            for (std::uint64_t index = 0; index < results.Count(); ++index)
            {
                Scan::PointerPath each;
                if (!results.Read(index, each) || results.PathText(each) != known)
                    continue;
                const auto resolved = Scan::ResolvePointerPath(module->Base, each, [&](std::uint64_t address) { return process->ReadPointer(address); });
                CHECK(resolved && *resolved == program.Health);
            }

            Scan::PointerRescan rescan;
            rescan.Address = program.Health;
            Scan::PointerResults kept;
            CHECK(Scan::RescanPointers(source, results, rescan, kept).Ok());
            CHECK_EQ(Paths(kept).count(known), 1u);
            CHECK(kept.Count() <= results.Count() && kept.Count() > 0);
            rescan.Address = program.Health + 4;
            Scan::PointerResults moved;
            CHECK(Scan::RescanPointers(source, results, rescan, moved).Ok());
            CHECK_EQ(Paths(moved).count(known), 0u);

            Scan::ProcessPointerSource everything(process, { true, true, true });
            Scan::PointerResults wide;
            CHECK(Scan::ScanPointers(everything, options, wide, nullptr, &stats).Ok());
            CHECK_EQ(Paths(wide).count(known), 1u);
            CHECK(wide.Count() >= results.Count());
        }

        std::string Argument(const std::vector<std::string>& arguments, const char* name)
        {
            const std::string prefix = std::string(name) + "=";
            for (const std::string& argument : arguments)
            {
                if (argument.rfind(prefix, 0) == 0)
                    return argument.substr(prefix.size());
            }
            return {};
        }

        bool Flag(const std::vector<std::string>& arguments, const char* name)
        {
            return std::find(arguments.begin(), arguments.end(), name) != arguments.end();
        }

        std::uint64_t PickTarget(const Target::Process& process, const Scan::ProcessPointerSource& source)
        {
            const auto main = process.MainModule();
            if (!main)
                return 0;
            const std::size_t pointer = static_cast<std::size_t>(process.PointerSize());
            const auto heap = [&](std::uint64_t value)
                {
                    const auto region = process.RegionAt(value);
                    return region && region->Committed() && region->Writable() && !region->Guarded() && region->Type == 0x20000;
                };
            std::vector<std::uint8_t> bytes(0x10000);
            for (const Scan::PointerRegion& region : source.Regions())
            {
                if (region.Base < main->Base || region.Base >= main->Base + main->Size)
                    continue;
                const std::size_t read = process.Read(region.Base, bytes.data(), static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), region.Size)));
                for (std::size_t at = 0; at + pointer <= read; at += pointer)
                {
                    std::uint64_t value = 0;
                    std::memcpy(&value, bytes.data() + at, pointer);
                    if (!heap(value))
                        continue;
                    for (std::uint64_t offset = 0; offset < 0x200; offset += pointer)
                    {
                        const auto next = process.ReadPointer(value + offset);
                        if (next && heap(*next) && (*next < value || *next > value + 0x1000))
                            return *next + 0x10;
                    }
                }
            }
            return 0;
        }

        int Benchmark(const std::vector<std::string>& arguments)
        {
            const std::string wanted = Argument(arguments, "--process");
            if (wanted.empty())
            {
                std::printf("--pointer-scan-bench --process=<name or id> [--address=<hex>] [--level=5] [--offset=1000] [--results=2000000] [--threads=0] [--readonly] [--mapped] [--system] [--any-offset]\n");
                return 1;
            }
            std::optional<ProcessId> id;
            if (std::all_of(wanted.begin(), wanted.end(), [](char c) { return c >= '0' && c <= '9'; }))
                id = MakeProcessId(static_cast<std::uint32_t>(std::stoul(wanted)));
            else
                id = Target::FindProcessId(wanted);
            if (!id)
            {
                std::printf("No process named %s runs.\n", wanted.c_str());
                return 1;
            }
            auto opened = Target::Process::Open(*id);
            if (!opened.Ok())
            {
                std::printf("%s\n", opened.Error().Text.c_str());
                return 1;
            }
            ::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
            const std::shared_ptr<Target::Process> process = *opened;
            process->RefreshModules();
            Scan::PointerMemory memory;
            memory.ReadOnly = Flag(arguments, "--readonly");
            memory.Mapped = Flag(arguments, "--mapped");
            memory.SystemModules = Flag(arguments, "--system");
            Scan::ProcessPointerSource source(process, memory);
            Scan::PointerScanOptions options;
            options.Target = ParseHex(Argument(arguments, "--address")).value_or(0);
            if (!options.Target)
                options.Target = PickTarget(*process, source);
            if (!options.Target)
            {
                std::printf("No address to scan for was found; pass --address=<hex>.\n");
                return 1;
            }
            if (const std::string level = Argument(arguments, "--level"); !level.empty())
                options.MaxLevel = std::atoi(level.c_str());
            if (const auto offset = ParseHex(Argument(arguments, "--offset")))
                options.MaxOffset = static_cast<std::uint32_t>(*offset);
            if (const std::string results = Argument(arguments, "--results"); !results.empty())
                options.MaxResults = std::strtoull(results.c_str(), nullptr, 10);
            if (const std::string threads = Argument(arguments, "--threads"); !threads.empty())
                options.Threads = std::atoi(threads.c_str());
            options.Aligned = !Flag(arguments, "--any-offset");

            std::printf("process          %s (%d-bit), id %u\n", process->Name().c_str(), process->PointerSize() * 8, Raw(*id));
            std::printf("address          %s, up to %d levels, offsets up to %s\n", Hex(options.Target).c_str(), options.MaxLevel, Hex(options.MaxOffset).c_str());
            Scan::PointerMap map;
            Scan::PointerProgress progress;
            auto started = std::chrono::steady_clock::now();
            Status status = map.Build(source, options.Aligned, options.Threads, &progress);
            const double mapSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (!status)
            {
                std::printf("%s\n", status.Text.c_str());
                return 1;
            }
            std::printf("memory read      %s\n", ByteSize(map.ScannedBytes()).c_str());
            std::printf("pointers         %s\n", Grouped(map.Count()).c_str());
            std::printf("map build        %.3f s (%.2f GB/s)\n", mapSeconds, mapSeconds > 0 ? static_cast<double>(map.ScannedBytes()) / mapSeconds / (1024.0 * 1024.0 * 1024.0) : 0.0);
            std::printf("map memory       %s\n", ByteSize(map.MemoryBytes()).c_str());
            Scan::PointerResults results;
            Scan::PointerScanStats stats;
            double searchSeconds = 0.0;
            for (int run = 0; run < 3; ++run)
            {
                started = std::chrono::steady_clock::now();
                status = map.Search(options, results, &progress, &stats);
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                searchSeconds = run == 0 ? seconds : std::min(searchSeconds, seconds);
                if (!status)
                {
                    std::printf("%s\n", status.Text.c_str());
                    return 1;
                }
            }
            std::printf("search           %.3f s (best of 3), %d levels, %s nodes\n", searchSeconds, stats.Levels, Grouped(stats.Nodes).c_str());
            std::printf("search memory    %s\n", ByteSize(stats.SearchBytes).c_str());
            std::printf("paths            %s%s%s\n", Grouped(results.Count()).c_str(), stats.Truncated ? " (stopped at the limit)" : "", stats.TargetOutside ? " (the address is not in the memory read)" : "");
            std::printf("paths held       %s in memory, %s in a file\n", ByteSize(results.MemoryBytes()).c_str(), ByteSize(results.SpilledBytes()).c_str());
            Scan::PointerRescan rescan;
            rescan.Address = options.Target;
            Scan::PointerResults kept;
            started = std::chrono::steady_clock::now();
            status = Scan::RescanPointers(source, results, rescan, kept, &progress);
            std::printf("rescan           %.3f s, %s of %s paths still lead to the address\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                Grouped(kept.Count()).c_str(), Grouped(results.Count()).c_str());
            PROCESS_MEMORY_COUNTERS counters{ sizeof(counters) };
            if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)))
                std::printf("peak memory      %s working set, %s committed\n", ByteSize(counters.PeakWorkingSetSize).c_str(), ByteSize(counters.PeakPagefileUsage).c_str());
            Scan::PointerPath path;
            for (std::uint64_t index = 0; index < std::min<std::uint64_t>(results.Count(), 5); ++index)
            {
                if (results.Read(index, path))
                    std::printf("  %s\n", results.PathText(path).c_str());
            }
            return 0;
        }
    }

    int RunPointerScanTests(const std::filesystem::path& target)
    {
        const int before = Totals().Failures;
        for (const int pointerSize : { 8, 4 })
        {
            KnownChains(pointerSize);
            RescanAfterRestart(pointerSize);
            RescanByRoundedValue(pointerSize);
        }
        SpillSaveSort();
        Cancelled();
        ParallelMatchesSerial();
        if constexpr (sizeof(void*) == 8)
            BeyondFourGigabytes();
        LiveTarget(target.parent_path() / L"TestTarget.exe", 8);
        LiveTarget(target.parent_path() / L"TestTarget32.exe", 4);
        return Totals().Failures - before;
    }

    MANIFOLD_SUITE("pointerscan", "Pointer scan", 26, [](const SuiteContext& context) { return RunPointerScanTests(context.Target); });
    MANIFOLD_MODE("--pointer-scan-bench", &Benchmark);
}
