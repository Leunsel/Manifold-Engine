#include "App/Commands.h"

#include "Core/Text.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

namespace Manifold
{
    namespace
    {
        constexpr ImGuiKeyChord Ctrl = ImGuiMod_Ctrl;
        constexpr ImGuiKeyChord Shift = ImGuiMod_Shift;
        constexpr ImGuiKeyChord Alt = ImGuiMod_Alt;
        constexpr bool CE = true;
        constexpr bool Own = false;

        using enum Command;
        using Scope = CommandScope;

        constexpr CommandInfo kCommands[] = {
            { OpenProcess, "OpenProcess", Scope::Global, "Open Process", Ctrl | ImGuiKey_P, 0, CE },
            { OpenTable, "OpenTable", Scope::Global, "Open Table", Ctrl | ImGuiKey_O, 0, CE },
            { SaveTable, "SaveTable", Scope::Global, "Save Table", Ctrl | ImGuiKey_S, 0, CE },
            { SaveTableAs, "SaveTableAs", Scope::Global, "Save Table As", Ctrl | Alt | ImGuiKey_S, Ctrl | Shift | ImGuiKey_S, CE },
            { OpenMemoryViewer, "OpenMemoryViewer", Scope::Global, "Memory Viewer", Ctrl | ImGuiKey_M, 0, CE },
            { OpenStructureDissect, "OpenStructureDissect", Scope::Global, "Structure Dissect", Ctrl | Alt | ImGuiKey_D, 0, CE },
            { OpenAutoAssembler, "OpenAutoAssembler", Scope::Global, "Auto Assembler", Ctrl | Alt | ImGuiKey_A, 0, CE },
            { OpenLuaEngine, "OpenLuaEngine", Scope::Global, "Lua Engine", Ctrl | Alt | Shift | ImGuiKey_L, Ctrl | ImGuiKey_L, CE },
            { OpenSettings, "OpenSettings", Scope::Global, "Settings", Ctrl | ImGuiKey_Comma, 0, Own },
            { OpenShortcuts, "OpenShortcuts", Scope::Global, "Keyboard Shortcuts", ImGuiKey_F1, 0, Own },
            { OpenLog, "OpenLog", Scope::Global, "Log", Ctrl | Shift | ImGuiKey_L, 0, Own },
            { OpenMono, "OpenMono", Scope::Global, "Mono / .NET Dissector", Ctrl | Alt | ImGuiKey_M, 0, Own },
            { OpenUnreal, "OpenUnreal", Scope::Global, "Unreal Engine", Ctrl | Alt | ImGuiKey_U, 0, Own },
            { OpenProfiler, "OpenProfiler", Scope::Global, "Profiler and Diagnostics", Ctrl | Shift | ImGuiKey_F12, 0, Own },
            { OpenLayoutProblems, "OpenLayoutProblems", Scope::Global, "Layout Problems", Ctrl | Shift | ImGuiKey_F11, 0, Own },
            { OpenPointerScan, "OpenPointerScan", Scope::Global, "Pointer Scan", Ctrl | Alt | ImGuiKey_P, 0, Own },
            { OpenStaticPointers, "OpenStaticPointers", Scope::Global, "Find Static Pointers", Ctrl | Alt | ImGuiKey_K, 0, Own },
            { OpenFormEditor, "OpenFormEditor", Scope::Global, "Form Editor", Ctrl | Alt | ImGuiKey_G, 0, Own },
            { OpenOpcodeConverter, "OpenOpcodeConverter", Scope::Global, "Opcode Converter", Ctrl | Alt | ImGuiKey_O, 0, Own },
            { OpenRecordDebug, "OpenRecordDebug", Scope::Global, "Address List Debug", Ctrl | Alt | Shift | ImGuiKey_D, 0, Own },

            { NewTable, "NewTable", Scope::Main, "New Table", Ctrl | ImGuiKey_N, 0, Own },
            { OpenLastProcess, "OpenLastProcess", Scope::Main, "Open Last Process", Ctrl | Shift | ImGuiKey_P, 0, Own },
            { TableLuaScript, "TableLuaScript", Scope::Main, "Table Lua Script", Ctrl | Alt | ImGuiKey_L, 0, CE },
            { TableFiles, "TableFiles", Scope::Main, "Table Files", Ctrl | Alt | ImGuiKey_F, 0, Own },
            { AddAddress, "AddAddress", Scope::Main, "Add Address Manually", ImGuiKey_Insert, 0, Own },
            { AddGroup, "AddGroup", Scope::Main, "Add Group", Ctrl | Shift | ImGuiKey_N, 0, Own },
            { AddScript, "AddScript", Scope::Main, "Add Auto Assembler Script", Ctrl | Shift | ImGuiKey_A, 0, Own },
            { DeactivateAll, "DeactivateAll", Scope::Main, "Deactivate All Records", Ctrl | Shift | ImGuiKey_D, 0, Own },
            { FocusScanValue, "FocusScanValue", Scope::Main, "Type Scan Value", Ctrl | ImGuiKey_F, 0, Own },
            { FirstScan, "FirstScan", Scope::Main, "First Scan / Next Scan", ImGuiKey_F3, 0, Own },
            { NewScan, "NewScan", Scope::Main, "New Scan", Shift | ImGuiKey_F3, 0, Own },
            { UndoScan, "UndoScan", Scope::Main, "Undo Last Scan", Ctrl | Alt | ImGuiKey_Z, 0, Own },
            { CompactMode, "CompactMode", Scope::Main, "Compact Mode", Ctrl | Shift | ImGuiKey_C, 0, Own },
            { ReleaseMode, "ReleaseMode", Scope::Main, "Release Mode", Ctrl | Shift | ImGuiKey_R, 0, Own },
            { ToggleSpeedhack, "ToggleSpeedhack", Scope::Main, "Enable / Disable Speedhack", Ctrl | Shift | ImGuiKey_H, 0, Own },
            { SpeedhackFaster, "SpeedhackFaster", Scope::Main, "Speedhack: Faster", Ctrl | Alt | ImGuiKey_UpArrow, 0, Own },
            { SpeedhackSlower, "SpeedhackSlower", Scope::Main, "Speedhack: Slower", Ctrl | Alt | ImGuiKey_DownArrow, 0, Own },

            { ToggleRecords, "ToggleRecords", Scope::AddressList, "Activate / Deactivate", ImGuiKey_Space, 0, CE },
            { ChangeRecordValue, "ChangeRecordValue", Scope::AddressList, "Change Value", ImGuiKey_Enter, 0, CE },
            { EditValueInline, "EditValueInline", Scope::AddressList, "Edit Value in Place", ImGuiKey_F2, 0, Own },
            { ChangeDescription, "ChangeDescription", Scope::AddressList, "Change Description", Ctrl | ImGuiKey_Enter, 0, CE },
            { EditRecord, "EditRecord", Scope::AddressList, "Edit Record", Ctrl | Alt | ImGuiKey_Enter, Alt | ImGuiKey_Enter, CE },
            { AssignHotkey, "AssignHotkey", Scope::AddressList, "Assign Hotkey", Ctrl | ImGuiKey_H, 0, CE },
            { UndoRecordValue, "UndoRecordValue", Scope::AddressList, "Undo Last Value Change", Ctrl | ImGuiKey_Z, 0, CE },
            { ShowAsHex, "ShowAsHex", Scope::AddressList, "Show as Hexadecimal", Ctrl | Alt | ImGuiKey_H, 0, CE },
            { FreezeAllowIncrease, "FreezeAllowIncrease", Scope::AddressList, "Freeze, Allow Increase", ImGuiKey_KeypadAdd, 0, CE },
            { FreezeAllowDecrease, "FreezeAllowDecrease", Scope::AddressList, "Freeze, Allow Decrease", ImGuiKey_KeypadSubtract, 0, CE },
            { BrowseRecord, "BrowseRecord", Scope::AddressList, "Browse This Memory Region", Ctrl | ImGuiKey_B, 0, CE },
            { DisassembleRecord, "DisassembleRecord", Scope::AddressList, "Disassemble This Memory Region", Ctrl | ImGuiKey_D, 0, CE },
            { DissectRecord, "DissectRecord", Scope::AddressList, "Dissect Data at This Address", Ctrl | Alt | ImGuiKey_D, 0, Own },
            { RecordAccesses, "RecordAccesses", Scope::AddressList, "Find Out What Accesses This Address", ImGuiKey_F5, 0, CE },
            { RecordWrites, "RecordWrites", Scope::AddressList, "Find Out What Writes to This Address", ImGuiKey_F6, 0, CE },
            { EditScript, "EditScript", Scope::AddressList, "Edit Script", Ctrl | ImGuiKey_E, 0, Own },
            { GroupRecords, "GroupRecords", Scope::AddressList, "Group Selected Records", Ctrl | ImGuiKey_G, 0, Own },
            { ExpandGroups, "ExpandGroups", Scope::AddressList, "Expand All Groups", Ctrl | Alt | ImGuiKey_X, 0, Own },
            { CollapseGroups, "CollapseGroups", Scope::AddressList, "Collapse All Groups", Ctrl | Alt | ImGuiKey_C, 0, Own },
            { CutRecords, "CutRecords", Scope::AddressList, "Cut", Ctrl | ImGuiKey_X, 0, CE },
            { CopyRecords, "CopyRecords", Scope::AddressList, "Copy", Ctrl | ImGuiKey_C, 0, CE },
            { PasteRecords, "PasteRecords", Scope::AddressList, "Paste", Ctrl | ImGuiKey_V, 0, CE },
            { SelectAllRecords, "SelectAllRecords", Scope::AddressList, "Select All Records", Ctrl | ImGuiKey_A, 0, CE },
            { DeleteRecords, "DeleteRecords", Scope::AddressList, "Delete Selected Records", ImGuiKey_Delete, 0, CE },

            { AddResults, "AddResults", Scope::ScanResults, "Add to Address List", ImGuiKey_Enter, 0, Own },
            { ChangeResultValue, "ChangeResultValue", Scope::ScanResults, "Change Value", Ctrl | ImGuiKey_E, 0, CE },
            { BrowseResult, "BrowseResult", Scope::ScanResults, "Browse This Memory Region", Ctrl | ImGuiKey_B, 0, CE },
            { DisassembleResult, "DisassembleResult", Scope::ScanResults, "Disassemble This Memory Region", Ctrl | ImGuiKey_D, 0, CE },
            { DissectResult, "DissectResult", Scope::ScanResults, "Dissect Data at This Address", Ctrl | Alt | ImGuiKey_D, 0, Own },
            { ResultAccesses, "ResultAccesses", Scope::ScanResults, "Find Out What Accesses This Address", Ctrl | ImGuiKey_F5, 0, CE },
            { ResultWrites, "ResultWrites", Scope::ScanResults, "Find Out What Writes to This Address", Ctrl | ImGuiKey_F6, 0, CE },
            { CopyResults, "CopyResults", Scope::ScanResults, "Copy Addresses", Ctrl | ImGuiKey_C, 0, CE },
            { SelectAllResults, "SelectAllResults", Scope::ScanResults, "Select All Results", Ctrl | ImGuiKey_A, 0, CE },

            { NewViewerWindow, "NewViewerWindow", Scope::MemoryViewer, "New Memory Viewer Window", Ctrl | ImGuiKey_N, 0, Own },
            { SaveMemoryRegion, "SaveMemoryRegion", Scope::MemoryViewer, "Save Memory Region", Ctrl | ImGuiKey_S, 0, CE },
            { LoadMemoryRegion, "LoadMemoryRegion", Scope::MemoryViewer, "Load File into Memory", Ctrl | ImGuiKey_O, 0, CE },
            { GotoAddress, "GotoAddress", Scope::MemoryViewer, "Go to Address", Ctrl | ImGuiKey_G, 0, CE },
            { FindMemory, "FindMemory", Scope::MemoryViewer, "Find Bytes", Ctrl | ImGuiKey_F, 0, CE },
            { FindAssembly, "FindAssembly", Scope::MemoryViewer, "Find Assembly Code", Ctrl | Shift | ImGuiKey_F, 0, Own },
            { UndoMemoryEdit, "UndoMemoryEdit", Scope::MemoryViewer, "Undo Last Memory Edit", Ctrl | ImGuiKey_Z, 0, CE },
            { MemoryRegions, "MemoryRegions", Scope::MemoryViewer, "Memory Regions", Ctrl | ImGuiKey_R, 0, CE },
            { ReferencedFunctions, "ReferencedFunctions", Scope::MemoryViewer, "Referenced Functions", Ctrl | Alt | ImGuiKey_F, 0, CE },
            { ReferencedStrings, "ReferencedStrings", Scope::MemoryViewer, "Referenced Strings", Ctrl | Alt | ImGuiKey_R, 0, CE },
            { SymbolsAndModules, "SymbolsAndModules", Scope::MemoryViewer, "Modules and Symbols", Ctrl | Alt | ImGuiKey_S, 0, CE },
            { DissectCode, "DissectCode", Scope::MemoryViewer, "Dissect Code", Ctrl | ImGuiKey_J, 0, CE },
            { DissectData, "DissectData", Scope::MemoryViewer, "Dissect Data", Ctrl | ImGuiKey_D, 0, CE },
            { ViewerAutoAssemble, "ViewerAutoAssemble", Scope::MemoryViewer, "Auto Assembler", Ctrl | ImGuiKey_A, 0, CE },
            { ViewerLuaEngine, "ViewerLuaEngine", Scope::MemoryViewer, "Lua Engine", Ctrl | ImGuiKey_L, 0, CE },
            { InjectLibrary, "InjectLibrary", Scope::MemoryViewer, "Inject Library", Ctrl | ImGuiKey_I, 0, CE },
            { AllocateMemory, "AllocateMemory", Scope::MemoryViewer, "Allocate Memory", Ctrl | Alt | ImGuiKey_M, 0, CE },
            { FillMemory, "FillMemory", Scope::MemoryViewer, "Fill Memory", Ctrl | Alt | Shift | ImGuiKey_F, 0, CE },
            { CreateThread, "CreateThread", Scope::MemoryViewer, "Create Thread", Ctrl | Alt | ImGuiKey_T, 0, CE },
            { ScanForPatches, "ScanForPatches", Scope::MemoryViewer, "Scan for Patches", Ctrl | Shift | ImGuiKey_P, 0, Own },
            { DebugRun, "DebugRun", Scope::MemoryViewer, "Run", ImGuiKey_F9, 0, CE },
            { DebugStepInto, "DebugStepInto", Scope::MemoryViewer, "Step Into", ImGuiKey_F7, 0, CE },
            { DebugStepOver, "DebugStepOver", Scope::MemoryViewer, "Step Over", ImGuiKey_F8, 0, CE },
            { DebugRunToReturn, "DebugRunToReturn", Scope::MemoryViewer, "Step Out", Shift | ImGuiKey_F8, 0, CE },
            { DebugBreak, "DebugBreak", Scope::MemoryViewer, "Break into Process", ImGuiKey_F12, 0, Own },
            { ToggleBreakpoint, "ToggleBreakpoint", Scope::MemoryViewer, "Toggle Breakpoint", ImGuiKey_F5, 0, CE },
            { SetInstructionPointer, "SetInstructionPointer", Scope::MemoryViewer, "Set Instruction Pointer Here", Ctrl | ImGuiKey_F4, 0, CE },
            { BreakpointList, "BreakpointList", Scope::MemoryViewer, "Breakpoint List", Ctrl | Alt | ImGuiKey_B, 0, Own },
            { EditBreakpoint, "EditBreakpoint", Scope::MemoryViewer, "Conditional Breakpoint", Shift | ImGuiKey_F5, 0, Own },
            { BreakAndTrace, "BreakAndTrace", Scope::MemoryViewer, "Break and Trace", Ctrl | ImGuiKey_T, 0, Own },
            { ThreadList, "ThreadList", Scope::MemoryViewer, "Thread List", Ctrl | Shift | ImGuiKey_T, 0, Own },
            { CommentList, "CommentList", Scope::MemoryViewer, "Comment List", Ctrl | Alt | Shift | ImGuiKey_C, 0, Own },

            { FollowInstruction, "FollowInstruction", Scope::Disassembler, "Follow Jump or Call", ImGuiKey_Space, 0, CE },
            { NavigateBack, "NavigateBack", Scope::Disassembler, "Back", ImGuiKey_Backspace, 0, CE },
            { AssembleInstruction, "AssembleInstruction", Scope::Disassembler, "Assemble", ImGuiKey_Enter, 0, Own },
            { NopInstruction, "NopInstruction", Scope::Disassembler, "Replace with NOPs", Ctrl | Shift | ImGuiKey_N, 0, Own },
            { CopyInstructions, "CopyInstructions", Scope::Disassembler, "Copy Instructions", Ctrl | ImGuiKey_C, 0, Own },
            { CopyInstructionBytes, "CopyInstructionBytes", Scope::Disassembler, "Copy Bytes Only", Ctrl | Alt | ImGuiKey_C, 0, CE },
            { InstructionAccesses, "InstructionAccesses", Scope::Disassembler, "Find Out What Addresses This Instruction Accesses", Ctrl | Alt | ImGuiKey_I, 0, Own },
            { CodeReferences, "CodeReferences", Scope::Disassembler, "Find References", ImGuiKey_X, 0, Own },
            { FlowGraph, "FlowGraph", Scope::Disassembler, "Flow Graph", ImGuiKey_G, 0, Own },
            { GotoFunctionStart, "GotoFunctionStart", Scope::Disassembler, "Go to Function Start", ImGuiKey_Home, 0, Own },
            { GotoFunctionEnd, "GotoFunctionEnd", Scope::Disassembler, "Go to Function End", ImGuiKey_End, 0, Own },
            { SelectFunction, "SelectFunction", Scope::Disassembler, "Select Function", Ctrl | Shift | ImGuiKey_A, 0, Own },
            { AddInstructionToList, "AddInstructionToList", Scope::Disassembler, "Add to Address List", Ctrl | ImGuiKey_KeypadAdd, 0, CE },
            { SetBookmark, "SetBookmark", Scope::Disassembler, "Toggle Bookmark", Ctrl | Alt | ImGuiKey_0, 0, CE, true },
            { GotoBookmark, "GotoBookmark", Scope::Disassembler, "Go to Bookmark", Ctrl | ImGuiKey_0, 0, CE, true },
            { CommentInstruction, "CommentInstruction", Scope::Disassembler, "Edit Comment", Ctrl | Shift | ImGuiKey_C, 0, Own },

            { HexFollow, "HexFollow", Scope::HexView, "Follow Pointer", ImGuiKey_Space, 0, CE },
            { HexBack, "HexBack", Scope::HexView, "Back", ImGuiKey_Backspace, 0, CE },
            { HexEditValue, "HexEditValue", Scope::HexView, "Edit Value", ImGuiKey_Enter, 0, Own },
            { HexCopy, "HexCopy", Scope::HexView, "Copy 16 Bytes", Ctrl | ImGuiKey_C, 0, CE },
            { HexPaste, "HexPaste", Scope::HexView, "Paste Bytes", Ctrl | ImGuiKey_P, Ctrl | ImGuiKey_V, CE },
            { HexAccesses, "HexAccesses", Scope::HexView, "Find Out What Accesses This Address", ImGuiKey_F5, 0, Own },
            { HexWrites, "HexWrites", Scope::HexView, "Find Out What Writes to This Address", ImGuiKey_F6, 0, Own },
            { HexBreakOnWrite, "HexBreakOnWrite", Scope::HexView, "Break on Write", Ctrl | Shift | ImGuiKey_F6, 0, Own },
            { HexBreakOnAccess, "HexBreakOnAccess", Scope::HexView, "Break on Access", Ctrl | Shift | ImGuiKey_F5, 0, Own },
            { HexAddToList, "HexAddToList", Scope::HexView, "Add to Address List", Ctrl | ImGuiKey_KeypadAdd, 0, CE },
            { HexBytes, "HexBytes", Scope::HexView, "Show Bytes", Ctrl | ImGuiKey_1, 0, CE },
            { HexTwoBytes, "HexTwoBytes", Scope::HexView, "Show 2 Bytes (Hex)", Ctrl | ImGuiKey_3, 0, CE },
            { HexFourBytes, "HexFourBytes", Scope::HexView, "Show 4 Bytes (Hex)", Ctrl | ImGuiKey_5, 0, CE },
            { HexFourBytesDecimal, "HexFourBytesDecimal", Scope::HexView, "Show 4 Bytes", Ctrl | ImGuiKey_6, 0, CE },
            { HexEightBytes, "HexEightBytes", Scope::HexView, "Show 8 Bytes (Hex)", Ctrl | ImGuiKey_7, 0, CE },
            { HexFloat, "HexFloat", Scope::HexView, "Show Floats", Ctrl | ImGuiKey_9, 0, CE },
            { HexDouble, "HexDouble", Scope::HexView, "Show Doubles", Ctrl | ImGuiKey_0, 0, CE },

            { DefineStructure, "DefineStructure", Scope::Structure, "Define New Structure", Ctrl | ImGuiKey_N, 0, CE },
            { AddStructureColumn, "AddStructureColumn", Scope::Structure, "Add Address to Compare", Ctrl | ImGuiKey_A, 0, CE },
            { CollapseStructure, "CollapseStructure", Scope::Structure, "Collapse All Pointers", Ctrl | Alt | ImGuiKey_C, 0, CE },
            { StructureGotoAddress, "StructureGotoAddress", Scope::Structure, "Go to Address", Ctrl | ImGuiKey_G, 0, Own },
            { ChangeFieldValue, "ChangeFieldValue", Scope::Structure, "Change Value", ImGuiKey_Enter, 0, CE },
            { RenameField, "RenameField", Scope::Structure, "Rename Field", Ctrl | ImGuiKey_Enter, ImGuiKey_F2, CE },
            { InsertField, "InsertField", Scope::Structure, "Insert 4 Bytes", ImGuiKey_Insert, 0, CE },
            { DeleteField, "DeleteField", Scope::Structure, "Delete Field", ImGuiKey_Delete, 0, CE },
            { FieldToAddressList, "FieldToAddressList", Scope::Structure, "Add Field to Address List", ImGuiKey_A, 0, CE },
            { BrowseField, "BrowseField", Scope::Structure, "Browse Field Memory", ImGuiKey_Space, 0, CE },
            { BrowseFieldPointer, "BrowseFieldPointer", Scope::Structure, "Browse Pointer Target", ImGuiKey_F, 0, CE },
            { FieldAccesses, "FieldAccesses", Scope::Structure, "Find Out What Accesses This Field", ImGuiKey_F5, 0, Own },
            { FieldWrites, "FieldWrites", Scope::Structure, "Find Out What Writes to This Field", ImGuiKey_F6, 0, Own },

            { ScriptNewWindow, "ScriptNewWindow", Scope::ScriptEditor, "New Auto Assembler Window", Ctrl | ImGuiKey_N, 0, CE },
            { ScriptOpen, "ScriptOpen", Scope::ScriptEditor, "Open Script", Ctrl | ImGuiKey_O, 0, CE },
            { ScriptSave, "ScriptSave", Scope::ScriptEditor, "Save", Ctrl | ImGuiKey_S, 0, CE },
            { ScriptSaveAs, "ScriptSaveAs", Scope::ScriptEditor, "Save to File", Ctrl | Alt | ImGuiKey_S, 0, Own },
            { ScriptCheck, "ScriptCheck", Scope::ScriptEditor, "Check Syntax", Ctrl | Alt | ImGuiKey_C, 0, CE },
            { ScriptExecute, "ScriptExecute", Scope::ScriptEditor, "Execute [ENABLE]", Ctrl | ImGuiKey_Enter, 0, Own },
            { ScriptExecuteDisable, "ScriptExecuteDisable", Scope::ScriptEditor, "Execute [DISABLE]", Ctrl | Shift | ImGuiKey_Enter, 0, Own },
            { TemplateCodeInjection, "TemplateCodeInjection", Scope::ScriptEditor, "Template: Code Injection", Ctrl | ImGuiKey_I, 0, CE },
            { TemplateAobInjection, "TemplateAobInjection", Scope::ScriptEditor, "Template: AOB Injection", Ctrl | Shift | ImGuiKey_A, 0, CE },
            { TemplateFullInjection, "TemplateFullInjection", Scope::ScriptEditor, "Template: Full Injection", Ctrl | Shift | ImGuiKey_F, 0, CE },
            { TemplateCallLua, "TemplateCallLua", Scope::ScriptEditor, "Template: Call Lua Function", Ctrl | ImGuiKey_L, 0, CE },
            { TemplateFramework, "TemplateFramework", Scope::ScriptEditor, "Template: Cheat Table Framework Code", Ctrl | Alt | ImGuiKey_T, 0, CE },

            { LuaOpen, "LuaOpen", Scope::LuaEngine, "Open Script", Ctrl | ImGuiKey_O, 0, CE },
            { LuaSave, "LuaSave", Scope::LuaEngine, "Save Script", Ctrl | ImGuiKey_S, 0, CE },
            { LuaSaveAs, "LuaSaveAs", Scope::LuaEngine, "Save Script As", Ctrl | Alt | ImGuiKey_S, 0, CE },
            { LuaExecute, "LuaExecute", Scope::LuaEngine, "Execute", Ctrl | ImGuiKey_Enter, 0, Own },
            { LuaClearOutput, "LuaClearOutput", Scope::LuaEngine, "Clear Output", Ctrl | ImGuiKey_L, 0, Own },
            { LuaFunctions, "LuaFunctions", Scope::LuaEngine, "Documentation", Ctrl | Shift | ImGuiKey_F, 0, Own },
            { LuaStop, "LuaStop", Scope::LuaEngine, "Stop", Shift | ImGuiKey_F5, 0, Own },
            { LuaConsoleTab, "LuaConsoleTab", Scope::LuaEngine, "Console Tab", Ctrl | ImGuiKey_1, 0, Own },
            { LuaScriptTab, "LuaScriptTab", Scope::LuaEngine, "Script Tab", Ctrl | ImGuiKey_2, 0, Own },
            { LuaFind, "LuaFind", Scope::LuaEngine, "Search Console", Ctrl | ImGuiKey_F, 0, Own },

            { EditorComplete, "EditorComplete", Scope::CodeEditor, "Show Completions", Ctrl | ImGuiKey_Space, 0, Own },
            { EditorToggleComment, "EditorToggleComment", Scope::CodeEditor, "Toggle Comment", Ctrl | ImGuiKey_Slash, Ctrl | Shift | ImGuiKey_7, Own },
            { EditorDuplicateLines, "EditorDuplicateLines", Scope::CodeEditor, "Duplicate Lines", Ctrl | ImGuiKey_D, 0, Own },
            { EditorMoveLinesUp, "EditorMoveLinesUp", Scope::CodeEditor, "Move Lines Up", Alt | ImGuiKey_UpArrow, 0, Own },
            { EditorMoveLinesDown, "EditorMoveLinesDown", Scope::CodeEditor, "Move Lines Down", Alt | ImGuiKey_DownArrow, 0, Own },
            { EditorDeleteLines, "EditorDeleteLines", Scope::CodeEditor, "Delete Lines", Ctrl | Shift | ImGuiKey_K, 0, Own },
            { EditorGotoLine, "EditorGotoLine", Scope::CodeEditor, "Go to Line", Ctrl | ImGuiKey_G, 0, Own },
            { EditorFind, "EditorFind", Scope::CodeEditor, "Find", Ctrl | ImGuiKey_F, 0, CE },
            { EditorReplace, "EditorReplace", Scope::CodeEditor, "Replace", Ctrl | ImGuiKey_H, 0, Own },
            { EditorFindNext, "EditorFindNext", Scope::CodeEditor, "Find Next", ImGuiKey_F3, 0, CE },
            { EditorFindPrevious, "EditorFindPrevious", Scope::CodeEditor, "Find Previous", Shift | ImGuiKey_F3, 0, Own },

            { FlowGraphFit, "FlowGraphFit", Scope::FlowGraph, "Fit to Window", ImGuiKey_F, 0, Own },
            { FlowGraphActualSize, "FlowGraphActualSize", Scope::FlowGraph, "Actual Size", ImGuiKey_0, 0, Own },
            { FlowGraphBack, "FlowGraphBack", Scope::FlowGraph, "Back to Previous Function", ImGuiKey_Backspace, 0, Own },

            { LogCopy, "LogCopy", Scope::Log, "Copy Entries", Ctrl | ImGuiKey_C, 0, Own },
            { LogSelectAll, "LogSelectAll", Scope::Log, "Select All", Ctrl | ImGuiKey_A, 0, Own },
            { LogFind, "LogFind", Scope::Log, "Search Log", Ctrl | ImGuiKey_F, 0, Own },

            { TableFilesAdd, "TableFilesAdd", Scope::TableFiles, "Add Files", Ctrl | ImGuiKey_O, 0, Own },
            { TableFilesExport, "TableFilesExport", Scope::TableFiles, "Export Files", Ctrl | ImGuiKey_E, 0, Own },
            { TableFilesApply, "TableFilesApply", Scope::TableFiles, "Apply Changes", Ctrl | ImGuiKey_S, 0, Own },
            { TableFilesRename, "TableFilesRename", Scope::TableFiles, "Rename File", ImGuiKey_F2, 0, Own },
            { TableFilesDelete, "TableFilesDelete", Scope::TableFiles, "Delete Files", ImGuiKey_Delete, 0, Own },
            { TableFilesSelectAll, "TableFilesSelectAll", Scope::TableFiles, "Select All", Ctrl | ImGuiKey_A, 0, Own },
            { TableFilesCopyName, "TableFilesCopyName", Scope::TableFiles, "Copy Names", Ctrl | Shift | ImGuiKey_C, 0, Own },
            { TableFilesFind, "TableFilesFind", Scope::TableFiles, "Find", Ctrl | ImGuiKey_F, 0, Own },

            { FormNew, "FormNew", Scope::FormEditor, "New Form", Ctrl | ImGuiKey_N, 0, Own },
            { FormOpen, "FormOpen", Scope::FormEditor, "Open Form", Ctrl | ImGuiKey_O, 0, Own },
            { FormSave, "FormSave", Scope::FormEditor, "Save Form", Ctrl | ImGuiKey_S, 0, Own },
            { FormSaveAs, "FormSaveAs", Scope::FormEditor, "Save Form As", Ctrl | Shift | ImGuiKey_S, 0, Own },
            { FormExport, "FormExport", Scope::FormEditor, "Export Script", Ctrl | ImGuiKey_E, 0, Own },
            { FormUndo, "FormUndo", Scope::FormEditor, "Undo", Ctrl | ImGuiKey_Z, 0, Own },
            { FormRedo, "FormRedo", Scope::FormEditor, "Redo", Ctrl | ImGuiKey_Y, Ctrl | Shift | ImGuiKey_Z, Own },
            { FormDuplicate, "FormDuplicate", Scope::FormEditor, "Duplicate Widget", Ctrl | ImGuiKey_D, 0, Own },
            { FormDelete, "FormDelete", Scope::FormEditor, "Delete Widget", ImGuiKey_Delete, 0, Own },
            { FormMoveUp, "FormMoveUp", Scope::FormEditor, "Move Widget Up", Alt | ImGuiKey_UpArrow, 0, Own },
            { FormMoveDown, "FormMoveDown", Scope::FormEditor, "Move Widget Down", Alt | ImGuiKey_DownArrow, 0, Own },
            { FormTry, "FormTry", Scope::FormEditor, "Design or Try", Ctrl | ImGuiKey_T, 0, Own },
            { FormRun, "FormRun", Scope::FormEditor, "Run Script", ImGuiKey_F5, 0, Own },
            { FormCopy, "FormCopy", Scope::FormEditor, "Copy Widget", Ctrl | ImGuiKey_C, 0, Own },
            { FormCut, "FormCut", Scope::FormEditor, "Cut Widget", Ctrl | ImGuiKey_X, 0, Own },
            { FormPaste, "FormPaste", Scope::FormEditor, "Paste Widgets", Ctrl | ImGuiKey_V, 0, Own },
            { FormRename, "FormRename", Scope::FormEditor, "Rename Widget", ImGuiKey_F2, 0, Own },
            { FormSelectParent, "FormSelectParent", Scope::FormEditor, "Select Parent", Alt | ImGuiKey_LeftArrow, 0, Own },
            { FormFind, "FormFind", Scope::FormEditor, "Find a Widget", Ctrl | ImGuiKey_F, 0, Own },
            { FormSelectAll, "FormSelectAll", Scope::FormEditor, "Select All in the Container", Ctrl | ImGuiKey_A, 0, Own },
        };

