#include "UI/UpdateDialog.h"

#include "App/Engine.h"
#include "Core/BackgroundWork.h"
#include "Core/Paths.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui.h"

#include <windows.h>
#include <shellapi.h>

namespace Manifold::UI
{
    namespace
    {
        constexpr const char* kPopup = "Update##workspace";
        constexpr const char* kCheckGroup = "update-check";
        constexpr const char* kInstallGroup = "update-install";

        void OpenAddress(const std::string& address)
        {
            if (StartsWithNoCase(address, "https://"))
                ::ShellExecuteW(nullptr, L"open", Utf8ToWide(address).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        // Release notes are Markdown; the dialog shows them as text: links keep
        // their words, ** and ` go, headings and list items keep their shape.
        std::string PlainLine(std::string_view line)
        {
            std::string text;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                const char c = line[i];
                if (c == '*' && i + 1 < line.size() && line[i + 1] == '*')
                {
                    ++i;
                    continue;
                }
                if (c == '`')
                    continue;
                if (c == '[')
                {
                    const std::size_t close = line.find("](", i);
                    const std::size_t end = close == std::string_view::npos ? close : line.find(')', close);
                    if (end != std::string_view::npos)
                    {
                        text += line.substr(i + 1, close - i - 1);
                        i = end;
                        continue;
                    }
                }
                text += c;
            }
            return text;
        }
    }

    UpdateDialog::UpdateDialog(Engine& engine, Restart restart) : m_engine(engine), m_restart(std::move(restart))
    {
    }

    UpdateDialog::~UpdateDialog()
    {
        if (m_work)
            m_engine.Work().Cancel(m_work);
    }

    void UpdateDialog::CheckAtStart()
    {
        if (m_engine.Options().CheckForUpdates)
            Check(false);
    }

    void UpdateDialog::CheckNow()
    {
        Check(true);
    }

    void UpdateDialog::OpenAvailable()
    {
        if (m_available)
            Show(*m_available, Updates::NoticeFor(m_available->Version, MANIFOLD_VERSION_TEXT, m_engine.Options().UpdateSeen, {}) != Updates::Notice::Required);
    }

    void UpdateDialog::Check(bool manual)
    {
        if (m_checking || m_phase == Phase::Downloading)
            return;
        m_checking = true;
        std::weak_ptr<int> alive = m_alive;
        m_engine.Work().Start({ .Title = "Checking for updates", .Group = kCheckGroup, .Cancellable = false, .Quiet = true },
            [this, alive, manual](WorkProgress&) -> BackgroundWork::Finish
            {
                Updates::Release release;
                Status status = Updates::FetchLatest(release);
                return [this, alive, manual, status = std::move(status), release = std::move(release)]()
                    {
                        if (alive.expired())
                            return;
                        m_checking = false;
                        if (!status)
                        {
                            if (manual)
                                m_engine.Notify(status, LogLevel::Warning);
                            else
                                LOG_INFO("Updates", status.Text);
                            return;
                        }
                        if (Updates::CompareVersions(release.Version, MANIFOLD_VERSION_TEXT) <= 0)
                        {
                            m_available.reset();
                            const std::string text = "Manifold Engine " MANIFOLD_VERSION_TEXT " is the newest version.";
                            if (manual)
                                m_engine.Notify(LogLevel::Success, text);
                            else
                                LOG_INFO("Updates", text);
                            return;
                        }
                        m_available = release;
                        const Settings& options = m_engine.Options();
                        const Updates::Notice notice = Updates::NoticeFor(release.Version, MANIFOLD_VERSION_TEXT, options.UpdateSeen, options.UpdateSkipped);
                        LOG_INFO("Updates", "Version " + release.Version + " is out (this is " MANIFOLD_VERSION_TEXT ")");
                        if (manual)
                            Show(release, notice != Updates::Notice::Required);
                        else if (notice != Updates::Notice::None)
                            Show(release, notice == Updates::Notice::Skippable);
                    };
            });
    }

