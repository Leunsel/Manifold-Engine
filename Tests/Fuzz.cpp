#include "TestKit.h"

#include "App/Themes.h"
#include "Asm/Assembler.h"
#include "Asm/AutoAssembler.h"
#include "Core/Deflate.h"
#include "Core/ImageDecode.h"
#include "Core/Ini.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Core/Xml.h"
#include "Disasm/Signature.h"
#include "Lua/AutorunModules.h"
#include "Scan/PointerScan.h"
#include "Table/CeForms.h"
#include "Table/CheatTable.h"
#include "Table/StructExchange.h"
#include "Target/ClrMetadata.h"
#include "Target/ManagedDumper.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

// ∑ Fuzzing the parsers that read what other people made: tables, table files,
//   images, forms, structure sources, themes, scripts, signatures, the managed
//   dump and .NET metadata read from a game, autorun headers and pointer scans.
//   `--fuzz <target> [libFuzzer options]` runs libFuzzer in a build made with
//   -fsanitize=fuzzer-no-link,address and MANIFOLD_FUZZ (Docs/DEVELOPMENT.md);
//   any other build replays files instead: `--fuzz <target> <file or folder>...`.
//   The suite "fuzz" replays Tests/Fuzz/<target>/*, the inputs that once broke a
//   parser, through every target, so a fixed one stays fixed.
namespace Manifold::Tests
{
    namespace
    {
        constexpr std::size_t kMaxInput = 1u << 20;
        constexpr std::size_t kLimit = 16u << 20;

        std::string_view Text(std::span<const std::uint8_t> data)
        {
            return { reinterpret_cast<const char*>(data.data()), data.size() };
        }

        void Table(std::span<const std::uint8_t> data)
        {
            Manifold::Table::LoadedTable table;
            Manifold::Table::ParseTable(Text(data), table);
        }

        void XmlText(std::span<const std::uint8_t> data)
        {
            const Xml::ParseResult parsed = Xml::Parse(Text(data));
            if (parsed.Root)
                Xml::Write(*parsed.Root);
        }

        void Inflate(std::span<const std::uint8_t> data)
        {
            Deflate::Decompress(data, kLimit);
        }

        void TableFile(std::span<const std::uint8_t> data)
        {
            Manifold::Table::DecodeTableFile(Text(data), kLimit);
        }

        void Image(std::span<const std::uint8_t> data)
        {
            DecodeImage(data, 4u << 20);
        }

        void CeForm(std::span<const std::uint8_t> data)
        {
            const Xml::ParseResult parsed = Xml::Parse(Text(data));
            if (!parsed.Root)
                return;
            std::string error;
            Manifold::Table::DecodeCeForm(*parsed.Root, error);
            for (const auto& child : parsed.Root->Children)
                Manifold::Table::DecodeCeForm(*child, error);
        }

        void Structures(std::span<const std::uint8_t> data)
        {
            for (const bool is64Bit : { true, false })
            {
                Manifold::Table::StructImportOptions options;
                options.Is64Bit = is64Bit;
                Manifold::Table::ImportStructures(Text(data), options);
            }
        }

        void ReClass(std::span<const std::uint8_t> data)
        {
            Manifold::Table::StructImportOptions options;
            Manifold::Table::ImportReClass(data, options);
        }

        void Theme(std::span<const std::uint8_t> data)
        {
            std::vector<std::string> problems;
            Themes::FromText(Text(data), "fuzz", &problems);
            const std::string_view text = Text(data);
            Themes::ParseColor(text.substr(0, std::min<std::size_t>(text.size(), 64)));
        }

        void IniText(std::span<const std::uint8_t> data)
        {
            Ini::Parse(Text(data));
        }

        void Assembler(std::span<const std::uint8_t> data)
        {
            const std::string_view text = Text(data);
            for (const bool is64Bit : { true, false })
            {
                Asm::AssembleOptions options;
                options.Is64Bit = is64Bit;
                options.Address = is64Bit ? 0x140001000ull : 0x401000ull;
                std::size_t start = 0;
                for (int lines = 0; start <= text.size() && lines < 64; ++lines)
                {
                    const std::size_t end = std::min(text.find('\n', start), text.size());
                    Asm::Assemble(text.substr(start, end - start), options);
                    start = end + 1;
                }
            }
        }

        void AutoAssembler(std::span<const std::uint8_t> data)
        {
            Asm::AaHost host;
            Asm::AaMemory memory;
            Asm::AaOptions options;
            options.CheckOnly = true;
            Asm::RunAutoAssembler(Text(data), host, memory, options);
        }

        void Signature(std::span<const std::uint8_t> data)
        {
            std::string error;
            Disasm::ParseSignature(Text(data), error);
        }

        void Managed(std::span<const std::uint8_t> data)
        {
            Target::ParseManagedDump(data);
        }

