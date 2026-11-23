#include "UI/SignatureWindow.h"

#include "App/Commands.h"
#include "App/Engine.h"
#include "App/SignatureSettings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Target/Process.h"
#include "UI/Fonts.h"
#include "UI/SettingsWindow.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui_internal.h"

#include <algorithm>

namespace Manifold::UI
{
    namespace
    {
        ImVec4 RoleColor(const Disasm::SignatureByte& byte)
        {
            if (byte.Wild)
                return ManifoldTheme::Muted;
            switch (byte.Role)
            {
            case Disasm::ByteRole::Prefix: return ManifoldTheme::CodeKeyword;
            case Disasm::ByteRole::ModRm: return ManifoldTheme::CodeRegister;
            case Disasm::ByteRole::Address: return ManifoldTheme::AddressText;
            case Disasm::ByteRole::Branch: return ManifoldTheme::FlowJump;
            case Disasm::ByteRole::Offset:
            case Disasm::ByteRole::Immediate: return ManifoldTheme::CodeNumber;
            case Disasm::ByteRole::Data: return ManifoldTheme::BytesText;
            default: return ManifoldTheme::Text;
            }
        }

        std::string OffsetText(std::int64_t offset)
        {
            if (offset == 0)
                return "0";
            return offset < 0 ? "-" + Hex(static_cast<std::uint64_t>(-offset)) : "+" + Hex(static_cast<std::uint64_t>(offset));
        }

