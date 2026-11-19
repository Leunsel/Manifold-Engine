#pragma once

#include "Disasm/AssemblySearch.h"
#include "Disasm/CodeDissect.h"
#include "UI/BackgroundTask.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    struct ModuleScope
    {
        int Kind = 0;
        std::string Module;
    };

    inline constexpr int kScopeAllMemory = -2;
    inline constexpr int kScopeExecutable = -1;

    void ModuleScopeCombo(const char* id, Engine& engine, ModuleScope& scope, float width, bool memory = false);
    std::vector<Target::ModuleInfo> ScopeModules(Engine& engine, const ModuleScope& scope);
    std::string ScopeName(Engine& engine, const ModuleScope& scope);

    class CodeDissectWindow : public ToolWindowBase
    {
    public:
        enum Tab
        {
            Strings,
            Functions,
            References
        };

        explicit CodeDissectWindow(Workspace& workspace);
        ~CodeDissectWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }
        void Show(int tab, std::uint64_t address);

    private:
        void Start();
        void Collect();
        void Toolbar();
        void StringsTab(const Disasm::DissectResult& result, float height);
        void FunctionsTab(const Disasm::DissectResult& result, float height);
        void ReferencesTab(const Disasm::DissectResult& result, float height);
        void ReferenceList(const Disasm::DissectResult& result);

        Workspace& m_workspace;
        int m_tab = Strings;
        int m_wantTab = -1;
        ModuleScope m_scope;
        std::string m_filter;
        std::string m_referenceText;
        std::uint64_t m_selected = 0;
        std::shared_ptr<const Disasm::DissectResult> m_sorted;
        std::vector<std::size_t> m_byCalls;
        std::vector<std::size_t> m_visible;
        std::string m_visibleFilter;
        std::shared_ptr<const Disasm::DissectResult> m_visibleFor;
        BackgroundTask<std::shared_ptr<Disasm::DissectResult>, Disasm::DissectProgress> m_task;
        std::uint64_t m_generation = 0;
    };

    class AsmSearchWindow : public ToolWindowBase
    {
    public:
        explicit AsmSearchWindow(Workspace& workspace);
        ~AsmSearchWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }

    private:
        void Start();
        void Collect();
        void Results();

        Workspace& m_workspace;
        bool m_focus = true;
        ModuleScope m_scope;
        std::string m_pattern;
        std::string m_searched;
        std::shared_ptr<Target::Process> m_process;
        BackgroundTask<Disasm::AsmSearchResult, Disasm::AsmSearchProgress> m_task;
        std::optional<Disasm::AsmSearchResult> m_result;
    };

    std::optional<Scan::Pattern> BytePattern(std::string_view text);
}
