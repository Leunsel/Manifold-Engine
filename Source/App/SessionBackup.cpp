#include "App/SessionBackup.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Paths.h"
#include "Core/Profiler.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Table/CheatTable.h"

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>

#include <windows.h>

namespace Manifold
{
    namespace Backups
    {
        namespace
        {
            constexpr std::size_t kStampLength = 19;
            constexpr wchar_t kMarkerStart[] = L"backup-";
            constexpr wchar_t kMarkerEnd[] = L".open";

            bool Digit(wchar_t c)
            {
                return c >= L'0' && c <= L'9';
            }

            bool Reserved(const std::wstring& name)
            {
                static constexpr const wchar_t* kDevices[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
                    L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
                const std::wstring base = name.substr(0, name.find(L'.'));
                return std::any_of(std::begin(kDevices), std::end(kDevices), [&](const wchar_t* device) { return ::_wcsicmp(base.c_str(), device) == 0; });
            }

            void TrimEnds(std::wstring& name)
            {
                while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
                    name.pop_back();
                while (!name.empty() && name.front() == L' ')
                    name.erase(name.begin());
            }

            std::string WithoutExtension(std::string_view name, bool onlyExe)
            {
                const std::size_t dot = name.rfind('.');
                if (dot == std::string_view::npos || dot == 0)
                    return std::string(name);
                if (onlyExe && !EqualsNoCase(name.substr(dot), ".exe"))
                    return std::string(name);
                return std::string(name.substr(0, dot));
            }

            unsigned long Number(const std::wstring& stem)
            {
                return stem.size() > kStampLength + 1 ? std::wcstoul(stem.c_str() + kStampLength + 1, nullptr, 10) : 1ul;
            }

            bool Newer(const std::filesystem::path& a, const std::filesystem::path& b)
            {
                const std::wstring first = a.stem().wstring();
                const std::wstring second = b.stem().wstring();
                if (const int order = first.compare(0, kStampLength, second, 0, kStampLength))
                    return order > 0;
                return Number(first) > Number(second);
            }

            bool InstanceAlive(unsigned long processId)
            {
                const HANDLE instance = ::OpenMutexW(SYNCHRONIZE, FALSE, InstanceMutexName(processId).c_str());
                if (instance)
                {
                    ::CloseHandle(instance);
                    return true;
                }
                return ::GetLastError() == ERROR_ACCESS_DENIED;
            }
        }

        std::filesystem::path Folder(const Settings& settings)
        {
            if (settings.BackupFolder.empty())
                return Paths::BackupFolder();
            std::filesystem::path folder = Utf8ToWide(settings.BackupFolder);
            return folder.is_relative() ? Paths::DataFolder() / folder : folder;
        }

        std::string SafeName(std::string_view name)
        {
            std::wstring wide = Utf8ToWide(name);
            for (wchar_t& c : wide)
            {
                if (c < 0x20 || std::wcschr(L"<>:\"/\\|?*", c))
                    c = L'_';
            }
            if (wide.size() > kMaxNameLength)
            {
                wide.resize(kMaxNameLength);
                if (wide.back() >= 0xD800 && wide.back() <= 0xDBFF)
                    wide.pop_back();
            }
            TrimEnds(wide);
            if (wide.empty())
                return kFallbackName;
            if (Reserved(wide))
                wide.insert(wide.begin(), L'_');
            return WideToUtf8(wide);
        }

        std::string SessionName(std::string_view tablePath, std::string_view processName)
        {
            if (!tablePath.empty())
            {
                const std::size_t slash = tablePath.find_last_of("\\/");
                return SafeName(WithoutExtension(slash == std::string_view::npos ? tablePath : tablePath.substr(slash + 1), false));
            }
            if (!processName.empty())
                return SafeName(WithoutExtension(processName, true));
            return kFallbackName;
        }

