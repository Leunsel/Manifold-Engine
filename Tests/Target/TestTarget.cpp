#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    struct Player
    {
        int Health = 1000;
        float Speed = 3.5f;
        double Gold = 1234.5;
        char Name[16] = "ManifoldHero";
        int Ammo = 30;
    };

    struct World
    {
        int Level = 7;
        Player* Hero = nullptr;
    };

    volatile int g_ticks = 0;

    __declspec(noinline) int ReadHealth(const Player* player)
    {
        return player->Health;
    }

    __declspec(noinline) void Shoot(Player* player)
    {
        if (player->Ammo > 0)
            --player->Ammo;
        else
            player->Ammo = 30;
    }
}

extern "C" __declspec(dllexport) World* g_world = nullptr;

extern "C" __declspec(dllexport) __declspec(noinline) void Damage(Player* player, int amount)
{
    player->Health -= amount;
}

extern "C" __declspec(dllexport) volatile long g_shared = 0;

extern "C" __declspec(dllexport) __declspec(noinline) void Work(volatile long* value)
{
    *value = *value + 1;
}

extern "C" __declspec(dllexport) __declspec(align(64)) volatile long long g_stress[4] = {};

// A tiny stand-in for the Mono runtime: enough exported mono_* functions, with
// the same shapes the real ones have, to exercise the remote-call worker and
// the enumeration path headlessly. Not a real Mono ABI, just the accessors the
// bridge calls. MONO_TABLE_TYPEDEF = 2, MONO_TOKEN_TYPE_DEF = 0x02000000.
namespace
{
    struct FakeMethod { const char* Name; void* Address; };
    struct FakeField { const char* Name; int Offset; int TypeCode; int Flags; };
    struct FakeClass { const char* Namespace; const char* Name; const FakeMethod* Methods; int MethodCount; const FakeField* Fields; int FieldCount; };
    struct FakeImage { const char* Name; const FakeClass* Classes; int ClassCount; };

    long long FakeAdd(long long a) { return a + 1; }
    long long FakeSub(long long a) { return a - 1; }

    const FakeMethod kPlayerMethods[] = { { "TakeDamage", reinterpret_cast<void*>(&FakeAdd) }, { "Heal", reinterpret_cast<void*>(&FakeSub) } };
    // health and ammo are instance I4 fields; s_instances is static and must be skipped.
    const FakeField kPlayerFields[] = { { "health", 0x10, 0x08, 0 }, { "ammo", 0x1C, 0x08, 0 }, { "s_instances", 0x00, 0x12, 0x10 } };
    const FakeMethod kGameMethods[] = { { "Update", reinterpret_cast<void*>(&FakeAdd) } };
    const FakeClass kClasses[] = {
        { "Game", "Player", kPlayerMethods, 2, kPlayerFields, 3 },
        { "Game", "GameManager", kGameMethods, 1, nullptr, 0 },
    };
    const FakeImage kImage = { "Assembly-CSharp", kClasses, 2 };
    int g_domain = 0xD0;

    struct FakeVTable { const FakeClass* Klass; };
    FakeVTable g_playerVT = { &kClasses[0] };
    struct FakeObject { FakeVTable* VTable; int Health; int Ammo; };
}

extern "C" __declspec(dllexport) FakeObject g_fakePlayer = { &g_playerVT, 1000, 30 };

