#include "Scan/Scanner.h"

#include "Core/MainThread.h"
#include "Core/Paths.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Scan/Obscured.h"
#include "Scan/ScanStore.h"
#include "Scan/Workers.h"
#include "Target/Process.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <functional>
#include <type_traits>

#include <windows.h>

namespace Manifold::Scan
{
    namespace
    {
        constexpr std::uint64_t kChunkBytes = 2ull * 1024 * 1024;
        constexpr std::uint64_t kNextBatchRecords = 32768;
        constexpr std::uint64_t kWindowBytes = 64 * 1024;

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

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
            if ((options.SkipNoCache && region.NoCache()) || (options.SkipWriteCombine && region.WriteCombine()))
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

        // ∑ Changed, Unchanged and Same as first ask whether the memory changed, so a NaN equals a NaN with the same bits. By float rules
        //   a NaN never equals itself: 0xFFFFFFFF (int -1) showed in every Changed scan and never in Unchanged.
        template <class T>
        bool SameFloat(T now, T previous)
        {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            return now == previous || (now != now && std::bit_cast<Bits>(now) == std::bit_cast<Bits>(previous));
        }

        class Matcher
        {
        public:
            Status Build(const ScanOptions& options, int pointerSize)
            {
                m_kind = options.Kind;
                m_mode = options.Mode;
                m_floats = options.Floats;
                if (m_kind == ScanKind::Grouped || m_kind == ScanKind::Custom)
                {
                    if (!CompareAvailable(m_kind, m_mode, options.CustomName))
                        return Status::Fail(ErrorCode::ScanCompareUnavailable, CompareName(m_mode));
                    m_type = ValueType::ByteArray;
                    if (m_kind == ScanKind::Custom && FindObscured(options.CustomName))
                        return BuildObscured(options, pointerSize);
                    return m_kind == ScanKind::Grouped ? BuildGroup(options, pointerSize) : BuildCustom(options);
                }
                m_type = options.Type;
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
                m_numberType = m_type;
                m_numberSize = m_size;
                return ParseNumbers(options, options.Value, options.Value2, format);
            }

            Status ParseNumbers(const ScanOptions& options, const std::string& valueA, const std::string& valueB, const ValueFormat& format)
            {
                if (CompareNeedsValue(m_mode))
                {
                    if (TrimView(valueA).empty())
                        return Status::Fail(ErrorCode::ScanNoValue);
                    const auto parse = [&](const std::string& text, double& asDouble, std::int64_t& asInteger) -> bool
                        {
                            if (IsFloating(m_numberType) && !options.Hex)
                            {
                                const auto value = ParseDouble(text);
                                if (!value)
                                    return false;
                                asDouble = *value;
                                asInteger = static_cast<std::int64_t>(*value);
                                return true;
                            }
                            const auto bytes = ParseValue(m_numberType, text, format);
                            if (!bytes)
                                return false;
                            if (IsFloating(m_numberType))
                            {
                                asDouble = *AsDouble(m_numberType, *bytes, true);
                                asInteger = static_cast<std::int64_t>(asDouble);
                                return true;
                            }
                            std::uint64_t raw = 0;
                            std::memcpy(&raw, bytes->data(), std::min<std::size_t>(8, bytes->size()));
                            asInteger = m_numberSigned ? SignExtend(raw) : static_cast<std::int64_t>(raw);
                            asDouble = static_cast<double>(asInteger);
                            return true;
                        };
                    if (!parse(valueA, m_doubleA, m_intA))
                        return Status::Fail(ErrorCode::ScanBadValue, valueA);
                    if (CompareNeedsTwoValues(m_mode))
                    {
                        if (!parse(valueB, m_doubleB, m_intB))
                            return Status::Fail(ErrorCode::ScanBadValue, valueB);
                        if (m_doubleA > m_doubleB)
                            std::swap(m_doubleA, m_doubleB);
                        if (m_intA > m_intB)
                            std::swap(m_intA, m_intB);
                    }
                    const int decimals = IsFloating(m_numberType) && !options.Hex ? DecimalsOf(valueA) : 0;
                    m_step = std::pow(10.0, -decimals);
                    if (options.Hex && IsFloating(m_numberType))
                        m_floats = FloatMode::Exact;
                }
                return Status::Success();
            }

            Status BuildGroup(const ScanOptions& options, int pointerSize)
            {
                const bool needsSpec = m_mode == Compare::Exact || options.BlockSize <= 0;
                if (needsSpec)
                {
                    std::string error;
                    auto spec = ParseGroup(options.Value, options.Hex, pointerSize, error);
                    if (!spec)
                        return Status::Fail(ErrorCode::ScanGroupSyntax, error);
                    if (options.BlockSize > 0 && spec->BlockSize != options.BlockSize)
                        return Status::Fail(ErrorCode::ScanGroupSyntax,
                            Manifold::Format("the group covers %d bytes, the results %d; a next scan needs the same size", spec->BlockSize, options.BlockSize));
                    m_group = std::make_shared<const GroupSpec>(std::move(*spec));
                    m_size = m_group->BlockSize;
                    return Status::Success();
                }
                m_size = options.BlockSize;
                return Status::Success();
            }

            Status BuildObscured(const ScanOptions& options, int pointerSize)
            {
                m_obscured = FindObscured(options.CustomName);
                m_layout = ObscuredLayoutOf(m_obscured->Kind, pointerSize);
                m_size = m_layout.Size;
                if (options.CustomSize > 0 && options.CustomSize != m_size)
                    return Status::Fail(ErrorCode::ScanCustomMissing, options.CustomName);
                m_numberType = m_obscured->Plain;
                m_numberSize = ObscuredComponentSize(*m_obscured);
                m_numberSigned = m_obscured->Signed;
                if (!ObscuredScalar(*m_obscured))
                    return Status::Success();
                const auto boolText = [&](const std::string& text) -> std::string
                    {
                        if (m_obscured->Kind != ObscuredKind::Bool)
                            return text;
                        if (EqualsNoCase(TrimView(text), "true"))
                            return "1";
                        if (EqualsNoCase(TrimView(text), "false"))
                            return "0";
                        return text;
                    };
                ValueFormat format;
                format.Hex = options.Hex;
                format.PointerSize = pointerSize;
                return ParseNumbers(options, boolText(options.Value), boolText(options.Value2), format);
            }

            Status BuildCustom(const ScanOptions& options)
            {
                m_size = options.CustomSize;
                if (m_size <= 0 || m_size > kGroupMaxBytes)
                    return Status::Fail(ErrorCode::ScanCustomMissing, options.CustomName);
                if (m_mode == Compare::Exact)
                {
                    if (options.CustomBytes.size() != static_cast<std::size_t>(m_size))
                        return Status::Fail(ErrorCode::ScanBadValue, options.Value);
                    m_patternA = options.CustomBytes;
                    m_patternB = options.CustomBytes;
                    m_mask.assign(m_patternA.size(), 0xFF);
                }
                return Status::Success();
            }

            int Size() const { return m_size; }
            ValueType Type() const { return m_type; }
            Compare Mode() const { return m_mode; }
            std::shared_ptr<const GroupSpec> Group() const { return m_group; }
            bool NeedsPrevious() const
            {
                return m_mode == Compare::Increased || m_mode == Compare::IncreasedBy || m_mode == Compare::Decreased || m_mode == Compare::DecreasedBy ||
                       m_mode == Compare::Changed || m_mode == Compare::Unchanged;
            }

