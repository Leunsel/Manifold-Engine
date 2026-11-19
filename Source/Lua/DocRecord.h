#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold::Lua
{
    enum class DocItemKind : std::uint8_t
    {
        Function,
        Method,
        Property,
        Event,
        Table,
        Class,
        Constant,
        Keyword,
        Field
    };

    enum class DocOrigin : std::uint8_t
    {
        Reflection,
        CheatEngine,
        Standard,
        Annotation,
        Script,
        Manifold
    };

    enum class DocSupport : std::uint8_t
    {
        Full,
        NoEffect,
        Missing
    };

    struct DocParam
    {
        std::string Name;
        std::string Type;
        std::string Text;
        std::string Default;
        bool Optional = false;
        bool Variadic = false;
        std::vector<std::string> Choices;
        std::vector<std::pair<std::string, std::string>> Keys;
    };

    struct DocSignature
    {
        std::vector<DocParam> Params;
        std::string Returns;
        std::string ReturnsText;
    };

    struct DocRecord
    {
        std::uint32_t Id = 0;
        std::uint32_t Order = 0;
        std::string Path;
        std::string Owner;
        std::string Name;
        bool OwnerIsClass = false;
        DocItemKind Kind = DocItemKind::Function;
        DocOrigin Origin = DocOrigin::Reflection;
        std::string Title;
        std::string Summary;
        std::string Text;
        std::string Type;
        std::string Value;
        std::string Parent;
        std::string Deprecated;
        bool IsDeprecated = false;
        DocSupport Support = DocSupport::Full;
        std::string SupportNote;
        std::string Source;
        int Line = 0;
        std::vector<DocSignature> Signatures;
        std::vector<std::string> Choices;
        std::vector<std::string> See;
        bool Indexed = false;
        bool Hidden = false;
        bool Async = false;

        bool Callable() const { return Kind == DocItemKind::Function || Kind == DocItemKind::Method; }
    };

    using DocPtr = std::shared_ptr<const DocRecord>;

    const char* DocKindName(DocItemKind kind);
    const char* DocOriginName(DocOrigin origin);
    std::string FormatParam(const DocParam& param);
    std::string FormatParams(const DocSignature& signature);
    std::string FormatCall(const DocRecord& record, std::size_t signature, bool fullPath);
    std::string FirstSentence(std::string_view text, std::size_t limit = 200);
    void SplitPath(std::string_view path, std::string& owner, std::string& name);
}
