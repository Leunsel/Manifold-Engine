#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold
{
    class WorkProgress;
}

namespace Manifold::Target
{
    class MonoBridge;

    enum class ManagedRuntime
    {
        None,
        Mono,
        Il2Cpp,
        Clr  // .NET Framework or .NET (CoreCLR), read from outside the game
    };

    struct MonoObjectLayout
    {
        std::uint64_t Pointer = 8;
        std::uint64_t ObjectHeader = 0x10;
        std::uint64_t StringLength = 0x10;
        std::uint64_t StringChars = 0x14;
        std::uint64_t ArrayLength = 0x18;
        std::uint64_t ArrayData = 0x20;
    };

    constexpr MonoObjectLayout MonoLayoutFor(int pointerSize)
    {
        const std::uint64_t pointer = pointerSize == 4 ? 4 : 8;
        return { pointer, 2 * pointer, 2 * pointer, 2 * pointer + 4, 3 * pointer, 4 * pointer };
    }

    // One type on the GC heap: how many objects, how many bytes.
    struct ManagedHeapType
    {
        std::uint64_t Handle = 0;
        std::string Name;
        std::uint64_t Count = 0;
        std::uint64_t Bytes = 0;
    };

    struct ManagedHeap
    {
        std::uint64_t Objects = 0;
        std::uint64_t Bytes = 0;
        std::uint64_t Skipped = 0;
        std::uint32_t Segments = 0;
        double Seconds = 0.0;
        std::vector<ManagedHeapType> Types;  // most bytes first
    };

    struct ManagedInstances
    {
        std::uint64_t Total = 0;
        std::vector<std::uint64_t> Addresses;
    };

    enum class ManagedObjectKind
    {
        String,
        Free,
        Object,
        Array,
        Other
    };

    // An object as the runtime describes it: its class, its size and, for an
    // array or a string, its elements.
    struct ManagedObject
    {
        std::uint64_t Address = 0;
        std::uint64_t Handle = 0;
        std::string Name;
        ManagedObjectKind Kind = ManagedObjectKind::Object;
        std::uint64_t Size = 0;
        std::uint64_t ElementHandle = 0;
        std::string ElementName;
        int ElementType = 0;   // CorElementType of an array's elements
        int Rank = 0;
        std::uint64_t Length = 0;
        std::uint64_t ElementSize = 0;
        std::uint64_t Data = 0;  // the first element
    };

    class RuntimeBackend
    {
    public:
        explicit RuntimeBackend(MonoBridge& bridge) : m_bridge(bridge) {}
        virtual ~RuntimeBackend() = default;

        RuntimeBackend(const RuntimeBackend&) = delete;
        RuntimeBackend& operator=(const RuntimeBackend&) = delete;

        virtual ManagedRuntime Kind() const = 0;
        virtual Status Attach(const std::string& module) = 0;
        virtual void Detach() = 0;
        virtual Status Enumerate(WorkProgress* progress, bool starting) = 0;
        virtual void EnsureMembers(int classIndex) = 0;
        virtual void ResolveValueTypes(int) {}
        virtual std::optional<std::string> ResolveHead(std::uint64_t header, bool allowFallback) = 0;
        virtual std::optional<std::string> MethodNameAt(std::uint64_t address) = 0;
        virtual std::optional<std::string> MethodStartNameAt(std::uint64_t address) = 0;
        virtual std::optional<std::pair<std::uint64_t, std::uint64_t>> MethodRange(std::uint64_t address) = 0;
        virtual std::size_t MethodsSharingCode(std::uint64_t address) = 0;
        virtual bool CompilesOnDemand() const = 0;
        virtual std::optional<std::uint64_t> Compile(std::uint64_t method) = 0;
        virtual std::uint64_t ManagedClass(std::string_view, std::string_view) { return 0; }
        // The class index of a class's parent when the backend knows it
        // without the runtime's C API (nullopt: ask mono_/il2cpp_class_get_parent).
        virtual std::optional<int> ParentIndex(int) { return std::nullopt; }
        // The address of a static field, for runtimes that report statics.
        virtual std::optional<std::uint64_t> StaticAddress(int, std::string_view) { return std::nullopt; }
        // The runtime's flavour and version, when the backend knows it.
        virtual std::string Description() const { return {}; }
        // The GC heap, for runtimes that can walk it from outside.
        virtual Result<ManagedHeap> ReadHeap() { return Status::Fail(ErrorCode::MonoCallFailed, "this runtime's heap cannot be read"); }
        virtual Result<ManagedInstances> Instances(std::uint64_t, std::uint32_t) { return Status::Fail(ErrorCode::MonoCallFailed, "this runtime's heap cannot be read"); }
        virtual std::optional<ManagedObject> ObjectAt(std::uint64_t) { return std::nullopt; }

    protected:
        MonoBridge& m_bridge;
    };
}
