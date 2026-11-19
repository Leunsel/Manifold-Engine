#pragma once

#include "Core/RuntimeLog.h"
#include "UI/Theme.h"

#include "imgui.h"

#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::UI
{
    struct EmptyAction
    {
        std::string Label;
        const char* Shortcut = nullptr;
        bool Accent = false;
        bool NewRow = false;
        const char* Tooltip = nullptr;
    };

    struct SkeletonColumn
    {
        float Share = 1.0f;
        ImVec4 Color = ManifoldTheme::Text;
        float Alpha = 0.06f;
        bool Square = false;
    };

    int EmptyState(const char* icon, std::string_view title, std::string_view text, const std::vector<EmptyAction>& actions, bool card, std::string_view footer = {});
    void TableSkeleton(ImVec2 min, ImVec2 max, const std::vector<SkeletonColumn>& columns, unsigned int seed, ImVec4 background);
    unsigned int SkeletonNoise(unsigned int value);

    void BeginCard(const char* id, const char* title, ImVec2 size, ImGuiWindowFlags flags = 0);
    void EndCard();
    void CardTitle(const char* icon, const char* title, std::string_view note = {});

    bool IconButton(const char* icon, const char* tooltip, bool danger = false, float side = 0.0f);

    class GhostButtons
    {
    public:
        explicit GhostButtons(ImVec4 text = ManifoldTheme::Label);
        ~GhostButtons();

        GhostButtons(const GhostButtons&) = delete;
        GhostButtons& operator=(const GhostButtons&) = delete;
    };

    bool AccentButton(const char* label, ImVec2 size = ImVec2(0.0f, 0.0f));
    bool DangerButton(const char* label, ImVec2 size = ImVec2(0.0f, 0.0f));
    bool DangerMenuItem(const char* label, const char* shortcut = nullptr, bool enabled = true);

    void ItemTooltip(const char* text);
    void ShowTooltip(const std::function<void()>& content);
    bool ErrorTooltip(std::string_view message);
    void TextWithError(std::string_view text, ImVec4 color);
    void Muted(std::string_view text);
    void SectionLabel(const char* text);

    bool InputText(const char* label, std::string& value, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user = nullptr);
    bool InputTextWithHint(const char* label, const char* hint, std::string& value, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user = nullptr);
    bool InputMultiline(const char* label, std::string& value, ImVec2 size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user = nullptr);
    void SkipEditMenu();

    bool Splitter(const char* id, bool vertical, float thickness, float* first, float* second, float minFirst, float minSecond, float length = 0.0f);
    void Spinner(float size, ImVec4 color);
    void AsmOperands(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, const ImVec4& clip, std::string_view operands);
    bool ToggleSwitch(const char* id, bool* value);
    float ToggleSwitchWidth();
    bool Segmented(const char* id, int* value, const std::vector<const char*>& options, float width);
    bool TriStateCheckbox(const char* label, int& state);

    bool KeepOutOfCentralDock(const char* title);
    float CheckboxWidth(const char* label);
    float SmallButtonWidth(const char* label);
    bool FlowNext(float width, float indent = -1.0f);
    void TextFitted(std::string_view text, float maxWidth, const ImVec4& color);
    bool BeginToolWindow(const char* title, bool* open, ImVec2 size, ImGuiWindowFlags flags = 0, ImVec2 minimum = ImVec2(260.0f, 160.0f), bool escapeCloses = true);
    void EndToolWindow();
    void KeepEscape();
    void HoldEscape();
    bool EscapeFree();
    bool EscapeLeavesWindow();
    bool FocusRequested();
    void RequestWindowRaise(const char* title);
    void RaisePlatformWindow(bool requested);
    bool BeginModal(const char* id, const char* icon, const char* title, ImVec2 size, bool* open = nullptr, ImGuiID viewport = 0, ImGuiWindowFlags flags = 0);
    ImGuiID CallerViewport();
    void EndModal();

    enum class DialogChoice
    {
        None,
        Ok,
        Cancel
    };

    DialogChoice DialogButtons(const char* ok = "OK", bool okEnabled = true, bool enterConfirms = true, const char* cancel = "Cancel");

    class Dialog
    {
    public:
        Dialog(std::string id, std::string icon, std::string title, ImVec2 size = ImVec2(0.0f, 0.0f));

        void Open(ImGuiID viewport = 0);
        bool Begin();
        void End();
        bool Appearing() const { return m_appearing; }
        void SetTitle(std::string title) { m_title = std::move(title); }
        DialogChoice Buttons(const char* ok = "OK", bool okEnabled = true, bool enterConfirms = true, const char* cancel = "Cancel");
        void Close();

    private:
        std::string m_id;
        std::string m_icon;
        std::string m_title;
        ImVec2 m_size;
        bool m_pending = false;
        bool m_appearing = false;
        ImGuiID m_viewport = 0;
    };

    struct DataColumn
    {
        const char* Name = "";
        float Weight = 1.0f;
        float Fixed = 0.0f;
    };

    bool BeginDataTable(const char* id, std::initializer_list<DataColumn> columns, ImVec2 size = ImVec2(0.0f, 0.0f), ImGuiTableFlags extra = 0);

    template <class Row>
    void ClippedRows(int count, Row&& row)
    {
        ImGuiListClipper clipper;
        clipper.Begin(count);
        while (clipper.Step())
        {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                ImGui::PushID(index);
                row(index);
                ImGui::PopID();
            }
        }
    }

    bool TaskStatus(std::string_view text, bool cancellable = true);

    ImVec4 LevelColor(LogLevel level);
    const char* LevelIcon(LogLevel level);
    ImVec4 RecordColor(std::uint32_t bgr);

    void PushMono();
    void PopMono();
}
