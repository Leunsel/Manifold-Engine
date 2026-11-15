#pragma once

#include "Core/Errors.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace Manifold
{
    class Engine;
}

namespace Manifold::Table
{
    struct MemoryRecord;
    struct StructDefinition;
}

namespace Manifold::Lua
{
    enum class OutputKind
    {
        Print,
        Error,
        Info
    };

    struct OutputLine
    {
        OutputKind Kind = OutputKind::Print;
        std::string Text;
    };

    struct Timer
    {
        int Id = 0;
        int Interval = 1000;
        bool Enabled = true;
        int Callback = 0;
        double NextAt = 0.0;
        bool Destroyed = false;
        bool OneShot = false;
    };

    struct ScriptGlobals
    {
        int SyntaxCheck = -2;
        int Record = -2;
    };

    inline constexpr std::size_t kCustomTypeByteSizeOffset = 0x54;
    inline constexpr int kCustomTypeMaxBytes = 1024;

    struct CustomTypeObject
    {
        std::array<std::uint8_t, 0x80> Raw{};
    };

    struct CustomType
    {
        std::string Name;
        int ToValue = 0;
        int ToBytes = 0;
        bool Float = false;
        bool String = false;
        std::shared_ptr<CustomTypeObject> Object = std::make_shared<CustomTypeObject>();

        int ByteCount() const;
        void SetByteCount(int count);
    };

    enum class StructureCallback
    {
        NameLookup,
        DissectOverride
    };

    struct StructureLookup
    {
        std::string Name;
        std::uint64_t Address = 0;
    };

    struct LuaCompletion
    {
        std::string Name;
        std::string Kind;
        std::string Signature;
        std::string Text;
    };

    struct LuaHotkey
    {
        int Id = 0;
        std::vector<int> Keys;
        int Callback = 0;
        bool WasDown = false;
    };

    enum class ScriptMenu
    {
        MemoryViewPopup,
        MemoryViewMenu
    };

    struct ScriptMenuItem
    {
        int Id = 0;
        std::string Caption;
        std::string Shortcut;
        bool Enabled = true;
        bool Checked = false;
        std::vector<ScriptMenuItem> Children;
    };

    struct ScriptControl
    {
        int Id = 0;
        std::string Kind;
        std::string Text;
        int Align = 0;
        int Left = 0;
        int Top = 0;
        int Width = 100;
        int Height = 100;
        bool Enabled = true;
        bool Checked = false;
        bool ReadOnly = false;
        int ItemIndex = -1;
        std::vector<std::string> Items;
        std::vector<ScriptControl> Children;
    };

    struct ScriptForm
    {
        int Id = 0;
        std::string Caption;
        int Left = 0;
        int Top = 0;
        int Width = 400;
        int Height = 300;
        bool Center = false;
        int Raise = 0;
        std::string Unsupported;
        std::vector<ScriptControl> Controls;
    };

    class LuaEngine
    {
    public:
        explicit LuaEngine(Engine& engine);
        ~LuaEngine();

        LuaEngine(const LuaEngine&) = delete;
        LuaEngine& operator=(const LuaEngine&) = delete;

        Status Run(std::string_view code, std::string_view chunkName = "script");
        Status RunWithResult(std::string_view code, std::string_view chunkName, std::string& result);
        Status RunFile(const std::filesystem::path& path);
        Status RunAutorun(const std::filesystem::path& folder);
        bool CallReference(int reference, int arguments, int results, std::string& error);
        bool CallStack(int arguments, int results, std::string& error);
        void Release(int reference);

        void SetAutoAssemblerCommand(const std::string& name, int reference);
        bool IsAutoAssemblerCommand(std::string_view name) const;
        Status RunAutoAssemblerCommand(std::string_view name, std::string_view parameters, bool syntaxCheck, std::string& output);
        void SetScriptGlobals(const Table::MemoryRecord* record, bool syntaxCheck);
        ScriptGlobals SaveScriptGlobals();
        void RestoreScriptGlobals(const ScriptGlobals& saved);

        bool BeforeActivation(Table::MemoryRecord& record, bool active);
        void AfterActivation(Table::MemoryRecord& record, bool active, bool succeeded);
        bool DisplayValue(Table::MemoryRecord& record, std::string& text);
        bool HasRecordEvent(int id, std::string_view name);
        void ValueChanged(Table::MemoryRecord& record, const std::string& before, const std::string& after);
        bool AddressListEvent(std::string_view name, Table::MemoryRecord* record);
        void RecordDestroyed(int id);
        void ForgetRecords();

        void RegisterCustomType(CustomType type);
        const CustomType* FindCustomType(std::string_view name) const;
        CustomType* FindCustomType(std::string_view name);
        std::vector<std::string> CustomTypeNames() const;
        bool CustomToText(const CustomType& type, std::span<const std::uint8_t> bytes, std::uint64_t address, std::string& text);
        bool CustomToNumber(const CustomType& type, std::span<const std::uint8_t> bytes, std::uint64_t address, double& value);
        bool CustomFromText(const CustomType& type, std::string_view text, std::uint64_t address, std::vector<std::uint8_t>& bytes);