        std::wstring StampName(std::chrono::system_clock::time_point time)
        {
            const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
            std::tm local{};
            ::localtime_s(&local, &seconds);
            wchar_t text[32] = L"";
            ::swprintf_s(text, L"%04d-%02d-%02d_%02d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
            return text;
        }

        bool IsVersion(const std::filesystem::path& file)
        {
            if (!EqualsNoCase(WideToUtf8(file.extension().wstring()), ".ct"))
                return false;
            const std::wstring stem = file.stem().wstring();
            if (stem.size() < kStampLength)
                return false;
            static constexpr wchar_t kShape[] = L"dddd-dd-dd_dd-dd-dd";
            for (std::size_t i = 0; i < kStampLength; ++i)
            {
                if (kShape[i] == L'd' ? !Digit(stem[i]) : stem[i] != kShape[i])
                    return false;
            }
            if (stem.size() == kStampLength)
                return true;
            if (stem[kStampLength] != L'_' || stem.size() == kStampLength + 1 || stem.size() > kStampLength + 5)
                return false;
            return std::all_of(stem.begin() + kStampLength + 1, stem.end(), Digit);
        }

        std::string When(const std::filesystem::path& file)
        {
            if (!IsVersion(file))
                return {};
            std::string text = WideToUtf8(file.stem().wstring().substr(0, kStampLength));
            text[10] = ' ';
            text[13] = ':';
            text[16] = ':';
            return text;
        }

        std::vector<std::filesystem::path> Versions(const std::filesystem::path& folder)
        {
            std::vector<std::filesystem::path> versions;
            std::error_code error;
            for (std::filesystem::directory_iterator at(folder, error), end; !error && at != end; at.increment(error))
            {
                std::error_code ignored;
                if (at->is_regular_file(ignored) && IsVersion(at->path()))
                    versions.push_back(at->path());
            }
            std::sort(versions.begin(), versions.end(), Newer);
            return versions;
        }

        int Prune(const std::filesystem::path& folder, int keep, const std::filesystem::path& newest)
        {
            std::vector<std::filesystem::path> versions = Versions(folder);
            keep = std::max(keep, 1);
            if (const auto written = std::find(versions.begin(), versions.end(), newest); written != versions.end())
            {
                versions.erase(written);
                --keep;
            }
            int removed = 0;
            for (std::size_t i = static_cast<std::size_t>(keep); i < versions.size(); ++i)
            {
                std::error_code error;
                if (std::filesystem::remove(versions[i], error) && !error)
                    ++removed;
            }
            return removed;
        }

        std::filesystem::path Write(const std::filesystem::path& folder, std::string_view text, std::chrono::system_clock::time_point time)
        {
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            const std::wstring stamp = StampName(time);
            unsigned long taken = 0;
            for (const std::filesystem::path& version : Versions(folder))
            {
                const std::wstring stem = version.stem().wstring();
                if (stem.compare(0, kStampLength, stamp) == 0)
                    taken = std::max(taken, Number(stem));
            }
            if (taken >= 9999)
                return {};
            const std::filesystem::path file = folder / (taken == 0 ? stamp + L".CT" : stamp + L"_" + std::to_wstring(taken + 1) + L".CT");
            if (!Table::WriteFileSafely(file, text, false))
                return {};
            return file;
        }

        std::wstring InstanceMutexName(unsigned long processId)
        {
            return L"Local\\ManifoldEngineBackup-" + std::to_wstring(processId);
        }

        std::filesystem::path MarkerFile(const std::filesystem::path& folder, unsigned long processId)
        {
            return folder / (kMarkerStart + std::to_wstring(processId) + kMarkerEnd);
        }

        std::vector<Abandoned> TakeAbandoned(const std::filesystem::path& folder, unsigned long ownProcessId)
        {
            std::vector<std::filesystem::path> markers;
            std::error_code error;
            for (std::filesystem::directory_iterator at(folder, error), end; !error && at != end; at.increment(error))
            {
                const std::wstring name = at->path().filename().wstring();
                const std::size_t start = std::size(kMarkerStart) - 1;
                const std::size_t tail = std::size(kMarkerEnd) - 1;
                if (name.size() <= start + tail || name.size() > start + tail + 10 || name.compare(0, start, kMarkerStart) != 0 || name.compare(name.size() - tail, tail, kMarkerEnd) != 0)
                    continue;
                const std::wstring digits = name.substr(start, name.size() - start - tail);
                if (!std::all_of(digits.begin(), digits.end(), Digit))
                    continue;
                const unsigned long processId = std::wcstoul(digits.c_str(), nullptr, 10);
                if (processId != ownProcessId && !InstanceAlive(processId))
                    markers.push_back(at->path());
            }

            std::vector<Abandoned> abandoned;
            for (const std::filesystem::path& marker : markers)
            {
                Abandoned entry;
                {
                    std::ifstream in(marker, std::ios::binary);
                    std::string file;
                    std::getline(in, file);
                    std::getline(in, entry.Name);
                    while (!file.empty() && (file.back() == '\r' || file.back() == '\n'))
                        file.pop_back();
                    while (!entry.Name.empty() && (entry.Name.back() == '\r' || entry.Name.back() == '\n'))
                        entry.Name.pop_back();
                    entry.File = Utf8ToWide(file);
                }
                std::filesystem::remove(marker, error);
                if (IsVersion(entry.File) && std::filesystem::is_regular_file(entry.File, error))
                    abandoned.push_back(std::move(entry));
            }
            std::sort(abandoned.begin(), abandoned.end(), [](const Abandoned& a, const Abandoned& b) { return Newer(a.File, b.File); });
            return abandoned;
        }
    }

    SessionBackup::SessionBackup(Engine& engine) : m_engine(engine)
    {
        m_instance = ::CreateMutexW(nullptr, FALSE, Backups::InstanceMutexName(::GetCurrentProcessId()).c_str());
    }

    SessionBackup::~SessionBackup()
    {
        if (m_instance)
            ::CloseHandle(static_cast<HANDLE>(m_instance));
    }

    void SessionBackup::NoteProcess()
    {
        if (!m_engine.HasProcess())
            return;
        const std::string& name = m_engine.Process()->Name();
        if (name != m_process)
            m_process = name;
    }

