#pragma once

#include "Core/Errors.h"
#include "Target/Process.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Manifold::Target
{
    struct CodePatch
    {
        std::uint64_t Address = 0;
        std::string Module;
        std::uint64_t Offset = 0;
        std::uint64_t Size = 0;
        std::vector<std::uint8_t> Original;
        std::vector<std::uint8_t> Current;

        bool Complete() const { return Original.size() == Size && Current.size() == Size; }
    };

    struct ModulePatchSummary
    {
        std::string Module;
        std::uint64_t Base = 0;
        std::uint64_t CodeBytes = 0;
        std::uint64_t ChangedBytes = 0;
        std::uint64_t UnreadableBytes = 0;
        std::size_t Patches = 0;
        bool TooMany = false;
        Status Problem;
    };

    struct PatchScanProgress
    {
        std::atomic<int> Done{ 0 };
        std::atomic<int> Total{ 0 };
        std::atomic<bool> Cancel{ false };
    };

    struct PatchScanResult
    {
        std::vector<CodePatch> Patches;
        std::vector<ModulePatchSummary> Modules;
    };

    struct ImageRange
    {
        std::uint32_t Start = 0;
        std::uint32_t End = 0;
    };

    struct DiskImage
    {
        std::vector<std::uint8_t> Bytes;
        std::vector<ImageRange> Code;
        std::vector<ImageRange> Skip;
        bool Is64Bit = true;
    };

    constexpr std::size_t kMaxPatchesPerModule = 2000;

    Status MapDiskImage(const std::filesystem::path& path, std::uint64_t loadedBase, std::uint64_t loadedSize, DiskImage& image);
    ModulePatchSummary ScanModuleForPatches(const Process& process, const ModuleInfo& module, std::vector<CodePatch>& patches, const std::atomic<bool>* cancel = nullptr);
    PatchScanResult ScanForPatches(const Process& process, const std::vector<ModuleInfo>& modules, PatchScanProgress* progress = nullptr);
}
