#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

struct lua_State;

namespace Manifold::Templates
{
    using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

    struct InputDefinition
    {
        std::string Name;
        std::string Type = "string";
        std::string Caption;
        Value Default;
        std::vector<std::string> Values;
        std::optional<double> Min;
        std::optional<double> Max;
    };

    struct MemoryOverrides
    {
        std::optional<bool> AskForInjectionAddress;
        std::optional<bool> AskForHookName;
        std::optional<bool> AllocationNear;
        std::optional<std::string> AppendToHookName;
        std::optional<std::string> DefaultHookName;
        std::optional<std::string> AllocationSize;
    };

    struct TemplateInfo
    {
        std::string Id;
        std::string Name;
        std::string Caption;
        std::string Category;
        std::string Description;
        std::string Author;
        std::string Version;
        std::string Shortcut;
        int SchemaVersion = 1;
        bool InSubMenu = true;
        std::optional<double> CategoryOrder;
        std::optional<double> Order;
        std::vector<std::string> Tags;
        std::vector<std::string> Requires;
        std::vector<std::string> Optional;
        std::vector<std::string> Capabilities;
        std::vector<std::string> Architectures;
        bool AllowUnsafeGlobals = true;
        std::vector<InputDefinition> Inputs;
        MemoryOverrides Memory;
        bool BuiltIn = false;
        bool HasSettings = false;
        bool ExplicitCategory = false;
        bool SettingsHasComments = false;
        std::vector<std::pair<std::string, std::string>> CustomFields;
        std::filesystem::path ScriptPath;
        std::filesystem::path SettingsPath;
        std::string SourceName;
        std::string SettingsName;
        std::string BuiltinScript;
        std::string SettingsText;
    };

    struct LibraryProblem
    {
        std::string File;
        std::string Message;
    };

    struct SourceText
    {
        std::string Name;
        std::string Text;
    };

    std::optional<std::string> NormalizeAllocationSize(const Value& value);
    std::optional<TemplateInfo> ReadSettings(lua_State* state, std::string_view text, const std::string& chunkName, const std::string& templateName, std::string& error);
    bool PushSettingsTable(lua_State* state, std::string_view text, const std::string& chunkName, std::string& error);
    std::string CategoryLabel(std::string_view segment, std::optional<double>* order = nullptr);

    class Library
    {
    public:
        explicit Library(std::filesystem::path folder = {});

        void SetFolder(std::filesystem::path folder);
        const std::filesystem::path& Folder() const { return m_folder; }

        void Load(lua_State* state);
        bool ReloadIfChanged(lua_State* state);

        const std::vector<TemplateInfo>& Templates() const { return m_templates; }
        const std::vector<LibraryProblem>& Problems() const { return m_problems; }
        const TemplateInfo* Find(std::string_view id) const;

        std::optional<SourceText> Script(const TemplateInfo& info) const;
        std::optional<SourceText> Partial(std::string_view name, std::vector<std::string>* searched = nullptr) const;
        std::optional<SourceText> Header() const;

    private:
        std::uint64_t Fingerprint() const;

        std::filesystem::path m_folder;
        std::vector<TemplateInfo> m_templates;
        std::vector<LibraryProblem> m_problems;
        std::uint64_t m_fingerprint = 0;
        bool m_loaded = false;
    };
}
