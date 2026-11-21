#include "UI/StructureWindow.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "Table/ManagedStructures.h"
#include "Table/RttiStructures.h"
#include "Table/UnrealStructures.h"
#include "Target/Mono.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace Manifold::UI
{
    namespace
    {
        Table::StructDefinition* LiveStructure(Engine& engine, const Table::StructDefinition* definition)
        {
            for (const auto& item : engine.Structures().Items)
            {
                if (item.get() == definition)
                    return item.get();
            }
            return nullptr;
        }

        bool Embedded(const Table::StructField& field)
        {
            return field.Type != Scan::ValueType::Pointer && !field.ChildStructure.empty();
        }

        WorkOptions ManagedLookup()
        {
            return { .Title = "Reading the managed class", .Group = "structure", .Cancellable = false, .Quiet = true, .Serial = true };
        }

        WorkOptions UnrealLookup()
        {
            return { .Title = "Reading the Unreal object", .Group = "structure", .Cancellable = false, .Quiet = true, .Serial = true };
        }

        WorkOptions RttiLookup()
        {
            return { .Title = "Reading the C++ class", .Group = "structure", .Cancellable = false, .Quiet = true, .Serial = true };
        }

        Table::UnrealArrayLookup UnrealArrays(Engine& engine, std::shared_ptr<const Target::UnrealRuntime> runtime)
        {
            return [&engine, runtime = std::move(runtime)](std::uint64_t data) -> std::optional<std::pair<Target::UnrealField, std::uint64_t>>
                {
                    const auto array = engine.Unreal().ArrayAt(data, runtime->ProcessHandle().get());
                    if (!array)
                        return std::nullopt;
                    return std::make_pair(array->Field, array->Header);
                };
        }

        bool NativeLookups(Engine& engine)
        {
            return engine.MonoPresent() || engine.Unreal().Names() || engine.Rtti().Dissects() || engine.Lua().HasStructureNameLookups();
        }
    }

    namespace
    {
        std::string FieldKey(const std::string& path, int offset, int twin)
        {
            std::string key = path + "/" + std::to_string(offset);
            if (twin > 0)
                key += "." + std::to_string(twin);
            return key;
        }

        const Scan::ValueType kFieldTypes[] = { Scan::ValueType::Byte, Scan::ValueType::TwoBytes, Scan::ValueType::FourBytes, Scan::ValueType::EightBytes,
            Scan::ValueType::Binary, Scan::ValueType::Float, Scan::ValueType::Double, Scan::ValueType::Pointer, Scan::ValueType::String, Scan::ValueType::ByteArray };

        bool IsWholeNumber(Scan::ValueType type)
        {
            return type == Scan::ValueType::Byte || type == Scan::ValueType::TwoBytes || type == Scan::ValueType::FourBytes ||
                   type == Scan::ValueType::EightBytes || type == Scan::ValueType::Binary;
        }

        bool ShowsBits(const Table::StructField& field)
        {
            return field.CustomType.empty() && (field.Type == Scan::ValueType::Binary || (field.Display == Table::FieldDisplay::Binary && IsWholeNumber(field.Type)));
        }

        int BitCount(const Table::StructField& field)
        {
            return field.Type == Scan::ValueType::Binary ? std::clamp(field.BitLength, 1, 64) : std::clamp(field.Size, 1, 8) * 8;
        }

        std::uint64_t BitMask(int bits)
        {
            return bits >= 64 ? ~0ull : (1ull << bits) - 1;
        }

        std::uint64_t RawValue(std::span<const std::uint8_t> bytes)
        {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes.data(), std::min<std::size_t>(8, bytes.size()));
            return value;
        }

        std::uint64_t FieldBits(const Table::StructField& field, std::span<const std::uint8_t> bytes)
        {
            const std::uint64_t raw = RawValue(bytes);
            if (field.Type == Scan::ValueType::Binary)
                return (raw >> std::clamp(field.BitStart, 0, 63)) & BitMask(BitCount(field));
            return raw & BitMask(BitCount(field));
        }

        std::string BitText(std::uint64_t value, int bits)
        {
            std::string text;
            for (int bit = bits - 1; bit >= 0; --bit)
            {
                text += ((value >> bit) & 1) ? '1' : '0';
                if (bit > 0 && bit % 4 == 0)
                    text += ' ';
            }
            return text;
        }

        std::string BitsValueText(const Table::StructField& field, std::uint64_t value)
        {
            const int bits = BitCount(field);
            switch (field.Display)
            {
            case Table::FieldDisplay::Binary: return BitText(value, bits);
            case Table::FieldDisplay::Hex: return Hex(value);
            case Table::FieldDisplay::Signed:
                if (bits < 64 && ((value >> (bits - 1)) & 1))
                    return std::to_string(static_cast<std::int64_t>(value | ~BitMask(bits)));
                return std::to_string(static_cast<std::int64_t>(value));
            default: return std::to_string(value);
            }
        }

        std::optional<std::uint64_t> ParseBitsValue(const Table::StructField& field, std::string_view text)
        {
            std::string clean;
            for (char c : text)
            {
                if (c != ' ' && c != '_' && c != '\'')
                    clean += c;
            }
            if (clean.empty())
                return std::nullopt;
            std::optional<std::uint64_t> value;
            if (field.Display == Table::FieldDisplay::Binary)
            {
                std::string_view digits = clean;
                if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B'))
                    digits.remove_prefix(2);
                if (digits.empty() || digits.size() > 64 || digits.find_first_not_of("01") != std::string_view::npos)
                    return std::nullopt;
                std::uint64_t bits = 0;
                for (char c : digits)
                    bits = (bits << 1) | static_cast<std::uint64_t>(c - '0');
                value = bits;
            }
            else if (field.Display == Table::FieldDisplay::Hex)
                value = ParseHex(clean);
            else if (const auto number = ParseDecimal(clean))
                value = static_cast<std::uint64_t>(*number);
            if (!value)
                return std::nullopt;
            return *value & BitMask(BitCount(field));
        }

        std::string TypeLabel(const Table::StructField& field)
        {
            if (!field.CustomType.empty())
                return field.CustomType;
            if (Embedded(field))
            {
                const std::size_t dot = field.ChildStructure.rfind('.');
                return dot == std::string::npos ? field.ChildStructure : field.ChildStructure.substr(dot + 1);
            }
            if (field.Type == Scan::ValueType::String)
                return "String[" + std::to_string(field.Size) + "]";
            if (field.Type == Scan::ValueType::ByteArray)
                return "AOB[" + std::to_string(field.Size) + "]";
            if (field.Type == Scan::ValueType::Binary)
                return field.BitLength <= 1 ? "Bit " + std::to_string(field.BitStart) : "Bits " + std::to_string(field.BitStart) + "-" + std::to_string(field.BitStart + field.BitLength - 1);
            return Scan::TypeName(field.Type);
        }

        constexpr std::int64_t kMaxSnapshotBytes = 0x10000;
        constexpr int kMaxDepth = 12;

        int SizeOf(Scan::ValueType type, int pointerSize, int current)
        {
            switch (type)
            {
            case Scan::ValueType::Byte: return 1;
            case Scan::ValueType::TwoBytes: return 2;
            case Scan::ValueType::FourBytes:
            case Scan::ValueType::Float: return 4;
            case Scan::ValueType::EightBytes:
            case Scan::ValueType::Double: return 8;
            case Scan::ValueType::Pointer: return pointerSize;
            case Scan::ValueType::Binary: return 1;
            default: return std::max(1, current);
            }
        }

        Scan::ValueFormat FieldFormat(const Table::StructField& field, int pointerSize)
        {
            Scan::ValueFormat format;
            format.Hex = field.Display == Table::FieldDisplay::Hex;
            format.Signed = field.Display == Table::FieldDisplay::Signed;
            format.Unicode = field.Unicode;
            format.Length = field.Size;
            format.BitStart = field.BitStart;
            format.BitLength = field.BitLength;
            format.PointerSize = pointerSize;
            return format;
        }

        std::uint64_t FieldAddress(std::uint64_t base, int offset)
        {
            return base ? base + static_cast<std::uint64_t>(static_cast<std::int64_t>(offset)) : 0;
        }

        Table::MemoryRecord& StructureToAddressList(Engine& engine, const Table::StructDefinition& definition, std::uint64_t base)
        {
            Table::AddressList& table = engine.Table();
            Table::MemoryRecord& group = table.AddGroup(definition.Name.empty() ? "Structure" : definition.Name);
            group.AddressGroupHeader = true;
            if (base)
                group.Address = engine.Symbols().ModuleOffset(base);
            for (const Table::StructField& field : definition.Fields)
            {
                const std::int64_t offset = field.Offset;
                std::string address = offset < 0 ? "-" + Hex(static_cast<std::uint64_t>(-offset)) : "+" + Hex(static_cast<std::uint64_t>(offset));
                Table::MemoryRecord& record = table.AddValue(field.Name, std::move(address), field.Type, &group);
                record.Length = field.Size;
                record.Unicode = field.Unicode;
                record.ShowHex = field.Display == Table::FieldDisplay::Hex;
                record.CustomTypeName = field.CustomType;
                if (field.Type == Scan::ValueType::Binary)
                {
                    record.BitStart = field.BitStart;
                    record.BitLength = field.BitLength;
                }
            }
            engine.RefreshValues(true);
            return group;
        }
    }

    StructureWindow::StructureWindow(Workspace& workspace, int number) : m_workspace(workspace), m_engine(workspace.Core()), m_number(number)
    {
        m_title = NextWindowTitle(ICON_FA_SITEMAP "  Structure Dissect", number);
        m_windowId = m_title.substr(m_title.find("###"));
        m_columns.push_back({});
    }

    void StructureWindow::Show(std::uint64_t address, const std::string& structure)
    {
        m_open = true;
        RequestWindowRaise(m_title.c_str());
        if (!structure.empty() && m_engine.Structures().Find(structure))
        {
            m_structure = structure;
            m_expanded.clear();
        }
        if (address)
        {
            m_columns.front().Address.Text = m_engine.Symbols().ModuleOffset(address);
            if (!Current())
            {
                m_newOpen = true;
                PrepareNew(address);
            }
        }
    }

    void StructureWindow::LookupName(std::uint64_t address, bool containing, std::function<void(std::optional<Lua::StructureLookup>)> done)
    {
        const std::weak_ptr<int> alive = m_alive;
        Engine& engine = m_engine;
        auto scripts = [&engine, alive, address, done]()
            {
                if (alive.expired())
                    return;
                if (!engine.Lua().HasStructureNameLookups())
                {
                    done(std::nullopt);
                    return;
                }
                engine.Lua().LookupStructureNameLater(address, [alive, done](std::optional<Lua::StructureLookup> found)
                    {
                        if (!alive.expired())
                            done(std::move(found));
                    });
            };
        auto rtti = [&engine, alive, address, scripts, done]()
            {
                if (alive.expired())
                    return;
                const auto runtime = engine.Rtti().Dissects() ? engine.Rtti().Runtime() : nullptr;
                if (!runtime)
                {
                    scripts();
                    return;
                }
                engine.Work().Start(RttiLookup(), [&engine, runtime, alive, address, scripts, done](WorkProgress&) -> BackgroundWork::Finish
                    {
                        const auto name = Table::RttiStructureName(*runtime, address);
                        return [&engine, alive, address, name, scripts, done]()
                            {
                                if (alive.expired())
                                    return;
                                if (!name || (name->Object && engine.Rtti().OthersClaim(address)))
                                    scripts();
                                else
                                    done(Lua::StructureLookup{ name->Name, name->Start });
                            };
                    });
            };
        auto managed = [&engine, alive, address, containing, scripts = rtti, done]()
            {
                if (alive.expired())
                    return;
                if (!engine.MonoPresent())
                {
                    scripts();
                    return;
                }
                engine.Work().Start(ManagedLookup(), [&engine, alive, address, containing, scripts, done](WorkProgress&) -> BackgroundWork::Finish
                    {
                        std::optional<Lua::StructureLookup> found;
                        if (containing)
                        {
                            if (const auto instance = engine.Mono().ClassAt(address))
                                found = Lua::StructureLookup{ instance->Name, instance->Base };
                        }
                        else if (const auto name = engine.Mono().ClassNameAt(address))
                        {
                            found = Lua::StructureLookup{ *name, address };
                        }
                        return [alive, found, scripts, done]()
                            {
                                if (alive.expired())
                                    return;
                                if (found)
                                    done(found);
                                else
                                    scripts();
                            };
                        });
            };
        if (!engine.Unreal().Names())
        {
            managed();
            return;
        }
        const auto runtime = engine.Unreal().Runtime();
        engine.Work().Start(UnrealLookup(), [&engine, runtime, alive, address, containing, managed, done](WorkProgress&) -> BackgroundWork::Finish
            {
                std::optional<Lua::StructureLookup> found;
                if (const auto name = Table::UnrealStructureName(*runtime, address, containing, UnrealArrays(engine, runtime)))
                    found = Lua::StructureLookup{ name->first, name->second };
                return [alive, found, managed, done]()
                    {
                        if (alive.expired())
                            return;
                        if (found)
                            done(found);
                        else
                            managed();
                    };
            });
    }

    void StructureWindow::PrepareNew(std::uint64_t address)
    {
        m_newFrom = address;
        m_newStart = address;
        m_newName = address ? "Structure at " + Hex(address) : "Structure";
        m_suggestedName = m_newName;
        m_lookupRunning = false;
        const std::uint64_t serial = ++m_lookupSerial;
        if (!address || !m_engine.HasProcess() || !NativeLookups(m_engine))
            return;
        m_lookupRunning = true;
        LookupName(address, true, [this, serial](std::optional<Lua::StructureLookup> found)
            {
                if (serial != m_lookupSerial)
                    return;
                m_lookupRunning = false;
                if (!found || found->Name.empty())
                    return;
                if (m_newName == m_suggestedName)
                {
                    m_newName = found->Name;
                    m_suggestedName = m_newName;
                }
                m_newStart = found->Address ? found->Address : m_newFrom;
            });
    }

    void StructureWindow::ExpandPointer(Table::StructDefinition& parent, Table::StructField& field, std::uint64_t target, const std::string& path)
    {
        const bool lookups = target && m_engine.HasProcess() && NativeLookups(m_engine);
        if (!lookups)
        {
            field.ChildStructure = Guess(parent.Name + "." + field.Name, target, 256).Name;
            return;
        }
        m_childLookups.insert(path);
        const std::string parentName = parent.Name;
        const std::string fieldName = field.Name;
        const int offset = field.Offset;
        LookupName(target, false, [this, parentName, fieldName, offset, target, path](std::optional<Lua::StructureLookup> found)
            {
                m_childLookups.erase(path);
                Table::StructureStore& store = m_engine.Structures();
                Table::StructDefinition* owner = store.Find(parentName);
                if (!owner)
                    return;
                const auto slot = std::find_if(owner->Fields.begin(), owner->Fields.end(), [&](const Table::StructField& item) { return item.Offset == offset && item.Name == fieldName; });
                if (slot == owner->Fields.end() || (!slot->ChildStructure.empty() && store.Find(slot->ChildStructure)))
                    return;
                const std::size_t index = static_cast<std::size_t>(slot - owner->Fields.begin());
                std::string child;
                if (found && !found->Name.empty() && found->Address == target)
                {
                    if (Table::StructDefinition* existing = store.Find(found->Name); existing && existing != owner)
                        child = existing->Name;
                    else
                        child = Guess(found->Name, target, 256).Name;
                }
                else
                {
                    child = Guess(parentName + "." + fieldName, target, 256).Name;
                }
                owner->Fields[index].ChildStructure = child;
                ++store.Generation;
            });
    }

    std::string StructureWindow::FieldText(const Table::StructField& field, std::span<const std::uint8_t> bytes, std::uint64_t address, int pointerSize)
    {
        if (!field.CustomType.empty())
        {
            std::string text;
            const Lua::CustomType* custom = m_engine.Lua().FindCustomType(field.CustomType);
            if (custom && m_engine.Lua().CustomToText(*custom, bytes, address, text))
                return text;
            return "??";
        }
        if (ShowsBits(field))
            return BitsValueText(field, FieldBits(field, bytes));
        return Scan::FormatValue(field.Type, bytes, FieldFormat(field, pointerSize));
    }

    Status StructureWindow::WriteBits(const Table::StructField& field, std::uint64_t address, const std::string& text)
    {
        const auto value = ParseBitsValue(field, text);
        if (!value)
            return Status::Fail(ErrorCode::ScanBadValue, text);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::clamp(field.Size, 1, 8)));
        if (!m_engine.Process()->ReadExact(address, bytes.data(), bytes.size()))
            return Status::Fail(ErrorCode::ReadFailed, Hex(address));
        std::uint64_t whole = RawValue(bytes);
        if (field.Type == Scan::ValueType::Binary)
        {
            const int start = std::clamp(field.BitStart, 0, 63);
            const std::uint64_t mask = BitMask(BitCount(field)) << start;
            whole = (whole & ~mask) | ((*value << start) & mask);
        }
        else
        {
            whole = *value;
        }
        std::memcpy(bytes.data(), &whole, bytes.size());
        return m_engine.Process()->Write(address, bytes);
    }

    Table::StructDefinition* StructureWindow::Current()
    {
        return m_structure.empty() ? nullptr : m_engine.Structures().Find(m_structure);
    }

    Table::StructDefinition& StructureWindow::Guess(const std::string& name, std::uint64_t address, int size)
    {
        Table::StructureStore& store = m_engine.Structures();
        std::string unique = name;
        for (int suffix = 2; store.Find(unique); ++suffix)
            unique = name + " " + std::to_string(suffix);
        Table::StructDefinition& definition = store.Add(unique);
        if (!m_engine.HasProcess())
            return definition;
        const bool mono = address && m_engine.MonoPresent();
        if (mono || (address && (m_engine.Unreal().Dissects() || m_engine.Rtti().Dissects() || m_engine.Lua().HasDissectOverrides())))
        {
            Fill(&definition, address, size, mono);
            return definition;
        }
        definition.Fields.clear();
        Table::GuessFields(*m_engine.Process(), definition, address, 0, size);
        ++store.Generation;
        return definition;
    }

    void StructureWindow::Fill(Table::StructDefinition* definition, std::uint64_t address, int size, bool mono)
    {
        m_filling.insert(definition);
        const std::weak_ptr<int> alive = m_alive;
        Engine& engine = m_engine;
        auto finish = [this, &engine, alive, definition, address, size, mono](bool handled, bool native)
            {
                if (!alive.expired())
                    m_filling.erase(definition);
                Table::StructDefinition* live = LiveStructure(engine, definition);
                if (!live)
                    return;
                if (handled)
                {
                    live->Sort();
                    if (!mono && !native && engine.HasProcess() && live->Size() < size)
                        Table::GuessFields(*engine.Process(), *live, address, live->Size(), size - live->Size());
                }
                else if (engine.HasProcess())
                {
                    live->Fields.clear();
                    Table::GuessFields(*engine.Process(), *live, address, 0, size);
                }
                ++engine.Structures().Generation;
            };
        auto scripts = [&engine, definition, address, finish]()
            {
                if (!LiveStructure(engine, definition) || !engine.Lua().HasDissectOverrides())
                {
                    finish(false, false);
                    return;
                }
                engine.Lua().DissectStructureLater(definition, address, [finish](bool handled) { finish(handled, false); });
            };
        auto rtti = [this, &engine, alive, definition, address, size, scripts, finish]()
            {
                const auto runtime = engine.Rtti().Dissects() ? engine.Rtti().Runtime() : nullptr;
                if (!runtime || !LiveStructure(engine, definition))
                {
                    scripts();
                    return;
                }
                const std::uint64_t generation = engine.ProcessGeneration();
                std::set<std::string> existing = Table::StructureNames(engine.Structures());
                engine.Work().Start(RttiLookup(), [this, &engine, runtime, alive, generation, definition, address, size, existing = std::move(existing), scripts, finish](WorkProgress&) -> BackgroundWork::Finish
                    {
                        auto fill = std::make_shared<Table::RttiFill>(Table::FillFromRtti(*runtime, address, size, existing));
                        return [this, &engine, alive, generation, definition, address, fill, scripts, finish]()
                            {
                                if (generation != engine.ProcessGeneration())
                                {
                                    if (!alive.expired())
                                        m_filling.erase(definition);
                                    return;
                                }
                                if (!fill->Handled || (fill->Object && engine.Rtti().OthersClaim(address)))
                                {
                                    scripts();
                                    return;
                                }
                                Table::StructDefinition* live = LiveStructure(engine, definition);
                                if (!live)
                                {
                                    finish(false, true);
                                    return;
                                }
                                Table::ApplyRttiFill(engine.Structures(), *live, std::move(*fill));
                                finish(true, true);
                            };
                    });
            };
        if (!mono && engine.Unreal().Dissects())
        {
            const auto runtime = engine.Unreal().Runtime();
            const bool customTypes = engine.Unreal().Config().Types;
            const std::uint64_t generation = engine.ProcessGeneration();
            engine.Work().Start(UnrealLookup(), [this, &engine, runtime, alive, generation, definition, address, customTypes, scripts = rtti, finish](WorkProgress&) -> BackgroundWork::Finish
                {
                    Table::UnrealFill fill = Table::FillFromUnreal(*runtime, address, UnrealArrays(engine, runtime), customTypes);
                    return [this, &engine, alive, generation, definition, fill = std::move(fill), scripts, finish]()
                        {
                            if (generation != engine.ProcessGeneration())
                            {
                                if (!alive.expired())
                                    m_filling.erase(definition);
                                return;
                            }
                            if (!fill.Handled)
                            {
                                scripts();
                                return;
                            }
                            Table::StructDefinition* live = LiveStructure(engine, definition);
                            if (!live)
                            {
                                finish(false, true);
                                return;
                            }
                            live->Fields = fill.Fields;
                            std::vector<std::pair<std::uint64_t, UnrealArray>> arrays;
                            for (const Table::UnrealArrayRef& array : fill.Arrays)
                                arrays.push_back({ array.Data, UnrealArray{ array.Field, array.Header } });
                            engine.Unreal().RememberArrays(arrays);
                            finish(true, true);
                        };
                });
            return;
        }
        if (!mono)
        {
            rtti();
            return;
        }
        engine.Work().Start(ManagedLookup(), [&engine, definition, address, scripts = rtti, finish](WorkProgress&) -> BackgroundWork::Finish
            {
                std::vector<Target::MonoField> fields;
                Table::ManagedLayouts nested;
                if (const auto className = engine.Mono().ClassNameAt(address))
                {
                    fields = engine.Mono().InstanceFieldsOf(*className);
                    Table::CollectManagedLayouts(engine.Mono(), fields, nested);
                }
                return [&engine, definition, fields, nested, scripts, finish]()
                    {
                        if (fields.empty())
                        {
                            scripts();
                            return;
                        }
                        Table::StructDefinition* live = LiveStructure(engine, definition);
                        if (!live)
                        {
                            finish(false, false);
                            return;
                        }
                        Table::FillFromManaged(*live, fields, 0, nested);
                        const int header = 2 * (engine.HasProcess() ? engine.Process()->PointerSize() : 8);
                        Table::AddManagedLayouts(engine.Structures(), nested, header, live->Name);
                        finish(true, false);
                    };
            });
    }

    void StructureWindow::Draw()
    {
        if (!m_open)
            return;
        if (!m_placed)
        {
            const std::uint32_t dock = m_engine.Options().StructureTabs ? m_workspace.StructureDock(*this) : 0;
            if (dock)
                ImGui::SetNextWindowDockID(dock, ImGuiCond_Always);
            else
                m_placed = true;
        }
        const bool visible = BeginToolWindow(m_title.c_str(), &m_open, ImVec2(820.0f, 640.0f), ImGuiWindowFlags_MenuBar, ImVec2(680.0f, 320.0f));
        if (!m_placed)
        {
            const ImGuiWindow* self = ImGui::FindWindowByName(m_windowId.c_str());
            m_placed = (self && self->DockId != 0) || ++m_dockTries >= 30;
        }
        if (!visible)
        {
            EndToolWindow();
            return;
        }
        MenuBar();
        if (!m_pendingStructure.empty())
        {
            if (Table::StructDefinition* target = m_engine.Structures().Find(m_pendingStructure))
            {
                if (m_pendingInsert)
                {
                    Table::StructField added;
                    added.Offset = m_pendingOffset;
                    added.Name = "Field " + Hex(static_cast<std::uint64_t>(added.Offset), 4);
                    target->Fields.push_back(added);
                    target->Sort();
                }
                else
                {
                    const int offset = m_pendingOffset;
                    target->Fields.erase(std::remove_if(target->Fields.begin(), target->Fields.end(), [&](const Table::StructField& item) { return item.Offset == offset; }),
                        target->Fields.end());
                }
                m_engine.Table().Touch();
            }
            m_pendingStructure.clear();
        }
        if (!m_pendingDeletes.empty())
        {
            for (const PendingDelete& pending : m_pendingDeletes)
            {
                if (Table::StructDefinition* target = m_engine.Structures().Find(pending.Structure))
                {
                    const int offset = pending.Offset;
                    target->Fields.erase(std::remove_if(target->Fields.begin(), target->Fields.end(), [&](const Table::StructField& item) { return item.Offset == offset; }), target->Fields.end());
                }
            }
            m_pendingDeletes.clear();
            m_engine.Table().Touch();
        }
        Toolbar();
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputTextWithHint("##fieldfilter", ICON_FA_FILTER "  Filter the fields: name, offset (+1A0), type or value", m_filter);
        Table::StructDefinition* definition = Current();
        if (m_structure != m_selectionStructure)
        {
            m_selected.clear();
            m_selectAnchor.clear();
            m_selectionStructure = m_structure;
        }
        std::vector<std::uint64_t> bases;
        for (Column& column : m_columns)
        {
            const std::optional<std::uint64_t>& value = Resolve(m_engine, column.Address);
            column.Valid = value.has_value();
            column.Value = value.value_or(0);
        }
        for (const Column& column : m_columns)
            bases.push_back(column.Valid ? column.Value : 0);

        m_rows.clear();
        m_blocks.clear();
        m_snapshots.clear();
        if (!definition)
        {
            Muted("Pick a structure above, or let one be guessed from the memory at the address.");
        }
        else
        {
            BuildRows(*definition, std::move(bases), "", 0);
            const int pointerSize = m_engine.HasProcess() ? m_engine.Process()->PointerSize() : 8;
            const std::size_t total = m_rows.size();
            if (const std::string needle = Lower(Trim(m_filter)); !needle.empty())
            {
                std::string offsetNeedle = needle;
                if (!offsetNeedle.empty() && offsetNeedle.front() == '+')
                    offsetNeedle.erase(0, 1);
                if (offsetNeedle.rfind("0x", 0) == 0)
                    offsetNeedle.erase(0, 2);
                const bool hexNeedle = !offsetNeedle.empty() && std::all_of(offsetNeedle.begin(), offsetNeedle.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
                std::erase_if(m_rows, [&](const Row& row)
                    {
                        const Table::StructField& field = row.Definition->Fields[static_cast<std::size_t>(row.Field)];
                        if (Lower(field.Name).find(needle) != std::string::npos || Lower(TypeLabel(field)).find(needle) != std::string::npos)
                            return false;
                        if (hexNeedle && Lower(Hex(static_cast<std::uint64_t>(field.Offset))).find(offsetNeedle) != std::string::npos)
                            return false;
                        const Block& block = m_blocks[static_cast<std::size_t>(row.Block)];
                        const std::uint64_t base = block.Bases.empty() ? 0 : block.Bases.front();
                        if (!base || !m_engine.HasProcess() || Embedded(field))
                            return true;
                        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::clamp(field.Size, 1, 256)));
                        const std::uint64_t address = FieldAddress(base, field.Offset);
                        if (!m_engine.Process()->ReadExact(address, bytes.data(), bytes.size()))
                            return true;
                        return Lower(FieldText(field, bytes, address, pointerSize)).find(needle) == std::string::npos;
                    });
            }
            if (!m_filter.empty())
                Muted(std::to_string(m_rows.size()) + " of " + std::to_string(total) + " fields match");
            const int columns = 3 + static_cast<int>(m_columns.size());
            const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##fields", columns, flags))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                for (std::size_t i = 0; i < m_columns.size(); ++i)
                    ImGui::TableSetupColumn(("Value " + std::to_string(i + 1)).c_str(), ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableHeadersRow();
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(m_rows.size()));
                if (const int editRow = FindEditRow(); editRow >= 0)
                    clipper.IncludeItemByIndex(editRow);
                while (clipper.Step())
                {
                    ReadVisible(clipper.DisplayStart, clipper.DisplayEnd);
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        DrawRow(i, pointerSize);
                }
                FieldShortcuts(pointerSize);
                ImGui::EndTable();
            }
        }
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope == CommandScope::Structure && IsWindowCommand(info.Id) && Pressed(info.Id))
                RunCommand(info.Id);
        }

        if (m_newOpen)
        {
            ImGui::OpenPopup("##newstructure");
            m_newOpen = false;
        }
        if (ImGui::BeginPopup("##newstructure"))
        {
            ImGui::TextUnformatted("Guess a new structure from the first address");
            const bool moved = m_newStart && m_newStart != m_newFrom && !m_columns.empty() && m_columns.front().Valid && m_columns.front().Value == m_newFrom;
            if (moved)
                Muted("Starts at " + Hex(m_newStart) + ", where the structure that contains " + Hex(m_newFrom) + " begins");
            ImGui::SetNextItemWidth(Px(280.0f));
            InputText("##name", m_newName);
            if (m_lookupRunning)
            {
                ImGui::SameLine(0.0f, Px(8.0f));
                Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
                ImGui::SameLine(0.0f, Px(6.0f));
                Muted("Looking up the name");
            }
            ImGui::SetNextItemWidth(Px(120.0f));
            ImGui::InputInt("Size (bytes)", &m_guessSize, 64, 1024);
            m_guessSize = std::clamp(m_guessSize, 8, 0x100000);
            if (AccentButton("Create", Px(100.0f, 0.0f)))
            {
                if (moved)
                {
                    m_columns.front().Address.Text = m_engine.Symbols().ModuleOffset(m_newStart);
                    m_columns.front().Value = m_newStart;
                }
                if (!m_columns.empty() && m_columns.front().Valid)
                {
                    Table::StructDefinition& created = Guess(m_newName.empty() ? "Structure" : m_newName, m_columns.front().Value, m_guessSize);
                    m_structure = created.Name;
                    m_expanded.clear();
                }
                else
                {
                    m_workspace.Report(Status::Fail(ErrorCode::ExprSyntax, m_columns.empty() ? "" : m_columns.front().Address.Text));
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (m_renameOpen)
        {
            ImGui::OpenPopup("##renamestructure");
            m_renameOpen = false;
        }
        if (ImGui::BeginPopup("##renamestructure"))
        {
            ImGui::SetNextItemWidth(Px(260.0f));
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            if (InputText("##rename", m_renameText, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (Table::StructDefinition* current = Current(); current && !m_renameText.empty() && !m_engine.Structures().Find(m_renameText))
                {
                    for (const auto& item : m_engine.Structures().Items)
                    {
                        for (Table::StructField& field : item->Fields)
                        {
                            if (field.ChildStructure == current->Name)
                                field.ChildStructure = m_renameText;
                        }
                    }
                    current->Name = m_renameText;
                    m_structure = m_renameText;
                    m_engine.Table().Touch();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        EndToolWindow();
    }

    bool StructureWindow::IsWindowCommand(Command command)
    {
        return command == Command::DefineStructure || command == Command::AddStructureColumn || command == Command::CollapseStructure;
    }

    void StructureWindow::RunCommand(Command command)
    {
        switch (command)
        {
        case Command::DefineStructure:
            m_newOpen = true;
            PrepareNew(!m_columns.empty() && m_columns.front().Valid ? m_columns.front().Value : 0);
            break;
        case Command::AddStructureColumn: m_columns.push_back({}); break;
        case Command::CollapseStructure: m_expanded.clear(); break;
        default: break;
        }
    }

    void StructureWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        const auto item = [&](const char* label, Command command, bool enabled = true)
            {
                if (CommandItem(label, command, false, enabled))
                    RunCommand(command);
            };
        Table::StructDefinition* current = Current();
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem(ICON_FA_CLONE "  New Window"))
                m_workspace.OpenStructure(!m_columns.empty() && m_columns.front().Valid ? m_columns.front().Value : 0, m_structure, true);
            if (ImGui::MenuItem(ICON_FA_SEARCH_LOCATION "  Find Static Pointers to the Address", nullptr, false, !m_columns.empty() && m_columns.front().Valid && m_engine.HasProcess()))
                m_workspace.FindStaticPointers(m_columns.front().Value);
            if (ImGui::MenuItem(ICON_FA_ROUTE "  Pointer Scan for the Address", nullptr, false, !m_columns.empty() && m_columns.front().Valid && m_engine.HasProcess()))
                m_workspace.OpenPointerScan(m_columns.front().Value);
            ImGui::Separator();
            item(ICON_FA_PLUS "  Add an Address to Compare", Command::AddStructureColumn);
            ImGui::BeginDisabled(m_columns.size() < 2);
            if (ImGui::MenuItem(ICON_FA_TIMES "  Remove the Last Address"))
                m_columns.pop_back();
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TIMES "  Close"))
                m_open = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Structures"))
        {
            item(ICON_FA_MAGIC "  Define a New Structure...", Command::DefineStructure);
            if (!m_engine.Structures().Items.empty())
            {
                ImGui::Separator();
                for (const auto& structure : m_engine.Structures().Items)
                {
                    if (ImGui::MenuItem(structure->Name.c_str(), nullptr, structure->Name == m_structure))
                    {
                        m_structure = structure->Name;
                        m_expanded.clear();
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Rename the Structure...", nullptr, false, current != nullptr))
            {
                m_renameOpen = true;
                m_renameText = current->Name;
            }
            if (ImGui::MenuItem(ICON_FA_LIST "  Add to the Address List", nullptr, false, current != nullptr && !current->Fields.empty()))
            {
                const std::uint64_t base = !m_columns.empty() && m_columns.front().Valid ? m_columns.front().Value : 0;
                StructureToAddressList(m_engine, *current, base);
            }
            ImGui::Separator();
            if (DangerMenuItem(ICON_FA_TRASH "  Delete the Structure", nullptr, current != nullptr))
            {
                const std::string name = current->Name;
                m_workspace.Confirm("Delete Structure", "Delete the structure \"" + name + "\"?", [this, name]()
                    {
                        if (Table::StructDefinition* target = m_engine.Structures().Find(name))
                            m_engine.Structures().Remove(*target);
                        if (m_structure == name)
                            m_structure.clear();
                    }, "Delete");
            }
            if (DangerMenuItem(ICON_FA_BROOM "  Delete All Structures", nullptr, !m_engine.Structures().Items.empty()))
            {
                const std::size_t count = m_engine.Structures().Items.size();
                m_workspace.Confirm("Delete All Structures", "Delete all " + std::to_string(count) + (count == 1 ? " structure?" : " structures?"), [this]()
                    {
                        m_engine.Structures().Clear();
                        m_structure.clear();
                        m_expanded.clear();
                    }, "Delete all");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            item(ICON_FA_COMPRESS_ALT "  Collapse All Pointers", Command::CollapseStructure, !m_expanded.empty());
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void StructureWindow::FieldShortcuts(int pointerSize)
    {
        if (!m_editPath.empty())
            return;
        Table::StructDefinition* definition = nullptr;
        Table::StructField* field = nullptr;
        std::uint64_t address = 0;
        for (const Row& row : m_rows)
        {
            Table::StructField& candidate = row.Definition->Fields[static_cast<std::size_t>(row.Field)];
            const Block& block = m_blocks[static_cast<std::size_t>(row.Block)];
            if (FieldKey(block.Path, candidate.Offset, row.Twin) != m_selectAnchor)
                continue;
            definition = row.Definition;
            field = &candidate;
            address = block.Bases.empty() ? 0 : FieldAddress(block.Bases.front(), candidate.Offset);
            break;
        }
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope == CommandScope::Structure && !IsWindowCommand(info.Id) && Pressed(info.Id) && field)
                RunFieldCommand(info.Id, *definition, *field, address, m_selectAnchor, pointerSize);
        }
    }

    void StructureWindow::RunFieldCommand(Command command, Table::StructDefinition& definition, Table::StructField& field, std::uint64_t address, const std::string& path, int pointerSize)
    {
        const bool readable = address != 0 && m_engine.HasProcess();
        const int watchSize = std::min(8, std::max(1, field.Size));
        switch (command)
        {
        case Command::ChangeFieldValue:
            if (readable)
            {
                m_editPath = path;
                m_editName = false;
                m_editText.clear();
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max(1, field.Size)));
                if (m_engine.Process()->ReadExact(address, bytes.data(), bytes.size()))
                    m_editText = FieldText(field, bytes, address, pointerSize);
            }
            break;
        case Command::RenameField:
            m_editPath = path;
            m_editName = true;
            m_editText = field.Name;
            break;
        case Command::InsertField:
            m_pendingStructure = definition.Name;
            m_pendingOffset = field.Offset + field.Size;
            m_pendingInsert = true;
            break;
        case Command::DeleteField:
            if (m_selected.size() > 1 && m_selected.count(path))
            {
                DeleteSelection();
            }
            else
            {
                m_pendingStructure = definition.Name;
                m_pendingOffset = field.Offset;
                m_pendingInsert = false;
            }
            break;
        case Command::FieldToAddressList:
            if (m_selected.size() > 1 && m_selected.count(path))
            {
                AddSelectionToAddressList();
            }
            else if (readable)
            {
                Table::MemoryRecord& record = m_engine.Table().AddValue(field.Name, m_engine.Symbols().ModuleOffset(address), field.Type);
                record.Length = field.Size;
                record.Unicode = field.Unicode;
                record.ShowHex = field.Display == Table::FieldDisplay::Hex;
                record.CustomTypeName = field.CustomType;
                if (field.Type == Scan::ValueType::Binary)
                {
                    record.BitStart = field.BitStart;
                    record.BitLength = field.BitLength;
                }
                m_engine.RefreshValues(true);
            }
            break;
        case Command::BrowseField:
            if (readable)
                m_workspace.OpenMemoryViewer(address, true);
            break;
        case Command::BrowseFieldPointer:
            if (readable && field.Type == Scan::ValueType::Pointer)
            {
                if (const auto target = m_engine.Process()->ReadPointer(address); target && *target)
                    m_workspace.OpenMemoryViewer(*target, true);
            }
            break;
        case Command::FieldAccesses:
            if (readable)
                m_workspace.StartWatch(Debug::WatchKind::Accesses, address, watchSize);
            break;
        case Command::FieldWrites:
            if (readable)
                m_workspace.StartWatch(Debug::WatchKind::Writes, address, watchSize);
            break;
        default:
            break;
        }
    }

    void StructureWindow::AddSelectionToAddressList()
    {
        if (!m_engine.HasProcess())
            return;
        bool any = false;
        for (const Row& row : m_rows)
        {
            const Table::StructField& field = row.Definition->Fields[static_cast<std::size_t>(row.Field)];
            const Block& block = m_blocks[static_cast<std::size_t>(row.Block)];
            if (!m_selected.count(FieldKey(block.Path, field.Offset, row.Twin)))
                continue;
            const std::uint64_t address = block.Bases.empty() ? 0 : FieldAddress(block.Bases.front(), field.Offset);
            if (!address)
                continue;
            Table::MemoryRecord& record = m_engine.Table().AddValue(field.Name, m_engine.Symbols().ModuleOffset(address), field.Type);
            record.Length = field.Size;
            record.Unicode = field.Unicode;
            record.ShowHex = field.Display == Table::FieldDisplay::Hex;
            record.CustomTypeName = field.CustomType;
            if (field.Type == Scan::ValueType::Binary)
            {
                record.BitStart = field.BitStart;
                record.BitLength = field.BitLength;
            }
            any = true;
        }
        if (any)
            m_engine.RefreshValues(true);
    }

    void StructureWindow::DeleteSelection()
    {
        for (const Row& row : m_rows)
        {
            const Table::StructField& field = row.Definition->Fields[static_cast<std::size_t>(row.Field)];
            const Block& block = m_blocks[static_cast<std::size_t>(row.Block)];
            if (!m_selected.count(FieldKey(block.Path, field.Offset, row.Twin)))
                continue;
            m_pendingDeletes.push_back({ row.Definition->Name, field.Offset });
        }
        m_selected.clear();
        m_selectAnchor.clear();
    }

    void StructureWindow::Toolbar()
    {
        for (std::size_t i = 0; i < m_columns.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            AddressInput("##address", m_engine, m_columns[i].Address, Px(200.0f), "address of the structure");
            if (m_columns.size() > 1)
            {
                ImGui::SameLine(0.0f, Px(2.0f));
                if (IconButton(ICON_FA_TIMES, "Remove This Column"))
                {
                    m_columns.erase(m_columns.begin() + static_cast<std::ptrdiff_t>(i));
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::SameLine();
            ImGui::PopID();
        }
        if (IconButton(ICON_FA_PLUS "##addcolumn", WithKeys("Add an Address to Compare", Command::AddStructureColumn).c_str()))
            m_columns.push_back({});

        ImGui::SetNextItemWidth(Px(240.0f));
        if (ImGui::BeginCombo("##structure", m_structure.empty() ? "No structure" : m_structure.c_str()))
        {
            for (const auto& item : m_engine.Structures().Items)
            {
                if (ImGui::Selectable(item->Name.c_str(), item->Name == m_structure))
                {
                    m_structure = item->Name;
                    m_expanded.clear();
                }
            }
            ImGui::EndCombo();
        }
        Table::StructDefinition* current = Current();

        ImGui::SameLine(0.0f, Px(14.0f));
        if (IconButton(ICON_FA_MAGIC, WithKeys("Guess a new structure from the memory at the first address", Command::DefineStructure).c_str()))
        {
            m_newOpen = true;
            PrepareNew(m_columns.front().Valid ? m_columns.front().Value : 0);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!current);
        if (IconButton(ICON_FA_I_CURSOR, "Rename the Structure") && current)
        {
            m_renameOpen = true;
            m_renameText = current->Name;
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::SetNextItemWidth(Px(72.0f));
        InputTextWithHint("##addoffset", "offset", m_addOffset, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine(0.0f, Px(4.0f));
        ImGui::BeginDisabled(!current);
        if (IconButton(ICON_FA_PLUS "##addfield", "Add a field at that offset, or at the end of the structure when it is blank") && current)
        {
            Table::StructField field;
            field.Offset = static_cast<int>(ParseHex(m_addOffset).value_or(static_cast<std::uint64_t>(current->Size())));
            field.Name = "Field " + Hex(static_cast<std::uint64_t>(field.Offset), 4);
            current->Fields.push_back(field);
            current->Sort();
            m_engine.Table().Touch();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!current || current->Fields.empty());
        if (IconButton(ICON_FA_LIST, "Add the whole structure to the address list: a group header at the base, one entry per field at its offset") && current && !current->Fields.empty())
        {
            const std::uint64_t base = !m_columns.empty() && m_columns.front().Valid ? m_columns.front().Value : 0;
            StructureToAddressList(m_engine, *current, base);
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::BeginDisabled(m_expanded.empty());
        if (IconButton(ICON_FA_COMPRESS_ALT, WithKeys("Collapse All Pointers", Command::CollapseStructure).c_str()))
            m_expanded.clear();
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::BeginDisabled(!current);
        if (IconButton(ICON_FA_TRASH, "Delete the Structure", true) && current)
        {
            const std::string name = current->Name;
            m_workspace.Confirm("Delete Structure", "Delete the structure \"" + name + "\"?", [this, name]()
                {
                    if (Table::StructDefinition* target = m_engine.Structures().Find(name))
                        m_engine.Structures().Remove(*target);
                    if (m_structure == name)
                        m_structure.clear();
                }, "Delete");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_engine.Structures().Items.empty());
        if (IconButton(ICON_FA_BROOM, "Delete All Structures", true))
        {
            const std::size_t count = m_engine.Structures().Items.size();
            m_workspace.Confirm("Delete All Structures", "Delete all " + std::to_string(count) + (count == 1 ? " structure?" : " structures?"), [this]()
                {
                    m_engine.Structures().Clear();
                    m_structure.clear();
                    m_expanded.clear();
                }, "Delete all");
        }
        ImGui::EndDisabled();

        if (!m_filling.empty())
        {
            ImGui::SameLine(0.0f, Px(14.0f));
            Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
            ImGui::SameLine(0.0f, Px(6.0f));
            ImGui::AlignTextToFramePadding();
            Muted(m_filling.size() == 1 ? std::string("Filling in the fields") : "Filling in " + std::to_string(m_filling.size()) + " structures");
        }
    }

    void StructureWindow::BuildRows(Table::StructDefinition& definition, std::vector<std::uint64_t> bases, const std::string& path, int depth)
    {
        if (depth > kMaxDepth)
            return;
        const int block = static_cast<int>(m_blocks.size());
        m_blocks.push_back({ std::move(bases), path });
        const bool anyExpanded = !m_expanded.empty();
        const bool ordered = std::is_sorted(definition.Fields.begin(), definition.Fields.end(), [](const Table::StructField& a, const Table::StructField& b) { return a.Offset < b.Offset; });
        std::map<int, int> seen;
        int twin = 0;
        for (std::size_t index = 0; index < definition.Fields.size(); ++index)
        {
            const Table::StructField& field = definition.Fields[index];
            if (ordered)
                twin = index > 0 && definition.Fields[index - 1].Offset == field.Offset ? twin + 1 : 0;
            else
                twin = seen[field.Offset]++;
            Row row{ &definition, static_cast<int>(index), depth, block, false, twin };
            const bool embedded = Embedded(field);
            if (!anyExpanded || (field.Type != Scan::ValueType::Pointer && !embedded))
            {
                m_rows.push_back(row);
                continue;
            }
            const std::string fieldPath = FieldKey(path, field.Offset, twin);
            row.Expanded = m_expanded.count(fieldPath) > 0;
            m_rows.push_back(row);
            if (!row.Expanded || field.ChildStructure.empty())
                continue;
            Table::StructDefinition* child = m_engine.Structures().Find(field.ChildStructure);
            if (!child || child == &definition)
                continue;
            std::vector<std::uint64_t> childBases;
            childBases.reserve(m_blocks[static_cast<std::size_t>(block)].Bases.size());
            for (std::uint64_t base : m_blocks[static_cast<std::size_t>(block)].Bases)
            {
                const std::uint64_t address = FieldAddress(base, field.Offset);
                if (embedded)
                    childBases.push_back(address);
                else
                    childBases.push_back(address && m_engine.HasProcess() ? m_engine.Process()->ReadPointer(address).value_or(0) : 0);
            }
            BuildRows(*child, std::move(childBases), fieldPath, depth + 1);
        }
    }

    int StructureWindow::FindEditRow() const
    {
        const std::size_t slash = m_editPath.rfind('/');
        if (m_editPath.empty() || slash == std::string::npos)
            return -1;
        const std::string_view prefix = std::string_view(m_editPath).substr(0, slash);
        for (std::size_t i = 0; i < m_rows.size(); ++i)
        {
            const Row& row = m_rows[i];
            const std::string& path = m_blocks[static_cast<std::size_t>(row.Block)].Path;
            if (path != prefix)
                continue;
            if (FieldKey(path, row.Definition->Fields[static_cast<std::size_t>(row.Field)].Offset, row.Twin) == m_editPath)
                return static_cast<int>(i);
        }
        return -1;
    }

    void StructureWindow::ReadVisible(int first, int last)
    {
        m_snapshots.clear();
        if (!m_engine.HasProcess() || first >= last)
            return;
        const Target::Process& process = *m_engine.Process();
        struct Span
        {
            int Block = 0;
            std::int64_t Begin = 0;
            std::int64_t End = 0;
        };
        std::vector<Span> spans;
        for (int i = first; i < last; ++i)
        {
            const Row& row = m_rows[static_cast<std::size_t>(i)];
            const Table::StructField& field = row.Definition->Fields[static_cast<std::size_t>(row.Field)];
            const std::int64_t begin = field.Offset;
            const std::int64_t end = begin + std::max(1, field.Size);
            const auto span = std::find_if(spans.begin(), spans.end(), [&](const Span& item) { return item.Block == row.Block; });
            if (span == spans.end())
            {
                spans.push_back({ row.Block, begin, end });
            }
            else
            {
                span->Begin = std::min(span->Begin, begin);
                span->End = std::max(span->End, end);
            }
        }
        for (const Span& span : spans)
        {
            if (span.End - span.Begin > kMaxSnapshotBytes)
                continue;
            const std::vector<std::uint64_t>& bases = m_blocks[static_cast<std::size_t>(span.Block)].Bases;
            for (std::size_t column = 0; column < bases.size(); ++column)
            {
                if (!bases[column])
                    continue;
                Snapshot snapshot;
                snapshot.Block = span.Block;
                snapshot.Column = column;
                snapshot.Start = bases[column] + static_cast<std::uint64_t>(span.Begin);
                snapshot.Bytes.resize(static_cast<std::size_t>(span.End - span.Begin));
                snapshot.Whole = process.ReadExact(snapshot.Start, snapshot.Bytes.data(), snapshot.Bytes.size());
                if (!snapshot.Whole)
                {
                    snapshot.Valid.assign(snapshot.Bytes.size(), 0);
                    const std::uint64_t end = snapshot.Start + snapshot.Bytes.size();
                    for (std::uint64_t at = snapshot.Start; at < end;)
                    {
                        const std::uint64_t pageEnd = std::min((at & ~0xFFFull) + 0x1000, end);
                        const std::size_t offset = static_cast<std::size_t>(at - snapshot.Start);
                        const std::size_t chunk = static_cast<std::size_t>(pageEnd - at);
                        if (process.ReadExact(at, snapshot.Bytes.data() + offset, chunk))
                            std::fill_n(snapshot.Valid.begin() + static_cast<std::ptrdiff_t>(offset), chunk, std::uint8_t(1));
                        at = pageEnd;
                    }
                }
                m_snapshots.push_back(std::move(snapshot));
            }
        }
    }

    bool StructureWindow::ReadField(int block, std::size_t column, std::uint64_t address, std::uint8_t* out, std::size_t size) const
    {
        for (const Snapshot& snapshot : m_snapshots)
        {
            if (snapshot.Block != block || snapshot.Column != column || address < snapshot.Start || address - snapshot.Start + size > snapshot.Bytes.size())
                continue;
            const std::size_t at = static_cast<std::size_t>(address - snapshot.Start);
            if (!snapshot.Whole)
            {
                const auto begin = snapshot.Valid.begin() + static_cast<std::ptrdiff_t>(at);
                if (std::find(begin, begin + static_cast<std::ptrdiff_t>(size), std::uint8_t(0)) != begin + static_cast<std::ptrdiff_t>(size))
                    return false;
            }
            std::memcpy(out, snapshot.Bytes.data() + at, size);
            return true;
        }
        return m_engine.HasProcess() && m_engine.Process()->ReadExact(address, out, size);
    }

    void StructureWindow::DrawRow(int index, int pointerSize)
    {
        const Row row = m_rows[static_cast<std::size_t>(index)];
        Table::StructDefinition& definition = *row.Definition;
        Table::StructField& field = definition.Fields[static_cast<std::size_t>(row.Field)];
        const Block& block = m_blocks[static_cast<std::size_t>(row.Block)];
        const std::string fieldPath = FieldKey(block.Path, field.Offset, row.Twin);
        std::vector<std::uint64_t> addresses;
        addresses.reserve(block.Bases.size());
        for (std::uint64_t base : block.Bases)
            addresses.push_back(FieldAddress(base, field.Offset));

        ImGui::TableNextRow();
        ImGui::PushID(fieldPath.c_str());
        ImGui::TableNextColumn();
        const float indent = Px(14.0f) * static_cast<float>(row.Depth);
        if (indent > 0.0f)
            ImGui::Indent(indent);
        const bool pointer = field.Type == Scan::ValueType::Pointer;
        const bool embedded = Embedded(field);
        const bool expanded = (pointer || embedded) && row.Expanded;
        if (pointer || embedded)
        {
            if (m_childLookups.count(fieldPath))
                Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
            else
                ImGui::TextColored(ManifoldTheme::Label, "%s", expanded ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
            if (ImGui::IsItemClicked())
            {
                if (expanded)
                {
                    m_expanded.erase(fieldPath);
                }
                else
                {
                    m_expanded.insert(fieldPath);
                    if (pointer && (field.ChildStructure.empty() || !m_engine.Structures().Find(field.ChildStructure)) && !m_childLookups.count(fieldPath))
                    {
                        std::uint64_t target = 0;
                        if (!addresses.empty() && addresses.front() && m_engine.HasProcess())
                            target = m_engine.Process()->ReadPointer(addresses.front()).value_or(0);
                        ExpandPointer(definition, field, target, fieldPath);
                    }
                }
            }
            ImGui::SameLine(0.0f, Px(4.0f));
        }
        PushMono();
        const std::string offsetText = Hex(static_cast<std::uint64_t>(field.Offset), 4) +
            (field.Type == Scan::ValueType::Binary && field.CustomType.empty() ? ":" + std::to_string(field.BitStart) : std::string());
        const bool selected = m_selected.count(fieldPath) > 0;
        const bool clicked = ImGui::Selectable(offsetText.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap);
        const ImGuiIO& io = ImGui::GetIO();
        if (clicked)
        {
            bool ranged = false;
            if (io.KeyShift && !m_selectAnchor.empty())
            {
                const auto rowPath = [&](const Row& r) { return FieldKey(m_blocks[static_cast<std::size_t>(r.Block)].Path, r.Definition->Fields[static_cast<std::size_t>(r.Field)].Offset, r.Twin); };
                int from = -1;
                int to = -1;
                for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
                {
                    const std::string candidate = rowPath(m_rows[static_cast<std::size_t>(i)]);
                    if (candidate == m_selectAnchor)
                        from = i;
                    if (candidate == fieldPath)
                        to = i;
                }
                if (from >= 0 && to >= 0)
                {
                    if (!io.KeyCtrl)
                        m_selected.clear();
                    for (int i = std::min(from, to); i <= std::max(from, to); ++i)
                        m_selected.insert(rowPath(m_rows[static_cast<std::size_t>(i)]));
                    ranged = true;
                }
            }
            if (!ranged)
            {
                if (io.KeyCtrl)
                {
                    if (selected)
                        m_selected.erase(fieldPath);
                    else
                        m_selected.insert(fieldPath);
                }
                else
                {
                    m_selected.clear();
                    m_selected.insert(fieldPath);
                }
                m_selectAnchor = fieldPath;
            }
        }
        else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
        {
            m_selected.clear();
            m_selected.insert(fieldPath);
            m_selectAnchor = fieldPath;
        }
        if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            const int column = ImGui::TableGetHoveredColumn();
            if (column == 2)
            {
                ImGui::OpenPopup("##fieldmenu");
            }
            else
            {
                m_editPath = fieldPath;
                m_editName = column == 1;
                m_editText = m_editName ? field.Name : "";
                if (!m_editName && !addresses.empty() && addresses.front() && m_engine.HasProcess())
                {
                    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max(1, field.Size)));
                    if (m_engine.Process()->ReadExact(addresses.front(), bytes.data(), bytes.size()))
                        m_editText = FieldText(field, bytes, addresses.front(), pointerSize);
                }
            }
        }
        PopMono();
        if (ImGui::BeginPopupContextItem("##fieldmenu"))
        {
            FieldMenu(definition, field, addresses, fieldPath);
            ImGui::EndPopup();
        }
        if (indent > 0.0f)
            ImGui::Unindent(indent);

        ImGui::TableNextColumn();
        if (m_editPath == fieldPath && m_editName)
        {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SetKeyboardFocusHere();
            if (InputText("##name", m_editText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                field.Name = m_editText;
                m_editPath.clear();
                m_engine.Table().Touch();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                m_editPath.clear();
        }
        else
        {
            ImGui::TextUnformatted(field.Name.c_str());
        }
        ImGui::TableNextColumn();
        Muted(TypeLabel(field));

        std::vector<std::uint8_t> bytes;
        for (std::size_t column = 0; column < addresses.size(); ++column)
        {
            ImGui::TableNextColumn();
            const std::uint64_t address = addresses[column];
            if (!address || !m_engine.HasProcess())
            {
                Muted("??");
                continue;
            }
            if (column == 0 && m_editPath == fieldPath && !m_editName)
            {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SetKeyboardFocusHere();
                const bool entered = InputText("##value", m_editText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (entered && ShowsBits(field))
                {
                    m_workspace.Report(WriteBits(field, address, m_editText));
                    m_editPath.clear();
                }
                else if (entered)
                {
                    Scan::ValueFormat format;
                    format.Hex = field.Display == Table::FieldDisplay::Hex;
                    format.Unicode = field.Unicode;
                    format.PointerSize = pointerSize;
                    std::vector<std::uint8_t> converted;
                    const Lua::CustomType* custom = field.CustomType.empty() ? nullptr : m_engine.Lua().FindCustomType(field.CustomType);
                    if (custom && m_engine.Lua().CustomFromText(*custom, m_editText, address, converted))
                        m_workspace.Report(m_engine.Process()->Write(address, converted));
                    else if (const auto parsed = field.CustomType.empty() ? Scan::ParseValue(field.Type, m_editText, format) : std::nullopt)
                        m_workspace.Report(m_engine.Process()->Write(address, *parsed));
                    else
                        m_workspace.Report(Status::Fail(ErrorCode::ScanBadValue, m_editText));
                    m_editPath.clear();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                    m_editPath.clear();
                continue;
            }
            bytes.assign(static_cast<std::size_t>(std::max(1, field.Size)), 0);
            if (!ReadField(row.Block, column, address, bytes.data(), bytes.size()))
            {
                Muted("??");
                continue;
            }
            std::string text = FieldText(field, bytes, address, pointerSize);
            if (pointer)
            {
                std::uint64_t value = 0;
                std::memcpy(&value, bytes.data(), std::min<std::size_t>(8, bytes.size()));
                const std::string described = m_engine.Describe(value);
                text = "P->" + Hex(value) + (described != Hex(value) ? "  " + described : "");
            }
            PushMono();
            ImGui::PushStyleColor(ImGuiCol_Text, pointer ? ManifoldTheme::AddressText : ManifoldTheme::Text);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopStyleColor();
            PopMono();
        }
        ImGui::PopID();
    }

    void StructureWindow::FieldMenu(Table::StructDefinition& definition, Table::StructField& field, const std::vector<std::uint64_t>& addresses, const std::string& path)
    {
        const int pointerSize = m_engine.HasProcess() ? m_engine.Process()->PointerSize() : 8;
        const std::uint64_t first = addresses.empty() ? 0 : addresses.front();
        const bool multi = m_selected.size() > 1 && m_selected.count(path) > 0;
        const auto item = [&](const char* label, Command command, bool enabled = true)
            {
                if (CommandItem(label, command, false, enabled))
                    RunFieldCommand(command, definition, field, first, path, pointerSize);
            };
        if (multi)
            ImGui::TextDisabled("%zu fields selected", m_selected.size());
        else
            ImGui::TextDisabled("%s", field.Name.c_str());
        ImGui::Separator();
        item(ICON_FA_EDIT "  Change the Value", Command::ChangeFieldValue, first != 0 && m_engine.HasProcess());
        item(ICON_FA_I_CURSOR "  Rename", Command::RenameField);
        ImGui::Separator();
        if (ImGui::BeginMenu(ICON_FA_EXCHANGE_ALT "  Type"))
        {
            std::vector<std::uint8_t> sample(64);
            const bool sampled = first != 0 && m_engine.HasProcess() && m_engine.Process()->Read(first, sample.data(), sample.size()) >= 8;
            const auto preview = [&](const Table::StructField& probe) -> std::string
                {
                    if (!sampled)
                        return {};
                    const std::size_t size = static_cast<std::size_t>(std::clamp(probe.Size, 1, static_cast<int>(sample.size())));
                    std::string text;
                    if (probe.Type == Scan::ValueType::Pointer)
                    {
                        std::uint64_t value = 0;
                        std::memcpy(&value, sample.data(), static_cast<std::size_t>(pointerSize));
                        const std::string described = m_engine.Describe(value);
                        text = "-> " + (described != Hex(value) ? described : Hex(value));
                        if (!(value >= 0x10000 && m_engine.Process()->RegionAt(value).value_or(Target::RegionInfo{}).Committed()))
                            text = Hex(value) + " (not mapped)";
                    }
                    else
                    {
                        text = FieldText(probe, std::span<const std::uint8_t>(sample.data(), size), first, pointerSize);
                    }
                    if (text.size() > 28)
                        text = text.substr(0, 27) + "\xE2\x80\xA6";
                    return text;
                };
            for (Scan::ValueType type : kFieldTypes)
            {
                Table::StructField probe = field;
                probe.CustomType.clear();
                probe.ChildStructure.clear();
                probe.Type = type;
                probe.Size = SizeOf(type, pointerSize, type == Scan::ValueType::String || type == Scan::ValueType::ByteArray ? std::max(field.Size, 16) : field.Size);
                if (type == Scan::ValueType::Pointer)
                    probe.Display = Table::FieldDisplay::Hex;
                else if ((probe.Display == Table::FieldDisplay::Binary && type != Scan::ValueType::Binary) || field.Type == Scan::ValueType::Pointer)
                    probe.Display = Table::FieldDisplay::Signed;
                const std::string shown = type == Scan::ValueType::Binary ? std::string() : preview(probe);
                if (ImGui::MenuItem(Scan::TypeName(type), shown.empty() ? nullptr : shown.c_str(), field.CustomType.empty() && !Embedded(field) && field.Type == type))
                {
                    if (Embedded(field))
                        field.ChildStructure.clear();
                    field.CustomType.clear();
                    field.Type = type;
                    field.Size = SizeOf(type, pointerSize, field.Size);
                    if (type == Scan::ValueType::Pointer)
                        field.Display = Table::FieldDisplay::Hex;
                    if (type == Scan::ValueType::Binary)
                    {
                        field.BitStart = std::clamp(field.BitStart, 0, 63);
                        field.BitLength = std::clamp(field.BitLength, 1, 64 - field.BitStart);
                        field.Size = Table::BitFieldSize(field.BitStart, field.BitLength);
                        field.Display = Table::FieldDisplay::Binary;
                    }
                    else if (field.Display == Table::FieldDisplay::Binary && !IsWholeNumber(type))
                    {
                        field.Display = Table::FieldDisplay::Unsigned;
                    }
                    m_engine.Table().Touch();
                }
            }
            const std::vector<std::string> customTypes = m_engine.Lua().CustomTypeNames();
            if (!customTypes.empty())
            {
                ImGui::Separator();
                for (const std::string& name : customTypes)
                {
                    if (ImGui::MenuItem(name.c_str(), nullptr, EqualsNoCase(field.CustomType, name)))
                    {
                        if (const Lua::CustomType* custom = m_engine.Lua().FindCustomType(name))
                        {
                            field.CustomType = custom->Name;
                            field.Type = Scan::ValueType::ByteArray;
                            field.Size = custom->ByteCount();
                            m_engine.Table().Touch();
                        }
                    }
                }
            }
            if (field.CustomType.empty() && (field.Type == Scan::ValueType::String || field.Type == Scan::ValueType::ByteArray))
            {
                ImGui::Separator();
                ImGui::SetNextItemWidth(Px(80.0f));
                if (ImGui::InputInt("Length", &field.Size))
                    field.Size = std::clamp(field.Size, 1, Table::kMaxFieldBytes);
                if (field.Type == Scan::ValueType::String)
                    ImGui::MenuItem("UTF-16", nullptr, &field.Unicode);
            }
            if (field.CustomType.empty() && field.Type == Scan::ValueType::Binary)
            {
                ImGui::Separator();
                int start = field.BitStart;
                int length = field.BitLength;
                ImGui::SetNextItemWidth(Px(80.0f));
                const bool startChanged = ImGui::InputInt("Start bit", &start);
                ImGui::SetNextItemWidth(Px(80.0f));
                const bool lengthChanged = ImGui::InputInt("Bit count", &length);
                if (startChanged || lengthChanged)
                {
                    field.BitStart = std::clamp(start, 0, 63);
                    field.BitLength = std::clamp(length, 1, 64 - field.BitStart);
                    field.Size = Table::BitFieldSize(field.BitStart, field.BitLength);
                    m_engine.Table().Touch();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_HASHTAG "  Show As"))
        {
            const auto show = [&](const char* label, Table::FieldDisplay display)
                {
                    if (ImGui::MenuItem(label, nullptr, field.Display == display) && field.Display != display)
                    {
                        field.Display = display;
                        m_engine.Table().Touch();
                    }
                };
            show("Unsigned", Table::FieldDisplay::Unsigned);
            show("Signed", Table::FieldDisplay::Signed);
            show("Hexadecimal", Table::FieldDisplay::Hex);
            if (field.CustomType.empty() && IsWholeNumber(field.Type))
                show("Binary", Table::FieldDisplay::Binary);
            ImGui::EndMenu();
        }
        if (field.Type == Scan::ValueType::Pointer && ImGui::BeginMenu(ICON_FA_LINK "  Points to Structure"))
        {
            for (const auto& item : m_engine.Structures().Items)
            {
                if (ImGui::MenuItem(item->Name.c_str(), nullptr, item->Name == field.ChildStructure))
                    field.ChildStructure = item->Name;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::BeginDisabled(first == 0);
        const std::string addLabel = multi ? ICON_FA_PLUS "  Add " + std::to_string(m_selected.size()) + " fields to the address list" : std::string(ICON_FA_PLUS "  Add to the Address List");
        item(addLabel.c_str(), Command::FieldToAddressList);
        item(ICON_FA_MEMORY "  Browse This Memory", Command::BrowseField);
        if (field.Type == Scan::ValueType::Pointer)
            item(ICON_FA_ARROW_RIGHT "  Browse the Memory It Points To", Command::BrowseFieldPointer);
        item(ICON_FA_EYE "  Find Out What Accesses This Field", Command::FieldAccesses);
        item(ICON_FA_PEN "  Find Out What Writes to This Field", Command::FieldWrites);
        ImGui::EndDisabled();
        ImGui::Separator();
        item(ICON_FA_PLUS "  Insert 4 Bytes After", Command::InsertField);
        const std::string deleteLabel = multi ? ICON_FA_TRASH "  Delete " + std::to_string(m_selected.size()) + " fields" : std::string(ICON_FA_TRASH "  Delete Field");
        if (DangerCommandItem(deleteLabel.c_str(), Command::DeleteField))
            RunFieldCommand(Command::DeleteField, definition, field, first, path, pointerSize);
    }
}
