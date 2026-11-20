#include "FakeRtti.h"
#include "FakeUnreal.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

extern "C" __declspec(dllexport) __declspec(noinline) int MixedCall(int a, double b, float c, int d, int e, double f)
{
    return a + static_cast<int>(b) + static_cast<int>(c * 2.0f) + d * 10 + e * 100 + static_cast<int>(f * 1000.0);
}

struct CallProbe
{
    virtual int Add(int add, int more);
    int Base = 40;
};

__declspec(noinline) int CallProbe::Add(int add, int more)
{
    return Base + add + more * 3;
}

CallProbe g_callProbeObject;
extern "C" __declspec(dllexport) CallProbe* g_callProbe = &g_callProbeObject;

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
    struct FakeClass;
    struct FakeField { const char* Name; int Offset; int TypeCode; int Flags; const FakeClass* TypeClass = nullptr; };
    struct FakeImage;
    // Image comes after the name, like MonoClass::image in the real runtime;
    // the explorer learns both offsets from mono_class_get_image/_get_name.
    struct FakeClass { const char* Namespace; const char* Name; const FakeMethod* Methods; int MethodCount; const FakeField* Fields; int FieldCount; const FakeImage* Image = nullptr;
        bool ValueType = false; int ValueSize = 0; int EnumBase = 0; };
    struct FakeImage { const char* Name; const FakeClass* Classes; int ClassCount; };
    extern const FakeImage kImage;

    long long FakeAdd(long long a) { return a + 1; }
    long long FakeSub(long long a) { return a - 1; }

    const FakeMethod kPlayerMethods[] = { { "TakeDamage", reinterpret_cast<void*>(&FakeAdd) }, { "Heal", reinterpret_cast<void*>(&FakeSub) } };
    // health and ammo are instance I4 fields; s_instances is static and must be skipped.
    const FakeField kPlayerFields[] = { { "health", 0x10, 0x08, 0 }, { "ammo", 0x1C, 0x08, 0 }, { "s_instances", 0x00, 0x12, 0x10 } };
    const FakeMethod kGameMethods[] = { { "Update", reinterpret_cast<void*>(&FakeAdd) } };
    const FakeClass kClasses[] = {
        { "Game", "Player", kPlayerMethods, 2, kPlayerFields, 3, &kImage },
        { "Game", "GameManager", kGameMethods, 1, nullptr, 0, &kImage },
    };
    const FakeImage kImage = { "Assembly-CSharp", kClasses, 2 };

    extern const FakeImage kValuesImage;
    const FakeField kVec3Fields[] = { { "x", 0x10, 0x0C, 0 }, { "y", 0x14, 0x0C, 0 }, { "z", 0x18, 0x0C, 0 } };
    const FakeField kTeamFields[] = { { "value__", 0x10, 0x08, 0 } };
    extern const FakeClass kValueClasses[];
    const FakeField kMoverFields[] = { { "speed", 0x10, 0x0C, 0 }, { "team", 0x20, 0x11, 0, &kValueClasses[1] }, { "position", 0x14, 0x11, 0, &kValueClasses[0] }, { "owner", 0x28, 0x12, 0 } };
    const FakeClass kValueClasses[] = {
        { "Game", "Vec3", nullptr, 0, kVec3Fields, 3, &kValuesImage, true, 12, 0 },
        { "Game", "Team", nullptr, 0, kTeamFields, 1, &kValuesImage, true, 4, 0x08 },
        { "Game", "Mover", nullptr, 0, kMoverFields, 4, &kValuesImage },
    };
    const FakeImage kValuesImage = { "Assembly-Values", kValueClasses, 3 };
    bool g_monoValues = false;
    // A class the image does not list, like a generic instance the runtime
    // makes on demand: the explorer may only accept it by its shape.
    const FakeClass kListClass = { "System.Collections.Generic", "List`1", nullptr, 0, nullptr, 0, &kImage };
    // Readable memory that is not a class (no image inside).
    const unsigned long long kNotAClass[8] = { 0x4141414141414141ull, 0x4242424242424242ull, 0x4343434343434343ull, 0x4444444444444444ull,
                                               0x4545454545454545ull, 0x4646464646464646ull, 0x4747474747474747ull, 0x4848484848484848ull };
    int g_domain = 0xD0;

    struct FakeVTable { const FakeClass* Klass; };
    FakeVTable g_playerVT = { &kClasses[0] };
    FakeVTable g_listVT = { &kListClass };
    // What a collected object's memory can look like: a "class" pointer that
    // is no address at all (the value from the Content Warning crash), and
    // one that points at readable memory holding no class.
    FakeVTable g_wildVT = { reinterpret_cast<const FakeClass*>(0x01000001A9DE141Dull) };
    FakeVTable g_notClassVT = { reinterpret_cast<const FakeClass*>(kNotAClass) };
    struct FakeObject { FakeVTable* VTable; int Health; int Ammo; };

    // A tiny JIT-info table so MethodNameAt (reverse resolution) has something to
    // find: the code of FakeAdd (= Game.Player:TakeDamage's compiled address)
    // maps back to that method. mono_jit_info_table_find reports null outside the
    // range, exactly like the real runtime off the JIT code.
    struct FakeJitInfo { const FakeMethod* Method; void* CodeStart; int CodeSize; };
    const FakeJitInfo kTakeDamageJit = { &kPlayerMethods[0], reinterpret_cast<void*>(&FakeAdd), 0x20 };
    const FakeJitInfo* const kJitTable[] = { &kTakeDamageJit };

    struct SyntheticModel
    {
        std::vector<std::string> Names;
        std::vector<FakeClass> Classes;
        FakeImage Image = { "Assembly-Synthetic", nullptr, 0 };
    };

    const SyntheticModel& Synthetic()
    {
        static const SyntheticModel model = []
            {
                SyntheticModel built;
                wchar_t buffer[16] = L"";
                if (::GetEnvironmentVariableW(L"MANIFOLD_FAKE_MONO_CLASSES", buffer, 16) == 0)
                    return built;
                const int count = std::clamp(_wtoi(buffer), 0, 200000);
                built.Names.reserve(static_cast<std::size_t>(count));
                for (int i = 0; i < count; ++i)
                    built.Names.push_back("Generated" + std::to_string(i));
                for (const std::string& name : built.Names)
                    built.Classes.push_back({ "Synthetic", name.c_str(), nullptr, 0, nullptr, 0 });
                built.Image.Classes = built.Classes.data();
                built.Image.ClassCount = count;
                return built;
            }();
        return model;
    }
}

