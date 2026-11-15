#include "Table/CheatTable.h"

#include "Core/Deflate.h"
#include "Core/Text.h"
#include "Core/Xml.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <windows.h>

namespace Manifold::Table
{
    namespace
    {
        constexpr const char* kScriptType = "Auto Assembler Script";
        constexpr std::string_view kBase85 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%()*+,-./:;=?@[]^_{}";
        constexpr const char* kKnownTopLevel[] = { "CheatEntries", "UserdefinedSymbols", "Structures", "LuaScript", "Comments", "Files", "Signature", "CheatCodes" };

        struct OptionName
        {
            const char* Name;
            bool RecordOptions::*Field;
        };

        constexpr OptionName kOptionNames[] = {
            { "moHideChildren", &RecordOptions::HideChildren },
            { "moActivateChildrenAsWell", &RecordOptions::ActivateChildren },
            { "moDeactivateChildrenAsWell", &RecordOptions::DeactivateChildren },
            { "moRecursiveSetValue", &RecordOptions::RecursiveSetValue },
            { "moAllowManualCollapseAndExpand", &RecordOptions::AllowManualExpand },
            { "moManualExpandCollapse", &RecordOptions::ManualExpand },
            { "moAlwaysHideChildren", &RecordOptions::AlwaysHideChildren },
        };

        struct HotkeyName
        {
            const char* Name;
            HotkeyAction Action;
        };

        constexpr HotkeyName kHotkeyNames[] = {
            { "Toggle Activation", HotkeyAction::Toggle },
            { "Toggle Activation Allow Increase", HotkeyAction::ToggleAllowIncrease },
            { "Toggle Activation Allow Decrease", HotkeyAction::ToggleAllowDecrease },
            { "Activate", HotkeyAction::Activate },
            { "Deactivate", HotkeyAction::Deactivate },
            { "Set Value", HotkeyAction::SetValue },
            { "Increase Value", HotkeyAction::Increase },
            { "Decrease Value", HotkeyAction::Decrease },
        };

        std::string StripQuotes(std::string text)
        {
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                return text.substr(1, text.size() - 2);
            return text;
        }

        bool Flag(const Xml::Node& node, std::string_view name)
        {
            const std::string text = Trim(node.ChildText(name));
            return text == "1" || EqualsNoCase(text, "true");
        }

