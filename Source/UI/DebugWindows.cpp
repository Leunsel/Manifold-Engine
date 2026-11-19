#include "UI/DebugWindows.h"

#include "Core/Text.h"
#include "Debug/BreakAction.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <array>

#include <windows.h>
#include <tlhelp32.h>

namespace Manifold::UI
{
    namespace
    {
        using QueryThread = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

        QueryThread NtQueryThread()
        {
            static const QueryThread query = reinterpret_cast<QueryThread>(
                reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
            return query;
        }

        template <typename T>
        bool QueryThreadInfo(HANDLE thread, int info, T& out)
        {
            const QueryThread query = NtQueryThread();
            return query && query(thread, info, &out, sizeof(out), nullptr) >= 0;
        }

        Debug::SymbolResolver Resolver(Engine& engine)
        {
            return [&engine](std::string_view text) { return engine.Evaluate(std::string(text)); };
        }

        const char* kBreakTypes[] = { "Execute", "Write", "Access (read or write)" };
        const char* kBreakSizes[] = { "1 byte", "2 bytes", "4 bytes", "8 bytes" };

        int SizeIndex(int size) { return size >= 8 ? 3 : size >= 4 ? 2 : size >= 2 ? 1 : 0; }
        int IndexSize(int index) { return index == 3 ? 8 : index == 2 ? 4 : index == 1 ? 2 : 1; }

        void RegisterGrid(Engine& engine, const Disasm::RegisterValues& r, bool is64)
        {
            PushMono();
            const int digits = is64 ? 16 : 8;
            if (is64)
            {
                ImGui::Text("RAX %s  RBX %s  RCX %s  RDX %s", Hex(r.Rax, digits).c_str(), Hex(r.Rbx, digits).c_str(), Hex(r.Rcx, digits).c_str(), Hex(r.Rdx, digits).c_str());
                ImGui::Text("RSI %s  RDI %s  RBP %s  RSP %s", Hex(r.Rsi, digits).c_str(), Hex(r.Rdi, digits).c_str(), Hex(r.Rbp, digits).c_str(), Hex(r.Rsp, digits).c_str());
                ImGui::Text("R8  %s  R9  %s  R10 %s  R11 %s", Hex(r.R8, 16).c_str(), Hex(r.R9, 16).c_str(), Hex(r.R10, 16).c_str(), Hex(r.R11, 16).c_str());
                ImGui::Text("R12 %s  R13 %s  R14 %s  R15 %s", Hex(r.R12, 16).c_str(), Hex(r.R13, 16).c_str(), Hex(r.R14, 16).c_str(), Hex(r.R15, 16).c_str());
            }
            else
            {
                ImGui::Text("EAX %s  EBX %s  ECX %s  EDX %s", Hex(r.Rax, digits).c_str(), Hex(r.Rbx, digits).c_str(), Hex(r.Rcx, digits).c_str(), Hex(r.Rdx, digits).c_str());
                ImGui::Text("ESI %s  EDI %s  EBP %s  ESP %s", Hex(r.Rsi, digits).c_str(), Hex(r.Rdi, digits).c_str(), Hex(r.Rbp, digits).c_str(), Hex(r.Rsp, digits).c_str());
            }
            const std::string rip = engine.Describe(r.Rip);
            ImGui::Text("%cIP %s  %s", is64 ? 'R' : 'E', Hex(r.Rip, digits).c_str(), rip.c_str());
            PopMono();
        }
    }

    // --- Breakpoint list -------------------------------------------------

    BreakpointWindow::BreakpointWindow(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core()) {}

