#include "TestKit.h"

#include "App/Engine.h"
#include "App/WriteHistory.h"
#include "Core/Text.h"
#include "Core/CheckedMutex.h"
#include "Core/RuntimeLog.h"
#include "Lua/LuaEngine.h"
#include "Target/PatchScan.h"
#include "Disasm/AssemblySearch.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <thread>

#include <windows.h>

#include "Syscalls/Syscall.h"

namespace Manifold::Tests
{
    void CheckBreakActions(Engine& engine, bool veh);
    void CheckFpuAndExceptions(Engine& engine, bool veh);
    void CheckStepOut(Engine& engine, bool veh);
    void CheckTraceOneThread(Engine& engine, bool veh);
    void CheckWritesKeepBreakpoints(Engine& engine);
    void CheckWatchesOnOneInstruction(Engine& engine, bool veh);
    void CheckAttachArmsMainThread(Engine& engine, HANDLE mainThread, std::uint64_t address);
    void CheckMono(Engine& engine);
    void CheckIl2CppTarget(const std::filesystem::path& targetPath);
    void CheckManagedDumperTarget(const std::filesystem::path& targetPath);

    void CheckCodeCalls(Engine& engine, const std::string& module)
    {
        const bool wide = engine.Process()->Is64Bit();
        std::string answer;
        const Status lua = engine.Lua().RunWithResult("local module, wide = '" + module + "', " + (wide ? "true" : "false") + R"LUA(
            local pid = executeCodeEx(0, nil, 'kernel32.GetCurrentProcessId')
            if pid ~= getOpenedProcessID() then return 'process id ' .. tostring(pid) end
            if executeCodeEx(0, 5000, 'kernel32.MulDiv', 6, 7, 2) ~= 21 then return 'three integers' end
            if executeCodeEx(0, nil, 'kernel32.MultiByteToWideChar', 65001, 0, 'Hallo', -1, 0, 0) ~= 6 then return 'six parameters with text' end
            if executeCodeEx(0, nil, 'kernel32.lstrlenW', { type = 4, value = 'Hallo du' }) ~= 8 then return 'wide text' end
            if executeCodeEx(0, nil, 'kernel32.lstrlenA', { type = 3, value = 'Hallo' }) ~= 5 then return 'typed text' end
            local mixed = executeCodeEx(1, nil, module .. '.MixedCall', 1, { type = 2, value = 2.5 }, { type = 1, value = 1.5 }, 4, 5, { type = 2, value = 0.25 })
            if mixed ~= 1 + 2 + 3 + 40 + 500 + 250 then return 'mixed parameters ' .. tostring(mixed) end
            if pcall(executeCodeEx, 0, nil, 0) then return 'address 0 accepted' end
            local answerStub = allocateMemory(0x100)
            if not autoAssemble(string.format('%X:\nmov eax,2A\n%s', answerStub, wide and 'ret' or 'ret 4')) then return 'the answer stub did not assemble' end
            local answered = executeCode(answerStub, 0)
            if answered ~= 42 then return 'executeCode gave ' .. tostring(answered) end
            local unwaited = executeCode(answerStub, 0, 0)
            if unwaited ~= nil then return 'executeCode without a wait gave ' .. tostring(unwaited) end
            local slowStub = allocateMemory(0x100)
            local slow = wide and 'sub rsp,28\nmov ecx,#1000\nmov rax,kernel32.Sleep\ncall rax\nadd rsp,28\nret' or 'push #1000\nmov eax,kernel32.Sleep\ncall eax\nret 4'
            if not autoAssemble(string.format('%X:\n%s', slowStub, slow)) then return 'the slow stub did not assemble' end
            local timedOut, late = executeCode(slowStub, 0, 100)
            if timedOut ~= nil or not late then return 'executeCode ignored its timeout: ' .. tostring(timedOut) end
            local patientStub = allocateMemory(0x100)
            local patient = wide and 'sub rsp,28\nmov ecx,#10500\nmov rax,kernel32.Sleep\ncall rax\nadd rsp,28\nmov eax,2A\nret' or 'push #10500\nmov eax,kernel32.Sleep\ncall eax\nmov eax,2A\nret 4'
            if not autoAssemble(string.format('%X:\n%s', patientStub, patient)) then return 'the patient stub did not assemble' end
            local waited, reason = executeCode(patientStub, 0)
            if waited ~= 42 then return 'executeCode without a timeout gave ' .. tostring(waited) .. ' ' .. tostring(reason) end

            local probe = readPointer(module .. '.g_callProbe')
            local add = readPointer(readPointer(probe))
            if executeMethod(0, nil, add, probe, 2, 5) ~= 57 then return 'method' end
            if executeMethod(0, nil, add, { classinstance = probe }, 2, 5) ~= 57 then return 'method by table' end
            if executeMethod(0, nil, add, { regnr = 1, classinstance = probe }, 3, 1) ~= 46 then return 'method in ECX' end

            local script = wide and 'alloc(callprobe,64)\nregisterSymbol(callprobe)\ncallprobe:\nmov eax,[rbx]\nadd eax,ecx\nadd eax,edx\nret'
              or 'alloc(callprobe,64)\nregisterSymbol(callprobe)\ncallprobe:\nmov eax,[ebx]\nadd eax,[esp+4]\nadd eax,[esp+8]\nret 8'
            if not autoAssemble(script) then return 'the probe did not assemble' end
            local cell = allocateMemory(16)
            writeInteger(cell, 1000)
            local viaRegister = executeMethod(0, nil, 'callprobe', { regnr = 3, classinstance = cell }, 20, 3)
            deAlloc(cell)
            if viaRegister ~= 1023 then return 'instance in EBX ' .. tostring(viaRegister) end
            if pcall(executeMethod, 0, nil, 'callprobe', { regnr = 4, classinstance = 1 }) then
              local value, reason = executeMethod(0, nil, 'callprobe', { regnr = 4, classinstance = 1 })
              if value ~= nil or not reason then return 'the stack register was accepted' end
            end

            local damage = getAddress(module .. '.Damage')
            local aob, offset = getUniqueAOB(damage)
            if type(aob) ~= 'string' or offset == nil then return 'no unique AOB' end
            local hits = AOBScan(aob)
            if not hits or hits.Count ~= 1 then return 'the AOB is not unique: ' .. aob end
            if tonumber(hits[0], 16) + offset ~= damage then return 'the AOB does not lead to the address' end
            local dotted, first = pcall(function() return hits.getString(0) end)
            if not dotted or first ~= hits[0] then return 'dot getString: ' .. tostring(first) end
            if hits:getString(0) ~= hits[0] then return 'colon getString' end
            hits.destroy()
            if getUniqueAOB(0x10) ~= nil then return 'an AOB for unreadable memory' end
            return 'ok|' .. aob .. '|' .. offset)LUA",
            "code calls", answer);
        CHECK_MSG(lua.Ok(), lua.Text);
        CHECK_MSG(answer.rfind("ok|", 0) == 0, module + ": " + answer);
        std::printf("  %s: unique AOB of Damage: %s\n", module.c_str(), answer.c_str());
    }

    namespace
    {
        struct TargetProgram
        {
            PROCESS_INFORMATION Info{};
            std::map<std::string, std::uint64_t> Addresses;

            ~TargetProgram()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }
        };

        bool Launch(const std::filesystem::path& path, TargetProgram& target)
        {
            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE read = nullptr;
            HANDLE write = nullptr;
            if (!::CreatePipe(&read, &write, &security, 0))
                return false;
            ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = write;
            startup.hStdError = write;
            std::wstring command = L"\"" + path.wstring() + L"\"";
            const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &target.Info);
            ::CloseHandle(write);
            if (!started)
            {
                ::CloseHandle(read);
                return false;
            }
            std::string text;
            char buffer[512];
            DWORD got = 0;
            while (text.find("shoot at") == std::string::npos || text.find('\n', text.find("shoot at")) == std::string::npos)
            {
                if (!::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
                    break;
                text.append(buffer, got);
            }
            ::CloseHandle(read);
            const auto take = [&](const char* label)
                {
                    const std::size_t at = text.find(label);
                    if (at == std::string::npos)
                        return;
                    const std::size_t start = at + std::strlen(label);
                    std::size_t end = start;
                    while (end < text.size() && IsHexDigit(text[end]))
                        ++end;
                    if (const auto value = ParseHex(text.substr(start, end - start)))
                        target.Addresses[label] = *value;
                };
            for (const char* label : { "g_world at ", "hero at ", "health at ", "damage at ", "readhealth at ", "shoot at " })
                take(label);
            return target.Addresses.size() == 6;
        }

        bool WaitScan(Engine& engine)
        {
            for (int i = 0; i < 600; ++i)
            {
                if (engine.Scanner().Poll())
                    return engine.Scanner().LastError().Ok();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return false;
        }

        bool Contains(Engine& engine, std::uint64_t address)
        {
            const std::uint64_t count = std::min<std::uint64_t>(engine.Scanner().Count(), 5'000'000);
            Scan::ScanRecord record;
            for (std::uint64_t i = 0; i < count; ++i)
            {
                if (engine.Scanner().Record(i, record) && record.Address == address)
                    return true;
            }
            return false;
        }

        void Pause(Engine& engine, int milliseconds)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (std::chrono::steady_clock::now() < until)
            {
                engine.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        std::uint64_t PlaceStill(Engine& engine, std::int32_t value)
        {
            const Result<Address> allocated = engine.Process()->Allocate(4096, PAGE_READWRITE);
            CHECK_MSG(allocated.Ok(), allocated.Error().Text);
            const std::uint64_t page = allocated.ValueOr(0);
            if (!page)
                return 0;
            CHECK(engine.Process()->Write(page + 0x40, &value, sizeof(value)).Ok());
            return page + 0x40;
        }

        bool ContainsTyped(Engine& engine, std::uint64_t address, Scan::ValueType type)
        {
            const std::uint64_t count = std::min<std::uint64_t>(engine.Scanner().Count(), 5'000'000);
            Scan::ScanRecord record;
            for (std::uint64_t i = 0; i < count; ++i)
            {
                if (engine.Scanner().Record(i, record) && record.Address == address && record.Type == type)
                    return true;
            }
            return false;
        }

        bool RunScan(Engine& engine, const Scan::ScanOptions& options, bool first)
        {
            const Status started = engine.Scanner().Start(engine.Process(), options, first);
            CHECK_MSG(started.Ok(), started.Text);
            return started.Ok() && WaitScan(engine);
        }

        void ScanKindTests(Engine& engine, std::uint64_t hero, std::uint64_t health)
        {
            const Target::Process& process = *engine.Process();
            Scan::ScanOptions options;
            options.Kind = Scan::ScanKind::All;
            options.Value = "1234.5";
            options.Alignment = 4;
            CHECK(RunScan(engine, options, true));
            CHECK(ContainsTyped(engine, hero + 8, Scan::ValueType::Double));
            CHECK(!ContainsTyped(engine, hero + 8, Scan::ValueType::EightBytes));
            engine.Scanner().Reset();

            options = {};
            options.Kind = Scan::ScanKind::All;
            const std::int32_t healthValue = process.ReadValue<std::int32_t>(health).value_or(0);
            const std::uint64_t still = PlaceStill(engine, healthValue);
            options.Value = std::to_string(healthValue);
            options.Alignment = 4;
            CHECK(RunScan(engine, options, true));
            CHECK_MSG(ContainsTyped(engine, health, Scan::ValueType::FourBytes) && ContainsTyped(engine, health, Scan::ValueType::TwoBytes), options.Value);
            CHECK(ContainsTyped(engine, still, Scan::ValueType::FourBytes));
            const std::uint64_t allCount = engine.Scanner().Count();
            Pause(engine, 1300);
            options.Mode = Scan::Compare::Decreased;
            CHECK(RunScan(engine, options, false));
            CHECK(ContainsTyped(engine, health, Scan::ValueType::FourBytes));
            CHECK(!ContainsTyped(engine, still, Scan::ValueType::FourBytes));
            CHECK(engine.Scanner().Count() < allCount);
            CHECK(engine.Scanner().Kind() == Scan::ScanKind::All);
            engine.Scanner().Reset();
            engine.Process()->Free(still & ~0xFFFull);

            options = {};
            options.Value = std::to_string(process.ReadValue<std::int32_t>(health).value_or(0));
            CHECK(RunScan(engine, options, true));
            CHECK(engine.Scanner().SaveResults("start"));
            CHECK(engine.Scanner().HasSaved("start"));
            Pause(engine, 1300);
            options.Mode = Scan::Compare::Changed;
            CHECK(RunScan(engine, options, false));
            CHECK(Contains(engine, health));
            options.Mode = Scan::Compare::Decreased;
            options.Against = Scan::CompareTo::Saved;
            options.SavedName = "start";
            CHECK(RunScan(engine, options, false));
            CHECK(Contains(engine, health));
            options.Mode = Scan::Compare::Unchanged;
            options.Against = Scan::CompareTo::First;
            CHECK(RunScan(engine, options, false));
            CHECK(!Contains(engine, health));
            options.Against = Scan::CompareTo::Saved;
            options.SavedName = "missing";
            CHECK_EQ(engine.Scanner().Start(engine.Process(), options, false).Code, ErrorCode::ScanSavedMissing);
            engine.Scanner().Reset();
            CHECK(!engine.Scanner().HasSaved("start"));

            // A NaN that stays (0xFFFFFFFF, int -1) is unchanged memory: in Unchanged and Same as first, never in Changed.
            const std::uint64_t nan = PlaceStill(engine, -1);
            for (const Scan::ValueType type : { Scan::ValueType::Float, Scan::ValueType::Double })
            {
                options = {};
                options.Type = type;
                options.Mode = Scan::Compare::Unknown;
                options.Start = nan & ~0xFFFull;
                options.Stop = options.Start + 0x1000;
                if (type == Scan::ValueType::Double)
                    CHECK(engine.Process()->Write(nan + 4, "\xFF\xFF\xFF\xFF", 4).Ok());
                CHECK(RunScan(engine, options, true));
                CHECK(Contains(engine, nan));
                options.Mode = Scan::Compare::Changed;
                CHECK(RunScan(engine, options, false));
                CHECK_MSG(!Contains(engine, nan), Scan::TypeName(type));
                engine.Scanner().Reset();
                options.Mode = Scan::Compare::Unknown;
                CHECK(RunScan(engine, options, true));
                options.Mode = Scan::Compare::Unchanged;
                CHECK(RunScan(engine, options, false));
                CHECK_MSG(Contains(engine, nan), Scan::TypeName(type));
                options.Mode = Scan::Compare::Unchanged;
                options.Against = Scan::CompareTo::First;
                CHECK(RunScan(engine, options, false));
                CHECK_MSG(Contains(engine, nan), Scan::TypeName(type));
                engine.Scanner().Reset();
            }
            engine.Process()->Free(nan & ~0xFFFull);

            options = {};
            options.Kind = Scan::ScanKind::Grouped;
            options.Value = "4:0..2000 f:3.5 d:1234.5";
            options.Alignment = 4;
            CHECK(RunScan(engine, options, true));
            CHECK(Contains(engine, hero));
            CHECK(RunScan(engine, options, false));
            CHECK(Contains(engine, hero));
            options.Value = "4:* f:3.5";
            CHECK_EQ(engine.Scanner().Start(engine.Process(), options, false).Code, ErrorCode::ScanGroupSyntax);
            options.Mode = Scan::Compare::Bigger;
            CHECK_EQ(engine.Scanner().Start(engine.Process(), options, false).Code, ErrorCode::ScanCompareUnavailable);
            engine.Scanner().Reset();
            options = {};
            options.Kind = Scan::ScanKind::Grouped;
            options.Value = "4:0..2000 d:1234.5 f:3.5 within:16";
            options.Alignment = 4;
            CHECK(RunScan(engine, options, true));
            CHECK(Contains(engine, hero));
            engine.Scanner().Reset();

            const Status registered = engine.Lua().Run(
                "registerCustomTypeLua('Big endian test', 4, function(a, b, c, d) return ((a * 256 + b) * 256 + c) * 256 + d end, "
                "function(v) return (v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255 end, false)", "custom type");
            CHECK_MSG(registered.Ok(), registered.Text);
            const Lua::CustomType* custom = engine.Lua().FindCustomType("Big endian test");
            CHECK(custom != nullptr);
            const std::uint64_t box = engine.Process()->Allocate(4096, PAGE_READWRITE).ValueOr(0);
            CHECK(box != 0);
            if (custom && box)
            {
                const std::uint8_t big[] = { 0x12, 0x34, 0x56, 0x78 };
                CHECK(engine.Process()->Write(box + 16, big, sizeof(big)).Ok());
                options = {};
                options.Kind = Scan::ScanKind::Custom;
                options.CustomName = custom->Name;
                options.CustomSize = custom->ByteCount();
                options.Value = "305419896";
                CHECK(engine.Lua().CustomFromText(*custom, options.Value, 0, options.CustomBytes));
                CHECK(options.CustomBytes == std::vector<std::uint8_t>(big, big + 4));
                options.Start = box;
                options.Stop = box + 4096;
                options.Writable = Scan::Tristate::Any;
                CHECK(RunScan(engine, options, true));
                CHECK(Contains(engine, box + 16));
                Scan::ScanRecord record;
                CHECK(engine.Scanner().Record(0, record) && record.Previous == std::vector<std::uint8_t>(big, big + 4));
                CHECK_EQ(engine.Scanner().CustomName(), custom->Name);
                options.Mode = Scan::Compare::Unchanged;
                CHECK(RunScan(engine, options, false));
                CHECK(Contains(engine, box + 16));
                options.Mode = Scan::Compare::Bigger;
                CHECK_EQ(engine.Scanner().Start(engine.Process(), options, false).Code, ErrorCode::ScanCompareUnavailable);
                engine.Scanner().Reset();
                engine.Process()->Free(box);
            }

            std::string luaResult;
            const Status lua = engine.Lua().RunWithResult(
                "local ms = createMemScan()\n"
                "ms.firstScan(soExactValue, vtAll, rtRounded, '1234.5', '', 0, 0x7fffffffffff, '+W*X-C', fsmAligned, '4', false, true, false, false)\n"
                "ms.waitTillDone()\n"
                "ms.saveCurrentResults('first')\n"
                "local names = ms.getSavedResultList()\n"
                "local found = ms.FoundList\n"
                "return tostring(found.Count > 0) .. '|' .. names[1] .. '|' .. found.getValue(0)",
                "memscan all", luaResult);
            CHECK_MSG(lua.Ok() && luaResult.rfind("true|first|1234.", 0) == 0, lua.Text + " " + luaResult);
        }

        void DirectSystemCalls(std::uint32_t id, std::uint64_t health)
        {
            InitSysCalls();
            CHECK(OpenSysCall != 0 && ReadVmSysCall != 0 && WriteVmSysCall != 0 && AllocVmSysCall != 0);

            const auto missing = Target::Process::Open(MakeProcessId(0xFFFFFFF0u));
            CHECK(!missing);
            CHECK_MSG(missing.Error().Code == ErrorCode::ProcessNotFound, missing.Error().Text);
            const auto denied = Target::Process::Open(MakeProcessId(4));
            CHECK(!denied);
            CHECK_MSG(denied.Error().Code == ErrorCode::ProcessAccessDenied, denied.Error().Text);

            const auto opened = Target::Process::Open(MakeProcessId(id));
            CHECK_MSG(opened.Ok(), opened.Error().Text);
            if (!opened)
                return;
            const std::shared_ptr<Target::Process>& process = *opened;
            std::int32_t value = 0;
            CHECK(process->ReadExact(health, &value, sizeof(value)));
            CHECK(process->Write(health, &value, sizeof(value)).Ok());
            CHECK(!process->ReadExact(0x10, &value, sizeof(value)));

            ::SetLastError(ERROR_SUCCESS);
            const Result<Address> huge = process->Allocate(std::size_t(1) << 46, PAGE_READWRITE);
            CHECK(!huge);
            CHECK_MSG(huge.Error().Code == ErrorCode::AllocFailed && huge.Error().Text.find("completed successfully") == std::string::npos, huge.Error().Text);

            const Result<Address> small = process->Allocate(0x1000, PAGE_READWRITE);
            CHECK_MSG(small.Ok(), small.Error().Text);
            const std::uint64_t memory = small.ValueOr(0);
            if (memory)
                CHECK(process->Free(memory).Ok());
        }
    }

    // The names Cheat Engine gives exports, on the exports TestTarget makes for
    // them: shown names, aliases, an ordinal-only export, the reach.
    void ExportNaming(Engine& engine, const Target::Process& process)
    {
        const auto module = process.FindModule("TestTarget.exe");
        CHECK(module.has_value());
        if (!module)
            return;
        const auto exported = [&](std::string_view name) { return process.FindExport(module->Name, name); };
        const std::uint64_t alias = exported("_mfAliasA");
        const std::uint64_t stdcall = exported("_mfStd@8");
        const std::uint64_t member = exported("?mfMember@Widget@mf@@QEAAHH@Z");
        const std::uint64_t underscore = exported("_mfTwin");
        const std::uint64_t plain = exported("mfTwin");
        const std::uint64_t low = exported("mfGapLow");
        const std::uint64_t high = exported("mfGapHigh");
        CHECK(alias && stdcall && member && underscore && plain && low && high);
        CHECK_EQ(exported("_mfAliasB"), alias);
        if (!alias || !stdcall || !member || !underscore || !plain || !low || !high)
            return;
        const auto offset = [&](std::uint64_t address) { return module->Name + "+" + Hex(address - module->Base); };
        Target::Symbols& symbols = engine.Symbols();

        CHECK_EQ(symbols.Describe(alias), std::string("TestTarget.mfAliasA"));
        CHECK_EQ(symbols.Describe(alias + 1), std::string("TestTarget.mfAliasA+1"));
        CHECK(symbols.ExactName(alias) == std::optional<std::string>("TestTarget.mfAliasA"));
        const auto inside = symbols.Containing(alias + 2, 0x100);
        CHECK(inside && inside->first == alias && inside->second == "TestTarget.mfAliasA");
        CHECK_EQ(symbols.Describe(stdcall), std::string("TestTarget.mfStd"));
        CHECK_EQ(symbols.Describe(member), std::string("TestTarget.mf::Widget::mfMember"));
        CHECK_EQ(symbols.Describe(underscore), std::string("TestTarget.mfTwin"));
        CHECK_EQ(symbols.Describe(plain), std::string("TestTarget.mfTwin"));

        // Shown names resolve too, but a name in the export table wins.
        CHECK_EQ(engine.Evaluate("TestTarget.mfAliasA").value_or(0), alias);
        CHECK_EQ(engine.Evaluate("TestTarget._mfAliasA").value_or(0), alias);
        CHECK_EQ(engine.Evaluate("mfAliasB").value_or(0), alias);
        CHECK_EQ(engine.Evaluate("TestTarget.mfStd").value_or(0), stdcall);
        CHECK_EQ(engine.Evaluate("TestTarget._mfStd@8").value_or(0), stdcall);
        CHECK_EQ(engine.Evaluate("TestTarget.mf::Widget::mfMember").value_or(0), member);
        CHECK_EQ(engine.Evaluate("TestTarget.mfTwin").value_or(0), plain);
        CHECK_EQ(engine.Evaluate("mfTwin").value_or(0), plain);
        CHECK_EQ(engine.Evaluate("TestTarget._mfTwin").value_or(0), underscore);

        // An export with only an ordinal: OrdinalN, N decimal.
        const std::vector<Target::ExportSymbol> exports = symbols.ExportsOf(*module);
        const auto ordinal = std::find_if(exports.begin(), exports.end(), [](const Target::ExportSymbol& symbol) { return symbol.Name == "Ordinal4321"; });
        CHECK(ordinal != exports.end());
        if (ordinal != exports.end())
        {
            // ExportNameOrdinal: mov eax,5102 / ret
            std::uint8_t code[6] = {};
            CHECK(process.ReadExact(ordinal->Address, code, sizeof(code)) && code[0] == 0xB8 && code[1] == 0x02 && code[2] == 0x51 && code[5] == 0xC3);
            CHECK_EQ(symbols.Describe(ordinal->Address), std::string("TestTarget.Ordinal4321"));
            CHECK_EQ(engine.Evaluate("TestTarget.Ordinal4321").value_or(0), ordinal->Address);
        }

        // The reach: up to the next export, at most 64 KB (both ends included),
        // nothing past the last export.
        CHECK(high - low > 0x10000);
        CHECK_EQ(symbols.Describe(low + 0x3000), std::string("TestTarget.mfGapLow+3000"));
        CHECK_EQ(symbols.Describe(low + 0x10000), std::string("TestTarget.mfGapLow+10000"));
        CHECK_EQ(symbols.Describe(low + 0x10001), offset(low + 0x10001));
        const auto last = std::max_element(exports.begin(), exports.end(), [](const Target::ExportSymbol& a, const Target::ExportSymbol& b) { return a.Address < b.Address; });
        CHECK(last != exports.end() && last->Reach == 0);
        if (last != exports.end() && module->Contains(last->Address + 1))
            CHECK_EQ(symbols.Describe(last->Address + 1), offset(last->Address + 1));
        for (std::size_t i = 0; i + 1 < exports.size(); ++i)
        {
            if (exports[i + 1].Address != exports[i].Address)
                CHECK_MSG(exports[i].Reach == std::min<std::uint64_t>(exports[i + 1].Address - exports[i].Address, 0x10000), exports[i].Name);
        }
    }

    int RunIntegrationTests(const std::filesystem::path& targetPath)
    {
        const int before = Totals().Failures;
        TargetProgram target;
        if (!Launch(targetPath, target))
        {
            Skip("the test target " + targetPath.string() + " could not be started");
            return 0;
        }
        const std::uint64_t world = target.Addresses["g_world at "];
        const std::uint64_t hero = target.Addresses["hero at "];
        const std::uint64_t health = target.Addresses["health at "];
        const std::uint64_t damage = target.Addresses["damage at "];
        CHECK_EQ(hero, health);

        RuntimeLog::SetVerbose(true);
        Settings settings;
        settings.ScanMapped = false;
        Engine engine(settings);
        const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
        CHECK_MSG(opened.Ok(), opened.Text);
        if (!opened)
            return Totals().Failures - before;
        const Target::Process& process = *engine.Process();
        CHECK(process.Is64Bit());

        CHECK_EQ(engine.Evaluate("[" + Hex(world) + "]+8").value_or(0) != 0, true);
        CHECK_EQ(engine.Evaluate("[[" + Hex(world) + "]+8]").value_or(0), hero);
        const auto module = process.FindModule("TestTarget.exe");
        CHECK(module.has_value());
        if (module)
        {
            CHECK_EQ(engine.Evaluate("TestTarget.exe").value_or(0), module->Base);
            CHECK_EQ(engine.Evaluate("\"TestTarget.exe\"+10").value_or(0), module->Base + 0x10);
            CHECK_EQ(engine.Evaluate("$process").value_or(0), module->Base);
            CHECK_EQ(engine.Evaluate("$process+1234").value_or(0), module->Base + 0x1234);
            CHECK_EQ(engine.Evaluate("\"$process\"+10").value_or(0), module->Base + 0x10);
            CHECK_EQ(engine.Evaluate("[$process+3C]-[mainmodule+3C]").value_or(1), 0u);
            CHECK_EQ(engine.Evaluate("mainmodule+10").value_or(0), module->Base + 0x10);
            CHECK_EQ(engine.Evaluate("$10+$process").value_or(0), module->Base + 0x10);
            Status unknownDollar;
            CHECK(!engine.Evaluate("$nosuchname", &unknownDollar) && unknownDollar.Code == ErrorCode::ExprUnknownSymbol);
            CHECK_EQ(engine.Symbols().ModuleOffset(damage), "\"TestTarget.exe\"+" + Hex(damage - module->Base));
        }
        const auto sleep = engine.Evaluate("kernel32.Sleep");
        CHECK(sleep.has_value());
        if (sleep)
        {
            const std::string described = engine.Describe(*sleep);
            CHECK_MSG(described.find("Sleep") != std::string::npos, described);
            // The function a disassembly line is in: the export itself, a few
            // bytes into it, and nothing once the reach is used up.
            const auto at = engine.Symbols().Containing(*sleep, 0x1000);
            CHECK(at && at->first == *sleep && at->second.find("Sleep") != std::string::npos);
            const auto inside = engine.Symbols().Containing(*sleep + 4, 0x1000);
            CHECK(inside && inside->first == *sleep);
            CHECK(!engine.Symbols().Containing(*sleep + 4, 4));
        }
        ExportNaming(engine, process);
        CHECK(engine.Symbols().Register("containing_test_symbol", damage).Ok());
        {
            const auto registered = engine.Symbols().Containing(damage + 2, 0x100);
            CHECK(!registered || registered->second != "containing_test_symbol");
            engine.Symbols().Unregister("containing_test_symbol");
        }
        Status unknown;
        CHECK(!engine.Evaluate("no_such_symbol_here", &unknown) && unknown.Code == ErrorCode::ExprUnknownSymbol);

        const Disasm::Instruction first = engine.Disassembler().Read(process, damage, &engine.Symbols());
        CHECK_MSG(first.Valid && first.Mnemonic == "sub" && first.Memory.Present && first.Memory.Written, first.Text());

        if (module)
        {
            Disasm::DissectProgress progress;
            const auto dissect = Disasm::DissectCode(process, { *module }, &progress);
            CHECK(dissect && !dissect->Cancelled && dissect->Stats.Instructions > 1000 && !dissect->Spans.empty());
            if (dissect)
            {
                const auto refs = dissect->To(damage);
                CHECK_MSG(std::any_of(refs.begin(), refs.end(), [](const Disasm::CodeRef& ref) { return ref.Kind() == Disasm::RefKind::Call; }),
                          std::to_string(refs.size()) + " references to Damage");
                CHECK(std::any_of(dissect->Functions.begin(), dissect->Functions.end(), [&](const Disasm::ReferencedFunction& function) { return function.Address == damage && function.Calls > 0; }));
                const bool banner = std::any_of(dissect->Strings.begin(), dissect->Strings.end(),
                    [](const Disasm::ReferencedString& entry) { return entry.Text.find("Manifold test target") != std::string::npos && !entry.Wide; });
                CHECK_MSG(banner, std::to_string(dissect->Strings.size()) + " referenced strings, none with the banner");
                CHECK(std::is_sorted(dissect->Refs.begin(), dissect->Refs.end(), [](const Disasm::CodeRef& a, const Disasm::CodeRef& b) { return a.Target < b.Target; }));
            }
            const auto spans = Disasm::ExecutableSpans(process, { *module });
            for (const char* text : { "sub [rcx],edx\nret", "s?b *[rcx],*; ret" })
            {
                const auto pattern = Disasm::ParseAsmPattern(text);
                CHECK(pattern.has_value());
                if (!pattern)
                    continue;
                const Disasm::AsmSearchResult found = Disasm::SearchAssembly(process, engine.Disassembler(), &engine.Symbols(), spans, *pattern);
                CHECK_MSG(std::any_of(found.Matches.begin(), found.Matches.end(), [&](const Disasm::AsmMatch& match) { return match.Address == damage && match.Lines.size() == 2; }),
                          std::string(text) + ": " + std::to_string(found.Matches.size()) + " matches");
            }
        }

        const auto healthNow = process.ReadValue<std::int32_t>(health);
        CHECK(healthNow.has_value());
        const std::uint64_t still = PlaceStill(engine, healthNow.value_or(0));
        Scan::ScanOptions options;
        options.Type = Scan::ValueType::FourBytes;
        options.Mode = Scan::Compare::Exact;
        options.Value = std::to_string(healthNow.value_or(0));
        options.Mapped = false;
        CHECK(engine.Scanner().Start(engine.Process(), options, true).Ok());
        CHECK(WaitScan(engine));
        CHECK_MSG(Contains(engine, health), "exact scan for " + options.Value + " found " + std::to_string(engine.Scanner().Count()));
        CHECK(Contains(engine, still));
        const std::uint64_t firstCount = engine.Scanner().Count();
        Pause(engine, 1300);
        options.Mode = Scan::Compare::Decreased;
        CHECK(engine.Scanner().Start(engine.Process(), options, false).Ok());
        CHECK(WaitScan(engine));
        CHECK(Contains(engine, health));
        CHECK(!Contains(engine, still));
        CHECK(engine.Scanner().Count() < firstCount);
        engine.Scanner().Undo();
        CHECK_EQ(engine.Scanner().Count(), firstCount);
        engine.Scanner().Reset();
        engine.Process()->Free(still & ~0xFFFull);

        options = {};
        options.Type = Scan::ValueType::Float;
        options.Mode = Scan::Compare::Exact;
        options.Value = "3.5";
        CHECK(engine.Scanner().Start(engine.Process(), options, true).Ok());
        CHECK(WaitScan(engine));
        CHECK(Contains(engine, hero + 4));
        engine.Scanner().Reset();

        options = {};
        options.Type = Scan::ValueType::String;
        options.Value = "manifoldhero";
        options.CaseSensitive = false;
        options.Alignment = 1;
        CHECK(engine.Scanner().Start(engine.Process(), options, true).Ok());
        CHECK(WaitScan(engine));
        CHECK(Contains(engine, hero + 16));
        engine.Scanner().Reset();

        ScanKindTests(engine, hero, health);

        options = {};
        options.Type = Scan::ValueType::FourBytes;
        options.Mode = Scan::Compare::Unknown;
        options.Start = hero & ~0xFFFFull;
        options.Stop = (hero & ~0xFFFFull) + 0x100000;
        CHECK(engine.Scanner().Start(engine.Process(), options, true).Ok());
        CHECK(WaitScan(engine));
        CHECK(engine.Scanner().IsSnapshot());
        Pause(engine, 1100);
        options.Mode = Scan::Compare::Changed;
        CHECK(engine.Scanner().Start(engine.Process(), options, false).Ok());
        CHECK(WaitScan(engine));
        CHECK(Contains(engine, hero + 0x20));
        engine.Scanner().Reset();

        Table::MemoryRecord& pointer = engine.Table().AddValue("Health by pointer", Hex(world), Scan::ValueType::FourBytes);
        pointer.Offsets = { "8", "0" };
        engine.ResolveRecord(pointer);
        CHECK(pointer.AddressOk && pointer.ResolvedAddress == health);
        CHECK(module && world >= module->Base && world < module->Base + module->Size);
        if (module && world >= module->Base && world < module->Base + module->Size)
        {
            const std::string offset = Hex(world - module->Base);
            for (const std::string& written : { "$process+" + offset, "\"$process\"+" + offset, "mainmodule+" + offset })
            {
                Table::MemoryRecord& record = engine.Table().AddValue("Health by " + written, written, Scan::ValueType::FourBytes);
                record.Offsets = { "8", "0" };
                engine.ResolveRecord(record);
                CHECK_MSG(record.AddressOk && record.ResolvedAddress == health, written + ": " + record.AddressError);
            }
            Settings& options = engine.Options();
            Table::MemoryRecord& named = engine.Table().AddValue("Main module", "mainmodule+10", Scan::ValueType::FourBytes);
            engine.ResolveRecord(named);
            options.AddressColumn = 2;
            options.AddressDigits = 0;
            RecordAddressText text = engine.Tables().AddressTextOf(named);
            CHECK_MSG(text.Text == "mainmodule+10" && text.Resolved == Hex(module->Base + 0x10) && text.Tooltip == Hex(module->Base + 0x10), text.Text + " | " + text.Resolved);
            options.AddressColumn = 1;
            text = engine.Tables().AddressTextOf(named);
            CHECK_MSG(text.Text == Hex(module->Base + 0x10) && text.Resolved.empty() && text.Tooltip == "mainmodule+10", text.Text);
            options.AddressColumn = 0;
            text = engine.Tables().AddressTextOf(named);
            CHECK(text.Text == "mainmodule+10" && text.Resolved.empty() && text.Tooltip == Hex(module->Base + 0x10));
            options.AddressColumn = 2;
            Table::MemoryRecord& numeric = engine.Table().AddValue("Numeric", Hex(hero + 0x20), Scan::ValueType::FourBytes);
            engine.ResolveRecord(numeric);
            text = engine.Tables().AddressTextOf(numeric);
            CHECK(text.Text == Hex(hero + 0x20) && text.Resolved.empty());
            text = engine.Tables().AddressTextOf(pointer);
            CHECK_MSG(text.Text == "P->" + Hex(health), text.Text);
            options.AddressDigits = 1;
            CHECK_EQ(engine.Tables().AddressTextOf(pointer).Text, "P->" + Hex(health, 8));
            CHECK_EQ(engine.Tables().AddressHex(0), std::string("00000000"));
            options.AddressDigits = 2;
            CHECK_EQ(engine.Tables().AddressTextOf(pointer).Text, "P->" + Hex(health, 16));
            options.AddressDigits = 0;
            CHECK_EQ(engine.Tables().AddressHex(0), std::string("0"));
        }
        Table::MemoryRecord& ammo = engine.Table().AddValue("Ammo", Hex(hero + 0x20), Scan::ValueType::FourBytes);
        CHECK(engine.WriteRecord(ammo, "25").Ok());
        CHECK(engine.SetActive(ammo, true).Ok());
        Pause(engine, 1200);
        const std::int32_t frozen = process.ReadValue<std::int32_t>(hero + 0x20).value_or(-1);
        CHECK_MSG(frozen == 25 || frozen == 24, std::to_string(frozen));
        CHECK(engine.SetActive(ammo, false).Ok());

        std::uint8_t original[5]{};
        CHECK(process.ReadExact(damage, original, sizeof(original)));
        CHECK(engine.Symbols().Register("DAMAGE", damage, "test").Ok());
        Table::MemoryRecord& script = engine.Table().AddScript("Heal instead of damage",
            "[ENABLE]\nalloc(newmem,$100,DAMAGE)\nlabel(code)\nnewmem:\ncode:\n  add [rcx],edx\n  ret\nDAMAGE:\n  jmp newmem\nregistersymbol(code)\n"
            "[DISABLE]\nDAMAGE:\n  db " + HexBytes(original) + "\nunregistersymbol(code)\ndealloc(newmem)\n");
        const Status enabled = engine.SetActive(script, true);
        CHECK_MSG(enabled.Ok(), enabled.Text);
        CHECK(engine.Symbols().FindRegistered("code").has_value());
        const Disasm::Instruction jump = engine.Disassembler().Read(process, damage, nullptr);
        CHECK_MSG(jump.Kind == Disasm::Flow::Jump && jump.Length == 5, jump.Text());
        const auto scanTarget = [&](std::vector<Target::CodePatch>& patches)
            {
                const auto exe = process.FindModule("TestTarget.exe");
                CHECK(exe.has_value());
                if (!exe)
                    return Target::ModulePatchSummary();
                return Target::ScanModuleForPatches(process, *exe, patches);
            };
        {
            std::vector<Target::CodePatch> patches;
            const Target::ModulePatchSummary summary = scanTarget(patches);
            CHECK_MSG(summary.Problem.Ok(), summary.Problem.Text);
            CHECK(summary.CodeBytes > 0 && !summary.TooMany && summary.UnreadableBytes == 0);
            CHECK_MSG(patches.size() == 1, std::to_string(patches.size()) + " patches, first at " + (patches.empty() ? std::string("-") : Hex(patches.front().Address)));
            if (patches.size() == 1)
            {
                const Target::CodePatch& patch = patches.front();
                CHECK(patch.Address == damage && patch.Complete() && patch.Size <= 5 && patch.Current[0] == 0xE9);
                CHECK(std::memcmp(patch.Original.data(), original, static_cast<std::size_t>(patch.Size)) == 0);
            }
        }
        const std::int32_t healedFrom = process.ReadValue<std::int32_t>(health).value_or(0);
        Pause(engine, 2300);
        CHECK_MSG(process.ReadValue<std::int32_t>(health).value_or(0) > healedFrom, "health did not rise while the script was active");
        const Status disabled = engine.SetActive(script, false);
        CHECK_MSG(disabled.Ok(), disabled.Text);
        std::uint8_t restored[5]{};
        CHECK(process.ReadExact(damage, restored, sizeof(restored)) && std::memcmp(restored, original, sizeof(original)) == 0);
        {
            std::vector<Target::CodePatch> patches;
            const Target::ModulePatchSummary summary = scanTarget(patches);
            CHECK_MSG(summary.Problem.Ok() && patches.empty(), std::to_string(patches.size()) + " patches left after the script was turned off");
        }
        CHECK(!engine.Symbols().FindRegistered("code").has_value());
        CHECK(script.ScriptMemory.Allocations.empty());

        std::string luaResult;
        Status lua = engine.Lua().RunWithResult("registerSymbol('hero', " + std::to_string(hero) + ")\n"
                                                "writeFloat('hero+4', 7.25)\n"
                                                "local list = getAddressList()\n"
                                                "local record = list.createMemoryRecord()\n"
                                                "record.Description = 'From Lua'\n"
                                                "record.Address = 'hero+8'\n"
                                                "record.Type = vtDouble\n"
                                                "local hits = AOBScan('4D 61 6E 69 66 6F 6C 64 48 65 72 6F')\n"
                                                "return string.format('%.2f|%s|%d|%s', readFloat('hero+4'), record.Value, hits and hits.Count or 0, getNameFromAddress(getAddress('TestTarget.exe')))",
            "test", luaResult);
        CHECK_MSG(lua.Ok(), lua.Text);
        const auto parts = Split(luaResult, '|', true);
        CHECK_MSG(parts.size() == 4 && parts[0] == "7.25" && parts[1] == "1234.5" && parts[2] != "0" && parts[3] == "TestTarget.exe", luaResult);
        CheckCodeCalls(engine, "TestTarget");

        lua = engine.Lua().RunWithResult(R"LUA(
            local memory = allocateMemory(16)
            local record = getAddressList().createMemoryRecord()
            record.Address = memory
            record.Type = vtDword
            record.Description = 100
            record.Value = 100
            local plain = readInteger(memory)
            record.ShowAsHex = true
            record.Value = 100
            local hex = readInteger(memory)
            local description = record.Description
            record.destroy()
            deAlloc(memory)
            return string.format('%s|%d|%d', description, plain, hex))LUA", "record numbers", luaResult);
        CHECK_MSG(lua.Ok() && luaResult == "100|100|256", lua.Ok() ? luaResult : lua.Text);

        if (const std::uint64_t touch = process.FindExport(process.Name(), "Touch"))
        {
            const Target::Symbols& symbols = engine.Symbols();
            CHECK_EQ(symbols.AddressText(touch + 1, Target::AddressStyle::Plain), Hex(touch + 1));
            CHECK(symbols.AddressText(touch + 1, Target::AddressStyle::ModuleOffset).rfind("\"" + process.Name() + "\"+", 0) == 0);
            const std::string named = symbols.AddressText(touch, Target::AddressStyle::Name);
            CHECK_MSG(named.find("Touch") != std::string::npos, named);
            const std::vector<Disasm::Instruction> lines = engine.Disassembler().ReadMany(process, touch, 2, &symbols);
            const std::string copied = Disasm::CopyText(lines, symbols, Target::AddressStyle::Name);
            CHECK_MSG(copied.rfind(named + " - ", 0) == 0 && std::count(copied.begin(), copied.end(), '\n') == 2, copied);
        }
        else
        {
            CHECK_MSG(false, "TestTarget exports no Touch");
        }

        if (const Result<Address> scratch = process.Allocate(0x100, PAGE_READWRITE))
        {
            WriteHistory history;
            const std::uint64_t generation = engine.ProcessGeneration();
            const std::uint64_t at = *scratch;
            const std::uint8_t first[4] = { 1, 2, 3, 4 };
            const std::uint8_t second[2] = { 9, 9 };
            CHECK(history.Write(process, generation, at, first, "Paste").Ok());
            CHECK(history.Write(process, generation, at + 1, second, "Hex typing").Ok());
            CHECK_EQ(history.Entries().size(), std::size_t(2));
            CHECK_EQ(history.Undoable(generation), std::size_t(2));
            CHECK(history.Entries()[0].Original == std::vector<std::uint8_t>({ 0, 0, 0, 0 }) && history.Entries()[0].Source == "Paste");
            CHECK(history.StateOf(process, history.Entries()[0]) == WriteState::ChangedSince);
            CHECK(history.StateOf(process, history.Entries()[1]) == WriteState::InMemory);
            const std::uint8_t amended = 7;
            CHECK(process.Write(at + 1, &amended, 1).Ok());
            history.Amend(history.LastId(), std::span<const std::uint8_t>(&amended, 1));
            CHECK(history.StateOf(process, history.Entries()[1]) == WriteState::ChangedSince);
            const std::uint8_t both[2] = { 7, 9 };
            history.Amend(history.LastId(), both);
            CHECK(history.StateOf(process, history.Entries()[1]) == WriteState::InMemory);
            const auto last = history.LastUndoable(generation);
            CHECK(last && *last == history.LastId());
            CHECK(history.Undo(process, generation, *last).Ok());
            std::uint8_t now[4]{};
            CHECK(process.ReadExact(at, now, sizeof(now)) && now[0] == 1 && now[1] == 2 && now[2] == 3 && now[3] == 4);
            CHECK(history.StateOf(process, history.Entries()[1]) == WriteState::Undone && history.Undoable(generation) == 1);
            CHECK(history.Redo(process, generation, *last).Ok());
            CHECK(process.ReadExact(at, now, sizeof(now)) && now[1] == 7 && now[2] == 9);
            CHECK(!history.Undo(process, generation + 1, *last).Ok());
            CHECK(!history.LastUndoable(generation + 1));
            history.Clear(generation);
            CHECK(history.Entries().empty() && !history.Find(*last));

            WriteHistory order;
            const std::uint64_t spot = at + 0x40;
            const std::uint8_t nops[2] = { 0x90, 0x90 };
            const std::uint8_t jump[2] = { 0xEB, 0xFE };
            std::uint8_t pair[2]{};
            CHECK(order.Write(process, generation, spot, nops, "NOP").Ok());
            CHECK(order.Write(process, generation, spot, jump, "Assemble").Ok());
            const int earlier = order.Entries()[0].Id;
            const int later = order.Entries()[1].Id;
            CHECK(order.BlockedBy(order.Entries()[0]) == later && !order.BlockedBy(order.Entries()[1]).has_value());
            CHECK(!order.Undo(process, generation, earlier).Ok());
            CHECK(process.ReadExact(spot, pair, sizeof(pair)) && pair[0] == 0xEB && pair[1] == 0xFE);
            CHECK(order.Undo(process, generation, later).Ok() && order.Undo(process, generation, earlier).Ok());
            CHECK(process.ReadExact(spot, pair, sizeof(pair)) && pair[0] == 0 && pair[1] == 0);
            CHECK(order.BlockedBy(order.Entries()[1]) == earlier);
            CHECK(!order.Redo(process, generation, later).Ok());
            CHECK(order.Redo(process, generation, earlier).Ok() && order.Redo(process, generation, later).Ok());
            CHECK(process.ReadExact(spot, pair, sizeof(pair)) && pair[0] == 0xEB && pair[1] == 0xFE);
            while (const auto id = order.LastUndoable(generation))
            {
                if (!order.Undo(process, generation, *id))
                    break;
            }
            CHECK(order.Undoable(generation) == 0 && process.ReadExact(spot, pair, sizeof(pair)) && pair[0] == 0 && pair[1] == 0);

            const std::uint8_t high = 0x10;
            const std::uint8_t whole = 0x12;
            CHECK(order.Write(process, generation, spot + 8, std::span<const std::uint8_t>(&high, 1), "Hex typing").Ok());
            const int typed = order.LastId();
            CHECK(order.WriteOrAmend(process, generation, typed, spot + 8, std::span<const std::uint8_t>(&whole, 1), "Hex typing").Ok());
            CHECK(order.Entries().size() == 3 && order.Find(typed) && order.Find(typed)->Written[0] == 0x12 && order.Find(typed)->Original[0] == 0);
            CHECK(order.Undo(process, generation, typed).Ok());
            CHECK(order.WriteOrAmend(process, generation, typed, spot + 8, std::span<const std::uint8_t>(&whole, 1), "Hex typing").Ok());
            CHECK(order.Entries().size() == 4 && order.LastId() != typed && order.Find(typed)->Undone);

            CHECK(!order.Write(process, generation, 0x10, nops, "Paste").Ok());
            CHECK_EQ(order.Entries().size(), std::size_t(4));
            CHECK(order.Write(process, generation + 1, spot + 16, nops, "Paste").Ok());
            CHECK(order.Entries().size() == 1 && order.Entries()[0].Generation == generation + 1);

            WriteHistory many;
            const std::uint8_t tick = 1;
            for (std::size_t i = 0; i < WriteHistory::kLimit + 5; ++i)
                many.Write(process, generation, spot + 24, std::span<const std::uint8_t>(&tick, 1), "Hex typing");
            CHECK(many.Entries().size() == WriteHistory::kLimit && many.Dropped(generation) == 5 && many.Dropped(generation + 1) == 0);

            if (const Result<Address> wide = process.Allocate(0x40000, PAGE_READWRITE))
            {
                WriteHistory blocks;
                const std::vector<std::uint8_t> fill(0x30000, 0x5A);
                CHECK(blocks.Write(process, generation, *wide, fill, "Fill").Ok());
                CHECK(blocks.StateOf(process, blocks.Entries()[0]) == WriteState::InMemory);
                const std::uint8_t other = 1;
                CHECK(process.Write(*wide + 0x2FFFF, &other, 1).Ok());
                CHECK(blocks.StateOf(process, blocks.Entries()[0]) == WriteState::ChangedSince);
                CHECK(blocks.Undo(process, generation, blocks.LastId()).Ok() && blocks.StateOf(process, blocks.Entries()[0]) == WriteState::Undone);
                process.Free(*wide);
            }
            else
            {
                CHECK_MSG(false, wide.Error().Text);
            }
            process.Free(at);
        }
        else
        {
            CHECK_MSG(false, scratch.Error().Text);
        }

        lua = engine.Lua().RunWithResult(R"LUA(
            local base = getAddress('TestTarget.exe') + 0x1000
            local bytes = readBytes(base, 12, true)
            local parts = {}
            for i, b in ipairs(bytes) do parts[i] = string.format('%02X', b) end
            local pattern = table.concat(parts, ' ')
            local function has(list, address)
              if not list then return false end
              for i = 0, list.Count - 1 do
                if tonumber(list[i], 16) == address then return true end
              end
              return false
            end
            if not has(AOBScan(pattern, '+X'), base) then return 'executable' end
            if has(AOBScan(pattern, '-X'), base) then return 'not executable' end
            if not has(AOBScan(pattern, '+X', 1, '1000'), base) then return 'divisor' end
            if not has(AOBScan(pattern, '+X', 2, '000'), base) then return 'last digits' end
            if has(AOBScan(pattern, '+X', 2, 'FFF'), base) then return 'wrong digits' end
            local line = disassembleBytes(bytes, base)
            local text = disassemble(base)
            if line ~= text then return 'disassembleBytes ' .. tostring(line) .. ' vs ' .. tostring(text) end
            local hexLine = disassembleBytes(pattern, base)
            if hexLine ~= text then return 'hex string ' .. tostring(hexLine) end
            return 'ok')LUA", "aob filters", luaResult);
        CHECK_MSG(lua.Ok() && luaResult == "ok", lua.Ok() ? luaResult : lua.Text);

        lua = engine.Lua().RunWithResult("local s = createStructure('guess test')\n"
                                         "s.autoGuess(getAddress('hero'), 0, 16)\n"
                                         "local e = s.getElementByOffset(4)\n"
                                         "if s.Count < 3 or not e or e.Vartype ~= vtSingle then return 'guess ' .. s.Count end\n"
                                         "local count = s.Count\n"
                                         "s.autoGuess(getAddress('hero'), 4, 4)\n"
                                         "if s.Count ~= count then return 'guessing again changed the count' end\n"
                                         "local b = createStructure('guess offset')\n"
                                         "b.autoGuess(getAddress('hero'), 4, 4)\n"
                                         "if b.Count ~= 1 or b.getElement(0).Offset ~= 4 then return 'offset ' .. b.Count end\n"
                                         "s.destroy()\n"
                                         "b.destroy()\n"
                                         "return 'ok'",
            "autoGuess", luaResult);
        CHECK_MSG(lua.Ok() && luaResult == "ok", lua.Ok() ? luaResult : lua.Text);

        lua = engine.Lua().Run("local t = createTimer(nil, false)\nt.Interval = 50\nticks = 0\nt.OnTimer = function(sender) ticks = ticks + 1 if ticks >= 3 then sender.destroy() end end\nt.Enabled = true\n"
                               "createThread(function() sleep(100) threadDone = true end)", "timers");
        CHECK_MSG(lua.Ok(), lua.Text);
        Pause(engine, 600);
        lua = engine.Lua().RunWithResult("return tostring(ticks) .. '|' .. tostring(threadDone)", "timers", luaResult);
        CHECK_MSG(lua.Ok() && luaResult == "3|true", luaResult);

        lua = engine.Lua().Run("error('expected failure')", "failing");
        CHECK(!lua.Ok() && lua.Code == ErrorCode::LuaRuntime);
        lua = engine.Lua().Run("this is not lua", "broken");
        CHECK(!lua.Ok() && lua.Code == ErrorCode::LuaSyntax);

        CHECK_EQ(engine.Evaluate("TestTarget.g_world").value_or(0), world);
        CHECK_EQ(engine.Evaluate("TestTarget.Damage").value_or(0), damage);
        std::filesystem::path table;
        for (std::filesystem::path at : { targetPath.parent_path(), std::filesystem::current_path() })
        {
            for (int i = 0; i < 8 && table.empty(); ++i, at = at.parent_path())
            {
                if (std::filesystem::exists(at / "Tests" / "Target" / "TestTarget.CT"))
                    table = at / "Tests" / "Target" / "TestTarget.CT";
            }
        }
        if (!table.empty())
        {
            settings.TableScripts = TableScriptPolicy::Never;
            const Status loaded = engine.LoadTable(table, false);
            CHECK_MSG(loaded.Ok(), loaded.Text);
            Table::MemoryRecord* healthRecord = engine.Table().FindByDescription("Health");
            CHECK(healthRecord != nullptr);
            if (healthRecord)
            {
                engine.ResolveRecord(*healthRecord);
                CHECK(healthRecord->AddressOk && healthRecord->ResolvedAddress == health);
                CHECK(healthRecord->Hotkeys.size() == 1);
            }
            Table::MemoryRecord* nameRecord = engine.Table().FindByDescription("Name");
            if (nameRecord)
                CHECK_EQ(engine.FormatRecord(*nameRecord, engine.ReadRecord(*nameRecord)), "ManifoldHero");
            Table::MemoryRecord* ammoRecord = engine.Table().FindByDescription("Ammo");
            if (ammoRecord)
            {
                CHECK(engine.WriteRecord(*ammoRecord, "Full").Ok());
                CHECK_EQ(process.ReadValue<std::int32_t>(hero + 0x20).value_or(0), 30);
            }
            Table::MemoryRecord* heal = engine.Table().FindByDescription("Heal instead of damage");
            CHECK(heal != nullptr);
            if (heal)
            {
                const Status on = engine.SetActive(*heal, true);
                CHECK_MSG(on.Ok(), on.Text);
                const Status off = engine.SetActive(*heal, false);
                CHECK_MSG(off.Ok(), off.Text);
                std::uint8_t back[3]{};
                CHECK(process.ReadExact(damage, back, 3) && back[0] == 0x29 && back[1] == 0x11 && back[2] == 0xC3);
            }
            Table::MemoryRecord* luaRecord = engine.Table().FindByDescription("Lua: print the health");
            if (luaRecord)
            {
                const Status on = engine.SetActive(*luaRecord, true);
                CHECK_MSG(on.Ok(), on.Text);
                const auto last = engine.Lua().Console().Last();
                CHECK(last && last->Text.rfind("Health is ", 0) == 0);
                CHECK(engine.SetActive(*luaRecord, false).Ok());
            }
            const std::string saved = Table::WriteTable(engine.Table(), engine.Structures(), engine.TableSymbols());
            Table::LoadedTable again;
            CHECK(Table::ParseTable(saved, again).Ok() && again.Records.size() == 3);
        }
        else
        {
            Skip("TestTarget.CT not found");
        }

        CheckAttachArmsMainThread(engine, target.Info.hThread, health);
        const Status attached = engine.EnsureDebugger();
        CHECK_MSG(attached.Ok(), attached.Text);
        if (attached)
        {
            const auto recorded = [&engine](int id)
                {
                    Debug::Watch copy;
                    return engine.Debugger().CopyWatch(id, copy) && !copy.Entries.empty();
                };
            int writes = 0;
            CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Writes, health, 4, writes).Ok());
            const auto firstWrite = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!recorded(writes) && std::chrono::steady_clock::now() < firstWrite)
                Pause(engine, 100);
            int calls = 0;
            CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Calls, damage, 1, calls).Ok());
            int accesses = 0;
            CHECK(engine.Debugger().StartWatch(Debug::WatchKind::InstructionAccesses, target.Addresses["readhealth at "], 1, accesses).Ok());
            Pause(engine, 2600);
            const auto patience = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while ((!recorded(calls) || !recorded(accesses)) && std::chrono::steady_clock::now() < patience)
                Pause(engine, 100);
            Debug::Watch watch;
            CHECK(engine.Debugger().CopyWatch(writes, watch));
            CHECK_MSG(!watch.Entries.empty() && watch.Entries.front().Instruction == damage, watch.Entries.empty() ? "no write recorded" : Hex(watch.Entries.front().Instruction));
            CHECK(engine.Debugger().CopyWatch(calls, watch));
            CHECK_MSG(!watch.Entries.empty() && module && module->Contains(watch.Entries.front().Instruction), "no caller recorded");
            if (!watch.Entries.empty())
            {
                const Disasm::Instruction call = engine.Disassembler().Read(process, watch.Entries.front().Instruction, nullptr);
                CHECK_MSG(call.Kind == Disasm::Flow::Call && call.Target == damage, call.Text());
            }
            CHECK(engine.Debugger().CopyWatch(accesses, watch));
            CHECK_MSG(!watch.Entries.empty() && watch.Entries.front().Accessed == health, watch.Entries.empty() ? "no access recorded" : Hex(watch.Entries.front().Accessed));

            engine.Debugger().RemoveWatch(writes);
            engine.Debugger().RemoveWatch(calls);
            engine.Debugger().RemoveWatch(accesses);
            CHECK(engine.Debugger().SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false).Ok());
            bool broke = false;
            for (int i = 0; i < 100 && !broke; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                broke = engine.Debugger().Broken();
            }
            CHECK(broke);
            if (broke)
            {
                const Debug::BreakInfo info = engine.Debugger().Break();
                CHECK_EQ(info.Address, damage);
                CHECK_EQ(info.Registers.Rcx, hero);
                CHECK_EQ(info.Vectors.Count, 16);
                Debug::VectorRegisters vectors = info.Vectors;
                for (std::size_t b = 0; b < 16; ++b)
                    vectors.Xmm[5][b] = static_cast<std::uint8_t>(0x11 + b);
                engine.Debugger().SetVectors(vectors);
                CHECK(engine.Debugger().Break().Vectors.Xmm[5] == vectors.Xmm[5]);
                CHECK(!engine.Debugger().Stepping());
                engine.Debugger().Resume(Debug::ResumeMode::StepInto);
                CHECK(engine.Debugger().Stepping() || engine.Debugger().Broken());
                bool stepped = false;
                for (int i = 0; i < 100 && !stepped; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    const Debug::BreakInfo next = engine.Debugger().Break();
                    stepped = next.Broken && next.Serial != info.Serial;
                }
                CHECK(stepped);
                CHECK(!engine.Debugger().Stepping());
                CHECK_EQ(engine.Debugger().Break().Address, first.Next());
                CHECK_MSG(engine.Debugger().Break().Vectors.Xmm[5] == vectors.Xmm[5], "XMM5 did not keep the value set at the breakpoint");
                std::string xmmText;
                CHECK(engine.Lua().RunWithResult("debug_getContext(true) return tostring(XMM5[1]) .. ',' .. tostring(XMM5[16]) .. ',' .. tostring(XMM15 ~= nil)", "xmm context", xmmText).Ok());
                CHECK_EQ(xmmText, std::string("17,32,true"));
                CHECK(engine.Debugger().RemoveBreakpoint(damage).Ok());
                engine.Debugger().Resume(Debug::ResumeMode::Continue);
            }
            std::uint8_t afterBreakpoints = 0;
            CHECK(process.ReadExact(damage, &afterBreakpoints, 1) && afterBreakpoints == original[0]);
            CheckBreakActions(engine, false);
            CheckFpuAndExceptions(engine, false);
            CheckStepOut(engine, false);
            CheckTraceOneThread(engine, false);
            CheckWritesKeepBreakpoints(engine);
            CheckWatchesOnOneInstruction(engine, false);
            CHECK(process.ReadExact(damage, &afterBreakpoints, 1) && afterBreakpoints == original[0]);
            CheckMono(engine);

            const std::uint64_t shared = engine.Evaluate("TestTarget.g_shared").value_or(0);
            const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
            CHECK(shared != 0 && work != 0);
            std::uint64_t sharedHits = 0;
            for (int round = 0; round < 6 && shared && work; ++round)
            {
                int sharedWrites = 0;
                int sharedAccesses = 0;
                int workCalls = 0;
                CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Writes, shared, 4, sharedWrites).Ok());
                CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Calls, work, 1, workCalls).Ok());
                if (round % 2 == 0)
                    CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Accesses, health, 4, sharedAccesses).Ok());
                Pause(engine, 350);
                Debug::Watch copy;
                if (engine.Debugger().CopyWatch(sharedWrites, copy))
                    sharedHits += copy.Hits;
                engine.Debugger().RemoveWatch(sharedWrites);
                engine.Debugger().RemoveWatch(workCalls);
                if (sharedAccesses)
                    engine.Debugger().RemoveWatch(sharedAccesses);
                Pause(engine, 60);
            }
            CHECK_MSG(sharedHits > 20, std::to_string(sharedHits) + " writes seen");
            CHECK_MSG(process.Alive(), "the target died while watches started and stopped");
            if (!process.Alive())
            {
                for (const LogEntry& entry : RuntimeLog::Entries())
                {
                    if (entry.Area == "Debugger")
                        std::printf("    log: %s\n", entry.Message.c_str());
                }
            }
            engine.Debugger().Detach();
            CHECK(!engine.Debugger().Attached());
            Pause(engine, 600);
            CHECK(process.Alive());
        }

        {
            std::string answer;
            const Status run = engine.Lua().RunWithResult(R"LUA(
                local script = "[ENABLE]\nalloc(compatblock,$100)\nlabel(compatvalue other)\ncompatblock:\ncompatvalue:\n  dd 5\nother:\n  dd 6\nregistersymbol(compatvalue other)\n[DISABLE]\nunregistersymbol(*)\ndealloc(*)\n"
                local ok, info = autoAssemble(script)
                if not ok then return "enable: " .. tostring(info) end
                local address = getAddressSafe("compatvalue")
                if not address or readInteger(address) ~= 5 or readInteger(getAddressSafe("other")) ~= 6 then return "values" end
                if readInteger(address + 0.0) ~= 5 then return "whole float address" end
                local fractional, problem = pcall(readInteger, address + 0.5)
                if fractional or not tostring(problem):find("not a whole number", 1, true) then return "fractional address " .. tostring(problem) end
                if type(info.allocs) ~= "table" or not info.allocs.compatblock then return "allocs" end
                if autoAssemble(script, false, info) ~= true then return "disable" end
                if getAddressSafe("compatvalue") ~= nil or getAddressSafe("other") ~= nil then return "still registered" end
                if readInteger(address) ~= nil then return "not freed" end
                local again, info2 = autoAssemble(script)
                if not again then return "enable again: " .. tostring(info2) end
                local disabled, why = autoAssemble(script, info2)
                if disabled ~= true then return "two-arg disable: " .. tostring(why) end
                if getAddressSafe("compatvalue") ~= nil then return "still registered after the two-arg disable" end
                if autoAssemble(script, true) ~= false then return "targetself accepted" end
                return "ok")LUA", "integration", answer);
            CHECK_MSG(run.Ok() && answer == "ok", run.Ok() ? answer : run.Text);
        }

        CHECK(engine.Symbols().Register("compatkeep", 0x1234, "test").Ok());
        CHECK(engine.OpenProcess(MakeProcessId(target.Info.dwProcessId)).Ok());
        CHECK(engine.Symbols().FindRegistered("compatkeep").has_value());
        CHECK(engine.Process().get() == &process);

        {
            const auto stored = process.ReadValue<std::uint64_t>(world);
            CHECK(stored.has_value());
            Target::Process::SetDryRun(true);
            const std::uint64_t marker = 0xDEADBEEFCAFEF00Dull;
            CHECK(process.Write(world, &marker, sizeof(marker)).Ok());
            CHECK_EQ(process.ReadValue<std::uint64_t>(world).value_or(0), marker);
            std::uint64_t raw = 0;
            SIZE_T done = 0;
            ::ReadProcessMemory(process.Handle(), reinterpret_cast<LPCVOID>(world), &raw, sizeof(raw), &done);
            CHECK_EQ(raw, stored.value_or(1));
            const Result<Address> allocated = process.Allocate(0x100, PAGE_EXECUTE_READWRITE, world);
            CHECK(allocated.Ok());
            const std::uint64_t fake = allocated.ValueOr(0);
            const auto region = process.RegionAt(fake);
            CHECK(!region || region->State != MEM_COMMIT);
            CHECK(process.Free(fake).Ok());
            Target::Process::SetDryRun(false);
            CHECK_EQ(process.ReadValue<std::uint64_t>(world).value_or(0), stored.value_or(1));
        }
        {
            engine.SetAsync(true);
            const auto pump = [&](const std::function<bool()>& done)
                {
                    const auto start = std::chrono::steady_clock::now();
                    while (!done() && std::chrono::steady_clock::now() - start < std::chrono::seconds(15))
                    {
                        engine.Tick();
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    return done();
                };

            bool finished = false;
            Status result;
            std::string output;
            const int task = engine.Lua().StartTask("local s = 0 for i = 1, 3000000 do s = s + i end return tostring(s)", "async sum",
                [&](const Status& status, const std::string& value)
                {
                    result = status;
                    output = value;
                    finished = true;
                });
            CHECK(task > 0);
            int slices = 0;
            while (!finished && slices < 20000)
            {
                engine.Lua().RunTasks(0.002);
                ++slices;
            }
            CHECK(finished);
            CHECK_MSG(result.Ok(), result.Text);
            CHECK_EQ(output, std::string("4500001500000"));
            CHECK_MSG(slices > 1, "the task ran in one go instead of in slices");

            bool stopped = false;
            const int endless = engine.Lua().StartTask("while true do end", "async endless", [&](const Status& status, const std::string&) { stopped = !status.Ok(); });
            engine.Lua().RunTasks(0.002);
            CHECK(engine.Lua().TaskAlive(endless));
            engine.Lua().StopTask(endless);
            for (int i = 0; i < 100 && engine.Lua().TaskAlive(endless); ++i)
                engine.Lua().RunTasks(0.002);
            CHECK(!engine.Lua().TaskAlive(endless));
            CHECK(stopped);

            Table::MemoryRecord& asyncScript = engine.Table().AddScript("async script",
                "[ENABLE]\n{$lua}\nasyncFlag = (asyncFlag or 0) + 1\n{$asm}\nalloc(asyncmem,16)\nregistersymbol(asyncmem)\n[DISABLE]\nunregistersymbol(asyncmem)\ndealloc(asyncmem)\n");
            const int scriptId = asyncScript.Id;
            engine.RequestActive(asyncScript, true);
            CHECK(engine.ActivationPending(scriptId));
            CHECK(!asyncScript.Active);
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK(!engine.ActivationPending(scriptId));
            CHECK_MSG(asyncScript.Active, asyncScript.LastError);
            CHECK(engine.Symbols().FindRegistered("asyncmem").has_value());
            std::string flag;
            CHECK(engine.Lua().RunWithResult("return tostring(asyncFlag)", "async flag", flag).Ok());
            CHECK_EQ(flag, std::string("1"));

            {
                std::string lookups = "[ENABLE]\nalloc(lookupmem,$400)\nlookupmem:\n";
                for (int i = 0; i < 60; ++i)
                    lookups += "dq asyncmem\n";
                lookups += "dq lookupviaresolver\n[DISABLE]\ndealloc(lookupmem)\n";
                // a resolver asked after the program's own names, as the bundled Godot module registers one
                CHECK(engine.Lua().Run("lookupResolver = registerSymbolLookupCallback(function(name) if name == 'lookupviaresolver' then return getAddress('asyncmem') end end, 5)",
                    "lookup resolver").Ok());
                Table::MemoryRecord& lookupScript = engine.Table().AddScript("async lookups", lookups);
                const int lookupId = lookupScript.Id;
                engine.RequestActive(lookupScript, true);
                int ticks = 0;
                while (engine.Working() && ticks < 2000)
                {
                    engine.Tick();
                    ++ticks;
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                }
                Table::MemoryRecord* switched = engine.Table().FindById(lookupId);
                CHECK_MSG(switched && switched->Active, switched ? switched->LastError : std::string("gone"));
                CHECK_MSG(ticks < 10, "60 symbol lookups on the activation worker took " + std::to_string(ticks) + " ticks of the main thread");
                const auto asyncBlock = engine.Symbols().FindRegistered("asyncmem");
                if (switched && switched->Active && asyncBlock)
                {
                    const std::uint64_t block = switched->ScriptMemory.Allocations.empty() ? 0 : switched->ScriptMemory.Allocations.front().Address;
                    CHECK_EQ(process.ReadValue<std::uint64_t>(block + 60 * 8).value_or(0), *asyncBlock);
                }
                if (switched)
                {
                    engine.RequestActive(*switched, false);
                    CHECK(pump([&]() { return !engine.Working(); }));
                }
                engine.Lua().Run("unregisterSymbolLookupCallback(lookupResolver)", "lookup resolver");

                // one asked before the program's own names is still asked from the worker
                CHECK(engine.Lua().Run("lookupFirst = registerSymbolLookupCallback(function(name) if name == 'lookupviafirst' then return getAddress('asyncmem') + 8 end end, 0)",
                    "lookup first").Ok());
                std::string firstLookups = "[ENABLE]\nalloc(firstmem,$400)\nfirstmem:\n";
                for (int i = 0; i < 60; ++i)
                    firstLookups += "dq lookupviafirst\n";
                firstLookups += "[DISABLE]\ndealloc(firstmem)\n";
                Table::MemoryRecord& firstScript = engine.Table().AddScript("async first lookup", firstLookups);
                const int firstId = firstScript.Id;
                engine.RequestActive(firstScript, true);
                int firstTicks = 0;
                while (engine.Working() && firstTicks < 2000)
                {
                    engine.Tick();
                    ++firstTicks;
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                }
                Table::MemoryRecord* firstDone = engine.Table().FindById(firstId);
                CHECK_MSG(firstDone && firstDone->Active, firstDone ? firstDone->LastError : std::string("gone"));
                // each of these lookups has to go to the main thread: the Lua resolver is asked first
                CHECK_MSG(firstTicks < 10, "60 trips of the activation worker to the main thread took " + std::to_string(firstTicks) + " ticks");
                if (firstDone && firstDone->Active && asyncBlock && !firstDone->ScriptMemory.Allocations.empty())
                    CHECK_EQ(process.ReadValue<std::uint64_t>(firstDone->ScriptMemory.Allocations.front().Address).value_or(0), *asyncBlock + 8);
                if (firstDone)
                {
                    engine.RequestActive(*firstDone, false);
                    CHECK(pump([&]() { return !engine.Working(); }));
                }
                engine.Lua().Run("unregisterSymbolLookupCallback(lookupFirst)", "lookup first");
            }

            Table::MemoryRecord& group = engine.Table().AddGroup("async group");
            group.Options.ActivateChildren = true;
            group.Options.DeactivateChildren = true;
            Table::MemoryRecord& child = engine.Table().AddScript("async child", "[ENABLE]\n{$lua}\nasyncChild = 'on'\n{$asm}\n[DISABLE]\n{$lua}\nasyncChild = 'off'\n{$asm}\n", &group);
            const int childId = child.Id;
            engine.RequestActive(group, true);
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK(group.Active);
            CHECK(engine.Table().FindById(childId) && engine.Table().FindById(childId)->Active);

            bool waited = false;
            Status waitResult;
            engine.Lua().StartTask(
                "local list = getAddressList()\n"
                "local record = list:getMemoryRecordByDescription('async script')\n"
                "record.Active = false\n"
                "asyncAfter = tostring(record.Active)\n"
                "return asyncAfter",
                "async wait", [&](const Status& status, const std::string& value)
                {
                    waitResult = status;
                    waited = value == "false";
                });
            CHECK(pump([&]() { return waited || !waitResult.Ok(); }));
            CHECK_MSG(waitResult.Ok(), waitResult.Text);
            CHECK_MSG(waited, "memrec.Active did not wait for the switch");
            CHECK(!engine.Symbols().FindRegistered("asyncmem").has_value());

            engine.RequestActive(group, false);
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK(!group.Active);
            std::string childState;
            CHECK(engine.Lua().RunWithResult("return asyncChild", "async child", childState).Ok());
            CHECK_EQ(childState, std::string("off"));

            const auto orderScript = [](const std::string& name)
                {
                    return "[ENABLE]\n{$lua}\nasyncOrder = (asyncOrder or '') .. '+" + name + "'\n{$asm}\n[DISABLE]\n{$lua}\nasyncOrder = (asyncOrder or '') .. '-" + name + "'\n{$asm}\n";
                };
            Table::MemoryRecord& tree = engine.Table().AddScript("order parent", orderScript("P"));
            tree.Options.ActivateChildren = true;
            tree.Options.DeactivateChildren = true;
            engine.Table().AddScript("order a", orderScript("A"), &tree);
            engine.Table().AddScript("order b", orderScript("B"), &tree);
            engine.Table().AddScript("order c", orderScript("C"), &tree);
            const auto order = [&]()
                {
                    std::string value;
                    engine.Lua().RunWithResult("return asyncOrder", "order", value);
                    return value;
                };
            engine.Lua().Run("asyncOrder = ''", "order reset");
            engine.RequestActive(tree, true);
            CHECK(pump([&]() { return !engine.Working(); }));
            engine.RequestActive(tree, false);
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));

            engine.Lua().Run("asyncOrder = ''", "order reset");
            engine.RequestActive(tree, true);
            CHECK(pump([&]() { return !engine.Working(); }));
            engine.RequestDeactivateAll();
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));

            engine.Lua().Run("asyncOrder = ''", "order reset");
            engine.RequestActive(tree, true);
            CHECK(pump([&]() { return !engine.Working(); }));
            Table::MemoryRecord& restore = engine.Table().AddScript("restore state",
                "[ENABLE]\n{$lua}\nif syntaxcheck then return end\n"
                "local list = getAddressList()\n"
                "for i = list.Count - 1, 0, -1 do\n"
                "  local mr = list:getMemoryRecord(i)\n"
                "  if mr and mr.Active then mr.Active = false end\n"
                "end\n"
                "restoreSeen = tostring(list:getMemoryRecordByDescription('order parent').Active)\n"
                "{$asm}\n[DISABLE]\n");
            const int restoreId = restore.Id;
            engine.RequestActive(restore, true);
            CHECK_MSG(pump([&]() { return !engine.Working(); }), "a script that switches other records from its own Lua never finished");
            CHECK(!engine.ActivationPending(restoreId));
            CHECK_MSG(restore.Active, restore.LastError);
            CHECK(!tree.Active);
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));
            std::string restoreSeen;
            CHECK(engine.Lua().RunWithResult("return restoreSeen", "restore seen", restoreSeen).Ok());
            CHECK_EQ(restoreSeen, std::string("false"));
            engine.SetAsync(false);

            engine.Lua().Run("asyncOrder = ''", "order reset");
            CHECK(engine.SetActive(tree, true).Ok());
            CHECK(engine.SetActive(tree, false).Ok());
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));

            engine.Lua().Run("asyncOrder = ''", "order reset");
            CHECK(engine.SetActive(tree, true).Ok());
            CHECK(engine.DeactivateAll().empty());
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));
        }
        engine.CloseProcess();
        DirectSystemCalls(target.Info.dwProcessId, health);
        CheckIl2CppTarget(targetPath);
        CheckManagedDumperTarget(targetPath);
        CHECK_MSG(UnheldLockNotes() == 0, std::to_string(UnheldLockNotes()) + " calls into the game ran without the Mono lock; the log names the places");
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("integration", "Integration", 120, [](const SuiteContext& context)
        {
            std::printf("  target %ls\n", context.Target.c_str());
            return RunIntegrationTests(context.Target);
        });
}
