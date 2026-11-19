#include "TestKit.h"

#include "Lua/DocAnnotations.h"
#include "Lua/DocRegistry.h"
#include "Lua/DocText.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        using namespace Manifold::Lua;

        std::string Params(std::string_view loose)
        {
            return FormatParams(ParseLooseParams(loose));
        }

        void LooseParams()
        {
            CHECK_EQ(Params("address,bytecount, ReturnAsTable"), std::string("address, bytecount, ReturnAsTable"));
            CHECK_EQ(Params("string, local OPTIONAL"), std::string("string, local?"));
            CHECK_EQ(Params("text, type, buttons..."), std::string("text, type, buttons..."));
            CHECK_EQ(Params("x,x,x,x,..."), std::string("x, x, x, x, ..."));
            CHECK_EQ(Params("visible OPT"), std::string("visible?"));
            CHECK_EQ(Params("[size]"), std::string("size?"));
            CHECK_EQ(Params(""), std::string());

            DocSignature signature = ParseLooseParams("\"aobstring\", protectionflags OPTIONAL, alignmenttype OPTIONAL, alignmentparam HALFOPTIONAL");
            CHECK_EQ(FormatParams(signature), std::string("aobstring, protectionflags?, alignmenttype?, alignmentparam?"));
            CHECK_EQ(signature.Params.front().Type, std::string("string"));

            signature = ParseLooseParams("OPTIONAL signed:boolean, recursive:boolean=TRUE OPTIONAL, commentradius(default 10), BOOL extraregs, ignore BOOLEAN");
            CHECK_EQ(signature.Params.size(), static_cast<std::size_t>(5));
            if (signature.Params.size() == 5)
            {
                CHECK(signature.Params[0].Name == "signed" && signature.Params[0].Type == "boolean" && signature.Params[0].Optional);
                CHECK(signature.Params[1].Name == "recursive" && signature.Params[1].Type == "boolean" && signature.Params[1].Default == "TRUE" && signature.Params[1].Optional);
                CHECK(signature.Params[2].Name == "commentradius" && signature.Params[2].Default == "10" && !signature.Params[2].Optional);
                CHECK(signature.Params[3].Name == "extraregs" && signature.Params[3].Type == "boolean");
                CHECK(signature.Params[4].Name == "ignore" && signature.Params[4].Type == "boolean");
            }

            signature = ParseLooseParams("function(sender), functiontocall() OPTIONAL, function StructureListCallback()");
            CHECK_EQ(signature.Params.size(), static_cast<std::size_t>(3));
            if (signature.Params.size() == 3)
            {
                CHECK(signature.Params[0].Name == "function(sender)" && signature.Params[0].Type == "function");
                CHECK(signature.Params[1].Name == "functiontocall" && signature.Params[1].Type == "function" && signature.Params[1].Optional);
                CHECK(signature.Params[2].Name == "StructureListCallback" && signature.Params[2].Type == "function");
            }

            signature = ParseLooseParams("callmethod, timeout, address, {type=x,value=param1} or param1,{type=x,value=param2} or param2,...");
            CHECK_EQ(signature.Params.size(), static_cast<std::size_t>(6));
            if (signature.Params.size() == 6)
            {
                CHECK_EQ(signature.Params[3].Name, std::string("{type=x,value=param1} or param1"));
                CHECK(signature.Params[5].Variadic);
            }

            signature = ParseLooseParams("address1: integer; address2: integer; size: integer; method: integer");
            CHECK_EQ(FormatParams(signature), std::string("address1, address2, size, method"));
            CHECK(std::all_of(signature.Params.begin(), signature.Params.end(), [](const DocParam& param) { return param.Type == "integer"; }));
        }

        void Properties()
        {
            CeProperty property = ParseCeProperty("string - The value in stringform.");
            CHECK(property.Type == "string" && property.Text == "The value in stringform.");
            property = ParseCeProperty("'dsFocus', 'dsSelected', 'dsNormal', 'dsTransparent'");
            CHECK(property.Type == "string" && property.Choices.size() == 4 && property.Choices[1] == "dsSelected" && property.Text.empty());
            property = ParseCeProperty("Integer");
            CHECK(property.Type == "integer" && property.Text.empty());
            property = ParseCeProperty("function(sender)");
            CHECK(property.Type == "function(sender)" && property.Text.empty());
            property = ParseCeProperty("function(sender) - Called when clicked");
            CHECK(property.Type == "function(sender)" && property.Text == "Called when clicked");
            property = ParseCeProperty("Array to access each child menuitem");
            CHECK(property.Type.empty() && property.Text == "Array to access each child menuitem");
        }

        void Types()
        {
            DocSignature signature;
            CHECK(ParseFunType("fun(a: integer, b?: string, ...: any): boolean", signature));
            CHECK_EQ(FormatParams(signature), std::string("a, b?, ..."));
            CHECK_EQ(signature.Returns, std::string("boolean"));
            CHECK(signature.Params.size() == 3 && signature.Params[2].Variadic && signature.Params[0].Type == "integer");
            CHECK(!ParseFunType("function(a)", signature));

            DocParam param;
            param.Name = "kind?";
            param.Type = "\"i8\"|\"u8\"|nil";
            ApplyTypeDetails(param);
            CHECK(param.Name == "kind" && param.Optional && param.Type == "string" && param.Choices.size() == 2 && param.Choices[1] == "u8");

            param = DocParam{};
            param.Name = "options";
            param.Type = "{ depth: integer, hex?: boolean }?";
            ApplyTypeDetails(param);
            CHECK(param.Optional && param.Type == "table" && param.Keys.size() == 2 && param.Keys[1].first == "hex" && param.Keys[1].second == "boolean");

            param = DocParam{};
            param.Name = "target";
            param.Type = "\"auto\"|integer";
            ApplyTypeDetails(param);
            CHECK(param.Type == "integer|string" && param.Choices.size() == 1);

            CHECK_EQ(TypeEnd("table<string, integer> rest", 0), static_cast<std::size_t>(22));
            CHECK_EQ(TypeEnd("string | nil the text", 0), static_cast<std::size_t>(12));
            CHECK_EQ(TypeEnd("fun(x: integer): boolean # d", 0), static_cast<std::size_t>(24));
            CHECK_EQ(TypeEnd("\"a b\"|\"c\" desc", 0), static_cast<std::size_t>(9));

            CHECK_EQ(FirstSentence("Reads memory. Returns nil on failure."), std::string("Reads memory."));
            CHECK_EQ(FirstSentence("Takes e.g. a name. More."), std::string("Takes e.g. a name."));
            CHECK_EQ(FirstSentence("First paragraph\nstill first\n\nSecond"), std::string("First paragraph still first"));
            CHECK_EQ(FirstSentence("Version 1.5 is out"), std::string("Version 1.5 is out"));
            const std::string cut = FirstSentence("aaaa bbbb cccc dddd eeee", 12);
            CHECK_EQ(cut, std::string("aaaa bbbb\xE2\x80\xA6"));
        }

        constexpr const char* kAnnotated = R"lua(---@alias ReadType "i8"|"u8"