        static_assert(std::size(kCommands) == static_cast<std::size_t>(Command::Count));

        constexpr bool InOrder()
        {
            for (std::size_t i = 0; i < std::size(kCommands); ++i)
            {
                if (static_cast<std::size_t>(kCommands[i].Id) != i)
                    return false;
            }
            return true;
        }

        static_assert(InOrder());

        const char* KeyText(ImGuiKey key)
        {
            switch (key)
            {
            case ImGuiKey_Delete: return "Del";
            case ImGuiKey_Insert: return "Ins";
            case ImGuiKey_Escape: return "Esc";
            case ImGuiKey_PageUp: return "PgUp";
            case ImGuiKey_PageDown: return "PgDn";
            case ImGuiKey_KeypadAdd: return "Num +";
            case ImGuiKey_KeypadSubtract: return "Num -";
            case ImGuiKey_Comma: return ",";
            case ImGuiKey_UpArrow: return "Up";
            case ImGuiKey_DownArrow: return "Down";
            case ImGuiKey_LeftArrow: return "Left";
            case ImGuiKey_RightArrow: return "Right";
            default: return ImGui::GetKeyName(key);
            }
        }

        constexpr std::size_t kCommandCount = static_cast<std::size_t>(Command::Count);

        struct Runtime
        {
            std::array<Binding, kCommandCount> Bindings{};
            std::array<std::string, kCommandCount> Texts;

