#pragma once

#include "App/Commands.h"
#include "Disasm/FunctionBounds.h"
#include "Lua/LuaEngine.h"
#include "UI/Templates.h"
#include "UI/ToolWindowBase.h"

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Manifold::UI
{
    class MemoryViewer : public ToolWindow
    {
    public:
        MemoryViewer(Workspace& workspace, int number);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show(std::uint64_t address, bool hexView);
        void Select(std::uint64_t address);
        void ShowHex(std::uint64_t address, std::uint64_t start, std::uint64_t stop);
        void Raise();
        int Number() const { return m_number; }

    private:
        enum RegionMode
        {
            RegionSave,
            RegionLoad,
            RegionFill
        };

        void MenuBar();
        void Toolbar();
        void Disassembly(float height);
        void HexView(float height);
        void Registers(float width, float height, const Debug::BreakInfo& info, bool running);
        bool RegisterSection(const char* label, int bit, std::string_view note);
        void RegisterViewItems();
        void GeneralRegisters(const Debug::BreakInfo& info, const Disasm::ConditionInfo& condition, float rowHeight, float nameWidth);
        void FlagCells(const Debug::BreakInfo& info, const Disasm::ConditionInfo& condition);
        void VectorSection(const Debug::BreakInfo& info, float rowHeight, float nameWidth);
        void FpuSection(const Debug::BreakInfo& info, float rowHeight, float nameWidth);
        const char* EditHint() const;
        struct StackNote
        {
            enum Type
            {
                Plain,
                Pointer,
                Code,
                Return
            };
            Type Kind = Plain;
            std::string Short;
            std::string Text;
            std::string Pointee;
            bool Named = false;
            bool Literal = false;
            std::uint64_t Call = 0;
            std::uint64_t Target = 0;
        };
        struct StackRow
        {
            int Index = 0;
            int Count = 1;
        };
        StackNote NoteFor(std::uint64_t value, bool is64Bit);
        void StackViewItems();
        void Stack(float width, float height, const Debug::BreakInfo& info, bool running);
        void SetHighlight(const std::string& token);
        void HighlightItems(const Disasm::Instruction* instruction);
        void ExceptionBreakItems();
        void ExceptionDialog();
        Debug::BreakInfo ShownBreak() const;
        bool RegisterLine(const std::string& id, std::string_view name, const std::string& editText, float rowHeight, float nameWidth, std::string& edited, ImVec2& valueMin, ImVec2& valueMax);
        void CodeMenu(const Disasm::Instruction& instruction);
        void ExplainInstruction(const Disasm::Instruction& line, const Disasm::RegisterValues* registers);
        void HexMenu(std::uint64_t address);
        std::vector<Disasm::Instruction> SelectedInstructions(bool names) const;
        std::vector<Disasm::Instruction> InstructionsBetween(std::uint64_t first, std::uint64_t last, bool names) const;
        std::optional<Disasm::FunctionSpan> FunctionOf(std::uint64_t address);
        std::string NoFunctionReason(std::uint64_t address) const;
        std::uint64_t SymbolEnd(std::uint64_t start);
        void FunctionMenu(const Disasm::Instruction& instruction);
        void JumpToFunction(Command command);
        void CopyInstructions(Target::AddressStyle style);
        void AddressCopyItems(std::uint64_t address);
        void BookmarkItems();
        void GotoDialog();
        void CommentDialog();
        void AssembleDialog();
        void FindDialog();
        void AllocateDialog();
        void InjectDialog();
        void RegionDialog();
        void ThreadDialog();
        void OpenRegionDialog(int mode);
        void Navigate(std::uint64_t address, bool remember = true);
        void HandleCodeKeys();
        void HandleHexKeys();
        void Shortcuts();
        void ScriptMenuBar();
        void ScriptShortcuts(std::vector<ImGuiKeyChord>& taken);
        void PublishSelection();
        void RunCommand(Command command);
        void Bookmark(bool set, int slot);
        void RemoveBookmark(int slot);
        Status WriteBytes(std::uint64_t address, std::span<const std::uint8_t> bytes, std::string source);
        std::size_t UndoableEdits() const;
        void UndoEdit();
        void Nop(const Disasm::Instruction& instruction);
        int HexValueSize() const;
        Scan::ValueType HexValueType() const;

        int m_nibbleWrite = 0;
        std::array<std::uint64_t, 10> m_bookmarks{};
        std::vector<std::uint64_t> m_hexHistory;
        int m_regionMode = RegionSave;
        bool m_regionOpen = false;
        std::string m_regionAddress;
        std::string m_regionSize = "1000";
        std::string m_regionFill = "90";
        std::string m_regionError;
        bool m_threadOpen = false;
        std::string m_threadAddress;
        std::string m_threadParameter = "0";
        std::string m_threadError;

        Workspace& m_workspace;
        Engine& m_engine;
        TemplateRunner m_templates;
        int m_number = 1;
        std::string m_title;
        bool m_open = true;
        bool m_bringToFront = false;

        std::uint64_t m_top = 0;
        std::uint64_t m_selected = 0;
        std::uint64_t m_selectionEnd = 0;
        std::uint64_t m_explainAddress = 0;
        double m_explainSince = 0.0;
        struct FunctionCache
        {
            std::uint64_t Address = 0;
            std::uint64_t Generation = 0;
            std::uint64_t Writes = 0;
            bool Valid = false;
            std::optional<Disasm::FunctionSpan> Span;
        };
        FunctionCache m_function;
        struct SymbolEndCache
        {
            std::uint64_t Generation = 0;
            std::uint64_t Symbols = 0;
            std::uint64_t Writes = 0;
            double Stamp = 0.0;
            std::unordered_map<std::uint64_t, std::uint64_t> Ends;
        };
        SymbolEndCache m_symbolEnds;
        std::vector<std::uint64_t> m_history;
        std::vector<Disasm::Instruction> m_lines;
        int m_visibleLines = 30;
        bool m_followBreaks = true;
        float m_registersWidth = 0.0f;
        Debug::BreakInfo m_heldBreak;
        bool m_registersRunning = false;
        std::uint64_t m_conditionSerial = 0;
        std::uint64_t m_conditionAddress = 0;
        Disasm::Instruction m_conditionInstruction;
        bool m_registersEditable = false;
        std::string m_registerEdit;
        std::string m_registerEditText;
        bool m_registerEditFocus = false;
        std::uint64_t m_registerSerial = 0;
        bool m_havePreviousRegisters = false;
        Disasm::RegisterValues m_previousRegisters;
        Disasm::RegisterValues m_lastRegisters;
        Debug::VectorRegisters m_previousVectors;
        Debug::VectorRegisters m_lastVectors;
        Debug::FpuRegisters m_previousFpu;
        Debug::FpuRegisters m_lastFpu;
        std::uint64_t m_stackSerial = 0;
        std::uint64_t m_stackTop = 0;
        std::uint64_t m_stackWrites = 0;
        std::vector<std::uint8_t> m_stackData;
        std::vector<StackRow> m_stackRows;
        std::unordered_map<int, StackNote> m_stackNotes;
        bool m_stackCallsOnly = false;
        int m_stackDigits = 16;
        std::uint64_t m_stackPreviousSerial = 0;
        std::vector<std::uint8_t> m_stackPrevious;
        std::uint64_t m_stackPreviousTop = 0;
        std::vector<std::uint8_t> m_stackShown;
        std::uint64_t m_stackShownTop = 0;
        std::string m_highlight;
        bool m_exceptionDialog = false;
        std::string m_exceptionStart;
        std::string m_exceptionStop;
        std::string m_exceptionError;
        std::uint64_t m_lastBreak = 0;
        bool m_focusCode = false;
        float m_addressWidth = 0.0f;
        float m_bytesWidth = 0.0f;
        float m_opcodeWidth = 0.0f;
        float m_dragStartWidth = 0.0f;
        bool m_addressAuto = true;
        std::uint32_t m_addressProcess = 0;

        std::uint64_t m_hexTop = 0;
        std::uint64_t m_hexSelected = 0;
        std::uint64_t m_hexMarkStart = 0;
        std::uint64_t m_hexMarkStop = 0;
        std::vector<Lua::ScriptMenuItem> m_scriptPopup;
        std::vector<Lua::ScriptMenuItem> m_scriptMenu;
        std::uint64_t m_scriptMenuGeneration = 0;
        int m_hexNibble = 0;
        bool m_hexFocus = false;
        int m_hexDisplay = 0;
        int m_hexColumns = 16;
        bool m_hexEditing = false;
        std::vector<std::uint8_t> m_hexPrevious;
        std::uint64_t m_hexPreviousTop = 0;
        std::vector<double> m_hexChangedAt;
        std::string m_valueBuffer;

        bool m_gotoOpen = false;
        bool m_gotoHex = false;
        std::string m_gotoText;
        std::string m_gotoError;
        bool m_assembleOpen = false;
        std::uint64_t m_assembleAddress = 0;
        std::string m_assembleText;
        std::string m_assembleError;
        bool m_assembleNopFill = true;
        bool m_findOpen = false;
        std::string m_findText;
        std::string m_findError;
        bool m_allocateOpen = false;
        std::string m_allocateSize = "1000";
        bool m_injectOpen = false;
        std::string m_injectPath;
        std::map<std::uint64_t, std::vector<std::uint8_t>> m_nopped;
        std::weak_ptr<Target::Process> m_noppedProcess;
        std::uint64_t m_noppedWrites = 0;

        ImGuiID m_viewport = 0;
        bool m_commentOpen = false;
        bool m_commentHeader = false;
        std::uint64_t m_commentAddress = 0;
        std::string m_commentText;
    };
}
