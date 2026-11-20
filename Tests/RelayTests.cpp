#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Asm/AutoAssembler.h"
#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Disasm/FlowGraph.h"
#include "Disasm/FunctionBounds.h"
#include "Lua/LuaEngine.h"
#include "Target/CodeMap.h"
#include "Target/Process.h"
#include "Target/Symbols.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        constexpr std::uint32_t kImageSize = 0x4000;
        constexpr std::uint32_t kText = 0x1000;
        constexpr std::uint32_t kData = 0x2000;
        constexpr std::uint32_t kUnwind = 0x3000;
        constexpr std::uint32_t kFileAlignment = 0x200;
        constexpr std::uint32_t kFrameAt = 0x00;
        constexpr std::uint32_t kLeafAt = 0x10;
        constexpr std::uint32_t kSecondAt = 0x40;
        constexpr std::uint32_t kHookFunctionAt = 0x60;
        constexpr std::uint32_t kHookAt = 0x67;
        constexpr std::uint32_t kPlayerAt = 0x80;
        constexpr std::uint64_t kSlots = 0x500;
        constexpr std::uint64_t kFarCall = 0x540;
        constexpr std::uint64_t kNearCall = 0x560;
        constexpr std::uint64_t kJunk = 0x59E;
        constexpr std::uint64_t kZeroData = 0x5C0;
        constexpr std::uint64_t kFarTarget = 0x1CB8D400000ull;
        constexpr const char* kOwner = "relaytest";

        const std::uint8_t kFrame[] = { 0x48, 0x83, 0xEC, 0x28, 0x31, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3 };
        const std::uint8_t kLeaf[] = { 0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3 };
        const std::uint8_t kHooked[] = { 0x85, 0xC9, 0x74, 0x03, 0x31, 0xC0, 0xC3, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3 };
        const std::uint8_t kPlayer[] = { 0x8B, 0x9A, 0x44, 0x13, 0x00, 0x00, 0x89, 0x4D, 0xF8, 0x8D, 0x0C, 0x24, 0xC3 };
        const char* const kSlotNames[] = { "PlayerController", "World", "HealthDec" };

        struct UnwindEntry
        {
            std::uint32_t Begin;
            std::uint32_t End;
            std::uint32_t Unwind;
        };

        template <class T>
        void Put(std::vector<std::uint8_t>& file, std::size_t at, const T& value)
        {
            std::memcpy(file.data() + at, &value, sizeof(T));
        }

        std::vector<std::uint8_t> RelayImageFile(std::uint64_t imageBase)
        {
            std::vector<std::uint8_t> file(kFileAlignment * 4, 0);
            IMAGE_DOS_HEADER dos{};
            dos.e_magic = IMAGE_DOS_SIGNATURE;
            dos.e_lfanew = 0x40;
            Put(file, 0, dos);
            IMAGE_NT_HEADERS64 nt{};
            nt.Signature = IMAGE_NT_SIGNATURE;
            nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
            nt.FileHeader.NumberOfSections = 3;
            nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
            nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL;
            IMAGE_OPTIONAL_HEADER64& optional = nt.OptionalHeader;
            optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
            optional.SizeOfCode = kFileAlignment;
            optional.SizeOfInitializedData = kFileAlignment * 2;
            optional.BaseOfCode = kText;
            optional.ImageBase = imageBase;
            optional.SectionAlignment = 0x1000;
            optional.FileAlignment = kFileAlignment;
            optional.MajorOperatingSystemVersion = 6;
            optional.MajorSubsystemVersion = 6;
            optional.SizeOfImage = kImageSize;
            optional.SizeOfHeaders = kFileAlignment;
            optional.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
            optional.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
            optional.SizeOfStackReserve = 0x100000;
            optional.SizeOfStackCommit = 0x1000;
            optional.SizeOfHeapReserve = 0x100000;
            optional.SizeOfHeapCommit = 0x1000;
            optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
            optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = kUnwind;
            optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = static_cast<DWORD>(2 * sizeof(UnwindEntry));
            Put(file, 0x40, nt);
            const struct
            {
                const char* Name;
                std::uint32_t Rva;
                std::uint32_t Size;
                std::uint32_t Characteristics;
            } sections[] = {
                { ".text", kText, 0x100, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ },
                { ".rdata", kData, 0x200, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ },
                { ".pdata", kUnwind, static_cast<std::uint32_t>(2 * sizeof(UnwindEntry)), IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ },
            };
            for (std::size_t i = 0; i < std::size(sections); ++i)
            {
                IMAGE_SECTION_HEADER header{};
                std::memcpy(header.Name, sections[i].Name, std::strlen(sections[i].Name));
                header.Misc.VirtualSize = sections[i].Size;
                header.VirtualAddress = sections[i].Rva;
                header.SizeOfRawData = kFileAlignment;
                header.PointerToRawData = kFileAlignment * static_cast<DWORD>(i + 1);
                header.Characteristics = sections[i].Characteristics;
                Put(file, 0x40 + sizeof(IMAGE_NT_HEADERS64) + i * sizeof(IMAGE_SECTION_HEADER), header);
            }
            std::uint8_t* text = file.data() + kFileAlignment;
            std::memset(text, 0xCC, 0x100);
            std::memcpy(text + kFrameAt, kFrame, sizeof(kFrame));
            std::memcpy(text + kLeafAt, kLeaf, sizeof(kLeaf));
            std::memcpy(text + kSecondAt, kFrame, sizeof(kFrame));
            std::memcpy(text + kHookFunctionAt, kHooked, sizeof(kHooked));
            std::memcpy(text + kPlayerAt, kPlayer, sizeof(kPlayer));
            const std::uint8_t unwindInfo[] = { 0x01, 0x04, 0x01, 0x00, 0x04, 0x42, 0x00, 0x00 };
            std::memcpy(file.data() + kFileAlignment * 2, unwindInfo, sizeof(unwindInfo));
            std::memcpy(file.data() + kFileAlignment * 2 + 0x100, kFrame, sizeof(kFrame));
            const UnwindEntry functions[] = {
                { kText + kFrameAt, kText + kFrameAt + static_cast<std::uint32_t>(sizeof(kFrame)), kData },
                { kText + kSecondAt, kText + kSecondAt + static_cast<std::uint32_t>(sizeof(kFrame)), kData },
            };
            std::memcpy(file.data() + kFileAlignment * 3, functions, sizeof(functions));
            return file;
        }

        class RelayImage
        {
        public:
            RelayImage()
            {
                void* probe = ::VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
                if (!probe)
                    return;
                const auto imageBase = reinterpret_cast<std::uint64_t>(probe);
                ::VirtualFree(probe, 0, MEM_RELEASE);
                wchar_t temp[MAX_PATH]{};
                ::GetTempPathW(MAX_PATH, temp);
                m_path = std::filesystem::path(temp) / ("Manifold-RelayImage-" + std::to_string(::GetCurrentProcessId()) + ".dll");
                const std::vector<std::uint8_t> file = RelayImageFile(imageBase);
                {
                    std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
                    if (!out)
                        return;
                }
                m_module = ::LoadLibraryExW(m_path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
                if (!m_module)
                {
                    std::printf("  the relay image did not load (error %lu)\n", ::GetLastError());
                    return;
                }
                Base = reinterpret_cast<std::uint64_t>(m_module);
                DWORD old = 0;
                if (!::VirtualProtect(m_module, 0x1000, PAGE_EXECUTE_READWRITE, &old))
                {
                    Base = 0;
                    return;
                }
                Targets[0] = Base + kText + kFrameAt;
                Targets[1] = Base + kText + kLeafAt;
                Targets[2] = kFarTarget;
                auto* header = reinterpret_cast<std::uint8_t*>(m_module);
                const std::uint8_t jump[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
                for (std::size_t slot = 0; slot < 3; ++slot)
                {
                    std::uint8_t* at = header + kSlots + slot * 0x10;
                    std::memcpy(at, jump, sizeof(jump));
                    std::memcpy(at + sizeof(jump), &Targets[slot], sizeof(std::uint64_t));
                }
                const std::uint64_t second = Base + kText + kSecondAt;
                const std::uint8_t farCall[] = { 0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08 };
                std::memcpy(header + kFarCall, farCall, sizeof(farCall));
                std::memcpy(header + kFarCall + sizeof(farCall), &second, sizeof(second));
                const std::uint8_t nearCall[] = { 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00 };
                std::memcpy(header + kNearCall, nearCall, sizeof(nearCall));
                std::memcpy(header + kNearCall + sizeof(nearCall), &Targets[0], sizeof(std::uint64_t));
                const std::uint8_t junk[] = { 0x48, 0xB8 };
                std::memcpy(header + kJunk, junk, sizeof(junk));
                std::memcpy(header + kJunk + sizeof(junk), kLeaf, sizeof(kLeaf));
            }

            ~RelayImage()
            {
                if (m_module)
                    ::FreeLibrary(m_module);
                std::error_code ignored;
                if (!m_path.empty())
                    std::filesystem::remove(m_path, ignored);
            }

            RelayImage(const RelayImage&) = delete;
            RelayImage& operator=(const RelayImage&) = delete;

            std::uint64_t Base = 0;
            std::uint64_t Targets[3]{};

        private:
            std::filesystem::path m_path;
            HMODULE m_module = nullptr;
        };

        const Disasm::Instruction* RowAt(const std::vector<Disasm::Instruction>& rows, std::uint64_t address)
        {
            const auto found = std::find_if(rows.begin(), rows.end(), [&](const Disasm::Instruction& row)
                {
                    return row.Address == address;
                });
            return found == rows.end() ? nullptr : &*found;
        }

        bool Contiguous(const std::vector<Disasm::Instruction>& rows)
        {
            for (std::size_t i = 1; i < rows.size(); ++i)
            {
                if (rows[i - 1].Next() != rows[i].Address)
                    return false;
            }
            return true;
        }

        void CheckRelay(const std::vector<Disasm::Instruction>& rows, std::uint64_t slot, std::uint64_t target, const std::string& operands, const std::string& label)
        {
            const Disasm::Instruction* relay = RowAt(rows, slot);
            CHECK_MSG(relay != nullptr, label + ": no row at the relay");
            if (!relay)
                return;
            CHECK_MSG(relay->Valid && relay->Mnemonic == "jmp" && relay->Kind == Disasm::Flow::Jump, label + ": " + relay->Text());
            CHECK_MSG(relay->Length == 14 && relay->InlineBytes == 8, label + ": length " + std::to_string(relay->Length));
            CHECK_MSG(relay->Target && *relay->Target == target, label + ": target");
            CHECK_MSG(relay->Operands == operands, label + ": operands " + relay->Operands);
            CHECK_MSG(!RowAt(rows, slot + 1) && !RowAt(rows, slot + 6), label + ": a row starts inside the relay");
            const Disasm::Instruction* gap = RowAt(rows, slot + 14);
            CHECK_MSG(gap && gap->Valid && gap->Length == 2 && gap->Mnemonic == "add", label + ": no zero pair after the relay");
        }

        void Listing(Engine& engine, const RelayImage& image)
        {
            const Target::Process& process = *engine.Process();
            const Disasm::Disassembler& disassembler = engine.Disassembler();
            const Target::CodeMap* map = engine.CodeMap();
            const std::uint64_t base = image.Base;
            for (const Disasm::CodeHints* hints : { static_cast<const Disasm::CodeHints*>(map), static_cast<const Disasm::CodeHints*>(nullptr) })
            {
                const std::string label = hints ? "with hints" : "without hints";
                const std::vector<Disasm::Instruction> rows = disassembler.ReadMany(process, base + 0x4F5, 24, nullptr, hints);
                CHECK_MSG(Contiguous(rows), label);
                for (const std::uint64_t pair : { 0x4F5ull, 0x4F7ull, 0x4F9ull, 0x4FBull, 0x4FDull })
                {
                    const Disasm::Instruction* zero = RowAt(rows, base + pair);
                    CHECK_MSG(zero && zero->Length == 2, label + ": zero pair at +" + Hex(pair));
                }
                const Disasm::Instruction* lone = RowAt(rows, base + 0x4FF);
                CHECK_MSG(lone && lone->Length == 1 && !lone->Valid && lone->Mnemonic == "db", label + ": the last zero before the relay");
                for (std::size_t slot = 0; slot < 3; ++slot)
                    CheckRelay(rows, base + kSlots + slot * 0x10, image.Targets[slot], Hex(image.Targets[slot]), label + " slot " + std::to_string(slot));
            }

            std::vector<std::uint8_t> buffer(0x100);
            std::memcpy(buffer.data(), reinterpret_cast<const void*>(base + 0x4F0), buffer.size());
            const std::vector<bool> readable(buffer.size(), true);
            const Target::Symbols& symbols = engine.Symbols();
            const std::vector<Disasm::Instruction> named = disassembler.DecodeMany(buffer, readable, base + 0x4F0, 20, &symbols, map);
            for (std::size_t slot = 0; slot < 3; ++slot)
                CheckRelay(named, base + kSlots + slot * 0x10, image.Targets[slot], symbols.Describe(image.Targets[slot]), "symbols slot " + std::to_string(slot));

            const Disasm::Instruction single = disassembler.Read(process, base + kSlots);
            CHECK(single.Length == 14 && single.Target && *single.Target == image.Targets[0] && single.Text() == "jmp " + Hex(image.Targets[0]));
            const Disasm::Instruction longCall = disassembler.Read(process, base + kFarCall);
            CHECK_MSG(longCall.Mnemonic == "call" && longCall.Kind == Disasm::Flow::Call && longCall.Length == 16 && longCall.InlineBytes == 10, longCall.Text());
            CHECK(longCall.Target && *longCall.Target == base + kText + kSecondAt);
            const Disasm::Instruction shortCall = disassembler.Read(process, base + kNearCall);
            CHECK_MSG(shortCall.Mnemonic == "call" && shortCall.Length == 14 && shortCall.InlineBytes == 8, shortCall.Text());
            CHECK(shortCall.Target && *shortCall.Target == image.Targets[0]);
            const std::vector<Disasm::Instruction> calls = disassembler.ReadMany(process, base + kFarCall, 4, nullptr, map);
            CHECK(calls.size() == 4 && calls[1].Address == base + kFarCall + 0x10 && Contiguous(calls));

            Disasm::Disassembler legacy(false);
            const Disasm::Instruction narrow = legacy.Decode(base + kSlots, reinterpret_cast<const std::uint8_t*>(base + kSlots), 16, nullptr);
            CHECK_MSG(narrow.Mnemonic == "jmp" && narrow.Length == 6 && narrow.InlineBytes == 0, narrow.Text());

            for (const Disasm::CodeHints* hints : { static_cast<const Disasm::CodeHints*>(map), static_cast<const Disasm::CodeHints*>(nullptr) })
            {
                const std::vector<Disasm::Instruction> rows = disassembler.ReadMany(process, base + 0x590, 12, nullptr, hints);
                const Disasm::Instruction* swallowed = RowAt(rows, base + kJunk);
                CHECK_MSG(swallowed && swallowed->Length == 10 && !RowAt(rows, base + kJunk + 2), std::string("a symbol registered in a module is no sync point ") + (hints ? "with hints" : "without hints"));
            }

            const std::vector<Disasm::Instruction> function = disassembler.ReadMany(process, base + kText, 3, nullptr, map);
            CHECK_MSG(!function.empty() && function[0].Length == 4 && function[0].Mnemonic == "sub", "a symbol inside an unwind function does not cut its instructions");
        }

        void Scrolling(Engine& engine, const RelayImage& image)
        {
            const Target::Process& process = *engine.Process();
            const Disasm::Disassembler& disassembler = engine.Disassembler();
            const Target::CodeMap* map = engine.CodeMap();
            const std::uint64_t base = image.Base;
            const auto up = [&](std::uint64_t from, int lines)
            {
                return disassembler.BackUp(process, base + from, lines, map) - base;
            };
            CHECK(up(0x530, 1) == 0x52E);
            CHECK(up(0x530, 2) == 0x520);
            CHECK(up(0x530, 3) == 0x51E);
            CHECK(up(0x530, 4) == 0x510);
            CHECK(up(0x530, 6) == 0x500);
            CHECK(up(0x520, 2) == 0x510);
            CHECK(up(0x50E, 1) == 0x500);
            CHECK(up(0x5A0, 1) == kJunk);
            CHECK(disassembler.BackUp(process, base + 0x510, 1, nullptr) == base + 0x50E);

            const std::vector<Disasm::Instruction> rows = disassembler.ReadMany(process, base, 1200, nullptr, map);
            CHECK(Contiguous(rows));
            CHECK(RowAt(rows, base + kSlots) && RowAt(rows, base + kSlots)->Length == 14);
            int checked = 0;
            int wrong = 0;
            std::string first;
            for (std::size_t i = 1; i < rows.size(); ++i)
            {
                if (rows[i].Address < base + 0x4F0 || rows[i].Address > base + 0x5B0)
                    continue;
                ++checked;
                const std::uint64_t above = disassembler.BackUp(process, rows[i].Address, 1, map);
                if (above != rows[i - 1].Address)
                {
                    if (!wrong)
                        first = "+" + Hex(rows[i].Address - base) + " backs up to +" + Hex(above - base) + ", the listing has +" + Hex(rows[i - 1].Address - base);
                    ++wrong;
                }
            }
            CHECK(checked > 40);
            CHECK_MSG(wrong == 0, first);
        }

        void Functions(Engine& engine, const RelayImage& image)
        {
            const Target::CodeMap* map = engine.CodeMap();
            const std::uint64_t base = image.Base;
            for (const std::uint64_t offset : { 0x0ull, 0x100ull, 0x4F5ull, 0x500ull, 0x506ull, 0x50Eull, 0x510ull, 0x520ull, 0x540ull, 0x560ull, 0x5A0ull, 0xFF0ull,
                     static_cast<std::uint64_t>(kData) + 0x100, static_cast<std::uint64_t>(kData) + 0x104 })
                CHECK_MSG(!engine.FunctionAround(base + offset).has_value(), "a function at +" + Hex(offset));

            const auto header = map ? map->SectionAt(base + kSlots) : std::nullopt;
            CHECK(header && !header->Code && header->Begin == base && header->End == base + kText);
            const auto text = map ? map->SectionAt(base + kText + 0x20) : std::nullopt;
            CHECK(text && text->Code && text->Begin == base + kText && text->End == base + kData);
            const auto data = map ? map->SectionAt(base + kData + 0x100) : std::nullopt;
            CHECK(data && !data->Code);

            const auto frame = engine.FunctionAround(base + kText + kFrameAt + 4);
            CHECK(frame && frame->Source == Disasm::FunctionSource::Unwind && frame->Start == base + kText + kFrameAt && frame->End == base + kText + kFrameAt + sizeof(kFrame));
            const auto leaf = engine.FunctionAround(base + kText + kLeafAt + 5);
            CHECK_MSG(leaf.has_value(), "the leaf function after the first one");
            if (leaf)
            {
                CHECK(leaf->Source != Disasm::FunctionSource::Unwind);
                CHECK(leaf->Start == base + kText + kLeafAt);
                CHECK(leaf->End == base + kText + kLeafAt + sizeof(kLeaf));
            }

            const std::uint64_t hookFunction = base + kText + kHookFunctionAt;
            const auto hooked = engine.FunctionAround(base + kText + kHookAt + 5);
            CHECK_MSG(hooked && hooked->Start == hookFunction, "the function around an injection point starts at +" + (hooked ? Hex(hooked->Start - base) : std::string("?")));
            const auto onHook = engine.FunctionAround(base + kText + kHookAt);
            CHECK(onHook && onHook->Start == hookFunction && onHook->End == hookFunction + sizeof(kHooked));
            Target::Symbols& symbols = engine.Symbols();
            CHECK(symbols.LabelOnly(base + kText + kHookAt));
            CHECK(symbols.LabelOnly(base + kText + kFrameAt + 1));
            CHECK(symbols.LabelOnly(base + kData + 0x100));
            CHECK(symbols.LabelOnly(base + kSlots) && symbols.LabelOnly(base + kSlots + 6));
            CHECK(!symbols.LabelOnly(base + kText + kSecondAt));
            CHECK(!engine.FunctionStartAt(base + kText + kHookAt));
            CHECK(!engine.FunctionStartAt(base + kText + kLeafAt));
            CHECK(!engine.FunctionStartAt(base + kSlots));
            CHECK(!engine.FunctionStartAt(base + kData + 0x100));
            CHECK(engine.FunctionStartAt(base + kText + kFrameAt));
            CHECK(engine.FunctionStartAt(base + kText + kSecondAt));
            const Disasm::FlowGraph graph = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), hookFunction, &symbols, map, [&](std::uint64_t target, Disasm::EdgeKind)
                {
                    return engine.FunctionStartAt(target);
                });
            CHECK_MSG(graph.Blocks.size() == 3 && graph.BlockStarting(base + kText + kHookAt) >= 0, "the jump to the injection point stays inside the graph");
            CHECK(std::all_of(graph.Blocks.begin(), graph.Blocks.end(), [](const Disasm::FlowBlock& block)
                {
                    return block.Exits.empty();
                }));
        }

        bool HasJump(const Disasm::FlowGraph& graph, int from, int to)
        {
            return std::any_of(graph.Edges.begin(), graph.Edges.end(), [&](const Disasm::FlowEdge& edge)
                {
                    return edge.From == from && edge.To == to && edge.Kind == Disasm::EdgeKind::Jump;
                });
        }

        void Graphs(Engine& engine, const RelayImage& image)
        {
            const Target::Process& process = *engine.Process();
            const Disasm::Disassembler& disassembler = engine.Disassembler();
            const Target::CodeMap* map = engine.CodeMap();
            const std::uint64_t base = image.Base;
            const auto functionStart = [&](std::uint64_t target, Disasm::EdgeKind)
            {
                const auto function = map ? map->FunctionAt(target) : std::nullopt;
                return function && function->first == target;
            };
            const Disasm::FlowGraph relay = Disasm::BuildFlowGraph(disassembler, process, base + kSlots, &engine.Symbols(), map, functionStart);
            CHECK(relay.Blocks.size() == 1);
            if (!relay.Blocks.empty())
            {
                CHECK(relay.Blocks[0].Lines.size() == 1);
                CHECK(!relay.Blocks[0].Indirect && !relay.Blocks[0].Truncated);
                CHECK(relay.Blocks[0].Exits == std::vector<std::uint64_t>{ image.Targets[0] });
            }

            const Disasm::FlowGraph followed = Disasm::BuildFlowGraph(disassembler, process, base + kSlots, nullptr, map, {});
            CHECK(followed.Blocks.size() == 2);
            CHECK(HasJump(followed, 0, 1));
            CHECK(followed.Blocks.size() == 2 && followed.Blocks[1].Start == image.Targets[0] && followed.Blocks[1].Returns);

            const Disasm::FlowGraph longCall = Disasm::BuildFlowGraph(disassembler, process, base + kSlots + 0x20, nullptr, map, {});
            CHECK(longCall.Blocks.size() == 1 && longCall.Blocks[0].Exits == std::vector<std::uint64_t>{ kFarTarget });

            const Disasm::FlowGraph call = Disasm::BuildFlowGraph(disassembler, process, base + kFarCall, nullptr, map, functionStart);
            CHECK(!call.Blocks.empty() && !call.Blocks[0].Calls.empty() && call.Blocks[0].Calls.front() == base + kText + kSecondAt);
            CHECK(!call.Blocks.empty() && call.Blocks[0].Lines.size() >= 2 && call.Blocks[0].Lines[1].Address == base + kFarCall + 0x10);
        }
    }

    namespace
    {
        bool OpenRelays(Engine& engine, const RelayImage& image)
        {
            CHECK_MSG(image.Base != 0, "the relay image could not be built");
            if (!image.Base)
                return false;
            engine.Lua().Run("__manifoldHeadless = true", "headless");
            const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return false;
            CHECK(engine.Process()->ModuleAt(image.Base).has_value());
            Target::Symbols& symbols = engine.Symbols();
            for (std::size_t slot = 0; slot < 3; ++slot)
            {
                const std::uint64_t at = image.Base + kSlots + slot * 0x10;
                const std::string name = kSlotNames[slot];
                symbols.Register(name + "_Block", at, kOwner);
                symbols.Register(name + "_Relay", at, kOwner);
                symbols.Register(name + "_Destination", at + 6, kOwner);
            }
            symbols.Register("Junk_Code", image.Base + kJunk + 2, kOwner);
            symbols.Register("Inside_Frame", image.Base + kText + kFrameAt + 1, kOwner);
            symbols.Register("Leaf_Start", image.Base + kText + kLeafAt, kOwner);
            symbols.Register("Hook_Point", image.Base + kText + kHookAt, kOwner);
            symbols.Register("Data_Ptr", image.Base + kData + 0x100, kOwner);
            symbols.Register("Zero_Data", image.Base + kZeroData, kOwner);
            return true;
        }

        std::vector<std::string> LabelColumn(Engine& engine, const std::vector<Disasm::Instruction>& rows)
        {
            std::vector<std::string> labels;
            std::uint64_t start = 0;
            std::uint64_t end = 0;
            for (const Disasm::Instruction& row : rows)
            {
                if (engine.Symbols().ExactName(row.Address) && engine.FunctionStartAt(row.Address))
                {
                    start = row.Address;
                    end = std::max(engine.SymbolEnd(row.Address), row.Next());
                }
                if (start && row.Address >= start && row.Address < end)
                    labels.push_back("+" + Hex(row.Address - start));
                else
                    labels.push_back(ReplaceAll(engine.Symbols().ModuleOffset(row.Address), "\"", ""));
            }
            return labels;
        }

        void Labels(Engine& engine, const RelayImage& image)
        {
            const std::uint64_t base = image.Base;
            const auto end = [&](std::uint64_t offset)
            {
                return engine.SymbolEnd(base + offset) - base;
            };
            for (std::size_t slot = 0; slot < 3; ++slot)
            {
                const std::uint64_t at = kSlots + slot * 0x10;
                CHECK_MSG(end(at) == at + 14, "the relay at +" + Hex(at) + " ends at +" + Hex(end(at)));
                CHECK_MSG(end(at + 6) == at + 6, "the destination at +" + Hex(at + 6) + " ends at +" + Hex(end(at + 6)));
            }
            CHECK_MSG(end(0) > 0 && end(0) < 0x40, "the module base ends at +" + Hex(end(0)));
            CHECK_MSG(end(kZeroData) == kZeroData + 2, "zero data ends at +" + Hex(end(kZeroData)));
            CHECK_MSG(end(kJunk + 2) == kJunk + 2 + 5, "the header code ends at +" + Hex(end(kJunk + 2)));
            CHECK_MSG(end(kText + kFrameAt + 1) == kText + kFrameAt + 4, "a symbol inside a function ends at +" + Hex(end(kText + kFrameAt + 1)));
            CHECK_MSG(end(kText + kLeafAt) == kText + kLeafAt + 5, "a symbol at a leaf start ends at +" + Hex(end(kText + kLeafAt)));
            CHECK_MSG(end(kText + kHookAt) == kText + kHookAt + 5, "an injection point ends at +" + Hex(end(kText + kHookAt)));
            CHECK_MSG(end(kData + 0x100) == kData + 0x104, "a data symbol ends at +" + Hex(end(kData + 0x100)));
            CHECK(end(kText + kSecondAt) == kText + kSecondAt + sizeof(kFrame));

            const Target::Process& process = *engine.Process();
            const std::vector<Disasm::Instruction> rows = engine.Disassembler().ReadMany(process, base + 0x4F0, 40, nullptr, engine.CodeMap());
            const std::vector<std::string> labels = LabelColumn(engine, rows);
            const std::string module = process.ModuleAt(base)->Name + "+";
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const std::uint64_t offset = rows[i].Address - base;
                const std::string expected = module + Hex(offset);
                if (labels[i] != expected)
                    CHECK_MSG(false, "the row at +" + Hex(offset) + " is labelled " + labels[i] + ", expected " + expected);
            }
            const std::vector<Disasm::Instruction> hookRows = engine.Disassembler().ReadMany(process, base + kText + kHookFunctionAt, 6, nullptr, engine.CodeMap());
            const std::vector<std::string> hookLabels = LabelColumn(engine, hookRows);
            for (std::size_t i = 0; i < hookRows.size(); ++i)
                CHECK_MSG(hookLabels[i] == module + Hex(hookRows[i].Address - base), "the row at +" + Hex(hookRows[i].Address - base) + " is labelled " + hookLabels[i]);
            for (const std::uint64_t top : { kSlots + 0xE, kSlots + 0x2E, 0x530ull, 0x5B0ull, kZeroData + 2 })
            {
                const auto symbol = engine.Symbols().Containing(base + top, 0x1000);
                CHECK_MSG(!symbol || engine.SymbolEnd(symbol->first) <= base + top, "the top row +" + Hex(top) + " stays labelled by " + (symbol ? symbol->second : std::string()));
            }

            auto* cave = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            CHECK(cave != nullptr);
            if (!cave)
                return;
            const std::uint8_t code[] = { 0x85, 0xC9, 0x75, 0x07, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x05, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
            std::memcpy(cave, code, sizeof(code));
            std::memcpy(cave + sizeof(code), &kFarTarget, sizeof(kFarTarget));
            const char text[] = "Hello";
            std::memcpy(cave + 0x1E, text, sizeof(text));
            const auto at = reinterpret_cast<std::uint64_t>(cave);
            const std::uint8_t junk[] = { 0x48, 0xB8 };
            std::memcpy(cave + 0x5E, junk, sizeof(junk));
            std::memcpy(cave + 0x60, kLeaf, sizeof(kLeaf));
            engine.Symbols().Register("Cave_Code", at, kOwner);
            engine.Symbols().Register("Cave_Zero", at + 0x40, kOwner);
            engine.Symbols().Register("Cave_Sync", at + 0x60, kOwner);
            CHECK_MSG(engine.SymbolEnd(at) == at + 2, "the cave code label ends at +" + Hex(engine.SymbolEnd(at) - at));
            CHECK_MSG(engine.SymbolEnd(at + 0x40) == at + 0x42, "the cave zero data ends at +" + Hex(engine.SymbolEnd(at + 0x40) - at));
            CHECK(engine.SymbolEnd(at + 0x60) == at + 0x60 + sizeof(kLeaf) - 1);
            CHECK(engine.Symbols().LabelOnly(at) && engine.Symbols().LabelOnly(at + 0x60));
            CHECK_MSG(!engine.FunctionStartAt(at) && !engine.FunctionStartAt(at + 0x60), "a registered symbol without known bounds is a label, not a function");
            const auto around = engine.Symbols().Containing(at + 0x62, 0x100);
            CHECK(around && around->first == at + 0x60 && around->second == "Cave_Sync");
            const auto inModule = engine.Symbols().Containing(base + kText + kHookAt + 2, 0x1000);
            CHECK(!inModule || inModule->first != base + kText + kHookAt);
            const std::vector<Disasm::Instruction> synced = engine.Disassembler().ReadMany(process, at + 0x50, 12, nullptr, engine.CodeMap());
            const Disasm::Instruction* first = RowAt(synced, at + 0x5E);
            const Disasm::Instruction* second = RowAt(synced, at + 0x5F);
            const Disasm::Instruction* entry = RowAt(synced, at + 0x60);
            CHECK_MSG(first && first->Length == 1 && second && second->Length == 1, "the bytes in front of a symbol in allocated memory are cut");
            CHECK_MSG(entry && entry->Valid && entry->Length == sizeof(kLeaf) - 1 && entry->Mnemonic == "mov", "a symbol in allocated memory starts a row");
            const std::vector<std::string> caveLabels = LabelColumn(engine, synced);
            CHECK_MSG(entry && caveLabels[static_cast<std::size_t>(entry - synced.data())] == ReplaceAll(engine.Symbols().ModuleOffset(entry->Address), "\"", ""),
                "a symbol in allocated memory labels its row and starts no block");
            CHECK_MSG(entry && caveLabels[static_cast<std::size_t>(entry - synced.data()) + 1] != "+5", "the rows after a label keep their own addresses");
            engine.Symbols().Unregister("Cave_Code");
            engine.Symbols().Unregister("Cave_Zero");
            engine.Symbols().Unregister("Cave_Sync");
            ::VirtualFree(cave, 0, MEM_RELEASE);
        }

        std::string PlayerCave(bool is64Bit)
        {
            const std::string reg = is64Bit ? "rdx" : "edx";
            return "  cmp dword ptr [" + reg + "+000013C0],(int)20\n"
                                               "  je @f\n"
                                               "    mov [PlayerPtr],edx\n"
                                               "    jmp o_Player\n"
                                               "  @@:\n"
                                               "    mov [PlayerPtr+4],edx\n"
                                               "o_Player:\n"
                                               "  mov ebx,[" +
                   reg + "+00001344]\n"
                         "  jmp r_Player\n"
                         "PlayerPtr:\n"
                         " dd 0\n"
                         " dd 0\n";
        }

        void CheckPlayerCave(const Disasm::Disassembler& disassembler, const Target::Process& process, const Target::Symbols& symbols, const Asm::AaResult& result,
            const Asm::AaMemory& memory, const std::string& label)
        {
            CHECK_MSG(result.Ok(), label + ": " + result.Error.Text + " (line " + std::to_string(result.Line) + ")");
            const auto cave = std::find_if(memory.Allocations.begin(), memory.Allocations.end(), [](const Asm::AaAllocation& allocation)
                {
                    return EqualsNoCase(allocation.Name, "n_Player");
                });
            const auto pointer = result.Symbols.find("PlayerPtr");
            CHECK_MSG(cave != memory.Allocations.end() && pointer != result.Symbols.end(), label + ": no cave or no PlayerPtr");
            if (!result.Ok() || cave == memory.Allocations.end() || pointer == result.Symbols.end())
                return;
            const std::uint64_t ptr = pointer->second;
            const std::vector<Disasm::Instruction> rows = disassembler.ReadMany(process, cave->Address, 8, &symbols, nullptr);
            std::vector<std::string> mnemonics;
            for (const Disasm::Instruction& row : rows)
            {
                if (row.Address >= ptr)
                    break;
                mnemonics.push_back(row.Mnemonic);
            }
            std::string listing;
            for (const std::string& mnemonic : mnemonics)
                listing += mnemonic + " ";
            CHECK_MSG(mnemonics == (std::vector<std::string>{ "cmp", "jz", "mov", "jmp", "mov", "mov", "jmp" }), label + ": " + listing);
            if (mnemonics.size() != 7)
                return;
            CHECK_MSG(rows[1].Length == 6 && rows[1].Bytes[0] == 0x0F && rows[1].Bytes[1] == 0x84, label + ": je is " + rows[1].BytesText());
            CHECK_MSG(rows[3].Length == 5 && rows[3].Bytes[0] == 0xE9, label + ": jmp is " + rows[3].BytesText());
            CHECK_MSG(rows[1].Target && *rows[1].Target == rows[4].Address, label + ": je lands on the second store");
            CHECK_MSG(rows[3].Target && *rows[3].Target == rows[5].Address, label + ": jmp lands on o_Player");
            CHECK_MSG(rows[6].Next() == ptr, label + ": PlayerPtr follows the last jmp");
            CHECK_MSG(rows[2].Operands.find("[PlayerPtr]") != std::string::npos, label + ": " + rows[2].Text());
            CHECK_MSG(rows[4].Operands.find("[PlayerPtr+4]") != std::string::npos, label + ": " + rows[4].Text());
            const auto size = result.SymbolSizes.find("PlayerPtr");
            CHECK_MSG(size != result.SymbolSizes.end() && size->second == 8, label + ": PlayerPtr has no size of 8");
            CHECK(symbols.RegisteredSize(ptr) == 8);
            CHECK(symbols.RegisteredName(ptr + 4) == std::optional<std::string>("PlayerPtr+4"));
            CHECK(symbols.Describe(ptr + 6) == "PlayerPtr+6");
            CHECK(!symbols.RegisteredName(ptr + 8));
            CHECK(symbols.LabelOnly(ptr));
        }

        void AutoAssembled(Engine& engine, const RelayImage& image)
        {
            Target::Process& process = *engine.Process();
            const std::string module = process.ModuleAt(image.Base)->Name;
            const std::string script = "aobscanmodule(PlayerHook," + module + ",8B 9A ? ? ? ? 89 4D ? 8D 0C)\n"
                                                                              "alloc(n_Player,$1000,PlayerHook)\n"
                                                                              "label(o_Player r_Player)\n"
                                                                              "label(PlayerPtr)\n"
                                                                              "n_Player:\n" +
                                       PlayerCave(true) +
                                       "PlayerHook:\n"
                                       "  jmp n_Player\n"
                                       "  db 90\n"
                                       "r_Player:\n"
                                       "registersymbol(PlayerHook PlayerPtr)\n";
            Asm::AaHost host;
            host.Process = &process;
            host.Symbols = &engine.Symbols();
            Asm::AaMemory memory;
            Asm::AaOptions options;
            options.Owner = kOwner;
            const Asm::AaResult result = Asm::RunAutoAssembler(script, host, memory, options);
            CheckPlayerCave(engine.Disassembler(), process, engine.Symbols(), result, memory, "64-bit");
            const std::uint64_t hook = image.Base + kText + kPlayerAt;
            const auto registered = result.Symbols.find("PlayerHook");
            CHECK(registered != result.Symbols.end() && registered->second == hook);
            CHECK(!result.SymbolSizes.count("PlayerHook"));
            CHECK(engine.Symbols().LabelOnly(hook));
            CHECK_MSG(engine.SymbolEnd(hook) == hook + 5, "PlayerHook ends at +" + Hex(engine.SymbolEnd(hook) - hook));
            const Disasm::Instruction jump = engine.Disassembler().Read(process, hook, &engine.Symbols());
            const auto cave = std::find_if(memory.Allocations.begin(), memory.Allocations.end(), [](const Asm::AaAllocation& allocation)
                {
                    return EqualsNoCase(allocation.Name, "n_Player");
                });
            CHECK(jump.Mnemonic == "jmp" && jump.Length == 5 && cave != memory.Allocations.end() && jump.Target && *jump.Target == cave->Address);
            const auto pointer = result.Symbols.find("PlayerPtr");
            if (pointer != result.Symbols.end())
                CHECK_MSG(engine.SymbolEnd(pointer->second) < pointer->second + 8, "PlayerPtr is a label but spans +" + Hex(engine.SymbolEnd(pointer->second) - pointer->second));
            for (const Asm::AaAllocation& allocation : memory.Allocations)
                process.Free(allocation.Block ? allocation.Block : allocation.Address);
        }

        void AutoAssembled32()
        {
            wchar_t folder[MAX_PATH]{};
            if (!::GetSystemWow64DirectoryW(folder, MAX_PATH))
            {
                Skip("32-bit auto assembler listing: this Windows has no 32-bit system folder");
                return;
            }
            std::wstring path = std::wstring(folder) + L"\\cmd.exe";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION info{};
            if (!::CreateProcessW(path.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
            {
                Skip("32-bit auto assembler listing: no 32-bit process could be started");
                return;
            }
            {
                Result<std::shared_ptr<Target::Process>> opened = Target::Process::Open(MakeProcessId(info.dwProcessId));
                CHECK_MSG(opened.Ok(), opened.Ok() ? std::string() : opened.Error().Text);
                if (opened.Ok())
                {
                    std::shared_ptr<Target::Process> process = std::move(opened).Value();
                    CHECK(!process->Is64Bit());
                    Target::Symbols symbols;
                    symbols.Attach(process);
                    const std::string script = "alloc(n_Player,$1000)\n"
                                               "label(o_Player r_Player)\n"
                                               "label(PlayerPtr)\n"
                                               "n_Player:\n" +
                                               PlayerCave(false) +
                                               "r_Player:\n"
                                               "  ret\n"
                                               "registersymbol(PlayerPtr)\n";
                    Asm::AaHost host;
                    host.Process = process.get();
                    host.Symbols = &symbols;
                    Asm::AaMemory memory;
                    Asm::AaOptions options;
                    options.Owner = kOwner;
                    const Asm::AaResult result = Asm::RunAutoAssembler(script, host, memory, options);
                    const Disasm::Disassembler legacy(false);
                    CheckPlayerCave(legacy, *process, symbols, result, memory, "32-bit");
                    symbols.Detach();
                }
            }
            ::TerminateProcess(info.hProcess, 0);
            ::WaitForSingleObject(info.hProcess, 5000);
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
        }

        int RunRelayListing()
        {
            RelayImage image;
            Settings settings;
            Engine engine(settings);
            if (!OpenRelays(engine, image))
                return 1;
            const Target::Symbols& symbols = engine.Symbols();
            const std::uint64_t top = engine.Disassembler().BackUp(*engine.Process(), image.Base + kSlots, 4, engine.CodeMap());
            std::vector<Disasm::Instruction> rows = engine.Disassembler().ReadMany(*engine.Process(), top, 40, &symbols, engine.CodeMap());
            rows.erase(std::find_if(rows.begin(), rows.end(), [&](const Disasm::Instruction& row)
                           {
                               return row.Address >= image.Base + 0x5B0;
                           }),
                rows.end());
            const std::vector<std::string> labels = LabelColumn(engine, rows);
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                if (const auto name = symbols.ExactName(rows[i].Address))
                    std::printf("%s\n", name->c_str());
                std::printf("  %-36s %-48s %s\n", labels[i].c_str(), rows[i].BytesText().c_str(), rows[i].Text().c_str());
            }
            for (const std::uint64_t offset : { kSlots, kSlots + 0xE, kSlots + 0x10, kJunk + 2 })
                std::printf("function at +%s: %s\n", Hex(offset).c_str(), engine.FunctionAround(image.Base + offset) ? "found" : "none");
            engine.Symbols().UnregisterOwner(kOwner);
            engine.CloseProcess();
            return 0;
        }
    }

    int RunRelayTests()
    {
        const int before = Totals().Failures;
        RelayImage image;
        Settings settings;
        Engine engine(settings);
        if (!OpenRelays(engine, image))
            return Totals().Failures - before;
        Listing(engine, image);
        Scrolling(engine, image);
        Functions(engine, image);
        Graphs(engine, image);
        Labels(engine, image);
        AutoAssembled(engine, image);
        AutoAssembled32();
        engine.Symbols().UnregisterOwner(kOwner);
        engine.CloseProcess();
        return Totals().Failures - before;
    }

    MANIFOLD_SUITE("relays", "Header relays", 36, [](const SuiteContext&)
        {
            return RunRelayTests();
        });
    MANIFOLD_MODE("--relay-check", [](const std::vector<std::string>&)
        {
            return RunRelayListing();
        });
}
