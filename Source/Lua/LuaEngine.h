#pragma once

#include "Core/Errors.h"
#include "Lua/AutorunModules.h"
#include "Lua/LuaConsole.h"
#include "Lua/UiModel.h"

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
#include <unordered_map>
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
    class DocRegistry;
    struct DocRecord;

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
        std::function<std::optional<std::string>(std::span<const std::uint8_t> bytes)> NativeText;
        std::function<std::optional<std::vector<std::uint8_t>>(std::string_view text)> NativeBytes;
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
        std::shared_ptr<const DocRecord> Doc;
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
        MemoryViewMenu,
        MainMenu
    };

    struct ScriptMenuItem
    {
        int Id = 0;
        std::string Caption;
        std::string Merge;
        std::string Shortcut;
        bool Enabled = true;
        bool Checked = false;
        std::vector<ScriptMenuItem> Children;
    };

    struct ScriptNode
    {
        int Id = 0;
        std::string Text;
        bool Expanded = false;
        bool HasChildren = false;
        std::vector<ScriptNode> Children;
    };

    struct ScriptFont
    {
        float Points = 9.0f;
        int Color = -1;
        bool Bold = false;
        bool Mono = false;
    };

    enum class CanvasOp : std::uint8_t
    {
        Line = 1,
        Rect = 2,
        FillRect = 3,
        Ellipse = 4,
        RoundRect = 5,
        Pixel = 6,
        Text = 7,
        Draw = 8,
        Polygon = 9,
        Polyline = 10
    };

    struct CanvasCommand
    {
        CanvasOp Op = CanvasOp::Line;
        bool PenClear = false;
        bool BrushClear = false;
        bool Bold = false;
        bool Mono = false;
        float X1 = 0.0f;
        float Y1 = 0.0f;
        float X2 = 0.0f;
        float Y2 = 0.0f;
        float Rx = 0.0f;
        float Ry = 0.0f;
        float PenWidth = 1.0f;
        float Points = 9.0f;
        std::uint32_t Pen = 0;
        std::uint32_t Brush = 0xFFFFFF;
        std::int32_t Ref = 0;
        std::int32_t Count = 0;
    };

    struct ScriptCanvas
    {
        std::vector<CanvasCommand> Commands;
        std::vector<std::string> Texts;
        std::vector<float> Coordinates;
        std::uint64_t Generation = 0;
    };

    struct ScriptColumn
    {
        std::string Caption;
        int Width = 50;
        int Alignment = 0;
        bool Visible = true;
    };

    struct ScriptRow
    {
        int Id = 0;
        std::string Text;
        std::vector<std::string> Sub;
        bool Checked = false;
    };

    struct ScriptControl
    {
        int Id = 0;
        std::string Kind;
        std::string Text;
        int X = 0;
        int Y = 0;
        int W = 0;
        int H = 0;
        bool Enabled = true;
        int Color = -1;
        ScriptFont Font;
        int Cursor = 0;
        bool Click = false;
        bool DoubleClick = false;
        bool Hover = false;
        int Popup = 0;
        int Bevel = 0;
        int BevelInner = 0;
        int BevelWidth = 1;
        int BevelColor = -1;
        int Alignment = 0;
        int Layout = 0;
        bool WordWrap = false;
        bool Checked = false;
        bool ReadOnly = false;
        bool Password = false;
        bool Frame = true;
        std::string Hint;
        int ItemIndex = -1;
        std::vector<std::string> Items;
        int Resizes = 0;
        int MinSize = 30;
        int Align = 0;
        std::vector<ScriptNode> Nodes;
        int Selected = 0;
        bool RightClickSelect = false;
        int ViewStyle = 0;
        bool GridLines = false;
        bool CheckBoxes = false;
        bool Headers = true;
        std::vector<ScriptColumn> Columns;
        std::vector<ScriptRow> Rows;
        int Canvas = 0;
        bool Mouse = false;
        bool Wheel = false;
        std::string Unsupported;
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
        int ClientWidth = -1;
        int ClientHeight = -1;
        int MinWidth = 0;
        int MinHeight = 0;
        bool Center = false;
        int Raise = 0;
        int Menu = 0;
        int Color = -1;
        bool KeyDown = false;
        ScriptFont Font;
        std::string Unsupported;
        std::vector<ScriptControl> Controls;
    };

    using TextMeasurer = bool (*)(std::string_view text, float points, bool bold, bool mono, float& width, float& height);
    void SetTextMeasurer(TextMeasurer measurer);

    struct AutorunOptions
    {
        std::filesystem::path StateFile;
        bool HoldNew = false;
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
        Status Compile(std::string_view code, std::string_view chunkName = "check");
        Status RunFile(const std::filesystem::path& path);
        Status RunAutorun(const std::filesystem::path& folder, const AutorunOptions& options = {});
        const std::vector<Autorun::Module>& Modules() const { return m_modules; }
        std::uint64_t ModulesRevision() const { return m_modulesRevision; }
        const std::filesystem::path& AutorunFolder() const { return m_autorunFolder; }
        const AutorunOptions& AutorunSettings() const { return m_autorunOptions; }
        Status ReloadModule(std::string_view name);
        Status SetModuleEnabled(std::string_view name, bool enabled);
        Status AllowModule(std::string_view name);
        const Autorun::Module* ModuleOfSource(std::string_view source) const;
        std::filesystem::path ModuleDataFolder(const Autorun::Module& module) const;
        int ModulesNeedingAttention() const;
        void NoteScriptWrite(lua_State* state, std::string_view function, std::string_view path);
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
        std::vector<int> RecordsWithEvent(std::string_view name);
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
        void ProcessMessages();
        void BlockingSleep(std::uint32_t milliseconds);
        using TaskDone = std::function<void(const Status& status, const std::string& output)>;
        using TaskRead = std::function<void(lua_State* thread, int results)>;
        int StartTask(std::string_view code, std::string_view chunkName, TaskDone done, std::function<void()> enter = {}, std::function<void()> leave = {}, TaskRead read = {},
            std::uint64_t inputId = 0);
        int StartCall(int reference, const std::function<int(lua_State*)>& push, std::string_view name, TaskRead read, TaskDone done);
        void LookupStructureNameLater(std::uint64_t address, std::function<void(std::optional<StructureLookup>)> done);
        void DissectStructureLater(Table::StructDefinition* definition, std::uint64_t address, std::function<void(bool handled)> done);
        bool HasStructureNameLookups() const { return !m_structureNameLookups.empty(); }
        std::vector<int> StructureCallbackReferences(StructureCallback kind) const;
        bool HasDissectOverrides() const { return !m_dissectOverrides.empty(); }
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
        void OnProcessClosed();
        void DispatchEvent(const char* name);
        bool EngineScriptClaims(std::uint64_t address);

        std::uint64_t AddEntry(ConsoleEntry entry);
        void PrintError(const std::string& message, std::uint64_t inputId = 0);
        void PrintText(std::string text);
        ConsoleLog& Console() { return m_console; }
        const ConsoleLog& Console() const { return m_console; }
        std::shared_ptr<const ValueSnapshot> Snapshot(lua_State* state, int index);
        ConsoleEntry ErrorEntry(lua_State* state, int index, std::string traceback, std::uint64_t inputId);
        std::uint64_t Evaluate(const std::string& code, std::function<void(const Status&)> done = {}, int* task = nullptr);
        static bool Incomplete(lua_State* state, const std::string& code);
        bool ShowOnPrint() const;
        void SetShowOnPrint(bool show) { m_showOnPrintOverride = show ? 1 : 0; }
        void RememberShowOnPrint(bool show);
        std::uint64_t PrintSignal() const { return m_printSignal.load(); }
        void ClearOutput() { m_console.Clear(); }

        lua_State* State() const { return m_state; }
        Engine& Owner() { return m_engine; }

        int AddTimer(int interval, bool enabled);
        Timer* FindTimer(int id);
        const std::map<int, Timer>& Timers() const { return m_timers; }
        std::size_t TaskCount() const { return m_tasks.size(); }
        int AddHotkey(std::vector<int> keys, int callback);
        void RemoveHotkey(int id);

        void TouchMenus() { ++m_menuGeneration; }
        std::uint64_t MenuGeneration() const { return m_menuGeneration; }
        std::vector<ScriptMenuItem> MenuItems(ScriptMenu menu);
        std::vector<ScriptMenuItem> MenuItemsOf(int menu);
        void MenuOpening(ScriptMenu menu);
        void MenuOpeningOf(int menu);
        void ClickMenuItem(int id);
        void TouchForms()
        {
            ++m_formsGeneration;
            m_formsShown = true;
        }
        bool FormsShown() const { return m_formsShown; }
        std::uint64_t FormsGeneration() const { return m_formsGeneration; }
        std::vector<ScriptForm> ShownForms();
        void SetFormValue(int control, std::string_view field, std::string_view value);
        void FireFormEvent(int control, std::string_view event, std::initializer_list<int> arguments = {});
        ScriptCanvas& CanvasFor(int id);
        const ScriptCanvas* FindCanvas(int id) const;
        void ResetCanvas(int id);
        void FreeCanvas(int id);
        void CloseForm(int form);
        void SetFormBounds(int form, int left, int top, int width, int height, int clientWidth, int clientHeight);

        std::vector<std::string> FunctionNames() const;
        DocRegistry& Docs() { return *m_docs; }
        UiModel& Ui() { return m_ui; }
        std::shared_ptr<DocRegistry> SharedDocs() const { return m_docs; }
        void RememberChunk(std::string name, std::string text);
        std::string ChunkText(std::string_view name) const;
        std::shared_ptr<const DocRecord> Describe(std::string_view chain, bool method);
        std::vector<LuaCompletion> Complete(std::string_view chain, std::string_view prefix, bool method);
        std::string ResolveDocType(std::string_view type, std::string_view scope) const;
        std::vector<LuaCompletion> CompleteClass(std::string_view className, std::string_view prefix, bool method);
        std::shared_ptr<const DocRecord> ClassMember(std::string_view className, std::string_view member) const;
        std::vector<std::string> CallSignatures(std::string_view chain, bool method);
        std::vector<std::string> CallArgumentChoices(std::string_view chain, bool method, int argument);
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
            TaskRead Read;
            std::function<void()> Enter;
            std::function<void()> Leave;
            int Arguments = 0;
            double WakeAt = 0.0;
            double Deadline = 0.0;
            std::uint64_t InputId = 0;
            bool Stop = false;
            bool Running = false;
        };

        void Open();
        void Register();
        void RunCallback(const std::string& code, const char* name);
        std::vector<ScriptMenuItem> ReadMenu(bool byId, int id, const char* which);
        void OpenMenu(bool byId, int id, const char* which);
        void FinishTask(Task& task, int status, int results);
        static void TaskHook(lua_State* state, lua_Debug* debug);
        static void CallHook(lua_State* state, lua_Debug* debug);
        bool GuardedCall(int arguments, int results, int handler);

        Engine& m_engine;
        lua_State* m_state = nullptr;
        std::shared_ptr<DocRegistry> m_docs;
        std::unordered_map<std::string, std::string> m_chunks;
        std::deque<std::string> m_chunkOrder;
        ConsoleLog m_console;
        std::uint64_t m_inputCounter = 0;
        std::atomic<std::uint64_t> m_printSignal{ 0 };
        std::atomic<int> m_showOnPrintOverride{ -1 };
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
        int m_onOpenProcess = 0;
        bool m_inTick = false;
        UiModel m_ui;
        void TickUi(double now);
        void ChunkStarting(const std::string& name);
        bool RunModuleEntry(Autorun::Module& module);
        bool UnloadModule(Autorun::Module& module);
        void LoadPendingModules(bool announce, Status& first);
        void DropDependents();
        Status ModuleStatus(std::string_view name) const;
        int ManifoldApiVersion();
        void AddLibraryPath();
        std::vector<Autorun::Module> m_modules;
        std::vector<std::string> m_loadedModules;
        Autorun::StateStore m_moduleStore;
        std::filesystem::path m_autorunFolder;
        AutorunOptions m_autorunOptions;
        std::string m_loadingModule;
        std::uint64_t m_modulesRevision = 0;
        double m_callDeadline = 0.0;
        int m_callLimit = 0;
        std::vector<std::unique_ptr<Task>> m_tasks;
        int m_nextTask = 1;
        std::vector<Task*> m_taskStack;
        std::uint64_t m_menuGeneration = 1;
        std::uint64_t m_formsGeneration = 1;
        std::unordered_map<int, ScriptCanvas> m_canvases;
        bool m_formsShown = false;
    };
}
