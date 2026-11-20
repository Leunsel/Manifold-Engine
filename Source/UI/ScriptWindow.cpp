#include "UI/ScriptWindow.h"

#include "App/Engine.h"
#include "App/Sounds.h"
#include "Asm/AsmAssist.h"
#include "Asm/MnemonicDocs.h"
#include "Core/Errors.h"
#include "Lua/DocRecord.h"
#include "UI/LuaWindow.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Shortcuts.h"
#include "UI/Templates.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>
#include <sstream>

namespace Manifold::UI
{
    namespace
    {
        int LineOf(const std::string& message)
        {
            const std::size_t at = message.find("Line ");
            if (at == std::string::npos)
                return 0;
            return static_cast<int>(ParseDecimal(message.substr(at + 5, message.find(':', at) - at - 5)).value_or(0));
        }

        std::shared_ptr<const Lua::DocRecord> AsmCommandRecord(std::string_view name)
        {
            const Asm::AsmCommandDoc* command = Asm::FindAsmCommand(name);
            if (!command)
            {
                const std::string summary = Asm::MnemonicSummary(name);
                if (summary.empty())
                    return nullptr;
                auto record = std::make_shared<Lua::DocRecord>();
                record->Name = Lower(name);
                record->Path = record->Name;
                record->Kind = Lua::DocItemKind::Keyword;
                record->Origin = Lua::DocOrigin::Manifold;
                record->Text = summary;
                record->Summary = summary;
                return record;
            }
            auto record = std::make_shared<Lua::DocRecord>();
            record->Name = command->Name;
            record->Path = command->Name;
            record->Kind = Lua::DocItemKind::Function;
            record->Origin = Lua::DocOrigin::CheatEngine;
            record->Text = command->Text;
            record->Summary = command->Text;
            Lua::DocSignature signature;
            for (const Asm::AsmCommandParam& param : command->Params)
            {
                Lua::DocParam entry;
                entry.Name = param.Name;
                entry.Type = param.Type;
                entry.Optional = param.Optional;
                entry.Text = param.Text;
                signature.Params.push_back(std::move(entry));
            }
            record->Signatures.push_back(std::move(signature));
            return record;
        }

        std::string HeldBackNote(bool luaHeldBack, bool passed)
        {
            if (!luaHeldBack)
                return {};
            const char* reason = "this table's scripts have not been allowed yet (run the table's Lua script or turn a script on yourself)";
            return passed ? std::string(" Its Lua parts were only compiled, not run, because ") + reason + "."
                          : std::string(" Its Lua parts were not run, because ") + reason + ", so this can be a false alarm.";
        }
    }

    ScriptWindow::ScriptWindow(Workspace& workspace, int number, std::string script, int recordId, bool lua)
        : m_workspace(workspace), m_engine(workspace.Core()), m_text(std::move(script)), m_recordId(recordId), m_lua(lua), m_templates(workspace)
    {
        m_saved = m_text;
        std::string name;
        if (recordId == kTableLuaScript)
            name = "Table Lua script";
        else if (recordId >= 0)
        {
            const Table::MemoryRecord* record = m_engine.Table().FindById(recordId);
            name = record ? record->Description : "Script";
        }
        else
            name = lua ? "Lua script" : "Auto Assembler";
        m_windowId = "###script" + std::to_string(number);
        m_title = std::string(lua ? ICON_FA_MOON "  " : ICON_FA_CODE "  ") + name + m_windowId;

        m_luaAssist = MakeLuaAssist(m_engine);
        m_asmAssist.CompleteAt = [this](const std::string& text, std::size_t cursor, std::size_t& start)
            {
                Asm::AsmAssistInput input;
                if (const auto& process = m_engine.Process())
                {
                    input.Is64Bit = process->Is64Bit();
                    if (const Target::ModuleList modules = process->Modules())
                    {
                        for (const Target::ModuleInfo& module : *modules)
                            input.Modules.push_back(module.Name);
                    }
                }
                for (const Target::UserSymbol& symbol : m_engine.Symbols().Registered())
                    input.Symbols.push_back(symbol.Name);
                start = Asm::AsmWordAt(text, cursor).Start;
                std::vector<CodeSuggestion> result;
                for (Asm::AsmCompletion& item : Asm::CompleteAsm(text, cursor, input))
                {
                    std::shared_ptr<const Lua::DocRecord> doc = item.Kind == "function" || (item.Kind == "keyword" && !item.Text.empty()) ? AsmCommandRecord(item.Name) : nullptr;
                    result.push_back({ std::move(item.Name), std::move(item.Kind), std::move(item.Signature), std::move(item.Text), std::move(doc) });
                }
                return result;
            };
        m_asmAssist.Describe = [](const std::string& chain, bool) { return AsmCommandRecord(chain); };
        m_asmAssist.Embedded = &m_luaAssist;
        m_asmAssist.UseEmbedded = [](const std::string& text, std::size_t cursor) { return Asm::InLuaBlock(text, cursor); };
        m_editor.Assist = lua ? &m_luaAssist : &m_asmAssist;
    }

