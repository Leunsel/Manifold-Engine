#include "TestKit.h"

#include "App/Commands.h"
#include "App/Settings.h"
#include "Core/Text.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        std::vector<ImGuiKeyChord> Chords(const CommandInfo& info)
        {
            std::vector<ImGuiKeyChord> chords;
            if (info.Digits)
            {
                for (int i = 0; i < 10; ++i)
                    chords.push_back((info.Chord & ImGuiMod_Mask_) | (ImGuiKey_0 + i));
            }
            else if (info.Chord)
                chords.push_back(info.Chord);
            if (info.Alternate)
                chords.push_back(info.Alternate);
            return chords;
        }

        void TableIsComplete()
        {
            const auto commands = AllCommands();
            CHECK_EQ(commands.size(), static_cast<std::size_t>(Command::Count));
            for (std::size_t i = 0; i < commands.size(); ++i)
            {
                CHECK_MSG(static_cast<std::size_t>(commands[i].Id) == i, "command table out of order at " + std::to_string(i));
                CHECK_MSG(commands[i].Name && commands[i].Name[0], "command without a name at " + std::to_string(i));
                CHECK_MSG(commands[i].Chord != 0, std::string("command without keys: ") + commands[i].Name);
                CHECK_MSG(ShortcutText(commands[i].Id) != nullptr, std::string("no shortcut text for ") + commands[i].Name);
            }
            CHECK_EQ(std::string(ShortcutText(Command::SaveTableAs)), "Ctrl+Alt+S");
            CHECK_EQ(std::string(ShortcutText(Command::OpenLuaEngine)), "Ctrl+Alt+Shift+L");
            CHECK_EQ(std::string(ShortcutText(Command::FreezeAllowIncrease)), "Num +");
            CHECK_EQ(std::string(ShortcutText(Command::SetBookmark)), "Ctrl+Alt+0-9");
            CHECK_EQ(std::string(ShortcutText(Command::DeleteRecords)), "Del");
        }

        void NoDuplicateKeysInAScope()
        {
            std::map<std::pair<int, ImGuiKeyChord>, const char*> seen;
            for (const CommandInfo& info : AllCommands())
            {
                for (ImGuiKeyChord chord : Chords(info))
                {
                    const auto key = std::make_pair(static_cast<int>(info.Scope), chord);
                    const auto [at, added] = seen.emplace(key, info.Name);
                    CHECK_MSG(added, std::string(ScopeName(info.Scope)) + ": " + ChordText(chord) + " is used by \"" + at->second + "\" and \"" + info.Name + "\"");
                }
            }
        }

        void ChordTextRoundTrips()
        {
            for (const CommandInfo& info : AllCommands())
            {
                for (ImGuiKeyChord chord : Chords(info))
                    CHECK_MSG(ParseChord(ChordText(chord)) == chord, "ParseChord(\"" + ChordText(chord) + "\") does not give the chord back");
            }
            CHECK_EQ(ParseChord("Ctrl+Shift+A"), ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_A);
            CHECK_EQ(ParseChord("ctrl+alt+t"), ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_T);
            CHECK_EQ(ParseChord(" Ctrl+I "), ImGuiMod_Ctrl | ImGuiKey_I);
            CHECK_EQ(ParseChord("Shift+F8"), ImGuiMod_Shift | ImGuiKey_F8);
            CHECK_EQ(ParseChord("Delete"), static_cast<ImGuiKeyChord>(ImGuiKey_Delete));
            CHECK_EQ(ParseChord("Ctrl+Num +"), ImGuiMod_Ctrl | ImGuiKey_KeypadAdd);
            CHECK_EQ(ParseChord(""), 0);
            CHECK_EQ(ParseChord("Ctrl+"), 0);
            CHECK_EQ(ParseChord("Ctrl+Nothing"), 0);
        }

        void BindingsCanChange()
        {
            std::map<std::string, int> keys;
            for (const CommandInfo& info : AllCommands())
            {
                CHECK_MSG(++keys[Lower(info.Key)] == 1, std::string("command key used twice: ") + info.Key);
                CHECK_MSG(FindCommand(info.Key) == info.Id, std::string("FindCommand misses ") + info.Key);
                const std::string text = BindingText(info.Id, DefaultBinding(info.Id));
                CHECK_MSG(ParseBinding(info.Id, text) == DefaultBinding(info.Id), std::string("binding text does not round trip: ") + info.Key + "=" + text);
            }
            CHECK(FindCommand("openprocess") == Command::OpenProcess);
            CHECK(!FindCommand("NoSuchCommand"));

            const ImGuiKeyChord moved = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O;
            SetBinding(Command::OpenProcess, { moved, 0 });
            CHECK(IsCustomized(Command::OpenProcess));
            CHECK_EQ(std::string(ShortcutText(Command::OpenProcess)), "Ctrl+Shift+O");
            CHECK_EQ(ShortcutSettings().at("OpenProcess"), "Ctrl+Shift+O");
            CHECK_EQ(ShortcutSettings().size(), static_cast<std::size_t>(1));
            SetBinding(Command::OpenProcess, {});
            CHECK(ShortcutText(Command::OpenProcess) == nullptr);
            CHECK(ShortcutLabel(Command::OpenProcess).empty());
            CHECK_EQ(ShortcutSettings().at("OpenProcess"), "none");
            SetBinding(Command::OpenProcess, { 0, moved });
            CHECK(CurrentBinding(Command::OpenProcess) == (Binding{ moved, 0 }));

            CHECK(ParseBinding(Command::SetBookmark, "Ctrl+Shift+0-9") == (Binding{ ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_0, 0 }));
            CHECK(!ParseBinding(Command::SetBookmark, "Ctrl+Shift+B"));
            CHECK(ParseBinding(Command::SaveTableAs, "Ctrl+Alt+S;Ctrl+Shift+S") == DefaultBinding(Command::SaveTableAs));
            CHECK(ParseBinding(Command::SaveTable, " none ") == Binding{});
            CHECK(!ParseBinding(Command::SaveTable, "Ctrl+Nothing"));
            CHECK(!ParseBinding(Command::SaveTable, "Ctrl+A;Ctrl+B;Ctrl+C"));

            const auto conflicts = ConflictsWith(Command::CopyRecords, ImGuiMod_Ctrl | ImGuiKey_V);
            CHECK(std::find(conflicts.begin(), conflicts.end(), Command::PasteRecords) != conflicts.end());
            CHECK(ConflictsWith(Command::CopyRecords, ImGuiMod_Ctrl | ImGuiKey_C).empty());
            CHECK(ConflictsWith(Command::GotoBookmark, ImGuiMod_Ctrl | ImGuiKey_0).empty());

            ApplyShortcutSettings({ { "openprocess", "Ctrl+Shift+O" }, { "Bogus", "Ctrl+Q" }, { "SaveTable", "garbage" }, { "TemplateCodeInjection", "Ctrl+Q" } });
            CHECK(CurrentBinding(Command::OpenProcess) == (Binding{ moved, 0 }));
            CHECK(!IsCustomized(Command::SaveTable));
            CHECK(!IsCustomized(Command::TemplateCodeInjection));
            CHECK_EQ(ShortcutSettings().size(), static_cast<std::size_t>(1));

            Settings written;
            written.Shortcuts = ShortcutSettings();
            written.Shortcuts["GotoBookmark"] = "Ctrl+Shift+0-9";
            const std::filesystem::path file = std::filesystem::temp_directory_path() / ("manifold-shortcuts-" + std::to_string(::GetCurrentProcessId()) + ".ini");
            CHECK(written.Save(file).Ok());
            Settings read;
            CHECK(read.Load(file));
            CHECK(read.Shortcuts == written.Shortcuts);
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
            ApplyShortcutSettings(read.Shortcuts);
            CHECK_EQ(std::string(ShortcutText(Command::GotoBookmark)), "Ctrl+Shift+0-9");

            ApplyShortcutSettings({});
            for (const CommandInfo& info : AllCommands())
                CHECK_MSG(!IsCustomized(info.Id), std::string("not reset: ") + info.Key);
            CHECK_EQ(std::string(ShortcutText(Command::OpenProcess)), "Ctrl+P");
        }

        void DocsAreCurrent()
        {
            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Source" / "App" / "Commands.h"); ++i)
                at = at.parent_path();
            const std::filesystem::path docs = at / "Docs" / "SHORTCUTS.md";
            if (!std::filesystem::exists(docs))
            {
                Skip("Docs/SHORTCUTS.md not found above " + std::filesystem::current_path().string());
                return;
            }
            std::ifstream file(docs, std::ios::binary);
            std::stringstream content;
            content << file.rdbuf();
            std::string written = content.str();
            written.erase(std::remove(written.begin(), written.end(), '\r'), written.end());
            CHECK_MSG(written == ShortcutsMarkdown(), "Docs/SHORTCUTS.md is out of date: run the tests with --shortcuts-markdown");
        }
    }

    int RunCommandTests()
    {
        const int before = Totals().Failures;
        TableIsComplete();
        NoDuplicateKeysInAScope();
        ChordTextRoundTrips();
        BindingsCanChange();
        DocsAreCurrent();
        return Totals().Failures - before;
    }
}
