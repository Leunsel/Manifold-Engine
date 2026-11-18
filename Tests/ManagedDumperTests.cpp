#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Target/ManagedAgent.h"
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
#include <map>
#include <memory>
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

    namespace
    {
        void CheckAgentProtocol()
        {
            namespace P = Target::AgentProtocol;
            Target::AgentRequest request;
            request.Op(P::SetName).I32(7).Text("abc");
            request.Op(P::Stop);
            const std::vector<std::uint8_t> bytes = request.Finish();
            CHECK_EQ(request.Count(), 2u);
            CHECK_EQ(bytes.size(), 4u + 8u + 4u + 4u + 3u + 8u);
            if (bytes.size() == 31)
            {
                std::uint32_t words[5] = {};
                std::memcpy(words, bytes.data(), 4 * 5);
                CHECK(words[0] == 2 && words[1] == P::SetName && words[2] == 11 && words[3] == 7 && words[4] == 3);
                std::uint32_t stop[2] = {};
                std::memcpy(stop, bytes.data() + 23, 8);
                CHECK(stop[0] == P::Stop && stop[1] == 0);
            }

            std::vector<std::uint8_t> response;
            const auto put = [&](std::uint32_t value) { const auto* raw = reinterpret_cast<const std::uint8_t*>(&value); response.insert(response.end(), raw, raw + 4); };
            put(2);
            put(P::Scenes);
            put(0);
            put(4);
            put(42);
            put(P::Invoke);
            put(1);
            put(8);
            put(4);
            response.insert(response.end(), { 'b', 'o', 'o', 'm' });
            const Result<std::vector<Target::AgentReply>> parsed = Target::ParseAgentReplies(response);
            CHECK_MSG(parsed.Ok(), parsed.Error().Text);
            if (parsed && parsed->size() == 2)
            {
                Target::AgentCursor in((*parsed)[0].Payload);
                CHECK((*parsed)[0].Ok() && in.I32() == 42 && in.Ok());
                in.I32();
                CHECK(!in.Ok());
                CHECK(!(*parsed)[1].Ok() && (*parsed)[1].Error() == "boom");
            }
            bool prefixes = true;
            for (std::size_t size = 0; size < response.size(); ++size)
                prefixes = prefixes && !Target::ParseAgentReplies(std::span<const std::uint8_t>(response.data(), size)).Ok();
            CHECK(prefixes);
            std::vector<std::uint8_t> longer = response;
            longer.push_back(0);
            CHECK(!Target::ParseAgentReplies(longer).Ok());
            std::vector<std::uint8_t> huge(4, 0xFF);
            CHECK(!Target::ParseAgentReplies(huge).Ok());
            std::vector<std::uint8_t> negative = { 0xFF, 0xFF, 0xFF, 0xFF };
            Target::AgentCursor in(negative);
            CHECK(in.Bytes().empty() && !in.Ok());
        }

        ICLRRuntimeHost* StartClr()
        {
            HMODULE mscoree = ::LoadLibraryW(L"mscoree.dll");
            const auto create = mscoree ? reinterpret_cast<CLRCreateInstanceFnPtr>(reinterpret_cast<void*>(::GetProcAddress(mscoree, "CLRCreateInstance"))) : nullptr;
            if (!create)
                return nullptr;
            constexpr GUID kMetaHost = { 0x9280188D, 0x0E8E, 0x4867, { 0xB3, 0x0C, 0x7F, 0xA8, 0x38, 0x84, 0xE8, 0xDE } };
            constexpr GUID kRuntimeHost = { 0x90F1A06E, 0x7712, 0x4762, { 0x86, 0xB5, 0x7A, 0x5E, 0xBA, 0x6B, 0xDB, 0x02 } };
            ICLRMetaHost* host = nullptr;
            ICLRRuntimeInfo* info = nullptr;
            ICLRRuntimeHost* runtime = nullptr;
            if (FAILED(create(kMetaHost, __uuidof(ICLRMetaHost), reinterpret_cast<void**>(&host))) || !host)
                return nullptr;
            if (FAILED(host->GetRuntime(L"v4.0.30319", __uuidof(ICLRRuntimeInfo), reinterpret_cast<void**>(&info))) || !info)
                return nullptr;
            if (FAILED(info->GetInterface(kRuntimeHost, __uuidof(ICLRRuntimeHost), reinterpret_cast<void**>(&runtime))) || !runtime)
                return nullptr;
            if (FAILED(runtime->Start()))
                return nullptr;
            return runtime;
        }

        std::string Argument(const std::vector<std::string>& arguments, const std::string& prefix)
        {
            for (const std::string& argument : arguments)
            {
                if (argument.rfind(prefix, 0) == 0)
                    return argument.substr(prefix.size());
            }
            return {};
        }

        int RunChildCaptured(const std::wstring& arguments, DWORD waitMs, std::string& output)
        {
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE read = nullptr;
            HANDLE write = nullptr;
            if (!::CreatePipe(&read, &write, &security, 0))
                return -1;
            ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = write;
            startup.hStdError = write;
            std::wstring command = L"\"" + std::wstring(self) + L"\" " + arguments;
            PROCESS_INFORMATION info{};
            const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
            ::CloseHandle(write);
            if (!started)
            {
                ::CloseHandle(read);
                return -1;
            }
            char buffer[1024];
            DWORD got = 0;
            while (::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got > 0)
                output.append(buffer, got);
            ::CloseHandle(read);
            DWORD code = static_cast<DWORD>(-2);
            if (::WaitForSingleObject(info.hProcess, waitMs) == WAIT_OBJECT_0)
                ::GetExitCodeProcess(info.hProcess, &code);
            else
                ::TerminateProcess(info.hProcess, 99);
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
            return static_cast<int>(code);
        }

        void CheckAgentOnClr()
        {
            const std::span<const std::uint8_t> image = Target::ManagedDumperImage();
            if (image.empty())
                return;
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            const std::filesystem::path fake = std::filesystem::path(self).parent_path().parent_path().parent_path() / L"Managed" / L"UnityEngine.CoreModule.dll";
            std::error_code error;
            if (!std::filesystem::exists(fake, error))
            {
                Skip("the fake UnityEngine assembly was not built, so the agent is not run on the .NET Framework");
                return;
            }
            const std::filesystem::path folder = std::filesystem::temp_directory_path(error) / ("manifold-agent-test-" + std::to_string(::GetCurrentProcessId()));
            std::filesystem::create_directories(folder, error);
            const std::filesystem::path dll = folder / L"ManifoldDumper.dll";
            const std::filesystem::path engine = folder / L"UnityEngine.CoreModule.dll";
            {
                std::ofstream file(dll, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
            }
            std::filesystem::copy_file(fake, engine, std::filesystem::copy_options::overwrite_existing, error);
            std::string output;
            const int code = RunChildCaptured(L"--managed-agent-clr --dll=\"" + dll.wstring() + L"\" --fake=\"" + engine.wstring() + L"\" --dir=\"" + folder.wstring() + L"\"",
                120000, output);
            for (const wchar_t* name : { L"ManifoldDumper.dll", L"UnityEngine.CoreModule.dll", L"manifest.txt", L"request.bin", L"response.bin" })
                std::filesystem::remove(folder / name, error);
            std::filesystem::remove(folder, error);
            if (code == 3)
            {
                Skip("no .NET Framework 4 runtime to run the managed agent on");
                return;
            }
            CHECK_MSG(code == 0, "the agent child ended with " + std::to_string(code) + "\n" + output);
        }
    }

    void CheckManagedDumper(const std::filesystem::path& targetPath)
    {
        CheckParser();
        CheckAgentProtocol();
        CheckEmbeddedImage();
        CheckOnClr();
        CheckAgentTarget(targetPath);
        CheckAgentOnClr();
    }

    int RunManagedDumpClr(const std::vector<std::string>& arguments)
    {
        const std::wstring dll = Utf8ToWide(Argument(arguments, "--dll="));
        const std::wstring out = Utf8ToWide(Argument(arguments, "--out="));
        if (dll.empty() || out.empty())
            return 2;
        ICLRRuntimeHost* runtime = StartClr();
        if (!runtime)
            return 3;
        DWORD size = 0;
        const HRESULT result = runtime->ExecuteInDefaultAppDomain(dll.c_str(), L"Manifold.Managed.Dumper", L"DumpToFile", out.c_str(), &size);
        std::printf("DumpToFile: 0x%08lX, %lu bytes\n", static_cast<unsigned long>(result), static_cast<unsigned long>(size));
        return SUCCEEDED(result) && size > 0 ? 0 : 1;
    }

    int RunManagedAgentClr(const std::vector<std::string>& arguments)
    {
        namespace P = Target::AgentProtocol;
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        const std::wstring dll = Utf8ToWide(Argument(arguments, "--dll="));
        const std::wstring fake = Utf8ToWide(Argument(arguments, "--fake="));
        const std::filesystem::path folder = Utf8ToWide(Argument(arguments, "--dir="));
        if (dll.empty() || fake.empty() || folder.empty())
            return 2;
        ICLRRuntimeHost* runtime = StartClr();
        if (!runtime)
            return 3;
        const std::filesystem::path manifestPath = folder / L"manifest.txt";
        DWORD built = 0;
        HRESULT result = runtime->ExecuteInDefaultAppDomain(fake.c_str(), L"Game.FakeWorld", L"Build", manifestPath.wstring().c_str(), &built);
        CHECK_MSG(SUCCEEDED(result) && built >= 5, "FakeWorld.Build: " + Hex(static_cast<std::uint32_t>(result)));
        std::map<std::string, std::string> manifest;
        {
            std::ifstream file(manifestPath);
            std::string line;
            while (std::getline(file, line))
            {
                const std::size_t equals = line.find('=');
                if (equals != std::string::npos)
                    manifest[line.substr(0, equals)] = Trim(line.substr(equals + 1));
            }
        }
        const auto token = [&](const char* name) { return static_cast<std::uint32_t>(ParseDecimal(manifest[name]).value_or(0)); };
        const std::string engineImage = manifest["image"];
        CHECK(!engineImage.empty() && token("heal") != 0 && token("getspeed") != 0 && token("health") != 0);

        struct Answer
        {
            std::vector<std::uint8_t> Bytes;
            std::vector<Target::AgentReply> Replies;
        };
        const std::filesystem::path requestPath = folder / L"request.bin";
        const std::filesystem::path responsePath = folder / L"response.bin";
        const auto run = [&](Target::AgentRequest& request, std::size_t expected) -> std::unique_ptr<Answer>
            {
                const std::vector<std::uint8_t> bytes = request.Finish();
                {
                    std::ofstream file(requestPath, std::ios::binary | std::ios::trunc);
                    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                }
                const std::wstring paths = requestPath.wstring() + L"|" + responsePath.wstring();
                DWORD ok = 0;
                const HRESULT ran = runtime->ExecuteInDefaultAppDomain(dll.c_str(), L"Manifold.Managed.Agent", L"RunScript", paths.c_str(), &ok);
                CHECK_MSG(SUCCEEDED(ran) && ok == 1, "RunScript: " + Hex(static_cast<std::uint32_t>(ran)));
                auto answer = std::make_unique<Answer>();
                {
                    std::ifstream file(responsePath, std::ios::binary);
                    answer->Bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                }
                Result<std::vector<Target::AgentReply>> parsed = Target::ParseAgentReplies(answer->Bytes);
                CHECK_MSG(parsed.Ok(), parsed.Error().Text);
                if (parsed)
                    answer->Replies = std::move(*parsed);
                CHECK_EQ(answer->Replies.size(), expected);
                answer->Replies.resize(expected);
                return answer;
            };
        const auto ok = [](const Target::AgentReply& reply, const char* what)
            {
                CHECK_MSG(reply.Ok(), std::string(what) + ": " + reply.Error());
                return reply.Ok();
            };
        const auto bytesOf = [](float a, float b, float c)
            {
                std::vector<std::uint8_t> bytes(12);
                std::memcpy(bytes.data(), &a, 4);
                std::memcpy(bytes.data() + 4, &b, 4);
                std::memcpy(bytes.data() + 8, &c, 4);
                return bytes;
            };
        const auto valueBytes = [](Target::AgentCursor& in) -> std::vector<std::uint8_t>
            {
                if (in.U8() != P::ValueBytes)
                    return {};
                return in.Bytes();
            };
        const auto valueText = [](Target::AgentCursor& in) -> std::string
            {
                if (in.U8() != P::ValueText)
                    return "<not text>";
                in.I32();
                return in.Text();
            };

        Target::AgentRequest first;
        first.Op(P::Info);
        first.Op(P::Scenes).I32(1);
        const auto scenes = run(first, 2);
        int mainSlot = 0;
        if (ok(scenes->Replies[0], "Info"))
        {
            Target::AgentCursor in(scenes->Replies[0].Payload);
            CHECK_EQ(in.Text(), std::string("6000.0.0f1-fake"));
            CHECK_EQ(in.I32(), 0);
            in.Text();
            CHECK(in.Ok());
        }
        if (ok(scenes->Replies[1], "Scenes"))
        {
            Target::AgentCursor in(scenes->Replies[1].Payload);
            CHECK_EQ(in.I32(), 1);
            const std::int32_t count = in.I32();
            CHECK_EQ(count, 2);
            for (std::int32_t i = 0; i < count && in.Ok(); ++i)
            {
                const int slot = in.I32();
                const int handle = in.I32();
                const std::string name = in.Text();
                in.I32();
                const std::int32_t roots = in.I32();
                in.U8();
                if (name == "Main")
                {
                    mainSlot = slot;
                    CHECK_EQ(handle, 1);
                    CHECK_EQ(roots, 2);
                }
            }
            CHECK(in.Ok() && mainSlot > 0);
        }

        Target::AgentRequest second;
        second.Op(P::Roots).I32(1).I32(mainSlot).I32(100);
        second.Op(P::Roots).I32(1).I32(0).I32(100);
        const auto roots = run(second, 2);
        int worldObject = 0;
        int worldTransform = 0;
        if (ok(roots->Replies[0], "Roots of Main"))
        {
            Target::AgentCursor in(roots->Replies[0].Payload);
            in.I32();
            CHECK_EQ(in.I32(), 1);
            CHECK_EQ(in.I32(), 2);
            const std::int32_t count = in.I32();
            CHECK_EQ(count, 2);
            for (std::int32_t i = 0; i < count && in.Ok(); ++i)
            {
                const int object = in.I32();
                const int transform = in.I32();
                const std::string name = in.Text();
                const bool active = in.U8() != 0;
                const std::int32_t children = in.I32();
                if (name == "World")
                {
                    worldObject = object;
                    worldTransform = transform;
                    CHECK(active && children == 2);
                }
                else
                    CHECK(name == "Lights" && !active && children == 0);
            }
            CHECK(in.Ok() && worldObject > 0 && worldTransform > 0);
        }
        if (ok(roots->Replies[1], "Roots of DontDestroyOnLoad"))
        {
            Target::AgentCursor in(roots->Replies[1].Payload);
            CHECK(in.I32() > 0);
            CHECK_EQ(in.I32(), -1);
            CHECK_EQ(in.I32(), 0);
        }

        Target::AgentRequest third;
        third.Op(P::Children).I32(1).I32(worldTransform).I32(100);
        const auto children = run(third, 1);
        int heroObject = 0;
        int heroTransform = 0;
        if (ok(children->Replies[0], "Children"))
        {
            Target::AgentCursor in(children->Replies[0].Payload);
            CHECK_EQ(in.I32(), 2);
            const std::int32_t count = in.I32();
            for (std::int32_t i = 0; i < count && in.Ok(); ++i)
            {
                const int object = in.I32();
                const int transform = in.I32();
                const std::string name = in.Text();
                in.U8();
                in.I32();
                if (name == "Player")
                {
                    heroObject = object;
                    heroTransform = transform;
                }
            }
            CHECK(in.Ok() && heroObject > 0 && heroTransform > 0);
        }

        Target::AgentRequest fourth;
        fourth.Op(P::GameObject).I32(2).I32(heroObject).I32(10);
        const auto described = run(fourth, 1);
        int player = 0;
        if (ok(described->Replies[0], "GameObject"))
        {
            Target::AgentCursor in(described->Replies[0].Payload);
            CHECK_EQ(in.Text(), std::string("Player"));
            CHECK_EQ(in.Text(), std::string("Player"));
            CHECK_EQ(in.I32(), 8);
            CHECK_EQ(in.U8(), 1);
            CHECK_EQ(in.U8(), 1);
            CHECK_EQ(in.Text(), std::string("Main"));
            CHECK_EQ(in.I32(), heroTransform);
            CHECK_EQ(in.U8(), 1);
            float values[12] = {};
            for (float& value : values)
                value = in.F32();
            CHECK(values[0] == 4.0f && values[1] == 5.0f && values[2] == 6.0f && values[6] == 1.0f && values[9] == 4.0f);
            CHECK_EQ(in.I32(), worldObject);
            CHECK_EQ(in.Text(), std::string("World"));
            CHECK_EQ(in.I32(), 0);
            CHECK_EQ(in.I32(), 0);
            const std::int32_t components = in.I32();
            CHECK_EQ(components, 2);
            for (std::int32_t i = 0; i < components && in.Ok(); ++i)
            {
                const int slot = in.I32();
                const bool switchable = in.U8() != 0;
                const bool enabled = in.U8() != 0;
                if (switchable)
                {
                    player = slot;
                    CHECK(enabled);
                }
            }
            CHECK(in.Ok() && player > 0);
        }

        const std::uint64_t pidFunction = reinterpret_cast<std::uint64_t>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId"));
        std::int32_t five = 5;
        float nine = 9.0f;
        Target::AgentRequest fifth;
        fifth.Op(P::UnityObject).I32(2).I32(player);
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("getspeed")).I32(player).I32(4).I32(0);
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("heal")).I32(player).I32(4).I32(1).U8(P::ArgBytes)
            .Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&five), 4));
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("offset")).I32(player).I32(12).I32(1).U8(P::ArgBytes).Bytes(bytesOf(1.0f, 1.0f, 1.0f));
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("describe")).I32(player).I32(8).I32(0);
        fifth.Op(P::Fields).I32(2).I32(player).I32(3).Text(engineImage).U32(token("health")).I32(4).Text(engineImage).U32(token("title")).I32(8)
            .Text(engineImage).U32(token("target")).I32(8);
        fifth.Op(P::SetField).I32(player).Text(engineImage).U32(token("title")).U8(P::ArgText).Text("champ");
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("describe")).I32(player).I32(8).I32(0);
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("setspeed")).I32(player).I32(0).I32(1).U8(P::ArgBytes)
            .Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&nine), 4));
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("getspeed")).I32(player).I32(4).I32(0);
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("explode")).I32(player).I32(0).I32(0);
        fifth.Op(P::Invoke).I32(2).Text(engineImage).U32(token("heal")).I32(player).I32(4).I32(0);
        fifth.Op(P::Find).I32(3).Text(engineImage).U32(token("player")).U8(0).Text("pla").I32(10);
        fifth.Op(P::Fields).I32(2).I32(player).I32(1).Text(engineImage).U32(token("friends")).I32(8);
        fifth.Op(P::SetActive).I32(worldObject).U8(0);
        fifth.Op(P::GameObject).I32(2).I32(heroObject).I32(0);
        fifth.Op(P::SetActive).I32(worldObject).U8(1);
        fifth.Op(P::SetName).I32(heroObject).Text("Hero");
        fifth.Op(P::SetVector).I32(heroTransform).I32(0).F32(7.0f).F32(8.0f).F32(9.0f);
        fifth.Op(P::SetEnabled).I32(player).U8(0);
        fifth.Op(P::UnityObject).I32(2).I32(player);
        fifth.Op(P::RunInBackground).I32(1);
        fifth.Op(P::RunInBackground).I32(0);
        fifth.Op(P::Native).I64(static_cast<std::int64_t>(pidFunction)).I64(0).I64(0).I64(0).I64(0);
        fifth.Op(P::GameObject).I32(2).I32(heroObject).I32(0);
        const auto calls = run(fifth, 25);
        const std::vector<Target::AgentReply>& r = calls->Replies;
        if (ok(r[0], "UnityObject"))
        {
            Target::AgentCursor in(r[0].Payload);
            CHECK_EQ(in.Text(), std::string("Player"));
            CHECK_EQ(in.I32(), heroObject);
            CHECK_EQ(in.Text(), std::string("Player"));
            CHECK_EQ(in.U8(), 1);
            CHECK_EQ(in.U8(), 0);
        }
        if (ok(r[1], "Speed"))
        {
            Target::AgentCursor in(r[1].Payload);
            const std::vector<std::uint8_t> bytes = valueBytes(in);
            float speed = 0.0f;
            if (bytes.size() == 4)
                std::memcpy(&speed, bytes.data(), 4);
            CHECK_EQ(speed, 3.5f);
        }
        if (ok(r[2], "Heal"))
        {
            Target::AgentCursor in(r[2].Payload);
            const std::vector<std::uint8_t> bytes = valueBytes(in);
            std::int32_t health = 0;
            if (bytes.size() == 4)
                std::memcpy(&health, bytes.data(), 4);
            CHECK_EQ(health, 105);
        }
        if (ok(r[3], "Offset"))
        {
            Target::AgentCursor in(r[3].Payload);
            CHECK(valueBytes(in) == bytesOf(2.0f, 3.0f, 4.0f));
        }
        if (ok(r[4], "Describe"))
        {
            Target::AgentCursor in(r[4].Payload);
            CHECK_EQ(valueText(in), std::string("hero:105"));
        }
        int cameraSlot = 0;
        if (ok(r[5], "Fields"))
        {
            Target::AgentCursor in(r[5].Payload);
            const std::vector<std::uint8_t> health = valueBytes(in);
            CHECK(health.size() == 4 && health[0] == 105);
            CHECK_EQ(valueText(in), std::string("hero"));
            CHECK_EQ(in.U8(), P::ValueObject);
            cameraSlot = in.I32();
            CHECK(in.Ok() && cameraSlot > 0);
        }
        ok(r[6], "SetField");
        if (ok(r[7], "Describe after SetField"))
        {
            Target::AgentCursor in(r[7].Payload);
            CHECK_EQ(valueText(in), std::string("champ:105"));
        }
        if (ok(r[8], "Speed setter"))
        {
            Target::AgentCursor in(r[8].Payload);
            CHECK_EQ(in.U8(), P::ValueNone);
        }
        if (ok(r[9], "Speed after the setter"))
        {
            Target::AgentCursor in(r[9].Payload);
            const std::vector<std::uint8_t> bytes = valueBytes(in);
            float speed = 0.0f;
            if (bytes.size() == 4)
                std::memcpy(&speed, bytes.data(), 4);
            CHECK_EQ(speed, 9.0f);
        }
        CHECK_MSG(!r[10].Ok() && r[10].Error().find("boom") != std::string::npos, r[10].Error());
        CHECK_MSG(!r[11].Ok() && r[11].Error().find("argument") != std::string::npos, r[11].Error());
        if (ok(r[12], "Find"))
        {
            Target::AgentCursor in(r[12].Payload);
            CHECK_EQ(in.I32(), 1);
            CHECK_EQ(in.I32(), 1);
            CHECK_EQ(in.I32(), player);
            CHECK_EQ(in.Text(), std::string("Player"));
        }
        int friends = 0;
        if (ok(r[13], "Fields friends"))
        {
            Target::AgentCursor in(r[13].Payload);
            CHECK_EQ(in.U8(), P::ValueObject);
            friends = in.I32();
            CHECK(in.Ok() && friends > 0);
        }
        ok(r[14], "SetActive off");
        if (ok(r[15], "GameObject under an inactive parent"))
        {
            Target::AgentCursor in(r[15].Payload);
            in.Text();
            in.Text();
            in.I32();
            CHECK_EQ(in.U8(), 1);
            CHECK_EQ(in.U8(), 0);
        }
        ok(r[16], "SetActive on");
        ok(r[17], "SetName");
        ok(r[18], "SetVector");
        ok(r[19], "SetEnabled");
        if (ok(r[20], "UnityObject after SetEnabled"))
        {
            Target::AgentCursor in(r[20].Payload);
            in.Text();
            in.I32();
            in.Text();
            CHECK_EQ(in.U8(), 0);
        }
        if (ok(r[21], "RunInBackground on"))
        {
            Target::AgentCursor in(r[21].Payload);
            CHECK_EQ(in.I32(), 1);
            CHECK_EQ(in.U8(), 1);
        }
        if (ok(r[22], "RunInBackground off"))
        {
            Target::AgentCursor in(r[22].Payload);
            CHECK_EQ(in.I32(), 0);
            CHECK_EQ(in.U8(), 0);
        }
        if (ok(r[23], "Native"))
        {
            Target::AgentCursor in(r[23].Payload);
            CHECK_EQ(static_cast<std::uint32_t>(in.I64()), static_cast<std::uint32_t>(::GetCurrentProcessId()));
        }
        if (ok(r[24], "GameObject after the edits"))
        {
            Target::AgentCursor in(r[24].Payload);
            CHECK_EQ(in.Text(), std::string("Hero"));
            in.Text();
            in.I32();
            CHECK_EQ(in.U8(), 1);
            CHECK_EQ(in.U8(), 1);
            in.Text();
            in.I32();
            CHECK_EQ(in.U8(), 1);
            const float x = in.F32();
            const float y = in.F32();
            const float z = in.F32();
            CHECK(x == 7.0f && y == 8.0f && z == 9.0f);
        }

        Target::AgentRequest sixth;
        sixth.Op(P::Elements).I32(2).I32(friends).I32(0).I32(10);
        sixth.Op(P::Hold).I32(5).I32(1).I32(player);
        sixth.Op(P::Release).I32(2);
        sixth.Op(P::Invoke).I32(5).Text(engineImage).U32(token("describe")).I32(player).I32(8).I32(0);
        sixth.Op(P::Invoke).I32(5).Text(engineImage).U32(token("describe")).I32(friends).I32(8).I32(0);
        sixth.Op(P::Children).I32(1).I32(worldTransform).I32(1);
        const auto released = run(sixth, 6);
        const std::vector<Target::AgentReply>& q = released->Replies;
        if (ok(q[0], "Elements"))
        {
            Target::AgentCursor in(q[0].Payload);
            CHECK_EQ(in.I32(), 2);
            CHECK_EQ(in.I32(), 2);
            CHECK_EQ(in.I32(), cameraSlot);
            CHECK_EQ(in.I32(), worldObject);
        }
        ok(q[1], "Hold");
        if (ok(q[2], "Release"))
        {
            Target::AgentCursor in(q[2].Payload);
            CHECK(in.I32() > 0);
        }
        if (ok(q[3], "Describe on a held object"))
        {
            Target::AgentCursor in(q[3].Payload);
            CHECK_EQ(valueText(in), std::string("champ:105"));
        }
        CHECK_MSG(!q[4].Ok() && q[4].Error().find("no longer holds") != std::string::npos, q[4].Error());
        ok(q[5], "Children of the tree group after the release");

        const int failures = Totals().Failures;
        std::printf("agent on the CLR: %d checks, %d failed\n", Totals().Checks, failures);
        return failures == 0 ? 0 : 10 + std::min(failures, 200);
    }
}