            bool Test(const std::uint8_t* now, const std::uint8_t* previous, const std::uint8_t* first) const
            {
                if (m_obscured)
                    return TestObscured(now, previous, first);
                if (m_kind == ScanKind::Grouped)
                {
                    switch (m_mode)
                    {
                    case Compare::Exact: return m_group && m_group->Matches(now);
                    case Compare::Changed: return std::memcmp(now, previous, static_cast<std::size_t>(m_size)) != 0;
                    case Compare::Unchanged: return std::memcmp(now, previous, static_cast<std::size_t>(m_size)) == 0;
                    case Compare::SameAsFirst: return std::memcmp(now, first, static_cast<std::size_t>(m_size)) == 0;
                    default: return true;
                    }
                }
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

            // ∑ The struct in memory changes with every new key, the value does not: compares go to the decoded
            //   value, and only a struct that reads as a set ACTk value counts, which keeps zeroed memory out.
            bool TestObscured(const std::uint8_t* now, const std::uint8_t* previous, const std::uint8_t* first) const
            {
                std::uint8_t plainNow[16]{};
                std::uint8_t plainPrevious[16]{};
                std::uint8_t plainFirst[16]{};
                if (ObscuredDecode(*m_obscured, m_layout, now, plainNow, true) != ObscuredState::Plain)
                    return false;
                if (NeedsPrevious() && ObscuredDecode(*m_obscured, m_layout, previous, plainPrevious, false) == ObscuredState::Invalid)
                    return false;
                if (m_mode == Compare::SameAsFirst && ObscuredDecode(*m_obscured, m_layout, first, plainFirst, false) == ObscuredState::Invalid)
                    return false;
                if (!ObscuredScalar(*m_obscured))
                {
                    const std::size_t size = static_cast<std::size_t>(ObscuredPlainSize(*m_obscured));
                    switch (m_mode)
                    {
                    case Compare::Unknown: return true;
                    case Compare::Changed: return std::memcmp(plainNow, plainPrevious, size) != 0;
                    case Compare::Unchanged: return std::memcmp(plainNow, plainPrevious, size) == 0;
                    case Compare::SameAsFirst: return std::memcmp(plainNow, plainFirst, size) == 0;
                    default: return false;
                    }
                }
                const bool sign = m_obscured->Signed;
                switch (m_obscured->Plain)
                {
                case ValueType::Byte: return sign ? TestInteger<std::int8_t>(plainNow, plainPrevious, plainFirst) : TestInteger<std::uint8_t>(plainNow, plainPrevious, plainFirst);
                case ValueType::TwoBytes: return sign ? TestInteger<std::int16_t>(plainNow, plainPrevious, plainFirst) : TestInteger<std::uint16_t>(plainNow, plainPrevious, plainFirst);
                case ValueType::FourBytes: return sign ? TestInteger<std::int32_t>(plainNow, plainPrevious, plainFirst) : TestInteger<std::uint32_t>(plainNow, plainPrevious, plainFirst);
                case ValueType::EightBytes: return sign ? TestInteger<std::int64_t>(plainNow, plainPrevious, plainFirst) : TestInteger<std::uint64_t>(plainNow, plainPrevious, plainFirst);
                case ValueType::Float: return TestFloat<float>(plainNow, plainPrevious, plainFirst);
                case ValueType::Double: return TestFloat<double>(plainNow, plainPrevious, plainFirst);
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
                {
                    switch (m_mode)
                    {
                    case Compare::Unknown: return true;
                    case Compare::Changed: return !SameFloat(now, Load<T>(previousBytes));
                    case Compare::Unchanged: return SameFloat(now, Load<T>(previousBytes));
                    case Compare::SameAsFirst: return SameFloat(now, Load<T>(firstBytes));
                    default: return false;
                    }
                }
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

            template <class Visit>
            bool Scan(const std::uint8_t* now, const std::uint8_t* previous, const std::uint8_t* first, std::size_t begin, std::size_t end, std::size_t stride, Visit& visit) const
            {
                if (m_kind == ScanKind::Single)
                {
                    switch (m_type)
                    {
                    case ValueType::Byte: return ScanInteger<std::int8_t>(now, previous, first, begin, end, stride, visit);
                    case ValueType::TwoBytes: return ScanInteger<std::int16_t>(now, previous, first, begin, end, stride, visit);
                    case ValueType::FourBytes: return ScanInteger<std::int32_t>(now, previous, first, begin, end, stride, visit);
                    case ValueType::EightBytes: return ScanInteger<std::int64_t>(now, previous, first, begin, end, stride, visit);
                    case ValueType::Pointer:
                        return m_size == 4 ? ScanInteger<std::int32_t>(now, previous, first, begin, end, stride, visit)
                                           : ScanInteger<std::int64_t>(now, previous, first, begin, end, stride, visit);
                    case ValueType::Float: return ScanFloat<float>(now, previous, first, begin, end, stride, visit);
                    case ValueType::Double: return ScanFloat<double>(now, previous, first, begin, end, stride, visit);
                    default: break;
                    }
                }
                for (std::size_t offset = begin; offset < end; offset += stride)
                {
                    if (Test(now + offset, previous + offset, first + offset) && !visit(offset))
                        return false;
                }
                return true;
            }

        private:
            static constexpr std::size_t kBlock = 64;

            template <class T, class Visit, class Pred>
            static bool Each(const std::uint8_t* data, std::size_t begin, std::size_t end, std::size_t stride, Visit& visit, Pred pred)
            {
                std::size_t offset = begin;
                if (stride == sizeof(T))
                {
                    for (; offset < end && end - offset >= kBlock * sizeof(T); offset += kBlock * sizeof(T))
                    {
                        const std::uint8_t* block = data + offset;
                        unsigned any = 0;
                        for (std::size_t i = 0; i < kBlock; ++i)
                            any |= pred(Load<T>(block + i * sizeof(T))) ? 1u : 0u;
                        if (!any)
                            continue;
                        for (std::size_t i = 0; i < kBlock; ++i)
                        {
                            if (pred(Load<T>(block + i * sizeof(T))) && !visit(offset + i * sizeof(T)))
                                return false;
                        }
                    }
                }
                for (; offset < end; offset += stride)
                {
                    if (pred(Load<T>(data + offset)) && !visit(offset))
                        return false;
                }
                return true;
            }

            template <class T, class Visit, class Pred>
            static bool EachPair(const std::uint8_t* data, const std::uint8_t* other, std::size_t begin, std::size_t end, std::size_t stride, Visit& visit, Pred pred)
            {
                std::size_t offset = begin;
                if (stride == sizeof(T))
                {
                    for (; offset < end && end - offset >= kBlock * sizeof(T); offset += kBlock * sizeof(T))
                    {
                        const std::uint8_t* block = data + offset;
                        const std::uint8_t* against = other + offset;
                        unsigned any = 0;
                        for (std::size_t i = 0; i < kBlock; ++i)
                            any |= pred(Load<T>(block + i * sizeof(T)), Load<T>(against + i * sizeof(T))) ? 1u : 0u;
                        if (!any)
                            continue;
                        for (std::size_t i = 0; i < kBlock; ++i)
                        {
                            if (pred(Load<T>(block + i * sizeof(T)), Load<T>(against + i * sizeof(T))) && !visit(offset + i * sizeof(T)))
                                return false;
                        }
                    }
                }
                for (; offset < end; offset += stride)
                {
                    if (pred(Load<T>(data + offset), Load<T>(other + offset)) && !visit(offset))
                        return false;
                }
                return true;
            }

            template <class T>
            static T Wrapped(T a, T b, bool add)
            {
                using U = std::make_unsigned_t<T>;
                return static_cast<T>(add ? static_cast<U>(static_cast<U>(a) + static_cast<U>(b)) : static_cast<U>(static_cast<U>(a) - static_cast<U>(b)));
            }

            template <class T, class Visit>
            bool ScanInteger(const std::uint8_t* now, const std::uint8_t* previous, const std::uint8_t* first, std::size_t begin, std::size_t end, std::size_t stride, Visit& visit) const
            {
                const T a = static_cast<T>(m_intA);
                const std::int64_t low = m_intA;
                const std::int64_t high = m_intB;
                switch (m_mode)
                {
                case Compare::Exact: return Each<T>(now, begin, end, stride, visit, [a](T v) { return v == a; });
                case Compare::Bigger: return Each<T>(now, begin, end, stride, visit, [low](T v) { return static_cast<std::int64_t>(v) > low; });
                case Compare::Smaller: return Each<T>(now, begin, end, stride, visit, [low](T v) { return static_cast<std::int64_t>(v) < low; });
                case Compare::Between:
                    return Each<T>(now, begin, end, stride, visit, [low, high](T v) { return static_cast<std::int64_t>(v) >= low && static_cast<std::int64_t>(v) <= high; });
                case Compare::Unknown: return Each<T>(now, begin, end, stride, visit, [](T) { return true; });
                case Compare::Increased: return EachPair<T>(now, previous, begin, end, stride, visit, [](T v, T p) { return v > p; });
                case Compare::Decreased: return EachPair<T>(now, previous, begin, end, stride, visit, [](T v, T p) { return v < p; });
                case Compare::IncreasedBy: return EachPair<T>(now, previous, begin, end, stride, visit, [a](T v, T p) { return v == Wrapped<T>(p, a, true); });
                case Compare::DecreasedBy: return EachPair<T>(now, previous, begin, end, stride, visit, [a](T v, T p) { return v == Wrapped<T>(p, a, false); });
                case Compare::Changed: return EachPair<T>(now, previous, begin, end, stride, visit, [](T v, T p) { return v != p; });
                case Compare::Unchanged: return EachPair<T>(now, previous, begin, end, stride, visit, [](T v, T p) { return v == p; });
                case Compare::SameAsFirst: return EachPair<T>(now, first, begin, end, stride, visit, [](T v, T p) { return v == p; });
                }
                return true;
            }

            template <class T, class Visit, class Equal>
            bool ScanFloatWith(const std::uint8_t* now, const std::uint8_t* previous, const std::uint8_t* first, std::size_t begin, std::size_t end, std::size_t stride, Visit& visit,
                Equal equal) const
            {
                const double a = m_doubleA;
                const double b = m_doubleB;
                switch (m_mode)
                {
                case Compare::Exact: return Each<T>(now, begin, end, stride, visit, [a, equal](T v) { return equal(static_cast<double>(v), a); });
                case Compare::Bigger: return Each<T>(now, begin, end, stride, visit, [a](T v) { return static_cast<double>(v) > a; });
                case Compare::Smaller: return Each<T>(now, begin, end, stride, visit, [a](T v) { return static_cast<double>(v) < a; });
                case Compare::Between: return Each<T>(now, begin, end, stride, visit, [a, b](T v) { return static_cast<double>(v) >= a && static_cast<double>(v) <= b; });
                case Compare::Unknown: return Each<T>(now, begin, end, stride, visit, [](T) { return true; });
                case Compare::Increased: return EachPair<T>(now, previous, begin, end, stride, visit, [](T v, T p) { return v > p; });
                case Compare::Decreased: return EachPair<T>(now, previous, begin, end, stride, visit, [](T v, T p) { return v < p; });
                case Compare::IncreasedBy:
                    return EachPair<T>(now, previous, begin, end, stride, visit, [a, equal](T v, T p) { return equal(static_cast<double>(v), static_cast<double>(p) + a); });
                case Compare::DecreasedBy:
                    return EachPair<T>(now, previous, begin, end, stride, visit, [a, equal](T v, T p) { return equal(static_cast<double>(v), static_cast<double>(p) - a); });
                case Compare::Changed: return EachPair<T>(now, previous, begin, end, stride, visit, [](T v, T p) { return !SameFloat(v, p); });
                case Compare::Unchanged: return EachPair<T>(now, previous, begin, end, stride, visit, [](T v, T p) { return SameFloat(v, p); });
                case Compare::SameAsFirst: return EachPair<T>(now, first, begin, end, stride, visit, [](T v, T p) { return SameFloat(v, p); });
                }
                return true;
            }

            template <class T, class Visit>
            bool ScanFloat(const std::uint8_t* now, const std::uint8_t* previous, const std::uint8_t* first, std::size_t begin, std::size_t end, std::size_t stride, Visit& visit) const
            {
                const double step = m_step;
                switch (m_floats)
                {
                case FloatMode::Exact:
                    return ScanFloatWith<T>(now, previous, first, begin, end, stride, visit, [](double value, double wanted) {
                        if constexpr (std::is_same_v<T, float>)
                            return static_cast<float>(value) == static_cast<float>(wanted);
                        else
                            return value == wanted;
                    });
                case FloatMode::Truncated:
                    return ScanFloatWith<T>(now, previous, first, begin, end, stride, visit, [step](double value, double wanted) {
                        return wanted >= 0.0 ? (value >= wanted && value < wanted + step) : (value <= wanted && value > wanted - step);
                    });
                case FloatMode::Rounded:
                default:
                    return ScanFloatWith<T>(now, previous, first, begin, end, stride, visit,
                        [step](double value, double wanted) { return std::fabs(value - wanted) <= step * 0.5 + std::fabs(wanted) * 1e-12; });
                }
            }

            std::int64_t SignExtend(std::uint64_t raw) const
            {
                switch (m_numberSize)
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
                    return m_numberType == ValueType::Float ? static_cast<float>(value) == static_cast<float>(wanted) : value == wanted;
                case FloatMode::Truncated:
                    return wanted >= 0.0 ? (value >= wanted && value < wanted + m_step) : (value <= wanted && value > wanted - m_step);
                case FloatMode::Rounded:
                default:
                    return std::fabs(value - wanted) <= m_step * 0.5 + std::fabs(wanted) * 1e-12;
                }
            }

            ScanKind m_kind = ScanKind::Single;
            ValueType m_type = ValueType::FourBytes;
            Compare m_mode = Compare::Exact;
            FloatMode m_floats = FloatMode::Rounded;
            std::shared_ptr<const GroupSpec> m_group;
            int m_size = 4;
            ValueType m_numberType = ValueType::FourBytes;
            int m_numberSize = 4;
            bool m_numberSigned = true;
            const ObscuredType* m_obscured = nullptr;
            ObscuredLayout m_layout;
            std::int64_t m_intA = 0;
            std::int64_t m_intB = 0;
            double m_doubleA = 0.0;
            double m_doubleB = 0.0;
            double m_step = 1.0;
            std::vector<std::uint8_t> m_patternA;
            std::vector<std::uint8_t> m_patternB;
            std::vector<std::uint8_t> m_mask;
        };

        constexpr std::size_t kTypeSlots = 16;

        std::size_t Slot(ValueType type)
        {
            return static_cast<std::size_t>(type) % kTypeSlots;
        }

        struct TypedMatchers
        {
            std::vector<ValueType> Types;
            std::optional<Matcher> ByType[kTypeSlots];

            const Matcher* Find(ValueType type) const
            {
                const auto& slot = ByType[Slot(type)];
                return slot ? &*slot : nullptr;
            }
        };

        template <class Write>
        bool ScanTypes(const TypedMatchers& typed, const std::vector<ValueType>& types, const std::uint8_t* now, const std::uint8_t* previous, const std::uint8_t* first,
            std::size_t begin, std::size_t end, std::size_t stride, std::vector<std::vector<std::uint32_t>>& hits, Write& write)
        {
            const std::size_t count = std::min(types.size(), kTypeSlots);
            if (hits.size() < count)
                hits.resize(count);
            for (std::size_t t = 0; t < count; ++t)
            {
                std::vector<std::uint32_t>& list = hits[t];
                list.clear();
                const Matcher* matcher = typed.Find(types[t]);
                if (!matcher)
                    continue;
                auto collect = [&list](std::size_t offset) {
                    list.push_back(static_cast<std::uint32_t>(offset));
                    return true;
                };
                matcher->Scan(now, previous, first, begin, end, stride, collect);
            }
            std::array<std::size_t, kTypeSlots> at{};
            for (;;)
            {
                std::size_t best = count;
                std::uint32_t lowest = UINT32_MAX;
                for (std::size_t t = 0; t < count; ++t)
                {
                    if (at[t] < hits[t].size() && hits[t][at[t]] < lowest)
                    {
                        lowest = hits[t][at[t]];
                        best = t;
                    }
                }
                if (best == count)
                    return true;
                ++at[best];
                if (!write(lowest, types[best]))
                    return false;
            }
        }

        std::vector<ValueType> MaskTypes(std::uint32_t mask)
        {
            std::vector<ValueType> types;
            for (ValueType type : kAllTypes)
            {
                if (mask & AllTypeBit(type))
                    types.push_back(type);
            }
            return types;
        }

        bool Fractional(std::string_view text)
        {
            return TrimView(text).find_first_of(".,") != std::string_view::npos;
        }

        Status BuildAll(const ScanOptions& options, int pointerSize, const std::vector<ValueType>& types, TypedMatchers& out)
        {
            const bool needsValue = CompareNeedsValue(options.Mode);
            const bool fractional = needsValue && !options.Hex && (Fractional(options.Value) || (CompareNeedsTwoValues(options.Mode) && Fractional(options.Value2)));
            Status first;
            for (ValueType type : types)
            {
                if (fractional && !IsFloating(type))
                    continue;
                ScanOptions single = options;
                single.Kind = ScanKind::Single;
                single.Type = type;
                Matcher matcher;
                if (Status built = matcher.Build(single, pointerSize); !built)
                {
                    if (first.Ok())
                        first = built;
                    continue;
                }
                out.ByType[Slot(type)] = std::move(matcher);
                out.Types.push_back(type);
            }
            if (out.Types.empty())
                return first.Ok() || first.Code == ErrorCode::ScanBadValue ? Status::Fail(ErrorCode::ScanNoTypes) : first;
            return Status::Success();
        }
    }