    std::string SessionBackup::Name() const
    {
        const std::string& path = m_engine.Table().FilePath;
        if (!path.empty())
            return Backups::SessionName(path, {});
        if (!m_restored.empty())
            return m_restored;
        return Backups::SessionName({}, m_engine.HasProcess() ? m_engine.Process()->Name() : m_process);
    }

    std::filesystem::path SessionBackup::SessionFolder() const
    {
        return Backups::Folder(m_engine.Options()) / Utf8ToWide(Name());
    }

    void SessionBackup::Tick(double now)
    {
        NoteProcess();
        if (m_marked && !m_engine.Table().Dirty())
            DropMarker();
        const Settings& settings = m_engine.Options();
        if (!settings.BackupsEnabled || m_lastAt < 0.0)
        {
            m_lastAt = now;
            return;
        }
        if (now - m_lastAt < settings.BackupIntervalMinutes * 60.0 || m_engine.Working())
            return;
        m_lastAt = now;
        PROFILE_ZONE("Session backup");
        const Status status = BackUpNow();
        if (status)
        {
            m_failed = false;
            return;
        }
        if (!m_failed)
            m_engine.Notify(status, LogLevel::Warning);
        else
            LOG_WARN("Backup", status.Text);
        m_failed = true;
    }

    Status SessionBackup::BackUpNow(bool* written)
    {
        if (written)
            *written = false;
        NoteProcess();
        Table::AddressList& table = m_engine.Table();
        Table::StructureStore& structures = m_engine.Structures();
        if (!table.Dirty())
            return Status::Success();
        const std::vector<Table::TableSymbol> symbols = m_engine.TableSymbols();
        if (!Table::TableHasContent(table, structures, symbols))
            return Status::Success();
        const auto started = std::chrono::steady_clock::now();
        const std::string text = Table::WriteTable(table, structures, symbols, m_engine.TableFileNaming());
        const std::size_t hash = std::hash<std::string>{}(text);
        if (m_hasLast && hash == m_lastHash && text.size() == m_lastSize)
            return Status::Success();

        const std::string name = Name();
        const std::filesystem::path folder = Backups::Folder(m_engine.Options()) / Utf8ToWide(name);
        const std::filesystem::path file = Backups::Write(folder, text, std::chrono::system_clock::now());
        if (file.empty())
            return Status::Fail(ErrorCode::BackupNotSaved, WideToUtf8(folder.wstring()));
        m_hasLast = true;
        m_lastHash = hash;
        m_lastSize = text.size();
        m_lastFile = file;
        const int removed = Backups::Prune(folder, m_engine.Options().BackupVersions, file);
        WriteMarker(file, name);
        const long long milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        LOG_INFO("Backup", "Backed up " + name + " to " + WideToUtf8(file.wstring()) + " (" + std::to_string((text.size() + 1023) / 1024) + " KB, " + std::to_string(milliseconds) + " ms)" +
            (removed > 0 ? ", removed " + std::to_string(removed) + (removed == 1 ? " older version" : " older versions") : std::string()));
        if (written)
            *written = true;
        return Status::Success();
    }

    void SessionBackup::Finish()
    {
        if (m_engine.Options().BackupsEnabled && !m_engine.Working())
        {
            const Status status = BackUpNow();
            if (!status)
                LOG_WARN("Backup", status.Text);
        }
        DropMarker();
    }

    void SessionBackup::TableReplaced()
    {
        m_restored.clear();
        m_hasLast = false;
        DropMarker();
    }

    void SessionBackup::Restored(const std::filesystem::path& file)
    {
        m_restored = Backups::IsVersion(file) ? Backups::SafeName(WideToUtf8(file.parent_path().filename().wstring())) : std::string();
        const std::string text = Table::WriteTable(m_engine.Table(), m_engine.Structures(), m_engine.TableSymbols(), m_engine.TableFileNaming());
        m_hasLast = true;
        m_lastHash = std::hash<std::string>{}(text);
        m_lastSize = text.size();
        m_lastFile = file;
    }

    std::vector<Backups::Abandoned> SessionBackup::TakeAbandoned()
    {
        return Backups::TakeAbandoned(Paths::ConfigFolder(), ::GetCurrentProcessId());
    }

    void SessionBackup::WriteMarker(const std::filesystem::path& file, const std::string& name)
    {
        const std::filesystem::path marker = Backups::MarkerFile(Paths::ConfigFolder(), ::GetCurrentProcessId());
        std::error_code ignored;
        std::filesystem::create_directories(marker.parent_path(), ignored);
        std::ofstream out(marker, std::ios::binary | std::ios::trunc);
        out << WideToUtf8(file.wstring()) << '\n' << name << '\n';
        m_marked = m_marked || static_cast<bool>(out);
    }

    void SessionBackup::DropMarker()
    {
        if (!m_marked)
            return;
        std::error_code ignored;
        std::filesystem::remove(Backups::MarkerFile(Paths::ConfigFolder(), ::GetCurrentProcessId()), ignored);
        m_marked = false;
    }
}
