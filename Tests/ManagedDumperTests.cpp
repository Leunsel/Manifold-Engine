#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Target/ManagedDumper.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>
#include <ole2.h>
#include <metahost.h>

namespace Manifold::Tests
{
    namespace
    {
        namespace F = Target::ManagedDumpFormat;

        class BlobWriter
        {
        public:
            std::uint32_t Text(const std::string& text)
            {
                const auto found = std::find(m_strings.begin(), m_strings.end(), text);
                if (found != m_strings.end())
                    return static_cast<std::uint32_t>(found - m_strings.begin());
                m_strings.push_back(text);
                return static_cast<std::uint32_t>(m_strings.size() - 1);
            }
            void U32(std::uint32_t value) { Put(m_types, &value, sizeof(value)); }
            void I32(std::int32_t value) { Put(m_types, &value, sizeof(value)); }
            void U64(std::uint64_t value) { Put(m_types, &value, sizeof(value)); }

            void Type(const std::string& image, const std::string& space, const std::string& name, std::uint32_t parent, std::uint32_t token, std::uint64_t klass, std::uint32_t flags)
            {
                U32(Text(image));
                U32(Text(space));
                U32(Text(name));
                U32(parent);
                U32(token);
                U64(klass);
                U32(flags);
            }
            void Field(const std::string& name, std::int32_t offset, std::uint32_t flags, std::uint32_t code)
            {
                U32(Text(name));
                I32(offset);
                U32(flags);
                U32(code);
            }
            void Method(const std::string& name, std::uint64_t handle, std::uint32_t flags, std::int32_t parameters)
            {
                U32(Text(name));
                U64(handle);
                U32(flags);
                I32(parameters);
            }

            std::vector<std::uint8_t> Finish(std::uint32_t header, std::uint32_t types, std::uint32_t version = F::kVersion, std::uint32_t magic = F::kMagic) const
            {
                std::vector<std::uint8_t> out;
                const std::uint32_t head[] = { magic, version, header, static_cast<std::uint32_t>(m_strings.size()) };
                Put(out, head, sizeof(head));
                for (const std::string& text : m_strings)
                {
                    const std::uint32_t length = static_cast<std::uint32_t>(text.size());
                    Put(out, &length, sizeof(length));
                    Put(out, text.data(), text.size());
                }
                Put(out, &types, sizeof(types));
                out.insert(out.end(), m_types.begin(), m_types.end());
                return out;
            }

        private:
            static void Put(std::vector<std::uint8_t>& to, const void* data, std::size_t size)
            {
                const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
                to.insert(to.end(), bytes, bytes + size);
            }

            std::vector<std::string> m_strings;
            std::vector<std::uint8_t> m_types;
        };

        std::vector<std::uint8_t> SampleBlob(std::uint32_t header)
        {
            BlobWriter writer;
            const std::uint32_t parent = writer.Text("UnityEngine.MonoBehaviour");
            writer.Type("Assembly-CSharp", "Game", "Player", parent, 0x02000002u, 0x1000, F::kTypeAbstract);
            writer.U32(4);
            writer.Field("health", 0x10, 0, 0x08);
            writer.Field("ammo", 0x1C, F::kFieldInitOnly, 0x08);
            writer.Field("s_count", 0x00, F::kFieldStatic, 0x08);
            writer.Field("MaxHealth", -1, F::kFieldStatic | F::kFieldLiteral, 0x08);
            writer.U32(2);
            writer.Method(".ctor", 0x2000, F::kMethodConstructor, 0);
            writer.Method("TakeDamage", 0x2008, F::kMethodVirtual, 1);

            writer.Type("mscorlib", "System.Collections.Generic", "List`1", writer.Text("System.Object"), 0x02000010u, 0x3000, F::kTypeGenericDefinition);
            writer.U32(1);
            writer.Field("_size", 0x18, 0, 0x08);
            writer.U32(0);

            writer.Type("Assembly-CSharp", "", "Inner", F::kNoString, 0x02000003u, 0x4000, F::kTypeValueType | F::kTypeNested);
            writer.U32(0);
            writer.U32(0);

            writer.Type("ManifoldDumper", std::string(F::kNamespace), std::string(F::kClass), writer.Text("System.Object"), 0x02000002u, 0x5800, F::kTypeAbstract);
            writer.U32(0);
            writer.U32(0);

            writer.Type(std::string(F::kImageName), std::string(F::kNamespace), std::string(F::kClass), writer.Text("System.Object"), 0x02000002u, 0x5000, F::kTypeAbstract);
            writer.U32(0);
            writer.U32(1);
            writer.Method("Begin", 0x6000, F::kMethodStatic, 0);
            return writer.Finish(header, 5);
        }

