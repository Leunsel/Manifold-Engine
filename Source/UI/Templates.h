#pragma once

#include "App/Engine.h"
#include "Templates/TemplateGenerator.h"
#include "Templates/TemplateLibrary.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class Workspace;

    Templates::Library& TemplateLibrary(Engine& engine);
    std::filesystem::path TemplateFolder();

    class TemplateRunner
    {
    public:
        explicit TemplateRunner(Workspace& workspace);

        void MenuItems(std::uint64_t address, const std::string* script = nullptr);
        bool Choose(const std::string& id, std::uint64_t address);
        void Draw();
        std::optional<std::string> TakeText();

    private:
        void Start(const Templates::TemplateInfo& info);
        void Run(const Templates::TemplateInfo& info, std::map<std::string, Templates::Value> inputs);
        void InputsPopup();
        void PreviewPopup();
        void FailurePopup();

        Workspace& m_workspace;
        Engine& m_engine;
        std::string m_pending;
        std::string m_running;
        std::uint64_t m_address = 0;
        std::vector<std::string> m_inputTexts;
        std::deque<bool> m_inputFlags;
        std::vector<int> m_inputChoices;
        std::string m_inputError;
        bool m_openInputs = false;
        bool m_openPreview = false;
        bool m_openFailure = false;
        std::string m_preview;
        std::string m_previewCaption;
        std::string m_failure;
        std::optional<std::string> m_ready;
    };
}