            Runtime()
            {
                for (const CommandInfo& info : kCommands)
                    Bindings[static_cast<std::size_t>(info.Id)] = { info.Chord, info.Alternate };
                Refresh();
            }

            void Refresh()
            {
                for (const CommandInfo& info : kCommands)
                {
                    const std::size_t index = static_cast<std::size_t>(info.Id);
                    Texts[index] = ChordText(Bindings[index].Chord, info.Digits);
                }
            }
        };

        Runtime& State()
        {
            static Runtime runtime;
            return runtime;
        }

        std::vector<ImGuiKeyChord> Expanded(const CommandInfo& info, Binding binding)
        {
            std::vector<ImGuiKeyChord> chords;
            for (ImGuiKeyChord chord : { binding.Chord, binding.Alternate })
            {
                if (!chord)
                    continue;
                if (!info.Digits)
                {
                    chords.push_back(chord);
                    continue;
                }
                for (int i = 0; i < 10; ++i)
                    chords.push_back((chord & ImGuiMod_Mask_) | (ImGuiKey_0 + i));
            }
            return chords;
        }

        std::string MarkdownCell(std::string_view text)
        {
            std::string cell;
            for (char c : text)
            {
                if (c == '|')
                    cell += "\\|";
                else if (c == '+' && cell.empty())
                    cell += "\\+";
                else
                    cell += c;
            }
            return cell;
        }
    }

