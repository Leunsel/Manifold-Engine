#pragma once

#include "Core/Errors.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold
{
    class WorkProgress;
}

// The update check and the installer. The check asks GitHub for the newest
// release of the public repository; the installer downloads its program zip,
// checks the SHA-256 GitHub lists for it, and swaps the files next to the
// running program: a running .exe or a loaded .dll can be renamed but not
// overwritten, so each one moves aside as <name>.update-old and the new one
// takes its place. The leftovers go at the next start (or the one after, when
// a game still holds the old agent DLL).
namespace Manifold::Updates
{
    inline constexpr std::string_view kRepository = "Leunsel/Manifold-Engine";
    inline constexpr std::string_view kLatestRelease = "https://api.github.com/repos/Leunsel/Manifold-Engine/releases/latest";
    inline constexpr std::string_view kReleasesPage = "https://github.com/Leunsel/Manifold-Engine/releases";
    inline constexpr std::string_view kRepositoryPage = "https://github.com/Leunsel/Manifold-Engine";
    // the release zips hold about 8 MB; anything far above that is not ours
    inline constexpr std::uint64_t kMaxDownload = 256ull * 1024ull * 1024ull;

    struct Asset
    {
        std::string Name;
        std::string Url;
        std::string Sha256;   // lower-case hex, empty when GitHub gave none
        std::uint64_t Size = 0;
    };

    struct Release
    {
        std::string Tag;      // "v1.1.0"
        std::string Version;  // "1.1.0"
        std::string Title;
        std::string Notes;    // the release text without its SHA-256 block
        std::string Page;
        std::string Published;
        bool Prerelease = false;
        std::vector<Asset> Assets;
    };

    // <0, 0, >0 like strcmp. Numbers part by part ("1.10" > "1.9"), a leading
    // 'v' ignored, a pre-release ("1.1.0-beta") below its release.
    int CompareVersions(std::string_view a, std::string_view b);
    std::optional<Release> ParseRelease(std::string_view json, std::string* error = nullptr);
    // The zip with the program, "Manifold-Engine-<version>-win-x64.zip".
    const Asset* ProgramAsset(const Release& release);

    // ∑ Owner rule: a new version is shown once without a way out (Required),
    // later starts offer to skip it (Skippable), and a skipped version stays
    // quiet until a newer one comes. "seen" is the version whose required
    // notice was shown, "skipped" the one the user skipped.
    enum class Notice
    {
        None,
        Required,
        Skippable,
    };
    Notice NoticeFor(std::string_view latest, std::string_view current, std::string_view seen, std::string_view skipped);

    // Where the check reads the newest release: the GitHub API, or the
    // MANIFOLD_UPDATE_FEED environment variable (an https address or a local
    // JSON file, for tests; asset addresses in a local feed may be files too).
    std::string FeedAddress();
    // https:// through WinINet, anything else as a local file. Cancels when the progress does.
    Status Fetch(const std::string& address, std::vector<std::uint8_t>& out, WorkProgress* progress, std::uint64_t limit, ErrorCode code = ErrorCode::UpdateCheckFailed);
    Status FetchLatest(Release& out);
    std::string Sha256Hex(std::span<const std::uint8_t> data);

    struct File
    {
        std::string Path;     // relative, with '/' between folders
        std::vector<std::uint8_t> Data;
    };

    // The files of a release zip, its one top folder taken off. Refuses names
    // that leave the folder (.., drive letters, absolute paths) and a zip
    // without Manifold-Engine.exe.
    Status Unpack(std::span<const std::uint8_t> zip, std::vector<File>& files);
    // Why the program in this folder cannot replace itself (a build folder of
    // the source, no write access), or nothing when it can.
    std::optional<std::string> InstallBlocker(const std::filesystem::path& folder);

    struct Applied
    {
        int Replaced = 0;
        int Added = 0;
        int Unchanged = 0;
        std::vector<std::string> Kept;   // changed autorun files whose old text went to the backup folder
    };
    // Puts the files into the folder. Every new file is written beside its
    // target first, so a full disk changes nothing; a failed swap puts the
    // old files back. Autorun files the user may have edited are copied to
    // backup before they are replaced. Nothing else in the folder is touched.
    Status Apply(const std::filesystem::path& folder, const std::vector<File>& files, const std::filesystem::path& backup, Applied* applied = nullptr);
    // Removes the <name>.update-old files the last install left (listed in
    // the folder's update-leftovers.txt, nothing else). Returns what is still locked.
    std::vector<std::string> RemoveLeftovers(const std::filesystem::path& folder);
}