extern "C" __declspec(dllexport) FakeObject g_fakePlayer = { &g_playerVT, 1000, 30 };
extern "C" __declspec(dllexport) FakeObject g_fakeList = { &g_listVT, 0, 0 };
extern "C" __declspec(dllexport) FakeObject g_fakeWild = { &g_wildVT, 0, 0 };
extern "C" __declspec(dllexport) FakeObject g_fakeNotClass = { &g_notClassVT, 0, 0 };

struct FakeAgentStats
{
    long Loads;
    long Begins;
    long Releases;
    long ImageBytes;
    long Live;
    long Cancels;
};
extern "C" __declspec(dllexport) FakeAgentStats g_agentStats = {};

// With --il2cpp the target plays an IL2CPP game instead (see below): the fake
// Mono then has no root domain, so the bridge moves on to the il2cpp_* exports.
namespace
{
    bool g_il2cpp = false;
}

namespace
{
    enum class AgentMode
    {
        Off,
        Good,
        BadHandles,
        BadOffsets,
        Unsure,
        Fail
    };
    AgentMode g_agentMode = AgentMode::Off;

    const FakeMethod kAgentMethods[] = { { "Begin", nullptr }, { "Release", nullptr } };
    const FakeField kAgentFields[] = { { "State", 0, 0x08, 0x10 }, { "Types", 0, 0x08, 0x10 }, { "Cancel", 0, 0x08, 0x10 }, { "Data", 0, 0x18, 0x10 }, { "Size", 0, 0x08, 0x10 } };
    extern const FakeImage kAgentImage;
    const FakeClass kAgentClass = { "Manifold.Managed", "Dumper", kAgentMethods, 2, kAgentFields, 5, &kAgentImage };
    const FakeImage kAgentImage = { "ManifoldDumper3", &kAgentClass, 1 };
    FakeVTable g_agentVT = { &kAgentClass };
    long long g_agentBoxed[4] = {};
    bool g_agentLoaded = false;

    struct AgentStatics
    {
        volatile long State = 0;
        volatile long Types = 0;
        volatile long Cancel = 0;
        void* volatile Data = nullptr;
        volatile long Size = 0;
    };
    AgentStatics g_agentStatics;

    class BlobWriter
    {
    public:
        std::uint32_t Text(const std::string& text)
        {
            for (std::size_t i = 0; i < m_strings.size(); ++i)
            {
                if (m_strings[i] == text)
                    return static_cast<std::uint32_t>(i);
            }
            m_strings.push_back(text);
            return static_cast<std::uint32_t>(m_strings.size() - 1);
        }
        void U32(std::uint32_t value) { Put(m_types, &value, sizeof(value)); }
        void I32(std::int32_t value) { Put(m_types, &value, sizeof(value)); }
        void U64(std::uint64_t value) { Put(m_types, &value, sizeof(value)); }

        std::vector<unsigned char> Finish(std::uint32_t flags, std::uint32_t types) const
        {
            std::vector<unsigned char> out;
            const std::uint32_t header[] = { 0x4D44464Du, 2u, flags, static_cast<std::uint32_t>(m_strings.size()) };
            Put(out, header, sizeof(header));
            for (const std::string& text : m_strings)
            {
                const std::uint32_t length = static_cast<std::uint32_t>(text.size());
                Put(out, &length, sizeof(length));
                Put(out, text.data(), text.size());
            }
            Put(out, &types, sizeof(types));
            out.insert(out.end(), m_types.begin(), m_types.end());
            return out;
        }

    private:
        static void Put(std::vector<unsigned char>& to, const void* data, std::size_t size)
        {
            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            to.insert(to.end(), bytes, bytes + size);
        }

        std::vector<std::string> m_strings;
        std::vector<unsigned char> m_types;
    };