    class ResultSet
    {
    public:
        ScanKind Kind = ScanKind::Single;
        ValueType Type = ValueType::FourBytes;
        ValueFormat Format;
        int ValueSize = 4;
        int Alignment = 4;
        bool IsSnapshot = false;
        std::vector<ValueType> AllList;
        std::shared_ptr<const GroupSpec> Group;
        std::string CustomName;
        std::vector<std::unique_ptr<ScanStore>> Files;
        std::vector<Segment> Segments;
        std::vector<Snapshot> Snapshots;
        std::uint64_t Count = 0;

        std::size_t TagBytes() const { return Kind == ScanKind::All ? 1 : 0; }
        void AddStorage(ScanStorage& storage) const
        {
            for (const auto& file : Files)
            {
                storage.MemoryBytes += file->MemoryBytes();
                storage.DiskBytes += file->DiskBytes();
            }
        }
        std::uint64_t Multiplicity() const { return Kind == ScanKind::All ? std::max<std::uint64_t>(1, AllList.size()) : 1; }
        std::size_t RecordSize() const { return 8 + TagBytes() + 2 * static_cast<std::size_t>(ValueSize); }

        bool Record(std::uint64_t index, ScanRecord& out) const
        {
            if (index >= Count)
                return false;
            out.Type = Type;
            if (IsSnapshot)
            {
                auto found = std::upper_bound(Snapshots.begin(), Snapshots.end(), index, [](std::uint64_t value, const Snapshot& snapshot) { return value < snapshot.Before; });
                if (found == Snapshots.begin())
                    return false;
                --found;
                const std::uint64_t entry = index - found->Before;
                const std::uint64_t position = entry / Multiplicity();
                if (Kind == ScanKind::All && !AllList.empty())
                    out.Type = AllList[static_cast<std::size_t>(entry % Multiplicity())];
                const std::uint64_t offset = position * static_cast<std::uint64_t>(Alignment);
                out.Address = found->Base + offset;
                out.Previous.resize(static_cast<std::size_t>(ValueSize));
                if (!Files[static_cast<std::size_t>(found->File)]->ReadAt(found->Offset + offset, out.Previous.data(), out.Previous.size()))
                    return false;
                Fit(out.Previous, out.Type);
                out.First = out.Previous;
                return true;
            }
            std::vector<std::uint8_t> raw;
            std::uint64_t got = 0;
            if (!ReadBatch(index, 1, raw, got) || got != 1)
                return false;
            std::memcpy(&out.Address, raw.data(), 8);
            const std::size_t tag = TagBytes();
            if (tag)
                out.Type = static_cast<ValueType>(raw[8]);
            out.Previous.assign(raw.begin() + static_cast<std::ptrdiff_t>(8 + tag), raw.begin() + static_cast<std::ptrdiff_t>(8 + tag + static_cast<std::size_t>(ValueSize)));
            out.First.assign(raw.begin() + static_cast<std::ptrdiff_t>(8 + tag + static_cast<std::size_t>(ValueSize)), raw.end());
            Fit(out.Previous, out.Type);
            Fit(out.First, out.Type);
            return true;
        }

