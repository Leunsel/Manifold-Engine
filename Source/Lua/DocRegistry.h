#pragma once

#include "Lua/DocRecord.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Manifold::Lua
{
    class DocRegistry
    {
    public:
        explicit DocRegistry(std::shared_ptr<const DocRegistry> base = nullptr);
        DocRegistry(const DocRegistry&) = delete;
        DocRegistry& operator=(const DocRegistry&) = delete;

        DocPtr Add(DocRecord record);
        bool Remove(std::string_view path);
        bool RemoveId(std::uint32_t id);
        void AddClassAlias(std::string_view alias, std::string_view target);

        DocPtr Get(std::uint32_t id) const;
        DocPtr Find(std::string_view path) const;
        std::vector<DocPtr> Members(std::string_view owner) const;
        std::vector<DocPtr> ClassMembers(std::string_view className) const;
        std::string ResolveClass(std::string_view name) const;
        std::string ParentOf(std::string_view className) const;
        std::vector<DocPtr> Search(std::string_view query, std::size_t limit) const;
        std::vector<DocPtr> All() const;
        std::uint64_t Generation() const;
        std::size_t Size() const;
        const DocRegistry* Base() const { return m_base.get(); }

    private:
        bool RemoveKey(const std::string& key);
        std::string AliasOf(const std::string& key) const;
        DocPtr FindIn(std::string_view path, bool exact) const;

        std::shared_ptr<const DocRegistry> m_base;
        mutable std::shared_mutex m_mutex;
        std::unordered_map<std::string, DocPtr> m_byPath;
        std::unordered_map<std::string, std::vector<std::string>> m_byLower;
        std::unordered_map<std::uint32_t, DocPtr> m_byId;
        std::unordered_map<std::string, std::vector<std::string>> m_byOwner;
        std::unordered_map<std::string, std::string> m_aliases;
        std::atomic<std::uint64_t> m_generation{ 0 };
        std::uint32_t m_order = 0;
    };

    std::string DocKey(std::string_view path);
    std::string ExactKey(std::string_view path);
    std::string MarkdownCell(std::string_view text);
    std::string RenderMarkdown(const DocRegistry& registry, std::string_view prefix);
    void AddCheatEngineDocs(DocRegistry& registry);
}