    std::vector<unsigned char> BuildAgentBlob()
    {
        BlobWriter writer;
        std::uint32_t total = 0;
        const auto write = [&](const FakeImage& image, bool main)
            {
                const std::uint32_t count = static_cast<std::uint32_t>(image.ClassCount);
                for (std::uint32_t c = 0; c < count; ++c)
                {
                    const FakeClass& klass = image.Classes[c];
                    const FakeClass* handle = g_agentMode == AgentMode::BadHandles ? &image.Classes[(c + 1) % count] : &klass;
                    const bool nested = main && c == 1;
                    writer.U32(writer.Text(image.Name));
                    writer.U32(writer.Text(nested ? "" : klass.Namespace));
                    writer.U32(writer.Text(klass.Name));
                    writer.U32(writer.Text(main ? "UnityEngine.MonoBehaviour" : "System.ValueType"));
                    writer.U32(0x02000001u + c);
                    writer.U64(reinterpret_cast<std::uint64_t>(handle));
                    writer.U32(nested ? 32u : 0u);
                    writer.U32(static_cast<std::uint32_t>(klass.FieldCount));
                    for (int f = 0; f < klass.FieldCount; ++f)
                    {
                        const FakeField& field = klass.Fields[f];
                        writer.U32(writer.Text(field.Name));
                        writer.I32(g_agentMode == AgentMode::BadOffsets ? field.Offset + 0x99 : field.Offset);
                        writer.U32((field.Flags & 0x10) ? 1u : 0u);
                        writer.U32(static_cast<std::uint32_t>(field.TypeCode));
                    }
                    writer.U32(static_cast<std::uint32_t>(klass.MethodCount));
                    for (int m = 0; m < klass.MethodCount; ++m)
                    {
                        writer.U32(writer.Text(klass.Methods[m].Name));
                        writer.U64(reinterpret_cast<std::uint64_t>(&klass.Methods[m]));
                        writer.U32(0);
                        writer.I32(1);
                    }
                }
                total += count;
            };
        write(kImage, true);
        if (g_monoValues)
            write(kValuesImage, false);
        return writer.Finish(g_agentMode == AgentMode::Unsure ? 1u : 3u, total);
    }

    DWORD WINAPI AgentWorker(void*)
    {
        ::Sleep(120);
        if (g_agentStatics.Cancel || g_agentMode == AgentMode::Fail)
        {
            g_agentStatics.State = 3;
            return 0;
        }
        const std::vector<unsigned char> blob = BuildAgentBlob();
        void* data = ::HeapAlloc(::GetProcessHeap(), 0, blob.size());
        if (!data)
        {
            g_agentStatics.State = 3;
            return 0;
        }
        std::memcpy(data, blob.data(), blob.size());
        ::InterlockedIncrement(&g_agentStats.Live);
        g_agentStatics.Data = data;
        g_agentStatics.Size = static_cast<long>(blob.size());
        g_agentStatics.Types = kImage.ClassCount + (g_monoValues ? kValuesImage.ClassCount : 0);
        g_agentStatics.State = 2;
        return 0;
    }

    void AgentBegin()
    {
        if (g_agentStatics.State == 1)
            return;
        ::InterlockedIncrement(&g_agentStats.Begins);
        g_agentStatics.Cancel = 0;
        g_agentStatics.Types = 0;
        g_agentStatics.State = 1;
        ::CloseHandle(::CreateThread(nullptr, 0, &AgentWorker, nullptr, 0, nullptr));
    }

    void AgentRelease()
    {
        ::InterlockedIncrement(&g_agentStats.Releases);
        if (g_agentStatics.State == 1)
            return;
        if (void* data = g_agentStatics.Data)
        {
            g_agentStatics.Data = nullptr;
            ::HeapFree(::GetProcessHeap(), 0, data);
            ::InterlockedDecrement(&g_agentStats.Live);
        }
        g_agentStatics.Size = 0;
        g_agentStatics.State = 0;
    }
}

