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
            { OpenProcess, "OpenProcess", Scope::Global, "Open a process", Ctrl | ImGuiKey_P, 0, CE },
            { OpenTable, "OpenTable", Scope::Global, "Open a cheat table", Ctrl | ImGuiKey_O, 0, CE },
            { SaveTable, "SaveTable", Scope::Global, "Save the cheat table", Ctrl | ImGuiKey_S, 0, CE },
            { SaveTableAs, "SaveTableAs", Scope::Global, "Save the cheat table under a new name", Ctrl | Alt | ImGuiKey_S, Ctrl | Shift | ImGuiKey_S, CE },
            { OpenMemoryViewer, "OpenMemoryViewer", Scope::Global, "Memory viewer", Ctrl | ImGuiKey_M, 0, CE },
            { OpenStructureDissect, "OpenStructureDissect", Scope::Global, "Structure dissect", Ctrl | Alt | ImGuiKey_D, 0, CE },
            { OpenAutoAssembler, "OpenAutoAssembler", Scope::Global, "Auto Assembler", Ctrl | Alt | ImGuiKey_A, 0, CE },
            { OpenLuaEngine, "OpenLuaEngine", Scope::Global, "Lua engine", Ctrl | Alt | Shift | ImGuiKey_L, Ctrl | ImGuiKey_L, CE },
            { OpenSettings, "OpenSettings", Scope::Global, "Settings", Ctrl | ImGuiKey_Comma, 0, Own },
            { OpenShortcuts, "OpenShortcuts", Scope::Global, "Keyboard shortcuts", ImGuiKey_F1, 0, Own },
            { OpenLog, "OpenLog", Scope::Global, "Log", Ctrl | Shift | ImGuiKey_L, 0, Own },
            { OpenMono, "OpenMono", Scope::Global, "Mono / .NET dissector", Ctrl | Alt | ImGuiKey_M, 0, Own },

            { NewTable, "NewTable", Scope::Main, "New table", Ctrl | ImGuiKey_N, 0, Own },
            { OpenLastProcess, "OpenLastProcess", Scope::Main, "Open the process from last time", Ctrl | Shift | ImGuiKey_P, 0, Own },
            { TableLuaScript, "TableLuaScript", Scope::Main, "Table Lua script", Ctrl | Alt | ImGuiKey_L, 0, CE },
            { TableFiles, "TableFiles", Scope::Main, "Table files", Ctrl | Alt | ImGuiKey_F, 0, Own },
            { AddAddress, "AddAddress", Scope::Main, "Add an address manually", ImGuiKey_Insert, 0, Own },
            { AddGroup, "AddGroup", Scope::Main, "Add a group", Ctrl | Shift | ImGuiKey_N, 0, Own },
            { AddScript, "AddScript", Scope::Main, "Add an Auto Assembler script", Ctrl | Shift | ImGuiKey_A, 0, Own },
            { DeactivateAll, "DeactivateAll", Scope::Main, "Deactivate all records", Ctrl | Shift | ImGuiKey_D, 0, Own },
            { FocusScanValue, "FocusScanValue", Scope::Main, "Type the scan value", Ctrl | ImGuiKey_F, 0, Own },
            { FirstScan, "FirstScan", Scope::Main, "First scan, or next scan once there are results", ImGuiKey_F3, 0, Own },
            { NewScan, "NewScan", Scope::Main, "New scan", Shift | ImGuiKey_F3, 0, Own },
            { UndoScan, "UndoScan", Scope::Main, "Undo the last scan", Ctrl | Alt | ImGuiKey_Z, 0, Own },
            { CompactMode, "CompactMode", Scope::Main, "Compact mode", Ctrl | Shift | ImGuiKey_C, 0, Own },
            { ReleaseMode, "ReleaseMode", Scope::Main, "Release mode", Ctrl | Shift | ImGuiKey_R, 0, Own },

            { ToggleRecords, "ToggleRecords", Scope::AddressList, "Activate or deactivate the selected records", ImGuiKey_Space, 0, CE },
            { ChangeRecordValue, "ChangeRecordValue", Scope::AddressList, "Change the value", ImGuiKey_Enter, 0, CE },
            { EditValueInline, "EditValueInline", Scope::AddressList, "Edit the value in place", ImGuiKey_F2, 0, Own },
            { ChangeDescription, "ChangeDescription", Scope::AddressList, "Change the description", Ctrl | ImGuiKey_Enter, 0, CE },
            { EditRecord, "EditRecord", Scope::AddressList, "Edit the record (address, type, options)", Ctrl | Alt | ImGuiKey_Enter, Alt | ImGuiKey_Enter, CE },
            { AssignHotkey, "AssignHotkey", Scope::AddressList, "Assign a hotkey", Ctrl | ImGuiKey_H, 0, CE },
            { UndoRecordValue, "UndoRecordValue", Scope::AddressList, "Undo the last value change", Ctrl | ImGuiKey_Z, 0, CE },
            { ShowAsHex, "ShowAsHex", Scope::AddressList, "Show as hexadecimal", Ctrl | Alt | ImGuiKey_H, 0, CE },
            { FreezeAllowIncrease, "FreezeAllowIncrease", Scope::AddressList, "Freeze, but allow the value to increase", ImGuiKey_KeypadAdd, 0, CE },
            { FreezeAllowDecrease, "FreezeAllowDecrease", Scope::AddressList, "Freeze, but allow the value to decrease", ImGuiKey_KeypadSubtract, 0, CE },
            { BrowseRecord, "BrowseRecord", Scope::AddressList, "Browse this memory region", Ctrl | ImGuiKey_B, 0, CE },
            { DisassembleRecord, "DisassembleRecord", Scope::AddressList, "Disassemble this memory region", Ctrl | ImGuiKey_D, 0, CE },
            { DissectRecord, "DissectRecord", Scope::AddressList, "Dissect the data at this address", Ctrl | Alt | ImGuiKey_D, 0, Own },
            { RecordAccesses, "RecordAccesses", Scope::AddressList, "Find out what accesses this address", ImGuiKey_F5, 0, CE },
            { RecordWrites, "RecordWrites", Scope::AddressList, "Find out what writes to this address", ImGuiKey_F6, 0, CE },
            { EditScript, "EditScript", Scope::AddressList, "Edit the script", Ctrl | ImGuiKey_E, 0, Own },
            { GroupRecords, "GroupRecords", Scope::AddressList, "Put the selected records into a new group", Ctrl | ImGuiKey_G, 0, Own },
            { ExpandGroups, "ExpandGroups", Scope::AddressList, "Expand every group", Ctrl | Alt | ImGuiKey_X, 0, Own },
            { CollapseGroups, "CollapseGroups", Scope::AddressList, "Collapse every group", Ctrl | Alt | ImGuiKey_C, 0, Own },
            { CutRecords, "CutRecords", Scope::AddressList, "Cut", Ctrl | ImGuiKey_X, 0, CE },
            { CopyRecords, "CopyRecords", Scope::AddressList, "Copy", Ctrl | ImGuiKey_C, 0, CE },
            { PasteRecords, "PasteRecords", Scope::AddressList, "Paste", Ctrl | ImGuiKey_V, 0, CE },
            { SelectAllRecords, "SelectAllRecords", Scope::AddressList, "Select all records", Ctrl | ImGuiKey_A, 0, CE },
            { DeleteRecords, "DeleteRecords", Scope::AddressList, "Delete the selected records", ImGuiKey_Delete, 0, CE },

            { AddResults, "AddResults", Scope::ScanResults, "Add the selected results to the address list", ImGuiKey_Enter, 0, Own },
            { ChangeResultValue, "ChangeResultValue", Scope::ScanResults, "Change the value of the selected results", Ctrl | ImGuiKey_E, 0, CE },
            { BrowseResult, "BrowseResult", Scope::ScanResults, "Browse this memory region", Ctrl | ImGuiKey_B, 0, CE },
            { DisassembleResult, "DisassembleResult", Scope::ScanResults, "Disassemble this memory region", Ctrl | ImGuiKey_D, 0, CE },
            { DissectResult, "DissectResult", Scope::ScanResults, "Dissect the data at this address", Ctrl | Alt | ImGuiKey_D, 0, Own },
            { ResultAccesses, "ResultAccesses", Scope::ScanResults, "Find out what accesses this address", Ctrl | ImGuiKey_F5, 0, CE },
            { ResultWrites, "ResultWrites", Scope::ScanResults, "Find out what writes to this address", Ctrl | ImGuiKey_F6, 0, CE },
            { CopyResults, "CopyResults", Scope::ScanResults, "Copy the selected addresses", Ctrl | ImGuiKey_C, 0, CE },
            { SelectAllResults, "SelectAllResults", Scope::ScanResults, "Select all results", Ctrl | ImGuiKey_A, 0, CE },

            { NewViewerWindow, "NewViewerWindow", Scope::MemoryViewer, "New memory viewer window", Ctrl | ImGuiKey_N, 0, Own },
            { SaveMemoryRegion, "SaveMemoryRegion", Scope::MemoryViewer, "Save a memory region to a file", Ctrl | ImGuiKey_S, 0, CE },
            { LoadMemoryRegion, "LoadMemoryRegion", Scope::MemoryViewer, "Load a file into memory", Ctrl | ImGuiKey_O, 0, CE },
            { GotoAddress, "GotoAddress", Scope::MemoryViewer, "Go to an address", Ctrl | ImGuiKey_G, 0, CE },
            { FindMemory, "FindMemory", Scope::MemoryViewer, "Find bytes", Ctrl | ImGuiKey_F, 0, CE },
            { FindAssembly, "FindAssembly", Scope::MemoryViewer, "Find assembly code", Ctrl | Shift | ImGuiKey_F, 0, Own },
            { UndoMemoryEdit, "UndoMemoryEdit", Scope::MemoryViewer, "Undo the last memory edit", Ctrl | ImGuiKey_Z, 0, CE },
            { MemoryRegions, "MemoryRegions", Scope::MemoryViewer, "Memory regions", Ctrl | ImGuiKey_R, 0, CE },
            { ReferencedFunctions, "ReferencedFunctions", Scope::MemoryViewer, "Referenced functions", Ctrl | Alt | ImGuiKey_F, 0, CE },
            { ReferencedStrings, "ReferencedStrings", Scope::MemoryViewer, "Referenced strings", Ctrl | Alt | ImGuiKey_R, 0, CE },
            { SymbolsAndModules, "SymbolsAndModules", Scope::MemoryViewer, "Modules and symbols", Ctrl | Alt | ImGuiKey_S, 0, CE },
            { DissectCode, "DissectCode", Scope::MemoryViewer, "Dissect code", Ctrl | ImGuiKey_J, 0, CE },
            { DissectData, "DissectData", Scope::MemoryViewer, "Dissect data at the hex view address", Ctrl | ImGuiKey_D, 0, CE },
            { ViewerAutoAssemble, "ViewerAutoAssemble", Scope::MemoryViewer, "Auto Assembler", Ctrl | ImGuiKey_A, 0, CE },
            { ViewerLuaEngine, "ViewerLuaEngine", Scope::MemoryViewer, "Lua engine", Ctrl | ImGuiKey_L, 0, CE },
            { InjectLibrary, "InjectLibrary", Scope::MemoryViewer, "Inject a library", Ctrl | ImGuiKey_I, 0, CE },
            { AllocateMemory, "AllocateMemory", Scope::MemoryViewer, "Allocate memory", Ctrl | Alt | ImGuiKey_M, 0, CE },
            { FillMemory, "FillMemory", Scope::MemoryViewer, "Fill memory", Ctrl | Alt | Shift | ImGuiKey_F, 0, CE },
            { CreateThread, "CreateThread", Scope::MemoryViewer, "Create a thread", Ctrl | Alt | ImGuiKey_T, 0, CE },
            { ScanForPatches, "ScanForPatches", Scope::MemoryViewer, "Scan for patches", Ctrl | Shift | ImGuiKey_P, 0, Own },
            { DebugRun, "DebugRun", Scope::MemoryViewer, "Run", ImGuiKey_F9, 0, CE },
            { DebugStepInto, "DebugStepInto", Scope::MemoryViewer, "Step into", ImGuiKey_F7, 0, CE },
            { DebugStepOver, "DebugStepOver", Scope::MemoryViewer, "Step over", ImGuiKey_F8, 0, CE },
            { DebugRunToReturn, "DebugRunToReturn", Scope::MemoryViewer, "Run until the function returns", Shift | ImGuiKey_F8, 0, CE },
            { DebugBreak, "DebugBreak", Scope::MemoryViewer, "Break into the process", ImGuiKey_F12, 0, Own },
            { ToggleBreakpoint, "ToggleBreakpoint", Scope::MemoryViewer, "Toggle a breakpoint on the selected instruction", ImGuiKey_F5, 0, CE },
            { SetInstructionPointer, "SetInstructionPointer", Scope::MemoryViewer, "Set the instruction pointer to the selected instruction", Ctrl | ImGuiKey_F4, 0, CE },
            { BreakpointList, "BreakpointList", Scope::MemoryViewer, "Breakpoint list", Ctrl | Alt | ImGuiKey_B, 0, Own },
            { EditBreakpoint, "EditBreakpoint", Scope::MemoryViewer, "Set a breakpoint with a condition or register changes", Shift | ImGuiKey_F5, 0, Own },
            { BreakAndTrace, "BreakAndTrace", Scope::MemoryViewer, "Break and trace from the selected instruction", Ctrl | ImGuiKey_T, 0, Own },
            { ThreadList, "ThreadList", Scope::MemoryViewer, "Thread list", Ctrl | Shift | ImGuiKey_T, 0, Own },

            { FollowInstruction, "FollowInstruction", Scope::Disassembler, "Follow the jump or call", ImGuiKey_Space, 0, CE },
            { NavigateBack, "NavigateBack", Scope::Disassembler, "Back", ImGuiKey_Backspace, 0, CE },
            { AssembleInstruction, "AssembleInstruction", Scope::Disassembler, "Assemble", ImGuiKey_Enter, 0, Own },
            { NopInstruction, "NopInstruction", Scope::Disassembler, "Replace with code that does nothing", Ctrl | Shift | ImGuiKey_N, 0, Own },
            { CopyInstructions, "CopyInstructions", Scope::Disassembler, "Copy the selected instructions", Ctrl | ImGuiKey_C, 0, Own },
            { CopyInstructionBytes, "CopyInstructionBytes", Scope::Disassembler, "Copy only the bytes", Ctrl | Alt | ImGuiKey_C, 0, CE },
            { InstructionAccesses, "InstructionAccesses", Scope::Disassembler, "Find out what addresses this instruction accesses", Ctrl | Alt | ImGuiKey_I, 0, Own },
            { CodeReferences, "CodeReferences", Scope::Disassembler, "Code that references this address", ImGuiKey_X, 0, Own },
            { AddInstructionToList, "AddInstructionToList", Scope::Disassembler, "Add this address to the address list", Ctrl | ImGuiKey_KeypadAdd, 0, CE },
            { SetBookmark, "SetBookmark", Scope::Disassembler, "Set a bookmark", Ctrl | Alt | ImGuiKey_0, 0, CE, true },
            { GotoBookmark, "GotoBookmark", Scope::Disassembler, "Go to a bookmark", Ctrl | ImGuiKey_0, 0, CE, true },
            { CommentInstruction, "CommentInstruction", Scope::Disassembler, "Add or change the comment", Ctrl | Shift | ImGuiKey_C, 0, Own },

            { HexFollow, "HexFollow", Scope::HexView, "Follow the pointer", ImGuiKey_Space, 0, CE },
            { HexBack, "HexBack", Scope::HexView, "Back", ImGuiKey_Backspace, 0, CE },
            { HexEditValue, "HexEditValue", Scope::HexView, "Edit the value", ImGuiKey_Enter, 0, Own },
            { HexCopy, "HexCopy", Scope::HexView, "Copy 16 bytes", Ctrl | ImGuiKey_C, 0, CE },
            { HexPaste, "HexPaste", Scope::HexView, "Paste bytes", Ctrl | ImGuiKey_P, Ctrl | ImGuiKey_V, CE },
            { HexAccesses, "HexAccesses", Scope::HexView, "Find out what accesses this address", ImGuiKey_F5, 0, Own },
            { HexWrites, "HexWrites", Scope::HexView, "Find out what writes to this address", ImGuiKey_F6, 0, Own },
            { HexBreakOnWrite, "HexBreakOnWrite", Scope::HexView, "Break when this address is written", Ctrl | Shift | ImGuiKey_F6, 0, Own },
            { HexBreakOnAccess, "HexBreakOnAccess", Scope::HexView, "Break when this address is read or written", Ctrl | Shift | ImGuiKey_F5, 0, Own },
            { HexAddToList, "HexAddToList", Scope::HexView, "Add this address to the address list", Ctrl | ImGuiKey_KeypadAdd, 0, CE },
            { HexBytes, "HexBytes", Scope::HexView, "Show bytes", Ctrl | ImGuiKey_1, 0, CE },
            { HexTwoBytes, "HexTwoBytes", Scope::HexView, "Show 2 bytes (hex)", Ctrl | ImGuiKey_3, 0, CE },
            { HexFourBytes, "HexFourBytes", Scope::HexView, "Show 4 bytes (hex)", Ctrl | ImGuiKey_5, 0, CE },
            { HexFourBytesDecimal, "HexFourBytesDecimal", Scope::HexView, "Show 4 bytes", Ctrl | ImGuiKey_6, 0, CE },
            { HexEightBytes, "HexEightBytes", Scope::HexView, "Show 8 bytes (hex)", Ctrl | ImGuiKey_7, 0, CE },
            { HexFloat, "HexFloat", Scope::HexView, "Show floats", Ctrl | ImGuiKey_9, 0, CE },
            { HexDouble, "HexDouble", Scope::HexView, "Show doubles", Ctrl | ImGuiKey_0, 0, CE },

            { DefineStructure, "DefineStructure", Scope::Structure, "Define a new structure", Ctrl | ImGuiKey_N, 0, CE },
            { AddStructureColumn, "AddStructureColumn", Scope::Structure, "Add an address to compare", Ctrl | ImGuiKey_A, 0, CE },
            { CollapseStructure, "CollapseStructure", Scope::Structure, "Collapse all pointers", Ctrl | Alt | ImGuiKey_C, 0, CE },
            { ChangeFieldValue, "ChangeFieldValue", Scope::Structure, "Change the value", ImGuiKey_Enter, 0, CE },
            { RenameField, "RenameField", Scope::Structure, "Rename the field", Ctrl | ImGuiKey_Enter, ImGuiKey_F2, CE },
            { InsertField, "InsertField", Scope::Structure, "Insert 4 bytes after the field", ImGuiKey_Insert, 0, CE },
            { DeleteField, "DeleteField", Scope::Structure, "Delete the field", ImGuiKey_Delete, 0, CE },
            { FieldToAddressList, "FieldToAddressList", Scope::Structure, "Add the field to the address list", ImGuiKey_A, 0, CE },
            { BrowseField, "BrowseField", Scope::Structure, "Browse the memory of this field", ImGuiKey_Space, 0, CE },
            { BrowseFieldPointer, "BrowseFieldPointer", Scope::Structure, "Browse the memory the pointer points to", ImGuiKey_F, 0, CE },
            { FieldAccesses, "FieldAccesses", Scope::Structure, "Find out what accesses this field", ImGuiKey_F5, 0, Own },
            { FieldWrites, "FieldWrites", Scope::Structure, "Find out what writes to this field", ImGuiKey_F6, 0, Own },

            { ScriptNewWindow, "ScriptNewWindow", Scope::ScriptEditor, "New Auto Assembler window", Ctrl | ImGuiKey_N, 0, CE },
            { ScriptOpen, "ScriptOpen", Scope::ScriptEditor, "Open a script", Ctrl | ImGuiKey_O, 0, CE },
            { ScriptSave, "ScriptSave", Scope::ScriptEditor, "Save (into the record, or to a file)", Ctrl | ImGuiKey_S, 0, CE },
            { ScriptSaveAs, "ScriptSaveAs", Scope::ScriptEditor, "Save to a file", Ctrl | Alt | ImGuiKey_S, 0, Own },
            { ScriptCheck, "ScriptCheck", Scope::ScriptEditor, "Check the syntax", Ctrl | Alt | ImGuiKey_C, 0, CE },
            { ScriptExecute, "ScriptExecute", Scope::ScriptEditor, "Execute ([ENABLE])", Ctrl | ImGuiKey_Enter, 0, Own },
            { ScriptExecuteDisable, "ScriptExecuteDisable", Scope::ScriptEditor, "Execute [DISABLE]", Ctrl | Shift | ImGuiKey_Enter, 0, Own },
            { TemplateCodeInjection, "TemplateCodeInjection", Scope::ScriptEditor, "Template: code injection", Ctrl | ImGuiKey_I, 0, CE },
            { TemplateAobInjection, "TemplateAobInjection", Scope::ScriptEditor, "Template: AOB injection", Ctrl | Shift | ImGuiKey_A, 0, CE },
            { TemplateFullInjection, "TemplateFullInjection", Scope::ScriptEditor, "Template: full injection", Ctrl | Shift | ImGuiKey_F, 0, CE },
            { TemplateCallLua, "TemplateCallLua", Scope::ScriptEditor, "Template: call a Lua function", Ctrl | ImGuiKey_L, 0, CE },
            { TemplateFramework, "TemplateFramework", Scope::ScriptEditor, "Template: cheat table framework code", Ctrl | Alt | ImGuiKey_T, 0, CE },

            { LuaOpen, "LuaOpen", Scope::LuaEngine, "Open a script", Ctrl | ImGuiKey_O, 0, CE },
            { LuaSave, "LuaSave", Scope::LuaEngine, "Save the script", Ctrl | ImGuiKey_S, 0, CE },
            { LuaSaveAs, "LuaSaveAs", Scope::LuaEngine, "Save the script under a new name", Ctrl | Alt | ImGuiKey_S, 0, CE },
            { LuaExecute, "LuaExecute", Scope::LuaEngine, "Execute", Ctrl | ImGuiKey_Enter, 0, Own },
            { LuaClearOutput, "LuaClearOutput", Scope::LuaEngine, "Clear the output", Ctrl | ImGuiKey_L, 0, Own },
            { LuaFunctions, "LuaFunctions", Scope::LuaEngine, "Show the function list", Ctrl | Shift | ImGuiKey_F, 0, Own },
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
        case CommandScope::Main: return "Main window";
        case CommandScope::AddressList: return "Address list";
        case CommandScope::ScanResults: return "Scan results";
        case CommandScope::MemoryViewer: return "Memory viewer";
        case CommandScope::Disassembler: return "Memory viewer: disassembler";
        case CommandScope::HexView: return "Memory viewer: hex view";
        case CommandScope::Structure: return "Structure dissect";
        case CommandScope::ScriptEditor: return "Auto Assembler and script editor";
        case CommandScope::LuaEngine: return "Lua engine";
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
        case CommandScope::Structure: return "While a structure dissect window has the focus. Field keys act on the selected field.";
        case CommandScope::ScriptEditor: return "While an Auto Assembler or script window has the focus. Template keys come from each template's Shortcut setting; the ones listed are the built-in templates.";
        case CommandScope::LuaEngine: return "While the Lua engine window has the focus.";
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

    std::string ShortcutsMarkdown()
    {
        std::string text = "# Keyboard shortcuts\n\n";
        text += "Generated by `Manifold-Engine.Tests.exe --shortcuts-markdown` from `Source/App/Commands.cpp`; the core tests compare this file with the table.\n\n";
        text += "Every shortcut belongs to a window or pane. Keys of the focused window win over the ones that work everywhere, ";
        text += "so `Ctrl+S` saves the script in an Auto Assembler window and the cheat table elsewhere. ";
        text += "Shortcuts marked **CE** are the ones Cheat Engine 7.5 uses for the same feature; the others are Manifold Engine's own. ";
        text += "Keys without Ctrl or Alt do nothing while a text field has the focus. ";
        text += "In the app, **Help > Keyboard shortcuts** (`F1`) shows the same list with a search.\n\n";
        text += "This file lists the defaults. **Settings > Keyboard** changes any of them: click the keys and press new ones, right-click to remove them. ";
        text += "Changed keys are stored in `Manifold-Engine.ini` under `[Shortcuts]` as `Key.<Command>=Ctrl+Shift+O` (a second shortcut after `;`, `none` for no keys), ";
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