        std::unique_ptr<MemoryRecord> ReadEntry(const Xml::Node& node, std::vector<std::string>& warnings)
        {
            auto record = std::make_unique<MemoryRecord>();
            if (const auto id = ParseDecimal(node.ChildText("ID")))
                record->Id = static_cast<int>(*id);
            record->Description = StripQuotes(node.ChildText("Description"));
            const std::string type = Trim(node.ChildText("VariableType"));
            if (const Xml::Node* script = node.Child("AssemblerScript"); script || EqualsNoCase(type, kScriptType))
            {
                record->Kind = RecordKind::Script;
                record->Script = script ? script->Text : "";
                if (script)
                    record->Options.AsyncScript = script->Attribute("Async").value_or("0") == "1";
            }
            else if (Flag(node, "GroupHeader") || type.empty())
            {
                record->Kind = RecordKind::Group;
            }
            else if (const auto known = Scan::TypeFromTableName(type))
            {
                record->Type = *known;
                if (EqualsNoCase(type, "Unicode String"))
                    record->Unicode = true;
            }
            else if (EqualsNoCase(type, "Custom") && !Trim(node.ChildText("CustomType")).empty())
            {
                record->Type = Scan::ValueType::FourBytes;
            }
            else
            {
                warnings.push_back(ErrorMessage(ErrorCode::TableEntryDamaged, record->Description + " (" + type + ")"));
                record->Type = Scan::ValueType::FourBytes;
            }

            if (record->Kind == RecordKind::Group && node.HasChild("Address") && !Trim(node.ChildText("Address")).empty())
                record->AddressGroupHeader = true;
            record->DropDownLink = Trim(node.ChildText("DropDownListLink"));
            record->DontSave = Flag(node, "DontSave");
            record->CustomTypeName = node.ChildText("CustomType");

            record->Address = Trim(node.ChildText("Address"));
            if (const Xml::Node* offsets = node.Child("Offsets"))
            {
                for (const Xml::Node* offset : offsets->ChildrenNamed("Offset"))
                    record->Offsets.insert(record->Offsets.begin(), Trim(offset->Text));
            }
            record->ShowHex = Flag(node, "ShowAsHex");
            if (node.HasChild("ShowAsSigned"))
                record->ShowSigned = Flag(node, "ShowAsSigned");
            else
                record->ShowSigned = record->Type == Scan::ValueType::Float || record->Type == Scan::ValueType::Double;
            if (const auto length = ParseDecimal(node.ChildText("Length")))
                record->Length = static_cast<int>(*length);
            if (const auto length = ParseDecimal(node.ChildText("ByteLength")))
                record->Length = static_cast<int>(*length);
            if (node.HasChild("Unicode"))
                record->Unicode = Flag(node, "Unicode");
            if (node.HasChild("ZeroTerminate"))
                record->ZeroTerminate = Flag(node, "ZeroTerminate");
            if (const auto bit = ParseDecimal(node.ChildText("BitStart")))
                record->BitStart = static_cast<int>(*bit);
            if (const auto bits = ParseDecimal(node.ChildText("BitLength")))
                record->BitLength = static_cast<int>(*bits);
            if (const auto color = ParseHex(node.ChildText("Color")))
            {
                record->Color = static_cast<std::uint32_t>(*color);
                record->HasColor = (*color & 0xFF000000u) == 0;
            }
            record->Comment = node.ChildText("Comment");

            if (const Xml::Node* options = node.Child("Options"))
            {
                for (const OptionName& option : kOptionNames)
                    record->Options.*option.Field = options->Attribute(option.Name).value_or("0") == "1";
            }
            if (const Xml::Node* list = node.Child("DropDownList"))
            {
                record->DropDownOnly = list->Attribute("ReadOnly").value_or("0") == "1";
                record->DropDownDescriptionOnly = list->Attribute("DescriptionOnly").value_or("0") == "1";
                for (const std::string& line : SplitLines(list->Text))
                {
                    const std::string trimmed = Trim(line);
                    if (trimmed.empty())
                        continue;
                    const std::size_t colon = trimmed.find(':');
                    DropDownEntry entry;
                    entry.Value = colon == std::string::npos ? trimmed : Trim(trimmed.substr(0, colon));
                    entry.Text = colon == std::string::npos ? trimmed : Trim(trimmed.substr(colon + 1));
                    record->DropDown.push_back(std::move(entry));
                }
            }
            if (const Xml::Node* hotkeys = node.Child("Hotkeys"))
            {
                for (const Xml::Node* hotkey : hotkeys->ChildrenNamed("Hotkey"))
                {
                    Hotkey key;
                    const std::string action = Trim(hotkey->ChildText("Action"));
                    for (const HotkeyName& name : kHotkeyNames)
                    {
                        if (EqualsNoCase(action, name.Name))
                            key.Action = name.Action;
                    }
                    if (const Xml::Node* keys = hotkey->Child("Keys"))
                    {
                        for (const Xml::Node* value : keys->ChildrenNamed("Key"))
                        {
                            if (const auto code = ParseDecimal(value->Text))
                                key.Keys.push_back(static_cast<int>(*code));
                        }
                    }
                    key.Value = hotkey->ChildText("Value");
                    key.Description = hotkey->ChildText("Description");
                    if (const auto id = ParseDecimal(hotkey->ChildText("ID")))
                        key.Id = static_cast<int>(*id);
                    if (!key.Keys.empty())
                        record->Hotkeys.push_back(std::move(key));
                }
            }
            if (const Xml::Node* children = node.Child("CheatEntries"))
            {
                for (const Xml::Node* child : children->ChildrenNamed("CheatEntry"))
                {
                    auto entry = ReadEntry(*child, warnings);
                    entry->Parent = record.get();
                    record->Children.push_back(std::move(entry));
                }
            }
            if (record->Kind == RecordKind::Value && record->Address.empty() && !record->Children.empty())
                record->Kind = RecordKind::Group;
            record->Expanded = !record->Options.ManualExpand || record->Options.AllowManualExpand;
            return record;
        }

