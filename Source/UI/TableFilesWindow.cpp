#include "UI/TableFilesWindow.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/AutorunModules.h"
#include "Table/CheatTable.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include <d3d11.h>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::size_t kHexBytes = 16u * 1024u * 1024u;
        constexpr std::size_t kEditBytes = 16u * 1024u * 1024u;
        constexpr std::size_t kImageBytes = 64u * 1024u * 1024u;
        constexpr int kImageSide = 16384;
        constexpr const char* kWorkGroup = "tablefiles";

        const char* KindIcon(Table::FileKind kind)
        {
            switch (kind)
            {
            case Table::FileKind::Lua:
            case Table::FileKind::Json:
            case Table::FileKind::Xml:
            case Table::FileKind::AutoAssembler: return ICON_FA_FILE_CODE;
            case Table::FileKind::Text: return ICON_FA_FILE_ALT;
            case Table::FileKind::Image: return ICON_FA_FILE_IMAGE;
            case Table::FileKind::Audio: return ICON_FA_FILE_AUDIO;
            case Table::FileKind::Archive: return ICON_FA_FILE_ARCHIVE;
            case Table::FileKind::Font:
            case Table::FileKind::Program:
            case Table::FileKind::Binary: break;
            }
            return ICON_FA_FILE;
        }

        Language LanguageOf(Table::FileKind kind)
        {
            switch (kind)
            {
            case Table::FileKind::Lua: return Language::Lua;
            case Table::FileKind::Json: return Language::Json;
            case Table::FileKind::AutoAssembler: return Language::AutoAssembler;
            default: return Language::Plain;
            }
        }

        const char* ViewName(int view)
        {
            static const char* kNames[] = { "Automatic", "Text", "Image", "Hex", "Details" };
            return kNames[view];
        }

        std::string Count(std::size_t count, const char* one, const char* many)
        {
            return std::to_string(count) + " " + (count == 1 ? one : many);
        }

        std::optional<std::vector<std::uint8_t>> ReadWhole(const std::filesystem::path& path, std::size_t limit, bool& tooLarge)
        {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error)
                return std::nullopt;
            if (size > limit)
            {
                tooLarge = true;
                return std::nullopt;
            }
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return std::nullopt;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (static_cast<std::size_t>(file.gcount()) != bytes.size())
                return std::nullopt;
            return bytes;
        }

        bool WriteWhole(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            return !out.fail();
        }

        std::string UnsafeNameText(std::size_t count, const std::string& first)
        {
            return Count(count, "file was", "files were") + " not exported because Windows does not allow " + (count == 1 ? "its name" : "their names") +
                   " as a file name (a path, a device name like CON or NUL, or a name that ends with a dot). Rename " + (count == 1 ? "it" : "them") + " first: " + first;
        }

        void RightAligned(const std::string& text, const ImVec4& color)
        {
            const float width = ImGui::CalcTextSize(text.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
            ImGui::TextColored(color, "%s", text.c_str());
        }

        void Fact(const char* name, const std::string& value, bool mono = false)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Muted(name);
            ImGui::TableNextColumn();
            if (mono)
                PushMono();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(value.c_str());
            ImGui::PopTextWrapPos();
            if (mono)
                PopMono();
        }
    }

    TableFilesWindow::TableFilesWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    TableFilesWindow::~TableFilesWindow()
    {
        m_workspace.Core().Work().CancelGroup(kWorkGroup);
        ReleaseImage();
    }

    bool TableFilesWindow::TakesDrop(void* window, float x, float y) const
    {
        if (!m_open || m_drawnFrame < ImGui::GetFrameCount() - 2 || window != m_viewportWindow)
            return false;
        return x >= m_windowMin.x && y >= m_windowMin.y && x < m_windowMax.x && y < m_windowMax.y;
    }

    const TableFilesWindow::Info* TableFilesWindow::InfoOf(const std::string& key) const
    {
        const auto found = m_info.find(key);
        return found == m_info.end() ? nullptr : &found->second;
    }

    bool TableFilesWindow::HasDraft(const std::string& key) const
    {
        return m_drafts.count(key) > 0 || (key == m_textKey && m_textDirty);
    }

    Table::TableFile* TableFilesWindow::Current()
    {
        return m_current.empty() ? nullptr : m_workspace.Core().Table().FindFile(m_current);
    }

    void TableFilesWindow::TableChanged()
    {
        const std::size_t drafts = m_drafts.size() + (m_textDirty && !m_drafts.count(m_textKey) ? 1 : 0);
        if (drafts > 0)
            m_workspace.Core().Notify(LogLevel::Warning, Count(drafts, "table file had", "table files had") + " changes that were not put into the table. Another table was opened, so they were dropped.");
        m_drafts.clear();
        m_textKey.clear();
        m_text.clear();
        m_textData = nullptr;
        m_textVersion = 0;
        m_textDirty = false;
        m_textStale = false;
        m_editor = {};
        m_info.clear();
        m_names.clear();
        m_selected.clear();
        m_current.clear();
        m_anchor.clear();
        m_scrollTo.clear();
        m_renaming.clear();
        ReleaseImage();
        m_rowsDirty = true;
        m_usesDirty = true;
    }

    void TableFilesWindow::Sync()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        if (list.Identity() != m_identity)
        {
            m_identity = list.Identity();
            TableChanged();
        }
        bool changed = list.Files.size() != m_names.size();
        for (auto& [key, info] : m_info)
            info.Seen = false;
        m_totalSize = 0;
        for (std::size_t i = 0; i < list.Files.size(); ++i)
        {
            const Table::TableFile& file = list.Files[i];
            if (!changed && m_names[i] != file.Name)
                changed = true;
            Info& info = m_info[Table::FileUseKey(file.Name)];
            info.Seen = true;
            m_totalSize += file.Data->size();
            if (info.Data == file.Data.get() && info.Version == *file.Version && info.Size == file.Data->size())
                continue;
            info.Data = file.Data.get();
            info.Version = *file.Version;
            info.Size = file.Data->size();
            info.Kind = Table::DetectFileKind(file.Name, *file.Data);
            info.Stored = -1;
            info.Sha256.clear();
            changed = true;
        }
        for (auto it = m_info.begin(); it != m_info.end();)
        {
            if (it->second.Seen)
            {
                ++it;
                continue;
            }
            changed = true;
            it = m_info.erase(it);
        }
        if (changed)
        {
            m_names.clear();
            for (const Table::TableFile& file : list.Files)
                m_names.push_back(file.Name);
            m_rowsDirty = true;
            m_usesDirty = true;
            std::erase_if(m_selected, [&](const std::string& key) { return !m_info.count(key); });
            std::erase_if(m_drafts, [&](const auto& entry) { return !m_info.count(entry.first); });
            if (!m_info.count(m_current))
                m_current = m_selected.empty() ? std::string() : *m_selected.begin();
            if (!m_info.count(m_anchor))
                m_anchor = m_current;
            if (!m_info.count(m_renaming))
                m_renaming.clear();
            if (!m_textKey.empty() && !m_info.count(m_textKey))
            {
                m_textKey.clear();
                m_textDirty = false;
                m_textStale = false;
            }
        }
        if (!m_textKey.empty())
        {
            const Table::TableFile* file = list.FindFile(m_textKey);
            if (file && (file->Data.get() != m_textData || *file->Version != m_textVersion))
            {
                if (m_textDirty)
                    m_textStale = true;
                else
                    m_textKey.clear();
            }
        }
        if (list.Generation() != m_usesGeneration)
        {
            m_usesGeneration = list.Generation();
            m_usesDirty = true;
        }
        if (m_usesDirty && ImGui::GetTime() - m_usesAt > 0.4)
        {
            m_uses = Table::FindFileUses(list);
            m_usesAt = ImGui::GetTime();
            m_usesDirty = false;
            m_rowsDirty = true;
        }
    }

    void TableFilesWindow::RebuildRows()
    {
        m_rowsDirty = false;
        Table::AddressList& list = m_workspace.Core().Table();
        std::vector<Table::FileRow> rows;
        rows.reserve(list.Files.size());
        m_totalStored = 0;
        m_storedPending = false;
        for (const Table::TableFile& file : list.Files)
        {
            const std::string key = Table::FileUseKey(file.Name);
            const Info* info = InfoOf(key);
            Table::FileRow row;
            row.Name = file.Name;
            row.Size = file.Data->size();
            if (info)
            {
                row.Kind = info->Kind;
                row.Stored = info->Stored;
            }
            if (row.Stored < 0)
                m_storedPending = true;
            else
                m_totalStored += row.Stored;
            if (const auto uses = m_uses.find(key); uses != m_uses.end())
                row.Uses = static_cast<int>(uses->second.size());
            rows.push_back(std::move(row));
        }
        m_rows = Table::ArrangeFiles(std::move(rows), m_filter, m_sort, m_descending);
    }

    void TableFilesWindow::StartFacts()
    {
        if (m_factsRunning)
            return;
        Table::AddressList& list = m_workspace.Core().Table();
        const Table::TableFile* next = nullptr;
        if (const Table::TableFile* current = Current())
        {
            const Info* info = InfoOf(m_current);
            if (info && info->Stored < 0)
                next = current;
        }
        for (std::size_t i = 0; !next && i < list.Files.size(); ++i)
        {
            const Info* info = InfoOf(Table::FileUseKey(list.Files[i].Name));
            if (info && info->Stored < 0)
                next = &list.Files[i];
        }
        if (!next)
            return;
        WorkOptions options;
        options.Title = "Measure " + next->Name;
        options.Group = kWorkGroup;
        options.Quiet = true;
        options.Serial = true;
        auto bytes = std::make_shared<const std::vector<std::uint8_t>>(*next->Data);
        FactsResult result;
        result.Identity = list.Identity();
        result.Key = Table::FileUseKey(next->Name);
        result.Data = next->Data.get();
        result.Version = *next->Version;
        const int id = m_workspace.Core().Work().Start(std::move(options), [results = m_results, bytes, result](WorkProgress&) mutable -> BackgroundWork::Finish
            {
                result.Stored = Table::StoredFileSize(*bytes);
                result.Sha256 = Autorun::Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
                return [results, result]() { results->Facts.push_back(result); };
            });
        m_factsRunning = id != 0;
    }

    void TableFilesWindow::Collect()
    {
        const std::uint64_t identity = m_workspace.Core().Table().Identity();
        for (FactsResult& result : std::exchange(m_results->Facts, {}))
        {
            m_factsRunning = false;
            const auto found = m_info.find(result.Key);
            if (result.Identity != identity || found == m_info.end() || found->second.Data != result.Data || found->second.Version != result.Version)
                continue;
            found->second.Stored = static_cast<std::int64_t>(result.Stored);
            found->second.Sha256 = std::move(result.Sha256);
            m_rowsDirty = true;
        }
        for (ImageResult& result : std::exchange(m_results->Images, {}))
        {
            if (result.Identity != identity || result.Key != m_image.Key || result.Data != m_image.Data || result.Version != m_image.Version)
                continue;
            m_image.Loading = false;
            m_image.Failed = true;
            ID3D11Device* device = static_cast<ID3D11Device*>(m_workspace.Window().Device());
            if (!result.Image || !device || result.Image->Width <= 0 || result.Image->Height <= 0 || result.Image->Width > kImageSide || result.Image->Height > kImageSide)
                continue;
            D3D11_TEXTURE2D_DESC description{};
            description.Width = static_cast<UINT>(result.Image->Width);
            description.Height = static_cast<UINT>(result.Image->Height);
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = result.Image->Rgba.data();
            data.SysMemPitch = description.Width * 4;
            ID3D11Texture2D* texture = nullptr;
            if (FAILED(device->CreateTexture2D(&description, &data, &texture)) || !texture)
                continue;
            ID3D11ShaderResourceView* view = nullptr;
            const HRESULT made = device->CreateShaderResourceView(texture, nullptr, &view);
            texture->Release();
            if (FAILED(made) || !view)
                continue;
            m_image.Texture = view;
            m_image.Width = result.Image->Width;
            m_image.Height = result.Image->Height;
            m_image.Failed = false;
        }
        for (AddResult& result : std::exchange(m_results->Added, {}))
        {
            --m_reading;
            Apply(result);
        }
    }

    void TableFilesWindow::ReleaseImage()
    {
        if (m_image.Texture)
            static_cast<ID3D11ShaderResourceView*>(m_image.Texture)->Release();
        m_image = {};
    }

    void TableFilesWindow::StartImage(const Table::TableFile& file, const std::string& key)
    {
        ReleaseImage();
        m_image.Key = key;
        m_image.Data = file.Data.get();
        m_image.Version = *file.Version;
        if (file.Data->empty() || file.Data->size() > kImageBytes)
        {
            m_image.Failed = true;
            return;
        }
        WorkOptions options;
        options.Title = "Decode " + file.Name;
        options.Group = kWorkGroup;
        options.Quiet = true;
        auto bytes = std::make_shared<const std::vector<std::uint8_t>>(*file.Data);
        ImageResult result;
        result.Identity = m_workspace.Core().Table().Identity();
        result.Key = key;
        result.Data = m_image.Data;
        result.Version = m_image.Version;
        const int id = m_workspace.Core().Work().Start(std::move(options), [results = m_results, bytes, result](WorkProgress&) mutable -> BackgroundWork::Finish
            {
                auto shared = std::make_shared<ImageResult>(std::move(result));
                shared->Image = DecodeImage(*bytes);
                return [results, shared]() { results->Images.push_back(std::move(*shared)); };
            });
        m_image.Loading = id != 0;
        m_image.Failed = id == 0;
    }

    void TableFilesWindow::SelectOnly(const std::string& key)
    {
        m_selected.clear();
        if (!key.empty())
            m_selected.insert(key);
        m_current = key;
        m_anchor = key;
        m_scrollTo = key;
    }

    void TableFilesWindow::SelectAll()
    {
        m_selected = VisibleKeys();
        if (!m_rows.empty() && !m_selected.count(m_current))
        {
            m_current = Table::FileUseKey(m_rows.front().Name);
            m_anchor = m_current;
        }
    }

    std::set<std::string> TableFilesWindow::VisibleKeys() const
    {
        std::set<std::string> keys;
        for (const Table::FileRow& row : m_rows)
            keys.insert(Table::FileUseKey(row.Name));
        return keys;
    }

    void TableFilesWindow::Click(int index, bool toggle, bool range)
    {
        if (index < 0 || index >= static_cast<int>(m_rows.size()))
            return;
        const std::string key = Table::FileUseKey(m_rows[static_cast<std::size_t>(index)].Name);
        if (range && !m_anchor.empty())
        {
            int anchor = index;
            for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
            {
                if (Table::FileUseKey(m_rows[static_cast<std::size_t>(i)].Name) == m_anchor)
                    anchor = i;
            }
            if (!toggle)
                m_selected.clear();
            for (int i = std::min(anchor, index); i <= std::max(anchor, index); ++i)
                m_selected.insert(Table::FileUseKey(m_rows[static_cast<std::size_t>(i)].Name));
            m_current = key;
            return;
        }
        if (toggle)
        {
            if (m_selected.count(key) && m_selected.size() > 1)
            {
                m_selected.erase(key);
                if (m_current == key)
                    m_current = *m_selected.begin();
            }
            else
            {
                m_selected.insert(key);
                m_current = key;
            }
            m_anchor = key;
            return;
        }
        m_selected.clear();
        m_selected.insert(key);
        m_current = key;
        m_anchor = key;
    }

    std::vector<std::string> TableFilesWindow::SelectedNames() const
    {
        std::vector<std::string> names;
        for (const Table::TableFile& file : m_workspace.Core().Table().Files)
        {
            if (m_selected.count(Table::FileUseKey(file.Name)))
                names.push_back(file.Name);
        }
        return names;
    }

    void TableFilesWindow::Read(std::vector<std::filesystem::path> paths, std::string replace)
    {
        if (paths.empty())
            return;
        const Table::AddressList& list = m_workspace.Core().Table();
        const std::uint64_t identity = list.Identity();
        std::size_t room = Table::kMaxTableFilesBytes;
        for (const Table::TableFile& file : list.Files)
            room -= std::min(room, file.Data->size());
        WorkOptions options;
        options.Title = paths.size() == 1 ? "Read " + WideToUtf8(paths.front().filename().wstring()) : "Read " + std::to_string(paths.size()) + " files";
        options.Group = kWorkGroup;
        options.Quiet = true;
        const int id = m_workspace.Core().Work().Start(std::move(options),
            [results = m_results, paths = std::move(paths), replace = std::move(replace), room, identity](WorkProgress& progress) mutable -> BackgroundWork::Finish
            {
                auto result = std::make_shared<AddResult>();
                result->Identity = identity;
                result->Replace = std::move(replace);
                std::vector<std::filesystem::path> files;
                for (const std::filesystem::path& path : paths)
                {
                    std::error_code error;
                    if (!std::filesystem::is_directory(path, error))
                    {
                        files.push_back(path);
                        continue;
                    }
                    std::vector<std::filesystem::path> inside;
                    for (const auto& entry : std::filesystem::directory_iterator(path, error))
                    {
                        if (entry.is_regular_file(error))
                            inside.push_back(entry.path());
                    }
                    std::sort(inside.begin(), inside.end());
                    files.insert(files.end(), inside.begin(), inside.end());
                }
                progress.SetSteps(0, static_cast<std::int64_t>(files.size()));
                for (const std::filesystem::path& path : files)
                {
                    if (progress.Cancelled())
                        break;
                    const std::string name = WideToUtf8(path.filename().wstring());
                    bool tooLarge = false;
                    auto bytes = ReadWhole(path, std::min(room, Table::kMaxTableFileBytes), tooLarge);
                    progress.Step();
                    if (!bytes)
                    {
                        (tooLarge ? result->TooLarge : result->Failed).push_back(name);
                        continue;
                    }
                    room -= bytes->size();
                    result->Files.push_back({ name, std::move(*bytes) });
                }
                return [results, result]() { results->Added.push_back(std::move(*result)); };
            });
        if (id != 0)
            ++m_reading;
    }

    void TableFilesWindow::Apply(AddResult& result)
    {
        Engine& engine = m_workspace.Core();
        Table::AddressList& list = engine.Table();
        if (result.Identity != list.Identity())
        {
            if (!result.Files.empty())
                engine.Notify(LogLevel::Warning, Count(result.Files.size(), "file that was", "files that were") + " read from disk " + (result.Files.size() == 1 ? "was" : "were") +
                                                     " not put into the table: another table was opened in the meantime.");
            return;
        }
        int added = 0;
        int updated = 0;
        std::string last;
        for (LoadedFile& loaded : result.Files)
        {
            const std::string name = result.Replace.empty() ? loaded.Name : result.Replace;
            if (!Table::FileNameProblem(list, name, name).empty())
            {
                result.Failed.push_back(loaded.Name);
                continue;
            }
            if (Table::TableFile* existing = list.FindFile(name))
            {
                Table::SetFileData(list, *existing, std::move(loaded.Bytes));
                ++updated;
            }
            else if (result.Replace.empty())
            {
                Table::SetFileData(list, list.AddFile(name), std::move(loaded.Bytes));
                ++added;
            }
            last = name;
        }
        if (!last.empty())
        {
            m_selected.clear();
            if (result.Replace.empty())
            {
                for (const LoadedFile& loaded : result.Files)
                    m_selected.insert(Table::FileUseKey(loaded.Name));
            }
            const std::string key = Table::FileUseKey(last);
            m_selected.insert(key);
            m_current = key;
            m_anchor = key;
            m_scrollTo = key;
        }
        std::string text;
        if (added)
            text = "Added " + Count(static_cast<std::size_t>(added), "file", "files");
        if (updated && !result.Replace.empty())
            text = "Replaced the content of " + result.Replace;
        else if (updated)
            text += (text.empty() ? "Updated " : ", updated ") + Count(static_cast<std::size_t>(updated), "file that was there already", "files that were there already");
        if (!text.empty())
            engine.Notify(LogLevel::Success, text);
        if (!result.Failed.empty())
            engine.Notify(LogLevel::Error, Count(result.Failed.size(), "file could not be read: ", "files could not be read: ") + result.Failed.front());
        if (!result.TooLarge.empty())
            engine.Notify(LogLevel::Error, Count(result.TooLarge.size(), "file is too large for a table (", "files are too large for a table (") + ByteSize(Table::kMaxTableFileBytes) + " per file, " +
                                               ByteSize(Table::kMaxTableFilesBytes) + " in all): " + result.TooLarge.front());
    }

    void TableFilesWindow::AddFiles(const std::vector<std::filesystem::path>& paths)
    {
        Read(paths, {});
    }

    void TableFilesWindow::Import()
    {
        AddFiles(m_workspace.OpenFilesDialog({ { L"All files", L"*.*" } }));
    }

    void TableFilesWindow::ImportFolder()
    {
        if (const auto folder = m_workspace.FolderDialog())
            AddFiles({ *folder });
    }

    void TableFilesWindow::NewTextFile()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        const std::string name = Table::UniqueFileName(list, "new_file.txt");
        list.AddFile(name);
        SelectOnly(Table::FileUseKey(name));
        m_renaming = m_current;
        m_rename = name;
        m_renameFocus = true;
    }

    void TableFilesWindow::ReplaceFromDisk()
    {
        const Table::TableFile* file = Current();
        if (!file)
            return;
        if (const auto path = m_workspace.OpenFileDialog({ { L"All files", L"*.*" } }))
            Read({ *path }, file->Name);
    }

    void TableFilesWindow::Export()
    {
        ExportTo(SelectedNames());
    }

    void TableFilesWindow::ExportAll()
    {
        std::vector<std::string> names;
        for (const Table::TableFile& file : m_workspace.Core().Table().Files)
            names.push_back(file.Name);
        ExportTo(names);
    }

    void TableFilesWindow::ExportTo(const std::vector<std::string>& names)
    {
        Engine& engine = m_workspace.Core();
        if (names.empty())
            return;
        if (names.size() == 1)
        {
            const Table::TableFile* file = engine.Table().FindFile(names.front());
            if (!file)
                return;
            if (!Table::SafeExportName(file->Name))
            {
                engine.Notify(LogLevel::Error, UnsafeNameText(1, file->Name));
                return;
            }
            const std::wstring extension = Utf8ToWide(Table::FileExtension(file->Name));
            const auto path = m_workspace.SaveFileDialog({ { L"All files", L"*.*" } }, file->Name, extension.empty() ? nullptr : extension.c_str());
            if (!path)
                return;
            if (WriteWhole(*path, *file->Data))
                engine.Notify(LogLevel::Success, "Saved " + file->Name + " to " + WideToUtf8(path->wstring()));
            else
                engine.Notify(LogLevel::Error, "The file could not be written: " + WideToUtf8(path->wstring()));
            return;
        }
        const auto folder = m_workspace.FolderDialog();
        if (!folder)
            return;
        std::size_t existing = 0;
        for (const std::string& name : names)
        {
            std::error_code error;
            if (Table::SafeExportName(name) && std::filesystem::exists(*folder / Utf8ToWide(name), error))
                ++existing;
        }
        if (existing == 0)
        {
            WriteFiles(names, *folder);
            return;
        }
        const std::uint64_t identity = engine.Table().Identity();
        m_workspace.Confirm("Export table files",
            Count(existing, "file exists", "files exist") + " already in " + WideToUtf8(folder->wstring()) + ". Overwrite " + (existing == 1 ? "it" : "them") + "?",
            [this, names, folder = *folder, identity]()
            {
                if (m_workspace.Core().Table().Identity() == identity)
                    WriteFiles(names, folder);
                else
                    m_workspace.Core().Notify(LogLevel::Warning, "Nothing was exported: another table was opened in the meantime.");
            }, "Overwrite");
    }

    void TableFilesWindow::WriteFiles(const std::vector<std::string>& names, const std::filesystem::path& folder)
    {
        Engine& engine = m_workspace.Core();
        std::size_t written = 0;
        std::size_t failures = 0;
        std::string failed;
        std::vector<std::string> unsafe;
        for (const std::string& name : names)
        {
            // ∑ Names come from the loaded table: ..\..\x.bat, C:\x, a:stream or CON would write outside the folder or to a device.
            if (!Table::SafeExportName(name))
            {
                unsafe.push_back(name);
                continue;
            }
            const Table::TableFile* file = engine.Table().FindFile(name);
            if (file && WriteWhole(folder / Utf8ToWide(name), *file->Data))
            {
                ++written;
                continue;
            }
            ++failures;
            if (failed.empty())
                failed = name;
        }
        if (written)
            engine.Notify(LogLevel::Success, "Saved " + Count(written, "file", "files") + " to " + WideToUtf8(folder.wstring()));
        if (!unsafe.empty())
            engine.Notify(LogLevel::Error, UnsafeNameText(unsafe.size(), unsafe.front()));
        if (failures)
            engine.Notify(LogLevel::Error, Count(failures, "file", "files") + " could not be written: " + failed);
    }

    void TableFilesWindow::StartRename()
    {
        const Table::TableFile* file = Current();
        if (!file)
            return;
        SelectOnly(m_current);
        m_renaming = m_current;
        m_rename = file->Name;
        m_renameFocus = true;
    }

    void TableFilesWindow::CommitRename()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        const std::string from = std::exchange(m_renaming, {});
        const Table::TableFile* file = list.FindFile(from);
        if (!file)
            return;
        const std::string old = file->Name;
        const std::string wanted = Trim(m_rename);
        if (wanted == old || !Table::RenameFile(list, old, wanted))
            return;
        const std::string key = Table::FileUseKey(wanted);
        if (key == from)
            return;
        if (auto node = m_info.extract(from); !node.empty())
        {
            node.key() = key;
            node.mapped().Kind = Table::DetectFileKind(wanted, *file->Data);
            m_info.insert(std::move(node));
        }
        if (auto node = m_drafts.extract(from); !node.empty())
        {
            node.key() = key;
            m_drafts.insert(std::move(node));
        }
        if (m_textKey == from)
        {
            StashText();
            if (auto node = m_drafts.extract(from); !node.empty())
            {
                node.key() = key;
                m_drafts.insert(std::move(node));
            }
            m_textKey.clear();
            m_textDirty = false;
        }
        if (m_image.Key == from)
            ReleaseImage();
        m_selected.erase(from);
        m_selected.insert(key);
        m_current = key;
        m_anchor = key;
        m_scrollTo = key;
    }

    void TableFilesWindow::Duplicate()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        std::string last;
        for (const std::string& name : SelectedNames())
        {
            if (const Table::TableFile* copy = Table::DuplicateFile(list, name))
                last = copy->Name;
        }
        if (!last.empty())
            SelectOnly(Table::FileUseKey(last));
    }

    void TableFilesWindow::Delete()
    {
        const std::set<std::string> visible = VisibleKeys();
        std::vector<std::string> names = SelectedNames();
        std::erase_if(names, [&](const std::string& name) { return !visible.count(Table::FileUseKey(name)); });
        if (names.empty())
            return;
        const std::uint64_t identity = m_workspace.Core().Table().Identity();
        std::size_t used = 0;
        for (const std::string& name : names)
        {
            if (m_uses.count(Table::FileUseKey(name)))
                ++used;
        }
        std::string text = names.size() == 1 ? "Delete \"" + names.front() + "\" from the table?" : "Delete " + std::to_string(names.size()) + " files from the table?";
        if (used)
            text += names.size() == 1 ? " A script in the table names it." : " Scripts in the table name " + std::to_string(used) + " of them.";
        m_workspace.Confirm(names.size() == 1 ? "Delete table file" : "Delete table files", text, [this, names, identity]()
            {
                if (m_workspace.Core().Table().Identity() == identity)
                    Table::RemoveFiles(m_workspace.Core().Table(), names);
            }, "Delete");
    }

    void TableFilesWindow::CopyNames()
    {
        const std::vector<std::string> names = SelectedNames();
        if (!names.empty())
            ImGui::SetClipboardText(Join(names, "\n").c_str());
    }

    void TableFilesWindow::OpenUse(const Table::FileUse& use)
    {
        Table::AddressList& list = m_workspace.Core().Table();
        switch (use.Where)
        {
        case Table::FileUse::Place::TableScript:
            m_workspace.Run(Command::TableLuaScript);
            break;
        case Table::FileUse::Place::Record:
            if (const Table::MemoryRecord* record = list.FindById(use.RecordId))
                m_workspace.OpenScript(record->Script, record->Id);
            break;
        case Table::FileUse::Place::File:
            if (list.FindFile(use.Label))
                SelectOnly(Table::FileUseKey(use.Label));
            break;
        }
    }

    void TableFilesWindow::StashText()
    {
        if (!m_textKey.empty() && m_textDirty)
            m_drafts[m_textKey] = { m_text, m_textData, m_textVersion };
    }

    void TableFilesWindow::LoadText(Table::TableFile& file, const std::string& key)
    {
        StashText();
        m_textKey = key;
        m_editor = {};
        m_jsonChecked = false;
        m_jsonChangedAt = 0.0;
        m_jsonProblem.reset();
        m_findCounted.clear();
        m_findCount = 0;
        if (auto node = m_drafts.extract(key); !node.empty())
        {
            m_text = std::move(node.mapped().Text);
            m_textData = node.mapped().Data;
            m_textVersion = node.mapped().Version;
            m_textDirty = true;
            m_textStale = m_textData != file.Data.get() || m_textVersion != *file.Version;
            return;
        }
        m_text.assign(file.Data->begin(), file.Data->end());
        m_textData = file.Data.get();
        m_textVersion = *file.Version;
        m_textDirty = false;
        m_textStale = false;
    }

    void TableFilesWindow::ApplyText()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        Table::TableFile* file = m_textKey.empty() || !m_textDirty ? nullptr : list.FindFile(m_textKey);
        if (!file)
            return;
        if (m_textStale || file->Data.get() != m_textData || *file->Version != m_textVersion)
        {
            m_textStale = true;
            m_workspace.Core().Notify(LogLevel::Warning, file->Name + " changed in the table while you were editing it. Choose \"Load the table's version\" or \"Keep mine\" above the text first.");
            return;
        }
        Table::SetFileData(list, *file, std::vector<std::uint8_t>(m_text.begin(), m_text.end()));
        m_textData = file->Data.get();
        m_textVersion = *file->Version;
        m_textDirty = false;
        m_textStale = false;
    }

    void TableFilesWindow::ApplyAll()
    {
        Table::AddressList& list = m_workspace.Core().Table();
        ApplyText();
        std::size_t stale = 0;
        std::string first;
        for (auto it = m_drafts.begin(); it != m_drafts.end();)
        {
            Table::TableFile* file = list.FindFile(it->first);
            if (file && (file->Data.get() != it->second.Data || *file->Version != it->second.Version))
            {
                if (stale++ == 0)
                    first = file->Name;
                ++it;
                continue;
            }
            if (file)
                Table::SetFileData(list, *file, std::vector<std::uint8_t>(it->second.Text.begin(), it->second.Text.end()));
            it = m_drafts.erase(it);
        }
        if (stale > 0)
            m_workspace.Core().Notify(LogLevel::Warning, Count(stale, "file was", "files were") + " left out because " + (stale == 1 ? "it" : "they") +
                                                             " changed in the table after you edited " + (stale == 1 ? "it" : "them") + ". Open " + first + " to choose which version to keep.");
    }

    void TableFilesWindow::RevertText()
    {
        m_drafts.erase(m_textKey);
        m_textKey.clear();
        m_textDirty = false;
        m_textStale = false;
    }

    void TableFilesWindow::CountMatches()
    {
        const Text::TextBuffer& buffer = m_editor.View.Buffer;
        if (m_findCounted == m_find && m_findRevision == buffer.Revision())
            return;
        m_findCounted = m_find;
        m_findRevision = buffer.Revision();
        m_findCount = 0;
        if (m_find.empty())
            return;
        const std::string text = Lower(buffer.Text());
        const std::string needle = Lower(m_find);
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
            ++m_findCount;
    }

    void TableFilesWindow::FindNext(bool backwards)
    {
        Text::TextBuffer& buffer = m_editor.View.Buffer;
        if (m_find.empty() || buffer.Size() == 0)
            return;
        const std::string text = Lower(buffer.Text());
        const std::string needle = Lower(m_find);
        const auto [start, end] = buffer.Selection();
        std::size_t at = std::string::npos;
        if (backwards)
        {
            if (start > 0)
                at = text.rfind(needle, start - 1);
            if (at == std::string::npos)
                at = text.rfind(needle);
        }
        else
        {
            at = text.find(needle, end);
            if (at == std::string::npos)
                at = text.find(needle);
        }
        if (at == std::string::npos)
            return;
        buffer.Select(at, at + needle.size());
        m_editor.View.ScrollToCaret = true;
    }

    void TableFilesWindow::Shortcuts()
    {
        const bool add = Pressed(Command::TableFilesAdd);
        const bool find = Pressed(Command::TableFilesFind);
        const bool exportFiles = Pressed(Command::TableFilesExport);
        const bool apply = Pressed(Command::TableFilesApply);
        const bool copy = Pressed(Command::TableFilesCopyName);
        const bool rename = Pressed(Command::TableFilesRename);
        const bool remove = Pressed(Command::TableFilesDelete);
        const bool all = Pressed(Command::TableFilesSelectAll);
        const bool typing = ImGui::GetIO().WantTextInput || m_editorActive;
        if (add)
            Import();
        if (find)
        {
            if (m_editorActive || (m_findOpen && !m_textKey.empty() && m_textKey == m_current))
            {
                m_findOpen = true;
                m_findFocus = true;
            }
            else
            {
                m_focusFilter = true;
            }
        }
        if (exportFiles)
            Export();
        if (apply)
        {
            if (m_textDirty)
                ApplyText();
            else
                m_workspace.Run(Command::SaveTable);
        }
        if (copy)
            CopyNames();
        if (typing)
            return;
        if (rename)
            StartRename();
        if (remove)
            Delete();
        if (all)
            SelectAll();
    }

    void TableFilesWindow::AddMenuItems()
    {
        if (CommandItem(ICON_FA_PLUS "  Add files...", Command::TableFilesAdd))
            Import();
        ItemTooltip("Copy files from disk into the table. Pick as many as you like; a file with the same name is updated. Dropping files from Explorer onto this window does the same.");
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Add a folder..."))
            ImportFolder();
        ItemTooltip("Copy every file that lies directly in a folder into the table.");
        if (ImGui::MenuItem(ICON_FA_FILE_ALT "  New text file"))
            NewTextFile();
    }

    void TableFilesWindow::FileMenuItems()
    {
        const std::size_t selected = m_selected.size();
        if (ImGui::MenuItem(ICON_FA_FILE_IMPORT "  Replace from disk...", nullptr, false, selected == 1))
            ReplaceFromDisk();
        ItemTooltip("Puts the content of a file on disk into this table file. The name stays.");
        if (CommandItem(selected > 1 ? ICON_FA_FILE_EXPORT "  Export the selected files..." : ICON_FA_FILE_EXPORT "  Export...", Command::TableFilesExport, false, selected > 0))
            Export();
        ItemTooltip(selected > 1 ? "Save copies of the selected files into a folder." : "Save a copy of the file to disk.");
    }

    void TableFilesWindow::EditMenuItems()
    {
        const std::size_t selected = m_selected.size();
        const Table::TableFile* file = Current();
        if (CommandItem(ICON_FA_I_CURSOR "  Rename", Command::TableFilesRename, false, file != nullptr))
            StartRename();
        if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate", nullptr, false, selected > 0))
            Duplicate();
        if (DangerCommandItem(ICON_FA_TRASH "  Delete...", Command::TableFilesDelete, selected > 0))
            Delete();
        ImGui::Separator();
        if (CommandItem(selected > 1 ? ICON_FA_COPY "  Copy the names" : ICON_FA_COPY "  Copy the name", Command::TableFilesCopyName, false, selected > 0))
            CopyNames();
        if (ImGui::BeginMenu(ICON_FA_CODE "  Copy as Lua", file != nullptr))
        {
            if (file)
            {
                const std::string find = Table::LuaFindSnippet(file->Name);
                if (ImGui::MenuItem(find.c_str()))
                    ImGui::SetClipboardText(find.c_str());
                if (ImGui::MenuItem("Read the file into a string"))
                    ImGui::SetClipboardText(Table::LuaReadSnippet(file->Name).c_str());
                ItemTooltip(Table::LuaReadSnippet(file->Name).c_str());
            }
            ImGui::EndMenu();
        }
    }

    void TableFilesWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        const bool any = !m_workspace.Core().Table().Files.empty();
        if (ImGui::BeginMenu("File"))
        {
            AddMenuItems();
            ImGui::Separator();
            FileMenuItems();
            if (ImGui::MenuItem(ICON_FA_FILE_EXPORT "  Export all...", nullptr, false, any))
                ExportAll();
            ImGui::Separator();
            if (CommandItem(ICON_FA_SAVE "  Put the changes into the table", Command::TableFilesApply, false, m_textDirty))
                ApplyText();
            const std::size_t drafts = m_drafts.size() + (m_textDirty && !m_drafts.count(m_textKey) ? 1 : 0);
            if (ImGui::MenuItem(ICON_FA_SAVE "  Put all changes into the table", nullptr, false, drafts > 0))
                ApplyAll();
            ItemTooltip("Every file with a dot in the list has changes that are not in the table yet.");
            if (ImGui::MenuItem(ICON_FA_UNDO "  Revert", nullptr, false, m_textDirty))
                RevertText();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            EditMenuItems();
            ImGui::Separator();
            if (CommandItem(ICON_FA_CHECK_DOUBLE "  Select all", Command::TableFilesSelectAll, false, any))
                SelectAll();
            if (CommandItem(ICON_FA_SEARCH "  Find", Command::TableFilesFind, false, any))
            {
                if (!m_textKey.empty() && m_textKey == m_current)
                {
                    m_findOpen = true;
                    m_findFocus = true;
                }
                else
                {
                    m_focusFilter = true;
                }
            }
            ItemTooltip("In the text of a file while the cursor is in it, otherwise in the list.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            for (int view = 0; view <= static_cast<int>(View::Details); ++view)
            {
                if (ImGui::MenuItem(ViewName(view), nullptr, static_cast<int>(m_view) == view))
                    m_view = static_cast<View>(view);
            }
            ItemTooltip("Automatic picks the view from the content: text in the editor, images as images, everything else as hex.");
            ImGui::Separator();
            ImGui::MenuItem("Fit images into the pane", nullptr, &m_imageFit);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void TableFilesWindow::Draw()
    {
        Collect();
        if (!BeginWindow(ICON_FA_FILE_ARCHIVE "  Table files###tablefiles", ImVec2(980.0f, 600.0f), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar,
                ImVec2(640.0f, 340.0f)))
            return;
        m_viewportWindow = ImGui::GetWindowViewport()->PlatformHandleRaw;
        m_windowMin = ImGui::GetWindowPos();
        m_windowMax = ImVec2(m_windowMin.x + ImGui::GetWindowSize().x, m_windowMin.y + ImGui::GetWindowSize().y);
        m_drawnFrame = ImGui::GetFrameCount();
        m_editorNow = false;
        Sync();
        if (m_rowsDirty)
            RebuildRows();
        if (m_current.empty() && m_selected.empty() && m_filter.empty() && !m_rows.empty())
        {
            SelectOnly(Table::FileUseKey(m_rows.front().Name));
            m_scrollTo.clear();
        }
        StartFacts();
        Shortcuts();
        MenuBar();

        if (m_workspace.Core().Table().Files.empty())
        {
            const int clicked = EmptyState(ICON_FA_FILE_ARCHIVE, m_reading > 0 ? "Reading the files..." : "No table files",
                "Files stored inside the table, for example Lua modules, images or sounds. Scripts read them with findTableFile.",
                { { ICON_FA_PLUS "  Add files", ShortcutText(Command::TableFilesAdd), true }, { ICON_FA_FOLDER_OPEN "  Add a folder" }, { ICON_FA_FILE_ALT "  New text file", nullptr, false, true } }, false,
                "Or drop files from Explorer onto this window.");
            if (clicked == 0)
                Import();
            else if (clicked == 1)
                ImportFolder();
            else if (clicked == 2)
                NewTextFile();
            m_editorActive = false;
            EndToolWindow();
            return;
        }

        const float gap = Px(6.0f);
        const float status = ImGui::GetTextLineHeightWithSpacing();
        const float height = ImGui::GetContentRegionAvail().y - status;
        const float total = ImGui::GetContentRegionAvail().x;
        const float minList = Px(240.0f);
        const float minPreview = Px(300.0f);
        if (m_listWidth <= 0.0f)
            m_listWidth = Px(400.0f);
        m_listWidth = std::round(std::clamp(m_listWidth, minList, std::max(minList, total - minPreview - gap)));
        float first = m_listWidth;
        float second = total - first - gap;
        const ImVec2 start = ImGui::GetCursorPos();
        List(first, height);
        ImGui::SetCursorPos(start);
        if (Splitter("##filesplit", true, gap, &first, &second, minList, minPreview, height))
            m_listWidth = first;
        ImGui::SetCursorPos(ImVec2(start.x + first + gap, start.y));
        Preview(height);
        ImGui::SetCursorPos(ImVec2(start.x, start.y + height + ImGui::GetStyle().ItemSpacing.y * 0.5f));
        StatusLine();
        m_editorActive = m_editorNow;
        EndToolWindow();
    }

    void TableFilesWindow::StatusLine()
    {
        const std::size_t files = m_workspace.Core().Table().Files.size();
        std::string line = m_rows.size() == files ? Count(files, "file", "files") : Format("%zu of %zu files", m_rows.size(), files);
        line += "  ·  " + ByteSize(m_totalSize);
        if (!m_storedPending)
            line += "  ·  " + ByteSize(static_cast<std::uint64_t>(m_totalStored)) + " in the table";
        if (m_selected.size() > 1)
            line += Format("  ·  %zu selected", m_selected.size());
        const std::size_t drafts = m_drafts.size() + (m_textDirty && !m_drafts.count(m_textKey) ? 1 : 0);
        if (drafts)
            line += "  ·  " + Count(drafts, "file has", "files have") + " changes that are not in the table yet";
        if (m_reading > 0)
        {
            Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
            ImGui::SameLine(0.0f, Px(6.0f));
            line = "Reading files from disk  ·  " + line;
        }
        Muted(line);
    }

    void TableFilesWindow::List(float width, float height)
    {
        BeginCard("##filelist", nullptr, ImVec2(width, height), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float button = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
        if (m_focusFilter)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusFilter = false;
        }
        if (InputTextWithHint("##filefilter", ICON_FA_SEARCH " Filter by name or type", m_filter))
        {
            m_rowsDirty = true;
            RebuildRows();
            const std::set<std::string> visible = VisibleKeys();
            std::erase_if(m_selected, [&](const std::string& key) { return !visible.count(key); });
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_PLUS, "Add files"))
            Import();

        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                                      ImGuiTableFlags_SortTristate | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##files", 5, flags, ImVec2(0.0f, 0.0f)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 1.0f, static_cast<ImGuiID>(Table::FileColumn::Name));
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(70.0f), static_cast<ImGuiID>(Table::FileColumn::Kind));
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, Px(64.0f), static_cast<ImGuiID>(Table::FileColumn::Size));
            ImGui::TableSetupColumn("In table", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_DefaultHide, Px(64.0f),
                static_cast<ImGuiID>(Table::FileColumn::Stored));
            ImGui::TableSetupColumn("Used", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, Px(44.0f), static_cast<ImGuiID>(Table::FileColumn::Uses));
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty)
            {
                m_sort = specs->SpecsCount > 0 ? static_cast<Table::FileColumn>(specs->Specs[0].ColumnUserID) : Table::FileColumn::Order;
                m_descending = specs->SpecsCount > 0 && specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                specs->SpecsDirty = false;
                RebuildRows();
            }
            int scrollTo = -1;
            if (!m_scrollTo.empty())
            {
                for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
                {
                    if (Table::FileUseKey(m_rows[static_cast<std::size_t>(i)].Name) == m_scrollTo)
                        scrollTo = i;
                }
                if (scrollTo < 0)
                    m_scrollTo.clear();
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_rows.size()));
            if (scrollTo >= 0)
                clipper.IncludeItemByIndex(scrollTo);
            while (clipper.Step())
            {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd && index < static_cast<int>(m_rows.size()); ++index)
                    Row(index);
            }
            if (m_rows.empty())
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Muted("No file matches the filter.");
            }
            if (ImGui::BeginPopupContextWindow("##listmenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
            {
                AddMenuItems();
                ImGui::EndPopup();
            }
            ImGui::EndTable();
        }
        EndCard();
    }

    void TableFilesWindow::RenameField(const Table::FileRow& row)
    {
        Table::AddressList& list = m_workspace.Core().Table();
        const std::string problem = Table::FileNameProblem(list, m_rename, row.Name);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_renameFocus)
            ImGui::SetKeyboardFocusHere();
        if (!problem.empty())
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.22f));
        const bool entered = InputText("##rename", m_rename, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (!problem.empty())
            ImGui::PopStyleColor();
        const bool active = ImGui::IsItemActive();
        if (!problem.empty() && (active || ImGui::IsItemHovered()))
            ImGui::SetTooltip("%s", problem.c_str());
        if (m_renameFocus)
        {
            if (active)
                m_renameFocus = false;
            return;
        }
        if (entered)
        {
            if (problem.empty())
                CommitRename();
            else
                m_renameFocus = true;
            return;
        }
        if (!ImGui::IsItemDeactivated())
            return;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || !problem.empty())
            m_renaming.clear();
        else
            CommitRename();
    }

    void TableFilesWindow::Row(int index)
    {
        const Table::FileRow row = m_rows[static_cast<std::size_t>(index)];
        const std::string key = Table::FileUseKey(row.Name);
        const bool selected = m_selected.count(key) > 0;
        ImGui::PushID(row.Name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (m_scrollTo == key)
        {
            ImGui::SetScrollHereY(0.5f);
            m_scrollTo.clear();
        }
        if (m_renaming == key)
        {
            RenameField(row);
        }
        else
        {
            const std::string label = std::string(KindIcon(row.Kind)) + "  " + row.Name;
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                Click(index, io.KeyCtrl, io.KeyShift);
            else if (ImGui::IsItemFocused() && !selected && ImGui::GetCurrentContext()->NavJustMovedToId == ImGui::GetItemID())
                Click(index, false, io.KeyShift);
            if (key == m_current && selected && m_selected.size() > 1)
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.55f)), Px(3.0f));
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                SelectOnly(key);
                StartRename();
            }
            if (ImGui::BeginPopupContextItem("##filemenu"))
            {
                if (!selected)
                    SelectOnly(key);
                else
                    m_current = key;
                EditMenuItems();
                ImGui::Separator();
                FileMenuItems();
                ImGui::EndPopup();
            }
            if (HasDraft(key))
            {
                const ImVec2 min = ImGui::GetItemRectMin();
                const float cell = ImGui::GetColumnWidth();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(min.x + cell - Px(8.0f), (min.y + ImGui::GetItemRectMax().y) * 0.5f), Px(3.0f), ImGui::GetColorU32(ManifoldTheme::Warning));
            }
        }
        ImGui::TableNextColumn();
        Muted(Table::FileKindName(row.Kind));
        ImGui::TableNextColumn();
        RightAligned(ByteSize(row.Size), ManifoldTheme::Text);
        ImGui::TableNextColumn();
        RightAligned(row.Stored < 0 ? std::string("...") : ByteSize(static_cast<std::uint64_t>(row.Stored)), ManifoldTheme::Muted);
        ImGui::TableNextColumn();
        if (row.Uses > 0)
        {
            RightAligned(std::to_string(row.Uses), ManifoldTheme::Accent);
            if (ImGui::IsItemHovered())
            {
                if (const auto uses = m_uses.find(key); uses != m_uses.end())
                {
                    std::string text = "Named in:";
                    for (const Table::FileUse& use : uses->second)
                        text += "\n  " + use.Label + ", line " + std::to_string(use.Line);
                    ImGui::SetTooltip("%s", text.c_str());
                }
            }
        }
        else
        {
            RightAligned("-", ManifoldTheme::Muted);
        }
        ImGui::PopID();
    }

    void TableFilesWindow::Preview(float height)
    {
        BeginCard("##filedetail", nullptr, ImVec2(0.0f, height), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Table::TableFile* file = Current();
        const Info* info = file ? InfoOf(m_current) : nullptr;
        if (!file || !info)
        {
            EmptyState(ICON_FA_FILE, "No file selected", "Pick a file on the left to see, edit or export it.", {}, false);
            EndCard();
            return;
        }
        const bool text = Table::IsTextKind(info->Kind) && info->Size <= kEditBytes;
        const bool image = info->Kind == Table::FileKind::Image;
        std::vector<View> views;
        if (text || (Table::LooksLikeText(*file->Data) && info->Size <= kEditBytes))
            views.push_back(View::Text);
        if (image)
            views.push_back(View::Image);
        views.push_back(View::Hex);
        views.push_back(View::Details);
        View view = m_view;
        if (std::find(views.begin(), views.end(), view) == views.end())
            view = text ? View::Text : image ? View::Image : View::Hex;
        PreviewHeader(*file, *info, view, views);
        switch (view)
        {
        case View::Text: TextView(*file, *info); break;
        case View::Image: ImageView(*file, *info); break;
        case View::Details: DetailsView(*file, *info); break;
        default: HexView(*file); break;
        }
        EndCard();
    }

    void TableFilesWindow::PreviewHeader(Table::TableFile& file, const Info& info, View view, const std::vector<View>& views)
    {
        std::vector<const char*> names;
        int chosen = 0;
        for (std::size_t i = 0; i < views.size(); ++i)
        {
            names.push_back(ViewName(static_cast<int>(views[i])));
            if (views[i] == view)
                chosen = static_cast<int>(i);
        }
        float switchWidth = 0.0f;
        for (const char* name : names)
            switchWidth += ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 2.0f + Px(8.0f);
        const float room = ImGui::GetContentRegionAvail().x - switchWidth - Px(12.0f);
        const float top = ImGui::GetCursorPosY();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
        ImGui::TextUnformatted(KindIcon(info.Kind));
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Px(8.0f));
        std::string facts = std::string(Table::FileKindName(info.Kind)) + "  ·  " + ByteSize(info.Size);
        if (const auto uses = m_uses.find(m_current); uses != m_uses.end())
            facts += "  ·  named in " + Count(uses->second.size(), "script", "scripts");
        const float factsWidth = ImGui::CalcTextSize(facts.c_str()).x + Px(10.0f);
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        const float used = ImGui::GetCursorPosX() - ImGui::GetStyle().WindowPadding.x;
        TextFitted(file.Name, std::max(Px(60.0f), room - used - factsWidth), ManifoldTheme::Text);
        ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", file.Name.c_str());
        ImGui::SameLine(0.0f, Px(10.0f));
        TextFitted(facts, std::max(0.0f, room - (ImGui::GetCursorPosX() - ImGui::GetStyle().WindowPadding.x)), ManifoldTheme::Muted);
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x + ImGui::GetContentRegionMax().x - ImGui::GetStyle().WindowPadding.x - switchWidth, top));
        if (Segmented("##fileview", &chosen, names, switchWidth))
            m_view = views[static_cast<std::size_t>(chosen)];
        ImGui::Spacing();
    }

    void TableFilesWindow::FindBar()
    {
        CountMatches();
        ImGui::SetNextItemWidth(std::min(Px(260.0f), ImGui::GetContentRegionAvail().x * 0.5f));
        if (m_findFocus)
        {
            ImGui::SetKeyboardFocusHere();
            m_findFocus = false;
        }
        const bool entered = InputTextWithHint("##find", ICON_FA_SEARCH " Find in the file", m_find, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool left = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (entered)
        {
            FindNext(ImGui::GetIO().KeyShift);
            m_findFocus = true;
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_CHEVRON_UP, "Previous match (Shift+Enter)"))
            FindNext(true);
        ImGui::SameLine();
        if (IconButton(ICON_FA_CHEVRON_DOWN, "Next match (Enter)"))
            FindNext(false);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Muted(m_find.empty() ? std::string("Type to search") : m_findCount == 0 ? std::string("No match") : Count(static_cast<std::size_t>(m_findCount), "match", "matches"));
        ImGui::SameLine();
        if (IconButton(ICON_FA_TIMES, "Close the search") || left)
        {
            m_findOpen = false;
            m_editor.Focus = true;
        }
    }

    void TableFilesWindow::TextView(Table::TableFile& file, const Info& info)
    {
        if (m_textKey != m_current)
            LoadText(file, m_current);
        const bool json = info.Kind == Table::FileKind::Json;
        const float row = ImGui::GetFrameHeightWithSpacing();
        if (m_findOpen)
            FindBar();
        if (m_textStale)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_EXCLAMATION_TRIANGLE "  The file changed in the table while you were editing.");
            ImGui::SameLine();
            if (ImGui::Button("Load the table's version"))
            {
                RevertText();
                LoadText(file, m_current);
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep mine"))
            {
                m_textData = file.Data.get();
                m_textVersion = *file.Version;
                m_textStale = false;
            }
        }
        const float footer = row + Px(4.0f) + (json ? ImGui::GetTextLineHeightWithSpacing() : 0.0f);
        if (CodeEditor("##filetext", m_text, LanguageOf(info.Kind), ImVec2(0.0f, ImGui::GetContentRegionAvail().y - footer), m_editor))
        {
            m_textDirty = true;
            m_jsonChecked = false;
            m_jsonChangedAt = ImGui::GetTime();
        }
        m_editorNow = m_editor.Active;
        if (json && !m_jsonChecked && ImGui::GetTime() - m_jsonChangedAt > 0.25)
        {
            m_jsonProblem = Text::CheckJson(m_text);
            m_jsonChecked = true;
            m_editor.ErrorLine = m_jsonProblem ? m_jsonProblem->Line : 0;
        }
        if (json)
        {
            JsonStatus();
            if (ImGui::Button(ICON_FA_ALIGN_LEFT "  Format"))
                ReformatJson(2);
            ItemTooltip("Puts every value on its own line, indented two spaces per level. Strings and numbers stay exactly as they are.");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_COMPRESS_ALT "  Compact"))
                ReformatJson(0);
            ItemTooltip("Removes all spaces and line breaks between the values.");
            ImGui::SameLine(0.0f, Px(14.0f));
        }
        ImGui::BeginDisabled(!m_textDirty);
        if (AccentButton(ICON_FA_SAVE "  Put into the table"))
            ApplyText();
        if (const char* keys = ShortcutText(Command::TableFilesApply))
            ItemTooltip(keys);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO "  Revert"))
            RevertText();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (m_textDirty)
            ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_CIRCLE "  Changes are not in the table yet");
        else
            Muted("Line " + std::to_string(m_editor.CursorLine) + ", column " + std::to_string(m_editor.CursorColumn) + "  ·  " + Count(static_cast<std::size_t>(m_editor.LineCount), "line", "lines"));
    }

    void TableFilesWindow::ImageView(Table::TableFile& file, const Info& info)
    {
        if (m_image.Key != m_current || m_image.Data != info.Data || m_image.Version != info.Version)
            StartImage(file, m_current);
        if (m_image.Loading)
        {
            TaskStatus("Decoding the image...", false);
            return;
        }
        if (m_image.Failed || !m_image.Texture)
        {
            EmptyState(ICON_FA_FILE_IMAGE, "No picture to show",
                "This image format cannot be shown here (PNG, JPEG, BMP, GIF and TGA can), or the file is damaged or too large. The Hex and Details views still work.", {}, false);
            return;
        }
        const float line = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##image", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - line), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 room = ImGui::GetContentRegionAvail();
        float scale = 1.0f;
        if (m_imageFit)
            scale = std::min(1.0f, std::min(room.x / static_cast<float>(m_image.Width), room.y / static_cast<float>(m_image.Height)));
        const ImVec2 size(std::max(1.0f, std::floor(static_cast<float>(m_image.Width) * scale)), std::max(1.0f, std::floor(static_cast<float>(m_image.Height) * scale)));
        const ImVec2 offset(std::max(0.0f, std::floor((room.x - size.x) * 0.5f)), std::max(0.0f, std::floor((room.y - size.y) * 0.5f)));
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + offset.x, ImGui::GetCursorPosY() + offset.y));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float cell = Px(8.0f);
        const ImU32 light = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.10f));
        const ImU32 dark = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.04f));
        draw->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), dark);
        draw->PushClipRect(min, ImVec2(min.x + size.x, min.y + size.y), true);
        const ImVec2 clipMin = draw->GetClipRectMin();
        const ImVec2 clipMax = draw->GetClipRectMax();
        const int firstRow = std::max(0, static_cast<int>((clipMin.y - min.y) / cell));
        const int lastRow = static_cast<int>((std::min(clipMax.y, min.y + size.y) - min.y) / cell);
        const int firstColumn = std::max(0, static_cast<int>((clipMin.x - min.x) / cell));
        const int lastColumn = static_cast<int>((std::min(clipMax.x, min.x + size.x) - min.x) / cell);
        for (int y = firstRow; y <= lastRow; ++y)
        {
            for (int x = firstColumn + ((y + firstColumn) % 2 ? 0 : 1); x <= lastColumn; x += 2)
                draw->AddRectFilled(ImVec2(min.x + static_cast<float>(x) * cell, min.y + static_cast<float>(y) * cell), ImVec2(min.x + static_cast<float>(x + 1) * cell, min.y + static_cast<float>(y + 1) * cell), light);
        }
        draw->PopClipRect();
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(m_image.Texture))), size);
        ImGui::EndChild();
        Muted(Format("%d x %d pixels  ·  shown at %d %%", m_image.Width, m_image.Height, static_cast<int>(std::round(scale * 100.0f))));
        ImGui::SameLine(0.0f, Px(12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
        if (ImGui::SmallButton(m_imageFit ? "Actual size" : "Fit"))
            m_imageFit = !m_imageFit;
        ImGui::PopStyleVar();
    }

    void TableFilesWindow::HexView(const Table::TableFile& file)
    {
        const std::vector<std::uint8_t>& data = *file.Data;
        if (data.empty())
        {
            EmptyState(ICON_FA_FILE, "The file is empty", "It has no bytes yet. Replace it from disk, or let a script write to its stream.", {}, false);
            return;
        }
        const std::size_t shown = std::min(data.size(), kHexBytes);
        if (shown < data.size())
            Muted("Showing the first " + ByteSize(shown) + " of " + ByteSize(data.size()) + ". Export the file to see all of it.");
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
                        const std::uint8_t value = data[i];
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

    void TableFilesWindow::DetailsView(Table::TableFile& file, const Info& info)
    {
        ImGui::BeginChild("##details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        if (ImGui::BeginTable("##facts", 2, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, Px(110.0f));
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            Fact("Name", file.Name);
            Fact("Type", Table::FileKindName(info.Kind));
            Fact("Size", ByteSize(info.Size) + (info.Size >= 1024 ? " (" + Grouped(info.Size) + " bytes)" : std::string()));
            if (info.Stored < 0)
                Fact("In the table", "Measuring...");
            else
                Fact("In the table", ByteSize(static_cast<std::uint64_t>(info.Stored)) + (info.Size > 0 ? Format(", %.0f %% of the size", static_cast<double>(info.Stored) * 100.0 / static_cast<double>(info.Size)) : std::string()) +
                                         "; packed and written as text");
            if (info.Kind == Table::FileKind::Image && m_image.Key == m_current && m_image.Texture)
                Fact("Picture", Format("%d x %d pixels", m_image.Width, m_image.Height));
            Fact("SHA-256", info.Sha256.empty() ? std::string("Measuring...") : info.Sha256, true);
            const std::size_t head = std::min<std::size_t>(file.Data->size(), 32);
            Fact("First bytes", head ? HexBytes(std::span<const std::uint8_t>(file.Data->data(), head)) : std::string("none"), true);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (!info.Sha256.empty())
        {
            if (ImGui::Button(ICON_FA_COPY "  Copy the SHA-256"))
                ImGui::SetClipboardText(info.Sha256.c_str());
            ImGui::SameLine();
        }
        const std::string find = Table::LuaFindSnippet(file.Name);
        if (ImGui::Button((ICON_FA_CODE "  Copy " + find).c_str()))
            ImGui::SetClipboardText(find.c_str());
        ImGui::Spacing();
        ImGui::Spacing();
        const auto uses = m_uses.find(m_current);
        if (uses == m_uses.end())
        {
            SectionLabel("NAMED IN");
            Muted("No script in the table names this file. A script that builds the name at run time is not found by this search.");
        }
        else
        {
            SectionLabel("NAMED IN");
            const std::vector<Table::FileUse> list = uses->second;
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                const Table::FileUse& use = list[i];
                ImGui::PushID(static_cast<int>(i));
                const char* icon = use.Where == Table::FileUse::Place::File ? ICON_FA_FILE_CODE : use.Where == Table::FileUse::Place::Record ? ICON_FA_SCROLL : ICON_FA_CODE;
                const std::string label = std::string(icon) + "  " + use.Label + "  ·  line " + std::to_string(use.Line);
                if (ImGui::Selectable(label.c_str()))
                    OpenUse(use);
                ItemTooltip(use.Where == Table::FileUse::Place::File ? "Show this table file" : use.Where == Table::FileUse::Place::Record ? "Open the script of this record" : "Open the table Lua script");
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void TableFilesWindow::ReformatJson(int indent)
    {
        const Text::JsonFormatted formatted = Text::FormatJson(m_text, indent);
        if (formatted.Problem)
        {
            m_jsonProblem = formatted.Problem;
            m_jsonChecked = true;
            m_editor.ErrorLine = formatted.Problem->Line;
            ShowJsonProblem(*formatted.Problem);
            return;
        }
        if (formatted.Text == m_text)
            return;
        Text::TextBuffer& buffer = m_editor.View.Buffer;
        buffer.BreakUndoGroup();
        buffer.Replace(0, buffer.Size(), formatted.Text);
        buffer.BreakUndoGroup();
        buffer.SetCaret(0, false);
        m_editor.View.ScrollToCaret = true;
        m_text = buffer.Text();
        m_textDirty = true;
        m_jsonProblem.reset();
        m_jsonChecked = true;
        m_editor.ErrorLine = 0;
    }

    void TableFilesWindow::ShowJsonProblem(const Text::JsonProblem& problem)
    {
        m_editor.View.Buffer.SetCaret(std::min(problem.Offset, m_editor.View.Buffer.Size()), false);
        m_editor.View.ScrollToCaret = true;
        m_editor.Focus = true;
    }

    void TableFilesWindow::JsonStatus()
    {
        if (!m_jsonChecked)
        {
            Muted("Checking the JSON...");
            return;
        }
        if (!m_jsonProblem)
        {
            ImGui::TextColored(ManifoldTheme::Success, ICON_FA_CHECK_CIRCLE "  Valid JSON");
            return;
        }
        const std::string text = ICON_FA_EXCLAMATION_TRIANGLE "  Line " + std::to_string(m_jsonProblem->Line) + ", column " + std::to_string(m_jsonProblem->Column) + ": " + m_jsonProblem->Message;
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
        const bool clicked = ImGui::Selectable(text.c_str(), false, ImGuiSelectableFlags_None, ImVec2(ImGui::CalcTextSize(text.c_str()).x, 0.0f));
        ImGui::PopStyleColor();
        ItemTooltip("Not valid JSON. Click to put the cursor where the problem is.");
        if (clicked)
            ShowJsonProblem(*m_jsonProblem);
    }
}
