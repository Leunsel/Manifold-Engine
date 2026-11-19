#include "Core/Paths.h"

#include "Core/Errors.h"
#include "Core/Text.h"

#include <optional>
#include <system_error>

#include <windows.h>

namespace Manifold
{
    namespace
    {
        constexpr wchar_t kConfigFolder[] = L"configs";
        constexpr wchar_t kSettingsName[] = L"Manifold-Engine.ini";
        constexpr wchar_t kLayoutName[] = L"Manifold-Engine.layout.ini";
        constexpr wchar_t kScriptSettingsName[] = L"Manifold-Engine.scripts.ini";
        constexpr wchar_t kTablesFolder[] = L"tables";
        constexpr wchar_t kSoundsFolder[] = L"sounds";
        constexpr wchar_t kLogFolder[] = L"logs";
        constexpr wchar_t kLogName[] = L"Manifold-Engine.log";
        constexpr const wchar_t* kConfigNames[] = { kSettingsName, kLayoutName, kScriptSettingsName };
        constexpr const wchar_t* kLogNames[] = { kLogName, L"Manifold-Engine.previous.log", L"Manifold-CheatEngine.log", L"Manifold-CheatEngine.previous.log" };

        std::filesystem::path& DataFolderOverride()
        {
            static std::filesystem::path folder;
            return folder;
        }

        bool Present(const std::filesystem::path& path)
        {
            return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        struct DiskEntry
        {
            std::wstring Name;
            bool Folder = false;
        };

        std::optional<DiskEntry> EntryOnDisk(const std::filesystem::path& path)
        {
            WIN32_FIND_DATAW data{};
            const HANDLE find = ::FindFirstFileExW(path.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
            if (find == INVALID_HANDLE_VALUE)
                return std::nullopt;
            ::FindClose(find);
            return DiskEntry{ data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 };
        }

        void RenameFolder(const std::filesystem::path& programFolder, const wchar_t* earlier, const wchar_t* wanted, Paths::Migration& migration)
        {
            const std::string earlierText = WideToUtf8(earlier);
            const std::string wantedText = WideToUtf8(wanted);
            const std::optional<DiskEntry> current = EntryOnDisk(programFolder / wanted);
            if (current && current->Name == wanted)
            {
                const std::optional<DiskEntry> old = EntryOnDisk(programFolder / earlier);
                if (old && old->Name == earlier)
                    migration.Problems.push_back("The folders " + earlierText + " and " + wantedText + " both exist; " + wantedText + " is used, " + earlierText + " is left as it is");
                return;
            }
            const std::optional<DiskEntry> source = current ? current : EntryOnDisk(programFolder / earlier);
            if (!source)
                return;
            const std::string sourceText = WideToUtf8(source->Name);
            if (!source->Folder)
            {
                migration.Problems.push_back(sourceText + " is a file, not a folder; it was not renamed to " + wantedText);
                return;
            }
            std::filesystem::path between;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                between = programFolder / (std::wstring(wanted) + L".renaming" + (attempt ? std::to_wstring(attempt) : std::wstring()));
                if (!Present(between))
                    break;
                between.clear();
            }
            const std::filesystem::path from = programFolder / source->Name;
            const std::filesystem::path to = programFolder / wanted;
            if (between.empty() || !::MoveFileExW(from.c_str(), between.c_str(), 0))
            {
                migration.Problems.push_back("The folder " + sourceText + " could not be renamed to " + wantedText + ": " + SystemErrorText(::GetLastError()));
                return;
            }
            if (!::MoveFileExW(between.c_str(), to.c_str(), 0))
            {
                const DWORD error = ::GetLastError();
                const bool restored = ::MoveFileExW(between.c_str(), from.c_str(), 0) != FALSE;
                migration.Problems.push_back("The folder " + sourceText + " could not be renamed to " + wantedText + ": " + SystemErrorText(error) +
                    (restored ? std::string() : "; it is now called " + WideToUtf8(between.filename().wstring())));
                return;
            }
            migration.Moved.push_back("The folder " + sourceText + " is now called " + wantedText);
        }
    }

    namespace Paths
    {
        std::filesystem::path ProgramFolder()
        {
            std::wstring program(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = ::GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()));
                if (length == 0)
                    return std::filesystem::current_path();
                if (length < program.size())
                {
                    program.resize(length);
                    break;
                }
                program.resize(program.size() * 2);
            }
            return std::filesystem::path(program).parent_path();
        }

        std::filesystem::path DataFolder()
        {
            const std::filesystem::path& data = DataFolderOverride();
            return data.empty() ? ProgramFolder() : data;
        }

