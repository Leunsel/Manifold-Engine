#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Asm/AutoAssembler.h"
#include "Core/Text.h"
#include "Disasm/StaticRef.h"
#include "Lua/LuaEngine.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"
#include "Target/Symbols.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        constexpr std::uint64_t kCode64 = 0x140001000;
        constexpr std::uint64_t kCode32 = 0x401000;

        struct Fake
        {
            bool Is64 = true;
            std::map<std::uint64_t, std::uint64_t> Pointers;
            bool Mapped = true;

            bool Image(std::uint64_t at) const
            {
                return Is64 ? at >= 0x140000000 && at < 0x140100000 : at >= 0x400000 && at < 0x500000;
            }

            Result<Disasm::StaticRef> Resolve(std::vector<std::uint8_t> bytes, Disasm::StaticOptions options = {}) const
            {
                const auto mapped = [this](std::uint64_t at) { return Mapped && at >= 0x10000 && at < 0x7FFF00000000ull; };
                const auto image = [this](std::uint64_t at) { return Image(at); };
                const auto pointer = [this](std::uint64_t at) -> std::optional<std::uint64_t> {
                    const auto found = Pointers.find(at);
                    if (found == Pointers.end())
                        return std::nullopt;
                    return found->second;
                };
                return Disasm::ResolveStaticBytes(Is64, Is64 ? kCode64 : kCode32, bytes.data(), bytes.size(), options, mapped, image, pointer);
            }
        };

        Disasm::StaticOptions With(Disasm::StaticKind kind)
        {
            Disasm::StaticOptions options;
            options.Kind = kind;
            return options;
        }

        ErrorCode CodeOf(const Result<Disasm::StaticRef>& result)
        {
            return result.Ok() ? ErrorCode::None : result.Error().Code;
        }

        void Synthetic64()
        {
            Fake fake;
            const auto load = fake.Resolve({ 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00 });
            CHECK_MSG(load.Ok() && load->Address == kCode64 + 0x17 && load->Kind == Disasm::StaticKind::Memory && load->Relative && load->FieldOffset == 3 &&
                          load->Length == 7 && load->OperandSize == 8,
                load.Ok() ? Hex(load->Address) : load.Error().Text);
            CHECK_MSG(load.Ok() && load->Text.starts_with("mov rax,"), load.Ok() ? load->Text : "");

            const auto lea = fake.Resolve({ 0x48, 0x8D, 0x0D, 0xF0, 0xFF, 0xFF, 0xFF });
            CHECK(lea.Ok() && lea->Address == kCode64 + 7 - 0x10 && lea->Kind == Disasm::StaticKind::Memory);

            const auto movss = fake.Resolve({ 0xF3, 0x0F, 0x10, 0x05, 0x10, 0x00, 0x00, 0x00 });
            CHECK(movss.Ok() && movss->Address == kCode64 + 8 + 0x10 && movss->FieldOffset == 4 && movss->Length == 8);

            const std::vector<std::uint8_t> store = { 0xC7, 0x05, 0x10, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12 };
            const auto stored = fake.Resolve(store);
            CHECK(stored.Ok() && stored->Address == kCode64 + 10 + 0x10 && stored->Length == 10 && stored->FieldOffset == 2 && !stored->Notes.empty());
            Disasm::StaticOptions oldLength;
            oldLength.Length = 7;
            CHECK_EQ(CodeOf(fake.Resolve(store, oldLength)), ErrorCode::StaticMismatch);
            Disasm::StaticOptions oldOffset;
            oldOffset.FieldOffset = 3;
            CHECK_EQ(CodeOf(fake.Resolve(store, oldOffset)), ErrorCode::StaticMismatch);
            Disasm::StaticOptions immediateField;
            immediateField.FieldOffset = 6;
            const auto number = fake.Resolve(store, immediateField);
            CHECK(number.Ok() && number->Kind == Disasm::StaticKind::Immediate && number->Address == 0x12345678);

            const auto movabs = fake.Resolve({ 0x48, 0xB8, 0x00, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00 });
            CHECK(movabs.Ok() && movabs->Kind == Disasm::StaticKind::Immediate && movabs->Address == 0x140002000 && movabs->FieldOffset == 2 && movabs->FieldSize == 8);
            const std::vector<std::uint8_t> distant = { 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00 };
            CHECK_EQ(CodeOf(fake.Resolve(distant)), ErrorCode::StaticNoAddress);
            const auto taken = fake.Resolve(distant, With(Disasm::StaticKind::Immediate));
            CHECK(taken.Ok() && taken->Address == 0x1000000000ull);

            const auto call = fake.Resolve({ 0xE8, 0xFB, 0x0F, 0x00, 0x00 });
            CHECK(call.Ok() && call->Kind == Disasm::StaticKind::Branch && call->Address == 0x140002000 && call->Relative);
            CHECK_EQ(CodeOf(fake.Resolve({ 0xE8, 0xFB, 0x0F, 0x00, 0x00 }, With(Disasm::StaticKind::Memory))), ErrorCode::StaticNoAddress);

            const std::vector<std::uint8_t> hook = { 0xE9, 0x00, 0x00, 0x00, 0x10 };
            CHECK_EQ(CodeOf(fake.Resolve(hook)), ErrorCode::StaticHooked);
            const auto hookTarget = fake.Resolve(hook, With(Disasm::StaticKind::Branch));
            CHECK(hookTarget.Ok() && hookTarget->Address == kCode64 + 5 + 0x10000000);

            Fake relay = fake;
            relay.Pointers[kCode64 + 6] = 0x100000000ull;
            CHECK_EQ(CodeOf(relay.Resolve({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 })), ErrorCode::StaticHooked);

            Fake imports = fake;
            imports.Pointers[kCode64 + 0x16] = 0x7FF800001000ull;
            const auto slot = imports.Resolve({ 0xFF, 0x15, 0x10, 0x00, 0x00, 0x00 });
            CHECK(slot.Ok() && slot->Address == kCode64 + 0x16 && slot->Pointer == 0x7FF800001000ull && slot->Value == slot->Address);
            Disasm::StaticOptions deref;
            deref.Output = Disasm::StaticOutput::Pointer;
            const auto function = imports.Resolve({ 0xFF, 0x15, 0x10, 0x00, 0x00, 0x00 }, deref);
            CHECK(function.Ok() && function->Value == 0x7FF800001000ull);
            CHECK_EQ(CodeOf(fake.Resolve({ 0xFF, 0x15, 0x10, 0x00, 0x00, 0x00 }, deref)), ErrorCode::StaticPointerUnreadable);

            const auto moffs = fake.Resolve({ 0x48, 0xA1, 0x00, 0x30, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00 });
            CHECK(moffs.Ok() && moffs->Address == 0x140003000 && !moffs->Relative && moffs->FieldSize == 8);
            Disasm::StaticOptions rip;
            rip.Encoding = Disasm::StaticEncoding::Relative;
            CHECK_EQ(CodeOf(fake.Resolve({ 0x48, 0xA1, 0x00, 0x30, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00 }, rip)), ErrorCode::StaticMismatch);
            CHECK(fake.Resolve({ 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00 }, rip).Ok());

            const auto table = fake.Resolve({ 0x48, 0x8B, 0x04, 0xC5, 0x00, 0x30, 0x00, 0x40 });
            CHECK(table.Ok() && table->Indexed && table->Address == 0x40003000);

            CHECK_EQ(CodeOf(fake.Resolve({ 0x48, 0x8B, 0x40, 0x08 })), ErrorCode::StaticNoAddress);
            const auto thread = fake.Resolve({ 0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00 });
            CHECK_MSG(CodeOf(thread) == ErrorCode::StaticNoAddress && thread.Error().Text.find("thread") != std::string::npos, thread.Error().Text);
            CHECK_EQ(CodeOf(fake.Resolve({ 0xB8, 0x01, 0x00, 0x00, 0x00 })), ErrorCode::StaticNoAddress);
            CHECK_EQ(CodeOf(fake.Resolve({ 0xC3 })), ErrorCode::StaticNoAddress);
            CHECK_EQ(CodeOf(fake.Resolve({ 0xFF, 0xFF })), ErrorCode::StaticUnreadable);
            CHECK_EQ(CodeOf(fake.Resolve({})), ErrorCode::StaticUnreadable);

            Fake unmapped = fake;
            unmapped.Mapped = false;
            CHECK_EQ(CodeOf(unmapped.Resolve({ 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00 })), ErrorCode::StaticUnmapped);
        }

        void Synthetic32()
        {
            Fake fake;
            fake.Is64 = false;
            const auto moffs = fake.Resolve({ 0xA1, 0x78, 0x56, 0x44, 0x00 });
            CHECK(moffs.Ok() && moffs->Address == 0x445678 && !moffs->Relative && moffs->FieldOffset == 1 && moffs->Length == 5 && moffs->OperandSize == 4);
            const auto load = fake.Resolve({ 0x8B, 0x0D, 0x00, 0x20, 0x40, 0x00 });
            CHECK(load.Ok() && load->Address == 0x402000 && load->FieldOffset == 2);
            Disasm::StaticOptions oldOffset;
            oldOffset.FieldOffset = 1;
            CHECK_EQ(CodeOf(fake.Resolve({ 0x8B, 0x0D, 0x00, 0x20, 0x40, 0x00 }, oldOffset)), ErrorCode::StaticMismatch);
            Disasm::StaticOptions rip;
            rip.Encoding = Disasm::StaticEncoding::Relative;
            CHECK_EQ(CodeOf(fake.Resolve({ 0x8B, 0x0D, 0x00, 0x20, 0x40, 0x00 }, rip)), ErrorCode::StaticMismatch);

            const auto table = fake.Resolve({ 0x8B, 0x04, 0x85, 0x00, 0x30, 0x40, 0x00 });
            CHECK(table.Ok() && table->Indexed && table->Address == 0x403000);
            CHECK_EQ(CodeOf(fake.Resolve({ 0x8D, 0x04, 0x85, 0x10, 0x00, 0x00, 0x00 })), ErrorCode::StaticNoAddress);

            const auto push = fake.Resolve({ 0x68, 0x00, 0x20, 0x40, 0x00 });
            CHECK(push.Ok() && push->Kind == Disasm::StaticKind::Immediate && push->Address == 0x402000);
            const auto vtable = fake.Resolve({ 0xC7, 0x46, 0x08, 0x00, 0x20, 0x40, 0x00 });
            CHECK(vtable.Ok() && vtable->Kind == Disasm::StaticKind::Immediate && vtable->Address == 0x402000 && vtable->FieldOffset == 3);
            const auto offset = fake.Resolve({ 0xB8, 0x00, 0x20, 0x40, 0x00 });
            CHECK(offset.Ok() && offset->Kind == Disasm::StaticKind::Immediate);
            CHECK_EQ(CodeOf(fake.Resolve({ 0xB8, 0x10, 0x00, 0x00, 0x00 })), ErrorCode::StaticNoAddress);

            const auto call = fake.Resolve({ 0xE8, 0xFB, 0x0F, 0x00, 0x00 });
            CHECK(call.Ok() && call->Kind == Disasm::StaticKind::Branch && call->Address == 0x402000);
            const auto back = fake.Resolve({ 0xE8, 0xF6, 0xFF, 0xFF, 0xFF });
            CHECK(back.Ok() && back->Address == kCode32 + 5 - 10);
            const auto thread = fake.Resolve({ 0x64, 0xA1, 0x18, 0x00, 0x00, 0x00 });
            CHECK_EQ(CodeOf(thread), ErrorCode::StaticNoAddress);

            Fake imports = fake;
            imports.Pointers[0x402000] = 0x76001000;
            Disasm::StaticOptions deref;
            deref.Output = Disasm::StaticOutput::Pointer;
            const auto function = imports.Resolve({ 0xFF, 0x15, 0x00, 0x20, 0x40, 0x00 }, deref);
            CHECK(function.Ok() && function->Address == 0x402000 && function->Value == 0x76001000);
        }

        void Options()
        {
            const auto parse = [](std::vector<std::string> words, Disasm::StaticOptions& options) {
                std::string error;
                for (const std::string& word : words)
                {
                    if (!Disasm::ParseStaticOption(word, options, error))
                        return error;
                }
                return std::string();
            };
            Disasm::StaticOptions old;
            CHECK(parse({ "rip", "3", "7", "pointer" }, old).empty());
            CHECK(old.Encoding == Disasm::StaticEncoding::Relative && old.FieldOffset == 3u && old.Length == 7u && old.Output == Disasm::StaticOutput::Pointer);
            Disasm::StaticOptions words;
            CHECK(parse({ " Abs32 ", "'deref'", "0x0B", "Memory", "first" }, words).empty());
            CHECK(words.Encoding == Disasm::StaticEncoding::Absolute && words.FieldOffset == 11u && words.Kind == Disasm::StaticKind::Memory && words.FirstHit);
            Disasm::StaticOptions decimal;
            CHECK(parse({ "#2", "10" }, decimal).empty() && decimal.FieldOffset == 2u && decimal.Length == 10u);
            Disasm::StaticOptions bad;
            CHECK(!parse({ "rip", "absolute" }, bad).empty());
            Disasm::StaticOptions both;
            CHECK(!parse({ "pointer", "address" }, both).empty());
            Disasm::StaticOptions kinds;
            CHECK(!parse({ "memory", "branch" }, kinds).empty());
            Disasm::StaticOptions three;
            CHECK(!parse({ "1", "2", "3" }, three).empty());
            Disasm::StaticOptions unknown;
            CHECK(parse({ "sideways" }, unknown).find("unknown option") != std::string::npos);
            Disasm::StaticOptions zero;
            CHECK(!parse({ "1", "0" }, zero).empty());
            Disasm::StaticOptions distant;
            CHECK(!parse({ "15" }, distant).empty());
            Disasm::StaticOptions empty;
            CHECK(parse({ "", "auto" }, empty).empty() && empty.Kind == Disasm::StaticKind::Auto && !empty.FieldOffset);
        }

        struct Child
        {
            PROCESS_INFORMATION Info{};
            std::uint64_t World = 0;

            Child() = default;
            Child(const Child&) = delete;
            Child& operator=(const Child&) = delete;
            ~Child()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }

            bool Start(const std::filesystem::path& path)
            {
                SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
                HANDLE read = nullptr;
                HANDLE write = nullptr;
                if (!::CreatePipe(&read, &write, &security, 0))
                    return false;
                ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW startup{ sizeof(startup) };
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdOutput = write;
                startup.hStdError = write;
                std::wstring command = L"\"" + path.wstring() + L"\"";
                const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &Info);
                ::CloseHandle(write);
                if (!started)
                {
                    ::CloseHandle(read);
                    return false;
                }
                std::string text;
                char buffer[512];
                DWORD got = 0;
                while (text.find("g_world at ") == std::string::npos || text.find('\n', text.find("g_world at ")) == std::string::npos)
                {
                    if (!::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
                        break;
                    text.append(buffer, got);
                }
                ::CloseHandle(read);
                const std::size_t at = text.find("g_world at ");
                if (at == std::string::npos)
                    return false;
                std::size_t end = at + 11;
                while (end < text.size() && IsHexDigit(text[end]))
                    ++end;
                World = ParseHex(text.substr(at + 11, end - at - 11)).value_or(0);
                return World != 0;
            }
        };

        // Real compiler output: every instruction of the target's code that names g_world.
        std::size_t FindWorldReferences(const Target::Process& process, std::uint64_t world, std::vector<std::uint64_t>& sites)
        {
            const auto module = process.ModuleAt(world);
            if (!module)
                return 0;
            const bool is64 = process.Is64Bit();
            for (const Target::RegionInfo& region : process.Regions(module->Base, module->Base + module->Size))
            {
                if (!region.Executable() || !region.Readable())
                    continue;
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(region.Size));
                const std::size_t got = process.Read(region.Base, bytes.data(), bytes.size());
                for (std::size_t p = 4; p + 4 <= got; ++p)
                {
                    std::int32_t field = 0;
                    std::memcpy(&field, bytes.data() + p, 4);
                    bool candidate = false;
                    if (is64)
                    {
                        const std::uint64_t after = region.Base + p + 4 + static_cast<std::uint64_t>(static_cast<std::int64_t>(field));
                        candidate = world >= after && world - after <= 4;
                    }
                    else
                        candidate = static_cast<std::uint32_t>(field) == static_cast<std::uint32_t>(world);
                    if (!candidate)
                        continue;
                    for (std::size_t back = 1; back <= 4; ++back)
                    {
                        const std::uint64_t start = region.Base + p - back;
                        const auto ref = Disasm::ResolveStatic(process, start, {});
                        if (ref.Ok() && ref->Address == world && ref->Kind == Disasm::StaticKind::Memory && ref->FieldOffset == back)
                        {
                            sites.push_back(start);
                            break;
                        }
                    }
                }
            }
            return sites.size();
        }

        Asm::AaResult Assemble(Target::Process& process, Target::Symbols& symbols, Asm::AaMemory& memory, const std::string& script, bool check = false)
        {
            Asm::AaHost host;
            host.Process = &process;
            host.Symbols = &symbols;
            Asm::AaOptions options;
            options.Owner = "static-test";
            options.CheckOnly = check;
            options.Log = true;
            return Asm::RunAutoAssembler(script, host, memory, options);
        }

        std::string Outcome(const Asm::AaResult& result)
        {
            return result.Ok() ? std::string("ok") : result.Error.Text + " at line " + std::to_string(result.Line);
        }

        void InTarget(const std::filesystem::path& path, bool is64, const char* label)
        {
            std::error_code missing;
            if (!std::filesystem::exists(path, missing))
            {
                Skip("the test target " + path.string() + " was not built");
                return;
            }
            Child child;
            const bool started = child.Start(path);
            CHECK_MSG(started, std::string(label) + ": could not start the target");
            if (!started)
                return;
            auto opened = Target::Process::Open(MakeProcessId(child.Info.dwProcessId));
            CHECK_MSG(opened.Ok(), label);
            if (!opened.Ok())
                return;
            const std::shared_ptr<Target::Process> process = *opened;
            CHECK_MSG(process->Is64Bit() == is64, label);

            std::vector<std::uint64_t> sites;
            const std::size_t found = FindWorldReferences(*process, child.World, sites);
            CHECK_MSG(found > 0, std::string(label) + ": no instruction of the compiled code names g_world");
            const auto world = process->ReadPointer(child.World);
            for (const std::uint64_t site : sites)
            {
                const auto ref = Disasm::ResolveStatic(*process, site, {});
                CHECK_MSG(ref.Ok() && ref->Relative == is64 && ref->Pointer == world, std::string(label) + " at " + Hex(site) + ": " + (ref.Ok() ? ref->Text : ref.Error().Text));
            }
            if (!sites.empty())
                std::printf("  %s: %zu instructions name g_world, first %s\n", label, sites.size(), Disasm::ResolveStatic(*process, sites.front(), {})->Text.c_str());

            Target::Symbols symbols;
            symbols.Attach(process);
            const auto block = process->Allocate(0x1000, PAGE_EXECUTE_READWRITE);
            CHECK_MSG(block.Ok(), label);
            if (!block.Ok())
                return;
            const std::uint64_t base = *block;
            const std::uint64_t slot = base + 0x800;
            const std::uint64_t stored = is64 ? 0x1122334455667788ull : 0x11223344ull;
            CHECK(process->Write(slot, &stored, is64 ? 8 : 4).Ok());
            const auto site = [&](std::uint64_t at) {
                std::vector<std::uint8_t> code;
                if (is64)
                {
                    const std::int32_t disp = static_cast<std::int32_t>(slot - (at + 7));
                    code = { 0x48, 0x8B, 0x05, 0, 0, 0, 0 };
                    std::memcpy(code.data() + 3, &disp, 4);
                }
                else
                {
                    const std::uint32_t absolute = static_cast<std::uint32_t>(slot);
                    code = { 0x8B, 0x0D, 0, 0, 0, 0 };
                    std::memcpy(code.data() + 2, &absolute, 4);
                }
                CHECK(process->Write(at, code).Ok());
            };
            site(base + 0x100);
            site(base + 0x200);
            const std::string pattern = is64 ? "48 8B 05 ?? ?? ?? ??" : "8B 0D ?? ?? ?? ??";
            const std::string start = Hex(base);
            const std::string half = Hex(base + 0x180);
            const std::string all = Hex(base + 0x400);

            {
                Asm::AaMemory memory;
                symbols.Register("aaStaticSlot", 0x1234, "another-script");
                const std::string script = "aobscanregion(SITE," + start + "," + half + "," + pattern + ")\nresolvestatic(aaStaticSlot,SITE)\nresolvestatic(aaStaticValue,SITE,pointer)\n"
                                           "registersymbol(aaStaticSlot aaStaticValue)\n";
                const Asm::AaResult first = Assemble(*process, symbols, memory, script);
                CHECK_MSG(first.Ok(), std::string(label) + ": " + Outcome(first));
                CHECK_MSG(symbols.FindRegistered("aaStaticSlot") == slot, label);
                CHECK_MSG(symbols.FindRegistered("aaStaticValue") == stored, label);
                const Asm::AaResult again = Assemble(*process, symbols, memory, script);
                CHECK_MSG(again.Ok() && symbols.FindRegistered("aaStaticSlot") == slot, std::string(label) + " again: " + Outcome(again));
                bool logged = false;
                for (const Asm::AaLogLine& line : first.Log)
                    logged = logged || (line.Command == "resolvestatic(aaStaticSlot)" && line.Text.find(is64 ? "rip-relative" : "absolute") != std::string::npos);
                CHECK_MSG(logged, label);
                symbols.Unregister("aaStaticSlot");
                symbols.Unregister("aaStaticValue");
            }
            {
                Asm::AaMemory memory;
                const std::string script = "aobscanregion(SITE," + start + "," + all + "," + pattern + ")\nresolvestatic(aaStaticSlot,SITE+0)\nregistersymbol(aaStaticSlot)\n";
                const Asm::AaResult twice = Assemble(*process, symbols, memory, script);
                CHECK_MSG(twice.Error.Code == ErrorCode::AaStaticNotUnique && twice.Line == 2, std::string(label) + ": " + Outcome(twice));
                CHECK(!symbols.FindRegistered("aaStaticSlot"));
                const Asm::AaResult first = Assemble(*process, symbols, memory, ReplaceAll(script, "SITE+0)", "SITE+0,first)"));
                CHECK_MSG(first.Ok() && symbols.FindRegistered("aaStaticSlot") == slot, std::string(label) + " first: " + Outcome(first));
                symbols.Unregister("aaStaticSlot");
                const Asm::AaResult check = Assemble(*process, symbols, memory, script, true);
                CHECK_MSG(check.Ok(), std::string(label) + " check: " + Outcome(check));
            }
            {
                // a check reads nothing: an address that is no code passes, the real run names the fault
                Asm::AaMemory memory;
                const std::string script = "resolvestatic(aaNothing," + Hex(base + 0x300) + ")\nresolvestatic(aaGone,10)\nalloc(cave,$10)\ncave:\nmov eax,[aaNothing]\n";
                const Asm::AaResult check = Assemble(*process, symbols, memory, script, true);
                CHECK_MSG(check.Ok(), std::string(label) + " check: " + Outcome(check));
                const Asm::AaResult run = Assemble(*process, symbols, memory, script);
                CHECK_MSG(run.Error.Code == ErrorCode::StaticUnreadable || run.Error.Code == ErrorCode::StaticNoAddress, std::string(label) + ": " + Outcome(run));
                CHECK_MSG(run.Line == 1, label);
                for (const Asm::AaAllocation& allocation : memory.Allocations)
                    process->Free(allocation.Block);
            }
            {
                Asm::AaMemory memory;
                CHECK_EQ(Assemble(*process, symbols, memory, "label(x1)\nresolvestatic(x1," + Hex(base + 0x100) + ")\n").Error.Code, ErrorCode::AaLabelTwice);
                CHECK_EQ(Assemble(*process, symbols, memory, "resolvestatic(BEEF," + Hex(base + 0x100) + ")\n").Error.Code, ErrorCode::AaBadArguments);
                CHECK_EQ(Assemble(*process, symbols, memory, "resolvestatic(9lives," + Hex(base + 0x100) + ")\n").Error.Code, ErrorCode::AaBadArguments);
                CHECK_EQ(Assemble(*process, symbols, memory, "resolvestatic(only)\n").Error.Code, ErrorCode::AaBadArguments);
                CHECK_EQ(Assemble(*process, symbols, memory, "resolvestatic(opt," + Hex(base + 0x100) + ",sideways)\n", true).Error.Code, ErrorCode::AaBadArguments);
                const Asm::AaResult wrongLength = Assemble(*process, symbols, memory, "resolvestatic(opt," + Hex(base + 0x100) + "," + (is64 ? "3,8" : "2,5") + ")\n");
                CHECK_MSG(wrongLength.Error.Code == ErrorCode::StaticMismatch, std::string(label) + ": " + Outcome(wrongLength));
                const Asm::AaResult rightLength = Assemble(*process, symbols, memory, "resolvestatic(opt," + Hex(base + 0x100) + "," + (is64 ? "rip,3,7" : "abs,2,6") + ")\n");
                CHECK_MSG(rightLength.Ok(), std::string(label) + ": " + Outcome(rightLength));
            }
            process->Free(base);
            DWORD code = 0;
            ::GetExitCodeProcess(child.Info.hProcess, &code);
            CHECK_MSG(code == STILL_ACTIVE, std::string(label) + ": the target ended with " + Hex(code, 8));
        }

        void LuaApi()
        {
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            const Status none = lua.Run("local r, e = manifold.memory.resolveStatic(0x10000)\nassert(r == nil and e.code == 'ME-1005', tostring(e and e.code))\n", "static api, no process");
            CHECK_MSG(none.Ok(), none.Text);
            CHECK(engine.OpenProcess(MakeProcessId(::GetCurrentProcessId())).Ok());
            std::uint8_t* block = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            CHECK(block != nullptr);
            if (!block)
                return;
            const std::uint64_t at = reinterpret_cast<std::uint64_t>(block) + 0x100;
            const std::uint64_t slot = reinterpret_cast<std::uint64_t>(block) + 0x800;
            const std::int32_t disp = static_cast<std::int32_t>(slot - (at + 7));
            const std::uint8_t code[] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3 };
            std::memcpy(block + 0x100, code, sizeof(code));
            std::memcpy(block + 0x103, &disp, 4);
            const std::uint64_t value = 0x0102030405060708ull;
            std::memcpy(block + 0x800, &value, 8);
            const std::string where = std::to_string(at);
            const std::string script =
                "local r = assert(manifold.memory.resolveStatic(" + where + "))\n"
                "assert(r.address == " + std::to_string(slot) + " and r.pointer == " + std::to_string(value) + ", r.address)\n"
                "assert(r.kind == 'memory' and r.encoding == 'relative' and r.fieldOffset == 3 and r.fieldSize == 4 and r.length == 7 and r.operandSize == 8)\n"
                "assert(r.instruction == " + where + " and r.text:sub(1, 8) == 'mov rax,' and type(r.notes) == 'table' and r.indexed == false)\n"
                "assert(manifold.memory.resolveStatic(" + where + ", { kind = 'memory', encoding = 'relative', offset = 3, length = 7, pointer = true }))\n"
                "local bad, problem = manifold.memory.resolveStatic(" + where + ", { length = 5 })\n"
                "assert(bad == nil and problem.code == 'ME-2022', tostring(problem and problem.code))\n"
                "local ret, why = manifold.memory.resolveStatic(" + std::to_string(at + 7) + ")\n"
                "assert(ret == nil and why.code == 'ME-2020', tostring(why and why.code))\n"
                "assert(not pcall(manifold.memory.resolveStatic, " + where + ", { kind = 'sideways' }))\n"
                "assert(not pcall(manifold.memory.resolveStatic, " + where + ", { offset = 20 }))\n"
                "assert(manifold.has('memory.resolveStatic') and manifold.apiVersion >= 2)\n";
            const Status status = lua.Run(script, "static api");
            CHECK_MSG(status.Ok(), status.Text);
            engine.CloseProcess();
            ::VirtualFree(block, 0, MEM_RELEASE);
        }
    }

    int RunStaticRefTests(const std::filesystem::path& target)
    {
        const int before = Totals().Failures;
        Synthetic64();
        Synthetic32();
        Options();
        LuaApi();
        InTarget(target, true, "64-bit target");
        InTarget(target.parent_path() / L"TestTarget32.exe", false, "32-bit target");
        return Totals().Failures - before;
    }

    MANIFOLD_SUITE("statics", "Static addresses from code", 33, [](const SuiteContext& context) { return RunStaticRefTests(context.Target); });
}
