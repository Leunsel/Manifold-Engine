#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// A synthetic Unreal Engine 3 runtime: flat TArray<FNameEntry*> GNames, flat TArray<UObject*> GObjObjects,
// UProperty-based reflection with UField::Next chains that mix functions, structs, enums and properties,
// UClass -> UState -> UStruct, a 32-bit BoolProperty BitMask and enums as TArray<FName>.
//
// Two header layouts per pointer size. "Late" follows the UE3 builds of 2010+ (FNameEntry with 64-bit flags first,
// UObject with HashNext, ObjectFlags, HashOuterNext, StateFrame, Linker, LinkerIndex, Index, NetIndex, Outer, Name,
// Class, ObjectArchetype); "early" follows the 2008 builds (FNameEntry starts with its index, no NetIndex) and links
// the Vector members the other way round. The offsets are made up to that recipe, not copied from one game.
namespace FakeUnreal3
{
    struct Offsets
    {
        int Pointer;
        int EntryIndex;
        int EntryName;
        int ObjectFlags;
        int Index;
        int Outer;
        int Name;
        int Class;
        int Next;
        int Super;
        int Children;
        int PropertySize;
        int ArrayDim;
        int Offset;
        int Extra;
        int EnumNames;
        int FunctionFlags;
    };

    inline constexpr Offsets kLate32 = { 4, 0x08, 0x10, 0x04, 0x20, 0x28, 0x2C, 0x34, 0x3C, 0x48, 0x4C, 0x50, 0x40, 0x60, 0x80, 0x40, 0x8C };
    // x86 early: FNameEntry of an early UE3 Unicode build with {Index (not shifted), ..., UTF-16 name at +0x10}.
    inline constexpr Offsets kEarly32 = { 4, 0x00, 0x10, 0x08, 0x20, 0x24, 0x28, 0x30, 0x38, 0x40, 0x44, 0x48, 0x3C, 0x58, 0x74, 0x3C, 0x80 };
    inline constexpr Offsets kLate64 = { 8, 0x08, 0x18, 0x10, 0x38, 0x40, 0x48, 0x50, 0x60, 0x78, 0x80, 0x88, 0x68, 0x90, 0xC0, 0x68, 0xF8 };
    // x64 early: a byte-packed FNameEntry, {int32 Index, HashNext at +4 unaligned, Name at +0x0C}.
    inline constexpr Offsets kEarly64 = { 8, 0x00, 0x0C, 0x10, 0x34, 0x38, 0x40, 0x48, 0x58, 0x68, 0x70, 0x78, 0x60, 0x80, 0xB0, 0x60, 0xE0 };

    inline constexpr const Offsets& Native(bool early)
    {
        if constexpr (sizeof(void*) == 8)
            return early ? kEarly64 : kLate64;
        else
            return early ? kEarly32 : kLate32;
    }

    inline constexpr int kObjectCount = 2100;
    inline constexpr int kNameCount = 0x4200;
    inline constexpr std::size_t kObjectStride = 0x300;
    inline constexpr std::size_t kNameStride = 0x40;

    inline constexpr int kOwner = 0x100;
    inline constexpr int kLocation = 0x110;
    inline constexpr int kBits = 0x120;
    inline constexpr int kHealth = 0x124;
    inline constexpr int kTags = 0x128;
    inline constexpr int kWorldInfo = 0x138;
    inline constexpr int kActorSize = 0x140;
    inline constexpr int kPawn = 0x140;
    inline constexpr int kOnTouch = 0x148;
    inline constexpr int kControllerSize = 0x160;
    inline constexpr int kPlayerActor = 0x100;
    inline constexpr int kGamePlayers = 0x100;
    inline constexpr std::uint32_t kHiddenMask = 0x00000004;
    inline constexpr std::uint32_t kTeleportMask = 0x00000200;

    inline constexpr std::uint32_t kFuncFinal = 0x1;
    inline constexpr std::uint32_t kFuncExec = 0x200;
    inline constexpr std::uint32_t kFuncNative = 0x400;
    inline constexpr std::uint32_t kFuncEvent = 0x800;
    inline constexpr std::uint32_t kFuncOperator = 0x1000;
    inline constexpr std::uint32_t kFuncStatic = 0x2000;

