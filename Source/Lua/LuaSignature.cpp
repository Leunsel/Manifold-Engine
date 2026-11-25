#include "Lua/LuaSignature.h"

#include "App/Engine.h"
#include "App/SignatureSettings.h"
#include "Core/Text.h"
#include "Disasm/Signature.h"
#include "Disasm/StaticRef.h"
#include "Lua/LuaArgs.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"

#include "lua.hpp"

#include <algorithm>
#include <initializer_list>
#include <string>

namespace Manifold::Lua
{
    namespace
    {
        std::shared_ptr<Target::Process> Opened(lua_State* state, Status& error)
        {
            const auto& process = Owner(state).Process();
            if (!process)
                error = Status::Fail(ErrorCode::NoProcess);
            else if (!process->Alive())
                error = Status::Fail(ErrorCode::ProcessExited);
            else
                return process;
            return nullptr;
        }

        bool Field(lua_State* state, int table, const char* key)
        {
            if (!lua_istable(state, table))
                return false;
            lua_getfield(state, table, key);
            if (lua_isnil(state, -1))
            {
                lua_pop(state, 1);
                return false;
            }
            return true;
        }

        void ReadBool(lua_State* state, int table, const char* key, bool& value)
        {
            if (!Field(state, table, key))
                return;
            value = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
        }

        void ReadInt(lua_State* state, int table, const char* key, int& value)
        {
            if (!Field(state, table, key))
                return;
            if (lua_isinteger(state, -1))
                value = static_cast<int>(std::clamp<lua_Integer>(lua_tointeger(state, -1), 0, 4096));
            lua_pop(state, 1);
        }

        template <class Enum>
        void ReadChoice(lua_State* state, int table, const char* key, Enum& value, std::initializer_list<const char*> names)
        {
            if (!Field(state, table, key))
                return;
            if (const char* text = lua_tostring(state, -1))
            {
                int index = 0;
                for (const char* name : names)
                {
                    if (EqualsNoCase(text, name))
                        value = static_cast<Enum>(index);
                    ++index;
                }
            }
            lua_pop(state, 1);
        }

        void ReadOptions(lua_State* state, int table, Disasm::SignatureOptions& options, Disasm::SignatureFormatting& formatting)
        {
            ReadChoice(state, table, "scope", options.Scope, { "module", "process" });
            ReadBool(state, table, "codeOnly", options.CodeOnly);
            ReadBool(state, table, "maskAddresses", options.MaskAddresses);
            ReadChoice(state, table, "branches", options.Branches, { "keep", "long", "all" });
            ReadChoice(state, table, "offsets", options.Offsets, { "keep", "long", "all" });
            ReadChoice(state, table, "immediates", options.Immediates, { "keep", "large", "all" });
            if (Field(state, table, "immediateThreshold"))
            {
                if (lua_isinteger(state, -1))
                    options.ImmediateThreshold = static_cast<std::uint64_t>(std::max<lua_Integer>(lua_tointeger(state, -1), 0));
                lua_pop(state, 1);
            }
            ReadChoice(state, table, "registers", options.Registers, { "keep", "sib", "all" });
            ReadInt(state, table, "maxBytes", options.MaxBytes);
            options.MaxBytes = std::clamp(options.MaxBytes, 1, 1024);
            ReadInt(state, table, "minBytes", options.MinBytes);
            ReadBool(state, table, "stopAtFunctionEnd", options.StopAtFunctionEnd);
            ReadBool(state, table, "offset", options.AllowOffset);
            ReadInt(state, table, "maxOffset", options.MaxOffset);
            ReadInt(state, table, "minSaving", options.MinSaving);
            ReadChoice(state, table, "wildcard", formatting.Wildcard, { "??", "?", "**", "*" });
            ReadBool(state, table, "spaces", formatting.Spaces);
            if (Field(state, table, "label"))
            {
                if (const char* text = lua_tostring(state, -1))
                    formatting.Label = text;
                lua_pop(state, 1);
            }
        }

        void SetString(lua_State* state, const char* key, const std::string& value)
        {
            lua_pushlstring(state, value.data(), value.size());
            lua_setfield(state, -2, key);
        }

