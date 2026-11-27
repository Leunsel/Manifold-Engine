#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Layout/Layout.h"
#include "UI/Workspace.h"

#include "imgui.h"
#include "imgui_internal.h"

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

        int RunWorkspaceTests()
        {
            const int before = Totals().Failures;
            Smoke();
            return Totals().Failures - before;
        }
    }

    MANIFOLD_SUITE("workspace", "Workspace (the program's windows without a GPU)", 48, [](const SuiteContext&) { return RunWorkspaceTests(); });
}
