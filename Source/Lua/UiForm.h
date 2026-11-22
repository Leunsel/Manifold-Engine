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
        File,
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
        std::string Opens;
        bool Closes = false;
        std::string EnabledWhen; // "god", "not god" or "mode = Multiplier, Double" (see FormParseWhen)
    };

    // enabledWhen: the widget is enabled while another one says so.
    struct FormRule
    {
        std::string Source;
        bool Negate = false;
        std::vector<std::string> Values; // empty: the source is on / has a choice
    };

    struct FormNode
    {
        float X = 0.0f;
        float Y = 0.0f;
        bool Folded = false;
    };

    struct FormFunction
    {
        std::size_t Begin = 0;
        std::size_t HeaderEnd = 0;
        std::size_t BodyBegin = 0;
        std::size_t BodyEnd = 0;
        std::size_t End = 0;
        std::string Indent;
        std::string Params;
    };

    struct FormDocument
    {
        UiModel Model;
        int Window = 0;
        std::string Key;
        std::map<int, FormMeta> Meta;
        bool Fragment = false;
        std::string Script;
        float CanvasX = 40.0f;
        float CanvasY = 30.0f;
        std::map<std::string, FormNode> Nodes;
        std::vector<int> Modals;
        std::map<std::string, FormNode> ModalNodes;
        std::string ScriptText;
        bool ScriptLoaded = false;
    };

    struct FormTemplate
    {
        std::string_view Id;
        std::string_view Title;
        std::string_view Help;
        bool Snippet = false;
        std::string_view Text;
    };

    struct FormMerge
    {
        std::string Text;
        std::vector<std::string> Added;
        bool Anchored = false;
    };

    enum class FormScriptStyle : std::uint8_t
    {
        Standalone,
        FormFile,
        OneFile, // the script as the editor shows it: the form table as kFormStandIn
    };

    // A form kept inside its script: "local <Name> = { form = 1, ... }".
    struct FormBlock
    {
        std::size_t Begin = 0; // the "local"
        std::size_t End = 0;   // just after the closing brace
        std::size_t Table = 0; // the opening brace
        std::string Name;
        int Line = 1;          // line of the opening brace
    };

    // What the editor shows in place of the form table: it builds an empty
    // table, so the script stays valid Lua for highlighting and completion.
    inline constexpr std::string_view kFormStandIn = "{ --[[ edited on the canvas ]] }";

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
    int FormGraft(FormDocument& form, const FormDocument& from, int node, int parent, int index, std::map<std::string, std::string>* renames = nullptr);
    std::string FormFragment(const FormDocument& form, const std::vector<int>& nodes);
    std::vector<int> FormPaste(FormDocument& form, std::string_view text, int parent, int index, std::string& error);
    std::span<const FormTemplate> FormTemplates();
    std::vector<std::pair<std::string, std::string>> FormHandlerList(const FormDocument& form);
    FormMerge FormMergeScript(std::string_view script, const FormDocument& form);
    int FormFunctionLine(std::string_view script, std::string_view name);
    std::optional<FormFunction> FormFindFunction(std::string_view script, std::string_view name);
    std::string FormFunctionBody(std::string_view script, std::string_view name);
    std::optional<std::string> FormReplaceBody(std::string_view script, std::string_view name, std::string_view body);
    int FormIndexOf(const FormDocument& form, int node);
    int FormAddModal(FormDocument& form, std::string_view base = "dialog");
    bool FormIsModal(const FormDocument& form, int node);
    int FormRootWindow(const FormDocument& form, int node);
    std::string FormModalName(const FormDocument& form, int window);
    bool FormRenameModal(FormDocument& form, int window, const std::string& name);
    std::vector<int> FormWindows(const FormDocument& form);
    std::string FormUniqueName(const FormDocument& form, std::string_view base, int except = 0);
    bool FormIsIdentifier(std::string_view name);

    std::string FormPropValue(const UiNode& node, std::string_view key);
    bool FormSetProp(UiNode& node, std::string_view key, std::string_view value);
    bool FormPropIsDefault(const UiNode& node, std::string_view key);
    std::string FormPropDefault(UiKind kind, std::string_view key);
    std::string FormPropLiteral(const UiNode& node, std::string_view key);

    std::vector<FormProblem> FormProblems(const FormDocument& form);

    std::optional<FormRule> FormParseWhen(std::string_view text);
    std::string FormWhenText(const FormRule& rule);
    int FormFindNamed(const FormDocument& form, std::string_view name);
    bool FormRuleHolds(const FormDocument& form, const FormRule& rule);
    void FormApplyRules(FormDocument& form);
    void FormRenameReferences(FormDocument& form, const std::string& from, const std::string& to);

    std::string FormSave(const FormDocument& form);
    bool FormLoad(std::string_view text, FormDocument& form, std::string& error, std::vector<std::string>* warnings = nullptr);

    // One file: the form lives in its script as a block the editor owns.
    std::string FormBlockText(const FormDocument& form, std::string_view name);
    std::optional<FormBlock> FormFindBlock(std::string_view script);
    std::string FormBlockChunk(std::string_view script);
    bool FormLoadScript(std::string_view script, FormDocument& form, std::string& error, std::vector<std::string>* warnings = nullptr);
    std::string FormScriptView(std::string_view script);
    bool FormViewIntact(std::string_view view);
    std::optional<std::string> FormComposeScript(std::string_view view, const FormDocument& form);
    std::string FormConvertScript(std::string_view script);
    std::string FormScript(const FormDocument& form, FormScriptStyle style, std::string_view formFile = {});
}