extern "C"
{
    __declspec(dllexport) void* mono_image_open_from_data(char* data, unsigned int length, int needCopy, int* status)
    {
        static const char kName[] = "ManifoldDumper";
        const bool dumper = g_agentMode != AgentMode::Off && data && needCopy && length > 0x200 && data[0] == 'M' && data[1] == 'Z'
            && std::search(data, data + length, kName, kName + sizeof(kName) - 1) != data + length;
        if (status)
            *status = dumper ? 0 : 3;
        if (!dumper)
            return nullptr;
        g_agentStats.ImageBytes = static_cast<long>(length);
        return const_cast<FakeImage*>(&kAgentImage);
    }
    __declspec(dllexport) void* mono_assembly_load_from_full(void* image, const char* path, int* status, int)
    {
        const bool ok = image == &kAgentImage && path && std::strstr(path, "ManifoldDumper") != nullptr;
        if (status)
            *status = ok ? 0 : 3;
        if (!ok)
            return nullptr;
        g_agentLoaded = true;
        ::InterlockedIncrement(&g_agentStats.Loads);
        return image;
    }
    __declspec(dllexport) void* mono_class_from_name(void* image, const char* space, const char* name)
    {
        const FakeImage* img = static_cast<const FakeImage*>(image);
        for (int i = 0; img && space && name && i < img->ClassCount; ++i)
        {
            if (std::strcmp(img->Classes[i].Namespace, space) == 0 && std::strcmp(img->Classes[i].Name, name) == 0)
                return const_cast<FakeClass*>(&img->Classes[i]);
        }
        return nullptr;
    }
    __declspec(dllexport) void* mono_class_get_method_from_name(void* klass, const char* name, int)
    {
        const FakeClass* c = static_cast<const FakeClass*>(klass);
        for (int i = 0; c && name && i < c->MethodCount; ++i)
        {
            if (std::strcmp(c->Methods[i].Name, name) == 0)
                return const_cast<FakeMethod*>(&c->Methods[i]);
        }
        return nullptr;
    }
    __declspec(dllexport) void* mono_class_get_field_from_name(void* klass, const char* name)
    {
        const FakeClass* c = static_cast<const FakeClass*>(klass);
        for (int i = 0; c && name && i < c->FieldCount; ++i)
        {
            if (std::strcmp(c->Fields[i].Name, name) == 0)
                return const_cast<FakeField*>(&c->Fields[i]);
        }
        return nullptr;
    }
    __declspec(dllexport) void* mono_class_vtable(void*, void* klass) { return klass == &kAgentClass ? &g_agentVT : nullptr; }
    __declspec(dllexport) void* mono_runtime_invoke(void* method, void*, void**, void** exception)
    {
        if (exception)
            *exception = nullptr;
        if (method == &kAgentMethods[0])
            AgentBegin();
        else if (method == &kAgentMethods[1])
            AgentRelease();
        else
            return nullptr;
        g_agentBoxed[2] = g_agentStatics.State;
        return g_agentBoxed;
    }
    __declspec(dllexport) void mono_field_static_get_value(void* vtable, void* field, void* value)
    {
        if (vtable != &g_agentVT || !value)
            return;
        if (field == &kAgentFields[0])
            *static_cast<std::int32_t*>(value) = g_agentStatics.State;
        else if (field == &kAgentFields[1])
            *static_cast<std::int32_t*>(value) = g_agentStatics.Types;
        else if (field == &kAgentFields[2])
            *static_cast<std::int32_t*>(value) = g_agentStatics.Cancel;
        else if (field == &kAgentFields[3])
            *static_cast<void**>(value) = g_agentStatics.Data;
        else if (field == &kAgentFields[4])
            *static_cast<std::int32_t*>(value) = g_agentStatics.Size;
    }
    __declspec(dllexport) void mono_field_static_set_value(void* vtable, void* field, void* value)
    {
        if (vtable != &g_agentVT || !value || field != &kAgentFields[2])
            return;
        g_agentStatics.Cancel = *static_cast<std::int32_t*>(value);
        ::InterlockedIncrement(&g_agentStats.Cancels);
    }
}


extern "C"
{
    __declspec(dllexport) void* mono_get_root_domain() { return g_il2cpp ? nullptr : &g_domain; }
    __declspec(dllexport) void* mono_thread_attach(void* domain) { return domain; }
    __declspec(dllexport) void mono_assembly_foreach(void (*fn)(void*, void*), void* user)
    {
        fn(const_cast<FakeImage*>(&kImage), user);
        if (g_monoValues)
            fn(const_cast<FakeImage*>(&kValuesImage), user);
        if (Synthetic().Image.ClassCount > 0)
            fn(const_cast<FakeImage*>(&Synthetic().Image), user);
        if (g_agentLoaded)
            fn(const_cast<FakeImage*>(&kAgentImage), user);
    }
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
    __declspec(dllexport) void* mono_class_get_image(void* klass) { return const_cast<FakeImage*>(static_cast<FakeClass*>(klass)->Image); }
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
    __declspec(dllexport) void* mono_class_from_mono_type(void* type)
    {
        const FakeField* field = reinterpret_cast<const FakeField*>(static_cast<const char*>(type) - offsetof(FakeField, TypeCode));
        return const_cast<FakeClass*>(field->TypeClass);
    }
    __declspec(dllexport) int mono_class_is_valuetype(void* klass) { return static_cast<FakeClass*>(klass)->ValueType ? 1 : 0; }
    __declspec(dllexport) int mono_class_is_enum(void* klass) { return static_cast<FakeClass*>(klass)->EnumBase != 0 ? 1 : 0; }
    __declspec(dllexport) void* mono_class_enum_basetype(void* klass) { return &static_cast<FakeClass*>(klass)->EnumBase; }
    __declspec(dllexport) int mono_class_value_size(void* klass, unsigned int* align)
    {
        if (align)
            *align = 4;
        return static_cast<FakeClass*>(klass)->ValueSize;
    }
    __declspec(dllexport) int mono_field_get_flags(void* field) { return static_cast<FakeField*>(field)->Flags; }
    __declspec(dllexport) void* mono_object_get_class(void* object) { return const_cast<FakeClass*>(static_cast<FakeObject*>(object)->VTable->Klass); }
    __declspec(dllexport) void* mono_jit_info_table_find(void* /*domain*/, void* addr)
    {
        const std::size_t a = reinterpret_cast<std::size_t>(addr);
        for (const FakeJitInfo* ji : kJitTable)
        {
            const std::size_t start = reinterpret_cast<std::size_t>(ji->CodeStart);
            if (a >= start && a < start + static_cast<std::size_t>(ji->CodeSize))
                return const_cast<FakeJitInfo*>(ji);
        }
        return nullptr;
    }
    __declspec(dllexport) void* mono_jit_info_get_method(void* ji) { return const_cast<FakeMethod*>(static_cast<FakeJitInfo*>(ji)->Method); }
    __declspec(dllexport) void* mono_jit_info_get_code_start(void* ji) { return static_cast<FakeJitInfo*>(ji)->CodeStart; }
    __declspec(dllexport) int mono_jit_info_get_code_size(void* ji) { return static_cast<FakeJitInfo*>(ji)->CodeSize; }
    __declspec(dllexport) void* mono_method_get_class(void* method)
    {
        for (const FakeClass& c : kClasses)
            for (int i = 0; i < c.MethodCount; ++i)
                if (static_cast<const void*>(&c.Methods[i]) == method)
                    return const_cast<FakeClass*>(&c);
        return nullptr;
    }
}

