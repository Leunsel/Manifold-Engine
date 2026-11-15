#include "Templates/TemplateGenerator.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Disasm/Disassembler.h"
#include "Lua/LuaEngine.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"
#include "Target/Symbols.h"
#include "Templates/TemplateEngine.h"

#include "lua.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <exception>
#include <regex>
#include <set>
#include <sstream>

namespace Manifold::Templates
{
    namespace
    {
        constexpr const char* kFailureMarker = "\x01manifold-template-failure";
        constexpr int kMaxIncludeDepth = 16;
        constexpr std::size_t kPatternLimit = 64;
        constexpr std::size_t kHitLimit = 512;
        constexpr const char* kMonoHint = "Manifold Engine does not read Mono metadata yet, so templates that need a managed method cannot be generated.";
        constexpr const char* kModuleHint = "The selected address must be inside a loaded module. Dynamically allocated memory has no module to scan in.";

        constexpr const char* kEnvironment = R"LUA(
local index = ...
local function copy(t) local c = {} for k, v in pairs(t) do c[k] = v end return c end
local env = {
    string = copy(string), math = copy(math), table = copy(table),
    ipairs = ipairs, pairs = pairs, tonumber = tonumber, tostring = tostring, select = select
}
function env.hex(value)
    local number = tonumber(value)
    if number then return string.format("%X", math.floor(number)) end
    return tostring(value or "")
end
function env.join(separator, ...)
    local parts = {}
    for i = 1, select("#", ...) do
        local value = select(i, ...)
        if value ~= nil and value ~= "" then parts[#parts + 1] = tostring(value) end
    end
    return table.concat(parts, tostring(separator or ""))
end
function env.default(value, fallback)
    if value == nil or value == "" then return fallback end
    return value
end
function env.isEmpty(value) return value == nil or value == "" end
function env.byteCount(bytes)
    local count = 0
    for _ in tostring(bytes or ""):gmatch("%S+") do count = count + 1 end
    return count
end
function env.bytesPrefix(bytes, count)
    local limit = tonumber(count) or 0
    if limit < 1 then return "" end
    local kept = {}
    for token in tostring(bytes or ""):gmatch("%S+") do
        kept[#kept + 1] = token
        if #kept >= limit then break end
    end
    return table.concat(kept, " ")
end
function env.trim(value) return type(value) == "string" and value:match("^%s*(.-)%s*$") or value end
function env._safe(value) if value == nil then return "" end return tostring(value) end
return setmetatable(env, { __index = index })
)LUA";

        struct Failure
        {
            GenerationFailure Data;
        };

        Failure Fail(ErrorCode code, std::string message, std::string hint = {}, std::string variable = {})
        {
            Failure failure;
            failure.Data.Code = code;
            failure.Data.Message = std::move(message);
            failure.Data.Hint = std::move(hint);
            failure.Data.Variable = std::move(variable);
            return failure;
        }

        Failure Cancel(std::string message)
        {
            Failure failure = Fail(ErrorCode::TemplateContext, std::move(message));
            failure.Data.Cancelled = true;
            return failure;
        }

        const std::set<std::string>& BaseRegisters()
        {
            static const std::set<std::string> registers = {
                "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
                "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
            };
            return registers;
        }

        bool IsBaseRegister(std::string_view name)
        {
            return BaseRegisters().count(Lower(name)) > 0;
        }

        std::string TrimText(std::string_view text)
        {
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            const std::size_t last = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(first, last - first + 1));
        }

        std::string Join(const std::vector<std::string>& parts, std::string_view separator)
        {
            std::string text;
            for (const std::string& part : parts)
            {
                if (!text.empty())
                    text += separator;
                text += part;
            }
            return text;
        }

        std::string Now(const char* format)
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            char text[32] = "";
            if (::localtime_s(&local, &now) == 0)
                std::strftime(text, sizeof(text), format, &local);
            return text;
        }

        bool Empty(const Value& value)
        {
            if (std::holds_alternative<std::monostate>(value))
                return true;
            const auto* text = std::get_if<std::string>(&value);
            return text && text->empty();
        }

        void PushValue(lua_State* state, const Value& value)
        {
            if (const auto* flag = std::get_if<bool>(&value))
                lua_pushboolean(state, *flag ? 1 : 0);
            else if (const auto* integer = std::get_if<std::int64_t>(&value))
                lua_pushinteger(state, static_cast<lua_Integer>(*integer));
            else if (const auto* number = std::get_if<double>(&value))
                lua_pushnumber(state, *number);
            else if (const auto* text = std::get_if<std::string>(&value))
                lua_pushlstring(state, text->data(), text->size());
            else
                lua_pushnil(state);
        }

        std::string ValueText(const Value& value)
        {
            if (const auto* text = std::get_if<std::string>(&value))
                return *text;
            if (const auto* flag = std::get_if<bool>(&value))
                return *flag ? "true" : "false";
            if (const auto* integer = std::get_if<std::int64_t>(&value))
                return std::to_string(*integer);
            if (const auto* number = std::get_if<double>(&value))
            {
                std::ostringstream stream;
                stream << *number;
                return stream.str();
            }
            return {};
        }

        bool ValueFlag(const Value& value)
        {
            if (const auto* flag = std::get_if<bool>(&value))
                return *flag;
            return !std::holds_alternative<std::monostate>(value);
        }

        std::int64_t ValueInteger(const Value& value)
        {
            if (const auto* integer = std::get_if<std::int64_t>(&value))
                return *integer;
            if (const auto* number = std::get_if<double>(&value))
                return static_cast<std::int64_t>(*number);
            return 0;
        }

