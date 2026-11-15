#include "Scan/Scanner.h"

#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Process.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <functional>

#include <windows.h>

namespace Manifold::Scan
{
    namespace
    {
        constexpr std::uint64_t kChunkBytes = 2ull * 1024 * 1024;
        constexpr std::size_t kWriteBuffer = 1024 * 1024;
        constexpr std::uint64_t kNextBatchRecords = 32768;
        constexpr std::uint64_t kWindowBytes = 64 * 1024;

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        class TempFile
        {
        public:
            TempFile() = default;
            ~TempFile()
            {
                if (m_handle != INVALID_HANDLE_VALUE)
                    ::CloseHandle(m_handle);
            }
            TempFile(const TempFile&) = delete;
            TempFile& operator=(const TempFile&) = delete;

            bool Create()
            {
                static std::atomic<unsigned> counter{ 0 };
                const std::filesystem::path path = Paths::ScanFolder() /
                    (std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(counter.fetch_add(1)) + L"-" + std::to_wstring(::GetTickCount64()) + L".scan");
                m_handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                m_buffer.reserve(kWriteBuffer);
                return m_handle != INVALID_HANDLE_VALUE;
            }

            std::uint64_t Size() const { return m_written + m_buffer.size(); }

            bool Append(const void* data, std::size_t size)
            {
                if (m_buffer.size() + size > kWriteBuffer && !Flush())
                    return false;
                if (size > kWriteBuffer)
                    return WriteRaw(data, size);
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                m_buffer.insert(m_buffer.end(), bytes, bytes + size);
                return true;
            }

            bool Flush()
            {
                if (m_buffer.empty())
                    return true;
                const bool ok = WriteRaw(m_buffer.data(), m_buffer.size());
                m_buffer.clear();
                return ok;
            }

            bool ReadAt(std::uint64_t offset, void* buffer, std::size_t size) const
            {
                auto* out = static_cast<std::uint8_t*>(buffer);
                while (size > 0)
                {
                    OVERLAPPED overlapped{};
                    overlapped.Offset = static_cast<DWORD>(offset);
                    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
                    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 64u * 1024 * 1024));
                    DWORD got = 0;
                    if (!::ReadFile(m_handle, out, want, &got, &overlapped) || got == 0)
                        return false;
                    out += got;
                    offset += got;
                    size -= got;
                }
                return true;
            }

