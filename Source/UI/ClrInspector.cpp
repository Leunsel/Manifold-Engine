#include "UI/ClrInspector.h"

#include "App/Engine.h"
#include "Core/BackgroundWork.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Table/ManagedStructures.h"
#include "Target/Process.h"
#include "UI/AddressActions.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui.h"

#include <algorithm>
#include <cstring>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::uint32_t kInstanceLimit = 20000;
        constexpr int kMaxDepth = 4;
        constexpr const char* kWorkGroup = "mono";

        bool IsReference(int typeCode)
        {
            // CLASS, STRING, OBJECT, SZARRAY, ARRAY, GENERICINST of a class
            return typeCode == 0x12 || typeCode == 0x0E || typeCode == 0x1C || typeCode == 0x1D || typeCode == 0x14 || typeCode == 0x15;
        }

        std::string ShortName(const std::string& full)
        {
            const std::size_t bracket = full.find('[');
            const std::size_t dot = full.rfind('.', bracket == std::string::npos ? std::string::npos : bracket);
            return dot == std::string::npos ? full : full.substr(dot + 1);
        }

        std::string Quoted(const std::string& text)
        {
            return "\"" + text + "\"";
        }
    }

    std::string ManagedValueText(const Target::Process& process, std::uint64_t address, int typeCode, int enumCode)
    {
        const int code = enumCode ? enumCode : typeCode;
        if (!address || IsReference(code) || code == 0x11)
            return {};
        int size = 0;
        Table::FieldDisplay display = Table::FieldDisplay::Unsigned;
        const Scan::ValueType type = Table::ManagedFieldType(code, process.PointerSize(), size, display);
        std::uint8_t bytes[8] = {};
        if (size <= 0 || size > 8 || !process.ReadExact(address, bytes, static_cast<std::size_t>(size)))
            return {};
        std::uint64_t raw = 0;
        std::memcpy(&raw, bytes, sizeof(raw));
        if (type == Scan::ValueType::Float)
        {
            float number = 0.0f;
            std::memcpy(&number, bytes, sizeof(number));
            return Format("%g", static_cast<double>(number));
        }
        if (type == Scan::ValueType::Double)
        {
            double number = 0.0;
            std::memcpy(&number, bytes, sizeof(number));
            return Format("%g", number);
        }
        if (size < 8)
            raw &= (1ull << (size * 8)) - 1;
        if (type == Scan::ValueType::Pointer)
            return Hex(raw);
        if (code == 0x02)
            return raw ? "true" : "false";
        if (code == 0x03)
        {
            const wchar_t c = static_cast<wchar_t>(raw);
            return c >= 0x20 && c < 0x7F ? Format("'%c' (%u)", static_cast<char>(c), static_cast<unsigned>(raw)) : std::to_string(raw);
        }
        if (display == Table::FieldDisplay::Signed)
        {
            const int shift = 64 - size * 8;
            return std::to_string(static_cast<std::int64_t>(raw << shift) >> shift);
        }
        return std::to_string(raw);
    }

    std::optional<std::string> ClrStringText(const Target::Process& process, std::uint64_t object, std::size_t maxChars)
    {
        const std::uint64_t pointer = static_cast<std::uint64_t>(process.PointerSize());
        std::int32_t length = 0;
        if (!object || !process.ReadExact(object + pointer, &length, sizeof(length)) || length < 0 || length > 0x10000000)
            return std::nullopt;
        const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(length), maxChars);
        std::wstring text(count, L'\0');
        if (count && !process.ReadExact(object + pointer + 4, text.data(), count * sizeof(wchar_t)))
            return std::nullopt;
        std::string utf8 = WideToUtf8(text);
        if (count < static_cast<std::size_t>(length))
            utf8 += "...";
        return utf8;
    }

    ClrInspector::ClrInspector(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core()) {}

    void ClrInspector::Reset()
    {
        m_heap.reset();
        m_heapError.clear();
        m_selectedType = 0;
        m_instancesOf.clear();
        m_instancesHandle = 0;
        m_instances.reset();
        m_instancesError.clear();
        m_history.clear();
        m_position = -1;
        m_current.reset();
        m_currentAddress = 0;
    }

    void ClrInspector::ReadHeap()
    {
        const std::uint64_t generation = m_engine.Mono().Generation();
        m_heapError.clear();
        m_engine.Work().Start({ .Title = "Reading the .NET heap", .Group = kWorkGroup, .Cancellable = false }, [this, generation](WorkProgress&) -> BackgroundWork::Finish
            {
                Result<Target::ManagedHeap> heap = m_engine.Mono().ReadHeap();
                return [this, generation, heap = std::move(heap)]()
                    {
                        if (generation != m_engine.Mono().Generation())
                            return;
                        if (heap)
                            m_heap = *heap;
                        else
                            m_heapError = heap.Error().Text;
                    };
            });
    }

    void ClrInspector::Sync()
    {
        // A reload or another process: every address here is stale.
        if (m_engine.Mono().Generation() != m_generation)
        {
            m_generation = m_engine.Mono().Generation();
            Reset();
        }
    }

    void ClrInspector::FindInstances(std::uint64_t klass, const std::string& name)
    {
        Sync();
        const std::uint64_t generation = m_engine.Mono().Generation();
        m_instancesOf = name;
        m_instancesHandle = klass;
        m_instances.reset();
        m_instancesError.clear();
        m_engine.Work().Start({ .Title = "Finding the objects of " + ShortName(name), .Group = kWorkGroup, .Cancellable = false },
            [this, generation, klass](WorkProgress&) -> BackgroundWork::Finish
            {
                Result<Target::ManagedInstances> found = m_engine.Mono().Instances(klass, kInstanceLimit);
                return [this, generation, klass, found = std::move(found)]()
                    {
                        if (generation != m_engine.Mono().Generation() || klass != m_instancesHandle)
                            return;
                        if (found)
                            m_instances = *found;
                        else
                            m_instancesError = found.Error().Text;
                    };
            });
    }

    void ClrInspector::OpenFromStatic(const std::string& symbol, std::uint64_t slot)
    {
        Sync();
        const auto object = m_engine.HasProcess() ? m_engine.Process()->ReadPointer(slot) : std::nullopt;
        if (!object || !*object)
            return;
        Path route;
        route.Base = symbol;
        route.Stable = true;
        const std::size_t colon = symbol.rfind(':');
        route.Label = ShortName(colon == std::string::npos ? symbol : symbol.substr(0, colon)) + (colon == std::string::npos ? "" : "." + symbol.substr(colon + 1));
        Open(*object, std::move(route));
    }

    void ClrInspector::Open(std::uint64_t object, Path route)
    {
        if (m_position + 1 < static_cast<int>(m_history.size()))
            m_history.resize(static_cast<std::size_t>(m_position + 1));
        m_history.push_back({ object, std::move(route) });
        if (m_history.size() > 200)
            m_history.erase(m_history.begin());
        m_position = static_cast<int>(m_history.size()) - 1;
        m_current.reset();
        m_currentAddress = 0;
    }

    void ClrInspector::Navigate(int position)
    {
        if (position < 0 || position >= static_cast<int>(m_history.size()))
            return;
        m_position = position;
        m_current.reset();
        m_currentAddress = 0;
    }

    ClrInspector::Path ClrInspector::Through(const Page& page, std::uint64_t objectOffset, const std::string& name) const
    {
        // The reference at object + offset becomes the new slot.
        Path next;
        next.Label = page.Route.Label.empty() ? name : page.Route.Label + (name.rfind('[', 0) == 0 ? "" : ".") + name;
        if (page.Route.BaseIsObject)
        {
            next.Base = Hex(page.Address + objectOffset);
            next.Stable = false;
            return next;
        }
        next.Base = page.Route.Base;
        next.Offsets = page.Route.Offsets;
        next.Offsets.push_back(objectOffset);
        next.Stable = page.Route.Stable;
        return next;
    }

    void ClrInspector::AddToList(const Page& page, const std::string& name, std::uint64_t objectOffset, int typeCode, int enumCode)
    {
        int size = 0;
        Table::FieldDisplay display = Table::FieldDisplay::Unsigned;
        const int code = enumCode ? enumCode : typeCode;
        const int pointerSize = m_engine.HasProcess() ? m_engine.Process()->PointerSize() : 8;
        Scan::ValueType type = IsReference(code) ? Scan::ValueType::Pointer : Table::ManagedFieldType(code, pointerSize, size, display);
        const std::string description = page.Route.Label.empty() ? name : page.Route.Label + "." + name;
        std::string address;
        std::vector<std::string> offsets;
        if (page.Route.BaseIsObject)
            address = Hex(page.Address + objectOffset);
        else
        {
            address = page.Route.Base;
            for (const std::uint64_t offset : page.Route.Offsets)
                offsets.push_back(Hex(offset));
            offsets.push_back(Hex(objectOffset));
        }
        Table::MemoryRecord& record = m_engine.Table().AddValue(page.Route.Stable ? description : description + " (moves with the GC)", address, type);
        record.Offsets = std::move(offsets);
        if (display == Table::FieldDisplay::Signed)
            record.ShowSigned = true;
        m_engine.RefreshValues(true);
    }

    void ClrInspector::AddressMenu(const Page& page, const std::string& name, std::uint64_t address, std::uint64_t objectOffset, int typeCode, int enumCode)
    {
        if (ImGui::MenuItem(ICON_FA_PLUS "  Add to the Address List"))
            AddToList(page, name, objectOffset, typeCode, enumCode);
        ItemTooltip(page.Route.Stable ? "Adds a pointer from the static field this object was reached through, so it still finds the value after the GC moves objects."
                                      : "Adds this address as it is now. The object was picked from the heap, so the GC may move it; reach it through a static field "
                                        "for an entry that keeps working.");
        ImGui::Separator();
        AddressMenuItems(m_workspace, address, ShowHex | ShowCode | DissectHere | WatchAccesses | WatchWrites | CopyAddress);
    }

    void ClrInspector::ObjectsTab()
    {
        Sync();
        const bool busy = m_engine.Work().Busy(kWorkGroup);
        if (ImGui::Button(m_heap ? ICON_FA_SYNC "  Read the Heap Again" : ICON_FA_DATABASE "  Read the Heap") && !busy)
            ReadHeap();
        ItemTooltip("Walks the game's GC heap from outside and counts the objects of every type. The game keeps running; a heap of a few hundred MB takes a few seconds.");
        if (m_heap)
        {
            ImGui::SameLine();
            Muted(Format("%llu objects, %.1f MB, %.2f s", static_cast<unsigned long long>(m_heap->Objects), static_cast<double>(m_heap->Bytes) / (1024.0 * 1024.0), m_heap->Seconds));
        }
        if (!m_heapError.empty())
            TextWithError(m_heapError, ManifoldTheme::Error);
        if (!m_heap)
        {
            ImGui::Spacing();
            Muted("Or right-click a class in Classes and pick Find Objects of This Class, or open an object from a static field.");
            return;
        }
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##heapfilter", "filter types", m_filter);
        std::vector<const Target::ManagedHeapType*> shown;
        for (const Target::ManagedHeapType& type : m_heap->Types)
        {
            if (m_filter.empty() || ContainsNoCase(type.Name, m_filter))
                shown.push_back(&type);
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##heaptypes", 3, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const Target::ManagedHeapType& type = *shown[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::PushID(row);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(type.Name.c_str(), m_selectedType == type.Handle, ImGuiSelectableFlags_SpanAllColumns) && !busy)
                {
                    m_selectedType = type.Handle;
                    FindInstances(type.Handle, type.Name);
                }
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem(ICON_FA_SEARCH "  Find the Objects", nullptr, false, !busy))
                    {
                        m_selectedType = type.Handle;
                        FindInstances(type.Handle, type.Name);
                    }
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Name"))
                        ImGui::SetClipboardText(type.Name.c_str());
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                Muted(std::to_string(type.Count));
                ImGui::TableNextColumn();
                Muted(type.Bytes >= 1024 * 1024 ? Format("%.1f MB", static_cast<double>(type.Bytes) / (1024.0 * 1024.0))
                                                : Format("%.1f KB", static_cast<double>(type.Bytes) / 1024.0));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void ClrInspector::Pane()
    {
        if (!m_engine.HasProcess())
            return;
        Sync();
        const Target::Process& process = *m_engine.Process();
        // The objects of the type picked last.
        if (!m_instancesOf.empty())
        {
            const std::string title = m_instances ? Format("%s  ·  %llu objects", ShortName(m_instancesOf).c_str(), static_cast<unsigned long long>(m_instances->Total))
                                                  : ShortName(m_instancesOf);
            if (ImGui::CollapsingHeader((ICON_FA_LIST "  " + title + "###instances").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (!m_instancesError.empty())
                    TextWithError(m_instancesError, ManifoldTheme::Error);
                else if (!m_instances)
                    Muted("Walking the heap...");
                else if (m_instances->Addresses.empty())
                    Muted("No objects of this type on the heap.");
                else
                {
                    if (m_instances->Total > m_instances->Addresses.size())
                        Muted(Format("The first %zu are listed.", m_instances->Addresses.size()));
                    // ∑ The list had a fixed height of at most 160 px: with no object open the rest of the pane stayed
                    //   empty, with one open the border could not be moved (owner, Terraria). Without an object the
                    //   list fills the pane; with one a splitter sits between the list and the object.
                    const bool objectOpen = m_position >= 0 && m_position < static_cast<int>(m_history.size());
                    const float gap = Px(6.0f);
                    const float rows = (static_cast<float>(m_instances->Addresses.size()) + 0.5f) * ImGui::GetTextLineHeightWithSpacing();
                    const float available = ImGui::GetContentRegionAvail().y;
                    const float minList = std::min(rows, 3.5f * ImGui::GetTextLineHeightWithSpacing());
                    const float minObject = Px(160.0f);
                    if (m_listHeight <= 0.0f)
                        m_listHeight = Px(160.0f);
                    m_listHeight = std::clamp(m_listHeight, minList, std::max(minList, available - minObject - gap));
                    const float height = objectOpen ? std::min(m_listHeight, rows) : std::max(minList, available);
                    const ImVec2 start = ImGui::GetCursorPos();
                    ImGui::BeginChild("##instances", ImVec2(0.0f, height));
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(m_instances->Addresses.size()));
                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        {
                            const std::uint64_t object = m_instances->Addresses[static_cast<std::size_t>(i)];
                            ImGui::PushID(i);
                            PushMono();
                            const bool open = !m_history.empty() && m_position >= 0 && m_history[static_cast<std::size_t>(m_position)].Address == object;
                            if (ImGui::Selectable(Hex(object).c_str(), open))
                            {
                                Path route;
                                route.Base = Hex(object);
                                route.BaseIsObject = true;
                                route.Label = ShortName(m_instancesOf);
                                Open(object, std::move(route));
                            }
                            PopMono();
                            if (ImGui::BeginPopupContextItem())
                            {
                                AddressMenuItems(m_workspace, object, ShowHex | DissectHere | CopyAddress);
                                ImGui::EndPopup();
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndChild();
                    if (objectOpen)
                    {
                        float top = height;
                        float bottom = std::max(minObject, available - height - gap);
                        ImGui::SetCursorPos(start);
                        if (Splitter("##clrsplit", false, gap, &top, &bottom, minList, minObject))
                            m_listHeight = top;
                        ImGui::SetCursorPos(ImVec2(start.x, start.y + height + gap));
                    }
                }
            }
        }
        if (m_position < 0 || m_position >= static_cast<int>(m_history.size()))
        {
            if (m_instancesOf.empty())
                EmptyState(ICON_FA_SEARCH, "No object open",
                    "Read the heap and pick a type, find the objects of a class, or click a reference in a static field. References inside an object open the next one.",
                    {}, false);
            return;
        }
        const Page page = m_history[static_cast<std::size_t>(m_position)];
        if (IconButton(ICON_FA_ARROW_LEFT "##clrback", "Back") && m_position > 0)
            Navigate(m_position - 1);
        ImGui::SameLine();
        if (IconButton(ICON_FA_ARROW_RIGHT "##clrforward", "Forward") && m_position + 1 < static_cast<int>(m_history.size()))
            Navigate(m_position + 1);
        ImGui::SameLine();
        if (m_currentAddress != page.Address)
        {
            m_current = m_engine.Mono().ObjectAt(page.Address);
            m_currentAddress = page.Address;
        }
        const std::string name = m_current ? m_current->Name : m_engine.Mono().ClassNameAt(page.Address).value_or("unknown object");
        CardTitle(ICON_FA_CUBE, name.c_str(), Hex(page.Address));
        ItemTooltip("The object's class and address. Right-click a value for the address list, the memory viewer or Structure Dissect.");
        if (!page.Route.Label.empty())
            Muted((page.Route.Stable ? ICON_FA_LINK "  " : ICON_FA_UNLINK "  ") + page.Route.Label);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", page.Route.Stable ? "Reached from a static field: entries added from here follow the pointers again each time."
                                                      : "Picked from the heap: the GC may move this object, so entries added from here can go stale.");
        if (!m_current)
        {
            TextWithError("The runtime does not see an object here any more (it may have been collected).", ManifoldTheme::Error);
            return;
        }
        (void)process;
        Inspect(page);
    }

    void ClrInspector::Inspect(const Page& page)
    {
        // A copy: a click on a reference opens the next object, which drops
        // m_current while the rows of this one are still being drawn.
        const Target::ManagedObject object = *m_current;
        if (object.Kind == Target::ManagedObjectKind::String)
        {
            const auto text = ClrStringText(*m_engine.Process(), page.Address, 65536);
            Muted(Format("%llu characters", static_cast<unsigned long long>(object.Length)));
            std::string shown = text.value_or("");
            ImGui::BeginChild("##clrstring", ImVec2(0.0f, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", shown.c_str());
            ImGui::EndChild();
            return;
        }
        if (object.Kind == Target::ManagedObjectKind::Array)
        {
            ArrayElements(page, object);
            return;
        }
        ObjectFields(page, page.Address);
    }

    void ClrInspector::ObjectFields(const Page& page, std::uint64_t object)
    {
        std::vector<Target::MonoField> fields = m_engine.Mono().InstanceFieldsAt(object);
        std::stable_sort(fields.begin(), fields.end(), [](const Target::MonoField& a, const Target::MonoField& b) { return a.Offset < b.Offset; });
        Muted(Format("%zu fields  ·  %llu bytes", fields.size(), static_cast<unsigned long long>(m_current->Size)));
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##clrfields", 4, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, Px(64.0f));
        ImGui::TableHeadersRow();
        FieldRows(page, object, fields, object, 0, {}, 0);
        ImGui::EndTable();
    }

    void ClrInspector::FieldRows(const Page& page, std::uint64_t object, const std::vector<Target::MonoField>& fields, std::uint64_t base, std::int64_t shift,
        const std::string& prefix, int depth)
    {
        for (const Target::MonoField& field : fields)
        {
            const std::int64_t offset = static_cast<std::int64_t>(field.Offset) + shift;
            const std::uint64_t address = base + static_cast<std::uint64_t>(offset);
            const std::uint64_t fromObject = address - object;
            ValueRow(page, prefix.empty() ? field.Name : prefix + "." + field.Name, field.TypeName, address, fromObject, field.TypeCode, field.EnumCode,
                field.TypeHandle, field.ValueSize, depth);
        }
    }

    void ClrInspector::ValueRow(const Page& page, const std::string& name, const std::string& typeName, std::uint64_t address, std::uint64_t objectOffset, int typeCode,
        int enumCode, std::uint64_t typeHandle, int valueSize, int depth)
    {
        const Target::Process& process = *m_engine.Process();
        ImGui::TableNextRow();
        ImGui::PushID(name.c_str());
        ImGui::TableNextColumn();
        const bool embedded = typeCode == 0x11 && enumCode == 0 && typeHandle && valueSize > 0;
        bool open = false;
        const std::string leaf = name.substr(name.rfind('.') == std::string::npos ? 0 : name.rfind('.') + 1);
        if (embedded && depth < kMaxDepth)
            open = Layout::TreeNode(leaf.c_str(), leaf, ImGuiTreeNodeFlags_SpanAllColumns);
        else
        {
            ImGui::Indent(Px(4.0f) + ImGui::GetTreeNodeToLabelSpacing() * static_cast<float>(embedded ? 1 : 0));
            ImGui::TextUnformatted(leaf.c_str());
            ImGui::Unindent(Px(4.0f) + ImGui::GetTreeNodeToLabelSpacing() * static_cast<float>(embedded ? 1 : 0));
        }
        if (ImGui::BeginPopupContextItem("##valuemenu"))
        {
            AddressMenu(page, name, address, objectOffset, typeCode, enumCode);
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
        Muted(typeName.empty() ? std::string("?") : ShortName(typeName));
        if (ImGui::IsItemHovered() && !typeName.empty())
            ImGui::SetTooltip("%s", typeName.c_str());
        ImGui::TableNextColumn();
        if (IsReference(enumCode ? enumCode : typeCode))
            ReferenceCell(page, name, address, objectOffset);
        else if (embedded)
            Muted("{ }");
        else
        {
            const std::string value = ManagedValueText(process, address, typeCode, enumCode);
            PushMono();
            if (value.empty())
                Muted("--");
            else
                ImGui::TextUnformatted(value.c_str());
            PopMono();
        }
        ImGui::TableNextColumn();
        PushMono();
        ImGui::TextColored(ManifoldTheme::BytesText, "+%llX", static_cast<unsigned long long>(objectOffset));
        PopMono();
        if (open)
        {
            // A struct stored inline: its fields carry the boxed convention's
            // two-pointer header, which an embedded copy does not have.
            const std::vector<Target::MonoField> inner = m_engine.Mono().TypedFieldsOf(typeHandle);
            const std::int64_t header = 2 * static_cast<std::int64_t>(process.PointerSize());
            FieldRows(page, address - objectOffset, inner, address, -header, name, depth + 1);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    bool ClrInspector::ReferenceCell(const Page& page, const std::string& name, std::uint64_t slot, std::uint64_t objectOffset)
    {
        const Target::Process& process = *m_engine.Process();
        const std::uint64_t target = process.ReadPointer(slot).value_or(0);
        if (!target)
        {
            Muted("null");
            return false;
        }
        std::string text = m_engine.Mono().ClassNameAt(target).value_or("?");
        if (text == "System.String")
        {
            const auto value = ClrStringText(process, target, 120);
            text = value ? Quoted(*value) : text;
        }
        else
            text = ShortName(text) + "  " + Hex(target);
        PushMono();
        const bool clicked = ImGui::Selectable((ICON_FA_ARROW_RIGHT "  " + text + "##ref").c_str(), false);
        PopMono();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Opens the object at %s", Hex(target).c_str());
        if (clicked)
        {
            const std::string leaf = name.substr(name.rfind('.') == std::string::npos ? 0 : name.rfind('.') + 1);
            Open(target, Through(page, objectOffset, name.rfind('[', 0) == 0 ? name : leaf));
        }
        return clicked;
    }

    void ClrInspector::ArrayElements(const Page& page, const Target::ManagedObject& array)
    {
        const Target::Process& process = *m_engine.Process();
        Muted(Format("%llu elements of %s  ·  %llu bytes each", static_cast<unsigned long long>(array.Length), ShortName(array.ElementName.empty() ? std::string("?") : array.ElementName).c_str(),
            static_cast<unsigned long long>(array.ElementSize)));
        if (!array.Data || !array.ElementSize || array.Data < page.Address)
            return;
        const std::uint64_t dataOffset = array.Data - page.Address;
        // A struct element shows its fields like an embedded field does.
        std::vector<Target::MonoField> structFields;
        const bool structs = array.ElementType == 0x11 && array.ElementHandle;
        if (structs)
            structFields = m_engine.Mono().TypedFieldsOf(array.ElementHandle);
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##clrelements", 4, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, Px(64.0f));
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(std::min<std::uint64_t>(array.Length, 10000000)));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const std::uint64_t offset = dataOffset + static_cast<std::uint64_t>(i) * array.ElementSize;
                const std::uint64_t address = page.Address + offset;
                const std::string name = "[" + std::to_string(i) + "]";
                if (structs)
                    ValueRow(page, name, array.ElementName, address, offset, 0x11, 0, array.ElementHandle, static_cast<int>(array.ElementSize), 0);
                else
                    ValueRow(page, name, array.ElementName, address, offset, array.ElementType, 0, 0, 0, 0);
            }
        }
        ImGui::EndTable();
        (void)process;
    }
}