// A stand-in for IL2CPP over a static model, with the struct layouts the
// bridge relies on and nothing else: an Il2CppObject starts with its
// Il2CppClass*, Il2CppClass starts { image, gc_desc, name, namespaze },
// MethodInfo starts { methodPointer, virtualMethodPointer, invoker, name }
// (Unity 2021.2+), and the method and field iterators walk arrays the way
// the real ones do (the iterator holds a pointer into the array, not an
// index). There is no il2cpp_method_get_pointer, like in Unity's own API, so
// the bridge reads methodPointer at offset 0.
namespace
{
    struct FakeIl2CppImage;
    struct FakeIl2CppClass;
    struct FakeIl2CppType { int Type; const FakeIl2CppClass* Klass = nullptr; bool ByRef = false; };
    struct FakeFieldInfo { const char* Name; const FakeIl2CppType* Type; const FakeIl2CppClass* Parent; int Offset; unsigned Token; int Flags; long long Literal = 0; };
    struct FakeMethodInfo
    {
        void* MethodPointer;
        void* VirtualMethodPointer;
        void* Invoker;
        const char* Name;
        const FakeIl2CppClass* Klass;
        void* ReturnType;
        void* Parameters;
        unsigned ParamCount;
        unsigned Flags = 0;
        const FakeIl2CppType* const* ParamTypes = nullptr;
        const char* const* ParamNames = nullptr;
        bool Generic = false;
    };
    struct FakePropertyInfo { const char* Name; const FakeMethodInfo* Get; const FakeMethodInfo* Set; const FakeIl2CppClass* Parent; };
    struct FakeIl2CppClass
    {
        const FakeIl2CppImage* Image;
        void* GcDesc;
        const char* Name;
        const char* Namespace;
        const FakeIl2CppClass* Parent;
        const FakeMethodInfo* const* Methods;
        unsigned MethodCount;
        const FakeFieldInfo* Fields;
        unsigned FieldCount;
        bool Generic;
        bool ValueType = false;
        bool Enum = false;
        int ValueSize = 0;
        int Rank = 0;
        const FakeIl2CppClass* Element = nullptr;
        void* StaticData = nullptr;
        const FakePropertyInfo* Properties = nullptr;
        unsigned PropertyCount = 0;
        FakeIl2CppType ByVal = { 0x12 };
        const FakeIl2CppType* EnumBase = nullptr;
    };
    struct FakeIl2CppImage { const char* Name; const FakeIl2CppClass* Classes; unsigned ClassCount; };
    struct FakeIl2CppAssembly { const FakeIl2CppImage* Image; };
    struct FakeIl2CppObject { const FakeIl2CppClass* Klass; void* Monitor; int Id; int Pad; int Health; int Ammo; float Speed; };

    // Distinct bodies, so the linker cannot fold them into one function.
    __declspec(noinline) long long Il2CppTakeDamage(long long a) { return a * 3 + 7; }
    __declspec(noinline) long long Il2CppHeal(long long a) { return a * 5 - 11; }
    __declspec(noinline) long long Il2CppHealMore(long long a, long long b) { return a * 7 + b * 13; }
    __declspec(noinline) long long Il2CppUpdate(long long a) { return (a ^ 0x55) + 17; }
    __declspec(noinline) long long Il2CppToString(long long a) { return (a << 3) - 19; }
    // One body for two getters, the way the compiler folds identical methods.
    __declspec(noinline) long long Il2CppGetter(long long a) { return (a >> 2) + 23; }

    extern const FakeIl2CppImage kCorlib;
    extern const FakeIl2CppImage kGameImage;
    extern const FakeIl2CppClass kCorlibClasses[];
    extern const FakeIl2CppClass kGameClasses[];
    const FakeIl2CppType kI4 = { 0x08 };
    const FakeIl2CppType kR4 = { 0x0C };
    const FakeIl2CppType kClassType = { 0x12 };

    const FakeMethodInfo kObjectToString = { reinterpret_cast<void*>(&Il2CppToString), nullptr, nullptr, "ToString", &kCorlibClasses[1], nullptr, nullptr, 0 };
    const FakeMethodInfo* const kObjectMethods[] = { &kObjectToString };
    // Open generic methods have no code of their own.
    const FakeMethodInfo kListAdd = { nullptr, nullptr, nullptr, "Add", &kCorlibClasses[2], nullptr, nullptr, 1 };
    const FakeMethodInfo* const kListMethods[] = { &kListAdd };
    const FakeFieldInfo kListFields[] = { { "_items", &kClassType, &kCorlibClasses[2], 0x10, 0, 0 } };

