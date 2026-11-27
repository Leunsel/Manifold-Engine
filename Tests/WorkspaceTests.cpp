#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "App/Updates.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Layout/Layout.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/UpdateDialog.h"

#include <windows.h>
#include "UI/Workspace.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace Manifold::Tests
{
    namespace
    {
        using UI::Shell;
        using UI::Workspace;

        // ∑ The program's window, without a window: it records what the workspace asks of it. Device() is null, which every
        //   texture path of the UI takes as "no textures".
        class FakeShell final : public Shell
        {
        public:
            void Minimise() override {}
            void ToggleMaximised() override {}
            bool Maximised() const override { return false; }
            void Close() override { ++Closes; }
            void AddDragArea(float, float, float, float) override {}
            float CornerRadius() const override { return 0.0f; }
            void* NativeWindow() const override { return nullptr; }
            void* Device() const override { return nullptr; }
            void RestartElevated() override { ++ElevatedRestarts; }
            void RestartAfterUpdate(const std::string&) override { ++UpdateRestarts; }
            void Restyle() override {}

            int Closes = 0;
            int ElevatedRestarts = 0;
            int UpdateRestarts = 0;
        };

        // ∑ An ImGui context without a GPU, set up the way the program's quiet window is (docking, no platform windows). The
        //   font atlas's textures are marked as made the way a renderer backend would.
        class UiFrames
        {
        public:
            UiFrames()
            {
                m_context = ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.LogFilename = nullptr;
                io.DisplaySize = ImVec2(1600.0f, 1000.0f);
                io.DeltaTime = 1.0f / 60.0f;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
                io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
                io.Fonts->AddFontDefault();
            }

            ~UiFrames()
            {
                for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
                {
                    if (texture->RefCount == 1)
                    {
                        texture->SetTexID(ImTextureID_Invalid);
                        texture->SetStatus(ImTextureStatus_Destroyed);
                    }
                }
                ImGui::DestroyContext(m_context);
            }

            UiFrames(const UiFrames&) = delete;
            UiFrames& operator=(const UiFrames&) = delete;

            void Frame(const std::function<void()>& draw)
            {
                ImGui::NewFrame();
                Layout::NewFrame();
                draw();
                Layout::Audit::EndFrame();
                ImGui::Render();
                for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
                {
                    if (texture->Status == ImTextureStatus_WantCreate)
                    {
                        texture->SetTexID(static_cast<ImTextureID>(1));
                        texture->SetStatus(ImTextureStatus_OK);
                    }
                    else if (texture->Status == ImTextureStatus_WantUpdates)
                        texture->SetStatus(ImTextureStatus_OK);
                    else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0)
                    {
                        texture->SetTexID(ImTextureID_Invalid);
                        texture->SetStatus(ImTextureStatus_Destroyed);
                    }
                }
            }

            void Frames(Workspace& workspace, int count)
            {
                for (int i = 0; i < count; ++i)
                    Frame([&] { workspace.Draw(); });
            }

            // ∑ A click on a button or menu item drawn directly in a window (not in a child of it): its ID is the label hashed
            //   with the window's, and ImGui presses it on the next frame as keyboard navigation would.
            bool Activate(Workspace& workspace, const std::string& window, const char* label)
            {
                bool found = false;
                Frame([&] {
                    for (ImGuiWindow* candidate : ImGui::GetCurrentContext()->Windows)
                    {
                        if (!found && candidate->WasActive && std::string(candidate->Name).find(window) != std::string::npos)
                        {
                            ImGui::FocusWindow(candidate);
                            ImGui::ActivateItemByID(ImHashStr(label, 0, candidate->ID));
                            found = true;
                        }
                    }
                    workspace.Draw();
                });
                Frame([&] { workspace.Draw(); });
                Frame([&] { workspace.Draw(); });
                return found;
            }

            // A key pressed and let go; with a name, first the window whose name contains it gets the focus.
            void Key(Workspace& workspace, ImGuiKey key, const std::string& window = {})
            {
                if (!window.empty())
                {
                    Frame([&] {
                        for (ImGuiWindow* candidate : ImGui::GetCurrentContext()->Windows)
                        {
                            if (std::string(candidate->Name).find(window) != std::string::npos)
                                ImGui::FocusWindow(candidate);
                        }
                        workspace.Draw();
                    });
                }
                ImGui::GetIO().AddKeyEvent(key, true);
                Frame([&] { workspace.Draw(); });
                ImGui::GetIO().AddKeyEvent(key, false);
                Frame([&] { workspace.Draw(); });
            }

            // A key chord pressed and let go while the window whose name ends in windowId has the focus.
            void Press(Workspace& workspace, const std::string& windowId, ImGuiKey key, ImGuiKeyChord mods)
            {
                ImGuiIO& io = ImGui::GetIO();
                const auto set = [&](bool down)
                    {
                        if (mods & ImGuiMod_Ctrl)
                        {
                            io.AddKeyEvent(ImGuiMod_Ctrl, down);
                            io.AddKeyEvent(ImGuiKey_LeftCtrl, down);
                        }
                        io.AddKeyEvent(key, down);
                    };
                Frame([&] {
                    ImGui::SetWindowFocus(windowId.c_str());
                    workspace.Draw();
                });
                set(true);
                Frame([&] { workspace.Draw(); });
                set(false);
                Frame([&] { workspace.Draw(); });
            }

        private:
            ImGuiContext* m_context = nullptr;
        };

        void Smoke()
        {
            Settings settings;
            Engine engine(settings);
            UiFrames frames;
            FakeShell shell;
            {
                Workspace workspace(engine, shell);
                frames.Frames(workspace, 5);
                workspace.OpenAbout();
                frames.Frames(workspace, 3);
                const ImGuiWindow* about = ImGui::FindWindowByName("About##workspace");
                CHECK_MSG(about && about->Active, "the About dialog did not open");
            }
            CHECK_EQ(shell.Closes, 0);
        }

        std::filesystem::path TestFolder()
        {
            const std::filesystem::path folder = Paths::DataFolder() / L"workspace";
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            return folder;
        }

        std::filesystem::path WriteGroups(const char* name, const char* tag)
        {
            const std::filesystem::path path = TestFolder() / name;
            std::ofstream file(path, std::ios::binary);
            file << "<CheatTable><CheatEntries>";
            for (const int id : { 1, 2, 5 })
                file << "<CheatEntry><ID>" << id << "</ID><Description>\"" << tag << " " << id << "\"</Description><GroupHeader>1</GroupHeader></CheatEntry>";
            file << "</CheatEntries></CheatTable>";
            return path;
        }

        // U3: Delete Records asks first; a table dropped while the question is open replaces the table, and answering Delete
        // must not delete the records of the same IDs in the new table.
        void DeleteConfirmAfterDrop()
        {
            const std::filesystem::path a = WriteGroups("delete-a.CT", "a");
            const std::filesystem::path b = WriteGroups("delete-b.CT", "b");
            Settings settings;
            Engine engine(settings);
            UiFrames frames;
            FakeShell shell;
            Workspace workspace(engine, shell);
            CHECK(engine.LoadTable(a, false).Ok());
            frames.Frames(workspace, 3);
            engine.Table().Select(1, false);
            engine.Table().Select(2, true);
            engine.Table().Select(5, true);
            frames.Key(workspace, ImGuiKey_Delete, "##addresslist");
            frames.Frames(workspace, 2);
            bool asked = false;
            for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
                asked = asked || (window->Active && std::string(window->Name).find("Delete Records") != std::string::npos);
            CHECK_MSG(asked, "Delete Records did not ask");
            workspace.DropFiles({ b }, false);
            frames.Frames(workspace, 3);
            const Table::MemoryRecord* before = engine.Table().FindById(5);
            CHECK_MSG(before && before->Description.find("b 5") != std::string::npos, "the dropped table did not load");
            frames.Key(workspace, ImGuiKey_Enter);
            frames.Frames(workspace, 2);
            bool stillAsking = false;
            for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
                stillAsking = stillAsking || (window->Active && std::string(window->Name).find("Delete Records") != std::string::npos);
            CHECK_MSG(!stillAsking, "Enter did not answer Delete Records");
            std::string left;
            for (const int id : { 1, 2, 5 })
                left += engine.Table().FindById(id) ? std::to_string(id) + " " : "";
            CHECK_MSG(left == "1 2 5 ", "records of the dropped table left after answering Delete: " + (left.empty() ? std::string("none") : left));
        }

        std::filesystem::path WriteTable(const std::filesystem::path& folder, const char* name, const std::string& script)
        {
            const std::filesystem::path path = folder / name;
            std::ofstream(path, std::ios::binary) << "<CheatTable><CheatEntries><CheatEntry><ID>3</ID><Description>\"Script\"</Description>"
                "<VariableType>Auto Assembler Script</VariableType><AssemblerScript>" << script << "</AssemblerScript></CheatEntry></CheatEntries></CheatTable>";
            return path;
        }

        std::string ScriptWindowId()
        {
            for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
            {
                const std::string name = window->Name;
                if (const std::size_t at = name.find("###script"); at != std::string::npos && name.find('/') == std::string::npos)
                    return name.substr(at);
            }
            return {};
        }

        // U1: a script editor opened on a record of one table, then another table loaded: Save must not write the first table's
        // script into the record of the same ID in the second.
        void EditorAfterTableReplaced()
        {
            const std::filesystem::path folder = TestFolder();
            const std::filesystem::path a = WriteTable(folder, "a.CT", "[ENABLE]\n// table a\n[DISABLE]\n");
            const std::filesystem::path b = WriteTable(folder, "b.CT", "[ENABLE]\n// table b\n[DISABLE]\n");
            Settings settings;
            Engine engine(settings);
            UiFrames frames;
            FakeShell shell;
            Workspace workspace(engine, shell);
            CHECK(engine.LoadTable(a, false).Ok());
            const Table::MemoryRecord* record = engine.Table().FindById(3);
            CHECK(record != nullptr);
            if (!record)
                return;
            workspace.OpenScript(record->Script, 3);
            frames.Frames(workspace, 5);
            const std::string window = ScriptWindowId();
            CHECK_MSG(!window.empty(), "no script window");
            CHECK(engine.LoadTable(b, false).Ok());
            frames.Frames(workspace, 2);
            frames.Press(workspace, window, ImGuiKey_S, ImGuiMod_Ctrl);
            bool refused = false;
            for (const ImGuiWindow* shown : ImGui::GetCurrentContext()->Windows)
                refused = refused || (shown->Active && std::string(shown->Name).find("###scripterror") != std::string::npos);
            CHECK_MSG(refused, "Ctrl+S in the editor of table a did not say that it did not save");
            const Table::MemoryRecord* other = engine.Table().FindById(3);
            CHECK_MSG(other && other->Script.find("table b") != std::string::npos,
                "table b's record 3 after Save in the editor opened for table a: " + (other ? ReplaceAll(other->Script, "\n", " | ") : std::string("missing")));
            if (other)
                workspace.OpenScript(other->Script, 3);
            frames.Frames(workspace, 3);
            int editors = 0;
            for (const ImGuiWindow* candidate : ImGui::GetCurrentContext()->Windows)
            {
                const std::string name = candidate->Name;
                const std::size_t at = name.find("###script");
                editors += candidate->Active && at != std::string::npos && name.size() > at + 9 &&
                    name.find_first_not_of("0123456789", at + 9) == std::string::npos;
            }
            CHECK_MSG(editors == 2, "opening record 3 of table b gave " + std::to_string(editors) + " script editors: the one of table a came back instead of a new one");
        }

        bool ShowsSaveError()
        {
            for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
            {
                if (window->Active && std::string(window->Name).find("###scripterror") != std::string::npos)
                    return true;
            }
            return false;
        }

        // An Auto Assemble window opened before a table, then Add to Table: the record is in the current table, so Save saves.
        void AddToTableAfterNewTable()
        {
            Settings settings;
            Engine engine(settings);
            UiFrames frames;
            FakeShell shell;
            Workspace workspace(engine, shell);
            workspace.OpenScript("[ENABLE]\n// free\n[DISABLE]\n", UI::kNoRecord);
            frames.Frames(workspace, 5);
            engine.NewTable();
            frames.Frames(workspace, 2);
            const std::string window = ScriptWindowId();
            CHECK_MSG(frames.Activate(workspace, window, ICON_FA_TABLE "  Add to Table"), "no script window");
            std::size_t records = 0;
            engine.Table().ForEach([&](Table::MemoryRecord&) { ++records; });
            CHECK_MSG(records == 1, "Add to Table made " + std::to_string(records) + " records");
            frames.Press(workspace, window, ImGuiKey_S, ImGuiMod_Ctrl);
            CHECK_MSG(!ShowsSaveError(), "Save of a script added to the current table was refused");
        }

        bool ShowsWindow(const std::string& part)
        {
            for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
            {
                if (window->Active && std::string(window->Name).find(part) != std::string::npos)
                    return true;
            }
            return false;
        }

        // U4: a manual check that finds a newer version while the update dialog is already open must leave it open and taking
        // input (a second Show on an open dialog).
        void UpdateDialogShownTwice()
        {
            const std::filesystem::path feed = TestFolder() / "feed.json";
            std::ofstream(feed, std::ios::binary) << R"({"tag_name": "v99.0.0", "name": "Manifold Engine 99.0.0", "assets": []})";
            ::SetEnvironmentVariableW(L"MANIFOLD_UPDATE_FEED", feed.c_str());
            Settings settings;
            Engine engine(settings);
            UiFrames frames;
            FakeShell shell;
            Workspace workspace(engine, shell);
            int hidden = 0;
            const auto settle = [&]
                {
                    for (int i = 0; i < 300 && workspace.Updates().Checking(); ++i)
                    {
                        ::Sleep(10);
                        engine.Tick();
                        frames.Frames(workspace, 1);
                        hidden += !ShowsWindow("Update##workspace");
                    }
                    for (int i = 0; i < 5; ++i)
                    {
                        frames.Frames(workspace, 1);
                        hidden += !ShowsWindow("Update##workspace");
                    }
                    CHECK_MSG(!workspace.Updates().Checking() && workspace.Updates().Available(), "the check against the local feed did not finish with a newer version");
                };
            workspace.Updates().CheckNow();
            settle();
            CHECK_MSG(ShowsWindow("Update##workspace"), "the update dialog did not open");
            hidden = 0;
            workspace.Updates().CheckNow();
            settle();
            CHECK_MSG(hidden == 0, "after a second check the update dialog was not drawn in " + std::to_string(hidden) + " frames");
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            CHECK_MSG(g.OpenPopupStack.Size == 1, "popups open after a second check: " + std::to_string(g.OpenPopupStack.Size));
            ::SetEnvironmentVariableW(L"MANIFOLD_UPDATE_FEED", nullptr);
        }

        // The record editor opened on record 5 of one table, then another table loaded: Apply must not write into record 5 of
        // the new table.
        void RecordEditorAfterTableReplaced()
        {
            Settings settings;
            Engine engine(settings);
            UiFrames frames;
            FakeShell shell;
            Workspace workspace(engine, shell);
            CHECK(engine.LoadTable(WriteGroups("editor-a.CT", "a"), false).Ok());
            workspace.OpenRecordEditor(5);
            frames.Frames(workspace, 5);
            CHECK_MSG(ShowsWindow("###record5"), "no record editor");
            CHECK(engine.LoadTable(WriteGroups("editor-b.CT", "b"), false).Ok());
            frames.Frames(workspace, 2);
            frames.Activate(workspace, "###record5", "Apply");
            const Table::MemoryRecord* record = engine.Table().FindById(5);
            CHECK_MSG(record && record->Description.find("b 5") != std::string::npos,
                "record 5 of table b after Apply in the editor opened for table a: " + (record ? record->Description : std::string("missing")));
            workspace.OpenRecordEditor(5);
            frames.Frames(workspace, 3);
            CHECK_MSG(ShowsWindow("###record5"), "opening record 5 of table b showed no record editor");
        }

        int RunWorkspaceTests()
        {
            const int before = Totals().Failures;
            Smoke();
            DeleteConfirmAfterDrop();
            EditorAfterTableReplaced();
            AddToTableAfterNewTable();
            UpdateDialogShownTwice();
            RecordEditorAfterTableReplaced();
            return Totals().Failures - before;
        }
    }

    MANIFOLD_SUITE("workspace", "Workspace (the program's windows without a GPU)", 48, [](const SuiteContext&) { return RunWorkspaceTests(); });
}
