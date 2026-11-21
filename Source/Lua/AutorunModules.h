#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold::Autorun
{
    enum class Shape
    {
        Loose,
        File,
        Folder
    };

    enum class Origin
    {
        User,
        Shipped
    };

    enum class State
    {
        Pending,
        Loaded,
        Disabled,
        Failed,
        Blocked,
        Held,
        Superseded,
        BuiltIn
    };

    struct Requirement
    {
        std::string Name;
        std::string MinVersion;
    };

    struct Header
    {
        std::string Name;
        std::string Version;
        std::string Author;
        std::string Description;
        std::vector<Requirement> Requires;
        int Api = 0;
        std::vector<std::pair<std::string, std::string>> Other;
        std::string Problem;
    };

    struct Module
    {
        std::string Name;
        std::string Key;
        std::string Category;
        Autorun::Shape Shape = Autorun::Shape::Loose;
        std::filesystem::path Entry;
        std::filesystem::path Root;
        std::string Where;
        Autorun::Header Header;
        std::string Hash;
        Autorun::Origin Origin = Autorun::Origin::User;
        bool Unmodified = false;
        Autorun::State State = Autorun::State::Pending;
        std::string Reason;
        std::string Traceback;
        bool Enabled = true;
        bool Changed = false;
        bool New = false;
        bool Untracked = false;
        bool ActiveUntilRestart = false;
        bool HeldNext = false;
        bool Duplicate = false;
        double LoadMs = 0.0;
        std::string FirstSeen;
    };

    struct Stored
    {
        std::string Name;
        bool Enabled = true;
        bool Allowed = true;
        std::string Hash;
        std::string FirstSeen;
    };

    class StateStore
    {
    public:
        bool Load(const std::filesystem::path& file);
        bool Save();
        bool Existed() const { return m_existed; }
        bool Persistent() const { return !m_file.empty(); }
        Stored* Find(std::string_view key);
        Stored& Put(std::string_view key, std::string_view name);
        void Touch() { m_dirty = true; }
        std::string Text() const;

    private:
        std::filesystem::path m_file;
        std::map<std::string, Stored> m_entries;
        bool m_existed = false;
        bool m_dirty = false;
    };

    struct ShippedModule
    {
        std::string_view Key;
        std::string_view Hash;
    };

    struct ApplyOptions
    {
        bool HoldNew = false;
        std::span<const ShippedModule> Shipped;
    };

    std::span<const ShippedModule> ShippedModules();

    std::string KeyOf(std::string_view name);
    Header ParseHeader(std::string_view text);
    int CompareVersions(std::string_view left, std::string_view right);
    std::string Sha256Hex(std::string_view bytes);
    std::string HashModule(const Module& module);
    const char* BuiltInReplacement(std::string_view head);

    std::vector<Module> Scan(const std::filesystem::path& folder);
    void Refresh(Module& module);
    void ApplyShipped(Module& module, std::span<const ShippedModule> shipped);
    void ApplyState(std::vector<Module>& modules, StateStore& store, const ApplyOptions& options);
    void ApplyState(Module& module, StateStore& store, const ApplyOptions& options);
    std::vector<std::size_t> Order(std::vector<Module>& modules, int apiVersion);
    Module* Find(std::vector<Module>& modules, std::string_view name);
    const Module* Find(const std::vector<Module>& modules, std::string_view name);
    bool Satisfied(const std::vector<Module>& modules, const Module& module, std::string& reason);
    const char* StateText(const Module& module);
    std::string Today();
}
