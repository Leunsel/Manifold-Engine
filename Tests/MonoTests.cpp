#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Target/Mono.h"
#include "Target/Process.h"
#include "Target/RemoteCall.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

namespace Manifold::Tests
{
    namespace
    {
        // A RemoteCall worker thread starts with push rsi; push rdi; mov rsi, rcx;
        // mov rdi, rsp; and rsp, -16. Finding workers this way only reads.
        constexpr std::uint8_t kWorkerStart[] = { 0x56, 0x57, 0x48, 0x89, 0xCE, 0x48, 0x89, 0xE7, 0x48, 0x83, 0xE4, 0xF0 };

        struct WorkerThread
        {
            std::uint32_t Id = 0;
            std::uint64_t Start = 0;
            double CpuSeconds = 0;
        };

        double ThreadCpuSeconds(HANDLE thread)
        {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!::GetThreadTimes(thread, &created, &exited, &kernel, &user))
                return 0;
            const auto ticks = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
            return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
        }

        std::vector<WorkerThread> FindWorkers(std::uint32_t pid)
        {
            using QueryThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
            static const auto query = reinterpret_cast<QueryThread>(
                reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
            std::vector<WorkerThread> workers;
            HANDLE process = ::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (process && query && snapshot != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 entry{ sizeof(entry) };
                for (BOOL more = ::Thread32First(snapshot, &entry); more; more = ::Thread32Next(snapshot, &entry))
                {
                    if (entry.th32OwnerProcessID != pid)
                        continue;
                    HANDLE thread = ::OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                    if (!thread)
                        continue;
                    std::uint64_t start = 0;
                    std::uint8_t bytes[sizeof(kWorkerStart)] = {};
                    SIZE_T got = 0;
                    if (query(thread, 9, &start, sizeof(start), nullptr) >= 0 && start &&
                        ::ReadProcessMemory(process, reinterpret_cast<LPCVOID>(start), bytes, sizeof(bytes), &got) && got == sizeof(bytes) &&
                        std::memcmp(bytes, kWorkerStart, sizeof(bytes)) == 0)
                        workers.push_back({ entry.th32ThreadID, start, ThreadCpuSeconds(thread) });
                    ::CloseHandle(thread);
                }
            }
            if (snapshot != INVALID_HANDLE_VALUE)
                ::CloseHandle(snapshot);
            if (process)
                ::CloseHandle(process);
            return workers;
        }

        // The workers in `after` that were not in `before`.
        std::vector<WorkerThread> NewWorkers(const std::vector<WorkerThread>& before, const std::vector<WorkerThread>& after)
        {
            std::vector<WorkerThread> added;
            for (const WorkerThread& worker : after)
            {
                if (std::none_of(before.begin(), before.end(), [&](const WorkerThread& old) { return old.Id == worker.Id; }))
                    added.push_back(worker);
            }
            return added;
        }

        std::uint32_t ProcessHandleCount(std::uint32_t pid)
        {
            DWORD count = 0;
            if (HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                ::GetProcessHandleCount(process, &count);
                ::CloseHandle(process);
            }
            return count;
        }

        struct ChildHarness
        {
            PROCESS_INFORMATION Info{};
            HANDLE Output = nullptr;
            std::string Text;

            ~ChildHarness()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 1);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
                if (Output)
                    ::CloseHandle(Output);
            }

            bool Start(const std::wstring& arguments)
            {
                wchar_t self[MAX_PATH] = L"";
                ::GetModuleFileNameW(nullptr, self, MAX_PATH);
                SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
                HANDLE write = nullptr;
                if (!::CreatePipe(&Output, &write, &security, 0))
                    return false;
                ::SetHandleInformation(Output, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW startup{ sizeof(startup) };
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdOutput = write;
                startup.hStdError = write;
                std::wstring command = L"\"" + std::wstring(self) + L"\" " + arguments;
                const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &Info);
                ::CloseHandle(write);
                return started != FALSE;
            }

