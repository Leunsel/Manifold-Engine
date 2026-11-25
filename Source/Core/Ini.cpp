#include "Core/Ini.h"

#include "Core/SafeFile.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include <windows.h>

namespace Manifold::Ini
{
    std::vector<Section> Parse(std::string_view text)
    {
        std::vector<Section> sections(1);
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            const std::size_t first = line.find_first_not_of(" \t");
            if (first == std::string_view::npos || line[first] == ';' || line[first] == '#')
                continue;
            const std::size_t last = line.find_last_not_of(" \t");
            if (line[first] == '[' && line[last] == ']' && last > first)
            {
                sections.push_back({ std::string(line.substr(first + 1, last - first - 1)), {} });
                continue;
            }
            const std::size_t equals = line.find('=');
            if (equals == std::string_view::npos)
                continue;
            sections.back().Entries.push_back({ std::string(line.substr(0, equals)), std::string(line.substr(equals + 1)) });
        }
        if (sections.front().Entries.empty())
            sections.erase(sections.begin());
        return sections;
    }

    std::optional<std::string> ReadText(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;
        std::ostringstream content;
        content << file.rdbuf();
        return content.str();
    }

    bool WriteAtomic(const std::filesystem::path& path, std::string_view text)
    {
        if (path.has_parent_path())
        {
            std::error_code ignored;
            std::filesystem::create_directories(path.parent_path(), ignored);
        }
        // ∑ The settings, themes and module state went to a fixed .tmp without a flush: a
        //   power loss could leave the ini empty, and the next start would save defaults over it.
        return WriteFileSafely(path, text, true);
    }
}
