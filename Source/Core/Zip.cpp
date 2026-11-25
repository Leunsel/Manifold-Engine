#include "Core/Zip.h"

#include "Core/Deflate.h"

#include <array>
#include <cstring>

namespace Manifold::Zip
{
    namespace
    {
        constexpr std::uint32_t kEndSignature = 0x06054B50;
        constexpr std::uint32_t kCentralSignature = 0x02014B50;
        constexpr std::uint32_t kLocalSignature = 0x04034B50;
        constexpr std::size_t kEndSize = 22;
        constexpr std::size_t kCentralSize = 46;
        constexpr std::size_t kLocalSize = 30;

        std::uint16_t U16(std::span<const std::uint8_t> data, std::size_t at)
        {
            return static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8));
        }

        std::uint32_t U32(std::span<const std::uint8_t> data, std::size_t at)
        {
            return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8) | (static_cast<std::uint32_t>(data[at + 2]) << 16) |
                   (static_cast<std::uint32_t>(data[at + 3]) << 24);
        }

        const std::array<std::uint32_t, 256>& CrcTable()
        {
            static const std::array<std::uint32_t, 256> table = [] {
                std::array<std::uint32_t, 256> made{};
                for (std::uint32_t i = 0; i < 256; ++i)
                {
                    std::uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    made[i] = c;
                }
                return made;
            }();
            return table;
        }

        bool Fail(std::string* error, std::string text)
        {
            if (error)
                *error = std::move(text);
            return false;
        }
    }

    std::uint32_t Crc32(std::span<const std::uint8_t> data)
    {
        const auto& table = CrcTable();
        std::uint32_t crc = 0xFFFFFFFFu;
        for (const std::uint8_t byte : data)
            crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    std::optional<std::vector<Entry>> Read(std::span<const std::uint8_t> archive, std::string* error, std::size_t limit)
    {
        if (archive.size() < kEndSize)
        {
            Fail(error, "the file is too short for a zip file");
            return std::nullopt;
        }
        // the end record sits in the last 22 bytes plus a comment of up to 64 KB
        std::size_t end = std::string::npos;
        const std::size_t lowest = archive.size() > kEndSize + 0xFFFF ? archive.size() - kEndSize - 0xFFFF : 0;
        for (std::size_t at = archive.size() - kEndSize + 1; at-- > lowest;)
        {
            if (U32(archive, at) == kEndSignature)
            {
                end = at;
                break;
            }
        }
        if (end == std::string::npos)
        {
            Fail(error, "the end of the zip directory was not found");
            return std::nullopt;
        }
        const std::size_t count = U16(archive, end + 10);
        const std::size_t directorySize = U32(archive, end + 12);
        const std::size_t directory = U32(archive, end + 16);
        if (count == 0xFFFF || directory == 0xFFFFFFFFu || directory > end || directorySize > end - directory)
        {
            Fail(error, "the zip directory is out of range (Zip64 is not read)");
            return std::nullopt;
        }

        std::vector<Entry> entries;
        entries.reserve(count);
        std::size_t total = 0;
        std::size_t at = directory;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (at + kCentralSize > end || U32(archive, at) != kCentralSignature)
            {
                Fail(error, "zip directory entry " + std::to_string(i + 1) + " is damaged");
                return std::nullopt;
            }
            const std::uint16_t flags = U16(archive, at + 8);
            const std::uint16_t method = U16(archive, at + 10);
            const std::uint32_t crc = U32(archive, at + 16);
            const std::size_t packedSize = U32(archive, at + 20);
            const std::size_t size = U32(archive, at + 24);
            const std::size_t nameLength = U16(archive, at + 28);
            const std::size_t extraLength = U16(archive, at + 30);
            const std::size_t commentLength = U16(archive, at + 32);
            const std::size_t local = U32(archive, at + 42);
            if (at + kCentralSize + nameLength + extraLength + commentLength > end)
            {
                Fail(error, "zip directory entry " + std::to_string(i + 1) + " is cut off");
                return std::nullopt;
            }
            Entry entry;
            entry.Name.assign(reinterpret_cast<const char*>(archive.data() + at + kCentralSize), nameLength);
            for (char& c : entry.Name)
            {
                if (c == '\\')
                    c = '/';
            }
            at += kCentralSize + nameLength + extraLength + commentLength;

            if (flags & 1)
            {
                Fail(error, entry.Name + " is encrypted");
                return std::nullopt;
            }
            if (packedSize == 0xFFFFFFFFu || size == 0xFFFFFFFFu || local == 0xFFFFFFFFu)
            {
                Fail(error, entry.Name + " needs Zip64, which is not read");
                return std::nullopt;
            }
            total += size;
            if (size > limit || total > limit)
            {
                Fail(error, "the zip holds more than " + std::to_string(limit / (1024 * 1024)) + " MB");
                return std::nullopt;
            }
            if (local + kLocalSize > archive.size() || U32(archive, local) != kLocalSignature)
            {
                Fail(error, entry.Name + ": the local header is damaged");
                return std::nullopt;
            }
            const std::size_t data = local + kLocalSize + U16(archive, local + 26) + U16(archive, local + 28);
            if (data > archive.size() || packedSize > archive.size() - data)
            {
                Fail(error, entry.Name + " is cut off");
                return std::nullopt;
            }
            entry.Folder = !entry.Name.empty() && entry.Name.back() == '/';
            const std::span<const std::uint8_t> packed = archive.subspan(data, packedSize);
            if (method == 0)
                entry.Data.assign(packed.begin(), packed.end());
            else if (method == 8)
            {
                if (size > 0)
                {
                    auto inflated = Deflate::Decompress(packed, size);
                    if (!inflated)
                    {
                        Fail(error, entry.Name + " could not be unpacked");
                        return std::nullopt;
                    }
                    entry.Data = std::move(*inflated);
                }
            }
            else
            {
                Fail(error, entry.Name + " uses packing method " + std::to_string(method) + ", only stored and deflate are read");
                return std::nullopt;
            }
            if (entry.Data.size() != size || Crc32(entry.Data) != crc)
            {
                Fail(error, entry.Name + " is damaged (size or CRC-32 does not match)");
                return std::nullopt;
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }
}
