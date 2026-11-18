#pragma once

#include "Core/Types.h"
#include "Target/Mono.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    std::span<const std::uint8_t> ManagedDumperImage();

    namespace ManagedDumpFormat
    {
        constexpr std::string_view kImagePrefix = "ManifoldDumper";
        constexpr std::string_view kImageName = "ManifoldDumper2";
        constexpr std::string_view kNamespace = "Manifold.Managed";
        constexpr std::string_view kClass = "Dumper";

        constexpr std::uint32_t kMagic = 0x4D44464Du;
        constexpr std::uint32_t kVersion = 2;
        constexpr std::uint32_t kNoString = 0xFFFFFFFFu;

        constexpr std::uint32_t kHeaderMonoHandles = 1u;
        constexpr std::uint32_t kHeaderOffsetsSure = 2u;

        constexpr std::uint32_t kTypeValueType = 1u;
        constexpr std::uint32_t kTypeEnum = 2u;
        constexpr std::uint32_t kTypeGenericDefinition = 4u;
        constexpr std::uint32_t kTypeInterface = 8u;
        constexpr std::uint32_t kTypeAbstract = 16u;
        constexpr std::uint32_t kTypeNested = 32u;

        constexpr std::uint32_t kFieldStatic = 1u;
        constexpr std::uint32_t kFieldLiteral = 2u;
        constexpr std::uint32_t kFieldInitOnly = 4u;

        constexpr std::uint32_t kMethodStatic = 1u;
        constexpr std::uint32_t kMethodVirtual = 2u;
        constexpr std::uint32_t kMethodAbstract = 4u;
        constexpr std::uint32_t kMethodConstructor = 8u;

        enum State : int
        {
            Idle = 0,
            Running = 1,
            Done = 2,
            Failed = 3
        };
    }

    struct ManagedDump
    {
        bool MonoHandles = false;
        bool OffsetsSure = false;
        std::vector<MonoClass> Classes;
        std::vector<std::uint32_t> Tokens;
        std::vector<std::uint32_t> Flags;
    };

    bool IsDumperImage(std::string_view image);
    Result<ManagedDump> ParseManagedDump(std::span<const std::uint8_t> blob);
}
