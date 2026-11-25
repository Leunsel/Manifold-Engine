#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Debug/BreakAction.h"
#include "Debug/Trace.h"
#include "Lua/LuaEngine.h"
#include "Target/PatchScan.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "Syscalls/Syscall.h"

namespace Manifold::Tests
{
    void CheckCodeCalls(Engine& engine, const std::string& module);

    namespace
    {
        struct Target32
        {
            PROCESS_INFORMATION Info{};
            std::uint64_t World = 0;
            std::uint64_t Hero = 0;
            std::uint64_t Health = 0;
            std::uint64_t Damage = 0;

            Target32() = default;
            Target32(const Target32&) = delete;
            Target32& operator=(const Target32&) = delete;

            ~Target32()
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

        bool Launch32(const std::filesystem::path& path, Target32& target)
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
            const auto take = [&](const char* label) -> std::uint64_t
                {
                    const std::size_t at = text.find(label);
                    if (at == std::string::npos)
                        return 0;
                    const std::size_t start = at + std::strlen(label);
                    std::size_t end = start;
                    while (end < text.size() && IsHexDigit(text[end]))
                        ++end;
                    return ParseHex(text.substr(start, end - start)).value_or(0);
                };
            target.World = take("g_world at ");
            target.Hero = take("hero at ");
            target.Health = take("health at ");
            target.Damage = take("damage at ");
            return target.World && target.Hero && target.Health && target.Damage;
        }

        void Wait(Engine& engine, int milliseconds)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (std::chrono::steady_clock::now() < until)
            {
                engine.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        bool WaitForBreak(Engine& engine, std::uint64_t afterSerial, int milliseconds)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (std::chrono::steady_clock::now() < until)
            {
                const Debug::BreakInfo info = engine.Debugger().Break();
                if (info.Broken && info.Serial != afterSerial)
                    return true;
                engine.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        }

        template <typename Done>
        bool WaitUntil(Engine& engine, int milliseconds, Done done)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (std::chrono::steady_clock::now() < until)
            {
                if (done())
                    return true;
                Wait(engine, 20);
            }
            return done();
        }

        std::uint64_t HitsAt(Engine& engine, std::uint64_t address)
        {
            for (const Debug::Breakpoint& breakpoint : engine.Debugger().Breakpoints())
            {
                if (breakpoint.Address == address)
                    return breakpoint.Hits;
            }
            return 0;
        }

        Debug::BreakAction Action(Engine& engine, const std::string& condition, const std::string& changes, bool stop)
        {
            Debug::BreakAction action;
            action.Condition = condition;
            action.Changes = changes;
            action.Break = stop;
            const Debug::SymbolResolver resolve = [&engine](std::string_view text) { return engine.Evaluate(std::string(text)); };
            const Status compiled = Debug::CompileAction(action, false, resolve);
            CHECK_MSG(compiled.Ok(), compiled.Text);
            return action;
        }

        std::optional<Disasm::Instruction> HealthWriter(Engine& engine, std::uint64_t damage)
        {
            std::uint64_t at = damage;
            for (int i = 0; i < 12; ++i)
            {
                const Disasm::Instruction instruction = engine.Disassembler().Read(*engine.Process(), at, nullptr);
                if (!instruction.Valid || instruction.Kind == Disasm::Flow::Return)
                    break;
                if (instruction.Memory.Present && instruction.Memory.Written && instruction.Memory.Size == 4)
                    return instruction;
                at = instruction.Next();
            }
            return std::nullopt;
        }

        std::string SourceRegister(const Disasm::Instruction& instruction)
        {
            const std::size_t comma = instruction.Operands.rfind(',');
            return Upper(Trim(comma == std::string::npos ? std::string() : instruction.Operands.substr(comma + 1)));
        }

        bool MainThreadWatches(HANDLE thread, std::uint64_t address)
        {
            WOW64_CONTEXT context{};
            context.ContextFlags = WOW64_CONTEXT_DEBUG_REGISTERS;
            if (!::Wow64GetThreadContext(thread, &context))
                return false;
            const DWORD slots[4] = { context.Dr0, context.Dr1, context.Dr2, context.Dr3 };
            for (int slot = 0; slot < 4; ++slot)
            {
                if (slots[slot] == address && (context.Dr7 & (1u << (slot * 2))) != 0)
                    return true;
            }
            return false;
        }

        std::int32_t HealthOf(const Target::Process& process, std::uint64_t health)
        {
            return process.ReadValue<std::int32_t>(health).value_or(0);
        }

