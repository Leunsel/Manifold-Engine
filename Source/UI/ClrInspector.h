#pragma once

#include "Target/ManagedRuntime.h"
#include "Target/Mono.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Manifold
{
    class Engine;
}

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::UI
{
    class Workspace;

    // A managed value read straight from the game's memory, as text: numbers,
    // bool, char; empty when it is not a primitive or cannot be read.
    std::string ManagedValueText(const Target::Process& process, std::uint64_t address, int typeCode, int enumCode);
    // The text of a .NET string object, at most maxChars characters.
    std::optional<std::string> ClrStringText(const Target::Process& process, std::uint64_t object, std::size_t maxChars);

    // The .NET side of the Mono / .NET window: the GC heap counted per type,
    // the objects of a type, and an inspector that walks from object to
    // object. Everything is read from outside the game; values are read live
    // from memory each frame. The inspector remembers how an object was
    // reached (a static field and the offsets after it), so a value can go to
    // the address list as a pointer that still finds it after the GC moves
    // the objects.
    class ClrInspector
    {
    public:
        explicit ClrInspector(Workspace& workspace);

        // The left pane's Objects tab: the heap summary and its filter.
        void ObjectsTab();
        // The right pane while the Objects tab is open.
        void Pane();

        // Opens an object reached through a static field (symbol is its
        // Class:Field, slot the address that holds the reference).
        void OpenFromStatic(const std::string& symbol, std::uint64_t slot);
        // Lists the objects of a class (from the class list).
        void FindInstances(std::uint64_t klass, const std::string& name);
        void Reset();

    private:
        // How an object was reached: [[[Base] + o1] + o2]... ends at the slot
        // that holds the object's reference. Without a stable base (an object
        // picked from the heap) Base is the object's own address.
        struct Path
        {
            std::string Base;
            std::vector<std::uint64_t> Offsets;
            std::string Label;  // "World.LocalPlayer.Bag._items[0]"
            bool Stable = false;
            bool BaseIsObject = false;
        };
        struct Page
        {
            std::uint64_t Address = 0;
            Path Route;
        };

        void Sync();
        void Open(std::uint64_t object, Path route);
        void Navigate(int position);
        void ReadHeap();
        void Inspect(const Page& page);
        void ObjectFields(const Page& page, std::uint64_t object);
        void ArrayElements(const Page& page, const Target::ManagedObject& array);
        void FieldRows(const Page& page, std::uint64_t object, const std::vector<Target::MonoField>& fields, std::uint64_t base, std::int64_t shift,
            const std::string& prefix, int depth);
        void ValueRow(const Page& page, const std::string& name, const std::string& typeName, std::uint64_t address, std::uint64_t objectOffset, int typeCode,
            int enumCode, std::uint64_t typeHandle, int valueSize, int depth);
        bool ReferenceCell(const Page& page, const std::string& name, std::uint64_t slot, std::uint64_t objectOffset);
        Path Through(const Page& page, std::uint64_t objectOffset, const std::string& name) const;
        void AddToList(const Page& page, const std::string& name, std::uint64_t objectOffset, int typeCode, int enumCode);
        void AddressMenu(const Page& page, const std::string& name, std::uint64_t address, std::uint64_t objectOffset, int typeCode, int enumCode);

        Workspace& m_workspace;
        Engine& m_engine;
        std::uint64_t m_generation = ~0ull;

        std::optional<Target::ManagedHeap> m_heap;
        std::string m_heapError;
        std::string m_filter;
        std::uint64_t m_selectedType = 0;

        std::string m_instancesOf;
        float m_listHeight = 0.0f;  // the object list's height while an object is open (0: not set yet)
        std::uint64_t m_instancesHandle = 0;
        std::optional<Target::ManagedInstances> m_instances;
        std::string m_instancesError;

        std::vector<Page> m_history;
        int m_position = -1;
        std::optional<Target::ManagedObject> m_current;  // what the runtime says about the open object
        std::uint64_t m_currentAddress = 0;
    };
}
