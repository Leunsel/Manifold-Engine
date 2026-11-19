#pragma once

#include "Target/Unreal.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class UnrealWindow : public ToolWindowBase
    {
    public:
        explicit UnrealWindow(Workspace& workspace) : m_workspace(workspace) {}
        void Draw() override;
        void ShowType(const std::string& name);
        void ShowAddress(std::uint64_t address);

    private:
        enum class Tab
        {
            Properties,
            Functions,
            Instances,
            Entries
        };

        struct Hit
        {
            std::string Text;
            std::size_t Type = 0;
        };

        void Header(const Target::UnrealRuntime& runtime);
        void Whois(const Target::UnrealRuntime& runtime);
        void TypeList(const Target::UnrealRuntime& runtime, float height);
        void Details(const Target::UnrealRuntime& runtime);
        void Properties(const Target::UnrealRuntime& runtime, std::uint64_t type);
        void Functions(const Target::UnrealRuntime& runtime, std::uint64_t type);
        void Instances(const Target::UnrealRuntime& runtime, std::uint64_t type);
        void Entries(const Target::UnrealRuntime& runtime, std::uint64_t type);
        void FieldMenu(const Target::UnrealRuntime& runtime, const Target::UnrealField& field);
        void RefreshFilter(const Target::UnrealRuntime& runtime);
        void SearchProperties(const std::shared_ptr<const Target::UnrealRuntime>& runtime);
        void FindInstances(const std::shared_ptr<const Target::UnrealRuntime>& runtime, std::uint64_t type);
        void Dissect(std::uint64_t address, const char* what);
        std::string Dump(const Target::UnrealRuntime& runtime, std::uint64_t type) const;

        Workspace& m_workspace;
        std::shared_ptr<const Target::UnrealRuntime> m_runtime;
        std::string m_filter;
        std::string m_appliedFilter = "\x01";
        int m_kind = 0;
        int m_appliedKind = -1;
        std::vector<std::size_t> m_visible;
        std::vector<Hit> m_hits;
        bool m_searching = false;
        std::size_t m_selected = static_cast<std::size_t>(-1);
        std::string m_pendingType;
        Tab m_tab = Tab::Properties;
        bool m_ownOnly = false;
        bool m_inherited = true;
        std::uint64_t m_instancesOf = 0;
        std::vector<std::pair<std::uint64_t, std::string>> m_instances;
        bool m_findingInstances = false;
        std::string m_whoisText;
        std::string m_whoisResult;
        std::uint64_t m_whoisObject = 0;
        float m_listWidth = 300.0f;
    };
}