        std::string ModuleLabel(const std::string& name)
        {
            const bool plain = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return Target::Symbols::IsNameChar(c); });
            return plain ? name : "\"" + name + "\"";
        }

        GenerationOptions Merge(GenerationOptions options, const MemoryOverrides& overrides)
        {
            const std::string global = NormalizeAllocationSize(Value(options.AllocationSize)).value_or("$1000");
            if (overrides.AskForInjectionAddress)
                options.AskForInjectionAddress = *overrides.AskForInjectionAddress;
            if (overrides.AskForHookName)
                options.AskForHookName = *overrides.AskForHookName;
            if (overrides.AllocationNear)
                options.AllocationNear = *overrides.AllocationNear;
            if (overrides.AppendToHookName)
                options.AppendToHookName = *overrides.AppendToHookName;
            if (overrides.DefaultHookName)
                options.DefaultHookName = *overrides.DefaultHookName;
            options.AllocationSize = overrides.AllocationSize ? NormalizeAllocationSize(Value(*overrides.AllocationSize)).value_or(global) : global;
            return options;
        }

        class Context;

        struct RenderState
        {
            Engine* Owner = nullptr;
            const Library* Templates = nullptr;
            Context* Variables = nullptr;
            lua_State* L = nullptr;
            int Env = LUA_NOREF;
            bool AllowGlobals = false;
            std::map<std::string, CompiledTemplate> Chunks;
            std::set<std::string> Stack;
            int Depth = 0;
            int Counter = 0;
            std::optional<GenerationFailure> Pending;
            std::set<std::string> WarnedUnknown;
            std::vector<std::string>* Warnings = nullptr;

            Failure Translate(const std::string& message, ErrorCode code, const char* prefix, const std::string& source) const
            {
                static const std::regex location(R"((tpl#\d+):(\d+):\s*([\s\S]*))");
                std::smatch match;
                Failure failure = Fail(code, prefix + message);
                failure.Data.Source = source;
                if (std::regex_search(message, match, location))
                {
                    failure.Data.Message = prefix + match[3].str();
                    const auto chunk = Chunks.find(match[1].str());
                    if (chunk != Chunks.end())
                    {
                        failure.Data.Source = chunk->second.Source;
                        failure.Data.Line = chunk->second.TemplateLine(std::stoi(match[2].str()));
                    }
                }
                return failure;
            }

            std::string Load(const SourceText& source)
            {
                const ParseResult parsed = Parse(source.Text);
                if (!parsed.Ok())
                {
                    Failure failure = Fail(ErrorCode::TemplateSyntax, parsed.Error);
                    failure.Data.Source = source.Name;
                    failure.Data.Line = parsed.ErrorLine;
                    throw failure;
                }
                const std::string chunkName = "tpl#" + std::to_string(++Counter);
                Chunks[chunkName] = Compile(parsed.Nodes, source.Name);
                const CompiledTemplate& compiled = Chunks[chunkName];
                const std::string label = "=" + chunkName;
                if (luaL_loadbufferx(L, compiled.Chunk.data(), compiled.Chunk.size(), label.c_str(), "t") != LUA_OK)
                {
                    const std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
                    lua_pop(L, 1);
                    throw Translate(message, ErrorCode::TemplateSyntax, "Template syntax error: ", source.Name);
                }
                return chunkName;
            }

            void Validate(const SourceText& source)
            {
                Load(source);
                lua_pop(L, 1);
            }

            std::string Render(const SourceText& source)
            {
                Load(source);
                lua_rawgeti(L, LUA_REGISTRYINDEX, Env);
                if (lua_pcall(L, 1, 1, 0) != LUA_OK)
                {
                    const std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
                    lua_pop(L, 1);
                    if (message == kFailureMarker && Pending)
                    {
                        Failure failure{ *Pending };
                        Pending.reset();
                        if (failure.Data.Source.empty())
                            failure.Data.Source = source.Name;
                        throw failure;
                    }
                    throw Translate(message, ErrorCode::TemplateRender, "Template execution error: ", source.Name);
                }
                std::size_t size = 0;
                const char* text = lua_tolstring(L, -1, &size);
                if (!text)
                {
                    lua_pop(L, 1);
                    Failure failure = Fail(ErrorCode::TemplateRender, "Template did not produce text");
                    failure.Data.Source = source.Name;
                    throw failure;
                }
                std::string result(text, size);
                lua_pop(L, 1);
                return result;
            }

            void Warn(std::string text)
            {
                if (Warnings)
                    Warnings->push_back(std::move(text));
            }
        };

        class Context
        {
        public:
            Context(Engine& engine, const GenerationRequest& request, GenerationOptions options, RenderState& render)
                : m_engine(engine), m_request(request), m_options(std::move(options)), m_render(render)
            {
            }

            bool Resolve(const std::string& name, Value& value);
            void Downgrade(const std::string& name) { m_memo[name] = std::monostate{}; }

            std::string Text(const char* name) { Value value; Resolve(name, value); return ValueText(value); }
            std::int64_t Integer(const char* name) { Value value; Resolve(name, value); return ValueInteger(value); }
            bool Flag(const char* name) { Value value; Resolve(name, value); return ValueFlag(value); }

            const Target::Process& Process()
            {
                if (!m_engine.HasProcess())
                    throw Fail(ErrorCode::TemplateContext, "No target process is attached", "Open a process first.");
                return *m_engine.Process();
            }

            Engine& Owner() { return m_engine; }
            const GenerationOptions& Options() const { return m_options; }
            const GenerationRequest& Request() const { return m_request; }
            RenderState& Render() { return m_render; }

            std::string NameOf(std::uint64_t address)
            {
                if (const auto module = Process().ModuleAt(address))
                {
                    const std::string label = ModuleLabel(module->Name);
                    return address == module->Base ? label : label + "+" + Hex(address - module->Base);
                }
                return Hex(address);
            }

            std::string FormatAddress(std::uint64_t address)
            {
                return Hex(address, Flag("IsTarget64Bit") ? 16 : 8);
            }

            Disasm::Instruction Instruction(std::uint64_t address)
            {
                const Disasm::Instruction instruction = m_engine.Disassembler().Read(Process(), address, &m_engine.Symbols());
                if (!instruction.Valid || instruction.Length == 0)
                    throw Fail(ErrorCode::TemplateContext, "Unable to disassemble the instruction at " + Hex(address));
                return instruction;
            }

            std::vector<Disasm::Instruction> Span()
            {
                std::vector<Disasm::Instruction> span;
                const std::int64_t required = Integer("JumpSize");
                std::uint64_t current = static_cast<std::uint64_t>(Integer("AddressValue"));
                std::int64_t total = 0;
                while (total < required)
                {
                    span.push_back(Instruction(current));
                    total += span.back().Length;
                    current += span.back().Length;
                }
                return span;
            }

            std::pair<std::string, std::uint64_t> InjectionTarget()
            {
                if (m_target)
                    return *m_target;
                const Target::Process& process = Process();
                (void)process;
                std::uint64_t address = m_request.Address;
                std::string requested = address ? NameOf(address) : std::string();
                if (m_options.AskForInjectionAddress)
                {
                    const auto answer = Ask("Injection Address", "Address to use for the injection:", requested);
                    if (!answer)
                        throw Cancel("Injection address prompt was cancelled");
                    requested = TrimText(*answer);
                    if (requested.empty())
                        throw Fail(ErrorCode::TemplateContext, "No injection address was provided", "Select an instruction in the memory viewer, or enter an address.");
                    Status error;
                    const auto evaluated = m_engine.Evaluate(requested, &error);
                    if (!evaluated)
                        throw Fail(ErrorCode::TemplateContext, "Unable to resolve injection address '" + requested + "'", error.Text);
                    address = *evaluated;
                }
                if (!address)
                    throw Fail(ErrorCode::TemplateContext, "No injection address was provided", "Select an instruction in the memory viewer, or enter an address.");
                m_target = std::make_pair(NameOf(address), address);
                return *m_target;
            }

            std::pair<std::string, std::string> HookNames()
            {
                if (m_hookNames)
                    return *m_hookNames;
                std::string requested = m_options.DefaultHookName;
                if (m_options.AskForHookName)
                {
                    const auto answer = Ask("Hook Name", "Name for the generated hook:", m_options.DefaultHookName.empty() ? "Injection" : m_options.DefaultHookName);
                    if (!answer)
                        throw Cancel("Hook name prompt was cancelled");
                    requested = *answer;
                }
                requested = TrimText(requested);
                if (requested.empty())
                    throw Fail(ErrorCode::TemplateContext, "No hook name was provided");
                std::string name = requested;
                for (char& c : name)
                {
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                        c = '_';
                }
                if (std::isdigit(static_cast<unsigned char>(name.front())))
                    name = "_" + name;
                if (name != requested)
                    m_render.Warn("Hook name was normalized to a valid Auto Assembler symbol: " + name);
                std::string parsed = name;
                const std::string& append = m_options.AppendToHookName;
                if (!append.empty() && !(parsed.size() >= append.size() && parsed.compare(parsed.size() - append.size(), append.size(), append) == 0))
                    parsed += append;
                m_hookNames = std::make_pair(name, parsed);
                return *m_hookNames;
            }

            std::pair<std::string, std::string> Aob()
            {
                if (m_aob)
                    return *m_aob;
                const std::uint64_t address = static_cast<std::uint64_t>(Integer("AddressValue"));
                Text("Module");
                const auto module = Process().ModuleAt(address);
                if (!module)
                    throw Fail(ErrorCode::TemplateContext, "Injection address is not inside a loaded module", kModuleHint, "Module");
                const std::size_t minimum = static_cast<std::size_t>(std::max<std::int64_t>(1, Integer("JumpSize")));
                std::uint64_t start = address;
                for (int attempt = 0; attempt <= 8 && start >= module->Base; ++attempt)
                {
                    std::vector<std::uint8_t> pattern;
                    if (UniquePattern(start, static_cast<std::size_t>(address - start) + minimum, *module, pattern))
                    {
                        const std::uint64_t back = address - start;
                        m_aob = std::make_pair(HexBytes(pattern), back ? "+" + Hex(back) : std::string());
                        return *m_aob;
                    }
                    const std::uint64_t previous = m_engine.Disassembler().BackUp(Process(), start, 1);
                    if (previous >= start)
                        break;
                    start = previous;
                }
                throw Fail(ErrorCode::TemplateContext, "No unique AoB was found for the injection address",
                    "Manifold Engine could not build a unique signature at this location. Pick a nearby instruction with a more distinctive byte pattern.");
            }

            std::string BaseRegister()
            {
                std::string instruction = Text("OriginalInstruction");
                if (const auto parsed = Operand(instruction))
                    return parsed->first;
                std::string suggestion;
                static const std::regex token("[A-Za-z][A-Za-z0-9]*");
                for (auto it = std::sregex_iterator(instruction.begin(), instruction.end(), token); it != std::sregex_iterator(); ++it)
                {
                    if (IsBaseRegister(it->str()))
                    {
                        suggestion = it->str();
                        break;
                    }
                }
                std::string message = "No memory operand such as [rax] or [rbx+30] was found in:\n\n" + instruction + "\n\nEnter the register that holds the base address at this point.";
                while (true)
                {
                    const auto answer = Ask("Base Address Register", message, suggestion);
                    if (!answer)
                        throw Cancel("Base address register prompt was cancelled");
                    const std::string chosen = TrimText(*answer);
                    if (IsBaseRegister(chosen))
                        return chosen;
                    if (!m_request.Ask.Ask)
                        throw Fail(ErrorCode::TemplateContext, "No base register could be determined for '" + instruction + "'");
                    std::vector<std::string> names(BaseRegisters().begin(), BaseRegisters().end());
                    message = "'" + chosen + "' is not a base register.\n\nPick one of:\n\n" + Join(names, ", ");
                }
            }

            static std::optional<std::pair<std::string, std::string>> Operand(const std::string& instruction)
            {
                static const std::regex bracket(R"(\[([^\]]*)\])");
                static const std::regex withOffset(R"(^\s*([A-Za-z][A-Za-z0-9]*)\s*([+-])\s*([0-9A-Fa-f]+)\s*$)");
                static const std::regex plain(R"(^\s*([A-Za-z][A-Za-z0-9]*)\s*$)");
                std::smatch match;
                if (!std::regex_search(instruction, match, bracket))
                    return std::nullopt;
                const std::string operand = match[1].str();
                std::smatch parts;
                if (std::regex_match(operand, parts, withOffset))
                {
                    if (!IsBaseRegister(parts[1].str()))
                        return std::nullopt;
                    return std::make_pair(parts[1].str(), (parts[2].str() == "-" ? "-" : "") + parts[3].str());
                }
                if (std::regex_match(operand, parts, plain))
                {
                    if (!IsBaseRegister(parts[1].str()))
                        return std::nullopt;
                    return std::make_pair(parts[1].str(), std::string("0"));
                }
                return std::nullopt;
            }

            std::string InjectionInfo()
            {
                const int lines = std::max(0, m_options.InjectionInfoLines);
                std::uint64_t current = static_cast<std::uint64_t>(Integer("AddressValue"));
                if (lines / 2 > 0)
                    current = m_engine.Disassembler().BackUp(Process(), current, lines / 2);
                std::vector<std::string> out;
                for (int index = 0; index < lines; ++index)
                {
                    const Disasm::Instruction instruction = m_engine.Disassembler().Read(Process(), current, &m_engine.Symbols());
                    if (!instruction.Valid || instruction.Length == 0)
                        break;
                    const std::string bytes = HexBytes(std::span<const std::uint8_t>(instruction.Bytes.data(), instruction.Length), m_options.InjectionInfoRemoveSpaces ? "" : " ");
                    std::string line = NameOf(current) + " - " + bytes + " - " + instruction.Text();
                    if (m_options.InjectionInfoAddTabs)
                        line = "\t  " + line;
                    out.push_back(std::move(line));
                    current += instruction.Length;
                }
                return Join(out, "\n");
            }

            std::string CodeContext()
            {
                const std::uint64_t address = static_cast<std::uint64_t>(Integer("AddressValue"));
                const std::uint64_t done = address + static_cast<std::uint64_t>(Integer("JumpSize"));
                const auto& disassembler = m_engine.Disassembler();
                const std::uint64_t start = disassembler.BackUp(Process(), address, 8);
                const int count = 16 + static_cast<int>(Span().size());
                std::vector<std::string> out;
                for (const Disasm::Instruction& instruction : disassembler.ReadMany(Process(), start, count, &m_engine.Symbols()))
                {
                    if (instruction.Address == address)
                        out.push_back("// ---------- INJECTING HERE ----------");
                    if (instruction.Address == done)
                        out.push_back("// ---------- DONE INJECTING  ----------");
                    std::string line = NameOf(instruction.Address) + ": " + instruction.BytesText();
                    line.resize(std::max<std::size_t>(line.size(), 44), ' ');
                    out.push_back(line + "- " + instruction.Text());
                }
                return Join(out, "\n");
            }

            bool Live() const
            {
                return m_engine.HasProcess() && m_request.Address != 0;
            }

            bool Sample(const std::string& name, Value& value)
            {
                struct Line
                {
                    const char* Address;
                    const char* Bytes;
                    const char* Text;
                };
                static const Line kLines[] = {
                    { "Game.exe+1255B50", "40 53", "push rbx" },
                    { "Game.exe+1255B52", "48 83 EC 20", "sub rsp,20" },
                    { "Game.exe+1255B56", "48 8B D9", "mov rbx,rcx" },
                    { "Game.exe+1255B59", "90 90", "nop 2" },
                    { "Game.exe+1255B5B", "48 8B 81 08 01 00 00", "mov rax,[rcx+00000108]" },
                    { "Game.exe+1255B62", "F3 0F 10 40 10", "movss xmm0,[rax+10]" },
                    { "Game.exe+1255B67", "48 83 C4 28", "add rsp,28" },
                    { "Game.exe+1255B6B", "C3", "ret" },
                };
                constexpr std::size_t kAt = 4;
                const bool wide = m_request.Use14ByteJump;
                const std::size_t spanCount = wide ? 3 : 1;
                if (name == "Process" || name == "Module")
                    value = std::string("Game.exe");
                else if (name == "ProcessBase" || name == "ModuleBase")
                    value = std::string("0000000140000000");
                else if (name == "IsTarget64Bit")
                    value = true;
                else if (name == "Address")
                    value = std::string("Game.exe+1255B5B");
                else if (name == "AddressValue")
                    value = std::int64_t(0x141255B5B);
                else if (name == "JumpSize")
                    value = std::int64_t(wide ? 16 : 7);
                else if (name == "SelectionSize")
                    value = std::int64_t(7);
                else if (name == "OriginalInstruction")
                    value = std::string(kLines[kAt].Text);
                else if (name == "OriginalOpcodes" || name == "OriginalBytes")
                {
                    std::vector<std::string> parts;
                    for (std::size_t index = kAt; index < kAt + spanCount; ++index)
                        parts.push_back(name == "OriginalOpcodes" ? "  " + std::string(kLines[index].Text) : std::string(kLines[index].Bytes));
                    value = Join(parts, name == "OriginalOpcodes" ? "\n" : " ");
                }
                else if (name == "AoBStr")
                    value = std::string("48 8B 81 08 01 00 00 F3 0F 10 40 10");
                else if (name == "AoBOffset")
                    value = std::string();
                else if (name == "InjectionInfo")
                {
                    const int lines = std::clamp(m_options.InjectionInfoLines, 0, static_cast<int>(std::size(kLines)));
                    const std::size_t first = kAt >= static_cast<std::size_t>(lines / 2) ? kAt - static_cast<std::size_t>(lines / 2) : 0;
                    std::vector<std::string> out;
                    for (std::size_t index = first; index < std::size(kLines) && out.size() < static_cast<std::size_t>(lines); ++index)
                    {
                        std::string bytes = kLines[index].Bytes;
                        if (m_options.InjectionInfoRemoveSpaces)
                            bytes.erase(std::remove(bytes.begin(), bytes.end(), ' '), bytes.end());
                        std::string line = std::string(kLines[index].Address) + " - " + bytes + " - " + kLines[index].Text;
                        if (m_options.InjectionInfoAddTabs)
                            line = "\t  " + line;
                        out.push_back(std::move(line));
                    }
                    value = Join(out, "\n");
                }
                else if (name == "CodeContext")
                {
                    std::vector<std::string> out;
                    for (std::size_t index = 0; index < std::size(kLines); ++index)
                    {
                        if (index == kAt)
                            out.push_back("// ---------- INJECTING HERE ----------");
                        if (index == kAt + spanCount)
                            out.push_back("// ---------- DONE INJECTING  ----------");
                        std::string line = std::string(kLines[index].Address) + ": " + kLines[index].Bytes;
                        line.resize(std::max<std::size_t>(line.size(), 44), ' ');
                        out.push_back(line + "- " + kLines[index].Text);
                    }
                    value = Join(out, "\n");
                }
                else
                    return false;
                return true;
            }

            bool CommandRegistered(const char* command)
            {
                return m_engine.Lua().IsAutoAssemblerCommand(command);
            }

            bool GlobalTable(const char* name)
            {
                lua_State* state = m_render.L;
                const bool present = lua_getglobal(state, name) == LUA_TTABLE;
                lua_pop(state, 1);
                return present;
            }

            std::string Header()
            {
                const auto source = m_render.Templates->Header();
                if (!source)
                {
                    m_render.Warn("Header template not found. Generation continues without it.");
                    return {};
                }
                return m_render.Render(*source);
            }

        private:
            std::optional<std::string> Ask(const std::string& title, const std::string& message, const std::string& value)
            {
                if (!m_request.Ask.Ask)
                    return value;
                return m_request.Ask.Ask(title, message, value);
            }

            bool UniquePattern(std::uint64_t start, std::size_t minimum, const Target::ModuleInfo& module, std::vector<std::uint8_t>& pattern)
            {
                const Target::Process& process = Process();
                std::vector<std::uint8_t> target(kPatternLimit);
                const std::uint64_t moduleEnd = module.Base + module.Size;
                const std::size_t readable = static_cast<std::size_t>(std::min<std::uint64_t>(kPatternLimit, moduleEnd - start));
                target.resize(process.Read(start, target.data(), readable));
                if (target.size() < minimum)
                    return false;
                for (const std::size_t seed : { std::size_t(8), std::size_t(16), std::size_t(32), kPatternLimit })
                {
                    const std::size_t seedLength = std::min(target.size(), std::max(seed, minimum));
                    Scan::Pattern search;
                    search.Bytes.assign(target.begin(), target.begin() + static_cast<std::ptrdiff_t>(seedLength));
                    search.Mask.assign(seedLength, 0xFF);
                    const std::vector<std::uint64_t> hits = Scan::FindPattern(process, search, module.Base, moduleEnd, false, kHitLimit);
                    if (hits.size() >= kHitLimit && seedLength < target.size())
                        continue;
                    if (hits.size() >= kHitLimit || std::find(hits.begin(), hits.end(), start) == hits.end())
                        return false;
                    std::size_t longest = 0;
                    std::vector<std::uint8_t> other(target.size());
                    for (const std::uint64_t hit : hits)
                    {
                        if (hit == start)
                            continue;
                        const std::size_t got = process.Read(hit, other.data(), other.size());
                        std::size_t common = 0;
                        while (common < got && common < target.size() && other[common] == target[common])
                            ++common;
                        longest = std::max(longest, common);
                    }
                    const std::size_t length = std::max({ minimum, seedLength, longest + 1 });
                    if (length > target.size())
                        return false;
                    pattern.assign(target.begin(), target.begin() + static_cast<std::ptrdiff_t>(length));
                    return true;
                }
                return false;
            }

            Engine& m_engine;
            const GenerationRequest& m_request;
            GenerationOptions m_options;
            RenderState& m_render;
            std::map<std::string, Value> m_memo;
            std::set<std::string> m_resolving;
            std::optional<std::pair<std::string, std::uint64_t>> m_target;
            std::optional<std::pair<std::string, std::string>> m_hookNames;
            std::optional<std::pair<std::string, std::string>> m_aob;
        };

        using Resolver = Value (*)(Context&);

        struct Variable
        {
            const char* Name;
            const char* Provider;
            const char* Description;
            const char* Hint;
            Resolver Resolve;
        };

        const std::vector<Variable>& VariableTable()
        {
            static const std::vector<Variable> table = {
                { "Version", "Runtime", "Manifold Engine version", nullptr, [](Context&) -> Value { return std::string(MANIFOLD_VERSION_TEXT); } },
                { "Date", "Runtime", "Generation date (YYYY-MM-DD)", nullptr, [](Context&) -> Value { return Now("%Y-%m-%d"); } },
                { "Time", "Runtime", "Generation time (HH:MM:SS)", nullptr, [](Context&) -> Value { return Now("%H:%M:%S"); } },
                { "DateTime", "Runtime", "Generation date and time", nullptr, [](Context&) -> Value { return Now("%Y-%m-%d %H:%M:%S"); } },
                { "MonoSupportStatus", "Runtime", "Whether managed Mono metadata can be read", nullptr, [](Context&) -> Value { return std::string(kMonoHint); } },
                { "Header", "Runtime", "Rendered Header.CEA", nullptr, [](Context& c) -> Value { return c.Header(); } },
                { "Process", "Process", "Attached process name", nullptr, [](Context& c) -> Value { return c.Process().Name(); } },
                { "ProcessBase", "Process", "Base address of the process main module, formatted", nullptr, [](Context& c) -> Value
                    {
                        const auto modules = c.Process().Modules();
                        if (modules)
                        {
                            for (const Target::ModuleInfo& module : *modules)
                            {
                                if (EqualsNoCase(module.Name, c.Process().Name()))
                                    return c.FormatAddress(module.Base);
                            }
                        }
                        return std::string("No process base found.");
                    } },
                { "IsTarget64Bit", "Process", "True when the target is a 64-bit process", nullptr, [](Context& c) -> Value { return c.Owner().HasProcess() ? c.Process().Is64Bit() : true; } },
                { "PointerType", "Process", "'dq' on x64, 'dd' on x86", nullptr, [](Context& c) -> Value { return std::string(c.Flag("IsTarget64Bit") ? "dq" : "dd"); } },
                { "PointerSize", "Process", "8 on x64, 4 on x86", nullptr, [](Context& c) -> Value { return std::int64_t(c.Flag("IsTarget64Bit") ? 8 : 4); } },
                { "DefaultPointerBytes", "Process", "Same as PointerSize", nullptr, [](Context& c) -> Value { return c.Integer("PointerSize"); } },
                { "Module", "Process", "Module containing the injection address", kModuleHint, [](Context& c) -> Value
                    {
                        const auto module = c.Process().ModuleAt(static_cast<std::uint64_t>(c.Integer("AddressValue")));
                        if (!module)
                            throw Fail(ErrorCode::TemplateContext, "Injection address is not inside a loaded module", kModuleHint, "Module");
                        return module->Name;
                    } },
                { "ModuleBase", "Process", "Base address of that module, formatted", nullptr, [](Context& c) -> Value
                    {
                        const auto module = c.Process().ModuleAt(static_cast<std::uint64_t>(c.Integer("AddressValue")));
                        return module ? c.FormatAddress(module->Base) : std::string("No module base found.");
                    } },
                { "Address", "Instruction", "Injection address as a name, e.g. Game.exe+1255B5B", nullptr, [](Context& c) -> Value { return c.InjectionTarget().first; } },
                { "AddressValue", "Instruction", "Numeric injection address", nullptr, [](Context& c) -> Value { return static_cast<std::int64_t>(c.InjectionTarget().second); } },
                { "Is14ByteJump", "Instruction", "Whether 14-byte jumps are switched on", nullptr, [](Context& c) -> Value { return c.Request().Use14ByteJump; } },
                { "MinJumpSize", "Instruction", "5, or 14 with 14-byte jumps", nullptr, [](Context& c) -> Value { return std::int64_t(c.Flag("Is14ByteJump") ? 14 : 5); } },
                { "JumpType", "Instruction", "'jmp' or 'jmp far'", nullptr, [](Context& c) -> Value { return std::string(c.Flag("Is14ByteJump") ? "jmp far" : "jmp"); } },
                { "JumpSize", "Instruction", "Bytes overwritten, whole instructions covering at least MinJumpSize", nullptr, [](Context& c) -> Value
                    {
                        const std::int64_t minimum = c.Integer("MinJumpSize");
                        std::uint64_t current = static_cast<std::uint64_t>(c.Integer("AddressValue"));
                        std::int64_t total = 0;
                        for (int count = 0; total < minimum; ++count)
                        {
                            if (count >= 64)
                                throw Fail(ErrorCode::TemplateContext, "Too many instructions while calculating the jump span");
                            const Disasm::Instruction instruction = c.Instruction(current);
                            total += instruction.Length;
                            current += instruction.Length;
                        }
                        return total;
                    } },
                { "SelectionSize", "Instruction", "Size of the first instruction", nullptr, [](Context& c) -> Value
                    {
                        const Disasm::Instruction instruction = c.Owner().Disassembler().Read(c.Process(), static_cast<std::uint64_t>(c.Integer("AddressValue")), &c.Owner().Symbols());
                        return std::int64_t(instruction.Valid ? instruction.Length : 0);
                    } },
                { "OriginalInstruction", "Instruction", "Disassembly of the first overwritten instruction", nullptr, [](Context& c) -> Value { return c.Instruction(static_cast<std::uint64_t>(c.Integer("AddressValue"))).Text(); } },
                { "OriginalOpcodes", "Instruction", "All overwritten instructions, one per line, two spaces indented", nullptr, [](Context& c) -> Value
                    {
                        std::vector<std::string> lines;
                        for (const Disasm::Instruction& instruction : c.Span())
                            lines.push_back("  " + instruction.Text());
                        return Join(lines, "\n");
                    } },
                { "OriginalBytes", "Instruction", "Overwritten bytes as hex pairs, e.g. '48 8B 41 34'", nullptr, [](Context& c) -> Value
                    {
                        std::vector<std::uint8_t> bytes;
                        for (const Disasm::Instruction& instruction : c.Span())
                            bytes.insert(bytes.end(), instruction.Bytes.begin(), instruction.Bytes.begin() + instruction.Length);
                        return HexBytes(bytes);
                    } },
                { "NopPadding", "Instruction", "'db 90 ...' line for the overwritten remainder, or ''", nullptr, [](Context& c) -> Value
                    {
                        const std::int64_t padding = c.Integer("JumpSize") - c.Integer("MinJumpSize");
                        if (padding <= 0)
                            return std::string();
                        std::string text = "db";
                        for (std::int64_t index = 0; index < padding; ++index)
                            text += " 90";
                        return text + "\n";
                    } },
                { "NopBytes", "Instruction", "'90 90 ...' covering the whole overwritten span", nullptr, [](Context& c) -> Value
                    {
                        std::vector<std::string> nops(static_cast<std::size_t>(std::max<std::int64_t>(0, c.Integer("JumpSize"))), "90");
                        return Join(nops, " ");
                    } },
                { "BaseAddressRegister", "Instruction", "Base register of the memory operand, or one entered by hand", nullptr, [](Context& c) -> Value { return c.BaseRegister(); } },
                { "BaseAddressOffset", "Instruction", "Displacement of that operand, '0' when the register was entered by hand", nullptr, [](Context& c) -> Value
                    {
                        const auto operand = Context::Operand(c.Text("OriginalInstruction"));
                        return operand ? operand->second : std::string("0");
                    } },
                { "HookName", "Hook", "Normalized hook symbol name", nullptr, [](Context& c) -> Value { return c.HookNames().first; } },
                { "HookNameParsed", "Hook", "HookName plus the configured suffix", nullptr, [](Context& c) -> Value { return c.HookNames().second; } },
                { "Alloc", "Hook", "alloc(n_<Hook>,<Size>[,<HookNameParsed>])", nullptr, [](Context& c) -> Value
                    {
                        const std::string name = c.Text("HookName");
                        if (c.Options().AllocationNear && !c.Flag("Is14ByteJump"))
                            return "alloc(n_" + name + "," + c.Options().AllocationSize + "," + c.Text("HookNameParsed") + ")";
                        return "alloc(n_" + name + "," + c.Options().AllocationSize + ")";
                    } },
                { "GlobalAlloc", "Hook", "alloc(n_<Hook>,<Size>), never near", nullptr, [](Context& c) -> Value { return "alloc(n_" + c.Text("HookName") + "," + c.Options().AllocationSize + ")"; } },
                { "AoBStr", "Hook", "Unique AoB signature for the injection address", nullptr, [](Context& c) -> Value { return c.Aob().first; } },
                { "AoBOffset", "Hook", "'+3F' offset of the address inside the signature, or ''", nullptr, [](Context& c) -> Value { return c.Aob().second; } },
                { "InjectionInfo", "Hook", "Surrounding instructions as a comment block", nullptr, [](Context& c) -> Value { return c.InjectionInfo(); } },
                { "InjInfoLineCount", "Hook", "Injection info line count", nullptr, [](Context& c) -> Value { return std::int64_t(c.Options().InjectionInfoLines); } },
                { "InjInfoRemoveSpaces", "Hook", "Whether injection info drops the spaces between bytes", nullptr, [](Context& c) -> Value { return c.Options().InjectionInfoRemoveSpaces; } },
                { "InjInfoAddTabs", "Hook", "Whether injection info lines are indented", nullptr, [](Context& c) -> Value { return c.Options().InjectionInfoAddTabs; } },
                { "AppendToHookName", "Hook", "Hook name suffix for this generation", nullptr, [](Context& c) -> Value { return c.Options().AppendToHookName; } },
                { "AskForHookName", "Hook", "Whether the hook name is asked for", nullptr, [](Context& c) -> Value { return c.Options().AskForHookName; } },
                { "AskForInjectionAddress", "Hook", "Whether the injection address is asked for", nullptr, [](Context& c) -> Value { return c.Options().AskForInjectionAddress; } },
                { "AllocationSize", "Hook", "Allocation size", nullptr, [](Context& c) -> Value { return c.Options().AllocationSize; } },
                { "AllocationNear", "Hook", "Whether allocations are placed near the hook", nullptr, [](Context& c) -> Value { return c.Options().AllocationNear; } },
                { "HasManifoldCommands", "Framework", "True when the Manifold Framework's Auto Assembler commands are loaded", nullptr, [](Context& c) -> Value
                    { return c.CommandRegistered("ManifoldScanModule") || c.GlobalTable("assemblerCommands"); } },
                { "HasManifoldTrampolines", "Framework", "True when the framework's detour support is loaded", nullptr, [](Context& c) -> Value
                    { return c.CommandRegistered("ManifoldInstallDetour") || (c.GlobalTable("assemblerCommands") && c.GlobalTable("trampolines")); } },
                { "ScanModule", "Framework", "ManifoldScanModule, or aobScanModule without the framework", nullptr, [](Context& c) -> Value
                    {
                        const std::string arguments = c.Text("HookNameParsed") + "," + c.Text("Module") + "," + c.Text("AoBStr");
                        return (c.Flag("HasManifoldCommands") ? "ManifoldScanModule(" : "aobScanModule(") + arguments + ")";
                    } },
                { "AssertBytes", "Framework", "ManifoldAssert, or assert without the framework", nullptr, [](Context& c) -> Value
                    {
                        const std::string site = c.Text("HookNameParsed") + c.Text("AoBOffset");
                        return (c.Flag("HasManifoldCommands") ? "ManifoldAssert(" : "assert(") + site + "," + c.Text("OriginalBytes") + ")";
                    } },
                { "FrameworkWarning", "Framework", "Comment block when the Manifold commands are missing", nullptr, [](Context& c) -> Value
                    {
                        if (c.Flag("HasManifoldCommands"))
                            return std::string();
                        return std::string("{\n   WARNING: this script needs the Manifold Framework, which is not loaded.\n   The Manifold* commands below will not assemble until it is.\n}");
                    } },
                { "TrampolineWarning", "Framework", "Comment block when detour support is missing", nullptr, [](Context& c) -> Value
                    {
                        if (c.Flag("HasManifoldTrampolines"))
                            return std::string();
                        const std::string missing = c.Flag("HasManifoldCommands") ? "the Manifold Framework's Trampolines module" : "the Manifold Framework";
                        return "{\n   WARNING: this script needs " + missing + ", which is not loaded.\n   ManifoldInstallDetour / ManifoldEmitOriginal / ManifoldDestroyDetour\n   below will not assemble until it is. There is no plain Cheat Engine\n   equivalent for a trampoline, use a non-trampoline template instead.\n}";
                    } },
                { "MonoAvailable", "Mono", "Whether the target runs Mono", kMonoHint, [](Context&) -> Value { return false; } },
                { "MonoIsIl2Cpp", "Mono", "Whether the target is an IL2CPP build", kMonoHint, [](Context&) -> Value { return false; } },
                { "MonoIsJitted", "Mono", "Whether the address is in JIT compiled managed code", kMonoHint, [](Context&) -> Value { return false; } },
                { "MonoNamespace", "Mono", "Namespace of the managed method", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoClass", "Mono", "Class of the managed method", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoClassFullName", "Mono", "Full class name of the managed method", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoMethod", "Mono", "Name of the managed method", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoImage", "Mono", "Image of the managed method", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoDescriptor", "Mono", "Namespace:Class:Method for FINDMONOMETHOD", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoMethodEntry", "Mono", "Entry of the compiled method", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoMethodSize", "Mono", "Size of the compiled method", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoMethodOffset", "Mono", "Offset of the address in the method", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoHookName", "Mono", "Class_Method as a hook name", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "MonoResolve", "Mono", "USEMONO and FINDMONOMETHOD statements", kMonoHint, [](Context&) -> Value { return std::monostate{}; } },
                { "CodeContext", "Manifold", "Original code around the injection point, with markers", nullptr, [](Context& c) -> Value { return c.CodeContext(); } },
            };
            return table;
        }

        const Variable* FindVariable(std::string_view name)
        {
            for (const Variable& variable : VariableTable())
            {
                if (name == variable.Name)
                    return &variable;
            }
            return nullptr;
        }

        bool Context::Resolve(const std::string& name, Value& value)
        {
            if (const auto memo = m_memo.find(name); memo != m_memo.end())
            {
                value = memo->second;
                return true;
            }
            const Variable* variable = FindVariable(name);
            if (!variable)
                return false;
            if (m_request.SampleValues && !Live() && Sample(name, value))
            {
                m_memo[name] = value;
                return true;
            }
            if (!m_resolving.insert(name).second)
                throw Fail(ErrorCode::TemplateContext, "Context variable '" + name + "' depends on itself", {}, name);
            try
            {
                value = variable->Resolve(*this);
            }
            catch (Failure& failure)
            {
                m_resolving.erase(name);
                if (failure.Data.Variable.empty())
                    failure.Data.Variable = name;
                throw;
            }
            m_resolving.erase(name);
            m_memo[name] = value;
            return true;
        }

        int IndexImpl(lua_State* state)
        {
            auto* render = static_cast<RenderState*>(lua_touserdata(state, lua_upvalueindex(1)));
            if (lua_type(state, 2) != LUA_TSTRING)
            {
                lua_pushnil(state);
                return 1;
            }
            const std::string key = lua_tostring(state, 2);
            Value value;
            bool known = false;
            try
            {
                known = render->Variables->Resolve(key, value);
            }
            catch (const Failure& failure)
            {
                render->Pending = failure.Data;
                lua_pushstring(state, kFailureMarker);
                return -1;
            }
            catch (const std::exception& error)
            {
                render->Pending = Fail(ErrorCode::TemplateContext, error.what(), {}, key).Data;
                lua_pushstring(state, kFailureMarker);
                return -1;
            }
            if (known)
            {
                PushValue(state, value);
                return 1;
            }
            if (render->AllowGlobals)
            {
                lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
                lua_pushvalue(state, 2);
                lua_rawget(state, -2);
                lua_remove(state, -2);
                return 1;
            }
            if (render->WarnedUnknown.insert(key).second)
                render->Warn("Unknown context variable '" + key + "' evaluated to nil.");
            lua_pushnil(state);
            return 1;
        }

        int Index(lua_State* state)
        {
            const int result = IndexImpl(state);
            if (result < 0)
                return lua_error(state);
            return result;
        }

        int IncludeImpl(lua_State* state)
        {
            auto* render = static_cast<RenderState*>(lua_touserdata(state, lua_upvalueindex(1)));
            if (lua_type(state, 1) != LUA_TSTRING)
            {
                render->Pending = Fail(ErrorCode::TemplateNotFound, "include() expects the name of a template").Data;
                lua_pushstring(state, kFailureMarker);
                return -1;
            }
            const std::string name = lua_tostring(state, 1);
            std::vector<std::string> searched;
            const auto source = render->Templates->Partial(name, &searched);
            if (!source)
            {
                render->Pending = Fail(ErrorCode::TemplateNotFound, "Included template '" + name + "' was not found. Looked in: " + Join(searched, ", ")).Data;
                lua_pushstring(state, kFailureMarker);
                return -1;
            }
            if (render->Stack.count(source->Name))
            {
                render->Pending = Fail(ErrorCode::TemplateIncludeCycle, "Include cycle detected: '" + name + "' is already being rendered").Data;
                lua_pushstring(state, kFailureMarker);
                return -1;
            }
            if (render->Depth >= kMaxIncludeDepth)
            {
                render->Pending = Fail(ErrorCode::TemplateIncludeCycle, "Include depth limit (" + std::to_string(kMaxIncludeDepth) + ") exceeded").Data;
                lua_pushstring(state, kFailureMarker);
                return -1;
            }
            render->Stack.insert(source->Name);
            ++render->Depth;
            std::string text;
            bool failed = false;
            try
            {
                text = render->Render(*source);
            }
            catch (const Failure& failure)
            {
                render->Pending = failure.Data;
                failed = true;
            }
            render->Stack.erase(source->Name);
            --render->Depth;
            if (failed)
            {
                lua_pushstring(state, kFailureMarker);
                return -1;
            }
            lua_pushlstring(state, text.data(), text.size());
            return 1;
        }

        int Include(lua_State* state)
        {
            const int result = IncludeImpl(state);
            if (result < 0)
                return lua_error(state);
            return result;
        }

        void BuildEnvironment(RenderState& render, const TemplateInfo& info, const GenerationRequest& request, const GenerationOptions& options)
        {
            lua_State* state = render.L;
            if (luaL_loadbuffer(state, kEnvironment, std::char_traits<char>::length(kEnvironment), "=template environment") != LUA_OK)
            {
                const std::string message = lua_tostring(state, -1) ? lua_tostring(state, -1) : "environment failed";
                lua_pop(state, 1);
                throw Fail(ErrorCode::TemplateRender, message);
            }
            lua_pushlightuserdata(state, &render);
            lua_pushcclosure(state, &Index, 1);
            if (lua_pcall(state, 1, 1, 0) != LUA_OK)
            {
                const std::string message = lua_tostring(state, -1) ? lua_tostring(state, -1) : "environment failed";
                lua_pop(state, 1);
                throw Fail(ErrorCode::TemplateRender, message);
            }
            const int env = lua_gettop(state);
            lua_pushlightuserdata(state, &render);
            lua_pushcclosure(state, &Include, 1);
            lua_setfield(state, env, "include");
            for (const InputDefinition& input : info.Inputs)
            {
                const auto given = request.Inputs.find(input.Name);
                PushValue(state, given != request.Inputs.end() ? given->second : input.Default);
                lua_setfield(state, env, input.Name.c_str());
            }
            lua_pushboolean(state, 0);
            lua_setfield(state, env, "FinalCompilation");

            std::string error;
            if (!info.SettingsText.empty() && PushSettingsTable(state, info.SettingsText, "=" + (info.SettingsName.empty() ? info.Name : info.SettingsName), error))
            {
                const int raw = lua_gettop(state);
                lua_newtable(state);
                lua_pushnil(state);
                while (lua_next(state, raw) != 0)
                {
                    lua_pushvalue(state, -2);
                    lua_insert(state, -2);
                    lua_settable(state, -4);
                }
                lua_remove(state, raw);
            }
            else
                lua_newtable(state);
            const int view = lua_gettop(state);
            auto setText = [&](const char* key, const std::string& text)
            {
                lua_pushlstring(state, text.data(), text.size());
                lua_setfield(state, view, key);
            };
            auto setNumber = [&](const char* key, const std::optional<double>& number)
            {
                if (number && *number == std::floor(*number) && std::abs(*number) < 9.0e15)
                    lua_pushinteger(state, static_cast<lua_Integer>(*number));
                else if (number)
                    lua_pushnumber(state, *number);
                else
                    lua_pushnil(state);
                lua_setfield(state, view, key);
            };
            setText("Id", info.Id);
            setText("Caption", info.Caption);
            setText("Category", info.Category);
            setText("SubMenuName", info.Category);
            setNumber("Order", info.Order);
            setNumber("MenuOrder", info.Order);
            setNumber("CategoryOrder", info.CategoryOrder);
            lua_pushinteger(state, info.SchemaVersion);
            lua_setfield(state, view, "SchemaVersion");
            lua_pushboolean(state, info.InSubMenu ? 1 : 0);
            lua_setfield(state, view, "InSubMenu");
            lua_pushboolean(state, info.AllowUnsafeGlobals ? 1 : 0);
            lua_setfield(state, view, "AllowUnsafeGlobals");
            lua_pushboolean(state, options.AskForInjectionAddress ? 1 : 0);
            lua_setfield(state, view, "AskForInjectionAddress");
            lua_pushboolean(state, options.AskForHookName ? 1 : 0);
            lua_setfield(state, view, "AskForHookName");
            lua_pushboolean(state, options.AllocationNear ? 1 : 0);
            lua_setfield(state, view, "AllocationNear");
            setText("AppendToHookName", options.AppendToHookName);
            setText("DefaultHookName", options.DefaultHookName);
            setText("AllocationSize", options.AllocationSize);
            lua_setfield(state, env, "TemplateSettings");
            render.Env = luaL_ref(state, LUA_REGISTRYINDEX);
        }
    }

    GenerationResult Generate(Engine& engine, const Library& library, const TemplateInfo& info, const GenerationRequest& request)
    {
        GenerationResult result;
        lua_State* state = engine.Lua().State();
        const int top = lua_gettop(state);
        RenderState render;
        render.Owner = &engine;
        render.Templates = &library;
        render.L = state;
        render.AllowGlobals = info.AllowUnsafeGlobals;
        render.Warnings = &result.Warnings;
        const GenerationOptions options = Merge(request.Options, info.Memory);
        Context context(engine, request, options, render);
        render.Variables = &context;
        try
        {
            if (!info.Architectures.empty() && engine.HasProcess())
            {
                const std::string target = engine.Process()->Is64Bit() ? "x64" : "x86";
                if (std::find(info.Architectures.begin(), info.Architectures.end(), target) == info.Architectures.end())
                {
                    std::vector<std::string> supported = info.Architectures;
                    std::sort(supported.begin(), supported.end());
                    throw Fail(ErrorCode::TemplateArchitecture, "This template supports " + Join(supported, "/") + ", but the attached process is " + target + ".");
                }
            }
            for (const std::string& capability : info.Capabilities)
            {
                Value available;
                if (capability == "Manifold.AssemblerCommands")
                    context.Resolve("HasManifoldCommands", available);
                else if (capability == "Manifold.Trampolines")
                    context.Resolve("HasManifoldTrampolines", available);
                else
                    continue;
                if (!ValueFlag(available))
                    result.Warnings.push_back("The template declares capability '" + capability + "', which is not available. The generated script may not assemble.");
            }
            const auto source = request.Script ? std::optional<SourceText>(SourceText{ info.SourceName.empty() ? std::string("editor") : info.SourceName, *request.Script }) : library.Script(info);
            if (!source)
                throw Fail(ErrorCode::TemplateNotFound, "Template file was not found: " + info.SourceName);
            render.Validate(*source);
            BuildEnvironment(render, info, request, options);

            std::set<std::string> provided = { "TemplateSettings", "FinalCompilation" };
            for (const InputDefinition& input : info.Inputs)
                provided.insert(input.Name);
            if (!info.Requires.empty() || info.SchemaVersion >= 2)
            {
                for (const std::string& name : info.Requires)
                {
                    if (provided.count(name))
                        continue;
                    const Variable* variable = FindVariable(name);
                    if (!variable)
                        throw Fail(ErrorCode::TemplateRequirement, "Required context variable '" + name + "' is not provided by any registered provider.", {}, name);
                    Value value;
                    context.Resolve(name, value);
                    if (Empty(value))
                        throw Fail(ErrorCode::TemplateRequirement, "Required context variable '" + name + "' could not be resolved.", variable->Hint ? variable->Hint : "", name);
                }
            }
            else
            {
                for (const char* name : { "Address", "Module", "HookName" })
                {
                    Value value;
                    context.Resolve(name, value);
                }
            }
            for (const std::string& name : info.Optional)
            {
                Value value;
                try
                {
                    context.Resolve(name, value);
                }
                catch (const Failure& failure)
                {
                    if (failure.Data.Cancelled)
                        throw;
                    result.Warnings.push_back("Optional variable '" + name + "' failed: " + failure.Data.Message);
                    context.Downgrade(name);
                }
            }
            std::string text = render.Render(*source);
            if (text.empty())
            {
                Failure failure = Fail(ErrorCode::TemplateRender, "The template rendered no text");
                failure.Data.Source = source->Name;
                throw failure;
            }
            result.Text = std::move(text);
        }
        catch (const Failure& failure)
        {
            result.Failure = failure.Data;
        }
        catch (const std::exception& error)
        {
            result.Failure = Fail(ErrorCode::TemplateRender, error.what()).Data;
        }
        if (render.Env != LUA_NOREF)
        {
            lua_rawgeti(state, LUA_REGISTRYINDEX, render.Env);
            lua_pushnil(state);
            lua_setmetatable(state, -2);
            lua_pushnil(state);
            lua_setfield(state, -2, "include");
            lua_pop(state, 1);
            luaL_unref(state, LUA_REGISTRYINDEX, render.Env);
        }
        lua_settop(state, top);
        return result;
    }

    std::string Describe(const GenerationFailure& failure, const std::string& caption)
    {
        std::string text = "Template:  " + caption;
        if (!failure.Source.empty())
            text += "\nSource:    " + failure.Source + (failure.Line > 0 ? " (line " + std::to_string(failure.Line) + ")" : "");
        if (!failure.Variable.empty())
        {
            text += "\nVariable:  " + failure.Variable;
            if (const Variable* variable = FindVariable(failure.Variable))
                text += "\nProvider:  " + std::string(variable->Provider);
        }
        text += "\nReason:    " + failure.Message;
        if (!failure.Hint.empty())
            text += "\n\n" + failure.Hint;
        return text;
    }

    std::vector<VariableInfo> Variables()
    {
        std::vector<VariableInfo> list;
        for (const Variable& variable : VariableTable())
            list.push_back({ variable.Name, variable.Provider, variable.Description });
        return list;
    }

    bool IsVariable(std::string_view name)
    {
        return FindVariable(name) != nullptr;
    }

    std::optional<Value> ParseInput(const InputDefinition& input, const std::string& text, std::string& error)
    {
        if (input.Type == "boolean")
        {
            const std::string lowered = Lower(TrimText(text));
            return lowered == "true" || lowered == "1" || lowered == "yes";
        }
        if (input.Type == "enum")
        {
            if (std::find(input.Values.begin(), input.Values.end(), text) == input.Values.end())
            {
                error = "'" + input.Caption + "' must be one of: " + Join(input.Values, ", ");
                return std::nullopt;
            }
            return text;
        }
        if (input.Type == "integer" || input.Type == "number")
        {
            const std::string trimmed = TrimText(text);
            char* end = nullptr;
            double number = std::strtod(trimmed.c_str(), &end);
            bool parsed = !trimmed.empty() && end && *end == '\0';
            if (!parsed && trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X'))
            {
                number = static_cast<double>(std::strtoull(trimmed.c_str() + 2, &end, 16));
                parsed = end && *end == '\0';
            }
            if (!parsed)
            {
                error = "'" + input.Caption + "' must be a number";
                return std::nullopt;
            }
            if (input.Type == "integer" && number != std::floor(number))
            {
                error = "'" + input.Caption + "' must be a whole number";
                return std::nullopt;
            }
            if (input.Min && number < *input.Min)
            {
                error = "'" + input.Caption + "' must be at least " + ValueText(Value(*input.Min));
                return std::nullopt;
            }
            if (input.Max && number > *input.Max)
            {
                error = "'" + input.Caption + "' must be at most " + ValueText(Value(*input.Max));
                return std::nullopt;
            }
            if (input.Type == "integer")
                return static_cast<std::int64_t>(number);
            return number;
        }
        return text;
    }

    std::string InputText(const Value& value)
    {
        return ValueText(value);
    }
}
