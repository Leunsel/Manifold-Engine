#pragma once

#include "Core/BackgroundWork.h"

#include "imgui.h"

#include <string>
#include <string_view>

namespace Manifold
{
    class Engine;
}

namespace Manifold::UI
{
    void NoteAnimation();
    bool AnimationShown();

    void WorkBar(double fraction, ImVec2 size);
    std::string WorkDetail(const WorkInfo& info);
    bool LoadingCard(const char* icon, const WorkInfo& info, std::string_view note = {});
    bool LoadingLine(const WorkInfo& info);

    void ClaimWork(int id);
    float DrawWorkToasts(Engine& engine, float bottom);
}
