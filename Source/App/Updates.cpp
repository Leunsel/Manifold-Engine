#include "App/Updates.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Core/Zip.h"
#include "Text/JsonValue.h"

#include <windows.h>
#include <bcrypt.h>
#include <wininet.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>

namespace Manifold::Updates
{
    namespace
    {
        constexpr std::string_view kOldSuffix = ".update-old";
        constexpr std::string_view kNewSuffix = ".update-new";
        constexpr const wchar_t* kLeftovers = L"update-leftovers.txt";
        constexpr DWORD kTimeoutMs = 20000;

        struct VersionParts
        {
            std::vector<long long> Numbers;
            std::string Pre;
        };

        VersionParts SplitVersion(std::string_view text)
        {
            text = TrimView(text);
            if (!text.empty() && (text[0] == 'v' || text[0] == 'V'))
                text.remove_prefix(1);
            VersionParts parts;
            const std::size_t dash = text.find_first_of("-+");
            if (dash != std::string_view::npos)
            {
                // build metadata after '+' does not order
                if (text[dash] == '-')
                    parts.Pre = std::string(text.substr(dash + 1, text.find('+', dash) - dash - 1));
                text = text.substr(0, dash);
            }
            for (const std::string& part : Manifold::Split(text, '.', true))
                parts.Numbers.push_back(std::atoll(part.c_str()));
            while (parts.Numbers.size() > 1 && parts.Numbers.back() == 0)
                parts.Numbers.pop_back();
            return parts;
        }

        std::string ToHex(const unsigned char* bytes, std::size_t size)
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string text;
            text.reserve(size * 2);
            for (std::size_t i = 0; i < size; ++i)
            {
                text += kDigits[bytes[i] >> 4];
                text += kDigits[bytes[i] & 15];
            }
            return text;
        }

        // The release workflow writes "<sha256>  <file>" lines under "### SHA-256".
        std::string SumFromNotes(std::string_view notes, std::string_view file)
        {
            for (const std::string& line : SplitLines(notes))
            {
                const std::string trimmed = Trim(line);
                if (trimmed.size() < 66 || trimmed.compare(64, 2, "  ") != 0)
                    continue;
                const std::string name = Trim(std::string_view(trimmed).substr(66));
                if (name != file)
                    continue;
                const std::string sum = Lower(trimmed.substr(0, 64));
                if (std::all_of(sum.begin(), sum.end(), [](char c) { return IsHexDigit(c); }))
                    return sum;
            }
            return {};
        }

        std::string WithoutSums(std::string_view notes)
        {
            std::string text(notes);
            const std::size_t at = text.find("### SHA-256");
            if (at != std::string::npos)
                text.resize(at);
            text = ReplaceAll(std::move(text), "\r\n", "\n");
            while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
                text.pop_back();
            return text;
        }

        bool IsRemote(std::string_view address)
        {
            return StartsWithNoCase(address, "https://") || StartsWithNoCase(address, "http://");
        }

        struct Internet
        {
            HINTERNET Handle = nullptr;
            ~Internet()
            {
                if (Handle)
                    ::InternetCloseHandle(Handle);
            }
        };

