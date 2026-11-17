#include "UI/Loading.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Manifold::UI
{
    namespace
    {
        constexpr double kToastDelaySeconds = 0.35;
        constexpr double kToastFadeSeconds = 0.2;

        int g_animatedFrame = -10;
        std::unordered_map<int, int> g_claims;

        bool Claimed(int id)
        {
            const auto found = g_claims.find(id);
            return found != g_claims.end() && found->second >= ImGui::GetFrameCount() - 1;
        }

        void ForgetOldClaims()
        {
            const int frame = ImGui::GetFrameCount();
            for (auto it = g_claims.begin(); it != g_claims.end();)
                it = it->second < frame - 2 ? g_claims.erase(it) : std::next(it);
        }

        std::string Elapsed(double seconds)
        {
            if (seconds < 2.0)
                return {};
            if (seconds < 60.0)
                return Format("%d s", static_cast<int>(seconds));
            return Format("%d min %02d s", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
        }

        void Centered(ImDrawList* draw, float centerX, float y, ImU32 color, const std::string& text, float maxWidth)
        {
            const float width = ImGui::CalcTextSize(text.c_str()).x;
            const float left = std::round(centerX - std::min(width, maxWidth) * 0.5f);
            const ImVec4 clip(left, y, left + maxWidth, y + ImGui::GetTextLineHeight());
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(left, y), color, text.c_str(), nullptr, 0.0f, &clip);
        }

        void Ring(ImDrawList* draw, ImVec2 center, float radius, float thickness)
        {
            NoteAnimation();
            const double time = ImGui::GetTime();
            draw->AddCircle(center, radius, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.16f)), 48, thickness);
            const float start = static_cast<float>(std::fmod(time * 4.2, 6.283185307179586));
            const float sweep = 1.2f + 1.6f * static_cast<float>(0.5 + 0.5 * std::sin(time * 2.1));
            draw->PathArcTo(center, radius, start, start + sweep, 32);
            draw->PathStroke(ImGui::GetColorU32(ManifoldTheme::Accent), 0, thickness);
        }
    }

    void NoteAnimation()
    {
        g_animatedFrame = ImGui::GetFrameCount();
    }

    bool AnimationShown()
    {
        return ImGui::GetCurrentContext() && g_animatedFrame >= ImGui::GetFrameCount() - 1;
    }

    void WorkBar(double fraction, ImVec2 size)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        const ImVec2 max(min.x + size.x, min.y + size.y);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float rounding = size.y * 0.5f;
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.07f)), rounding);
        const ImU32 fill = ImGui::GetColorU32(ManifoldTheme::Accent);
        if (fraction >= 0.0)
        {
            const float width = std::round(size.x * static_cast<float>(std::clamp(fraction, 0.0, 1.0)));
            if (width >= 1.0f)
                draw->AddRectFilled(min, ImVec2(min.x + std::max(width, size.y), max.y), fill, rounding);
            return;
        }
        NoteAnimation();
        const double phase = std::fmod(ImGui::GetTime() / 1.4, 1.0);
        const float eased = static_cast<float>(phase < 0.5 ? 2.0 * phase * phase : 1.0 - std::pow(-2.0 * phase + 2.0, 2.0) * 0.5);
        const float segment = size.x * 0.32f;
        const float left = min.x - segment + (size.x + segment) * eased;
        const float clippedLeft = std::max(min.x, left);
        const float clippedRight = std::min(max.x, left + segment);
        if (clippedRight > clippedLeft)
            draw->AddRectFilled(ImVec2(clippedLeft, min.y), ImVec2(clippedRight, max.y), fill, rounding);
    }

    std::string WorkDetail(const WorkInfo& info)
    {
        std::string detail = info.Cancelling ? std::string("Cancelling") : info.Text.empty() ? std::string("Working") : info.Text;
        if (!info.Cancelling && info.Fraction >= 0.0)
            detail += Format("  ·  %d %%", static_cast<int>(std::floor(info.Fraction * 100.0)));
        if (const std::string elapsed = Elapsed(info.Seconds); !elapsed.empty())
            detail += "  ·  " + elapsed;
        return detail;
    }

    bool LoadingCard(const char* icon, const WorkInfo& info, std::string_view note)
    {
        ClaimWork(info.Id);
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float line = ImGui::GetTextLineHeight();
        const float button = ImGui::GetFrameHeight();
        const float width = std::round(std::clamp(available.x - Px(24.0f), Px(220.0f), Px(440.0f)));
        const float radius = std::round(Px(30.0f));
        const float padding = std::round(Px(26.0f));
        const float inner = width - padding * 2.0f;
        const float barHeight = std::max(2.0f, std::round(Px(4.0f)));
        const std::string detail = WorkDetail(info);
        const std::string noteText(note);

        float height = padding * 2.0f + radius * 2.0f + Px(14.0f) + line + Px(6.0f) + line + Px(14.0f) + barHeight;
        if (!noteText.empty())
            height += Px(10.0f) + line;
        if (info.Cancellable)
            height += Px(18.0f) + button;

        const ImVec2 min(std::round(origin.x + (available.x - width) * 0.5f), std::round(origin.y + std::max(0.0f, (available.y - height) * 0.45f)));
        const ImVec2 max(min.x + width, min.y + height);
        const float centerX = std::round(min.x + width * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float rounding = Px(8.0f);
        for (int layer = 3; layer >= 1; --layer)
        {
            const float spread = Px(3.0f) * static_cast<float>(layer);
            draw->AddRectFilled(ImVec2(min.x - spread, min.y - spread + Px(4.0f)), ImVec2(max.x + spread, max.y + spread + Px(4.0f)), IM_COL32(0, 0, 0, 16), rounding + spread);
        }
        const ImVec4 card(ManifoldTheme::Card.x + (ManifoldTheme::Text.x - ManifoldTheme::Card.x) * 0.025f, ManifoldTheme::Card.y + (ManifoldTheme::Text.y - ManifoldTheme::Card.y) * 0.025f,
            ManifoldTheme::Card.z + (ManifoldTheme::Text.z - ManifoldTheme::Card.z) * 0.025f, ManifoldTheme::Card.w);
        draw->AddRectFilled(min, max, ImGui::GetColorU32(card), rounding);
        draw->AddRect(min, max, ImGui::GetColorU32(ManifoldTheme::Border), rounding);

        float y = min.y + padding;
        const ImVec2 center(centerX, y + radius);
        draw->AddCircleFilled(center, radius, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.08f)), 48);
        Ring(draw, center, radius, std::max(1.5f, Px(2.5f)));
        if (icon && *icon)
        {
            ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.6f);
            const ImVec2 iconSize = ImGui::CalcTextSize(icon);
            draw->AddText(ImVec2(std::round(center.x - iconSize.x * 0.5f), std::round(center.y - iconSize.y * 0.5f)), ImGui::GetColorU32(ManifoldTheme::Accent), icon);
            ImGui::PopFont();
        }
        y += radius * 2.0f + Px(14.0f);

        ImGui::PushFont(Fonts::Bold(), 0.0f);
        Centered(draw, centerX, y, ImGui::GetColorU32(ManifoldTheme::Text), info.Title, inner);
        ImGui::PopFont();
        y += line + Px(6.0f);
        Centered(draw, centerX, y, ImGui::GetColorU32(ManifoldTheme::Muted), detail, inner);
        y += line + Px(14.0f);

        ImGui::SetCursorScreenPos(ImVec2(min.x + padding, y));
        WorkBar(info.Cancelling ? -1.0 : info.Fraction, ImVec2(inner, barHeight));
        y += barHeight;
        if (!noteText.empty())
        {
            y += Px(10.0f);
            Centered(draw, centerX, y, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.8f)), noteText, inner);
            y += line;
        }

        bool cancel = false;
        if (info.Cancellable)
        {
            y += Px(18.0f);
            const char* label = info.Cancelling ? "Cancelling..." : "Cancel";
            const float buttonWidth = std::round(ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f + Px(18.0f));
            ImGui::SetCursorScreenPos(ImVec2(std::round(centerX - buttonWidth * 0.5f), y));
            ImGui::BeginDisabled(info.Cancelling);
            ImGui::PushID(info.Id);
            cancel = ImGui::Button(label, ImVec2(buttonWidth, 0.0f));
            ImGui::PopID();
            ImGui::EndDisabled();
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(available.x, std::max(available.y, max.y - origin.y)));
        return cancel;
    }

    bool LoadingLine(const WorkInfo& info)
    {
        ClaimWork(info.Id);
        ImGui::PushID(info.Id);
        Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
        ImGui::SameLine(0.0f, Px(6.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ManifoldTheme::Label, "%s", info.Title.c_str());
        ImGui::SameLine(0.0f, Px(8.0f));
        Muted(WorkDetail(info));
        bool cancel = false;
        if (info.Cancellable)
        {
            ImGui::SameLine(0.0f, Px(8.0f));
            ImGui::BeginDisabled(info.Cancelling);
            cancel = ImGui::SmallButton(info.Cancelling ? "Cancelling..." : "Cancel");
            ImGui::EndDisabled();
        }
        ImGui::PopID();
        return cancel;
    }

    void ClaimWork(int id)
    {
        g_claims[id] = ImGui::GetFrameCount();
    }

    float DrawWorkToasts(Engine& engine, float bottom)
    {
        ForgetOldClaims();
        const std::vector<WorkInfo> work = engine.BusyWork();
        if (work.empty())
            return bottom;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = std::min(Px(380.0f), viewport->Size.x - Px(40.0f));
        float y = bottom;
        for (auto it = work.rbegin(); it != work.rend(); ++it)
        {
            const WorkInfo& info = *it;
            if (info.Seconds < kToastDelaySeconds || Claimed(info.Id))
                continue;
            const float alpha = static_cast<float>(std::clamp((info.Seconds - kToastDelaySeconds) / kToastFadeSeconds, 0.0, 1.0));
            const std::string id = "##work" + std::to_string(info.Id);
            ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - Px(20.0f), y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.97f * alpha);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(6.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(12.0f, 10.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ManifoldTheme::Card);
            ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.45f));
            ImGui::Begin(id.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoDocking);
            const float lineStart = ImGui::GetCursorPosX();
            const float lineWidth = ImGui::GetContentRegionAvail().x;
            const float spinner = ImGui::GetTextLineHeight();
            Spinner(spinner, ManifoldTheme::Accent);
            ImGui::SameLine(0.0f, Px(8.0f));
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::TextColored(ManifoldTheme::Text, "%s", info.Title.c_str());
            ImGui::PopFont();
            if (info.Cancellable)
            {
                const float side = ImGui::GetTextLineHeight() + Px(4.0f);
                ImGui::SameLine(lineStart + lineWidth - side);
                ImGui::BeginDisabled(info.Cancelling);
                ImGui::PushID(info.Id);
                if (IconButton(ICON_FA_TIMES, info.Cancelling ? "Cancelling" : "Cancel", false, side))
                    engine.CancelWork(info.Id);
                ImGui::PopID();
                ImGui::EndDisabled();
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + spinner + Px(8.0f));
            Muted(WorkDetail(info));
            ImGui::Spacing();
            WorkBar(info.Cancelling ? -1.0 : info.Fraction, ImVec2(ImGui::GetContentRegionAvail().x, std::max(2.0f, std::round(Px(3.0f)))));
            y -= ImGui::GetWindowHeight() + Px(8.0f);
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
        }
        return y;
    }
}