        const Target::MonoClass* Find(const std::vector<Target::MonoClass>& classes, const std::string& full)
        {
            const auto found = std::find_if(classes.begin(), classes.end(), [&](const Target::MonoClass& info) { return info.FullName() == full; });
            return found == classes.end() ? nullptr : &*found;
        }

        void CheckParser()
        {
            const std::vector<std::uint8_t> blob = SampleBlob(F::kHeaderMonoHandles | F::kHeaderOffsetsSure);
            const Result<Target::ManagedDump> parsed = Target::ParseManagedDump(blob);
            CHECK_MSG(parsed.Ok(), parsed.Error().Text);
            if (parsed)
            {
                const Target::ManagedDump& dump = *parsed;
                CHECK(dump.MonoHandles);
                CHECK(dump.OffsetsSure);
                CHECK_EQ(dump.Classes.size(), 3u);
                CHECK_EQ(dump.Tokens.size(), dump.Classes.size());
                CHECK_EQ(dump.Flags.size(), dump.Classes.size());
                const Target::MonoClass* player = Find(dump.Classes, "Game.Player");
                CHECK(player != nullptr);
                if (player)
                {
                    CHECK_EQ(player->Image, std::string("Assembly-CSharp"));
                    CHECK_EQ(player->Parent, std::string("UnityEngine.MonoBehaviour"));
                    CHECK_EQ(player->Handle, 0x1000ull);
                    CHECK(player->MembersLoaded);
                    CHECK(!player->Generic);
                    CHECK_MSG(player->Fields.size() == 2, "static and constant fields are dropped, got " + std::to_string(player->Fields.size()));
                    if (player->Fields.size() == 2)
                    {
                        CHECK(player->Fields[0].Name == "health" && player->Fields[0].Offset == 0x10 && player->Fields[0].TypeCode == 0x08);
                        CHECK(player->Fields[1].Name == "ammo" && player->Fields[1].Offset == 0x1C);
                    }
                    CHECK_EQ(player->Methods.size(), 2u);
                    if (player->Methods.size() == 2)
                    {
                        CHECK(player->Methods[0].Name == ".ctor" && player->Methods[0].ParamCount == 0 && player->Methods[0].Handle == 0x2000);
                        CHECK(player->Methods[1].Name == "TakeDamage" && player->Methods[1].ParamCount == 1 && player->Methods[1].Handle == 0x2008);
                    }
                    CHECK_EQ(dump.Tokens[static_cast<std::size_t>(player - dump.Classes.data())], 0x02000002u);
                }
                const Target::MonoClass* list = Find(dump.Classes, "System.Collections.Generic.List`1");
                CHECK(list && list->Generic && list->Fields.size() == 1 && list->Parent == "System.Object");
                const Target::MonoClass* inner = Find(dump.Classes, "Inner");
                CHECK(inner && inner->Namespace.empty() && inner->Parent.empty());
                if (inner && dump.Flags.size() == dump.Classes.size())
                    CHECK(dump.Flags[static_cast<std::size_t>(inner - dump.Classes.data())] & F::kTypeNested);
                CHECK(std::none_of(dump.Classes.begin(), dump.Classes.end(), [](const Target::MonoClass& info) { return Target::IsDumperImage(info.Image); }));
                CHECK(Find(dump.Classes, "Manifold.Managed.Dumper") == nullptr);
            }

            const Result<Target::ManagedDump> foreign = Target::ParseManagedDump(SampleBlob(0));
            CHECK_MSG(foreign.Ok(), foreign.Error().Text);
            if (foreign)
            {
                CHECK(!foreign->MonoHandles);
                CHECK(!foreign->OffsetsSure);
                const Target::MonoClass* player = Find(foreign->Classes, "Game.Player");
                CHECK(player && player->Handle == 0 && !player->Methods.empty() && player->Methods[0].Handle == 0);
            }

            bool prefixesFail = true;
            for (std::size_t size = 0; size < blob.size(); ++size)
            {
                if (Target::ParseManagedDump(std::span<const std::uint8_t>(blob.data(), size)).Ok())
                {
                    prefixesFail = false;
                    std::printf("  a dump cut to %zu of %zu bytes was accepted\n", size, blob.size());
                    break;
                }
            }
            CHECK(prefixesFail);

            std::vector<std::uint8_t> trailing = blob;
            trailing.push_back(0);
            CHECK(!Target::ParseManagedDump(trailing).Ok());

            std::vector<std::uint8_t> badMagic = blob;
            badMagic[0] ^= 0xFF;
            CHECK(!Target::ParseManagedDump(badMagic).Ok());

            BlobWriter later;
            later.Type("A", "", "B", F::kNoString, 0x02000001u, 0, 0);
            later.U32(0);
            later.U32(0);
            const Result<Target::ManagedDump> newer = Target::ParseManagedDump(later.Finish(0, 1, F::kVersion + 1));
            CHECK(!newer.Ok() && newer.Error().Text.find("format " + std::to_string(F::kVersion + 1)) != std::string::npos);

            BlobWriter wild;
            wild.U32(77);
            wild.U32(0);
            wild.U32(0);
            wild.U32(F::kNoString);
            wild.U32(0x02000001u);
            wild.U64(0);
            wild.U32(0);
            wild.U32(0);
            wild.U32(0);
            const Result<Target::ManagedDump> unknownString = Target::ParseManagedDump(wild.Finish(0, 1));
            CHECK(!unknownString.Ok());

            BlobWriter huge;
            huge.Type("A", "", "B", F::kNoString, 0x02000001u, 0, 0);
            huge.U32(0x7FFFFFFFu);
            CHECK(!Target::ParseManagedDump(huge.Finish(0, 1)).Ok());
            CHECK(!Target::ParseManagedDump(BlobWriter().Finish(0, 0x7FFFFFFFu)).Ok());
        }