        void CheckProcess(Engine& engine, const Target32& target)
        {
            const Target::Process& process = *engine.Process();
            CHECK(!process.Is64Bit());
            CHECK_EQ(process.PointerSize(), 4);
            CHECK_MSG(process.HighestAddress() <= 0xFFFFFFFFull, Hex(process.HighestAddress()));
            const auto exe = process.FindModule("TestTarget32.exe");
            CHECK(exe.has_value());
            if (exe)
                CHECK_EQ(engine.Symbols().ModuleOffset(target.Damage), "\"TestTarget32.exe\"+" + Hex(target.Damage - exe->Base));
            CHECK_EQ(engine.Evaluate("TestTarget32.Damage").value_or(0), target.Damage);
            CHECK_EQ(engine.Evaluate("[" + Hex(target.World) + "]").value_or(0), process.ReadPointer(target.World).value_or(1));
            CHECK_EQ(engine.Evaluate("[[" + Hex(target.World) + "]+4]").value_or(0), target.Hero);
            const auto kernel = process.FindModule("kernel32.dll");
            CHECK(kernel.has_value());
            if (kernel)
            {
                CHECK_MSG(kernel->Base < 0x100000000ull, Hex(kernel->Base));
                CHECK_MSG(Lower(WideToUtf8(kernel->Path)).find("syswow64") != std::string::npos, WideToUtf8(kernel->Path));
            }
            const auto sleep = engine.Evaluate("kernel32.Sleep");
            CHECK(sleep && *sleep < 0x100000000ull);
            const std::uint64_t forwarded = process.FindExport("kernel32.dll", "AcquireSRWLockExclusive");
            const std::uint64_t ntdllTarget = process.FindExport("ntdll.dll", "RtlAcquireSRWLockExclusive");
            CHECK_MSG(forwarded != 0 && forwarded == ntdllTarget, Hex(forwarded) + " vs " + Hex(ntdllTarget));
            for (const char* name : { "Sleep", "WaitForMultipleObjects", "CloseHandle" })
            {
                const std::uint64_t address = process.FindExport("kernel32.dll", name);
                CHECK_MSG(kernel && address >= kernel->Base && address < kernel->Base + kernel->Size, std::string(name) + " at " + Hex(address));
            }
            for (const char* name : { "kernel32.dll", "TestTarget32.exe" })
            {
                const auto module = process.FindModule(name);
                std::vector<Target::CodePatch> patches;
                const Target::ModulePatchSummary summary = module ? Target::ScanModuleForPatches(process, *module, patches) : Target::ModulePatchSummary();
                CHECK_MSG(module && summary.Problem.Ok() && summary.CodeBytes > 0 && patches.empty(),
                    std::string(name) + ": " + summary.Problem.Text + ", " + std::to_string(patches.size()) + " patches" +
                    (patches.empty() ? std::string() : ", first at " + engine.Symbols().ModuleOffset(patches.front().Address)));
            }
            for (const Target::ModuleInfo& module : *process.Modules())
                CHECK_MSG(module.Base + module.Size <= 0x100000000ull, module.Name + " at " + Hex(module.Base));
            const Disasm::Instruction first = engine.Disassembler().Read(process, target.Damage, nullptr);
            CHECK_MSG(first.Valid && !engine.Disassembler().Is64Bit(), first.Text());
            CHECK(WaitUntil(engine, 5000, [&] { return engine.Unreal().State() != UnrealState::Idle && engine.Unreal().State() != UnrealState::Resolving; }));
            CHECK_MSG(engine.Unreal().State() == UnrealState::NotUnreal, "a 32-bit game without Unreal Engine got an Unreal warning");
        }

        std::string HexBytes(const std::uint8_t* bytes, std::size_t size)
        {
            std::string text;
            for (std::size_t i = 0; i < size; ++i)
                text += (i ? " " : "") + Hex(bytes[i], 2);
            return text;
        }