    void UpdateDialog::Show(const Updates::Release& release, bool skippable)
    {
        if (m_phase == Phase::Downloading)
            return;
        m_shown = release;
        m_skippable = skippable;
        m_phase = m_installed == release.Version ? Phase::Installed : Phase::Offer;
        m_error.clear();
        m_blocker = Updates::InstallBlocker(Paths::ProgramFolder());
        if (!m_blocker)
        {
            const Updates::Asset* asset = Updates::ProgramAsset(release);
            if (!asset)
                m_blocker = "This release has no program zip for 64-bit Windows. Download it from the release page.";
            else if (asset->Sha256.size() != 64)
                m_blocker = "GitHub lists no SHA-256 for " + asset->Name + ", so it is not installed unchecked. Download it from the release page.";
        }
        m_pendingOpen = true;
    }

    void UpdateDialog::Close()
    {
        if (m_phase == Phase::Downloading)
            return;
        m_phase = Phase::Hidden;
        m_pendingOpen = false;
        ImGui::CloseCurrentPopup();
    }

    void UpdateDialog::Install()
    {
        if (!m_shown || m_blocker || m_phase == Phase::Downloading)
            return;
        const Updates::Asset* found = Updates::ProgramAsset(*m_shown);
        if (!found)
            return;
        const Updates::Asset asset = *found;
        const std::string version = m_shown->Version;
        const std::filesystem::path folder = Paths::ProgramFolder();
        const std::filesystem::path backup = Paths::BackupFolder() / Utf8ToWide("update-" MANIFOLD_VERSION_TEXT);
        std::weak_ptr<int> alive = m_alive;
        m_phase = Phase::Downloading;
        m_error.clear();
        LOG_INFO("Updates", "Downloading " + asset.Name + " (" + ByteSize(asset.Size) + ")");
        m_work = m_engine.Work().Start({ .Title = "Downloading Manifold Engine " + version, .Group = kInstallGroup, .Cancellable = true, .Quiet = true },
            [this, alive, asset, version, folder, backup](WorkProgress& progress) -> BackgroundWork::Finish
            {
                progress.SetText("Downloading " + asset.Name);
                std::vector<std::uint8_t> zip;
                Status status = Updates::Fetch(asset.Url, zip, &progress, Updates::kMaxDownload, ErrorCode::UpdateDownloadFailed);
                if (status && asset.Size && zip.size() != asset.Size)
                    status = Status::Fail(ErrorCode::UpdateDownloadFailed, Format("it has %zu bytes, GitHub lists %llu", zip.size(), static_cast<unsigned long long>(asset.Size)));
                if (status)
                {
                    progress.SetText("Checking the SHA-256");
                    const std::string sum = Updates::Sha256Hex(zip);
                    if (sum != asset.Sha256)
                        status = Status::Fail(ErrorCode::UpdateDownloadFailed, "its SHA-256 is " + sum + ", GitHub lists " + asset.Sha256);
                }
                std::vector<Updates::File> files;
                if (status)
                {
                    progress.SetText("Unpacking");
                    status = Updates::Unpack(zip, files);
                }
                if (status && progress.Cancelled())
                    status = Status::Fail(ErrorCode::UpdateDownloadFailed, "cancelled");
                Updates::Applied applied;
                if (status)
                {
                    progress.SetText("Installing");
                    status = Updates::Apply(folder, files, backup, &applied);
                }
                return [this, alive, status = std::move(status), applied = std::move(applied), version, backup]()
                    {
                        if (alive.expired())
                            return;
                        m_work = 0;
                        if (!status)
                        {
                            m_phase = Phase::Failed;
                            m_error = status.Text;
                            LOG_WARN("Updates", status.Text);
                            return;
                        }
                        m_phase = Phase::Installed;
                        m_installed = version;
                        m_result = Format("%d files replaced, %d added, %d unchanged.", applied.Replaced, applied.Added, applied.Unchanged);
                        if (!applied.Kept.empty())
                            m_result += " The earlier text of " + std::to_string(applied.Kept.size()) + " changed autorun files is in " + WideToUtf8(backup.wstring()) + ".";
                        LOG_INFO("Updates", "Version " + version + " installed: " + m_result);
                    };
            });
    }