        void CheckEmbeddedImage()
        {
            const std::span<const std::uint8_t> image = Target::ManagedDumperImage();
            if (image.empty())
            {
                Skip("this build embedded no managed dumper (no .NET SDK at build time)");
                return;
            }
            CHECK(image.size() > 0x400 && image[0] == 'M' && image[1] == 'Z');
            const std::string_view text(reinterpret_cast<const char*>(image.data()), image.size());
            CHECK(text.find(F::kImageName) != std::string_view::npos);
            CHECK(text.find(F::kNamespace) != std::string_view::npos);
            for (const char* member : { "Begin", "Release", "DumpToFile", "State", "Types", "Cancel", "Data", "Size" })
                CHECK_MSG(text.find(member) != std::string_view::npos, member);
        }

        int RunChild(const std::wstring& arguments, DWORD waitMs)
        {
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring command = L"\"" + std::wstring(self) + L"\" " + arguments;
            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION info{};
            if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
                return -1;
            DWORD code = static_cast<DWORD>(-2);
            if (::WaitForSingleObject(info.hProcess, waitMs) == WAIT_OBJECT_0)
                ::GetExitCodeProcess(info.hProcess, &code);
            else
                ::TerminateProcess(info.hProcess, 99);
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
            return static_cast<int>(code);
        }

