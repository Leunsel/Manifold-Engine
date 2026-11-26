#pragma once

#include "Scan/ValueType.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Scan
{
    // The value types of CodeStage's Anti-Cheat Toolkit (ACTk) for Unity: a
    // struct that keeps its value XOR'd with a key of its own, so the plain
    // number is nowhere in memory. A fake copy may sit beside it as a
    // honeypot: with fakeValueActive set, ACTk's ObscuredCheatingDetector
    // compares the two and reports a change of the fake one.
    enum class ObscuredKind
    {
        Int,
        UInt,
        Long,
        ULong,
        Short,
        UShort,
        Byte,
        SByte,
        Float,
        Double,
        Bool,
        Vector2,
        Vector3,
        Quaternion
    };

    // Offsets inside the unboxed struct.
    struct ObscuredLayout
    {
        int Key = 0;
        int KeyBytes = 4;
        int Hidden = 4;
        int HiddenBytes = 4;
        int Inited = 8;
        int Fake = 12;
        int FakeActive = 16;
        int Old = -1;  // hiddenValueOld of float and double: a byte[] ACTk migrates older saves from
        int OldBytes = 0;
        int Size = 20;
    };

    struct ObscuredType
    {
        ObscuredKind Kind;
        const char* Name;   // the custom type: "ACTk ObscuredInt"
        const char* Class;  // the managed struct: "ObscuredInt"
        ValueType Plain;    // the type of one component of the plain value
        int Components;
        bool Signed;
    };

    inline constexpr const char* kObscuredNamespace = "CodeStage.AntiCheat.ObscuredTypes";

    std::span<const ObscuredType> ObscuredTypes();
    const ObscuredType* FindObscured(std::string_view customName);
    // A managed type name, with or without the namespace.
    const ObscuredType* FindObscuredClass(std::string_view managedName);
    // ACTk 2.x as IL2CPP and Mono lay it out; float and double hold a reference, so they depend on the pointer size.
    ObscuredLayout ObscuredLayoutOf(ObscuredKind kind, int pointerSize);
    int ObscuredComponentSize(const ObscuredType& type);
    int ObscuredPlainSize(const ObscuredType& type);
    // Numeric compares work on the plain value for these; vectors only compare whole.
    bool ObscuredScalar(const ObscuredType& type);

    enum class ObscuredState
    {
        Plain,      // decoded
        Unset,      // never assigned: ACTk reads it as 0
        Invalid     // these bytes are no obscured value
    };

    // The plain value of the struct at `block` (Size bytes) into `plain` (ObscuredPlainSize bytes).
    // Strict also rejects an unset struct and a fake copy that differs, which keeps a scan to real values.
    ObscuredState ObscuredDecode(const ObscuredType& type, const ObscuredLayout& layout, const std::uint8_t* block, std::uint8_t* plain, bool strict);
    // The struct with `plain` in it. It keeps the key; an unset struct gets `freshKey` (made non-zero).
    // The fake copy follows when ACTk compares it, and hiddenValueOld is cleared so ACTk migrates nothing over it.
    std::vector<std::uint8_t> ObscuredEncode(const ObscuredType& type, const ObscuredLayout& layout, std::span<const std::uint8_t> current,
        std::span<const std::uint8_t> plain, std::uint64_t freshKey);

    std::string ObscuredText(const ObscuredType& type, std::span<const std::uint8_t> plain);
    // "12", "0x1F", "1.5", "true", "1, 2, 3" for a Vector3.
    std::optional<std::vector<std::uint8_t>> ObscuredParse(const ObscuredType& type, std::string_view text);
}
