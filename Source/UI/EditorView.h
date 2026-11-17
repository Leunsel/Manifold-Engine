#pragma once

#include "UI/CodeEditor.h"

namespace Manifold::UI
{
    bool EditorView(const char* id, std::string& text, Language language, ImVec2 size, CodeEditorState& state);
}
