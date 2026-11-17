#pragma once

#include "Lua/DocRecord.h"

#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Lua
{
    struct AnnotatedFunction
    {
        int Line = 0;
        int CommentLine = 0;
        bool Local = false;
        bool Documented = false;
        DocRecord Doc;
    };

    struct AnnotatedAlias
    {
        std::string Name;
        std::string Type;
        std::vector<std::string> Choices;
    };

    struct AnnotatedFile
    {
        std::vector<AnnotatedFunction> Functions;
        std::vector<DocRecord> Classes;
        std::vector<DocRecord> Fields;
        std::vector<AnnotatedAlias> Aliases;

        const AnnotatedFunction* At(int line) const;
        const AnnotatedFunction* Named(std::string_view path) const;
    };

    AnnotatedFile ParseAnnotations(std::string_view text, std::string_view source = {});
}
