#include "TestKit.h"

#include "App/Updates.h"
#include "Core/Deflate.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Core/Zip.h"
#include "Text/JsonValue.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The update check and installer: version order, the once-required notice,
// the GitHub answer, the zip reader and the file swap. Everything happens in
// the tests' own data folder; no request leaves the machine.
namespace Manifold::Tests
{
    namespace
    {
        using Bytes = std::vector<std::uint8_t>;

        Bytes ToBytes(std::string_view text)
        {
            return Bytes(text.begin(), text.end());
        }

        std::string ReadText(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        void WriteText(const std::filesystem::path& path, std::string_view text)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary);
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
        }

        void Put16(Bytes& out, std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value));
            out.push_back(static_cast<std::uint8_t>(value >> 8));
        }

        void Put32(Bytes& out, std::uint32_t value)
        {
            Put16(out, value & 0xFFFF);
            Put16(out, value >> 16);
        }

        // A zip like Compress-Archive writes: deflated entries when that is smaller, a central directory, an end record.
        Bytes MakeZip(const std::vector<std::pair<std::string, std::string>>& files)
        {
            Bytes zip;
            Bytes directory;
            for (const auto& [name, text] : files)
            {
                const Bytes data = ToBytes(text);
                const std::uint32_t crc = Zip::Crc32(data);
                Bytes packed = data;
                std::uint16_t method = 0;
                if (auto deflated = Deflate::Compress(data); deflated && deflated->size() < data.size())
                {
                    packed = std::move(*deflated);
                    method = 8;
                }
                const std::uint32_t offset = static_cast<std::uint32_t>(zip.size());
                Put32(zip, 0x04034B50);
                Put16(zip, 20);
                Put16(zip, 0);
                Put16(zip, method);
                Put32(zip, 0);
                Put32(zip, crc);
                Put32(zip, static_cast<std::uint32_t>(packed.size()));
                Put32(zip, static_cast<std::uint32_t>(data.size()));
                Put16(zip, static_cast<std::uint32_t>(name.size()));
                Put16(zip, 0);
                zip.insert(zip.end(), name.begin(), name.end());
                zip.insert(zip.end(), packed.begin(), packed.end());

                Put32(directory, 0x02014B50);
                Put16(directory, 20);
                Put16(directory, 20);
                Put16(directory, 0);
                Put16(directory, method);
                Put32(directory, 0);
                Put32(directory, crc);
                Put32(directory, static_cast<std::uint32_t>(packed.size()));
                Put32(directory, static_cast<std::uint32_t>(data.size()));
                Put16(directory, static_cast<std::uint32_t>(name.size()));
                Put16(directory, 0);
                Put16(directory, 0);
                Put16(directory, 0);
                Put16(directory, 0);
                Put32(directory, 0);
                Put32(directory, offset);
                directory.insert(directory.end(), name.begin(), name.end());
            }
            const std::uint32_t start = static_cast<std::uint32_t>(zip.size());
            zip.insert(zip.end(), directory.begin(), directory.end());
            Put32(zip, 0x06054B50);
            Put16(zip, 0);
            Put16(zip, 0);
            Put16(zip, static_cast<std::uint32_t>(files.size()));
            Put16(zip, static_cast<std::uint32_t>(files.size()));
            Put32(zip, static_cast<std::uint32_t>(directory.size()));
            Put32(zip, start);
            Put16(zip, 0);
            return zip;
        }

        void Versions()
        {
            using Updates::CompareVersions;
            CHECK(CompareVersions("1.1.0", "1.0.0") > 0);
            CHECK(CompareVersions("v1.0.0", "1.0.0") == 0);
            CHECK(CompareVersions("1.0", "1.0.0") == 0);
            CHECK(CompareVersions("1.10.0", "1.9.3") > 0);
            CHECK(CompareVersions("2.0.0", "10.0.0") < 0);
            CHECK(CompareVersions("1.1.0-beta", "1.1.0") < 0);
            CHECK(CompareVersions("1.1.0-beta", "1.0.9") > 0);
            CHECK(CompareVersions("1.1.0+abc", "1.1.0") == 0);
        }

        // ∑ Owner rule: once without a way out, then skippable, then quiet until a newer one.
        void Notices()
        {
            using Updates::Notice;
            using Updates::NoticeFor;
            CHECK(NoticeFor("1.1.0", "1.1.0", "", "") == Notice::None);
            CHECK(NoticeFor("1.0.0", "1.1.0", "", "") == Notice::None);
            CHECK(NoticeFor("1.1.0", "1.0.0", "", "") == Notice::Required);
            CHECK(NoticeFor("1.1.0", "1.0.0", "1.1.0", "") == Notice::Skippable);
            CHECK(NoticeFor("1.1.0", "1.0.0", "1.1.0", "1.1.0") == Notice::None);
            // a newer version than the one seen or skipped is required again
            CHECK(NoticeFor("1.2.0", "1.0.0", "1.1.0", "1.1.0") == Notice::Required);
            CHECK(NoticeFor("v1.2.0", "1.0.0", "1.2.0", "") == Notice::Skippable);
        }

        void Json()
        {
            std::string error;
            const auto value = Text::ParseJson(R"({"a": [1, 2.5, -3e2], "b": {"c": "x\u00e4\ud83d\ude00\n"}, "t": true, "n": null})", &error);
            CHECK_MSG(value && value->IsObject(), error);
            if (value)
            {
                const Text::JsonValue* a = value->Find("a");
                CHECK(a && a->IsArray() && a->Items.size() == 3 && a->Items[1].Number == 2.5 && a->Items[2].Number == -300.0);
                const Text::JsonValue* b = value->Find("b");
                CHECK(b && b->Text("c") == "x\xC3\xA4\xF0\x9F\x98\x80\n");
                CHECK(value->BoolOf("t") && value->Find("n") && value->Find("n")->Type == Text::JsonValue::Kind::Null);
                CHECK(value->Text("missing").empty() && value->NumberOf("a", 7.0) == 7.0);
            }
            CHECK(!Text::ParseJson("{\"a\": }"));
            CHECK(!Text::ParseJson("[1, 2"));
            CHECK(!Text::ParseJson("{} x"));
            CHECK(!Text::ParseJson(std::string(1000, '[')));
        }

        void Release()
        {
            const std::string sum(64, 'a');
            const std::string json = R"({"tag_name": "v1.1.0", "name": "Manifold Engine 1.1.0", "html_url": "https://github.com/x/y/releases/tag/v1.1.0",
                "published_at": "2026-12-01T10:00:00Z", "prerelease": false,
                "body": "## Highlights\r\n\r\n- **New** thing\r\n\r\n### SHA-256\r\n\r\n```text\r\n)" + sum + R"(  Manifold-Engine-1.1.0-win-x64-symbols.zip\r\n```\r\n",
                "assets": [
                  {"name": "Manifold-Engine-1.1.0-win-x64.zip", "browser_download_url": "https://example.invalid/a.zip", "size": 1234, "digest": "sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},
                  {"name": "Manifold-Engine-1.1.0-win-x64-symbols.zip", "browser_download_url": "https://example.invalid/b.zip", "size": 99}
                ]})";
            std::string error;
            const auto release = Updates::ParseRelease(json, &error);
            CHECK_MSG(release.has_value(), error);
            if (!release)
                return;
            CHECK(release->Version == "1.1.0" && release->Tag == "v1.1.0" && !release->Prerelease);
            CHECK_MSG(release->Notes == "## Highlights\n\n- **New** thing", release->Notes);
            const Updates::Asset* program = Updates::ProgramAsset(*release);
            CHECK(program && program->Size == 1234 && program->Sha256 == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
            // no digest: the sum comes from the release text
            CHECK(release->Assets.size() == 2 && release->Assets[1].Sha256 == sum);
            CHECK(!Updates::ParseRelease(R"({"tag_name": "latest"})"));
            CHECK(!Updates::ParseRelease("[]"));
        }

        void Sha()
        {
            CHECK(Updates::Sha256Hex(ToBytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
            CHECK(Zip::Crc32(ToBytes("123456789")) == 0xCBF43926u);
        }

        void Unpacking()
        {
            const std::string big(5000, 'x');
            std::vector<Updates::File> files;
            CHECK(Updates::Unpack(MakeZip({ { "Manifold-Engine-1.1.0-win-x64/Manifold-Engine.exe", "new exe" }, { "Manifold-Engine-1.1.0-win-x64/autorun/modules/a.lua", big } }), files).Ok());
            CHECK(files.size() == 2 && files[0].Path == "Manifold-Engine.exe" && files[1].Path == "autorun/modules/a.lua" && files[1].Data.size() == big.size());
            // backslashes as Windows PowerShell 5.1 writes them
            CHECK(Updates::Unpack(MakeZip({ { "top\\Manifold-Engine.exe", "x" } }), files).Ok() && files.size() == 1 && files[0].Path == "Manifold-Engine.exe");
            CHECK(!Updates::Unpack(MakeZip({ { "top/Manifold-Engine.exe", "x" }, { "top/../evil.dll", "x" } }), files).Ok());
            CHECK(!Updates::Unpack(MakeZip({ { "top/Manifold-Engine.exe", "x" }, { "top/C:/evil.dll", "x" } }), files).Ok());
            CHECK(!Updates::Unpack(MakeZip({ { "top/readme.txt", "x" } }), files).Ok());
            CHECK(!Updates::Unpack(MakeZip({ { "top/Manifold-Engine.exe", "x" }, { "top/a.dll.update-old", "x" } }), files).Ok());
            Bytes damaged = MakeZip({ { "top/Manifold-Engine.exe", "abcdef" } });
            damaged[30 + 23 + 2] ^= 0xFF;   // a byte of the stored data, after the 30-byte header and the 23-byte name
            CHECK(!Updates::Unpack(damaged, files).Ok());
            CHECK(!Updates::Unpack(ToBytes("not a zip at all, but long enough"), files).Ok());
        }

        std::vector<Updates::File> Release2()
        {
            return {
                { "Manifold-Engine.exe", ToBytes("exe 2") },
                { "ManifoldAgent.dll", ToBytes("agent 2") },
                { "autorun/modules/shipped.lua", ToBytes("shipped 2") },
                { "autorun/modules/same.lua", ToBytes("same") },
                { "Licenses/New.txt", ToBytes("license") },
            };
        }

        void Install(const std::filesystem::path& root)
        {
            const std::filesystem::path folder = root / "program";
            const std::filesystem::path backup = root / "backup";
            // the running program: a real image, mapped like a started exe (renamable, not deletable)
            wchar_t system[MAX_PATH] = {};
            ::GetSystemDirectoryW(system, MAX_PATH);
            const std::string oldExe = ReadText(std::filesystem::path(system) / "version.dll");
            CHECK(oldExe.size() > 1024);
            WriteText(folder / "Manifold-Engine.exe", oldExe);
            WriteText(folder / "ManifoldAgent.dll", "agent 1");
            WriteText(folder / "autorun/modules/shipped.lua", "shipped 1, edited");
            WriteText(folder / "autorun/modules/same.lua", "same");
            WriteText(folder / "autorun/modules/mine.lua", "mine");
            WriteText(folder / "configs/settings.ini", "keep");

            CHECK(!Updates::InstallBlocker(folder));
            const HMODULE running = ::LoadLibraryExW((folder / "Manifold-Engine.exe").c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            CHECK(running != nullptr);

            Updates::Applied applied;
            const Status status = Updates::Apply(folder, Release2(), backup, &applied);
            CHECK_MSG(status.Ok(), status.Text);
            CHECK(ReadText(folder / "Manifold-Engine.exe") == "exe 2" && ReadText(folder / "ManifoldAgent.dll") == "agent 2");
            CHECK(ReadText(folder / "autorun/modules/shipped.lua") == "shipped 2" && ReadText(folder / "Licenses/New.txt") == "license");
            CHECK(ReadText(folder / "autorun/modules/mine.lua") == "mine" && ReadText(folder / "configs/settings.ini") == "keep");
            CHECK_MSG(applied.Replaced == 3 && applied.Added == 1 && applied.Unchanged == 1, Format("%d %d %d", applied.Replaced, applied.Added, applied.Unchanged));
            // the edited autorun file is kept in backup
            CHECK(applied.Kept.size() == 1 && ReadText(backup / "autorun/modules/shipped.lua") == "shipped 1, edited");
            CHECK(ReadText(folder / "Manifold-Engine.exe.update-old") == oldExe);
            CHECK(!std::filesystem::exists(folder / "Manifold-Engine.exe.update-new"));
            const std::string listed = ReadText(folder / "update-leftovers.txt");
            CHECK_MSG(listed.find("Manifold-Engine.exe.update-old") != std::string::npos && listed.find("ManifoldAgent.dll.update-old") != std::string::npos, listed);

            // still running: the old file stays listed, the agent's goes
            std::vector<std::string> still = Updates::RemoveLeftovers(folder);
            CHECK(still.size() == 1 && still[0] == "Manifold-Engine.exe.update-old");
            CHECK(!std::filesystem::exists(folder / "ManifoldAgent.dll.update-old"));
            // a second update while the first old program still runs takes the next name
            CHECK(Updates::Apply(folder, { { "Manifold-Engine.exe", ToBytes("exe 3") } }, backup).Ok());
            CHECK(ReadText(folder / "Manifold-Engine.exe") == "exe 3" && ReadText(folder / "Manifold-Engine.exe.update-old") == oldExe);
            CHECK(ReadText(folder / "Manifold-Engine.exe.update-old-2") == "exe 2");
            ::FreeLibrary(running);
            still = Updates::RemoveLeftovers(folder);
            CHECK_MSG(still.empty(), Join(still, ", "));
            CHECK(!std::filesystem::exists(folder / "Manifold-Engine.exe.update-old") && !std::filesystem::exists(folder / "Manifold-Engine.exe.update-old-2"));
            CHECK(!std::filesystem::exists(folder / "update-leftovers.txt"));

            // the leftover list never deletes anything that is not an installer leftover
            WriteText(folder / "update-leftovers.txt", "configs/settings.ini\r\n..\\outside.update-old\r\n");
            CHECK(Updates::RemoveLeftovers(folder).empty() && ReadText(folder / "configs/settings.ini") == "keep");
        }

        void Rollback(const std::filesystem::path& root)
        {
            const std::filesystem::path folder = root / "rollback";
            WriteText(folder / "Manifold-Engine.exe", "exe 1");
            WriteText(folder / "ManifoldClr.exe", "clr 1");
            // held without delete sharing: it cannot be renamed, so the swap fails at it
            const HANDLE held = ::CreateFileW((folder / "ManifoldClr.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            const Status status = Updates::Apply(folder, { { "Manifold-Engine.exe", ToBytes("exe 2") }, { "ManifoldClr.exe", ToBytes("clr 2") } }, root / "backup");
            ::CloseHandle(held);
            CHECK(!status.Ok() && status.Code == ErrorCode::UpdateInstallFailed);
            CHECK_MSG(ReadText(folder / "Manifold-Engine.exe") == "exe 1" && ReadText(folder / "ManifoldClr.exe") == "clr 1", ReadText(folder / "Manifold-Engine.exe"));
            CHECK(!std::filesystem::exists(folder / "Manifold-Engine.exe.update-new") && !std::filesystem::exists(folder / "ManifoldClr.exe.update-new"));
            CHECK(!std::filesystem::exists(folder / "Manifold-Engine.exe.update-old"));
        }

        void Blocker(const std::filesystem::path& root)
        {
            WriteText(root / "source/Manifold-Engine.sln", "");
            std::filesystem::create_directories(root / "source/Build/x64/Release");
            CHECK(Updates::InstallBlocker(root / "source/Build/x64/Release").has_value());
            CHECK(Updates::InstallBlocker(root / "does-not-exist").has_value());
        }

        void LocalFeed(const std::filesystem::path& root)
        {
            const std::filesystem::path feed = root / "feed.json";
            WriteText(feed, R"({"tag_name": "v9.0.0", "assets": []})");
            ::SetEnvironmentVariableW(L"MANIFOLD_UPDATE_FEED", feed.c_str());
            Updates::Release release;
            const Status status = Updates::FetchLatest(release);
            ::SetEnvironmentVariableW(L"MANIFOLD_UPDATE_FEED", nullptr);
            CHECK_MSG(status.Ok() && release.Version == "9.0.0" && !release.Page.empty(), status.Text);
            CHECK(Updates::FeedAddress() == Updates::kLatestRelease);
            Bytes body;
            CHECK(!Updates::Fetch("http://example.invalid/", body, nullptr, 100).Ok());
        }
    }

    int RunUpdateTests()
    {
        const int before = Totals().Failures;
        const std::filesystem::path root = Paths::DataFolder() / "update-tests";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        Versions();
        Notices();
        Json();
        Release();
        Sha();
        Unpacking();
        Install(root);
        Rollback(root);
        Blocker(root);
        LocalFeed(root);
        std::filesystem::remove_all(root, ignored);
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("updates", "Update check and installer", 133, [](const SuiteContext&) { return RunUpdateTests(); });
}
