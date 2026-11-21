#pragma once

#include "Disasm/OpcodeConvert.h"
#include "UI/AddressActions.h"
#include "UI/CodeEditor.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Manifold::UI
{
    // Bytes to instructions and back, for x64 and x86, at an address so that
    // rip-relative operands and branch targets come out right.
    class OpcodeWindow : public ToolWindowBase
    {
    public:
        explicit OpcodeWindow(Workspace& workspace);
        void Draw() override;
        // Starts from the given bytes at the address, in the process's bitness.
        void Load(std::uint64_t address, std::span<const std::uint8_t> bytes);

    private:
        enum class Source
        {
            Instructions,
            Bytes
        };

        void MenuBar();
        void Convert();
        void Listing();
        std::vector<std::uint8_t> AllBytes() const;
        std::string InstructionText() const;
        std::string ListingText() const;

        Workspace& m_workspace;
        AddressField m_address;
        bool m_is64 = true;
        bool m_symbols = true;
        Source m_source = Source::Instructions;
        bool m_dirty = false;
        std::uint64_t m_lastAddress = 0;
        std::string m_asmText;
        std::string m_bytesText;
        CodeEditorState m_asmState;
        CodeEditorState m_bytesState;
        std::string m_byteError;
        std::vector<Disasm::ConvertedLine> m_lines;
    };
}
