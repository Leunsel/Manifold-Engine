#include "Target/PatchScan.h"

#include "Core/Text.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint32_t kGap = 4;
        constexpr std::uint32_t kChunk = 0x10000;
        constexpr std::uint32_t kPage = 0x1000;
        constexpr std::uint64_t kStoredBytes = 0x10000;

        template <typename T>
        bool ReadAt(const std::vector<std::uint8_t>& data, std::size_t offset, T& out)
        {
            if (offset > data.size() || data.size() - offset < sizeof(T))
                return false;
            std::memcpy(&out, data.data() + offset, sizeof(T));
            return true;
        }

        void AddDelta(std::vector<std::uint8_t>& image, std::uint32_t rva, std::uint64_t delta, bool wide)
        {
            if (wide)
            {
                if (static_cast<std::uint64_t>(rva) + 8 > image.size())
                    return;
                std::uint64_t value = 0;
                std::memcpy(&value, image.data() + rva, 8);
                value += delta;
                std::memcpy(image.data() + rva, &value, 8);
                return;
            }
            if (static_cast<std::uint64_t>(rva) + 4 > image.size())
                return;
            std::uint32_t value = 0;
            std::memcpy(&value, image.data() + rva, 4);
            value += static_cast<std::uint32_t>(delta);
            std::memcpy(image.data() + rva, &value, 4);
        }

        void ApplyRelocations(DiskImage& image, std::uint32_t start, std::uint32_t size, std::uint64_t delta)
        {
            const std::uint64_t limit = std::min<std::uint64_t>(image.Bytes.size(), static_cast<std::uint64_t>(start) + size);
            std::uint64_t at = start;
            while (at + sizeof(IMAGE_BASE_RELOCATION) <= limit)
            {
                IMAGE_BASE_RELOCATION block{};
                std::memcpy(&block, image.Bytes.data() + at, sizeof(block));
                if (block.SizeOfBlock < sizeof(block) || at + block.SizeOfBlock > limit)
                    break;
                const std::uint32_t count = (block.SizeOfBlock - static_cast<std::uint32_t>(sizeof(block))) / 2;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    WORD entry = 0;
                    std::memcpy(&entry, image.Bytes.data() + at + sizeof(block) + i * 2ull, 2);
                    const int type = entry >> 12;
                    const std::uint32_t rva = block.VirtualAddress + (entry & 0xFFFu);
                    if (type == IMAGE_REL_BASED_DIR64)
                        AddDelta(image.Bytes, rva, delta, true);
                    else if (type == IMAGE_REL_BASED_HIGHLOW)
                        AddDelta(image.Bytes, rva, delta, false);
                }
                at += block.SizeOfBlock;
            }
        }

        bool Skipped(const DiskImage& image, std::uint32_t rva)
        {
            return std::any_of(image.Skip.begin(), image.Skip.end(), [&](const ImageRange& range) { return rva >= range.Start && rva < range.End; });
        }

        struct RunBuilder
        {
            const Process& Live;
            const ModuleInfo& Module;
            const DiskImage& Image;
            ModulePatchSummary& Summary;
            std::vector<CodePatch>& Patches;
            bool Open = false;
            std::uint32_t Start = 0;
            std::uint32_t End = 0;

            void Add(std::uint32_t rva)
            {
                if (Open && rva - End < kGap)
                {
                    End = rva + 1;
                    return;
                }
                Flush();
                Open = true;
                Start = rva;
                End = rva + 1;
            }

            void Flush()
            {
                if (!Open)
                    return;
                Open = false;
                if (Summary.Patches >= kMaxPatchesPerModule)
                {
                    Summary.TooMany = true;
                    return;
                }
                ++Summary.Patches;
                CodePatch patch;
                patch.Address = Module.Base + Start;
                patch.Module = Module.Name;
                patch.Offset = Start;
                patch.Size = End - Start;
                const std::size_t stored = static_cast<std::size_t>(std::min<std::uint64_t>(patch.Size, kStoredBytes));
                patch.Original.assign(Image.Bytes.begin() + Start, Image.Bytes.begin() + Start + stored);
                patch.Current.resize(stored);
                if (!Live.ReadExact(patch.Address, patch.Current.data(), stored))
                    patch.Current.clear();
                Patches.push_back(std::move(patch));
            }
        };
    }

    Status MapDiskImage(const std::filesystem::path& path, std::uint64_t loadedBase, std::uint64_t loadedSize, DiskImage& image)
    {
        image = DiskImage();
        const std::string shown = WideToUtf8(path.wstring());
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return Status::Fail(ErrorCode::ModuleFileUnreadable, shown);
        const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        IMAGE_DOS_HEADER dos{};
        if (!ReadAt(raw, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
            return Status::Fail(ErrorCode::ModuleFileDifferent, shown);
        const std::size_t nt = static_cast<std::size_t>(dos.e_lfanew);
        DWORD signature = 0;
        IMAGE_FILE_HEADER header{};
        if (!ReadAt(raw, nt, signature) || signature != IMAGE_NT_SIGNATURE || !ReadAt(raw, nt + sizeof(DWORD), header))
            return Status::Fail(ErrorCode::ModuleFileDifferent, shown);
        const std::size_t optional = nt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        WORD magic = 0;
        if (!ReadAt(raw, optional, magic))
            return Status::Fail(ErrorCode::ModuleFileDifferent, shown);
        std::uint64_t imageBase = 0;
        std::uint32_t sizeOfImage = 0;
        std::uint32_t sizeOfHeaders = 0;
        IMAGE_DATA_DIRECTORY relocations{};
        IMAGE_DATA_DIRECTORY imports{};
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER64 options{};
            if (!ReadAt(raw, optional, options))
                return Status::Fail(ErrorCode::ModuleFileDifferent, shown);
            imageBase = options.ImageBase;
            sizeOfImage = options.SizeOfImage;
            sizeOfHeaders = options.SizeOfHeaders;
            if (options.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC)
                relocations = options.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (options.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IAT)
                imports = options.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
            image.Is64Bit = true;
        }
        else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER32 options{};
            if (!ReadAt(raw, optional, options))
                return Status::Fail(ErrorCode::ModuleFileDifferent, shown);
            imageBase = options.ImageBase;
            sizeOfImage = options.SizeOfImage;
            sizeOfHeaders = options.SizeOfHeaders;
            if (options.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC)
                relocations = options.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (options.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IAT)
                imports = options.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
            image.Is64Bit = false;
        }
        else
        {
            return Status::Fail(ErrorCode::ModuleFileDifferent, shown);
        }
        if (sizeOfImage == 0 || sizeOfImage > 0x40000000u || (loadedSize && sizeOfImage != loadedSize))
            return Status::Fail(ErrorCode::ModuleFileDifferent, shown);

        image.Bytes.assign(sizeOfImage, 0);
        std::memcpy(image.Bytes.data(), raw.data(), std::min<std::size_t>({ static_cast<std::size_t>(sizeOfHeaders), raw.size(), static_cast<std::size_t>(sizeOfImage) }));
        const std::size_t sections = optional + header.SizeOfOptionalHeader;
        for (WORD index = 0; index < header.NumberOfSections; ++index)
        {
            IMAGE_SECTION_HEADER section{};
            if (!ReadAt(raw, sections + index * sizeof(IMAGE_SECTION_HEADER), section))
                return Status::Fail(ErrorCode::ModuleFileDifferent, shown);
            const std::uint32_t start = section.VirtualAddress;
            if (start >= sizeOfImage)
                continue;
            const std::uint32_t virtualSize = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
            const std::uint32_t span = std::min(virtualSize, sizeOfImage - start);
            const std::uint32_t rawSize = std::min<std::uint32_t>(section.SizeOfRawData, span);
            if (rawSize && section.PointerToRawData < raw.size())
                std::memcpy(image.Bytes.data() + start, raw.data() + section.PointerToRawData, std::min<std::size_t>(rawSize, raw.size() - section.PointerToRawData));
            if (span && (section.Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)))
                image.Code.push_back({ start, start + span });
        }
        const std::uint64_t delta = loadedBase - imageBase;
        if (delta && relocations.VirtualAddress && relocations.Size)
            ApplyRelocations(image, relocations.VirtualAddress, relocations.Size, delta);
        if (imports.VirtualAddress && imports.Size)
            image.Skip.push_back({ imports.VirtualAddress, imports.VirtualAddress + imports.Size });
        return Status::Success();
    }

    ModulePatchSummary ScanModuleForPatches(const Process& process, const ModuleInfo& module, std::vector<CodePatch>& patches, const std::atomic<bool>* cancel)
    {
        ModulePatchSummary summary;
        summary.Module = module.Name;
        summary.Base = module.Base;
        DiskImage image;
        summary.Problem = MapDiskImage(module.Path, module.Base, module.Size, image);
        if (!summary.Problem)
            return summary;
        RunBuilder runs{ process, module, image, summary, patches };
        std::vector<std::uint8_t> live(kChunk);
        std::vector<std::uint8_t> readable;
        for (const ImageRange& range : image.Code)
        {
            summary.CodeBytes += range.End - range.Start;
            for (std::uint32_t at = range.Start; at < range.End; at += kChunk)
            {
                if (cancel && cancel->load())
                    return summary;
                const std::uint32_t size = std::min(kChunk, range.End - at);
                const std::uint32_t pages = (size + kPage - 1) / kPage;
                readable.assign(pages, 1);
                if (!process.ReadExact(module.Base + at, live.data(), size))
                {
                    for (std::uint32_t page = 0; page < pages; ++page)
                    {
                        const std::uint32_t offset = page * kPage;
                        const std::uint32_t length = std::min(kPage, size - offset);
                        if (!process.ReadExact(module.Base + at + offset, live.data() + offset, length))
                        {
                            readable[page] = 0;
                            summary.UnreadableBytes += length;
                        }
                    }
                }
                const std::uint8_t* disk = image.Bytes.data() + at;
                for (std::uint32_t i = 0; i < size; ++i)
                {
                    if (!readable[i / kPage])
                    {
                        runs.Flush();
                        i = (i / kPage + 1) * kPage - 1;
                        continue;
                    }
                    if (live[i] == disk[i] || Skipped(image, at + i))
                        continue;
                    ++summary.ChangedBytes;
                    runs.Add(at + i);
                }
            }
            runs.Flush();
        }
        runs.Flush();
        return summary;
    }

    PatchScanResult ScanForPatches(const Process& process, const std::vector<ModuleInfo>& modules, PatchScanProgress* progress)
    {
        PatchScanResult result;
        if (progress)
        {
            progress->Done = 0;
            progress->Total = static_cast<int>(modules.size());
        }
        for (const ModuleInfo& module : modules)
        {
            if (progress && progress->Cancel.load())
                break;
            result.Modules.push_back(ScanModuleForPatches(process, module, result.Patches, progress ? &progress->Cancel : nullptr));
            if (progress)
                ++progress->Done;
        }
        return result;
    }
}