        bool ReadBatch(std::uint64_t index, std::uint64_t most, std::vector<std::uint8_t>& out, std::uint64_t& got) const
        {
            got = 0;
            if (IsSnapshot || index >= Count)
                return false;
            auto found = std::upper_bound(Segments.begin(), Segments.end(), index, [](std::uint64_t value, const Segment& segment) { return value < segment.Before; });
            if (found == Segments.begin())
                return false;
            --found;
            const std::uint64_t position = index - found->Before;
            got = std::min(most, found->Count - position);
            out.resize(static_cast<std::size_t>(got) * RecordSize());
            return Files[static_cast<std::size_t>(found->File)]->ReadAt(found->Offset + position * RecordSize(), out.data(), out.size());
        }

    private:
        void Fit(std::vector<std::uint8_t>& bytes, ValueType type) const
        {
            if (Kind == ScanKind::All)
                bytes.resize(static_cast<std::size_t>(Scan::ValueSize(type, Format)));
        }
    };

    namespace
    {
        std::uint64_t KeyOf(std::uint64_t address, std::uint8_t tag)
        {
            return (address << 8) | tag;
        }

        class SavedLookup
        {
        public:
            explicit SavedLookup(const ResultSet& saved) : m_saved(saved) {}

            const std::uint8_t* Find(std::uint64_t address, ValueType type)
            {
                if (m_saved.IsSnapshot)
                    return FromSnapshot(address);
                if (m_saved.Count == 0)
                    return nullptr;
                const std::uint8_t tag = m_saved.TagBytes() ? static_cast<std::uint8_t>(type) : 0;
                const std::uint64_t key = KeyOf(address, tag);
                if (!m_maxKey)
                {
                    std::uint64_t last = 0;
                    if (!KeyAt(m_saved.Count - 1, last))
                        return nullptr;
                    m_maxKey = last;
                }
                if (key > *m_maxKey)
                    return nullptr;
                if (m_count == 0 || key < m_firstKey || key > m_lastKey)
                {
                    bool placed = false;
                    if (m_count != 0 && key > m_lastKey)
                    {
                        for (int step = 0; step < 4 && m_start + m_count < m_saved.Count; ++step)
                        {
                            if (!Load(m_start + m_count))
                                return nullptr;
                            if (key <= m_lastKey)
                            {
                                placed = true;
                                break;
                            }
                        }
                    }
                    if (!placed)
                    {
                        std::uint64_t low = 0;
                        std::uint64_t high = m_saved.Count;
                        while (low < high)
                        {
                            const std::uint64_t middle = low + (high - low) / 2;
                            std::uint64_t value = 0;
                            if (!KeyAt(middle, value))
                                return nullptr;
                            if (value < key)
                                low = middle + 1;
                            else
                                high = middle;
                        }
                        if (low >= m_saved.Count || !Load(low))
                            return nullptr;
                    }
                }
                std::uint64_t low = 0;
                std::uint64_t high = m_count;
                while (low < high)
                {
                    const std::uint64_t middle = low + (high - low) / 2;
                    if (EntryKey(middle) < key)
                        low = middle + 1;
                    else
                        high = middle;
                }
                if (low >= m_count || EntryKey(low) != key)
                    return nullptr;
                return m_bytes.data() + static_cast<std::size_t>(low) * m_saved.RecordSize() + 8 + m_saved.TagBytes();
            }

        private:
            const std::uint8_t* FromSnapshot(std::uint64_t address)
            {
                const auto& snapshots = m_saved.Snapshots;
                auto found = std::upper_bound(snapshots.begin(), snapshots.end(), address, [](std::uint64_t value, const Snapshot& snapshot) { return value < snapshot.Base; });
                if (found == snapshots.begin())
                    return nullptr;
                --found;
                if (address + static_cast<std::uint64_t>(m_saved.ValueSize) > found->Base + found->Size)
                    return nullptr;
                const std::size_t index = static_cast<std::size_t>(found - snapshots.begin());
                if (index != m_snapshot)
                {
                    m_bytes.resize(static_cast<std::size_t>(found->Size));
                    if (!m_saved.Files[static_cast<std::size_t>(found->File)]->ReadAt(found->Offset, m_bytes.data(), m_bytes.size()))
                    {
                        m_snapshot = static_cast<std::size_t>(-1);
                        return nullptr;
                    }
                    m_snapshot = index;
                }
                return m_bytes.data() + (address - found->Base);
            }