    void BreakpointWindow::Edit(std::uint64_t address, Debug::BreakpointType type, int size, ImGuiID viewport)
    {
        m_modalViewport = viewport ? viewport : m_selfViewport;
        m_editExisting = false;
        for (const Debug::Breakpoint& breakpoint : m_engine.Debugger().Breakpoints())
        {
            if (breakpoint.Address != address)
                continue;
            m_editExisting = true;
            type = breakpoint.Type;
            size = breakpoint.Size;
            m_editCondition = breakpoint.Action.Condition;
            m_editChanges = breakpoint.Action.Changes;
            m_editBreak = breakpoint.Action.Break;
            m_editThread = breakpoint.Action.ThreadId ? std::to_string(breakpoint.Action.ThreadId) : std::string();
            break;
        }
        if (!m_editExisting)
        {
            m_editCondition.clear();
            m_editChanges.clear();
            m_editThread.clear();
            m_editBreak = true;
        }
        m_editAddress = address;
        m_editAddressText = address ? m_engine.Symbols().ModuleOffset(address) : std::string();
        m_editType = static_cast<int>(type);
        m_editSize = SizeIndex(size);
        m_editError.clear();
        m_editOpen = true;
    }

    void BreakpointWindow::Apply()
    {
        Status status = m_engine.EnsureDebugger();
        std::uint64_t address = 0;
        if (status)
        {
            if (const auto parsed = m_engine.Evaluate(m_editAddressText, &status))
                address = *parsed;
        }
        Debug::BreakAction action;
        action.Condition = m_editCondition;
        action.Changes = m_editChanges;
        action.Break = m_editBreak;
        if (const auto thread = ParseDecimal(TrimView(m_editThread)); thread && *thread > 0)
            action.ThreadId = static_cast<std::uint32_t>(*thread);
        else if (!TrimView(m_editThread).empty())
            status = Status::Fail(ErrorCode::DebugBadExpression, "the thread id is not a number");
        if (status)
            status = Debug::CompileAction(action, m_engine.Process() && m_engine.Process()->Is64Bit(), Resolver(m_engine));
        const auto type = static_cast<Debug::BreakpointType>(m_editType);
        if (status && m_editExisting)
            m_engine.Debugger().RemoveBreakpoint(m_editAddress);
        if (status)
            status = m_engine.Debugger().SetBreakpoint(address, type, IndexSize(m_editSize), type != Debug::BreakpointType::Execute, action);
        if (!status)
        {
            m_editError = status.Text;
            return;
        }
        m_editOpen = false;
    }

