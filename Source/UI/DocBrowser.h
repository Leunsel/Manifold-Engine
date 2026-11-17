#pragma once

#include "Lua/DocRegistry.h"

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class DocBrowser
    {
    public:
        void Draw(const Lua::DocRegistry& registry, ImVec2 size, const std::function<void(const std::string&)>& insert);
        void FocusSearch() { m_focusSearch = true; }

    private:
        struct Group
        {
            std::string Title;
            std::vector<Lua::DocPtr> Items;
            bool Flat = false;
        };

        void Rebuild(const Lua::DocRegistry& registry);
        void Leaf(const Lua::DocPtr& record, const std::string& label, const std::function<void(const std::string&)>& insert);
        void Branch(const Lua::DocRegistry& registry, const Lua::DocPtr& record, const std::function<void(const std::string&)>& insert, int depth);

        std::vector<Group> m_groups;
        std::uint64_t m_generation = ~0ull;
        std::string m_search;
        std::string m_resultsQuery;
        std::uint64_t m_resultsGeneration = ~0ull;
        std::vector<Lua::DocPtr> m_results;
        Lua::DocPtr m_selected;
        bool m_focusSearch = false;
        float m_treeShare = 0.55f;
    };

    std::string InsertText(const Lua::DocRecord& record);
}