    void ScriptWindow::Check()
    {
        m_shownError.clear();
        m_editor.ErrorLine = 0;
        if (m_lua)
        {
            const Status status = m_engine.Lua().Compile(m_text);
            m_resultOk = status.Ok();
            m_result = status.Ok() ? "The Lua script compiles." : status.Text;
            return;
        }
        Asm::AaMemory memory;
        if (Asm::HasSections(m_text))
        {
            bool luaHeldBack = false;
            for (const Asm::AaSection section : { Asm::AaSection::Enable, Asm::AaSection::Disable })
            {
                const ScriptCheck check = m_engine.CheckScript(m_text, section, memory);
                luaHeldBack = luaHeldBack || check.LuaHeldBack > 0;
                if (!check.Result.Ok())
                {
                    m_resultOk = false;
                    m_result = std::string(section == Asm::AaSection::Enable ? "[ENABLE] " : "[DISABLE] ") + check.Result.Error.Text + HeldBackNote(luaHeldBack, false);
                    m_editor.ErrorLine = check.Result.Line;
                    return;
                }
            }
            m_resultOk = true;
            m_result = "Both sections assemble." + HeldBackNote(luaHeldBack, true);
            return;
        }
        const ScriptCheck check = m_engine.CheckScript(m_text, Asm::AaSection::Whole, memory);
        const Asm::AaResult& result = check.Result;
        m_resultOk = result.Ok();
        m_result = (result.Ok() ? "The script assembles." : result.Error.Text) + HeldBackNote(check.LuaHeldBack > 0, result.Ok());
        m_editor.ErrorLine = result.Ok() ? 0 : (result.Line ? result.Line : LineOf(result.Error.Text));
    }

    void ScriptWindow::Run(bool disable)
    {
        m_shownError.clear();
        m_editor.ErrorLine = 0;
        if (m_lua)
        {
            const char* name = m_recordId == kTableLuaScript ? "table script" : "script";
            Lua::LuaEngine& lua = m_engine.Lua();
            if (!m_engine.Async())
            {
                const Status status = lua.Run(m_text, name);
                m_resultOk = status.Ok();
                m_result = status.Ok() ? "The script ran." : status.Text;
                if (!status)
                    Sounds::Play(Sounds::Cue::Error);
                return;
            }
            if (m_task && lua.TaskAlive(m_task))
                return;
            m_resultOk = true;
            m_result.clear();
            m_stopped = false;
            const std::weak_ptr<int> alive = m_alive;
            m_task = lua.StartTask(m_text, name, [this, alive](const Status& status, const std::string&)
                {
                    if (alive.expired())
                        return;
                    m_task = 0;
                    m_resultOk = status.Ok();
                    m_result = status.Ok() ? "The script ran." : status.Text;
                    const bool stopped = std::exchange(m_stopped, false);
                    if (!status && !stopped)
                        Sounds::Play(Sounds::Cue::Error);
                    if (stopped)
                        m_shownError = m_result;
                });
            return;
        }
        const Asm::AaSection section = Asm::HasSections(m_text) ? (disable ? Asm::AaSection::Disable : Asm::AaSection::Enable) : Asm::AaSection::Whole;
        const Asm::AaResult result = m_engine.AutoAssemble(m_text, section, m_engine.ScratchMemory(), false, "window");
        m_resultOk = result.Ok();
        if (!result.Ok())
            Sounds::Play(Sounds::Cue::Error);
        if (result.Ok())
        {
            std::size_t bytes = 0;
            for (const Asm::AaWrite& write : result.Writes)
                bytes += write.Bytes.size();
            m_result = "Done: " + std::to_string(bytes) + " bytes written in " + std::to_string(result.Writes.size()) + " blocks.";
            for (const std::string& note : result.Notes)
                m_result += "  " + note + ".";
            for (const auto& [name, address] : result.Symbols)
                m_result += "  " + name + " = " + Hex(address) + ".";
            LOG_SUCCESS("Auto Assembler", m_result);
        }
        else
        {
            m_result = result.Error.Text;
            m_editor.ErrorLine = result.Line ? result.Line : LineOf(result.Error.Text);
        }
    }

