#pragma once

#include "Core/Errors.h"
#include "Templates/TemplateLibrary.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold::Templates
{
    struct SettingsForm
    {
        int SchemaVersion = 2;
        std::string Id;
        std::string Caption;
        std::string Description;
        std::string Author;
        std::string Version;
        std::string Shortcut;
        std::string Category;
        bool ExplicitCategory = false;
        bool InSubMenu = true;
        std::optional<double> CategoryOrder;
        std::optional<double> Order;
        std::vector<std::string> Tags;
        std::vector<std::string> Requires;
        std::vector<std::string> Optional;
        std::vector<std::string> Capabilities;
        std::vector<std::string> Architectures;
        bool AllowUnsafeGlobals = false;
        std::vector<InputDefinition> Inputs;
        MemoryOverrides Memory;
        std::vector<std::pair<std::string, std::string>> CustomFields;
    };

    SettingsForm FormOf(const TemplateInfo& info);
    std::string WriteSettings(const SettingsForm& form);
    std::string FileStem(std::string_view caption);
    std::string UniqueId(const Library& library, std::string_view caption);
    std::filesystem::path CategoryFolder(const std::filesystem::path& root, std::string_view category);
    std::filesystem::path SettingsPathOf(const TemplateInfo& info);

    Status CreateTemplate(const std::filesystem::path& root, const SettingsForm& form, const std::string& script, std::filesystem::path* created = nullptr);
    Status SaveScript(const TemplateInfo& info, const std::string& script);
    Status SaveSettingsText(const TemplateInfo& info, const std::string& text);
    Status DuplicateTemplate(const Library& library, const TemplateInfo& info, const std::filesystem::path& root, bool keepId, std::string* newId = nullptr);
    Status MoveTemplate(const TemplateInfo& info, const std::filesystem::path& root, std::string_view category);
    Status DeleteTemplate(const TemplateInfo& info, bool recycle);
    Status ImportTemplates(const std::filesystem::path& source, const std::filesystem::path& root, int* imported = nullptr);
}
