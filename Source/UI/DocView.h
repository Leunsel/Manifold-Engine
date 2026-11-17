#pragma once

#include "Lua/DocRecord.h"

#include "imgui.h"

#include <string>
#include <string_view>
#include <vector>

namespace Manifold::UI
{
    struct DocViewOptions
    {
        float Width = 420.0f;
        int Signature = 0;
        int Parameter = -1;
        bool SignatureHelp = false;
        bool Detail = true;
    };

    struct DocViewResult
    {
        std::string Choice;
        int SignatureStep = 0;
    };

    DocViewResult DocView(const Lua::DocRecord& record, const DocViewOptions& options);
    void RichText(std::string_view text, float width, const ImVec4& color);
    std::string ChoiceChips(const std::vector<std::string>& values, float width, bool interactive);
    int BestSignature(const Lua::DocRecord& record, int argument);
    const char* DocKindIcon(Lua::DocItemKind kind);
    ImVec4 DocKindColor(Lua::DocItemKind kind);
    const char* DocOriginLabel(Lua::DocOrigin origin);
}
