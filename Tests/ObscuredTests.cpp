#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Scan/Obscured.h"
#include "Scan/Scanner.h"
#include "Scan/TypeCatalog.h"
#include "Table/ManagedStructures.h"
#include "Target/Process.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        using namespace Manifold::Scan;

        const ObscuredType& TypeOf(ObscuredKind kind)
        {
            for (const ObscuredType& type : ObscuredTypes())
            {
                if (type.Kind == kind)
                    return type;
            }
            return ObscuredTypes().front();
        }

        template <class T>
        std::vector<std::uint8_t> Plain(T value)
        {
            std::vector<std::uint8_t> bytes(sizeof(T));
            std::memcpy(bytes.data(), &value, sizeof(T));
            return bytes;
        }

        // A struct as ACTk 2.x leaves it after `value = x`: key, value ^ key, inited, and the fake copy when the detector runs.
        std::vector<std::uint8_t> Make(ObscuredKind kind, const std::vector<std::uint8_t>& plain, std::uint64_t key, bool fakeActive, int pointerSize = 8)
        {
            const ObscuredType& type = TypeOf(kind);
            const ObscuredLayout layout = ObscuredLayoutOf(kind, pointerSize);
            std::vector<std::uint8_t> zero(static_cast<std::size_t>(layout.Size), 0);
            std::memcpy(zero.data() + layout.Key, &key, static_cast<std::size_t>(layout.KeyBytes));
            zero[static_cast<std::size_t>(layout.Inited)] = 1;
            zero[static_cast<std::size_t>(layout.FakeActive)] = fakeActive ? 1 : 0;
            return ObscuredEncode(type, layout, zero, plain, 0);
        }

        std::string Decoded(ObscuredKind kind, const std::vector<std::uint8_t>& block, bool strict, ObscuredState* state = nullptr, int pointerSize = 8)
        {
            const ObscuredType& type = TypeOf(kind);
            std::uint8_t plain[16]{};
            const ObscuredState result = ObscuredDecode(type, ObscuredLayoutOf(kind, pointerSize), block.data(), plain, strict);
            if (state)
                *state = result;
            if (result == ObscuredState::Invalid)
                return "invalid";
            return ObscuredText(type, std::span<const std::uint8_t>(plain, static_cast<std::size_t>(ObscuredPlainSize(type))));
        }

        void CheckLayouts()
        {
            // The offsets IL2CPP gave for ACTk 2.x in a 64-bit game (boxed offsets minus the 0x10 object header).
            const ObscuredLayout integer = ObscuredLayoutOf(ObscuredKind::Int, 8);
            CHECK(integer.Key == 0 && integer.Hidden == 4 && integer.Inited == 8 && integer.Fake == 0xC && integer.FakeActive == 0x10 && integer.Size == 0x14);
            const ObscuredLayout floating = ObscuredLayoutOf(ObscuredKind::Float, 8);
            CHECK(floating.Hidden == 4 && floating.Old == 8 && floating.Inited == 0x10 && floating.Fake == 0x14 && floating.FakeActive == 0x18 && floating.Size == 0x20);
            const ObscuredLayout floating32 = ObscuredLayoutOf(ObscuredKind::Float, 4);
            CHECK(floating32.Old == 8 && floating32.Inited == 0xC && floating32.Fake == 0x10 && floating32.FakeActive == 0x14 && floating32.Size == 0x18);
            const ObscuredLayout dbl = ObscuredLayoutOf(ObscuredKind::Double, 8);
            CHECK(dbl.Hidden == 8 && dbl.Old == 0x10 && dbl.Inited == 0x18 && dbl.Fake == 0x20 && dbl.FakeActive == 0x28 && dbl.Size == 0x30);
            const ObscuredLayout lng = ObscuredLayoutOf(ObscuredKind::Long, 8);
            CHECK(lng.Hidden == 8 && lng.Inited == 0x10 && lng.Fake == 0x18 && lng.FakeActive == 0x20 && lng.Size == 0x28);
            const ObscuredLayout shrt = ObscuredLayoutOf(ObscuredKind::UShort, 8);
            CHECK(shrt.Hidden == 2 && shrt.Inited == 4 && shrt.Fake == 6 && shrt.FakeActive == 8);
            const ObscuredLayout byte = ObscuredLayoutOf(ObscuredKind::SByte, 8);
            CHECK(byte.Hidden == 1 && byte.Inited == 2 && byte.Fake == 3 && byte.FakeActive == 4 && byte.Size == 5);
            const ObscuredLayout boolean = ObscuredLayoutOf(ObscuredKind::Bool, 8);
            CHECK(boolean.KeyBytes == 1 && boolean.Hidden == 4 && boolean.Inited == 8 && boolean.Fake == 9 && boolean.FakeActive == 0xA);
            const ObscuredLayout vector2 = ObscuredLayoutOf(ObscuredKind::Vector2, 8);
            CHECK(vector2.Hidden == 4 && vector2.Inited == 0xC && vector2.Fake == 0x10 && vector2.FakeActive == 0x18);
            const ObscuredLayout vector3 = ObscuredLayoutOf(ObscuredKind::Vector3, 8);
            CHECK(vector3.Inited == 0x10 && vector3.Fake == 0x14 && vector3.FakeActive == 0x20);
            const ObscuredLayout quaternion = ObscuredLayoutOf(ObscuredKind::Quaternion, 8);
            CHECK(quaternion.Inited == 0x14 && quaternion.Fake == 0x18 && quaternion.FakeActive == 0x28);

            CHECK(FindObscured("actk obscuredint") == &TypeOf(ObscuredKind::Int));
            CHECK(FindObscuredClass("CodeStage.AntiCheat.ObscuredTypes.ObscuredFloat") == &TypeOf(ObscuredKind::Float));
            CHECK(FindObscuredClass("ObscuredBool") == &TypeOf(ObscuredKind::Bool));
            CHECK(FindObscuredClass("Game.ObscuredInt") == nullptr);
            CHECK(FindObscuredClass("CodeStage.AntiCheat.ObscuredTypes.ObscuredPrefs") == nullptr);
        }

        void CheckCodec()
        {
            const auto integer = Make(ObscuredKind::Int, Plain<std::int32_t>(1234), 0x1A2B3C4D, true);
            std::int32_t hidden = 0;
            std::memcpy(&hidden, integer.data() + 4, 4);
            CHECK_EQ(hidden, 1234 ^ 0x1A2B3C4D);
            CHECK_EQ(Decoded(ObscuredKind::Int, integer, true), "1234");
            auto negative = Make(ObscuredKind::Int, Plain<std::int32_t>(-7), 0x55, false);
            CHECK_EQ(Decoded(ObscuredKind::Int, negative, true), "-7");
            CHECK_EQ(Decoded(ObscuredKind::UInt, Make(ObscuredKind::UInt, Plain<std::uint32_t>(3000000000u), 0x99, false), true), "3000000000");
            CHECK_EQ(Decoded(ObscuredKind::Long, Make(ObscuredKind::Long, Plain<std::int64_t>(-5), 0x1122334455667788ll, true), true), "-5");
            CHECK_EQ(Decoded(ObscuredKind::Short, Make(ObscuredKind::Short, Plain<std::int16_t>(-300), 0x4242, false), true), "-300");
            CHECK_EQ(Decoded(ObscuredKind::Byte, Make(ObscuredKind::Byte, Plain<std::uint8_t>(200), 0x3C, false), true), "200");
            CHECK_EQ(Decoded(ObscuredKind::Float, Make(ObscuredKind::Float, Plain(3.5f), 0x2468ACE, true), true), "3.5");
            CHECK_EQ(Decoded(ObscuredKind::Double, Make(ObscuredKind::Double, Plain(-0.25), 0x13579BDF2468ACEll, false), true), "-0.25");
            const auto vector = Make(ObscuredKind::Vector3, [] { std::vector<std::uint8_t> v = Plain(1.0f); auto b = Plain(-2.5f); auto c = Plain(30.0f);
                v.insert(v.end(), b.begin(), b.end()); v.insert(v.end(), c.begin(), c.end()); return v; }(), 0x777, true);
            CHECK_EQ(Decoded(ObscuredKind::Vector3, vector, true), "1, -2.5, 30");

            // ACTk's bool: 213 for true, 181 for false, under a one-byte key.
            const auto yes = Make(ObscuredKind::Bool, { 1 }, 0x7B, false);
            std::uint32_t raw = 0;
            std::memcpy(&raw, yes.data() + 4, 4);
            CHECK_EQ(raw, 213u ^ 0x7Bu);
            CHECK_EQ(Decoded(ObscuredKind::Bool, yes, true), "True");
            CHECK_EQ(Decoded(ObscuredKind::Bool, Make(ObscuredKind::Bool, { 0 }, 0x7B, false), true), "False");

            ObscuredState state{};
            std::vector<std::uint8_t> unset(0x14, 0);
            CHECK_EQ(Decoded(ObscuredKind::Int, unset, false, &state), "0");
            CHECK(state == ObscuredState::Unset);
            CHECK_EQ(Decoded(ObscuredKind::Int, unset, true), "invalid");
            auto badFlag = integer;
            badFlag[8] = 2;
            CHECK_EQ(Decoded(ObscuredKind::Int, badFlag, false), "invalid");
            auto padded = integer;
            padded[9] = 0x40;
            CHECK_EQ(Decoded(ObscuredKind::Int, padded, true), "invalid");
            CHECK_EQ(Decoded(ObscuredKind::Int, padded, false), "1234");
            auto sameAsKey = Make(ObscuredKind::Long, Plain<std::int64_t>(14), 14, true);
            CHECK_EQ(Decoded(ObscuredKind::Long, sameAsKey, true), "invalid");
            CHECK_EQ(Decoded(ObscuredKind::Long, sameAsKey, false), "14");
            const std::vector<std::uint8_t> realTrue = { 0xD7, 0, 0, 0, 0x02, 0, 0, 0, 1, 1, 1, 0 };
            const std::vector<std::uint8_t> realFalse = { 0xD7, 0, 0, 0, 0x62, 0, 0, 0, 1, 0, 1, 0 };
            CHECK_EQ(Decoded(ObscuredKind::Bool, realTrue, true), "True");
            CHECK_EQ(Decoded(ObscuredKind::Bool, realFalse, true), "False");
            auto noKey = integer;
            std::memset(noKey.data(), 0, 4);
            CHECK_EQ(Decoded(ObscuredKind::Int, noKey, false), "invalid");
            // The honeypot: a fake copy that no longer matches is a changed fake, not a value.
            auto honeypot = integer;
            honeypot[0xC] ^= 0x01;
            CHECK_EQ(Decoded(ObscuredKind::Int, honeypot, true), "invalid");
            CHECK_EQ(Decoded(ObscuredKind::Int, honeypot, false), "1234");

            // A write keeps the key, moves the fake copy along and clears hiddenValueOld.
            const ObscuredType& intType = TypeOf(ObscuredKind::Int);
            const auto written = ObscuredEncode(intType, ObscuredLayoutOf(ObscuredKind::Int, 8), integer, Plain<std::int32_t>(99), 0xDEAD);
            CHECK(std::memcmp(written.data(), integer.data(), 4) == 0);
            CHECK_EQ(Decoded(ObscuredKind::Int, written, true), "99");
            const auto fresh = ObscuredEncode(intType, ObscuredLayoutOf(ObscuredKind::Int, 8), unset, Plain<std::int32_t>(5), 0);
            CHECK(fresh[8] == 1 && fresh[0x10] == 0);
            CHECK_EQ(Decoded(ObscuredKind::Int, fresh, true), "5");
            auto withOld = Make(ObscuredKind::Float, Plain(1.0f), 0x1234, false);
            std::memset(withOld.data() + 8, 0xAB, 8);
            const auto rewritten = ObscuredEncode(TypeOf(ObscuredKind::Float), ObscuredLayoutOf(ObscuredKind::Float, 8), withOld, Plain(2.0f), 1);
            CHECK(std::all_of(rewritten.begin() + 8, rewritten.begin() + 16, [](std::uint8_t b) { return b == 0; }));
            CHECK_EQ(Decoded(ObscuredKind::Float, rewritten, true), "2");

            CHECK(ObscuredParse(TypeOf(ObscuredKind::Bool), " TRUE ") == std::vector<std::uint8_t>{ 1 });
            CHECK(ObscuredParse(TypeOf(ObscuredKind::Bool), "maybe") == std::nullopt);
            CHECK(ObscuredParse(TypeOf(ObscuredKind::Vector2), "1.5, 2") == [] { auto a = Plain(1.5f); auto b = Plain(2.0f); a.insert(a.end(), b.begin(), b.end()); return a; }());
            CHECK(ObscuredParse(TypeOf(ObscuredKind::Vector3), "1, 2") == std::nullopt);

            CHECK(CompareAvailable(ScanKind::Custom, Compare::Bigger, "ACTk ObscuredInt"));
            CHECK(CompareAvailable(ScanKind::Custom, Compare::IncreasedBy, "ACTk ObscuredFloat"));
            CHECK(!CompareAvailable(ScanKind::Custom, Compare::Exact, "ACTk ObscuredVector3"));
            CHECK(CompareAvailable(ScanKind::Custom, Compare::Changed, "ACTk ObscuredVector3"));
            CHECK(!CompareAvailable(ScanKind::Custom, Compare::Bigger, "Big endian test"));

            TypeCatalogOptions catalog{ .Types = kScanTypes };
            catalog.Customs = { { "ACTk ObscuredInt", 0x14 } };
            bool listed = false;
            for (const TypeSection& section : TypeCatalog(catalog))
                listed = listed || (section.Title == "Unity Anti-Cheat Toolkit" && section.Entries.size() == 1);
            CHECK(listed);
        }

        // Structure Dissect types a field as ACTk only when the runtime's field offsets agree with the codec.
        void CheckDissectTyping()
        {
            const auto inner = [](int shift, int fakeAt)
                {
                    std::vector<Target::MonoField> fields;
                    for (const auto& [name, offset] : { std::pair<const char*, int>{ "currentCryptoKey", 0 }, { "hiddenValue", 4 }, { "inited", 8 }, { "fakeValue", fakeAt },
                             { "fakeValueActive", 0x10 } })
                    {
                        Target::MonoField field;
                        field.Name = name;
                        field.Offset = offset + shift;
                        fields.push_back(field);
                    }
                    return fields;
                };
            Target::MonoField field;
            field.Name = "money";
            field.Offset = 0x28;
            field.TypeName = "CodeStage.AntiCheat.ObscuredTypes.ObscuredInt";
            field.TypeHandle = 0x1000;
            field.TypeCode = 0x11;
            field.ValueSize = 0x14;
            Table::ManagedLayouts nested;
            nested[field.TypeName] = inner(0x10, 0xC);
            CHECK(Table::ObscuredFieldType(field, nested, 8) == std::string_view("ACTk ObscuredInt"));
            Table::StructDefinition definition;
            Table::FillFromManaged(definition, { field }, 0, nested, 8);
            CHECK(definition.Fields.size() == 1 && definition.Fields[0].CustomType == "ACTk ObscuredInt" && definition.Fields[0].ChildStructure == field.TypeName &&
                  definition.Fields[0].Size == 0x14 && definition.Fields[0].Offset == 0x28);
            nested[field.TypeName] = inner(0x10, 0x14);
            CHECK(Table::ObscuredFieldType(field, nested, 8) == nullptr);
            nested[field.TypeName] = inner(0x10, 0xC);
            field.TypeName = "Game.ObscuredInt";
            nested[field.TypeName] = inner(0x10, 0xC);
            CHECK(Table::ObscuredFieldType(field, nested, 8) == nullptr);
        }

        bool WaitScan(Engine& engine, int seconds = 12)
        {
            for (int i = 0; i < seconds * 50; ++i)
            {
                if (engine.Scanner().Poll())
                    return engine.Scanner().LastError().Ok();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        }

        bool Contains(Engine& engine, std::uint64_t address)
        {
            Scan::ScanRecord record;
            for (std::uint64_t i = 0; i < engine.Scanner().Count(); ++i)
            {
                if (engine.Scanner().Record(i, record) && record.Address == address)
                    return true;
            }
            return false;
        }

        // Scans, next scans and writes in this process, on one page with ACTk structs, a plain copy of the value and noise.
        void CheckScansAndWrites()
        {
            Settings settings;
            Engine engine(settings);
            CHECK(engine.OpenProcess(MakeProcessId(::GetCurrentProcessId())).Ok());
            if (!engine.Process())
                return;
            constexpr std::size_t kPage = 0x10000;
            auto* page = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            CHECK(page != nullptr);
            if (!page)
                return;
            std::uint32_t state = 7;
            for (std::size_t i = 0; i < kPage; ++i)
            {
                state = state * 1664525u + 1013904223u;
                page[i] = i < 0x8000 ? static_cast<std::uint8_t>(state >> 24) : 0;
            }
            const auto place = [&](std::size_t offset, const std::vector<std::uint8_t>& bytes) { std::memcpy(page + offset, bytes.data(), bytes.size()); };
            place(0x8100, Make(ObscuredKind::Int, Plain<std::int32_t>(1234), 0x1A2B3C4D, true));
            place(0x8200, Make(ObscuredKind::Int, Plain<std::int32_t>(1234), 0x600DF00D, false));
            auto honeypot = Make(ObscuredKind::Int, Plain<std::int32_t>(1234), 0x0BADCAFE, true);
            std::int32_t changedFake = 999;
            std::memcpy(honeypot.data() + 0xC, &changedFake, 4);
            place(0x8300, honeypot);
            const std::int32_t plainCopy = 1234;
            std::memcpy(page + 0x8400, &plainCopy, 4);
            place(0x8500, Make(ObscuredKind::Float, Plain(3.5f), 0x2468ACE, true));
            place(0x8600, Make(ObscuredKind::UInt, Plain<std::uint32_t>(3000000000u), 0x42, false));
            place(0x8700, Make(ObscuredKind::Bool, { 1 }, 0x7B, false));
            place(0x8800, Make(ObscuredKind::Long, Plain<std::int64_t>(-5), 0x1122334455667788ll, false));
            auto xyz = Plain(1.0f);
            for (float c : { 2.0f, 3.0f })
            {
                const auto more = Plain(c);
                xyz.insert(xyz.end(), more.begin(), more.end());
            }
            place(0x8900, Make(ObscuredKind::Vector3, xyz, 0x31337, false));
            const std::uint64_t base = reinterpret_cast<std::uint64_t>(page);

            const auto options = [&](const char* type, Compare mode, std::string value)
                {
                    ScanOptions scan;
                    scan.Kind = ScanKind::Custom;
                    scan.CustomName = type;
                    const Lua::CustomType* custom = engine.Lua().FindCustomType(type);
                    scan.CustomSize = custom ? custom->ByteCount() : 0;
                    scan.Mode = mode;
                    scan.Value = std::move(value);
                    scan.Start = base;
                    scan.Stop = base + kPage;
                    scan.Writable = Tristate::Any;
                    scan.Alignment = 4;
                    return scan;
                };
            const auto run = [&](const ScanOptions& scan, bool first)
                {
                    const Status started = engine.Scanner().Start(engine.Process(), scan, first);
                    CHECK_MSG(started.Ok(), started.Text);
                    return started.Ok() && WaitScan(engine);
                };

            CHECK(run(options("ACTk ObscuredInt", Compare::Exact, "1234"), true));
            CHECK(Contains(engine, base + 0x8100) && Contains(engine, base + 0x8200));
            CHECK(!Contains(engine, base + 0x8300) && !Contains(engine, base + 0x8400));
            CHECK_MSG(engine.Scanner().Count() == 2, Format("%llu results", static_cast<unsigned long long>(engine.Scanner().Count())));
            // A new key with the same value: the bytes change, the value does not.
            place(0x8100, Make(ObscuredKind::Int, Plain<std::int32_t>(1234), 0x0FEDCBA9, true));
            place(0x8200, Make(ObscuredKind::Int, Plain<std::int32_t>(1300), 0x600DF00D, false));
            CHECK(run(options("ACTk ObscuredInt", Compare::Unchanged, ""), false));
            CHECK(Contains(engine, base + 0x8100) && !Contains(engine, base + 0x8200));
            engine.Scanner().Undo();
            CHECK(run(options("ACTk ObscuredInt", Compare::IncreasedBy, "66"), false));
            CHECK(!Contains(engine, base + 0x8100) && Contains(engine, base + 0x8200));
            engine.Scanner().Reset();

            CHECK(run(options("ACTk ObscuredUInt", Compare::Bigger, "2999999999"), true));
            CHECK(Contains(engine, base + 0x8600));
            engine.Scanner().Reset();
            CHECK(run(options("ACTk ObscuredFloat", Compare::Exact, "3.5"), true));
            CHECK(Contains(engine, base + 0x8500));
            engine.Scanner().Reset();
            CHECK(run(options("ACTk ObscuredBool", Compare::Exact, "true"), true));
            CHECK(Contains(engine, base + 0x8700));
            engine.Scanner().Reset();
            ScanOptions between = options("ACTk ObscuredLong", Compare::Between, "-6");
            between.Value2 = "-4";
            CHECK(run(between, true));
            CHECK(Contains(engine, base + 0x8800));
            engine.Scanner().Reset();
            CHECK_EQ(engine.Scanner().Start(engine.Process(), options("ACTk ObscuredVector3", Compare::Exact, "1, 2, 3"), true).Code, ErrorCode::ScanCompareUnavailable);
            CHECK(run(options("ACTk ObscuredVector3", Compare::Unknown, ""), true));
            CHECK(Contains(engine, base + 0x8900));
            xyz = Plain(1.0f);
            for (float c : { 2.0f, 4.0f })
            {
                const auto more = Plain(c);
                xyz.insert(xyz.end(), more.begin(), more.end());
            }
            place(0x8900, Make(ObscuredKind::Vector3, xyz, 0x31337, false));
            CHECK(run(options("ACTk ObscuredVector3", Compare::Changed, ""), false));
            CHECK(Contains(engine, base + 0x8900));
            engine.Scanner().Reset();

            // The custom types: the address list and Structure Dissect go through these.
            const Lua::CustomType* type = engine.Lua().FindCustomType("ACTk ObscuredInt");
            CHECK(type != nullptr);
            if (type)
            {
                std::string text;
                CHECK(engine.Lua().CustomToText(*type, std::span<const std::uint8_t>(page + 0x8100, 0x14), base + 0x8100, text));
                CHECK_EQ(text, "1234");
                std::vector<std::uint8_t> bytes;
                CHECK(engine.Lua().CustomFromText(*type, "777", base + 0x8100, bytes));
                CHECK(bytes.size() == 0x14 && std::memcmp(bytes.data(), page + 0x8100, 4) == 0);
                CHECK(engine.Process()->Write(base + 0x8100, bytes).Ok());
                CHECK_EQ(Decoded(ObscuredKind::Int, std::vector<std::uint8_t>(page + 0x8100, page + 0x8114), true), "777");
                std::int32_t fake = 0;
                std::memcpy(&fake, page + 0x8100 + 0xC, 4);
                CHECK_EQ(fake, 777);
                CHECK(!engine.Lua().CustomFromText(*type, "777", base + 0x10, bytes));
                CHECK(!engine.Lua().CustomFromText(*type, "seven", base + 0x8200, bytes));
                CHECK(engine.Lua().CustomFromText(*type, "5", 0, bytes) && bytes.size() == 0x14 && bytes[8] == 1);
            }
            const Lua::CustomType* vectorType = engine.Lua().FindCustomType("ACTk ObscuredVector3");
            std::string vectorText;
            CHECK(vectorType && engine.Lua().CustomToText(*vectorType, std::span<const std::uint8_t>(page + 0x8900, 0x24), base + 0x8900, vectorText) && vectorText == "1, 2, 4");
            const Lua::CustomType* floatType = engine.Lua().FindCustomType("ACTk ObscuredFloat");
            CHECK(floatType && floatType->ByteCount() == 0x20);
            ::VirtualFree(page, 0, MEM_RELEASE);
        }

        // Read only: how many structs in a running game read as each ACTk type, and how many of them carry the
        // honeypot copy, which has to equal the decrypted value (a check of the codec against the real game).
        int RunObscuredScan(const std::vector<std::string>& arguments)
        {
            std::uint32_t pid = 0;
            std::vector<std::uint64_t> watched;
            for (const std::string& argument : arguments)
            {
                if (argument.rfind("--pid=", 0) == 0)
                    pid = static_cast<std::uint32_t>(ParseDecimal(argument.substr(6)).value_or(0));
                else if (argument.rfind("--keys=", 0) == 0)
                {
                    for (const std::string& key : Split(argument.substr(7), ','))
                        watched.push_back(ParseDecimal(key).value_or(0));
                }
            }
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(pid));
            if (!opened)
            {
                std::printf("could not open %u: %s\n", pid, opened.Text.c_str());
                return 2;
            }
            std::printf("opened %s\n", engine.Process()->Name().c_str());
            for (const char* name : { "ACTk ObscuredInt", "ACTk ObscuredFloat", "ACTk ObscuredLong", "ACTk ObscuredBool", "ACTk ObscuredDouble" })
            {
                const ObscuredType* type = FindObscured(name);
                const ObscuredLayout layout = ObscuredLayoutOf(type->Kind, engine.Process()->PointerSize());
                ScanOptions scan;
                scan.Kind = ScanKind::Custom;
                scan.CustomName = name;
                scan.CustomSize = layout.Size;
                scan.Mode = Compare::Between;
                switch (type->Kind)
                {
                case ObscuredKind::Int: scan.Value = "-2147483648"; scan.Value2 = "2147483647"; break;
                case ObscuredKind::Long: scan.Value = "-9223372036854775808"; scan.Value2 = "9223372036854775807"; break;
                case ObscuredKind::Float: scan.Value = "-1e38"; scan.Value2 = "1e38"; break;
                case ObscuredKind::Double: scan.Value = "-1e300"; scan.Value2 = "1e300"; break;
                default: scan.Value = "0"; scan.Value2 = "1"; break;
                }
                scan.Alignment = 4;
                const auto start = std::chrono::steady_clock::now();
                const Status started = engine.Scanner().Start(engine.Process(), scan, true);
                if (!started || !WaitScan(engine, 300))
                {
                    std::printf("%s: scan failed: %s\n", name, started ? engine.Scanner().LastError().Text.c_str() : started.Text.c_str());
                    continue;
                }
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                std::uint64_t honeypots = 0;
                struct KeyCount
                {
                    std::uint64_t Count = 0;
                    std::uint64_t Honeypots = 0;
                    std::vector<std::string> Samples;
                };
                std::map<std::uint64_t, KeyCount> keys;
                Scan::ScanRecord record;
                for (std::uint64_t i = 0; i < engine.Scanner().Count(); ++i)
                {
                    if (!engine.Scanner().Record(i, record) || static_cast<int>(record.Previous.size()) < layout.Size)
                        continue;
                    std::uint64_t key = 0;
                    std::memcpy(&key, record.Previous.data() + layout.Key, static_cast<std::size_t>(layout.KeyBytes));
                    KeyCount& entry = keys[key];
                    ++entry.Count;
                    const bool honeypot = record.Previous[static_cast<std::size_t>(layout.FakeActive)] == 1;
                    if (honeypot)
                    {
                        ++honeypots;
                        ++entry.Honeypots;
                    }
                    std::uint8_t plain[16]{};
                    ObscuredDecode(*type, layout, record.Previous.data(), plain, true);
                    if (entry.Samples.size() < 5)
                        entry.Samples.push_back(Hex(record.Address) + "=" + ObscuredText(*type, std::span<const std::uint8_t>(plain, static_cast<std::size_t>(ObscuredPlainSize(*type)))) +
                            (honeypot ? "*" : ""));
                }
                std::printf("%-20s %8llu candidates, %6llu with an active honeypot copy that matches, %zu keys (%.1fs)\n", name,
                    static_cast<unsigned long long>(engine.Scanner().Count()), static_cast<unsigned long long>(honeypots), keys.size(), seconds);
                std::vector<std::pair<std::uint64_t, const KeyCount*>> ranked;
                for (const auto& [key, entry] : keys)
                    ranked.push_back({ key, &entry });
                std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second->Count > b.second->Count; });
                for (const std::uint64_t key : watched)
                {
                    if (const auto found = keys.find(key); found != keys.end())
                        ranked.insert(ranked.begin(), { key, &found->second });
                }
                for (std::size_t k = 0; k < ranked.size() && k < 8 + watched.size(); ++k)
                {
                    std::printf("    key %llu (0x%llX): %llu, %llu honeypot:", static_cast<unsigned long long>(ranked[k].first), static_cast<unsigned long long>(ranked[k].first),
                        static_cast<unsigned long long>(ranked[k].second->Count), static_cast<unsigned long long>(ranked[k].second->Honeypots));
                    for (const std::string& sample : ranked[k].second->Samples)
                        std::printf(" %s", sample.c_str());
                    std::printf("\n");
                }
                engine.Scanner().Reset();
            }
            std::printf("done; detaching\n");
            return 0;
        }

        std::string Option(const std::vector<std::string>& arguments, const std::string& name)
        {
            const std::string prefix = "--" + name + "=";
            for (const std::string& argument : arguments)
            {
                if (argument.rfind(prefix, 0) == 0)
                    return argument.substr(prefix.size());
            }
            return {};
        }

        struct LiveTarget
        {
            std::unique_ptr<Settings> Config = std::make_unique<Settings>();
            std::unique_ptr<Engine> Host;
            const ObscuredType* Type = nullptr;
            ObscuredLayout Layout;
        };

        bool OpenLive(const std::vector<std::string>& arguments, LiveTarget& live)
        {
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            live.Type = FindObscured(Option(arguments, "type"));
            if (!live.Type)
            {
                std::printf("--type= names no ACTk type\n");
                return false;
            }
            live.Host = std::make_unique<Engine>(*live.Config);
            const auto pid = static_cast<std::uint32_t>(ParseDecimal(Option(arguments, "pid")).value_or(0));
            const Status opened = live.Host->OpenProcess(MakeProcessId(pid));
            if (!opened)
            {
                std::printf("could not open %u: %s\n", pid, opened.Text.c_str());
                return false;
            }
            live.Layout = ObscuredLayoutOf(live.Type->Kind, live.Host->Process()->PointerSize());
            return true;
        }

        std::string Describe(const LiveTarget& live, std::uint64_t address, const std::vector<std::uint8_t>& block)
        {
            std::uint64_t key = 0;
            std::memcpy(&key, block.data() + live.Layout.Key, static_cast<std::size_t>(live.Layout.KeyBytes));
            std::uint8_t plain[16]{};
            const ObscuredState state = ObscuredDecode(*live.Type, live.Layout, block.data(), plain, false);
            std::uint8_t fake[16]{};
            std::memcpy(fake, block.data() + live.Layout.Fake, static_cast<std::size_t>(ObscuredPlainSize(*live.Type)));
            const std::span<const std::uint8_t> size(plain, static_cast<std::size_t>(ObscuredPlainSize(*live.Type)));
            return Format("%s  key %llu  value %s  fake %s (%s)  %s", Hex(address).c_str(), static_cast<unsigned long long>(key),
                state == ObscuredState::Invalid ? "invalid" : ObscuredText(*live.Type, size).c_str(),
                ObscuredText(*live.Type, std::span<const std::uint8_t>(fake, size.size())).c_str(),
                block[static_cast<std::size_t>(live.Layout.FakeActive)] ? "active" : "inactive", live.Host->Describe(address).c_str());
        }

        // Read only: an exact scan for an ACTk value, or with --from= the addresses of an earlier run that still hold it.
        // --pid= --type="ACTk ObscuredInt" --value=1234 [--from=file] [--out=file]
        int RunObscuredFind(const std::vector<std::string>& arguments)
        {
            LiveTarget live;
            if (!OpenLive(arguments, live))
                return 2;
            const std::string value = Option(arguments, "value");
            const auto wanted = ObscuredParse(*live.Type, value);
            if (!wanted)
            {
                std::printf("--value=%s is no %s\n", value.c_str(), live.Type->Name);
                return 2;
            }
            std::vector<std::uint64_t> found;
            const std::string from = Option(arguments, "from");
            if (from.empty())
            {
                ScanOptions scan;
                scan.Kind = ScanKind::Custom;
                scan.CustomName = live.Type->Name;
                scan.CustomSize = live.Layout.Size;
                scan.Value = value;
                scan.Alignment = 4;
                const Status started = live.Host->Scanner().Start(live.Host->Process(), scan, true);
                if (!started || !WaitScan(*live.Host, 300))
                {
                    std::printf("scan failed: %s\n", started ? live.Host->Scanner().LastError().Text.c_str() : started.Text.c_str());
                    return 1;
                }
                Scan::ScanRecord record;
                for (std::uint64_t i = 0; i < live.Host->Scanner().Count(); ++i)
                {
                    if (live.Host->Scanner().Record(i, record))
                        found.push_back(record.Address);
                }
            }
            else
            {
                FILE* file = nullptr;
                if (fopen_s(&file, from.c_str(), "rb") != 0 || !file)
                {
                    std::printf("cannot read %s\n", from.c_str());
                    return 2;
                }
                char line[64];
                while (std::fgets(line, sizeof(line), file))
                {
                    const auto address = ParseHex(Trim(line));
                    if (!address)
                        continue;
                    std::vector<std::uint8_t> block(static_cast<std::size_t>(live.Layout.Size));
                    std::uint8_t plain[16]{};
                    if (live.Host->Process()->ReadExact(*address, block.data(), block.size()) &&
                        ObscuredDecode(*live.Type, live.Layout, block.data(), plain, true) == ObscuredState::Plain &&
                        std::memcmp(plain, wanted->data(), wanted->size()) == 0)
                        found.push_back(*address);
                }
                std::fclose(file);
            }
            std::printf("%zu address(es) hold %s as %s\n", found.size(), value.c_str(), live.Type->Name);
            for (std::size_t i = 0; i < found.size() && i < 25; ++i)
            {
                std::vector<std::uint8_t> block(static_cast<std::size_t>(live.Layout.Size));
                if (live.Host->Process()->ReadExact(found[i], block.data(), block.size()))
                    std::printf("  %s\n", Describe(live, found[i], block).c_str());
            }
            const std::string out = Option(arguments, "out");
            if (!out.empty())
            {
                FILE* file = nullptr;
                if (fopen_s(&file, out.c_str(), "wb") == 0 && file)
                {
                    for (const std::uint64_t address : found)
                        std::fprintf(file, "%s\n", Hex(address).c_str());
                    std::fclose(file);
                }
            }
            std::printf("done; detaching\n");
            return 0;
        }

        // Writes one ACTk value the way the address list does: through the custom type, under the key in memory.
        // --pid= --type= --address=hex --value=
        int RunObscuredWrite(const std::vector<std::string>& arguments)
        {
            LiveTarget live;
            if (!OpenLive(arguments, live))
                return 2;
            const auto address = ParseHex(Option(arguments, "address"));
            const Lua::CustomType* custom = live.Host->Lua().FindCustomType(live.Type->Name);
            if (!address || !custom)
            {
                std::printf("--address= missing\n");
                return 2;
            }
            std::vector<std::uint8_t> block(static_cast<std::size_t>(live.Layout.Size));
            if (!live.Host->Process()->ReadExact(*address, block.data(), block.size()))
            {
                std::printf("cannot read %s\n", Hex(*address).c_str());
                return 1;
            }
            std::printf("before  %s\n", Describe(live, *address, block).c_str());
            std::vector<std::uint8_t> bytes;
            if (!live.Host->Lua().CustomFromText(*custom, Option(arguments, "value"), *address, bytes))
            {
                std::printf("refused: the value does not parse or the bytes are no %s\n", live.Type->Name);
                return 1;
            }
            const Status written = live.Host->Process()->Write(*address, bytes);
            std::printf("write   %s\n", written ? "ok" : written.Text.c_str());
            live.Host->Process()->ReadExact(*address, block.data(), block.size());
            std::printf("after   %s\n", Describe(live, *address, block).c_str());
            std::printf("done; detaching\n");
            return written ? 0 : 1;
        }

        int RunObscuredTests()
        {
            const int before = Totals().Failures;
            CheckLayouts();
            CheckCodec();
            CheckDissectTyping();
            CheckScansAndWrites();
            return Totals().Failures - before;
        }
    }

    MANIFOLD_MODE("--actk-scan", &RunObscuredScan);
    MANIFOLD_MODE("--actk-find", &RunObscuredFind);
    MANIFOLD_MODE("--actk-write", &RunObscuredWrite);

    MANIFOLD_SUITE("actk", "Anti-Cheat Toolkit values", 134, [](const SuiteContext&) { return RunObscuredTests(); });
}
