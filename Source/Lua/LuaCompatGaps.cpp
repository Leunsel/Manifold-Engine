#include "Lua/LuaCompatGaps.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaConsole.h"
#include "Lua/LuaEngine.h"

#include "lua.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kStubsKey = "Manifold.CompatStubs";
        constexpr const char* kWarnedKey = "Manifold.CompatWarned";

        constexpr GapReason kKernel{ "Kernel driver", "it needs Cheat Engine's kernel driver (DBK)" };
        constexpr GapReason kDbvm{ "Hypervisor", "it needs Cheat Engine's hypervisor (DBVM)" };
        constexpr GapReason kCompiler{ "Compilers", "Manifold Engine has no built-in C or C# compiler" };
        constexpr GapReason kDotNet{ ".NET", "Manifold Engine has no .NET data collector and does not inject .NET code; the Mono tool covers Mono games" };
        constexpr GapReason kServer{ "ceserver", "Manifold Engine does not connect to ceserver" };
        constexpr GapReason kDatabase{ "Databases", "Manifold Engine has no SQLite or ODBC classes" };
        constexpr GapReason kXml{ "XML documents", "the XML document classes are not built in" };
        constexpr GapReason kRemoteCall{ "Calls into the target", "calls with several arguments or a calling convention are not built in yet; executeCode takes one parameter" };
        constexpr GapReason kSection{ "Shared sections", "sections are not mapped into the target yet; allocateSharedMemoryLocal works inside Manifold Engine" };
        constexpr GapReason kSpeedhack{ "Speedhack", "Manifold Engine has no speedhack" };
        constexpr GapReason kPipe{ "Pipes", "Lua pipes and the Lua server are not built in" };
        constexpr GapReason kHook{ "Callbacks without a hook point", "Manifold Engine has no hook point for this callback yet, so it is never called" };
        constexpr GapReason kWndProc{ "Window hooks", "window procedure hooks are not built in" };
        constexpr GapReason kWindow{ "Missing windows", "this Cheat Engine window does not exist in Manifold Engine" };
        constexpr GapReason kScreen{ "Screen drawing", "drawing on the screen is not built in" };
        constexpr GapReason kFileProcess{ "Files as a process", "files cannot be opened as a process" };
        constexpr GapReason kPlugin{ "Plugins", "Cheat Engine plugins cannot be loaded" };
        constexpr GapReason kBytecode{ "Encoded functions", "Cheat Engine encodes Lua 5.3 bytecode, and Manifold Engine runs Lua 5.4" };
        constexpr GapReason kForms{ "Form files", "form files (.FRM) cannot be read yet" };
        constexpr GapReason kD3D{ "Direct3D hook", "the Direct3D overlay hook is not built in" };
        constexpr GapReason kHandles{ "Handles", "the handle list and handle duplication are not built in yet" };
        constexpr GapReason kGenerators{ "Script generators", "the Lua script generators are not built in yet; the Template menu of the Auto Assembler window writes the same scripts" };
        constexpr GapReason kProtection{ "Self-protection", "Manifold Engine does not hide or protect its own process" };
        constexpr GapReason kLuaState{ "Lua state", "Manifold Engine runs one Lua state for the whole session" };
        constexpr GapReason kDebugger{ "Debugger", "the debugger does not support this yet" };
        constexpr GapReason kFonts{ "Fonts", "script windows draw with Manifold Engine's fonts" };
        constexpr GapReason kSignature{ "Signatures", "Manifold Engine does not sign tables" };
        constexpr GapReason kPointerSize{ "Pointer size", "the pointer size follows the opened process" };
        constexpr GapReason kApiPointer{ "API pointers", "Manifold Engine does not redirect its Windows API calls" };
        constexpr GapReason kStructures{ "Structure windows", "structure windows and their selection cannot be scripted yet" };
        constexpr GapReason kMemscan{ "Main scan", "the main window's scan cannot be scripted yet; createMemScan gives a scan of your own" };
        constexpr GapReason kUniqueAob{ "Unique AOB", "the unique AOB search is not available from Lua yet" };
        constexpr GapReason kRipScanner{ "RIP-relative scanner", "the RIP-relative scanner is not built in yet" };
        constexpr GapReason kPdbStructures{ "PDB structures", "structures are not created from PDB types yet" };
        constexpr GapReason kSymbolLoader{ "Symbol loader", "the symbol loader cannot be stopped from Lua yet" };

        constexpr GapReason kSymbolsReady{ "Symbols", "symbols are ready when a process is opened, so there is nothing to wait for" };
        constexpr GapReason kLocalSymbols{ "Own symbols", "names inside Manifold Engine are looked up on every call" };
        constexpr GapReason kManagedSymbols{ "Mono symbols", "Mono names are looked up when they are used" };
        constexpr GapReason kKernelSymbols{ "Kernel symbols", "there are no kernel symbols" };
        constexpr GapReason kNoAds{ "Ad window", "there is no ad window" };
        constexpr GapReason kGarbage{ "Garbage collector", "Lua's own garbage collector runs as usual" };
        constexpr GapReason kHotkeyTiming{ "Hotkey timing", "hotkeys use Manifold Engine's own timing" };
        constexpr GapReason kTaskbar{ "Taskbar", "the taskbar button shows no progress" };
        constexpr GapReason kMainWindow{ "Main window", "the main window stays as it is" };
        constexpr GapReason kRefresh{ "Refresh", "the windows redraw every frame anyway" };
        constexpr GapReason kStepping{ "Step state", "the step state is not reported, so the answer is always false" };
        constexpr GapReason kSettingsForm{ "Settings form", "it returns a stand-in form; Manifold Engine's settings are not on it" };
        constexpr GapReason kTimers{ "Timers", "it returns a stand-in timer whose Interval shows the setting; changing it does not change Manifold Engine's settings" };
        constexpr GapReason kPlaceholder{ "Placeholder controls", "the control exists for scripts, but script windows draw it as a placeholder box" };
        constexpr GapReason kRegion{ "Regions", "the region keeps what is added to it, but it does not shape a window" };

        constexpr GapKind U = GapKind::Unavailable;
        constexpr GapKind E = GapKind::NoEffect;

        constexpr CompatGap kGaps[] = {
            { "waitforsymbols", E, GapResult::True, &kSymbolsReady },
            { "waitForExports", E, GapResult::True, &kSymbolsReady },
            { "waitForSections", E, GapResult::True, &kSymbolsReady },
            { "waitForDotNet", E, GapResult::True, &kSymbolsReady },
            { "reinitializeSelfSymbolhandler", E, GapResult::Nothing, &kLocalSymbols },
            { "reinitializeDotNetSymbolhandler", E, GapResult::Nothing, &kManagedSymbols },
            { "enableKernelSymbols", E, GapResult::Nothing, &kKernelSymbols },
            { "fuckCheatEngine", E, GapResult::Nothing, &kNoAds },
            { "supportCheatEngine", E, GapResult::Nothing, &kNoAds },
            { "gc_setActive", E, GapResult::Nothing, &kGarbage },
            { "gc_setPassive", E, GapResult::Nothing, &kGarbage },
            { "setGlobalKeyPollInterval", E, GapResult::Nothing, &kHotkeyTiming },
            { "setGlobalDelayBetweenHotkeyActivation", E, GapResult::Nothing, &kHotkeyTiming },
            { "setProgressState", E, GapResult::Nothing, &kTaskbar },
            { "setProgressValue", E, GapResult::Nothing, &kTaskbar },
            { "hideAllCEWindows", E, GapResult::Nothing, &kMainWindow },
            { "unhideMainCEwindow", E, GapResult::Nothing, &kMainWindow },
            { "debug_updateGUI", E, GapResult::Nothing, &kRefresh },
            { "debug_isStepping", E, GapResult::False, &kStepping },
            { "getSettingsForm", E, GapResult::Script, &kSettingsForm },
            { "getFreezeTimer", E, GapResult::Script, &kTimers },
            { "getUpdateTimer", E, GapResult::Script, &kTimers },
            { "createCalendar", E, GapResult::Script, &kPlaceholder },
            { "createColorBox", E, GapResult::Script, &kPlaceholder },
            { "createDiagram", E, GapResult::Script, &kPlaceholder },
            { "createVirtualStringTree", E, GapResult::Script, &kPlaceholder },
            { "createRegion", E, GapResult::Script, &kRegion },

            { "allocateKernelMemory", U, GapResult::Nil, &kKernel },
            { "freeKernelMemory", U, GapResult::Nothing, &kKernel },
            { "mapMemory", U, GapResult::Nil, &kKernel },
            { "unmapMemory", U, GapResult::Nothing, &kKernel },
            { "readProcessMemoryCR3", U, GapResult::Nil, &kKernel },
            { "writeProcessMemoryCR3", U, GapResult::False, &kKernel },
            { "getPhysicalAddressCR3", U, GapResult::Nil, &kKernel },
            { "dbk_executeKernelMemory", U, GapResult::Nil, &kKernel },
            { "dbk_getCR0", U, GapResult::Nil, &kKernel },
            { "dbk_getCR3", U, GapResult::Nil, &kKernel },
            { "dbk_getCR4", U, GapResult::Nil, &kKernel },
            { "dbk_getPEProcess", U, GapResult::Nil, &kKernel },
            { "dbk_getPEThread", U, GapResult::Nil, &kKernel },
            { "dbk_getPhysicalAddress", U, GapResult::Nil, &kKernel },
            { "dbk_initialize", U, GapResult::False, &kKernel },
            { "dbk_readMSR", U, GapResult::Nil, &kKernel },
            { "dbk_readPhysicalMemory", U, GapResult::Nil, &kKernel },
            { "dbk_setSaferPhysicalMemoryScanning", U, GapResult::Nothing, &kKernel },
            { "dbk_useKernelmodeOpenProcess", U, GapResult::Nothing, &kKernel },
            { "dbk_useKernelmodeProcessMemoryAccess", U, GapResult::Nothing, &kKernel },
            { "dbk_useKernelmodeQueryMemoryRegions", U, GapResult::Nothing, &kKernel },
            { "dbk_usePhysicalMemoryAccess", U, GapResult::Nothing, &kKernel },
            { "dbk_writeMSR", U, GapResult::Nothing, &kKernel },
            { "dbk_writePhysicalMemory", U, GapResult::False, &kKernel },
            { "dbk_writesIgnoreWriteProtection", U, GapResult::Nothing, &kKernel },
            { "debug_getLastBranchRecord", U, GapResult::Nil, &kKernel },
            { "debug_setLastBranchRecording", U, GapResult::Nothing, &kKernel },

            { "dbvm_addMemory", U, GapResult::Nil, &kDbvm },
            { "dbvm_bp_getBrokenThreadEventFull", U, GapResult::Nil, &kDbvm },
            { "dbvm_bp_getBrokenThreadEventShort", U, GapResult::Nil, &kDbvm },
            { "dbvm_bp_getBrokenThreadListSize", U, GapResult::Zero, &kDbvm },
            { "dbvm_bp_getProcessAndThreadIDFromEvent", U, GapResult::Nil, &kDbvm },
            { "dbvm_bp_resumeBrokenThread", U, GapResult::Nothing, &kDbvm },
            { "dbvm_bp_setBrokenThreadEventFull", U, GapResult::Nothing, &kDbvm },
            { "dbvm_changeregonbp", U, GapResult::False, &kDbvm },
            { "dbvm_cloak_activate", U, GapResult::Nil, &kDbvm },
            { "dbvm_cloak_deactivate", U, GapResult::Nil, &kDbvm },
            { "dbvm_cloak_readOriginal", U, GapResult::Nil, &kDbvm },
            { "dbvm_cloak_writeOriginal", U, GapResult::Nil, &kDbvm },
            { "dbvm_getCR4", U, GapResult::Nil, &kDbvm },
            { "dbvm_getMemory", U, GapResult::Nil, &kDbvm },
            { "dbvm_initialize", U, GapResult::False, &kDbvm },
            { "dbvm_log_cr3_start", U, GapResult::Nothing, &kDbvm },
            { "dbvm_log_cr3_stop", U, GapResult::Nil, &kDbvm },
            { "dbvm_readMSR", U, GapResult::Nil, &kDbvm },
            { "dbvm_readPhysicalMemory", U, GapResult::Nil, &kDbvm },
            { "dbvm_removechangeregonbp", U, GapResult::Nothing, &kDbvm },
            { "dbvm_setKeys", U, GapResult::Nothing, &kDbvm },
            { "dbvm_setTSCAdjust", U, GapResult::Nothing, &kDbvm },
            { "dbvm_speedhack_setSpeed", U, GapResult::Nothing, &kDbvm },
            { "dbvm_traceonbp", U, GapResult::False, &kDbvm },
            { "dbvm_traceonbp_getstatus", U, GapResult::Zero, &kDbvm },
            { "dbvm_traceonbp_remove", U, GapResult::Nothing, &kDbvm },
            { "dbvm_traceonbp_retrievelog", U, GapResult::Nil, &kDbvm },
            { "dbvm_traceonbp_stoptrace", U, GapResult::Nothing, &kDbvm },
            { "dbvm_watch_disable", U, GapResult::Nothing, &kDbvm },
            { "dbvm_watch_executes", U, GapResult::Nil, &kDbvm },
            { "dbvm_watch_reads", U, GapResult::Nil, &kDbvm },
            { "dbvm_watch_retrievelog", U, GapResult::Nil, &kDbvm },
            { "dbvm_watch_writes", U, GapResult::Nil, &kDbvm },
            { "dbvm_writeMSR", U, GapResult::Nothing, &kDbvm },
            { "dbvm_writePhysicalMemory", U, GapResult::False, &kDbvm },

            { "compile", U, GapResult::Nil, &kCompiler },
            { "compileFiles", U, GapResult::Nil, &kCompiler },
            { "compileCS", U, GapResult::Nil, &kCompiler },
            { "compileTCCLib", U, GapResult::False, &kCompiler },
            { "addCIncludePath", U, GapResult::Nothing, &kCompiler },
            { "removeCIncludePath", U, GapResult::Nothing, &kCompiler, true },

            { "dotNetExecuteClassMethod", U, GapResult::Nil, &kDotNet },
            { "injectDotNetDLL", U, GapResult::Nil, &kDotNet },
            { "getDotNetDataCollector", U, GapResult::Nil, &kDotNet },

            { "connectToCEServer", U, GapResult::False, &kServer },
            { "getCEServerPath", U, GapResult::Nil, &kServer },

            { "createSQLite3Connection", U, GapResult::Nil, &kDatabase },
            { "createODBCConnection", U, GapResult::Nil, &kDatabase },
            { "createSQLQuery", U, GapResult::Nil, &kDatabase },
            { "createSQLTransaction", U, GapResult::Nil, &kDatabase },
            { "setSQLiteLibraryName", U, GapResult::Nothing, &kDatabase },

            { "createXMLDocument", U, GapResult::Nil, &kXml },
            { "createXMLDocumentFromFile", U, GapResult::Nil, &kXml },
            { "createXMLDocumentFromStream", U, GapResult::Nil, &kXml },

            { "executeCodeEx", U, GapResult::Nil, &kRemoteCall },
            { "executeMethod", U, GapResult::Nil, &kRemoteCall },
            { "createExecuteCodeExStub", U, GapResult::Nil, &kRemoteCall },
            { "createExecuteMethodStub", U, GapResult::Nil, &kRemoteCall },
            { "createRemoteExecutor", U, GapResult::Nil, &kRemoteCall },

            { "allocateSharedMemory", U, GapResult::Nil, &kSection },
            { "createSection", U, GapResult::Nil, &kSection },
            { "mapViewOfSection", U, GapResult::Nil, &kSection },
            { "unMapViewOfSection", U, GapResult::False, &kSection },

            { "speedhack_setSpeed", U, GapResult::False, &kSpeedhack },

            { "createPipe", U, GapResult::Nil, &kPipe },
            { "connectToPipe", U, GapResult::Nil, &kPipe },
            { "openLuaServer", U, GapResult::Nothing, &kPipe },

            { "registerAddressLookupCallback", U, GapResult::Id, &kHook },
            { "registerAssembler", U, GapResult::Id, &kHook },
            { "registerAutoAssemblerPrologue", U, GapResult::Id, &kHook },
            { "registerBinUtil", U, GapResult::Nothing, &kHook },
            { "registerEXETrainerFeature", U, GapResult::Id, &kHook },
            { "registerGlobalDisassembleOverride", U, GapResult::Id, &kHook },
            { "registerGlobalStructureListUpdateNotification", U, GapResult::Id, &kHook },
            { "registerStructureAndElementListCallback", U, GapResult::Id, &kHook },
            { "onAPIPointerChange", U, GapResult::Nothing, &kHook },
            { "onAutoGuess", U, GapResult::Nothing, &kHook },
            { "unregisterAddressLookupCallback", U, GapResult::Nothing, &kHook, true },
            { "unregisterAssembler", U, GapResult::Nothing, &kHook, true },
            { "unregisterAutoAssemblerPrologue", U, GapResult::Nothing, &kHook, true },
            { "unregisterEXETrainerFeature", U, GapResult::Nothing, &kHook, true },
            { "unregisterGlobalDisassembleOverride", U, GapResult::Nothing, &kHook, true },
            { "unregisterGlobalStructureListUpdateNotification", U, GapResult::Nothing, &kHook, true },
            { "unregisterStructureAndElementListCallback", U, GapResult::Nothing, &kHook, true },
            { "hookWndProc", U, GapResult::False, &kWndProc },
            { "unhookWndProc", U, GapResult::Nothing, &kWndProc, true },

            { "getUltimap2", U, GapResult::Nil, &kWindow },
            { "getCodeFilter", U, GapResult::Nil, &kWindow },
            { "getDissectCode", U, GapResult::Nil, &kWindow },
            { "getHotkeyHandlerThread", U, GapResult::Nil, &kWindow },
            { "getScreenCanvas", U, GapResult::Nil, &kScreen },

            { "openFileAsProcess", U, GapResult::False, &kFileProcess },
            { "saveOpenedFile", U, GapResult::False, &kFileProcess },
            { "getOpenedFileSize", U, GapResult::Nil, &kFileProcess },

            { "loadPlugin", U, GapResult::Nil, &kPlugin },

            { "encodeFunction", U, GapResult::Nil, &kBytecode },
            { "encodeFunctionEx", U, GapResult::Nil, &kBytecode },
            { "decodeFunction", U, GapResult::Nil, &kBytecode },

            { "createFormFromFile", U, GapResult::Nil, &kForms },
            { "createFormFromStream", U, GapResult::Nil, &kForms },

            { "createD3DHook", U, GapResult::Nil, &kD3D },

            { "getHandleList", U, GapResult::EmptyTable, &kHandles },
            { "closeRemoteHandle", U, GapResult::False, &kHandles },
            { "duplicateHandle", U, GapResult::Nil, &kHandles },

            { "generateAOBInjectionScript", U, GapResult::Nil, &kGenerators },
            { "generateAPIHookScript", U, GapResult::Nil, &kGenerators },
            { "generateCodeInjectionScript", U, GapResult::Nil, &kGenerators },
            { "generateFullInjectionScript", U, GapResult::Nil, &kGenerators },
            { "addSnapshotAsComment", U, GapResult::Nothing, &kGenerators },

            { "activateProtection", U, GapResult::Nothing, &kProtection },
            { "enableDRM", U, GapResult::False, &kProtection },

            { "resetLuaState", U, GapResult::Nothing, &kLuaState },

            { "debug_addThreadToNoBreakList", U, GapResult::Nothing, &kDebugger },
            { "debug_removeThreadFromNoBreakList", U, GapResult::Nothing, &kDebugger, true },
            { "debug_setBreakpointForThread", U, GapResult::False, &kDebugger },
            { "debug_getXMMPointer", U, GapResult::Nil, &kDebugger },

            { "loadFontFromStream", U, GapResult::Nil, &kFonts },
            { "unloadLoadedFont", U, GapResult::Nothing, &kFonts, true },

            { "signTable", U, GapResult::False, &kSignature },
            { "setPointerSize", U, GapResult::Nothing, &kPointerSize },
            { "setAPIPointer", U, GapResult::Nothing, &kApiPointer },

            { "enumStructureForms", U, GapResult::EmptyTable, &kStructures },
            { "getSelectedStructElement", U, GapResult::Nil, &kStructures },
            { "createStructureFromName", U, GapResult::Nil, &kPdbStructures },
            { "getCurrentMemscan", U, GapResult::Nil, &kMemscan },
            { "getUniqueAOB", U, GapResult::Nil, &kUniqueAob },
            { "createRipRelativeScanner", U, GapResult::Nil, &kRipScanner },
            { "releaseDebugFiles", U, GapResult::Nothing, &kSymbolLoader },
        };

        constexpr const char* kMethodNames[] = {
            "addColumn", "addColumns", "addGroup", "focus", "structChange", "OnStatusbarUpdate",
            "split", "startsWith", "endsWith",
            "enumDomains", "enumModuleList", "enumTypeDefs", "getTypeDefMethods", "getTypeDefParent", "getTypeDefData",
            "getMethodParameters", "getAddressData", "enumAllObjects", "enumAllObjectsOfType",
            "HotkeyHandlerThread", "ModuleLoader",
        };

        const std::unordered_map<std::string_view, const CompatGap*>& Index()
        {
            static const std::unordered_map<std::string_view, const CompatGap*> index = []
                {
                    std::unordered_map<std::string_view, const CompatGap*> map;
                    for (const CompatGap& gap : kGaps)
                        map.emplace(gap.Name, &gap);
                    return map;
                }();
            return index;
        }

        const char* ResultText(GapResult result)
        {
            switch (result)
            {
            case GapResult::False: return "false";
            case GapResult::True: return "true";
            case GapResult::Zero: return "0";
            case GapResult::EmptyTable: return "an empty table";
            case GapResult::Id: return "an id";
            case GapResult::Nothing:
            case GapResult::Nil:
            case GapResult::Script:
                break;
            }
            return "nil";
        }

        std::string Sentence(const char* text)
        {
            std::string sentence = text;
            if (!sentence.empty())
                sentence[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(sentence[0])));
            return sentence;
        }

        int PushResult(lua_State* state, GapResult result)
        {
            static lua_Integer nextId = 0;
            switch (result)
            {
            case GapResult::Nothing:
            case GapResult::Script:
                return 0;
            case GapResult::Nil: lua_pushnil(state); return 1;
            case GapResult::False: lua_pushboolean(state, 0); return 1;
            case GapResult::True: lua_pushboolean(state, 1); return 1;
            case GapResult::Zero: lua_pushinteger(state, 0); return 1;
            case GapResult::EmptyTable: lua_newtable(state); return 1;
            case GapResult::Id: lua_pushinteger(state, ++nextId); return 1;
            }
            return 0;
        }

        void WarnOnce(lua_State* state, const CompatGap& gap, const char* calledAs)
        {
            lua_getfield(state, LUA_REGISTRYINDEX, kWarnedKey);
            if (!lua_istable(state, -1))
            {
                lua_pop(state, 1);
                return;
            }
            const bool warned = lua_getfield(state, -1, gap.Name) != LUA_TNIL;
            lua_pop(state, 1);
            if (warned)
            {
                lua_pop(state, 1);
                return;
            }
            lua_pushboolean(state, 1);
            lua_setfield(state, -2, gap.Name);
            lua_pop(state, 1);

            ConsoleEntry entry;
            entry.Kind = EntryKind::Log;
            entry.Level = LogLevel::Warning;
            entry.Channel = "Cheat Engine API";
            entry.Text = std::string(calledAs) + " is not available in Manifold Engine: " + gap.Reason->Text + ". The call does nothing and returns " + ResultText(gap.Result) + ".";
            lua_Debug where{};
            if (lua_getstack(state, 1, &where) && lua_getinfo(state, "Sl", &where) && where.currentline > 0)
            {
                entry.Source.Chunk = ChunkLabel(where.source ? where.source : "", where.short_src);
                entry.Source.Line = where.currentline;
            }
            const std::string chunk = entry.Source.Chunk.empty() ? std::string("lua") : entry.Source.Chunk;
            RuntimeLog::Write(LogLevel::Warning, "Lua", chunk.c_str(), "chunk", entry.Source.Line, "[" + entry.Channel + "] " + entry.Text);
            EngineOf(state).AddEntry(std::move(entry));
        }

        int CallUnavailable(lua_State* state)
        {
            const auto* gap = static_cast<const CompatGap*>(lua_touserdata(state, lua_upvalueindex(1)));
            if (!gap->Quiet)
                WarnOnce(state, *gap, lua_tostring(state, lua_upvalueindex(2)));
            return PushResult(state, gap->Result);
        }

        int CallNoEffect(lua_State* state)
        {
            const auto* gap = static_cast<const CompatGap*>(lua_touserdata(state, lua_upvalueindex(1)));
            return PushResult(state, gap->Result);
        }

        int CompatStub(lua_State* state)
        {
            std::size_t length = 0;
            const char* name = luaL_checklstring(state, 1, &length);
            const CompatGap* gap = FindCompatGap(std::string_view(name, length));
            if (!gap || gap->Kind != GapKind::Unavailable)
                return 0;
            lua_getfield(state, LUA_REGISTRYINDEX, kStubsKey);
            if (lua_getfield(state, -1, name) == LUA_TFUNCTION)
                return 1;
            lua_pop(state, 1);
            lua_pushlightuserdata(state, const_cast<CompatGap*>(gap));
            lua_pushvalue(state, 1);
            lua_pushcclosure(state, &CallUnavailable, 2);
            lua_pushvalue(state, -1);
            lua_setfield(state, -3, name);
            return 1;
        }

        int CompatGapInfo(lua_State* state)
        {
            const CompatGap* gap = FindCompatGap(luaL_checkstring(state, 1));
            if (!gap)
                return 0;
            lua_pushstring(state, gap->Kind == GapKind::Unavailable ? "unavailable" : "noeffect");
            lua_pushstring(state, gap->Reason->Text);
            return 2;
        }
    }

    std::span<const CompatGap> CompatGaps()
    {
        return kGaps;
    }

    std::span<const char* const> CheatEngineMethodNames()
    {
        return kMethodNames;
    }

    const CompatGap* FindCompatGap(std::string_view name)
    {
        const auto& index = Index();
        if (const auto found = index.find(name); found != index.end())
            return found->second;
        if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z')
        {
            std::string lower(name);
            lower[0] = static_cast<char>(lower[0] - 'A' + 'a');
            if (const auto found = index.find(lower); found != index.end())
                return found->second;
        }
        return nullptr;
    }

    std::string CompatGapNote(const CompatGap& gap)
    {
        if (gap.Kind == GapKind::NoEffect)
            return std::string("No effect in Manifold Engine: ") + gap.Reason->Text + ".";
        std::string note = std::string("Not available in Manifold Engine: ") + gap.Reason->Text + ". A call does nothing and returns " + ResultText(gap.Result);
        note += gap.Quiet ? "." : "; the first call writes a warning to the Lua output.";
        return note;
    }

    std::string CompatGapsMarkdown()
    {
        auto byName = [](const CompatGap* a, const CompatGap* b) { return Lower(a->Name) < Lower(b->Name); };

        std::string text;
        text += "# Cheat Engine API coverage\n\n";
        text += "Generated by `Manifold-Engine.Tests.exe --ce-compat-markdown`; the tests compare this file with the program.\n\n";
        text += "Manifold Engine provides the global functions of Cheat Engine 7.5's Lua documentation (`celua.txt`). The functions below are the exceptions: some exist but have no effect, the others are not available. [LUA-API.md](LUA-API.md) describes the functions that work. The documentation panel of the Lua engine window shows the same notes.\n\n";

        std::vector<const CompatGap*> noEffect;
        for (const CompatGap& gap : kGaps)
        {
            if (gap.Kind == GapKind::NoEffect)
                noEffect.push_back(&gap);
        }
        std::sort(noEffect.begin(), noEffect.end(), byName);
        text += "## Accepted without effect\n\n";
        text += "These are real global functions, so scripts that call them run on.\n\n";
        text += "| Function | Returns | Why |\n| --- | --- | --- |\n";
        for (const CompatGap* gap : noEffect)
        {
            const char* result = gap->Result == GapResult::Script ? "a stand-in" : gap->Result == GapResult::Nothing ? "nothing" : ResultText(gap->Result);
            text += "| `" + std::string(gap->Name) + "` | " + result + " | " + gap->Reason->Text + " |\n";
        }

        text += "\n## Not available\n\n";
        text += "A call does nothing and returns nil, or the value named after the function. The first call of each function in a session writes a warning to the Lua output with the script and line; the marked partners of `register` functions stay silent. These names are not stored in the global table, so `rawget(_G, name)` stays nil and a script that checks for a function first still takes its own way.\n";
        std::vector<const GapReason*> reasons;
        for (const CompatGap& gap : kGaps)
        {
            if (gap.Kind == GapKind::Unavailable && std::find(reasons.begin(), reasons.end(), gap.Reason) == reasons.end())
                reasons.push_back(gap.Reason);
        }
        for (const GapReason* reason : reasons)
        {
            std::vector<const CompatGap*> names;
            for (const CompatGap& gap : kGaps)
            {
                if (gap.Kind == GapKind::Unavailable && gap.Reason == reason)
                    names.push_back(&gap);
            }
            std::sort(names.begin(), names.end(), byName);
            text += "\n### " + std::string(reason->Title) + "\n\n";
            text += Sentence(reason->Text) + ".\n\n";
            std::string line;
            for (const CompatGap* gap : names)
            {
                if (!line.empty())
                    line += ", ";
                line += "`" + std::string(gap->Name) + "`";
                if (gap->Result != GapResult::Nil && gap->Result != GapResult::Nothing)
                    line += std::string(" (") + ResultText(gap->Result) + ")";
                if (gap->Quiet)
                    line += " (silent)";
            }
            text += line + "\n";
        }

        text += "\n## Methods listed among the functions\n\n";
        text += "`celua.txt` lists these methods and classes where its global functions are, so the documentation panel shows them as functions. They are not global functions in Cheat Engine either: ";
        std::vector<std::string> methods(std::begin(kMethodNames), std::end(kMethodNames));
        std::sort(methods.begin(), methods.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
        for (std::size_t i = 0; i < methods.size(); ++i)
            text += (i ? ", `" : "`") + methods[i] + "`";
        text += ".\n";
        return text;
    }

    void RegisterCompatGaps(LuaEngine& engine, lua_State* state)
    {
        (void)engine;
        lua_newtable(state);
        lua_setfield(state, LUA_REGISTRYINDEX, kStubsKey);
        lua_newtable(state);
        lua_setfield(state, LUA_REGISTRYINDEX, kWarnedKey);
        lua_pushcfunction(state, &CompatStub);
        lua_setglobal(state, "__manifoldCompatStub");
        lua_pushcfunction(state, &CompatGapInfo);
        lua_setglobal(state, "__manifoldCompatGap");
        for (const CompatGap& gap : kGaps)
        {
            if (gap.Kind != GapKind::NoEffect || gap.Result == GapResult::Script)
                continue;
            lua_pushlightuserdata(state, const_cast<CompatGap*>(&gap));
            lua_pushcclosure(state, &CallNoEffect, 1);
            lua_setglobal(state, gap.Name);
            NoteFunction(gap.Name);
        }
    }
}
