#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Shortcuts.h"
#include "UI/Templates.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <filesystem>
#include <fstream>
#include <optional>
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
        m_title = std::string(lua ? ICON_FA_MOON "  " : ICON_FA_CODE "  ") + name + "###script" + std::to_string(number);
    }

    void ScriptWindow::Check()
    {
        m_editor.ErrorLine = 0;
        if (m_lua)
        {
            std::string ignored;
            const Status status = m_engine.Lua().RunWithResult("return function() " + m_text + "\nend", "check", ignored);
            m_resultOk = status.Ok();
            m_result = status.Ok() ? "The Lua script compiles." : status.Text;
            return;
        }
        Asm::AaMemory memory;
        if (Asm::HasSections(m_text))
        {
            for (const Asm::AaSection section : { Asm::AaSection::Enable, Asm::AaSection::Disable })
            {
                const Asm::AaResult result = m_engine.AutoAssemble(m_text, section, memory, true, "check");
                if (!result.Ok())
                {
                    m_resultOk = false;
                    m_result = std::string(section == Asm::AaSection::Enable ? "[ENABLE] " : "[DISABLE] ") + result.Error.Text;
                    m_editor.ErrorLine = result.Line;
                    return;
                }
            }
            m_resultOk = true;
            m_result = "Both sections assemble.";
            return;
        }
        const Asm::AaResult result = m_engine.AutoAssemble(m_text, Asm::AaSection::Whole, memory, true, "check");
        m_resultOk = result.Ok();
        m_result = result.Ok() ? "The script assembles." : result.Error.Text;
        m_editor.ErrorLine = result.Ok() ? 0 : (result.Line ? result.Line : LineOf(result.Error.Text));
    }

    void ScriptWindow::Run(bool disable)
    {
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
                return;
            }
            if (m_task && lua.TaskAlive(m_task))
                return;
            m_resultOk = true;
            m_result.clear();
            const std::weak_ptr<int> alive = m_alive;
            m_task = lua.StartTask(m_text, name, [this, alive](const Status& status, const std::string&)
                {
                    if (alive.expired())
                        return;
                    m_task = 0;
                    m_resultOk = status.Ok();
                    m_result = status.Ok() ? "The script ran." : status.Text;
                });
            return;
        }
        const Asm::AaSection section = Asm::HasSections(m_text) ? (disable ? Asm::AaSection::Disable : Asm::AaSection::Enable) : Asm::AaSection::Whole;
        const Asm::AaResult result = m_engine.AutoAssemble(m_text, section, m_engine.ScratchMemory(), false, "window");
        m_resultOk = result.Ok();
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
            const Asm::AaResult result = m_engine.AutoAssemble(m_text, Asm::HasSections(m_text) ? Asm::AaSection::Enable : Asm::AaSection::Whole, memory, true, "check");
            if (!result.Ok())
            {
                m_resultOk = false;
                m_result = "Saved, but it does not assemble: " + result.Error.Text;
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
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        const bool dirty = m_recordId != kNoRecord && m_text != m_saved;
        std::string title = m_title;
        if (dirty)
        {
            const std::size_t hidden = title.find("###");
            title.insert(hidden, " *");
        }
        if (!BeginToolWindow(title.c_str(), &m_open, ImVec2(760.0f, 640.0f), ImGuiWindowFlags_MenuBar, ImVec2(560.0f, 300.0f)))
        {
            EndToolWindow();
            return;
        }
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
                m_engine.Lua().StopTask(m_task);
        }
        else if (!m_result.empty())
        {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            TextWithError(m_result, m_resultOk ? ManifoldTheme::Accent : ManifoldTheme::Error);
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
        EndToolWindow();
    }
}