    const CommandInfo& Describe(Command command)
    {
        return kCommands[static_cast<std::size_t>(command)];
    }

    std::span<const CommandInfo> AllCommands()
    {
        return kCommands;
    }

    std::optional<Command> FindCommand(std::string_view key)
    {
        for (const CommandInfo& info : kCommands)
        {
            if (EqualsNoCase(info.Key, TrimView(key)))
                return info.Id;
        }
        return std::nullopt;
    }

    bool FromTemplateSettings(Command command)
    {
        switch (command)
        {
        case Command::TemplateCodeInjection:
        case Command::TemplateAobInjection:
        case Command::TemplateFullInjection:
        case Command::TemplateCallLua:
        case Command::TemplateFramework:
            return true;
        default:
            return false;
        }
    }

    Binding DefaultBinding(Command command)
    {
        const CommandInfo& info = Describe(command);
        return { info.Chord, info.Alternate };
    }

    Binding CurrentBinding(Command command)
    {
        return State().Bindings[static_cast<std::size_t>(command)];
    }

    void SetBinding(Command command, Binding binding)
    {
        if (!binding.Chord && binding.Alternate)
            std::swap(binding.Chord, binding.Alternate);
        if (binding.Alternate == binding.Chord)
            binding.Alternate = 0;
        Runtime& state = State();
        state.Bindings[static_cast<std::size_t>(command)] = binding;
        state.Refresh();
    }

