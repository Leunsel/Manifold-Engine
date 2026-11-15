#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Templates.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <fstream>
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
        : m_workspace(workspace), m_engine(workspace.Core()), m_text(std::move(script)), m_recordId(recordId), m_lua(lua)
    {
        m_saved = m_text;
        std::string name;
        if (recordId == -1)
            name = "Table Lua script";
        else if (recordId > 0)
        {
            const Table::MemoryRecord* record = m_engine.Table().FindById(recordId);
            name = record ? record->Description : "Script";
        }
        else
            name = lua ? "Lua script" : "Auto Assembler";
        m_title = std::string(lua ? ICON_FA_MOON "  " : ICON_FA_CODE "  ") + name + "###script" + std::to_string(number);
        m_templateAddress = workspace.LastCodeAddress ? m_engine.Symbols().ModuleOffset(workspace.LastCodeAddress) : "";
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
            const Status status = m_engine.Lua().Run(m_text, m_recordId == -1 ? "table script" : "script");
            m_resultOk = status.Ok();
            m_result = status.Ok() ? "The script ran." : status.Text;
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
        if (m_recordId == -1)
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

    void ScriptWindow::Menu()
    {
        if (!ImGui::BeginMenuBar())
            return;
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open..."))
            {
                const std::vector<FileFilter> filters = m_lua ? std::vector<FileFilter>{ { L"Lua scripts (*.lua)", L"*.lua" }, { L"All files", L"*.*" } }
                                                              : std::vector<FileFilter>{ { L"Auto Assembler scripts (*.cea)", L"*.cea" }, { L"All files", L"*.*" } };
                if (const auto path = m_workspace.OpenFileDialog(filters))
                {
                    std::ifstream file(*path, std::ios::binary);
                    std::stringstream content;
                    content << file.rdbuf();
                    m_text = content.str();
                }
            }
            if (ImGui::MenuItem(ICON_FA_SAVE "  Save as..."))
            {
                const auto path = m_lua ? m_workspace.SaveFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" } }, "script.lua", L"lua")
                                        : m_workspace.SaveFileDialog({ { L"Auto Assembler scripts (*.cea)", L"*.cea" } }, "script.cea", L"cea");
                if (path)
                {
                    std::ofstream file(*path, std::ios::binary | std::ios::trunc);
                    file.write(m_text.data(), static_cast<std::streamsize>(m_text.size()));
                }
            }
            if (m_recordId == 0 && !m_lua && ImGui::MenuItem(ICON_FA_TABLE "  Assign to the cheat table"))
            {
                Table::MemoryRecord& record = m_engine.Table().AddScript("Auto Assemble script", m_text);
                m_recordId = record.Id;
                m_saved = m_text;
                m_resultOk = true;
                m_result = "Added to the address list.";
            }
            ImGui::EndMenu();
        }
        if (!m_lua && ImGui::BeginMenu("Template"))
        {
            for (int kind = 0; kind < static_cast<int>(std::size(kTemplateNames)); ++kind)
            {
                if (ImGui::MenuItem(kTemplateNames[kind]))
                {
                    m_templateKind = kind;
                    if (kind >= static_cast<int>(TemplateKind::CheatTable))
                    {
                        std::string error;
                        if (const auto script = MakeTemplate(m_engine, static_cast<TemplateKind>(kind), 0, "", error))
                            m_text = *script + (m_text.empty() ? "" : "\n" + m_text);
                    }
                    else
                    {
                        m_templateOpen = true;
                        m_templateError.clear();
                    }
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_CHECK "  Check"))
            Check();
        ImGui::EndMenuBar();
    }

    void ScriptWindow::TemplateDialog()
    {
        if (m_templateOpen)
        {
            ImGui::OpenPopup("##template");
            m_templateOpen = false;
        }
        if (!ImGui::BeginPopup("##template"))
            return;
        ImGui::Text("%s template", kTemplateNames[m_templateKind]);
        ImGui::TextUnformatted("Inject at");
        ImGui::SetNextItemWidth(Px(300.0f));
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        InputTextWithHint("##address", "game.exe+1234", m_templateAddress);
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(Px(300.0f));
        InputText("##name", m_templateName);
        if (!m_templateError.empty())
            TextWithError(m_templateError, ManifoldTheme::Error);
        if (AccentButton("Generate", Px(100.0f, 0.0f)))
        {
            Status error;
            if (const auto address = m_engine.Evaluate(m_templateAddress, &error))
            {
                std::string message;
                if (const auto script = MakeTemplate(m_engine, static_cast<TemplateKind>(m_templateKind), *address, m_templateName, message))
                {
                    m_text = *script;
                    ImGui::CloseCurrentPopup();
                }
                else
                    m_templateError = message;
            }
            else
                m_templateError = error.Text;
        }
        ImGui::EndPopup();
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
        const bool dirty = m_recordId != 0 && m_text != m_saved;
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
        TemplateDialog();
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && m_recordId != 0)
            SaveToRecord();
        if (focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
            Run(false);

        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + Px(4.0f);
        CodeEditor("##editor", m_text, m_lua ? Language::Lua : Language::AutoAssembler, ImVec2(0.0f, -footer), m_editor);

        if (!m_result.empty())
        {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            TextWithError(m_result, m_resultOk ? ManifoldTheme::Accent : ManifoldTheme::Error);
            ImGui::PopTextWrapPos();
        }
        else
        {
            Muted("Line " + std::to_string(m_editor.CursorLine) + ", column " + std::to_string(m_editor.CursorColumn) + "  ·  " + std::to_string(m_editor.LineCount) + " lines");
        }

        if (m_recordId != 0)
        {
            if (AccentButton(ICON_FA_SAVE "  Save", Px(100.0f, 0.0f)))
                SaveToRecord();
            ImGui::SameLine();
            if (m_recordId > 0 && !m_lua)
            {
                if (ImGui::Button(ICON_FA_CHECK "  Check", Px(100.0f, 0.0f)))
                    Check();
            }
            else if (m_lua && ImGui::Button(ICON_FA_PLAY "  Run", Px(100.0f, 0.0f)))
                Run(false);
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
            if (sections)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_STOP "  Execute [DISABLE]", Px(150.0f, 0.0f)))
                    Run(true);
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CHECK "  Check", Px(90.0f, 0.0f)))
                Check();
            if (!m_lua)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_TABLE "  Add to table", Px(120.0f, 0.0f)))
                {
                    Table::MemoryRecord& record = m_engine.Table().AddScript("Auto Assemble script", m_text);
                    m_recordId = record.Id;
                    m_saved = m_text;
                    m_resultOk = true;
                    m_result = "Added to the address list.";
                }
            }
        }
        EndToolWindow();
    }
}
