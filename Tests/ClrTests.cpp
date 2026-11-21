#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Table/ManagedStructures.h"
#include "Target/ClrLink.h"
#include "Target/ClrMetadata.h"
#include "Target/ClrProtocol.h"
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

            // Native code is never a managed method, and asking does not
            // reach the helper.
            const std::uint64_t native = process.FindExport("kernel32.dll", "GetTickCount");
            CHECK(native && !mono.MethodNameAt(native).has_value());

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