    bool IsCustomized(Command command)
    {
        return !(CurrentBinding(command) == DefaultBinding(command));
    }

    std::string BindingText(Command command, Binding binding)
    {
        const bool digits = Describe(command).Digits;
        if (!binding.Chord)
            return "none";
        std::string text = ChordText(binding.Chord, digits);
        if (binding.Alternate)
            text += ";" + ChordText(binding.Alternate, digits);
        return text;
    }

    std::optional<Binding> ParseBinding(Command command, std::string_view text)
    {
        const bool digits = Describe(command).Digits;
        const std::string trimmed = Trim(text);
        if (trimmed.empty() || EqualsNoCase(trimmed, "none"))
            return Binding{};
        const std::vector<std::string> parts = Split(trimmed, ';');
        if (parts.empty() || parts.size() > 2)
            return std::nullopt;
        ImGuiKeyChord chords[2] = { 0, 0 };
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            std::string part = Trim(parts[i]);
            if (digits)
            {
                const std::size_t range = part.rfind("0-9");
                if (range == std::string::npos || range + 3 != part.size())
                    return std::nullopt;
                part.replace(range, 3, "0");
            }
            chords[i] = ParseChord(part);
            if (!chords[i] || (digits && (chords[i] & ~ImGuiMod_Mask_) != ImGuiKey_0))
                return std::nullopt;
        }
        return Binding{ chords[0], chords[1] };
    }

    void ApplyShortcutSettings(const std::map<std::string, std::string>& saved)
    {
        Runtime& state = State();
        for (const CommandInfo& info : kCommands)
            state.Bindings[static_cast<std::size_t>(info.Id)] = { info.Chord, info.Alternate };
        for (const auto& [key, text] : saved)
        {
            const auto command = FindCommand(key);
            if (!command || FromTemplateSettings(*command))
                continue;
            if (const auto binding = ParseBinding(*command, text))
                state.Bindings[static_cast<std::size_t>(*command)] = *binding;
        }
        state.Refresh();
    }

    std::map<std::string, std::string> ShortcutSettings()
    {
        std::map<std::string, std::string> saved;
        for (const CommandInfo& info : kCommands)
        {
            if (IsCustomized(info.Id))
                saved[info.Key] = BindingText(info.Id, CurrentBinding(info.Id));
        }
        return saved;
    }

    std::vector<Command> ConflictsWith(Command command, ImGuiKeyChord chord)
    {
        std::vector<Command> found;
        if (!chord)
            return found;
        const CommandInfo& self = Describe(command);
        const std::vector<ImGuiKeyChord> wanted = Expanded(self, { chord, 0 });
        for (const CommandInfo& info : kCommands)
        {
            if (info.Id == command || info.Scope != self.Scope || FromTemplateSettings(info.Id))
                continue;
            for (ImGuiKeyChord other : Expanded(info, CurrentBinding(info.Id)))
            {
                if (std::find(wanted.begin(), wanted.end(), other) != wanted.end())
                {
                    found.push_back(info.Id);
                    break;
                }
            }
        }
        return found;
    }

    const char* ScopeName(CommandScope scope)
    {
        switch (scope)
        {
        case CommandScope::Global: return "Everywhere";
        case CommandScope::Main: return "Main Window";
        case CommandScope::AddressList: return "Address List";
        case CommandScope::ScanResults: return "Scan Results";
        case CommandScope::MemoryViewer: return "Memory Viewer";
        case CommandScope::Disassembler: return "Memory Viewer: Disassembler";
        case CommandScope::HexView: return "Memory Viewer: Hex View";
        case CommandScope::Structure: return "Structure Dissect";
        case CommandScope::ScriptEditor: return "Auto Assembler and Script Editor";
        case CommandScope::LuaEngine: return "Lua Engine";
        case CommandScope::CodeEditor: return "Code Editors";
        case CommandScope::FlowGraph: return "Flow Graph";
        case CommandScope::Log: return "Log";
        case CommandScope::TableFiles: return "Table Files";
        case CommandScope::FormEditor: return "Form Editor";
        case CommandScope::Count: break;
        }
        return "";
    }

    const char* ScopeHint(CommandScope scope)
    {
        switch (scope)
        {
        case CommandScope::Global: return "Works in every window, unless the focused window uses the same keys for something else.";
        case CommandScope::Main: return "While the main window has the focus.";
        case CommandScope::AddressList: return "While the address list has the focus. The keys act on the selected records.";
        case CommandScope::ScanResults: return "While the scan result list has the focus. The keys act on the selected results.";
        case CommandScope::MemoryViewer: return "While a memory viewer window has the focus.";
        case CommandScope::Disassembler: return "While the disassembler pane has the focus. The keys act on the selected instruction.";
        case CommandScope::HexView: return "While the hex view pane has the focus. The keys act on the selected byte.";
        case CommandScope::Structure: return "While a Structure Dissect window has the focus. Field keys act on the selected field. The arrow keys, Page Up, Page Down, Home and End move through the fields (Shift extends the selection); Right opens a pointer or an embedded structure and then steps into it, Left closes it and then steps out.";
        case CommandScope::ScriptEditor: return "While an Auto Assembler or script window has the focus. Template keys come from each template's Shortcut setting; the ones listed are the built-in templates.";
        case CommandScope::LuaEngine: return "While the Lua Engine window has the focus.";
        case CommandScope::CodeEditor: return "While the cursor is in a code editor: the Lua engine, Auto Assembler and script windows, the Templates window and Table Files. Find Next and Find Previous also work while typing.";
        case CommandScope::FlowGraph: return "While the flow graph window has the focus.";
        case CommandScope::Log: return "While the log window has the focus.";
        case CommandScope::TableFiles: return "While the Table Files window has the focus. List keys act on the selected files and rest while the cursor is in a text.";
        case CommandScope::FormEditor: return "While the Form Editor has the focus. Widget keys act on the selected widget and rest while the cursor is in a text.";
        case CommandScope::Count: break;
        }
        return "";
    }

    std::string ChordText(ImGuiKeyChord chord, bool digits)
    {
        if (!chord)
            return {};
        std::string text;
        if (chord & ImGuiMod_Ctrl)
            text += "Ctrl+";
        if (chord & ImGuiMod_Alt)
            text += "Alt+";
        if (chord & ImGuiMod_Shift)
            text += "Shift+";
        const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
        text += digits ? "0-9" : KeyText(key);
        return text;
    }

    ImGuiKeyChord ParseChord(std::string_view text)
    {
        std::string rest = Lower(Trim(text));
        ImGuiKeyChord mods = 0;
        for (bool found = true; found;)
        {
            found = false;
            for (const auto& [name, mod] : { std::pair<const char*, ImGuiKeyChord>{ "ctrl+", ImGuiMod_Ctrl }, { "control+", ImGuiMod_Ctrl }, { "alt+", ImGuiMod_Alt }, { "shift+", ImGuiMod_Shift } })
            {
                if (rest.rfind(name, 0) == 0 && rest.size() > std::strlen(name))
                {
                    mods |= mod;
                    rest.erase(0, std::strlen(name));
                    found = true;
                }
            }
        }
        rest = Trim(rest);
        if (rest.empty())
            return 0;
        static const std::pair<const char*, ImGuiKey> kAliases[] = { { "del", ImGuiKey_Delete }, { "ins", ImGuiKey_Insert }, { "esc", ImGuiKey_Escape }, { "return", ImGuiKey_Enter },
            { "pgup", ImGuiKey_PageUp }, { "pgdn", ImGuiKey_PageDown }, { "num +", ImGuiKey_KeypadAdd }, { "num+", ImGuiKey_KeypadAdd }, { "num -", ImGuiKey_KeypadSubtract },
            { "num-", ImGuiKey_KeypadSubtract }, { ",", ImGuiKey_Comma }, { "up", ImGuiKey_UpArrow }, { "down", ImGuiKey_DownArrow }, { "left", ImGuiKey_LeftArrow }, { "right", ImGuiKey_RightArrow } };
        for (const auto& [name, key] : kAliases)
        {
            if (rest == name)
                return mods | key;
        }
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
        {
            if (key >= ImGuiKey_ReservedForModCtrl)
                break;
            if (EqualsNoCase(ImGui::GetKeyName(static_cast<ImGuiKey>(key)), rest))
                return mods | key;
        }
        return 0;
    }

    const char* ShortcutText(Command command)
    {
        const std::string& text = State().Texts[static_cast<std::size_t>(command)];
        return text.empty() ? nullptr : text.c_str();
    }

    std::string ShortcutLabel(Command command)
    {
        const char* text = ShortcutText(command);
        return text ? std::string(text) : std::string();
    }

    std::string ShortcutsMarkdown()
    {
        std::string text = "# Keyboard shortcuts\n\n";
        text += "Generated by `Manifold-Engine.Tests.exe --shortcuts-markdown` from `Source/App/Commands.cpp`; the core tests compare this file with the table.\n\n";
        text += "Every shortcut belongs to a window or pane. Keys of the focused window win over the ones that work everywhere, ";
        text += "so `Ctrl+S` saves the script in an Auto Assembler window and the cheat table elsewhere. ";
        text += "Shortcuts marked **CE** are the ones Cheat Engine 7.5 uses for the same feature; the others are Manifold Engine's own. ";
        text += "Keys without Ctrl or Alt do nothing while a text field has the focus. ";
        text += "In the app, **Help > Keyboard Shortcuts** (`F1`) shows the same list with a search.\n\n";
        text += "This file lists the defaults. **Settings > Keyboard** changes any of them: click the keys and press new ones, right-click to remove them. ";
        text += "Changed keys are stored in `configs\\Manifold-Engine.ini` under `[Shortcuts]` as `Key.<Command>=Ctrl+Shift+O` (a second shortcut after `;`, `none` for no keys), ";
        text += "using the command names below.\n";
        for (int scope = 0; scope < static_cast<int>(CommandScope::Count); ++scope)
        {
            text += "\n## " + std::string(ScopeName(static_cast<CommandScope>(scope))) + "\n\n";
            text += std::string(ScopeHint(static_cast<CommandScope>(scope))) + "\n\n";
            text += "| Keys | Command | Name in the settings | Origin |\n|---|---|---|---|\n";
            for (const CommandInfo& info : kCommands)
            {
                if (static_cast<int>(info.Scope) != scope)
                    continue;
                std::string keys = "`" + ChordText(info.Chord, info.Digits) + "`";
                if (info.Alternate)
                    keys += " or `" + ChordText(info.Alternate) + "`";
                const std::string name = FromTemplateSettings(info.Id) ? "(template setting)" : "`" + std::string(info.Key) + "`";
                text += "| " + keys + " | " + MarkdownCell(info.Name) + " | " + name + " | " + (info.FromCheatEngine ? "CE" : "Manifold") + " |\n";
            }
        }
        return text;
    }
}
