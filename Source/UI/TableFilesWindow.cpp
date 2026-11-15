#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/CodeEditor.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::size_t kHexPreviewBytes = 64 * 1024;

        std::string Extension(const std::string& name)
        {
            const std::size_t dot = name.rfind('.');
            if (dot == std::string::npos || dot + 1 >= name.size())
                return {};
            std::string extension = name.substr(dot + 1);
            for (char& c : extension)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return extension;
        }

        bool LooksLikeText(const std::vector<std::uint8_t>& data)
        {
            if (data.empty())
                return true;
            std::size_t printable = 0;
            std::size_t i = 0;
            while (i < data.size())
            {
                const std::uint8_t c = data[i];
                if (c == 0)
                    return false;
                if (c < 0x80)
                {
                    if (c >= 0x20 || c == '\n' || c == '\r' || c == '\t')
                        ++printable;
                    ++i;
                    continue;
                }
                const int follow = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
                if (follow < 0 || i + static_cast<std::size_t>(follow) >= data.size())
                    return false;
                for (int k = 1; k <= follow; ++k)
                {
                    if ((data[i + static_cast<std::size_t>(k)] & 0xC0) != 0x80)
                        return false;
                }
                printable += static_cast<std::size_t>(follow) + 1;
                i += static_cast<std::size_t>(follow) + 1;
            }
            return printable * 10 >= data.size() * 9;
        }

        const char* FileIcon(const std::string& name, bool text)
        {
            const std::string extension = Extension(name);
            if (extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "bmp" || extension == "gif" || extension == "ico")
                return ICON_FA_FILE_IMAGE;
            if (extension == "wav" || extension == "mp3" || extension == "ogg")
                return ICON_FA_FILE_AUDIO;
            if (extension == "lua" || extension == "cea" || extension == "asm")
                return ICON_FA_FILE_CODE;
            if (extension == "zip" || extension == "7z" || extension == "rar")
                return ICON_FA_FILE_ARCHIVE;
            return text ? ICON_FA_FILE_ALT : ICON_FA_FILE;
        }

        std::string SizeText(std::size_t bytes)
        {
            if (bytes < 1024)
                return std::to_string(bytes) + " B";
            if (bytes < 1024 * 1024)
                return Format("%.1f KB", static_cast<double>(bytes) / 1024.0);
            return Format("%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }

        std::string UniqueName(Table::AddressList& list, const std::string& wanted)
        {
            if (!list.FindFile(wanted))
                return wanted;
            const std::size_t dot = wanted.rfind('.');
            const std::string stem = dot == std::string::npos ? wanted : wanted.substr(0, dot);
            const std::string tail = dot == std::string::npos ? std::string() : wanted.substr(dot);
            for (int i = 2;; ++i)
            {
                const std::string candidate = stem + " (" + std::to_string(i) + ")" + tail;
                if (!list.FindFile(candidate))
                    return candidate;
            }
        }
    }

    TableFilesWindow::TableFilesWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void TableFilesWindow::Select(const std::string& name)
    {
        m_selected = name;
        m_renaming = false;
        m_textFor.clear();
    }

    void TableFilesWindow::AddFiles(const std::vector<std::filesystem::path>& paths)
    {
        Table::AddressList& list = m_workspace.Core().Table();
        int added = 0;
        int updated = 0;
        std::vector<std::string> failed;
        std::string last;
        for (const std::filesystem::path& path : paths)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                failed.push_back(WideToUtf8(path.filename().wstring()));
                continue;
            }
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const std::string name = WideToUtf8(path.filename().wstring());
            if (Table::TableFile* existing = list.FindFile(name))
            {
                *existing->Data = std::move(bytes);
                ++updated;
            }
            else
            {
                *list.AddFile(name).Data = std::move(bytes);
                ++added;
            }
            last = name;
        }
        if (added + updated == 0 && failed.empty())
            return;
        list.Touch();
        if (!last.empty())
            Select(last);
        std::string text;
        if (added)
            text = "Added " + std::to_string(added) + (added == 1 ? " file" : " files");
        if (updated)
            text += (text.empty() ? "Updated " : ", updated ") + std::to_string(updated) + (updated == 1 ? " file that was there already" : " files that were there already");
        if (!text.empty())
            m_workspace.Core().Notify(LogLevel::Success, text);
        if (!failed.empty())
            m_workspace.Core().Notify(LogLevel::Error, std::to_string(failed.size()) + (failed.size() == 1 ? " file could not be read: " : " files could not be read: ") + failed.front());
    }

    void TableFilesWindow::Import()
    {
        AddFiles(m_workspace.OpenFilesDialog({ { L"All files", L"*.*" } }));
    }

    void TableFilesWindow::ImportFolder()
    {
        const auto folder = m_workspace.FolderDialog();
        if (!folder)
            return;
        std::vector<std::filesystem::path> paths;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(*folder, error))
        {
            if (entry.is_regular_file(error))
                paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        if (paths.empty())
        {
            m_workspace.Core().Notify(LogLevel::Warning, "The folder has no files: " + WideToUtf8(folder->wstring()));
            return;
        }
        AddFiles(paths);
    }

    void TableFilesWindow::NewTextFile()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        const std::string name = UniqueName(list, "new file.txt");
        list.AddFile(name);
        list.Touch();
        Select(name);
    }

    void TableFilesWindow::Export(const Table::TableFile& file)
    {
        const std::string extension = Extension(file.Name);
        const std::wstring wide = Utf8ToWide(extension);
        const auto path = m_workspace.SaveFileDialog({ { L"All files", L"*.*" } }, file.Name, extension.empty() ? nullptr : wide.c_str());
        if (!path)
            return;
        std::ofstream out(*path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.Data->data()), static_cast<std::streamsize>(file.Data->size()));
        if (out)
            m_workspace.Core().Notify(LogLevel::Success, "Saved " + file.Name + " to " + WideToUtf8(path->wstring()));
        else
            m_workspace.Core().Notify(LogLevel::Error, "The file could not be written: " + WideToUtf8(path->wstring()));
    }

    void TableFilesWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_FILE_ARCHIVE "  Table files###tablefiles", &m_open, ImVec2(820.0f, 560.0f), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(560.0f, 320.0f)))
        {
            EndToolWindow();
            return;
        }
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();

        if (list.Files.empty())
        {
            const int clicked = EmptyState(ICON_FA_FILE_ARCHIVE, "No table files",
                "Files stored inside the table, for example sounds, images or Lua modules. Scripts read them with findTableFile.",
                { { ICON_FA_PLUS "  Add files" }, { ICON_FA_FOLDER_OPEN "  Add a folder" }, { ICON_FA_FILE_ALT "  New text file", nullptr, false, true } }, false);
            if (clicked == 0)
                Import();
            else if (clicked == 1)
                ImportFolder();
            else if (clicked == 2)
                NewTextFile();
            EndToolWindow();
            return;
        }

        const float listWidth = std::round(Px(250.0f));
        const float height = ImGui::GetContentRegionAvail().y;
        BeginCard("##filelist", nullptr, ImVec2(listWidth, height));
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputTextWithHint("##filefilter", ICON_FA_SEARCH " Filter", m_filter);
        const float buttons = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y + Px(10.0f);
        ImGui::BeginChild("##files", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - buttons), ImGuiChildFlags_None);
        for (const Table::TableFile& file : list.Files)
        {
            if (!m_filter.empty() && !ContainsNoCase(file.Name, m_filter))
                continue;
            ImGui::PushID(file.Name.c_str());
            const bool text = LooksLikeText(*file.Data);
            const std::string label = std::string(FileIcon(file.Name, text)) + "  " + file.Name;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(label.c_str(), EqualsNoCase(file.Name, m_selected)))
                Select(file.Name);
            const std::string size = SizeText(file.Data->size());
            const float sizeWidth = ImGui::CalcTextSize(size.c_str()).x;
            ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + ImGui::GetContentRegionAvail().x - sizeWidth, pos.y), ImGui::GetColorU32(ManifoldTheme::Muted), size.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        const float half = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
        if (ImGui::Button(ICON_FA_PLUS "  Add files", ImVec2(half, 0.0f)))
            Import();
        ItemTooltip("Copy files from disk into the table. Pick as many as you like; a file with the same name is updated.");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Add folder", ImVec2(-FLT_MIN, 0.0f)))
            ImportFolder();
        ItemTooltip("Copy every file that lies directly in a folder into the table");
        if (ImGui::Button(ICON_FA_FILE_ALT "  New text file", ImVec2(-FLT_MIN, 0.0f)))
            NewTextFile();
        EndCard();

        ImGui::SameLine();
        BeginCard("##filedetail", nullptr, ImVec2(0.0f, height), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Table::TableFile* file = m_selected.empty() ? nullptr : list.FindFile(m_selected);
        if (!file)
        {
            EmptyState(ICON_FA_FILE, "No file selected", "Pick a file on the left to see, edit or export it.", {}, false);
            EndCard();
            EndToolWindow();
            return;
        }

        const bool text = LooksLikeText(*file->Data);
        if (m_renaming)
        {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Px(180.0f));
            if (ImGui::IsWindowAppearing() || m_renameFocus)
            {
                ImGui::SetKeyboardFocusHere();
                m_renameFocus = false;
            }
            const bool done = InputText("##rename", m_rename, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const std::string wanted = Trim(m_rename);
            const bool taken = !EqualsNoCase(wanted, file->Name) && list.FindFile(wanted);
            ImGui::BeginDisabled(wanted.empty() || taken);
            if (AccentButton("Rename") || (done && !wanted.empty() && !taken))
            {
                file->Name = wanted;
                m_selected = wanted;
                m_renaming = false;
                list.Touch();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                m_renaming = false;
            if (taken)
                TextWithError("A table file with this name exists already.", ManifoldTheme::Error);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
            ImGui::TextUnformatted(FileIcon(file->Name, text));
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, Px(8.0f));
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::TextUnformatted(file->Name.c_str());
            ImGui::PopFont();
            ImGui::SameLine(0.0f, Px(10.0f));
            Muted(SizeText(file->Data->size()) + (text ? "  ·  text" : "  ·  binary"));
            const float actions = ImGui::CalcTextSize(ICON_FA_I_CURSOR "  Rename" ICON_FA_FILE_EXPORT "  Export" ICON_FA_TRASH "  Delete").x + ImGui::GetStyle().FramePadding.x * 6.0f + Px(16.0f);
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Px(10.0f), ImGui::GetWindowContentRegionMax().x - actions));
            if (ImGui::Button(ICON_FA_I_CURSOR "  Rename"))
            {
                m_renaming = true;
                m_renameFocus = true;
                m_rename = file->Name;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FILE_EXPORT "  Export"))
                Export(*file);
            ItemTooltip("Save a copy of the file to disk");
            ImGui::SameLine();
            if (DangerButton(ICON_FA_TRASH "  Delete"))
            {
                const std::string name = file->Name;
                m_workspace.Confirm("Delete table file", "Delete \"" + name + "\" from the table?", [this, name]()
                    {
                        Table::AddressList& files = m_workspace.Core().Table();
                        files.Files.erase(std::remove_if(files.Files.begin(), files.Files.end(), [&](const Table::TableFile& entry) { return EqualsNoCase(entry.Name, name); }), files.Files.end());
                        files.Touch();
                        if (EqualsNoCase(m_selected, name))
                            m_selected.clear();
                    }, "Delete");
            }
        }
        ImGui::Spacing();

        if (text)
        {
            if (m_textFor != file->Name)
            {
                m_textFor = file->Name;
                m_text.assign(file->Data->begin(), file->Data->end());
                m_textDirty = false;
                m_editor = {};
            }
            const std::string extension = Extension(file->Name);
            const Language language = extension == "lua" ? Language::Lua : (extension == "cea" || extension == "asm" ? Language::AutoAssembler : Language::Plain);
            const float footer = ImGui::GetFrameHeight() + Px(8.0f);
            if (CodeEditor("##filetext", m_text, language, ImVec2(0.0f, ImGui::GetContentRegionAvail().y - footer), m_editor))
                m_textDirty = true;
            ImGui::BeginDisabled(!m_textDirty);
            if (AccentButton(ICON_FA_SAVE "  Save into the table"))
            {
                file->Data->assign(m_text.begin(), m_text.end());
                m_textDirty = false;
                list.Touch();
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_UNDO "  Revert"))
                m_textFor.clear();
            ImGui::EndDisabled();
            ImGui::SameLine();
            Muted(m_textDirty ? "Changes are not in the table yet" : "Line " + std::to_string(m_editor.CursorLine) + ", column " + std::to_string(m_editor.CursorColumn));
        }
        else
        {
            const std::size_t shown = std::min(file->Data->size(), kHexPreviewBytes);
            if (shown < file->Data->size())
                Muted("Showing the first " + SizeText(shown) + ". Export the file to see all of it.");
            ImGui::BeginChild("##hex", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            PushMono();
            const float charWidth = ImGui::CalcTextSize("0").x;
            const float wide = charWidth * (8.0f + 16.0f * 3.0f + 1.0f + 16.0f) + Px(28.0f) + ImGui::GetStyle().ScrollbarSize;
            const std::size_t perRow = ImGui::GetContentRegionAvail().x >= wide ? 16 : 8;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>((shown + perRow - 1) / perRow));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const std::size_t start = static_cast<std::size_t>(row) * perRow;
                    std::string bytes;
                    std::string chars;
                    for (std::size_t i = start; i < start + perRow; ++i)
                    {
                        if (i < shown)
                        {
                            const std::uint8_t value = (*file->Data)[i];
                            bytes += Format("%02X ", value);
                            chars += value >= 0x20 && value < 0x7F ? static_cast<char>(value) : '.';
                        }
                        else
                        {
                            bytes += "   ";
                        }
                        if (perRow == 16 && i == start + 7)
                            bytes += ' ';
                    }
                    ImGui::TextColored(ManifoldTheme::AddressText, "%08zX", start);
                    ImGui::SameLine(0.0f, Px(14.0f));
                    ImGui::TextColored(ManifoldTheme::BytesText, "%s", bytes.c_str());
                    ImGui::SameLine(0.0f, Px(10.0f));
                    ImGui::TextUnformatted(chars.c_str());
                }
            }
            PopMono();
            ImGui::EndChild();
        }
        EndCard();
        EndToolWindow();
    }
}