        void WriteEntry(Xml::Node& parent, const MemoryRecord& record)
        {
            Xml::Node& node = parent.Add("CheatEntry");
            node.Add("ID", std::to_string(record.Id));
            node.Add("Description", "\"" + record.Description + "\"");
            bool anyOption = false;
            Xml::Node options;
            options.Name = "Options";
            for (const OptionName& option : kOptionNames)
            {
                if (record.Options.*option.Field)
                {
                    options.SetAttribute(option.Name, "1");
                    anyOption = true;
                }
            }
            if (anyOption)
            {
                Xml::Node& written = node.Add("Options");
                written.Attributes = options.Attributes;
            }
            if (record.HasColor)
                node.Add("Color", Hex(record.Color, 6));
            if (!record.Comment.empty())
                node.Add("Comment", record.Comment);

            if (record.Kind == RecordKind::Group)
            {
                node.Add("GroupHeader", "1");
                if (record.AddressGroupHeader && !record.Address.empty())
                {
                    node.Add("Address", record.Address);
                    if (!record.Offsets.empty())
                    {
                        Xml::Node& offsets = node.Add("Offsets");
                        for (auto it = record.Offsets.rbegin(); it != record.Offsets.rend(); ++it)
                            offsets.Add("Offset", *it);
                    }
                }
            }
            else if (record.Kind == RecordKind::Script)
            {
                node.Add("VariableType", kScriptType);
                Xml::Node& script = node.Add("AssemblerScript", record.Script);
                if (record.Options.AsyncScript)
                    script.SetAttribute("Async", "1");
            }
            else
            {
                if (record.ShowHex)
                    node.Add("ShowAsHex", "1");
                node.Add("ShowAsSigned", record.ShowSigned ? "1" : "0");
                node.Add("VariableType", record.CustomTypeName.empty() ? Scan::TableTypeName(record.Type) : "Custom");
                if (!record.CustomTypeName.empty())
                    node.Add("CustomType", record.CustomTypeName);
                else if (record.Type == Scan::ValueType::String)
                {
                    node.Add("Length", std::to_string(record.Length));
                    node.Add("Unicode", record.Unicode ? "1" : "0");
                    node.Add("CodePage", "0");
                    node.Add("ZeroTerminate", record.ZeroTerminate ? "1" : "0");
                }
                else if (record.Type == Scan::ValueType::ByteArray)
                {
                    node.Add("ByteLength", std::to_string(record.Length));
                }
                else if (record.Type == Scan::ValueType::Binary)
                {
                    node.Add("BitStart", std::to_string(record.BitStart));
                    node.Add("BitLength", std::to_string(record.BitLength));
                }
                node.Add("Address", record.Address);
                if (!record.Offsets.empty())
                {
                    Xml::Node& offsets = node.Add("Offsets");
                    for (auto it = record.Offsets.rbegin(); it != record.Offsets.rend(); ++it)
                        offsets.Add("Offset", *it);
                }
            }
            if (!record.DropDownLink.empty())
                node.Add("DropDownListLink", record.DropDownLink);
            if (!record.DropDown.empty())
            {
                std::string text;
                for (const DropDownEntry& entry : record.DropDown)
                    text += entry.Value + ":" + entry.Text + "\n";
                Xml::Node& list = node.Add("DropDownList", text);
                if (record.DropDownOnly)
                    list.SetAttribute("ReadOnly", "1");
                if (record.DropDownDescriptionOnly)
                    list.SetAttribute("DescriptionOnly", "1");
                list.SetAttribute("DisplayValueAsItem", "1");
            }
            if (!record.Hotkeys.empty())
            {
                Xml::Node& hotkeys = node.Add("Hotkeys");
                for (const Hotkey& key : record.Hotkeys)
                {
                    Xml::Node& hotkey = hotkeys.Add("Hotkey");
                    for (const HotkeyName& name : kHotkeyNames)
                    {
                        if (name.Action == key.Action)
                            hotkey.Add("Action", name.Name);
                    }
                    Xml::Node& keys = hotkey.Add("Keys");
                    for (int code : key.Keys)
                        keys.Add("Key", std::to_string(code));
                    if (!key.Value.empty())
                        hotkey.Add("Value", key.Value);
                    if (!key.Description.empty())
                        hotkey.Add("Description", key.Description);
                    hotkey.Add("ID", std::to_string(key.Id));
                }
            }
            bool anyChild = false;
            for (const auto& child : record.Children)
                anyChild = anyChild || !child->DontSave;
            if (anyChild)
            {
                Xml::Node& children = node.Add("CheatEntries");
                for (const auto& child : record.Children)
                {
                    if (!child->DontSave)
                        WriteEntry(children, *child);
                }
            }
        }