        // The bytes in the mono font, each in the colour of its role, wrapped at
        // the place's width (or cut with "..." on one line). Returns the byte
        // under the mouse.
        std::optional<std::size_t> ColoredBytes(const std::vector<Disasm::SignatureByte>& bytes, std::size_t first, std::size_t count, const std::string& wild, bool wrap)
        {
            ImFont* font = Fonts::Mono();
            const float size = ImGui::GetFontSize();
            const float space = font->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x;
            const float cell = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "00").x;
            const float line = ImGui::GetTextLineHeightWithSpacing();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = std::max(cell, ImGui::GetContentRegionAvail().x);
            const std::size_t end = std::min(bytes.size(), first + count);
            const float ellipsis = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "...").x;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            float x = 0.0f;
            float y = 0.0f;
            std::optional<std::size_t> hovered;
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            for (std::size_t i = first; i < end; ++i)
            {
                const std::string text = bytes[i].Wild ? wild : Hex(bytes[i].Value, 2);
                const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
                if (x > 0.0f && x + w > width)
                {
                    if (!wrap)
                    {
                        draw->AddText(font, size, ImVec2(origin.x + std::max(0.0f, std::min(x, width - ellipsis)), origin.y), ImGui::GetColorU32(ManifoldTheme::Muted), "...");
                        x = width;
                        break;
                    }
                    x = 0.0f;
                    y += line;
                }
                const ImVec2 at(origin.x + x, origin.y + y);
                draw->AddText(font, size, at, ImGui::GetColorU32(RoleColor(bytes[i])), text.c_str());
                if (mouse.x >= at.x && mouse.x < at.x + w + space && mouse.y >= at.y && mouse.y < at.y + line)
                    hovered = i;
                x += w + space;
            }
            ImGui::Dummy(ImVec2(wrap ? width : std::max(1.0f, std::min(width, x)), y + ImGui::GetTextLineHeight()));
            if (!ImGui::IsItemHovered())
                hovered.reset();
            return hovered;
        }
    }

    SignatureWindow::SignatureWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    SignatureWindow::~SignatureWindow() = default;

    void SignatureWindow::Prepare(std::uint64_t address)
    {
        Show();
        if (address)
            m_address = m_workspace.Core().Symbols().ModuleOffset(address);
        m_focus = true;
    }

    void SignatureWindow::MakeWhenReady(const std::string& expression)
    {
        Show();
        m_address = expression;
        m_pending = true;
    }

    void SignatureWindow::Make(std::uint64_t address)
    {
        Prepare(address);
        Start(address, false);
    }

    void SignatureWindow::Copy(std::uint64_t address)
    {
        if (!m_open)
            m_address = m_workspace.Core().Symbols().ModuleOffset(address);
        Start(address, true);
    }

    void SignatureWindow::Start(std::uint64_t address, bool copy)
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.Running())
            return;
        m_copyWhenDone = copy;
        m_process = engine.Process();
        const Disasm::SignatureOptions options = SignatureOptionsFrom(engine.Options());
        std::shared_ptr<Target::Process> process = m_process;
        m_task.Start(engine.Work(), "Making a signature", [process, options, address](Scan::PatternProgress& progress)
            {
                Outcome outcome;
                const Disasm::Disassembler disassembler(process->Is64Bit());
                outcome.Signature = Disasm::MakeSignature(*process, disassembler, address, options, outcome.Error, &progress);
                return outcome;
            });
    }

    void SignatureWindow::Collect()
    {
        std::optional<Outcome> outcome = m_task.Collect();
        if (!outcome)
            return;
        Engine& engine = m_workspace.Core();
        if (!outcome->Signature)
        {
            if (m_task.State().Cancel)
                return;
            m_error = outcome->Error;
            m_result.reset();
            if (m_copyWhenDone)
                engine.Notify(LogLevel::Warning, "No signature copied: " + outcome->Error);
            return;
        }
        m_error.clear();
        m_result = std::move(outcome->Signature);
        const Disasm::SignatureFormatting formatting = SignatureFormattingFrom(engine.Options());
        const std::string text = Disasm::FormatSignature(*m_result, SignatureCopyFormat(engine.Options()), formatting);
        LOG_INFO("Signature", engine.Describe(m_result->Address) + ": " + Disasm::SignatureText(m_result->Bytes, formatting) + " (" + Summary(*m_result) + ")");
        if (m_copyWhenDone)
        {
            ImGui::SetClipboardText(text.c_str());
            engine.Notify(LogLevel::Info, "Signature copied: " + std::to_string(m_result->Bytes.size()) + " bytes, unique in " + m_result->ScopeText() +
                (m_result->Offset ? ", the address at " + OffsetText(m_result->Offset) : "") + ".");
        }
    }

    std::string SignatureWindow::Summary(const Disasm::Signature& signature) const
    {
        std::string text = std::to_string(signature.Bytes.size()) + " bytes  ·  " + std::to_string(signature.Wildcards()) + " wildcards  ·  " +
            std::to_string(signature.Lines.size()) + (signature.Lines.size() == 1 ? " instruction" : " instructions") + "  ·  unique in " + signature.ScopeText();
        text += signature.Verified ? "  ·  checked by a scan" : "  ·  not confirmed by a scan";
        text += "  ·  " + Format("%.0f ms", signature.Seconds * 1000.0);
        return text;
    }

    void SignatureWindow::Options()
    {
        Engine& engine = m_workspace.Core();
        Settings& settings = engine.Options();
        bool changed = false;
        {
            Layout::Row bar("##sigoptions", { Layout::Size::Fit(), Layout::Size::Fixed(170.0f), Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fill(), Layout::Size::Fit() },
                Layout::Toolbar());
            bar.Cell([&] { Layout::Text("Unique In"); });
            bar.Cell([&] {
                changed |= Segmented("##sigscope", &settings.SignatureScope, { "Module", "Process" }, ImGui::GetContentRegionAvail().x);
                ItemTooltip("Module counts matches in the address's module, like aobscanmodule. Process counts them in all memory, like aobscan.");
            });
            bar.Cell([&] {
                changed |= ImGui::Checkbox("Code Only", &settings.SignatureCodeOnly);
                ItemTooltip("Count matches in executable memory only.");
            });
            bar.Cell([&] {
                changed |= ImGui::Checkbox("AoB Offset", &settings.SignatureOffset);
                ItemTooltip(("May start at an instruction up to " + std::to_string(settings.SignatureMaxOffset) +
                             " bytes before or after the address when that makes the signature at least " + std::to_string(settings.SignatureMinSaving) +
                             " bytes shorter. The address is then the hit plus the offset.").c_str());
            });
            bar.Skip();
            bar.Cell([&] {
                if (Layout::IconButton("##sigsettings", ICON_FA_COG, "Signature Settings"))
                    m_workspace.OpenSettings(SettingsWindow::SignaturesPage);
            });
        }
        if (changed)
            engine.SaveOptionsSoon();
    }

    void SignatureWindow::Pattern(const Disasm::Signature& signature)
    {
        const std::string wild = Disasm::WildcardToken(SignatureFormattingFrom(m_workspace.Core().Options()).Wildcard);
        const auto hovered = ColoredBytes(signature.Bytes, 0, signature.Bytes.size(), wild, true);
        if (hovered)
        {
            const Disasm::SignatureByte& byte = signature.Bytes[*hovered];
            Layout::TextTooltip(Hex(signature.Start + *hovered) + "  ·  " + Hex(byte.Value, 2) + "  ·  " + Disasm::ByteRoleName(byte.Role) + (byte.Wild ? ", wildcard" : ", kept"));
        }
    }

    void SignatureWindow::Formats(const Disasm::Signature& signature)
    {
        Engine& engine = m_workspace.Core();
        Settings& settings = engine.Options();
        const Disasm::SignatureFormatting formatting = SignatureFormattingFrom(settings);
        Layout::CardOptions card;
        card.Icon = ICON_FA_COPY;
        card.Title = "Formats";
        card.Badge = "right-click picks what Copy copies";
        Layout::Card formats("##sigformatscard", card);
        if (!formats.Visible())
            return;
        for (int index = 0; index < 4; ++index)
        {
            const auto format = static_cast<Disasm::SignatureFormat>(index);
            const std::string text = Disasm::FormatSignature(signature, format, formatting);
            const bool preferred = settings.SignatureCopyFormat == index;
            ImGui::PushID(index);
            Layout::Row row("##format", { Layout::Size::Fixed(120.0f), Layout::Size::Fill(), Layout::Size::Fit() });
            row.Cell([&] {
                Layout::Text(Disasm::SignatureFormatName(format), { .Color = preferred ? ManifoldTheme::Accent : ManifoldTheme::Label });
                if (preferred && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    Layout::TextTooltip("Copy and Copy Signature copy this one.");
            });
            row.Cell([&] {
                Layout::Text(text, { .Font = Fonts::Mono() });
                if (ImGui::BeginPopupContextItem("##formatmenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy"))
                        ImGui::SetClipboardText(text.c_str());
                    if (ImGui::MenuItem(ICON_FA_CHECK "  Use for Copy Signature", nullptr, preferred))
                    {
                        settings.SignatureCopyFormat = index;
                        engine.SaveOptionsSoon();
                    }
                    ImGui::EndPopup();
                }
            });
            row.Cell([&] {
                if (Layout::IconButton("##copyformat", ICON_FA_COPY, (std::string("Copy as ") + Disasm::SignatureFormatName(format)).c_str()))
                    ImGui::SetClipboardText(text.c_str());
            });
            row.End();
            ImGui::PopID();
        }
        formats.End();
    }

    void SignatureWindow::Instructions(const Disasm::Signature& signature)
    {
        if (!BeginDataTable("##siglinetable", { { "Address", 0.9f }, { "Bytes", 1.0f }, { "Instruction", 1.4f } }))
            return;
        Engine& engine = m_workspace.Core();
        const std::string wild = Disasm::WildcardToken(SignatureFormattingFrom(engine.Options()).Wildcard);
        for (std::size_t row = 0; row < signature.Lines.size(); ++row)
        {
            const Disasm::SignatureLine& line = signature.Lines[row];
            const bool target = line.Address == signature.Address;
            ImGui::PushID(static_cast<int>(row));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (Layout::RowSelectable("##line", target, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                m_workspace.RevealInMemoryViewer(line.Address, line.Length, true, true);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                Layout::TextTooltip(target ? "The address the signature is for. Double-click shows it in the memory viewer." : "Double-click shows it in the memory viewer.");
            Layout::Text(engine.Symbols().ModuleOffset(line.Address) + (target ? "  " ICON_FA_ARROW_LEFT : ""), { .Color = ManifoldTheme::AddressText, .Font = Fonts::Mono() });
            ImGui::TableNextColumn();
            const std::size_t first = static_cast<std::size_t>(line.Address - signature.Start);
            ColoredBytes(signature.Bytes, first, line.Length, wild, false);
            ImGui::TableNextColumn();
            Layout::Text(line.Text, { .Font = Fonts::Mono() });
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void SignatureWindow::Candidates(const Disasm::Signature& signature)
    {
        if (!BeginDataTable("##sigstarttable", { { "Start", 1.0f }, { "Offset", 0.5f }, { "Length", 1.0f } }))
            return;
        Engine& engine = m_workspace.Core();
        for (const Disasm::SignatureCandidate& candidate : signature.Candidates)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Layout::Text(engine.Symbols().ModuleOffset(candidate.Start) + (candidate.Chosen ? "  " ICON_FA_CHECK : ""),
                { .Color = candidate.Chosen ? ManifoldTheme::Accent : ManifoldTheme::AddressText, .Font = Fonts::Mono() });
            ImGui::TableNextColumn();
            Layout::Text(OffsetText(candidate.Offset), { .Font = Fonts::Mono() });
            ImGui::TableNextColumn();
            if (candidate.Unique)
                Layout::Text(std::to_string(candidate.Length) + " bytes");
            else
                Layout::Muted(candidate.NotShorter ? "not shorter" : "not unique");
        }
        ImGui::EndTable();
    }

    void SignatureWindow::Result()
    {
        const Disasm::Signature& signature = *m_result;
        Engine& engine = m_workspace.Core();
        const std::size_t wildcards = signature.Wildcards();
        const std::string badge = std::to_string(signature.Bytes.size()) + " bytes  ·  " + std::to_string(wildcards) + (wildcards == 1 ? " wildcard" : " wildcards");
        Layout::CardOptions options;
        options.Icon = ICON_FA_FINGERPRINT;
        options.Title = "Signature";
        options.Badge = badge;
        Layout::Card card("##sigcard", options);
        if (card.Visible())
        {
            Pattern(signature);
            std::string summary = "Unique in " + signature.ScopeText() + "  ·  " + std::to_string(signature.Lines.size()) +
                (signature.Lines.size() == 1 ? " instruction" : " instructions") + "  ·  " + (signature.Verified ? "checked by a scan" : "not confirmed by a scan") +
                "  ·  " + Format("%.0f ms", signature.Seconds * 1000.0);
            Layout::Muted(summary, Layout::Overflow::Wrap);
            if (signature.Offset != 0)
            {
                const std::string label = SignatureFormattingFrom(engine.Options()).Label;
                const std::uint64_t distance = static_cast<std::uint64_t>(signature.Offset < 0 ? -signature.Offset : signature.Offset);
                Layout::Colored(std::string(ICON_FA_INFO_CIRCLE "  ") + "Starts " + Hex(distance) + (signature.Offset > 0 ? " bytes before " : " bytes after ") +
                        engine.Describe(signature.Address) + ": the address is " + label + OffsetText(signature.Offset) + ".",
                    ManifoldTheme::Accent, Layout::Overflow::Wrap);
            }
            if (signature.FunctionEnd)
                Layout::Colored(std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + "Reaches past the function end at " + engine.Describe(*signature.FunctionEnd) +
                        ", so it also describes the code placed after it.",
                    ManifoldTheme::Warning, Layout::Overflow::Wrap);
            if (!signature.Verified)
                Layout::Colored(std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + "A scan did not find exactly this one place; the memory may have changed while it was made.",
                    ManifoldTheme::Warning, Layout::Overflow::Wrap);
            Layout::Spacing(Layout::Space::S);
            const Disasm::SignatureFormat format = SignatureCopyFormat(engine.Options());
            const std::string tip = std::string("Copies it as ") + Disasm::SignatureFormatName(format) + ".";
            Layout::Row actions("##sigactions", { Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fit() }, Layout::Toolbar());
            actions.Cell([&] {
                Layout::ButtonStyle copy;
                copy.Kind = Layout::ButtonKind::Accent;
                copy.Tooltip = tip.c_str();
                if (Layout::Button(ICON_FA_COPY "  Copy", copy))
                    ImGui::SetClipboardText(Disasm::FormatSignature(signature, format, SignatureFormattingFrom(engine.Options())).c_str());
            });
            actions.Cell([&] {
                Layout::ButtonStyle find;
                find.Tooltip = "Searches memory for the pattern in Find Bytes.";
                if (Layout::Button(ICON_FA_SEARCH "  Find Bytes", find))
                    m_workspace.OpenByteSearch(Disasm::SignatureText(signature.Bytes, {}));
            });
            actions.Cell([&] {
                if (Layout::Button(ICON_FA_CODE "  Show in the Memory Viewer"))
                    m_workspace.RevealInMemoryViewer(signature.Address, 1, true, true);
            });
            actions.End();
        }
        card.End();
        Layout::Spacing(Layout::Space::S);
        Formats(signature);
        Layout::Spacing(Layout::Space::S);
        if (ImGui::BeginTabBar("##sigtabs"))
        {
            if (ImGui::BeginTabItem(("Instructions (" + std::to_string(signature.Lines.size()) + ")###siglines").c_str()))
            {
                Instructions(signature);
                ImGui::EndTabItem();
            }
            if (signature.Candidates.size() > 1 && ImGui::BeginTabItem(("Starts Tried (" + std::to_string(signature.Candidates.size()) + ")###sigstarts").c_str()))
            {
                Candidates(signature);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void SignatureWindow::Draw()
    {
        Collect();
        if (!m_open)
            return;
        if (!BeginWindow(ICON_FA_FINGERPRINT "  Signature Maker###signaturemaker", ImVec2(720.0f, 560.0f), 0, ImVec2(440.0f, 260.0f)))
            return;
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() && !m_task.Running() && !m_result)
        {
            m_workspace.NoProcessState(ICON_FA_FINGERPRINT, "Open a process to make signatures.");
            EndToolWindow();
            return;
        }
        const bool running = m_task.Running();
        // ∑ A short search (a module takes milliseconds) shows nothing at all;
        //   only one that runs longer shows the spinner and dims the old result.
        //   Both sit in places that are always there, so nothing moves.
        const bool slow = running && m_task.Elapsed() > 0.15;
        bool make = false;
        {
            Layout::Row bar("##sigaddressbar", { Layout::Size::Fill(), Layout::Size::Fixed(96.0f), Layout::Size::Fixed(20.0f) });
            bar.Cell([&] {
                Layout::FieldOptions field;
                field.Placement = Layout::LabelPlacement::Left;
                field.MaxWidth = Px(320.0f);
                Layout::Field("Address", field);
                if (m_focus)
                {
                    ImGui::SetKeyboardFocusHere();
                    m_focus = false;
                }
                make |= InputTextWithHint("##sigaddress", "address or expression", m_address, ImGuiInputTextFlags_EnterReturnsTrue);
            });
            bar.Cell([&] {
                Layout::ButtonStyle style;
                style.Width = Layout::Size::Fill();
                if (slow)
                {
                    if (Layout::Button(ICON_FA_TIMES "  Cancel##sigmake", style))
                        m_task.Cancel();
                    return;
                }
                style.Kind = Layout::ButtonKind::Accent;
                ImGui::BeginDisabled(!engine.HasProcess());
                make |= Layout::Button(ICON_FA_FINGERPRINT "  Make##sigmake", style);
                ImGui::EndDisabled();
            });
            bar.Cell([&] {
                const float side = ImGui::GetFrameHeight();
                if (!slow)
                {
                    ImGui::Dummy(ImVec2(side, side));
                    return;
                }
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float spin = ImGui::GetTextLineHeight();
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (side - spin) * 0.5f));
                Spinner(spin, ManifoldTheme::Accent);
                const Scan::PatternProgress& progress = m_task.State();
                const std::uint64_t total = std::max<std::uint64_t>(1, progress.Total.load());
                if (ImGui::IsItemHovered())
                    Layout::TextTooltip(Format("Searching, %d%%", static_cast<int>(std::min<std::uint64_t>(100, progress.Done.load() * 100 / total))));
            });
        }
        if (m_pending && engine.HasProcess() && !running)
        {
            m_pending = false;
            make = true;
        }
        if (make && !running)
        {
            Status error;
            const auto address = engine.Evaluate(Trim(m_address), &error);
            if (!address)
            {
                m_error = error.Text.empty() ? ErrorMessage(ErrorCode::ExprUnknownSymbol, Trim(m_address)) : error.Text;
                m_result.reset();
            }
            else
                Start(*address, false);
        }
        Options();
        if (!m_error.empty())
        {
            Layout::CardOptions card;
            card.Icon = ICON_FA_EXCLAMATION_TRIANGLE;
            card.Title = "No Signature";
            Layout::Card failed("##sigerror", card);
            if (failed.Visible())
                Layout::Colored(m_error, ManifoldTheme::Error, Layout::Overflow::Wrap);
            failed.End();
        }
        else if (m_result)
        {
            ImGui::BeginDisabled(slow);
            Result();
            ImGui::EndDisabled();
        }
        else if (slow)
        {
            const Scan::PatternProgress& progress = m_task.State();
            const std::uint64_t total = std::max<std::uint64_t>(1, progress.Total.load());
            EmptyState(ICON_FA_FINGERPRINT, "Searching",
                Format("Measuring how far every other place agrees with the bytes at the address: %d%%.", static_cast<int>(std::min<std::uint64_t>(100, progress.Done.load() * 100 / total))) +
                    (m_workspace.Core().Options().SignatureScope == 1 ? " All memory of the process is read, which takes a while in a large game." : ""),
                {}, false);
        }
        else
        {
            const char* keys = ShortcutText(Command::CopySignature);
            EmptyState(ICON_FA_FINGERPRINT, "No Signature Yet",
                std::string("Enter an address, or right-click an instruction in the memory viewer and pick Copy Signature") +
                    (keys && *keys ? std::string(" (") + keys + ")" : std::string()) + ". The signature grows until no other place in the module matches it.",
                {}, false);
        }
        EndToolWindow();
    }
}