        void CheckOnClr()
        {
            const std::span<const std::uint8_t> image = Target::ManagedDumperImage();
            if (image.empty())
                return;
            std::error_code error;
            const std::filesystem::path folder = std::filesystem::temp_directory_path(error) / ("manifold-dumper-test-" + std::to_string(::GetCurrentProcessId()));
            std::filesystem::create_directories(folder, error);
            const std::filesystem::path dll = folder / L"ManifoldDumper.dll";
            const std::filesystem::path out = folder / L"dump.bin";
            {
                std::ofstream file(dll, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
            }
            const int code = RunChild(L"--managed-dump-clr --dll=\"" + dll.wstring() + L"\" --out=\"" + out.wstring() + L"\"", 120000);
            std::vector<std::uint8_t> blob;
            {
                std::ifstream file(out, std::ios::binary);
                blob.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
            std::filesystem::remove(dll, error);
            std::filesystem::remove(out, error);
            std::filesystem::remove(folder, error);
            if (code == 3)
            {
                Skip("no .NET Framework 4 runtime to run the managed dumper on");
                return;
            }
            CHECK_MSG(code == 0, "the dumper child ended with " + std::to_string(code));
            CHECK(!blob.empty());
            if (blob.empty())
                return;
            const Result<Target::ManagedDump> parsed = Target::ParseManagedDump(blob);
            CHECK_MSG(parsed.Ok(), parsed.Error().Text);
            if (!parsed)
                return;
            const Target::ManagedDump& dump = *parsed;
            CHECK(!dump.MonoHandles);
            CHECK_MSG(dump.Classes.size() > 1000, std::to_string(dump.Classes.size()) + " classes");
            const Target::MonoClass* text = Find(dump.Classes, "System.String");
            CHECK(text != nullptr);
            if (text)
            {
                CHECK_EQ(text->Image, std::string("mscorlib"));
                CHECK_EQ(text->Parent, std::string("System.Object"));
                CHECK(!text->Fields.empty());
                CHECK(std::all_of(text->Fields.begin(), text->Fields.end(), [](const Target::MonoField& field) { return field.Offset == -1; }));
                const auto concat = std::find_if(text->Methods.begin(), text->Methods.end(), [](const Target::MonoMethod& method) { return method.Name == "Concat"; });
                CHECK(concat != text->Methods.end() && concat->ParamCount == -1);
                CHECK(std::any_of(text->Methods.begin(), text->Methods.end(), [](const Target::MonoMethod& method) { return method.Name == ".ctor"; }));
            }
            const Target::MonoClass* list = Find(dump.Classes, "System.Collections.Generic.List`1");
            CHECK(list && list->Generic);
            const Target::MonoClass* number = Find(dump.Classes, "System.Int32");
            CHECK(number && number->Parent == "System.ValueType");
            CHECK(std::any_of(dump.Classes.begin(), dump.Classes.end(), [](const Target::MonoClass& info) { return info.Name == "Enumerator" && info.Namespace.empty(); }));
            CHECK(std::none_of(dump.Classes.begin(), dump.Classes.end(), [](const Target::MonoClass& info) { return Target::IsDumperImage(info.Image); }));
        }

        struct AgentTarget
        {
            PROCESS_INFORMATION Info{};

            ~AgentTarget()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }

            bool Start(const std::filesystem::path& path, const std::wstring& mode)
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
                std::wstring command = L"\"" + path.wstring() + L"\" " + mode;
                const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &Info);
                ::CloseHandle(write);
                std::string text;
                char buffer[512];
                DWORD got = 0;
                while (started && text.find("shoot at") == std::string::npos)
                {
                    if (!::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
                        break;
                    text.append(buffer, got);
                }
                ::CloseHandle(read);
                return started && text.find("shoot at") != std::string::npos;
            }
        };

        struct AgentStats
        {
            std::int32_t Loads = 0;
            std::int32_t Begins = 0;
            std::int32_t Releases = 0;
            std::int32_t ImageBytes = 0;
            std::int32_t Live = 0;
            std::int32_t Cancels = 0;
        };

        AgentStats ReadStats(const Target::Process& process)
        {
            AgentStats stats;
            const std::uint64_t address = process.FindExport(process.Name(), "g_agentStats");
            if (address)
                process.ReadExact(address, &stats, sizeof(stats));
            return stats;
        }

        std::string Describe(const AgentStats& stats)
        {
            return "loads " + std::to_string(stats.Loads) + ", begins " + std::to_string(stats.Begins) + ", releases " + std::to_string(stats.Releases) +
                ", live " + std::to_string(stats.Live) + ", image " + std::to_string(stats.ImageBytes) + " bytes";
        }

