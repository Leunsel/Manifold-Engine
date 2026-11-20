#include "UI/MemoryViewer.h"

#include "App/Settings.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace Manifold::UI
{
    namespace
    {
        struct RegisterRow
        {
            const char* Name;
            std::uint64_t Disasm::RegisterValues::*Field;
            bool Only64;
        };

        const RegisterRow kRegisterRows[] = {
            { "RAX", &Disasm::RegisterValues::Rax, false }, { "RBX", &Disasm::RegisterValues::Rbx, false }, { "RCX", &Disasm::RegisterValues::Rcx, false },
            { "RDX", &Disasm::RegisterValues::Rdx, false }, { "RSI", &Disasm::RegisterValues::Rsi, false }, { "RDI", &Disasm::RegisterValues::Rdi, false },
            { "RBP", &Disasm::RegisterValues::Rbp, false }, { "RSP", &Disasm::RegisterValues::Rsp, false }, { "R8", &Disasm::RegisterValues::R8, true },
            { "R9", &Disasm::RegisterValues::R9, true }, { "R10", &Disasm::RegisterValues::R10, true }, { "R11", &Disasm::RegisterValues::R11, true },
            { "R12", &Disasm::RegisterValues::R12, true }, { "R13", &Disasm::RegisterValues::R13, true }, { "R14", &Disasm::RegisterValues::R14, true },
            { "R15", &Disasm::RegisterValues::R15, true }, { "RIP", &Disasm::RegisterValues::Rip, false },
        };

        struct FlagBit
        {
            const char* Name;
            int Bit;
        };

        const FlagBit kFlags[] = { { "CF", 0 }, { "PF", 2 }, { "AF", 4 }, { "ZF", 6 }, { "SF", 7 }, { "TF", 8 }, { "DF", 10 }, { "OF", 11 } };

        struct FlagHelp
        {
            const char* Title;
            const char* Meaning;
            const char* Readers;
        };

        const FlagHelp kFlagHelp[] = {
            { "Carry", "1 when an unsigned addition carried out of the top bit, or a subtraction or comparison had to borrow because the first value was below the second. Shifts and rotates put the bit they move out here.",
              "jb/jc, jae/jnc, jbe, ja, adc, sbb, setb, cmovb" },
            { "Parity", "1 when the lowest byte of the result has an even number of set bits. Compilers check it after float comparisons (ucomiss, comisd) to catch NaN.",
              "jp/jpe, jnp/jpo, setp, cmovp" },
            { "Auxiliary carry", "1 when bit 3 carried into bit 4 or borrowed from it. Only the decimal (BCD) adjustments use it.", "daa, das, aaa, aas" },
            { "Zero", "1 when the result was zero: cmp of two equal values, test of a zero value, or a count that reached zero.", "je/jz, jne/jnz, jbe, ja, jle, jg, sete, cmove" },
            { "Sign", "Copy of the top bit of the result: 1 when the result is negative as a signed number.", "js, jns, jl, jge, jle, jg, sets, cmovs" },
            { "Trap", "When 1, the processor stops the thread after every instruction. The debugger sets it itself for single steps; switching it on here makes the thread stop again after its next instruction.",
              "" },
            { "Direction", "Direction of the string instructions movs, stos, cmps, scas and lods: 0 counts up, 1 counts down. Code expects 0 outside of such loops.",
              "rep movs, rep stos, cmps, scas, lods" },
            { "Overflow", "1 when a signed result did not fit, for example when adding two large positive numbers gave a negative one.", "jo, jno, jl, jge, jle, jg, seto, cmovo" },
        };

        const std::vector<const char*> kVectorFormats = { "Float", "Double", "Int", "Hex" };

        int VectorLanes(int format)
        {
            return format == 1 ? 2 : format == 3 ? 1 : 4;
        }

        std::string NumberText(double value, int digits)
        {
            if (std::isnan(value))
                return "nan";
            if (std::isinf(value))
                return value < 0 ? "-inf" : "inf";
            return Format("%.*g", digits, value);
        }

        std::string LaneText(const std::array<std::uint8_t, 16>& xmm, int format, int lane, int digits)
        {
            switch (format)
            {
            case 0:
            {
                float value = 0.0f;
                std::memcpy(&value, xmm.data() + lane * 4, sizeof(value));
                return NumberText(value, digits);
            }
            case 1:
            {
                double value = 0.0;
                std::memcpy(&value, xmm.data() + lane * 8, sizeof(value));
                return NumberText(value, digits);
            }
            case 2:
            {
                std::int32_t value = 0;
                std::memcpy(&value, xmm.data() + lane * 4, sizeof(value));
                return std::to_string(value);
            }
            default:
            {
                std::uint64_t low = 0;
                std::uint64_t high = 0;
                std::memcpy(&low, xmm.data(), 8);
                std::memcpy(&high, xmm.data() + 8, 8);
                return Hex(high, 16) + " " + Hex(low, 16);
            }
            }
        }

        std::string VectorText(const std::array<std::uint8_t, 16>& xmm, int format)
        {
            std::string text;
            for (int lane = 0; lane < VectorLanes(format); ++lane)
            {
                if (lane)
                    text += ' ';
                text += LaneText(xmm, format, lane, format == 1 ? 17 : 9);
            }
            return text;
        }

        std::optional<std::array<std::uint8_t, 16>> ParseVector(std::string_view text, int format)
        {
            std::string cleaned(text);
            for (char& c : cleaned)
            {
                if (c == ',' || c == ';' || c == '\t')
                    c = ' ';
            }
            std::array<std::uint8_t, 16> xmm{};
            if (format == 3)
            {
                std::string digits;
                for (const char c : cleaned)
                {
                    if (c != ' ')
                        digits += c;
                }
                if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
                    digits.erase(0, 2);
                if (digits.empty() || digits.size() > 32)
                    return std::nullopt;
                digits.insert(0, 32 - digits.size(), '0');
                const auto high = ParseHex(digits.substr(0, 16));
                const auto low = ParseHex(digits.substr(16));
                if (!high || !low)
                    return std::nullopt;
                std::memcpy(xmm.data(), &*low, 8);
                std::memcpy(xmm.data() + 8, &*high, 8);
                return xmm;
            }
            const std::vector<std::string> parts = Split(cleaned, ' ');
            if (static_cast<int>(parts.size()) != VectorLanes(format))
                return std::nullopt;
            for (int lane = 0; lane < VectorLanes(format); ++lane)
            {
                const std::string& part = parts[static_cast<std::size_t>(lane)];
                if (format == 2)
                {
                    const bool hex = part.size() > 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X');
                    std::int64_t value = 0;
                    if (hex)
                    {
                        const auto parsed = ParseHex(part.substr(2));
                        if (!parsed || *parsed > 0xFFFFFFFFull)
                            return std::nullopt;
                        value = static_cast<std::int64_t>(*parsed);
                    }
                    else
                    {
                        const auto parsed = ParseDecimal(part);
                        if (!parsed || *parsed < INT32_MIN || *parsed > static_cast<std::int64_t>(UINT32_MAX))
                            return std::nullopt;
                        value = *parsed;
                    }
                    const std::uint32_t bits = static_cast<std::uint32_t>(value);
                    std::memcpy(xmm.data() + lane * 4, &bits, 4);
                    continue;
                }
                const auto parsed = ParseDouble(part);
                if (!parsed)
                    return std::nullopt;
                if (format == 0)
                {
                    const float value = static_cast<float>(*parsed);
                    std::memcpy(xmm.data() + lane * 4, &value, 4);
                }
                else
                {
                    std::memcpy(xmm.data() + lane * 8, &*parsed, 8);
                }
            }
            return xmm;
        }

        void ClippedText(ImDrawList* draw, ImVec2 at, float width, ImU32 color, const std::string& text)
        {
            draw->PushClipRect(at, ImVec2(at.x + width, at.y + ImGui::GetTextLineHeight() + 2.0f), true);
            draw->AddText(at, color, text.c_str());
            draw->PopClipRect();
        }
    }

    bool MemoryViewer::RegisterLine(const std::string& id, std::string_view name, const std::string& editText, float rowHeight, float nameWidth, std::string& edited,
        ImVec2& valueMin, ImVec2& valueMax)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float line = ImGui::GetTextLineHeight();
        const float right = start.x + ImGui::GetContentRegionAvail().x;
        valueMin = valueMax = start;
        if (m_registerEdit == id)
        {
            const float frame = ImGui::GetFrameHeight();
            const float height = std::max(rowHeight, frame);
            draw->AddText(ImVec2(start.x + Px(4.0f), std::round(start.y + (height - line) * 0.5f)), ImGui::GetColorU32(ManifoldTheme::CodeRegister),
                std::string(name).c_str());
            ImGui::SetCursorScreenPos(ImVec2(start.x + nameWidth, start.y + (height - frame) * 0.5f));
            ImGui::SetNextItemWidth(right - (start.x + nameWidth));
            if (m_registerEditFocus)
            {
                ImGui::SetKeyboardFocusHere();
                m_registerEditFocus = false;
            }
            const bool enter = InputText("##edit", m_registerEditText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            bool committed = false;
            if (enter)
            {
                edited = m_registerEditText;
                committed = true;
                m_registerEdit.clear();
            }
            else if (ImGui::IsItemDeactivated() || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                m_registerEdit.clear();
            }
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            return committed;
        }
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight)) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            !m_registersRunning)
        {
            m_registerEdit = id;
            m_registerEditText = editText;
            m_registerEditFocus = true;
        }
        const float textY = std::round(start.y + (rowHeight - line) * 0.5f);
        draw->AddText(ImVec2(start.x + Px(4.0f), textY), ImGui::GetColorU32(ManifoldTheme::CodeRegister), std::string(name).c_str());
        valueMin = ImVec2(start.x + nameWidth, textY);
        valueMax = ImVec2(right - Px(2.0f), textY + line);
        return false;
    }

    Debug::BreakInfo MemoryViewer::ShownBreak() const
    {
        const Debug::BreakInfo live = m_engine.Debugger().Break();
        if (!live.Broken && m_heldBreak.Broken && m_engine.Debugger().Stepping())
            return m_heldBreak;
        return live;
    }

    namespace
    {
        constexpr int kSectionGeneral = 1;
        constexpr int kSectionFlags = 2;
        constexpr int kSectionVectors = 4;
        constexpr int kSectionFpu = 8;

        const char* const kRoundingNames[] = { "nearest", "down", "up", "toward zero" };
        const char* const kPrecisionNames[] = { "24-bit", "reserved", "53-bit", "64-bit" };
        const char* const kExceptionBits[] = { "IE", "DE", "ZE", "OE", "UE", "PE" };
        const char* const kExceptionNames[] = { "invalid operation", "denormal operand", "divide by zero", "overflow", "underflow", "precision" };

        std::string RaisedExceptions(std::uint32_t bits)
        {
            std::string text;
            for (int i = 0; i < 6; ++i)
            {
                if ((bits >> i) & 1)
                    text += std::string(text.empty() ? "" : " ") + kExceptionBits[i];
            }
            return text;
        }

        std::string ExtendedHex(const std::uint8_t* ten)
        {
            std::uint64_t mantissa = 0;
            std::memcpy(&mantissa, ten, 8);
            return Hex(static_cast<std::uint64_t>(ten[8] | (ten[9] << 8)), 4) + " " + Hex(mantissa, 16);
        }

        const char* TagName(const std::uint8_t* ten, bool used)
        {
            if (!used)
                return "empty";
            std::uint64_t mantissa = 0;
            std::memcpy(&mantissa, ten, 8);
            const unsigned exponent = static_cast<unsigned>(ten[8] | (ten[9] << 8)) & 0x7FFF;
            if (exponent == 0 && mantissa == 0)
                return "zero";
            if (exponent == 0x7FFF || exponent == 0 || (mantissa >> 63) == 0)
                return "special";
            return "valid";
        }

        bool LaneDiffers(const std::array<std::uint8_t, 16>& now, const std::array<std::uint8_t, 16>& before, int format, int lane)
        {
            const int width = format == 1 ? 8 : format == 3 ? 16 : 4;
            return std::memcmp(now.data() + lane * width, before.data() + lane * width, static_cast<std::size_t>(width)) != 0;
        }
    }

    bool MemoryViewer::RegisterSection(const char* label, int bit, std::string_view note)
    {
        Settings& options = m_engine.Options();
        const bool open = (options.RegisterSections & bit) != 0;
        PopMono();
        ImGui::PushID(label);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = std::round(ImGui::GetTextLineHeight() + Px(6.0f));
        if (ImGui::InvisibleButton("##section", ImVec2(width, height)))
        {
            options.RegisterSections ^= bit;
            m_engine.SaveOptionsSoon();
        }
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (hovered)
            draw->AddRectFilled(start, ImVec2(start.x + width, start.y + height), ImGui::GetColorU32(ImGuiCol_HeaderHovered), ImGui::GetStyle().FrameRounding);
        const float textY = std::round(start.y + (height - ImGui::GetTextLineHeight()) * 0.5f);
        const ImU32 muted = ImGui::GetColorU32(ManifoldTheme::Muted);
        draw->AddText(ImVec2(start.x + Px(2.0f), textY), muted, open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
        draw->AddText(ImVec2(start.x + Px(18.0f), textY), muted, label);
        if (!note.empty())
        {
            const std::string text(note);
            const float noteWidth = ImGui::CalcTextSize(text.c_str()).x;
            const float labelEnd = start.x + Px(26.0f) + ImGui::CalcTextSize(label).x;
            const float noteX = std::max(labelEnd, start.x + width - noteWidth - Px(4.0f));
            ClippedText(draw, ImVec2(noteX, textY), start.x + width - noteX, muted, text);
        }
        ImGui::PopID();
        PushMono();
        return open;
    }

    void MemoryViewer::RegisterViewItems()
    {
        Settings& options = m_engine.Options();
        if (ImGui::MenuItem(ICON_FA_PEN "  Allow changing registers", nullptr, &options.RegisterEditing))
            m_engine.SaveOptionsSoon();
        ItemTooltip("On: double-click a register, flag or FPU/SSE value to change it while the process is stopped. Off: the panel only shows them.");
        ImGui::Separator();
        const struct { const char* Label; int Bit; } sections[] = {
            { "General registers", kSectionGeneral }, { "Flags", kSectionFlags }, { "SSE (XMM, MXCSR)", kSectionVectors }, { "x87 FPU (ST0-ST7)", kSectionFpu } };
        for (const auto& section : sections)
        {
            bool shown = (options.RegisterSections & section.Bit) != 0;
            if (ImGui::MenuItem(section.Label, nullptr, &shown))
            {
                options.RegisterSections ^= section.Bit;
                m_engine.SaveOptionsSoon();
            }
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("XMM values as"))
        {
            for (int format = 0; format < static_cast<int>(kVectorFormats.size()); ++format)
            {
                if (ImGui::MenuItem(kVectorFormats[static_cast<std::size_t>(format)], nullptr, options.VectorFormat == format))
                {
                    options.VectorFormat = format;
                    m_registerEdit.clear();
                    m_engine.SaveOptionsSoon();
                }
            }
            ImGui::EndMenu();
        }
    }

    void MemoryViewer::Registers(float width, float height, const Debug::BreakInfo& info, bool running)
    {
        Debug::Debugger& debugger = m_engine.Debugger();
        Settings& options = m_engine.Options();
        m_registersRunning = running;
        m_registersEditable = options.RegisterEditing && !running;
        if (running)
            m_registerEdit.clear();
        if (info.Serial != m_registerSerial)
        {
            m_havePreviousRegisters = m_registerSerial != 0;
            m_previousRegisters = m_lastRegisters;
            m_previousVectors = m_lastVectors;
            m_previousFpu = m_lastFpu;
            m_registerSerial = info.Serial;
            m_registerEdit.clear();
        }
        m_lastRegisters = info.Registers;
        m_lastVectors = info.Vectors;
        m_lastFpu = info.Fpu;

        if (info.Serial != m_conditionSerial || info.Registers.Rip != m_conditionAddress)
        {
            m_conditionSerial = info.Serial;
            m_conditionAddress = info.Registers.Rip;
            std::uint8_t bytes[16] = {};
            const std::size_t got = m_engine.Process()->Read(info.Registers.Rip, bytes, sizeof(bytes));
            debugger.Unpatch(info.Registers.Rip, bytes, sizeof(bytes));
            m_conditionInstruction = got ? m_engine.Disassembler().Decode(info.Registers.Rip, bytes, got, nullptr) : Disasm::Instruction{};
        }
        const Disasm::ConditionInfo condition = m_conditionInstruction.Valid ? Disasm::EvaluateCondition(m_conditionInstruction.Mnemonic, info.Registers, info.Is64Bit)
                                                                            : Disasm::ConditionInfo{};

        BeginCard("##registers", nullptr, ImVec2(width, height));
        CardTitle(ICON_FA_MICROCHIP, "Registers", "thread " + std::to_string(info.ThreadId) + (running ? "  ·  stepping..." : ""));
        if (ImGui::BeginPopupContextWindow("##registerpanel", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            RegisterViewItems();
            ImGui::EndPopup();
        }
        if (info.Exception.Raised)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_EXCLAMATION_TRIANGLE "  %s", Debug::DescribeException(info.Exception).c_str());
            ImGui::PopTextWrapPos();
            ImGui::TextColored(ManifoldTheme::Muted, "Run passes it to the program. Change a register (RIP past the instruction) or use Debug > Continue, exception handled, to go on without it.");
            ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
        }
        PushMono();
        const float line = ImGui::GetTextLineHeight();
        const float rowHeight = std::round(line + Px(5.0f));
        const float nameWidth = ImGui::CalcTextSize("MXCSR").x + Px(14.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));

        if (RegisterSection("General", kSectionGeneral, {}))
            GeneralRegisters(info, condition, rowHeight, nameWidth);
        ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
        std::string verdict;
        if (condition.Kind != Disasm::ConditionKind::None)
        {
            const char* outcome = condition.Kind == Disasm::ConditionKind::Move ? (condition.Taken ? "moves" : "does not move")
                                : condition.Kind == Disasm::ConditionKind::Set  ? (condition.Taken ? "sets 1" : "sets 0")
                                                                                : (condition.Taken ? "jumps" : "does not jump");
            verdict = m_conditionInstruction.Mnemonic + " " + outcome;
        }
        if (RegisterSection("Flags", kSectionFlags, verdict))
            FlagCells(info, condition);
        if (info.Vectors.Count > 0)
        {
            ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
            const std::string note = std::string(kVectorFormats[static_cast<std::size_t>(std::clamp(options.VectorFormat, 0, 3))]) + ", lane 0 first";
            if (RegisterSection("SSE", kSectionVectors, note))
                VectorSection(info, rowHeight, nameWidth);
        }
        else if (debugger.UsingVeh())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ManifoldTheme::Muted, "XMM registers come from the in-process debugger. The one loaded into this process is older and does not send them; they appear after the process restarts.");
            ImGui::PopTextWrapPos();
        }
        if (info.Fpu.Valid)
        {
            ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
            const int used = std::popcount(static_cast<unsigned>(info.Fpu.Tags()));
            if (RegisterSection("x87 FPU", kSectionFpu, used == 0 ? std::string("empty") : std::to_string(used) + " in use, TOP " + std::to_string(info.Fpu.Top())))
                FpuSection(info, rowHeight, nameWidth);
        }
        ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
        ImGui::PopStyleVar();
        PopMono();
        EndCard();
    }

    void MemoryViewer::GeneralRegisters(const Debug::BreakInfo& info, const Disasm::ConditionInfo& condition, float rowHeight, float nameWidth)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 textColor = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 changedColor = ImGui::GetColorU32(ManifoldTheme::Changed);
        const int digits = info.Is64Bit ? 16 : 8;
        Disasm::RegisterValues values = info.Registers;
        bool changed = false;
        for (const RegisterRow& row : kRegisterRows)
        {
            if (row.Only64 && !info.Is64Bit)
                continue;
            std::string name = row.Name;
            if (!info.Is64Bit)
                name[0] = 'E';
            const std::uint64_t value = values.*row.Field;
            const std::string text = Hex(value, digits);
            ImGui::PushID(row.Name);
            std::string edited;
            ImVec2 valueMin;
            ImVec2 valueMax;
            if (RegisterLine(row.Name, name, text, rowHeight, nameWidth, edited, valueMin, valueMax))
            {
                if (const auto parsed = ParseHex(edited))
                {
                    values.*row.Field = info.Is64Bit ? *parsed : (*parsed & 0xFFFFFFFFull);
                    changed = true;
                }
                else
                {
                    m_engine.Notify(LogLevel::Error, "\"" + edited + "\" is not a hex number.");
                }
            }
            if (valueMax.x > valueMin.x)
            {
                if (condition.UsesCounter && row.Field == &Disasm::RegisterValues::Rcx)
                    draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ManifoldTheme::Accent), ImGui::GetStyle().FrameRounding);
                const bool differs = m_havePreviousRegisters && m_previousRegisters.*row.Field != value;
                draw->AddText(valueMin, differs ? changedColor : textColor, text.c_str());
                const std::string described = m_engine.Describe(value);
                const float used = ImGui::CalcTextSize(text.c_str()).x + Px(10.0f);
                if (described != Hex(value) && valueMax.x - valueMin.x > used + Px(30.0f))
                    ClippedText(draw, ImVec2(valueMin.x + used, valueMin.y), valueMax.x - valueMin.x - used, ImGui::GetColorU32(ManifoldTheme::AddressText), described);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s = %s", name.c_str(), Hex(value).c_str());
                    ImGui::TextColored(ManifoldTheme::Muted, "%llu  ·  %lld", static_cast<unsigned long long>(value), static_cast<long long>(value));
                    if (described != Hex(value))
                        ImGui::TextColored(ManifoldTheme::AddressText, "%s", described.c_str());
                    if (m_havePreviousRegisters && m_previousRegisters.*row.Field != value)
                        ImGui::TextColored(ManifoldTheme::Changed, "was %s", Hex(m_previousRegisters.*row.Field, digits).c_str());
                    if (condition.UsesCounter && row.Field == &Disasm::RegisterValues::Rcx)
                        ImGui::TextColored(ManifoldTheme::Accent, "%s at %s reads this register", m_conditionInstruction.Mnemonic.c_str(), info.Is64Bit ? "RIP" : "EIP");
                    ImGui::TextColored(ManifoldTheme::Muted, "%s", EditHint());
                    ImGui::EndTooltip();
                }
            }
            if (ImGui::BeginPopupContextItem("##regmenu"))
            {
                if (ImGui::MenuItem("Change value", nullptr, false, m_registersEditable))
                {
                    m_registerEdit = row.Name;
                    m_registerEditText = text;
                    m_registerEditFocus = true;
                }
                if (ImGui::MenuItem("Show in hex view"))
                {
                    m_hexTop = value & ~0xFull;
                    m_hexSelected = value;
                }
                if (ImGui::MenuItem("Show in disassembler"))
                    Navigate(value);
                if (ImGui::MenuItem("Highlight in the disassembler"))
                    SetHighlight(Lower(name));
                if (ImGui::MenuItem("Copy"))
                    ImGui::SetClipboardText(Hex(value).c_str());
                ImGui::Separator();
                RegisterViewItems();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (changed && m_registersEditable)
            m_engine.Debugger().SetRegisters(values);
    }

    const char* MemoryViewer::EditHint() const
    {
        if (m_registersRunning)
            return "The thread is running a step";
        if (!m_engine.Options().RegisterEditing)
            return "Read only: View > Registers > Allow changing registers";
        return "Double-click to change it";
    }

    void MemoryViewer::FlagCells(const Debug::BreakInfo& info, const Disasm::ConditionInfo& condition)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float line = ImGui::GetTextLineHeight();
        const ImU32 textColor = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 changedColor = ImGui::GetColorU32(ManifoldTheme::Changed);
        const ImU32 mutedColor = ImGui::GetColorU32(ManifoldTheme::Muted);
        Disasm::RegisterValues values = info.Registers;
        bool changed = false;
        ImGui::Dummy(ImVec2(0.0f, Px(2.0f)));
        const float available = ImGui::GetContentRegionAvail().x;
        const float spacing = Px(4.0f);
        const float cell = std::floor((available - spacing * 3.0f) / 4.0f);
        const float cellHeight = std::round(line + Px(6.0f));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        for (int i = 0; i < static_cast<int>(std::size(kFlags)); ++i)
        {
            const FlagBit& flag = kFlags[i];
            const bool trap = flag.Bit == 8;
            const ImVec2 min(origin.x + static_cast<float>(i % 4) * (cell + spacing), origin.y + static_cast<float>(i / 4) * (cellHeight + spacing));
            const ImVec2 max(min.x + cell, min.y + cellHeight);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(flag.Name);
            ImGui::InvisibleButton("##flag", ImVec2(cell, cellHeight));
            const bool hovered = ImGui::IsItemHovered();
            const bool canChange = m_registersEditable && !trap;
            const bool toggled = canChange && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            const bool set = (values.Flags >> flag.Bit) & 1;
            bool menuToggle = false;
            if (ImGui::BeginPopupContextItem("##flagmenu"))
            {
                if (ImGui::MenuItem(set ? "Switch to 0" : "Switch to 1", nullptr, false, canChange))
                    menuToggle = true;
                ImGui::Separator();
                RegisterViewItems();
                ImGui::EndPopup();
            }
            ImGui::PopID();
            const bool differs = m_havePreviousRegisters && (((m_previousRegisters.Flags ^ values.Flags) >> flag.Bit) & 1);
            const bool read = (condition.FlagsRead >> flag.Bit) & 1u;
            const ImVec4 fill = set ? ManifoldTheme::WithAlpha(ManifoldTheme::Accent, hovered ? 0.34f : 0.24f) : ImGui::GetStyleColorVec4(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
            draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), ImGui::GetStyle().FrameRounding);
            if (read)
                draw->AddRect(min, max, ImGui::GetColorU32(ManifoldTheme::Accent), ImGui::GetStyle().FrameRounding, 0, Px(1.5f));
            const std::string label = std::string(flag.Name) + " " + (set ? "1" : "0");
            const ImVec2 size = ImGui::CalcTextSize(label.c_str());
            draw->AddText(ImVec2(std::round(min.x + (cell - size.x) * 0.5f), std::round(min.y + (cellHeight - size.y) * 0.5f)),
                differs ? changedColor : set ? textColor : mutedColor, label.c_str());
            if (hovered)
            {
                const FlagHelp& help = kFlagHelp[i];
                PopMono();
                ImGui::BeginTooltip();
                ImGui::Text("%s  ·  %s", flag.Name, help.Title);
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
                ImGui::TextUnformatted(help.Meaning);
                if (help.Readers[0])
                    ImGui::TextColored(ManifoldTheme::Muted, "Read by %s", help.Readers);
                ImGui::PopTextWrapPos();
                ImGui::Separator();
                if (differs)
                    ImGui::TextColored(ManifoldTheme::Changed, "Now %d, was %d at the last stop", set ? 1 : 0, set ? 0 : 1);
                else
                    ImGui::Text("Now %d", set ? 1 : 0);
                if (read)
                    ImGui::TextColored(ManifoldTheme::Accent, "%s at %s reads it: %s", m_conditionInstruction.Mnemonic.c_str(), info.Is64Bit ? "RIP" : "EIP",
                        condition.Taken ? (condition.Kind == Disasm::ConditionKind::Jump ? "it jumps" : condition.Kind == Disasm::ConditionKind::Move ? "it moves" : "it sets 1")
                                        : (condition.Kind == Disasm::ConditionKind::Jump ? "it does not jump" : condition.Kind == Disasm::ConditionKind::Move ? "it does not move" : "it sets 0"));
                ImGui::TextColored(ManifoldTheme::Muted, "%s", trap ? "The debugger owns this flag for its steps, so it is not changed here." : EditHint());
                ImGui::EndTooltip();
                PushMono();
            }
            if ((toggled || menuToggle) && canChange)
            {
                values.Flags ^= 1ull << flag.Bit;
                changed = true;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (cellHeight + spacing) * 2.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        if (changed)
            m_engine.Debugger().SetRegisters(values);
    }

    void MemoryViewer::VectorSection(const Debug::BreakInfo& info, float rowHeight, float nameWidth)
    {
        Settings& options = m_engine.Options();
        const int format = std::clamp(options.VectorFormat, 0, 3);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 textColor = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 changedColor = ImGui::GetColorU32(ManifoldTheme::Changed);
        const ImU32 mutedColor = ImGui::GetColorU32(ManifoldTheme::Muted);
        const Debug::VectorRegisters& vectors = info.Vectors;
        const bool comparable = m_havePreviousRegisters && m_previousVectors.Count == vectors.Count;
        Debug::VectorRegisters newVectors = vectors;
        bool vectorsChanged = false;
        const int lanes = VectorLanes(format);
        for (int i = 0; i <= vectors.Count; ++i)
        {
            const bool mxcsr = i == vectors.Count;
            const std::string name = mxcsr ? "MXCSR" : "XMM" + std::to_string(i);
            const std::string id = "##" + name;
            const auto& xmm = vectors.Xmm[static_cast<std::size_t>(mxcsr ? 0 : i)];
            const std::string editText = mxcsr ? Hex(vectors.Mxcsr, 8) : VectorText(xmm, format);
            ImGui::PushID(name.c_str());
            std::string edited;
            ImVec2 valueMin;
            ImVec2 valueMax;
            if (RegisterLine(id, name, editText, rowHeight, nameWidth, edited, valueMin, valueMax))
            {
                if (mxcsr)
                {
                    if (const auto parsed = ParseHex(edited); parsed && *parsed <= 0xFFFFFFFFull)
                    {
                        newVectors.Mxcsr = static_cast<std::uint32_t>(*parsed);
                        vectorsChanged = true;
                    }
                    else
                    {
                        m_engine.Notify(LogLevel::Error, "\"" + edited + "\" is not a 32-bit hex number.");
                    }
                }
                else if (const auto parsed = ParseVector(edited, format))
                {
                    newVectors.Xmm[static_cast<std::size_t>(i)] = *parsed;
                    vectorsChanged = true;
                }
                else
                {
                    m_engine.Notify(LogLevel::Error, "\"" + edited + "\" does not give " + std::to_string(lanes) + (lanes == 1 ? " 128-bit hex value." : " " + std::string(kVectorFormats[static_cast<std::size_t>(format)]) + " values."));
                }
            }
            if (valueMax.x > valueMin.x)
            {
                if (mxcsr)
                {
                    const bool differs = comparable && m_previousVectors.Mxcsr != vectors.Mxcsr;
                    const std::string text = Hex(vectors.Mxcsr, 8);
                    draw->AddText(valueMin, differs ? changedColor : textColor, text.c_str());
                    const std::string raised = RaisedExceptions(vectors.Mxcsr);
                    const std::string note = std::string("RC ") + kRoundingNames[(vectors.Mxcsr >> 13) & 3] + ((vectors.Mxcsr & 0x8000) ? "  FZ" : "") +
                                             ((vectors.Mxcsr & 0x40) ? "  DAZ" : "") + (raised.empty() ? "" : "  " + raised);
                    const float used = ImGui::CalcTextSize(text.c_str()).x + Px(10.0f);
                    ClippedText(draw, ImVec2(valueMin.x + used, valueMin.y), valueMax.x - valueMin.x - used, raised.empty() ? mutedColor : ImGui::GetColorU32(ManifoldTheme::Warning), note);
                }
                else
                {
                    const bool zero = std::all_of(xmm.begin(), xmm.end(), [](std::uint8_t byte) { return byte == 0; });
                    const float laneWidth = std::floor((valueMax.x - valueMin.x) / static_cast<float>(lanes));
                    for (int lane = 0; lane < lanes; ++lane)
                    {
                        const bool differs = comparable && LaneDiffers(xmm, m_previousVectors.Xmm[static_cast<std::size_t>(i)], format, lane);
                        std::string text = LaneText(xmm, format, lane, format == 1 ? 10 : 7);
                        for (int fewer = format == 1 ? 8 : 5; fewer >= 2 && format < 2 && ImGui::CalcTextSize(text.c_str()).x > laneWidth - Px(8.0f); fewer -= 2)
                            text = LaneText(xmm, format, lane, fewer);
                        const float textWidth = ImGui::CalcTextSize(text.c_str()).x;
                        const float x = format == 3 ? valueMin.x + laneWidth * static_cast<float>(lane)
                                                    : valueMin.x + laneWidth * static_cast<float>(lane + 1) - Px(8.0f) - std::min(textWidth, laneWidth - Px(8.0f));
                        ClippedText(draw, ImVec2(x, valueMin.y), laneWidth - Px(6.0f), differs ? changedColor : zero ? mutedColor : textColor, text);
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    if (mxcsr)
                    {
                        ImGui::Text("MXCSR = %s", Hex(vectors.Mxcsr, 8).c_str());
                        ImGui::TextColored(ManifoldTheme::Muted, "Rounding %s%s%s", kRoundingNames[(vectors.Mxcsr >> 13) & 3], (vectors.Mxcsr & 0x8000) ? ", flush to zero" : "",
                            (vectors.Mxcsr & 0x40) ? ", denormals are zero" : "");
                        for (int bit = 0; bit < 6; ++bit)
                        {
                            const bool raised = (vectors.Mxcsr >> bit) & 1;
                            const bool masked = (vectors.Mxcsr >> (bit + 7)) & 1;
                            ImGui::TextColored(raised ? ManifoldTheme::Warning : ManifoldTheme::Muted, "%s %-18s %s, %s", kExceptionBits[bit], kExceptionNames[bit], raised ? "raised" : "clear",
                                masked ? "masked" : "unmasked");
                        }
                    }
                    else
                    {
                        ImGui::Text("%s", name.c_str());
                        for (int each = 0; each < static_cast<int>(kVectorFormats.size()); ++each)
                        {
                            std::string all;
                            for (int lane = 0; lane < VectorLanes(each); ++lane)
                                all += (lane ? "   " : "") + LaneText(xmm, each, lane, each == 1 ? 17 : 9);
                            ImGui::TextColored(ManifoldTheme::Muted, "%-6s", kVectorFormats[static_cast<std::size_t>(each)]);
                            ImGui::SameLine();
                            ImGui::TextUnformatted(all.c_str());
                        }
                        ImGui::TextColored(ManifoldTheme::Muted, "Lane 0 first; changed lanes are marked.");
                    }
                    ImGui::TextColored(ManifoldTheme::Muted, "%s", EditHint());
                    ImGui::EndTooltip();
                }
            }
            if (ImGui::BeginPopupContextItem("##xmmmenu"))
            {
                if (ImGui::MenuItem("Change value", nullptr, false, m_registersEditable))
                {
                    m_registerEdit = id;
                    m_registerEditText = editText;
                    m_registerEditFocus = true;
                }
                if (ImGui::MenuItem("Copy"))
                    ImGui::SetClipboardText(editText.c_str());
                if (!mxcsr && ImGui::MenuItem("Set to zero", nullptr, false, m_registersEditable))
                {
                    newVectors.Xmm[static_cast<std::size_t>(i)] = {};
                    vectorsChanged = true;
                }
                if (!mxcsr && ImGui::MenuItem("Highlight in the disassembler"))
                    SetHighlight(Lower(name));
                ImGui::Separator();
                RegisterViewItems();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (vectorsChanged && m_registersEditable)
            m_engine.Debugger().SetVectors(newVectors);
    }

    void MemoryViewer::FpuSection(const Debug::BreakInfo& info, float rowHeight, float nameWidth)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 textColor = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 changedColor = ImGui::GetColorU32(ManifoldTheme::Changed);
        const ImU32 mutedColor = ImGui::GetColorU32(ManifoldTheme::Muted);
        const Debug::FpuRegisters& fpu = info.Fpu;
        const bool comparable = m_havePreviousRegisters && m_previousFpu.Valid;
        Debug::FpuRegisters changedFpu = fpu;
        bool fpuChanged = false;
        for (int st = 0; st < 8; ++st)
        {
            const std::string name = "ST" + std::to_string(st);
            const std::string id = "##" + name;
            const bool used = fpu.Used(st);
            const std::uint8_t* raw = fpu.Stack(st);
            const std::string value = Debug::ExtendedText(raw);
            ImGui::PushID(name.c_str());
            std::string edited;
            ImVec2 valueMin;
            ImVec2 valueMax;
            if (RegisterLine(id, name, used ? value : "0", rowHeight, nameWidth, edited, valueMin, valueMax))
            {
                if (const auto parsed = ParseDouble(Trim(edited)))
                {
                    Debug::DoubleToExtended(*parsed, changedFpu.Stack(st));
                    changedFpu.SetTags(static_cast<std::uint8_t>(changedFpu.Tags() | (1u << ((fpu.Top() + st) & 7))));
                    fpuChanged = true;
                }
                else
                {
                    m_engine.Notify(LogLevel::Error, "\"" + edited + "\" is not a number.");
                }
            }
            if (valueMax.x > valueMin.x)
            {
                const bool differs = comparable && (std::memcmp(raw, m_previousFpu.Stack(st), 10) != 0 || m_previousFpu.Used(st) != used);
                if (used)
                    ClippedText(draw, valueMin, valueMax.x - valueMin.x - Px(56.0f), differs ? changedColor : textColor, value);
                const char* tag = TagName(raw, used);
                const float tagWidth = ImGui::CalcTextSize(tag).x;
                draw->AddText(ImVec2(valueMax.x - tagWidth, valueMin.y), used ? mutedColor : ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.6f)), tag);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s = %s", name.c_str(), used ? value.c_str() : "empty");
                    ImGui::TextColored(ManifoldTheme::Muted, "R%d (physical), tag %s", (fpu.Top() + st) & 7, tag);
                    ImGui::TextColored(ManifoldTheme::Muted, "80-bit %s", ExtendedHex(raw).c_str());
                    std::uint64_t mantissa = 0;
                    std::memcpy(&mantissa, raw, 8);
                    ImGui::TextColored(ManifoldTheme::Muted, "MM%d %s", (fpu.Top() + st) & 7, Hex(mantissa, 16).c_str());
                    ImGui::TextColored(ManifoldTheme::Muted, "%s", EditHint());
                    ImGui::EndTooltip();
                }
            }
            if (ImGui::BeginPopupContextItem("##fpumenu"))
            {
                if (ImGui::MenuItem("Change value", nullptr, false, m_registersEditable))
                {
                    m_registerEdit = id;
                    m_registerEditText = used ? value : "0";
                    m_registerEditFocus = true;
                }
                if (ImGui::MenuItem("Copy"))
                    ImGui::SetClipboardText(value.c_str());
                if (ImGui::MenuItem("Copy the 80 bits"))
                    ImGui::SetClipboardText(ExtendedHex(raw).c_str());
                ImGui::Separator();
                RegisterViewItems();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        const struct { const char* Name; std::uint16_t Value; std::uint16_t Before; int Digits; } words[] = {
            { "FCW", fpu.Control(), m_previousFpu.Control(), 4 }, { "FSW", fpu.Status(), m_previousFpu.Status(), 4 }, { "FTW", fpu.Tags(), m_previousFpu.Tags(), 2 } };
        for (const auto& word : words)
        {
            ImGui::PushID(word.Name);
            const std::string id = std::string("##") + word.Name;
            const std::string text = Hex(word.Value, word.Digits);
            std::string edited;
            ImVec2 valueMin;
            ImVec2 valueMax;
            if (RegisterLine(id, word.Name, text, rowHeight, nameWidth, edited, valueMin, valueMax))
            {
                const auto parsed = ParseHex(Trim(edited));
                if (parsed && *parsed <= (word.Digits == 2 ? 0xFFull : 0xFFFFull))
                {
                    const auto bits = static_cast<std::uint16_t>(*parsed);
                    if (word.Name[1] == 'C')
                        changedFpu.SetControl(bits);
                    else if (word.Name[1] == 'S')
                        changedFpu.SetStatus(bits);
                    else
                        changedFpu.SetTags(static_cast<std::uint8_t>(bits));
                    fpuChanged = true;
                }
                else
                {
                    m_engine.Notify(LogLevel::Error, "\"" + edited + "\" is not a " + (word.Digits == 2 ? "8" : "16") + "-bit hex number.");
                }
            }
            if (valueMax.x > valueMin.x)
            {
                const bool differs = comparable && word.Value != word.Before;
                draw->AddText(valueMin, differs ? changedColor : textColor, text.c_str());
                std::string note;
                ImU32 noteColor = mutedColor;
                if (word.Name[1] == 'C')
                    note = std::string(kPrecisionNames[(word.Value >> 8) & 3]) + ", " + kRoundingNames[(word.Value >> 10) & 3];
                else if (word.Name[1] == 'S')
                {
                    const std::string raised = RaisedExceptions(word.Value);
                    note = "TOP " + std::to_string((word.Value >> 11) & 7) + Format("  C3-C0 %d%d%d%d", (word.Value >> 14) & 1, (word.Value >> 10) & 1, (word.Value >> 9) & 1, (word.Value >> 8) & 1) +
                           ((word.Value & 0x40) ? "  SF" : "") + (raised.empty() ? "" : "  " + raised);
                    if (!raised.empty() || (word.Value & 0x40))
                        noteColor = ImGui::GetColorU32(ManifoldTheme::Warning);
                }
                else
                    note = "bit n: Rn holds a value";
                const float used = ImGui::CalcTextSize(text.c_str()).x + Px(10.0f);
                ClippedText(draw, ImVec2(valueMin.x + used, valueMin.y), valueMax.x - valueMin.x - used, noteColor, note);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    if (word.Name[1] == 'C')
                    {
                        ImGui::Text("Control word %s", text.c_str());
                        ImGui::TextColored(ManifoldTheme::Muted, "Precision %s, rounding %s", kPrecisionNames[(word.Value >> 8) & 3], kRoundingNames[(word.Value >> 10) & 3]);
                        for (int bit = 0; bit < 6; ++bit)
                            ImGui::TextColored(ManifoldTheme::Muted, "%s %-18s %s", kExceptionBits[bit], kExceptionNames[bit], ((word.Value >> bit) & 1) ? "masked" : "unmasked");
                    }
                    else if (word.Name[1] == 'S')
                    {
                        ImGui::Text("Status word %s", text.c_str());
                        ImGui::TextColored(ManifoldTheme::Muted, "Top of stack R%d, condition codes C3 %d C2 %d C1 %d C0 %d", (word.Value >> 11) & 7, (word.Value >> 14) & 1,
                            (word.Value >> 10) & 1, (word.Value >> 9) & 1, (word.Value >> 8) & 1);
                        for (int bit = 0; bit < 6; ++bit)
                            ImGui::TextColored(((word.Value >> bit) & 1) ? ManifoldTheme::Warning : ManifoldTheme::Muted, "%s %-18s %s", kExceptionBits[bit], kExceptionNames[bit],
                                ((word.Value >> bit) & 1) ? "raised" : "clear");
                        ImGui::TextColored(((word.Value >> 6) & 1) ? ManifoldTheme::Warning : ManifoldTheme::Muted, "SF stack fault %s", ((word.Value >> 6) & 1) ? "raised" : "clear");
                    }
                    else
                    {
                        ImGui::Text("Tag word %s (FXSAVE form)", text.c_str());
                        ImGui::TextColored(ManifoldTheme::Muted, "Bit n is 1 while the physical register Rn holds a value.");
                    }
                    if (fpu.Opcode())
                        ImGui::TextColored(ManifoldTheme::Muted, "Last x87 opcode %s", Hex(0xD800u | (fpu.Opcode() & 0x7FFu), 4).c_str());
                    ImGui::TextColored(ManifoldTheme::Muted, "%s", EditHint());
                    ImGui::EndTooltip();
                }
            }
            if (ImGui::BeginPopupContextItem("##wordmenu"))
            {
                if (ImGui::MenuItem("Change value", nullptr, false, m_registersEditable))
                {
                    m_registerEdit = id;
                    m_registerEditText = text;
                    m_registerEditFocus = true;
                }
                if (ImGui::MenuItem("Copy"))
                    ImGui::SetClipboardText(text.c_str());
                ImGui::Separator();
                RegisterViewItems();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (fpuChanged && m_registersEditable)
            m_engine.Debugger().SetFpu(changedFpu);
    }

    namespace
    {
        bool Printable(std::uint8_t c)
        {
            return c >= 0x20 && c < 0x7F;
        }

        std::string AsciiAt(const std::uint8_t* bytes, std::size_t size)
        {
            std::size_t length = 0;
            while (length < size && Printable(bytes[length]))
                ++length;
            if (length < 4 || (length < size && bytes[length] != 0 && length < 8))
                return {};
            std::string text(reinterpret_cast<const char*>(bytes), std::min<std::size_t>(length, 40));
            return "\"" + text + (length > 40 ? "...\"" : "\"");
        }

        std::string WideAt(const std::uint8_t* bytes, std::size_t size)
        {
            std::size_t length = 0;
            while (length * 2 + 1 < size && Printable(bytes[length * 2]) && bytes[length * 2 + 1] == 0)
                ++length;
            if (length < 3)
                return {};
            std::string text;
            for (std::size_t i = 0; i < std::min<std::size_t>(length, 40); ++i)
                text += static_cast<char>(bytes[i * 2]);
            return "L\"" + text + (length > 40 ? "...\"" : "\"");
        }

        bool PlausibleFloat(std::uint32_t bits)
        {
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            const float magnitude = std::fabs(value);
            return std::isfinite(value) && magnitude >= 1e-4f && magnitude <= 1e7f && (bits & 0xFFF) == 0;
        }
    }

    MemoryViewer::StackNote MemoryViewer::NoteFor(std::uint64_t value, bool is64Bit)
    {
        StackNote note;
        const Target::Process& process = *m_engine.Process();
        const int pointer = is64Bit ? 8 : 4;
        const auto region = value >= 0x10000 ? process.RegionAt(value) : std::nullopt;
        if (region && region->Committed() && region->Executable())
        {
            note.Kind = StackNote::Code;
            std::uint8_t bytes[16] = {};
            const std::uint64_t from = value - 7;
            if (process.Read(from, bytes, 7) == 7)
            {
                m_engine.Debugger().Unpatch(from, bytes, 7);
                for (const int length : { 5, 6, 2, 3, 7, 4 })
                {
                    const std::uint64_t at = value - static_cast<std::uint64_t>(length);
                    const Disasm::Instruction call = m_engine.Disassembler().Decode(at, bytes + (7 - length), static_cast<std::size_t>(length), nullptr);
                    if (call.Valid && call.Kind == Disasm::Flow::Call && call.Length == length)
                    {
                        note.Kind = StackNote::Return;
                        note.Call = at;
                        note.Target = call.Target.value_or(0);
                        break;
                    }
                }
            }
            note.Text = (note.Kind == StackNote::Return ? "return to " : "code ") + m_engine.Describe(value);
            if (note.Kind == StackNote::Return && note.Target)
                note.Text += " from " + m_engine.Describe(note.Target);
            return note;
        }
        if (region && region->Committed() && region->Readable() && !region->Guarded())
        {
            note.Kind = StackNote::Pointer;
            const std::string named = m_engine.Describe(value);
            std::uint8_t bytes[64] = {};
            const std::size_t got = process.Read(value, bytes, sizeof(bytes));
            std::string what;
            if (got >= 8)
            {
                what = AsciiAt(bytes, got);
                if (what.empty())
                    what = WideAt(bytes, got);
            }
            if (what.empty() && got >= static_cast<std::size_t>(pointer))
            {
                std::uint64_t inner = 0;
                std::memcpy(&inner, bytes, static_cast<std::size_t>(pointer));
                const std::string innerName = m_engine.Describe(inner);
                what = "-> " + Hex(inner) + (innerName != Hex(inner) ? "  " + innerName : std::string());
            }
            note.Text = (named != Hex(value) ? named + "  " : std::string()) + what;
            return note;
        }
        const std::uint32_t low = static_cast<std::uint32_t>(value);
        if (value != 0 && (value >> 32) == 0 && low <= 1000000)
            note.Text = std::to_string(low);
        else if (value != 0 && (value >> 32) == 0 && PlausibleFloat(low))
        {
            float number = 0.0f;
            std::memcpy(&number, &low, sizeof(number));
            note.Text = "float " + Format("%g", number);
        }
        else if (static_cast<std::int64_t>(value) < 0 && static_cast<std::int64_t>(value) > -1000000)
            note.Text = std::to_string(static_cast<std::int64_t>(value));
        note.Kind = StackNote::Plain;
        return note;
    }

    void MemoryViewer::StackViewItems()
    {
        Settings& options = m_engine.Options();
        const char* const offsets[] = { "Offsets from RSP", "Offsets from RBP", "Addresses" };
        for (int i = 0; i < 3; ++i)
        {
            if (ImGui::MenuItem(offsets[i], nullptr, options.StackOffsets == i))
            {
                options.StackOffsets = i;
                m_engine.SaveOptionsSoon();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Describe values", nullptr, &options.StackFollowPointers))
            m_engine.SaveOptionsSoon();
        ItemTooltip("Return addresses with the call they come from, code and symbol names, strings and what a pointer points to.");
        if (ImGui::MenuItem("Only return addresses (call stack)", nullptr, &m_stackCallsOnly))
            m_stackSerial = 0;
        ItemTooltip("Leaves the slots that hold a return address: the functions this thread is in, the innermost first. Found by the call in front of each address, so a stale one can show up.");
    }

    void MemoryViewer::Stack(float width, float height, const Debug::BreakInfo& info, bool running)
    {
        Settings& options = m_engine.Options();
        const Disasm::RegisterValues& values = info.Registers;
        const int pointer = info.Is64Bit ? 8 : 4;
        const char* sp = info.Is64Bit ? "RSP" : "ESP";
        const char* bp = info.Is64Bit ? "RBP" : "EBP";
        BeginCard("##stack", nullptr, ImVec2(width, height));
        CardTitle(ICON_FA_LAYER_GROUP, m_stackCallsOnly ? "Call stack" : "Stack", std::string(sp) + " " + Hex(values.Rsp, pointer * 2) + (running ? "  ·  stepping..." : ""));
        if (ImGui::BeginPopupContextWindow("##stackpanel", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            StackViewItems();
            ImGui::EndPopup();
        }
        constexpr int kEntries = 2048;
        if (info.Serial != m_stackPreviousSerial)
        {
            m_stackPreviousSerial = info.Serial;
            m_stackPrevious = m_stackShown;
            m_stackPreviousTop = m_stackShownTop;
        }
        if (info.Serial != m_stackSerial || values.Rsp != m_stackTop || m_stackWrites != m_engine.Writes().Revision())
        {
            m_stackSerial = info.Serial;
            m_stackTop = values.Rsp;
            m_stackWrites = m_engine.Writes().Revision();
            m_stackData.assign(static_cast<std::size_t>(kEntries * pointer), 0);
            const std::size_t got = m_engine.Process()->Read(values.Rsp, m_stackData.data(), m_stackData.size());
            m_stackData.resize(got - got % static_cast<std::size_t>(pointer));
            m_stackNotes.clear();
            m_stackRows.clear();
            const int entries = static_cast<int>(m_stackData.size() / static_cast<std::size_t>(pointer));
            for (int i = 0; i < entries; ++i)
                m_stackRows.push_back(i);
            if (m_stackCallsOnly)
            {
                std::vector<int> calls;
                for (int i = 0; i < entries; ++i)
                {
                    std::uint64_t value = 0;
                    std::memcpy(&value, m_stackData.data() + static_cast<std::size_t>(i * pointer), static_cast<std::size_t>(pointer));
                    auto found = m_stackNotes.find(i);
                    if (found == m_stackNotes.end())
                        found = m_stackNotes.emplace(i, NoteFor(value, info.Is64Bit)).first;
                    if (found->second.Kind == StackNote::Return)
                        calls.push_back(i);
                }
                m_stackRows = std::move(calls);
            }
        }
        PushMono();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float line = ImGui::GetTextLineHeight();
        const float rowHeight = std::round(line + Px(5.0f));
        const int offsetMode = std::clamp(options.StackOffsets, 0, 2);
        const float offsetWidth = offsetMode == 2 ? ImGui::CalcTextSize(std::string(static_cast<std::size_t>(pointer * 2), '0').c_str()).x + Px(12.0f)
                                                  : ImGui::CalcTextSize("RSP+0000").x + Px(12.0f);
        const float markWidth = Px(14.0f);
        const ImU32 textColor = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 mutedColor = ImGui::GetColorU32(ManifoldTheme::Muted);
        const ImU32 registerColor = ImGui::GetColorU32(ManifoldTheme::CodeRegister);
        const ImU32 symbolColor = ImGui::GetColorU32(ManifoldTheme::AddressText);
        const ImU32 returnColor = ImGui::GetColorU32(ManifoldTheme::FlowReturn);
        const ImU32 changedColor = ImGui::GetColorU32(ManifoldTheme::Changed);
        if (m_stackRows.empty())
        {
            const std::string empty = m_stackData.empty() ? "The stack at " + Hex(values.Rsp) + " cannot be read." : "No return addresses in the first " + std::to_string(kEntries) + " slots.";
            ImGui::TextColored(ManifoldTheme::Muted, "%s", empty.c_str());
        }
        const struct { const char* Name; std::uint64_t Disasm::RegisterValues::*Field; } pointers[] = {
            { "RAX", &Disasm::RegisterValues::Rax }, { "RBX", &Disasm::RegisterValues::Rbx }, { "RCX", &Disasm::RegisterValues::Rcx }, { "RDX", &Disasm::RegisterValues::Rdx },
            { "RSI", &Disasm::RegisterValues::Rsi }, { "RDI", &Disasm::RegisterValues::Rdi }, { "R8", &Disasm::RegisterValues::R8 }, { "R9", &Disasm::RegisterValues::R9 },
            { "R10", &Disasm::RegisterValues::R10 }, { "R11", &Disasm::RegisterValues::R11 }, { "R12", &Disasm::RegisterValues::R12 }, { "R13", &Disasm::RegisterValues::R13 },
            { "R14", &Disasm::RegisterValues::R14 }, { "R15", &Disasm::RegisterValues::R15 } };
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_stackRows.size()), rowHeight);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const int i = m_stackRows[static_cast<std::size_t>(row)];
                const std::uint64_t at = values.Rsp + static_cast<std::uint64_t>(i * pointer);
                std::uint64_t value = 0;
                std::memcpy(&value, m_stackData.data() + static_cast<std::size_t>(i * pointer), static_cast<std::size_t>(pointer));
                const StackNote* note = nullptr;
                if (options.StackFollowPointers || m_stackCallsOnly)
                {
                    auto found = m_stackNotes.find(i);
                    if (found == m_stackNotes.end())
                        found = m_stackNotes.emplace(i, NoteFor(value, info.Is64Bit)).first;
                    note = &found->second;
                }
                const bool isReturn = note && note->Kind == StackNote::Return;
                ImGui::PushID(i);
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const float right = start.x + ImGui::GetContentRegionAvail().x;
                const std::string editId = "stack:" + Hex(at);
                if (m_registerEdit == editId)
                {
                    ImGui::SetCursorScreenPos(ImVec2(start.x + markWidth + offsetWidth, start.y));
                    ImGui::SetNextItemWidth(right - (start.x + markWidth + offsetWidth));
                    if (m_registerEditFocus)
                    {
                        ImGui::SetKeyboardFocusHere();
                        m_registerEditFocus = false;
                    }
                    if (InputText("##stackedit", m_registerEditText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                    {
                        if (const auto parsed = m_engine.Evaluate(m_registerEditText))
                        {
                            std::uint8_t bytes[8] = {};
                            std::memcpy(bytes, &*parsed, static_cast<std::size_t>(pointer));
                            m_workspace.Report(WriteBytes(at, std::span<const std::uint8_t>(bytes, static_cast<std::size_t>(pointer)), "Stack " + Hex(at)));
                        }
                        else
                            m_engine.Notify(LogLevel::Error, "\"" + m_registerEditText + "\" is not an address or number.");
                        m_registerEdit.clear();
                    }
                    else if (ImGui::IsItemDeactivated() || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                        m_registerEdit.clear();
                    ImGui::PopID();
                    continue;
                }
                if (ImGui::Selectable("##slot", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight)) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (note && (note->Kind == StackNote::Code || note->Kind == StackNote::Return))
                        Navigate(isReturn ? note->Call : value);
                    else
                    {
                        m_hexTop = value & ~0xFull;
                        m_hexSelected = value;
                    }
                }
                const float textY = std::round(start.y + (rowHeight - line) * 0.5f);
                if (isReturn)
                {
                    draw->AddLine(ImVec2(start.x, start.y), ImVec2(right, start.y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::FlowReturn, 0.35f)), Px(1.0f));
                    draw->AddCircleFilled(ImVec2(start.x + Px(5.0f), start.y + rowHeight * 0.5f), Px(2.5f), returnColor);
                }
                std::string offset;
                ImU32 offsetColor = mutedColor;
                if (at == values.Rsp)
                {
                    offset = sp;
                    offsetColor = registerColor;
                }
                else if (at == values.Rbp)
                {
                    offset = bp;
                    offsetColor = registerColor;
                }
                else if (offsetMode == 0)
                    offset = Format("%s+%X", sp, i * pointer);
                else if (offsetMode == 1)
                    offset = at >= values.Rbp ? Format("%s+%llX", bp, static_cast<unsigned long long>(at - values.Rbp)) : Format("%s-%llX", bp, static_cast<unsigned long long>(values.Rbp - at));
                else
                    offset = Hex(at, pointer * 2);
                ClippedText(draw, ImVec2(start.x + markWidth, textY), offsetWidth - Px(4.0f), offsetColor, offset);
                const std::string text = Hex(value, pointer * 2);
                bool changed = false;
                if (m_stackPreviousTop && at >= m_stackPreviousTop)
                {
                    const std::uint64_t before = at - m_stackPreviousTop;
                    changed = before % static_cast<std::uint64_t>(pointer) == 0 && before + static_cast<std::uint64_t>(pointer) <= m_stackPrevious.size() &&
                              std::memcmp(m_stackPrevious.data() + before, &value, static_cast<std::size_t>(pointer)) != 0;
                }
                draw->AddText(ImVec2(start.x + markWidth + offsetWidth, textY), changed ? changedColor : textColor, text.c_str());
                std::string comment = note ? note->Text : std::string();
                if (!note)
                {
                    const std::string described = m_engine.Describe(value);
                    if (described != Hex(value))
                        comment = described;
                }
                std::string held;
                for (const auto& reg : pointers)
                {
                    if (!info.Is64Bit && reg.Name[1] >= '0' && reg.Name[1] <= '9')
                        continue;
                    if (values.*reg.Field != at)
                        continue;
                    const std::string name = info.Is64Bit ? std::string(reg.Name) : "E" + std::string(reg.Name + 1);
                    held += (held.empty() ? "<- " : " ") + name;
                }
                if (!held.empty())
                    comment = held + (comment.empty() ? "" : "  " + comment);
                const float used = markWidth + offsetWidth + ImGui::CalcTextSize(text.c_str()).x + Px(10.0f);
                if (!comment.empty() && right - (start.x + used) > Px(30.0f))
                    ClippedText(draw, ImVec2(start.x + used, textY), right - (start.x + used), isReturn ? returnColor : !held.empty() ? registerColor : symbolColor, comment);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(ManifoldTheme::Muted, "%s  (%s+%X)", Hex(at).c_str(), sp, i * pointer);
                    ImGui::TextUnformatted(Hex(value).c_str());
                    if (!comment.empty())
                        ImGui::TextColored(isReturn ? ManifoldTheme::FlowReturn : ManifoldTheme::AddressText, "%s", comment.c_str());
                    if (isReturn)
                        ImGui::TextColored(ManifoldTheme::Muted, "Called at %s", m_engine.Describe(note->Call).c_str());
                    ImGui::TextColored(ManifoldTheme::Muted, "%s", isReturn ? "Double-click to show the call" : note && note->Kind == StackNote::Code ? "Double-click to show the code"
                                                                                                                   : "Double-click to show the value in the hex view");
                    ImGui::EndTooltip();
                }
                if (ImGui::BeginPopupContextItem("##stackmenu"))
                {
                    if (isReturn && ImGui::MenuItem("Show the call"))
                        Navigate(note->Call);
                    if (isReturn && note->Target && ImGui::MenuItem("Show the called function"))
                        Navigate(note->Target);
                    if (ImGui::MenuItem("Show value in disassembler"))
                        Navigate(value);
                    if (ImGui::MenuItem("Show value in hex view"))
                    {
                        m_hexTop = value & ~0xFull;
                        m_hexSelected = value;
                    }
                    if (ImGui::MenuItem("Show this slot in hex view"))
                    {
                        m_hexTop = at & ~0xFull;
                        m_hexSelected = at;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Change value...", nullptr, false, !running))
                    {
                        m_registerEdit = editId;
                        m_registerEditText = Hex(value);
                        m_registerEditFocus = true;
                    }
                    if (ImGui::MenuItem("Copy value"))
                        ImGui::SetClipboardText(Hex(value).c_str());
                    if (ImGui::MenuItem("Copy address"))
                        ImGui::SetClipboardText(Hex(at).c_str());
                    if (!comment.empty() && ImGui::MenuItem("Copy the description"))
                        ImGui::SetClipboardText(comment.c_str());
                    ImGui::Separator();
                    StackViewItems();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopStyleVar();
        PopMono();
        EndCard();
        m_stackShown = m_stackData;
        m_stackShownTop = values.Rsp;
    }
}