---| "f32" # float

--- ∑ Reads a pasted signature and scans the attached process for it.
---   Second line of summary.
---
--- Longer text here.
--- @param text string The signature
--- @param kind? ReadType # which kind
--- @return table|nil, string|nil # { Pattern, Addresses,
---         Count } or nil and a reason.
--- @overload fun(text: string, limit: integer): table
--- @see Find.Where
function Find:Scan(text, kind)
end

-- A plain comment.
local function helper(a, b)
end

local f = function(...) end

---@class Point : Base
---@field x number The x
---@field y? number
local Point = {}

---@deprecated use other
---@private
function M.old() end

--[[ ---@param z integer
function commented() end ]]
function after() end
x = 5
---@param values integer ...
---@vararg integer
function sum(...)
end
)lua";

        void Annotations()
        {
            const AnnotatedFile file = ParseAnnotations(kAnnotated, "@test.lua");
            CHECK_EQ(file.Functions.size(), static_cast<std::size_t>(6));
            const AnnotatedFunction* scan = file.Named("Find.Scan");
            CHECK(scan != nullptr);
            if (scan)
            {
                const DocRecord& doc = scan->Doc;
                CHECK_EQ(scan->Line, 14);
                CHECK_EQ(scan->CommentLine, 4);
                CHECK(scan->Documented);
                CHECK(file.At(14) == scan);
                CHECK(doc.Kind == DocItemKind::Method && doc.Owner == "Find" && doc.Name == "Scan" && doc.Origin == DocOrigin::Annotation);
                CHECK_EQ(doc.Summary, std::string("Reads a pasted signature and scans the attached process for it."));
                CHECK(doc.Text.find("Second line of summary.\n\nLonger text here.") != std::string::npos);
                CHECK_EQ(doc.Source, std::string("@test.lua"));
                CHECK_EQ(doc.Signatures.size(), static_cast<std::size_t>(2));
                if (doc.Signatures.size() == 2)
                {
                    const DocSignature& main = doc.Signatures[0];
                    CHECK_EQ(FormatParams(main), std::string("text, kind?"));
                    CHECK(main.Params[0].Type == "string" && main.Params[0].Text == "The signature");
                    CHECK(main.Params[1].Type == "ReadType" && main.Params[1].Text == "which kind");
                    CHECK(main.Params[1].Choices == std::vector<std::string>({ "i8", "u8", "f32" }));
                    CHECK_EQ(main.Returns, std::string("table|nil, string|nil"));
                    CHECK_EQ(main.ReturnsText, std::string("{ Pattern, Addresses, Count } or nil and a reason."));
                    CHECK_EQ(FormatParams(doc.Signatures[1]), std::string("text, limit"));
                    CHECK_EQ(doc.Signatures[1].Returns, std::string("table"));
                }
                CHECK(doc.See == std::vector<std::string>({ "Find.Where" }));
            }

            const AnnotatedFunction* helper = file.Named("helper");
            CHECK(helper && helper->Local && helper->Documented && helper->Doc.Summary == "A plain comment." && helper->Line == 18);
            if (helper)
                CHECK_EQ(FormatParams(helper->Doc.Signatures.front()), std::string("a, b"));
            const AnnotatedFunction* f = file.Named("f");
            CHECK(f && f->Local && !f->Documented && f->Doc.Signatures.front().Params.front().Variadic);
            const AnnotatedFunction* old = file.Named("M.old");
            CHECK(old && old->Doc.IsDeprecated && old->Doc.Deprecated == "use other" && old->Doc.Hidden && old->Doc.Owner == "M");
            CHECK(file.Named("commented") == nullptr);
            const AnnotatedFunction* after = file.Named("after");
            CHECK(after && !after->Documented);
            const AnnotatedFunction* sum = file.Named("sum");
            CHECK(sum != nullptr);
            if (sum)
            {
                CHECK_EQ(sum->Doc.Signatures.front().Params.size(), static_cast<std::size_t>(1));
                CHECK(sum->Doc.Signatures.front().Params.front().Variadic && sum->Doc.Signatures.front().Params.front().Type == "integer");
            }

            CHECK_EQ(file.Classes.size(), static_cast<std::size_t>(1));
            if (!file.Classes.empty())
                CHECK(file.Classes[0].Path == "Point" && file.Classes[0].Parent == "Base" && file.Classes[0].Kind == DocItemKind::Class);
            CHECK_EQ(file.Fields.size(), static_cast<std::size_t>(2));
            if (file.Fields.size() == 2)
            {
                CHECK(file.Fields[0].Path == "Point.x" && file.Fields[0].Type == "number" && file.Fields[0].Text == "The x" && file.Fields[0].OwnerIsClass);
                CHECK(file.Fields[1].Name == "y");
            }
            CHECK_EQ(file.Aliases.size(), static_cast<std::size_t>(1));

            std::string crlf = "\xEF\xBB\xBF";
            for (const char c : std::string(kAnnotated))
            {
                if (c == '\n')
                    crlf += '\r';
                crlf += c;
            }
            const AnnotatedFile windows = ParseAnnotations(crlf);
            CHECK_EQ(windows.Functions.size(), file.Functions.size());
            const AnnotatedFunction* scanned = windows.Named("Find.Scan");
            CHECK(scanned && scanned->Line == 14 && scanned->Doc.Summary == "Reads a pasted signature and scans the attached process for it.");
            if (scanned && !scanned->Doc.Signatures.empty())
                CHECK(scanned->Doc.Signatures[0].Params.size() == 2 && scanned->Doc.Signatures[0].Params[0].Text == "The signature");

            const AnnotatedFile tolerant = ParseAnnotations("-- broken @param\n---@param\n---@return\n---@class\n---@field\n---@overload fun(\nfunction g(\n");
            CHECK(tolerant.Named("g") != nullptr);
        }

        DocRecord Record(std::string path, DocItemKind kind, DocOrigin origin, std::string text, std::string params = {}, std::string returns = {})
        {
            DocRecord record;
            record.Path = std::move(path);
            record.Kind = kind;
            record.Origin = origin;
            record.Text = std::move(text);
            if (record.Callable())
            {
                DocSignature signature = ParseLooseParams(params);
                signature.Returns = std::move(returns);
                record.Signatures.push_back(std::move(signature));
            }
            return record;
        }

        void Registry()
        {
            auto base = std::make_shared<DocRegistry>();
            AddCheatEngineDocs(*base);
            CHECK(base->Size() > 1500);

            DocPtr read = base->Find("readinteger");
            CHECK(read && read->Path == "readInteger" && read->Origin == DocOrigin::CheatEngine && read->Kind == DocItemKind::Function);
            if (read)
            {
                CHECK_EQ(FormatCall(*read, 0, true), std::string("readInteger(address)"));
                CHECK_EQ(read->Summary, std::string("Reads a 32-bit integer from the specified address"));
            }
            DocPtr scan = base->Find("AOBScan");
            CHECK(scan && scan->Signatures.size() == 2 && scan->Text == "see above but here you just input one string");
            DocPtr dialog = base->Find("messageDialog");
            CHECK(dialog && dialog->Signatures.size() == 3);
            if (dialog)
                CHECK(dialog->Text.rfind("pops up", 0) == 0);

            CHECK_EQ(base->ResolveClass("TMainForm"), std::string("Form"));
            CHECK_EQ(base->ResolveClass("tcheckbox"), std::string("CheckBox"));
            CHECK_EQ(base->ResolveClass("NoSuchThing"), std::string());
            CHECK_EQ(base->ParentOf("Form"), std::string("ScrollingWinControl"));
            const auto members = base->ClassMembers("TCustomForm");
            const auto has = [&](std::string_view name, DocItemKind kind)
            {
                return std::any_of(members.begin(), members.end(), [&](const DocPtr& member) { return member->Name == name && member->Kind == kind; });
            };
            CHECK(has("show", DocItemKind::Method));
            CHECK(has("Caption", DocItemKind::Property));
            CHECK(has("Name", DocItemKind::Property));
            DocPtr value = base->Find("MemoryRecord.Value");
            CHECK(value && value->Kind == DocItemKind::Property && value->Type == "string" && value->Text == "The value in stringform." && value->OwnerIsClass);

            DocRegistry engine(base);
            const std::uint64_t generation = engine.Generation();
            engine.Add(Record("readInteger", DocItemKind::Function, DocOrigin::Script, "Overridden.", "address, signed OPTIONAL", "integer"));
            CHECK(engine.Generation() > generation);
            DocPtr overridden = engine.Find("readInteger");
            CHECK(overridden && overridden->Origin == DocOrigin::Script && FormatCall(*overridden, 0, false) == "readInteger(address, signed?)");
            engine.Add(Record("readBytes", DocItemKind::Function, DocOrigin::Reflection, "", "a, b, c"));
            CHECK(engine.Find("readBytes") && engine.Find("readBytes")->Origin == DocOrigin::CheatEngine);
            CHECK(engine.Remove("readInteger"));
            CHECK(engine.Find("readInteger") && engine.Find("readInteger")->Origin == DocOrigin::CheatEngine);
            CHECK(!engine.Remove("readInteger"));

            DocPtr first = engine.Add(Record("demo.alpha", DocItemKind::Function, DocOrigin::Manifold, "Alpha."));
            DocPtr weaker = engine.Add(Record("demo.alpha", DocItemKind::Function, DocOrigin::Script, "Weaker."));
            CHECK(weaker == first);
            engine.Add(Record("demo.beta", DocItemKind::Function, DocOrigin::Manifold, "Beta."));
            DocPtr again = engine.Add(Record("demo.alpha", DocItemKind::Function, DocOrigin::Manifold, "Alpha again."));
            CHECK(again != first && again->Order == first->Order && again->Text == "Alpha again.");
            CHECK(engine.Get(first->Id) == nullptr);
            CHECK(engine.Get(again->Id) == again);
            CHECK(engine.Get(read->Id) == read);
            const auto demo = engine.Members("demo");
            CHECK(demo.size() == 2 && demo[0]->Name == "alpha" && demo[1]->Name == "beta");
            CHECK(engine.RemoveId(again->Id));
            CHECK(engine.Members("demo").size() == 1);

            const auto found = engine.Search("readint", 5);
            CHECK(!found.empty() && found.front()->Path == "readInteger");
            const auto byText = engine.Search("stringform", 5);
            CHECK(std::any_of(byText.begin(), byText.end(), [](const DocPtr& record) { return record->Path == "MemoryRecord.Value"; }));
            const auto all = engine.All();
            CHECK_EQ(all.size(), base->Size() + 1);
        }

        void Markdown()
        {
            DocRegistry registry;
            registry.Add(Record("demo", DocItemKind::Table, DocOrigin::Manifold, "Demo root."));
            DocRecord version = Record("demo.apiVersion", DocItemKind::Constant, DocOrigin::Manifold, "Version.");
            version.Value = "4";
            registry.Add(std::move(version));
            registry.Add(Record("demo.has", DocItemKind::Function, DocOrigin::Manifold, "Tests a name.", "name", "boolean"));
            registry.Add(Record("demo.log", DocItemKind::Table, DocOrigin::Manifold, "Logging."));
            registry.Add(Record("demo.log.info", DocItemKind::Function, DocOrigin::Manifold, "Info | pipe.\nSecond line.", "message, fields OPTIONAL", "nothing"));
            DocRecord channel = Record("demo.log.channel", DocItemKind::Class, DocOrigin::Manifold, "Objects.");
            channel.Title = "Channel objects";
            registry.Add(std::move(channel));
            registry.Add(Record("demo.log.channel.debug", DocItemKind::Function, DocOrigin::Manifold, "Debug.", "message", "nothing"));
            DocRecord hidden = Record("demo.log.secret", DocItemKind::Function, DocOrigin::Manifold, "Hidden.");
            hidden.Hidden = true;
            registry.Add(std::move(hidden));
            const std::string expected =
                "## demo\n"
                "\n"
                "Demo root.\n"
                "\n"
                "| Name | Value | |\n"
                "| --- | --- | --- |\n"
                "| `demo.apiVersion` | 4 | Version. |\n"
                "| `demo.has(name)` | boolean | Tests a name. |\n"
                "\n"
                "## demo.log\n"
                "\n"
                "Logging.\n"
                "\n"
                "| Function | Returns | |\n"
                "| --- | --- | --- |\n"
                "| `demo.log.info(message, fields?)` | nothing | Info \\| pipe. Second line. |\n"
                "\n"
                "### Channel objects\n"
                "\n"
                "Objects.\n"
                "\n"
                "| Name | Returns | |\n"
                "| --- | --- | --- |\n"
                "| `channel.debug(message)` | nothing | Debug. |\n";
            const std::string actual = RenderMarkdown(registry, "demo");
            CHECK_MSG(actual == expected, actual);
            CHECK(RenderMarkdown(registry, "missing").empty());
        }
    }

    int RunDocTests()
    {
        const int before = Totals().Failures;
        LooseParams();
        Properties();
        Types();
        Annotations();
        Registry();
        Markdown();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("docs", "Documentation", 60, [](const SuiteContext&) { return RunDocTests(); });
}
