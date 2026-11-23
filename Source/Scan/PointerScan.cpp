#include "Scan/PointerScan.h"

#include "Core/Paths.h"
#include "Core/Text.h"
#include "Scan/Workers.h"
#include "Target/Process.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>

#include <windows.h>

namespace Manifold::Scan
{
    namespace
    {
        constexpr std::size_t kChunkBytes = 2 * 1024 * 1024;
        constexpr std::size_t kSlabKeys = 32768;
        constexpr std::uint64_t kNoKey = ~0ull;
        constexpr int kMaxBuckets = 8;
        constexpr std::uint32_t kRootCode = 1;
        constexpr std::size_t kSweepBlock = 256;
        constexpr std::size_t kParallelFrontier = 2048;
        constexpr std::size_t kEmitRecords = 2048;
        constexpr std::size_t kRescanRecords = 4096;
        constexpr std::size_t kWindowRecords = 512;
        constexpr std::size_t kResultSlabBytes = 1024 * 1024;
        constexpr std::size_t kPageSize = 4096;
        constexpr std::size_t kCachedPages = 64;
        constexpr char kFileMagic[8] = { 'M', 'E', 'P', 'T', 'R', 'S', '0', '1' };
        constexpr std::uint32_t kMappedType = 0x40000;

        double Seconds(std::chrono::steady_clock::time_point since)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
        }

        void SortKeys(std::uint64_t* data, std::size_t count, int threads)
        {
            if (count < (1u << 16) || threads <= 1)
            {
                std::sort(data, data + count);
                return;
            }
            std::uint64_t highest = 0;
            for (std::size_t index = 0; index < count; ++index)
            {
                if (data[index] != kNoKey && data[index] > highest)
                    highest = data[index];
            }
            const int shift = std::max(0, static_cast<int>(std::bit_width(highest)) - 8);
            const auto slot = [shift](std::uint64_t key) { return static_cast<std::size_t>(std::min<std::uint64_t>(255, key >> shift)); };
            std::size_t start[257] = {};
            for (std::size_t index = 0; index < count; ++index)
                ++start[slot(data[index]) + 1];
            for (std::size_t index = 0; index < 256; ++index)
                start[index + 1] += start[index];
            std::size_t next[256];
            std::copy(start, start + 256, next);
            for (std::size_t bucket = 0; bucket < 256; ++bucket)
            {
                while (next[bucket] < start[bucket + 1])
                {
                    const std::size_t wanted = slot(data[next[bucket]]);
                    if (wanted == bucket)
                        ++next[bucket];
                    else
                        std::swap(data[next[bucket]], data[next[wanted]++]);
                }
            }
            std::atomic<std::size_t> claimed{ 0 };
            RunWorkers(threads, L"Pointer sort worker", [&](int)
                {
                    for (;;)
                    {
                        const std::size_t bucket = claimed.fetch_add(1);
                        if (bucket >= 256)
                            break;
                        std::sort(data + start[bucket], data + start[bucket + 1]);
                    }
                });
        }

        class KeyArena
        {
        public:
            KeyArena() = default;
            ~KeyArena() { Release(); }
            KeyArena(const KeyArena&) = delete;
            KeyArena& operator=(const KeyArena&) = delete;

            bool Reserve(std::uint64_t keys)
            {
                Release();
                keys = (keys / kSlabKeys + 2) * kSlabKeys;
                if (keys > (SIZE_MAX / 2) / sizeof(std::uint64_t))
                    return false;
                m_data = static_cast<std::uint64_t*>(::VirtualAlloc(nullptr, static_cast<std::size_t>(keys) * sizeof(std::uint64_t), MEM_RESERVE, PAGE_READWRITE));
                m_capacity = m_data ? static_cast<std::size_t>(keys) : 0;
                return m_data != nullptr;
            }

            void Release()
            {
                if (m_data)
                    ::VirtualFree(m_data, 0, MEM_RELEASE);
                m_data = nullptr;
                m_capacity = 0;
                m_cursor = 0;
                m_count = 0;
                m_peak = 0;
            }

            std::uint64_t* Claim()
            {
                const std::size_t at = m_cursor.fetch_add(kSlabKeys);
                if (at + kSlabKeys > m_capacity)
                    return nullptr;
                if (!::VirtualAlloc(m_data + at, kSlabKeys * sizeof(std::uint64_t), MEM_COMMIT, PAGE_READWRITE))
                    return nullptr;
                return m_data + at;
            }

            void Seal(int threads)
            {
                std::size_t used = std::min(m_cursor.load(), m_capacity);
                m_peak = std::max(m_peak, used);
                SortKeys(m_data, used, threads);
                m_count = static_cast<std::size_t>(std::lower_bound(m_data, m_data + used, kNoKey) - m_data);
            }

            void Reset()
            {
                m_cursor = 0;
                m_count = 0;
            }

            const std::uint64_t* Data() const { return m_data; }
            std::size_t Count() const { return m_count; }
            std::uint64_t Bytes() const { return static_cast<std::uint64_t>(m_peak) * sizeof(std::uint64_t); }

        private:
            std::uint64_t* m_data = nullptr;
            std::size_t m_capacity = 0;
            std::atomic<std::size_t> m_cursor{ 0 };
            std::size_t m_count = 0;
            std::size_t m_peak = 0;
        };

        struct KeyWriter
        {
            KeyArena* Arena = nullptr;
            std::uint64_t* At = nullptr;
            std::uint64_t* End = nullptr;

            bool Push(std::uint64_t key)
            {
                if (At == End)
                {
                    At = Arena ? Arena->Claim() : nullptr;
                    End = At ? At + kSlabKeys : nullptr;
                    if (!At)
                        return false;
                }
                *At++ = key;
                return true;
            }

            void Finish()
            {
                while (At != End)
                    *At++ = kNoKey;
            }
        };

        struct Range
        {
            std::uint64_t Base = 0;
            std::uint64_t End = 0;
            std::uint64_t Compact = 0;
            int Module = -1;

            std::uint64_t Size() const { return End - Base; }
        };

        struct Chunk
        {
            std::uint64_t Real = 0;
            std::uint64_t Compact = 0;
            std::size_t Size = 0;
        };

