#pragma once

#include <span>

namespace Manifold::Lua
{
    enum class DocKind
    {
        Function,
        Method,
        Property
    };

    struct DocEntry
    {
        const char* Owner;
        const char* Name;
        const char* Params;
        const char* Text;
        DocKind Kind;
    };

    struct DocClass
    {
        const char* Name;
        const char* Parent;
    };

    std::span<const DocEntry> DocEntries();
    std::span<const DocClass> DocClasses();
}
