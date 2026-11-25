#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "Core/ImageDecode.h"

#include <climits>
#include <cstdlib>
#include <cstring>

#pragma warning(push, 0)
#pragma warning(disable : 4505)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"
#pragma warning(pop)

namespace Manifold
{
    namespace
    {
        // ∑ stb_image grows its IDAT buffer to the length a chunk claims before it
        //   reads the chunk, so a 1 KB file claiming a 1 GB chunk allocated 1 GB
        //   (found by fuzzing). Every chunk has to fit in the file it claims to be in.
        bool PngChunksFit(std::span<const std::uint8_t> bytes)
        {
            static constexpr std::uint8_t kSignature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
            if (bytes.size() < sizeof(kSignature) || std::memcmp(bytes.data(), kSignature, sizeof(kSignature)) != 0)
                return true;
            std::size_t at = sizeof(kSignature);
            while (at + 8 <= bytes.size())
            {
                const std::uint32_t length = (std::uint32_t{ bytes[at] } << 24) | (std::uint32_t{ bytes[at + 1] } << 16) | (std::uint32_t{ bytes[at + 2] } << 8) | bytes[at + 3];
                const bool end = std::memcmp(bytes.data() + at + 4, "IEND", 4) == 0;
                if (length > bytes.size() - at - 8 || bytes.size() - at - 8 - length < 4)
                    return false;
                at += 8ull + length + 4ull;
                if (end)
                    return true;
            }
            return true;
        }
    }

    std::optional<DecodedImage> DecodeImage(std::span<const std::uint8_t> bytes, std::uint64_t maxPixels)
    {
        if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX) || !PngChunksFit(bytes))
            return std::nullopt;
        int width = 0;
        int height = 0;
        int channels = 0;
        // ∑ The header alone decides the allocation: a few hundred bytes of PNG can claim 23000 x 23000 pixels (2 GB, held twice).
        if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels))
            return std::nullopt;
        if (width <= 0 || height <= 0 || width > kMaxImageSide || height > kMaxImageSide ||
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > maxPixels)
            return std::nullopt;
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
        if (!pixels)
            return std::nullopt;
        DecodedImage image;
        image.Width = width;
        image.Height = height;
        image.Rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        stbi_image_free(pixels);
        return image;
    }
}