            bool KeyAt(std::uint64_t index, std::uint64_t& key) const
            {
                std::vector<std::uint8_t> raw;
                std::uint64_t got = 0;
                if (!m_saved.ReadBatch(index, 1, raw, got) || got != 1)
                    return false;
                std::uint64_t address = 0;
                std::memcpy(&address, raw.data(), 8);
                key = KeyOf(address, m_saved.TagBytes() ? raw[8] : 0);
                return true;
            }

            std::uint64_t EntryKey(std::uint64_t entry) const
            {
                const std::uint8_t* raw = m_bytes.data() + static_cast<std::size_t>(entry) * m_saved.RecordSize();
                std::uint64_t address = 0;
                std::memcpy(&address, raw, 8);
                return KeyOf(address, m_saved.TagBytes() ? raw[8] : 0);
            }

            bool Load(std::uint64_t index)
            {
                m_count = 0;
                std::uint64_t got = 0;
                if (!m_saved.ReadBatch(index, 4096, m_bytes, got) || got == 0)
                    return false;
                m_start = index;
                m_count = got;
                m_firstKey = EntryKey(0);
                m_lastKey = EntryKey(got - 1);
                return true;
            }

            const ResultSet& m_saved;
            std::vector<std::uint8_t> m_bytes;
            std::size_t m_snapshot = static_cast<std::size_t>(-1);
            std::uint64_t m_start = 0;
            std::uint64_t m_count = 0;
            std::uint64_t m_firstKey = 0;
            std::uint64_t m_lastKey = 0;
            std::optional<std::uint64_t> m_maxKey;
        };

        bool SavedFits(const ResultSet& saved, ScanKind kind, ValueType type, int valueSize, std::string_view custom)
        {
            if (saved.Kind != kind || saved.ValueSize != valueSize)
                return false;
            if (kind == ScanKind::Single)
                return saved.Type == type;
            if (kind == ScanKind::Custom)
                return EqualsNoCase(saved.CustomName, custom);
            return true;
        }
    }

    std::uint32_t AllTypeBit(ValueType type)
    {
        std::uint32_t bit = 1;
        for (ValueType candidate : kAllTypes)
        {
            if (candidate == type)
                return bit;
            bit <<= 1;
        }
        return 0;
    }

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

    bool CompareAvailable(ScanKind kind, Compare compare)
    {
        if (kind != ScanKind::Grouped && kind != ScanKind::Custom)
            return true;
        return compare == Compare::Exact || compare == Compare::Unknown || compare == Compare::Changed || compare == Compare::Unchanged || compare == Compare::SameAsFirst;
    }

    bool CompareAvailable(ScanKind kind, Compare compare, std::string_view customName)
    {
        if (kind == ScanKind::Custom)
        {
            if (const ObscuredType* obscured = FindObscured(customName))
                return ObscuredScalar(*obscured) || compare == Compare::Unknown || compare == Compare::Changed || compare == Compare::Unchanged || compare == Compare::SameAsFirst;
        }
        return CompareAvailable(kind, compare);
    }

    bool CompareUsesPrevious(Compare compare)
    {
        return compare == Compare::Increased || compare == Compare::IncreasedBy || compare == Compare::Decreased || compare == Compare::DecreasedBy ||
               compare == Compare::Changed || compare == Compare::Unchanged;
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

        MemoryBudget::SetLimit(options.MemoryLimitMb);
        ScanOptions effective = options;
        std::shared_ptr<const ResultSet> saved;
        if (firstScan)
        {
            effective.Against = CompareTo::Previous;
            effective.BlockSize = 0;
        }
        else
        {
            effective.Kind = m_current->Kind;
            effective.Type = m_current->Type;
            effective.CustomName = m_current->CustomName;
            if (effective.Kind == ScanKind::Custom)
                effective.CustomSize = m_current->ValueSize;
            effective.BlockSize = effective.Kind == ScanKind::Grouped ? m_current->ValueSize : 0;
            if (effective.Against == CompareTo::Saved && CompareUsesPrevious(effective.Mode))
            {
                auto found = m_saved.find(effective.SavedName);
                if (found == m_saved.end())
                    return Status::Fail(ErrorCode::ScanSavedMissing, effective.SavedName);
                if (!SavedFits(*found->second, m_current->Kind, m_current->Type, m_current->ValueSize, m_current->CustomName))
                    return Status::Fail(ErrorCode::ScanSavedDifferent, effective.SavedName);
                saved = found->second;
            }
            else if (effective.Against == CompareTo::Saved)
            {
                effective.Against = CompareTo::Previous;
            }
        }
        if (!CompareAvailable(effective.Kind, effective.Mode, effective.CustomName))
            return Status::Fail(ErrorCode::ScanCompareUnavailable, CompareName(effective.Mode));

        if (effective.Kind == ScanKind::All)
        {
            TypedMatchers probe;
            const std::vector<ValueType> types = firstScan ? MaskTypes(effective.AllTypes) : m_current->AllList;
            if (types.empty())
                return Status::Fail(ErrorCode::ScanNoTypes);
            if (Status built = BuildAll(effective, process->PointerSize(), types, probe); !built)
                return built;
        }
        else
        {
            Matcher probe;
            if (Status built = probe.Build(effective, process->PointerSize()); !built)
                return built;
        }

        if (m_thread.joinable())
            m_thread.join();
        m_cancel = false;
        m_busy = true;
        m_done = 0;
        m_total = 1;
        m_options = effective;
        m_startedAt = Now();
        std::shared_ptr<const ResultSet> previous = firstScan ? nullptr : m_current;
        m_thread = std::thread(&Scanner::Run, this, std::move(process), effective, std::move(previous), std::move(saved));
        return Status::Success();
    }

    ScanKind Scanner::Kind() const
    {
        return m_current ? m_current->Kind : m_options.Kind;
    }

    std::shared_ptr<const GroupSpec> Scanner::Group() const
    {
        return m_current ? m_current->Group : nullptr;
    }

    std::string Scanner::CustomName() const
    {
        return m_current ? m_current->CustomName : m_options.CustomName;
    }

    bool Scanner::SaveResults(const std::string& name)
    {
        const std::string trimmed = Trim(name);
        if (!m_current || m_busy || trimmed.empty())
            return false;
        m_saved[trimmed] = m_current;
        return true;
    }

    std::vector<std::string> Scanner::SavedNames() const
    {
        std::vector<std::string> names;
        for (const auto& [name, results] : m_saved)
            names.push_back(name);
        return names;
    }

