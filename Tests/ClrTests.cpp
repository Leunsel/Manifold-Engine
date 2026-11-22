#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Table/ManagedStructures.h"
#include "Target/ClrLink.h"
#include "Target/ClrMetadata.h"
#include "Target/ClrProtocol.h"
#include "Target/ClrRuntime.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

// The .NET reader: the metadata parser on the fake game's file, the helper's
// protocol, and the whole path (ManifoldClr.exe, the DAC, the bridge) against
// TestTarget hosting the real .NET Framework (64-bit and 32-bit) and .NET.
namespace Manifold::Tests
{
    namespace
    {
        // --only=<text> keeps the runtime runs whose label has the text in it.
        std::string g_only;

        std::filesystem::path Self()
        {
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            return std::filesystem::path(self);
        }

        std::filesystem::path FakeGame()
        {
            return Self().parent_path().parent_path().parent_path() / L"Managed" / L"ManifoldFakeGame.dll";
        }

        // The newest installed .NET shared runtime of a major version, or of
        // any version when major is 0.
        std::optional<std::filesystem::path> CoreRuntime(int major)
        {
            wchar_t programs[MAX_PATH] = L"";
            if (!::GetEnvironmentVariableW(L"ProgramFiles", programs, MAX_PATH))
                return std::nullopt;
            const std::filesystem::path root = std::filesystem::path(programs) / L"dotnet" / L"shared" / L"Microsoft.NETCore.App";
            std::error_code error;
            std::optional<std::filesystem::path> best;
            std::vector<int> bestVersion;
            for (const auto& entry : std::filesystem::directory_iterator(root, error))
            {
                std::vector<int> version;
                for (const std::string& part : Split(WideToUtf8(entry.path().filename().wstring()), '.'))
                    version.push_back(std::atoi(part.c_str()));
                if (version.empty() || (major && version[0] != major) || !std::filesystem::exists(entry.path() / L"mscordaccore.dll", error))
                    continue;
                if (!best || version > bestVersion)
                {
                    best = entry.path();
                    bestVersion = version;
                }
            }
            return best;
        }

        struct ClrTarget
        {
            PROCESS_INFORMATION Info{};
            std::map<std::string, std::uint64_t> Addresses;
            std::string Output;

            ~ClrTarget()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }

            bool Start(const std::filesystem::path& target, const std::wstring& arguments)
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
                std::wstring command = L"\"" + target.wstring() + L"\" " + arguments;
                const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &Info);
                ::CloseHandle(write);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                while (started && Output.find("clr ready") == std::string::npos && Output.find("clr failed") == std::string::npos
                       && std::chrono::steady_clock::now() < deadline)
                {
                    DWORD available = 0;
                    if (!::PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr))
                        break;
                    if (!available)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    char buffer[512];
                    DWORD got = 0;
                    if (!::ReadFile(read, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0)
                        break;
                    Output.append(buffer, got);
                }
                ::CloseHandle(read);
                for (const std::string& line : Split(Output, '\n'))
                {
                    const std::vector<std::string> parts = Split(Trim(line), ' ');
                    if (parts.size() == 3 && parts[0] == "clr" && parts[2].rfind("0x", 0) == 0)
                        Addresses[parts[1]] = std::strtoull(parts[2].c_str() + 2, nullptr, 16);
                }
                return started && Output.find("clr ready") != std::string::npos;
            }
        };

        const Target::MonoField* Field(const std::vector<Target::MonoField>& fields, const std::string& name)
        {
            const auto found = std::find_if(fields.begin(), fields.end(), [&](const Target::MonoField& field) { return field.Name == name; });
            return found == fields.end() ? nullptr : &*found;
        }

        void LuaCheck(Engine& engine, const std::string& name, const std::string& code)
        {
            std::string result;
            const Status status = engine.Lua().RunWithResult(code, name, result);
            CHECK_MSG(status.Ok() && result == "ok", name + ": " + (status.Ok() ? result : status.Text));
        }

        // manifold.dotnet and Cheat Engine's data collector against the fake
        // game, while its runtime is loaded.
        void CheckLua(Engine& engine, const std::string& label, std::uint64_t player, std::uint64_t inventory, std::uint64_t name, std::uint64_t pointer)
        {
            const std::string values = Format("local PLAYER, INVENTORY, NAME, P = 0x%llX, 0x%llX, 0x%llX, %llu\n", static_cast<unsigned long long>(player),
                static_cast<unsigned long long>(inventory), static_cast<unsigned long long>(name), static_cast<unsigned long long>(pointer));
            LuaCheck(engine, label + " manifold.dotnet", values + R"LUA(
                local d = manifold.dotnet
                if not d.isLoaded() then return "not loaded" end
                local s = d.status()
                if not s.loaded or not s.detected or s.classes < 200 or s.pointerSize ~= P then return "status" end
                local W = "Manifold.FakeGame.World:"
                local function eq(got, want, what) if got ~= want then error(what .. ": " .. tostring(got)) end end
                eq(d.value(W .. "Seed"), 4242, "seed")
                if math.abs(d.value(W .. "Gravity") - 9.81) > 1e-9 then return "gravity" end
                eq(d.value(W .. "Title"), "Fake World", "title")
                eq(d.value(W .. "LocalPlayer"), PLAYER, "player")
                eq(d.value(W .. "LocalPlayer", "Health"), 1234, "health")
                eq(d.value(W .. "LocalPlayer", "Name"), "Hero", "name")
                eq(d.value(W .. "LocalPlayer", "Alive"), true, "alive")
                eq(d.value(W .. "LocalPlayer", "Side"), 2, "side")
                eq(d.value(W .. "LocalPlayer", "Score"), 123456789012, "score")
                eq(d.value(W .. "LocalPlayer", "Speed"), 2.5, "speed")
                eq(d.value(W .. "LocalPlayer", "Position.Y"), -2.25, "position")
                eq(d.value(W .. "LocalPlayer", "Id"), 7, "inherited")
                eq(d.value(W .. "Players", "[0].Inventory[0].Name"), "Torch", "torch")
                eq(d.value(W .. "LocalPlayer", ".Bag._items[0].Name"), "Sword", "sword")
                eq(d.value(W .. "LocalPlayer", "Active.Time"), 600, "nested class")
                eq(d.value(W .. "Spawn", "X"), 100, "boxed static struct")
                eq(d.value(W .. "LocalPlayer", "Inventory[1]"), nil, "null element")
                eq(d.value(PLAYER, "Health"), 1234, "object base")
                local v, e = d.value(W .. "LocalPlayer", "Inventory[1].Name")
                if v ~= nil or not e or e.code ~= "ME-4105" then return "null step" end
                v, e = d.value(W .. "LocalPlayer", "Nope")
                if v ~= nil or not e or e.code ~= "ME-4105" then return "missing field" end
                v, e = d.value(W .. "LocalPlayer", "Inventory[9]")
                if v ~= nil or not e then return "index past the end" end
                v, e = d.value("Manifold.FakeGame.World:Nope")
                if v ~= nil or not e or e.code ~= "ME-4105" then return "missing static" end
                if pcall(d.value) then return "no base accepted" end
                local offset = d.fieldOffset("Manifold.FakeGame.Player", "Health")
                eq(d.address(W .. "LocalPlayer", "Health"), PLAYER + offset, "address")
                eq(d.address(PLAYER), PLAYER, "address of a base")
                eq(readInteger(d.staticAddress("Manifold.FakeGame.World", "Seed")), 4242, "static address")
                if d.fieldOffset("Manifold.FakeGame.Player", "Id") == nil then return "inherited offset" end
                eq(d.fieldOffset("Manifold.FakeGame.Player", "Nope"), nil, "missing offset")
                local c = d.class("Manifold.FakeGame.Player")
                if not c or c.parent ~= "Manifold.FakeGame.Entity" or c.token == 0 or c.module == 0 or c.image ~= "ManifoldFakeGame" then return "class" end
                eq(d.class("Nope.Nothing"), nil, "unknown class")
                local hurt
                for _, m in ipairs(c.methods) do if m.name == "Hurt" then hurt = m end end
                if not hurt or not hurt.address or hurt.params ~= 1 or hurt.static then return "methods" end
                eq(d.methodAddress("Manifold.FakeGame.Player", "Hurt"), hurt.address, "method address")
                eq(d.methodAddress("Manifold.FakeGame.Player", "NeverCalled"), nil, "not compiled")
                eq(d.methodAt(hurt.address + 1).name, "Manifold.FakeGame.Player:Hurt", "method at")
                local world = d.class("Manifold.FakeGame.World")
                local seed
                for _, f in ipairs(world.statics) do if f.name == "Seed" then seed = f end end
                if not seed or not seed.address or seed.type ~= "int" then return "statics" end
                local found = false
                for _, m in ipairs(d.modules()) do if m.handle == c.module and m.name == "ManifoldFakeGame" then found = true end end
                if not found then return "modules" end
                if #d.classes("manifold.fakegame") < 7 then return "classes filter" end
                local at = d.classAt(PLAYER + P)
                if not at or at.class ~= "Manifold.FakeGame.Player" or at.address ~= PLAYER or at.offset ~= P then return "classAt" end
                local o = d.object(INVENTORY)
                if not o or o.kind ~= "array" or o.length ~= 3 or o.elementClass ~= "Manifold.FakeGame.Item" or o.data ~= INVENTORY + 2 * P then return "object" end
                eq(d.object(PLAYER).kind, "object", "object kind")
                eq(d.object(PLAYER + P), nil, "no object")
                eq(d.text(NAME), "Hero", "text")
                eq(d.text(PLAYER), nil, "not a string")
                if #d.fields(PLAYER) < 10 then return "fields" end
                local h = d.heap()
                local players = 0
                for _, t in ipairs(h.types) do if t.name == "Manifold.FakeGame.Player" then players = t.count end end
                if players ~= 1 or h.objects < 100 then return "heap" end
                eq(#d.instances("Manifold.FakeGame.Item"), 2, "instances")
                eq(d.instances(c.handle)[1], PLAYER, "instances by handle")
                eq(#d.instances("Manifold.FakeGame.Item", 1), 1, "instances limit")
                return "ok")LUA");
            LuaCheck(engine, label + " getDotNetDataCollector", values + R"LUA(
                local d = manifold.dotnet
                local c = d.class("Manifold.FakeGame.Player")
                local dc = getDotNetDataCollector()
                local domains = dc.enumDomains()
                if #domains ~= 1 or domains[1].DomainHandle == nil then return "domains" end
                local module
                for _, m in ipairs(dc:enumModuleList(domains[1].DomainHandle)) do if m.ModuleHandle == c.module then module = m end end
                if not module or not module.Name:find("ManifoldFakeGame") then return "modules" end
                local typedef
                for _, t in ipairs(dc.enumTypeDefs(c.module)) do if t.Name == "Manifold.FakeGame.Player" then typedef = t end end
                if not typedef or typedef.TypeDefToken ~= c.token then return "typedefs" end
                local hurt
                for _, m in ipairs(dc.getTypeDefMethods(c.module, c.token)) do if m.Name == "Hurt" then hurt = m end end
                if not hurt or hurt.NativeCode == 0 then return "methods" end
                local params = dc.getMethodParameters(c.module, hurt.MethodToken)
                if #params ~= 1 then return "parameters " .. #params end
                local data = dc.getTypeDefData(c.module, c.token)
                local health
                for _, f in ipairs(data.Fields) do if f.Name == "Health" then health = f end end
                if not health or health.Offset ~= d.fieldOffset("Manifold.FakeGame.Player", "Health") then return "typedef data" end
                local at = dc.getAddressData(PLAYER + 4)
                if not at or at.StartAddress ~= PLAYER or at.ClassName ~= "Manifold.FakeGame.Player" then return "address data" end
                local array = dc.getAddressData(INVENTORY)
                if not array or array.ElementSize ~= P or array.FirstElementOffset ~= 2 * P or array.CountOffset ~= P then return "array data" end
                local parent = dc.getTypeDefParent(c.module, c.token)
                local entity = d.class("Manifold.FakeGame.Entity")
                if not parent or parent.TypedefToken ~= entity.token or parent.ModuleHandle ~= entity.module then return "parent" end
                local objects = dc.enumAllObjectsOfType(c.module, c.token)
                if #objects ~= 1 or objects[1] ~= PLAYER then return "objects of type" end
                return "ok")LUA");
        }

        const Target::MonoMethod* Method(const Target::MonoClass& info, const std::string& name)
        {
            const auto found = std::find_if(info.Methods.begin(), info.Methods.end(), [&](const Target::MonoMethod& method) { return method.Name == name; });
            return found == info.Methods.end() ? nullptr : &*found;
        }

        template <typename T>
        T Read(const Target::Process& process, std::uint64_t address)
        {
            T value{};
            process.ReadExact(address, &value, sizeof(value));
            return value;
        }

        int CountHelpers()
        {
            int count = 0;
            HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W entry{ sizeof(entry) };
            for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry))
            {
                if (_wcsicmp(entry.szExeFile, L"ManifoldClr.exe") == 0 || _wcsicmp(entry.szExeFile, L"ManifoldClr32.exe") == 0)
                {
                    // Only the helpers this test process started.
                    if (entry.th32ParentProcessID == ::GetCurrentProcessId())
                        ++count;
                }
            }
            ::CloseHandle(snapshot);
            return count;
        }

        void CheckMetadata()
        {
            std::ifstream file(FakeGame(), std::ios::binary);
            if (!file)
            {
                Skip("the fake .NET game was not built (no .NET SDK at build time)");
                return;
            }
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const auto block = Target::ClrMetadata::FromPeFile(bytes);
            CHECK(block.has_value());
            if (!block)
                return;
            Target::ClrMetadata metadata;
            CHECK(metadata.Parse(*block));
            std::map<std::string, std::uint32_t> types;
            for (std::uint32_t row = 1; row <= metadata.Rows(Target::ClrMetadata::TypeDef); ++row)
                types[metadata.TypeDefName(row)] = row;
            CHECK(types.count("Manifold.FakeGame.Player"));
            CHECK(types.count("Manifold.FakeGame.World"));
            CHECK(types.count("Manifold.FakeGame.Player+Buff"));
            const std::uint32_t player = types["Manifold.FakeGame.Player"];
            CHECK_EQ(metadata.EnclosingOf(types["Manifold.FakeGame.Player+Buff"]), player);
            const auto row = metadata.TypeDefAt(player);
            CHECK(row.has_value());
            if (!row)
                return;
            CHECK_EQ(metadata.TypeName(row->Extends), std::string("Manifold.FakeGame.Entity"));
            std::map<std::string, std::string> fieldTypes;
            for (std::uint32_t field = row->FieldFirst; field < row->FieldEnd; ++field)
            {
                const auto info = metadata.FieldAt(field);
                if (info)
                {
                    fieldTypes[info->Name] = metadata.FieldTypeName(field);
                    CHECK_EQ(metadata.FieldOwner(field), player);
                }
            }
            CHECK_EQ(fieldTypes["Health"], std::string("int"));
            CHECK_EQ(fieldTypes["Speed"], std::string("float"));
            CHECK_EQ(fieldTypes["Name"], std::string("string"));
            CHECK_EQ(fieldTypes["Inventory"], std::string("Manifold.FakeGame.Item[]"));
            CHECK_EQ(fieldTypes["Bag"], std::string("System.Collections.Generic.List<Manifold.FakeGame.Item>"));
            CHECK_EQ(fieldTypes["Side"], std::string("Manifold.FakeGame.Team"));
            CHECK_EQ(fieldTypes["Active"], std::string("Manifold.FakeGame.Player+Buff"));
            std::map<std::string, int> params;
            for (std::uint32_t method = row->MethodFirst; method < row->MethodEnd; ++method)
            {
                if (const auto info = metadata.MethodAt(method))
                {
                    params[info->Name] = metadata.ParamCount(method);
                    CHECK_EQ(metadata.MethodOwner(method), player);
                }
            }
            CHECK_EQ(params["Hurt"], 1);
            CHECK_EQ(params["NeverCalled"], 2);
            CHECK(!metadata.HasGenericParams(player));

            // Cut or scrambled metadata is refused or reads as nothing; it
            // never reads outside the buffer.
            int refused = 0;
            for (std::size_t cut = 0; cut < block->size(); cut += std::max<std::size_t>(1, block->size() / 97))
            {
                Target::ClrMetadata partial;
                if (!partial.Parse(std::span<const std::uint8_t>(block->data(), cut)))
                    ++refused;
                else
                    for (std::uint32_t r = 1; r <= partial.Rows(Target::ClrMetadata::TypeDef); ++r)
                        partial.TypeDefName(r);
            }
            CHECK(refused > 0);
            std::vector<std::uint8_t> scrambled = *block;
            std::uint32_t seed = 12345;
            for (int round = 0; round < 64; ++round)
            {
                std::vector<std::uint8_t> copy = scrambled;
                for (int flip = 0; flip < 16; ++flip)
                {
                    seed = seed * 1103515245u + 12345u;
                    copy[(seed >> 8) % copy.size()] ^= static_cast<std::uint8_t>(seed >> 24);
                }
                Target::ClrMetadata broken;
                if (broken.Parse(copy))
                {
                    for (std::uint32_t r = 1; r <= std::min<std::uint32_t>(broken.Rows(Target::ClrMetadata::TypeDef), 64); ++r)
                        broken.TypeDefName(r);
                    for (std::uint32_t r = 1; r <= std::min<std::uint32_t>(broken.Rows(Target::ClrMetadata::Field), 64); ++r)
                        broken.FieldTypeName(r);
                    for (std::uint32_t r = 1; r <= std::min<std::uint32_t>(broken.Rows(Target::ClrMetadata::MethodDef), 64); ++r)
                        broken.ParamCount(r);
                }
            }
            CHECK(!Target::ClrMetadata::FromPeFile(std::span<const std::uint8_t>(bytes.data(), 100)).has_value());
        }

        // Metadata built by hand whose names refer to themselves: a TypeSpec
        // naming itself (also from a field's signature), one whose generic
        // arguments name itself twice, and two TypeRefs nested in each
        // other. Each name fails, empty, instead of recursing until the stack
        // ends; the sound rows beside them still read.
        void CheckMetadataCycles()
        {
            using M = Target::ClrMetadata;
            const auto put16 = [](std::vector<std::uint8_t>& out, std::uint32_t value)
                {
                    out.push_back(static_cast<std::uint8_t>(value));
                    out.push_back(static_cast<std::uint8_t>(value >> 8));
                };
            const auto put32 = [&](std::vector<std::uint8_t>& out, std::uint32_t value)
                {
                    put16(out, value & 0xFFFF);
                    put16(out, value >> 16);
                };
            const auto pad = [](std::vector<std::uint8_t>& out)
                {
                    while (out.size() % 4)
                        out.push_back(0);
                };
            // "A" at 1, "B" at 3, "C" at 5, "N" at 7.
            std::vector<std::uint8_t> strings = { 0, 'A', 0, 'B', 0, 'C', 0, 'N', 0 };
            pad(strings);
            // TypeDefOrRef coded indexes: TypeSpec row 1 is 0x06, row 2 0x0A.
            std::vector<std::uint8_t> blobs = {
                0x00,
                0x02, 0x12, 0x06,                                     // 1: TypeSpec 1 = class TypeSpec 1
                0x03, 0x06, 0x12, 0x06,                               // 4: field of class TypeSpec 1
                0x08, 0x15, 0x12, 0x0A, 0x02, 0x12, 0x0A, 0x12, 0x0A, // 8: TypeSpec 2 = TypeSpec 2<TypeSpec 2, TypeSpec 2>
                0x02, 0x1D, 0x08,                                     // 17: TypeSpec 3 = int[]
            };
            pad(blobs);
            std::vector<std::uint8_t> tables;
            put32(tables, 0);
            tables.push_back(2);  // major
            tables.push_back(0);  // minor
            tables.push_back(0);  // heap sizes: 2-byte indexes
            tables.push_back(1);
            const std::uint64_t valid = (1ull << M::TypeRef) | (1ull << M::Field) | (1ull << M::TypeSpec);
            put32(tables, static_cast<std::uint32_t>(valid));
            put32(tables, static_cast<std::uint32_t>(valid >> 32));
            put32(tables, 0);
            put32(tables, 0);
            put32(tables, 3);  // TypeRef rows
            put32(tables, 1);  // Field rows
            put32(tables, 3);  // TypeSpec rows
            const auto row = [&](std::initializer_list<std::uint32_t> cells)
                {
                    for (const std::uint32_t cell : cells)
                        put16(tables, cell);
                };
            // TypeRef: resolution scope (tag 3 = another TypeRef), name, namespace.
            row({ (2 << 2) | 3, 1, 0 });  // A, nested in B
            row({ (1 << 2) | 3, 3, 0 });  // B, nested in A
            row({ 0, 5, 7 });             // N.C
            row({ 0x16, 1, 4 });          // Field: flags, name, signature
            row({ 1, 8, 17 });            // TypeSpec signatures
            pad(tables);
            const auto token = [](M::Table table, std::uint32_t rid) { return (static_cast<std::uint32_t>(table) << 24) | rid; };

            struct Stream
            {
                const char* Name;
                const std::vector<std::uint8_t>* Bytes;
            };
            const Stream streams[] = { { "#~", &tables }, { "#Strings", &strings }, { "#Blob", &blobs } };
            std::vector<std::uint8_t> headers;
            std::uint32_t offset = 32;
            for (const Stream& stream : streams)
                offset += 8 + static_cast<std::uint32_t>((std::strlen(stream.Name) + 4) & ~std::size_t{ 3 });
            for (const Stream& stream : streams)
            {
                put32(headers, offset);
                put32(headers, static_cast<std::uint32_t>(stream.Bytes->size()));
                for (const char* c = stream.Name; *c; ++c)
                    headers.push_back(static_cast<std::uint8_t>(*c));
                headers.push_back(0);
                pad(headers);
                offset += static_cast<std::uint32_t>(stream.Bytes->size());
            }
            std::vector<std::uint8_t> block;
            put32(block, 0x424A5342);  // BSJB
            put16(block, 1);
            put16(block, 1);
            put32(block, 0);
            const char version[12] = "v4.0.30319";
            put32(block, sizeof(version));
            block.insert(block.end(), version, version + sizeof(version));
            put16(block, 0);
            put16(block, static_cast<std::uint32_t>(std::size(streams)));
            block.insert(block.end(), headers.begin(), headers.end());
            for (const Stream& stream : streams)
                block.insert(block.end(), stream.Bytes->begin(), stream.Bytes->end());

            M metadata;
            CHECK(metadata.Parse(block));
            if (!metadata.Valid())
                return;
            CHECK_EQ(metadata.Rows(M::TypeRef), 3u);
            CHECK_EQ(metadata.TypeName(token(M::TypeRef, 3)), std::string("N.C"));
            CHECK_EQ(metadata.TypeName(token(M::TypeSpec, 3)), std::string("int[]"));
            CHECK_EQ(metadata.TypeName(token(M::TypeRef, 1)), std::string());
            CHECK_EQ(metadata.TypeName(token(M::TypeRef, 2)), std::string());
            CHECK_EQ(metadata.TypeName(token(M::TypeSpec, 1)), std::string());
            CHECK_EQ(metadata.FieldTypeName(1), std::string());
            const auto began = std::chrono::steady_clock::now();
            CHECK_EQ(metadata.TypeName(token(M::TypeSpec, 2)), std::string());
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            CHECK_MSG(seconds < 1.0, Format("a self-naming generic instance took %.2f s", seconds));
        }

        // The cache of method code ranges: a newer range replaces every older
        // one it overlaps, so the range found for an address contains it (an
        // older range inside a newer one used to win the lookup, which then
        // found nothing and was dereferenced anyway).
        void CheckCodeRanges()
        {
            Target::ClrCodeRanges ranges;
            CHECK(ranges.Add(0x2050, 0x2060, "Old:Inner") != nullptr);
            CHECK(ranges.Add(0x2000, 0x2100, "New:Whole") != nullptr);
            CHECK_EQ(ranges.Size(), 1u);
            const Target::ClrCodeRanges::Entry* whole = ranges.Find(0x2080);
            CHECK(whole && whole->Name == "New:Whole" && whole->Start == 0x2000 && whole->End == 0x2100);
            whole = ranges.Find(0x2055);
            CHECK(whole && whole->Name == "New:Whole");
            // One reaching into the new range from before goes as well.
            ranges.Add(0x1000, 0x1100, "Early");
            ranges.Add(0x1080, 0x1200, "Late");
            CHECK(ranges.Find(0x1040) == nullptr);
            const Target::ClrCodeRanges::Entry* late = ranges.Find(0x10F0);
            CHECK(late && late->Name == "Late" && late->Start == 0x1080);
            // Neighbours that only touch stay.
            ranges.Add(0x3000, 0x3010, "First");
            ranges.Add(0x3010, 0x3020, "Second");
            const Target::ClrCodeRanges::Entry* first = ranges.Find(0x300F);
            const Target::ClrCodeRanges::Entry* second = ranges.Find(0x3010);
            CHECK(first && first->Name == "First");
            CHECK(second && second->Name == "Second");
            CHECK(ranges.Find(0x3020) == nullptr);
            CHECK(ranges.Find(0x0FFF) == nullptr);
            CHECK(ranges.Add(0x4000, 0x4000, "Empty") == nullptr);
            CHECK(ranges.Find(0x4000) == nullptr);
            CHECK_EQ(ranges.Size(), 4u);
            ranges.Clear();
            CHECK(ranges.Find(0x2080) == nullptr);
        }

        void CheckProtocol()
        {
            Target::ClrProtocol::Writer writer;
            writer.U8(7);
            writer.U32(0x11223344);
            writer.U64(0x0102030405060708ull);
            writer.Text("Terraria.Main");
            writer.I32(-5);
            const std::vector<std::uint8_t> message = writer.Done();
            std::uint32_t size = 0;
            std::memcpy(&size, message.data(), sizeof(size));
            CHECK_EQ(size, static_cast<std::uint32_t>(message.size() - 4));
            const std::span<const std::uint8_t> body(message.data() + 4, message.size() - 4);
            Target::ClrProtocol::Reader in(body);
            CHECK_EQ(in.U8(), 7);
            CHECK_EQ(in.U32(), 0x11223344u);
            CHECK_EQ(in.U64(), 0x0102030405060708ull);
            CHECK_EQ(in.Text(), std::string("Terraria.Main"));
            CHECK_EQ(in.I32(), -5);
            CHECK(in.Ok() && in.AtEnd());
            for (std::size_t cut = 0; cut < body.size(); ++cut)
            {
                Target::ClrProtocol::Reader partial(body.first(cut));
                partial.U8();
                partial.U32();
                partial.U64();
                partial.Text();
                partial.I32();
                CHECK(!partial.Ok());
            }
            const std::uint8_t huge[] = { 0xFF, 0xFF, 0xFF, 0x7F, 1, 2 };
            Target::ClrProtocol::Reader counted(huge);
            CHECK_EQ(counted.Count(8), 0u);
            CHECK(!counted.Ok());
        }

        void CheckRuntime(const std::filesystem::path& targetPath, const std::wstring& arguments, const std::string& label);

        void CheckRuntimeIfChosen(const std::filesystem::path& targetPath, const std::wstring& arguments, const std::string& label)
        {
            if (g_only.empty() || label.find(g_only) != std::string::npos)
                CheckRuntime(targetPath, arguments, label);
        }

        void CheckRuntime(const std::filesystem::path& targetPath, const std::wstring& arguments, const std::string& label)
        {
            ClrTarget target;
            const bool started = target.Start(targetPath, arguments);
            CHECK_MSG(started, label + ": the fake .NET game did not start: " + target.Output);
            if (!started)
                return;
            const std::uint64_t playerAddress = target.Addresses["player"];
            CHECK_MSG(playerAddress != 0, label + ": " + target.Output);
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return;
            const Target::Process& process = *engine.Process();
            const std::uint64_t pointer = static_cast<std::uint64_t>(process.PointerSize());
            CHECK_EQ(Target::MonoBridge::DetectRuntime(process), Target::ManagedRuntime::Clr);
            LuaCheck(engine, label + " before loading", R"LUA(
                local v, e = manifold.dotnet.classes()
                if v ~= nil or not e or e.code ~= "ME-4104" then return "classes before load: " .. tostring(e and e.code) end
                if manifold.dotnet.isLoaded() then return "loaded" end
                local s = manifold.dotnet.status()
                if s.loaded or not s.detected then return "status" end
                return "ok")LUA");
            const auto began = std::chrono::steady_clock::now();
            const Status loaded = engine.Managed().Ensure();
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            CHECK_MSG(loaded.Ok(), label + ": " + loaded.Text);
            if (!loaded)
            {
                engine.CloseProcess();
                return;
            }
            Target::MonoBridge& mono = engine.Mono();
            std::printf("  %s: %s, %zu classes in %.2f s\n", label.c_str(), mono.RuntimeDescription().c_str(), mono.ClassCount(), seconds);
            CHECK(mono.IsClr());
            // Only loaded types have a method table; a small .NET program has
            // a few hundred, the .NET Framework's NGEN images thousands.
            CHECK_MSG(mono.ClassCount() > 200, std::to_string(mono.ClassCount()));
            CHECK_EQ(CountHelpers(), 1);

            const auto player = mono.ClassDetails("Manifold.FakeGame.Player");
            CHECK_MSG(player.has_value(), label);
            if (player)
            {
                CHECK_EQ(player->Image, std::string("ManifoldFakeGame"));
                CHECK_EQ(player->Parent, std::string("Manifold.FakeGame.Entity"));
                const auto offset = [&](const char* name) -> std::uint64_t
                    {
                        const Target::MonoField* field = Field(player->Fields, name);
                        CHECK_MSG(field != nullptr, std::string("no field ") + name);
                        return field ? static_cast<std::uint64_t>(field->Offset) : 0;
                    };
                CHECK_EQ(Read<std::int32_t>(process, playerAddress + offset("Health")), 1234);
                CHECK_EQ(Read<float>(process, playerAddress + offset("Speed")), 2.5f);
                CHECK_EQ(Read<double>(process, playerAddress + offset("Gold")), 99.75);
                CHECK_EQ(Read<std::int64_t>(process, playerAddress + offset("Score")), 123456789012ll);
                CHECK_EQ(Read<std::uint8_t>(process, playerAddress + offset("Alive")), 1);
                CHECK_EQ(Read<std::int32_t>(process, playerAddress + offset("Side")), 2);
                if (const Target::MonoField* name = Field(player->Fields, "Name"))
                {
                    CHECK_EQ(name->TypeName, std::string("string"));
                    CHECK_EQ(process.ReadPointer(playerAddress + static_cast<std::uint64_t>(name->Offset)).value_or(0), target.Addresses["name"]);
                }
                CHECK_EQ(process.ReadPointer(playerAddress + offset("Inventory")).value_or(0), target.Addresses["inventory"]);
                CHECK_EQ(process.ReadPointer(playerAddress + offset("Bag")).value_or(0), target.Addresses["bag"]);
                CHECK(Field(player->Fields, "Id") == nullptr);  // inherited: on Entity

                const Target::MonoMethod* hurt = Method(*player, "Hurt");
                const Target::MonoMethod* never = Method(*player, "NeverCalled");
                CHECK(hurt && never);
                if (hurt && never)
                {
                    CHECK_EQ(hurt->ParamCount, 1);
                    CHECK_EQ(never->ParamCount, 2);
                    CHECK_MSG(hurt->Address != 0, "Hurt has run, so it has code");
                    CHECK_MSG(never->Address == 0, "NeverCalled never ran, so it has no code");
                    CHECK_EQ(mono.AddressOf("Manifold.FakeGame.Player:Hurt").value_or(0), hurt->Address);
                    CHECK(!mono.AddressOf("Manifold.FakeGame.Player:NeverCalled").has_value());
                    if (hurt->Address)
                    {
                        CHECK_EQ(mono.MethodNameAt(hurt->Address + 1).value_or(""), std::string("Manifold.FakeGame.Player:Hurt"));
                        CHECK_EQ(mono.MethodStartNameAt(hurt->Address).value_or(""), std::string("Manifold.FakeGame.Player:Hurt"));
                        CHECK(!mono.MethodStartNameAt(hurt->Address + 1).has_value());
                        const auto range = mono.MethodRange(hurt->Address + 1);
                        CHECK(range && range->first == hurt->Address && range->second > hurt->Address + 1);
                        CHECK_EQ(engine.Symbols().Resolve("Manifold.FakeGame.Player:Hurt").value_or(0), hurt->Address);
                        const std::string described = engine.Describe(hurt->Address + 2);
                        CHECK_MSG(described.find("Player:Hurt+") != std::string::npos, described);
                    }
                }
            }

            // Inherited and struct fields, the way Structure Dissect asks.
            const std::vector<Target::MonoField> all = mono.InstanceFieldsOf("Manifold.FakeGame.Player");
            const Target::MonoField* id = Field(all, "Id");
            const Target::MonoField* position = Field(all, "Position");
            CHECK(id && position && Field(all, "Health"));
            if (id)
                CHECK_EQ(Read<std::int32_t>(process, playerAddress + static_cast<std::uint64_t>(id->Offset)), 7);
            if (position)
            {
                CHECK_EQ(position->TypeCode, 0x11);
                CHECK_EQ(position->ValueSize, 8);
                CHECK_EQ(position->TypeName, std::string("Manifold.FakeGame.Vector2"));
                CHECK_EQ(Read<float>(process, playerAddress + static_cast<std::uint64_t>(position->Offset)), 1.5f);
                CHECK_EQ(Read<float>(process, playerAddress + static_cast<std::uint64_t>(position->Offset) + 4), -2.25f);
                const std::vector<Target::MonoField> inner = mono.TypedFieldsOf(position->TypeHandle);
                const Target::MonoField* x = Field(inner, "X");
                const Target::MonoField* y = Field(inner, "Y");
                CHECK(x && y);
                if (x && y)
                {
                    CHECK_EQ(static_cast<std::uint64_t>(x->Offset), 2 * pointer);
                    CHECK_EQ(static_cast<std::uint64_t>(y->Offset), 2 * pointer + 4);
                }
                Table::ManagedLayouts nested;
                Table::CollectManagedLayouts(mono, all, nested);
                CHECK(nested.count("Manifold.FakeGame.Vector2"));
            }

            // Statics: Class:Field resolves to where the value is.
            const auto slot = mono.AddressOf("Manifold.FakeGame.World:LocalPlayer");
            CHECK_MSG(slot.has_value(), label + ": World:LocalPlayer");
            if (slot)
                CHECK_EQ(process.ReadPointer(*slot).value_or(0), playerAddress);
            const auto seed = mono.AddressOf("Manifold.FakeGame.World:Seed");
            CHECK(seed.has_value());
            if (seed)
                CHECK_EQ(Read<std::int32_t>(process, *seed), 4242);
            if (const auto gravity = engine.Symbols().Resolve("Manifold.FakeGame.World:Gravity"))
                CHECK_EQ(Read<double>(process, *gravity), 9.81);
            else
                CHECK_MSG(false, "World:Gravity does not resolve as an address");
            if (const auto ticks = mono.AddressOf("World:Ticks"))
                CHECK(Read<std::int32_t>(process, *ticks) >= 1);
            else
                CHECK_MSG(false, "World:Ticks does not resolve");
            const auto world = mono.ClassDetails("Manifold.FakeGame.World");
            CHECK(world.has_value());
            if (world)
            {
                const Target::MonoField* spawn = Field(world->Statics, "Spawn");
                CHECK(spawn && spawn->Address);
                if (spawn && spawn->Address)
                {
                    // A static struct is boxed: the slot points to the box.
                    const std::uint64_t box = process.ReadPointer(spawn->Address).value_or(0);
                    CHECK_EQ(Read<float>(process, box + pointer), 100.0f);
                    CHECK_EQ(Read<float>(process, box + pointer + 4), 200.0f);
                }
                CHECK(world->Fields.empty());
                CHECK(Field(world->Statics, "Title") != nullptr);
            }

            // Naming objects by their method table.
            CHECK_EQ(mono.ClassNameAt(playerAddress).value_or(""), std::string("Manifold.FakeGame.Player"));
            if (player)
            {
                if (const Target::MonoField* health = Field(player->Fields, "Health"))
                {
                    const auto inside = mono.ClassAt(playerAddress + static_cast<std::uint64_t>(health->Offset));
                    CHECK(inside && inside->Base == playerAddress && inside->Name == "Manifold.FakeGame.Player");
                }
            }
            CHECK_EQ(mono.ClassNameAt(target.Addresses["name"]).value_or(""), std::string("System.String"));
            CHECK_EQ(mono.ClassNameAt(target.Addresses["inventory"]).value_or(""), std::string("Manifold.FakeGame.Item[]"));
            const std::string bag = mono.ClassNameAt(target.Addresses["bag"]).value_or("");
            CHECK_MSG(bag.find("List`1") != std::string::npos, bag);
            // A generic instance found by its object still has its fields.
            const std::vector<Target::MonoField> bagFields = mono.InstanceFieldsOf(bag);
            CHECK_MSG(Field(bagFields, "_size") != nullptr, bag + " has no _size");
            if (const Target::MonoField* size = Field(bagFields, "_size"))
                CHECK_EQ(Read<std::int32_t>(process, target.Addresses["bag"] + static_cast<std::uint64_t>(size->Offset)), 1);
            CHECK(!mono.ClassNameAt(playerAddress + 4).has_value());
            CHECK(!mono.ClassNameAt(0x10000).has_value());
            CHECK(mono.ClassDetails("Manifold.FakeGame.Player+Buff").has_value());

            // The GC heap: counted per type, the objects of a type, one object.
            const auto heapBegan = std::chrono::steady_clock::now();
            const Result<Target::ManagedHeap> heap = mono.ReadHeap();
            CHECK_MSG(heap.Ok(), label + ": " + heap.Error().Text);
            if (heap)
            {
                std::printf("  %s: heap of %llu objects, %.1f MB in %u segments, %llu words skipped, %.2f s\n", label.c_str(),
                    static_cast<unsigned long long>(heap->Objects), static_cast<double>(heap->Bytes) / (1024.0 * 1024.0), heap->Segments,
                    static_cast<unsigned long long>(heap->Skipped), std::chrono::duration<double>(std::chrono::steady_clock::now() - heapBegan).count());
                const auto count = [&](const std::string& name) -> std::uint64_t
                    {
                        const auto found = std::find_if(heap->Types.begin(), heap->Types.end(), [&](const Target::ManagedHeapType& type) { return type.Name == name; });
                        return found == heap->Types.end() ? 0 : found->Count;
                    };
                CHECK_EQ(count("Manifold.FakeGame.Player"), 1u);
                CHECK_EQ(count("Manifold.FakeGame.Item"), 2u);
                CHECK_EQ(count("Manifold.FakeGame.Player+Buff"), 1u);
                // The inventory, the array inside the List<Item> and the
                // list class's shared empty array.
                CHECK_MSG(count("Manifold.FakeGame.Item[]") >= 2u, std::to_string(count("Manifold.FakeGame.Item[]")));
                CHECK(count("System.String") > 3);
                CHECK(heap->Objects > 100);
            }
            if (player)
            {
                const auto players = mono.Instances(player->Handle, 100);
                CHECK_MSG(players.Ok(), players.Error().Text);
                if (players)
                {
                    CHECK_EQ(players->Total, 1u);
                    CHECK(players->Addresses.size() == 1 && players->Addresses[0] == playerAddress);
                }
            }
            const std::uint64_t inventory = target.Addresses["inventory"];
            const std::uint64_t torch = process.ReadPointer(inventory + 2 * pointer).value_or(0);
            if (const auto item = mono.ClassDetails("Manifold.FakeGame.Item"))
            {
                const auto items = mono.Instances(item->Handle, 100);
                CHECK(items.Ok());
                if (items)
                {
                    CHECK_EQ(items->Total, 2u);
                    CHECK(std::find(items->Addresses.begin(), items->Addresses.end(), torch) != items->Addresses.end());
                }
                const auto one = mono.Instances(item->Handle, 1);
                CHECK(one && one->Total == 2 && one->Addresses.size() == 1);
            }
            if (const auto array = mono.ObjectAt(inventory))
            {
                CHECK(array->Kind == Target::ManagedObjectKind::Array);
                CHECK_EQ(array->Name, std::string("Manifold.FakeGame.Item[]"));
                CHECK_EQ(array->Length, 3u);
                CHECK_EQ(array->ElementType, 0x12);
                CHECK_EQ(array->ElementName, std::string("Manifold.FakeGame.Item"));
                CHECK_EQ(array->ElementSize, pointer);
                CHECK_EQ(array->Data, inventory + 2 * pointer);
                CHECK_EQ(mono.ClassNameAt(torch).value_or(""), std::string("Manifold.FakeGame.Item"));
            }
            else
                CHECK_MSG(false, label + ": no object at the inventory array");
            if (const auto text = mono.ObjectAt(target.Addresses["name"]))
            {
                CHECK(text->Kind == Target::ManagedObjectKind::String);
                CHECK_MSG(text->Length == 4u, std::to_string(text->Length));
                wchar_t chars[4] = {};
                process.ReadExact(target.Addresses["name"] + pointer + 4, chars, sizeof(chars));
                CHECK(std::wstring(chars, 4) == L"Hero");
            }
            else
                CHECK_MSG(false, label + ": no object at the name string");
            if (const auto object = mono.ObjectAt(playerAddress))
            {
                CHECK_MSG(object->Kind == Target::ManagedObjectKind::Object, std::to_string(static_cast<int>(object->Kind)));
                CHECK_EQ(object->Name, std::string("Manifold.FakeGame.Player"));
                CHECK(object->Size >= 3 * pointer);
            }
            else
                CHECK_MSG(false, label + ": no object at the player");
            CHECK(!mono.ObjectAt(playerAddress + pointer).has_value());
            // The fields of an object found by its own class pointer, with the parents'.
            const std::vector<Target::MonoField> atFields = mono.InstanceFieldsAt(playerAddress);
            CHECK(Field(atFields, "Health") && Field(atFields, "Id") && Field(atFields, "Position"));

            // A pointer from a static field, the way the inspector adds a
            // value to the address list: [World:LocalPlayer] + Health.
            if (player)
            {
                if (const Target::MonoField* health = Field(player->Fields, "Health"))
                {
                    Table::MemoryRecord& record = engine.Table().AddValue("LocalPlayer.Health", "Manifold.FakeGame.World:LocalPlayer", Scan::ValueType::FourBytes);
                    record.Offsets = { Hex(static_cast<std::uint64_t>(health->Offset)) };
                    engine.RefreshValues(true);
                    CHECK_MSG(record.AddressOk, record.AddressError);
                    CHECK_EQ(record.ResolvedAddress, playerAddress + static_cast<std::uint64_t>(health->Offset));
                }
            }

            // Native code is never a managed method, and asking does not
            // reach the helper.
            const std::uint64_t native = process.FindExport("kernel32.dll", "GetTickCount");
            CHECK(native && !mono.MethodNameAt(native).has_value());

            CheckLua(engine, label, playerAddress, target.Addresses["inventory"], target.Addresses["name"], pointer);
            LuaCheck(engine, label + " unload and load", R"LUA(
                manifold.dotnet.unload()
                if manifold.dotnet.isLoaded() then return "still loaded" end
                local ok, e = manifold.dotnet.load()
                if not ok then return "load: " .. tostring(e) end
                if manifold.dotnet.value("Manifold.FakeGame.World:Seed") ~= 4242 then return "after reload" end
                return "ok")LUA");

            engine.Managed().Stop(true);
            CHECK(!mono.Present());
            const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (CountHelpers() && std::chrono::steady_clock::now() < stopDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            CHECK_EQ(CountHelpers(), 0);
            engine.CloseProcess();
            DWORD code = 0;
            ::GetExitCodeProcess(target.Info.hProcess, &code);
            CHECK_MSG(code == STILL_ACTIVE, label + ": the fake .NET game ended with " + Hex(static_cast<std::uint32_t>(code), 8));
        }

        void CheckHelperMissing()
        {
            // The helper path is next to the program; both must be there in a
            // build, since the install copies them from here.
            CHECK(std::filesystem::exists(Target::ClrLink::HelperPath(false)));
            CHECK(std::filesystem::exists(Target::ClrLink::HelperPath(true)));
        }

        int RunClrTests(const std::filesystem::path& target)
        {
            const int before = Totals().Failures;
            CheckProtocol();
            CheckMetadata();
            CheckMetadataCycles();
            CheckCodeRanges();
            CheckHelperMissing();
            if (!std::filesystem::exists(FakeGame()))
            {
                Skip("the fake .NET game was not built, so no runtime is read");
                return Totals().Failures - before;
            }
            const std::wstring game = L"\"" + FakeGame().wstring() + L"\"";
            CheckRuntimeIfChosen(target, L"--clr=" + game, ".NET Framework x64");
            const std::filesystem::path target32 = target.parent_path() / L"TestTarget32.exe";
            if (std::filesystem::exists(target32))
                CheckRuntimeIfChosen(target32, L"--clr=" + game, ".NET Framework x86");
            else
                Skip("no TestTarget32.exe for the 32-bit .NET Framework");
            // .NET 8 keeps statics per module, .NET 9 and later per type: one of each.
            for (const int major : { 8, 0 })
            {
                const auto runtime = CoreRuntime(major);
                if (!runtime)
                {
                    Skip(major ? "no .NET 8 runtime installed" : "no .NET runtime installed");
                    continue;
                }
                if (major == 0 && runtime == CoreRuntime(8))
                    continue;
                CheckRuntimeIfChosen(target, L"--coreclr=" + game + L" --runtime=\"" + runtime->wstring() + L"\"", ".NET " + WideToUtf8(runtime->filename().wstring()));
            }
            return Totals().Failures - before;
        }

        int RunClrCheck(const std::vector<std::string>& arguments)
        {
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            for (const std::string& argument : arguments)
            {
                if (argument.rfind("--only=", 0) == 0)
                    g_only = argument.substr(7);
            }
            const int failures = RunClrTests(Self().parent_path() / L"TestTarget.exe");
            std::printf("clr check: %d checks, %d failed\n", Totals().Checks, failures);
            return failures == 0 ? 0 : 1;
        }
    }

    namespace
    {
        // --clr-dump --process=<name.exe>: a read-only look at a real .NET
        // game. Dry run keeps any write, allocation or thread the engine might
        // attempt in an overlay, so nothing reaches the game; the reading
        // itself happens in the helper, outside the game.
        int RunClrDump(const std::vector<std::string>& arguments)
        {
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            std::string name;
            for (const std::string& argument : arguments)
            {
                if (argument.rfind("--process=", 0) == 0)
                    name = argument.substr(10);
            }
            if (name.empty())
            {
                std::printf("usage: --clr-dump --process=<name.exe>\n");
                return 2;
            }
            Target::Process::SetDryRun(true);
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcessByName(name);
            if (!opened)
            {
                std::printf("could not open %s: %s\n", name.c_str(), opened.Text.c_str());
                return 1;
            }
            const Target::Process& process = *engine.Process();
            const std::uint64_t pointer = static_cast<std::uint64_t>(process.PointerSize());
            std::printf("%s pid %u, %d-bit, runtime detected: %s\n", process.Name().c_str(), static_cast<unsigned>(Raw(process.Id())), process.Is64Bit() ? 64 : 32,
                Target::MonoBridge::RuntimeName(Target::MonoBridge::DetectRuntime(process)));
            const auto began = std::chrono::steady_clock::now();
            const Status loaded = engine.Managed().Ensure();
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            if (!loaded)
            {
                std::printf("loading failed: %s\n", loaded.Text.c_str());
                engine.CloseProcess();
                return 1;
            }
            Target::MonoBridge& mono = engine.Mono();
            std::printf("%s, %zu classes in %.2f s (module %s)\n", mono.RuntimeDescription().c_str(), mono.ClassCount(), seconds, mono.Module().c_str());
            std::map<std::string, std::size_t> images;
            for (const Target::MonoClass& info : mono.Classes())
                ++images[info.Image];
            std::vector<std::pair<std::size_t, std::string>> biggest;
            for (const auto& [image, count] : images)
                biggest.emplace_back(count, image);
            std::sort(biggest.rbegin(), biggest.rend());
            for (std::size_t i = 0; i < biggest.size() && i < 8; ++i)
                std::printf("  %6zu  %s\n", biggest[i].first, biggest[i].second.c_str());

            const auto show = [&](const std::string& className)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto info = mono.ClassDetails(className);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    if (!info)
                    {
                        std::printf("%s: not found\n", className.c_str());
                        return info;
                    }
                    std::size_t compiled = 0;
                    for (const Target::MonoMethod& method : info->Methods)
                        compiled += method.Address ? 1 : 0;
                    std::printf("%s (%s, parent %s): %zu fields, %zu statics, %zu methods (%zu compiled), members read in %.1f ms\n", className.c_str(), info->Image.c_str(),
                        info->Parent.c_str(), info->Fields.size(), info->Statics.size(), info->Methods.size(), compiled, ms);
                    return info;
                };
            const auto main = show("Terraria.Main");
            const auto player = show("Terraria.Player");
            if (main)
            {
                std::size_t allocated = 0;
                for (const Target::MonoField& field : main->Statics)
                    allocated += field.Address ? 1 : 0;
                std::printf("Terraria.Main statics with storage: %zu of %zu\n", allocated, main->Statics.size());
            }
            const auto myPlayerAt = mono.AddressOf("Terraria.Main:myPlayer");
            const auto playersAt = mono.AddressOf("Terraria.Main:player");
            std::int32_t myPlayer = -1;
            if (myPlayerAt)
                process.ReadExact(*myPlayerAt, &myPlayer, sizeof(myPlayer));
            std::printf("Terraria.Main:myPlayer at %s = %d\n", myPlayerAt ? Hex(*myPlayerAt).c_str() : "?", myPlayer);
            if (playersAt)
            {
                const std::uint64_t array = process.ReadPointer(*playersAt).value_or(0);
                std::uint32_t length = 0;
                process.ReadExact(array + pointer, &length, sizeof(length));
                std::printf("Terraria.Main:player at %s -> %s (%s, length %u)\n", Hex(*playersAt).c_str(), Hex(array).c_str(), mono.ClassNameAt(array).value_or("?").c_str(), length);
                if (myPlayer >= 0 && static_cast<std::uint32_t>(myPlayer) < length && player)
                {
                    const std::uint64_t me = process.ReadPointer(array + 2 * pointer + static_cast<std::uint64_t>(myPlayer) * pointer).value_or(0);
                    std::printf("player[%d] at %s is %s\n", myPlayer, Hex(me).c_str(), mono.ClassNameAt(me).value_or("?").c_str());
                    const std::vector<Target::MonoField> fields = mono.InstanceFieldsOf("Terraria.Player");
                    for (const char* wanted : { "statLife", "statLifeMax", "statLifeMax2", "statMana", "statManaMax2", "name", "position", "whoAmI" })
                    {
                        const auto found = std::find_if(fields.begin(), fields.end(), [&](const Target::MonoField& field) { return field.Name == wanted; });
                        if (found == fields.end())
                        {
                            std::printf("  %-14s not found\n", wanted);
                            continue;
                        }
                        const std::uint64_t at = me + static_cast<std::uint64_t>(found->Offset);
                        std::int32_t value = 0;
                        process.ReadExact(at, &value, sizeof(value));
                        std::string shown = std::to_string(value);
                        if (found->TypeCode == 0x12 || found->TypeCode == 0x0E)
                            shown = Hex(process.ReadPointer(at).value_or(0)) + " " + mono.ClassNameAt(process.ReadPointer(at).value_or(0)).value_or("");
                        else if (found->TypeCode == 0x11)
                        {
                            float xy[2] = {};
                            process.ReadExact(at, xy, sizeof(xy));
                            shown = Format("(%.1f, %.1f) %s", static_cast<double>(xy[0]), static_cast<double>(xy[1]), found->TypeName.c_str());
                        }
                        std::printf("  %-14s +0x%X %-10s %s\n", wanted, found->Offset, found->TypeName.c_str(), shown.c_str());
                    }
                }
            }
            if (player)
            {
                for (const Target::MonoMethod& method : player->Methods)
                {
                    if (method.Name != "Update" || !method.Address)
                        continue;
                    std::printf("Terraria.Player:Update code at %s, named back as %s, described as %s\n", Hex(method.Address).c_str(),
                        mono.MethodNameAt(method.Address + 8).value_or("?").c_str(), engine.Describe(method.Address + 8).c_str());
                    break;
                }
            }
            // --lua=<file>: a script run once the runtime is loaded, for a
            // look through manifold.dotnet; it returns the text to print.
            for (const std::string& argument : arguments)
            {
                if (argument.rfind("--lua=", 0) != 0)
                    continue;
                std::ifstream file(std::filesystem::path(argument.substr(6)), std::ios::binary);
                const std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                std::string text;
                const Status ran = engine.Lua().RunWithResult(code, "clr-dump", text);
                std::printf("lua: %s\n%s\n", ran.Ok() ? "ok" : ran.Text.c_str(), text.c_str());
            }
            const auto heapBegan = std::chrono::steady_clock::now();
            const Result<Target::ManagedHeap> heap = mono.ReadHeap();
            if (!heap)
                std::printf("heap: %s\n", heap.Error().Text.c_str());
            else
            {
                std::printf("heap: %llu objects, %.1f MB in %u segments, %llu words skipped, %.2f s\n", static_cast<unsigned long long>(heap->Objects),
                    static_cast<double>(heap->Bytes) / (1024.0 * 1024.0), heap->Segments, static_cast<unsigned long long>(heap->Skipped),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - heapBegan).count());
                for (std::size_t i = 0; i < heap->Types.size() && i < 12; ++i)
                    std::printf("  %10llu objects %8.1f MB  %s\n", static_cast<unsigned long long>(heap->Types[i].Count),
                        static_cast<double>(heap->Types[i].Bytes) / (1024.0 * 1024.0), heap->Types[i].Name.c_str());
            }
            if (player)
            {
                const auto players = mono.Instances(player->Handle, 1000);
                if (players)
                    std::printf("Terraria.Player objects on the heap: %llu\n", static_cast<unsigned long long>(players->Total));
            }
            engine.Managed().Stop(true);
            engine.CloseProcess();
            std::printf("done\n");
            return 0;
        }
    }

    MANIFOLD_SUITE("clr", ".NET runtime", 116, [](const SuiteContext& context) { return RunClrTests(context.Target); });
    MANIFOLD_MODE("--clr-dump", &RunClrDump);
    MANIFOLD_MODE("--clr-check", &RunClrCheck);
}