        void CheckTableFeatures(Engine& engine, const Target32& target)
        {
            const Target::Process& process = *engine.Process();
            const std::uint64_t damage = target.Damage;

            Table::MemoryRecord& byAddress = engine.Table().AddValue("Health by pointer", Hex(target.World), Scan::ValueType::FourBytes);
            byAddress.Offsets = { "4", "0" };
            engine.ResolveRecord(byAddress);
            CHECK_MSG(byAddress.AddressOk && byAddress.ResolvedAddress == target.Health, Hex(byAddress.ResolvedAddress));
            Table::MemoryRecord& bySymbol = engine.Table().AddValue("Health by symbol", "TestTarget32.g_world", Scan::ValueType::FourBytes);
            bySymbol.Offsets = { "4", "0" };
            engine.ResolveRecord(bySymbol);
            CHECK_MSG(bySymbol.AddressOk && bySymbol.ResolvedAddress == target.Health, Hex(bySymbol.ResolvedAddress));
            Table::MemoryRecord& heroRecord = engine.Table().AddValue("Hero", "[[TestTarget32.g_world]+4]", Scan::ValueType::FourBytes);
            engine.ResolveRecord(heroRecord);
            CHECK_MSG(heroRecord.AddressOk && heroRecord.ResolvedAddress == target.Health, Hex(heroRecord.ResolvedAddress));

            std::uint8_t code[24]{};
            CHECK(process.ReadExact(damage, code, sizeof(code)));
            std::size_t length = 0;
            for (std::uint64_t at = damage; at < damage + sizeof(code);)
            {
                const Disasm::Instruction instruction = engine.Disassembler().Read(process, at, nullptr);
                at = instruction.Next();
                length = static_cast<std::size_t>(at - damage);
                if (instruction.Kind == Disasm::Flow::Return || !instruction.Valid)
                    break;
            }
            CHECK_MSG(length >= 6 && length <= sizeof(code), std::to_string(length));
            const std::string pattern = HexBytes(code, std::min<std::size_t>(length, sizeof(code)));

            std::string answer;
            Status lua = engine.Lua().RunWithResult("local world = getAddress('TestTarget32.g_world')\n"
                                                    "local hero = readPointer(readPointer(world) + 4)\n"
                                                    "local scratch = allocateMemory(16)\n"
                                                    "writeQword(scratch, -1)\n"
                                                    "writePointer(scratch, 0x12345678)\n"
                                                    "local written = readQword(scratch) or 0\n"
                                                    "local unique = AOBScanModuleUnique('TestTarget32.exe', '" + pattern + "')\n"
                                                    "local guess = createStructure('world32')\n"
                                                    "guess.autoGuess(readPointer(world), 0, 8)\n"
                                                    "local element = guess.getElementByOffset(4)\n"
                                                    "local pointerOk = element ~= nil and element.Vartype == vtPointer and element.Bytesize == 4\n"
                                                    "local added = guess.addElement()\n"
                                                    "added.Offset = 0x20\n"
                                                    "added.Vartype = vtPointer\n"
                                                    "pointerOk = pointerOk and added.Bytesize == 4\n"
                                                    "guess.destroy()\n"
                                                    "deAlloc(scratch)\n"
                                                    "return string.format('%s|%d|%X|%X|%X|%X|%s|%s|%X', tostring(targetIs64Bit()), getPointerSize(), hero, readPointer('[TestTarget32.g_world]+4'),"
                                                    " written, unique or 0, tostring(pointerOk), getNameFromAddress(" + std::to_string(damage) + "), getAddress('kernel32.Sleep'))",
                "32-bit lua", answer);
            CHECK_MSG(lua.Ok(), lua.Text);
            const auto parts = Split(answer, '|', true);
            CHECK_MSG(parts.size() == 9, answer);
            if (parts.size() == 9)
            {
                CHECK_MSG(parts[0] == "false" && parts[1] == "4", answer);
                CHECK_MSG(ParseHex(parts[2]).value_or(0) == target.Hero && ParseHex(parts[3]).value_or(0) == target.Hero, answer);
                CHECK_MSG(parts[4] == "FFFFFFFF12345678", "writePointer wrote more than 4 bytes: " + answer);
                CHECK_MSG(ParseHex(parts[5]).value_or(0) == damage, "AOBScanModuleUnique: " + answer);
                CHECK_MSG(parts[6] == "true", "the structure guess or a vtPointer element is not 4 bytes: " + answer);
                CHECK_MSG(parts[7].find("Damage") != std::string::npos, answer);
                CHECK_MSG(ParseHex(parts[8]).value_or(0) == engine.Evaluate("kernel32.Sleep").value_or(1), answer);
            }
            CheckCodeCalls(engine, "TestTarget32");

            const std::int32_t start = HealthOf(process, target.Health);
            Table::MemoryRecord& script = engine.Table().AddScript("Heal instead of damage (32-bit)",
                "[ENABLE]\n"
                "aobscanmodule(damage32,TestTarget32.exe," + pattern + ")\n"
                "alloc(newmem32,$1000,damage32)\n"
                "label(return32 heroPtr32)\n"
                "registersymbol(damage32 heroPtr32)\n"
                "newmem32:\n"
                "  push ecx\n"
                "  mov ecx,[esp+8]\n"
                "  mov eax,[esp+C]\n"
                "  cmp eax,(int)0\n"
                "  je @f\n"
                "  add [ecx],eax\n"
                "@@:\n"
                "  mov [heroPtr32],ecx\n"
                "  pop ecx\n"
                "  jmp return32\n"
                "heroPtr32:\n"
                "  dd 0\n"
                "  dq 1122334455667788\n"
                "damage32:\n"
                "  jmp newmem32\n"
                "return32:\n"
                "  ret\n"
                "[DISABLE]\n"
                "damage32:\n"
                "  db " + HexBytes(code, 6) + "\n"
                "unregistersymbol(*)\n"
                "dealloc(*)\n");
            const Status enabled = engine.SetActive(script, true);
            CHECK_MSG(enabled.Ok(), enabled.Text);
            if (enabled)
            {
                const Disasm::Instruction jump = engine.Disassembler().Read(process, damage, nullptr);
                const auto heroPtr = engine.Symbols().FindRegistered("heroPtr32");
                CHECK_MSG(jump.Kind == Disasm::Flow::Jump && jump.Length == 5 && jump.Target && heroPtr && *jump.Target < *heroPtr && *heroPtr - *jump.Target < 0x40,
                    jump.Text());
                CHECK(WaitUntil(engine, 3000, [&] { return HealthOf(process, target.Health) > start; }));
                CHECK_EQ(heroPtr ? process.ReadPointer(*heroPtr).value_or(0) : 0, target.Hero);
                const auto exe = process.FindModule("TestTarget32.exe");
                std::vector<Target::CodePatch> patches;
                const Target::ModulePatchSummary summary = exe ? Target::ScanModuleForPatches(process, *exe, patches) : Target::ModulePatchSummary();
                CHECK_MSG(exe && summary.Problem.Ok() && patches.size() == 1 && patches.front().Address == damage && patches.front().Current[0] == 0xE9,
                    summary.Problem.Text + ", " + std::to_string(patches.size()) + " patches");
                CHECK_EQ(heroPtr ? process.ReadValue<std::uint64_t>(*heroPtr + 4).value_or(0) : 0, 0x1122334455667788ull);
                const Status disabled = engine.SetActive(script, false);
                CHECK_MSG(disabled.Ok(), disabled.Text);
                std::uint8_t restored[6]{};
                CHECK(process.ReadExact(damage, restored, sizeof(restored)) && std::memcmp(restored, code, sizeof(restored)) == 0);
                CHECK(!engine.Symbols().FindRegistered("heroPtr32").has_value());
            }
            const std::int32_t before = HealthOf(process, target.Health);
            CHECK(WaitUntil(engine, 2500, [&] { return HealthOf(process, target.Health) < before; }));
        }