        Status FetchRemote(const std::string& address, std::vector<std::uint8_t>& out, WorkProgress* progress, std::uint64_t limit, ErrorCode code)
        {
            if (!StartsWithNoCase(address, "https://"))
                return Status::Fail(code, "only https addresses are read, not " + address);
            const std::wstring agent = Utf8ToWide("Manifold-Engine/" MANIFOLD_VERSION_TEXT);
            Internet session{ ::InternetOpenW(agent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0) };
            if (!session.Handle)
                return Status::Fail(code, "no internet session: " + SystemErrorText(::GetLastError()));
            DWORD timeout = kTimeoutMs;
            ::InternetSetOptionW(session.Handle, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
            ::InternetSetOptionW(session.Handle, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
            ::InternetSetOptionW(session.Handle, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
            const std::wstring headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
            const DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_SECURE |
                                INTERNET_FLAG_KEEP_CONNECTION;
            Internet request{ ::InternetOpenUrlW(session.Handle, Utf8ToWide(address).c_str(), headers.c_str(), static_cast<DWORD>(headers.size()), flags, 0) };
            if (!request.Handle)
            {
                const DWORD error = ::GetLastError();
                return Status::Fail(code, error == ERROR_INTERNET_NAME_NOT_RESOLVED ? "no connection to " + address.substr(8, address.find('/', 8) - 8)
                                                                                                             : SystemErrorText(error));
            }
            DWORD http = 0;
            DWORD size = sizeof(http);
            if (::HttpQueryInfoW(request.Handle, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &http, &size, nullptr) && http != 200)
            {
                if (http == 403 || http == 429)
                    return Status::Fail(code, "GitHub refused the request for now (HTTP " + std::to_string(http) + ", too many requests from this address)");
                if (http == 404)
                    return Status::Fail(code, "GitHub has no release there (HTTP 404)");
                return Status::Fail(code, "HTTP " + std::to_string(http) + " from " + address);
            }
            std::uint64_t total = 0;
            DWORD length = 0;
            size = sizeof(length);
            if (::HttpQueryInfoW(request.Handle, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &length, &size, nullptr))
                total = length;
            if (total > limit)
                return Status::Fail(code, "the answer is " + ByteSize(total) + ", more than the " + ByteSize(limit) + " a release can be");
            if (progress && total)
                progress->SetSteps(0, static_cast<std::int64_t>(total));
            out.clear();
            out.reserve(static_cast<std::size_t>(total));
            std::vector<std::uint8_t> buffer(64 * 1024);
            for (;;)
            {
                if (progress && progress->Cancelled())
                    return Status::Fail(code, "cancelled");
                DWORD read = 0;
                if (!::InternetReadFile(request.Handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
                    return Status::Fail(code, "the download broke off: " + SystemErrorText(::GetLastError()));
                if (read == 0)
                    break;
                out.insert(out.end(), buffer.begin(), buffer.begin() + read);
                if (out.size() > limit)
                    return Status::Fail(code, "the answer is larger than the " + ByteSize(limit) + " a release can be");
                if (progress && total)
                    progress->Step(read);
            }
            if (total && out.size() != total)
                return Status::Fail(code, "the download is incomplete (" + std::to_string(out.size()) + " of " + std::to_string(total) + " bytes)");
            return Status::Success();
        }

        Status FetchFile(const std::string& address, std::vector<std::uint8_t>& out, std::uint64_t limit, ErrorCode code = ErrorCode::UpdateCheckFailed)
        {
            std::string path = address;
            if (StartsWithNoCase(path, "file:///"))
                path = path.substr(8);
            std::ifstream file(std::filesystem::path(Utf8ToWide(path)), std::ios::binary);
            if (!file)
                return Status::Fail(code, path + " could not be read");
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < 0 || static_cast<std::uint64_t>(size) > limit)
                return Status::Fail(code, path + " is too large");
            file.seekg(0);
            out.resize(static_cast<std::size_t>(size));
            if (size && !file.read(reinterpret_cast<char*>(out.data()), size))
                return Status::Fail(code, path + " could not be read");
            return Status::Success();
        }

        bool SafePart(std::string_view part)
        {
            if (part.empty() || part == "." || part == "..")
                return false;
            for (const char c : part)
            {
                if (c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' || static_cast<unsigned char>(c) < 0x20)
                    return false;
            }
            return part.back() != '.' && part.back() != ' ';
        }

        bool IsBinary(const std::filesystem::path& path)
        {
            const std::wstring extension = path.extension().wstring();
            return _wcsicmp(extension.c_str(), L".exe") == 0 || _wcsicmp(extension.c_str(), L".dll") == 0 || _wcsicmp(extension.c_str(), L".pdb") == 0;
        }

        bool SameContent(const std::filesystem::path& path, const std::vector<std::uint8_t>& data)
        {
            std::error_code error;
            if (std::filesystem::file_size(path, error) != data.size() || error)
                return false;
            std::vector<std::uint8_t> current;
            return FetchFile(WideToUtf8(path.wstring()), current, data.size() + 1).Ok() && current == data;
        }

        bool WriteAll(const std::filesystem::path& path, const std::vector<std::uint8_t>& data)
        {
            const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            std::size_t done = 0;
            bool ok = true;
            while (ok && done < data.size())
            {
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 16u * 1024u * 1024u));
                DWORD written = 0;
                ok = ::WriteFile(file, data.data() + done, chunk, &written, nullptr) && written == chunk;
                done += written;
            }
            ok = ok && ::FlushFileBuffers(file);
            ::CloseHandle(file);
            return ok;
        }

        std::filesystem::path WithSuffix(const std::filesystem::path& path, std::string_view suffix)
        {
            return std::filesystem::path(path.wstring() + Utf8ToWide(suffix));
        }

        std::vector<std::filesystem::path> ReadLeftovers(const std::filesystem::path& folder)
        {
            std::vector<std::uint8_t> bytes;
            std::vector<std::filesystem::path> paths;
            if (!FetchFile(WideToUtf8((folder / kLeftovers).wstring()), bytes, 1024 * 1024).Ok())
                return paths;
            for (const std::string& line : SplitLines(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())))
            {
                const std::string name = Trim(line);
                // only a name the installer gave, right in this folder or below it
                if (name.empty() || name.find("..") != std::string::npos || name.find(':') != std::string::npos || name.find(kOldSuffix) == std::string::npos)
                    continue;
                paths.push_back(folder / std::filesystem::path(Utf8ToWide(name)));
            }
            return paths;
        }

        void WriteLeftovers(const std::filesystem::path& folder, const std::vector<std::string>& names)
        {
            std::error_code error;
            if (names.empty())
            {
                std::filesystem::remove(folder / kLeftovers, error);
                return;
            }
            std::string text;
            for (const std::string& name : names)
                text += name + "\r\n";
            WriteAll(folder / kLeftovers, std::vector<std::uint8_t>(text.begin(), text.end()));
        }

        std::string Relative(const std::filesystem::path& folder, const std::filesystem::path& path)
        {
            return WideToUtf8(path.lexically_relative(folder).generic_wstring());
        }
    }

    int CompareVersions(std::string_view a, std::string_view b)
    {
        const VersionParts left = SplitVersion(a);
        const VersionParts right = SplitVersion(b);
        const std::size_t count = std::max(left.Numbers.size(), right.Numbers.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            const long long x = i < left.Numbers.size() ? left.Numbers[i] : 0;
            const long long y = i < right.Numbers.size() ? right.Numbers[i] : 0;
            if (x != y)
                return x < y ? -1 : 1;
        }
        if (left.Pre.empty() != right.Pre.empty())
            return left.Pre.empty() ? 1 : -1;
        return left.Pre == right.Pre ? 0 : (left.Pre < right.Pre ? -1 : 1);
    }

    std::optional<Release> ParseRelease(std::string_view json, std::string* error)
    {
        std::string problem;
        const std::optional<Text::JsonValue> root = Text::ParseJson(json, &problem);
        if (!root || !root->IsObject())
        {
            if (error)
                *error = root ? "the answer is no JSON object" : "the answer is no JSON: " + problem;
            return std::nullopt;
        }
        Release release;
        release.Tag = root->Text("tag_name");
        release.Version = release.Tag;
        if (!release.Version.empty() && (release.Version[0] == 'v' || release.Version[0] == 'V'))
            release.Version.erase(0, 1);
        if (release.Version.empty() || release.Version[0] < '0' || release.Version[0] > '9')
        {
            if (error)
                *error = release.Tag.empty() ? "the release has no tag" : "the tag " + release.Tag + " is no version";
            return std::nullopt;
        }
        release.Title = root->Text("name");
        release.Page = root->Text("html_url");
        release.Published = root->Text("published_at");
        release.Prerelease = root->BoolOf("prerelease") || root->BoolOf("draft");
        const std::string body = root->Text("body");
        release.Notes = WithoutSums(body);
        if (const Text::JsonValue* assets = root->Find("assets"); assets && assets->IsArray())
        {
            for (const Text::JsonValue& item : assets->Items)
            {
                Asset asset;
                asset.Name = item.Text("name");
                asset.Url = item.Text("browser_download_url");
                asset.Size = static_cast<std::uint64_t>(std::max(0.0, item.NumberOf("size")));
                const std::string digest = item.Text("digest");
                if (StartsWithNoCase(digest, "sha256:") && digest.size() == 7 + 64)
                    asset.Sha256 = Lower(digest.substr(7));
                if (asset.Sha256.empty())
                    asset.Sha256 = SumFromNotes(body, asset.Name);
                if (!asset.Name.empty() && !asset.Url.empty())
                    release.Assets.push_back(std::move(asset));
            }
        }
        return release;
    }

    const Asset* ProgramAsset(const Release& release)
    {
        const std::string wanted = "Manifold-Engine-" + release.Version + "-win-x64.zip";
        for (const Asset& asset : release.Assets)
        {
            if (EqualsNoCase(asset.Name, wanted))
                return &asset;
        }
        return nullptr;
    }

    Notice NoticeFor(std::string_view latest, std::string_view current, std::string_view seen, std::string_view skipped)
    {
        if (latest.empty() || CompareVersions(latest, current) <= 0)
            return Notice::None;
        if (!skipped.empty() && CompareVersions(latest, skipped) == 0)
            return Notice::None;
        if (!seen.empty() && CompareVersions(latest, seen) == 0)
            return Notice::Skippable;
        return Notice::Required;
    }

    std::string FeedAddress()
    {
        wchar_t buffer[2048] = {};
        const DWORD length = ::GetEnvironmentVariableW(L"MANIFOLD_UPDATE_FEED", buffer, static_cast<DWORD>(std::size(buffer)));
        if (length > 0 && length < std::size(buffer))
            return WideToUtf8(std::wstring_view(buffer, length));
        return std::string(kLatestRelease);
    }

    Status Fetch(const std::string& address, std::vector<std::uint8_t>& out, WorkProgress* progress, std::uint64_t limit, ErrorCode code)
    {
        if (IsRemote(address))
            return FetchRemote(address, out, progress, limit, code);
        return FetchFile(address, out, limit, code);
    }

    Status FetchLatest(Release& out)
    {
        std::vector<std::uint8_t> body;
        const std::string feed = FeedAddress();
        if (Status fetched = Fetch(feed, body, nullptr, 4 * 1024 * 1024); !fetched)
            return fetched;
        std::string error;
        std::optional<Release> release = ParseRelease(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), &error);
        if (!release)
            return Status::Fail(ErrorCode::UpdateCheckFailed, error);
        if (release->Page.empty())
            release->Page = std::string(kReleasesPage);
        out = std::move(*release);
        return Status::Success();
    }

