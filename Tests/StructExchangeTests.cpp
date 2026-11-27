#include "TestKit.h"

#include "Core/Text.h"
#include "Table/StructExchange.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        using namespace Manifold::Table;
        using Scan::ValueType;

        StructField Field(int offset, std::string name, ValueType type, int size, FieldDisplay display = FieldDisplay::Unsigned)
        {
            StructField field;
            field.Offset = offset;
            field.Name = std::move(name);
            field.Type = type;
            field.Size = size;
            field.Display = display;
            return field;
        }

        // Player embeds Vec3 and points to Enemy; every kind of field the export writes.
        void Sample(StructureStore& store)
        {
            StructDefinition& vec = store.Add("Vec3");
            vec.Fields = { Field(0, "x", ValueType::Float, 4), Field(4, "y", ValueType::Float, 4), Field(8, "z", ValueType::Float, 4) };
            StructDefinition& enemy = store.Add("Enemy");
            enemy.Fields = { Field(8, "hp", ValueType::FourBytes, 4, FieldDisplay::Signed) };
            StructDefinition& player = store.Add("Player data");
            player.Fields.push_back(Field(0x10, "health", ValueType::FourBytes, 4, FieldDisplay::Signed));
            player.Fields.push_back(Field(0x12, "overlap", ValueType::TwoBytes, 2));
            player.Fields.push_back(Field(0x14, "speed", ValueType::Float, 4));
            StructField name = Field(0x18, "name", ValueType::String, 16);
            player.Fields.push_back(name);
            StructField wide = Field(0x28, "wide name", ValueType::String, 32);
            wide.Unicode = true;
            player.Fields.push_back(wide);
            StructField pos = Field(0x48, "pos", ValueType::ByteArray, 12);
            pos.ChildStructure = "Vec3";
            player.Fields.push_back(pos);
            StructField target = Field(0x58, "target", ValueType::Pointer, 8, FieldDisplay::Hex);
            target.ChildStructure = "Enemy";
            player.Fields.push_back(target);
            StructField flagA = Field(0x60, "alive", ValueType::Binary, 1);
            flagA.BitStart = 0;
            flagA.BitLength = 1;
            StructField flagB = Field(0x60, "team", ValueType::Binary, 1);
            flagB.BitStart = 3;
            flagB.BitLength = 3;
            player.Fields.push_back(flagA);
            player.Fields.push_back(flagB);
            player.Fields.push_back(Field(0x64, "raw", ValueType::ByteArray, 6));
            player.Fields.push_back(Field(0x70, "money", ValueType::EightBytes, 8, FieldDisplay::Hex));
            player.Fields.push_back(Field(0x78, "ratio", ValueType::Double, 8));
        }

        const StructDefinition* Named(const std::vector<StructDefinition>& structures, std::string_view name)
        {
            for (const StructDefinition& definition : structures)
            {
                if (definition.Name == name)
                    return &definition;
            }
            return nullptr;
        }

        const StructField* At(const StructDefinition* definition, int offset, std::string_view name = {})
        {
            if (!definition)
                return nullptr;
            for (const StructField& field : definition->Fields)
            {
                if (field.Offset == offset && (name.empty() || field.Name == name))
                    return &field;
            }
            return nullptr;
        }

        std::string Offsets(const StructDefinition* definition)
        {
            std::string out;
            if (!definition)
                return "(missing)";
            for (const StructField& field : definition->Fields)
                out += Format("%s@%X/%d ", field.Name.c_str(), field.Offset, field.Size);
            return out;
        }

        StructImport Import(std::string_view text, bool is64Bit = true)
        {
            StructImportOptions options;
            options.Is64Bit = is64Bit;
            Result<StructImport> result = ImportStructures(text, options);
            CHECK_MSG(result.Ok(), result ? std::string() : result.Error().Text);
            return result ? std::move(*result) : StructImport();
        }

        void CheckIdentifier()
        {
            CHECK(Identifier("Player data") == "Player_data");
            CHECK(Identifier("2nd") == "_2nd");
            CHECK(Identifier("a--b") == "a_b");
            CHECK(Identifier("") == "unnamed");
            CHECK(Identifier("int") == "int_");
            CHECK(Identifier("health") == "health");
        }

        void CheckExportCpp()
        {
            StructureStore store;
            Sample(store);
            const std::string text = ExportStructures(store, { "Player data" }, {});
            CHECK_MSG(text.find("struct Player_data\n{") != std::string::npos && text.find("// Player data") != std::string::npos, text);
            CHECK_MSG(text.find("struct Vec3\n{") < text.find("struct Player_data\n{"), text);  // embedded first
            CHECK_MSG(text.find("struct Enemy\n{") != std::string::npos, text);
            CHECK_MSG(text.find("    uint8_t pad_0000[0x10];") != std::string::npos, text);
            CHECK_MSG(text.find("    int32_t health; // +0x10") != std::string::npos, text);
            CHECK_MSG(text.find("    // +0x12 overlaps the field before: uint16_t overlap\n") != std::string::npos, text);
            CHECK_MSG(text.find("    char name[0x10];") != std::string::npos, text);
            CHECK_MSG(text.find("    char16_t wide_name[0x10]; // +0x28 wide name") != std::string::npos, text);
            CHECK_MSG(text.find("    Vec3 pos;") != std::string::npos && text.find("    Enemy* target;") != std::string::npos, text);
            CHECK_MSG(text.find("    uint8_t alive : 1;") != std::string::npos && text.find("    uint8_t : 2;") != std::string::npos &&
                text.find("    uint8_t team : 3;") != std::string::npos, text);
            CHECK_MSG(text.find("    uint8_t raw[0x6];") != std::string::npos && text.find("    uint64_t money;") != std::string::npos, text);
            CHECK_MSG(text.find("static_assert(sizeof(Player_data) == 0x80);") != std::string::npos, text);
            CHECK_MSG(text.find("static_assert(offsetof(Player_data, target) == 0x58);") != std::string::npos, text);
            CHECK_MSG(text.find("offsetof(Player_data, alive)") == std::string::npos, text);  // no offsetof on a bit field
            CHECK(text.find("#pragma pack(push, 1)") != std::string::npos && text.find("#pragma pack(pop)") != std::string::npos);

            StructExportOptions only;
            only.Children = false;
            only.Checks = false;
            only.OffsetComments = false;
            const std::string alone = ExportStructures(store, { "Player data" }, only);
            CHECK_MSG(alone.find("struct Vec3\n{") == std::string::npos && alone.find("    uint8_t pos[0xC]; // Vec3 (not exported)") != std::string::npos, alone);
            CHECK_MSG(alone.find("    void* target; // points to Enemy (not exported)") != std::string::npos, alone);
            CHECK(alone.find("static_assert") == std::string::npos && alone.find("// +0x10") == std::string::npos);

            StructExportOptions x86;
            x86.Is64Bit = false;
            const std::string narrow = ExportStructures(store, { "Player data" }, x86);
            CHECK_MSG(narrow.find("    uint64_t target; // +0x58 pointer to Enemy") != std::string::npos, narrow);  // an 8-byte pointer field in 32-bit code
        }

        void CheckRoundTripCpp()
        {
            StructureStore store;
            Sample(store);
            const std::string text = ExportStructures(store, { "Player data" }, {});
            const StructImport imported = Import(text);
            CHECK_MSG(imported.Warnings.empty(), imported.Warnings.empty() ? std::string() : imported.Warnings.front());
            const StructDefinition* player = Named(imported.Structures, "Player_data");
            CHECK_MSG(player && player->Size() == 0x80, Offsets(player));
            const StructField* health = At(player, 0x10, "health");
            CHECK(health && health->Type == ValueType::FourBytes && health->Display == FieldDisplay::Signed);
            const StructField* name = At(player, 0x18, "name");
            CHECK(name && name->Type == ValueType::String && name->Size == 16 && !name->Unicode);
            const StructField* wide = At(player, 0x28, "wide_name");
            CHECK(wide && wide->Type == ValueType::String && wide->Size == 32 && wide->Unicode);
            const StructField* pos = At(player, 0x48, "pos");
            CHECK(pos && pos->Type == ValueType::ByteArray && pos->ChildStructure == "Vec3" && pos->Size == 12);
            const StructField* target = At(player, 0x58, "target");
            CHECK(target && target->Type == ValueType::Pointer && target->ChildStructure == "Enemy" && target->Size == 8);
            const StructField* team = At(player, 0x60, "team");
            CHECK_MSG(team && team->Type == ValueType::Binary && team->BitStart == 3 && team->BitLength == 3, Offsets(player));
            const StructField* raw = At(player, 0x64, "raw");
            CHECK(raw && raw->Type == ValueType::ByteArray && raw->Size == 6);
            CHECK(At(player, 0x0) == nullptr);  // the padding gave no field
            const StructField* ratio = At(player, 0x78, "ratio");
            CHECK(ratio && ratio->Type == ValueType::Double);
            const StructDefinition* enemy = Named(imported.Structures, "Enemy");
            CHECK_MSG(enemy && enemy->Size() == 0xC && At(enemy, 8, "hp"), Offsets(enemy));
        }

        void CheckMsvcLayout()
        {
            const StructImport natural = Import("struct A { char c; int i; short s; double d; };");
            const StructDefinition* a = Named(natural.Structures, "A");
            CHECK_MSG(a && At(a, 0, "c") && At(a, 4, "i") && At(a, 8, "s") && At(a, 16, "d") && a->Size() == 24, Offsets(a));

            const StructImport packed = Import("#pragma pack(push, 1)\nstruct P { char c; int i; short s; double d; };\n#pragma pack(pop)\nstruct Q { char c; int i; };");
            const StructDefinition* p = Named(packed.Structures, "P");
            CHECK_MSG(p && At(p, 1, "i") && At(p, 5, "s") && At(p, 7, "d") && p->Size() == 15, Offsets(p));
            CHECK_MSG(At(Named(packed.Structures, "Q"), 4, "i"), Offsets(Named(packed.Structures, "Q")));

            const StructImport mixed = Import(
                "// a comment { with braces }\n"
                "typedef struct _Inner { int x; int y; } Inner, *PInner;\n"
                "enum class Team : uint8_t { Red, Blue };\n"
                "struct B {\n"
                "  char a;\n"
                "  union { int asInt; float asFloat; };\n"
                "  Inner inner; PInner next;\n"
                "  int arr[3];\n"
                "  wchar_t w[4];\n"
                "  unsigned f1 : 3, f2 : 5; unsigned g : 30;\n"
                "  Team team;\n"
                "  static int counter;\n"
                "  void Method(int x) { return; }\n"
                "  int Get() const;\n"
                "  BYTE b; DWORD d; uintptr_t up; HANDLE h;\n"
                "};\n");
            CHECK_MSG(mixed.Warnings.empty(), mixed.Warnings.empty() ? std::string() : mixed.Warnings.front());
            const StructDefinition* b = Named(mixed.Structures, "B");
            CHECK_MSG(b && At(b, 4, "asInt") && At(b, 4, "asFloat") && At(b, 8, "inner") && At(b, 0x10, "next"), Offsets(b));
            CHECK_MSG(At(b, 8, "inner") && At(b, 8, "inner")->ChildStructure == "Inner" && At(b, 0x10, "next")->ChildStructure == "Inner", Offsets(b));
            CHECK_MSG(At(b, 0x18, "arr[0]") && At(b, 0x20, "arr[2]") && At(b, 0x24, "w") && At(b, 0x24, "w")->Unicode, Offsets(b));
            const StructField* f2 = At(b, 0x2C, "f2");
            CHECK_MSG(At(b, 0x2C, "f1") && f2 && f2->BitStart == 3 && f2->BitLength == 5 && At(b, 0x30, "g"), Offsets(b));
            CHECK_MSG(At(b, 0x34, "team") && At(b, 0x34, "team")->Size == 1 && At(b, 0x35, "b") && At(b, 0x38, "d"), Offsets(b));
            CHECK_MSG(At(b, 0x40, "up") && At(b, 0x48, "h") && At(b, 0x48, "h")->Type == ValueType::Pointer && b->Size() == 0x50, Offsets(b));
            CHECK(At(b, 0, "counter") == nullptr);

            const StructImport narrow = Import("struct N { char c; void* p; size_t n; double d; };", false);
            const StructDefinition* n = Named(narrow.Structures, "N");
            CHECK_MSG(n && At(n, 4, "p") && At(n, 4, "p")->Size == 4 && At(n, 8, "n") && At(n, 16, "d") && n->Size() == 24, Offsets(n));

            const StructImport classes = Import(
                "class Base { public: virtual ~Base(); int id; };\n"
                "class Derived : public Base { public: virtual void Tick(); float speed; };\n"
                "struct Plain { int a; };\n"
                "struct More : Plain { char b; };\n");
            const StructDefinition* base = Named(classes.Structures, "Base");
            CHECK_MSG(base && At(base, 0, "vftable") && At(base, 8, "id") && base->Size() == 16, Offsets(base));
            const StructDefinition* derived = Named(classes.Structures, "Derived");
            CHECK_MSG(derived && At(derived, 0, "vftable") && At(derived, 8, "id") && At(derived, 16, "speed") && derived->Size() == 24, Offsets(derived));
            const StructDefinition* more = Named(classes.Structures, "More");
            CHECK_MSG(more && At(more, 0, "a") && At(more, 4, "b") && more->Size() == 8 && !At(more, 4, "padding"), Offsets(more));

            const StructImport unknown = Import("struct U { int a; Mystery m; int b; };");
            CHECK_MSG(!unknown.Warnings.empty() && unknown.Warnings.front().find("line 1") != std::string::npos && unknown.Warnings.front().find("Mystery") != std::string::npos,
                unknown.Warnings.empty() ? std::string() : unknown.Warnings.front());

            const StructImport later = Import("struct L { struct Node* next; int v; };\nstruct Node { int x; };");
            const StructDefinition* l = Named(later.Structures, "L");
            CHECK_MSG(l && At(l, 0, "next") && At(l, 0, "next")->ChildStructure == "Node" && At(l, 8, "v"), Offsets(l));

            CHECK(!ImportStructures("int x; void f();", {}).Ok());
            CHECK(ImportStructures("int x; void f();", {}).Error().Code == ErrorCode::StructNotFound);
        }

        void CheckHugeArrays()
        {
            static constexpr std::string_view kTexts[] = {
                "struct S { int a[1000000000]; int b; };",
                "struct S { char a[0x20000000]; int b; };",
                "struct S { int a[65536][65537]; int b; };",
                "struct S { int a[0x100000001]; int b; };",
                "struct S { struct Inner { int x; } a[65536][65537]; int b; };",
                "struct S { void (*a[65536][65537])(); int b; };",
                "struct S { int a[99999999999999999999]; int b; };",
                "struct S { int a[3][0x8000000000000001]; int b; };",
                "struct S { struct Inner { int x; } a[99999999999999999999]; int b; };",
                "struct S { void (*a[99999999999999999999])(); int b; };",
            };
            for (const std::string_view text : kTexts)
            {
                const StructImport imported = Import(text);
                const StructDefinition* s = Named(imported.Structures, "S");
                bool inside = s != nullptr;
                const StructField* b = nullptr;
                for (const StructField& field : s ? s->Fields : std::vector<StructField>())
                {
                    inside = inside && field.Offset >= 0 && field.Offset + field.Size <= kMaxFieldOffset;
                    if (field.Name == "b")
                        b = &field;
                }
                bool warned = false;
                bool cut = false;
                for (const std::string& warning : imported.Warnings)
                {
                    warned = warned || warning.find("b lies beyond") != std::string::npos;
                    cut = cut || warning.find(": a ") != std::string::npos;
                }
                CHECK_MSG(inside && !b && warned, std::string(text) + " -> " + Offsets(s));
                CHECK_MSG(At(s, 0, "a") && At(s, 0, "a")->Size == kMaxFieldBytes && cut, std::string(text) + " -> " + Offsets(s));
            }
        }

        void CheckFieldsPastOffsetLimit()
        {
            struct Case
            {
                std::string_view Text;
                int LastOffset;
                std::string_view Last;
                std::string_view Beyond;
            };
            static constexpr Case kCases[] = {
                { "struct S { char p[0xFFFFFF0]; int a[64]; };", 0xFFFFFFC, "a[3]", "a[4] lies beyond" },
                { "struct Inner { char x[0x8000000]; }; struct S { Inner a[20]; int b; };", 0x8000000, "a[1]", "a[2] lies beyond" },
                { "struct C { int x; }; struct S { char p[0xFFFFFF0]; void (C::*a[8])(); };", 0xFFFFFF8, "a[1]", "a[2] lies beyond" },
                { "struct Fwd; struct S { char p[0xFFFFFF8]; void (Fwd::*m)(); };", 0xFFFFFF8, "m", "m_thisAdjust lies beyond" },
                { "struct S { char p[0xFFFFFFC]; int a : 3; int b : 3; char c : 1; };", 0xFFFFFFC, "b", "c lies beyond" },
                { "struct S { char a[0x20000000]; int b : 3; };", 0, "a", "b lies beyond" },
                { "struct S { char p[0xFFFFFF0]; struct { int x[8]; int y; }; };", 0xFFFFFFC, "x[3]", "x[4] lies beyond" },
                { "[StructLayout(LayoutKind.Explicit)] public struct S { [FieldOffset(0x8)] public int k; [FieldOffset(0xFFFFFFFF)] public int a; };", 8, "k", "a lies beyond" },
                { "[StructLayout(LayoutKind.Explicit)] public struct S { [FieldOffset(0x8)] public int k; [FieldOffset(0x100000004)] public int a; };", 8, "k", "a lies beyond" },
                { "[StructLayout(LayoutKind.Explicit)] public struct S { [FieldOffset(0x8)] public int k; [FieldOffset(0x80000000)] public int a; public int b; };", 8, "k", "a lies beyond" },
                { "[StructLayout(LayoutKind.Sequential, Size = 0x20000000)] public struct S { public int k; };", 0, "k", "" },
                { "struct S { virtual void f(); char p[0xFFFFFF0]; int a[4]; };", 0xFFFFFFC, "a[1]", "a[2] lies beyond" },
            };
            for (const Case& test : kCases)
            {
                const StructImport imported = Import(test.Text);
                const StructDefinition* s = Named(imported.Structures, "S");
                bool inside = s != nullptr;
                for (const StructField& field : s ? s->Fields : std::vector<StructField>())
                    inside = inside && field.Offset >= 0 && field.Offset < kMaxFieldOffset;
                bool warned = test.Beyond.empty();
                for (const std::string& warning : imported.Warnings)
                    warned = warned || warning.find(test.Beyond) != std::string::npos;
                CHECK_MSG(inside && warned && At(s, test.LastOffset, test.Last), std::string(test.Text) + " -> " + Offsets(s));
            }
        }

        // #11: function pointers and pointers to members. The expected numbers are
        // offsetof/sizeof from cl 19.4x (VS 2022) on the same text, x64 then x86.
        void CheckFunctionPointers()
        {
            static constexpr std::string_view kText =
                "struct A { int a; }; struct B { int b; };\n"
                "struct Single : A { void f(); };\n"
                "struct Multi : A, B { void f(); };\n"
                "struct Virt : virtual A { void f(); };\n"
                "struct Poly : A { virtual void f(); };\n"
                "struct Poly2 : Poly { };\n"
                "struct Fwd;\n"
                "struct Mystery;\n"
                "typedef void (*Callback)(int);\n"
                "using Handler = void (Multi::*)();\n"
                "struct CallbackRecord { void (*callback)(); unsigned int state; };\n"
                "struct Arr { void (__stdcall *cbs[3])(int); char tail; };\n"
                "struct Ret { Mystery* (__stdcall *make)(int) noexcept; int tail; };\n"
                "struct Aliased { char c; Callback cb; Handler h; char tail; };\n"
                "struct Self { int x; void (Self::*m)(); char tail; };\n"
                "struct S1 { char c; void (Single::*m)(); char tail; };\n"
                "struct S2 { char c; void (Multi::*m)(); char tail; };\n"
                "struct S3 { char c; void (Virt::*m)(); char tail; };\n"
                "struct S4 { char c; void (Fwd::*m)(); char tail; };\n"
                "struct F5 { char c; void (Poly2::*m)(); char tail; };\n"
                "struct D3 { char c; int Virt::*m; char tail; };\n"
                "struct D4 { char c; int Fwd::*m; char tail; };\n"
                "struct PArr { int (*p)[4]; char tail; };\n"
                "struct Methods { int a; void f(); void (*get(int))(); int tail; };\n";
            struct Expected
            {
                std::string_view Structure;
                std::string_view Field;
                int Offset64, Size64, Offset32, Size32;
            };
            static constexpr Expected kNative[] = {
                { "CallbackRecord", "state", 8, 16, 4, 8 }, { "Arr", "tail", 24, 32, 12, 16 }, { "Ret", "tail", 8, 16, 4, 8 },
                { "Aliased", "h", 16, 40, 8, 24 }, { "Aliased", "tail", 32, 40, 16, 24 }, { "Self", "m", 8, 24, 4, 12 },
                { "Self", "tail", 16, 24, 8, 12 }, { "S1", "tail", 16, 24, 8, 12 }, { "S2", "tail", 24, 32, 16, 24 },
                { "S3", "tail", 24, 32, 20, 24 }, { "S4", "tail", 32, 40, 24, 32 }, { "F5", "tail", 24, 32, 16, 24 },
                { "D3", "tail", 12, 16, 16, 24 }, { "D4", "m", 4, 20, 8, 24 }, { "D4", "tail", 16, 20, 20, 24 },
                { "PArr", "tail", 8, 16, 4, 8 }, { "Methods", "tail", 4, 8, 4, 8 } };
            for (const bool is64 : { true, false })
            {
                const StructImport imported = Import(kText, is64);
                const int pointer = is64 ? 8 : 4;
                for (const Expected& expected : kNative)
                {
                    const StructDefinition* definition = Named(imported.Structures, expected.Structure);
                    const int offset = is64 ? expected.Offset64 : expected.Offset32;
                    const int size = is64 ? expected.Size64 : expected.Size32;
                    CHECK_MSG(definition && At(definition, offset, expected.Field) && definition->Size() == size,
                        Format("%s %.*s.%.*s: want %d / %d, got ", is64 ? "x64" : "x86", static_cast<int>(expected.Structure.size()), expected.Structure.data(),
                            static_cast<int>(expected.Field.size()), expected.Field.data(), offset, size) + Offsets(definition));
                }
                // the callback is one pointer field; the parts of a member pointer have names
                const StructDefinition* record = Named(imported.Structures, "CallbackRecord");
                CHECK(At(record, 0, "callback") && At(record, 0, "callback")->Type == ValueType::Pointer && At(record, 0, "callback")->Size == pointer);
                const StructDefinition* arr = Named(imported.Structures, "Arr");
                CHECK_MSG(At(arr, 0, "cbs[0]") && At(arr, 2 * pointer, "cbs[2]"), Offsets(arr));
                const StructDefinition* s2 = Named(imported.Structures, "S2");
                CHECK_MSG(At(s2, 8, "m") && At(s2, 8, "m")->Type == ValueType::Pointer && At(s2, 8 + pointer, "m_thisAdjust"), Offsets(s2));
                const StructDefinition* s4 = Named(imported.Structures, "S4");
                CHECK_MSG(At(s4, 8 + pointer, "m_thisAdjust") && At(s4, 12 + pointer, "m_vbptrOffset") && At(s4, 16 + pointer, "m_vbIndex"), Offsets(s4));
                const StructDefinition* d4 = Named(imported.Structures, "D4");
                const int d4m = is64 ? 4 : 8;
                CHECK_MSG(At(d4, d4m, "m") && At(d4, d4m + 4, "m_vbptrOffset") && At(d4, d4m + 8, "m_vbIndex"), Offsets(d4));
                CHECK(!At(Named(imported.Structures, "Methods"), 4, "get"));
                // the only warnings: the two pointers to members of Fwd, which is never defined
                int general = 0;
                for (const std::string& warning : imported.Warnings)
                    general += warning.find("Fwd is not defined") != std::string::npos;
                CHECK_MSG(imported.Warnings.size() == 2 && general == 2, imported.Warnings.empty() ? std::string("no warnings") : imported.Warnings.front());
            }

            // a declaration in parentheses that is no pointer says so instead of shifting silently
            const StructImport odd = Import("struct O { int (x); int y; };");
            CHECK(!odd.Warnings.empty() && odd.Warnings.front().find("parentheses") != std::string::npos);
        }

        void CheckCSharp()
        {
            StructureStore store;
            Sample(store);
            StructExportOptions options;
            options.Language = StructLanguage::CSharp;
            const std::string text = ExportStructures(store, { "Player data" }, options);
            CHECK_MSG(text.find("[StructLayout(LayoutKind.Explicit, Size = 0x80)]\npublic unsafe struct Player_data") != std::string::npos, text);
            CHECK_MSG(text.find("    [FieldOffset(0x10)] public int health;") != std::string::npos, text);
            CHECK_MSG(text.find("    [FieldOffset(0x12)] public ushort overlap;") != std::string::npos, text);  // C# may overlap
            CHECK_MSG(text.find("    [FieldOffset(0x18)] public fixed byte name[0x10];") != std::string::npos, text);
            CHECK_MSG(text.find("    [FieldOffset(0x28)] public fixed char wide_name[0x10];") != std::string::npos, text);
            CHECK_MSG(text.find("    [FieldOffset(0x48)] public Vec3 pos;") != std::string::npos && text.find("    [FieldOffset(0x58)] public Enemy* target;") != std::string::npos, text);
            CHECK_MSG(text.find("    [FieldOffset(0x70)] public ulong money;") != std::string::npos, text);

            const StructImport imported = Import(text);
            const StructDefinition* player = Named(imported.Structures, "Player_data");
            CHECK_MSG(player && player->Size() == 0x80 && At(player, 0x10, "health") && At(player, 0x12, "overlap"), Offsets(player));
            CHECK_MSG(At(player, 0x28, "wide_name") && At(player, 0x28, "wide_name")->Unicode && At(player, 0x58, "target") &&
                At(player, 0x58, "target")->ChildStructure == "Enemy", Offsets(player));
            CHECK_MSG(At(player, 0x48, "pos") && At(player, 0x48, "pos")->ChildStructure == "Vec3", Offsets(player));

            const StructImport sequential = Import(
                "using System;\nnamespace Game {\n"
                "  public struct S { public byte a; public int b; public IntPtr p; public int Hp { get; set; } public int Twice => b * 2;\n"
                "    public void Reset() { b = 0; } public long c; }\n}\n");
            const StructDefinition* s = Named(sequential.Structures, "S");
            CHECK_MSG(s && At(s, 0, "a") && At(s, 4, "b") && At(s, 8, "p") && At(s, 0x10, "c") && s->Size() == 0x18 && !At(s, 0x18), Offsets(s));
        }

        void CheckReClass()
        {
            StructureStore store;
            Sample(store);
            const std::vector<std::uint8_t> file = ExportReClass(store, { "Player data" }, true, true);
            CHECK(file.size() > 100 && file[0] == 'P' && file[1] == 'K' && LooksLikeReClass(file));
            CHECK(ExportReClass(store, { "Player data" }, true, true) == file);  // the same ids each time
            Result<StructImport> back = ImportReClass(file, {});
            CHECK_MSG(back.Ok(), back ? std::string() : back.Error().Text);
            if (!back)
                return;
            const StructDefinition* player = Named(back->Structures, "Player data");
            CHECK_MSG(player && player->Size() == 0x80, Offsets(player));
            CHECK_MSG(At(player, 0x10, "health") && At(player, 0x10, "health")->Display == FieldDisplay::Signed, Offsets(player));
            CHECK_MSG(At(player, 0x12, "overlap") == nullptr && At(player, 0x0) == nullptr, Offsets(player));
            CHECK_MSG(At(player, 0x18, "name") && At(player, 0x18, "name")->Type == ValueType::String && At(player, 0x18, "name")->Size == 16, Offsets(player));
            CHECK_MSG(At(player, 0x28, "wide name") && At(player, 0x28, "wide name")->Unicode && At(player, 0x28, "wide name")->Size == 32, Offsets(player));
            CHECK_MSG(At(player, 0x48, "pos") && At(player, 0x48, "pos")->ChildStructure == "Vec3" && At(player, 0x48, "pos")->Size == 12, Offsets(player));
            CHECK_MSG(At(player, 0x58, "target") && At(player, 0x58, "target")->ChildStructure == "Enemy", Offsets(player));
            CHECK_MSG(At(player, 0x60, "alive") && At(player, 0x60, "alive")->Display == FieldDisplay::Binary, Offsets(player));
            CHECK_MSG(At(player, 0x64, "raw") && At(player, 0x64, "raw")->Size == 6 && At(player, 0x70, "money") && At(player, 0x70, "money")->Display == FieldDisplay::Hex,
                Offsets(player));
            CHECK_MSG(At(player, 0x78, "ratio") && At(player, 0x78, "ratio")->Type == ValueType::Double, Offsets(player));
            const StructDefinition* enemy = Named(back->Structures, "Enemy");
            CHECK_MSG(enemy && enemy->Size() == 0xC && At(enemy, 8, "hp"), Offsets(enemy));

            // Written by ReClass.NET itself: text ids, auto-named Hex nodes, arrays, unions, the old node names.
            const std::string xml =
                "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--ReClass.NET-->\n<reclass version=\"65537\" type=\"x86\">\n"
                "<custom_data /><type_mapping /><enums><enum name=\"Team\" size=\"OneByte\" flags=\"false\"><item name=\"Red\" value=\"0\" /></enum></enums>\n"
                "<classes>\n"
                "<class uuid=\"00112233-4455-6677-8899-aabbccddeeff\" name=\"Vec\" comment=\"\" address=\"0\">"
                "<node type=\"Vector3Node\" name=\"v\" comment=\"\" hidden=\"false\" /></class>\n"
                "<class uuid=\"MyIRAFVEd2aImaq7zN3u/w==\" name=\"Dup\" comment=\"\" address=\"0\"><node type=\"Int8Node\" name=\"x\" comment=\"\" hidden=\"false\" /></class>\n"
                "<class uuid=\"AQAAAAAAAAAAAAAAAAAAAg==\" name=\"Thing\" comment=\"\" address=\"400000\">"
                "<node type=\"VTableNode\" name=\"\" comment=\"\" hidden=\"false\"><method name=\"a\" comment=\"\" hidden=\"false\" /></node>"
                "<node type=\"Hex32Node\" name=\"N0000001A\" comment=\"\" hidden=\"false\" />"
                "<node type=\"Hex32Node\" name=\"flags\" comment=\"\" hidden=\"false\" />"
                "<node type=\"ArrayNode\" name=\"ammo\" comment=\"\" hidden=\"false\" count=\"3\"><node type=\"Int16Node\" name=\"\" comment=\"\" hidden=\"false\" /></node>"
                "<node type=\"Hex16Node\" name=\"N0000001B\" comment=\"\" hidden=\"false\" />"
                "<node type=\"ClassPtrNode\" name=\"vec\" comment=\"\" hidden=\"false\" reference=\"00112233-4455-6677-8899-aabbccddeeff\" />"
                "<node type=\"UnionNode\" name=\"u\" comment=\"\" hidden=\"false\"><node type=\"FloatNode\" name=\"f\" comment=\"\" hidden=\"false\" />"
                "<node type=\"Int64Node\" name=\"l\" comment=\"\" hidden=\"false\" /></node>"
                "<node type=\"EnumNode\" name=\"team\" comment=\"\" hidden=\"false\" reference=\"Team\" />"
                "<node type=\"UTF8TextNode\" name=\"tag\" comment=\"\" hidden=\"false\" length=\"7\" />"
                "<node type=\"ClassInstanceArrayNode\" name=\"points\" comment=\"\" hidden=\"false\" count=\"2\" reference=\"00112233-4455-6677-8899-aabbccddeeff\" />"
                "<node type=\"WeirdPluginNode\" name=\"odd\" comment=\"\" hidden=\"false\" />"
                "<node type=\"Hex64Node\" name=\"N0000001C\" comment=\"\" hidden=\"false\" />"
                "</class>\n</classes>\n</reclass>\n";
            Result<StructImport> plain = ImportReClass(std::span(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()), {});
            CHECK_MSG(plain.Ok(), plain ? std::string() : plain.Error().Text);
            if (!plain)
                return;
            const StructDefinition* thing = Named(plain->Structures, "Thing");
            CHECK_MSG(thing && At(thing, 0, "vftable") && At(thing, 0, "vftable")->Size == 4 && !At(thing, 4) && At(thing, 8, "flags"), Offsets(thing));
            CHECK_MSG(At(thing, 0xC, "ammo[0]") && At(thing, 0x10, "ammo[2]") && !At(thing, 0x12) && At(thing, 0x14, "vec") &&
                At(thing, 0x14, "vec")->ChildStructure == "Vec", Offsets(thing));  // the text id matched the Base64 one's bytes
            CHECK_MSG(At(thing, 0x18, "u.f") && At(thing, 0x18, "u.l") && At(thing, 0x20, "team") && At(thing, 0x20, "team")->Size == 1, Offsets(thing));
            CHECK_MSG(At(thing, 0x21, "tag") && At(thing, 0x21, "tag")->Size == 7 && At(thing, 0x28, "points[0]") && At(thing, 0x34, "points[1]") &&
                At(thing, 0x34, "points[1]")->ChildStructure == "Vec", Offsets(thing));
            CHECK_MSG(thing->Size() == 0x48, Offsets(thing));  // the trailing Hex64 kept as padding; WeirdPluginNode took no room
            CHECK_MSG(plain->Warnings.size() == 1 && plain->Warnings.front().find("WeirdPluginNode") != std::string::npos,
                plain->Warnings.empty() ? std::string("no warning") : plain->Warnings.front());
            const StructDefinition* vec = Named(plain->Structures, "Vec");
            CHECK_MSG(vec && At(vec, 0, "v.x") && At(vec, 8, "v.z") && vec->Size() == 12, Offsets(vec));

            std::vector<std::uint8_t> damaged = file;
            damaged[damaged.size() / 2] ^= 0x55;
            CHECK(!ImportReClass(damaged, {}).Ok());
            const std::string notReClass = "<root><classes /></root>";
            CHECK(!ImportReClass(std::span(reinterpret_cast<const std::uint8_t*>(notReClass.data()), notReClass.size()), {}).Ok());
        }

        void CheckMerge()
        {
            const auto fresh = []
                {
                    StructureStore store;
                    store.Add("Vec3").Fields = { Field(0, "old", ValueType::FourBytes, 4) };
                    store.Add("Other");
                    return store;
                };
            const StructImport imported = Import("struct Vec3 { float x, y, z; };\nstruct Player { Vec3 pos; Vec3* aim; };\nstruct Vec3_2 { int clash; };");
            CHECK(imported.Structures.size() == 3);

            StructureStore replace = fresh();
            const std::uint64_t before = replace.Generation;
            const StructMerge replaced = MergeStructures(replace, imported.Structures, StructConflict::Replace);
            CHECK(replaced.Replaced == 1 && replaced.Added == 2 && replaced.Skipped == 0 && replace.Items.size() == 4 && replace.Generation > before);
            CHECK(replace.Find("Vec3") && replace.Find("Vec3")->Fields.size() == 3 && replace.Find("Vec3")->Fields.front().Name == "x");

            StructureStore rename = fresh();
            CHECK(PlanMerge(rename, imported.Structures, StructConflict::Rename) == std::vector<std::string>({ "Vec3_3", "Player", "Vec3_2" }));
            const StructMerge renamed = MergeStructures(rename, imported.Structures, StructConflict::Rename);
            CHECK(renamed.Added == 3 && rename.Find("Vec3")->Fields.front().Name == "old" && rename.Find("Vec3_3"));
            const StructDefinition* player = rename.Find("Player");
            CHECK_MSG(player && player->Fields.size() == 2 && player->Fields[0].ChildStructure == "Vec3_3" && player->Fields[1].ChildStructure == "Vec3_3", Offsets(player));

            StructureStore skip = fresh();
            const StructMerge skipped = MergeStructures(skip, imported.Structures, StructConflict::Skip);
            CHECK(skipped.Skipped == 1 && skipped.Added == 2 && skip.Find("Vec3")->Fields.front().Name == "old");
            CHECK(skip.Find("Player") && skip.Find("Player")->Fields[0].ChildStructure == "Vec3");
        }

        int RunStructExchangeTests()
        {
            const int before = Totals().Failures;
            CheckIdentifier();
            CheckExportCpp();
            CheckRoundTripCpp();
            CheckMsvcLayout();
            CheckFunctionPointers();
            CheckHugeArrays();
            CheckFieldsPastOffsetLimit();
            CheckCSharp();
            CheckReClass();
            CheckMerge();
            return Totals().Failures - before;
        }

        // Writes the sample as .h, .cs and .rcnet, so a compiler (static_assert) and ReClass.NET can check them.
        int RunStructSample(const std::vector<std::string>& arguments)
        {
            const std::filesystem::path folder = arguments.size() >= 2 ? std::filesystem::path(arguments[1]) : std::filesystem::current_path();
            StructureStore store;
            Sample(store);
            const auto save = [&](const char* name, std::string_view data)
                {
                    std::ofstream out(folder / name, std::ios::binary);
                    out.write(data.data(), static_cast<std::streamsize>(data.size()));
                    std::printf("wrote %s\n", (folder / name).string().c_str());
                };
            save("Sample.h", ExportStructures(store, { "Player data" }, {}));
            StructExportOptions x86;
            x86.Is64Bit = false;
            save("Sample32.h", ExportStructures(store, { "Vec3", "Enemy" }, x86));
            StructExportOptions csharp;
            csharp.Language = StructLanguage::CSharp;
            save("Sample.cs", ExportStructures(store, { "Player data" }, csharp));
            const std::vector<std::uint8_t> project = ExportReClass(store, { "Player data" }, true, true);
            save("Sample.rcnet", std::string_view(reinterpret_cast<const char*>(project.data()), project.size()));
            return 0;
        }
    }

    MANIFOLD_MODE("--struct-sample", &RunStructSample);

    MANIFOLD_SUITE("structexchange", "Struct export and import", 130, [](const SuiteContext&) { return RunStructExchangeTests(); });
}