    struct Array
    {
        std::uintptr_t Data;
        std::int32_t Num;
        std::int32_t Max;
    };

    template <class Word>
    struct ImageOf
    {
        Word Names, Objects, Early;
        Word Core, EnginePackage, ObjectClass, FieldClass, StructClass, StateClass, ClassClass, PackageClass, ScriptStructClass, EnumClass, FunctionClass;
        Word Vector, Actor, Pawn, PlayerController, LocalPlayer, GameEngineClass, WorldInfoClass, Mode;
        Word ActorInstance, PawnInstance, ControllerInstance, PlayerInstance, Engine, WorldInfo, DefaultActor, TagData, PlayerData;
        Word NotPreBool, Destroy, Touch, Fov;
    };

    using Image = ImageOf<std::uintptr_t>;

    inline Array g_names;
    inline Array g_objects;
    inline const std::uintptr_t g_vtable[4] = { 0x31, 0x32, 0x33, 0x34 };

    class Builder
    {
    public:
        Builder() = default;
        Builder(const Builder&) = delete;
        Builder& operator=(const Builder&) = delete;

        ~Builder()
        {
            for (void* block : { m_names, m_nameArray, m_objects, m_items, m_fields, m_chain })
            {
                if (block)
                    ::VirtualFree(block, 0, MEM_RELEASE);
            }
            std::memset(&g_names, 0, sizeof(g_names));
            std::memset(&g_objects, 0, sizeof(g_objects));
        }

