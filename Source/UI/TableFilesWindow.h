#pragma once

#include "Core/ImageDecode.h"
#include "Table/TableFiles.h"
#include "Text/Json.h"
#include "UI/CodeEditor.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class TableFilesWindow : public ToolWindowBase
    {
    public:
        explicit TableFilesWindow(Workspace& workspace);
        ~TableFilesWindow() override;

        void Draw() override;
        bool TakesDrop(void* window, float x, float y) const;
        void AddFiles(const std::vector<std::filesystem::path>& paths);

    private:
        enum class View
        {
            Automatic,
            Text,
            Image,
            Hex,
            Details
        };

        struct Info
        {
            const void* Data = nullptr;
            std::uint64_t Version = 0;
            std::size_t Size = 0;
            Table::FileKind Kind = Table::FileKind::Binary;
            std::int64_t Stored = -1;
            std::string Sha256;
            bool Seen = false;
        };

        struct Draft
        {
            std::string Text;
            const void* Data = nullptr;
            std::uint64_t Version = 0;
        };

        struct FactsResult
        {
            std::uint64_t Identity = 0;
            std::string Key;
            const void* Data = nullptr;
            std::uint64_t Version = 0;
            std::size_t Stored = 0;
            std::string Sha256;
        };

        struct ImageResult
        {
            std::uint64_t Identity = 0;
            std::string Key;
            const void* Data = nullptr;
            std::uint64_t Version = 0;
            std::optional<DecodedImage> Image;
        };

        struct LoadedFile
        {
            std::string Name;
            std::vector<std::uint8_t> Bytes;
        };

        struct AddResult
        {
            std::uint64_t Identity = 0;
            std::vector<LoadedFile> Files;
            std::vector<std::string> Failed;
            std::vector<std::string> TooLarge;
            std::string Replace;
        };

        struct Results
        {
            std::vector<FactsResult> Facts;
            std::vector<ImageResult> Images;
            std::vector<AddResult> Added;
        };

        struct ImageState
        {
            std::string Key;
            const void* Data = nullptr;
            std::uint64_t Version = 0;
            void* Texture = nullptr;
            int Width = 0;
            int Height = 0;
            bool Loading = false;
            bool Failed = false;
        };

        bool Busy() const override { return m_reading > 0; }
        bool EscapeCloses() const override { return !m_editorActive && m_renaming.empty(); }

        void Sync();
        void TableChanged();
        void Collect();
        void RebuildRows();
        void StartFacts();
        void Shortcuts();
        void MenuBar();
        void FileMenuItems();
        void EditMenuItems();
        void AddMenuItems();
        void List(float width, float height);
        void Row(int index);
        void RenameField(const Table::FileRow& row);
        void Preview(float height);
        void PreviewHeader(Table::TableFile& file, const Info& info, View view, const std::vector<View>& views);
        void TextView(Table::TableFile& file, const Info& info);
        void FindBar();
        void ImageView(Table::TableFile& file, const Info& info);
        void HexView(const Table::TableFile& file);
        void DetailsView(Table::TableFile& file, const Info& info);
        void StatusLine();

        void Click(int index, bool toggle, bool range);
        void SelectOnly(const std::string& key);
        void SelectAll();
        std::vector<std::string> SelectedNames() const;
        std::set<std::string> VisibleKeys() const;
        Table::TableFile* Current();
        const Info* InfoOf(const std::string& key) const;
        bool HasDraft(const std::string& key) const;

        void Import();
        void ImportFolder();
        void NewTextFile();
        void ReplaceFromDisk();
        void Read(std::vector<std::filesystem::path> paths, std::string replace);
        void Apply(AddResult& result);
        void Export();
        void ExportAll();
        void ExportTo(const std::vector<std::string>& names);
        void WriteFiles(const std::vector<std::string>& names, const std::filesystem::path& folder);
        void StartRename();
        void CommitRename();
        void Duplicate();
        void Delete();
        void CopyNames();
        void OpenUse(const Table::FileUse& use);

        void LoadText(Table::TableFile& file, const std::string& key);
        void StashText();
        void ApplyText();
        void ApplyAll();
        void RevertText();
        void FindNext(bool backwards);
        void CountMatches();
        void ReformatJson(int indent);
        void ShowJsonProblem(const Text::JsonProblem& problem);
        void JsonStatus();
        void ReleaseImage();
        void StartImage(const Table::TableFile& file, const std::string& key);

        Workspace& m_workspace;
        std::shared_ptr<Results> m_results = std::make_shared<Results>();
        std::uint64_t m_identity = 0;
        std::map<std::string, Info> m_info;
        std::vector<std::string> m_names;
        std::vector<Table::FileRow> m_rows;
        bool m_rowsDirty = true;
        Table::FileUses m_uses;
        bool m_usesDirty = true;
        std::uint64_t m_usesGeneration = 0;
        double m_usesAt = -10.0;
        bool m_factsRunning = false;
        int m_reading = 0;
        std::size_t m_totalSize = 0;
        std::int64_t m_totalStored = 0;
        bool m_storedPending = false;

        std::string m_filter;
        bool m_focusFilter = false;
        Table::FileColumn m_sort = Table::FileColumn::Order;
        bool m_descending = false;
        std::set<std::string> m_selected;
        std::string m_current;
        std::string m_anchor;
        std::string m_scrollTo;
        std::string m_renaming;
        std::string m_rename;
        bool m_renameFocus = false;
        float m_listWidth = 0.0f;

        View m_view = View::Automatic;
        std::string m_text;
        std::string m_textKey;
        const void* m_textData = nullptr;
        std::uint64_t m_textVersion = 0;
        bool m_textDirty = false;
        bool m_textStale = false;
        std::map<std::string, Draft> m_drafts;
        CodeEditorState m_editor;
        bool m_editorActive = false;
        bool m_editorNow = false;
        bool m_findOpen = false;
        bool m_findFocus = false;
        std::string m_find;
        std::string m_findCounted;
        std::uint64_t m_findRevision = 0;
        int m_findCount = 0;
        bool m_jsonChecked = false;
        double m_jsonChangedAt = 0.0;
        std::optional<Text::JsonProblem> m_jsonProblem;
        ImageState m_image;
        bool m_imageFit = true;

        void* m_viewportWindow = nullptr;
        ImVec2 m_windowMin;
        ImVec2 m_windowMax;
        int m_drawnFrame = -1;
    };
}