extern "C"
{
    __declspec(dllexport) void* mono_get_root_domain() { return &g_domain; }
    __declspec(dllexport) void* mono_thread_attach(void* domain) { return domain; }
    __declspec(dllexport) void mono_assembly_foreach(void (*fn)(void*, void*), void* user) { fn(const_cast<FakeImage*>(&kImage), user); }
    __declspec(dllexport) void* mono_assembly_get_image(void* assembly) { return assembly; }
    __declspec(dllexport) const char* mono_image_get_name(void* image) { return static_cast<FakeImage*>(image)->Name; }
    __declspec(dllexport) void* mono_image_get_table_info(void* image, int) { return image; }
    __declspec(dllexport) int mono_table_info_get_rows(void* tableinfo) { return static_cast<FakeImage*>(tableinfo)->ClassCount; }
    __declspec(dllexport) void* mono_class_get(void* image, unsigned int token)
    {
        FakeImage* img = static_cast<FakeImage*>(image);
        const unsigned int index = (token & 0x00FFFFFFu) - 1u;
        return index < static_cast<unsigned int>(img->ClassCount) ? const_cast<FakeClass*>(&img->Classes[index]) : nullptr;
    }
    __declspec(dllexport) const char* mono_class_get_name(void* klass) { return static_cast<FakeClass*>(klass)->Name; }
    __declspec(dllexport) const char* mono_class_get_namespace(void* klass) { return static_cast<FakeClass*>(klass)->Namespace; }
    __declspec(dllexport) void* mono_class_get_methods(void* klass, void** iter)
    {
        FakeClass* c = static_cast<FakeClass*>(klass);
        const std::size_t i = reinterpret_cast<std::size_t>(*iter);
        if (i >= static_cast<std::size_t>(c->MethodCount))
            return nullptr;
        *iter = reinterpret_cast<void*>(i + 1);
        return const_cast<FakeMethod*>(&c->Methods[i]);
    }
    __declspec(dllexport) const char* mono_method_get_name(void* method) { return static_cast<FakeMethod*>(method)->Name; }
    __declspec(dllexport) void* mono_compile_method(void* method) { return static_cast<FakeMethod*>(method)->Address; }
    __declspec(dllexport) void* mono_class_get_fields(void* klass, void** iter)
    {
        FakeClass* c = static_cast<FakeClass*>(klass);
        const std::size_t i = reinterpret_cast<std::size_t>(*iter);
        if (i >= static_cast<std::size_t>(c->FieldCount))
            return nullptr;
        *iter = reinterpret_cast<void*>(i + 1);
        return const_cast<FakeField*>(&c->Fields[i]);
    }
    __declspec(dllexport) const char* mono_field_get_name(void* field) { return static_cast<FakeField*>(field)->Name; }
    __declspec(dllexport) int mono_field_get_offset(void* field) { return static_cast<FakeField*>(field)->Offset; }
    __declspec(dllexport) void* mono_field_get_type(void* field) { return &static_cast<FakeField*>(field)->TypeCode; }
    __declspec(dllexport) int mono_type_get_type(void* type) { return *static_cast<int*>(type); }
    __declspec(dllexport) int mono_field_get_flags(void* field) { return static_cast<FakeField*>(field)->Flags; }
    __declspec(dllexport) void* mono_object_get_class(void* object) { return const_cast<FakeClass*>(static_cast<FakeObject*>(object)->VTable->Klass); }
}

extern "C" __declspec(dllexport) __declspec(noinline) long long Touch(volatile long long* value)
{
    *value = *value + 1;
    return *value;
}

namespace
{
    DWORD WINAPI Worker(void*)
    {
        HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (;;)
        {
            Work(&g_shared);
            ::WaitForSingleObject(wake, 1);
        }
    }
}

namespace
{
    int g_stressSleep = 0;

    DWORD WINAPI StressWorker(void* parameter)
    {
        const bool spin = parameter != nullptr;
        for (;;)
        {
            Touch(&g_stress[0]);
            if (g_stressSleep > 0)
                ::Sleep(g_stressSleep);
            else if (spin)
                ::SwitchToThread();
            else
                ::Sleep(0);
        }
    }

    DWORD WINAPI ShortLived(void*)
    {
        for (int i = 0; i < 50; ++i)
            Touch(&g_stress[0]);
        return 0;
    }

    DWORD WINAPI Churn(void*)
    {
        for (;;)
        {
            HANDLE thread = ::CreateThread(nullptr, 0, &ShortLived, nullptr, 0, nullptr);
            if (thread)
                ::CloseHandle(thread);
            ::Sleep(5);
        }
    }

    DWORD WINAPI KernelWriter(void*)
    {
        for (;;)
        {
            FILETIME created{}, exited{}, kernel{};
            ::GetThreadTimes(::GetCurrentThread(), &created, &exited, &kernel, reinterpret_cast<FILETIME*>(const_cast<long long*>(&g_stress[0])));
            ::Sleep(1);
        }
    }