        class PageCache
        {
        public:
            explicit PageCache(const PointerSource& source) : m_source(source), m_data(kCachedPages * kPageSize)
            {
                std::fill(std::begin(m_tags), std::end(m_tags), ~0ull);
            }

            bool Read(std::uint64_t address, void* out, std::size_t size)
            {
                const std::uint64_t page = address / kPageSize;
                const std::size_t inside = static_cast<std::size_t>(address % kPageSize);
                if (inside + size > kPageSize)
                    return m_source.Read(address, out, size) == size;
                const std::size_t slot = static_cast<std::size_t>(page % kCachedPages);
                if (m_tags[slot] != page)
                {
                    m_tags[slot] = page;
                    m_valid[slot] = m_source.Read(page * kPageSize, m_data.data() + slot * kPageSize, kPageSize) == kPageSize;
                }
                if (!m_valid[slot])
                    return m_source.Read(address, out, size) == size;
                std::memcpy(out, m_data.data() + slot * kPageSize + inside, size);
                return true;
            }

            std::optional<std::uint64_t> Pointer(std::uint64_t address, int pointerSize)
            {
                std::uint64_t value = 0;
                if (!Read(address, &value, static_cast<std::size_t>(pointerSize)))
                    return std::nullopt;
                return value;
            }

        private:
            const PointerSource& m_source;
            std::vector<std::uint8_t> m_data;
            std::uint64_t m_tags[kCachedPages];
            bool m_valid[kCachedPages] = {};
        };

