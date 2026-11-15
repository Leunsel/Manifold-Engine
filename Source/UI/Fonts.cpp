#include "UI/Fonts.h"

#include "Core/Errors.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Resource.h"

#include "imgui.h"

#include <string>

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
            g_mono = Add(atlas, mono, kMonoSize, monoConfig);
            if (g_mono)
                Add(atlas, icons, kMonoSize, iconConfig, iconRanges);
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