    void UpdateDialog::DrawNotes(const std::string& notes)
    {
        Layout::Panel panel("##updatenotes", 0.0f, Px(240.0f));
        if (!panel.Visible())
            return;
        bool any = false;
        for (const std::string& raw : SplitLines(notes))
        {
            const std::string_view line = TrimView(raw);
            if (line.empty())
            {
                if (any)
                    Layout::Spacing(Layout::Space::S);
                continue;
            }
            any = true;
            if (line.starts_with("#"))
            {
                const std::size_t start = line.find_first_not_of("# ");
                Layout::Heading(PlainLine(line.substr(start == std::string_view::npos ? line.size() : start)), Layout::Overflow::Wrap);
            }
            else if (line.starts_with("- ") || line.starts_with("* "))
                Layout::Wrapped("\xE2\x80\xA2  " + PlainLine(line.substr(2)));
            else
                Layout::Wrapped(PlainLine(line));
        }
        if (!any)
            Layout::Muted("This release has no notes.");
    }

    void UpdateDialog::DrawButtons()
    {
        enum Button
        {
            Install,
            Page,
            Later,
            Skip,
            Cancel,
            RestartNow,
            Again,
            CloseIt,
        };
        std::vector<std::pair<Button, std::string>> buttons;
        switch (m_phase)
        {
        case Phase::Offer:
            if (!m_blocker)
                buttons.emplace_back(Install, ICON_FA_DOWNLOAD "  Install Update");
            buttons.emplace_back(Page, ICON_FA_EXTERNAL_LINK_ALT "  Open Release Page");
            buttons.emplace_back(Later, "Later");
            if (m_skippable)
                buttons.emplace_back(Skip, "Skip This Version");
            break;
        case Phase::Downloading:
            buttons.emplace_back(Cancel, "Cancel");
            break;
        case Phase::Installed:
            buttons.emplace_back(RestartNow, ICON_FA_REDO "  Restart Now");
            buttons.emplace_back(Later, "Later");
            break;
        case Phase::Failed:
            buttons.emplace_back(Again, ICON_FA_REDO "  Try Again");
            buttons.emplace_back(Page, ICON_FA_EXTERNAL_LINK_ALT "  Open Release Page");
            buttons.emplace_back(CloseIt, "Close");
            break;
        case Phase::Hidden:
            return;
        }
        std::optional<Button> clicked;
        {
            Layout::Row row("##updatebuttons", std::vector<Layout::Size>(buttons.size(), Layout::Size::Fit()));
            for (std::size_t i = 0; i < buttons.size(); ++i)
            {
                row.Cell([&] {
                    Layout::ButtonStyle style;
                    style.Kind = i == 0 && buttons[i].first != Cancel && buttons[i].first != CloseIt ? Layout::ButtonKind::Accent : Layout::ButtonKind::Normal;
                    if (Layout::Button(buttons[i].second, style))
                        clicked = buttons[i].first;
                });
            }
        }
        if (!clicked || !m_shown)
            return;
        switch (*clicked)
        {
        case Install:
        case Again: this->Install(); break;
        case Page: OpenAddress(m_shown->Page); break;
        case Later:
        case CloseIt: Close(); break;
        case Skip:
            m_engine.Options().UpdateSkipped = m_shown->Version;
            m_engine.SaveOptionsSoon();
            LOG_INFO("Updates", "Version " + m_shown->Version + " is skipped");
            Close();
            break;
        case Cancel:
            if (m_work)
                m_engine.Work().Cancel(m_work);
            break;
        case RestartNow:
        {
            m_phase = Phase::Hidden;
            ImGui::CloseCurrentPopup();
            m_restart(MANIFOLD_VERSION_TEXT);
            break;
        }
        }
    }