        const char* DisplayName(FieldDisplay display)
        {
            switch (display)
            {
            case FieldDisplay::Signed: return "signed integer";
            case FieldDisplay::Hex: return "hexadecimal";
            default: return "unsigned integer";
            }
        }

        std::unique_ptr<StructDefinition> ReadStructure(const Xml::Node& node)
        {
            auto definition = std::make_unique<StructDefinition>();
            definition->Name = node.Attribute("Name").value_or("Structure");
            const Xml::Node* elements = node.Child("Elements");
            if (!elements)
                return definition;
            for (const Xml::Node* element : elements->ChildrenNamed("Element"))
            {
                StructField field;
                field.Offset = static_cast<int>(ParseDecimal(element->Attribute("Offset").value_or("0")).value_or(0));
                field.Name = element->Attribute("Description").value_or("");
                const std::string type = element->Attribute("Vartype").value_or("4 Bytes");
                field.Type = Scan::TypeFromTableName(type).value_or(Scan::ValueType::FourBytes);
                field.Unicode = EqualsNoCase(type, "Unicode String");
                field.Size = static_cast<int>(ParseDecimal(element->Attribute("Bytesize").value_or("4")).value_or(4));
                const std::string display = element->Attribute("DisplayMethod").value_or("");
                field.Display = EqualsNoCase(display, "hexadecimal") ? FieldDisplay::Hex : EqualsNoCase(display, "signed integer") ? FieldDisplay::Signed : FieldDisplay::Unsigned;
                field.ChildStructure = element->Attribute("ChildStruct").value_or("");
                if (EqualsNoCase(type, "Custom"))
                {
                    field.Type = Scan::ValueType::ByteArray;
                    field.CustomType = element->Attribute("CustomType").value_or("");
                }
                definition->Fields.push_back(std::move(field));
            }
            definition->Sort();
            return definition;
        }
    }

    Status ParseTable(std::string_view text, LoadedTable& table)
    {
        Xml::ParseResult parsed = Xml::Parse(text);
        if (!parsed.Root)
            return Status::Fail(ErrorCode::TableNotXml, parsed.Error + ", line " + std::to_string(parsed.Line));
        const Xml::Node& root = *parsed.Root;
        if (root.Name != "CheatTable")
            return Status::Fail(ErrorCode::TableNotCheatTable);
        table.Version = static_cast<int>(ParseDecimal(root.Attribute("CheatEngineTableVersion").value_or("0")).value_or(0));
        if (table.Version > kTableVersion)
            table.Warnings.push_back(ErrorMessage(ErrorCode::TableTooNew, std::to_string(table.Version)));
        if (const Xml::Node* entries = root.Child("CheatEntries"))
        {
            for (const Xml::Node* entry : entries->ChildrenNamed("CheatEntry"))
                table.Records.push_back(ReadEntry(*entry, table.Warnings));
        }
        if (const Xml::Node* symbols = root.Child("UserdefinedSymbols"))
        {
            for (const Xml::Node* symbol : symbols->ChildrenNamed("SymbolEntry"))
                table.Symbols.push_back({ Trim(symbol->ChildText("Name")), Trim(symbol->ChildText("Address")) });
        }
        if (const Xml::Node* structures = root.Child("Structures"))
        {
            for (const Xml::Node* structure : structures->ChildrenNamed("Structure"))
                table.Structures.push_back(ReadStructure(*structure));
        }
        table.LuaScript = root.ChildText("LuaScript");
        table.Comments = root.ChildText("Comments");
        if (const Xml::Node* files = root.Child("Files"))
        {
            for (const auto& file : files->Children)
            {
                TableFile entry;
                entry.Name = file->Name;
                if (file->Attribute("Encoding").value_or("") == "Ascii85")
                {
                    auto data = DecodeTableFile(file->Text);
                    if (!data)
                    {
                        table.Warnings.push_back(ErrorMessage(ErrorCode::TableEntryDamaged, "file " + file->Name));
                        continue;
                    }
                    *entry.Data = std::move(*data);
                }
                else
                {
                    const auto bytes = ParseHexBytes(file->Text);
                    if (bytes)
                        *entry.Data = *bytes;
                    else
                        entry.Data->assign(file->Text.begin(), file->Text.end());
                }
                table.Files.push_back(std::move(entry));
            }
        }
        for (const auto& child : root.Children)
        {
            const bool known = std::any_of(std::begin(kKnownTopLevel), std::end(kKnownTopLevel), [&](const char* name) { return child->Name == name; });
            if (!known)
                table.ExtraXml.push_back(Xml::Write(*child, false));
        }
        return Status::Success();
    }

