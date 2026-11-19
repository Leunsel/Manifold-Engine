#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        constexpr const char* kHeadless = R"LUA(
__manifoldHeadless = true
local function say(kind, text)
  print("[" .. kind .. "] " .. tostring(text):gsub("\r?\n", " | "))
end
showMessage = function(text) say("showMessage", text) end
messageDialog = function(...)
  local args = { ... }
  local text = args[1]
  if type(args[2]) == "string" then
    text = tostring(args[1]) .. ": " .. args[2]
  end
  local buttons = {}
  for i = 2, #args do
    local value = args[i]
    if type(value) == "table" then
      for _, button in ipairs(value) do buttons[button] = true end
    elseif i > 2 or type(args[2]) ~= "string" then
      buttons[value] = true
    end
  end
  local answer = mrOk
  if buttons[mbNo] then
    answer = mrNo
  elseif buttons[mbCancel] then
    answer = mrCancel
  end
  say("messageDialog", text .. " -> " .. (answer == mrNo and "No" or answer == mrCancel and "Cancel" or "OK"))
  return answer
end
os.execute = function(command)
  say("os.execute not run", command)
  return nil, "exit", 1
end
io.popen = function(command)
  say("io.popen not run", command)
  return nil, "blocked in the headless check"
end
ShellExecute = function(command)
  say("ShellExecute not run", command)
end
shellExecute = ShellExecute
os.exit = function(code)
  say("os.exit not run", code)
end
for _, name in ipairs({ "runCommand", "createProcess", "keyDown", "keyUp", "doKeyPress", "mouse_event", "setMousePos", "sendMessage",
    "beep", "playSound", "speak", "speakEnglish", "setXBox360ControllerVibration", "setUserRegistryEnvironmentVariable",
    "deleteFile", "saveTable", "createRemoteThread" }) do
  _G[name] = function(first)
    say(name .. " not run", first)
    return nil, "blocked in the headless check"
  end
end
inputQuery = function(caption, prompt, initial)
  say("inputQuery", tostring(caption) .. ": " .. tostring(prompt))
  return nil
