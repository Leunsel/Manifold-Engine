#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Paths.h"
#include "Layout/Layout.h"
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
            std::string left;
            for (const int id : { 1, 2, 5 })
                left += engine.Table().FindById(id) ? std::to_string(id) + " " : "";
            CHECK_MSG(left == "1 2 5 ", "records of the dropped table left after answering Delete: " + (left.empty() ? std::string("none") : left));
        }

        int RunWorkspaceTests()
        {
            const int before = Totals().Failures;
            Smoke();
            DeleteConfirmAfterDrop();
            return Totals().Failures - before;
        }
    }

    MANIFOLD_SUITE("workspace", "Workspace (the program's windows without a GPU)", 48, [](const SuiteContext&) { return RunWorkspaceTests(); });
}
