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
        Hotkey,
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
        std::string Record;      // record = a memory record's description, or its id when RecordById
        bool RecordById = false;
    };

    // the kinds a memory record can be bound to (record = ...)
    bool FormCanBind(UiKind kind);

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

    // what the problems list can repair with one click
    enum class FormFix
    {
        None,
        Rename,      // a Lua name that no other widget has
        NameHandler, // Detail = the handler key: named after the widget
        AddTab,
        SeedItems,
        ClampValue,
        ClearValue,  // a colour picker's value
        ClearIcon,
        ClearColour, // Detail = "color" or "background"
        MultiOff,
        Title,
        ClearOpens,
        ClearRule,
        ModalName,
        ClearHotkey,
    };

    struct FormProblem
    {
        int Node = 0;
        std::string Text;
        FormFix Fix = FormFix::None;
        std::string Detail;
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
    // the functions a script defines (function name(...) and function handlers.name(...)), in order
    std::vector<std::string> FormScriptFunctions(std::string_view script);
    // adds a line at the end of a function's body, indented like its body
    std::optional<std::string> FormAddLine(std::string_view script, std::string_view name, std::string_view line);
    // a Lua string literal for a text
    std::string FormLuaString(std::string_view text);
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

    // A record of the open table as the trainer generator sees it (the Table
    // library stays out of the form model).
    struct FormRecordInfo
    {
        enum class Type : std::uint8_t
        {
            Value,
            Script,
            Group,
        };
        int Id = 0;
        std::string Description;
        Type Kind = Type::Value;
        bool Unique = true;       // no other record has the description: bound by it, else by id
        bool Integer = true;
        bool Hex = false;
        bool Text = false;        // a string or byte array: a text field
        std::vector<std::string> DropDown;
        bool TableHotkey = false; // the table switches it by a hotkey already
        std::vector<FormRecordInfo> Children;
    };

    struct FormTrainerOptions
    {
        std::string Title = "Trainer";
        bool Values = true;                 // value records become fields; off: only switches
        bool Hotkeys = true;                // switches get F1, F2, ... (keys the table uses are skipped)
        std::vector<std::string> TakenKeys; // hotkeys of the table, as Table::KeysText writes them
    };

    struct FormTrainerResult
    {
        FormDocument Form;
        int Switches = 0;
        int Fields = 0;
        int Sections = 0;
        int Hotkeys = 0;
        int Skipped = 0; // past the widget limit
    };

    // A trainer from the records: groups become sections, scripts switches,
    // values fields bound to them; what belongs to a script sits indented
    // below its switch and is enabled while it is on.
    FormTrainerResult FormFromRecords(const std::vector<FormRecordInfo>& records, const FormTrainerOptions& options);

    std::vector<FormProblem> FormProblems(const FormDocument& form);
    std::string_view FormFixLabel(FormFix fix);
    bool FormApplyFix(FormDocument& form, const FormProblem& problem);

    // Change Kind keeps the widget, its children, name and handlers: rows,
    // columns, grids and sections into each other, number and slider, combo
    // and radio, text and heading.
    std::vector<UiKind> FormKindChoices(UiKind kind);
    bool FormChangeKind(FormDocument& form, int node, UiKind kind);
    // Unwrap moves a container's widgets into its parent, in its place.
    bool FormUnwrap(FormDocument& form, int node);
    // a Lua name for a text ("Infinite Health" -> infiniteHealth) no widget has
    std::string FormNameFromText(const FormDocument& form, std::string_view text);
    // renames a Lua name in a script where it stands as a name of its own or
    // as a handler (handlers.name, handlers["name"]): not in strings or
    // comments, not another table's member (w.name, obj:name()), not in the
    // form table's line; a rename FormCanRenameIdentifier refuses leaves the
    // script as it is
    std::string FormRenameIdentifier(std::string_view text, std::string_view from, std::string_view to);
    // false (and why) when renaming `from` to `to` would break the script: no
    // Lua names, the form table's variable or `handlers`, or `to` in use already
    bool FormCanRenameIdentifier(std::string_view text, std::string_view from, std::string_view to, std::string* why = nullptr);

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
