#include "UI/ProcessIcon.h"

#include "UI/Workspace.h"

#include "imgui.h"

#include <cstdint>
#include <vector>

#include <windows.h>
#include <shlobj.h>
#include <d3d11.h>

namespace Manifold::UI
{
    namespace
    {
        std::vector<std::uint8_t> IconPixels(HICON icon, int size)
        {
            std::vector<std::uint8_t> pixels;
            ICONINFO parts{};
            if (!::GetIconInfo(icon, &parts) || !parts.hbmColor)
            {
                if (parts.hbmMask)
                    ::DeleteObject(parts.hbmMask);
                return pixels;
            }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = size;
            info.bmiHeader.biHeight = -size;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            std::vector<std::uint8_t> bgra(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
            HDC dc = ::CreateCompatibleDC(nullptr);
            if (::GetDIBits(dc, parts.hbmColor, 0, static_cast<UINT>(size), bgra.data(), &info, DIB_RGB_COLORS) == size)
            {
                bool anyAlpha = false;
                for (std::size_t i = 3; i < bgra.size(); i += 4)
                    anyAlpha = anyAlpha || bgra[i] != 0;
                if (!anyAlpha && parts.hbmMask)
                {
                    std::vector<std::uint8_t> mask(bgra.size());
                    if (::GetDIBits(dc, parts.hbmMask, 0, static_cast<UINT>(size), mask.data(), &info, DIB_RGB_COLORS) == size)
                    {
                        for (std::size_t i = 0; i < bgra.size(); i += 4)
                            bgra[i + 3] = mask[i] ? 0 : 255;
                    }
                }
                pixels.resize(bgra.size());
                for (std::size_t i = 0; i < bgra.size(); i += 4)
                {
                    pixels[i] = bgra[i + 2];
                    pixels[i + 1] = bgra[i + 1];
                    pixels[i + 2] = bgra[i];
                    pixels[i + 3] = bgra[i + 3];
                }
            }
            ::DeleteDC(dc);
            ::DeleteObject(parts.hbmColor);
            if (parts.hbmMask)
                ::DeleteObject(parts.hbmMask);
            return pixels;
        }
    }

    IconCache::~IconCache()
    {
        Clear();
    }

    void IconCache::Clear()
    {
        for (const auto& [key, view] : m_entries)
        {
            if (view)
                static_cast<ID3D11ShaderResourceView*>(view)->Release();
        }
        m_entries.clear();
    }

    void* IconCache::Texture(Shell& shell, const std::wstring& path, int size, int budget)
    {
        if (path.empty() || size <= 0)
            return nullptr;
        const auto key = std::make_pair(path, size);
        if (const auto found = m_entries.find(key); found != m_entries.end())
            return found->second;
        if (ImGui::GetFrameCount() != m_frame)
        {
            m_frame = ImGui::GetFrameCount();
            m_made = 0;
        }
        if (budget > 0 && m_made >= budget)
            return nullptr;
        ++m_made;
        if (m_entries.size() >= 768)
            Clear();
        void*& slot = m_entries[key];
        ID3D11Device* device = static_cast<ID3D11Device*>(shell.Device());
        if (!device)
            return nullptr;
        HICON icon = nullptr;
        if (::SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, static_cast<UINT>(size)) != S_OK || !icon)
            return nullptr;
        const std::vector<std::uint8_t> pixels = IconPixels(icon, size);
        ::DestroyIcon(icon);
        if (pixels.empty())
            return nullptr;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(size);
        description.Height = static_cast<UINT>(size);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = description.Width * 4;
        ID3D11Texture2D* texture = nullptr;
        if (FAILED(device->CreateTexture2D(&description, &data, &texture)) || !texture)
            return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView* resource = nullptr;
        const HRESULT made = device->CreateShaderResourceView(texture, &view, &resource);
        texture->Release();
        if (SUCCEEDED(made))
            slot = resource;
        return slot;
    }
}
