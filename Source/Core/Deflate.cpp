#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "Core/Deflate.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#pragma warning(push, 0)
#pragma warning(disable : 4505)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#pragma warning(pop)

namespace Manifold::Deflate
{
    std::optional<std::vector<std::uint8_t>> Compress(std::span<const std::uint8_t> data)
    {
        if (data.size() > static_cast<std::size_t>(INT_MAX))
            return std::nullopt;
        int length = 0;
        unsigned char* packed = stbi_zlib_compress(const_cast<unsigned char*>(data.data()), static_cast<int>(data.size()), &length, 8);
        if (!packed)
            return std::nullopt;
        std::optional<std::vector<std::uint8_t>> raw;
        if (length >= 6)
            raw = std::vector<std::uint8_t>(packed + 2, packed + length - 4);
        std::free(packed);
        return raw;
    }

    std::optional<std::vector<std::uint8_t>> Decompress(std::span<const std::uint8_t> packed, std::size_t limit)
    {
        if (packed.size() > static_cast<std::size_t>(INT_MAX))
            return std::nullopt;
        const std::size_t ceiling = std::min({ limit, static_cast<std::size_t>(INT_MAX), packed.size() * kMaxRatio + 64 });
        std::size_t room = std::min(ceiling, std::max<std::size_t>(packed.size() * 4, 0x10000));
        for (;;)
        {
            const std::unique_ptr<std::uint8_t[]> buffer(new (std::nothrow) std::uint8_t[room]);
            if (!buffer)
                return std::nullopt;
            const int length = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(buffer.get()), static_cast<int>(room),
                reinterpret_cast<const char*>(packed.data()), static_cast<int>(packed.size()));
            if (length >= 0)
                return std::vector<std::uint8_t>(buffer.get(), buffer.get() + length);
            if (room >= ceiling)
                return std::nullopt;
            room = std::min(ceiling, room * 2);
        }
    }
}