    void ScriptWindow::SaveToRecord()
    {
        m_shownError.clear();
        if (m_recordId == kTableLuaScript)
        {
            m_engine.Table().TableLuaScript = m_text;
            m_engine.Table().Touch();
            m_saved = m_text;
            return;
        }
        Table::MemoryRecord* record = m_engine.Table().FindById(m_recordId);
        if (!record)
        {
            m_resultOk = false;
            m_result = "The record no longer exists.";
            return;
        }
        if (!m_lua)
        {
            Asm::AaMemory memory;
            const ScriptCheck check = m_engine.CheckScript(m_text, Asm::HasSections(m_text) ? Asm::AaSection::Enable : Asm::AaSection::Whole, memory);
            const Asm::AaResult& result = check.Result;
            if (!result.Ok())
            {
                m_resultOk = false;
                m_result = "Saved, but it does not assemble: " + result.Error.Text + HeldBackNote(check.LuaHeldBack > 0, false);
                m_editor.ErrorLine = result.Line;
            }
            else
            {
                m_resultOk = true;
                m_result = record->Active ? "Saved. The script is active; the change counts from the next switch." : "Saved.";
            }
        }
        record->Script = m_text;
        m_engine.Table().Touch();
        m_saved = m_text;
    }

    void ScriptWindow::OpenFile()
    {
        const std::vector<FileFilter> filters = m_lua ? std::vector<FileFilter>{ { L"Lua scripts (*.lua)", L"*.lua" }, { L"All files", L"*.*" } }
                                                      : std::vector<FileFilter>{ { L"Auto Assembler scripts (*.cea)", L"*.cea" }, { L"All files", L"*.*" } };
        if (const auto path = m_workspace.OpenFileDialog(filters))
        {
            std::ifstream file(*path, std::ios::binary);
            std::stringstream content;
            content << file.rdbuf();
            m_text = content.str();
            m_path = *path;
        }
    }

    void ScriptWindow::SaveFile(bool askName)
    {
        m_shownError.clear();
        std::optional<std::filesystem::path> path;
        if (!askName && !m_path.empty())
            path = m_path;
        else
            path = m_lua ? m_workspace.SaveFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" } }, "script.lua", L"lua")
                         : m_workspace.SaveFileDialog({ { L"Auto Assembler scripts (*.cea)", L"*.cea" } }, "script.cea", L"cea");
        if (!path)
            return;
        std::ofstream file(*path, std::ios::binary | std::ios::trunc);
        file.write(m_text.data(), static_cast<std::streamsize>(m_text.size()));
        m_resultOk = static_cast<bool>(file);
        m_result = file ? "Saved to " + WideToUtf8(path->filename().wstring()) + "." : "Could not write " + WideToUtf8(path->wstring()) + ".";
        if (file)
            m_path = *path;
    }

    void ScriptWindow::AssignToTable()
    {
        Table::MemoryRecord& record = m_engine.Table().AddScript("Auto Assemble script", m_text);
        m_recordId = record.Id;
        m_saved = m_text;
        m_resultOk = true;
        m_result = "Added to the address list.";
    }

    void ScriptWindow::RunCommand(Command command)
    {
        switch (command)
        {
        case Command::ScriptNewWindow: m_workspace.OpenScript(m_lua ? std::string() : Asm::CheatTableTemplate(), kNoRecord, m_lua); break;
        case Command::ScriptOpen: OpenFile(); break;
        case Command::ScriptSave:
            if (m_recordId != kNoRecord)
                SaveToRecord();
            else
                SaveFile(false);
            break;
        case Command::ScriptSaveAs: SaveFile(true); break;
        case Command::ScriptCheck: Check(); break;
        case Command::ScriptExecute: Run(false); break;
        case Command::ScriptExecuteDisable:
            if (!m_lua && Asm::HasSections(m_text))
                Run(true);
            break;
        default: break;
        }
    }