        template <class Value>
        void Put(std::vector<std::uint8_t>& out, Value value)
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(Value));
        }

        template <class Value>
        bool Take(std::istream& in, Value& value)
        {
            in.read(reinterpret_cast<char*>(&value), sizeof(Value));
            return static_cast<bool>(in);
        }

        std::uint32_t OffsetAt(const std::uint8_t* record, int index)
        {
            std::uint32_t value;
            std::memcpy(&value, record + 8 + 4 * index, 4);
            return value;
        }

        int ComparePaths(const std::uint8_t* a, const std::uint8_t* b, PointerOrder order)
        {
            const int countA = a[6];
            const int countB = b[6];
            const auto offsets = [&]() -> int
                {
                    for (int index = 0; index < std::min(countA, countB); ++index)
                    {
                        const std::uint32_t left = OffsetAt(a, index);
                        const std::uint32_t right = OffsetAt(b, index);
                        if (left != right)
                            return left < right ? -1 : 1;
                    }
                    return 0;
                };
            const auto base = [&]() -> int
                {
                    std::uint16_t moduleA;
                    std::uint16_t moduleB;
                    std::uint32_t offsetA;
                    std::uint32_t offsetB;
                    std::memcpy(&moduleA, a + 4, 2);
                    std::memcpy(&moduleB, b + 4, 2);
                    std::memcpy(&offsetA, a, 4);
                    std::memcpy(&offsetB, b, 4);
                    if (moduleA != moduleB)
                        return moduleA < moduleB ? -1 : 1;
                    if (offsetA != offsetB)
                        return offsetA < offsetB ? -1 : 1;
                    return 0;
                };
            const auto level = [&]() -> int { return countA == countB ? 0 : countA < countB ? -1 : 1; };
            if (order == PointerOrder::Base)
            {
                if (const int result = base())
                    return result;
                if (const int result = level())
                    return result;
                return offsets();
            }
            if (order == PointerOrder::LastOffset && countA && countB)
            {
                const std::uint32_t left = OffsetAt(a, countA - 1);
                const std::uint32_t right = OffsetAt(b, countB - 1);
                if (left != right)
                    return left < right ? -1 : 1;
            }
            if (const int result = level())
                return result;
            if (const int result = offsets())
                return result;
            return base();
        }
    }

    ProcessPointerSource::ProcessPointerSource(std::shared_ptr<const Target::Process> process, const PointerMemory& memory) : m_process(std::move(process)), m_memory(memory)
    {
        const Target::ModuleList modules = m_process->Modules();
        if (!modules)
            return;
        for (const Target::ModuleInfo& module : *modules)
        {
            Span span;
            span.Base = module.Base;
            span.End = module.Base + module.Size;
            span.Module = -2;
            if (!module.System || m_memory.SystemModules)
            {
                span.Module = static_cast<int>(m_modules.size());
                m_modules.push_back({ module.Name, module.Base, module.Size });
            }
            m_spans.push_back(span);
        }
        std::sort(m_spans.begin(), m_spans.end(), [](const Span& a, const Span& b) { return a.Base < b.Base; });
    }

    int ProcessPointerSource::PointerSize() const
    {
        return m_process->PointerSize();
    }

    std::vector<PointerRegion> ProcessPointerSource::Regions() const
    {
        std::vector<PointerRegion> regions;
        for (const Target::RegionInfo& region : m_process->Regions(m_process->LowestAddress(), m_process->HighestAddress()))
        {
            if (!region.Committed() || !region.Readable() || region.Guarded())
                continue;
            if ((m_memory.SkipNoCache && region.NoCache()) || (m_memory.SkipWriteCombine && region.WriteCombine()))
                continue;
            if (!m_memory.ReadOnly && !region.Writable())
                continue;
            if (region.Type == kMappedType && !m_memory.Mapped)
                continue;
            int module = -1;
            const auto after = std::upper_bound(m_spans.begin(), m_spans.end(), region.Base, [](std::uint64_t address, const Span& span) { return address < span.Base; });
            if (after != m_spans.begin() && region.Base < (after - 1)->End)
                module = (after - 1)->Module;
            if (module == -2)
                continue;
            regions.push_back({ region.Base, region.Size, module });
        }
        return regions;
    }

    std::size_t ProcessPointerSource::Read(std::uint64_t address, void* buffer, std::size_t size) const
    {
        return m_process->Read(address, buffer, size);
    }

    PointerResults::PointerResults() = default;

    PointerResults::~PointerResults()
    {
        if (m_file)
            ::CloseHandle(static_cast<HANDLE>(m_file));
    }

    void PointerResults::Begin(int pointerSize, int levels, std::vector<std::string> modules, std::uint64_t memoryLimit)
    {
        if (m_file)
            ::CloseHandle(static_cast<HANDLE>(m_file));
        m_file = nullptr;
        m_pointerSize = pointerSize == 4 ? 4 : 8;
        m_levels = std::clamp(levels, 1, kPointerMaxLevel);
        m_recordSize = 8 + 4 * static_cast<std::size_t>(m_levels);
        m_perSlab = std::max<std::size_t>(1, kResultSlabBytes / m_recordSize);
        m_memoryRecords = std::max<std::uint64_t>(1, memoryLimit / m_recordSize);
        m_modules = std::move(modules);
        m_slabs.clear();
        m_inMemory = 0;
        m_inFile = 0;
        m_count = 0;
        m_failed = false;
        m_target = 0;
        m_window.clear();
        m_windowCount = 0;
    }

    std::uint64_t PointerResults::MemoryBytes() const
    {
        return static_cast<std::uint64_t>(m_slabs.size()) * m_perSlab * m_recordSize;
    }

    std::uint64_t PointerResults::SpilledBytes() const
    {
        return m_inFile * m_recordSize;
    }

    bool PointerResults::Spill(const std::uint8_t* data, std::size_t size)
    {
        if (!m_file)
        {
            static std::atomic<unsigned> counter{ 0 };
            const std::filesystem::path path = Paths::ScanFolder() /
                (std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(counter.fetch_add(1)) + L"-" + std::to_wstring(::GetTickCount64()) + L".pointers");
            const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            m_file = file;
        }
        std::uint64_t at = m_inFile * m_recordSize;
        while (size)
        {
            OVERLAPPED where{};
            where.Offset = static_cast<DWORD>(at);
            where.OffsetHigh = static_cast<DWORD>(at >> 32);
            DWORD written = 0;
            const DWORD part = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 24));
            if (!::WriteFile(static_cast<HANDLE>(m_file), data, part, &written, &where) || written == 0)
                return false;
            data += written;
            size -= written;
            at += written;
        }
        return true;
    }

    bool PointerResults::Append(const std::uint8_t* records, std::size_t count)
    {
        if (count == 0)
            return !m_failed;
        std::lock_guard lock(m_lock);
        if (m_failed)
            return false;
        std::size_t done = 0;
        while (done < count && m_inMemory < m_memoryRecords)
        {
            const std::size_t inSlab = static_cast<std::size_t>(m_inMemory % m_perSlab);
            if (inSlab == 0 && m_inMemory / m_perSlab == m_slabs.size())
                m_slabs.push_back(std::make_unique_for_overwrite<std::uint8_t[]>(m_perSlab * m_recordSize));
            const std::size_t room = static_cast<std::size_t>(std::min<std::uint64_t>(m_perSlab - inSlab, m_memoryRecords - m_inMemory));
            const std::size_t take = std::min(room, count - done);
            std::memcpy(m_slabs.back().get() + inSlab * m_recordSize, records + done * m_recordSize, take * m_recordSize);
            m_inMemory += take;
            done += take;
        }
        if (done < count)
        {
            if (!Spill(records + done * m_recordSize, (count - done) * m_recordSize))
            {
                m_failed = true;
                m_count = m_inMemory + m_inFile;
                return false;
            }
            m_inFile += count - done;
        }
        m_count = m_inMemory + m_inFile;
        return true;
    }

    std::size_t PointerResults::ReadRaw(std::uint64_t index, std::size_t count, std::uint8_t* out) const
    {
        const std::uint64_t total = m_count.load();
        if (index >= total)
            return 0;
        count = static_cast<std::size_t>(std::min<std::uint64_t>(count, total - index));
        std::size_t done = 0;
        while (done < count && index + done < m_inMemory)
        {
            const std::uint64_t at = index + done;
            const std::size_t inSlab = static_cast<std::size_t>(at % m_perSlab);
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>({ m_perSlab - inSlab, count - done, m_inMemory - at }));
            std::memcpy(out + done * m_recordSize, m_slabs[static_cast<std::size_t>(at / m_perSlab)].get() + inSlab * m_recordSize, take * m_recordSize);
            done += take;
        }
        if (done < count && m_file)
        {
            std::uint64_t at = (index + done - m_inMemory) * m_recordSize;
            std::size_t left = (count - done) * m_recordSize;
            std::uint8_t* to = out + done * m_recordSize;
            while (left)
            {
                OVERLAPPED where{};
                where.Offset = static_cast<DWORD>(at);
                where.OffsetHigh = static_cast<DWORD>(at >> 32);
                DWORD got = 0;
                const DWORD part = static_cast<DWORD>(std::min<std::size_t>(left, 1u << 24));
                if (!::ReadFile(static_cast<HANDLE>(m_file), to, part, &got, &where) || got == 0)
                    break;
                to += got;
                left -= got;
                at += got;
            }
            done = count - (left + m_recordSize - 1) / m_recordSize;
        }
        return done;
    }

    void PointerResults::Decode(const std::uint8_t* record, PointerPath& out) const
    {
        std::memcpy(&out.ModuleOffset, record, 4);
        std::memcpy(&out.Module, record + 4, 2);
        out.Count = static_cast<std::uint8_t>(std::min<int>(record[6], m_levels));
        std::memcpy(out.Offsets, record + 8, 4 * static_cast<std::size_t>(out.Count));
    }

    void PointerResults::Encode(const PointerPath& path, std::uint8_t* record) const
    {
        std::memset(record, 0, m_recordSize);
        const int count = std::min<int>(path.Count, m_levels);
        std::memcpy(record, &path.ModuleOffset, 4);
        std::memcpy(record + 4, &path.Module, 2);
        record[6] = static_cast<std::uint8_t>(count);
        std::memcpy(record + 8, path.Offsets, 4 * static_cast<std::size_t>(count));
    }

    bool PointerResults::Read(std::uint64_t index, PointerPath& out) const
    {
        if (index >= m_count.load())
            return false;
        if (index < m_inMemory)
        {
            Decode(m_slabs[static_cast<std::size_t>(index / m_perSlab)].get() + static_cast<std::size_t>(index % m_perSlab) * m_recordSize, out);
            return true;
        }
        std::lock_guard lock(m_lock);
        if (index < m_windowFirst || index >= m_windowFirst + m_windowCount)
        {
            m_window.resize(kWindowRecords * m_recordSize);
            m_windowFirst = index - (index - m_inMemory) % kWindowRecords;
            m_windowCount = ReadRaw(m_windowFirst, kWindowRecords, m_window.data());
            if (index >= m_windowFirst + m_windowCount)
                return false;
        }
        Decode(m_window.data() + static_cast<std::size_t>(index - m_windowFirst) * m_recordSize, out);
        return true;
    }

    std::string PointerResults::BaseText(const PointerPath& path) const
    {
        const std::string name = path.Module < m_modules.size() ? m_modules[path.Module] : std::string("?");
        return "\"" + name + "\"+" + Hex(path.ModuleOffset);
    }

    std::string PointerResults::PathText(const PointerPath& path) const
    {
        std::string text = BaseText(path);
        for (int index = 0; index < path.Count; ++index)
            text = "[" + text + "]+" + Hex(path.Offsets[index]);
        return text;
    }

    Status PointerResults::Save(const std::filesystem::path& path) const
    {
        const std::string name = WideToUtf8(path.filename().wstring());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return Status::Fail(ErrorCode::PointerFileUnwritable, name);
        std::vector<std::uint8_t> head(kFileMagic, kFileMagic + sizeof(kFileMagic));
        Put(head, static_cast<std::uint32_t>(m_pointerSize));
        Put(head, static_cast<std::uint32_t>(m_levels));
        Put(head, static_cast<std::uint32_t>(m_modules.size()));
        Put(head, static_cast<std::uint32_t>(0));
        Put(head, m_count.load());
        Put(head, m_target);
        for (const std::string& module : m_modules)
        {
            const std::uint16_t length = static_cast<std::uint16_t>(std::min<std::size_t>(module.size(), 0xFFFF));
            Put(head, length);
            head.insert(head.end(), module.begin(), module.begin() + length);
        }
        out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        std::vector<std::uint8_t> block(kRescanRecords * m_recordSize);
        const std::uint64_t total = m_count.load();
        for (std::uint64_t index = 0; index < total;)
        {
            const std::size_t got = ReadRaw(index, kRescanRecords, block.data());
            if (got == 0)
                return Status::Fail(ErrorCode::PointerFileUnwritable, name);
            out.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(got * m_recordSize));
            index += got;
        }
        out.flush();
        if (!out)
            return Status::Fail(ErrorCode::PointerFileUnwritable, name);
        return Status::Success();
    }

    Status PointerResults::Load(const std::filesystem::path& path, std::uint64_t memoryLimit)
    {
        const std::string name = WideToUtf8(path.filename().wstring());
        const auto bad = [&]() { return Status::Fail(ErrorCode::PointerFileUnreadable, name); };
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return bad();
        char magic[sizeof(kFileMagic)] = {};
        in.read(magic, sizeof(magic));
        std::uint32_t pointerSize = 0;
        std::uint32_t levels = 0;
        std::uint32_t moduleCount = 0;
        std::uint32_t reserved = 0;
        std::uint64_t count = 0;
        std::uint64_t target = 0;
        if (!in || std::memcmp(magic, kFileMagic, sizeof(magic)) != 0 || !Take(in, pointerSize) || !Take(in, levels) || !Take(in, moduleCount) || !Take(in, reserved) ||
            !Take(in, count) || !Take(in, target))
            return bad();
        if ((pointerSize != 4 && pointerSize != 8) || levels < 1 || levels > static_cast<std::uint32_t>(kPointerMaxLevel) || moduleCount > 0x10000)
            return bad();
        std::vector<std::string> modules;
        modules.reserve(moduleCount);
        for (std::uint32_t index = 0; index < moduleCount; ++index)
        {
            std::uint16_t length = 0;
            if (!Take(in, length))
                return bad();
            std::string module(length, '\0');
            in.read(module.data(), length);
            if (!in)
                return bad();
            modules.push_back(std::move(module));
        }
        Begin(static_cast<int>(pointerSize), static_cast<int>(levels), std::move(modules), memoryLimit);
        m_target = target;
        std::vector<std::uint8_t> block(kRescanRecords * m_recordSize);
        while (count)
        {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(count, kRescanRecords));
            in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(want * m_recordSize));
            if (!in)
            {
                Begin(m_pointerSize, m_levels, {}, memoryLimit);
                return bad();
            }
            for (std::size_t index = 0; index < want; ++index)
            {
                const std::uint8_t* record = block.data() + index * m_recordSize;
                std::uint16_t module;
                std::memcpy(&module, record + 4, 2);
                if (module >= m_modules.size() || record[6] == 0 || record[6] > m_levels)
                {
                    Begin(m_pointerSize, m_levels, {}, memoryLimit);
                    return bad();
                }
            }
            if (!Append(block.data(), want))
            {
                Begin(m_pointerSize, m_levels, {}, memoryLimit);
                return Status::Fail(ErrorCode::ScanStoreFailed, name);
            }
            count -= want;
        }
        return Status::Success();
    }

    std::optional<std::vector<std::uint32_t>> SortPointerResults(const PointerResults& results, PointerOrder order, PointerProgress* progress)
    {
        const std::uint64_t count = results.Count();
        if (count > kPointerSortLimit)
            return std::nullopt;
        std::vector<std::uint32_t> indices(static_cast<std::size_t>(count));
        std::iota(indices.begin(), indices.end(), 0u);
        if (order == PointerOrder::Found || count < 2)
            return indices;
        if (progress)
            progress->Enter(PointerStage::Ordering, count);
        const std::size_t size = results.RecordSize();
        std::vector<std::uint8_t> records(static_cast<std::size_t>(count) * size);
        for (std::uint64_t index = 0; index < count;)
        {
            const std::size_t got = results.ReadRaw(index, kRescanRecords, records.data() + static_cast<std::size_t>(index) * size);
            if (got == 0 || (progress && progress->Cancel))
                return std::nullopt;
            index += got;
            if (progress)
                progress->Done = index / 2;
        }
        const std::uint8_t* data = records.data();
        std::sort(indices.begin(), indices.end(), [data, size, order](std::uint32_t a, std::uint32_t b)
            {
                const int result = ComparePaths(data + a * size, data + b * size, order);
                return result ? result < 0 : a < b;
            });
        if (progress)
            progress->Done = count;
        return indices;
    }

    struct PointerMap::Impl
    {
        int PointerSize = 8;
        int Shift = 3;
        std::vector<PointerModule> Modules;
        std::vector<Range> Ranges;
        std::vector<Range> ByCompact;
        std::vector<std::uint32_t> Cells;
        std::uint64_t Low = 0;
        std::uint64_t Span = 0;
        int CellShift = 12;
        std::uint64_t Total = 0;
        std::uint64_t StaticSlots = 0;
        int BucketCount = 0;
        KeyArena Buckets[kMaxBuckets];
        std::uint64_t BucketStart[kMaxBuckets + 1] = {};
        std::uint64_t Count = 0;
        bool Cancelled = false;
        bool Built = false;

        void Clear()
        {
            Modules.clear();
            Ranges.clear();
            ByCompact.clear();
            Cells.clear();
            for (KeyArena& bucket : Buckets)
                bucket.Release();
            std::fill(std::begin(BucketStart), std::end(BucketStart), 0ull);
            Total = 0;
            StaticSlots = 0;
            BucketCount = 0;
            Count = 0;
            Cancelled = false;
            Built = false;
        }

        const Range* Find(std::uint64_t value, const Range*& hint) const
        {
            if (value - hint->Base < hint->End - hint->Base)
                return hint;
            const std::size_t cell = static_cast<std::size_t>((value - Low) >> CellShift);
            const std::size_t first = Cells[cell];
            std::size_t low = first;
            std::size_t high = std::min<std::size_t>(static_cast<std::size_t>(Cells[cell + 1]) + 1, Ranges.size());
            while (low < high)
            {
                const std::size_t middle = (low + high) / 2;
                if (Ranges[middle].Base <= value)
                    low = middle + 1;
                else
                    high = middle;
            }
            if (low == first)
                return nullptr;
            const Range* range = &Ranges[low - 1];
            if (value >= range->End)
                return nullptr;
            hint = range;
            return range;
        }

        const Range* FindReal(std::uint64_t value) const
        {
            if (Ranges.empty() || value - Low >= Span)
                return nullptr;
            const Range* hint = Ranges.data();
            return Find(value, hint);
        }

        const Range& AtCompact(std::uint64_t compact, std::size_t& hint) const
        {
            const Range& last = ByCompact[hint];
            if (compact - last.Compact < last.Size())
                return last;
            const auto after = std::upper_bound(ByCompact.begin(), ByCompact.end(), compact, [](std::uint64_t value, const Range& range) { return value < range.Compact; });
            hint = static_cast<std::size_t>(after - ByCompact.begin()) - 1;
            return ByCompact[hint];
        }

        std::uint64_t Key(std::uint64_t entry, int& bucket) const
        {
            bucket = 0;
            while (bucket + 1 < BucketCount && entry >= BucketStart[bucket + 1])
                ++bucket;
            return Buckets[bucket].Data()[static_cast<std::size_t>(entry - BucketStart[bucket])];
        }

        template <class Word>
        bool Collect(const std::uint8_t* data, std::size_t size, std::uint64_t compact, bool aligned, const Range*& hint, KeyWriter* writers, std::uint64_t& found) const
        {
            const std::uint64_t low = Low;
            const std::uint64_t span = Span;
            const std::uint64_t mask = aligned ? sizeof(Word) - 1 : 0;
            const std::uint64_t slot = compact >> Shift;
            const std::size_t count = size / sizeof(Word);
            for (std::size_t index = 0; index < count; ++index)
            {
                Word word;
                std::memcpy(&word, data + index * sizeof(Word), sizeof(Word));
                const std::uint64_t value = word;
                if (value - low >= span || (value & mask))
                    continue;
                const Range* range = Find(value, hint);
                if (!range)
                    continue;
                const std::uint64_t to = range->Compact + (value - range->Base);
                if (!writers[to >> 32].Push(((to & 0xFFFFFFFFull) << 32) | (slot + index)))
                    return false;
                ++found;
            }
            return true;
        }
    };

    PointerMap::PointerMap() : m_impl(std::make_unique<Impl>())
    {
    }

    PointerMap::~PointerMap() = default;

    int PointerMap::PointerSize() const
    {
        return m_impl->PointerSize;
    }

    std::uint64_t PointerMap::Count() const
    {
        return m_impl->Count;
    }

    std::uint64_t PointerMap::ScannedBytes() const
    {
        return m_impl->Total;
    }

    bool PointerMap::Cancelled() const
    {
        return m_impl->Cancelled;
    }

    std::uint64_t PointerMap::MemoryBytes() const
    {
        std::uint64_t bytes = 0;
        for (const KeyArena& bucket : m_impl->Buckets)
            bytes += bucket.Bytes();
        return bytes;
    }

    Status PointerMap::Build(const PointerSource& source, bool aligned, int threads, PointerProgress* progress)
    {
        Impl& map = *m_impl;
        map.Clear();
        map.PointerSize = source.PointerSize() == 4 ? 4 : 8;
        map.Shift = map.PointerSize == 8 ? 3 : 2;
        map.Modules = source.Modules();
        threads = WorkerCount(threads);
        const std::uint64_t pointer = static_cast<std::uint64_t>(map.PointerSize);

        std::vector<PointerRegion> regions = source.Regions();
        std::sort(regions.begin(), regions.end(), [](const PointerRegion& a, const PointerRegion& b) { return a.Base < b.Base; });
        std::uint64_t floor = 0;
        for (const PointerRegion& region : regions)
        {
            std::uint64_t base = std::max(region.Base, floor);
            base = (base + pointer - 1) & ~(pointer - 1);
            const std::uint64_t end = (region.Base + region.Size) & ~(pointer - 1);
            if (end <= base)
                continue;
            floor = end;
            const int module = region.Module >= 0 && region.Module < static_cast<int>(std::min<std::size_t>(map.Modules.size(), 0xFFFF)) ? region.Module : -1;
            if (!map.Ranges.empty() && map.Ranges.back().End == base && map.Ranges.back().Module == module)
            {
                map.Ranges.back().End = end;
                continue;
            }
            Range range;
            range.Base = base;
            range.End = end;
            range.Module = module;
            map.Ranges.push_back(range);
        }
        if (map.Ranges.empty())
            return Status::Fail(ErrorCode::ScanRangeEmpty);

        std::uint64_t compact = 0;
        for (const bool statics : { true, false })
        {
            for (Range& range : map.Ranges)
            {
                if ((range.Module >= 0) != statics)
                    continue;
                range.Compact = compact;
                compact += range.Size();
                map.ByCompact.push_back(range);
            }
            if (statics)
                map.StaticSlots = compact >> map.Shift;
        }
        map.Total = compact;
        if (map.Total / pointer >= 0xFFFFFFF0ull || map.Total > (static_cast<std::uint64_t>(kMaxBuckets) << 32))
            return Status::Fail(ErrorCode::PointerScanTooLarge, ByteSize(map.Total));
        map.BucketCount = static_cast<int>((map.Total + 0xFFFFFFFFull) >> 32);

        map.Low = map.Ranges.front().Base;
        map.Span = map.Ranges.back().End - map.Low;
        map.CellShift = std::max(12, static_cast<int>(std::bit_width(map.Span - 1)) - 16);
        const std::size_t cells = static_cast<std::size_t>((map.Span - 1) >> map.CellShift) + 1;
        map.Cells.assign(cells + 1, static_cast<std::uint32_t>(map.Ranges.size()));
        std::size_t at = 0;
        for (std::size_t cell = 0; cell < cells; ++cell)
        {
            const std::uint64_t start = map.Low + (static_cast<std::uint64_t>(cell) << map.CellShift);
            while (at < map.Ranges.size() && map.Ranges[at].End <= start)
                ++at;
            map.Cells[cell] = static_cast<std::uint32_t>(at);
        }

        std::vector<Chunk> chunks;
        for (const Range& range : map.Ranges)
        {
            for (std::uint64_t offset = 0; offset < range.Size(); offset += kChunkBytes)
                chunks.push_back({ range.Base + offset, range.Compact + offset, static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, range.Size() - offset)) });
        }
        const std::uint64_t slots = map.Total / pointer + static_cast<std::uint64_t>(threads) * kSlabKeys;
        for (int bucket = 0; bucket < map.BucketCount; ++bucket)
        {
            if (!map.Buckets[bucket].Reserve(slots))
                return Status::Fail(ErrorCode::PointerScanNoMemory, ByteSize(slots * sizeof(std::uint64_t)));
        }
        if (progress)
            progress->Enter(PointerStage::Reading, map.Total);

        std::atomic<std::size_t> next{ 0 };
        std::atomic<bool> failed{ false };
        RunWorkers(threads, L"Pointer scan worker", [&](int)
            {
                std::vector<std::uint8_t> buffer(kChunkBytes);
                KeyWriter writers[kMaxBuckets];
                for (int bucket = 0; bucket < map.BucketCount; ++bucket)
                    writers[bucket].Arena = &map.Buckets[bucket];
                const Range* hint = map.Ranges.data();
                for (;;)
                {
                    const std::size_t item = next.fetch_add(1);
                    if (item >= chunks.size() || failed || (progress && progress->Cancel))
                        break;
                    const Chunk& chunk = chunks[item];
                    const std::size_t read = source.Read(chunk.Real, buffer.data(), chunk.Size);
                    std::uint64_t found = 0;
                    const bool stored = map.PointerSize == 8 ? map.Collect<std::uint64_t>(buffer.data(), read, chunk.Compact, aligned, hint, writers, found)
                                                             : map.Collect<std::uint32_t>(buffer.data(), read, chunk.Compact, aligned, hint, writers, found);
                    if (!stored)
                        failed = true;
                    if (progress)
                    {
                        progress->Done += chunk.Size;
                        progress->Pointers += found;
                    }
                }
                for (KeyWriter& writer : writers)
                    writer.Finish();
            });
        if (failed)
        {
            map.Clear();
            return Status::Fail(ErrorCode::PointerScanNoMemory, ByteSize(slots * sizeof(std::uint64_t)));
        }
        if (progress && progress->Cancel)
        {
            map.Clear();
            map.Cancelled = true;
            return Status::Success();
        }
        if (progress)
            progress->Enter(PointerStage::Sorting, static_cast<std::uint64_t>(map.BucketCount));
        for (int bucket = 0; bucket < map.BucketCount; ++bucket)
        {
            map.Buckets[bucket].Seal(threads);
            map.BucketStart[bucket + 1] = map.BucketStart[bucket] + map.Buckets[bucket].Count();
            if (progress)
                ++progress->Done;
        }
        map.Count = map.BucketStart[map.BucketCount];
        if (map.Count >= 0xFFFFFFF0ull)
        {
            map.Clear();
            return Status::Fail(ErrorCode::PointerScanTooLarge, Grouped(map.Count) + " pointers");
        }
        if (progress)
            progress->Pointers = map.Count;
        map.Built = true;
        return Status::Success();
    }

    Status PointerMap::Search(const PointerScanOptions& options, PointerResults& results, PointerProgress* progress, PointerScanStats* stats)
    {
        const Impl& map = *m_impl;
        const int maxLevel = std::clamp(options.MaxLevel, 1, kPointerMaxLevel);
        const std::uint64_t maxOffset = std::min(options.MaxOffset, kPointerMaxOffset);
        const int threads = WorkerCount(options.Threads);
        std::vector<std::string> names;
        names.reserve(map.Modules.size());
        for (const PointerModule& module : map.Modules)
            names.push_back(module.Name);
        results.Begin(map.PointerSize, maxLevel, std::move(names), options.MemoryLimit);
        results.SetTarget(options.Target);
        PointerScanStats local;
        PointerScanStats& out = stats ? *stats : local;
        out.ScannedBytes = map.Total;
        out.Pointers = map.Count;
        out.MapBytes = MemoryBytes();
        out.Nodes = 0;
        out.Levels = 0;
        out.Results = 0;
        out.SearchBytes = 0;
        out.Truncated = false;
        out.Cancelled = false;
        out.TargetOutside = false;
        if (!map.Built)
        {
            out.Cancelled = map.Cancelled;
            return Status::Success();
        }
        const Range* targetRange = map.FindReal(options.Target);
        if (!targetRange || map.Count == 0)
        {
            out.TargetOutside = !targetRange;
            return Status::Success();
        }
        const std::uint64_t rootAddress = targetRange->Compact + (options.Target - targetRange->Base);

        const std::size_t parentBytes = static_cast<std::size_t>(map.Count) * sizeof(std::uint32_t);
        auto* parents = static_cast<std::uint32_t*>(::VirtualAlloc(nullptr, parentBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        KeyArena arenas[2];
        const std::uint64_t arenaKeys = map.Count + static_cast<std::uint64_t>(threads) * kSlabKeys;
        if (!parents || (maxLevel > 1 && (!arenas[0].Reserve(arenaKeys) || !arenas[1].Reserve(arenaKeys))))
        {
            if (parents)
                ::VirtualFree(parents, 0, MEM_RELEASE);
            return Status::Fail(ErrorCode::PointerScanNoMemory, ByteSize(parentBytes));
        }

        std::atomic<std::uint64_t> emitted{ 0 };
        std::atomic<bool> stop{ false };
        std::atomic<bool> truncated{ false };
        const int shift = map.Shift;
        const std::size_t recordSize = results.RecordSize();
        if (progress)
            progress->Enter(PointerStage::Searching, 1);

        const std::uint64_t* frontier = nullptr;
        std::size_t frontierCount = 1;
        bool root = true;
        int current = 0;
        for (int level = 0; level < maxLevel && frontierCount && !stop; ++level)
        {
            const bool last = level + 1 == maxLevel;
            KeyArena& nextArena = arenas[current];
            nextArena.Reset();
            if (progress)
            {
                progress->Level = level + 1;
                progress->Total += root ? 0 : frontierCount;
            }
            const auto address = [&](std::size_t index) { return root ? rootAddress : (frontier[index] >> 32) << shift; };
            const auto code = [&](std::size_t index) { return root ? kRootCode : static_cast<std::uint32_t>(frontier[index]) + 2; };
            std::atomic<std::size_t> claimed{ 0 };
            const auto sweep = [&](int)
                {
                    std::vector<std::uint8_t> buffer(kEmitRecords * recordSize);
                    std::size_t held = 0;
                    KeyWriter writer;
                    writer.Arena = &nextArena;
                    std::size_t rangeHint = 0;
                    std::size_t baseHint = 0;
                    const auto flush = [&]()
                        {
                            if (held && !results.Append(buffer.data(), held))
                                stop = true;
                            if (progress)
                                progress->Results += held;
                            held = 0;
                        };
                    const auto emit = [&](std::uint32_t slot, std::uint64_t value, std::size_t node)
                        {
                            if (emitted.fetch_add(1) >= options.MaxResults)
                            {
                                truncated = true;
                                stop = true;
                                return;
                            }
                            PointerPath path;
                            const std::uint64_t compact = static_cast<std::uint64_t>(slot) << shift;
                            const Range& base = map.AtCompact(compact, baseHint);
                            path.Module = static_cast<std::uint16_t>(base.Module);
                            path.ModuleOffset = static_cast<std::uint32_t>(base.Base + (compact - base.Compact) - map.Modules[static_cast<std::size_t>(base.Module)].Base);
                            int count = 0;
                            path.Offsets[count++] = static_cast<std::uint32_t>(address(node) - value);
                            std::uint32_t link = code(node);
                            while (link != kRootCode && count < kPointerMaxLevel)
                            {
                                int bucket = 0;
                                const std::uint64_t key = map.Key(link - 2, bucket);
                                const std::uint64_t to = (static_cast<std::uint64_t>(bucket) << 32) | (key >> 32);
                                const std::uint32_t up = parents[link - 2];
                                int upBucket = 0;
                                const std::uint64_t upAddress = up == kRootCode ? rootAddress : (map.Key(up - 2, upBucket) & 0xFFFFFFFFull) << shift;
                                path.Offsets[count++] = static_cast<std::uint32_t>(upAddress - to);
                                link = up;
                            }
                            path.Count = static_cast<std::uint8_t>(count);
                            if (held == kEmitRecords)
                                flush();
                            results.Encode(path, buffer.data() + held * recordSize);
                            ++held;
                        };
                    for (;;)
                    {
                        const std::size_t first = claimed.fetch_add(kSweepBlock);
                        if (first >= frontierCount || stop || (progress && progress->Cancel))
                            break;
                        const std::size_t end = std::min(frontierCount, first + kSweepBlock);
                        for (std::size_t node = first; node < end && !stop; ++node)
                        {
                            const std::uint64_t here = address(node);
                            const Range& range = map.AtCompact(here, rangeHint);
                            const std::uint64_t rangeEnd = range.Compact + range.Size();
                            std::uint64_t low = here - range.Compact > maxOffset ? here - maxOffset : range.Compact;
                            if (node > 0)
                            {
                                const std::uint64_t before = address(node - 1);
                                if (before >= low)
                                    low = before + 1;
                            }
                            for (std::uint64_t bucket = low >> 32; bucket <= here >> 32; ++bucket)
                            {
                                const KeyArena& keys = map.Buckets[static_cast<std::size_t>(bucket)];
                                const std::uint64_t* begin = keys.Data();
                                const std::uint64_t* stopAt = begin + keys.Count();
                                const std::uint64_t lowKey = bucket == low >> 32 ? (low & 0xFFFFFFFFull) << 32 : 0;
                                const std::uint64_t highTo = bucket == here >> 32 ? here & 0xFFFFFFFFull : 0xFFFFFFFFull;
                                for (const std::uint64_t* key = std::lower_bound(begin, stopAt, lowKey); key < stopAt && (*key >> 32) <= highTo; ++key)
                                {
                                    const std::uint32_t slot = static_cast<std::uint32_t>(*key);
                                    const std::uint64_t value = (bucket << 32) | (*key >> 32);
                                    if (slot < map.StaticSlots)
                                    {
                                        for (std::size_t other = node; other < frontierCount && !stop; ++other)
                                        {
                                            const std::uint64_t there = address(other);
                                            if (there - value > maxOffset || there >= rangeEnd)
                                                break;
                                            emit(slot, value, other);
                                        }
                                        continue;
                                    }
                                    if (last)
                                        continue;
                                    const std::uint64_t entry = map.BucketStart[bucket] + static_cast<std::uint64_t>(key - begin);
                                    std::uint32_t& parent = parents[static_cast<std::size_t>(entry)];
                                    if (parent != 0)
                                        continue;
                                    parent = code(node);
                                    if (!writer.Push((static_cast<std::uint64_t>(slot) << 32) | entry))
                                    {
                                        stop = true;
                                        truncated = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (progress && !root)
                            progress->Done += end - first;
                    }
                    flush();
                    writer.Finish();
                };
            RunWorkers(frontierCount >= kParallelFrontier ? threads : 1, L"Pointer scan worker", sweep);
            out.Levels = level + 1;
            if (last || stop || (progress && progress->Cancel))
                break;
            nextArena.Seal(threads);
            frontier = nextArena.Data();
            frontierCount = nextArena.Count();
            out.Nodes += frontierCount;
            root = false;
            current ^= 1;
        }

        out.SearchBytes = parentBytes + arenas[0].Bytes() + arenas[1].Bytes();
        ::VirtualFree(parents, 0, MEM_RELEASE);
        out.Results = results.Count();
        out.Truncated = truncated;
        out.Cancelled = progress && progress->Cancel;
        if (results.Failed())
            return Status::Fail(ErrorCode::ScanStoreFailed, "the pointer paths did not fit on the drive");
        return Status::Success();
    }

    Status ScanPointers(const PointerSource& source, const PointerScanOptions& options, PointerResults& results, PointerProgress* progress, PointerScanStats* stats)
    {
        PointerScanStats local;
        PointerScanStats& out = stats ? *stats : local;
        out = {};
        PointerMap map;
        const auto started = std::chrono::steady_clock::now();
        Status status = map.Build(source, options.Aligned, options.Threads, progress);
        out.MapSeconds = Seconds(started);
        if (!status)
        {
            results.Begin(source.PointerSize(), options.MaxLevel, {}, options.MemoryLimit);
            return status;
        }
        const auto searching = std::chrono::steady_clock::now();
        status = map.Search(options, results, progress, &out);
        out.SearchSeconds = Seconds(searching);
        return status;
    }

    Status RescanPointers(const PointerSource& source, const PointerResults& from, const PointerRescan& rescan, PointerResults& out, PointerProgress* progress)
    {
        const int pointerSize = from.PointerSize();
        out.Begin(pointerSize, from.Levels(), from.Modules(), rescan.MemoryLimit);
        // ∑ A rescan by value leaves the paths at many addresses, so the old target would flag every row as leading elsewhere.
        out.SetTarget(rescan.Address ? *rescan.Address : rescan.Value.empty() ? from.Target() : 0);
        const bool rounded = rescan.Step > 0.0 && ((rescan.Type == ValueType::Float && rescan.Value.size() == 4) || (rescan.Type == ValueType::Double && rescan.Value.size() == 8));
        const double wanted = rounded ? AsDouble(rescan.Type, rescan.Value, true).value_or(0.0) : 0.0;
        const double slack = rescan.Step * 0.5 + std::fabs(wanted) * 1e-12;
        const auto matches = [&](const std::vector<std::uint8_t>& value)
            {
                if (!rounded)
                    return value == rescan.Value;
                const std::optional<double> found = AsDouble(rescan.Type, value, true);
                return found && !std::isnan(*found) && (*found == wanted || std::fabs(*found - wanted) <= slack);
            };
        const std::vector<PointerModule> loaded = source.Modules();
        std::vector<std::uint64_t> bases(from.Modules().size(), 0);
        for (std::size_t index = 0; index < bases.size(); ++index)
        {
            for (const PointerModule& module : loaded)
            {
                if (EqualsNoCase(module.Name, from.Modules()[index]))
                {
                    bases[index] = module.Base;
                    break;
                }
            }
        }
        const std::uint64_t total = from.Count();
        if (progress)
            progress->Enter(PointerStage::Rescanning, total);
        const std::size_t recordSize = from.RecordSize();
        std::atomic<std::uint64_t> next{ 0 };
        std::atomic<bool> failed{ false };
        RunWorkers(total >= kRescanRecords * 2 ? WorkerCount(rescan.Threads) : 1, L"Pointer rescan worker", [&](int)
            {
                PageCache cache(source);
                std::vector<std::uint8_t> block(kRescanRecords * recordSize);
                std::vector<std::uint8_t> kept(kRescanRecords * recordSize);
                std::vector<std::uint8_t> value(rescan.Value.size());
                const auto read = [&](std::uint64_t address) { return cache.Pointer(address, pointerSize); };
                for (;;)
                {
                    const std::uint64_t first = next.fetch_add(kRescanRecords);
                    if (first >= total || failed || (progress && progress->Cancel))
                        break;
                    const std::size_t got = from.ReadRaw(first, kRescanRecords, block.data());
                    std::size_t held = 0;
                    for (std::size_t index = 0; index < got; ++index)
                    {
                        const std::uint8_t* record = block.data() + index * recordSize;
                        PointerPath path;
                        from.Decode(record, path);
                        if (path.Module >= bases.size() || bases[path.Module] == 0)
                            continue;
                        const std::optional<std::uint64_t> address = ResolvePointerPath(bases[path.Module], path, read);
                        if (!address || (rescan.Address && *address != *rescan.Address))
                            continue;
                        if (!value.empty() && (!cache.Read(*address, value.data(), value.size()) || !matches(value)))
                            continue;
                        std::memcpy(kept.data() + held * recordSize, record, recordSize);
                        ++held;
                    }
                    if (held && !out.Append(kept.data(), held))
                        failed = true;
                    if (progress)
                    {
                        progress->Done += got;
                        progress->Results += held;
                    }
                }
            });
        if (failed || out.Failed())
            return Status::Fail(ErrorCode::ScanStoreFailed, "the pointer paths did not fit on the drive");
        return Status::Success();
    }
}