        void CheckWindowsDebugger(Engine& engine, const Target32& target)
        {
            const Target::Process& process = *engine.Process();
            Debug::Debugger& debugger = engine.Debugger();
            const std::uint64_t damage = target.Damage;
            const std::uint64_t health = target.Health;
            const auto writer = HealthWriter(engine, damage);
            CHECK_MSG(writer.has_value(), "no instruction in Damage writes the health");
            if (!writer)
                return;
            const Disasm::Instruction first = engine.Disassembler().Read(process, damage, nullptr);
            std::uint8_t original = 0;
            CHECK(process.ReadExact(damage, &original, 1) && original != 0xCC);

            const Status attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok(), attached.Text);
            if (!attached)
                return;

            {
                int writes = 0;
                const Status started = debugger.StartWatch(Debug::WatchKind::Writes, health, 4, writes);
                CHECK_MSG(started.Ok(), started.Text);
                CHECK_MSG(MainThreadWatches(target.Info.hThread, health), "windows: the watch left the main thread out");
                Debug::Watch watch;
                const bool recorded = WaitUntil(engine, 3500, [&] { return debugger.CopyWatch(writes, watch) && !watch.Entries.empty(); });
                CHECK_MSG(recorded, "windows: no write to the health recorded");
                if (recorded)
                {
                    CHECK_MSG(watch.Entries.size() == 1 && watch.Entries.front().Instruction == writer->Address,
                        "windows: " + std::to_string(watch.Entries.size()) + " writers, first " + Hex(watch.Entries.front().Instruction) + ", expected " + Hex(writer->Address));
                    const Disasm::RegisterValues& registers = watch.Entries.front().Registers;
                    CHECK_MSG(registers.Rip == writer->Next() && registers.Rsp != 0 && registers.Rsp < 0x100000000ull && registers.FsBase != 0,
                        "windows: eip " + Hex(registers.Rip) + ", esp " + Hex(registers.Rsp) + ", fs " + Hex(registers.FsBase));
                }
                debugger.RemoveWatch(writes);
            }

            {
                int accesses = 0;
                CHECK(debugger.StartWatch(Debug::WatchKind::Accesses, health, 4, accesses).Ok());
                Debug::Watch watch;
                const bool both = WaitUntil(engine, 4000, [&] { return debugger.CopyWatch(accesses, watch) && watch.Entries.size() >= 2; });
                CHECK_MSG(both, "windows: " + std::to_string(watch.Entries.size()) + " instructions accessed the health");
                bool sawWriter = false;
                for (const Debug::WatchEntry& entry : watch.Entries)
                    sawWriter = sawWriter || entry.Instruction == writer->Address;
                CHECK_MSG(sawWriter, "windows: the accesses watch missed the writer");
                debugger.RemoveWatch(accesses);
            }

            {
                int calls = 0;
                CHECK(debugger.StartWatch(Debug::WatchKind::Calls, damage, 1, calls).Ok());
                Debug::Watch watch;
                const bool called = WaitUntil(engine, 3500, [&] { return debugger.CopyWatch(calls, watch) && !watch.Entries.empty(); });
                CHECK_MSG(called, "windows: the calls watch saw no call of Damage");
                if (called)
                {
                    const Disasm::Instruction call = engine.Disassembler().Read(process, watch.Entries.front().Instruction, nullptr);
                    CHECK_MSG(call.Kind == Disasm::Flow::Call && call.Target == damage && call.Next() == watch.Entries.front().Accessed,
                        "windows: the caller is " + call.Text() + " at " + Hex(call.Address));
                }
                debugger.RemoveWatch(calls);
                std::uint8_t now = 0;
                CHECK(WaitUntil(engine, 500, [&] { return process.ReadExact(damage, &now, 1) && now == original; }));
            }

