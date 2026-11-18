#pragma once

#include "Target/ManagedRuntime.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Il2CppRuntime final : public RuntimeBackend
    {
    public:
        using RuntimeBackend::RuntimeBackend;

        ManagedRuntime Kind() const override { return ManagedRuntime::Il2Cpp; }
        Status Attach(const std::string& module) override;
        void Detach() override;
        Status Enumerate(WorkProgress* progress, bool starting) override;
        void EnsureMembers(int classIndex) override;
        std::optional<std::string> ResolveHead(std::uint64_t header, bool allowShape) override;
        std::optional<std::string> MethodNameAt(std::uint64_t address) override;
        std::optional<std::string> MethodStartNameAt(std::uint64_t address) override;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> MethodRange(std::uint64_t address) override;
        std::size_t MethodsSharingCode(std::uint64_t address) override;
        bool CompilesOnDemand() const override { return false; }
        std::optional<std::uint64_t> Compile(std::uint64_t) override { return std::nullopt; }

    private:
        // A method's code in IL2CPP's address index: [Start, End), the class
        // index, the MethodInfo*, the name once it was asked for, and how many
        // methods share this code (identical bodies are folded together).
        struct CodeEntry
        {
            std::uint64_t Start = 0;
            std::uint64_t End = 0;
            std::uint64_t Method = 0;
            int Class = -1;
            std::uint32_t Sharing = 1;
            std::string Name;
        };
        // Offsets learned from the runtime's own answers (il2cpp_class_get_name
        // returns klass->name, and so on), so names can be read from memory
        // without handing a pointer from game memory to the runtime.
        struct Il2CppShape
        {
            int ClassImage = -1;
            int ClassName = -1;
            int ClassNamespace = -1;
            int MethodName = -1;
        };

        std::string MethodName(std::uint64_t method);
        CodeEntry* FindCode(std::uint64_t address);
        const std::string& CodeName(CodeEntry& entry);

        // Every method's code, sorted by Start (one entry per distinct start),
        // the learned struct offsets, the Il2CppThread* of the worker and the
        // names of the images.
        std::vector<CodeEntry> m_code;
        Il2CppShape m_shape;
        std::uint64_t m_attached = 0;
        std::map<std::uint64_t, std::string> m_imageNames;
    };
}
