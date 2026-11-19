#pragma once

#include "imgui.h"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold
{
    enum class CommandScope
    {
        Global,
        Main,
        AddressList,
        ScanResults,
        MemoryViewer,
        Disassembler,
        HexView,
        Structure,
        ScriptEditor,
        LuaEngine,
        CodeEditor,
        FlowGraph,
        Log,
        Count
    };

    enum class Command
    {
        OpenProcess,
        OpenTable,
        SaveTable,
        SaveTableAs,
        OpenMemoryViewer,
        OpenStructureDissect,
        OpenAutoAssembler,
        OpenLuaEngine,
        OpenSettings,
        OpenShortcuts,
        OpenLog,
        OpenMono,
        OpenUnreal,
        OpenProfiler,

        NewTable,
        OpenLastProcess,
        TableLuaScript,
        TableFiles,
        AddAddress,
        AddGroup,
        AddScript,
        DeactivateAll,
        FocusScanValue,
        FirstScan,
        NewScan,
        UndoScan,
        CompactMode,
        ReleaseMode,

        ToggleRecords,
        ChangeRecordValue,
        EditValueInline,
        ChangeDescription,
        EditRecord,
        AssignHotkey,
        UndoRecordValue,
        ShowAsHex,
        FreezeAllowIncrease,
        FreezeAllowDecrease,
        BrowseRecord,
        DisassembleRecord,
        DissectRecord,
        RecordAccesses,
        RecordWrites,
        EditScript,
        GroupRecords,
        ExpandGroups,
        CollapseGroups,
        CutRecords,
        CopyRecords,
        PasteRecords,
        SelectAllRecords,
        DeleteRecords,

        AddResults,
        ChangeResultValue,
        BrowseResult,
        DisassembleResult,
        DissectResult,
        ResultAccesses,
        ResultWrites,
        CopyResults,
        SelectAllResults,

        NewViewerWindow,
        SaveMemoryRegion,
        LoadMemoryRegion,
        GotoAddress,
        FindMemory,
        FindAssembly,
        UndoMemoryEdit,
        MemoryRegions,
        ReferencedFunctions,
        ReferencedStrings,
        SymbolsAndModules,
        DissectCode,
        DissectData,
        ViewerAutoAssemble,
        ViewerLuaEngine,
        InjectLibrary,
        AllocateMemory,
        FillMemory,
        CreateThread,
        ScanForPatches,
        DebugRun,
        DebugStepInto,
        DebugStepOver,
        DebugRunToReturn,
        DebugBreak,
        ToggleBreakpoint,
        SetInstructionPointer,
        BreakpointList,
        EditBreakpoint,
        BreakAndTrace,
        ThreadList,

        FollowInstruction,
        NavigateBack,
        AssembleInstruction,
        NopInstruction,
        CopyInstructions,
        CopyInstructionBytes,
        InstructionAccesses,
        CodeReferences,
        FlowGraph,
        GotoFunctionStart,
        GotoFunctionEnd,
        SelectFunction,
        AddInstructionToList,
        SetBookmark,
        GotoBookmark,
        CommentInstruction,

        HexFollow,
        HexBack,
        HexEditValue,
        HexCopy,
        HexPaste,
        HexAccesses,
        HexWrites,
        HexBreakOnWrite,
        HexBreakOnAccess,
        HexAddToList,
        HexBytes,
        HexTwoBytes,
        HexFourBytes,
        HexFourBytesDecimal,
        HexEightBytes,
        HexFloat,
        HexDouble,

        DefineStructure,
        AddStructureColumn,
        CollapseStructure,
        ChangeFieldValue,
        RenameField,
        InsertField,
        DeleteField,
        FieldToAddressList,
        BrowseField,
        BrowseFieldPointer,
        FieldAccesses,
        FieldWrites,

        ScriptNewWindow,
        ScriptOpen,
        ScriptSave,
        ScriptSaveAs,
        ScriptCheck,
        ScriptExecute,
        ScriptExecuteDisable,
        TemplateCodeInjection,
        TemplateAobInjection,
        TemplateFullInjection,
        TemplateCallLua,
        TemplateFramework,

        LuaOpen,
        LuaSave,
        LuaSaveAs,
        LuaExecute,
        LuaClearOutput,
        LuaFunctions,
        LuaStop,
        LuaConsoleTab,
        LuaScriptTab,
        LuaFind,

        EditorComplete,
        EditorToggleComment,
        EditorDuplicateLines,
        EditorMoveLinesUp,
        EditorMoveLinesDown,
        EditorDeleteLines,
        EditorGotoLine,

        FlowGraphFit,
        FlowGraphActualSize,
        FlowGraphBack,

        LogCopy,
        LogSelectAll,
        LogFind,

        Count
    };

    struct CommandInfo
    {
        Command Id;
        const char* Key;
        CommandScope Scope;
        const char* Name;
        ImGuiKeyChord Chord;
        ImGuiKeyChord Alternate = 0;
        bool FromCheatEngine = false;
        bool Digits = false;
    };

    struct Binding
    {
        ImGuiKeyChord Chord = 0;
        ImGuiKeyChord Alternate = 0;

        bool operator==(const Binding&) const = default;
    };

    const CommandInfo& Describe(Command command);
    std::span<const CommandInfo> AllCommands();
    std::optional<Command> FindCommand(std::string_view key);
    bool FromTemplateSettings(Command command);
    const char* ScopeName(CommandScope scope);
    const char* ScopeHint(CommandScope scope);
    std::string ChordText(ImGuiKeyChord chord, bool digits = false);
    ImGuiKeyChord ParseChord(std::string_view text);
    const char* ShortcutText(Command command);
    std::string ShortcutLabel(Command command);
    std::string ShortcutsMarkdown();

    Binding DefaultBinding(Command command);
    Binding CurrentBinding(Command command);
    void SetBinding(Command command, Binding binding);
    bool IsCustomized(Command command);
    std::string BindingText(Command command, Binding binding);
    std::optional<Binding> ParseBinding(Command command, std::string_view text);
    void ApplyShortcutSettings(const std::map<std::string, std::string>& saved);
    std::map<std::string, std::string> ShortcutSettings();
    std::vector<Command> ConflictsWith(Command command, ImGuiKeyChord chord);
}
