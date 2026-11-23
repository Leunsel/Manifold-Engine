#pragma once

#include "Target/FrameCalls.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::Target
{
    namespace ExplorerLimits
    {
        constexpr int kTreeGroup = 1;
        constexpr int kSearchGroup = 2;
        constexpr int kScratchGroup = 3;
        constexpr int kPermanentGroup = 4;
        constexpr int kStoppedGroup = 5;
        constexpr std::size_t kMaxShownElements = 256;
        constexpr std::size_t kMaxNodes = 200000;
        constexpr std::size_t kMaxChildren = 20000;
        constexpr std::size_t kMaxHits = 5000;
        constexpr std::size_t kMaxShownChildren = 500;

        constexpr int InspectionGroup(int id)
        {
            return 1000 + id;
        }
    }

    struct Il2CppHeld
    {
        std::uint64_t Handle = 0;
        std::uint64_t Address = 0;
        std::vector<int> Groups;
    };

    struct Il2CppExplorerState
    {
        FrameCalls Frame;
        bool Ready = false;
        std::string Hook;
        std::optional<bool> BoxedThis;
        std::uint64_t Invoke = 0;
        std::uint64_t HandleNew = 0;
        std::uint64_t HandleFree = 0;
        std::uint64_t StringNew = 0;
        std::uint64_t TypeGetObject = 0;
        std::uint64_t ObjectNew = 0;
        std::uint64_t ValueBox = 0;
        std::uint64_t FieldSet = 0;
        std::uint64_t FieldStaticSet = 0;
        std::map<std::string, std::uint64_t> Methods;
        std::map<std::string, std::uint64_t> Icalls;
        std::map<int, Il2CppHeld> Slots;
        int NextSlot = 1;
        std::vector<std::uint64_t> Surplus;
        std::optional<bool> OriginalRunInBackground;
        int DontDestroySlot = 0;
    };
}
