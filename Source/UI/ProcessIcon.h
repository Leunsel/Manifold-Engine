#pragma once

#include <map>
#include <string>
#include <utility>

namespace Manifold::UI
{
    class Shell;

    class IconCache
    {
    public:
        IconCache() = default;
        ~IconCache();
        IconCache(const IconCache&) = delete;
        IconCache& operator=(const IconCache&) = delete;

        void* Texture(Shell& shell, const std::wstring& path, int size, int budget = 0);
        void Clear();

    private:
        std::map<std::pair<std::wstring, int>, void*> m_entries;
        int m_frame = -1;
        int m_made = 0;
    };
}