    const FakeFieldInfo kActorFields[] = { { "id", &kI4, &kGameClasses[1], 0x10, 0, 0 } };
    const FakeMethodInfo kTakeDamage = { reinterpret_cast<void*>(&Il2CppTakeDamage), nullptr, nullptr, "TakeDamage", &kGameClasses[2], nullptr, nullptr, 1 };
    const FakeMethodInfo kHeal = { reinterpret_cast<void*>(&Il2CppHeal), nullptr, nullptr, "Heal", &kGameClasses[2], nullptr, nullptr, 1 };
    const FakeMethodInfo kHealMore = { reinterpret_cast<void*>(&Il2CppHealMore), nullptr, nullptr, "Heal", &kGameClasses[2], nullptr, nullptr, 2 };
    const FakeMethodInfo kGetHealth = { reinterpret_cast<void*>(&Il2CppGetter), nullptr, nullptr, "get_Health", &kGameClasses[2], nullptr, nullptr, 0 };
    const FakeMethodInfo* const kPlayerIl2CppMethods[] = { &kTakeDamage, &kHeal, &kHealMore, &kGetHealth };
    // health, ammo and speed belong to an instance; s_count is static and skipped.
    const FakeFieldInfo kPlayerIl2CppFields[] = {
        { "health", &kI4, &kGameClasses[2], 0x18, 0, 0 },
        { "ammo", &kI4, &kGameClasses[2], 0x1C, 0, 0 },
        { "speed", &kR4, &kGameClasses[2], 0x20, 0, 0 },
        { "s_count", &kI4, &kGameClasses[2], 0x00, 0, 0x10 },
    };
    const FakeMethodInfo kManagerUpdate = { reinterpret_cast<void*>(&Il2CppUpdate), nullptr, nullptr, "Update", &kGameClasses[3], nullptr, nullptr, 0 };
    const FakeMethodInfo kGetLevel = { reinterpret_cast<void*>(&Il2CppGetter), nullptr, nullptr, "get_Level", &kGameClasses[3], nullptr, nullptr, 0 };
    const FakeMethodInfo kOnEvent = { nullptr, nullptr, nullptr, "OnEvent", &kGameClasses[3], nullptr, nullptr, 1 };
    const FakeMethodInfo* const kManagerMethods[] = { &kManagerUpdate, &kGetLevel, &kOnEvent };
    const FakeFieldInfo kManagerFields[] = { { "level", &kI4, &kGameClasses[3], 0x10, 0, 0 } };

    const FakeIl2CppClass kCorlibClasses[] = {
        { &kCorlib, nullptr, "<Module>", "", nullptr, nullptr, 0, nullptr, 0, false },
        { &kCorlib, nullptr, "Object", "System", nullptr, kObjectMethods, 1, nullptr, 0, false },
        { &kCorlib, nullptr, "List`1", "System.Collections.Generic", &kCorlibClasses[1], kListMethods, 1, kListFields, 1, true },
    };
    const FakeIl2CppClass kGameClasses[] = {
        { &kGameImage, nullptr, "<Module>", "", nullptr, nullptr, 0, nullptr, 0, false },
        { &kGameImage, nullptr, "Actor", "Game", &kCorlibClasses[1], nullptr, 0, kActorFields, 1, false },
        { &kGameImage, nullptr, "Player", "Game", &kGameClasses[1], kPlayerIl2CppMethods, 4, kPlayerIl2CppFields, 4, false },
        { &kGameImage, nullptr, "GameManager", "Game", &kCorlibClasses[1], kManagerMethods, 3, kManagerFields, 1, false },
    };
    const FakeIl2CppImage kCorlib = { "mscorlib.dll", kCorlibClasses, 3 };
    const FakeIl2CppImage kGameImage = { "Assembly-CSharp.dll", kGameClasses, 4 };
    const FakeIl2CppAssembly kCorlibAssembly = { &kCorlib };
    const FakeIl2CppAssembly kGameAssembly = { &kGameImage };
    const FakeIl2CppAssembly* const kAssemblies[] = { &kCorlibAssembly, &kGameAssembly };
    // A generic instance (List<int>): the runtime makes these on demand, so no
    // image lists them; the bridge may name one only by its shape.
    const FakeIl2CppClass kListOfInt = { &kCorlib, nullptr, "List`1", "System.Collections.Generic", &kCorlibClasses[1], nullptr, 0, nullptr, 0, false };
    // Something that looks like a class but whose image is no image.
    const FakeIl2CppImage kStrayImage = { "Stray.dll", nullptr, 0 };
    const FakeIl2CppClass kStrayClass = { &kStrayImage, nullptr, "Stray", "", nullptr, nullptr, 0, nullptr, 0, false };

    int g_il2cppDomain = 0x1D;
    int g_il2cppThread = 0x7D;
}

#include "FakeIl2CppScene.h"