    void HideCurrentThread()
    {
        using SetInformation = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
        if (const auto set = reinterpret_cast<SetInformation>(reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtSetInformationThread"))))
            set(::GetCurrentThread(), 0x11, nullptr, 0);
    }

    DWORD WINAPI HiddenWorker(void* parameter)
    {
        const DWORD delay = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(parameter));
        const ULONGLONG hideAt = ::GetTickCount64() + delay;
        if (!delay)
            HideCurrentThread();
        bool hidden = !delay;
        for (;;)
        {
            if (!hidden && ::GetTickCount64() >= hideAt)
            {
                HideCurrentThread();
                hidden = true;
            }
            Touch(&g_stress[0]);
            ::Sleep(2);
        }
    }

    void StartStress(bool hidden, DWORD hideLater)
    {
        wchar_t buffer[16] = {};
        int count = 200;
        if (::GetEnvironmentVariableW(L"MANIFOLD_STRESS_THREADS", buffer, 16) != 0)
            count = static_cast<int>(::wcstol(buffer, nullptr, 10));
        const bool churn = ::GetEnvironmentVariableW(L"MANIFOLD_NO_CHURN", nullptr, 0) == 0;
        wchar_t sleepBuf[16] = {};
        if (::GetEnvironmentVariableW(L"MANIFOLD_STRESS_SLEEP", sleepBuf, 16) != 0)
            g_stressSleep = static_cast<int>(::wcstol(sleepBuf, nullptr, 10));
        for (int i = 0; i < count; ++i)
            ::CloseHandle(::CreateThread(nullptr, 0, &StressWorker, i % 2 ? reinterpret_cast<void*>(1) : nullptr, 0, nullptr));
        if (churn)
            ::CloseHandle(::CreateThread(nullptr, 0, &Churn, nullptr, 0, nullptr));
        ::CloseHandle(::CreateThread(nullptr, 0, &KernelWriter, nullptr, 0, nullptr));
        if (hidden)
            ::CloseHandle(::CreateThread(nullptr, 0, &HiddenWorker, nullptr, 0, nullptr));
        if (hideLater)
            ::CloseHandle(::CreateThread(nullptr, 0, &HiddenWorker, reinterpret_cast<void*>(static_cast<ULONG_PTR>(hideLater)), 0, nullptr));
    }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--solo") == 0)
        {
            g_stressSleep = 2;
            ::CloseHandle(::CreateThread(nullptr, 0, &StressWorker, nullptr, 0, nullptr));
            std::printf("solo: one thread touching %p every 2ms\n", static_cast<void*>(const_cast<long long*>(&g_stress[0])));
            std::fflush(stdout);
            for (;;)
                ::Sleep(1000);
        }
    }
    for (int i = 0; i < 6; ++i)
        ::CloseHandle(::CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr));
    bool stress = false;
    bool hidden = false;
    bool hiddenMain = false;
    DWORD hideLater = 0;
    for (int i = 1; i < argc; ++i)
    {
        stress = stress || std::strcmp(argv[i], "--stress") == 0;
        hidden = hidden || std::strcmp(argv[i], "--hidden") == 0;
        hiddenMain = hiddenMain || std::strcmp(argv[i], "--hidden-main") == 0;
        if (std::strcmp(argv[i], "--hidden-late") == 0)
            hideLater = 3000;
    }
    if (stress)
        StartStress(hidden, hideLater);
    if (hiddenMain)
        HideCurrentThread();
    ::SetConsoleTitleW(L"Manifold Test Target");
    g_world = new World();
    g_world->Hero = new Player();
    std::printf("Manifold test target, pid %lu\n", ::GetCurrentProcessId());
    std::printf("g_world at %p, hero at %p, health at %p\n", static_cast<void*>(&g_world), static_cast<void*>(g_world->Hero), static_cast<void*>(&g_world->Hero->Health));
    std::printf("damage at %p, readhealth at %p, shoot at %p\n", reinterpret_cast<void*>(&Damage), reinterpret_cast<void*>(&ReadHealth), reinterpret_cast<void*>(&Shoot));
    std::fflush(stdout);
    for (;;)
    {
        if (hiddenMain)
        {
            for (int i = 0; i < 250; ++i)
            {
                Touch(&g_stress[0]);
                ::Sleep(2);
            }
        }
        else
        {
            ::Sleep(500);
        }
        g_ticks = g_ticks + 1;
        if (g_ticks % 2 == 0)
            Damage(g_world->Hero, 1);
        Shoot(g_world->Hero);
        const int health = ReadHealth(g_world->Hero);
        if (g_ticks % 4 == 0)
            std::printf("tick %d: health %d, ammo %d, speed %.2f, gold %.1f, name %s\n", g_ticks, health, g_world->Hero->Ammo, g_world->Hero->Speed, g_world->Hero->Gold,
                g_world->Hero->Name);
        std::fflush(stdout);
    }
}
