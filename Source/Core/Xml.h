#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold::Xml
{
    struct Node
    {
        std::string Name;
        std::vector<std::pair<std::string, std::string>> Attributes;
        std::string Text;
        std::vector<std::unique_ptr<Node>> Children;

        const Node* Child(std::string_view name) const;
        Node* Child(std::string_view name);
        std::vector<const Node*> ChildrenNamed(std::string_view name) const;
        std::string ChildText(std::string_view name, std::string_view fallback = {}) const;
        std::optional<std::string> Attribute(std::string_view name) const;
        bool HasChild(std::string_view name) const { return Child(name) != nullptr; }

        Node& Add(std::string name, std::string text = {});
        Node& SetAttribute(std::string name, std::string value);
    };

    struct ParseResult
    {
        std::unique_ptr<Node> Root;
        std::string Error;
        int Line = 0;
    };

    ParseResult Parse(std::string_view text);
    std::string Write(const Node& root, bool header = true);
}