extern "C" __declspec(dllexport) FakeIl2CppObject g_il2cppPlayer = { &kGameClasses[2], nullptr, 42, 0, 1000, 30, 3.5f };
extern "C" __declspec(dllexport) FakeIl2CppObject g_il2cppList = { &kListOfInt, nullptr, 0, 0, 0, 0, 0.0f };
extern "C" __declspec(dllexport) FakeIl2CppObject g_il2cppWild = { reinterpret_cast<const FakeIl2CppClass*>(0x01000001A9DE141Dull), nullptr, 0, 0, 0, 0, 0.0f };
extern "C" __declspec(dllexport) FakeIl2CppObject g_il2cppNotClass = { reinterpret_cast<const FakeIl2CppClass*>(kNotAClass), nullptr, 0, 0, 0, 0, 0.0f };
extern "C" __declspec(dllexport) FakeIl2CppObject g_il2cppStray = { &kStrayClass, nullptr, 0, 0, 0, 0, 0.0f };
extern "C" __declspec(dllexport) volatile long g_il2cppAttached = 0;
extern "C" __declspec(dllexport) volatile long g_il2cppDetached = 0;

extern "C"
{
    __declspec(dllexport) const void* il2cpp_get_corlib() { return g_il2cpp ? &kCorlib : nullptr; }
    __declspec(dllexport) void* il2cpp_domain_get() { return g_il2cpp ? &g_il2cppDomain : nullptr; }
    __declspec(dllexport) void* il2cpp_thread_attach(void*)
    {
        ::InterlockedIncrement(&g_il2cppAttached);
        return &g_il2cppThread;
    }
    __declspec(dllexport) void il2cpp_thread_detach(void* thread)
    {
        if (thread == &g_il2cppThread)
            ::InterlockedIncrement(&g_il2cppDetached);
    }
    __declspec(dllexport) const FakeIl2CppAssembly* const* il2cpp_domain_get_assemblies(const void*, std::size_t* size)
    {
        if (g_scene)
        {
            *size = sizeof(g_sceneAssemblies) / sizeof(g_sceneAssemblies[0]);
            return g_sceneAssemblies;
        }
        *size = sizeof(kAssemblies) / sizeof(kAssemblies[0]);
        return kAssemblies;
    }
    __declspec(dllexport) const FakeIl2CppImage* il2cpp_assembly_get_image(const FakeIl2CppAssembly* assembly) { return assembly->Image; }
    __declspec(dllexport) const char* il2cpp_image_get_name(const FakeIl2CppImage* image) { return image->Name; }
    __declspec(dllexport) std::size_t il2cpp_image_get_class_count(const FakeIl2CppImage* image) { return image->ClassCount; }
    __declspec(dllexport) const FakeIl2CppClass* il2cpp_image_get_class(const FakeIl2CppImage* image, std::size_t index)
    {
        return index < image->ClassCount ? &image->Classes[index] : nullptr;
    }
    __declspec(dllexport) const char* il2cpp_class_get_name(const FakeIl2CppClass* klass) { return klass->Name; }
    __declspec(dllexport) const char* il2cpp_class_get_namespace(const FakeIl2CppClass* klass) { return klass->Namespace; }
    __declspec(dllexport) const FakeIl2CppClass* il2cpp_class_get_parent(const FakeIl2CppClass* klass) { return klass->Parent; }
    __declspec(dllexport) const FakeIl2CppImage* il2cpp_class_get_image(const FakeIl2CppClass* klass) { return klass->Image; }
    __declspec(dllexport) bool il2cpp_class_is_generic(const FakeIl2CppClass* klass) { return klass->Generic; }
    __declspec(dllexport) const FakeMethodInfo* il2cpp_class_get_methods(const FakeIl2CppClass* klass, void** iter)
    {
        if (!iter || !klass->MethodCount)
            return nullptr;
        const FakeMethodInfo* const* slot = static_cast<const FakeMethodInfo* const*>(*iter);
        slot = slot ? slot + 1 : klass->Methods;
        if (slot >= klass->Methods + klass->MethodCount)
            return nullptr;
        *iter = const_cast<void*>(static_cast<const void*>(slot));
        return *slot;
    }
    __declspec(dllexport) const char* il2cpp_method_get_name(const FakeMethodInfo* method) { return method->Name; }
    __declspec(dllexport) unsigned il2cpp_method_get_param_count(const FakeMethodInfo* method) { return method->ParamCount; }
    __declspec(dllexport) const FakeFieldInfo* il2cpp_class_get_fields(const FakeIl2CppClass* klass, void** iter)
    {
        if (!iter || !klass->FieldCount)
            return nullptr;
        const FakeFieldInfo* field = static_cast<const FakeFieldInfo*>(*iter);
        field = field ? field + 1 : klass->Fields;
        if (field >= klass->Fields + klass->FieldCount)
            return nullptr;
        *iter = const_cast<void*>(static_cast<const void*>(field));
        return field;
    }
    __declspec(dllexport) const char* il2cpp_field_get_name(const FakeFieldInfo* field) { return field->Name; }
    __declspec(dllexport) std::size_t il2cpp_field_get_offset(const FakeFieldInfo* field) { return static_cast<std::size_t>(field->Offset); }
    __declspec(dllexport) const FakeIl2CppType* il2cpp_field_get_type(const FakeFieldInfo* field) { return field->Type; }
    __declspec(dllexport) int il2cpp_field_get_flags(const FakeFieldInfo* field) { return field->Flags; }
    __declspec(dllexport) int il2cpp_type_get_type(const FakeIl2CppType* type) { return type->Type; }
    // Not part of IL2CPP: a peek function for the remote-call drain test.
    __declspec(dllexport) void* fake_method_pointer(const FakeMethodInfo* method) { return method->MethodPointer; }
}