    void ScriptWindow::Shortcuts()
    {
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope != CommandScope::ScriptEditor || FromTemplateSettings(info.Id))
                continue;
            if (Pressed(info.Id))
                RunCommand(info.Id);
        }
        if (m_lua)
            return;
        for (const Templates::TemplateInfo& info : TemplateLibrary(m_engine).Templates())
        {
            const ImGuiKeyChord chord = info.Shortcut.empty() ? 0 : ParseChord(info.Shortcut);
            if (chord && ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused) && !m_templates.Choose(info.Id, m_workspace.LastCodeAddress))
                m_engine.Notify(LogLevel::Info, "The template \"" + info.Caption + "\" does not work for this process.");
        }
    }

    void ScriptWindow::Menu()
    {
        if (!ImGui::BeginMenuBar())
            return;
        const auto item = [&](const char* label, Command command, bool enabled = true)
            {
                if (CommandItem(label, command, false, enabled))
                    RunCommand(command);
            };
        if (ImGui::BeginMenu("File"))
        {
            item(m_lua ? ICON_FA_CLONE "  New script window" : ICON_FA_CLONE "  New Auto Assembler window", Command::ScriptNewWindow);
            item(ICON_FA_FOLDER_OPEN "  Open...", Command::ScriptOpen);
            item(m_recordId != kNoRecord ? ICON_FA_SAVE "  Save into the record" : ICON_FA_SAVE "  Save", Command::ScriptSave);
            item(ICON_FA_SAVE "  Save to a file...", Command::ScriptSaveAs);
            if (m_recordId == kNoRecord && !m_lua && ImGui::MenuItem(ICON_FA_TABLE "  Assign to the cheat table"))
                AssignToTable();
            ImGui::EndMenu();
        }
        if (!m_lua && ImGui::BeginMenu("Template"))
        {
            m_templates.MenuItems(m_workspace.LastCodeAddress, &m_text);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Run"))
        {
            const bool sections = !m_lua && Asm::HasSections(m_text);
            item(m_lua ? ICON_FA_PLAY "  Run" : sections ? ICON_FA_PLAY "  Execute [ENABLE]" : ICON_FA_PLAY "  Execute", Command::ScriptExecute);
            if (sections)
                item(ICON_FA_STOP "  Execute [DISABLE]", Command::ScriptExecuteDisable);
            item(ICON_FA_CHECK "  Check the syntax", Command::ScriptCheck);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void ScriptWindow::Draw()
    {
        if (!m_open)
            return;
        const bool dirty = m_recordId != kNoRecord && m_text != m_saved;
        std::string title = m_title;
        if (dirty)
        {
            const std::size_t hidden = title.find("###");
            title.insert(hidden, " *");
        }
        if (!m_placed)
        {
            const std::uint32_t dock = m_engine.Options().ScriptEditorTabs ? m_workspace.ScriptEditorDock(*this) : 0;
            if (dock)
                ImGui::SetNextWindowDockID(dock, ImGuiCond_Always);
            else
                m_placed = true;
        }
        const bool visible = BeginWindow(title.c_str(), ImVec2(760.0f, 640.0f), ImGuiWindowFlags_MenuBar, ImVec2(560.0f, 300.0f));
        if (!m_placed)
        {
            const ImGuiWindow* self = ImGui::FindWindowByName(m_windowId.c_str());
            m_placed = (self && self->DockId != 0) || ++m_dockTries >= kDockTries;
        }
        if (!visible)
            return;
        Menu();
        m_templates.Draw();
        if (const auto generated = m_templates.TakeText())
        {
            if (Trim(m_text).empty())
                m_text = *generated;
            else
                m_text += (m_text.back() == '\n' ? "\n" : "\n\n") + *generated;
        }
        Shortcuts();

        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + Px(4.0f);
        CodeEditor("##editor", m_text, m_lua ? Language::Lua : Language::AutoAssembler, ImVec2(0.0f, -footer), m_editor);

        if (m_task && m_engine.Lua().TaskAlive(m_task))
        {
            Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
            ImGui::SameLine();
            Muted("The script is running");
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_STOP "  Stop"))
            {
                m_stopped = true;
                m_engine.Lua().StopTask(m_task);
            }
        }
        else if (!m_result.empty() && !m_resultOk)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Error);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ICON_FA_TIMES_CIRCLE "  The last run failed");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            const GhostButtons ghost;
            if (ImGui::SmallButton(ICON_FA_EXPAND "  Show the message"))
                m_shownError.clear();
        }
        else if (!m_result.empty())
        {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            TextWithError(m_result, ManifoldTheme::Accent);
            ImGui::PopTextWrapPos();
        }
        else
        {
            Muted("Line " + std::to_string(m_editor.CursorLine) + ", column " + std::to_string(m_editor.CursorColumn) + "  ·  " + std::to_string(m_editor.LineCount) + " lines");
        }

        if (m_recordId != kNoRecord)
        {
            if (AccentButton(ICON_FA_SAVE "  Save", Px(100.0f, 0.0f)))
                SaveToRecord();
            ItemTooltip(WithKeys("Save into the record", Command::ScriptSave).c_str());
            ImGui::SameLine();
            if (m_recordId >= 0 && !m_lua)
            {
                if (ImGui::Button(ICON_FA_CHECK "  Check", Px(100.0f, 0.0f)))
                    Check();
                ItemTooltip(WithKeys("Check the syntax", Command::ScriptCheck).c_str());
            }
            else if (m_lua)
            {
                if (ImGui::Button(ICON_FA_PLAY "  Run", Px(100.0f, 0.0f)))
                    Run(false);
                ItemTooltip(WithKeys("Run the script", Command::ScriptExecute).c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", Px(100.0f, 0.0f)))
            {
                if (dirty)
                    m_workspace.Confirm("Close script", "The script has changes that are not saved. Close it anyway?", [this]() { m_open = false; }, "Close");
                else
                    m_open = false;
            }
        }
        else
        {
            const bool sections = !m_lua && Asm::HasSections(m_text);
            if (AccentButton(m_lua ? ICON_FA_PLAY "  Run" : sections ? ICON_FA_PLAY "  Execute [ENABLE]" : ICON_FA_PLAY "  Execute", Px(150.0f, 0.0f)))
                Run(false);
            ItemTooltip(WithKeys(m_lua ? "Run the script" : "Assemble and write the script", Command::ScriptExecute).c_str());
            if (sections)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_STOP "  Execute [DISABLE]", Px(150.0f, 0.0f)))
                    Run(true);
                ItemTooltip(WithKeys("Run the [DISABLE] section", Command::ScriptExecuteDisable).c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CHECK "  Check", Px(90.0f, 0.0f)))
                Check();
            ItemTooltip(WithKeys("Check the syntax", Command::ScriptCheck).c_str());
            if (!m_lua)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_TABLE "  Add to table", Px(120.0f, 0.0f)))
                    AssignToTable();
            }
        }
        ErrorPopup();
        EndToolWindow();
    }

    void ScriptWindow::ErrorPopup()
    {
        const std::string id = "scripterror" + m_windowId.substr(3);
        if (!m_resultOk && !m_result.empty() && m_result != m_shownError && !(m_task && m_engine.Lua().TaskAlive(m_task)))
        {
            m_shownError = m_result;
            m_errorLine = m_editor.ErrorLine;
            m_errorOpen = true;
        }
        if (m_errorOpen && !ImGui::IsPopupOpen(("###" + id).c_str()))
            ImGui::OpenPopup(("###" + id).c_str());
        const char* title = m_lua ? "The Lua script failed" : "The Auto Assembler script failed";
        if (!BeginModal(id.c_str(), ICON_FA_TIMES_CIRCLE, title, ImVec2(520.0f, 0.0f), nullptr, CallerViewport()))
            return;
        const ErrorInfo* info = ErrorOf(m_shownError);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        if (info)
        {
            ImGui::TextColored(ManifoldTheme::Accent, "%s  %s", ErrorName(info->Code).c_str(), std::string(ErrorArea(info->Code)).c_str());
            ImGui::TextUnformatted(info->Cause.data(), info->Cause.data() + info->Cause.size());
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
            ImGui::TextUnformatted(info->Help.data(), info->Help.data() + info->Help.size());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        ImGui::PopTextWrapPos();
        std::string message = m_shownError;
        const float textHeight = ImGui::CalcTextSize(message.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x - Px(16.0f)).y + Px(14.0f);
        const float height = std::clamp(textHeight, ImGui::GetFrameHeight() * 2.0f, ImGui::GetTextLineHeight() * 10.0f);
        InputMultiline("##scripterrortext", message, ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
        ImGui::Spacing();
        bool close = false;
        if (AccentButton("OK", Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COPY "  Copy", Px(100.0f, 0.0f)))
            ImGui::SetClipboardText(m_shownError.c_str());
        if (m_errorLine > 0)
        {
            ImGui::SameLine();
            if (ImGui::Button((ICON_FA_LONG_ARROW_ALT_RIGHT "  Go to line " + std::to_string(m_errorLine)).c_str()))
            {
                m_editor.GotoLine = m_errorLine;
                m_editor.Focus = true;
                close = true;
            }
        }
        if (close)
        {
            m_errorOpen = false;
            ImGui::CloseCurrentPopup();
        }
        EndModal();
    }
}