    bool Scanner::HasSaved(std::string_view name) const
    {
        return m_saved.count(std::string(name)) > 0;
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

    ScanStorage Scanner::Storage(bool everything) const
    {
        ScanStorage storage;
        if (m_current)
            m_current->AddStorage(storage);
        if (everything)
        {
            if (m_undo)
                m_undo->AddStorage(storage);
            for (const auto& [name, saved] : m_saved)
            {
                if (saved != m_current && saved != m_undo)
                    saved->AddStorage(storage);
            }
        }
        return storage;
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
        m_saved.clear();
        m_scanNumber = 0;
    }

    void Scanner::Run(std::shared_ptr<Target::Process> process, ScanOptions options, std::shared_ptr<const ResultSet> previous, std::shared_ptr<const ResultSet> saved)
    {
        SetThreadName(L"Scanner");
        PreferFastCores();
        const bool mixed = options.Kind == ScanKind::All;
        Matcher matcher;
        TypedMatchers typed;
        Status status = mixed ? BuildAll(options, process->PointerSize(), previous ? previous->AllList : MaskTypes(options.AllTypes), typed)
                              : matcher.Build(options, process->PointerSize());
        auto result = std::make_shared<ResultSet>();
        const int threads = WorkerCount(options.Threads);

        if (options.PauseProcess)
            process->Suspend();

        if (status)
        {
            result->Kind = options.Kind;
            result->Type = options.Type;
            result->CustomName = options.CustomName;
            result->ValueSize = mixed ? 8 : matcher.Size();
            result->AllList = mixed ? typed.Types : std::vector<ValueType>{};
            result->Group = matcher.Group();
            result->Alignment = std::max(1, options.Alignment);
            result->Format.Hex = options.Hex;
            result->Format.Unicode = options.Unicode;
            result->Format.PointerSize = process->PointerSize();
            result->Format.Length = options.Kind == ScanKind::Single && options.Type == ValueType::String ? static_cast<int>(Utf8ToWide(options.Value).size()) : result->ValueSize;
            if (options.Kind == ScanKind::Single && options.Type == ValueType::String && !options.Unicode)
                result->Format.Length = matcher.Size();
            if (previous)
            {
                result->Kind = previous->Kind;
                result->Type = previous->Type;
                result->CustomName = previous->CustomName;
                result->ValueSize = previous->ValueSize;
                result->Alignment = previous->Alignment;
                result->Format = previous->Format;
                result->AllList = previous->AllList;
                if (!result->Group)
                    result->Group = previous->Group;
            }
            if (options.Kind == ScanKind::Single && (options.Type == ValueType::String || options.Type == ValueType::ByteArray))
                result->Alignment = 1;

            // RAM first, then the scan folder (Scan/ScanStore.h); a file is only made when a store spills
            for (int i = 0; i < threads; ++i)
                result->Files.push_back(std::make_unique<ScanStore>());
        }

        std::mutex mergeLock;
        std::vector<Segment> segments;
        std::vector<Snapshot> snapshots;
        std::atomic<bool> storeFailed{ false };
        const int valueSize = result->ValueSize;
        const int alignment = result->Alignment;
        const std::size_t tagBytes = result->TagBytes();
        const ValueType singleType = result->Type;
        const bool usesPrevious = CompareUsesPrevious(options.Mode);
        const bool againstFirst = options.Against == CompareTo::First && usesPrevious;
        const bool againstSaved = saved != nullptr && usesPrevious;

        const auto writer = [&](ScanStore& file, std::vector<std::uint8_t>& record, Segment& segment)
            {
                return [&file, &record, &segment, &storeFailed, tagBytes, valueSize](std::uint64_t address, ValueType type, const std::uint8_t* now, const std::uint8_t* initial) -> bool
                    {
                        std::memcpy(record.data(), &address, 8);
                        if (tagBytes)
                            record[8] = static_cast<std::uint8_t>(type);
                        std::memcpy(record.data() + 8 + tagBytes, now, static_cast<std::size_t>(valueSize));
                        std::memcpy(record.data() + 8 + tagBytes + static_cast<std::size_t>(valueSize), initial, static_cast<std::size_t>(valueSize));
                        if (!file.Append(record.data(), record.size()))
                        {
                            storeFailed = true;
                            return false;
                        }
                        ++segment.Count;
                        return true;
                    };
            };

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
                    SetThreadName(L"Scan worker");
                    PreferFastCores();
                    ScanStore& file = *result->Files[static_cast<std::size_t>(index)];
                    std::vector<std::uint8_t> buffer(kChunkBytes + kGroupMaxBytes + 64);
                    std::vector<std::uint8_t> record(8 + tagBytes + 2 * static_cast<std::size_t>(valueSize));
                    std::vector<Segment> localSegments;
                    std::vector<Snapshot> localSnapshots;
                    std::vector<std::vector<std::uint32_t>> hits;
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
                        const auto write = writer(file, record, segment);
                        const std::size_t firstOffset = static_cast<std::size_t>((alignment - (chunk.Base % static_cast<std::uint64_t>(alignment))) % static_cast<std::uint64_t>(alignment));
                        const std::size_t limit = readSize >= static_cast<std::size_t>(valueSize)
                                                      ? static_cast<std::size_t>(std::min<std::uint64_t>(chunk.Size, readSize - static_cast<std::size_t>(valueSize) + 1))
                                                      : 0;
                        const std::uint8_t* data = buffer.data();
                        if (mixed)
                        {
                            auto emit = [&](std::uint32_t offset, ValueType type) { return write(chunk.Base + offset, type, data + offset, data + offset); };
                            ScanTypes(typed, typed.Types, data, data, data, firstOffset, limit, static_cast<std::size_t>(alignment), hits, emit);
                        }
                        else
                        {
                            auto emit = [&](std::size_t offset) { return write(chunk.Base + offset, singleType, data + offset, data + offset); };
                            matcher.Scan(data, data, data, firstOffset, limit, static_cast<std::size_t>(alignment), emit);
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
                    SetThreadName(L"Scan worker");
                    PreferFastCores();
                    ScanStore& file = *result->Files[static_cast<std::size_t>(index)];
                    std::vector<std::vector<std::uint32_t>> hits;
                    const std::size_t recordSize = previous->RecordSize();
                    const std::size_t previousTag = previous->TagBytes();
                    std::vector<std::uint8_t> record(8 + tagBytes + 2 * static_cast<std::size_t>(valueSize));
                    std::vector<std::uint8_t> input;
                    std::vector<std::uint8_t> window(kWindowBytes + kGroupMaxBytes + 64);
                    std::vector<std::uint8_t> current;
                    std::vector<Segment> localSegments;
                    std::optional<SavedLookup> lookup;
                    if (againstSaved)
                        lookup.emplace(*saved);
                    const auto compareWith = [&](std::uint64_t address, ValueType type, const std::uint8_t* before, const std::uint8_t* initial) -> const std::uint8_t*
                        {
                            if (againstSaved)
                                return lookup->Find(address, type);
                            return againstFirst ? initial : before;
                        };
                    for (;;)
                    {
                        const std::size_t item = next.fetch_add(1);
                        if (item >= items || m_cancel || storeFailed)
                            break;
                        Segment segment;
                        segment.Order = item;
                        segment.File = index;
                        segment.Offset = file.Size();
                        const auto write = writer(file, record, segment);
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
                            if (againstSaved)
                            {
                                for (std::uint64_t position = 0; position < snapshot.Positions && !storeFailed; ++position)
                                {
                                    const std::size_t offset = static_cast<std::size_t>(position * static_cast<std::uint64_t>(alignment));
                                    if (offset + static_cast<std::size_t>(valueSize) > current.size())
                                        break;
                                    const std::uint8_t* now = current.data() + offset;
                                    const std::uint8_t* before = input.data() + offset;
                                    const std::uint64_t address = snapshot.Base + offset;
                                    if (mixed)
                                    {
                                        for (ValueType type : previous->AllList)
                                        {
                                            const Matcher* test = typed.Find(type);
                                            const std::uint8_t* against = test ? compareWith(address, type, before, before) : nullptr;
                                            if (against && test->Test(now, against, before) && !write(address, type, now, before))
                                                break;
                                        }
                                        continue;
                                    }
                                    const std::uint8_t* against = compareWith(address, singleType, before, before);
                                    if (against && matcher.Test(now, against, before) && !write(address, singleType, now, before))
                                        break;
                                }
                            }
                            else
                            {
                                const std::size_t usable = current.size() >= static_cast<std::size_t>(valueSize) ? current.size() - static_cast<std::size_t>(valueSize) + 1 : 0;
                                const std::size_t limit = static_cast<std::size_t>(std::min<std::uint64_t>(snapshot.Positions * static_cast<std::uint64_t>(alignment), usable));
                                const std::uint8_t* now = current.data();
                                const std::uint8_t* before = input.data();
                                if (mixed)
                                {
                                    auto emit = [&](std::uint32_t offset, ValueType type) { return write(snapshot.Base + offset, type, now + offset, before + offset); };
                                    ScanTypes(typed, previous->AllList, now, before, before, 0, limit, static_cast<std::size_t>(alignment), hits, emit);
                                }
                                else
                                {
                                    auto emit = [&](std::size_t offset) { return write(snapshot.Base + offset, singleType, now + offset, before + offset); };
                                    matcher.Scan(now, before, before, 0, limit, static_cast<std::size_t>(alignment), emit);
                                }
                            }
                            m_done += snapshot.Size;
                        }
                        else
                        {
                            const Segment& source = previous->Segments[item];
                            std::uint64_t windowBase = 0;
                            std::size_t windowSize = 0;
                            for (std::uint64_t first = 0; first < source.Count && !m_cancel && !storeFailed; first += kNextBatchRecords)
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
                                    const ValueType type = previousTag ? static_cast<ValueType>(raw[8]) : singleType;
                                    const Matcher* test = mixed ? typed.Find(type) : &matcher;
                                    if (!test)
                                        continue;
                                    if (address < windowBase || address + static_cast<std::uint64_t>(valueSize) > windowBase + windowSize)
                                    {
                                        windowBase = address;
                                        windowSize = process->Read(address, window.data(), std::max<std::size_t>(kWindowBytes, static_cast<std::size_t>(valueSize)));
                                        if (windowSize < static_cast<std::size_t>(valueSize))
                                        {
                                            windowBase = 0;
                                            windowSize = 0;
                                            continue;
                                        }
                                    }
                                    const std::uint8_t* now = window.data() + (address - windowBase);
                                    const std::uint8_t* before = raw + 8 + previousTag;
                                    const std::uint8_t* initial = before + previous->ValueSize;
                                    const std::uint8_t* against = compareWith(address, type, before, initial);
                                    if (!against || !test->Test(now, against, initial))
                                        continue;
                                    if (!write(address, type, now, initial))
                                        break;
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
                count += snapshot.Positions * result->Multiplicity();
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

    namespace
    {
        std::optional<std::int64_t> GroupInteger(std::string_view text, bool hex)
        {
            text = TrimView(text);
            bool negative = false;
            if (!text.empty() && text.front() == '-')
            {
                negative = true;
                text.remove_prefix(1);
            }
            bool asHex = hex;
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                asHex = true;
                text.remove_prefix(2);
            }
            if (text.empty())
                return std::nullopt;
            std::optional<std::uint64_t> value;
            if (asHex)
            {
                value = ParseHex(text);
            }
            else if (const auto decimal = ParseDecimal(text); decimal && *decimal >= 0)
            {
                value = static_cast<std::uint64_t>(*decimal);
            }
            if (!value || (negative && *value > static_cast<std::uint64_t>(INT64_MAX)))
                return std::nullopt;
            return negative ? -static_cast<std::int64_t>(*value) : static_cast<std::int64_t>(*value);
        }

        bool FitsSize(std::int64_t value, int size)
        {
            if (size >= 8)
                return true;
            const std::int64_t high = (std::int64_t{ 1 } << (size * 8)) - 1;
            const std::int64_t low = -(std::int64_t{ 1 } << (size * 8 - 1));
            return value >= low && value <= high;
        }

        int ElementAlign(const GroupElement& element)
        {
            if (element.Type == ValueType::String)
                return element.Unicode ? 2 : 1;
            return std::clamp(element.Size, 1, 8);
        }

        bool ElementMatches(const GroupElement& element, const std::uint8_t* data)
        {
            if (element.Any || element.Skip)
                return true;
            switch (element.Type)
            {
            case ValueType::Float:
            {
                const float value = Load<float>(data);
                return std::isfinite(value) && value >= element.LowFloat && value <= element.HighFloat;
            }
            case ValueType::Double:
            {
                const double value = Load<double>(data);
                return std::isfinite(value) && value >= element.LowFloat && value <= element.HighFloat;
            }
            case ValueType::String:
                return std::memcmp(data, element.Text.data(), element.Text.size()) == 0;
            default:
            {
                std::uint64_t raw = 0;
                std::memcpy(&raw, data, static_cast<std::size_t>(element.Size));
                if (element.Signed)
                {
                    std::int64_t value = static_cast<std::int64_t>(raw);
                    if (element.Size < 8)
                    {
                        const int shift = 64 - element.Size * 8;
                        value = static_cast<std::int64_t>(raw << shift) >> shift;
                    }
                    return value >= element.Low && value <= element.High;
                }
                return raw >= static_cast<std::uint64_t>(element.Low) && raw <= static_cast<std::uint64_t>(element.High);
            }
            }
        }

        std::vector<std::string> GroupTokens(std::string_view text)
        {
            std::vector<std::string> tokens;
            std::string current;
            bool quoted = false;
            bool started = false;
            for (const char c : text)
            {
                if (c == '"')
                {
                    quoted = !quoted;
                    started = true;
                    continue;
                }
                if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
                {
                    if (started)
                        tokens.push_back(current);
                    current.clear();
                    started = false;
                    continue;
                }
                current += c;
                started = true;
            }
            if (started)
                tokens.push_back(current);
            return tokens;
        }

        std::string Quoted(std::string_view text)
        {
            return "\"" + std::string(text) + "\"";
        }
    }

    std::optional<GroupSpec> ParseGroup(std::string_view text, bool hex, int pointerSize, std::string& error)
    {
        GroupSpec spec;
        for (const std::string& token : GroupTokens(text))
        {
            const std::size_t colon = token.find(':');
            if (colon == std::string::npos || colon == 0)
            {
                error = Quoted(token) + " needs a type and a value, like 4:100";
                return std::nullopt;
            }
            const std::string kind = Lower(token.substr(0, colon));
            const std::string value = token.substr(colon + 1);
            if (kind == "within")
            {
                const auto size = GroupInteger(value, false);
                if (!size || *size < 1 || *size > kGroupMaxBytes)
                {
                    error = "within takes a size from 1 to " + std::to_string(kGroupMaxBytes) + " bytes";
                    return std::nullopt;
                }
                spec.Within = static_cast<int>(*size);
                continue;
            }
            GroupElement element;
            if (kind == "1" || kind == "2" || kind == "4" || kind == "8")
            {
                element.Size = kind[0] - '0';
                element.Type = element.Size == 1 ? ValueType::Byte : element.Size == 2 ? ValueType::TwoBytes : element.Size == 4 ? ValueType::FourBytes : ValueType::EightBytes;
            }
            else if (kind == "p")
            {
                element.Type = ValueType::Pointer;
                element.Size = pointerSize == 4 ? 4 : 8;
            }
            else if (kind == "f")
            {
                element.Type = ValueType::Float;
                element.Size = 4;
            }
            else if (kind == "d")
            {
                element.Type = ValueType::Double;
                element.Size = 8;
            }
            else if (kind == "s" || kind == "u")
            {
                element.Type = ValueType::String;
                element.Unicode = kind == "u";
                if (value.empty())
                {
                    error = Quoted(token) + " has no text";
                    return std::nullopt;
                }
                if (element.Unicode)
                {
                    for (const wchar_t c : Utf8ToWide(value))
                    {
                        element.Text.push_back(static_cast<std::uint8_t>(c & 0xFF));
                        element.Text.push_back(static_cast<std::uint8_t>((c >> 8) & 0xFF));
                    }
                }
                else
                {
                    element.Text.assign(value.begin(), value.end());
                }
                element.Size = static_cast<int>(element.Text.size());
            }
            else if (kind == "skip")
            {
                const auto size = GroupInteger(value, false);
                if (!size || *size < 1 || *size > kGroupMaxBytes)
                {
                    error = "skip takes a number of bytes from 1 to " + std::to_string(kGroupMaxBytes);
                    return std::nullopt;
                }
                element.Type = ValueType::ByteArray;
                element.Skip = true;
                element.Size = static_cast<int>(*size);
            }
            else
            {
                error = "unknown type " + Quoted(kind) + " in " + Quoted(token);
                return std::nullopt;
            }

            if (!element.Skip && element.Type != ValueType::String)
            {
                if (TrimView(value) == "*")
                {
                    element.Any = true;
                }
                else
                {
                    const std::size_t dots = value.find("..");
                    const std::string lowText = dots == std::string::npos ? value : value.substr(0, dots);
                    const std::string highText = dots == std::string::npos ? value : value.substr(dots + 2);
                    if (IsFloating(element.Type))
                    {
                        const auto low = ParseDouble(lowText);
                        const auto high = ParseDouble(highText);
                        if (!low || !high)
                        {
                            error = Quoted(value) + " is not a number in " + Quoted(token);
                            return std::nullopt;
                        }
                        if (dots == std::string::npos)
                        {
                            const double step = std::pow(10.0, -DecimalsOf(lowText));
                            const double slack = step * 0.5 + std::fabs(*low) * 1e-12;
                            element.LowFloat = *low - slack;
                            element.HighFloat = *low + slack;
                        }
                        else
                        {
                            element.LowFloat = std::min(*low, *high);
                            element.HighFloat = std::max(*low, *high);
                        }
                    }
                    else
                    {
                        const auto low = GroupInteger(lowText, hex);
                        const auto high = GroupInteger(highText, hex);
                        if (!low || !high || !FitsSize(*low, element.Size) || !FitsSize(*high, element.Size))
                        {
                            error = Quoted(value) + " does not fit " + std::to_string(element.Size) + (element.Size == 1 ? " byte" : " bytes") + " in " + Quoted(token);
                            return std::nullopt;
                        }
                        element.Low = std::min(*low, *high);
                        element.High = std::max(*low, *high);
                        element.Signed = element.Low < 0;
                    }
                }
            }
            spec.Elements.push_back(std::move(element));
        }

        if (spec.Elements.empty())
        {
            error = "enter values like 4:100 f:1.5";
            return std::nullopt;
        }
        if (std::all_of(spec.Elements.begin(), spec.Elements.end(), [](const GroupElement& element) { return element.Any || element.Skip; }))
        {
            error = "at least one value has to be more than * or skip";
            return std::nullopt;
        }
        int total = 0;
        for (GroupElement& element : spec.Elements)
        {
            element.Offset = total;
            total += element.Size;
        }
        if (spec.Within == 0)
        {
            if (total > kGroupMaxBytes)
            {
                error = "the group covers more than " + std::to_string(kGroupMaxBytes) + " bytes";
                return std::nullopt;
            }
            spec.BlockSize = total;
            return spec;
        }
        for (const GroupElement& element : spec.Elements)
        {
            if (element.Any || element.Skip)
            {
                error = "* and skip need a fixed order; leave out within";
                return std::nullopt;
            }
            if (element.Size > spec.Within)
            {
                error = "within has to be at least as big as each value";
                return std::nullopt;
            }
        }
        spec.BlockSize = spec.Within;
        for (std::size_t i = 1; i < spec.Elements.size(); ++i)
            spec.Elements[i].Offset = -1;
        return spec;
    }

    bool GroupSpec::Matches(const std::uint8_t* block, std::vector<int>* offsets) const
    {
        if (offsets)
            offsets->assign(Elements.size(), -1);
        if (Elements.empty())
            return false;
        if (Within == 0)
        {
            for (const GroupElement& element : Elements)
            {
                if (!ElementMatches(element, block + element.Offset))
                    return false;
            }
            if (offsets)
            {
                for (std::size_t i = 0; i < Elements.size(); ++i)
                    (*offsets)[i] = Elements[i].Offset;
            }
            return true;
        }
        if (!ElementMatches(Elements[0], block))
            return false;
        std::vector<std::pair<int, int>> used;
        used.reserve(Elements.size());
        used.emplace_back(0, Elements[0].Size);
        for (std::size_t i = 1; i < Elements.size(); ++i)
        {
            const GroupElement& element = Elements[i];
            const int align = ElementAlign(element);
            int found = -1;
            for (int at = 0; at + element.Size <= BlockSize; at += align)
            {
                bool overlaps = false;
                for (const auto& [start, end] : used)
                {
                    if (at < end && start < at + element.Size)
                    {
                        overlaps = true;
                        break;
                    }
                }
                if (!overlaps && ElementMatches(element, block + at))
                {
                    found = at;
                    break;
                }
            }
            if (found < 0)
                return false;
            used.emplace_back(found, found + element.Size);
        }
        if (offsets)
        {
            for (std::size_t i = 0; i < Elements.size(); ++i)
                (*offsets)[i] = used[i].first;
        }
        return true;
    }

    std::vector<int> GroupSpec::Offsets(std::span<const std::uint8_t> block) const
    {
        std::vector<int> offsets(Elements.size(), -1);
        if (Within == 0)
        {
            for (std::size_t i = 0; i < Elements.size(); ++i)
                offsets[i] = Elements[i].Offset;
            return offsets;
        }
        if (block.size() >= static_cast<std::size_t>(BlockSize))
            Matches(block.data(), &offsets);
        return offsets;
    }

    std::string GroupSpec::Describe(std::span<const std::uint8_t> block, int pointerSize) const
    {
        const std::vector<int> offsets = Offsets(block);
        std::string text;
        for (std::size_t i = 0; i < Elements.size(); ++i)
        {
            const GroupElement& element = Elements[i];
            if (element.Skip)
                continue;
            if (!text.empty())
                text += "  ";
            const int at = offsets[i];
            if (at < 0 || static_cast<std::size_t>(at + element.Size) > block.size())
            {
                text += "?";
                continue;
            }
            ValueFormat format;
            format.Signed = element.Signed;
            format.PointerSize = pointerSize;
            format.Unicode = element.Unicode;
            format.Length = element.Unicode ? element.Size / 2 : element.Size;
            text += FormatValue(element.Type, block.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(element.Size)), format);
        }
        return text;
    }

    bool PatternFilter::Accepts(const Target::RegionInfo& region) const
    {
        const auto check = [](Tristate wanted, bool has) { return wanted == Tristate::Any || (wanted == Tristate::Yes) == has; };
        if (!region.Readable() || !check(Writable, region.Writable()) || !check(Executable, region.Executable()) || !check(CopyOnWrite, region.CopyOnWrite()))
            return false;
        return IncludeReadOnly || region.Writable() || region.Executable();
    }

    bool PatternFilter::Accepts(std::uint64_t address) const
    {
        if (Divisor > 1 && address % Divisor != 0)
            return false;
        return Modulo <= 1 || address % Modulo == Remainder;
    }

    std::vector<std::uint64_t> FindPattern(const Target::Process& process, const Pattern& pattern, std::uint64_t start, std::uint64_t stop, bool executableOnly,
        std::size_t limit, bool includeReadOnly)
    {
        PatternFilter filter;
        filter.Executable = executableOnly ? Tristate::Yes : Tristate::Any;
        filter.IncludeReadOnly = includeReadOnly;
        return FindPattern(process, pattern, start, stop, filter, limit);
    }

    std::vector<std::uint64_t> FindPattern(const Target::Process& process, const Pattern& pattern, std::uint64_t start, std::uint64_t stop, const PatternFilter& filter,
        std::size_t limit, PatternProgress* progress)
    {
        std::vector<std::uint64_t> found;
        if (pattern.Empty())
            return found;
        std::vector<WorkChunk> chunks;
        const std::uint64_t size = pattern.Bytes.size();
        for (const Target::RegionInfo& region : process.Regions(std::max(start, process.LowestAddress()), std::min(stop, process.HighestAddress())))
        {
            if (!filter.Accepts(region))
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

        if (progress)
            progress->Total += chunks.size();
        std::vector<std::vector<std::uint64_t>> perChunk(chunks.size());
        std::atomic<std::size_t> next{ 0 };
        std::atomic<std::size_t> total{ 0 };
        const std::uint8_t firstByte = pattern.Bytes[0];
        const bool firstFixed = pattern.Mask[0] == 0xFF;
        const auto worker = [&]
            {
                SetThreadName(L"Scan worker");
                PreferFastCores();
                std::vector<std::uint8_t> buffer(kChunkBytes + 256);
                for (;;)
                {
                    const std::size_t item = next.fetch_add(1);
                    if (item >= chunks.size() || (limit && total >= limit) || (progress && progress->Stopped()))
                        break;
                    if (progress)
                        ++progress->Done;
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
                        if (pattern.Matches(buffer.data() + offset) && filter.Accepts(chunk.Base + offset))
                        {
                            perChunk[item].push_back(chunk.Base + offset);
                            ++total;
                        }
                    }
                }
            };
        const int threads = chunks.size() > 8 ? WorkerCount(0) : 1;
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