extern "C" __declspec(dllexport) __declspec(noinline) long long Touch(volatile long long* value)
{
    *value = *value + 1;
    return *value;
}

extern "C" __declspec(dllexport) volatile long g_faultsOn = 0;
extern "C" __declspec(dllexport) volatile long g_faultsHandled = 0;

// An access violation the program handles itself, for the debugger's
// "break on unexpected exceptions": writing to address 0x10 always faults,
// and the program's own vectored handler (added last, after any debugger's)
// steps over the store. Not __try: clang-cl drops an __except around a plain
// store without /EHa.
extern "C" __declspec(dllexport) __declspec(noinline) void Fault()
{
    *reinterpret_cast<volatile int*>(static_cast<std::uintptr_t>(0x10)) = 1;
}

namespace
{
    LONG CALLBACK SkipFault(EXCEPTION_POINTERS* info)
    {
        const EXCEPTION_RECORD& record = *info->ExceptionRecord;
        if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 || record.ExceptionInformation[1] != 0x10)
            return EXCEPTION_CONTINUE_SEARCH;
#if defined(_M_X64)
        const unsigned char* at = reinterpret_cast<const unsigned char*>(info->ContextRecord->Rip);
        if (at[0] != 0xC7 || at[1] != 0x04 || at[2] != 0x25)
            return EXCEPTION_CONTINUE_SEARCH;
        info->ContextRecord->Rip += 11;
#else
        const unsigned char* at = reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(info->ContextRecord->Eip));
        if (at[0] != 0xC7 || at[1] != 0x05)
            return EXCEPTION_CONTINUE_SEARCH;
        info->ContextRecord->Eip += 10;
#endif
        ::InterlockedIncrement(&g_faultsHandled);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

namespace
{
    DWORD WINAPI Worker(void*)
    {
        HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (unsigned round = 0;; ++round)
        {
            Work(&g_shared);
            if (g_faultsOn && round % 64 == 0)
                Fault();
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

struct UnrealMarker
{
    char Tag[8];
    void* Target;
};

extern "C" __declspec(dllexport) FakeUnreal::Image g_unrealFake = {};
extern "C" __declspec(dllexport) FakeRtti::Image g_rttiFake = {};
extern "C" __declspec(dllexport) UnrealMarker g_unrealNames = { { 'M', 'F', 'U', 'E', 'P', 'O', 'O', 'L' }, &FakeUnreal::g_pool };
extern "C" __declspec(dllexport) UnrealMarker g_unrealObjects = { { 'M', 'F', 'U', 'E', 'O', 'B', 'J', 'S' }, &FakeUnreal::g_objects };

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--il2cpp") == 0)
            g_il2cpp = true;
        else if (std::strcmp(argv[i], "--unreal") == 0)
        {
            static FakeUnreal::Builder unreal;
            if (unreal.Build())
                g_unrealFake = unreal.Fake;
        }
        else if (std::strcmp(argv[i], "--il2cpp-scene") == 0)
            g_scene = true;
        else if (std::strcmp(argv[i], "--il2cpp-alt") == 0)
            g_sceneAlt = true;
        else if (std::strcmp(argv[i], "--rtti") == 0)
        {
            static FakeRtti::Builder rtti;
            if (rtti.Build())
                g_rttiFake = rtti.Fake;
        }
        else if (std::strcmp(argv[i], "--mono-values") == 0)
            g_monoValues = true;
        else if (std::strcmp(argv[i], "--mono-agent") == 0)
            g_agentMode = AgentMode::Good;
        else if (std::strcmp(argv[i], "--mono-agent=bad-handles") == 0)
            g_agentMode = AgentMode::BadHandles;
        else if (std::strcmp(argv[i], "--mono-agent=bad-offsets") == 0)
            g_agentMode = AgentMode::BadOffsets;
        else if (std::strcmp(argv[i], "--mono-agent=unsure") == 0)
            g_agentMode = AgentMode::Unsure;
        else if (std::strcmp(argv[i], "--mono-agent=fail") == 0)
            g_agentMode = AgentMode::Fail;
    }
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
    ::AddVectoredExceptionHandler(0, &SkipFault);
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
    if (g_scene)
        BuildScene();
    ::SetConsoleTitleW(L"Manifold Test Target");
    g_world = new World();
    g_world->Hero = new Player();
    std::printf("Manifold test target, pid %lu\n", ::GetCurrentProcessId());
    std::printf("g_world at %p, hero at %p, health at %p\n", static_cast<void*>(&g_world), static_cast<void*>(g_world->Hero), static_cast<void*>(&g_world->Hero->Health));
    std::printf("damage at %p, readhealth at %p, shoot at %p\n", reinterpret_cast<void*>(&Damage), reinterpret_cast<void*>(&ReadHealth), reinterpret_cast<void*>(&Shoot));
    std::fflush(stdout);
    for (;;)
    {
        // Each tick lasts ~500 ms of wall time (the scan tests depend on that
        // cadence), bounded by the clock so a coarse Sleep granularity does not
        // stretch it.
        const unsigned long long until = ::GetTickCount64() + 500;
        do
        {
            if (hiddenMain)
                Touch(&g_stress[0]);
            if (g_scene && !g_il2cppPaused)
                SceneFrame();
            ::Sleep(2);
        } while (::GetTickCount64() < until);
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