        void Clr(std::span<const std::uint8_t> data)
        {
            for (const bool pe : { false, true })
            {
                std::vector<std::uint8_t> metadata;
                if (pe)
                {
                    const auto found = Target::ClrMetadata::FromPeFile(data);
                    if (!found)
                        continue;
                    metadata = *found;
                }
                else
                {
                    metadata.assign(data.begin(), data.end());
                }
                Target::ClrMetadata reader;
                if (!reader.Parse(metadata))
                    continue;
                const std::uint32_t types = std::min<std::uint32_t>(reader.Rows(Target::ClrMetadata::TypeDef), 256);
                for (std::uint32_t row = 1; row <= types; ++row)
                {
                    reader.TypeDefName(row);
                    reader.EnclosingOf(row);
                    reader.HasGenericParams(row);
                    reader.TypeDefAt(row);
                }
                const std::uint32_t fields = std::min<std::uint32_t>(reader.Rows(Target::ClrMetadata::Field), 256);
                for (std::uint32_t row = 1; row <= fields; ++row)
                {
                    reader.FieldTypeName(row);
                    reader.FieldOwner(row);
                    reader.FieldAt(row);
                }
                const std::uint32_t methods = std::min<std::uint32_t>(reader.Rows(Target::ClrMetadata::MethodDef), 256);
                for (std::uint32_t row = 1; row <= methods; ++row)
                {
                    reader.ParamCount(row);
                    reader.MethodOwner(row);
                    reader.MethodAt(row);
                }
            }
        }

        void AutorunHeader(std::span<const std::uint8_t> data)
        {
            Autorun::ParseHeader(Text(data));
        }

        void PointerScan(std::span<const std::uint8_t> data)
        {
            static const std::filesystem::path file = Paths::DataFolder() / (L"fuzz-" + std::to_wstring(::GetCurrentProcessId()) + L".mptr");
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            }
            Scan::PointerResults results;
            if (results.Load(file, kLimit))
            {
                Scan::PointerPath path;
                const std::uint64_t count = std::min<std::uint64_t>(results.Count(), 256);
                for (std::uint64_t i = 0; i < count; ++i)
                    results.Read(i, path);
            }
        }

        struct FuzzTarget
        {
            const char* Name;
            void (*Run)(std::span<const std::uint8_t> data);
        };

        constexpr FuzzTarget kTargets[] = {
            { "table", &Table },
            { "xml", &XmlText },
            { "deflate", &Inflate },
            { "tablefile", &TableFile },
            { "image", &Image },
            { "ceform", &CeForm },
            { "structs", &Structures },
            { "reclass", &ReClass },
            { "theme", &Theme },
            { "ini", &IniText },
            { "asm", &Assembler },
            { "autoassembler", &AutoAssembler },
            { "signature", &Signature },
            { "managed", &Managed },
            { "clr", &Clr },
            { "autorun", &AutorunHeader },
            { "pointerscan", &PointerScan },
        };

        const FuzzTarget* g_target = nullptr;

        const FuzzTarget* FindTarget(std::string_view name)
        {
            for (const FuzzTarget& target : kTargets)
            {
                if (name == target.Name)
                    return &target;
            }
            return nullptr;
        }

        void RunInput(const FuzzTarget& target, std::span<const std::uint8_t> data)
        {
            if (data.size() > kMaxInput)
                data = data.first(kMaxInput);
            target.Run(data);
        }

        std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        }

        std::vector<std::filesystem::path> InputsIn(const std::filesystem::path& path)
        {
            std::vector<std::filesystem::path> inputs;
            std::error_code error;
            if (std::filesystem::is_directory(path, error))
            {
                for (std::filesystem::directory_iterator at(path, error), end; !error && at != end; at.increment(error))
                {
                    if (at->is_regular_file(error))
                        inputs.push_back(at->path());
                }
                std::sort(inputs.begin(), inputs.end());
            }
            else if (std::filesystem::is_regular_file(path, error))
            {
                inputs.push_back(path);
            }
            return inputs;
        }
    }
}

#if defined(MANIFOLD_FUZZ)
extern "C" int LLVMFuzzerRunDriver(int* argc, char*** argv, int (*callback)(const std::uint8_t* data, std::size_t size));

