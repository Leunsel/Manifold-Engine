#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The messages between the program and its .NET helper (ManifoldClr.exe and
// ManifoldClr32.exe). The helper reads a .NET game through the runtime's own
// data access library (the DAC) from outside the game; this header is shared
// by both sides so the layout cannot drift apart. Every message is a 32-bit
// little-endian length followed by that many bytes. A request starts with its
// operation; a reply starts with a status byte (0 ok, else an error text).
namespace Manifold::Target::ClrProtocol
{
    // Bumped whenever a message changes; Open fails on a mismatch, so a helper
    // left over from another build never answers in a layout the program
    // misreads.
    constexpr std::uint32_t kVersion = 2;
    // A reply larger than this is a broken helper, not data.
    constexpr std::uint32_t kMaxMessage = 256u * 1024u * 1024u;

    enum class Op : std::uint8_t
    {
        Open = 1,          // u32 version, u32 pid -> runtime description
        Modules = 2,       // -> every managed module
        Types = 3,         // u64 module -> its loaded types
        Members = 4,       // u64 method table -> fields, statics, methods
        DescribeType = 5,  // u64 method table -> one type, or not valid
        MethodAt = 6,      // u64 address -> the managed method around it
        MethodCode = 7,    // u64 method desc -> its native code now
        Flush = 8,         // drop the cached memory and the DAC's caches
        HeapStats = 9,     // -> the objects on the GC heap, counted per type
        Instances = 10,    // u64 method table, u32 limit -> addresses of its objects
        ObjectInfo = 11,   // u64 address -> the object there, as the runtime sees it
    };

    enum class ObjectKind : std::uint32_t
    {
        String = 0,
        Free = 1,
        Object = 2,
        Array = 3,
        Other = 4,
    };

    enum TypeFlags : std::uint32_t
    {
        kTypeGeneric = 1,    // an open generic definition (List`1)
        kTypeValue = 2,      // a struct (derives from System.ValueType)
        kTypeEnum = 4,
        kTypeInterface = 8,
        kTypeArray = 16,
        kTypeString = 32,
    };

    enum FieldFlags : std::uint32_t
    {
        kFieldStatic = 1,
        kFieldThreadStatic = 2,
        kFieldBoxed = 4,       // a static struct: the slot holds a boxed copy
        kFieldUnallocated = 8, // a static whose storage the runtime has not made yet
    };

    enum MethodFlags : std::uint32_t
    {
        kMethodStatic = 1,
        kMethodVirtual = 2,
        kMethodAbstract = 4,
    };

    class Writer
    {
    public:
        Writer() { m_bytes.resize(4); }

        void U8(std::uint8_t value) { m_bytes.push_back(value); }
        void U32(std::uint32_t value) { Raw(&value, sizeof(value)); }
        void I32(std::int32_t value) { Raw(&value, sizeof(value)); }
        void U64(std::uint64_t value) { Raw(&value, sizeof(value)); }
        void Text(std::string_view text)
        {
            U32(static_cast<std::uint32_t>(text.size()));
            Raw(text.data(), text.size());
        }

        // The finished message with its length in front.
        const std::vector<std::uint8_t>& Done()
        {
            const auto size = static_cast<std::uint32_t>(m_bytes.size() - 4);
            std::memcpy(m_bytes.data(), &size, sizeof(size));
            return m_bytes;
        }

    private:
        void Raw(const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            m_bytes.insert(m_bytes.end(), bytes, bytes + size);
        }

        std::vector<std::uint8_t> m_bytes;
    };

    // Reads one message body. A read past the end yields zeros and marks the
    // reader failed, so a short or corrupt reply is caught by one Ok() check
    // at the end instead of after every field.
    class Reader
    {
    public:
        explicit Reader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

        std::uint8_t U8()
        {
            std::uint8_t value = 0;
            Raw(&value, sizeof(value));
            return value;
        }
        std::uint32_t U32()
        {
            std::uint32_t value = 0;
            Raw(&value, sizeof(value));
            return value;
        }
        std::int32_t I32()
        {
            std::int32_t value = 0;
            Raw(&value, sizeof(value));
            return value;
        }
        std::uint64_t U64()
        {
            std::uint64_t value = 0;
            Raw(&value, sizeof(value));
            return value;
        }
        std::string Text()
        {
            const std::uint32_t size = U32();
            if (!m_ok || size > m_bytes.size() - m_at)
            {
                m_ok = false;
                return {};
            }
            std::string text(reinterpret_cast<const char*>(m_bytes.data() + m_at), size);
            m_at += size;
            return text;
        }
        // A count of items that each take at least minimum bytes, so a
        // corrupt count cannot make the caller reserve gigabytes.
        std::uint32_t Count(std::size_t minimum)
        {
            const std::uint32_t count = U32();
            if (!m_ok || (minimum && count > (m_bytes.size() - m_at) / minimum))
            {
                m_ok = false;
                return 0;
            }
            return count;
        }

        bool Ok() const { return m_ok; }
        bool AtEnd() const { return m_at == m_bytes.size(); }

    private:
        void Raw(void* out, std::size_t size)
        {
            if (!m_ok || size > m_bytes.size() - m_at)
            {
                m_ok = false;
                return;
            }
            std::memcpy(out, m_bytes.data() + m_at, size);
            m_at += size;
        }

        std::span<const std::uint8_t> m_bytes;
        std::size_t m_at = 0;
        bool m_ok = true;
    };
}
