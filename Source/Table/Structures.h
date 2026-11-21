#pragma once

#include "Scan/ValueType.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Table
{
    enum class FieldDisplay
    {
        Unsigned,
        Signed,
        Hex,
        Binary
    };

    struct StructField
    {
        int Offset = 0;
        std::string Name;
        Scan::ValueType Type = Scan::ValueType::FourBytes;
        int Size = 4;
        FieldDisplay Display = FieldDisplay::Unsigned;
        bool Unicode = false;
        std::string ChildStructure;
        std::string CustomType;
        int BitStart = 0;
        int BitLength = 1;
    };

    // ∑ Limits for fields read from a table: the editor allows the same length, and offsets this far apart keep Offset + Size inside int.
    inline constexpr int kMaxFieldBytes = 4096;
    inline constexpr int kMaxFieldOffset = 0x10000000;

    int BitFieldSize(int bitStart, int bitLength);

    struct StructDefinition
    {
        std::string Name;
        std::vector<StructField> Fields;

        int Size() const;
        StructField* FieldAt(int offset);
        void Sort();
    };

    struct StructureStore
    {
        std::vector<std::unique_ptr<StructDefinition>> Items;
        std::uint64_t Generation = 1;

        StructDefinition* Find(const std::string& name);
        StructDefinition& Add(std::string name);
        void Remove(const StructDefinition& definition);
        void Clear();
    };

    void GuessFields(const Target::Process& process, StructDefinition& definition, std::uint64_t address, int offset, int size);
}