            {
                const Status set = debugger.SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false);
                CHECK_MSG(set.Ok(), set.Text);
                const bool broke = set && WaitForBreak(engine, 0, 4000);
                CHECK_MSG(broke, "windows: the int3 at Damage did not stop");
                if (broke)
                {
                    const Debug::BreakInfo info = debugger.Break();
                    CHECK_MSG(info.Address == damage && info.Registers.Rip == damage && !info.Is64Bit && info.Reason == "Breakpoint",
                        info.Reason + " at " + Hex(info.Address));
                    CHECK_MSG(info.Registers.Rsp != 0 && info.Registers.Rsp < 0x100000000ull && info.Registers.FsBase != 0 && info.Registers.R8 == 0,
                        "esp " + Hex(info.Registers.Rsp) + ", fs " + Hex(info.Registers.FsBase));
                    CHECK_EQ(process.ReadPointer(info.Registers.Rsp + 4).value_or(0), target.Hero);
                    CHECK_EQ(process.ReadValue<std::int32_t>(info.Registers.Rsp + 8).value_or(0), 1);
                    CHECK_MSG(info.Vectors.Count == 8, std::to_string(info.Vectors.Count) + " vector registers");
                    const std::uint64_t returnAddress = process.ReadPointer(info.Registers.Rsp).value_or(0);

                    debugger.Resume(Debug::ResumeMode::StepInto);
                    const bool stepped = WaitForBreak(engine, info.Serial, 3000);
                    CHECK_MSG(stepped, "windows: step into did not stop");
                    const Debug::BreakInfo step = debugger.Break();
                    CHECK_MSG(stepped && step.Reason == "Step" && step.Address == first.Next(), step.Reason + " at " + Hex(step.Address) + ", expected " + Hex(first.Next()));

                    debugger.Resume(Debug::ResumeMode::RunToReturn);
                    const bool returned = stepped && WaitForBreak(engine, step.Serial, 3000);
                    CHECK_MSG(returned, "windows: run to return did not stop");
                    const Debug::BreakInfo back = debugger.Break();
                    CHECK_MSG(returned && back.Reason.find("Returned") == 0 && back.Address == returnAddress, back.Reason + " at " + Hex(back.Address) + ", expected " + Hex(returnAddress));

                    debugger.Resume(Debug::ResumeMode::StepOver);
                    const bool over = returned && WaitForBreak(engine, back.Serial, 3000);
                    CHECK_MSG(over, "windows: step over did not stop");
                    const Debug::BreakInfo overInfo = debugger.Break();
                    const Disasm::Instruction after = engine.Disassembler().Read(process, back.Address, nullptr);
                    CHECK_MSG(over && overInfo.Address == after.Next(), overInfo.Reason + " at " + Hex(overInfo.Address) + " after " + after.Text());
                }
                CHECK(debugger.RemoveBreakpoint(damage).Ok());
                if (debugger.Broken())
                    debugger.Resume(Debug::ResumeMode::Continue);
                Wait(engine, 50);
                std::uint8_t now = 0;
                CHECK(process.ReadExact(damage, &now, 1) && now == original);
            }

            {
                int accessed = 0;
                CHECK(debugger.StartWatch(Debug::WatchKind::InstructionAccesses, writer->Address, 1, accessed).Ok());
                Debug::Watch watch;
                const bool seen = WaitUntil(engine, 3500, [&] { return debugger.CopyWatch(accessed, watch) && !watch.Entries.empty(); });
                CHECK_MSG(seen && watch.Entries.front().Accessed == health, "windows: the writer accessed " + (seen ? Hex(watch.Entries.front().Accessed) : std::string("nothing")));
                debugger.RemoveWatch(accessed);
            }

            {
                Debug::TraceRequest request;
                request.Address = damage;
                request.Count = 20;
                Debug::Tracer& tracer = engine.Tracer();
                const Status started = tracer.Start(engine.Process(), engine.Disassembler(), request);
                CHECK_MSG(started.Ok(), started.Text);
                CHECK(started && WaitUntil(engine, 5000, [&] { return !tracer.Running(); }));
                const std::vector<Debug::TraceStep> steps = tracer.Steps();
                CHECK_MSG(steps.size() == 20 && steps.front().Address == damage, std::to_string(steps.size()) + " steps, " + tracer.StatusText());
                bool wrote = false;
                bool returned = false;
                for (std::size_t i = 0; i + 1 < steps.size(); ++i)
                {
                    wrote = wrote || steps[i].Address == writer->Address;
                    returned = returned || (steps[i].Kind == Disasm::Flow::Return && steps[i].Address > damage && steps[i].Address < damage + 0x40 &&
                                            (steps[i + 1].Address < damage || steps[i + 1].Address >= damage + 0x40) && steps[i + 1].Address < 0x100000000ull);
                }
                CHECK_MSG(wrote && returned, "windows: the trace did not run through Damage and back to its caller");
                CHECK(!debugger.Broken() && !debugger.HasBreakpoint(damage));

                int leftBroken = 0;
                int notStarted = 0;
                request.Count = 3;
                for (int round = 0; round < 25; ++round)
                {
                    if (!tracer.Start(engine.Process(), engine.Disassembler(), request))
                    {
                        ++notStarted;
                        continue;
                    }
                    WaitUntil(engine, 5000, [&] { return !tracer.Running(); });
                    leftBroken += debugger.Broken() || debugger.HasBreakpoint(damage) ? 1 : 0;
                }
                CHECK_MSG(leftBroken == 0 && notStarted == 0, "windows: of 25 traces " + std::to_string(leftBroken) + " ended with the process still stopped and " + std::to_string(notStarted) + " did not start");
            }