    std::string Sha256Hex(std::span<const std::uint8_t> data)
    {
        unsigned char digest[32] = {};
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (::BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) != 0)
            return {};
        bool ok = true;
        for (std::size_t done = 0; ok && done < data.size();)
        {
            const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(data.size() - done, 1u << 30));
            ok = ::BCryptHashData(hash, const_cast<PUCHAR>(data.data() + done), chunk, 0) == 0;
            done += chunk;
        }
        ok = ok && ::BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
        ::BCryptDestroyHash(hash);
        return ok ? ToHex(digest, sizeof(digest)) : std::string();
    }

    Status Unpack(std::span<const std::uint8_t> zip, std::vector<File>& files)
    {
        std::string error;
        std::optional<std::vector<Zip::Entry>> entries = Zip::Read(zip, &error);
        if (!entries)
            return Status::Fail(ErrorCode::UpdateInstallFailed, "the zip could not be read: " + error);
        // one top folder named like the zip: take it off
        std::string top;
        bool shared = true;
        for (const Zip::Entry& entry : *entries)
        {
            const std::size_t slash = entry.Name.find('/');
            const std::string first = slash == std::string::npos ? std::string() : entry.Name.substr(0, slash);
            if (first.empty() || (!top.empty() && first != top))
                shared = false;
            if (top.empty())
                top = first;
        }
        files.clear();
        std::set<std::string> names;
        bool program = false;
        for (Zip::Entry& entry : *entries)
        {
            if (entry.Folder)
                continue;
            std::string name = shared ? entry.Name.substr(top.size() + 1) : entry.Name;
            if (name.empty())
                continue;
            const std::vector<std::string> parts = Manifold::Split(name, '/', true);
            if (!std::all_of(parts.begin(), parts.end(), SafePart) || name[0] == '/')
                return Status::Fail(ErrorCode::UpdateInstallFailed, "the zip holds a name that leaves the folder: " + entry.Name);
            if (name.find(kOldSuffix) != std::string::npos || name.find(kNewSuffix) != std::string::npos || EqualsNoCase(name, WideToUtf8(kLeftovers)))
                return Status::Fail(ErrorCode::UpdateInstallFailed, "the zip holds a name the installer uses itself: " + entry.Name);
            if (!names.insert(Lower(name)).second)
                return Status::Fail(ErrorCode::UpdateInstallFailed, "the zip holds " + name + " twice");
            program |= EqualsNoCase(name, "Manifold-Engine.exe");
            files.push_back({ std::move(name), std::move(entry.Data) });
        }
        if (!program)
            return Status::Fail(ErrorCode::UpdateInstallFailed, "the zip holds no Manifold-Engine.exe");
        return Status::Success();
    }

    std::optional<std::string> InstallBlocker(const std::filesystem::path& folder)
    {
        std::error_code error;
        // <source>\Build\<platform>\<configuration>: a build output, which git and msbuild update.
        // An install folder elsewhere in the source tree (Tools\Install.ps1 puts one there) is not one.
        std::filesystem::path at = folder;
        for (int up = 0; up < 3 && !at.empty(); ++up)
        {
            const std::filesystem::path parent = at.parent_path();
            if (_wcsicmp(at.filename().c_str(), L"Build") == 0 && std::filesystem::exists(parent / L"Manifold-Engine.sln", error))
                return "This copy runs from a build folder of the source code (" + WideToUtf8(folder.wstring()) + "). Update it with git and a new build.";
            if (parent == at)
                break;
            at = parent;
        }
        const std::filesystem::path probe = folder / L".update-write-test";
        const HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return "The program folder " + WideToUtf8(folder.wstring()) + " is not writable. Install the update by hand, or restart as administrator.";
        ::CloseHandle(file);
        return std::nullopt;
    }

    Status Apply(const std::filesystem::path& folder, const std::vector<File>& files, const std::filesystem::path& backup, Applied* applied)
    {
        Applied result;
        struct Step
        {
            std::filesystem::path Target;
            std::filesystem::path Staged;
            std::filesystem::path Old;   // where the old file went, empty when there was none or it was overwritten
            bool Moved = false;
            bool Existed = false;
        };
        std::vector<Step> steps;
        std::error_code error;

        const auto dropStaged = [&]() {
            for (const Step& step : steps)
            {
                if (!step.Staged.empty())
                    std::filesystem::remove(step.Staged, error);
            }
        };

        // 1. everything new beside its target; nothing in use changes yet
        for (const File& file : files)
        {
            const std::filesystem::path target = folder / std::filesystem::path(Utf8ToWide(file.Path)).make_preferred();
            const bool exists = std::filesystem::is_regular_file(target, error);
            if (exists && SameContent(target, file.Data))
            {
                ++result.Unchanged;
                continue;
            }
            std::filesystem::create_directories(target.parent_path(), error);
            Step step{ target, WithSuffix(target, kNewSuffix) };
            step.Existed = exists;
            if (!WriteAll(step.Staged, file.Data))
            {
                const std::string why = SystemErrorText(::GetLastError());
                steps.push_back(step);
                dropStaged();
                return Status::Fail(ErrorCode::UpdateInstallFailed, file.Path + " could not be written: " + why);
            }
            steps.push_back(std::move(step));
        }

        // 2. changed autorun files may hold the user's own edits: a copy goes to backup first
        for (const Step& step : steps)
        {
            const std::string relative = Relative(folder, step.Target);
            if (!step.Existed || !StartsWithNoCase(relative, "autorun/"))
                continue;
            const std::filesystem::path copy = backup / std::filesystem::path(Utf8ToWide(relative)).make_preferred();
            std::filesystem::create_directories(copy.parent_path(), error);
            if (!std::filesystem::copy_file(step.Target, copy, std::filesystem::copy_options::overwrite_existing, error))
            {
                dropStaged();
                return Status::Fail(ErrorCode::UpdateInstallFailed, "the old " + relative + " could not be copied to " + WideToUtf8(copy.wstring()) + ": " + error.message());
            }
            result.Kept.push_back(relative);
        }

        // 3. the swap: programs move aside (a running one cannot be overwritten), the rest is replaced
        std::size_t done = 0;
        std::string failure;
        for (; done < steps.size(); ++done)
        {
            Step& step = steps[done];
            if (step.Existed && IsBinary(step.Target))
            {
                std::filesystem::path old = WithSuffix(step.Target, kOldSuffix);
                bool moved = ::MoveFileExW(step.Target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
                // an older leftover may still be loaded (a game holding the old agent): take the next free name
                for (int n = 2; !moved && n < 50; ++n)
                {
                    old = WithSuffix(step.Target, std::string(kOldSuffix) + "-" + std::to_string(n));
                    moved = ::MoveFileExW(step.Target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
                }
                if (!moved)
                {
                    failure = Relative(folder, step.Target) + " could not be moved aside: " + SystemErrorText(::GetLastError());
                    break;
                }
                step.Old = old;
                if (!::MoveFileExW(step.Staged.c_str(), step.Target.c_str(), 0))
                {
                    failure = Relative(folder, step.Target) + " could not be put in place: " + SystemErrorText(::GetLastError());
                    ::MoveFileExW(step.Old.c_str(), step.Target.c_str(), MOVEFILE_REPLACE_EXISTING);
                    step.Old.clear();
                    break;
                }
            }
            else if (!::MoveFileExW(step.Staged.c_str(), step.Target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                failure = Relative(folder, step.Target) + " could not be replaced: " + SystemErrorText(::GetLastError());
                break;
            }
            step.Moved = true;
            step.Staged.clear();
        }
        if (!failure.empty())
        {
            // the programs go back; replaced text files keep the new text (their old one is in backup when it was autorun)
            for (std::size_t i = done; i-- > 0;)
            {
                Step& step = steps[i];
                if (!step.Old.empty() && ::MoveFileExW(step.Old.c_str(), step.Target.c_str(), MOVEFILE_REPLACE_EXISTING))
                    step.Old.clear();
            }
            dropStaged();
            std::vector<std::string> leftovers;
            for (const Step& step : steps)
            {
                if (!step.Old.empty())
                    leftovers.push_back(Relative(folder, step.Old));
            }
            WriteLeftovers(folder, leftovers);
            return Status::Fail(ErrorCode::UpdateInstallFailed, failure);
        }

        // 4. the old programs are still running: they go at the next start
        std::vector<std::string> leftovers;
        for (const std::filesystem::path& path : ReadLeftovers(folder))
            leftovers.push_back(Relative(folder, path));
        for (const Step& step : steps)
        {
            if (step.Existed)
                ++result.Replaced;
            else
                ++result.Added;
            if (!step.Old.empty())
                leftovers.push_back(Relative(folder, step.Old));
        }
        WriteLeftovers(folder, leftovers);
        if (applied)
            *applied = std::move(result);
        return Status::Success();
    }

    std::vector<std::string> RemoveLeftovers(const std::filesystem::path& folder)
    {
        std::vector<std::string> still;
        const std::vector<std::filesystem::path> paths = ReadLeftovers(folder);
        if (paths.empty())
            return still;
        for (const std::filesystem::path& path : paths)
        {
            if (::DeleteFileW(path.c_str()) || ::GetLastError() == ERROR_FILE_NOT_FOUND || ::GetLastError() == ERROR_PATH_NOT_FOUND)
                continue;
            still.push_back(Relative(folder, path));
        }
        WriteLeftovers(folder, still);
        return still;
    }
}
