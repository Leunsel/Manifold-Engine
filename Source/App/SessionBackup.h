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
        std::wstring StampName(std::chrono::system_clock::time_point time);
        bool IsVersion(const std::filesystem::path& file);
        std::string When(const std::filesystem::path& file);
        std::vector<std::filesystem::path> Versions(const std::filesystem::path& folder);
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
        const std::string& RestoredName() const { return m_restored; }
        std::filesystem::path SessionFolder() const;
        const std::filesystem::path& LastFile() const { return m_lastFile; }
        std::vector<Backups::Abandoned> TakeAbandoned();

    private:
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