        // noLocation: as in a heavily changed UE3 build, Actor has no "Location" (here "WorldLocation") and every
        // property keeps a pointer to its owning struct in the first slot after Offset, ahead of the class/struct/inner slot.
        bool Build(bool early, bool noLocation = false)
        {
            m_early = early;
            m_noLocation = noLocation;
            const Offsets& o = Native(early);
            m_names = Allocate(static_cast<std::size_t>(kNameCount + 64) * (kNameStride + 1) + 16);
            m_nameArray = Allocate(static_cast<std::size_t>(kNameCount + 64) * sizeof(std::uintptr_t));
            m_objects = Allocate(kObjectCount * kObjectStride + 0x1000);
            m_items = Allocate(kObjectCount * sizeof(std::uintptr_t) + 0x1000);
            m_fields = Allocate(0x10000);
            if (!m_names || !m_nameArray || !m_objects || !m_items || !m_fields)
                return false;
            // The hard-coded names UE3 registers first (UnNames.h), so object and name indices do not line up.
            for (const char* name : { "None", "ByteProperty", "IntProperty", "BoolProperty", "FloatProperty", "ObjectProperty", "NameProperty", "DelegateProperty",
                     "ClassProperty", "ArrayProperty", "StructProperty", "VectorProperty", "RotatorProperty", "StrProperty", "MapProperty", "InterfaceProperty" })
                Name(name);

            Fake.Early = early ? 1 : 0;
            if (!early)
                Fake.Core = Object("Core");
            Fake.ObjectClass = Object("Object");
            Fake.FieldClass = Object("Field");
            Fake.StructClass = Object("Struct");
            Fake.StateClass = Object("State");
            Fake.ClassClass = Object("Class");
            Fake.PackageClass = Object("Package");
            Fake.ScriptStructClass = Object("ScriptStruct");
            Fake.EnumClass = Object("Enum");
            Fake.FunctionClass = Object("Function");
            const std::uintptr_t floatClass = Object("FloatProperty");
            const std::uintptr_t objectClass = Object("ObjectProperty");
            const std::uintptr_t structClass = Object("StructProperty");
            const std::uintptr_t boolClass = Object("BoolProperty");
            const std::uintptr_t arrayClass = Object("ArrayProperty");
            const std::uintptr_t nameClass = Object("NameProperty");
            const std::uintptr_t delegateClass = Object("DelegateProperty");
            Fake.Vector = Object("Vector");
            const std::uintptr_t vector2D = Object("Vector2D");
            const std::uintptr_t linearColor = Object("LinearColor");
            const std::uintptr_t intPoint = Object("IntPoint");
            Fake.NotPreBool = Object("Not_PreBool");
            Fake.EnginePackage = Object("Engine");
            Fake.Actor = Object("Actor");
            Fake.Pawn = Object("Pawn");
            Fake.PlayerController = Object("PlayerController");
            Fake.LocalPlayer = Object("LocalPlayer");
            Fake.GameEngineClass = Object("GameEngine");
            Fake.WorldInfoClass = Object("WorldInfo");
            Fake.Mode = Object("EMyMode");
            Fake.Destroy = Object("Destroy");
            Fake.Touch = Object("Touch");
            Fake.Fov = Object("FOV");
            Fake.DefaultActor = Object("Default__Actor");
            Fake.ActorInstance = Object("MyActor");
            Fake.PawnInstance = Object("MyPawn", 0, true);
            Fake.ControllerInstance = Object("MyController");
            Fake.PlayerInstance = Object("LocalPlayer", 1);
            Fake.Engine = Object("GameEngine", 2);
            Fake.WorldInfo = Object("WorldInfo", 1);

            const auto meta = [&](std::uintptr_t object, std::uintptr_t klass, std::uintptr_t outer)
                {
                    Put<std::uintptr_t>(object, o.Class, klass);
                    Put<std::uintptr_t>(object, o.Outer, outer);
                };
            // Some UE3 games keep their Core package far behind the first 64 objects, which are Core's classes.
            const std::uint32_t filler = Name("Filler");
            std::vector<std::uintptr_t> early64;
            if (early)
            {
                while (m_objectCount < 80)
                    early64.push_back(Object(filler, static_cast<std::uint32_t>(m_objectCount)));
                Fake.Core = Object("Core");
            }
            meta(Fake.Core, Fake.PackageClass, 0);
            meta(Fake.EnginePackage, Fake.PackageClass, 0);
            for (const std::uintptr_t type : { Fake.ObjectClass, Fake.FieldClass, Fake.StructClass, Fake.StateClass, Fake.ClassClass, Fake.PackageClass, Fake.ScriptStructClass,
                     Fake.EnumClass, Fake.FunctionClass, floatClass, objectClass, structClass, boolClass, arrayClass, nameClass, delegateClass })
                meta(type, Fake.ClassClass, Fake.Core);
            for (const std::uintptr_t type : { Fake.Vector, vector2D, linearColor, intPoint })
                meta(type, Fake.ScriptStructClass, Fake.ObjectClass);
            meta(Fake.NotPreBool, Fake.FunctionClass, Fake.ObjectClass);
            for (const std::uintptr_t type : { Fake.Actor, Fake.Pawn, Fake.PlayerController, Fake.LocalPlayer, Fake.GameEngineClass, Fake.WorldInfoClass })
                meta(type, Fake.ClassClass, Fake.EnginePackage);
            meta(Fake.Mode, Fake.EnumClass, Fake.Actor);
            meta(Fake.Destroy, Fake.FunctionClass, Fake.Actor);
            meta(Fake.Touch, Fake.FunctionClass, Fake.Actor);
            meta(Fake.Fov, Fake.FunctionClass, Fake.PlayerController);
            meta(Fake.DefaultActor, Fake.Actor, Fake.EnginePackage);
            meta(Fake.ActorInstance, Fake.Actor, Fake.EnginePackage);
            meta(Fake.PawnInstance, Fake.Pawn, Fake.EnginePackage);
            meta(Fake.ControllerInstance, Fake.PlayerController, Fake.EnginePackage);
            meta(Fake.PlayerInstance, Fake.LocalPlayer, Fake.EnginePackage);
            meta(Fake.Engine, Fake.GameEngineClass, Fake.EnginePackage);
            meta(Fake.WorldInfo, Fake.WorldInfoClass, Fake.EnginePackage);

            Super(Fake.FieldClass, Fake.ObjectClass);
            Super(Fake.StructClass, Fake.FieldClass);
            Super(Fake.StateClass, Fake.StructClass);
            Super(Fake.ClassClass, Fake.StateClass);
            Super(Fake.ScriptStructClass, Fake.StructClass);
            Super(Fake.FunctionClass, Fake.StructClass);
            Super(Fake.EnumClass, Fake.FieldClass);
            Super(Fake.PackageClass, Fake.ObjectClass);
            Super(Fake.Actor, Fake.ObjectClass);
            Super(Fake.Pawn, Fake.Actor);
            Super(Fake.PlayerController, Fake.Actor);
            Super(Fake.WorldInfoClass, Fake.Actor);
            Super(Fake.LocalPlayer, Fake.ObjectClass);
            Super(Fake.GameEngineClass, Fake.ObjectClass);
            Size(Fake.ObjectClass, o.Class + 2 * o.Pointer);
            Size(Fake.Vector, 12);
            Size(vector2D, 8);
            Size(linearColor, 16);
            Size(intPoint, 8);
            Size(Fake.Actor, kActorSize);
            Size(Fake.Pawn, kActorSize);
            Size(Fake.WorldInfoClass, kActorSize);
            Size(Fake.PlayerController, kControllerSize);
            Size(Fake.LocalPlayer, kPlayerActor + o.Pointer);
            Size(Fake.GameEngineClass, kGamePlayers + o.Pointer + 8);

            Flags(Fake.NotPreBool, kFuncFinal | kFuncNative | kFuncOperator | kFuncStatic);
            Flags(Fake.Destroy, kFuncFinal | kFuncNative);
            Flags(Fake.Touch, kFuncEvent);
            Flags(Fake.Fov, kFuncExec);

            // Vector: X, Y, Z (early builds link them the other way round).
            if (early)
            {
                const std::uintptr_t x = Property(floatClass, Fake.Vector, "X", 0, 4, 0);
                const std::uintptr_t y = Property(floatClass, Fake.Vector, "Y", 4, 4, x);
                const std::uintptr_t z = Property(floatClass, Fake.Vector, "Z", 8, 4, y);
                Put<std::uintptr_t>(Fake.Vector, o.Children, z);
            }
            else
            {
                const std::uintptr_t z = Property(floatClass, Fake.Vector, "Z", 8, 4, 0);
                const std::uintptr_t y = Property(floatClass, Fake.Vector, "Y", 4, 4, z);
                const std::uintptr_t x = Property(floatClass, Fake.Vector, "X", 0, 4, y);
                Put<std::uintptr_t>(Fake.Vector, o.Children, x);
            }
            // Object's children: a function, nested structs and, like Object.uc, the header member Outer as a property.
            const std::uintptr_t outerProperty = Property(objectClass, Fake.ObjectClass, "Outer", o.Outer, o.Pointer, 0);
            Put<std::uintptr_t>(outerProperty, o.Extra, Fake.ObjectClass);
            Next(intPoint, outerProperty);
            Next(linearColor, intPoint);
            Next(vector2D, linearColor);
            Next(Fake.Vector, vector2D);
            Next(Fake.NotPreBool, Fake.Vector);
            Put<std::uintptr_t>(Fake.ObjectClass, o.Children, Fake.NotPreBool);

            // Actor: Touch -> Destroy -> EMyMode -> Owner -> Location -> bHidden -> bCanTeleport -> Health -> Tags -> WorldInfo.
            const std::uintptr_t worldInfo = Property(objectClass, Fake.Actor, "WorldInfo", kWorldInfo, o.Pointer, 0);
            Put<std::uintptr_t>(worldInfo, o.Extra, Fake.WorldInfoClass);
            const std::uintptr_t tagName = Property(nameClass, 0, "Tags", 0, 8, 0);
            const std::uintptr_t tags = Property(arrayClass, Fake.Actor, "Tags", kTags, o.Pointer + 8, worldInfo);
            Put<std::uintptr_t>(tagName, o.Outer, tags);
            Put<std::uintptr_t>(tags, o.Extra, tagName);
            const std::uintptr_t health = Property(floatClass, Fake.Actor, "Health", kHealth, 4, tags);
            const std::uintptr_t teleport = Property(boolClass, Fake.Actor, "bCanTeleport", kBits, 4, health);
            Put<std::uint32_t>(teleport, o.Extra, kTeleportMask);
            const std::uintptr_t hidden = Property(boolClass, Fake.Actor, "bHidden", kBits, 4, teleport);
            Put<std::uint32_t>(hidden, o.Extra, kHiddenMask);
            const std::uintptr_t location = Property(structClass, Fake.Actor, noLocation ? "WorldLocation" : "Location", kLocation, 12, hidden);
            Put<std::uintptr_t>(location, o.Extra, Fake.Vector);
            const std::uintptr_t owner = Property(objectClass, Fake.Actor, "Owner", kOwner, o.Pointer, location);
            Put<std::uintptr_t>(owner, o.Extra, Fake.Actor);
            Next(Fake.Mode, owner);
            Next(Fake.Destroy, Fake.Mode);
            Next(Fake.Touch, Fake.Destroy);
            Put<std::uintptr_t>(Fake.Actor, o.Children, Fake.Touch);

            const std::uintptr_t onTouch = Property(delegateClass, Fake.PlayerController, "OnTouch", kOnTouch, o.Pointer + 8, 0);
            const std::uintptr_t pawn = Property(objectClass, Fake.PlayerController, "Pawn", kPawn, o.Pointer, onTouch);
            Put<std::uintptr_t>(pawn, o.Extra, Fake.Pawn);
            Next(Fake.Fov, pawn);
            Put<std::uintptr_t>(Fake.PlayerController, o.Children, Fake.Fov);
            const std::uintptr_t playerActor = Property(objectClass, Fake.LocalPlayer, "Actor", kPlayerActor, o.Pointer, 0);
            Put<std::uintptr_t>(playerActor, o.Extra, Fake.PlayerController);
            Put<std::uintptr_t>(Fake.LocalPlayer, o.Children, playerActor);
            const std::uintptr_t playerInner = Property(objectClass, 0, "GamePlayers", 0, o.Pointer, 0);
            Put<std::uintptr_t>(playerInner, o.Extra, Fake.LocalPlayer);
            const std::uintptr_t players = Property(arrayClass, Fake.GameEngineClass, "GamePlayers", kGamePlayers, o.Pointer + 8, 0);
            Put<std::uintptr_t>(playerInner, o.Outer, players);
            Put<std::uintptr_t>(players, o.Extra, playerInner);
            Put<std::uintptr_t>(Fake.GameEngineClass, o.Children, players);

            // EMyMode: TArray<FName> { MM_A, MM_B, MM_MAX }.
            const std::uintptr_t entries = reinterpret_cast<std::uintptr_t>(m_fields) + 0xF000;
            const char* names[] = { "MM_A", "MM_B", "MM_MAX" };
            for (int i = 0; i < 3; ++i)
                Put<std::uint32_t>(entries, i * 8, Name(names[i]));
            Put<std::uintptr_t>(Fake.Mode, o.EnumNames, entries);
            Put<std::int32_t>(Fake.Mode, o.EnumNames + o.Pointer, 3);
            Put<std::int32_t>(Fake.Mode, o.EnumNames + o.Pointer + 4, 3);

            // Instances.
            Put<std::uintptr_t>(Fake.ActorInstance, kOwner, Fake.ControllerInstance);
            for (int i = 0; i < 3; ++i)
                Put<float>(Fake.ActorInstance, kLocation + 4 * i, 1.0f + static_cast<float>(i));
            Put<std::uint32_t>(Fake.ActorInstance, kBits, kHiddenMask | kTeleportMask);
            Put<float>(Fake.ActorInstance, kHealth, 75.0f);
            Fake.TagData = reinterpret_cast<std::uintptr_t>(m_fields) + 0xF100;
            Put<std::uint32_t>(Fake.TagData, 0, Name("Hero"));
            Put<std::uint32_t>(Fake.TagData, 8, Name("Brave"));
            Put<std::uintptr_t>(Fake.ActorInstance, kTags, Fake.TagData);
            Put<std::int32_t>(Fake.ActorInstance, kTags + o.Pointer, 2);
            Put<std::int32_t>(Fake.ActorInstance, kTags + o.Pointer + 4, 2);
            Put<std::uintptr_t>(Fake.ControllerInstance, kPawn, Fake.PawnInstance);
            Put<std::uintptr_t>(Fake.ControllerInstance, kWorldInfo, Fake.WorldInfo);
            Put<std::uintptr_t>(Fake.ControllerInstance, kOnTouch, Fake.ActorInstance);
            Put<std::uint32_t>(Fake.ControllerInstance, kOnTouch + o.Pointer, Name("Touch"));
            Put<std::uintptr_t>(Fake.PlayerInstance, kPlayerActor, Fake.ControllerInstance);
            Fake.PlayerData = reinterpret_cast<std::uintptr_t>(m_fields) + 0xF200;
            Put<std::uintptr_t>(Fake.PlayerData, 0, Fake.PlayerInstance);
            Put<std::uintptr_t>(Fake.Engine, kGamePlayers, Fake.PlayerData);
            Put<std::int32_t>(Fake.Engine, kGamePlayers + o.Pointer, 1);
            Put<std::int32_t>(Fake.Engine, kGamePlayers + o.Pointer + 4, 1);

            for (const std::uintptr_t at : early64)
                meta(at, Fake.ObjectClass, Fake.EnginePackage);
            while (m_objectCount < kObjectCount)
            {
                const std::uintptr_t at = Object(filler, static_cast<std::uint32_t>(m_objectCount));
                meta(at, Fake.ObjectClass, Fake.EnginePackage);
            }
            for (int i = 0; m_nameCount < kNameCount; ++i)
            {
                char text[32];
                std::snprintf(text, sizeof(text), "Padding%d", i);
                Name(text);
            }

            g_names = { reinterpret_cast<std::uintptr_t>(m_nameArray), m_nameCount, kNameCount + 64 };
            g_objects = { reinterpret_cast<std::uintptr_t>(m_items), m_objectCount, kObjectCount + 0x100 };
            Fake.Names = reinterpret_cast<std::uintptr_t>(&g_names);
            Fake.Objects = reinterpret_cast<std::uintptr_t>(&g_objects);
            return true;
        }

