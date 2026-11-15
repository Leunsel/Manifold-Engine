#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string_view>

namespace Manifold::UI
{
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
            Scan::ValueType::Float, Scan::ValueType::Double, Scan::ValueType::Pointer, Scan::ValueType::String, Scan::ValueType::ByteArray };

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
            format.PointerSize = pointerSize;
            return format;
        }

        std::uint64_t FieldAddress(std::uint64_t base, int offset)
        {
            return base ? base + static_cast<std::uint64_t>(static_cast<std::int64_t>(offset)) : 0;
        }
    }

    StructureWindow::StructureWindow(Workspace& workspace, int number) : m_workspace(workspace), m_engine(workspace.Core()), m_number(number)
    {
        m_title = NextWindowTitle(ICON_FA_SITEMAP "  Structure Dissect", number);
        m_columns.push_back({});
    }

    void StructureWindow::Show(std::uint64_t address, const std::string& structure)
    {
        m_open = true;
        ImGui::SetWindowFocus(m_title.c_str());
        if (!structure.empty() && m_engine.Structures().Find(structure))
        {
            m_structure = structure;
            m_expanded.clear();
        }
        if (address)
        {
            m_columns.front().Address = m_engine.Symbols().ModuleOffset(address);
            if (!Current())
            {
                m_newOpen = true;
                PrepareNew(address);
            }
        }
    }

    void StructureWindow::PrepareNew(std::uint64_t address)
    {
        m_newFrom = address;
        m_newStart = address;
        m_newName = address ? "Structure at " + Hex(address) : "Structure";
        if (!address || !m_engine.HasProcess())
            return;
        if (const auto lookup = m_engine.Lua().LookupStructureName(address))
        {
            m_newName = lookup->Name;
            m_newStart = lookup->Address ? lookup->Address : address;
        }
    }

    Table::StructDefinition& StructureWindow::ChildFor(const Table::StructDefinition& parent, const Table::StructField& field, std::uint64_t target)
    {
        if (target && m_engine.HasProcess())
        {
            if (const auto lookup = m_engine.Lua().LookupStructureName(target); lookup && lookup->Address == target)
            {
                if (Table::StructDefinition* existing = m_engine.Structures().Find(lookup->Name); existing && existing != &parent)
                    return *existing;
                return Guess(lookup->Name, target, 256);
            }
        }
        return Guess(parent.Name + "." + field.Name, target, 256);
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
        return Scan::FormatValue(field.Type, bytes, FieldFormat(field, pointerSize));
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
        if (address && m_engine.Lua().DissectStructure(definition, address))
        {
            ++store.Generation;
            return definition;
        }
        definition.Fields.clear();
        Table::GuessFields(*m_engine.Process(), definition, address, 0, size);
        ++store.Generation;
        return definition;
    }

    void StructureWindow::Draw()
    {
        if (!m_open)
            return;
        if (!BeginToolWindow(m_title.c_str(), &m_open, ImVec2(820.0f, 640.0f), 0, ImVec2(680.0f, 320.0f)))
        {
            EndToolWindow();
            return;
        }
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
        Toolbar();
        Table::StructDefinition* definition = Current();
        std::vector<std::uint64_t> bases;
        for (Column& column : m_columns)
        {
            column.Valid = false;
            if (column.Address.empty())
                continue;
            if (const auto value = m_engine.Evaluate(column.Address))
            {
                column.Value = *value;
                column.Valid = true;
            }
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
                ImGui::EndTable();
            }
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
            ImGui::SetNextItemWidth(Px(120.0f));
            ImGui::InputInt("Size (bytes)", &m_guessSize, 64, 1024);
            m_guessSize = std::clamp(m_guessSize, 8, 0x100000);
            if (AccentButton("Create", Px(100.0f, 0.0f)))
            {
                if (moved)
                {
                    m_columns.front().Address = m_engine.Symbols().ModuleOffset(m_newStart);
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
                    m_workspace.Report(Status::Fail(ErrorCode::ExprSyntax, m_columns.empty() ? "" : m_columns.front().Address));
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

    void StructureWindow::Toolbar()
    {
        for (std::size_t i = 0; i < m_columns.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(Px(200.0f));
            InputTextWithHint("##address", "address of the structure", m_columns[i].Address);
            if (!m_columns[i].Address.empty())
            {
                Status error;
                if (!m_engine.Evaluate(m_columns[i].Address, &error))
                {
                    ImGui::SameLine(0.0f, Px(2.0f));
                    ImGui::TextColored(ManifoldTheme::Error, ICON_FA_EXCLAMATION_TRIANGLE);
                    ErrorTooltip(error.Text);
                }
            }
            if (m_columns.size() > 1)
            {
                ImGui::SameLine(0.0f, Px(2.0f));
                if (IconButton(ICON_FA_TIMES, "Remove this column"))
                {
                    m_columns.erase(m_columns.begin() + static_cast<std::ptrdiff_t>(i));
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::SameLine();
            ImGui::PopID();
        }
        if (IconButton(ICON_FA_PLUS, "Add a column to compare another address"))
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
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_MAGIC "  Guess new"))
        {
            m_newOpen = true;
            PrepareNew(m_columns.front().Valid ? m_columns.front().Value : 0);
        }
        ItemTooltip("Read the memory at the first address and guess pointers, floats, strings and integers");
        Table::StructDefinition* current = Current();
        ImGui::BeginDisabled(!current);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_I_CURSOR "  Rename") && current)
        {
            m_renameOpen = true;
            m_renameText = current->Name;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(80.0f));
        InputTextWithHint("##addoffset", "offset", m_addOffset, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS "  Field") && current)
        {
            Table::StructField field;
            field.Offset = static_cast<int>(ParseHex(m_addOffset).value_or(static_cast<std::uint64_t>(current->Size())));
            field.Name = "Field " + Hex(static_cast<std::uint64_t>(field.Offset), 4);
            current->Fields.push_back(field);
            current->Sort();
            m_engine.Table().Touch();
        }
        ImGui::SameLine();
        if (DangerButton(ICON_FA_TRASH "  Delete") && current)
        {
            const std::string name = current->Name;
            m_workspace.Confirm("Delete structure", "Delete the structure \"" + name + "\"?", [this, name]()
                {
                    if (Table::StructDefinition* target = m_engine.Structures().Find(name))
                        m_engine.Structures().Remove(*target);
                    if (m_structure == name)
                        m_structure.clear();
                }, "Delete");
        }
        ImGui::EndDisabled();
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
            if (!anyExpanded || field.Type != Scan::ValueType::Pointer)
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
        const bool expanded = pointer && row.Expanded;
        if (pointer)
        {
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
                    if (field.ChildStructure.empty() || !m_engine.Structures().Find(field.ChildStructure))
                    {
                        std::uint64_t target = 0;
                        if (!addresses.empty() && addresses.front() && m_engine.HasProcess())
                            target = m_engine.Process()->ReadPointer(addresses.front()).value_or(0);
                        Table::StructDefinition& child = ChildFor(definition, field, target);
                        field.ChildStructure = child.Name;
                    }
                }
            }
            ImGui::SameLine(0.0f, Px(4.0f));
        }
        PushMono();
        const std::string offsetText = Hex(static_cast<std::uint64_t>(field.Offset), 4);
        if (ImGui::Selectable(offsetText.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            m_editPath = fieldPath;
            m_editName = ImGui::TableGetHoveredColumn() == 1;
            m_editText = m_editName ? field.Name : "";
            if (!m_editName && !addresses.empty() && addresses.front() && m_engine.HasProcess())
            {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max(1, field.Size)));
                if (m_engine.Process()->ReadExact(addresses.front(), bytes.data(), bytes.size()))
                    m_editText = FieldText(field, bytes, addresses.front(), pointerSize);
            }
        }
        PopMono();
        if (ImGui::BeginPopupContextItem("##fieldmenu"))
        {
            FieldMenu(definition, field, addresses);
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
        Muted(!field.CustomType.empty() ? field.CustomType :
              field.Type == Scan::ValueType::String ? "String[" + std::to_string(field.Size) + "]" :
              field.Type == Scan::ValueType::ByteArray ? "AOB[" + std::to_string(field.Size) + "]" : Scan::TypeName(field.Type));

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
                if (InputText("##value", m_editText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
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

    void StructureWindow::FieldMenu(Table::StructDefinition& definition, Table::StructField& field, const std::vector<std::uint64_t>& addresses)
    {
        const int pointerSize = m_engine.HasProcess() ? m_engine.Process()->PointerSize() : 8;
        ImGui::TextDisabled("%s", field.Name.c_str());
        ImGui::Separator();
        if (ImGui::BeginMenu(ICON_FA_EXCHANGE_ALT "  Type"))
        {
            for (Scan::ValueType type : kFieldTypes)
            {
                if (ImGui::MenuItem(Scan::TypeName(type), nullptr, field.CustomType.empty() && field.Type == type))
                {
                    field.CustomType.clear();
                    field.Type = type;
                    field.Size = SizeOf(type, pointerSize, field.Size);
                    if (type == Scan::ValueType::Pointer)
                        field.Display = Table::FieldDisplay::Hex;
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
                    field.Size = std::clamp(field.Size, 1, 4096);
                if (field.Type == Scan::ValueType::String)
                    ImGui::MenuItem("UTF-16", nullptr, &field.Unicode);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_HASHTAG "  Show as"))
        {
            if (ImGui::MenuItem("Unsigned", nullptr, field.Display == Table::FieldDisplay::Unsigned))
                field.Display = Table::FieldDisplay::Unsigned;
            if (ImGui::MenuItem("Signed", nullptr, field.Display == Table::FieldDisplay::Signed))
                field.Display = Table::FieldDisplay::Signed;
            if (ImGui::MenuItem("Hexadecimal", nullptr, field.Display == Table::FieldDisplay::Hex))
                field.Display = Table::FieldDisplay::Hex;
            ImGui::EndMenu();
        }
        if (field.Type == Scan::ValueType::Pointer && ImGui::BeginMenu(ICON_FA_LINK "  Points to structure"))
        {
            for (const auto& item : m_engine.Structures().Items)
            {
                if (ImGui::MenuItem(item->Name.c_str(), nullptr, item->Name == field.ChildStructure))
                    field.ChildStructure = item->Name;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        const std::uint64_t address = addresses.empty() ? 0 : addresses.front();
        ImGui::BeginDisabled(address == 0);
        if (ImGui::MenuItem(ICON_FA_PLUS "  Add to the address list"))
        {
            Table::MemoryRecord& record = m_engine.Table().AddValue(field.Name, m_engine.Symbols().ModuleOffset(address), field.Type);
            record.Length = field.Size;
            record.Unicode = field.Unicode;
            record.ShowHex = field.Display == Table::FieldDisplay::Hex;
            record.CustomTypeName = field.CustomType;
            m_engine.RefreshValues(true);
        }
        if (ImGui::MenuItem(ICON_FA_MEMORY "  Browse this memory"))
            m_workspace.OpenMemoryViewer(address, true);
        if (ImGui::MenuItem(ICON_FA_EYE "  Find out what accesses this field"))
            m_workspace.StartWatch(Debug::WatchKind::Accesses, address, std::min(8, std::max(1, field.Size)));
        if (ImGui::MenuItem(ICON_FA_PEN "  Find out what writes to this field"))
            m_workspace.StartWatch(Debug::WatchKind::Writes, address, std::min(8, std::max(1, field.Size)));
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_PLUS "  Insert 4 bytes after"))
        {
            m_pendingStructure = definition.Name;
            m_pendingOffset = field.Offset + field.Size;
            m_pendingInsert = true;
        }
        if (DangerMenuItem(ICON_FA_TRASH "  Delete field"))
        {
            m_pendingStructure = definition.Name;
            m_pendingOffset = field.Offset;
            m_pendingInsert = false;
        }
    }
}