namespace
{
    LONG ReportException(EXCEPTION_POINTERS* info)
    {
        HMODULE module = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(info->ExceptionRecord->ExceptionAddress), &module);
        std::fprintf(stderr, "exception %08lx at %p (module %p, offset %llx)\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress, static_cast<void*>(module),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress) - reinterpret_cast<std::uintptr_t>(module)));
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    int Traced(const std::uint8_t* data, std::size_t size)
    {
        __try
        {
            Manifold::Tests::RunInput(*Manifold::Tests::g_target, { data, size });
        }
        __except (ReportException(GetExceptionInformation()))
        {
        }
        return 0;
    }

    int FuzzOne(const std::uint8_t* data, std::size_t size)
    {
        // MANIFOLD_FUZZ_TRACE=1 prints every input before it runs, for a target that ends the process without a report
        static const bool trace = ::GetEnvironmentVariableW(L"MANIFOLD_FUZZ_TRACE", nullptr, 0) != 0;
        if (trace)
        {
            std::fprintf(stderr, "input %zu bytes: ", size);
            for (std::size_t i = 0; i < size && i < 200; ++i)
                std::fputc(data[i] >= 0x20 && data[i] < 0x7F ? data[i] : '.', stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
        }
        if (trace)
            return Traced(data, size);
        Manifold::Tests::RunInput(*Manifold::Tests::g_target, { data, size });
        return 0;
    }
}
#endif

namespace Manifold::Tests
{
    namespace
    {
        int RunFuzzMode(const std::vector<std::string>& arguments)
        {
            const FuzzTarget* target = arguments.size() >= 2 ? FindTarget(arguments[1]) : nullptr;
            if (!target)
            {
                std::printf("--fuzz <target> [inputs or libFuzzer options]. Targets:");
                for (const FuzzTarget& known : kTargets)
                    std::printf(" %s", known.Name);
                std::printf("\n");
                return 2;
            }
            g_target = target;
#if defined(MANIFOLD_FUZZ)
            std::vector<std::string> kept = { arguments[0] };
            kept.insert(kept.end(), arguments.begin() + 2, arguments.end());
            std::vector<char*> pointers;
            for (std::string& argument : kept)
                pointers.push_back(argument.data());
            pointers.push_back(nullptr);
            int count = static_cast<int>(kept.size());
            char** values = pointers.data();
            return LLVMFuzzerRunDriver(&count, &values, &FuzzOne);
#else
            int ran = 0;
            for (std::size_t i = 2; i < arguments.size(); ++i)
            {
                for (const std::filesystem::path& input : InputsIn(Utf8ToWide(arguments[i])))
                {
                    std::printf("%s: %s\n", target->Name, WideToUtf8(input.wstring()).c_str());
                    std::fflush(stdout);
                    RunInput(*target, ReadFile(input));
                    ++ran;
                }
            }
            std::printf("%d inputs, no crash\n", ran);
            return 0;
#endif
        }

        // Every stored input goes through every target: an input that broke one
        // parser is a good stress for the others too.
        int RunFuzzReplay(const SuiteContext&)
        {
            const int before = Totals().Failures;
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::filesystem::path at = std::filesystem::path(self).parent_path();
            std::error_code error;
            for (int i = 0; i < 8 && !std::filesystem::is_directory(at / L"Tests" / L"Fuzz", error); ++i)
                at = at.parent_path();
            const std::filesystem::path root = at / L"Tests" / L"Fuzz";
            std::vector<std::filesystem::path> inputs;
            for (const FuzzTarget& target : kTargets)
            {
                const std::vector<std::filesystem::path> found = InputsIn(root / Utf8ToWide(target.Name));
                inputs.insert(inputs.end(), found.begin(), found.end());
            }
            if (!std::filesystem::is_directory(root, error))
            {
                Skip("the stored fuzz inputs (Tests/Fuzz) are not next to the sources");
                return 0;
            }
            for (const std::filesystem::path& input : inputs)
            {
                const std::vector<std::uint8_t> data = ReadFile(input);
                for (const FuzzTarget& target : kTargets)
                    RunInput(target, data);
            }
            CHECK_MSG(!inputs.empty(), "no stored fuzz inputs in " + WideToUtf8(root.wstring()));

            // The findings themselves, so they fail in a build without sanitizers too.
            CHECK_MSG(!DecodeImage(ReadFile(root / L"image" / L"png-huge-idat.png")), "a PNG whose IDAT claims 1 GB was decoded");
            CHECK_MSG(!DecodeImage(ReadFile(root / L"image" / L"png-huge-size.png")), "a PNG with a huge size was decoded");
            {
                const std::vector<std::uint8_t> script = ReadFile(root / L"autoassembler" / L"control-char-reassemble.txt");
                Asm::AaHost host;
                Asm::AaMemory memory;
                Asm::AaOptions options;
                options.CheckOnly = true;
                const Asm::AaResult result = Asm::RunAutoAssembler(Text(script), host, memory, options);
                CHECK_MSG(!script.empty() && !result.Ok(), "a line starting with the reassemble marker's control character passed the check");
                // the marker alone, without the space and the target after it
                Asm::RunAutoAssembler("alloc(newmem,$100)\nnewmem:\n\x01reassemble\n\x01\n", host, memory, options);
            }
            std::printf("  %zu stored inputs through %zu parsers\n", inputs.size(), std::size(kTargets));
            return Totals().Failures - before;
        }
    }

    MANIFOLD_MODE("--fuzz", &RunFuzzMode);

    MANIFOLD_SUITE("fuzz", "Stored fuzz inputs", 18, &RunFuzzReplay);
}
