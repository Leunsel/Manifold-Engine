#include "TestKit.h"

#include "Asm/AsmAssist.h"

#include "Text/Highlight.h"
#include "Text/Json.h"
#include "Text/LuaContext.h"
#include "Text/TextBuffer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        using Text::Language;
        using Text::Token;

        struct Piece
        {
            std::string Text;
            Token Kind;
        };

        std::vector<Piece> Pieces(Language language, std::string_view text)
        {
            std::vector<Piece> pieces;
            for (const Text::TokenSpan& span : Text::Highlight(language, text))
                pieces.push_back({ std::string(text.substr(span.Start, span.End - span.Start)), span.Kind });
            return pieces;
        }

        bool Has(const std::vector<Piece>& pieces, std::string_view text, Token kind)
        {
            for (const Piece& piece : pieces)
            {
                if (piece.Text == text && piece.Kind == kind)
                    return true;
            }
            return false;
        }

        bool Covered(const std::vector<Piece>& pieces, std::string_view text)
        {
            for (const Piece& piece : pieces)
            {
                if (piece.Text == text)
                    return true;
            }
            return false;
        }

        void LuaHighlight()
        {
            auto pieces = Pieces(Language::Lua, "local x = 10 -- note");
            CHECK(Has(pieces, "local", Token::Keyword));
            CHECK(Has(pieces, "10", Token::Number));
            CHECK(Has(pieces, "-- note", Token::Comment));
            CHECK(!Covered(pieces, "x"));

            pieces = Pieces(Language::Lua, "print(\"a\\\"b\") manifold.log.info('x')");
            CHECK(Has(pieces, "print", Token::Call));
            CHECK(Has(pieces, "\"a\\\"b\"", Token::String));
            CHECK(Has(pieces, "info", Token::Call));
            CHECK(Has(pieces, "'x'", Token::String));
            CHECK(!Covered(pieces, "manifold"));

            pieces = Pieces(Language::Lua, "n = {0x1F, 1e-3, .5, 3., 0x1p+4}");
            CHECK(Has(pieces, "0x1F", Token::Number));
            CHECK(Has(pieces, "1e-3", Token::Number));
            CHECK(Has(pieces, ".5", Token::Number));
            CHECK(Has(pieces, "3.", Token::Number));
            CHECK(Has(pieces, "0x1p+4", Token::Number));

            pieces = Pieces(Language::Lua, "s = [[first\nsecond]] .. x\nlocal y");
            CHECK(Has(pieces, "[[first", Token::String));
            CHECK(Has(pieces, "second]]", Token::String));
            CHECK(Has(pieces, "local", Token::Keyword));

            pieces = Pieces(Language::Lua, "--[==[ a ]] b\nc ]==] return");
            CHECK(Has(pieces, "--[==[ a ]] b", Token::Comment));
            CHECK(Has(pieces, "c ]==]", Token::Comment));
            CHECK(Has(pieces, "return", Token::Keyword));

            pieces = Pieces(Language::Lua, "t[ [=[x]=] ] = a..b");
            CHECK(Has(pieces, "[=[x]=]", Token::String));
            CHECK(!Covered(pieces, ".."));

            Text::LineState state = Text::InitialState(Language::Lua);
            std::vector<Text::TokenSpan> spans;
            Text::HighlightLine(Language::Lua, "x = [==[ open", 0, state, spans);
            CHECK(state.Kind == Text::Block::LuaString);
            CHECK_EQ(state.Level, 2);
            Text::HighlightLine(Language::Lua, "]] still ]==] done", 13, state, spans);
            CHECK(state.Kind == Text::Block::Lua);
            CHECK_EQ(spans.back().End, static_cast<std::size_t>(13 + 13));
        }

        void AaHighlight()
        {
            auto pieces = Pieces(Language::AutoAssembler, "[ENABLE]\nalloc(newmem,$1000)\nnewmem:\n  mov rax,[rbx+10] // load\n");
            CHECK(Has(pieces, "[ENABLE]", Token::Section));
            CHECK(Has(pieces, "alloc", Token::Keyword));
            CHECK(Has(pieces, "$1000", Token::Number));
            CHECK(!Covered(pieces, "newmem,"));
            CHECK(Has(pieces, "newmem", Token::Label));
            CHECK(Has(pieces, "mov", Token::Mnemonic));
            CHECK(Has(pieces, "rax", Token::Register));
            CHECK(Has(pieces, "rbx", Token::Register));
            CHECK(Has(pieces, "10", Token::Number));
            CHECK(Has(pieces, "// load", Token::Comment));

            pieces = Pieces(Language::AutoAssembler, "{ note\nstill } nop\n/* a\nb */ ret");
            CHECK(Has(pieces, "{ note", Token::Comment));
            CHECK(Has(pieces, "still }", Token::Comment));
            CHECK(Has(pieces, "nop", Token::Mnemonic));
            CHECK(Has(pieces, "/* a", Token::Comment));
            CHECK(Has(pieces, "b */", Token::Comment));
            CHECK(Has(pieces, "ret", Token::Mnemonic));

            pieces = Pieces(Language::AutoAssembler, "{$lua}\nlocal v = readInteger('x') -- c\n{$ASM}\nnop\n");
            CHECK(Has(pieces, "{$lua}", Token::Section));
            CHECK(Has(pieces, "local", Token::Keyword));
            CHECK(Has(pieces, "readInteger", Token::Call));
            CHECK(Has(pieces, "-- c", Token::Comment));
            CHECK(Has(pieces, "{$ASM}", Token::Section));
            CHECK(Has(pieces, "nop", Token::Mnemonic));

            pieces = Pieces(Language::AutoAssembler, "{$lua} print([[a\nb]]) {$asm} jmp label");
            CHECK(Has(pieces, "print", Token::Call));
            CHECK(Has(pieces, "b]]", Token::String));
            CHECK(Has(pieces, "{$asm}", Token::Section));
            CHECK(Has(pieces, "jmp", Token::Mnemonic));
            CHECK(Has(pieces, "label", Token::Keyword));

            CHECK(Pieces(Language::Plain, "local x = 1").empty());
        }

        void JsonHighlight()
        {
            auto pieces = Pieces(Language::Json, "{\n  \"name\": \"Crab\",\n  \"level\": -12.5e3,\n  \"ok\": true, \"none\" : null\n}");
            CHECK(Has(pieces, "\"name\"", Token::Label));
            CHECK(Has(pieces, "\"Crab\"", Token::String));
            CHECK(Has(pieces, "-12.5e3", Token::Number));
            CHECK(Has(pieces, "true", Token::Keyword));
            CHECK(Has(pieces, "\"none\"", Token::Label));
            CHECK(Has(pieces, "null", Token::Keyword));
            CHECK(!Covered(pieces, "{"));
            CHECK(!Covered(pieces, ":"));

            pieces = Pieces(Language::Json, "{\"a\\\"b\": [\"x\", 2]} // note\n/* one\ntwo */ [3]");
            CHECK(Has(pieces, "\"a\\\"b\"", Token::Label));
            CHECK(Has(pieces, "\"x\"", Token::String));
            CHECK(Has(pieces, "2", Token::Number));
            CHECK(Has(pieces, "// note", Token::Comment));
            CHECK(Has(pieces, "/* one", Token::Comment));
            CHECK(Has(pieces, "two */", Token::Comment));
            CHECK(Has(pieces, "3", Token::Number));
        }

        void JsonFormat()
        {
            const Text::JsonFormatted pretty = Text::FormatJson("{\"a\":[1,2,{\"b\":\"x y\"}],\"c\":{},\"d\":[ ],\"e\":-0.5E+3}", 2);
            CHECK(!pretty.Problem);
            CHECK_EQ(pretty.Text, std::string("{\n  \"a\": [\n    1,\n    2,\n    {\n      \"b\": \"x y\"\n    }\n  ],\n  \"c\": {},\n  \"d\": [],\n  \"e\": -0.5E+3\n}"));
            CHECK_EQ(Text::FormatJson(pretty.Text, 0).Text, std::string("{\"a\":[1,2,{\"b\":\"x y\"}],\"c\":{},\"d\":[],\"e\":-0.5E+3}"));
            CHECK_EQ(Text::FormatJson("[1, 2]\r\n", 2).Text, std::string("[\r\n  1,\r\n  2\r\n]\r\n"));
            CHECK_EQ(Text::FormatJson("\xEF\xBB\xBF{ }", 0).Text, std::string("\xEF\xBB\xBF{}"));
            CHECK_EQ(Text::FormatJson("\"\\u00e9 \\n \\/\"", 2).Text, std::string("\"\\u00e9 \\n \\/\""));
            CHECK_EQ(Text::FormatJson("[ 1.000000000000000000001 , 12345678901234567890 , \"\xC3\xA9\" ]", 0).Text,
                std::string("[1.000000000000000000001,12345678901234567890,\"\xC3\xA9\"]"));
            CHECK(!Text::CheckJson("[1,{\"a\":null,\"b\":false}]"));

            const auto problem = [](std::string_view text, int line, int column, std::string_view needle)
                {
                    const std::optional<Text::JsonProblem> found = Text::CheckJson(text);
                    CHECK_MSG(found && found->Line == line && found->Column == column && found->Message.find(needle) != std::string::npos,
                        std::string(text) + " -> " + (found ? std::to_string(found->Line) + ":" + std::to_string(found->Column) + " " + found->Message : std::string("valid")));
                    const Text::JsonFormatted formatted = Text::FormatJson(text, 2);
                    CHECK(formatted.Problem.has_value() && formatted.Text.empty());
                };
            problem("{\n  \"a\": 1,\n  \"b\" 2\n}", 3, 7, "expected \":\"");
            problem("[1, 2,]", 1, 7, "a comma before");
            problem("{\"a\": 1 \"b\": 2}", 1, 9, "expected \",\" or \"}\"");
            problem("{\"a\": 01}", 1, 8, "leading zero");
            problem("{\"a\": tru}", 1, 7, "expected a value");
            problem("{1: 2}", 1, 2, "a key in quotes");
            problem("[1] 2", 1, 5, "more text");
            problem("", 1, 1, "empty");
            problem("{\"a\": \"x}", 1, 7, "not closed");
            problem("[\"a\nb\"]", 1, 4, "not closed on its line");
            problem("// c\n[1]", 1, 1, "no comments");
            problem("[\"\\x\"]", 1, 3, "no JSON escape");
            problem("[\"\\u12G4\"]", 1, 3, "four hex digits");
            problem("[1.]", 1, 4, "decimal point");
            problem("{\"\xC3\xA9\": x}", 1, 7, "expected a value");
            problem(std::string(600, '[') + std::string(600, ']'), 1, 514, "deeper than 512");

            CHECK(Text::LooksLikeJson("  \n[1]"));
            CHECK(Text::LooksLikeJson("\xEF\xBB\xBF{}"));
            CHECK(!Text::LooksLikeJson("local x = 1"));
            CHECK(!Text::LooksLikeJson(""));
        }

        void Words()
        {
            Text::WordContext word = Text::WordAt("x = manifold.memory.re", 22);
            CHECK(word.Valid);
            CHECK_EQ(word.Prefix, std::string("re"));
            CHECK_EQ(word.Chain, std::string("manifold.memory"));
            CHECK(word.Member);
            CHECK(!word.Method);
            CHECK_EQ(word.Start, static_cast<std::size_t>(20));

            word = Text::WordAt("process:Get", 11);
            CHECK(word.Valid && word.Method && word.Chain == "process" && word.Prefix == "Get");

            word = Text::WordAt("pri", 3);
            CHECK(word.Valid && !word.Member && word.Prefix == "pri");

            CHECK(!Text::WordAt("print(\"abc", 10).Valid);
            CHECK(!Text::WordAt("x = 1 -- comm", 13).Valid);
            CHECK(!Text::WordAt("x = 12", 6).Valid);
            CHECK(!Text::WordAt("a = 'x'..ab", 11).Member);

            const std::string script = "local window = manifold.ui.window.create(\"Test\")\nlocal button = window.add(\"button\", {})\n-- window = other.thing()\nwindow.";
            CHECK_EQ(Text::AssignedCall(script, script.size(), "window"), std::string("manifold.ui.window.create"));
            CHECK_EQ(Text::AssignedCall(script, script.size(), "button"), std::string("window.add"));
            CHECK_EQ(Text::AssignedCall(script, 10, "window"), std::string());
            CHECK_EQ(Text::AssignedCall(script, script.size(), "win"), std::string());
            CHECK_EQ(Text::AssignedCall("x = a.b(1)\nx = c:d{}\n", 20, "x"), std::string("c:d"));
            CHECK_EQ(Text::AssignedCall("if x == f() then end", 20, "x"), std::string());
            CHECK_EQ(Text::AssignedCall("y.x = f()", 9, "x"), std::string());
            CHECK_EQ(Text::AssignedCall("x = 5", 5, "x"), std::string());
            CHECK_EQ(Text::AssignedCall("s = \"x = f()\"", 13, "x"), std::string());
            CHECK_EQ(Text::AssignedCall("", 0, "x"), std::string());
        }

        bool Offers(const std::vector<Asm::AsmCompletion>& items, std::string_view name)
        {
            return std::any_of(items.begin(), items.end(), [&](const Asm::AsmCompletion& item) { return item.Name == name; });
        }

        void AssemblerAssist()
        {
            const std::string script =
                "[ENABLE]\n"
                "aobscanmodule(INJECT,game.exe,48 8B 05)\n"
                "alloc(newmem,$1000,INJECT)\n"
                "label(code,return)\n"
                "registersymbol(INJECT)\n"
                "\n"
                "newmem:\n"
                "code:\n"
                "  mov rax,[rbx+10]\n"
                "  jmp return\n"
                "// label(commented)\n"
                "{$lua}\n"
                "local hidden = 1\n"
                "{$asm}\n"
                "INJECT:\n"
                "  jmp newmem\n"
                "return:\n";
            const std::vector<std::string> names = Asm::ScriptNames(script);
            for (const char* name : { "INJECT", "newmem", "code", "return" })
                CHECK_MSG(std::find(names.begin(), names.end(), name) != names.end(), name);
            CHECK(std::find(names.begin(), names.end(), "commented") == names.end());
            CHECK(std::find(names.begin(), names.end(), "hidden") == names.end());
            CHECK_EQ(names.size(), 4u);

            const std::string luaBlock = "{$lua}\nprint(1)\n{$asm}\nmov eax,1\n{$LUA}\nx";
            CHECK(Asm::InLuaBlock(luaBlock, 10) && !Asm::InLuaBlock(luaBlock, 30) && Asm::InLuaBlock(luaBlock, luaBlock.size()));
            CHECK(!Asm::InLuaBlock("mov eax,1", 9));

            Asm::AsmWord word = Asm::AsmWordAt("  mo", 4);
            CHECK(word.Place == Asm::AsmPlace::LineStart && word.Prefix == "mo" && word.Start == 2);
            word = Asm::AsmWordAt("mov rax,[rb", 11);
            CHECK(word.Place == Asm::AsmPlace::Operand && word.Command == "mov" && word.Prefix == "rb");
            word = Asm::AsmWordAt("aobscanmodule(INJECT,ga", 23);
            CHECK(word.Place == Asm::AsmPlace::Argument && word.Command == "aobscanmodule" && word.Argument == 1 && word.Prefix == "ga");
            CHECK(Asm::AsmWordAt("{$lu", 4).Place == Asm::AsmPlace::Directive);
            CHECK(Asm::AsmWordAt("[ENA", 4).Place == Asm::AsmPlace::Section);
            CHECK(Asm::AsmWordAt("mov eax,1 // co", 15).Place == Asm::AsmPlace::None);
            CHECK(Asm::AsmWordAt("newmem:", 6).Place == Asm::AsmPlace::LineStart);
            CHECK(Asm::AsmWordAt("alloc(a,1)  x", 13).Place == Asm::AsmPlace::None);

            Asm::AsmAssistInput input;
            input.Modules = { "game.exe", "kernel32.dll" };
            input.Symbols = { "playerBase", "gameTimer" };
            const auto at = [&](std::string_view text) { return Asm::CompleteAsm(text, text.size(), input); };
            const auto start = at("al");
            CHECK(Offers(start, "alloc") && Offers(start, "aobscanmodule") && !Offers(start, "rax"));
            const auto mnemonic = at("mo");
            CHECK(Offers(mnemonic, "mov") && Offers(mnemonic, "movss") && Offers(mnemonic, "movzx"));
            const auto operand = at(script + "  mov r");
            CHECK(Offers(operand, "rax") && Offers(operand, "r15") && Offers(operand, "newmem"));
            CHECK(Offers(at("mov rax,ga"), "game.exe"));
            input.Is64Bit = false;
            const auto narrow = at("mov e");
            CHECK(Offers(narrow, "eax") && !Offers(narrow, "rax") && !Offers(narrow, "r8"));
            input.Is64Bit = true;
            CHECK(Offers(at("mov rax,pl"), "playerBase") && !Offers(at("mov rax,p"), "playerBase"));
            const auto module = at("aobscanmodule(X,ga");
            CHECK(Offers(module, "game.exe") && !Offers(module, "playerBase"));
            CHECK(at("aobscanmodule(ne").empty());
            CHECK(Offers(at(script + "dealloc("), "newmem"));
            CHECK(Offers(at("{$l"), "$lua}") && Offers(at("{$l"), "$asm}"));
            CHECK(Offers(at("[EN"), "ENABLE]") && Offers(at("[EN"), "DISABLE]"));
            CHECK(at("{$lua}\nloc").empty());
            CHECK(at("mov eax,1 // re").empty());

            const Asm::AsmCommandDoc* alloc = Asm::FindAsmCommand("ALLOC");
            CHECK(alloc && alloc->Params.size() == 3 && alloc->Params[2].Optional);
            CHECK(!Asm::FindAsmCommand("mov"));
            CHECK(Asm::AsmCommands().size() >= 19);
            CHECK(Asm::AsmWordAt("mov ", 4).Place == Asm::AsmPlace::Operand);
            CHECK(Asm::AsmWordAt("  ", 2).Place == Asm::AsmPlace::LineStart);
        }

        void Calls()
        {
            const std::string memory = "local v = manifold.memory.read(x, ";
            Text::CallContext call = Text::CallAt(memory, memory.size());
            CHECK(call.Valid);
            CHECK_EQ(call.Chain, std::string("manifold.memory.read"));
            CHECK_EQ(call.Argument, 1);
            CHECK_EQ(call.Paren, static_cast<std::size_t>(30));
            CHECK(!call.Method);

            const std::string nested = "f(a, g(b, c), {1, 2}, \"x,y\", ";
            call = Text::CallAt(nested, nested.size());
            CHECK(call.Valid && call.Chain == "f");
            CHECK_EQ(call.Argument, 4);

            const std::string inner = "f(a, g(b, ";
            call = Text::CallAt(inner, inner.size());
            CHECK(call.Valid && call.Chain == "g" && call.Argument == 1);

            const std::string method = "list:Add (";
            call = Text::CallAt(method, method.size());
            CHECK(call.Valid && call.Chain == "list.Add" && call.Method && call.Argument == 0);

            const std::string definition = "local function foo(a, ";
            CHECK(!Text::CallAt(definition, definition.size()).Valid);
            const std::string keyword = "if (";
            CHECK(!Text::CallAt(keyword, keyword.size()).Valid);
            const std::string closed = "f(1) ";
            CHECK(!Text::CallAt(closed, closed.size()).Valid);
            const std::string commented = "f(1, -- (\n";
            call = Text::CallAt(commented, commented.size());
            CHECK(call.Valid && call.Chain == "f" && call.Argument == 1);
            const std::string table = "f{ ";
            CHECK(!Text::CallAt(table, table.size()).Valid);
        }

        void Chains()
        {
            const std::string text = "local v = manifold.memory.read(x) -- manifold.log";
            Text::ChainContext chain = Text::ChainAt(text, text.find("memory") + 2);
            CHECK(chain.Valid);
            CHECK_EQ(chain.Chain, std::string("manifold.memory"));
            CHECK_EQ(chain.Name, std::string("memory"));
            CHECK_EQ(chain.Start, text.find("manifold"));
            CHECK_EQ(chain.End, text.find("memory") + 6);

            chain = Text::ChainAt(text, text.find("read"));
            CHECK(chain.Valid && chain.Chain == "manifold.memory.read");
            chain = Text::ChainAt(text, text.find("manifold"));
            CHECK(chain.Valid && chain.Chain == "manifold");
            CHECK(!Text::ChainAt(text, 1).Valid);
            CHECK(!Text::ChainAt(text, text.find("(")).Valid);
            CHECK(!Text::ChainAt(text, text.rfind("log")).Valid);

            const std::string method = "process:Find(1)";
            chain = Text::ChainAt(method, 9);
            CHECK(chain.Valid && chain.Chain == "process.Find" && chain.Method && chain.Name == "Find");
            CHECK(!Text::ChainAt("x = 'manifold'", 6).Valid);
            CHECK(!Text::ChainAt("x = 12", 4).Valid);
        }

        void Functions()
        {
            const std::string text = "function a.b:c(x, y) end\nlocal f = function( z ) end\nlocal function g() end\nfunction broken(";
            const auto functions = Text::BufferFunctions(text);
            CHECK_EQ(functions.size(), static_cast<std::size_t>(3));
            if (functions.size() == 3)
            {
                CHECK_EQ(functions[0].Name, std::string("a.b:c"));
                CHECK_EQ(functions[0].Params, std::string("self, x, y"));
                CHECK_EQ(functions[0].Offset, static_cast<std::size_t>(0));
                CHECK_EQ(functions[1].Name, std::string("f"));
                CHECK_EQ(functions[1].Params, std::string("z"));
                CHECK_EQ(functions[1].Offset, text.find("function( z"));
                CHECK_EQ(functions[2].Name, std::string("g"));
                CHECK(functions[2].Params.empty());
            }

            const auto parts = Text::SplitParameters(" a, {b, c}, f(d, e), \"x,y\", ... ");
            CHECK_EQ(parts.size(), static_cast<std::size_t>(5));
            if (parts.size() == 5)
            {
                CHECK_EQ(parts[0], std::string("a"));
                CHECK_EQ(parts[1], std::string("{b, c}"));
                CHECK_EQ(parts[2], std::string("f(d, e)"));
                CHECK_EQ(parts[3], std::string("\"x,y\""));
                CHECK_EQ(parts[4], std::string("..."));
            }
            CHECK(Text::SplitParameters("  ").empty());
        }

        void Fuzzy()
        {
            CHECK(Text::FuzzyScore("readInteger", "read") > Text::FuzzyScore("readIntegerLocal", "read"));
            CHECK(Text::FuzzyScore("AutoAssemble", "AutoAss") > Text::FuzzyScore("autoAssemble", "AutoAss"));
            CHECK(Text::FuzzyScore("autoAssemble", "AutoAss") > Text::FuzzyScore("getAddressList", "gal"));
            CHECK(Text::FuzzyScore("getAddressList", "gal") >= 0);
            CHECK(Text::FuzzyScore("isReadable", "re") >= 0);
            CHECK(Text::FuzzyScore("write", "re") < 0);
            CHECK(Text::FuzzyScore("isReadable", "r") < 0);
            CHECK(Text::FuzzyScore("ab", "abc") < 0);
            CHECK_EQ(Text::FuzzyScore("anything", ""), 1);
        }

        void TypeText(Text::TextBuffer& buffer, std::string_view text)
        {
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                std::size_t length = 1;
                while (i + length < text.size() && (static_cast<unsigned char>(text[i + length]) & 0xC0) == 0x80)
                    ++length;
                buffer.Type(text.substr(i, length));
                i += length - 1;
            }
        }

        void BufferLines()
        {
            Text::TextBuffer buffer("a\tb\n\xC3\xBC\xE2\x82\xAC" "x\n");
            CHECK_EQ(buffer.LineCount(), 3);
            CHECK_EQ(buffer.LineOf(0), 0);
            CHECK_EQ(buffer.LineOf(4), 1);
            CHECK_EQ(buffer.LineOf(buffer.Size()), 2);
            CHECK_EQ(buffer.LineStart(1), static_cast<std::size_t>(4));
            CHECK_EQ(buffer.LineEnd(1), static_cast<std::size_t>(10));
            CHECK(buffer.Line(1) == "\xC3\xBC\xE2\x82\xAC" "x");
            CHECK_EQ(buffer.ColumnOf(2), 4);
            CHECK_EQ(buffer.ColumnOf(9), 2);
            CHECK_EQ(buffer.OffsetAt(1, 1), static_cast<std::size_t>(6));
            CHECK_EQ(buffer.OffsetAt(0, 2), static_cast<std::size_t>(1));
            CHECK_EQ(buffer.OffsetAt(0, 3), static_cast<std::size_t>(2));
            CHECK_EQ(buffer.OffsetAt(0, 99), static_cast<std::size_t>(3));
            CHECK_EQ(buffer.LongestLine(), 5);
            CHECK_EQ(buffer.Next(4), static_cast<std::size_t>(6));
            CHECK_EQ(buffer.Previous(9), static_cast<std::size_t>(6));
            CHECK_EQ(Text::Columns("\t\t", 4), 8);
            buffer.SetCaret(5, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(4));
        }

        void BufferMotions()
        {
            Text::TextBuffer buffer("local value = 10\n    next line");
            buffer.Move(Text::Motion::WordRight, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(5));
            buffer.Move(Text::Motion::WordRight, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(11));
            buffer.Move(Text::Motion::WordRight, true);
            CHECK_EQ(buffer.SelectedText(), std::string(" ="));
            buffer.Move(Text::Motion::Left, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(11));
            buffer.Move(Text::Motion::WordLeft, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(6));
            buffer.Move(Text::Motion::DocumentEnd, false);
            buffer.Move(Text::Motion::Home, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(21));
            buffer.Move(Text::Motion::Home, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(17));
            buffer.Move(Text::Motion::WordLeft, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(16));
            buffer.Move(Text::Motion::End, false);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(16));

            Text::TextBuffer columns("abcdef\nab\nabcdef");
            columns.SetCaret(5, false);
            columns.Move(Text::Motion::Down, false);
            CHECK_EQ(columns.Caret(), static_cast<std::size_t>(9));
            columns.Move(Text::Motion::Down, false);
            CHECK_EQ(columns.Caret(), static_cast<std::size_t>(15));
            columns.Move(Text::Motion::Up, true);
            columns.Move(Text::Motion::Up, true);
            CHECK(columns.Caret() == 5 && columns.Anchor() == 15);
            columns.Move(Text::Motion::Up, false);
            CHECK_EQ(columns.Caret(), static_cast<std::size_t>(0));
            columns.Move(Text::Motion::PageDown, false, 10);
            CHECK_EQ(columns.Caret(), columns.Size());

            Text::TextBuffer words("foo.bar(baz)  \nnext");
            words.SelectWordAt(5);
            CHECK_EQ(words.SelectedText(), std::string("bar"));
            words.SelectWordAt(3);
            CHECK_EQ(words.SelectedText(), std::string("."));
            words.SelectWordAt(13);
            CHECK_EQ(words.SelectedText(), std::string("  "));
            words.SelectLineAt(2);
            CHECK_EQ(words.SelectedText(), std::string("foo.bar(baz)  \n"));
            words.SelectAll();
            CHECK_EQ(words.SelectedText(), words.Text());
        }

        void BufferUndo()
        {
            Text::TextBuffer buffer;
            TypeText(buffer, "hello world");
            CHECK_EQ(buffer.Text(), std::string("hello world"));
            CHECK(buffer.Undo());
            CHECK_EQ(buffer.Text(), std::string("hello "));
            CHECK(buffer.Undo());
            CHECK_EQ(buffer.Text(), std::string("hello"));
            CHECK(buffer.Undo());
            CHECK(buffer.Text().empty() && !buffer.CanUndo());
            CHECK(buffer.Redo() && buffer.Redo());
            CHECK_EQ(buffer.Text(), std::string("hello "));
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(6));
            TypeText(buffer, "x");
            CHECK(!buffer.CanRedo());

            Text::TextBuffer moved;
            TypeText(moved, "ab");
            moved.SetCaret(1, false);
            TypeText(moved, "x");
            CHECK_EQ(moved.Text(), std::string("axb"));
            CHECK(moved.Undo());
            CHECK_EQ(moved.Text(), std::string("ab"));
            CHECK_EQ(moved.Caret(), static_cast<std::size_t>(1));

            Text::TextBuffer erased("abc def");
            erased.Move(Text::Motion::DocumentEnd, false);
            erased.Backspace(false);
            erased.Backspace(false);
            CHECK_EQ(erased.Text(), std::string("abc d"));
            CHECK(erased.Undo());
            CHECK_EQ(erased.Text(), std::string("abc def"));
            erased.Backspace(true);
            CHECK_EQ(erased.Text(), std::string("abc "));
            erased.SetCaret(0, false);
            erased.Delete(true);
            CHECK_EQ(erased.Text(), std::string(" "));
            erased.Delete(false);
            CHECK(erased.Text().empty());

            Text::TextBuffer selected("one two three");
            selected.Select(4, 7);
            TypeText(selected, "2");
            CHECK_EQ(selected.Text(), std::string("one 2 three"));
            CHECK(selected.Undo());
            CHECK(selected.Text() == "one two three" && selected.Selection() == std::make_pair(std::size_t(4), std::size_t(7)));

            Text::TextBuffer pasted("x");
            pasted.SetCaret(1, false);
            pasted.Paste("a\r\nb\rc");
            CHECK_EQ(pasted.Text(), std::string("xa\nb\nc"));
            CHECK(pasted.Undo() && pasted.Text() == "x");
            pasted.Replace(0, 1, "replaced");
            CHECK(pasted.Text() == "replaced" && pasted.Caret() == 8);
            CHECK(pasted.Undo() && pasted.Text() == "x");

            Text::TextBuffer reset("old");
            TypeText(reset, "!");
            reset.SetText("new");
            CHECK(!reset.CanUndo() && reset.Caret() <= 3);

            Text::TextBuffer indented("        x");
            indented.SetCaret(8, false);
            indented.Backspace(false);
            CHECK(indented.Text() == "    x" && indented.Caret() == 4);
            indented.SetCaret(2, false);
            indented.Backspace(false);
            CHECK(indented.Text() == "  x" && indented.Caret() == 0);
        }

        void BufferIndent()
        {
            Text::TextBuffer buffer("if x then");
            buffer.Move(Text::Motion::DocumentEnd, false);
            buffer.Newline();
            CHECK_EQ(buffer.Text(), std::string("if x then\n    "));
            TypeText(buffer, "foo()");
            buffer.Newline();
            CHECK_EQ(buffer.Text(), std::string("if x then\n    foo()\n    "));
            TypeText(buffer, "end");
            buffer.Newline();
            CHECK_EQ(buffer.Text(), std::string("if x then\n    foo()\nend\n"));
            CHECK_EQ(buffer.Caret(), buffer.Size());

            Text::TextBuffer braces("t = {}");
            braces.SetCaret(5, false);
            braces.Newline();
            CHECK_EQ(braces.Text(), std::string("t = {\n    \n}"));
            CHECK_EQ(braces.Caret(), static_cast<std::size_t>(10));
            CHECK(braces.Undo() && braces.Text() == "t = {}");

            Text::TextBuffer branch("if a then\n    x()\n    else");
            branch.Move(Text::Motion::DocumentEnd, false);
            branch.Type(" ");
            CHECK_EQ(branch.Text(), std::string("if a then\n    x()\nelse "));
            Text::TextBuffer named("    local endpoint");
            named.Move(Text::Motion::DocumentEnd, false);
            named.Newline();
            CHECK_EQ(named.Text(), std::string("    local endpoint\n    "));
            Text::TextBuffer strings("print(\"then\") -- do");
            strings.Move(Text::Motion::DocumentEnd, false);
            strings.Newline();
            CHECK_EQ(strings.Text(), std::string("print(\"then\") -- do\n"));
            Text::TextBuffer plain("if x then");
            plain.LuaIndent = false;
            plain.Move(Text::Motion::DocumentEnd, false);
            plain.Newline();
            CHECK_EQ(plain.Text(), std::string("if x then\n"));

            Text::TextBuffer tab("ab");
            tab.SetCaret(1, false);
            tab.Type("\t");
            CHECK(tab.Text() == "a   b" && tab.Caret() == 4);

            Text::TextBuffer lines("a\n  b\n\nc");
            lines.Select(0, lines.Size());
            lines.Indent();
            CHECK_EQ(lines.Text(), std::string("    a\n      b\n\n    c"));
            lines.Outdent();
            CHECK_EQ(lines.Text(), std::string("a\n  b\n\nc"));
            lines.Outdent();
            CHECK_EQ(lines.Text(), std::string("a\nb\n\nc"));
            CHECK(lines.Undo() && lines.Text() == "a\n  b\n\nc");

            Text::TextBuffer comment("a\n  b\n");
            comment.Select(0, comment.Size());
            comment.ToggleComment("--");
            CHECK_EQ(comment.Text(), std::string("-- a\n--   b\n"));
            comment.ToggleComment("--");
            CHECK_EQ(comment.Text(), std::string("a\n  b\n"));
            Text::TextBuffer single("  foo");
            single.SetCaret(4, false);
            single.ToggleComment("//");
            CHECK(single.Text() == "  // foo" && single.Caret() == 7);
            single.ToggleComment("//");
            CHECK(single.Text() == "  foo" && single.Caret() == 4);
        }

        void BufferLineEdits()
        {
            Text::TextBuffer buffer("one\ntwo\nthree");
            buffer.SetCaret(5, false);
            buffer.DuplicateLines();
            CHECK(buffer.Text() == "one\ntwo\ntwo\nthree" && buffer.Caret() == 9);
            buffer.MoveLines(-1);
            CHECK(buffer.Text() == "one\ntwo\ntwo\nthree" && buffer.Caret() == 5);
            buffer.MoveLines(-1);
            CHECK(buffer.Text() == "two\none\ntwo\nthree" && buffer.Caret() == 1);
            buffer.MoveLines(-1);
            CHECK_EQ(buffer.Text(), std::string("two\none\ntwo\nthree"));
            buffer.SetCaret(buffer.Size(), false);
            buffer.MoveLines(1);
            CHECK_EQ(buffer.Text(), std::string("two\none\ntwo\nthree"));
            buffer.Select(4, 8);
            buffer.MoveLines(1);
            CHECK_EQ(buffer.Text(), std::string("two\ntwo\none\nthree"));
            CHECK_EQ(buffer.SelectedText(), std::string("one\n"));
            buffer.DeleteLines();
            CHECK_EQ(buffer.Text(), std::string("two\ntwo\nthree"));
            buffer.SetCaret(buffer.Size(), false);
            buffer.DeleteLines();
            CHECK(buffer.Text() == "two\ntwo" && buffer.Caret() == 4);
            CHECK(buffer.Undo() && buffer.Text() == "two\ntwo\nthree");
            buffer.GotoLine(2);
            CHECK_EQ(buffer.Caret(), static_cast<std::size_t>(8));
        }
    }

    int RunTextTests()
    {
        const int before = Totals().Failures;
        LuaHighlight();
        AaHighlight();
        JsonHighlight();
        JsonFormat();
        Words();
        AssemblerAssist();
        Calls();
        Chains();
        Functions();
        Fuzzy();
        BufferLines();
        BufferMotions();
        BufferUndo();
        BufferIndent();
        BufferLineEdits();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("text", "Text", 50, [](const SuiteContext&) { return RunTextTests(); });
}
