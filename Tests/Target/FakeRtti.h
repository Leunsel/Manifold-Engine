#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace FakeRtti
{
    namespace Zoo
    {
        struct Animal
        {
            virtual ~Animal() = default;
            virtual int Legs() const { return 4; }
            std::uint64_t Id = 0x11;
        };

        struct Walker
        {
            virtual ~Walker() = default;
            virtual int Speed() const { return 1; }
            std::uint64_t Steps = 0x22;
        };

        struct Dog : Animal
        {
            int Legs() const override { return 4; }
            std::uint64_t Bones = 0x33;
        };

        class Duck : public Animal, public Walker
        {
        public:
            int Legs() const override { return 2; }
            int Speed() const override { return 3; }
            std::uint64_t Feathers = 0x44;
        };

        struct Swimmer : virtual Animal
        {
            virtual int Depth() const { return 2; }
            std::uint64_t Laps = 0x55;
        };

        struct Fish : virtual Animal
        {
            std::uint64_t Scales = 0x66;
        };

        template <class T>
        struct Cage
        {
            virtual ~Cage() = default;
            T Inside{};
        };

        struct Keeper
        {
            virtual ~Keeper() = default;
            virtual int Kind() const { return 1; }
            std::uint64_t Before = 0x2222;
            Dog Pet;
            std::uint64_t After = 0x3333;
        };
    }

    template <class Word>
    struct ImageOf
    {
        Word Dog, Duck, DuckWalker, Swimmer, SwimmerAnimal, Fish, FishAnimal, Keeper, KeeperPet, KeeperAfter, KeeperSize, Cage;
        Word Base, Derived, Multi, MultiSecond, Virt, VirtPart, Owner, OwnerPart, OwnerSize;
        Word TableBase, TableMulti, TableMultiSecond, TablePart;
        Word BadSignature, BadSelf, BadCount, BadCountHuge, BadName, Plain;
    };

    using Image = ImageOf<std::uintptr_t>;

    struct Descriptor
    {
        std::uintptr_t VTable;
        std::uintptr_t Spare;
        char Name[48];
    };

    struct BaseDescriptor
    {
        std::uint32_t Type;
        std::uint32_t Contained;
        std::int32_t Member;
        std::int32_t TablePointer;
        std::int32_t TableSlot;
        std::uint32_t Attributes;
        std::uint32_t Hierarchy;
    };

    struct Hierarchy
    {
        std::uint32_t Signature;
        std::uint32_t Attributes;
        std::uint32_t Count;
        std::uint32_t Bases;
    };

    struct Locator
    {
        std::uint32_t Signature;
        std::uint32_t Offset;
        std::uint32_t Constructor;
        std::uint32_t Type;
        std::uint32_t Hierarchy;
        std::uint32_t Self;
    };

    struct Table
    {
        std::uintptr_t Locator;
        std::uintptr_t Functions[4];
    };

    enum Type { TBase, TDerived, TMulti, TSecond, TVirt, TVBase, TOwner, TPart, TBadName, TypeCount };
    enum Class { CBase, CDerived, CMulti, CVirt, COwner, CPart, CBadCount, CBadCountHuge, CBadName, ClassCount };
    enum Slot { LBase, LDerived, LMulti, LMultiSecond, LVirt, LOwner, LPart, LBadSignature, LBadSelf, LBadCount, LBadCountHuge, LBadName, SlotCount };

    inline constexpr std::size_t kPad = 0x1000 / sizeof(std::uintptr_t);
    inline constexpr std::size_t kWord = sizeof(std::uintptr_t);

    struct Real
    {
        Zoo::Dog Dog;
        std::uintptr_t PadDog[kPad];
        Zoo::Duck Duck;
        std::uintptr_t PadDuck[kPad];
        Zoo::Swimmer Swimmer;
        std::uintptr_t PadSwimmer[kPad];
        Zoo::Fish Fish;
        std::uintptr_t PadFish[kPad];
        Zoo::Keeper Keeper;
        std::uintptr_t PadKeeper[kPad];
        Zoo::Cage<int> Cage;
        std::uintptr_t PadCage[kPad];
    };

    struct Store
    {
        Descriptor Types[TypeCount];
        BaseDescriptor Bases[16];
        std::uint32_t BaseLists[16];
        Hierarchy Hierarchies[ClassCount];
        Locator Locators[SlotCount];
        Table Tables[SlotCount];
        std::int32_t VirtualBases[2];
        std::uintptr_t Base[2];
        std::uintptr_t PadBase[kPad];
        std::uintptr_t Derived[5];
        std::uintptr_t PadDerived[kPad];
        std::uintptr_t Multi[4];
        std::uintptr_t PadMulti[kPad];
        std::uintptr_t Virt[4];
        std::uintptr_t PadVirt[kPad];
        std::uintptr_t Owner[5];
        std::uintptr_t PadOwner[kPad];
        std::uintptr_t Bad[5][2];
        std::uintptr_t PadBad[kPad];
    };

    inline Real g_real;
    inline Store g_store;
    inline Store* volatile g_storeAt = &g_store;
    inline Real* volatile g_realAt = &g_real;

    inline int Stub() { return 7; }

    class Builder
    {
    public:
        Image Fake{};

        bool Build()
        {
            m_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
            m_store = g_storeAt;
            Store& s = *m_store;
            const char* names[TypeCount] = { ".?AVBase@Synth@@", ".?AVDerived@Synth@@", ".?AVMulti@Synth@@", ".?AUSecond@Synth@@", ".?AVVirt@Synth@@", ".?AVVBase@Synth@@",
                ".?AVOwner@Synth@@", ".?AVPart@Synth@@", "?AVNope@Synth@@" };
            for (int i = 0; i < TypeCount; ++i)
                ::strcpy_s(s.Types[i].Name, names[i]);

            const std::int32_t second = static_cast<std::int32_t>(2 * kWord);
            Chain(CBase, 0, { { TBase, 0, 0, -1, 0 } });
            Chain(CDerived, 0, { { TDerived, 1, 0, -1, 0 }, { TBase, 0, 0, -1, 0 } });
            Chain(CMulti, 1, { { TMulti, 2, 0, -1, 0 }, { TBase, 0, 0, -1, 0 }, { TSecond, 0, second, -1, 0 } });
            Chain(CVirt, 2, { { TVirt, 1, 0, -1, 0 }, { TVBase, 0, 0, 0, 4 } });
            Chain(COwner, 0, { { TOwner, 0, 0, -1, 0 } });
            Chain(CPart, 0, { { TPart, 0, 0, -1, 0 } });
            Chain(CBadCount, 0, { { TBase, 0, 0, -1, 0 } });
            Chain(CBadCountHuge, 0, { { TBase, 0, 0, -1, 0 } });
            Chain(CBadName, 0, { { TBadName, 0, 0, -1, 0 } });
            s.Hierarchies[CBadCount].Count = 0;
            s.Hierarchies[CBadCountHuge].Count = 513;

            Locate(LBase, TBase, CBase, 0);
            Locate(LDerived, TDerived, CDerived, 0);
            Locate(LMulti, TMulti, CMulti, 0);
            Locate(LMultiSecond, TMulti, CMulti, static_cast<std::uint32_t>(second));
            Locate(LVirt, TVirt, CVirt, static_cast<std::uint32_t>(second));
            Locate(LOwner, TOwner, COwner, 0);
            Locate(LPart, TPart, CPart, 0);
            Locate(LBadSignature, TBase, CBase, 0);
            Locate(LBadSelf, TBase, CBase, 0);
            Locate(LBadCount, TBase, CBadCount, 0);
            Locate(LBadCountHuge, TBase, CBadCountHuge, 0);
            Locate(LBadName, TBadName, CBadName, 0);
            s.Locators[LBadSignature].Signature = 7;
            if (kWord == 8)
                s.Locators[LBadSelf].Self += 4;
            else
                s.Locators[LBadSelf].Offset = 0x1000000;

            s.VirtualBases[0] = 0;
            s.VirtualBases[1] = second;
            s.Base[0] = TableOf(LBase);
            s.Base[1] = 0xB;
            s.Derived[0] = TableOf(LDerived);
            s.Derived[1] = reinterpret_cast<std::uintptr_t>(&s.Base[0]);
            s.Derived[2] = reinterpret_cast<std::uintptr_t>(&s.Multi[2]);
            s.Derived[3] = reinterpret_cast<std::uintptr_t>(&s.Multi[0]);
            s.Derived[4] = 0x77;
            s.Multi[0] = TableOf(LMulti);
            s.Multi[1] = 1;
            s.Multi[2] = TableOf(LMultiSecond);
            s.Multi[3] = 2;
            s.Virt[0] = reinterpret_cast<std::uintptr_t>(&s.VirtualBases[0]);
            s.Virt[1] = 7;
            s.Virt[2] = TableOf(LVirt);
            s.Virt[3] = 8;
            s.Owner[0] = TableOf(LOwner);
            s.Owner[1] = 0x1111;
            s.Owner[2] = TableOf(LPart);
            s.Owner[3] = 0x2222;
            s.Owner[4] = 0x3333;
            const Slot bad[5] = { LBadSignature, LBadSelf, LBadCount, LBadCountHuge, LBadName };
            for (int i = 0; i < 5; ++i)
            {
                s.Bad[i][0] = TableOf(bad[i]);
                s.Bad[i][1] = 0xBAD;
            }

            Real& r = *g_realAt;
            Fake.Dog = Address(&r.Dog);
            Fake.Duck = Address(&r.Duck);
            Fake.DuckWalker = Address(static_cast<Zoo::Walker*>(&r.Duck));
            Fake.Swimmer = Address(&r.Swimmer);
            Fake.SwimmerAnimal = Address(static_cast<Zoo::Animal*>(&r.Swimmer));
            Fake.Fish = Address(&r.Fish);
            Fake.FishAnimal = Address(static_cast<Zoo::Animal*>(&r.Fish));
            Fake.Keeper = Address(&r.Keeper);
            Fake.KeeperPet = Address(&r.Keeper.Pet);
            Fake.KeeperAfter = Address(&r.Keeper.After);
            Fake.KeeperSize = sizeof(Zoo::Keeper);
            Fake.Cage = Address(&r.Cage);
            Fake.Base = Address(&s.Base[0]);
            Fake.Derived = Address(&s.Derived[0]);
            Fake.Multi = Address(&s.Multi[0]);
            Fake.MultiSecond = Address(&s.Multi[2]);
            Fake.Virt = Address(&s.Virt[0]);
            Fake.VirtPart = Address(&s.Virt[2]);
            Fake.Owner = Address(&s.Owner[0]);
            Fake.OwnerPart = Address(&s.Owner[2]);
            Fake.OwnerSize = sizeof(s.Owner);
            Fake.TableBase = TableOf(LBase);
            Fake.TableMulti = TableOf(LMulti);
            Fake.TableMultiSecond = TableOf(LMultiSecond);
            Fake.TablePart = TableOf(LPart);
            Fake.BadSignature = Address(&s.Bad[0][0]);
            Fake.BadSelf = Address(&s.Bad[1][0]);
            Fake.BadCount = Address(&s.Bad[2][0]);
            Fake.BadCountHuge = Address(&s.Bad[3][0]);
            Fake.BadName = Address(&s.Bad[4][0]);
            Fake.Plain = Address(&s.PadBase[kPad / 2]);
            return m_base != 0 && r.Dog.Legs() == 4;
        }

    private:
        struct BaseSpec
        {
            Type Of;
            std::uint32_t Contained;
            std::int32_t Member;
            std::int32_t TablePointer;
            std::int32_t TableSlot;
        };

        template <class T>
        static std::uintptr_t Address(T* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }

        template <class T>
        std::uint32_t Ref(T* pointer) const
        {
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pointer);
            return static_cast<std::uint32_t>(kWord == 8 ? address - m_base : address);
        }

        std::uintptr_t TableOf(Slot slot) const { return reinterpret_cast<std::uintptr_t>(&m_store->Tables[slot].Functions[0]); }

        void Chain(Class of, std::uint32_t attributes, std::initializer_list<BaseSpec> bases)
        {
            Store& s = *m_store;
            Hierarchy& hierarchy = s.Hierarchies[of];
            hierarchy = { 0, attributes, static_cast<std::uint32_t>(bases.size()), Ref(&s.BaseLists[m_nextBase]) };
            for (const BaseSpec& spec : bases)
            {
                BaseDescriptor& base = s.Bases[m_nextBase];
                base = { Ref(&s.Types[spec.Of]), spec.Contained, spec.Member, spec.TablePointer, spec.TableSlot, 0, Ref(&hierarchy) };
                s.BaseLists[m_nextBase] = Ref(&base);
                ++m_nextBase;
            }
        }

        void Locate(Slot slot, Type type, Class of, std::uint32_t offset)
        {
            Store& s = *m_store;
            Locator& locator = s.Locators[slot];
            locator = { kWord == 8 ? 1u : 0u, offset, 0, Ref(&s.Types[type]), Ref(&s.Hierarchies[of]), kWord == 8 ? Ref(&locator) : 0u };
            Table& table = s.Tables[slot];
            table.Locator = reinterpret_cast<std::uintptr_t>(&locator);
            for (std::uintptr_t& function : table.Functions)
                function = reinterpret_cast<std::uintptr_t>(&Stub);
        }

        std::uintptr_t m_base = 0;
        Store* m_store = nullptr;
        int m_nextBase = 0;
    };
}
