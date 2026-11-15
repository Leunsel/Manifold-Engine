#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "Core/Deflate.h"

#include <climits>
#include <cstdlib>
#include <cstring>

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

    std::optional<std::vector<std::uint8_t>> Decompress(std::span<const std::uint8_t> packed)
    {
        if (packed.size() > static_cast<std::size_t>(INT_MAX))
            return std::nullopt;
        int length = 0;
        char* data = stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char*>(packed.data()), static_cast<int>(packed.size()), &length);
        if (!data || length < 0)
        {
            std::free(data);
            return std::nullopt;
        }
        std::vector<std::uint8_t> result(reinterpret_cast<std::uint8_t*>(data), reinterpret_cast<std::uint8_t*>(data) + length);
        std::free(data);
        return result;
    }

    std::optional<std::vector<std::uint8_t>> DecodePng(std::span<const std::uint8_t> file, int& width, int& height)
    {
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels, 4);
        if (!pixels)
            return std::nullopt;
        std::vector<std::uint8_t> result(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        stbi_image_free(pixels);
        return result;
    }
}
