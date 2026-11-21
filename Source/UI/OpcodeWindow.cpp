#include "UI/OpcodeWindow.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Target/Process.h"
#include "Target/Symbols.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <cmath>

namespace Manifold::UI
{
    OpcodeWindow::OpcodeWindow(Workspace& workspace) : m_workspace(workspace)
    {
        m_address.Text = "0";
        m_asmState.Placeholder = "mov rax,[rcx+10]";
        m_bytesState.Placeholder = "48 8B 41 10";
        Engine& engine = m_workspace.Core();
        if (engine.HasProcess())
            m_is64 = engine.Process()->Is64Bit();
    }

    void OpcodeWindow::Load(std::uint64_t address, std::span<const std::uint8_t> bytes)
    {
        Engine& engine = m_workspace.Core();
        if (engine.HasProcess())
            m_is64 = engine.Process()->Is64Bit();
        m_address.Text = address ? Hex(address) : "0";
        m_address.Generation = 0;
        if (!bytes.empty())
        {
            m_bytesText = Disasm::FormatBytes(bytes, Disasm::ByteStyle::Spaced);
            m_source = Source::Bytes;
        }
        m_dirty = true;
        Show();
    }

    std::vector<std::uint8_t> OpcodeWindow::AllBytes() const
    {
        std::vector<std::uint8_t> bytes;
        for (const Disasm::ConvertedLine& line : m_lines)
            bytes.insert(bytes.end(), line.Bytes.begin(), line.Bytes.end());
        return bytes;
    }

    std::string OpcodeWindow::InstructionText() const
    {
        std::string text;
        for (const Disasm::ConvertedLine& line : m_lines)
            text += line.Text + "\n";
        return text;
    }

    std::string OpcodeWindow::ListingText() const
    {
        std::string text;
        for (const Disasm::ConvertedLine& line : m_lines)
            text += Hex(line.Address) + " - " + Disasm::FormatBytes(line.Bytes, Disasm::ByteStyle::Spaced) + " - " + line.Text + "\n";
        return text;
    }

    void OpcodeWindow::Convert()
    {
        m_dirty = false;
        Engine& engine = m_workspace.Core();
        const bool symbols = m_symbols && engine.HasProcess();
        const std::uint64_t address = Resolve(engine, m_address).value_or(0);
        if (m_source == Source::Instructions)
        {
            Asm::SymbolResolver resolve;
            if (symbols)
            {
                resolve = [&engine](std::string_view name) -> Asm::SymbolValue
                    {
                        if (const auto value = engine.Symbols().Resolve(name))
                            return { Asm::SymbolState::Known, *value };
                        return {};
                    };
            }
            m_lines = Disasm::AssembleText(m_asmText, address, m_is64, resolve);
            // Line for line with the instructions: comments, blank and failed
            // lines stay empty, so each byte line sits beside its instruction.
            std::vector<std::string> rows(static_cast<std::size_t>(std::count(m_asmText.begin(), m_asmText.end(), '\n') + 1));
            for (const Disasm::ConvertedLine& line : m_lines)
            {
                if (line.SourceLine >= 1 && static_cast<std::size_t>(line.SourceLine) <= rows.size())
                    rows[static_cast<std::size_t>(line.SourceLine - 1)] = Disasm::FormatBytes(line.Bytes, Disasm::ByteStyle::Spaced);
            }
            while (!rows.empty() && rows.back().empty())
                rows.pop_back();
            std::string bytes;
            for (std::size_t i = 0; i < rows.size(); ++i)
                bytes += (i ? "\n" : "") + rows[i];
            m_bytesText = bytes;
            m_byteError.clear();
            return;
        }
        std::string error;
        const auto parsed = Disasm::ParseByteText(m_bytesText, &error);
        if (!parsed)
        {
            m_byteError = error;
            m_lines.clear();
            return;
        }
        m_byteError.clear();
        m_lines = Disasm::DecodeBytes(*parsed, address, m_is64, symbols ? &engine.Symbols() : nullptr);
        std::string text;
        for (const Disasm::ConvertedLine& line : m_lines)
            text += (text.empty() ? "" : "\n") + line.Text;
        m_asmText = text;
    }

