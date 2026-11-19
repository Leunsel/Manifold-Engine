#pragma once

#include "Debug/Trace.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class BreakpointWindow : public ToolWindowBase
    {
    public:
        explicit BreakpointWindow(Workspace& workspace);
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; }
        void Edit(std::uint64_t address, Debug::BreakpointType type, int size, ImGuiID viewport = 0);

    private:
        void EditDialog();
        void Apply();

        Workspace& m_workspace;
        Engine& m_engine;
        std::uint64_t m_generation = 0;
        std::vector<Debug::Breakpoint> m_list;

        bool m_editOpen = false;
        bool m_editExisting = false;
        std::uint64_t m_editAddress = 0;
        int m_editType = 0;
        int m_editSize = 1;
        std::string m_editAddressText;
        std::string m_editCondition;
        std::string m_editChanges;
        std::string m_editThread;
        bool m_editBreak = true;
        std::string m_editError;
        ImGuiID m_selfViewport = 0;
        ImGuiID m_modalViewport = 0;
        void DrawWindow();
    };

    class ThreadListWindow : public ToolWindowBase
    {
    public:
        explicit ThreadListWindow(Workspace& workspace);
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; m_refreshAt = 0.0; }

    private:
        struct Row
        {
            std::uint32_t Id = 0;
            std::uint64_t Start = 0;
            std::uint64_t Teb = 0;
            int Priority = 0;
            bool Suspended = false;
            bool Hidden = false;
            std::uint64_t Rip = 0;
            bool HaveRip = false;
        };

        void Refresh();

        Workspace& m_workspace;
        Engine& m_engine;
        double m_refreshAt = 0.0;
        std::uint32_t m_process = 0;
        std::vector<Row> m_rows;
        std::uint32_t m_selected = 0;
    };

    class TraceWindow : public ToolWindowBase
    {
    public:
        TraceWindow(Workspace& workspace, int number);
        ~TraceWindow() override;
        void Draw() override;
        int Number() const { return m_number; }
        void Start(std::uint64_t address, ImGuiID viewport = 0);

    private:
        void StartDialog();
        void Begin();
        void Poll();
        void Stop();
        void RowText(const Debug::TraceStep& step, int index);

        Workspace& m_workspace;
        Engine& m_engine;
        int m_number = 1;
        std::string m_title;

        bool m_startOpen = false;
        std::uint64_t m_address = 0;
        std::string m_addressText;
        std::string m_countText = "2000";
        std::string m_condition;
        std::string m_stopCondition;
        bool m_stepOver = false;
        std::string m_error;

        bool m_running = false;
        std::uint64_t m_generation = 0;
        std::vector<Debug::TraceStep> m_steps;
        int m_selected = -1;
        std::map<std::uint64_t, std::string> m_text;
        ImGuiID m_selfViewport = 0;
        ImGuiID m_modalViewport = 0;
        void DrawWindow();
    };

    // The Mono / .NET tool: a scene explorer, an object search and the class
    // list on the left, inspector tabs on the right (Unity Explorer style).
}