    Status LoadTable(const std::filesystem::path& path, LoadedTable& table)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return Status::Fail(ErrorCode::TableNotRead, WideToUtf8(path.wstring()));
        std::stringstream content;
        content << file.rdbuf();
        return ParseTable(content.str(), table);
    }

    std::string WriteTable(const AddressList& list, const StructureStore& structures, const std::vector<TableSymbol>& symbols)
    {
        Xml::Node root;
        root.Name = "CheatTable";
        root.SetAttribute("CheatEngineTableVersion", std::to_string(kTableVersion));
        Xml::Node& entries = root.Add("CheatEntries");
        for (const auto& record : list.Roots())
        {
            if (!record->DontSave)
                WriteEntry(entries, *record);
        }
        if (!symbols.empty())
        {
            Xml::Node& node = root.Add("UserdefinedSymbols");
            for (const TableSymbol& symbol : symbols)
            {
                Xml::Node& entry = node.Add("SymbolEntry");
                entry.Add("Name", symbol.Name);
                entry.Add("Address", symbol.Address);
            }
        }
        if (!structures.Items.empty())
        {
            Xml::Node& node = root.Add("Structures");
            node.SetAttribute("StructVersion", "2");
            for (const auto& definition : structures.Items)
            {
                Xml::Node& structure = node.Add("Structure");
                structure.SetAttribute("Name", definition->Name);
                structure.SetAttribute("AutoFill", "0");
                structure.SetAttribute("AutoCreate", "1");
                structure.SetAttribute("DefaultHex", "0");
                structure.SetAttribute("AutoDestroy", "0");
                structure.SetAttribute("DoNotSaveLocal", "0");
                structure.SetAttribute("RLECompression", "1");
                structure.SetAttribute("AutoCreateStructsize", "4096");
                Xml::Node& elements = structure.Add("Elements");
                for (const StructField& field : definition->Fields)
                {
                    Xml::Node& element = elements.Add("Element");
                    element.SetAttribute("Offset", std::to_string(field.Offset));
                    element.SetAttribute("Vartype", !field.CustomType.empty() ? "Custom" : field.Type == Scan::ValueType::String && field.Unicode ? "Unicode String" : Scan::TableTypeName(field.Type));
                    if (!field.CustomType.empty())
                        element.SetAttribute("CustomType", field.CustomType);
                    element.SetAttribute("Bytesize", std::to_string(field.Size));
                    element.SetAttribute("OffsetHex", Hex(static_cast<std::uint64_t>(field.Offset), 8));
                    element.SetAttribute("Description", field.Name);
                    element.SetAttribute("DisplayMethod", DisplayName(field.Display));
                    if (!field.ChildStructure.empty())
                        element.SetAttribute("ChildStruct", field.ChildStructure);
                }
            }
        }
        if (!list.Files.empty())
        {
            Xml::Node& node = root.Add("Files");
            for (const TableFile& file : list.Files)
                node.Add(file.Name, EncodeTableFile(*file.Data)).SetAttribute("Encoding", "Ascii85");
        }
        for (const std::string& extra : list.ExtraXml)
        {
            Xml::ParseResult parsed = Xml::Parse(extra);
            if (parsed.Root)
                root.Children.push_back(std::move(parsed.Root));
        }
        if (!list.TableLuaScript.empty())
            root.Add("LuaScript", list.TableLuaScript);
        if (!list.TableComment.empty())
            root.Add("Comments", list.TableComment);
        return Xml::Write(root);
    }

    Status SaveTable(const std::filesystem::path& path, const AddressList& list, const StructureStore& structures, const std::vector<TableSymbol>& symbols)
    {
        const std::string text = WriteTable(list, structures, symbols);
        std::filesystem::path temporary = path;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                return Status::Fail(ErrorCode::TableNotSaved, WideToUtf8(path.wstring()));
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file)
                return Status::Fail(ErrorCode::TableNotSaved, WideToUtf8(path.wstring()));
        }
        if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return Status::Fail(ErrorCode::TableNotSaved, WideToUtf8(path.wstring()));
        return Status::Success();
    }

    std::string RecordsToXml(const std::vector<const MemoryRecord*>& records)
    {
        Xml::Node root;
        root.Name = "CheatTable";
        Xml::Node& entries = root.Add("CheatEntries");
        for (const MemoryRecord* record : records)
            WriteEntry(entries, *record);
        return Xml::Write(root);
    }

    std::optional<std::vector<std::uint8_t>> DecodeTableFile(std::string_view text)
    {
        int index[256];
        std::fill(std::begin(index), std::end(index), -1);
        for (std::size_t i = 0; i < kBase85.size(); ++i)
            index[static_cast<unsigned char>(kBase85[i])] = static_cast<int>(i);
        std::string digits;
        digits.reserve(text.size());
        for (char c : text)
        {
            if (static_cast<unsigned char>(c) > ' ')
                digits += c;
        }
        std::vector<std::uint8_t> packed;
        packed.reserve(digits.size() / 5 * 4 + 4);
        for (std::size_t at = 0; at < digits.size(); at += 5)
        {
            const std::size_t count = std::min<std::size_t>(5, digits.size() - at);
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < 5; ++i)
            {
                int digit = 84;
                if (i < count)
                {
                    digit = index[static_cast<unsigned char>(digits[at + i])];
                    if (digit < 0)
                        return std::nullopt;
                }
                value = value * 85 + static_cast<std::uint64_t>(digit);
            }
            const std::uint32_t word = static_cast<std::uint32_t>(value);
            const std::uint8_t bytes[4] = { static_cast<std::uint8_t>(word >> 24), static_cast<std::uint8_t>(word >> 16), static_cast<std::uint8_t>(word >> 8),
                static_cast<std::uint8_t>(word) };
            packed.insert(packed.end(), bytes, bytes + (count == 5 ? 4 : count - 1));
        }
        auto unpacked = Deflate::Decompress(packed);
        if (!unpacked || unpacked->size() < 4)
            return std::nullopt;
        const std::uint32_t size = static_cast<std::uint32_t>((*unpacked)[0]) | (static_cast<std::uint32_t>((*unpacked)[1]) << 8) |
                                   (static_cast<std::uint32_t>((*unpacked)[2]) << 16) | (static_cast<std::uint32_t>((*unpacked)[3]) << 24);
        if (size > unpacked->size() - 4)
            return std::nullopt;
        return std::vector<std::uint8_t>(unpacked->begin() + 4, unpacked->begin() + 4 + size);
    }

    std::string EncodeTableFile(std::span<const std::uint8_t> data)
    {
        std::vector<std::uint8_t> plain(4 + data.size());
        const std::uint32_t size = static_cast<std::uint32_t>(data.size());
        plain[0] = static_cast<std::uint8_t>(size);
        plain[1] = static_cast<std::uint8_t>(size >> 8);
        plain[2] = static_cast<std::uint8_t>(size >> 16);
        plain[3] = static_cast<std::uint8_t>(size >> 24);
        std::copy(data.begin(), data.end(), plain.begin() + 4);
        const auto packed = Deflate::Compress(plain);
        if (!packed)
            return {};
        std::string text;
        text.reserve(packed->size() / 4 * 5 + 5);
        for (std::size_t at = 0; at < packed->size(); at += 4)
        {
            const std::size_t count = std::min<std::size_t>(4, packed->size() - at);
            std::uint32_t word = 0;
            for (std::size_t i = 0; i < 4; ++i)
                word = (word << 8) | (i < count ? (*packed)[at + i] : 0u);
            char chunk[5];
            std::uint64_t value = word;
            for (int i = 4; i >= 0; --i)
            {
                chunk[i] = kBase85[value % 85];
                value /= 85;
            }
            text.append(chunk, count + 1);
        }
        return text;
    }

    std::vector<std::unique_ptr<MemoryRecord>> RecordsFromXml(std::string_view text)
    {
        LoadedTable table;
        if (!ParseTable(text, table))
            return {};
        return std::move(table.Records);
    }
}
