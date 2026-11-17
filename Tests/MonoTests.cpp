#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Target/Mono.h"
#include "Target/Process.h"
#include "Target/RemoteCall.h"
#include "Target/UnityExplorer.h"

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

    void SettleMono(Engine& engine, int milliseconds)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (engine.MonoLoading() && std::chrono::steady_clock::now() < until)
        {
            engine.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        engine.Tick();
    }

    void CheckMonoInBackground(Engine& engine)
    {
        const std::uint64_t generation = engine.Mono().Generation();
        engine.ReloadMono(true);
        CHECK(engine.MonoLoading());
        CHECK(engine.MonoPresent());
        SettleMono(engine, 5000);
        CHECK(!engine.MonoLoading());
        CHECK(engine.Mono().Generation() != generation);
        CHECK_EQ(engine.Mono().ClassCount(), 2u);

        WorkProgress stopped;
        stopped.Cancel();
        const std::uint64_t before = engine.Mono().Generation();
        const Status refused = engine.Mono().Refresh(&stopped);
        CHECK(!refused.Ok());
        CHECK_EQ(engine.Mono().Generation(), before);
        CHECK_EQ(engine.Mono().ClassCount(), 2u);
        CHECK(engine.MonoPresent());

        engine.Mono().Stop();
        CHECK(!engine.MonoPresent());
        const Status cancelled = engine.Mono().Start(engine.Process(), &stopped);
        CHECK(!cancelled.Ok());
        CHECK(!engine.MonoPresent());
        CHECK_EQ(engine.Mono().ClassCount(), 0u);

        engine.LoadMono(true);
        CHECK(engine.MonoLoading());
        CHECK(engine.MonoWork() != 0);
        SettleMono(engine, 5000);
        CHECK(!engine.MonoLoading());
        CHECK(engine.MonoPresent());
        CHECK_EQ(engine.Mono().ClassCount(), 2u);
        Status error;
        CHECK_MSG(engine.Evaluate("Player:TakeDamage", &error).has_value(), error.Text);

        engine.Mono().Stop();
        engine.LoadMono(true);
        engine.CancelWork(engine.MonoWork());
        SettleMono(engine, 5000);
        CHECK(!engine.MonoLoading());
        CHECK(engine.MonoPresent() == (engine.Mono().ClassCount() == 2u));

        engine.Mono().Stop();
        engine.LoadMono(true);
        const Status ensured = engine.EnsureMono();
        CHECK_MSG(ensured.Ok(), ensured.Text);
        CHECK(!engine.MonoLoading());
        CHECK(engine.MonoPresent());

        engine.Mono().Stop();
        engine.SetAsync(true);
        const Status started = engine.Lua().Run("__monoWait = nil; createThread(function() __monoWait = tostring(mono_initialize()) .. ',' .. tostring(#mono_enumClasses()) end)", "mono wait");
        CHECK_MSG(started.Ok(), started.Text);
        bool sawLoading = false;
        std::string waited;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until)
        {
            engine.Tick();
            sawLoading = sawLoading || engine.MonoLoading();
            const Status read = engine.Lua().RunWithResult("return __monoWait or ''", "mono wait result", waited);
            if (!read || !waited.empty())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        engine.SetAsync(false);
        CHECK(sawLoading);
        CHECK_EQ(waited, std::string("true,2"));
        CHECK(engine.MonoPresent());
    }

    // The main-thread channel: with the stub wired into the target's per-frame
    // tick (as the Unity explorer hangs it on Application.onBeforeRender), calls
    // run on the target's main thread, and the channel ends cleanly.
    void CheckMainThread(Engine& engine)
    {
        Target::Process& process = *engine.Process();
        const std::uint64_t attach = process.FindExport(process.Name(), "mono_thread_attach");
        const std::uint64_t tickSlot = process.FindExport(process.Name(), "g_tick");
        CHECK(attach && tickSlot);
        if (!attach || !tickSlot)
            return;
        Target::RemoteCall call;
        CHECK(call.Start(engine.Process()).Ok());
        if (!call.Running())
            return;

        // Before the tick is wired, EnterMain must give up (the stub never runs).
        call.SetTimeout(std::chrono::milliseconds(5000));
        const Status noTick = call.EnterMain(std::chrono::milliseconds(400));
        CHECK_MSG(!noTick.Ok(), "EnterMain should fail while nothing calls the stub");
        CHECK(!call.InsideMain());

        const std::uint64_t stub = call.MainStub();
        CHECK(stub != 0);
        CHECK(process.Write(tickSlot, &stub, sizeof(stub)).Ok());

        const Status entered = call.EnterMain(std::chrono::milliseconds(3000));
        CHECK_MSG(entered.Ok(), entered.Text);
        CHECK(call.InsideMain());
        const std::uint32_t thread = call.MainThreadId();
        CHECK(thread != 0);

        std::uint64_t result = 0;
        bool ok = true;
        for (int i = 0; i < 200; ++i)
            ok = ok && call.CallMain(attach, static_cast<std::uint64_t>(i), 0, 0, 0, result).Ok() && result == static_cast<std::uint64_t>(i);
        CHECK_MSG(ok, "a batch of main-thread calls did not round-trip");
        CHECK_EQ(call.MainThreadId(), thread);
        CHECK(call.MainRuns() > 0);

        call.LeaveMain();
        CHECK(!call.InsideMain());

        // The stub keeps returning after leaving (idle), so the tick is harmless.
        const std::uint64_t runs = call.MainRuns();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK(call.MainRuns() >= runs);

        const std::uint64_t none = 0;
        process.Write(tickSlot, &none, sizeof(none));
        call.Stop();
    }

    // Host-side value formatting and parsing (no live game needed): the
    // inspector draws fields from these.
    void CheckUnityFormat(Engine& engine)
    {
        Target::UnityExplorer& unity = engine.Mono().Unity();
        const auto info = [](int code, int size)
            {
                Target::MonoTypeInfo type;
                type.Code = code;
                type.ValueCode = code;
                type.ValueType = true;
                type.Size = size;
                return type;
            };
        const std::int32_t number = 1337;
        const Target::MonoShown shownInt = unity.Format(info(Target::MonoTypeCode::I4, 4), reinterpret_cast<const std::uint8_t*>(&number), 4);
        CHECK_EQ(shownInt.Text, std::string("1337"));
        CHECK(shownInt.Editable);
        const std::uint8_t yes = 1;
        CHECK_EQ(unity.Format(info(Target::MonoTypeCode::Boolean, 1), &yes, 1).Text, std::string("true"));
        const float speed = 3.5f;
        CHECK_EQ(unity.Format(info(Target::MonoTypeCode::R4, 4), reinterpret_cast<const std::uint8_t*>(&speed), 4).Text, std::string("3.5"));

        std::string error;
        const auto bytes = unity.Parse(info(Target::MonoTypeCode::I4, 4), "0x100", &error);
        CHECK_MSG(bytes.has_value(), error);
        if (bytes)
        {
            CHECK_EQ(bytes->size(), 4u);
            std::int32_t parsed = 0;
            std::memcpy(&parsed, bytes->data(), 4);
            CHECK_EQ(parsed, 256);
        }
        CHECK(!unity.Parse(info(Target::MonoTypeCode::I1, 1), "999", &error).has_value());
        CHECK(unity.Parse(info(Target::MonoTypeCode::Boolean, 1), "true", &error).has_value());

        Target::MonoMember method;
        method.Kind = Target::MonoMemberKind::Method;
        method.Name = "Teleport";
        method.Params = { info(Target::MonoTypeCode::R4, 4), info(Target::MonoTypeCode::R4, 4) };
        method.Params[0].Name = "System.Single";
        method.Params[1].Name = "System.Single";
        method.ParamNames = { "x", "y" };
        CHECK_EQ(Target::UnityExplorer::Signature(method), std::string("(float x, float y)"));
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
        CheckMonoInBackground(engine);
        CheckMainThread(engine);
        CheckUnityFormat(engine);
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

    // Attaches to a running Unity game and exercises the explorer end to end,
    // read-mostly: it injects the per-frame callback and sets runInBackground
    // (restored on detach) but changes no game state. It reads the scenes,
    // expands one, searches for a class and inspects an object, then detaches.
    int RunUnityDump(const std::vector<std::string>& arguments)
    {
        std::string name = "CloverPit";
        std::uint32_t pid = 0;
        std::string searchClass = "UnityEngine.Camera";
        int idleSeconds = 0;
        int probe = 4;
        // Same default as the explorer: no GC handles unless asked for. With
        // pins on by default a probe run freed the delegate's handle at detach,
        // which is the crash the probes were meant to find elsewhere.
        bool pinning = false;
        for (const std::string& argument : arguments)
        {
            if (argument.rfind("--process=", 0) == 0)
                name = argument.substr(10);
            else if (argument.rfind("--pid=", 0) == 0)
                pid = static_cast<std::uint32_t>(ParseDecimal(argument.substr(6)).value_or(0));
            else if (argument.rfind("--class=", 0) == 0)
                searchClass = argument.substr(8);
            else if (argument.rfind("--idle-seconds=", 0) == 0)
                idleSeconds = static_cast<int>(ParseDecimal(argument.substr(15)).value_or(0));
            else if (argument.rfind("--probe=", 0) == 0)
                probe = static_cast<int>(ParseDecimal(argument.substr(8)).value_or(4));
            else if (argument == "--pin")
                pinning = true;
            else if (argument == "--no-pin")
                pinning = false;
        }
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        Settings settings;
        Engine engine(settings);
        const Status opened = pid ? engine.OpenProcess(pid) : engine.OpenProcessByName(name);
        if (!opened)
        {
            std::printf("could not open %s: %s\n", pid ? std::to_string(pid).c_str() : name.c_str(), opened.Text.c_str());
            return 2;
        }
        std::printf("opened %s (pid %lu)\n", engine.Process()->Name().c_str(), static_cast<unsigned long>(engine.Process()->Id()));
        const Status mono = engine.EnsureMono();
        if (!mono)
        {
            std::printf("no Mono: %s\n", mono.Text.c_str());
            return 1;
        }
        std::printf("Mono: %zu classes in %s\n", engine.Mono().ClassCount(), engine.Mono().Module().c_str());

        Target::UnityExplorer& unity = engine.Mono().Unity();
        const auto wait = [&](const char* what, auto ready, int seconds)
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                while (std::chrono::steady_clock::now() < deadline)
                {
                    const Target::UnityStatus status = unity.State();
                    if (!status.Error.empty())
                    {
                        std::printf("  %s: %s\n", what, status.Error.c_str());
                        return false;
                    }
                    if (!status.Busy && ready())
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
                std::printf("  %s: timed out\n", what);
                return false;
            };

        // The game must be in the FOREGROUND and rendering: Unity engine calls
        // run on its main thread, reached through the onBeforeRender callback,
        // which only fires while the game renders.
        // This harness exists to exercise the explorer, so it opts in for itself.
        unity.SetEnabled(true);
        unity.SetProbeLevel(probe);
        unity.SetPinning(pinning);
        std::printf("GC handles are switched %s for this run\n", pinning ? "ON (--pin)" : "OFF");
        unity.ReadScenes();  // the first real request installs the hook and enters main
        if (probe < 4)
        {
            // Bisection run: let the bootstrap stop at the probe level, then sit
            // still and see whether the game minds. Give the job a moment to
            // start first -- asking "is it idle?" before it ever began reports
            // the state from before the probe.
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            wait("probe", [&] { return !unity.State().Busy; }, 20);
            const Target::UnityStatus probed = unity.State();
            std::printf("probe level %d: hooked %d, callback runs %llu, note: %s\n", probe, probed.Hooked ? 1 : 0,
                static_cast<unsigned long long>(probed.CallbackRuns), probed.Error.c_str());
            const int seconds = idleSeconds > 0 ? idleSeconds : 6;
            for (int i = 0; i < seconds * 2; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!engine.Process()->Alive())
                {
                    std::printf("the game died %.1f s into probe level %d\n", (i + 1) * 0.5, probe);
                    return 1;
                }
            }
            std::printf("probe level %d: survived %d s; detaching\n", probe, seconds);
            return 0;
        }
        wait("hook", [&] { return unity.State().Hooked && unity.State().MainThread != 0; }, 20);
        const Target::UnityStatus status = unity.State();
        std::printf("hooked on %s, callback runs %llu, main thread %lu, Unity %s\n", status.Hook.c_str(),
            static_cast<unsigned long long>(status.CallbackRuns), static_cast<unsigned long>(status.MainThread),
            status.UnityVersion.empty() ? "?" : status.UnityVersion.c_str());
        if (!status.Missing.empty())
            std::printf("MISSING mono exports: %s\n", Join(status.Missing, ", ").c_str());
        if (!status.Hooked || status.MainThread == 0)
        {
            if (status.CallbackRuns == 0)
                std::printf("the game never called the callback at all: it is not rendering (background/minimized/loading)\n");
            else
                std::printf("the game called the callback %llu times but the handshake did not complete\n",
                    static_cast<unsigned long long>(status.CallbackRuns));
            return 1;
        }

        if (idleSeconds > 0)
        {
            // Bare-callback test: the hook is in and the main thread calls the
            // stub each frame, but nothing else runs. If the game survives this,
            // the callback itself is harmless and the managed calls are the risk.
            std::printf("idling %d s with the callback installed, no further calls...\n", idleSeconds);
            for (int i = 0; i < idleSeconds * 2; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!engine.Process()->Alive())
                {
                    std::printf("the game died after %.1f s of just the idle callback\n", (i + 1) * 0.5);
                    return 1;
                }
            }
            std::printf("survived %d s with the idle callback; detaching\n", idleSeconds);
            return 0;
        }

        unity.ReadScenes();
        if (!wait("scenes", [&] { return unity.Tree().Read; }, 20))
            return 1;
        Target::UnityTree tree = unity.Tree();
        std::printf("%zu scene(s):\n", tree.Scenes.size());
        int expandable = -1;
        for (int s = 0; s < static_cast<int>(tree.Scenes.size()); ++s)
        {
            const Target::UnityScene& scene = tree.Scenes[static_cast<std::size_t>(s)];
            std::printf("  [%d] %-28s roots %d%s%s\n", s, scene.Name.c_str(), scene.RootCount,
                scene.Active ? " (active)" : "", scene.DontDestroy ? " (DontDestroyOnLoad)" : "");
            if (expandable < 0 && scene.RootCount != 0 && !scene.DontDestroy)
                expandable = s;
        }
        if (expandable >= 0)
        {
            unity.ExpandScene(expandable);
            wait("expand", [&] { return unity.Tree().Scenes[static_cast<std::size_t>(expandable)].RootsLoaded; }, 20);
            tree = unity.Tree();
            const Target::UnityScene& scene = tree.Scenes[static_cast<std::size_t>(expandable)];
            std::printf("roots of \"%s\":\n", scene.Name.c_str());
            for (std::size_t i = 0; i < scene.Roots.size() && i < 12; ++i)
            {
                const Target::UnityNode& node = tree.Nodes[static_cast<std::size_t>(scene.Roots[i])];
                std::printf("  %s%-32s  %d child(ren)  %s\n", node.Active ? " " : "~", node.Name.c_str(), node.ChildCount, Hex(node.Object).c_str());
            }
        }

        unity.Search({ searchClass, std::string(), false });
        wait("search", [&] { return unity.SearchResult().Done; }, 30);
        const Target::UnitySearchResult result = unity.SearchResult();
        std::printf("search %s: %zu hit(s)%s\n", searchClass.c_str(), result.Hits.size(), result.HeapScan ? " (memory scan)" : "");
        std::uint64_t inspectObject = 0;
        for (std::size_t i = 0; i < result.Hits.size() && i < 8; ++i)
        {
            std::printf("  %-28s %s %s\n", result.Hits[i].Name.c_str(), result.Hits[i].Class.c_str(), Hex(result.Hits[i].Object).c_str());
            if (!inspectObject)
                inspectObject = result.Hits[i].Object;
        }
        if (!inspectObject && expandable >= 0 && !tree.Scenes[static_cast<std::size_t>(expandable)].Roots.empty())
            inspectObject = tree.Nodes[static_cast<std::size_t>(tree.Scenes[static_cast<std::size_t>(expandable)].Roots[0])].Object;

        if (inspectObject)
        {
            const int id = unity.Inspect(inspectObject);
            wait("inspect", [&] { const auto in = unity.Inspection(id); return in && !in->Loading; }, 20);
            if (const auto inspection = unity.Inspection(id))
            {
                std::printf("inspect %s -> %s\n", Hex(inspectObject).c_str(), inspection->Title.c_str());
                if (inspection->GameObject)
                {
                    const Target::UnityGameObject& go = *inspection->GameObject;
                    std::printf("  GameObject \"%s\"  active %d  scene %s  layer %d  %zu component(s)\n",
                        go.Name.c_str(), go.ActiveInHierarchy, go.Scene.c_str(), go.Layer, go.Components.size());
                    if (go.Transform.Read)
                        std::printf("  localPosition %.3f, %.3f, %.3f\n", go.Transform.LocalPosition[0], go.Transform.LocalPosition[1], go.Transform.LocalPosition[2]);
                    for (std::size_t i = 0; i < go.Components.size() && i < 10; ++i)
                        std::printf("    %s%s\n", go.Components[i].Class.c_str(), go.Components[i].HasEnabled ? (go.Components[i].Enabled ? " [enabled]" : " [disabled]") : "");
                }
                if (inspection->Layout)
                {
                    int shown = 0;
                    for (std::size_t i = 0; i < inspection->Layout->Members.size() && shown < 14; ++i)
                    {
                        const Target::MonoMember& member = inspection->Layout->Members[i];
                        if (member.Kind == Target::MonoMemberKind::Field && !member.Static)
                        {
                            const Target::MonoShown value = unity.ReadField(member, inspectObject);
                            std::printf("    %-24s %-18s = %s\n", member.Name.c_str(), Target::UnityExplorer::ShortTypeName(member.Type).c_str(), value.Text.c_str());
                            ++shown;
                        }
                    }
                }
                if (!inspection->Error.empty())
                    std::printf("  note: %s\n", inspection->Error.c_str());
            }
        }
        std::printf("done; detaching (callback removed, runInBackground restored)\n");
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
