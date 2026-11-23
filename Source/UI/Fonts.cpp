#include "UI/Fonts.h"

#include "Core/Errors.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Resource.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace Manifold::Fonts
{
    namespace
    {
        ImFont* g_bold = nullptr;
        ImFont* g_mono = nullptr;

        struct Source
        {
            std::string File;
            const void* Data = nullptr;
            int Size = 0;

            bool Found() const { return !File.empty() || (Data && Size > 0); }
        };

        ImFont* Add(ImFontAtlas* atlas, const Source& source, float size, ImFontConfig& config, const ImWchar* ranges = nullptr)
        {
            if (!source.File.empty())
                return atlas->AddFontFromFileTTF(source.File.c_str(), size, &config, ranges);
            config.FontDataOwnedByAtlas = false;
            return atlas->AddFontFromMemoryTTF(const_cast<void*>(source.Data), source.Size, size, &config, ranges);
        }

        Source SystemFont(const wchar_t* name)
        {
            wchar_t folder[MAX_PATH] = L"";
            const UINT length = ::GetWindowsDirectoryW(folder, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
                return {};
            const std::wstring path = std::wstring(folder) + L"\\Fonts\\" + name;
            if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                return {};
            return Source{ WideToUtf8(path) };
        }

        Source FirstFont(std::initializer_list<const wchar_t*> names)
        {
            for (const wchar_t* name : names)
            {
                if (Source source = SystemFont(name); source.Found())
                    return source;
            }
            return {};
        }

        // ∑ The vertical metrics of a TrueType file as fractions of the height ImGui scales to
        //   (hhea ascent - descent, what stb_truetype's ScaleForPixelHeight uses). Read from the
        //   file itself, since the UI and mono fonts are whatever Windows has.
        struct Metrics
        {
            float Ascent = 0.0f;
            float XHeight = 0.0f;
            float CapHeight = 0.0f;
        };

        std::optional<Metrics> ReadMetrics(const Source& source)
        {
            if (source.File.empty())
                return std::nullopt;
            std::ifstream file(Utf8ToWide(source.File), std::ios::binary);
            const std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const auto u16 = [&](std::size_t at) -> std::optional<int> {
                if (at + 2 > data.size())
                    return std::nullopt;
                return (data[at] << 8) | data[at + 1];
            };
            const auto s16 = [&](std::size_t at) -> std::optional<int> {
                const auto value = u16(at);
                if (!value)
                    return std::nullopt;
                return *value >= 0x8000 ? *value - 0x10000 : *value;
            };
            const auto tables = u16(4);
            if (!tables)
                return std::nullopt;
            std::size_t hhea = 0;
            std::size_t os2 = 0;
            for (int i = 0; i < *tables; ++i)
            {
                const std::size_t entry = 12 + 16 * static_cast<std::size_t>(i);
                if (entry + 16 > data.size())
                    return std::nullopt;
                const std::string tag(reinterpret_cast<const char*>(&data[entry]), 4);
                const std::size_t offset = (static_cast<std::size_t>(data[entry + 8]) << 24) | (data[entry + 9] << 16) | (data[entry + 10] << 8) | data[entry + 11];
                if (tag == "hhea")
                    hhea = offset;
                else if (tag == "OS/2")
                    os2 = offset;
            }
            const auto ascent = hhea ? s16(hhea + 4) : std::nullopt;
            const auto descent = hhea ? s16(hhea + 6) : std::nullopt;
            if (!ascent || !descent || *ascent - *descent <= 0)
                return std::nullopt;
            const float height = static_cast<float>(*ascent - *descent);
            Metrics metrics;
            metrics.Ascent = static_cast<float>(*ascent) / height;
            if (const auto version = os2 ? u16(os2) : std::nullopt; version && *version >= 2)
            {
                metrics.XHeight = static_cast<float>(s16(os2 + 86).value_or(0)) / height;
                metrics.CapHeight = static_cast<float>(s16(os2 + 88).value_or(0)) / height;
            }
            return metrics;
        }

        // ∑ Fonts drawn at the same ImGui size are not the same size to the eye: Consolas at 13 px has
        //   an 8 % larger x-height than Tahoma at 13 px and its baseline sits a pixel higher, so mono
        //   text next to normal text looked bigger and shifted up. The mono glyphs are scaled to the
        //   UI font's x-height and cap height (ExtraSizeScale leaves the line height alone, so rows
        //   keep their height) and moved down to its baseline.
        void MatchMono(const Source& text, const Source& mono, ImFontConfig& config)
        {
            const auto textMetrics = ReadMetrics(text);
            const auto monoMetrics = ReadMetrics(mono);
            if (!textMetrics || !monoMetrics)
                return;
            std::vector<float> ratios;
            if (textMetrics->XHeight > 0.0f && monoMetrics->XHeight > 0.0f)
                ratios.push_back(textMetrics->XHeight / monoMetrics->XHeight);
            if (textMetrics->CapHeight > 0.0f && monoMetrics->CapHeight > 0.0f)
                ratios.push_back(textMetrics->CapHeight / monoMetrics->CapHeight);
            if (!ratios.empty())
            {
                float sum = 0.0f;
                for (const float ratio : ratios)
                    sum += ratio;
                config.ExtraSizeScale = std::clamp(sum / static_cast<float>(ratios.size()), 0.75f, 1.33f);
            }
            config.GlyphOffset.y = std::ceil(kSize * textMetrics->Ascent) - std::ceil(kMonoSize * monoMetrics->Ascent);
        }

        Source IconFont()
        {
            HMODULE module = ::GetModuleHandleW(nullptr);
            HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(IDR_FONT_FA_SOLID), RT_RCDATA);
            HGLOBAL handle = resource ? ::LoadResource(module, resource) : nullptr;
            void* icons = handle ? ::LockResource(handle) : nullptr;
            const int iconsSize = resource ? static_cast<int>(::SizeofResource(module, resource)) : 0;
            if (!icons || iconsSize <= 0)
                return {};
            return Source{ {}, icons, iconsSize };
        }
    }

    bool Build(const char** error)
    {
        const char* dummy = nullptr;
        if (!error)
            error = &dummy;
        *error = nullptr;
        static std::string message;

        const Source icons = IconFont();
        if (!icons.Found())
        {
            message = ErrorMessage(ErrorCode::IconFontMissing);
            *error = message.c_str();
            return false;
        }

        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };

        ImFontConfig iconConfig{};
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.FontDataOwnedByAtlas = false;

        ImFontConfig textConfig{};
        textConfig.PixelSnapH = true;
        const Source text = FirstFont({ L"tahoma.ttf", L"segoeui.ttf" });
        ImFont* font = text.Found() ? Add(atlas, text, kSize, textConfig) : atlas->AddFontDefault(&textConfig);
        if (!font || !Add(atlas, icons, kSize, iconConfig, iconRanges))
        {
            message = ErrorMessage(ErrorCode::FontAtlasFailed);
            *error = message.c_str();
            return false;
        }
        if (const Source symbols = SystemFont(L"seguisym.ttf"); symbols.Found())
        {
            ImFontConfig symbolConfig{};
            symbolConfig.MergeMode = true;
            Add(atlas, symbols, kSize, symbolConfig);
        }

        if (const Source bold = FirstFont({ L"tahomabd.ttf", L"segoeuib.ttf" }); bold.Found())
        {
            ImFontConfig boldConfig{};
            boldConfig.PixelSnapH = true;
            g_bold = Add(atlas, bold, kSize, boldConfig);
            if (g_bold)
                Add(atlas, icons, kSize, iconConfig, iconRanges);
        }

        if (const Source mono = FirstFont({ L"consola.ttf", L"CascadiaMono.ttf", L"cour.ttf", L"lucon.ttf" }); mono.Found())
        {
            ImFontConfig monoConfig{};
            monoConfig.PixelSnapH = true;
            if (text.Found())
                MatchMono(text, mono, monoConfig);
            // ∑ PixelSnapH rounds every advance to whole pixels: Consolas scaled to Tahoma's size has
            //   an advance of 6.65 px at 13 px, which snapped back to 7 and undid the match. Every
            //   glyph of a mono font has the same advance, so columns stay in line without it.
            if (monoConfig.ExtraSizeScale != 1.0f)
                monoConfig.PixelSnapH = false;
            g_mono = Add(atlas, mono, kMonoSize, monoConfig);
            ImFontConfig monoIcons = iconConfig;
            monoIcons.GlyphOffset.y = monoConfig.GlyphOffset.y;
            if (g_mono)
                Add(atlas, icons, kMonoSize, monoIcons, iconRanges);
        }

        ImGui::GetIO().FontDefault = font;
        return true;
    }

    ImFont* Bold()
    {
        return g_bold ? g_bold : ImGui::GetIO().FontDefault;
    }

    ImFont* Mono()
    {
        return g_mono ? g_mono : ImGui::GetIO().FontDefault;
    }
}