end
)LUA";

        std::string Argument(const std::vector<std::string>& arguments, const std::string& name, const std::string& fallback = {})
        {
            const std::string prefix = "--" + name + "=";
            for (const std::string& argument : arguments)
            {
                if (argument.rfind(prefix, 0) == 0)
                    return argument.substr(prefix.size());
            }
            return fallback;
        }

        void SetHome(const std::filesystem::path& home)
        {
            std::error_code error;
            std::filesystem::create_directories(home / L"AppData" / L"Local", error);
            std::filesystem::create_directories(home / L"AppData" / L"Roaming", error);
            const std::wstring local = (home / L"AppData" / L"Local").wstring();
            const std::wstring roaming = (home / L"AppData" / L"Roaming").wstring();
            _wputenv_s(L"USERPROFILE", home.wstring().c_str());
            _wputenv_s(L"LOCALAPPDATA", local.c_str());
            _wputenv_s(L"APPDATA", roaming.c_str());
            ::SetEnvironmentVariableW(L"USERPROFILE", home.wstring().c_str());
            ::SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
            ::SetEnvironmentVariableW(L"APPDATA", roaming.c_str());
        }

        struct Printer
        {
            int Errors = 0;
            std::size_t NoticeCount = 0;

            void Flush(Engine& engine)
            {
                for (const Lua::ConsoleEntry& entry : engine.Lua().Console().Entries())
                {
                    const bool error = entry.Kind == Lua::EntryKind::Error || entry.Level == LogLevel::Error;
                    if (error)
                        ++Errors;
                    std::string text = entry.PlainText();
                    if (!entry.Source.Chunk.empty() && entry.Kind == Lua::EntryKind::Error)
                        text = entry.Source.Chunk + ":" + std::to_string(entry.Source.Line) + ": " + text;
                    for (std::uint32_t repeat = 0; repeat < entry.Repeat; ++repeat)
                        std::printf("%s %s\n", error ? "LUA-ERROR" : "lua      ", text.c_str());
                }
                engine.Lua().ClearOutput();
                for (const Notice& notice : engine.Notifications().Visible(engine.Now(), 1e9))
                {
                    if (notice.Id <= static_cast<int>(NoticeCount))
                        continue;
                    NoticeCount = static_cast<std::size_t>(notice.Id);
                    if (notice.Level == LogLevel::Error)
                        ++Errors;
                    std::printf("NOTICE   [%s] %s\n", RuntimeLog::LevelName(notice.Level), notice.Text.c_str());
                }
                std::fflush(stdout);
            }
        };
    }

    int RunCompatHarness(const std::vector<std::string>& arguments)
    {
        std::filesystem::path table;
        for (std::size_t i = 0; i < arguments.size(); ++i)
        {
            if (arguments[i] == "--compat" && i + 1 < arguments.size() && arguments[i + 1].rfind("--", 0) != 0)
                table = Utf8ToWide(arguments[i + 1]);
        }
        const std::string autorun = Argument(arguments, "autorun");
        const std::string processName = Argument(arguments, "process");
        const double seconds = ParseDouble(Argument(arguments, "seconds", "3")).value_or(3.0);
        wchar_t temp[MAX_PATH] = L"";
        ::GetTempPathW(MAX_PATH, temp);
        const std::filesystem::path home = Utf8ToWide(Argument(arguments, "home", WideToUtf8((std::filesystem::path(temp) / L"Manifold-Compat-Home").wstring())));
        SetHome(home);
        std::printf("compat home %ls\n", home.c_str());
        const bool dryRun = std::find(arguments.begin(), arguments.end(), "--dry-run") != arguments.end();
        Target::Process::SetDryRun(dryRun);
        if (dryRun)
            std::printf("dry run: nothing is written to the process\n");

        Settings settings;
        settings.TableScripts = TableScriptPolicy::Always;
        settings.AutoAttachEnabled = false;
        Engine engine(settings);
        Printer printer;
        printer.Flush(engine);
        engine.Lua().Run(kHeadless, "headless");

        if (!autorun.empty())
        {
            std::string folder = autorun;
            if (!folder.empty() && folder.back() != '\\' && folder.back() != '/')
                folder += "\\";
            engine.Lua().Run("getAutorunPath = function() return [==[" + folder + "]==] end", "autorun path");
            std::printf("autorun %s\n", folder.c_str());
            const Status status = engine.Lua().RunAutorun(Utf8ToWide(autorun));
            if (!status)
                std::printf("AUTORUN  %s\n", status.Text.c_str());
            printer.Flush(engine);
        }
        if (!processName.empty())
        {
            const Status status = engine.OpenProcessByName(processName);
            std::printf("process %s: %s\n", processName.c_str(), status ? "opened" : status.Text.c_str());
            printer.Flush(engine);
        }
        if (!table.empty())
        {
            std::printf("table %ls\n", table.c_str());
            const Status status = engine.LoadTable(table, false);
            if (!status)
            {
                std::printf("TABLE    %s\n", status.Text.c_str());
                ++printer.Errors;
            }
            printer.Flush(engine);
        }
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
        while (std::chrono::steady_clock::now() < end)
        {
            engine.Tick();
            printer.Flush(engine);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        if (std::find(arguments.begin(), arguments.end(), "--check-scripts") != arguments.end())
        {
            int checked = 0;
            int failed = 0;
            std::vector<Table::MemoryRecord*> scripts;
            engine.Table().ForEach([&](Table::MemoryRecord& record)
                {
                    if (record.Kind == Table::RecordKind::Script)
                        scripts.push_back(&record);
                });
            std::vector<std::string> provided;
            for (const Table::MemoryRecord* record : scripts)
            {
                const std::string lower = Lower(record->Script);
                for (const char* command : { "registersymbol(", "globalalloc(" })
                {
                    for (std::size_t at = lower.find(command); at != std::string::npos; at = lower.find(command, at + 1))
                    {
                        const std::size_t open = at + std::strlen(command);
                        const std::size_t close = lower.find_first_of(",)", open);
                        if (close == std::string::npos)
                            continue;
                        for (const std::string& name : Split(lower.substr(open, close - open), ','))
                            provided.push_back(Trim(name));
                    }
                }
            }
            int dependent = 0;
            int needsGame = 0;
            for (Table::MemoryRecord* record : scripts)
            {
                for (const Asm::AaSection section : { Asm::AaSection::Enable, Asm::AaSection::Disable })
                {
                    if (section == Asm::AaSection::Disable && !Asm::HasSections(record->Script))
                        continue;
                    Asm::AaMemory memory;
                    const Asm::AaResult result = engine.AutoAssemble(record->Script, Asm::HasSections(record->Script) ? section : Asm::AaSection::Whole, memory, true, "check", record);
                    ++checked;
                    if (!result.Ok() && result.Error.Code == ErrorCode::ExprUnknownSymbol)
                    {
                        const std::size_t open = result.Error.Text.find('"');
                        const std::size_t close = open == std::string::npos ? std::string::npos : result.Error.Text.find('"', open + 1);
                        const std::string symbol = close == std::string::npos ? std::string() : Lower(result.Error.Text.substr(open + 1, close - open - 1));
                        const bool luaBuilt = ContainsNoCase(record->Script, "{$lua}") && ContainsNoCase(record->Script, "syntaxcheck");
                        if (luaBuilt || (!symbol.empty() && std::find(provided.begin(), provided.end(), symbol) != provided.end()))
                        {
                            ++dependent;
                            continue;
                        }
                    }
                    if (!result.Ok() && !engine.HasProcess() && (result.Error.Code == ErrorCode::NoProcess || result.Error.Code == ErrorCode::ExprUnknownSymbol || result.Error.Code == ErrorCode::AsmUnknownSymbol))
                    {
                        ++needsGame;
                        continue;
                    }
                    if (!result.Ok())
                    {
                        ++failed;
                        std::printf("SCRIPT   [%s] %s line %d: %s\n", section == Asm::AaSection::Enable ? "enable" : "disable", record->Description.c_str(), result.Line, result.Error.Text.c_str());
                    }
                }
                printer.Flush(engine);
            }
            std::printf("checked %d script sections, %d failed, %d need a symbol that another script or a skipped {$lua} block registers, %d need the game\n", checked, failed, dependent, needsGame);
            printer.Errors += failed;
        }
        const std::string eval = Argument(arguments, "eval");
        if (!eval.empty())
        {
            std::string code = eval;
            if (std::filesystem::is_regular_file(Utf8ToWide(eval)))
            {
                FILE* file = nullptr;
                if (_wfopen_s(&file, Utf8ToWide(eval).c_str(), L"rb") == 0 && file)
                {
                    code.clear();
                    char buffer[4096];
                    std::size_t read = 0;
                    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
                        code.append(buffer, read);
                    std::fclose(file);
                }
            }
            const Status status = engine.Lua().Run(code, "eval");
            if (!status)
                std::printf("EVAL     %s\n", status.Text.c_str());
            printer.Flush(engine);
        }
        for (const LogEntry& entry : RuntimeLog::Entries())
        {
            if (entry.Level == LogLevel::Error || (entry.Level == LogLevel::Warning && entry.Area != "Program"))
                std::printf("LOG      [%s] %s\n", entry.Area.c_str(), entry.Message.c_str());
        }
        if (dryRun && engine.Process())
        {
            const Target::DryRunActivity activity = engine.Process()->DryActivity();
            std::printf("dry run held back: %d writes (%zu bytes), %d allocations, %d frees, %d protection changes, %d threads, %d libraries\n",
                activity.Writes, activity.BytesWritten, activity.Allocations, activity.Frees, activity.Protections, activity.Threads, activity.Libraries);
        }
        std::printf("\n%d Lua errors\n", printer.Errors);
        return printer.Errors;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_MODE("--compat", &RunCompatHarness);
}