        void SetInteger(lua_State* state, const char* key, std::int64_t value)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(value));
            lua_setfield(state, -2, key);
        }

        void SetBool(lua_State* state, const char* key, bool value)
        {
            lua_pushboolean(state, value ? 1 : 0);
            lua_setfield(state, -2, key);
        }

        int Make(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const auto address = ResolveAddress(state, 1, error);
            if (!address)
                return PushFailure(state, error);
            Engine& engine = Owner(state);
            Disasm::SignatureOptions options = SignatureOptionsFrom(engine.Options());
            Disasm::SignatureFormatting formatting = SignatureFormattingFrom(engine.Options());
            ReadOptions(state, 2, options, formatting);
            std::string failure;
            const auto signature = Disasm::MakeSignature(*process, engine.Disassembler(), *address, options, failure);
            if (!signature)
                return PushFailure(state, ErrorCode::SignatureFailed, failure);
            std::string mask;
            for (const Disasm::SignatureByte& byte : signature->Bytes)
                mask += byte.Wild ? '?' : 'x';
            lua_createtable(state, 0, 20);
            SetString(state, "pattern", Disasm::FormatSignature(*signature, Disasm::SignatureFormat::Aob, formatting));
            SetString(state, "mask", mask);
            SetString(state, "code", Disasm::FormatSignature(*signature, Disasm::SignatureFormat::CodeMask, formatting));
            SetString(state, "array", Disasm::FormatSignature(*signature, Disasm::SignatureFormat::ByteArray, formatting));
            SetString(state, "autoAssembler", Disasm::FormatSignature(*signature, Disasm::SignatureFormat::AutoAssembler, formatting));
            SetInteger(state, "address", static_cast<std::int64_t>(signature->Address));
            SetInteger(state, "start", static_cast<std::int64_t>(signature->Start));
            SetInteger(state, "offset", signature->Offset);
            SetInteger(state, "length", static_cast<std::int64_t>(signature->Bytes.size()));
            SetInteger(state, "wildcards", static_cast<std::int64_t>(signature->Wildcards()));
            SetInteger(state, "instructions", static_cast<std::int64_t>(signature->Lines.size()));
            if (!signature->Module.empty())
                SetString(state, "module", signature->Module);
            SetString(state, "scope", signature->Scope == Disasm::SignatureScope::Module ? "module" : "process");
            SetString(state, "uniqueIn", signature->ScopeText());
            SetBool(state, "verified", signature->Verified);
            if (signature->FunctionEnd)
                SetInteger(state, "functionEnd", static_cast<std::int64_t>(*signature->FunctionEnd));
            lua_pushnumber(state, signature->Seconds);
            lua_setfield(state, -2, "seconds");
            lua_createtable(state, static_cast<int>(signature->Bytes.size()), 0);
            for (std::size_t i = 0; i < signature->Bytes.size(); ++i)
            {
                lua_createtable(state, 0, 3);
                SetInteger(state, "value", signature->Bytes[i].Value);
                SetBool(state, "wild", signature->Bytes[i].Wild);
                SetString(state, "role", Disasm::ByteRoleName(signature->Bytes[i].Role));
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "bytes");
            return 1;
        }

        void PushParsed(lua_State* state, const Disasm::ParsedSignature& parsed)
        {
            lua_createtable(state, 0, 8);
            SetString(state, "pattern", parsed.Text);
            SetInteger(state, "length", static_cast<std::int64_t>(parsed.Pattern.Bytes.size()));
            SetInteger(state, "fixed", static_cast<std::int64_t>(parsed.Fixed));
            SetInteger(state, "wildcards", static_cast<std::int64_t>(parsed.Wildcards));
            SetInteger(state, "partial", static_cast<std::int64_t>(parsed.Partial));
            SetString(state, "source", parsed.Source);
            if (!parsed.Module.empty())
                SetString(state, "module", parsed.Module);
            lua_createtable(state, static_cast<int>(parsed.Notes.size()), 0);
            for (std::size_t i = 0; i < parsed.Notes.size(); ++i)
            {
                lua_pushlstring(state, parsed.Notes[i].data(), parsed.Notes[i].size());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "notes");
        }

        int Parse(lua_State* state)
        {
            std::string error;
            const auto parsed = Disasm::ParseSignature(luaL_checkstring(state, 1), error);
            if (!parsed)
                return PushFailure(state, ErrorCode::ScanBadPattern, error);
            PushParsed(state, *parsed);
            return 1;
        }

        int Find(lua_State* state)
        {
            Status status;
            const auto process = Opened(state, status);
            if (!process)
                return PushFailure(state, status);
            std::string error;
            const auto parsed = Disasm::ParseSignature(luaL_checkstring(state, 1), error);
            if (!parsed)
                return PushFailure(state, ErrorCode::ScanBadPattern, error);
            std::string moduleName = parsed->Module;
            if (Field(state, 2, "module"))
            {
                if (const char* text = lua_tostring(state, -1))
                    moduleName = text;
                lua_pop(state, 1);
            }
            int limit = 100;
            ReadInt(state, 2, "limit", limit);
            limit = std::clamp(limit, 1, 100000);
            Scan::PatternFilter filter;
            if (Field(state, 2, "executable"))
            {
                filter.Executable = lua_toboolean(state, -1) ? Scan::Tristate::Yes : Scan::Tristate::No;
                lua_pop(state, 1);
            }
            std::uint64_t start = 0;
            std::uint64_t stop = ~0ull;
            if (!moduleName.empty())
            {
                const auto module = process->FindModule(moduleName);
                if (!module)
                    return PushFailure(state, ErrorCode::ModuleNotFound, moduleName);
                start = module->Base;
                stop = module->Base + module->Size;
            }
            const std::vector<std::uint64_t> hits = Scan::FindPattern(*process, parsed->Pattern, start, stop, filter, static_cast<std::size_t>(limit));
            lua_createtable(state, static_cast<int>(hits.size()), 0);
            for (std::size_t i = 0; i < hits.size(); ++i)
            {
                lua_pushinteger(state, static_cast<lua_Integer>(hits[i]));
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int ResolveStaticNative(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const auto address = ResolveAddress(state, 1, error);
            if (!address)
                return PushFailure(state, error);
            Disasm::StaticOptions options;
            ReadChoice(state, 2, "kind", options.Kind, { "auto", "memory", "branch", "immediate" });
            ReadChoice(state, 2, "encoding", options.Encoding, { "auto", "relative", "absolute" });
            bool pointer = false;
            ReadBool(state, 2, "pointer", pointer);
            if (pointer)
                options.Output = Disasm::StaticOutput::Pointer;
            if (Field(state, 2, "offset"))
            {
                options.FieldOffset = static_cast<std::uint64_t>(lua_tointeger(state, -1));
                lua_pop(state, 1);
            }
            if (Field(state, 2, "length"))
            {
                options.Length = static_cast<std::uint64_t>(lua_tointeger(state, -1));
                lua_pop(state, 1);
            }
            const auto ref = Disasm::ResolveStatic(*process, *address, options);
            if (!ref)
                return PushFailure(state, ref.Error());
            lua_createtable(state, 0, 14);
            SetInteger(state, "address", static_cast<std::int64_t>(ref->Address));
            if (ref->Pointer)
                SetInteger(state, "pointer", static_cast<std::int64_t>(*ref->Pointer));
            SetInteger(state, "instruction", static_cast<std::int64_t>(ref->Instruction));
            SetInteger(state, "length", ref->Length);
            SetString(state, "text", ref->Text);
            SetString(state, "kind", std::string(Disasm::StaticKindName(ref->Kind)));
            SetString(state, "encoding", ref->Relative ? "relative" : "absolute");
            SetBool(state, "indexed", ref->Indexed);
            SetInteger(state, "fieldOffset", ref->FieldOffset);
            SetInteger(state, "fieldSize", ref->FieldSize);
            SetInteger(state, "fieldValue", ref->FieldValue);
            SetInteger(state, "operandSize", ref->OperandSize);
            lua_createtable(state, static_cast<int>(ref->Notes.size()), 0);
            for (std::size_t i = 0; i < ref->Notes.size(); ++i)
            {
                lua_pushlstring(state, ref->Notes[i].data(), ref->Notes[i].size());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "notes");
            return 1;
        }
    }

    void RegisterSignatureNatives(lua_State* state, int natives)
    {
        const int table = lua_absindex(state, natives);
        const luaL_Reg functions[] = {
            { "signatureMake", &Make },
            { "signatureParse", &Parse },
            { "signatureFind", &Find },
            { "resolveStatic", &ResolveStaticNative },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, table);
        luaL_setfuncs(state, functions, 0);
        lua_pop(state, 1);
    }
}
