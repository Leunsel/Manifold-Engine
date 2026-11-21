#pragma once

#include "Lua/UiModel.h"

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Lua
{
    enum class FormPropType : std::uint8_t
    {
        String,
        Text,
        Bool,
        Number,
        Integer,
        Choice,
        Color,
        Icon,
        Items,
        Columns,
        Rows,
        Tree,
    };

    struct FormProp
    {
        std::string_view Key;
        FormPropType Type;
        std::string_view Help;
        std::string_view Choices = {};
    };

    struct FormHandler
    {
        std::string_view Key;
        std::string_view Params;
        std::string_view Help;
    };

    struct FormMeta
    {
        std::string Name;
        std::map<std::string, std::string> Handlers;
    };

    struct FormDocument
    {
        UiModel Model;
        int Window = 0;
        std::string Key;
        std::map<int, FormMeta> Meta;
    };

    enum class FormScriptStyle : std::uint8_t
    {
        Standalone,
        FormFile,
    };

    struct FormProblem
    {
        int Node = 0;
        std::string Text;
    };

    std::span<const FormProp> FormProps(UiKind kind);
    std::span<const FormHandler> FormHandlers(UiKind kind);
    std::span<const UiKind> FormKinds();
    std::string_view FormKindTitle(UiKind kind);
    std::string_view FormKindHelp(UiKind kind);
    bool FormCanHold(UiKind parent, UiKind child);

    FormDocument NewForm(std::string title);
    FormDocument CopyForm(const FormDocument& form);
    int FormAdd(FormDocument& form, UiKind kind, int parent, int index = -1);
    bool FormMove(FormDocument& form, int node, int parent, int index);
    bool FormRemove(FormDocument& form, int node);
    int FormDuplicate(FormDocument& form, int node);
    int FormIndexOf(const FormDocument& form, int node);
    std::string FormUniqueName(const FormDocument& form, std::string_view base, int except = 0);
    bool FormIsIdentifier(std::string_view name);

    std::string FormPropValue(const UiNode& node, std::string_view key);
    bool FormSetProp(UiNode& node, std::string_view key, std::string_view value);
    bool FormPropIsDefault(const UiNode& node, std::string_view key);
    std::string FormPropLiteral(const UiNode& node, std::string_view key);

    std::vector<FormProblem> FormProblems(const FormDocument& form);

    std::string FormSave(const FormDocument& form);
    bool FormLoad(std::string_view text, FormDocument& form, std::string& error, std::vector<std::string>* warnings = nullptr);
    std::string FormScript(const FormDocument& form, FormScriptStyle style, std::string_view formFile = {});
}