    void BreakpointWindow::EditDialog()
    {
        if (m_editOpen && !ImGui::IsPopupOpen("Breakpoint##edit"))
            ImGui::OpenPopup("Breakpoint##edit");
        if (!BeginModal("Breakpoint##edit", ICON_FA_CIRCLE, m_editExisting ? "Edit breakpoint" : "New breakpoint", ImVec2(460.0f, 0.0f), nullptr, m_modalViewport))
            return;
        if (!m_editOpen)
        {
            ImGui::CloseCurrentPopup();
            EndModal();
            return;
        }
        ImGui::TextUnformatted("Address");
        ImGui::SetNextItemWidth(-1.0f);
        InputText("##bpaddress", m_editAddressText);
        ImGui::Spacing();
        ImGui::SetNextItemWidth(Px(200.0f));
        ImGui::Combo("Type", &m_editType, kBreakTypes, static_cast<int>(std::size(kBreakTypes)));
        if (m_editType != 0)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(120.0f));
            ImGui::Combo("Size", &m_editSize, kBreakSizes, static_cast<int>(std::size(kBreakSizes)));
        }
        ImGui::Spacing();
        Muted("Break only when this holds (optional), like  RCX == player  or  readInteger(RAX+8) > 100 :");
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##bpcond", "always", m_editCondition);
        Muted("Change registers each time, one per line, like  EAX = 0  or  RIP = RIP + 5 :");
        InputMultiline("##bpchanges", m_editChanges, ImVec2(-1.0f, Px(56.0f)));
        ImGui::Spacing();
        ImGui::SetNextItemWidth(Px(140.0f));
        InputTextWithHint("Only this thread", "any", m_editThread);
        ImGui::SameLine(0.0f, Px(18.0f));
        ImGui::Checkbox("Stop the process", &m_editBreak);
        ItemTooltip("Off: the condition and changes run and the process keeps going, so the register changes stick without stopping it.");
        if (!m_editError.empty())
        {
            ImGui::Spacing();
            TextWithError(m_editError, ManifoldTheme::Error);
        }
        const DialogChoice choice = DialogButtons(m_editExisting ? "Save" : "Set", true, false);
        if (choice == DialogChoice::Ok)
            Apply();
        else if (choice == DialogChoice::Cancel)
            m_editOpen = false;
        EndModal();
    }

    void BreakpointWindow::Draw()
    {
        if (!m_open)
            return;
        DrawWindow();
        // The editor is drawn outside the tool window, so it opens even when the
        // list is empty or the window is collapsed, and over the window that
        // asked for it (the memory viewer) rather than the main window.
        EditDialog();
    }

    void BreakpointWindow::DrawWindow()
    {
        if (!BeginWindow(ICON_FA_CIRCLE "  Breakpoints###breakpoints", ImVec2(640.0f, 360.0f), 0, ImVec2(460.0f, 240.0f)))
            return;
        m_selfViewport = ImGui::GetWindowViewport()->ID;
        Debug::Debugger& debugger = m_engine.Debugger();
        if (debugger.Generation() != m_generation)
        {
            m_generation = debugger.Generation();
            m_list = debugger.Breakpoints();
            std::sort(m_list.begin(), m_list.end(), [](const Debug::Breakpoint& a, const Debug::Breakpoint& b) { return a.Address < b.Address; });
        }

        const bool attached = debugger.Attached();
        ImGui::BeginDisabled(!m_engine.HasProcess());
        if (ImGui::Button(ICON_FA_PLUS "  Add"))
            Edit(0, Debug::BreakpointType::Execute, 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_list.empty());
        if (ImGui::Button(ICON_FA_TRASH "  Remove all"))
        {
            for (const Debug::Breakpoint& breakpoint : m_list)
                debugger.RemoveBreakpoint(breakpoint.Address);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Muted(attached ? std::to_string(m_list.size()) + (m_list.size() == 1 ? " breakpoint" : " breakpoints") : "the debugger is not attached");

        if (m_list.empty())
        {
            m_workspace.NoProcessState(ICON_FA_CIRCLE, "No breakpoints. Add one here, or from the memory viewer (F5 on an instruction, or right click for a condition).");
            EndToolWindow();
            return;
        }

        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##bplist", 4, flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
            ImGui::TableSetupColumn("Condition and changes", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, Px(64.0f));
            ImGui::TableHeadersRow();
            for (const Debug::Breakpoint& breakpoint : m_list)
            {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(breakpoint.Address));
                ImGui::TableNextColumn();
                PushMono();
                if (ImGui::Selectable(m_engine.Describe(breakpoint.Address).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        m_workspace.OpenMemoryViewer(breakpoint.Address);
                }
                PopMono();
                if (ImGui::BeginPopupContextItem("##bpmenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_EDIT "  Edit..."))
                        Edit(breakpoint.Address, breakpoint.Type, breakpoint.Size);
                    if (ImGui::MenuItem(ICON_FA_CODE "  Show in the disassembler"))
                        m_workspace.OpenMemoryViewer(breakpoint.Address);
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Remove"))
                        debugger.RemoveBreakpoint(breakpoint.Address);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(breakpoint.Type == Debug::BreakpointType::Write ? "Write" : breakpoint.Type == Debug::BreakpointType::Access ? "Access" : "Execute");
                ImGui::TableNextColumn();
                const std::string describe = Debug::DescribeAction(breakpoint.Action);
                if (describe.empty())
                    Muted("—");
                else
                    ImGui::TextUnformatted(describe.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Grouped(breakpoint.Hits).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        EndToolWindow();
    }

    // --- Thread list -----------------------------------------------------

    ThreadListWindow::ThreadListWindow(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core()) {}

    void ThreadListWindow::Refresh()
    {
        m_rows.clear();
        if (!m_engine.HasProcess())
            return;
        const Target::Process& process = *m_engine.Process();
        m_process = Raw(process.Id());
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (::Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != m_process)
                    continue;
                Row row;
                row.Id = entry.th32ThreadID;
                HANDLE thread = ::OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (thread)
                {
                    QueryThreadInfo(thread, 9, row.Start);
                    struct Basic { LONG ExitStatus; PVOID TebBaseAddress; PVOID Pid; PVOID Tid; ULONG_PTR Affinity; LONG Priority; LONG BasePriority; } basic{};
                    if (QueryThreadInfo(thread, 0, basic))
                        row.Teb = reinterpret_cast<std::uint64_t>(basic.TebBaseAddress);
                    const int priority = ::GetThreadPriority(thread);
                    if (priority != THREAD_PRIORITY_ERROR_RETURN)
                        row.Priority = priority;
                    BOOLEAN hidden = FALSE;
                    if (QueryThreadInfo(thread, 0x11, hidden))
                        row.Hidden = hidden != FALSE;
                    ::CloseHandle(thread);
                }
                m_rows.push_back(row);
            } while (::Thread32Next(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
        std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.Id < b.Id; });
    }

    void ThreadListWindow::Draw()
    {
        if (!m_open)
            return;
        if (!BeginWindow(ICON_FA_STREAM "  Threads###threads", ImVec2(620.0f, 380.0f), 0, ImVec2(440.0f, 240.0f)))
            return;
        const double now = ImGui::GetTime();
        if (m_engine.HasProcess() && Raw(m_engine.Process()->Id()) != m_process)
            m_refreshAt = 0.0;
        if (now >= m_refreshAt)
        {
            Refresh();
            m_refreshAt = now + 1.0;
        }
        if (ImGui::Button(ICON_FA_SYNC "  Refresh"))
            Refresh();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Muted(std::to_string(m_rows.size()) + (m_rows.size() == 1 ? " thread" : " threads"));

        if (!m_engine.HasProcess())
        {
            m_workspace.NoProcessState(ICON_FA_STREAM, "Open a process to list its threads.");
            EndToolWindow();
            return;
        }
        const bool is64 = m_engine.Process()->Is64Bit();
        const int digits = is64 ? 16 : 8;
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##threads", 4, flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, Px(80.0f));
            ImGui::TableSetupColumn("Start address", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("TEB", ImGuiTableColumnFlags_WidthFixed, Px(130.0f));
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
            ImGui::TableHeadersRow();
            for (const Row& row : m_rows)
            {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(row.Id));
                ImGui::TableNextColumn();
                PushMono();
                if (ImGui::Selectable((std::to_string(row.Id)).c_str(), m_selected == row.Id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selected = row.Id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && row.Start)
                        m_workspace.OpenMemoryViewer(row.Start);
                }
                PopMono();
                ImGui::TableNextColumn();
                ImGui::TextColored(ManifoldTheme::AddressText, "%s", row.Start ? m_engine.Describe(row.Start).c_str() : "?");
                if (row.Hidden)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ManifoldTheme::Warning, "%s", ICON_FA_EYE_SLASH "  hidden from debuggers");
                }
                ImGui::TableNextColumn();
                PushMono();
                ImGui::Text("%s", row.Teb ? Hex(row.Teb, digits).c_str() : "?");
                PopMono();
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.Priority);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        EndToolWindow();
    }

    // --- Break and trace -------------------------------------------------

    TraceWindow::TraceWindow(Workspace& workspace, int number) : m_workspace(workspace), m_engine(workspace.Core()), m_number(number)
    {
        m_title = std::string(ICON_FA_SHOE_PRINTS "  Trace") + (number > 1 ? " " + std::to_string(number) : "") + "###trace" + std::to_string(number);
    }

    TraceWindow::~TraceWindow()
    {
        if (m_running)
            m_engine.Tracer().Stop();
    }

    void TraceWindow::Start(std::uint64_t address, ImGuiID viewport)
    {
        m_modalViewport = viewport ? viewport : m_selfViewport;
        m_address = address;
        m_addressText = address ? m_engine.Symbols().ModuleOffset(address) : std::string();
        m_startOpen = true;
        m_error.clear();
    }

    void TraceWindow::Begin()
    {
        Status status = m_engine.EnsureDebugger();
        std::uint64_t address = m_address;
        if (status)
        {
            if (const auto parsed = m_engine.Evaluate(m_addressText, &status))
                address = *parsed;
        }
        const int count = static_cast<int>(ParseDecimal(TrimView(m_countText)).value_or(0));
        if (status && count <= 0)
            status = Status::Fail(ErrorCode::DebugTraceFailed, "the number of steps must be positive");
        Debug::TraceRequest request;
        request.Address = address;
        request.Count = count;
        request.StepOver = m_stepOver;
        const bool is64 = m_engine.Process() && m_engine.Process()->Is64Bit();
        if (status && !TrimView(m_condition).empty())
        {
            request.Trigger.Condition = m_condition;
            status = Debug::CompileAction(request.Trigger, is64, Resolver(m_engine));
        }
        if (status && !TrimView(m_stopCondition).empty())
        {
            request.StopText = m_stopCondition;
            status = Debug::CompileExpression(m_stopCondition, is64, Resolver(m_engine), request.StopCode);
        }
        if (status)
            status = m_engine.Tracer().Start(m_engine.Process(), m_engine.Disassembler(), request);
        if (!status)
        {
            m_error = status.Text;
            return;
        }
        m_address = address;
        m_running = true;
        m_steps.clear();
        m_text.clear();
        m_selected = -1;
        m_startOpen = false;
    }

    void TraceWindow::Stop()
    {
        if (m_running && m_engine.Tracer().TriggerAddress() == m_address)
            m_engine.Tracer().Stop();
        m_running = false;
    }

    void TraceWindow::Poll()
    {
        Debug::Tracer& tracer = m_engine.Tracer();
        const bool mine = tracer.TriggerAddress() == m_address;
        if (!mine)
            return;
        if (tracer.Generation() != m_generation)
        {
            m_generation = tracer.Generation();
            m_steps = tracer.Steps();
        }
        if (!tracer.Running())
            m_running = false;
    }

    void TraceWindow::StartDialog()
    {
        if (m_startOpen && !ImGui::IsPopupOpen("Break and trace##start"))
            ImGui::OpenPopup("Break and trace##start");
        if (!BeginModal("Break and trace##start", ICON_FA_SHOE_PRINTS, "Break and trace", ImVec2(460.0f, 0.0f), nullptr, m_modalViewport))
            return;
        if (!m_startOpen)
        {
            ImGui::CloseCurrentPopup();
            EndModal();
            return;
        }
        Muted("Set a breakpoint here and record each instruction the thread runs from it.");
        ImGui::Spacing();
        ImGui::TextUnformatted("From address");
        ImGui::SetNextItemWidth(-1.0f);
        InputText("##traceaddr", m_addressText);
        ImGui::SetNextItemWidth(Px(120.0f));
        InputText("Steps", m_countText, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(0.0f, Px(18.0f));
        ImGui::Checkbox("Step over calls", &m_stepOver);
        ImGui::Spacing();
        Muted("Start only when this holds (optional):");
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##tracecond", "always", m_condition);
        Muted("Stop early when this holds (optional):");
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##tracestop", "never", m_stopCondition);
        if (!m_error.empty())
        {
            ImGui::Spacing();
            TextWithError(m_error, ManifoldTheme::Error);
        }
        const DialogChoice choice = DialogButtons("Trace", true, false);
        if (choice == DialogChoice::Ok)
            Begin();
        else if (choice == DialogChoice::Cancel)
        {
            m_startOpen = false;
            if (m_steps.empty() && !m_running)
                m_open = false;  // nothing traced yet: do not leave an empty window behind
        }
        EndModal();
    }

    void TraceWindow::RowText(const Debug::TraceStep& step, int index)
    {
        ImGui::TableNextRow();
        ImGui::PushID(index);
        ImGui::TableNextColumn();
        PushMono();
        const std::string indent(static_cast<std::size_t>(std::clamp(step.Depth, 0, 12)) * 2, ' ');
        auto found = m_text.find(step.Address);
        if (found == m_text.end() && m_engine.HasProcess())
        {
            const Disasm::Instruction instruction = m_engine.Disassembler().Read(*m_engine.Process(), step.Address, &m_engine.Symbols());
            found = m_text.emplace(step.Address, instruction.Text()).first;
        }
        const std::string text = found == m_text.end() ? std::string("?") : found->second;
        if (ImGui::Selectable((indent + m_engine.Describe(step.Address)).c_str(), m_selected == index, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
        {
            m_selected = index;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                m_workspace.OpenMemoryViewer(step.Address);
        }
        ImGui::TableNextColumn();
        ImVec4 color = ManifoldTheme::CodeKeyword;
        if (step.Kind == Disasm::Flow::Call)
            color = ManifoldTheme::Accent;
        else if (step.Kind == Disasm::Flow::Return)
            color = ManifoldTheme::Warning;
        ImGui::TextColored(color, "%s", text.c_str());
        PopMono();
        ImGui::PopID();
    }

    void TraceWindow::Draw()
    {
        if (!m_open)
            return;
        Poll();
        // Before the first trace there is nothing to show, so only the setup
        // dialog appears (over the memory viewer); the window follows once it runs.
        if (m_running || !m_steps.empty() || !m_startOpen)
            DrawWindow();
        // Drawn outside the tool window so it appears over the memory viewer the
        // trace was started from, not centred on the main window.
        StartDialog();
    }

    void TraceWindow::DrawWindow()
    {
        if (!BeginWindow(m_title.c_str(), ImVec2(680.0f, 460.0f), 0, ImVec2(480.0f, 300.0f)))
            return;
        m_selfViewport = ImGui::GetWindowViewport()->ID;
        Debug::Tracer& tracer = m_engine.Tracer();
        const bool mine = tracer.TriggerAddress() == m_address;
        const bool running = m_running && mine && tracer.Running();
        if (running)
        {
            if (DangerButton(ICON_FA_STOP "  Stop", Px(90.0f, 0.0f)))
                Stop();
        }
        else if (ImGui::Button(ICON_FA_REDO "  Trace again", Px(110.0f, 0.0f)))
        {
            m_startOpen = true;
            m_modalViewport = m_selfViewport;
            m_error.clear();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        const std::string status = mine ? tracer.StatusText() : (m_steps.empty() ? "no steps" : "done");
        Muted(std::to_string(m_steps.size()) + " steps  ·  " + m_engine.Describe(m_address) + "  ·  " + status);

        const float detail = m_selected >= 0 && m_selected < static_cast<int>(m_steps.size()) ? Px(120.0f) : 0.0f;
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##trace", 2, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y - detail)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_steps.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    RowText(m_steps[static_cast<std::size_t>(i)], i);
            }
            if (running)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndTable();
        }
        if (detail > 0.0f)
        {
            BeginCard("##tracedetail", nullptr, ImVec2(0.0f, 0.0f));
            RegisterGrid(m_engine, m_steps[static_cast<std::size_t>(m_selected)].Registers, m_engine.Process() && m_engine.Process()->Is64Bit());
            EndCard();
        }
        EndToolWindow();
    }
}