        std::uint32_t Name(std::string_view text, bool wide = false)
        {
            if (const auto it = m_known.find(std::string(text)); it != m_known.end())
                return it->second;
            const Offsets& o = Native(m_early);
            const std::uint32_t index = static_cast<std::uint32_t>(m_nameCount++);
            // Early builds pack the entries byte by byte, so most of them sit at odd addresses.
            const std::uintptr_t entry = reinterpret_cast<std::uintptr_t>(m_names) + static_cast<std::uintptr_t>(index) * (m_early ? kNameStride + 1 : kNameStride) + (m_early ? 1 : 0);
            if (o.EntryIndex > 0)
                Put<std::uint64_t>(entry, 0, 0x0007001000000000ull);
            const bool unicode = m_early && sizeof(void*) == 4;
            wide = wide || unicode;
            Put<std::uint32_t>(entry, o.EntryIndex, unicode ? index : (index << 1) | (wide ? 1u : 0u));
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if (wide)
                    Put<std::uint16_t>(entry, o.EntryName + static_cast<int>(2 * i), static_cast<std::uint16_t>(static_cast<unsigned char>(text[i])));
                else
                    Put<char>(entry, o.EntryName + static_cast<int>(i), text[i]);
            }
            Put<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(m_nameArray), static_cast<int>(index * sizeof(std::uintptr_t)), entry);
            m_known[std::string(text)] = index;
            return index;
        }

        // After Build: a class (not in GObjObjects) whose child chain has `nodes` fields, script structs except the last,
        // which is a function. broken: the link after the middle node is no pointer. Returns the class; *last gets the
        // function (#8: a chain of exactly kMaxFields nodes is complete, one more is cut).
        std::uintptr_t LongChain(int nodes, std::uintptr_t* last, bool broken = false)
        {
            const Offsets& o = Native(m_early);
            constexpr std::size_t kStride = 0x100;
            m_chain = Allocate(static_cast<std::size_t>(nodes + 1) * kStride + 0x1000);
            if (!m_chain || nodes < 1)
                return 0;
            const std::uint32_t name = Name("Filler");
            const std::uintptr_t holder = reinterpret_cast<std::uintptr_t>(m_chain);
            Header(holder, 0, name, 1);
            Put<std::uintptr_t>(holder, o.Class, Fake.ClassClass);
            std::uintptr_t next = 0;
            for (int i = nodes - 1; i >= 0; --i)
            {
                const std::uintptr_t at = holder + static_cast<std::uintptr_t>(i + 1) * kStride;
                Header(at, 0, name, static_cast<std::uint32_t>(i + 2));
                Put<std::uintptr_t>(at, o.Class, i == nodes - 1 ? Fake.FunctionClass : Fake.ScriptStructClass);
                Put<std::uintptr_t>(at, o.Outer, holder);
                Put<std::uintptr_t>(at, o.Next, broken && i == nodes / 2 ? std::uintptr_t(0x10) : next);
                if (i == nodes - 1 && last)
                    *last = at;
                next = at;
            }
            Put<std::uintptr_t>(holder, o.Children, next);
            return holder;
        }

        Image Fake{};

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

        std::uintptr_t Object(const char* name, std::uint32_t number = 0, bool wide = false)
        {
            return Object(Name(name, wide), number);
        }

        std::uintptr_t Object(std::uint32_t name, std::uint32_t number)
        {
            const Offsets& o = Native(m_early);
            const int index = m_objectCount++;
            const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(m_objects) + static_cast<std::uintptr_t>(index) * kObjectStride;
            Header(at, index, name, number);
            Put<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(m_items), static_cast<int>(static_cast<std::size_t>(index) * sizeof(std::uintptr_t)), at);
            (void)o;
            return at;
        }

        void Header(std::uintptr_t at, int index, std::uint32_t name, std::uint32_t number)
        {
            const Offsets& o = Native(m_early);
            Put<std::uintptr_t>(at, 0, reinterpret_cast<std::uintptr_t>(&g_vtable[0]));
            Put<std::uint64_t>(at, o.ObjectFlags, 0x0004000000000001ull);
            Put<std::int32_t>(at, o.Index, index);
            Put<std::uint32_t>(at, o.Name, name);
            Put<std::uint32_t>(at, o.Name + 4, number);
        }

        // A UProperty is a UObject too: it gets an index in GObjObjects and lives in the field block.
        std::uintptr_t Property(std::uintptr_t klass, std::uintptr_t owner, const char* name, std::int32_t offset, std::int32_t size, std::uintptr_t next)
        {
            const Offsets& o = Native(m_early);
            const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(m_fields) + m_fieldUsed;
            m_fieldUsed += 0x100;
            const int index = m_objectCount++;
            Header(at, index, Name(name), 0);
            Put<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(m_items), static_cast<int>(static_cast<std::size_t>(index) * sizeof(std::uintptr_t)), at);
            Put<std::uintptr_t>(at, o.Class, klass);
            Put<std::uintptr_t>(at, o.Outer, owner);
            Put<std::uintptr_t>(at, o.Next, next);
            Put<std::int32_t>(at, o.ArrayDim, 1);
            Put<std::int32_t>(at, o.ArrayDim + 4, size);
            Put<std::uint64_t>(at, o.ArrayDim + 8, 0x0000000000000001ull);
            Put<std::int32_t>(at, o.Offset, offset);
            if (m_noLocation)
                Put<std::uintptr_t>(at, (o.Offset + 4 + o.Pointer - 1) & ~(o.Pointer - 1), owner);
            return at;
        }

        void Next(std::uintptr_t field, std::uintptr_t next) { Put<std::uintptr_t>(field, Native(m_early).Next, next); }
        void Super(std::uintptr_t type, std::uintptr_t parent) { Put<std::uintptr_t>(type, Native(m_early).Super, parent); }
        // Late 32-bit UE3 builds can keep PropertySize in the low 16 bits with another 16-bit field above it.
        void Size(std::uintptr_t type, std::int32_t size)
        {
            const bool narrow = !m_early && sizeof(void*) == 4;
            Put<std::int32_t>(type, Native(m_early).PropertySize, narrow ? (size | 0x04000000) : size);
        }
        void Flags(std::uintptr_t function, std::uint32_t flags) { Put<std::uint32_t>(function, Native(m_early).FunctionFlags, flags); }

        bool m_early = false;
        bool m_noLocation = false;
        std::uint8_t* m_names = nullptr;
        std::uint8_t* m_nameArray = nullptr;
        std::uint8_t* m_objects = nullptr;
        std::uint8_t* m_items = nullptr;
        std::uint8_t* m_fields = nullptr;
        std::uint8_t* m_chain = nullptr;
        int m_nameCount = 0;
        int m_objectCount = 0;
        std::size_t m_fieldUsed = 0;
        std::map<std::string, std::uint32_t> m_known;
    };
}