        void SetDataFolder(std::filesystem::path folder)
        {
            DataFolderOverride() = std::move(folder);
        }

        std::filesystem::path ConfigFolder()
        {
            return DataFolder() / kConfigFolder;
        }

        std::filesystem::path SettingsFile()
        {
            return ConfigFolder() / kSettingsName;
        }

        std::filesystem::path ScriptSettingsFile()
        {
            return ConfigFolder() / kScriptSettingsName;
        }

        std::filesystem::path LogFolder()
        {
            return DataFolder() / kLogFolder;
        }

        std::filesystem::path LogFile()
        {
            return LogFolder() / kLogName;
        }

        std::filesystem::path LayoutFile()
        {
            return ConfigFolder() / kLayoutName;
        }

        std::filesystem::path AutorunFolder()
        {
            return ProgramFolder() / L"autorun";
        }

        std::filesystem::path SoundsFolder()
        {
            return ProgramFolder() / kSoundsFolder;
        }

        std::filesystem::path TablesFolder()
        {
            return ProgramFolder() / kTablesFolder;
        }

        std::filesystem::path CrashFolder()
        {
            return DataFolder() / L"crashes";
        }

        std::filesystem::path ScanFolder()
        {
            wchar_t temp[MAX_PATH + 1] = L"";
            const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
            std::filesystem::path folder = length ? std::filesystem::path(temp) : ProgramFolder();
            folder /= L"Manifold-Engine";
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            return folder;
        }

        Migration AdoptEarlierFiles(const std::filesystem::path& programFolder)
        {
            Migration migration;
            const auto moveInto = [&](const wchar_t* folderName, std::span<const wchar_t* const> names)
                {
                    const std::string folderText = WideToUtf8(folderName);
                    const std::filesystem::path folder = programFolder / folderName;
                    std::error_code error;
                    if (!std::filesystem::is_directory(folder, error) && !std::filesystem::create_directories(folder, error))
                        migration.Problems.push_back("The " + folderText + " folder could not be created in " + WideToUtf8(programFolder.wstring()) + ": " + SystemErrorText(static_cast<DWORD>(error.value())));
                    for (const wchar_t* name : names)
                    {
                        const std::filesystem::path source = programFolder / name;
                        const std::filesystem::path target = folder / name;
                        if (!Present(source) || Present(target))
                            continue;
                        if (::MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
                            migration.Moved.push_back(WideToUtf8(name) + " moved into the " + folderText + " folder");
                        else
                            migration.Problems.push_back(WideToUtf8(name) + " could not be moved into the " + folderText + " folder: " + SystemErrorText(::GetLastError()));
                    }
                };
            moveInto(kConfigFolder, kConfigNames);
            moveInto(kLogFolder, kLogNames);
            const std::filesystem::path configs = programFolder / kConfigFolder;

            const std::pair<const wchar_t*, const wchar_t*> earlierNames[] = {
                { L"Manifold-CheatEngine.ini", kSettingsName },
                { L"Manifold-CheatEngine.layout.ini", kLayoutName },
            };
            for (const auto& [earlier, current] : earlierNames)
            {
                const std::filesystem::path source = programFolder / earlier;
                const std::filesystem::path target = configs / current;
                if (!Present(source) || Present(target) || Present(programFolder / current))
                    continue;
                if (::CopyFileW(source.c_str(), target.c_str(), TRUE))
                    migration.Moved.push_back(WideToUtf8(earlier) + " copied to " + WideToUtf8((std::filesystem::path(kConfigFolder) / current).wstring()));
                else
                    migration.Problems.push_back(WideToUtf8(earlier) + " could not be copied into the configs folder: " + SystemErrorText(::GetLastError()));
            }

            RenameFolder(programFolder, L"Tables", kTablesFolder, migration);
            RenameFolder(programFolder, L"Sounds", kSoundsFolder, migration);
            return migration;
        }

        std::vector<std::string> LeftoverFiles(const std::filesystem::path& programFolder)
        {
            std::vector<std::string> names;
            for (const wchar_t* name : kConfigNames)
            {
                if (Present(programFolder / name) && Present(programFolder / kConfigFolder / name))
                    names.push_back(WideToUtf8(name));
            }
            return names;
        }

        std::string CurrentTablePath(const std::string& path, const std::filesystem::path& programFolder)
        {
            const std::string earlier = WideToUtf8((programFolder / L"Tables").wstring()) + "\\";
            if (path.size() <= earlier.size() || !EqualsNoCase(std::string_view(path).substr(0, earlier.size()), earlier))
                return path;
            return WideToUtf8((programFolder / kTablesFolder).wstring()) + "\\" + path.substr(earlier.size());
        }
    }
}
