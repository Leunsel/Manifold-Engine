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
        void Stack(float width, float height, const Debug::BreakInfo& info, bool running);
        Debug::BreakInfo ShownBreak() const;
        bool RegisterLine(const std::string& id, std::string_view name, const std::string& editText, float rowHeight, float nameWidth, std::string& edited, ImVec2& valueMin, ImVec2& valueMax);
        void CodeMenu(const Disasm::Instruction& instruction);
        void HexMenu(std::uint64_t address);
        std::vector<Disasm::Instruction> SelectedInstructions(bool names) const;
        std::vector<Disasm::Instruction> InstructionsBetween(std::uint64_t first, std::uint64_t last, bool names) const;
        std::optional<Disasm::FunctionSpan> FunctionOf(std::uint64_t address);
        std::string NoFunctionReason(std::uint64_t address) const;
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
        struct FunctionCache
        {
            std::uint64_t Address = 0;
            std::uint64_t Generation = 0;
            std::uint64_t Writes = 0;
            bool Valid = false;
            std::optional<Disasm::FunctionSpan> Span;
        };
        FunctionCache m_function;
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
        int m_vectorFormat = 0;
        std::string m_registerEdit;
        std::string m_registerEditText;
        bool m_registerEditFocus = false;
        std::uint64_t m_registerSerial = 0;
        bool m_havePreviousRegisters = false;
        Disasm::RegisterValues m_previousRegisters;
        Disasm::RegisterValues m_lastRegisters;
        Debug::VectorRegisters m_previousVectors;
        Debug::VectorRegisters m_lastVectors;
        std::uint64_t m_lastBreak = 0;
        float m_codeShare = 0.62f;
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

        ImGuiID m_viewport = 0;
        bool m_commentOpen = false;
        bool m_commentHeader = false;
        std::uint64_t m_commentAddress = 0;
        std::string m_commentText;
    };
}