    void OpcodeWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        Engine& engine = m_workspace.Core();
        if (ImGui::BeginMenu("Convert"))
        {
            if (ImGui::MenuItem("64-Bit Code (x64)", nullptr, m_is64))
            {
                m_is64 = true;
                m_dirty = true;
            }
            if (ImGui::MenuItem("32-Bit Code (x86)", nullptr, !m_is64))
            {
                m_is64 = false;
                m_dirty = true;
            }
            if (ImGui::MenuItem(ICON_FA_MICROCHIP "  Like the Process", nullptr, false, engine.HasProcess()))
            {
                m_is64 = engine.Process()->Is64Bit();
                m_dirty = true;
            }
            ItemTooltip("Takes the bitness of the opened process.");
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TAG "  Use the Process's Symbols", nullptr, m_symbols))
            {
                m_symbols = !m_symbols;
                m_dirty = true;
            }
            ItemTooltip("Lets instructions name modules and symbols of the opened process, and shows them in the decoded instructions.");
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_ERASER "  Clear"))
            {
                m_asmText.clear();
                m_bytesText.clear();
                m_lines.clear();
                m_byteError.clear();
                m_source = Source::Instructions;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Copy"))
        {
            const std::vector<std::uint8_t> bytes = AllBytes();
            const bool any = !bytes.empty();
            if (ImGui::MenuItem(ICON_FA_COPY "  Bytes", nullptr, false, any))
                ImGui::SetClipboardText(Disasm::FormatBytes(bytes, Disasm::ByteStyle::Spaced).c_str());
            if (ImGui::MenuItem(ICON_FA_COPY "  Bytes Without Spaces", nullptr, false, any))
                ImGui::SetClipboardText(Disasm::FormatBytes(bytes, Disasm::ByteStyle::Compact).c_str());
            if (ImGui::MenuItem(ICON_FA_COPY "  Bytes as Escaped String", nullptr, false, any))
                ImGui::SetClipboardText(Disasm::FormatBytes(bytes, Disasm::ByteStyle::Escaped).c_str());
            if (ImGui::MenuItem(ICON_FA_COPY "  Bytes as C Array", nullptr, false, any))
                ImGui::SetClipboardText(("{ " + Disasm::FormatBytes(bytes, Disasm::ByteStyle::CArray) + " }").c_str());
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COPY "  Instructions", nullptr, false, !m_lines.empty()))
                ImGui::SetClipboardText(InstructionText().c_str());
            if (ImGui::MenuItem(ICON_FA_COPY "  Listing with Addresses", nullptr, false, !m_lines.empty()))
                ImGui::SetClipboardText(ListingText().c_str());
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void OpcodeWindow::Listing()
    {
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##listing", 4, flags, ImVec2(0.0f, 0.0f)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        ClippedRows(static_cast<int>(m_lines.size()), [&](int index)
            {
                const Disasm::ConvertedLine& line = m_lines[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                PushMono();
                ImGui::TextColored(ManifoldTheme::AddressText, "%s", Hex(line.Address).c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(ManifoldTheme::BytesText, "%s", Disasm::FormatBytes(line.Bytes, Disasm::ByteStyle::Spaced).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(line.Text.c_str());
                PopMono();
                ImGui::TableNextColumn();
                if (!line.Error.empty())
                {
                    const std::string problem = line.SourceLine ? "Line " + std::to_string(line.SourceLine) + ": " + line.Error : line.Error;
                    TextFitted(problem, ImGui::GetContentRegionAvail().x, ManifoldTheme::Error);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("%s", problem.c_str());
                }
            });
        ImGui::EndTable();
    }

    void OpcodeWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_EXCHANGE_ALT "  Opcode Converter###opcodeconverter", ImVec2(880.0f, 600.0f), ImGuiWindowFlags_MenuBar, ImVec2(560.0f, 340.0f)))
            return;
        Engine& engine = m_workspace.Core();
        MenuBar();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Address");
        ImGui::SameLine();
        AddressInput("##address", engine, m_address, Px(240.0f), "0 or an expression");
        ItemTooltip("Where the code would lie: rip-relative operands and jump or call targets are worked out from it.");
        ImGui::SameLine(0.0f, Px(12.0f));
        int bits = m_is64 ? 0 : 1;
        if (Segmented("##bits", &bits, { "x64", "x86" }, Px(120.0f)))
        {
            m_is64 = bits == 0;
            m_dirty = true;
        }
        const std::uint64_t address = Resolve(engine, m_address).value_or(0);
        if (address != m_lastAddress)
        {
            m_lastAddress = address;
            m_dirty = true;
        }
        std::size_t problems = 0;
        for (const Disasm::ConvertedLine& line : m_lines)
            problems += line.Error.empty() ? 0 : 1;
        ImGui::SameLine(0.0f, Px(12.0f));
        ImGui::AlignTextToFramePadding();
        const std::size_t total = AllBytes().size();
        std::string summary = std::to_string(m_lines.size()) + (m_lines.size() == 1 ? " instruction, " : " instructions, ") + std::to_string(total) + (total == 1 ? " byte" : " bytes");
        if (problems)
            summary += ", " + std::to_string(problems) + (problems == 1 ? " problem" : " problems");
        Muted(summary);

        const ImGuiStyle& style = ImGui::GetStyle();
        const float half = std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f);
        const float height = std::max(Px(120.0f), std::floor(ImGui::GetContentRegionAvail().y * 0.45f));
        ImGui::BeginGroup();
        SectionLabel(m_source == Source::Instructions ? "Instructions, one per line  (converting from here)" : "Instructions, one per line");
        if (CodeEditor("##instructions", m_asmText, Language::AutoAssembler, ImVec2(half, height), m_asmState))
        {
            m_source = Source::Instructions;
            m_dirty = true;
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        SectionLabel(m_source == Source::Bytes ? "Bytes in hex  (converting from here)" : "Bytes in hex");
        if (CodeEditor("##bytes", m_bytesText, Language::Plain, ImVec2(half, height), m_bytesState))
        {
            m_source = Source::Bytes;
            m_dirty = true;
        }
        ImGui::EndGroup();
        if (m_dirty)
            Convert();
        if (!m_byteError.empty())
            TextWithError(m_byteError, ManifoldTheme::Error);
        Listing();
        EndToolWindow();
    }
}