            {
                const Status set = debugger.SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, true);
                CHECK_MSG(set.Ok(), set.Text);
                const bool broke = set && WaitForBreak(engine, 0, 4000);
                CHECK_MSG(broke, "windows: the hardware breakpoint at Damage did not stop");
                const Debug::BreakInfo info = debugger.Break();
                CHECK_MSG(broke && info.Address == damage && info.Reason == "Hardware breakpoint", info.Reason + " at " + Hex(info.Address));
                if (broke)
                    debugger.Resume(Debug::ResumeMode::Continue);
                const bool again = broke && WaitForBreak(engine, info.Serial, 4000);
                CHECK_MSG(again, "windows: the hardware breakpoint did not stop a second time");
                CHECK(debugger.RemoveBreakpoint(damage).Ok());
                if (debugger.Broken())
                    debugger.Resume(Debug::ResumeMode::Continue);
            }

            {
                const std::uint64_t serial = debugger.Break().Serial;
                const Status set = debugger.SetBreakpoint(health, Debug::BreakpointType::Write, 4, true);
                CHECK_MSG(set.Ok(), set.Text);
                const bool broke = set && WaitForBreak(engine, serial, 4000);
                CHECK_MSG(broke, "windows: the write breakpoint did not stop");
                const Debug::BreakInfo info = debugger.Break();
                CHECK_MSG(broke && info.Reason.find("Write") != std::string::npos && info.BreakpointAddress == health && info.Address == writer->Next(),
                    info.Reason + " at " + Hex(info.Address) + ", expected " + Hex(writer->Next()));
                CHECK(debugger.RemoveBreakpoint(health).Ok());
                if (broke)
                    debugger.Resume(Debug::ResumeMode::Continue);
            }

            {
                const Status set = debugger.SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false, Action(engine, "readInteger(ESP+8) == 2", {}, true));
                CHECK_MSG(set.Ok(), set.Text);
                Wait(engine, 2300);
                CHECK_MSG(!debugger.Broken(), "windows: stopped although amount is never 2");
                CHECK_EQ(HitsAt(engine, damage), 0ull);
                CHECK(debugger.RemoveBreakpoint(damage).Ok());
                if (debugger.Broken())
                    debugger.Resume(Debug::ResumeMode::Continue);
                const std::uint64_t serial = debugger.Break().Serial;
                CHECK(debugger.SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false, Action(engine, "readInteger(ESP+8) == 1 and readPointer(ESP+4) == 0x" + Hex(target.Hero), {}, true)).Ok());
                const bool broke = WaitForBreak(engine, serial, 4000);
                CHECK_MSG(broke, "windows: the 32-bit condition never held");
                CHECK(debugger.RemoveBreakpoint(damage).Ok());
                if (broke)
                    debugger.Resume(Debug::ResumeMode::Continue);
            }

            const std::string source = SourceRegister(*writer);
            CHECK_MSG(source.size() == 3 && source[0] == 'E', writer->Text());
            if (source.size() == 3 && source[0] == 'E')
            {
                const Status set = debugger.SetBreakpoint(writer->Address, Debug::BreakpointType::Execute, 1, false, Action(engine, {}, source + " = 0", false));
                CHECK_MSG(set.Ok(), set.Text);
                CHECK(WaitUntil(engine, 4000, [&] { return HitsAt(engine, writer->Address) >= 1; }));
                const std::int32_t before = HealthOf(process, health);
                Wait(engine, 2300);
                CHECK_MSG(HealthOf(process, health) == before && !debugger.Broken(), "windows: health went from " + std::to_string(before) + " to " + std::to_string(HealthOf(process, health)));
                CHECK(debugger.RemoveBreakpoint(writer->Address).Ok());
                if (debugger.Broken())
                    debugger.Resume(Debug::ResumeMode::Continue);
            }

            {
                const std::uint64_t serial = debugger.Break().Serial;
                CHECK(debugger.BreakNow().Ok());
                const bool broke = WaitForBreak(engine, serial, 3000);
                CHECK_MSG(broke, "windows: Break did not stop the target");
                const Debug::BreakInfo info = debugger.Break();
                CHECK_MSG(broke && info.Address != 0 && info.Address < 0x100000000ull && info.Registers.Rsp < 0x100000000ull,
                    info.Reason + " at " + Hex(info.Address) + " in thread " + std::to_string(info.ThreadId));
                if (broke)
                {
                    debugger.Resume(Debug::ResumeMode::StepInto);
                    const bool stepped = WaitForBreak(engine, info.Serial, 3000);
                    CHECK_MSG(stepped && debugger.Break().ThreadId == info.ThreadId, "windows: a step after Break did not stop in the same thread");
                    if (debugger.Broken())
                        debugger.Resume(Debug::ResumeMode::Continue);
                }
            }

            debugger.Detach();
            Wait(engine, 300);
            CHECK(process.Alive());
            std::uint8_t now = 0;
            CHECK(process.ReadExact(damage, &now, 1) && now == original);
            CHECK_MSG(!MainThreadWatches(target.Info.hThread, health), "windows: the main thread still watches after the detach");
        }

        void CheckInProcessDebugger(Engine& engine, const Target32& target)
        {
            const Target::Process& process = *engine.Process();
            Debug::Debugger& debugger = engine.Debugger();
            const std::uint64_t damage = target.Damage;
            const std::uint64_t health = target.Health;
            const auto writer = HealthWriter(engine, damage);
            CHECK(writer.has_value());
            if (!writer)
                return;
            const Disasm::Instruction first = engine.Disassembler().Read(process, damage, nullptr);

            const Status attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok(), attached.Text);
            if (!attached)
                return;
            CHECK(debugger.UsingVeh());
            engine.Process()->RefreshModules();
            CHECK(process.FindModule("ManifoldAgent32.dll").has_value());

            {
                int writes = 0;
                CHECK(debugger.StartWatch(Debug::WatchKind::Writes, health, 4, writes).Ok());
                Debug::Watch watch;
                const bool recorded = WaitUntil(engine, 3500, [&] { return debugger.CopyWatch(writes, watch) && !watch.Entries.empty(); });
                CHECK_MSG(recorded, "in-process: no write to the health recorded");
                if (recorded)
                {
                    const Debug::WatchEntry& entry = watch.Entries.front();
                    CHECK_MSG(watch.Entries.size() == 1 && entry.Instruction == writer->Address, "in-process: first writer " + Hex(entry.Instruction) + ", expected " + Hex(writer->Address));
                    CHECK_MSG(entry.Registers.Rip == writer->Next() && entry.Registers.FsBase != 0 && entry.Registers.GsBase == 0 && entry.Registers.Rsp < 0x100000000ull,
                        "in-process: eip " + Hex(entry.Registers.Rip) + ", fs " + Hex(entry.Registers.FsBase));
                }
                debugger.RemoveWatch(writes);
            }

            {
                int calls = 0;
                CHECK(debugger.StartWatch(Debug::WatchKind::Calls, damage, 1, calls).Ok());
                Debug::Watch watch;
                const bool called = WaitUntil(engine, 3500, [&] { return debugger.CopyWatch(calls, watch) && !watch.Entries.empty(); });
                CHECK_MSG(called, "in-process: the calls watch saw no call of Damage");
                if (called)
                {
                    const Disasm::Instruction call = engine.Disassembler().Read(process, watch.Entries.front().Instruction, nullptr);
                    CHECK_MSG(call.Kind == Disasm::Flow::Call && call.Target == damage && call.Next() == watch.Entries.front().Accessed,
                        "in-process: the caller is " + call.Text() + " at " + Hex(call.Address));
                }
                debugger.RemoveWatch(calls);
            }

            {
                int accessed = 0;
                CHECK(debugger.StartWatch(Debug::WatchKind::InstructionAccesses, writer->Address, 1, accessed).Ok());
                Debug::Watch watch;
                const bool seen = WaitUntil(engine, 3500, [&] { return debugger.CopyWatch(accessed, watch) && !watch.Entries.empty(); });
                CHECK_MSG(seen && watch.Entries.front().Accessed == health, "in-process: the writer accessed " + (seen ? Hex(watch.Entries.front().Accessed) : std::string("nothing")));
                debugger.RemoveWatch(accessed);
            }

            {
                const Status set = debugger.SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false);
                CHECK_MSG(set.Ok(), set.Text);
                const bool broke = set && WaitForBreak(engine, 0, 4000);
                CHECK_MSG(broke, "in-process: the breakpoint at Damage did not stop");
                if (broke)
                {
                    const Debug::BreakInfo info = debugger.Break();
                    CHECK_MSG(info.Address == damage && !info.Is64Bit && info.Registers.Rsp < 0x100000000ull && info.Registers.FsBase != 0, Hex(info.Address));
                    CHECK_EQ(process.ReadPointer(info.Registers.Rsp + 4).value_or(0), target.Hero);
                    CHECK_MSG(info.Vectors.Count == 8, std::to_string(info.Vectors.Count) + " vector registers");
                    debugger.Resume(Debug::ResumeMode::StepInto);
                    const bool stepped = WaitForBreak(engine, info.Serial, 3000);
                    const Debug::BreakInfo step = debugger.Break();
                    CHECK_MSG(stepped && step.Reason == "Step" && step.Address == first.Next(), step.Reason + " at " + Hex(step.Address) + ", expected " + Hex(first.Next()));
                }
                CHECK(debugger.RemoveBreakpoint(damage).Ok());
                if (debugger.Broken())
                    debugger.Resume(Debug::ResumeMode::Continue);
                Wait(engine, 100);
            }

            {
                const Status set = debugger.SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false, Action(engine, "readInteger(ESP+8) == 2", {}, true));
                CHECK_MSG(set.Ok(), set.Text);
                Wait(engine, 2300);
                CHECK_MSG(!debugger.Broken(), "in-process: stopped although amount is never 2");
                CHECK(debugger.RemoveBreakpoint(damage).Ok());
                if (debugger.Broken())
                    debugger.Resume(Debug::ResumeMode::Continue);
            }

            {
                const std::uint64_t serial = debugger.Break().Serial;
                const Status set = debugger.SetBreakpoint(health, Debug::BreakpointType::Write, 4, true, Action(engine, "readInteger(0x" + Hex(health) + ") % 2 == 0", {}, true));
                CHECK_MSG(set.Ok(), set.Text);
                const bool broke = set && WaitForBreak(engine, serial, 5000);
                CHECK_MSG(broke, "in-process: the write breakpoint did not stop");
                const Debug::BreakInfo info = debugger.Break();
                CHECK_MSG(broke && info.Reason.find("Write") != std::string::npos && info.Address == writer->Next() && HealthOf(process, health) % 2 == 0,
                    info.Reason + " at " + Hex(info.Address) + ", health " + std::to_string(HealthOf(process, health)));
                CHECK(debugger.RemoveBreakpoint(health).Ok());
                if (broke)
                    debugger.Resume(Debug::ResumeMode::Continue);
            }

            debugger.Detach();
            Wait(engine, 400);
            CHECK(process.Alive());
            const std::int32_t before = HealthOf(process, health);
            Wait(engine, 1300);
            CHECK_MSG(HealthOf(process, health) < before, "in-process: Damage stopped working after the detach");
        }
    }

    void CheckMono(Engine& engine);
    void CheckIl2CppTarget(const std::filesystem::path& targetPath);
    void CheckManagedDumperTarget(const std::filesystem::path& targetPath);

    namespace
    {
        void CheckManaged32(const std::filesystem::path& targetPath)
        {
            {
                Target32 target;
                const bool started = Launch32(targetPath, target);
                CHECK_MSG(started, "could not start the 32-bit test target for Mono");
                if (started)
                {
                    Settings settings;
                    Engine engine(settings);
                    const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
                    CHECK_MSG(opened.Ok(), opened.Text);
                    if (opened)
                    {
                        CHECK(!engine.Process()->Is64Bit());
                        CheckMono(engine);
                    }
                    engine.CloseProcess();
                    DWORD code = 0;
                    ::GetExitCodeProcess(target.Info.hProcess, &code);
                    CHECK_MSG(code == STILL_ACTIVE, "the 32-bit Mono target ended with " + Hex(code, 8));
                }
            }
            CheckIl2CppTarget(targetPath);
            CheckManagedDumperTarget(targetPath);
        }
    }

    int RunWow64Tests(const std::filesystem::path& targetPath)
    {
        const int before = Totals().Failures;
        std::error_code missing;
        if (!std::filesystem::exists(targetPath, missing))
        {
            Skip("the 32-bit test target " + targetPath.string() + " was not built");
            return 0;
        }
        InitSysCalls();
        Target32 target;
        if (!Launch32(targetPath, target))
        {
            Skip("the 32-bit test target " + targetPath.string() + " could not be started");
            return 0;
        }
        RuntimeLog::SetVerbose(true);
        {
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (opened)
            {
                CheckProcess(engine, target);
                CheckTableFeatures(engine, target);
                CheckWindowsDebugger(engine, target);
            }
        }
        {
            Settings settings;
            settings.UseInProcessDebugger = true;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (opened)
                CheckInProcessDebugger(engine, target);
        }
        DWORD code = 0;
        ::GetExitCodeProcess(target.Info.hProcess, &code);
        CHECK_MSG(code == STILL_ACTIVE, "the 32-bit target ended with " + Hex(code, 8));
        CheckManaged32(targetPath);
        return Totals().Failures - before;
    }

    int RunWow64ManagedCheck(const std::vector<std::string>&)
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        InitSysCalls();
        const int before = Totals().Failures;
        CheckManaged32(std::filesystem::path(self).parent_path() / L"TestTarget32.exe");
        const int failures = Totals().Failures - before;
        std::printf("wow64 managed check: %d checks, %d failed\n", Totals().Checks, failures);
        return failures == 0 ? 0 : 1;
    }

    MANIFOLD_MODE("--wow64-managed-check", &RunWow64ManagedCheck);

    MANIFOLD_SUITE("wow64", "32-bit target", 125, [](const SuiteContext& context)
        {
            const std::filesystem::path target = context.Target.parent_path() / L"TestTarget32.exe";
            std::printf("  target %ls\n", target.c_str());
            return RunWow64Tests(target);
        });
}