            bool WaitFor(const std::string& needle, std::chrono::milliseconds limit)
            {
                const auto deadline = std::chrono::steady_clock::now() + limit;
                while (Text.find(needle) == std::string::npos)
                {
                    DWORD available = 0;
                    if (!::PeekNamedPipe(Output, nullptr, 0, nullptr, &available, nullptr))
                        break;
                    if (available)
                    {
                        char buffer[512];
                        DWORD got = 0;
                        if (!::ReadFile(Output, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0)
                            break;
                        Text.append(buffer, got);
                        continue;
                    }
                    if (std::chrono::steady_clock::now() > deadline)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return Text.find(needle) != std::string::npos;
            }
        };

        // Runs in a child harness: starts the Mono bridge in the target and then
        // waits to be killed, so the worker loses its host without Stop.
        int RunOrphanChild(std::uint32_t pid)
        {
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(pid);
            if (!opened)
            {
                std::printf("could not open %u: %s\n", pid, opened.Text.c_str());
                return 2;
            }
            const Status mono = engine.EnsureMono();
            if (!mono)
            {
                std::printf("no Mono: %s\n", mono.Text.c_str());
                return 1;
            }
            std::printf("bridge ready, %zu classes\n", engine.Mono().Classes().size());
            ::Sleep(INFINITE);
            return 0;
        }

        // A worker whose host is killed must notice, write Stopped and end on
        // its own, and an idle worker must not burn the core it runs on.
        void CheckOrphanedWorker(Engine& engine)
        {
            const std::uint32_t pid = engine.Process()->Id();
            const std::vector<WorkerThread> before = FindWorkers(pid);
            ChildHarness child;
            CHECK(child.Start(L"--mono-check --orphan-child --pid=" + std::to_wstring(pid)));
            const bool ready = child.WaitFor("bridge ready", std::chrono::seconds(15));
            CHECK_MSG(ready, "child harness: " + child.Text);
            if (!ready)
                return;
            const std::vector<WorkerThread> added = NewWorkers(before, FindWorkers(pid));
            CHECK_MSG(added.size() == 1, "expected one new worker thread, found " + std::to_string(added.size()));
            if (added.size() != 1)
                return;
            HANDLE thread = ::OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, added[0].Id);
            CHECK(thread != nullptr);
            if (!thread)
                return;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const double cpuBefore = ThreadCpuSeconds(thread);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const double idleMs = (ThreadCpuSeconds(thread) - cpuBefore) * 1000.0;
            CHECK_MSG(idleMs < 25.0, "the idle worker used " + std::to_string(idleMs) + " ms of CPU in 500 ms");

            ::TerminateProcess(child.Info.hProcess, 1);
            ::WaitForSingleObject(child.Info.hProcess, 5000);
            const auto killed = std::chrono::steady_clock::now();
            const DWORD wait = ::WaitForSingleObject(thread, 3000);
            const double exitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - killed).count();
            CHECK_MSG(wait == WAIT_OBJECT_0, "the worker kept running after its host was killed");
            DWORD code = 0;
            CHECK(::GetExitCodeThread(thread, &code) && code == 2);
            ::CloseHandle(thread);
            CHECK(NewWorkers(before, FindWorkers(pid)).empty());
            CHECK(engine.Process()->Alive());
            std::printf("  orphaned worker: %.1f ms CPU in 500 ms idle, ended %.1f ms after its host was killed (exit code %lu)\n",
                idleMs, exitMs, static_cast<unsigned long>(code));
        }

        // Round-trip cost of RemoteCall::Call back to back and after idle gaps,
        // plus the CPU an idle worker uses.
        void ReportCallCosts(Engine& engine)
        {
            const Target::Process& process = *engine.Process();
            const std::uint64_t attach = process.FindExport(process.Name(), "mono_thread_attach");
            const std::vector<WorkerThread> before = FindWorkers(process.Id());
            Target::RemoteCall call;
            std::uint64_t result = 0;
            if (!attach || !call.Start(engine.Process()) || !call.Call(attach, 1, result))
            {
                std::printf("call costs: the worker did not start\n");
                return;
            }
            using Clock = std::chrono::steady_clock;
            constexpr int kBatches = 10;
            constexpr int kBatch = 2000;
            int wrong = 0;
            std::vector<double> batches;
            for (int b = 0; b < kBatches; ++b)
            {
                const auto batchStart = Clock::now();
                for (int i = 0; i < kBatch; ++i)
                {
                    if (!call.Call(attach, static_cast<std::uint64_t>(i), result) || result != static_cast<std::uint64_t>(i))
                        ++wrong;
                }
                batches.push_back(std::chrono::duration<double, std::micro>(Clock::now() - batchStart).count() / kBatch);
            }
            std::sort(batches.begin(), batches.end());
            const double hotUs = batches[batches.size() / 2];
            constexpr int kHot = kBatches * kBatch;
            CHECK_EQ(wrong, 0);

            std::vector<double> gapped;
            for (int i = 0; i < 20; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                const auto start = Clock::now();
                CHECK(call.Call(attach, 7, result).Ok() && result == 7);
                gapped.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
            }
            std::sort(gapped.begin(), gapped.end());

            double idleMs = -1;
            const std::vector<WorkerThread> added = NewWorkers(before, FindWorkers(process.Id()));
            if (added.size() == 1)
            {
                if (HANDLE thread = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, added[0].Id))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    const double cpu = ThreadCpuSeconds(thread);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    idleMs = (ThreadCpuSeconds(thread) - cpu) * 1000.0;
                    ::CloseHandle(thread);
                }
            }
            std::printf("call costs: %.2f us per call back to back (median of %d batches, %d calls, best %.2f); after 30 ms idle: median %.1f us, max %.1f us; idle worker CPU %.1f ms per second\n",
                hotUs, kBatches, kHot, batches.front(), gapped[gapped.size() / 2], gapped.back(), idleMs);
        }
    }

    void CheckMono(Engine& engine)
    {
        const Target::Process& process = *engine.Process();
        const std::string module = process.Name();

        // The remote-call worker on its own: a pure function must round-trip.
        {
            const std::vector<WorkerThread> before = FindWorkers(process.Id());
            const std::uint32_t handlesBefore = ProcessHandleCount(process.Id());
            Target::RemoteCall call;
            const Status started = call.Start(engine.Process());
            CHECK_MSG(started.Ok(), started.Text);
            const std::vector<WorkerThread> added = NewWorkers(before, FindWorkers(process.Id()));
            CHECK_MSG(!started || added.size() == 1, "expected one new worker thread, found " + std::to_string(added.size()));
            HANDLE worker = added.size() == 1 ? ::OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, added[0].Id) : nullptr;
            if (started)
            {
                const std::uint64_t attach = process.FindExport(module, "mono_thread_attach");
                std::uint64_t result = 0;
                CHECK(attach && call.Call(attach, 0xABCDEF12u, result).Ok());
                CHECK_EQ(result, 0xABCDEF12ull);
                const std::uint64_t root = process.FindExport(module, "mono_get_root_domain");
                CHECK(root && call.Call(root, 0, result).Ok() && result != 0);

                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                CHECK(call.Call(attach, 0x1111u, result).Ok());
                CHECK_EQ(result, 0x1111ull);

                const std::uint64_t sleep = reinterpret_cast<std::uint64_t>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "Sleep"));
                call.SetTimeout(std::chrono::milliseconds(100));
                const Status late = call.Call(sleep, 700, result);
                CHECK_MSG(!late.Ok() && late.Code == ErrorCode::MonoCallFailed, late.Text);
                const Status busy = call.Call(attach, 0x2222u, result);
                CHECK_MSG(!busy.Ok() && busy.Text.find("not returned") != std::string::npos, busy.Text);
                std::this_thread::sleep_for(std::chrono::milliseconds(900));
                call.SetTimeout(std::chrono::milliseconds(5000));
                const Status again = call.Call(attach, 0x3333u, result);
                CHECK_MSG(again.Ok(), again.Text);
                CHECK_EQ(result, 0x3333ull);

                // A stop sent while a call still runs must survive the call's
                // return, so the worker ends once the call is done.
                Target::RemoteCall stuck;
                const std::vector<WorkerThread> others = FindWorkers(process.Id());
                CHECK(stuck.Start(engine.Process()).Ok());
                const std::vector<WorkerThread> stuckAdded = NewWorkers(others, FindWorkers(process.Id()));
                HANDLE stuckThread = stuckAdded.size() == 1 ? ::OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, stuckAdded[0].Id) : nullptr;
                CHECK(stuckThread != nullptr);
                stuck.SetTimeout(std::chrono::milliseconds(50));
                CHECK(!stuck.Call(sleep, 1200, result).Ok());
                stuck.Stop();
                if (stuckThread)
                {
                    DWORD code = 0;
                    CHECK_MSG(::WaitForSingleObject(stuckThread, 3000) == WAIT_OBJECT_0, "the worker kept running after a stop sent during a call");
                    CHECK(::GetExitCodeThread(stuckThread, &code) && code == 1);
                    ::CloseHandle(stuckThread);
                }
            }
            call.Stop();
            if (worker)
            {
                // Stop ends the thread with exit code 1, and the worker closes the
                // handles it was given, so the target keeps its handle count.
                DWORD code = 0;
                CHECK(::WaitForSingleObject(worker, 2000) == WAIT_OBJECT_0);
                CHECK(::GetExitCodeThread(worker, &code) && code == 1);
                ::CloseHandle(worker);
                const std::uint32_t handlesAfter = ProcessHandleCount(process.Id());
                CHECK_MSG(handlesAfter == handlesBefore, "target handles " + std::to_string(handlesBefore) + " before, " + std::to_string(handlesAfter) + " after");
            }
        }
        CheckOrphanedWorker(engine);

        const Status mono = engine.EnsureMono();
        CHECK_MSG(mono.Ok(), mono.Text);
        if (!mono)
            return;
        const std::vector<Target::MonoClass> classes = engine.Mono().Classes();
        CHECK_EQ(classes.size(), 2u);
        const auto player = std::find_if(classes.begin(), classes.end(), [](const Target::MonoClass& c) { return c.Name == "Player"; });
        CHECK(player != classes.end());
        if (player != classes.end())
        {
            CHECK_EQ(player->Namespace, std::string("Game"));
            CHECK_EQ(player->Image, std::string("Assembly-CSharp"));
        }
        const std::vector<std::string> playerMethods = engine.Mono().MethodsOf("Player");
        CHECK(playerMethods.size() == 2 && playerMethods[0] == "TakeDamage" && playerMethods[1] == "Heal");
        const std::vector<Target::MonoField> playerFields = engine.Mono().FieldsOf("Player");
        CHECK_MSG(playerFields.size() == 2, "static field should be skipped, got " + std::to_string(playerFields.size()));
        if (playerFields.size() == 2)
        {
            CHECK(playerFields[0].Name == "health" && playerFields[0].Offset == 0x10 && playerFields[0].TypeCode == 0x08);
            CHECK(playerFields[1].Name == "ammo" && playerFields[1].Offset == 0x1C && playerFields[1].TypeCode == 0x08);
        }
        // Class-from-address, the Structure Dissect lookup.
        const auto object = engine.Evaluate("TestTarget.g_fakePlayer");
        CHECK(object.has_value());
        if (object)
        {
            const auto className = engine.Mono().ClassNameAt(*object);
            CHECK_MSG(className && *className == "Game.Player", className ? *className : "nil");
            CHECK(!engine.Mono().ClassNameAt(*object + 0x100000).has_value());
            // A field address (inside the object) still finds the header.
            const auto fromField = engine.Mono().ClassAt(*object + 0x8);
            CHECK_MSG(fromField && fromField->Name == "Game.Player" && fromField->Base == *object,
                fromField ? fromField->Name + " at " + Hex(fromField->Base) : "nil");
            CHECK(!engine.Mono().ClassAt(*object + 0x100000).has_value());
        }
        CHECK(std::any_of(classes.begin(), classes.end(), [](const Target::MonoClass& c) { return c.Name == "GameManager"; }));
        const std::vector<std::string> managerMethods = engine.Mono().MethodsOf("GameManager");
        CHECK(managerMethods.size() == 1 && managerMethods[0] == "Update");

        const auto take = engine.Mono().AddressOf("Player:TakeDamage");
        const auto heal = engine.Mono().AddressOf("Game.Player:Heal");
        CHECK_MSG(take && *take, "Player:TakeDamage did not resolve");
        CHECK_MSG(heal && *heal, "Game.Player:Heal did not resolve");
        if (take)
        {
            const auto main = process.MainModule();
            CHECK_MSG(main && *take >= main->Base && *take < main->Base + main->Size, "the JIT address is not in the module");
        }
        CHECK(!engine.Mono().AddressOf("Nope:Missing").has_value());
        CHECK(engine.Mono().FieldOffset("Player", "ammo") == 0x1C);
        CHECK(engine.Mono().FieldOffset("Game.Player", "health") == 0x10);

        // Reverse resolution: an address inside a Mono method's JIT code names
        // the method, which is what labels Mono code in the memory viewer. The
        // fake maps TakeDamage's compiled address to a 0x20-byte code range.
        if (take && *take)
        {
            const auto atStart = engine.Mono().MethodNameAt(*take);
            CHECK_MSG(atStart && *atStart == "Game.Player:TakeDamage", atStart ? *atStart : "nil");
            const auto inside = engine.Mono().MethodNameAt(*take + 4);
            CHECK_MSG(inside && *inside == "Game.Player:TakeDamage", inside ? *inside : "nil");
            CHECK(!engine.Mono().MethodNameAt(*take + 0x1000).has_value());
            CHECK(!engine.Mono().MethodNameAt(0).has_value());
        }

        // The combined symbol provider resolves Mono names too.
        Status error;
        const auto viaSymbols = engine.Evaluate("Player:TakeDamage", &error);
        CHECK_MSG(viaSymbols && viaSymbols == take, error.Text);

        // The Lua API over the same bridge.
        std::string luaResult;
        const Status lua = engine.Lua().RunWithResult(
            "if not mono_isPresent() then return 'absent' end "
            "local classes = mono_enumClasses() "
            "local methods = mono_class_getMethods('Player') "
            "return tostring(#classes) .. ',' .. tostring(#methods) .. ',' .. "
            "string.format('%X', mono_getAddress('Player', 'TakeDamage') or 0) .. ',' .. "
            "tostring(mono_class_getFieldOffset('Game.Player', 'ammo'))",
            "mono lua", luaResult);
        CHECK_MSG(lua.Ok(), lua.Text);
        if (lua)
        {
            const std::string expected = "2,2," + Hex(*take) + ",28";
            CHECK_EQ(luaResult, expected);
        }
    }

    // Attaches to an already-running process and lists its Mono runtime. Only
    // our own pages are written; the game memory is read, not changed.
    int RunMonoDump(const std::vector<std::string>& arguments)
    {
        std::string name = "CloverPit";
        std::uint32_t pid = 0;
        std::uint64_t object = 0;
        std::uint64_t method = 0;
        for (const std::string& argument : arguments)
        {
            if (argument.rfind("--process=", 0) == 0)
                name = argument.substr(10);
            else if (argument.rfind("--pid=", 0) == 0)
                pid = static_cast<std::uint32_t>(ParseDecimal(argument.substr(6)).value_or(0));
            else if (argument.rfind("--object=", 0) == 0)
                object = ParseHex(argument.substr(9)).value_or(0);
            else if (argument.rfind("--mono-method=", 0) == 0)
                method = ParseHex(argument.substr(14)).value_or(0);
        }
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        if (std::find(arguments.begin(), arguments.end(), "--workers") != arguments.end())
        {
            // Read-only: list RemoteCall worker threads and the CPU they used.
            if (!pid)
                pid = Target::FindProcessId(name).value_or(0);
            if (!pid)
            {
                std::printf("%s is not running\n", name.c_str());
                return 2;
            }
            const std::vector<WorkerThread> workers = FindWorkers(pid);
            std::printf("pid %u: %zu remote-call worker thread(s)\n", pid, workers.size());
            for (const WorkerThread& worker : workers)
                std::printf("  thread %u at %s, %.1f s CPU\n", worker.Id, Hex(worker.Start).c_str(), worker.CpuSeconds);
            return 0;
        }
        Settings settings;
        Engine engine(settings);
        const Status opened = pid ? engine.OpenProcess(pid) : engine.OpenProcessByName(name);
        if (!opened)
        {
            std::printf("could not open %s: %s\n", pid ? std::to_string(pid).c_str() : name.c_str(), opened.Text.c_str());
            return 2;
        }
        std::printf("opened %s (pid %lu, %s)\n", engine.Process()->Name().c_str(), static_cast<unsigned long>(engine.Process()->Id()),
            engine.Process()->Is64Bit() ? "64-bit" : "32-bit");
        const auto start = std::chrono::steady_clock::now();
        const Status mono = engine.EnsureMono();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!mono)
        {
            std::printf("no Mono: %s\n", mono.Text.c_str());
            return 1;
        }
        const std::vector<Target::MonoClass> classes = engine.Mono().Classes();
        std::printf("Mono in %s: %zu classes in %.2fs (members load on demand)\n", engine.Mono().Module().c_str(), classes.size(), seconds);
        const std::size_t show = std::min<std::size_t>(classes.size(), 18);
        for (std::size_t i = 0; i < show; ++i)
        {
            const Target::MonoClass& c = classes[i];
            const std::size_t methods = engine.Mono().MethodsOf(c.FullName()).size();
            const std::size_t fields = engine.Mono().FieldsOf(c.FullName()).size();
            std::printf("  %-44s  %3zu methods  %3zu fields\n", c.FullName().c_str(), methods, fields);
        }
        if (classes.size() > show)
            std::printf("  ... and %zu more\n", classes.size() - show);

        // Prove address resolution on the first class that has a method, and a field offset.
        for (const Target::MonoClass& c : classes)
        {
            const std::vector<std::string> classMethods = engine.Mono().MethodsOf(c.FullName());
            if (classMethods.empty())
                continue;
            const std::string full = c.FullName() + ":" + classMethods.front();
            const auto address = engine.Mono().AddressOf(full);
            std::printf("resolve %s -> %s", full.c_str(), address ? Hex(*address).c_str() : "nil");
            if (address)
            {
                const auto module = engine.Process()->ModuleAt(*address);
                std::printf("  (%s)", module ? module->Name.c_str() : "not in a module");
            }
            std::printf("\n");
            if (address)
            {
                // Round-trip the JIT address back to a name; this is what labels
                // Mono code in the memory viewer (reverse of AddressOf).
                const auto reverse = engine.Mono().MethodNameAt(*address);
                std::printf("reverse %s -> %s\n", Hex(*address).c_str(), reverse ? reverse->c_str() : "not resolved");
            }
            const std::vector<Target::MonoField> classFields = engine.Mono().FieldsOf(c.FullName());
            if (!classFields.empty())
                std::printf("field %s:%s offset 0x%X\n", c.FullName().c_str(), classFields.front().Name.c_str(),
                    engine.Mono().FieldOffset(c.FullName(), classFields.front().Name).value_or(-1));
            break;
        }
        if (object)
        {
            const auto vtable = engine.Process()->ReadPointer(object);
            const auto klass = vtable ? engine.Process()->ReadPointer(*vtable) : std::nullopt;
            const auto instance = engine.Mono().ClassAt(object);
            std::printf("object %s: vtable %s, klass %s -> class %s", Hex(object).c_str(),
                vtable ? Hex(*vtable).c_str() : "unreadable", klass ? Hex(*klass).c_str() : "unreadable",
                instance ? instance->Name.c_str() : "not resolved");
            if (instance)
                std::printf(" at %s (%+lld from the address)", Hex(instance->Base).c_str(), static_cast<long long>(instance->Base) - static_cast<long long>(object));
            std::printf("\n");
            if (instance)
            {
                for (const Target::MonoField& field : engine.Mono().FieldsOf(instance->Name))
                    std::printf("  +0x%-4X %s\n", field.Offset, field.Name.c_str());
            }
        }
        if (method)
        {
            const auto reverse = engine.Mono().MethodNameAt(method);
            std::printf("method at %s: %s\n", Hex(method).c_str(), reverse ? reverse->c_str() : "not resolved");
        }
        std::printf("done; detaching\n");
        return 0;
    }

    int RunMonoCheck(const std::vector<std::string>& arguments)
    {
        bool orphanChild = false;
        std::uint32_t childPid = 0;
        for (const std::string& argument : arguments)
        {
            if (argument == "--orphan-child")
                orphanChild = true;
            else if (argument.rfind("--pid=", 0) == 0)
                childPid = static_cast<std::uint32_t>(ParseDecimal(argument.substr(6)).value_or(0));
        }
        if (orphanChild)
            return RunOrphanChild(childPid);
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::filesystem::path targetPath = std::filesystem::path(self).parent_path() / L"TestTarget.exe";
        std::wstring command = L"\"" + targetPath.wstring() + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION info{};
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        {
            std::printf("could not start %ls\n", targetPath.c_str());
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const int before = Totals().Failures;
        {
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(info.dwProcessId);
            CHECK_MSG(opened.Ok(), opened.Text);
            if (opened)
            {
                CheckMono(engine);
                ReportCallCosts(engine);
            }
        }
        ::TerminateProcess(info.hProcess, 0);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        const int failures = Totals().Failures - before;
        std::printf("mono check: %d checks, %d failed\n", Totals().Checks, failures);
        return failures == 0 ? 0 : 1;
    }
}
