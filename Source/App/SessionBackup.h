#pragma once

#include "Core/Errors.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold
{
    class Engine;
    struct Settings;

    namespace Backups
    {
        inline constexpr char kFallbackName[] = "session";
        inline constexpr std::size_t kMaxNameLength = 80;

        struct Abandoned
        {
            std::filesystem::path File;
            std::string Name;
        };

        std::filesystem::path Folder(const Settings& settings);
        std::string SafeName(std::string_view name);
        std::string SessionName(std::string_view tablePath, std::string_view processName);
        // ∑ Two tables with the same file name in different folders (or a table
        //   that moved) must not share one backup folder, where each would prune
        //   the other's versions: a saved table's folder carries a short hash of
        //   its full path after a '~'. Folders of earlier versions (name only) are
        //   still listed but never written to again for that table.
        std::string PathKey(std::string_view tablePath);
        std::string FolderName(std::string_view tablePath, std::string_view processName);
        std::string ShownName(std::string_view folderName);
        std::wstring StampName(std::chrono::system_clock::time_point time);
        bool IsVersion(const std::filesystem::path& file);
        std::string When(const std::filesystem::path& file);
        std::vector<std::filesystem::path> Versions(const std::filesystem::path& folder);
        std::vector<std::filesystem::path> Versions(const std::vector<std::filesystem::path>& folders);
        int Prune(const std::filesystem::path& folder, int keep, const std::filesystem::path& newest = {});
        std::filesystem::path Write(const std::filesystem::path& folder, std::string_view text, std::chrono::system_clock::time_point time);
        std::wstring InstanceMutexName(unsigned long processId);
        std::filesystem::path MarkerFile(const std::filesystem::path& folder, unsigned long processId);
        std::vector<Abandoned> TakeAbandoned(const std::filesystem::path& folder, unsigned long ownProcessId);
    }

    class SessionBackup
    {
    public:
        explicit SessionBackup(Engine& engine);
        ~SessionBackup();

        SessionBackup(const SessionBackup&) = delete;
        SessionBackup& operator=(const SessionBackup&) = delete;

        void Tick(double now);
        Status BackUpNow(bool* written = nullptr);
        void Finish();
        void TableReplaced();
        void Restored(const std::filesystem::path& file);
        std::string Name() const;
        std::string RestoredName() const { return Backups::ShownName(m_restored); }
        std::filesystem::path SessionFolder() const;
        std::filesystem::path LegacyFolder() const;
        std::vector<std::filesystem::path> Versions() const;
        const std::filesystem::path& LastFile() const { return m_lastFile; }
        std::vector<Backups::Abandoned> TakeAbandoned();

    private:
        std::string FolderName() const;
        void NoteProcess();
        void WriteMarker(const std::filesystem::path& file, const std::string& name);
        void DropMarker();

        Engine& m_engine;
        void* m_instance = nullptr;
        std::string m_process;
        std::string m_restored;
        std::filesystem::path m_lastFile;
        std::size_t m_lastHash = 0;
        std::size_t m_lastSize = 0;
        bool m_hasLast = false;
        bool m_marked = false;
        bool m_failed = false;
        double m_lastAt = -1.0;
    };
}