        private:
            bool WriteRaw(const void* data, std::size_t size)
            {
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                while (size > 0)
                {
                    OVERLAPPED overlapped{};
                    overlapped.Offset = static_cast<DWORD>(m_written);
                    overlapped.OffsetHigh = static_cast<DWORD>(m_written >> 32);
                    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 64u * 1024 * 1024));
                    DWORD done = 0;
                    if (!::WriteFile(m_handle, bytes, want, &done, &overlapped) || done == 0)
                        return false;
                    bytes += done;
                    size -= done;
                    m_written += done;
                }
                return true;
            }

            HANDLE m_handle = INVALID_HANDLE_VALUE;
            std::vector<std::uint8_t> m_buffer;
            std::uint64_t m_written = 0;
        };

        struct Segment
        {
            std::uint64_t Order = 0;
            int File = 0;
            std::uint64_t Offset = 0;
            std::uint64_t Count = 0;
            std::uint64_t Before = 0;
        };

        struct Snapshot
        {
            std::uint64_t Order = 0;
            std::uint64_t Base = 0;
            std::uint64_t Size = 0;
            int File = 0;
            std::uint64_t Offset = 0;
            std::uint64_t Positions = 0;
            std::uint64_t Before = 0;
        };

        struct WorkChunk
        {
            std::uint64_t Base = 0;
            std::uint64_t Size = 0;
            std::uint64_t ReadEnd = 0;
        };

        bool PassesFilter(const Target::RegionInfo& region, const ScanOptions& options)
        {
            if (!region.Readable())
                return false;
            if (region.Type == MEM_PRIVATE && !options.Private)
                return false;
            if (region.Type == MEM_IMAGE && !options.Image)
                return false;
            if (region.Type == MEM_MAPPED && !options.Mapped)
                return false;
            const auto check = [](Tristate wanted, bool has) { return wanted == Tristate::Any || (wanted == Tristate::Yes) == has; };
            return check(options.CopyOnWrite, region.CopyOnWrite()) && check(options.Writable, region.Writable()) && check(options.Executable, region.Executable());
        }

        int DecimalsOf(std::string_view text)
        {
            text = TrimView(text);
            const std::size_t dot = text.find_first_of(".,");
            if (dot == std::string_view::npos)
                return 0;
            int count = 0;
            for (std::size_t i = dot + 1; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
                ++count;
            return count;
        }

        template <class T>
        T Load(const std::uint8_t* data)
        {
            T value;
            std::memcpy(&value, data, sizeof(T));
            return value;
        }

        class Matcher
        {
        public:
            Status Build(const ScanOptions& options, int pointerSize)
            {
                m_type = options.Type;
                m_mode = options.Mode;
                m_floats = options.Floats;
                ValueFormat format;
                format.Hex = options.Hex;
                format.Unicode = options.Unicode;
                format.PointerSize = pointerSize;

                if (m_type == ValueType::ByteArray)
                {
                    if (CompareNeedsValue(m_mode) || m_mode == Compare::Unknown)
                    {
                        if (m_mode == Compare::Unknown)
                            return Status::Fail(ErrorCode::ScanBadPattern, "Unknown initial value needs a fixed length; scan for a pattern instead");
                        const auto pattern = ParsePattern(options.Value);
                        if (!pattern)
                            return Status::Fail(ErrorCode::ScanBadPattern, options.Value);
                        m_patternA = pattern->Bytes;
                        m_patternB = pattern->Bytes;
                        m_mask = pattern->Mask;
                        m_size = static_cast<int>(m_patternA.size());
                    }
                    return Status::Success();
                }
                if (m_type == ValueType::String)
                {
                    if (options.Value.empty())
                        return Status::Fail(ErrorCode::ScanNoValue);
                    if (options.Unicode)
                    {
                        const std::wstring wide = Utf8ToWide(options.Value);
                        for (wchar_t c : wide)
                        {
                            const wchar_t lower = options.CaseSensitive ? c : static_cast<wchar_t>(std::towlower(c));
                            const wchar_t upper = options.CaseSensitive ? c : static_cast<wchar_t>(std::towupper(c));
                            m_patternA.push_back(static_cast<std::uint8_t>(lower & 0xFF));
                            m_patternA.push_back(static_cast<std::uint8_t>(lower >> 8));
                            m_patternB.push_back(static_cast<std::uint8_t>(upper & 0xFF));
                            m_patternB.push_back(static_cast<std::uint8_t>(upper >> 8));
                        }
                    }
                    else
                    {
                        for (char c : options.Value)
                        {
                            const auto byte = static_cast<std::uint8_t>(c);
                            m_patternA.push_back(options.CaseSensitive ? byte : static_cast<std::uint8_t>(byte >= 'A' && byte <= 'Z' ? byte + 32 : byte));
                            m_patternB.push_back(options.CaseSensitive ? byte : static_cast<std::uint8_t>(byte >= 'a' && byte <= 'z' ? byte - 32 : byte));
                        }
                    }
                    m_mask.assign(m_patternA.size(), 0xFF);
                    m_size = static_cast<int>(m_patternA.size());
                    return Status::Success();
                }

                m_size = ValueSize(m_type, format);
                if (CompareNeedsValue(m_mode))
                {
                    if (TrimView(options.Value).empty())
                        return Status::Fail(ErrorCode::ScanNoValue);
                    const auto parse = [&](const std::string& text, double& asDouble, std::int64_t& asInteger) -> bool
                        {
                            if (IsFloating(m_type) && !options.Hex)
                            {
                                const auto value = ParseDouble(text);
                                if (!value)
                                    return false;
                                asDouble = *value;
                                asInteger = static_cast<std::int64_t>(*value);
                                return true;
                            }
                            const auto bytes = ParseValue(m_type, text, format);
                            if (!bytes)
                                return false;
                            if (IsFloating(m_type))
                            {
                                asDouble = *AsDouble(m_type, *bytes, true);
                                asInteger = static_cast<std::int64_t>(asDouble);
                                return true;
                            }
                            std::uint64_t raw = 0;
                            std::memcpy(&raw, bytes->data(), std::min<std::size_t>(8, bytes->size()));
                            asInteger = SignExtend(raw);
                            asDouble = static_cast<double>(asInteger);
                            return true;
                        };
                    if (!parse(options.Value, m_doubleA, m_intA))
                        return Status::Fail(ErrorCode::ScanBadValue, options.Value);
                    if (CompareNeedsTwoValues(m_mode))
                    {
                        if (!parse(options.Value2, m_doubleB, m_intB))
                            return Status::Fail(ErrorCode::ScanBadValue, options.Value2);
                        if (m_doubleA > m_doubleB)
                            std::swap(m_doubleA, m_doubleB);
                        if (m_intA > m_intB)
                            std::swap(m_intA, m_intB);
                    }
                    const int decimals = IsFloating(m_type) && !options.Hex ? DecimalsOf(options.Value) : 0;
                    m_step = std::pow(10.0, -decimals);
                    if (options.Hex && IsFloating(m_type))
                        m_floats = FloatMode::Exact;
                }
                return Status::Success();
            }

            int Size() const { return m_size; }
            ValueType Type() const { return m_type; }
            Compare Mode() const { return m_mode; }
            bool NeedsPrevious() const
            {
                return m_mode == Compare::Increased || m_mode == Compare::IncreasedBy || m_mode == Compare::Decreased || m_mode == Compare::DecreasedBy ||
                       m_mode == Compare::Changed || m_mode == Compare::Unchanged;
            }

            bool Test(const std::uint8_t* now, const std::uint8_t* previous, const std::uint8_t* first) const
            {
                if (m_type == ValueType::String || (m_type == ValueType::ByteArray && !m_patternA.empty() && CompareNeedsValue(m_mode)))
                {
                    if (m_mode == Compare::Changed)
                        return std::memcmp(now, previous, static_cast<std::size_t>(m_size)) != 0;
                    if (m_mode == Compare::Unchanged)
                        return std::memcmp(now, previous, static_cast<std::size_t>(m_size)) == 0;
                    if (m_mode == Compare::SameAsFirst)
                        return std::memcmp(now, first, static_cast<std::size_t>(m_size)) == 0;
                    for (int i = 0; i < m_size; ++i)
                    {
                        const std::uint8_t byte = now[i] & m_mask[static_cast<std::size_t>(i)];
                        if (byte != m_patternA[static_cast<std::size_t>(i)] && byte != m_patternB[static_cast<std::size_t>(i)])
                            return false;
                    }
                    return true;
                }
                if (m_type == ValueType::ByteArray)
                {
                    if (m_mode == Compare::Changed)
                        return std::memcmp(now, previous, static_cast<std::size_t>(m_size)) != 0;
                    if (m_mode == Compare::Unchanged)
                        return std::memcmp(now, previous, static_cast<std::size_t>(m_size)) == 0;
                    if (m_mode == Compare::SameAsFirst)
                        return std::memcmp(now, first, static_cast<std::size_t>(m_size)) == 0;
                    return true;
                }
                switch (m_type)
                {
                case ValueType::Byte: return TestInteger<std::int8_t>(now, previous, first);
                case ValueType::TwoBytes: return TestInteger<std::int16_t>(now, previous, first);
                case ValueType::FourBytes: return TestInteger<std::int32_t>(now, previous, first);
                case ValueType::EightBytes: return TestInteger<std::int64_t>(now, previous, first);
                case ValueType::Pointer: return m_size == 4 ? TestInteger<std::int32_t>(now, previous, first) : TestInteger<std::int64_t>(now, previous, first);
                case ValueType::Float: return TestFloat<float>(now, previous, first);
                case ValueType::Double: return TestFloat<double>(now, previous, first);
                default: return false;
                }
            }

            template <class T>
            bool TestInteger(const std::uint8_t* nowBytes, const std::uint8_t* previousBytes, const std::uint8_t* firstBytes) const
            {
                const T now = Load<T>(nowBytes);
                switch (m_mode)
                {
                case Compare::Exact: return now == static_cast<T>(m_intA);
                case Compare::Bigger: return static_cast<std::int64_t>(now) > m_intA;
                case Compare::Smaller: return static_cast<std::int64_t>(now) < m_intA;
                case Compare::Between: return static_cast<std::int64_t>(now) >= m_intA && static_cast<std::int64_t>(now) <= m_intB;
                case Compare::Unknown: return true;
                case Compare::Increased: return now > Load<T>(previousBytes);
                case Compare::Decreased: return now < Load<T>(previousBytes);
                case Compare::IncreasedBy: return now == static_cast<T>(Load<T>(previousBytes) + static_cast<T>(m_intA));
                case Compare::DecreasedBy: return now == static_cast<T>(Load<T>(previousBytes) - static_cast<T>(m_intA));
                case Compare::Changed: return now != Load<T>(previousBytes);
                case Compare::Unchanged: return now == Load<T>(previousBytes);
                case Compare::SameAsFirst: return now == Load<T>(firstBytes);
                }
                return false;
            }

            template <class T>
            bool TestFloat(const std::uint8_t* nowBytes, const std::uint8_t* previousBytes, const std::uint8_t* firstBytes) const
            {
                const T now = Load<T>(nowBytes);
                if (std::isnan(now))
                    return m_mode == Compare::Unknown || m_mode == Compare::Changed;
                const double value = static_cast<double>(now);
                switch (m_mode)
                {
                case Compare::Exact: return Equal(value, m_doubleA);
                case Compare::Bigger: return value > m_doubleA;
                case Compare::Smaller: return value < m_doubleA;
                case Compare::Between: return value >= m_doubleA && value <= m_doubleB;
                case Compare::Unknown: return true;
                case Compare::Increased: return now > Load<T>(previousBytes);
                case Compare::Decreased: return now < Load<T>(previousBytes);
                case Compare::IncreasedBy: return Equal(value, static_cast<double>(Load<T>(previousBytes)) + m_doubleA);
                case Compare::DecreasedBy: return Equal(value, static_cast<double>(Load<T>(previousBytes)) - m_doubleA);
                case Compare::Changed: return now != Load<T>(previousBytes);
                case Compare::Unchanged: return now == Load<T>(previousBytes);
                case Compare::SameAsFirst: return now == Load<T>(firstBytes);
                }
                return false;
            }

        private:
            std::int64_t SignExtend(std::uint64_t raw) const
            {
                switch (m_size)
                {
                case 1: return static_cast<std::int8_t>(raw);
                case 2: return static_cast<std::int16_t>(raw);
                case 4: return static_cast<std::int32_t>(raw);
                default: return static_cast<std::int64_t>(raw);
                }
            }

            bool Equal(double value, double wanted) const
            {
                switch (m_floats)
                {
                case FloatMode::Exact:
                    return m_type == ValueType::Float ? static_cast<float>(value) == static_cast<float>(wanted) : value == wanted;
                case FloatMode::Truncated:
                    return wanted >= 0.0 ? (value >= wanted && value < wanted + m_step) : (value <= wanted && value > wanted - m_step);
                case FloatMode::Rounded:
                default:
                    return std::fabs(value - wanted) <= m_step * 0.5 + std::fabs(wanted) * 1e-12;
                }
            }

            ValueType m_type = ValueType::FourBytes;
            Compare m_mode = Compare::Exact;
            FloatMode m_floats = FloatMode::Rounded;
            int m_size = 4;
            std::int64_t m_intA = 0;
            std::int64_t m_intB = 0;
            double m_doubleA = 0.0;
            double m_doubleB = 0.0;
            double m_step = 1.0;
            std::vector<std::uint8_t> m_patternA;
            std::vector<std::uint8_t> m_patternB;
            std::vector<std::uint8_t> m_mask;
        };

        int ThreadCount(int wanted)
        {
            if (wanted > 0)
                return std::clamp(wanted, 1, 64);
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            return static_cast<int>(std::clamp(cores / 2u, 2u, 8u));
        }
    }

    class ResultSet
    {
    public:
        ValueType Type = ValueType::FourBytes;
        ValueFormat Format;
        int ValueSize = 4;
        int Alignment = 4;
        bool IsSnapshot = false;
        std::vector<std::unique_ptr<TempFile>> Files;
        std::vector<Segment> Segments;
        std::vector<Snapshot> Snapshots;
        std::uint64_t Count = 0;

        std::size_t RecordSize() const { return 8 + 2 * static_cast<std::size_t>(ValueSize); }

        bool Record(std::uint64_t index, ScanRecord& out) const
        {
            if (index >= Count)
                return false;
            if (IsSnapshot)
            {
                auto found = std::upper_bound(Snapshots.begin(), Snapshots.end(), index, [](std::uint64_t value, const Snapshot& snapshot) { return value < snapshot.Before; });
                if (found == Snapshots.begin())
                    return false;
                --found;
                const std::uint64_t position = index - found->Before;
                const std::uint64_t offset = position * static_cast<std::uint64_t>(Alignment);
                out.Address = found->Base + offset;
                out.Previous.resize(static_cast<std::size_t>(ValueSize));
                if (!Files[static_cast<std::size_t>(found->File)]->ReadAt(found->Offset + offset, out.Previous.data(), out.Previous.size()))
                    return false;
                out.First = out.Previous;
                return true;
            }
            auto found = std::upper_bound(Segments.begin(), Segments.end(), index, [](std::uint64_t value, const Segment& segment) { return value < segment.Before; });
            if (found == Segments.begin())
                return false;
            --found;
            const std::uint64_t position = index - found->Before;
            std::vector<std::uint8_t> raw(RecordSize());
            if (!Files[static_cast<std::size_t>(found->File)]->ReadAt(found->Offset + position * RecordSize(), raw.data(), raw.size()))
                return false;
            std::memcpy(&out.Address, raw.data(), 8);
            out.Previous.assign(raw.begin() + 8, raw.begin() + 8 + ValueSize);
            out.First.assign(raw.begin() + 8 + ValueSize, raw.end());
            return true;
        }
    };

    const char* CompareName(Compare compare)
    {
        switch (compare)
        {
        case Compare::Exact: return "Exact value";
        case Compare::Bigger: return "Bigger than...";
        case Compare::Smaller: return "Smaller than...";
        case Compare::Between: return "Value between...";
        case Compare::Unknown: return "Unknown initial value";
        case Compare::Increased: return "Increased value";
        case Compare::IncreasedBy: return "Increased value by...";
        case Compare::Decreased: return "Decreased value";
        case Compare::DecreasedBy: return "Decreased value by...";
        case Compare::Changed: return "Changed value";
        case Compare::Unchanged: return "Unchanged value";
        case Compare::SameAsFirst: return "Same as first scan";
        }
        return "?";
    }

    bool CompareNeedsValue(Compare compare)
    {
        return compare == Compare::Exact || compare == Compare::Bigger || compare == Compare::Smaller || compare == Compare::Between ||
               compare == Compare::IncreasedBy || compare == Compare::DecreasedBy;
    }

    bool CompareNeedsTwoValues(Compare compare)
    {
        return compare == Compare::Between;
    }

    bool CompareAllowedFirst(Compare compare)
    {
        return compare == Compare::Exact || compare == Compare::Bigger || compare == Compare::Smaller || compare == Compare::Between || compare == Compare::Unknown;
    }

    bool CompareAllowedNext(Compare compare)
    {
        return compare != Compare::Unknown;
    }

    Scanner::Scanner() = default;

    Scanner::~Scanner()
    {
        m_cancel = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    Status Scanner::Start(std::shared_ptr<Target::Process> process, const ScanOptions& options, bool firstScan)
    {
        if (m_busy)
            return Status::Fail(ErrorCode::ScanBusy);
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        if (!firstScan && !m_current)
            return Status::Fail(ErrorCode::ScanNothingToCompare);
        if (firstScan && !CompareAllowedFirst(options.Mode))
            return Status::Fail(ErrorCode::ScanNothingToCompare);

        Matcher probe;
        if (Status built = probe.Build(options, process->PointerSize()); !built)
            return built;

        if (m_thread.joinable())
            m_thread.join();
        m_cancel = false;
        m_busy = true;
        m_done = 0;
        m_total = 1;
        m_options = options;
        m_startedAt = Now();
        std::shared_ptr<const ResultSet> previous = firstScan ? nullptr : m_current;
        m_thread = std::thread(&Scanner::Run, this, std::move(process), options, std::move(previous));
        return Status::Success();
    }

    void Scanner::Cancel()
    {
        m_cancel = true;
    }

    double Scanner::Progress() const
    {
        const double total = static_cast<double>(std::max<std::uint64_t>(1, m_total.load()));
        return std::clamp(static_cast<double>(m_done.load()) / total, 0.0, 1.0);
    }

    bool Scanner::Poll()
    {
        std::lock_guard lock(m_lock);
        if (!m_finishedReady)
            return false;
        m_finishedReady = false;
        if (m_thread.joinable())
            m_thread.join();
        m_lastError = m_finishedError;
        m_lastSeconds = Now() - m_startedAt;
        if (m_finished)
        {
            m_undo = m_current;
            m_current = std::move(m_finished);
            ++m_scanNumber;
            LOG_INFO("Scanner", Manifold::Format("Scan %d found %s results in %.2f s", m_scanNumber, Grouped(m_current->Count).c_str(), m_lastSeconds));
        }
        else if (!m_lastError.Ok())
        {
            LOG_ERROR("Scanner", m_lastError.Text);
        }
        m_busy = false;
        return true;
    }

    std::uint64_t Scanner::Count() const
    {
        return m_current ? m_current->Count : 0;
    }

    bool Scanner::IsSnapshot() const
    {
        return m_current && m_current->IsSnapshot;
    }

    bool Scanner::Record(std::uint64_t index, ScanRecord& out) const
    {
        return m_current && m_current->Record(index, out);
    }

    ValueType Scanner::Type() const
    {
        return m_current ? m_current->Type : m_options.Type;
    }

    int Scanner::ValueSize() const
    {
        return m_current ? m_current->ValueSize : 4;
    }

    ValueFormat Scanner::Format() const
    {
        return m_current ? m_current->Format : ValueFormat{};
    }

    void Scanner::Undo()
    {
        if (m_busy || !m_undo)
            return;
        m_current = std::move(m_undo);
        m_undo.reset();
        --m_scanNumber;
    }

    void Scanner::Reset()
    {
        if (m_busy)
            Cancel();
        if (m_thread.joinable())
            m_thread.join();
        m_busy = false;
        m_finishedReady = false;
        m_finished.reset();
        m_current.reset();
        m_undo.reset();
        m_scanNumber = 0;
    }

    void Scanner::Run(std::shared_ptr<Target::Process> process, ScanOptions options, std::shared_ptr<const ResultSet> previous)
    {
        Matcher matcher;
        Status status = matcher.Build(options, process->PointerSize());
        auto result = std::make_shared<ResultSet>();
        const int threads = ThreadCount(options.Threads);

        if (options.PauseProcess)
            process->Suspend();

        if (status)
        {
            result->Type = options.Type;
            result->ValueSize = matcher.Size();
            result->Alignment = std::max(1, options.Alignment);
            result->Format.Hex = options.Hex;
            result->Format.Unicode = options.Unicode;
            result->Format.PointerSize = process->PointerSize();
            result->Format.Length = options.Type == ValueType::String ? static_cast<int>(Utf8ToWide(options.Value).size()) : matcher.Size();
            if (options.Type == ValueType::String && !options.Unicode)
                result->Format.Length = matcher.Size();
            if (previous)
            {
                result->Type = previous->Type;
                result->ValueSize = previous->ValueSize;
                result->Alignment = previous->Alignment;
                result->Format = previous->Format;
            }
            if (options.Type == ValueType::String || options.Type == ValueType::ByteArray)
                result->Alignment = 1;

            for (int i = 0; i < threads; ++i)
            {
                auto file = std::make_unique<TempFile>();
                if (!file->Create())
                {
                    status = Status::Fail(ErrorCode::ScanStoreFailed, "temporary file not created");
                    break;
                }
                result->Files.push_back(std::move(file));
            }
        }

        std::mutex mergeLock;
        std::vector<Segment> segments;
        std::vector<Snapshot> snapshots;
        std::atomic<bool> storeFailed{ false };
        const int valueSize = result->ValueSize;
        const int alignment = result->Alignment;

        if (status && !previous)
        {
            std::vector<WorkChunk> chunks;
            std::uint64_t total = 0;
            const std::uint64_t start = std::max(options.Start, process->LowestAddress());
            const std::uint64_t stop = std::min(options.Stop, process->HighestAddress());
            for (const Target::RegionInfo& region : process->Regions(start, stop))
            {
                if (!PassesFilter(region, options))
                    continue;
                const std::uint64_t regionStart = std::max(region.Base, start);
                const std::uint64_t regionEnd = std::min(region.End(), stop);
                if (regionEnd <= regionStart)
                    continue;
                for (std::uint64_t at = regionStart; at < regionEnd; at += kChunkBytes)
                {
                    WorkChunk chunk;
                    chunk.Base = at;
                    chunk.Size = std::min(kChunkBytes, regionEnd - at);
                    chunk.ReadEnd = std::min(at + chunk.Size + static_cast<std::uint64_t>(valueSize) - 1, region.End());
                    chunks.push_back(chunk);
                    total += chunk.Size;
                }
            }
            if (chunks.empty())
                status = Status::Fail(ErrorCode::ScanRangeEmpty);
            m_total = std::max<std::uint64_t>(1, total);

            const bool snapshotMode = options.Mode == Compare::Unknown;
            std::atomic<std::size_t> next{ 0 };
            const auto worker = [&](int index)
                {
                    TempFile& file = *result->Files[static_cast<std::size_t>(index)];
                    std::vector<std::uint8_t> buffer(kChunkBytes + 64);
                    std::vector<std::uint8_t> record(8 + 2 * static_cast<std::size_t>(valueSize));
                    std::vector<Segment> localSegments;
                    std::vector<Snapshot> localSnapshots;
                    for (;;)
                    {
                        const std::size_t item = next.fetch_add(1);
                        if (item >= chunks.size() || m_cancel || storeFailed)
                            break;
                        const WorkChunk& chunk = chunks[item];
                        const std::size_t readSize = static_cast<std::size_t>(chunk.ReadEnd - chunk.Base);
                        if (process->Read(chunk.Base, buffer.data(), readSize) == 0)
                        {
                            m_done += chunk.Size;
                            continue;
                        }
                        if (snapshotMode)
                        {
                            Snapshot snapshot;
                            snapshot.Order = item;
                            snapshot.Base = chunk.Base;
                            snapshot.Size = readSize;
                            snapshot.File = index;
                            snapshot.Offset = file.Size();
                            const std::uint64_t usable = readSize >= static_cast<std::size_t>(valueSize) ? readSize - static_cast<std::size_t>(valueSize) : 0;
                            const std::uint64_t positions = readSize >= static_cast<std::size_t>(valueSize) ? std::min<std::uint64_t>(chunk.Size, usable + 1) : 0;
                            snapshot.Positions = positions == 0 ? 0 : (positions - 1) / static_cast<std::uint64_t>(alignment) + 1;
                            if (!file.Append(buffer.data(), readSize))
                                storeFailed = true;
                            localSnapshots.push_back(snapshot);
                            m_done += chunk.Size;
                            continue;
                        }
                        Segment segment;
                        segment.Order = item;
                        segment.File = index;
                        segment.Offset = file.Size();
                        const std::uint64_t firstOffset = (alignment - (chunk.Base % static_cast<std::uint64_t>(alignment))) % static_cast<std::uint64_t>(alignment);
                        for (std::uint64_t offset = firstOffset; offset < chunk.Size && offset + static_cast<std::uint64_t>(valueSize) <= readSize; offset += static_cast<std::uint64_t>(alignment))
                        {
                            const std::uint8_t* value = buffer.data() + offset;
                            if (!matcher.Test(value, value, value))
                                continue;
                            const std::uint64_t address = chunk.Base + offset;
                            std::memcpy(record.data(), &address, 8);
                            std::memcpy(record.data() + 8, value, static_cast<std::size_t>(valueSize));
                            std::memcpy(record.data() + 8 + valueSize, value, static_cast<std::size_t>(valueSize));
                            if (!file.Append(record.data(), record.size()))
                            {
                                storeFailed = true;
                                break;
                            }
                            ++segment.Count;
                        }
                        if (segment.Count)
                            localSegments.push_back(segment);
                        m_done += chunk.Size;
                    }
                    if (!file.Flush())
                        storeFailed = true;
                    std::lock_guard lock(mergeLock);
                    segments.insert(segments.end(), localSegments.begin(), localSegments.end());
                    snapshots.insert(snapshots.end(), localSnapshots.begin(), localSnapshots.end());
                };
            if (status)
            {
                std::vector<std::thread> pool;
                for (int i = 0; i < threads; ++i)
                    pool.emplace_back(worker, i);
                for (std::thread& thread : pool)
                    thread.join();
                result->IsSnapshot = snapshotMode;
            }
        }
        else if (status && previous)
        {
            const bool fromSnapshot = previous->IsSnapshot;
            std::uint64_t total = fromSnapshot ? 0 : previous->Count;
            if (fromSnapshot)
            {
                for (const Snapshot& snapshot : previous->Snapshots)
                    total += snapshot.Size;
            }
            m_total = std::max<std::uint64_t>(1, total);
            std::atomic<std::size_t> next{ 0 };
            const std::size_t items = fromSnapshot ? previous->Snapshots.size() : previous->Segments.size();
            const auto worker = [&](int index)
                {
                    TempFile& file = *result->Files[static_cast<std::size_t>(index)];
                    const std::size_t recordSize = previous->RecordSize();
                    std::vector<std::uint8_t> record(8 + 2 * static_cast<std::size_t>(valueSize));
                    std::vector<std::uint8_t> input;
                    std::vector<std::uint8_t> window(kWindowBytes + 64);
                    std::vector<std::uint8_t> current;
                    std::vector<Segment> localSegments;
                    for (;;)
                    {
                        const std::size_t item = next.fetch_add(1);
                        if (item >= items || m_cancel || storeFailed)
                            break;
                        Segment segment;
                        segment.Order = item;
                        segment.File = index;
                        segment.Offset = file.Size();
                        if (fromSnapshot)
                        {
                            const Snapshot& snapshot = previous->Snapshots[item];
                            input.resize(static_cast<std::size_t>(snapshot.Size));
                            current.resize(static_cast<std::size_t>(snapshot.Size));
                            if (!previous->Files[static_cast<std::size_t>(snapshot.File)]->ReadAt(snapshot.Offset, input.data(), input.size()) ||
                                process->Read(snapshot.Base, current.data(), current.size()) == 0)
                            {
                                m_done += snapshot.Size;
                                continue;
                            }
                            for (std::uint64_t position = 0; position < snapshot.Positions; ++position)
                            {
                                const std::size_t offset = static_cast<std::size_t>(position * static_cast<std::uint64_t>(alignment));
                                if (offset + static_cast<std::size_t>(valueSize) > current.size())
                                    break;
                                const std::uint8_t* now = current.data() + offset;
                                const std::uint8_t* before = input.data() + offset;
                                if (!matcher.Test(now, before, before))
                                    continue;
                                const std::uint64_t address = snapshot.Base + offset;
                                std::memcpy(record.data(), &address, 8);
                                std::memcpy(record.data() + 8, now, static_cast<std::size_t>(valueSize));
                                std::memcpy(record.data() + 8 + valueSize, before, static_cast<std::size_t>(valueSize));
                                if (!file.Append(record.data(), record.size()))
                                {
                                    storeFailed = true;
                                    break;
                                }
                                ++segment.Count;
                            }
                            m_done += snapshot.Size;
                        }
                        else
                        {
                            const Segment& source = previous->Segments[item];
                            std::uint64_t windowBase = 0;
                            std::size_t windowSize = 0;
                            for (std::uint64_t first = 0; first < source.Count && !m_cancel; first += kNextBatchRecords)
                            {
                                const std::uint64_t batch = std::min<std::uint64_t>(kNextBatchRecords, source.Count - first);
                                input.resize(static_cast<std::size_t>(batch) * recordSize);
                                if (!previous->Files[static_cast<std::size_t>(source.File)]->ReadAt(source.Offset + first * recordSize, input.data(), input.size()))
                                {
                                    storeFailed = true;
                                    break;
                                }
                                for (std::uint64_t i = 0; i < batch; ++i)
                                {
                                    const std::uint8_t* raw = input.data() + i * recordSize;
                                    std::uint64_t address = 0;
                                    std::memcpy(&address, raw, 8);
                                    if (address < windowBase || address + static_cast<std::uint64_t>(valueSize) > windowBase + windowSize)
                                    {
                                        windowBase = address;
                                        windowSize = process->Read(address, window.data(), kWindowBytes);
                                        if (windowSize == 0)
                                        {
                                            windowBase = 0;
                                            continue;
                                        }
                                    }
                                    const std::uint8_t* now = window.data() + (address - windowBase);
                                    const std::uint8_t* before = raw + 8;
                                    const std::uint8_t* initial = raw + 8 + previous->ValueSize;
                                    if (!matcher.Test(now, before, initial))
                                        continue;
                                    std::memcpy(record.data(), &address, 8);
                                    std::memcpy(record.data() + 8, now, static_cast<std::size_t>(valueSize));
                                    std::memcpy(record.data() + 8 + valueSize, initial, static_cast<std::size_t>(valueSize));
                                    if (!file.Append(record.data(), record.size()))
                                    {
                                        storeFailed = true;
                                        break;
                                    }
                                    ++segment.Count;
                                }
                                m_done += batch;
                            }
                        }
                        if (segment.Count)
                            localSegments.push_back(segment);
                    }
                    if (!file.Flush())
                        storeFailed = true;
                    std::lock_guard lock(mergeLock);
                    segments.insert(segments.end(), localSegments.begin(), localSegments.end());
                };
            std::vector<std::thread> pool;
            for (int i = 0; i < threads; ++i)
                pool.emplace_back(worker, i);
            for (std::thread& thread : pool)
                thread.join();
        }

        if (options.PauseProcess)
            process->Resume();

        if (status && storeFailed)
            status = Status::Fail(ErrorCode::ScanStoreFailed, "the disk may be full");

        std::shared_ptr<const ResultSet> finished;
        if (status && !m_cancel)
        {
            std::sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) { return a.Order < b.Order; });
            std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& a, const Snapshot& b) { return a.Order < b.Order; });
            std::uint64_t count = 0;
            for (Segment& segment : segments)
            {
                segment.Before = count;
                count += segment.Count;
            }
            for (Snapshot& snapshot : snapshots)
            {
                snapshot.Before = count;
                count += snapshot.Positions;
            }
            result->Segments = std::move(segments);
            result->Snapshots = std::move(snapshots);
            result->Count = count;
            finished = result;
        }

        std::lock_guard lock(m_lock);
        m_finished = std::move(finished);
        m_finishedError = m_cancel && status ? Status{} : status;
        m_finishedReady = true;
    }

    std::vector<std::uint64_t> FindPattern(const Target::Process& process, const Pattern& pattern, std::uint64_t start, std::uint64_t stop, bool executableOnly,
        std::size_t limit, bool includeReadOnly)
    {
        std::vector<std::uint64_t> found;
        if (pattern.Empty())
            return found;
        std::vector<WorkChunk> chunks;
        const std::uint64_t size = pattern.Bytes.size();
        for (const Target::RegionInfo& region : process.Regions(std::max(start, process.LowestAddress()), std::min(stop, process.HighestAddress())))
        {
            if (!region.Readable() || (executableOnly && !region.Executable()) || (!includeReadOnly && !region.Writable() && !region.Executable()))
                continue;
            const std::uint64_t regionStart = std::max(region.Base, start);
            const std::uint64_t regionEnd = std::min(region.End(), stop);
            for (std::uint64_t at = regionStart; at < regionEnd; at += kChunkBytes)
            {
                WorkChunk chunk;
                chunk.Base = at;
                chunk.Size = std::min(kChunkBytes, regionEnd - at);
                chunk.ReadEnd = std::min(at + chunk.Size + size - 1, region.End());
                chunks.push_back(chunk);
            }
        }

        std::vector<std::vector<std::uint64_t>> perChunk(chunks.size());
        std::atomic<std::size_t> next{ 0 };
        std::atomic<std::size_t> total{ 0 };
        const std::uint8_t firstByte = pattern.Bytes[0];
        const bool firstFixed = pattern.Mask[0] == 0xFF;
        const auto worker = [&]
            {
                std::vector<std::uint8_t> buffer(kChunkBytes + 256);
                for (;;)
                {
                    const std::size_t item = next.fetch_add(1);
                    if (item >= chunks.size() || (limit && total >= limit))
                        break;
                    const WorkChunk& chunk = chunks[item];
                    const std::size_t readSize = static_cast<std::size_t>(chunk.ReadEnd - chunk.Base);
                    if (readSize < size || process.Read(chunk.Base, buffer.data(), readSize) == 0)
                        continue;
                    const std::size_t last = std::min<std::size_t>(static_cast<std::size_t>(chunk.Size), readSize - static_cast<std::size_t>(size) + 1);
                    for (std::size_t offset = 0; offset < last; ++offset)
                    {
                        if (firstFixed)
                        {
                            const void* hit = std::memchr(buffer.data() + offset, firstByte, last - offset);
                            if (!hit)
                                break;
                            offset = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - buffer.data());
                        }
                        if (pattern.Matches(buffer.data() + offset))
                        {
                            perChunk[item].push_back(chunk.Base + offset);
                            ++total;
                        }
                    }
                }
            };
        const int threads = chunks.size() > 8 ? ThreadCount(0) : 1;
        std::vector<std::thread> pool;
        for (int i = 0; i < threads; ++i)
            pool.emplace_back(worker);
        for (std::thread& thread : pool)
            thread.join();
        for (const auto& hits : perChunk)
        {
            for (std::uint64_t address : hits)
            {
                found.push_back(address);
                if (limit && found.size() >= limit)
                    return found;
            }
        }
        return found;
    }
}
