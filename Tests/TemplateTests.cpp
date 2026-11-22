#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Asm/AutoAssembler.h"
#include "Disasm/Disassembler.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"
#include "Templates/BuiltinTemplates.h"
#include "Templates/TemplateCheck.h"
#include "Templates/TemplateFiles.h"
#include "Templates/TemplateEngine.h"
#include "Templates/TemplateGenerator.h"
#include "Templates/TemplateLibrary.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace Manifold::Tests
{
    int RunTemplateTests();

    namespace
    {
        using namespace Manifold::Templates;

        volatile std::uint64_t g_mainModuleProbe[5] = {};
        volatile std::uint8_t g_macroSource[32] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F };
        volatile std::uint8_t g_macroTarget[8] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8 };

        std::vector<std::uint8_t> WrittenAt(const Asm::AaResult& result, std::uint64_t address, std::size_t size)
        {
            for (const Asm::AaWrite& write : result.Writes)
            {
                if (address >= write.Address && address + size <= write.Address + write.Bytes.size())
                    return std::vector<std::uint8_t>(write.Bytes.begin() + static_cast<std::ptrdiff_t>(address - write.Address),
                        write.Bytes.begin() + static_cast<std::ptrdiff_t>(address - write.Address + size));
            }
            return {};
        }

        // Each Auto Assembler macro the owner's tables use, run for real in this process, against what
        // Cheat Engine does (autoassembler.pas line numbers in the messages).
        void AutoAssemblerMacros(Engine& engine)
        {
            const std::string source = Hex(reinterpret_cast<std::uint64_t>(&g_macroSource[0]));
            const std::string target = Hex(reinterpret_cast<std::uint64_t>(&g_macroTarget[0]));
            const auto cave = [](const Asm::AaMemory& memory, const char* name) -> std::uint64_t {
                for (const Asm::AaAllocation& allocation : memory.Allocations)
                {
                    if (EqualsNoCase(allocation.Name, name))
                        return allocation.Address;
                }
                return 0;
            };
            const auto run = [&](const std::string& script, Asm::AaMemory& memory, bool check = false) {
                return engine.AutoAssemble(script, Asm::AaSection::Whole, memory, check, "macro-test");
            };
            const auto release = [&](Asm::AaMemory& memory) {
                for (const Asm::AaAllocation& allocation : memory.Allocations)
                    engine.Process()->Free(allocation.Block);
                memory.Allocations.clear();
            };

            {
                // readmem sizes are decimal (StrToInt, 2455): 14 bytes, not 0x14
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(cave,$100)\ncave:\nreadmem(" + source + ",14)\nreadmem(" + source + ",$4)\n", memory);
                CHECK_MSG(result.Ok(), result.Error.Text);
                const std::uint64_t at = cave(memory, "cave");
                const std::vector<std::uint8_t> copied = WrittenAt(result, at, 18);
                CHECK_EQ(copied.size(), std::size_t(18));
                if (copied.size() == 18)
                {
                    for (std::size_t i = 0; i < 14; ++i)
                        CHECK_EQ(copied[i], static_cast<std::uint8_t>(i));
                    CHECK_EQ(copied[14], std::uint8_t(0));
                    CHECK_EQ(copied[17], std::uint8_t(3));
                }
                release(memory);
            }
            {
                // chained and positional defines (2004-2050); a define never changes a quoted string
                Asm::AaMemory memory;
                const Asm::AaResult result = run("define(a,10)\ndefine(b,a+4)\ndefine(abc,1234)\nalloc(cave,$100)\ncave:\nmov eax,b\ndb 'abc',0\ndefine(a,20)\nmov eax,a\n", memory);
                CHECK_MSG(result.Ok(), result.Error.Text);
                const std::uint64_t at = cave(memory, "cave");
                CHECK_EQ(HexBytes(WrittenAt(result, at, 14)), std::string("B8 14 00 00 00 61 62 63 00 B8 20 00 00 00"));
                release(memory);
            }
            {
                // @f/@b take every name: line (800-897), named labels and newmem: included; and a label
                // used by the code needs no label() (getPotentialLabels, 708-731)
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(cave,$100)\ncave:\ndec ecx\njnz @b\nje @f\ninc eax\nskip:\nret\n@@:\nnop\njmp @b\n", memory);
                CHECK_MSG(result.Ok(), result.Error.Text);
                const std::uint64_t at = cave(memory, "cave");
                CHECK_EQ(HexBytes(WrittenAt(result, at, 16)), std::string("FF C9 75 FC 0F 84 02 00 00 00 FF C0 C3 90 EB FD"));
                release(memory);
            }
            {
                // a bare name: that nothing jumps to and that does not resolve is an unknown symbol on the
                // real pass, never a label at the current address
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(cave,$100)\ncave:\nnop\nNoSuchHook:\ndb 90\n", memory);
                CHECK(!result.Ok());
                CHECK(memory.Allocations.empty());
            }
            {
                // an alloc without a near address joins the group of the one before (3359-3445)
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(newmem,$100," + target + ")\nalloc(data,8)\nalloc(more,#16)\n", memory);
                CHECK_MSG(result.Ok(), result.Error.Text);
                CHECK_EQ(cave(memory, "data"), cave(memory, "newmem") + 0x100);
                CHECK_EQ(cave(memory, "more"), cave(memory, "newmem") + 0x110);
                release(memory);
            }
            {
                // dealloc only frees what an earlier run allocated (2718-2758): the code written now
                // stays in place and readable
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(t,$10)\nt:\nret\ndealloc(t)\n", memory);
                CHECK_MSG(result.Ok(), result.Error.Text);
                const std::uint64_t at = cave(memory, "t");
                std::uint8_t byte = 0;
                CHECK(at != 0 && engine.Process()->ReadExact(at, &byte, 1) && byte == 0xC3);
                Asm::AaMemory second = memory;
                const Asm::AaResult freed = run("dealloc(t)\n", second);
                CHECK_MSG(freed.Ok(), freed.Error.Text);
                CHECK(second.Allocations.empty());
                CHECK(!engine.Process()->ReadExact(at, &byte, 1));
            }
            {
                // unregistersymbol runs before registersymbol (4278-4282), so the symbol stays
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(macroPtr,8)\nregistersymbol(macroPtr)\nunregistersymbol(macroPtr)\n", memory);
                CHECK_MSG(result.Ok(), result.Error.Text);
                CHECK(engine.Symbols().Resolve("macroPtr").has_value());
                engine.Symbols().Unregister("macroPtr");
                release(memory);
            }
            {
                // registersymbol of a name the script does not declare fails before anything is written
                Asm::AaMemory memory;
                const Asm::AaResult result = run(target + ":\ndb 11\nregistersymbol(nothingHere)\n", memory);
                CHECK(!result.Ok());
                CHECK_EQ(g_macroTarget[0], std::uint8_t(0xA1));
            }
            {
                // createthread of an address that does not resolve fails before anything is written
                Asm::AaMemory memory;
                const Asm::AaResult result = run(target + ":\ndb 11\ncreatethread(nowhereToRun)\n", memory);
                CHECK(!result.Ok());
                CHECK_EQ(g_macroTarget[0], std::uint8_t(0xA1));
            }
            {
                // a failed write puts back what was already written before the allocations are freed
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(cave,$100)\ncave:\nret\n" + target + ":\ndb 11 22\n10:\ndb 90\n", memory);
                CHECK(!result.Ok() && result.Error.Code == ErrorCode::AaWriteFailed);
                CHECK_EQ(g_macroTarget[0], std::uint8_t(0xA1));
                CHECK_EQ(g_macroTarget[1], std::uint8_t(0xA2));
                CHECK(memory.Allocations.empty());
            }
            {
                // align N [fill] (Assemblerunit.pas 4938), numbers in hex
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(cave,$100)\ncave:\nnop\nalign 10 CC\nret\nalign 4\ndb 01\n", memory);
                CHECK_MSG(result.Ok(), result.Error.Text);
                const std::uint64_t at = cave(memory, "cave");
                CHECK_EQ(HexBytes(WrittenAt(result, at, 21)), std::string("90 CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC C3 00 00 00 01"));
                release(memory);
            }
            {
                // directives that change what the code means are refused, not dropped like comments
                Asm::AaMemory memory;
                const Asm::AaResult result = run("alloc(cave,$100)\ncave:\n{$try}\nmov eax,[0]\n{$except}\nret\n", memory);
                CHECK(!result.Ok() && result.Error.Text.find("{$try}") != std::string::npos);
                CHECK(memory.Allocations.empty());
                Asm::AaMemory commented;
                const Asm::AaResult inComment = run("// {$try}\nalloc(cave,$100)\ncave:\nret\n", commented);
                CHECK_MSG(inComment.Ok(), inComment.Error.Text);
                release(commented);
            }
            {
                // {$strict} turns implicit labels off
                Asm::AaMemory memory;
                const Asm::AaResult result = run("{$strict}\nalloc(cave,$100)\ncave:\njmp skip\nskip:\nret\n", memory);
                CHECK(!result.Ok());
            }
            {
                // a [DISABLE] part checked before its [ENABLE] part ran: names it registers are labels
                Asm::AaMemory memory;
                const Asm::AaResult result = engine.AutoAssemble("[ENABLE]\n[DISABLE]\nSomeHook:\nreadmem(z_SomeHook,6)\nunregistersymbol(*)\n", Asm::AaSection::Disable, memory, true, "check");
                CHECK_MSG(result.Ok(), result.Error.Text);
            }
        }

        void WriteText(const std::filesystem::path& path, const std::string& text)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << text;
        }

        GenerationResult Run(Engine& engine, const Library& library, const std::string& id, GenerationRequest request = {})
        {
            const TemplateInfo* info = library.Find(id);
            if (!info)
            {
                GenerationResult missing;
                missing.Failure.Message = "no template " + id;
                return missing;
            }
            return Generate(engine, library, *info, request);
        }

        bool HasProblem(const Library& library, const std::string& file, const std::string& part)
        {
            for (const LibraryProblem& problem : library.Problems())
            {
                if (problem.File.find(file) != std::string::npos && problem.Message.find(part) != std::string::npos)
                    return true;
            }
            return false;
        }

        void ParserBasics()
        {
            const ParseResult parsed = Parse("a << b >> c\n<% local x = 1 %>\nd");
            CHECK(parsed.Ok());
            CHECK_EQ(parsed.Nodes.size(), std::size_t(5));
            if (parsed.Nodes.size() == 5)
            {
                CHECK(parsed.Nodes[1].Kind == NodeKind::Expression && parsed.Nodes[1].Text == "b");
                CHECK(parsed.Nodes[3].Kind == NodeKind::Code && parsed.Nodes[3].Line == 2);
                CHECK_EQ(parsed.Nodes[4].Text, std::string("d"));
            }
            const ParseResult unclosed = Parse("one\ntwo << three");
            CHECK(!unclosed.Ok() && unclosed.ErrorLine == 2 && unclosed.Error.find("Unclosed <<") != std::string::npos);
            const ParseResult empty = Parse("x <<   >> y");
            CHECK(!empty.Ok() && empty.Error.find("Empty") != std::string::npos);
            CHECK_EQ(QuoteLua("a\"b\\c\n\x01"), std::string("\"a\\\"b\\\\c\\n\\001\""));
            const ParseResult includes = Parse("<< include(\"Header\") >> <% include('Other.CEA') %>");
            CHECK_EQ(IncludedNames(includes.Nodes).size(), std::size_t(2));

            // InTag finds the tags the way Parse does: the cursor right after
            // an opening delimiter and right before the closing one is inside.
            const std::string text = "a << B >> c <% d %> e << open";
            CHECK(!InTag(text, 0));
            CHECK(!InTag(text, 3));
            CHECK(InTag(text, 4));
            CHECK(InTag(text, text.find("B")));
            CHECK(InTag(text, text.find(">>")));
            CHECK(!InTag(text, text.find(" c")));
            CHECK(InTag(text, text.find("d")));
            CHECK(!InTag(text, text.find(" e")));
            CHECK(InTag(text, text.size()));
            CHECK(InTag("<% x = a << 2 %>", 12));
        }

        void Rendering(Engine& engine, const std::filesystem::path& folder)
        {
            WriteText(folder / "Standalone.CEA", "[DISABLE]\n\n<% if Flag then %>\nManifoldNop(Hook)\n<% end %>\n\nunregisterSymbol(*)\n");
            WriteText(folder / "Standalone.Settings.lua", "return { SchemaVersion = 2, Id = \"test.standalone\", Caption = \"Standalone\", Inputs = { { Name = \"Flag\", Type = \"boolean\", Default = true } } }");
            WriteText(folder / "Helpers.CEA", "<< hex(255) >>|<< join(\",\", \"a\", nil, \"\", \"b\") >>|<< default(nil, \"x\") >>|<< isEmpty(\"\") >>|<< byteCount(\"F3 0F 5C\") >>|<< bytesPrefix(\"F3 0F 5C\", 2) >>|<< trim(\"  t  \") >>|<< Missing >>|<% for i = 1, 3 do %><< i >><% end %>");
            WriteText(folder / "Helpers.Settings.lua", "return { SchemaVersion = 2, Id = \"test.helpers\", Caption = \"Helpers\" }");
            WriteText(folder / "Broken.CEA", "line one\nline two\n<< nil + 1 >>\n");
            WriteText(folder / "Broken.Settings.lua", "return { SchemaVersion = 2, Id = \"test.broken\", Caption = \"Broken\" }");
            WriteText(folder / "Syntax.CEA", "first\n<% if then %>\n");
            WriteText(folder / "Syntax.Settings.lua", "return { SchemaVersion = 2, Id = \"test.syntax\", Caption = \"Syntax\" }");
            WriteText(folder / "Partials" / "Part.CEA", "[<< Word >>]");
            WriteText(folder / "Partials" / "LoopA.CEA", "<< include(\"LoopB\") >>");
            WriteText(folder / "Partials" / "LoopB.CEA", "<< include(\"LoopA\") >>");
            WriteText(folder / "Includes.CEA", "<< include(\"Part\") >><< include(\"Part.CEA\") >>");
            WriteText(folder / "Includes.Settings.lua", "return { SchemaVersion = 2, Id = \"test.includes\", Caption = \"Includes\", Inputs = { { Name = \"Word\", Default = \"w\" } } }");
            WriteText(folder / "Loop.CEA", "<< include(\"LoopA\") >>");
            WriteText(folder / "Loop.Settings.lua", "return { SchemaVersion = 2, Id = \"test.loop\", Caption = \"Loop\" }");
            WriteText(folder / "Restricted.CEA", "<< tostring(print) >>");
            WriteText(folder / "Restricted.Settings.lua", "return { SchemaVersion = 2, Id = \"test.restricted\", Caption = \"Restricted\" }");
            WriteText(folder / "Legacy.CEA", "<< Address >>");
            WriteText(folder / "Legacy.Settings.lua", "return { Caption = \"Legacy\", SubMenuName = \"[3] Pointer Hooks\", MenuOrder = 7 }");
            WriteText(folder / "Globals.CEA", "<< type(print) >>|<< TemplateSettings.MyOffset >>|<< TemplateSettings.SubMenuName >>|<< TemplateSettings.MenuOrder >>");
            WriteText(folder / "Globals.Settings.lua", "return { SchemaVersion = 2, Id = \"test.globals\", Caption = \"Globals\", AllowUnsafeGlobals = true, SubMenuName = \"Pointers\", MenuOrder = 7, MyOffset = 0x30 }");
            WriteText(folder / "Hooks.CEA", "<< HookName >>|<< HookNameParsed >>|<< GlobalAlloc >>");
            WriteText(folder / "Hooks.Settings.lua", "return { SchemaVersion = 2, Id = \"test.hooks\", Caption = \"Hooks\", Requires = { \"HookName\" }, Memory = { AllocationSize = 4096 } }");

            Library library(folder);
            library.Load(engine.Lua().State());

            const GenerationResult standalone = Run(engine, library, "test.standalone");
            CHECK_MSG(standalone.Text && *standalone.Text == "[DISABLE]\n\nManifoldNop(Hook)\n\nunregisterSymbol(*)\n", standalone.Text.value_or(standalone.Failure.Message));
            GenerationRequest off;
            off.Inputs["Flag"] = false;
            const GenerationResult without = Run(engine, library, "test.standalone", off);
            CHECK_MSG(without.Text && *without.Text == "[DISABLE]\n\n\nunregisterSymbol(*)\n", without.Text.value_or(without.Failure.Message));

            const GenerationResult helpers = Run(engine, library, "test.helpers");
            CHECK_MSG(helpers.Text && *helpers.Text == "FF|a,b|x|true|3|F3 0F|t||123", helpers.Text.value_or(helpers.Failure.Message));
            CHECK(!helpers.Warnings.empty() && helpers.Warnings.front().find("Missing") != std::string::npos);

            const GenerationResult broken = Run(engine, library, "test.broken");
            CHECK(!broken.Ok() && broken.Failure.Code == ErrorCode::TemplateRender && broken.Failure.Line == 3);
            CHECK(broken.Failure.Source.find("Broken.CEA") != std::string::npos);
            const GenerationResult syntax = Run(engine, library, "test.syntax");
            CHECK_MSG(!syntax.Ok() && syntax.Failure.Code == ErrorCode::TemplateSyntax && syntax.Failure.Line == 2, std::to_string(syntax.Failure.Line) + " " + syntax.Failure.Message);

            const GenerationResult includes = Run(engine, library, "test.includes");
            CHECK_MSG(includes.Text && *includes.Text == "[w][w]", includes.Text.value_or(includes.Failure.Message));
            const GenerationResult loop = Run(engine, library, "test.loop");
            CHECK(!loop.Ok() && loop.Failure.Code == ErrorCode::TemplateIncludeCycle);

            const GenerationResult restricted = Run(engine, library, "test.restricted");
            CHECK_MSG(restricted.Text && *restricted.Text == "nil", restricted.Text.value_or(restricted.Failure.Message));
            const TemplateInfo* legacy = library.Find("legacy.legacy");
            CHECK(legacy && legacy->Category == "[3] Pointer Hooks" && legacy->AllowUnsafeGlobals && legacy->Order && *legacy->Order == 7.0);
            const GenerationResult legacyText = Run(engine, library, "legacy.legacy");
            CHECK(!legacyText.Ok() && legacyText.Failure.Message.find("No target process") != std::string::npos);
            const GenerationResult globals = Run(engine, library, "test.globals");
            CHECK_MSG(globals.Text && *globals.Text == "function|48|Pointers|7", globals.Text.value_or(globals.Failure.Message));
            std::optional<double> order;
            CHECK(CategoryLabel("[3] Pointer Hooks", &order) == "Pointer Hooks" && order && *order == 3.0);

            GenerationRequest named;
            named.Ask.Ask = [](const std::string& title, const std::string&, const std::string&) -> std::optional<std::string>
            {
                return title == "Hook Name" ? std::optional<std::string>("9 lives!") : std::nullopt;
            };
            const GenerationResult hooks = Run(engine, library, "test.hooks", named);
            CHECK_MSG(hooks.Text && *hooks.Text == "_9_lives_|_9_lives_Hook|alloc(n__9_lives_,$1000)", hooks.Text.value_or(hooks.Failure.Message));
            GenerationRequest cancelled;
            cancelled.Ask.Ask = [](const std::string&, const std::string&, const std::string&) -> std::optional<std::string> { return std::nullopt; };
            const GenerationResult stopped = Run(engine, library, "test.hooks", cancelled);
            CHECK(!stopped.Ok() && stopped.Failure.Cancelled);
        }

        void Discovery(Engine& engine, const std::filesystem::path& folder)
        {
            WriteText(folder / "Header.CEA", "user header");
            WriteText(folder / "Sub" / "Deeper" / "Plain.CEA", "plain");
            WriteText(folder / "BadType.CEA", "x");
            WriteText(folder / "BadType.Settings.lua", "return { Caption = 5 }");
            WriteText(folder / "BadId.CEA", "x");
            WriteText(folder / "BadId.Settings.lua", "return { SchemaVersion = 2, Id = \"has space\" }");
            WriteText(folder / "BadEnum.CEA", "x");
            WriteText(folder / "BadEnum.Settings.lua", "return { SchemaVersion = 2, Id = \"test.badenum\", Inputs = { { Name = \"Mode\", Type = \"enum\", Values = { \"A\" }, Default = \"B\" } } }");
            WriteText(folder / "Escape.CEA", "x");
            WriteText(folder / "Escape.Settings.lua", "return { SchemaVersion = 2, Id = \"test.escape\", Caption = tostring(io) .. tostring(os) }");
            WriteText(folder / "Twin.CEA", "x");
            WriteText(folder / "Twin.Settings.lua", "return { SchemaVersion = 2, Id = \"test.twin\" }");
            WriteText(folder / "Sub" / "Twin.CEA", "y");
            WriteText(folder / "Sub" / "Twin.Settings.lua", "return { SchemaVersion = 2, Id = \"test.twin\" }");
            WriteText(folder / "Override.CEA", "user call lua");
            WriteText(folder / "Override.Settings.lua", "return { SchemaVersion = 2, Id = \"manifold.builtin.call-lua\", Caption = \"My Lua\" }");

            Library library(folder);
            library.Load(engine.Lua().State());
            const TemplateInfo* plain = library.Find("legacy.plain");
            CHECK(plain && plain->Category == "Sub > Deeper" && plain->Caption == "Plain" && !plain->HasSettings);
            CHECK(library.Find("legacy.header") == nullptr);
            CHECK(HasProblem(library, "BadType.CEA", "Caption must be a string"));
            CHECK(HasProblem(library, "BadId.CEA", "Id may only contain"));
            CHECK(HasProblem(library, "BadEnum.CEA", "is not in Values"));
            CHECK(HasProblem(library, "Twin.CEA", "already used"));
            const TemplateInfo* escape = library.Find("test.escape");
            CHECK(escape && escape->Caption == "nilnil");
            const TemplateInfo* overridden = library.Find("manifold.builtin.call-lua");
            CHECK(overridden && !overridden->BuiltIn && overridden->Caption == "My Lua");
            const auto header = library.Header();
            CHECK(header && header->Text == "user header");
            CHECK(!library.ReloadIfChanged(engine.Lua().State()));
            WriteText(folder / "Sub" / "New.CEA", "new");
            CHECK(library.ReloadIfChanged(engine.Lua().State()) && library.Find("legacy.new") != nullptr);
        }

        std::string ReadBack(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            std::stringstream content;
            content << file.rdbuf();
            return content.str();
        }

        void Manager(Engine& engine, const std::filesystem::path& root)
        {
            lua_State* state = engine.Lua().State();
            SettingsForm form;
            form.Id = "test.form";
            form.Caption = "Form \"Test\"";
            form.Description = "Line one\nline two";
            form.Category = "Hooks > Mine";
            form.ExplicitCategory = true;
            form.CategoryOrder = 3;
            form.Order = 1.5;
            form.InSubMenu = false;
            form.Tags = { "a", "b" };
            form.Requires = { "HookName" };
            form.Architectures = { "x64" };
            form.AllowUnsafeGlobals = true;
            InputDefinition count;
            count.Name = "Count";
            count.Type = "integer";
            count.Caption = "How many";
            count.Default = std::int64_t(2);
            count.Min = 1;
            count.Max = 16;
            InputDefinition mode;
            mode.Name = "Mode";
            mode.Type = "enum";
            mode.Caption = "Mode";
            mode.Values = { "A", "B" };
            mode.Default = std::string("B");
            InputDefinition flag;
            flag.Name = "Flag";
            flag.Type = "boolean";
            flag.Caption = "Flag";
            flag.Default = true;
            form.Inputs = { count, mode, flag };
            form.Memory.AskForHookName = false;
            form.Memory.DefaultHookName = "Hook";
            form.Memory.AllocationSize = "$2000";
            form.CustomFields = { { "MyOffset", "48" }, { "Nested", "{ 1, \"x\", deep = { y = true } }" } };
            const std::string written = WriteSettings(form);
            std::string error;
            const auto back = ReadSettings(state, written, "=form", "Form", error);
            CHECK_MSG(back.has_value(), error + "\n" + written);
            if (back)
            {
                CHECK(back->Id == form.Id && back->Caption == form.Caption && back->Description == form.Description);
                CHECK(back->Category == "Hooks > Mine" && back->ExplicitCategory && back->CategoryOrder == 3.0 && back->Order == 1.5 && !back->InSubMenu);
                CHECK(back->Tags == form.Tags && back->Requires == form.Requires && back->Architectures == form.Architectures && back->AllowUnsafeGlobals);
                CHECK(back->Inputs.size() == 3 && back->Inputs[0].Caption == "How many" && back->Inputs[0].Max == 16.0 && std::get<std::string>(back->Inputs[1].Default) == "B" && std::get<bool>(back->Inputs[2].Default));
                CHECK(back->Memory.AskForHookName == false && back->Memory.DefaultHookName == std::optional<std::string>("Hook") && back->Memory.AllocationSize == std::optional<std::string>("$2000"));
                CHECK_MSG(back->CustomFields == form.CustomFields, written);
                CHECK(!back->SettingsHasComments);
                CHECK(WriteSettings(FormOf(*back)) == written);
            }
            const auto commented = ReadSettings(state, "-- note\nreturn { Caption = \"x -- not a comment\" }", "=c", "C", error);
            CHECK(commented && commented->SettingsHasComments);
            const auto plain = ReadSettings(state, "return { Caption = \"x -- inside\" }", "=p", "P", error);
            CHECK(plain && !plain->SettingsHasComments);

            const std::filesystem::path folder = root / "manage";
            Library library(folder);
            library.Load(state);
            SettingsForm fresh;
            fresh.Id = UniqueId(library, "My Hook");
            fresh.Caption = "My Hook";
            fresh.Category = "Hooks > Mine";
            CHECK_EQ(fresh.Id, std::string("user.my-hook"));
            std::filesystem::path created;
            CHECK(CreateTemplate(folder, fresh, "<< HookName >>", &created).Ok());
            CHECK(created == folder / "Hooks" / "Mine" / "My Hook.CEA");
            library.Load(state);
            const TemplateInfo* mine = library.Find("user.my-hook");
            CHECK(mine && mine->Category == "Hooks > Mine" && !mine->ExplicitCategory);
            CHECK_EQ(UniqueId(library, "My Hook"), std::string("user.my-hook-2"));
            if (mine)
            {
                CHECK(SaveScript(*mine, "<< HookNameParsed >>").Ok());
                CHECK_EQ(ReadBack(created), std::string("<< HookNameParsed >>"));
                std::string copyId;
                CHECK(DuplicateTemplate(library, *mine, folder, false, &copyId).Ok() && copyId == "user.my-hook-copy");
                CHECK(MoveTemplate(*mine, folder, "Other").Ok());
            }
            library.Load(state);
            const TemplateInfo* moved = library.Find("user.my-hook");
            CHECK(moved && moved->Category == "Other" && moved->ScriptPath == folder / "Other" / "My Hook.CEA");
            const TemplateInfo* copy = library.Find("user.my-hook-copy");
            CHECK(copy && copy->Caption == "My Hook (copy)" && copy->Category == "Hooks > Mine");

            WriteText(folder / "Explicit.CEA", "x");
            WriteText(folder / "Explicit.Settings.lua", "return {\n    SchemaVersion = 2,\n    Id = \"test.explicit\",\n    -- keep me\n    Category = \"First\",\n}\n");
            library.Load(state);
            if (const TemplateInfo* explicitOne = library.Find("test.explicit"))
                CHECK(explicitOne->SettingsHasComments && MoveTemplate(*explicitOne, folder, "Second > Deeper").Ok());
            const std::string movedText = ReadBack(folder / "Explicit.Settings.lua");
            CHECK(movedText.find("-- keep me") != std::string::npos && movedText.find("Category = \"Second > Deeper\"") != std::string::npos);

            const TemplateInfo* builtin = library.Find("manifold.builtin.call-lua");
            CHECK(builtin && builtin->BuiltIn && !MoveTemplate(*builtin, folder, "X").Ok() && !DeleteTemplate(*builtin, false).Ok());
            if (builtin)
                CHECK(DuplicateTemplate(library, *builtin, folder, true).Ok());
            library.Load(state);
            const TemplateInfo* overriding = library.Find("manifold.builtin.call-lua");
            CHECK(overriding && !overriding->BuiltIn && overriding->Caption == "Call Lua" && overriding->ScriptPath == folder / "Call Lua.CEA");
            if (overriding)
                CHECK(DeleteTemplate(*overriding, false).Ok());
            library.Load(state);
            CHECK(library.Find("manifold.builtin.call-lua") && library.Find("manifold.builtin.call-lua")->BuiltIn);

            const std::filesystem::path source = root / "import";
            WriteText(source / "One.CEA", "1");
            WriteText(source / "One.Settings.lua", "return { Caption = \"One\" }");
            WriteText(source / "Partials" / "Part.CEA", "p");
            WriteText(source / "notes.txt", "ignored");
            int imported = 0;
            CHECK(ImportTemplates(source, folder, &imported).Ok() && imported == 2);
            CHECK(std::filesystem::exists(folder / "Partials" / "Part.CEA") && !std::filesystem::exists(folder / "notes.txt"));
            CHECK(ImportTemplates(source, folder, &imported).Ok() && imported == 0);
            CHECK(!ImportTemplates(folder, folder, &imported).Ok());

            WriteText(folder / "Checked.CEA", "line\n<< include(\"Missing\") >>\n<% local x = 1 %>\n<< x >> << Mystery >> << hex(1) >> << TemplateSettings.Caption >>\n");
            WriteText(folder / "Checked.Settings.lua", "return { SchemaVersion = 2, Id = \"test.checked\", Requires = { \"NoSuchValue\" }, Optional = { \"Address\" } }");
            WriteText(folder / "BadSyntax.CEA", "a\nb\n<% if then %>\n");
            library.Load(state);
            if (const TemplateInfo* checked = library.Find("test.checked"))
            {
                const CheckEntry entry = CheckTemplate(state, library, *checked);
                CHECK(entry.Errors.size() == 2 && entry.Warnings.size() == 1);
                CHECK(!entry.Warnings.empty() && entry.Warnings.front().find("'Mystery' on line 4") != std::string::npos);
            }
            if (const TemplateInfo* bad = library.Find("legacy.badsyntax"))
            {
                const CheckEntry entry = CheckTemplate(state, library, *bad);
                CHECK_MSG(entry.Errors.size() == 1 && entry.Errors.front().find("(line 3)") != std::string::npos, entry.Errors.empty() ? "" : entry.Errors.front());
            }
            Library builtins;
            builtins.Load(state);
            for (const CheckEntry& entry : CheckAll(state, builtins))
                CHECK_MSG(entry.Errors.empty() && entry.Warnings.empty(), entry.Caption + ": " + (entry.Errors.empty() ? (entry.Warnings.empty() ? "" : entry.Warnings.front()) : entry.Errors.front()));

            GenerationRequest sample;
            sample.SampleValues = true;
            const GenerationResult aob = Run(engine, builtins, "manifold.builtin.aob-injection", sample);
            CHECK_MSG(aob.Text && aob.Text->find("aobscanmodule(INJECT,Game.exe,48 8B 81 08 01 00 00") != std::string::npos && aob.Text->find("INJECTING HERE") != std::string::npos, aob.Text.value_or(aob.Failure.Message));
            sample.Use14ByteJump = true;
            const GenerationResult wide = Run(engine, builtins, "manifold.builtin.code-injection", sample);
            CHECK_MSG(wide.Text && wide.Text->find("jmp far newmem") != std::string::npos && wide.Text->find("db 90 90") != std::string::npos, wide.Text.value_or(wide.Failure.Message));
            sample.Use14ByteJump = false;
            sample.Script = "<< Process >>|<< BaseAddressRegister >>+<< BaseAddressOffset >>";
            const GenerationResult edited = Run(engine, builtins, "manifold.builtin.cheat-table-framework", sample);
            CHECK_MSG(edited.Text && *edited.Text == "Game.exe|rcx+00000108", edited.Text.value_or(edited.Failure.Message));
        }

        void BuiltIns(Engine& engine)
        {
            Library library;
            library.Load(engine.Lua().State());
            CHECK(library.Problems().empty());
            for (const char* id : { "manifold.builtin.aob-injection", "manifold.builtin.code-injection", "manifold.builtin.full-injection", "manifold.builtin.cheat-table-framework", "manifold.builtin.call-lua" })
            {
                const TemplateInfo* info = library.Find(id);
                CHECK_MSG(info && info->BuiltIn && !info->InSubMenu && info->Category == "Manifold Engine", id);
            }
            const GenerationResult framework = Run(engine, library, "manifold.builtin.cheat-table-framework");
            CHECK(framework.Text && *framework.Text == Asm::CheatTableTemplate());
            const GenerationResult lua = Run(engine, library, "manifold.builtin.call-lua");
            CHECK(lua.Text && lua.Text->find("{$lua}") != std::string::npos);
            const GenerationResult noProcess = Run(engine, library, "manifold.builtin.code-injection");
            CHECK(!noProcess.Ok() && !noProcess.Failure.Cancelled && noProcess.Failure.Message.find("No target process") != std::string::npos);
            CHECK(IsVariable("OriginalBytes") && !IsVariable("Nonsense") && Variables().size() > 50);
            std::string error;
            InputDefinition number;
            number.Name = "Count";
            number.Caption = "Count";
            number.Type = "integer";
            number.Min = 1;
            number.Max = 16;
            CHECK(ParseInput(number, "0x10", error) && std::get<std::int64_t>(*ParseInput(number, "0x10", error)) == 16);
            CHECK(!ParseInput(number, "17", error) && error.find("at most") != std::string::npos);
            CHECK(!ParseInput(number, "1.5", error) && error.find("whole") != std::string::npos);
        }

        void Live(Engine& engine)
        {
            const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return;
            Library library;
            library.Load(engine.Lua().State());
            GenerationRequest request;
            request.Address = reinterpret_cast<std::uint64_t>(&RunTemplateTests);
            request.Ask.Ask = [](const std::string&, const std::string&, const std::string& value) -> std::optional<std::string> { return value; };

            const GenerationResult aob = Run(engine, library, "manifold.builtin.aob-injection", request);
            CHECK_MSG(aob.Ok(), Describe(aob.Failure, "AOB injection"));
            if (aob.Text)
            {
                CHECK(aob.Text->find("aobscanmodule(INJECT,Manifold-Engine.Tests.exe,") != std::string::npos);
                CHECK(aob.Text->find("INJECTING HERE") != std::string::npos);
                const std::size_t start = aob.Text->find("aobscanmodule(INJECT,Manifold-Engine.Tests.exe,");
                const std::size_t open = aob.Text->find(',', aob.Text->find(',', start) + 1);
                const std::size_t close = aob.Text->find(')', open);
                const auto bytes = ParseHexBytes(aob.Text->substr(open + 1, close - open - 1));
                CHECK(bytes && bytes->size() >= 8);
                if (bytes)
                {
                    const auto module = engine.Process()->ModuleAt(request.Address);
                    Scan::Pattern pattern;
                    pattern.Bytes = *bytes;
                    pattern.Mask.assign(bytes->size(), 0xFF);
                    const auto hits = Scan::FindPattern(*engine.Process(), pattern, module->Base, module->Base + module->Size, false, 4);
                    CHECK_EQ(hits.size(), std::size_t(1));
                }
                const Asm::AaResult check = engine.AutoAssemble(*aob.Text, true);
                CHECK_MSG(check.Ok(), check.Error.Text + "\n" + *aob.Text);
            }
            std::vector<std::uint8_t> site(8);
            engine.Process()->Read(request.Address, site.data(), site.size());
            const std::string scanned = "aobscanmodule(SITE,Manifold-Engine.Tests.exe," + HexBytes(site) + ")\nalloc(cave,$100)\ncave:\nreadmem(SITE,6)\nreadmem(cave,4)\n";
            const Asm::AaResult readAfterScan = engine.AutoAssemble(scanned, true);
            CHECK_MSG(readAfterScan.Ok(), readAfterScan.Error.Text);
            const std::uint64_t mainBase = engine.Process()->MainModule().value().Base;
            const std::string viaProcess = "aobscanmodule(SITE,$process," + HexBytes(site) + ")\nalloc(cave,$100)\ncave:\nreadmem(SITE,6)\n";
            const Asm::AaResult scanViaProcess = engine.AutoAssemble(viaProcess, true);
            CHECK_MSG(scanViaProcess.Ok(), scanViaProcess.Error.Text);
            const std::string probe = Hex(reinterpret_cast<std::uint64_t>(&g_mainModuleProbe));
            const std::string written = "assert($process,4D 5A)\nassert(\"$process\",4D 5A)\nassert(mainmodule,4D 5A)\n" + probe + ":\ndq $process\ndq \"$process\"+10\ndq mainmodule+20\nmov rax,$process\n";
            const Asm::AaResult wroteMain = engine.AutoAssemble(written, false);
            CHECK_MSG(wroteMain.Ok(), wroteMain.Error.Text);
            CHECK_EQ(g_mainModuleProbe[0], mainBase);
            CHECK_EQ(g_mainModuleProbe[1], mainBase + 0x10);
            CHECK_EQ(g_mainModuleProbe[2], mainBase + 0x20);
            CHECK_EQ(g_mainModuleProbe[3] & 0xFFFF, std::uint64_t(0xB848));
            // A syntax check reads nothing it cannot read, like CE: custom commands answer the
            // check with define(Name, 0) (the owner's ManifoldScanModule) and readmem(Name, n) follows.
            const Asm::AaResult unreadable = engine.AutoAssemble("alloc(cave,$100)\ncave:\nreadmem(10,4)\n", true);
            CHECK_MSG(unreadable.Ok(), unreadable.Error.Text);
            const Asm::AaResult unknown = engine.AutoAssemble("alloc(cave,$100)\ncave:\nreadmem(NoSuchSymbol,4)\n", true);
            CHECK_MSG(unknown.Ok(), unknown.Error.Text);
            const Asm::AaResult definedZero = engine.AutoAssemble("define(SITE,0)\nalloc(cave,$100)\ncave:\nreadmem(SITE,7)\n", true);
            CHECK_MSG(definedZero.Ok(), definedZero.Error.Text);
            const std::string hookShape =
                "define(Hook,0000000000000000)\n"
                "alloc(n_Hook,$1000,Hook)\n"
                "assert(Hook,83 79 0C 05 75 0B)\n"
                "label(o_Hook r_Hook z_Hook)\n"
                "n_Hook:\n  mov dword ptr [rcx+0C],02\n"
                "o_Hook:\n  readMem(Hook,4)\n  reassemble(Hook+4)\n  jmp r_Hook\n"
                "z_Hook:\n  readMem(Hook,6)\n"
                "Hook:\n  jmp n_Hook\n  db 90\n"
                "r_Hook:\n"
                "registersymbol(Hook z_Hook)\n";
            const Asm::AaResult hookCheck = engine.AutoAssemble(hookShape, true);
            CHECK_MSG(hookCheck.Ok(), hookCheck.Error.Text);
            const Asm::AaResult missingSource = engine.AutoAssemble("alloc(cave,$100)\ncave:\nreassemble(NoSuchSymbol)\n", true);
            CHECK_MSG(missingSource.Ok(), missingSource.Error.Text);
            const Asm::AaResult missingSourceReal = engine.AutoAssemble("alloc(cave,$100)\ncave:\nreassemble(NoSuchSymbol)\n", false);
            CHECK(!missingSourceReal.Ok());
            const Asm::AaResult assertReal = engine.AutoAssemble("assert(10,83 79 0C 05)\n", false);
            CHECK(!assertReal.Ok() && assertReal.Error.Code == ErrorCode::AaAssertFailed);
            const std::string missingModule = "aobscanmodule(Gone,NoSuchModule.dll,90 90 90)\nalloc(cave,$100,Gone)\ncave:\nreadmem(Gone,3)\nGone:\njmp cave\n";
            const Asm::AaResult scriptCheck = engine.AutoAssemble(missingModule, true);
            CHECK_MSG(scriptCheck.Ok(), scriptCheck.Error.Text);
            Asm::AaMemory checkMemory;
            const ScriptCheck programCheck = engine.CheckScript(missingModule, Asm::AaSection::Whole, checkMemory);
            CHECK(!programCheck.Result.Ok() && programCheck.Result.Error.Code == ErrorCode::ExprUnknownSymbol);
            const Asm::AaResult badAssertPattern = engine.AutoAssemble("assert(10,ZZ)\n", true);
            CHECK(!badAssertPattern.Ok() && badAssertPattern.Error.Code == ErrorCode::ScanBadPattern);
            AutoAssemblerMacros(engine);
            const Asm::AaResult unreadableReal = engine.AutoAssemble("alloc(cave,$100)\ncave:\nreadmem(10,4)\n", false);
            CHECK(!unreadableReal.Ok() && unreadableReal.Error.Code == ErrorCode::AaReadMemFailed && unreadableReal.Line == 3);
            const Asm::AaResult unknownReal = engine.AutoAssemble("alloc(cave,$100)\ncave:\nreadmem(NoSuchSymbol,4)\n", false);
            CHECK(!unknownReal.Ok() && unknownReal.Error.Code == ErrorCode::AaBadArguments && unknownReal.Line == 3);
            for (const char* id : { "manifold.builtin.code-injection", "manifold.builtin.full-injection" })
            {
                const GenerationResult result = Run(engine, library, id, request);
                CHECK_MSG(result.Ok(), Describe(result.Failure, id));
                if (result.Text)
                {
                    const Asm::AaResult check = engine.AutoAssemble(*result.Text, true);
                    CHECK_MSG(check.Ok(), check.Error.Text + "\n" + *result.Text);
                }
            }
            engine.CloseProcess();
        }
        void Folder(Engine& engine)
        {
            wchar_t value[MAX_PATH] = L"";
            if (!::GetEnvironmentVariableW(L"MANIFOLD_TEMPLATE_CHECK", value, MAX_PATH))
                return;
            const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
            if (!opened)
                return;
            std::uint64_t address = reinterpret_cast<std::uint64_t>(&RunTemplateTests);
            for (int index = 0; index < 400; ++index)
            {
                const Disasm::Instruction instruction = engine.Disassembler().Read(*engine.Process(), address, &engine.Symbols());
                if (!instruction.Valid)
                    break;
                const std::string text = instruction.Text();
                if (instruction.Length >= 3 && text.find("[r") != std::string::npos && text.find("+") != std::string::npos && text.find("rip") == std::string::npos && text.find("rsp") == std::string::npos
                    && instruction.Kind == Disasm::Flow::None)
                    break;
                address += instruction.Length;
            }
            std::printf("  folder check at %s: %s\n", engine.Symbols().ModuleOffset(address).c_str(), engine.Disassembler().Read(*engine.Process(), address, &engine.Symbols()).Text().c_str());
            Library library(value);
            library.Load(engine.Lua().State());
            for (const LibraryProblem& problem : library.Problems())
                std::printf("  SKIPPED %s: %s\n", problem.File.c_str(), problem.Message.c_str());
            for (const TemplateInfo& info : library.Templates())
            {
                if (info.BuiltIn)
                    continue;
                GenerationRequest request;
                request.Address = address;
                request.Ask.Ask = [](const std::string&, const std::string&, const std::string& answer) -> std::optional<std::string> { return answer; };
                const GenerationResult result = Generate(engine, library, info, request);
                if (!result.Ok())
                {
                    std::printf("  NOT GENERATED %s\n%s\n", info.Caption.c_str(), Describe(result.Failure, info.Caption).c_str());
                    continue;
                }
                const Asm::AaResult check = engine.AutoAssemble(*result.Text, true);
                std::printf("  %s %s%s%s\n", check.Ok() ? "OK" : "CHECK FAILED", info.Caption.c_str(), check.Ok() ? "" : ": ", check.Ok() ? "" : check.Error.Text.c_str());
                for (const std::string& warning : result.Warnings)
                    std::printf("      warning: %s\n", warning.c_str());
                const std::filesystem::path out = std::filesystem::path(value) / L"_rendered" / (info.Name + ".txt");
                WriteText(out, *result.Text);
            }
            engine.CloseProcess();
        }
    }

    int RunTemplateTests()
    {
        const int before = Totals().Failures;
        Settings settings;
        Engine engine(settings);
        engine.Lua().Run("__manifoldHeadless = true", "headless");
        const std::filesystem::path root = std::filesystem::temp_directory_path() / ("Manifold-Template-Tests-" + std::to_string(::GetCurrentProcessId()));
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        ParserBasics();
        Rendering(engine, root / "render");
        Discovery(engine, root / "discover");
        Manager(engine, root);
        BuiltIns(engine);
        Live(engine);
        Folder(engine);
        std::filesystem::remove_all(root, ignored);
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("templates", "Templates", 110, [](const SuiteContext&) { return RunTemplateTests(); });
}
