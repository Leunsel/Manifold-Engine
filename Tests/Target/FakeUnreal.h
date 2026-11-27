#pragma once

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace FakeUnreal
{
    struct Offsets
    {
        int Pointer;
        int Index;
        int Class;
        int Name;
        int Outer;
        int Super;
        int ChildProperties;
        int StructSize;
        int FieldClass;
        int FieldOwner;
        int FieldNext;
        int FieldName;
        int FieldDim;
        int FieldSize;
        int FieldOffset;
        int Extra;
        int EnumNames;
        int ItemSize;
        int ObjectBias;
    };

    inline constexpr Offsets kOffsets64 = { 8, 0x0C, 0x10, 0x18, 0x20, 0x40, 0x50, 0x58, 0x08, 0x10, 0x20, 0x28, 0x38, 0x3C, 0x4C, 0x78, 0x40, 0x18, 0 };
    inline constexpr Offsets kOffsets32 = { 4, 0x08, 0x0C, 0x10, 0x18, 0x28, 0x30, 0x34, 0x04, 0x08, 0x10, 0x14, 0x20, 0x24, 0x34, 0x50, 0x2C, 0x10, 4 };
    inline constexpr Offsets kNative = sizeof(void*) == 8 ? kOffsets64 : kOffsets32;

    inline constexpr int kObjectCount = 2100;
    inline constexpr std::size_t kObjectStride = 0x300;
    inline constexpr std::size_t kFieldStride = 0xA0;
    inline constexpr int kHidden = 0x58;
    inline constexpr int kRoot = 0x130;
    inline constexpr int kHealth = 0x200;
    inline constexpr int kTags = 0x208;
    inline constexpr int kLocation = 0x11C;
    inline constexpr int kActorSize = 0x220;

    struct Pool
    {
        std::uintptr_t Lock;
        std::uint32_t CurrentBlock;
        std::uint32_t Cursor;
        std::uintptr_t Blocks[8192];
    };

    struct Array
    {
        std::uintptr_t Objects;
        std::uintptr_t PreAllocated;
        std::int32_t Max;
        std::int32_t Num;
        std::int32_t MaxChunks;
        std::int32_t NumChunks;
    };

    template <class Word>
    struct ImageOf
    {
        Word Pool, Objects;
        Word Package, ObjectClass, FieldClass, StructClass, ClassClass, PackageClass, ScriptStructClass, EnumClass;
        Word Vector, Vector2D, LinearColor, IntPoint, Actor, SceneComponent, GameEngineClass, Mode;
        Word ActorInstance, SceneInstance, Engine, TagData, Text;
        Word BigClass, BigInstance, BigStruct;
    };

    using Image = ImageOf<std::uintptr_t>;

    struct Variant
    {
        bool Doubles = false;
        bool ReverseChain = false;
        bool ShadowField = false;
        bool NarrowEnum = false;
        bool SplitEnum = false;
        bool NoMaxEnum = false;
        // A class with kBigFields int properties, more rows than one Structure Dissect fill writes (issue #4).
        bool BigClass = false;
        // How many int properties the big chain has (more than UnrealRuntime::kMaxFields tests a cut chain).
        int BigFields = 7000;
        // The big chain belongs to the script struct BigStruct, and BigActor has one StructProperty Payload of it
        // at kBigFirst: a small class embedding one large struct.
        bool BigStruct = false;
        // The link after the middle property of the big chain is not a pointer: a chain that breaks.
        bool BrokenChain = false;
        // The last property of the big chain links back to the first: a chain that never ends.
        bool LoopChain = false;
        bool Containers = false;
    };

    inline constexpr int kLocationDouble = 0x120;
    inline constexpr int kLookup = 0x30;
    inline constexpr int kContainersEnd = 0xF8;
    inline constexpr int kBuffer = 0x140;
    inline constexpr int kBufferDim = 100;
    inline constexpr int kBigFields = 7000;
    inline constexpr int kBigFirst = 0x100;
    inline constexpr int kBigSize = kBigFirst + 4 * kBigFields;

    inline Pool g_pool;
    inline Array g_objects;
    inline const std::uintptr_t g_vtable[4] = { 0x11, 0x22, 0x33, 0x44 };

    class Builder
    {
    public:
        Builder() = default;
        Builder(const Builder&) = delete;
        Builder& operator=(const Builder&) = delete;

        ~Builder()
        {
            for (void* block : { m_names, m_objects, m_items, m_fields, m_bigFields, m_bigInstance })
            {
                if (block)
                    ::VirtualFree(block, 0, MEM_RELEASE);
            }
            std::memset(&g_pool, 0, sizeof(g_pool));
            std::memset(&g_objects, 0, sizeof(g_objects));
        }

        bool Build(const Variant& variant = {})
        {
            const Offsets& o = kNative;
            const int component = variant.Doubles ? 8 : 4;
            const int locationAt = variant.Doubles ? kLocationDouble : kLocation;
            m_names = Allocate(0x10000);
            m_objects = Allocate(kObjectCount * kObjectStride + 0x1000);
            m_items = Allocate(0x10000 * static_cast<std::size_t>(o.ItemSize) + 0x1000);
            m_fields = Allocate(0x10000);
            if (!m_names || !m_objects || !m_items || !m_fields)
                return false;
            Name("None");
            Name("ByteProperty");
            Name("IntProperty");

            const auto object = [&](int index, const char* name, std::uint32_t number = 0) { return Object(index, Name(name), number); };
            Fake.Package = object(0, "/Script/CoreUObject");
            Fake.ObjectClass = object(1, "Object");
            Fake.FieldClass = object(variant.ShadowField ? 19 : 2, "Field");
            if (variant.ShadowField)
            {
                ShadowField = object(2, "Field");
                GamePackage = object(20, "/Script/MyGame");
            }
            Fake.StructClass = object(3, "Struct");
            Fake.ClassClass = object(4, "Class");
            Fake.PackageClass = object(5, "Package");
            Fake.ScriptStructClass = object(6, "ScriptStruct");
            Fake.EnumClass = object(7, "Enum");
            Fake.Vector = object(8, "Vector");
            Fake.Vector2D = object(9, "Vector2D");
            Fake.LinearColor = object(10, "LinearColor");
            Fake.IntPoint = object(11, "IntPoint");
            Fake.Actor = object(12, "Actor");
            Fake.SceneComponent = object(13, "SceneComponent");
            Fake.GameEngineClass = object(14, "GameEngine");
            Fake.Mode = object(15, "EMyMode");
            Fake.ActorInstance = object(16, "MyActor");
            Fake.SceneInstance = object(17, "Root");
            Fake.Engine = object(18, "GameEngine", 2);
            const std::uint32_t filler = Name("Filler");
            for (int i = variant.ShadowField ? 21 : 19; i < kObjectCount; ++i)
                Object(i, filler, static_cast<std::uint32_t>(i));

            for (int i = 0; i < kObjectCount; ++i)
            {
                const std::uintptr_t at = Address(i);
                std::uintptr_t meta = Fake.ObjectClass;
                if (at == Fake.Package)
                    meta = Fake.PackageClass;
                else if (at == ShadowField)
                    meta = Fake.ScriptStructClass;
                else if (at == GamePackage)
                    meta = Fake.PackageClass;
                else if ((at >= Fake.ObjectClass && at <= Fake.EnumClass) || at == Fake.FieldClass)
                    meta = Fake.ClassClass;
                else if (at >= Fake.Vector && at <= Fake.IntPoint)
                    meta = Fake.ScriptStructClass;
                else if (at == Fake.Actor || at == Fake.SceneComponent || at == Fake.GameEngineClass)
                    meta = Fake.ClassClass;
                else if (at == Fake.Mode)
                    meta = Fake.EnumClass;
                else if (at == Fake.ActorInstance)
                    meta = Fake.Actor;
                else if (at == Fake.SceneInstance)
                    meta = Fake.SceneComponent;
                else if (at == Fake.Engine)
                    meta = Fake.GameEngineClass;
                Put<std::uintptr_t>(at, o.Class, meta);
                Put<std::uintptr_t>(at, o.Outer, at == Fake.Package || at == GamePackage ? 0 : at == ShadowField ? GamePackage : Fake.Package);
            }
            Super(Fake.FieldClass, Fake.ObjectClass);
            Super(Fake.StructClass, Fake.FieldClass);
            Super(Fake.ClassClass, Fake.StructClass);
            Super(Fake.Actor, Fake.ObjectClass);
            Super(Fake.SceneComponent, Fake.ObjectClass);
            Super(Fake.GameEngineClass, Fake.ObjectClass);
            Size(Fake.Vector, 3 * component);
            Size(Fake.Vector2D, 2 * component);
            Size(Fake.LinearColor, 16);
            Size(Fake.IntPoint, 8);
            Size(Fake.Actor, kActorSize);
            Size(Fake.SceneComponent, 0x200);
            Size(Fake.GameEngineClass, 0x100);
            Size(Fake.ObjectClass, o.Outer + o.Pointer);

            const std::uintptr_t floatClass = FieldClassNamed("FloatProperty");
            const std::uintptr_t objectClass = FieldClassNamed("ObjectProperty");
            const std::uintptr_t boolClass = FieldClassNamed("BoolProperty");
            const std::uintptr_t arrayClass = FieldClassNamed("ArrayProperty");
            const std::uintptr_t nameClass = FieldClassNamed("NameProperty");
            const std::uintptr_t structClass = FieldClassNamed("StructProperty");

            const std::uintptr_t realClass = variant.Doubles ? FieldClassNamed("DoubleProperty") : floatClass;
            if (variant.ReverseChain)
            {
                const std::uintptr_t x = Field(realClass, Fake.Vector, "X", 0, component, 0);
                const std::uintptr_t y = Field(realClass, Fake.Vector, "Y", component, component, x);
                const std::uintptr_t z = Field(realClass, Fake.Vector, "Z", 2 * component, component, y);
                Put<std::uintptr_t>(Fake.Vector, o.ChildProperties, z);
            }
            else
            {
                const std::uintptr_t z = Field(realClass, Fake.Vector, "Z", 2 * component, component, 0);
                const std::uintptr_t y = Field(realClass, Fake.Vector, "Y", component, component, z);
                const std::uintptr_t x = Field(realClass, Fake.Vector, "X", 0, component, y);
                Put<std::uintptr_t>(Fake.Vector, o.ChildProperties, x);
            }

            const std::uintptr_t location = Field(structClass, Fake.SceneComponent, "RelativeLocation", locationAt, 3 * component, 0);
            Put<std::uintptr_t>(location, o.Extra, Fake.Vector);
            Put<std::uintptr_t>(Fake.SceneComponent, o.ChildProperties, location);
            if (variant.Containers)
            {
                std::uintptr_t next = location;
                next = Field(FieldClassNamed("MapProperty"), Fake.SceneComponent, "Lookup", kLookup, 0x50, next);
                next = Field(FieldClassNamed("TextProperty"), Fake.SceneComponent, "Title", 0x80, 0x18, next);
                next = Field(FieldClassNamed("SoftObjectProperty"), Fake.SceneComponent, "Mesh", 0x98, 0x28, next);
                next = Field(FieldClassNamed("InterfaceProperty"), Fake.SceneComponent, "Partner", 0xC0, 2 * o.Pointer, next);
                next = Field(FieldClassNamed("StrProperty"), Fake.SceneComponent, "Label", 0xD0, o.Pointer + 8, next);
                next = Field(FieldClassNamed("DelegateProperty"), Fake.SceneComponent, "OnHit", 0xE0, 0x10, next);
                next = Field(FieldClassNamed("WeakObjectProperty"), Fake.SceneComponent, "Target", 0xF0, 8, next);
                next = Field(FieldClassNamed("ByteProperty"), Fake.SceneComponent, "Buffer", kBuffer, 1, next);
                Put<std::int32_t>(next, o.FieldDim, kBufferDim);
                Put<std::uintptr_t>(Fake.SceneComponent, o.ChildProperties, next);
            }

            const std::uintptr_t inner = Field(nameClass, Fake.Actor, "Tags", 0, 8, 0);
            const std::uintptr_t tags = Field(arrayClass, Fake.Actor, "Tags", kTags, o.Pointer + 8, 0);
            Put<std::uintptr_t>(tags, o.Extra, inner);
            const std::uintptr_t hidden = Field(boolClass, Fake.Actor, "bHidden", kHidden, 1, tags);
            const std::uint8_t bits[4] = { 1, 0, 0x02, 0x02 };
            std::memcpy(reinterpret_cast<void*>(hidden + static_cast<std::uintptr_t>(o.Extra)), bits, 4);
            const std::uintptr_t root = Field(objectClass, Fake.Actor, "RootComponent", kRoot, o.Pointer, hidden);
            Put<std::uintptr_t>(root, o.Extra, Fake.SceneComponent);
            const std::uintptr_t health = Field(floatClass, Fake.Actor, "Health", kHealth, 4, root);
            Put<std::uintptr_t>(Fake.Actor, o.ChildProperties, health);

            const std::uintptr_t entries = reinterpret_cast<std::uintptr_t>(m_fields) + 0xF000;
            const char* names[] = { "EMyMode::A", "EMyMode::B", variant.NoMaxEnum ? "EMyMode::C" : "EMyMode_MAX" };
            const std::uint8_t narrowValues[] = { 0, 5, 6 };
            for (int i = 0; i < 3; ++i)
            {
                if (variant.NarrowEnum)
                {
                    Put<std::uint32_t>(entries, i * 12, Name(names[i]));
                    Put<std::uint8_t>(entries, i * 12 + 8, narrowValues[i]);
                    continue;
                }
                Put<std::uint32_t>(entries, i * 16, Name(names[i]));
                Put<std::int64_t>(entries, i * 16 + 8, i);
            }
            if (variant.SplitEnum)
            {
                // UE 5.7: names and values in two arrays, pointers tagged in bit 0, then the count.
                const std::uintptr_t splitNames = entries + 0x80;
                const std::uintptr_t splitValues = entries + 0xC0;
                for (int i = 0; i < 3; ++i)
                {
                    Put<std::uint32_t>(splitNames, i * 8, Name(names[i]));
                    Put<std::int64_t>(splitValues, i * 8, narrowValues[i]);
                }
                Put<std::uintptr_t>(Fake.Mode, o.EnumNames, splitNames | 1);
                Put<std::uintptr_t>(Fake.Mode, o.EnumNames + o.Pointer, splitValues | 1);
                Put<std::int32_t>(Fake.Mode, o.EnumNames + 2 * o.Pointer, 3);
            }
            else
            {
                Put<std::uintptr_t>(Fake.Mode, o.EnumNames, entries);
                Put<std::int32_t>(Fake.Mode, o.EnumNames + o.Pointer, 3);
                Put<std::int32_t>(Fake.Mode, o.EnumNames + o.Pointer + 4, 3);
            }

            Put<std::uintptr_t>(Fake.ActorInstance, kRoot, Fake.SceneInstance);
            Put<float>(Fake.ActorInstance, kHealth, 75.0f);
            Put<std::uint8_t>(Fake.ActorInstance, kHidden, 0x02);
            Fake.TagData = reinterpret_cast<std::uintptr_t>(m_fields) + 0xF100;
            Put<std::uint32_t>(Fake.TagData, 0, Name("Hero"));
            Put<std::uint32_t>(Fake.TagData, 8, Name("Brave"));
            Put<std::uintptr_t>(Fake.ActorInstance, kTags, Fake.TagData);
            Put<std::int32_t>(Fake.ActorInstance, kTags + o.Pointer, 2);
            Put<std::int32_t>(Fake.ActorInstance, kTags + o.Pointer + 4, 2);
            for (int i = 0; i < 3; ++i)
            {
                if (variant.Doubles)
                    Put<double>(Fake.SceneInstance, locationAt + i * 8, 1.0 + i);
                else
                    Put<float>(Fake.SceneInstance, locationAt + i * 4, 1.0f + static_cast<float>(i));
            }
            Fake.Text = reinterpret_cast<std::uintptr_t>(m_fields) + 0xF200;
            const wchar_t text[] = L"Hello";
            std::memcpy(reinterpret_cast<void*>(Fake.Text), text, sizeof(text));

            const int bigInstanceIndex = kObjectCount - 1;
            if (variant.BigClass && !BuildBig(o, bigInstanceIndex, variant))
                return false;

            const std::uintptr_t items = reinterpret_cast<std::uintptr_t>(m_items);
            const std::uintptr_t chunkTable = items + 0x10000 * static_cast<std::uintptr_t>(o.ItemSize);
            Put<std::uintptr_t>(chunkTable, 0, items);
            for (int i = 0; i < kObjectCount; ++i)
                Put<std::uintptr_t>(items, i * o.ItemSize, variant.BigClass && i == bigInstanceIndex ? Fake.BigInstance : Address(i));
            g_objects = { chunkTable, 0, 0x10000, kObjectCount, 1, 1 };
            g_pool.Blocks[0] = reinterpret_cast<std::uintptr_t>(m_names);
            g_pool.Cursor = static_cast<std::uint32_t>(m_used / 2);
            Fake.Pool = reinterpret_cast<std::uintptr_t>(&g_pool);
            Fake.Objects = reinterpret_cast<std::uintptr_t>(&g_objects);
            return true;
        }

        std::uint32_t Name(std::string_view text)
        {
            if (const auto it = m_known.find(std::string(text)); it != m_known.end())
                return it->second;
            const std::uint32_t index = static_cast<std::uint32_t>(m_used / 2);
            const std::uint16_t header = static_cast<std::uint16_t>(text.size() << 6);
            std::memcpy(m_names + m_used, &header, 2);
            std::memcpy(m_names + m_used + 2, text.data(), text.size());
            m_used += (2 + text.size() + 1) & ~std::size_t(1);
            m_known[std::string(text)] = index;
            return index;
        }

        std::uintptr_t Address(int index) const
        {
            return reinterpret_cast<std::uintptr_t>(m_objects) + static_cast<std::uintptr_t>(kNative.ObjectBias) + static_cast<std::uintptr_t>(index) * kObjectStride;
        }

        std::uintptr_t LoadClass(int index, const char* name)
        {
            const std::uintptr_t at = Object(index, Name(name), 0);
            g_pool.Cursor = static_cast<std::uint32_t>(m_used / 2);
            Put<std::uintptr_t>(at, kNative.Class, Fake.ClassClass);
            Super(at, Fake.Actor);
            return at;
        }

        Image Fake{};
        std::uintptr_t ShadowField = 0;
        std::uintptr_t GamePackage = 0;

    private:
        static std::uint8_t* Allocate(std::size_t size)
        {
            return static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }

        template <class T>
        static void Put(std::uintptr_t base, int offset, T value)
        {
            std::memcpy(reinterpret_cast<void*>(base + static_cast<std::uintptr_t>(offset)), &value, sizeof(T));
        }

        std::uintptr_t Object(int index, std::uint32_t name, std::uint32_t number)
        {
            const std::uintptr_t at = Address(index);
            Put<std::uintptr_t>(at, 0, reinterpret_cast<std::uintptr_t>(&g_vtable[0]));
            Put<std::int32_t>(at, kNative.Index, index);
            Put<std::uint32_t>(at, kNative.Name, name);
            Put<std::uint32_t>(at, kNative.Name + 4, number);
            return at;
        }

        // BigActor (one filler slot before the last) with kBigFields int properties Value_0.., and its instance
        // MyBigActor in a block of its own at the last slot, the property values counting up from 0.
        bool BuildBig(const Offsets& o, int instanceIndex, const Variant& variant)
        {
            const int count = std::max(1, variant.BigFields);
            const int size = kBigFirst + 4 * count;
            m_bigFields = Allocate(static_cast<std::size_t>(count) * kFieldStride + 0x1000);
            m_bigInstance = Allocate(static_cast<std::size_t>(size) + 0x1000);
            if (!m_bigFields || !m_bigInstance)
                return false;
            Fake.BigClass = Object(instanceIndex - 1, Name("BigActor"), 0);
            Put<std::uintptr_t>(Fake.BigClass, o.Class, Fake.ClassClass);
            Put<std::uintptr_t>(Fake.BigClass, o.Outer, Fake.Package);
            Super(Fake.BigClass, Fake.ObjectClass);
            Size(Fake.BigClass, size);
            // The owner of the big chain: the class itself, or BigStruct (fields from 0) behind Payload.
            std::uintptr_t owner = Fake.BigClass;
            int first = kBigFirst;
            if (variant.BigStruct)
            {
                Fake.BigStruct = Object(instanceIndex - 2, Name("BigStruct"), 0);
                Put<std::uintptr_t>(Fake.BigStruct, o.Class, Fake.ScriptStructClass);
                Put<std::uintptr_t>(Fake.BigStruct, o.Outer, Fake.Package);
                Size(Fake.BigStruct, 4 * count);
                const std::uintptr_t payload = Field(FieldClassNamed("StructProperty"), Fake.BigClass, "Payload", kBigFirst, 4 * count, 0);
                Put<std::uintptr_t>(payload, o.Extra, Fake.BigStruct);
                Put<std::uintptr_t>(Fake.BigClass, o.ChildProperties, payload);
                owner = Fake.BigStruct;
                first = 0;
            }

            Fake.BigInstance = reinterpret_cast<std::uintptr_t>(m_bigInstance) + static_cast<std::uintptr_t>(o.ObjectBias);
            Put<std::uintptr_t>(Fake.BigInstance, 0, reinterpret_cast<std::uintptr_t>(&g_vtable[0]));
            Put<std::int32_t>(Fake.BigInstance, o.Index, instanceIndex);
            Put<std::uint32_t>(Fake.BigInstance, o.Name, Name("MyBigActor"));
            Put<std::uint32_t>(Fake.BigInstance, o.Name + 4, 0);
            Put<std::uintptr_t>(Fake.BigInstance, o.Class, Fake.BigClass);
            Put<std::uintptr_t>(Fake.BigInstance, o.Outer, Fake.Package);

            const std::uintptr_t intClass = FieldClassNamed("IntProperty");
            const std::uint32_t value = Name("Value");
            std::uintptr_t next = 0;
            for (int i = count - 1; i >= 0; --i)
            {
                const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(m_bigFields) + static_cast<std::uintptr_t>(i) * kFieldStride;
                Put<std::uintptr_t>(at, o.FieldClass, intClass);
                Put<std::uintptr_t>(at, o.FieldOwner, owner);
                Put<std::uintptr_t>(at, o.FieldNext, variant.BrokenChain && i == count / 2 ? std::uintptr_t(0x10) : variant.LoopChain && i == count - 1 ? reinterpret_cast<std::uintptr_t>(m_bigFields) : next);
                Put<std::uint32_t>(at, o.FieldName, value);
                Put<std::uint32_t>(at, o.FieldName + 4, static_cast<std::uint32_t>(i + 1));
                Put<std::int32_t>(at, o.FieldDim, 1);
                Put<std::int32_t>(at, o.FieldSize, 4);
                Put<std::int32_t>(at, o.FieldOffset, first + 4 * i);
                Put<std::int32_t>(Fake.BigInstance, kBigFirst + 4 * i, i);
                next = at;
            }
            Put<std::uintptr_t>(owner, o.ChildProperties, next);
            g_pool.Cursor = static_cast<std::uint32_t>(m_used / 2);
            return true;
        }

        void Super(std::uintptr_t type, std::uintptr_t parent) { Put<std::uintptr_t>(type, kNative.Super, parent); }
        void Size(std::uintptr_t type, std::int32_t size) { Put<std::int32_t>(type, kNative.StructSize, size); }

        std::uintptr_t FieldClassNamed(const char* name)
        {
            const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(m_fields) + m_fieldUsed;
            m_fieldUsed += kFieldStride;
            Put<std::uint32_t>(at, 0, Name(name));
            return at;
        }

        std::uintptr_t Field(std::uintptr_t fieldClass, std::uintptr_t owner, const char* name, std::int32_t offset, std::int32_t size, std::uintptr_t next)
        {
            const Offsets& o = kNative;
            const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(m_fields) + m_fieldUsed;
            m_fieldUsed += kFieldStride;
            Put<std::uintptr_t>(at, o.FieldClass, fieldClass);
            Put<std::uintptr_t>(at, o.FieldOwner, owner);
            Put<std::uintptr_t>(at, o.FieldNext, next);
            Put<std::uint32_t>(at, o.FieldName, Name(name));
            Put<std::int32_t>(at, o.FieldDim, 1);
            Put<std::int32_t>(at, o.FieldSize, size);
            Put<std::int32_t>(at, o.FieldOffset, offset);
            return at;
        }

        std::uint8_t* m_names = nullptr;
        std::uint8_t* m_objects = nullptr;
        std::uint8_t* m_items = nullptr;
        std::uint8_t* m_fields = nullptr;
        std::uint8_t* m_bigFields = nullptr;
        std::uint8_t* m_bigInstance = nullptr;
        std::size_t m_used = 0;
        std::size_t m_fieldUsed = 0;
        std::map<std::string, std::uint32_t> m_known;
    };
}