        void CheckAgentMode(const std::filesystem::path& targetPath, const std::wstring& mode)
        {
            const std::string label = WideToUtf8(mode);
            AgentTarget target;
            const bool started = target.Start(targetPath, mode);
            CHECK_MSG(started, "could not start the test target with " + label);
            if (!started)
                return;
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(target.Info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return;
            const Status mono = engine.EnsureMono();
            CHECK_MSG(mono.Ok(), label + ": " + mono.Text);
            if (!mono)
                return;
            const Target::Process& process = *engine.Process();
            const std::size_t embedded = Target::ManagedDumperImage().size();
            const bool good = mode == L"--mono-agent";
            const bool badHandles = mode == L"--mono-agent=bad-handles";
            const bool badOffsets = mode == L"--mono-agent=bad-offsets" || mode == L"--mono-agent=unsure";
            const bool viaDump = good || badOffsets;

            const std::vector<Target::MonoClass> classes = engine.Mono().Classes();
            CHECK_MSG(classes.size() == 2, label + ": " + std::to_string(classes.size()) + " classes");
            const AgentStats first = ReadStats(process);
            CHECK_MSG(first.Loads == 1 && first.Begins == 1 && first.Releases == 1 && first.Live == 0, label + ": " + Describe(first));
            CHECK_MSG(first.ImageBytes == static_cast<std::int32_t>(embedded), label + ": " + Describe(first));

            const std::optional<Target::MonoClass> player = engine.Mono().ClassDetails("Player");
            CHECK(player.has_value());
            if (player)
            {
                CHECK_MSG(player->Parent == (viaDump ? "UnityEngine.MonoBehaviour" : ""), label + ": parent '" + player->Parent + "'");
                CHECK_EQ(player->Methods.size(), 2u);
                if (!player->Methods.empty())
                    CHECK_MSG(player->Methods[0].ParamCount == (good ? 1 : -1), label + ": " + std::to_string(player->Methods[0].ParamCount) + " parameters");
                CHECK_EQ(player->Fields.size(), 2u);
                if (player->Fields.size() == 2)
                {
                    CHECK(player->Fields[0].Name == "health" && player->Fields[0].Offset == 0x10 && player->Fields[0].TypeCode == 0x08);
                    CHECK(player->Fields[1].Name == "ammo" && player->Fields[1].Offset == 0x1C);
                }
            }
            CHECK_MSG(engine.Mono().ClassDetails("Game.GameManager").has_value(), label + ": the nested class lost its namespace");
            const auto take = engine.Mono().AddressOf("Game.Player:TakeDamage");
            CHECK_MSG(take && *take, label + ": Player:TakeDamage did not resolve");
            CHECK(engine.Mono().FieldOffset("Player", "ammo") == 0x1C);

            if (good || badHandles)
            {
                const Status refreshed = engine.Mono().Refresh();
                CHECK_MSG(refreshed.Ok(), refreshed.Text);
                const AgentStats second = ReadStats(process);
                const std::int32_t begins = good ? 2 : 1;
                CHECK_MSG(second.Loads == 1 && second.Begins == begins && second.Releases == begins && second.Live == 0, label + " after a refresh: " + Describe(second));
                CHECK_EQ(engine.Mono().Classes().size(), 2u);
            }
            engine.CloseProcess();
        }

        void CheckAgentTarget(const std::filesystem::path& targetPath)
        {
            if (Target::ManagedDumperImage().empty())
                return;
            for (const wchar_t* mode : { L"--mono-agent", L"--mono-agent=bad-handles", L"--mono-agent=bad-offsets", L"--mono-agent=unsure", L"--mono-agent=fail" })
                CheckAgentMode(targetPath, mode);
        }
    }

    void CheckManagedDumper(const std::filesystem::path& targetPath)
    {
        CheckParser();
        CheckEmbeddedImage();
        CheckOnClr();
        CheckAgentTarget(targetPath);
    }

    int RunManagedDumpClr(const std::vector<std::string>& arguments)
    {
        std::wstring dll;
        std::wstring out;
        for (const std::string& argument : arguments)
        {
            if (argument.rfind("--dll=", 0) == 0)
                dll = Utf8ToWide(argument.substr(6));
            else if (argument.rfind("--out=", 0) == 0)
                out = Utf8ToWide(argument.substr(6));
        }
        if (dll.empty() || out.empty())
            return 2;
        HMODULE mscoree = ::LoadLibraryW(L"mscoree.dll");
        const auto create = mscoree ? reinterpret_cast<CLRCreateInstanceFnPtr>(reinterpret_cast<void*>(::GetProcAddress(mscoree, "CLRCreateInstance"))) : nullptr;
        if (!create)
            return 3;
        constexpr GUID kMetaHost = { 0x9280188D, 0x0E8E, 0x4867, { 0xB3, 0x0C, 0x7F, 0xA8, 0x38, 0x84, 0xE8, 0xDE } };
        constexpr GUID kRuntimeHost = { 0x90F1A06E, 0x7712, 0x4762, { 0x86, 0xB5, 0x7A, 0x5E, 0xBA, 0x6B, 0xDB, 0x02 } };
        ICLRMetaHost* host = nullptr;
        ICLRRuntimeInfo* info = nullptr;
        ICLRRuntimeHost* runtime = nullptr;
        if (FAILED(create(kMetaHost, __uuidof(ICLRMetaHost), reinterpret_cast<void**>(&host))) || !host)
            return 3;
        if (FAILED(host->GetRuntime(L"v4.0.30319", __uuidof(ICLRRuntimeInfo), reinterpret_cast<void**>(&info))) || !info)
            return 3;
        if (FAILED(info->GetInterface(kRuntimeHost, __uuidof(ICLRRuntimeHost), reinterpret_cast<void**>(&runtime))) || !runtime)
            return 3;
        if (FAILED(runtime->Start()))
            return 3;
        DWORD size = 0;
        const HRESULT result = runtime->ExecuteInDefaultAppDomain(dll.c_str(), L"Manifold.Managed.Dumper", L"DumpToFile", out.c_str(), &size);
        std::printf("DumpToFile: 0x%08lX, %lu bytes\n", static_cast<unsigned long>(result), static_cast<unsigned long>(size));
        return SUCCEEDED(result) && size > 0 ? 0 : 1;
    }
}