    void UpdateDialog::Draw()
    {
        if (m_phase == Phase::Hidden || !m_shown)
            return;
        if (m_pendingOpen)
        {
            // another dialog first (the table script prompt, backups, About): this one waits
            const bool open = ImGui::IsPopupOpen(kPopup);
            if (!open && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
                return;
            if (!open)
                ImGui::OpenPopup(kPopup);
            m_pendingOpen = false;
            // ∑ the required notice counts as shown once it is on screen; the next start offers Skip
            if (!m_skippable && m_engine.Options().UpdateSeen != m_shown->Version)
            {
                m_engine.Options().UpdateSeen = m_shown->Version;
                m_engine.SaveOptionsSoon();
            }
        }
        bool open = true;
        const bool closable = m_skippable && m_phase != Phase::Downloading;
        const char* title = m_phase == Phase::Installed ? "Update Installed" : m_phase == Phase::Failed ? "Update Failed" : "Update Available";
        if (!BeginModal(kPopup, ICON_FA_ARROW_CIRCLE_UP, title, ImVec2(560.0f, 0.0f), closable ? &open : nullptr))
        {
            if (!open || !ImGui::IsPopupOpen(kPopup))
            {
                if (m_phase != Phase::Downloading)
                    m_phase = Phase::Hidden;
            }
            return;
        }
        const Updates::Release& release = *m_shown;
        Layout::Heading("Manifold Engine " + release.Version);
        std::string line = "You have " MANIFOLD_VERSION_TEXT;
        if (release.Published.size() >= 10)
            line += ", this one came out on " + release.Published.substr(0, 10);
        Layout::Muted(line + ".");
        Layout::Spacing(Layout::Space::S);

        switch (m_phase)
        {
        case Phase::Offer:
            DrawNotes(release.Notes);
            if (m_blocker)
            {
                Layout::Spacing(Layout::Space::S);
                Layout::Colored(*m_blocker, ManifoldTheme::Warning, Layout::Overflow::Wrap);
            }
            else
                Layout::Muted("Install downloads the release zip, checks its SHA-256 and replaces the program files. Tables, settings and your own files stay.", Layout::Overflow::Wrap);
            break;
        case Phase::Downloading:
        {
            const std::shared_ptr<WorkProgress> progress = m_engine.Work().ProgressOf(m_work);
            const double fraction = progress ? progress->Fraction() : -1.0;
            Layout::Text(progress ? progress->Text() : std::string("Downloading"));
            ImGui::ProgressBar(fraction >= 0.0 ? static_cast<float>(fraction) : static_cast<float>(-ImGui::GetTime()), ImVec2(-1.0f, 0.0f),
                progress && progress->Total() > 0 ? (ByteSize(static_cast<std::uint64_t>(progress->Done())) + " of " + ByteSize(static_cast<std::uint64_t>(progress->Total()))).c_str() : "");
            break;
        }
        case Phase::Installed:
            Layout::Colored("Version " + release.Version + " is installed. It runs from the next start; Restart Now closes the program the usual way and starts it again.",
                ManifoldTheme::Success, Layout::Overflow::Wrap);
            Layout::Muted(m_result, Layout::Overflow::Wrap);
            break;
        case Phase::Failed:
            Layout::Colored(m_error, ManifoldTheme::Error, Layout::Overflow::Wrap);
            Layout::Muted("Nothing was changed, or the program files were put back. The release page has the zip to unpack by hand.", Layout::Overflow::Wrap);
            break;
        case Phase::Hidden:
            break;
        }
        Layout::Spacing(Layout::Space::S);
        DrawButtons();
        EndModal();
    }
}