        int AddStructureCallback(StructureCallback kind, int reference);
        bool RemoveStructureCallback(StructureCallback kind, int id);
        std::optional<StructureLookup> LookupStructureName(std::uint64_t address);
        bool DissectStructure(Table::StructDefinition& definition, std::uint64_t address);

        int AddSymbolCallback(int reference, bool first);
        bool RemoveSymbolCallback(int id);
        std::optional<std::uint64_t> LookupSymbol(std::string_view name, bool first);

        void Tick(double now);
        using TaskDone = std::function<void(const Status& status, const std::string& output)>;
        int StartTask(std::string_view code, std::string_view chunkName, TaskDone done, std::function<void()> enter = {}, std::function<void()> leave = {});
        void RunTasks(double budgetSeconds);
        bool TaskAlive(int id) const;
        void StopTask(int id);
        bool HasTasks() const { return !m_tasks.empty(); }
        bool InTask() const { return !m_taskStack.empty(); }
        bool IsCooperative(lua_State* thread) const;
        bool HasBreakpointCallback(std::uint64_t address) const;
        bool RunBreakpointCallback(std::uint64_t address);
        void SetBreakpointCallback(std::uint64_t address, int reference);
        void ClearBreakpointCallback(std::uint64_t address);
        void OnProcessOpened();

        void Print(OutputKind kind, std::string text);
        std::vector<OutputLine> Output() const;
        std::size_t OutputCount() const;
        std::uint64_t OutputDropped() const;
        void VisitOutput(std::size_t first, std::size_t last, const std::function<void(std::size_t, const OutputLine&)>& visit) const;
        std::uint64_t OutputGeneration() const { return m_outputGeneration.load(); }
        void ClearOutput();

        lua_State* State() const { return m_state; }
        Engine& Owner() { return m_engine; }

        int AddTimer(int interval, bool enabled);
        Timer* FindTimer(int id);
        int AddHotkey(std::vector<int> keys, int callback);
        void RemoveHotkey(int id);

        void TouchMenus() { ++m_menuGeneration; }
        std::uint64_t MenuGeneration() const { return m_menuGeneration; }
        std::vector<ScriptMenuItem> MenuItems(ScriptMenu menu);
        void MenuOpening(ScriptMenu menu);
        void ClickMenuItem(int id);
        void TouchForms() { m_formsShown = true; }
        bool FormsShown() const { return m_formsShown; }
        std::vector<ScriptForm> ShownForms();
        void SetFormValue(int control, std::string_view field, std::string_view value);
        void FireFormEvent(int control, std::string_view event);
        void CloseForm(int form);
        void SetFormBounds(int form, int left, int top, int width, int height);

        std::vector<std::string> FunctionNames() const;
        std::vector<LuaCompletion> Complete(std::string_view chain, std::string_view prefix, bool method);
        std::vector<std::string> CallSignatures(std::string_view chain, bool method);
        static std::vector<std::string> DocumentedSignatures(std::string_view name);
        static std::string DocumentedText(std::string_view name);

    private:
        struct Task
        {
            int Id = 0;
            lua_State* Thread = nullptr;
            int Reference = 0;
            std::string Name;
            TaskDone Done;
            std::function<void()> Enter;
            std::function<void()> Leave;
            double WakeAt = 0.0;
            double Deadline = 0.0;
            bool Stop = false;
            bool Running = false;
        };

        void Open();
        void Register();
        void RunCallback(const std::string& code, const char* name);
        void FinishTask(Task& task, int status, int results);
        static void TaskHook(lua_State* state, lua_Debug* debug);

        Engine& m_engine;
        lua_State* m_state = nullptr;
        mutable std::mutex m_outputLock;
        std::deque<OutputLine> m_output;
        std::size_t m_outputBytes = 0;
        std::uint64_t m_outputDropped = 0;
        std::atomic<std::uint64_t> m_outputGeneration{ 1 };
        std::map<int, Timer> m_timers;
        int m_nextTimer = 1;
        std::map<int, LuaHotkey> m_hotkeys;
        int m_nextHotkey = 1;
        std::map<std::uint64_t, int> m_breakpointCallbacks;
        std::map<std::string, int> m_autoAssemblerCommands;
        std::map<std::string, CustomType> m_customTypes;
        std::map<int, int> m_structureNameLookups;
        std::map<int, int> m_dissectOverrides;
        int m_nextStructureCallback = 1;
        struct SymbolCallback
        {
            int Reference = 0;
            bool First = false;
        };
        std::map<int, SymbolCallback> m_symbolLookups;
        int m_nextSymbolCallback = 1;
        bool m_inSymbolLookup = false;
        std::thread::id m_thread = std::this_thread::get_id();
        int m_onOpenProcess = 0;
        bool m_inTick = false;
        std::vector<std::unique_ptr<Task>> m_tasks;
        int m_nextTask = 1;
        std::vector<Task*> m_taskStack;
        std::uint64_t m_menuGeneration = 1;
        bool m_formsShown = false;
    };
}
