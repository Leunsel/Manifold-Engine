#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Manifold::Text
{
    struct JsonProblem
    {
        std::size_t Offset = 0;
        int Line = 1;
        int Column = 1;
        std::string Message;
    };

    struct JsonFormatted
    {
        std::string Text;
        std::optional<JsonProblem> Problem;
    };

    JsonFormatted FormatJson(std::string_view text, int indent);
    std::optional<JsonProblem> CheckJson(std::string_view text);
    bool LooksLikeJson(std::string_view text);
}
