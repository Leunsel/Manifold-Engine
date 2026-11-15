#include "Table/AddressList.h"

#include "Core/Text.h"

#include <algorithm>

#include <windows.h>

namespace Manifold::Table
{
    Scan::ValueFormat MemoryRecord::Format(int pointerSize) const
    {
        Scan::ValueFormat format;
        format.Hex = ShowHex;
        format.Signed = ShowSigned;
        format.Unicode = Unicode;
        format.Length = Length;
        format.BitStart = BitStart;
        format.BitLength = BitLength;
        format.PointerSize = pointerSize;
        return format;
    }

    int MemoryRecord::Size(int pointerSize) const
    {
        return Scan::ValueSize(Type, Format(pointerSize));
    }

    bool MemoryRecord::ChildrenVisible() const
    {
        if (Children.empty())
            return false;
        if (Options.AlwaysHideChildren)
            return false;
        if (Options.HideChildren && !Active)
            return false;
        if (Options.AllowManualExpand || Kind == RecordKind::Group || !Options.HideChildren)
            return Expanded;
        return true;
    }

    bool MemoryRecord::IsDescendantOf(const MemoryRecord* other) const
    {
        for (const MemoryRecord* at = Parent; at; at = at->Parent)
        {
            if (at == other)
                return true;
        }
        return false;
    }

    std::unique_ptr<MemoryRecord> MemoryRecord::Clone(const std::function<int()>& nextId) const
    {
        auto copy = std::make_unique<MemoryRecord>();
        copy->Id = nextId();
        copy->Description = Description;
        copy->Kind = Kind;
        copy->Type = Type;
        copy->Length = Length;
        copy->Unicode = Unicode;
        copy->ZeroTerminate = ZeroTerminate;
        copy->BitStart = BitStart;
        copy->BitLength = BitLength;
        copy->ShowHex = ShowHex;
        copy->ShowSigned = ShowSigned;
        copy->Address = Address;
        copy->Offsets = Offsets;
        copy->Script = Script;
        copy->Comment = Comment;
        copy->Color = Color;
        copy->HasColor = HasColor;
        copy->Options = Options;
        copy->Freeze = Freeze;
        copy->DropDown = DropDown;
        copy->DropDownOnly = DropDownOnly;
        copy->DropDownDescriptionOnly = DropDownDescriptionOnly;
        copy->Expanded = Expanded;
        copy->LuaOnActivate = LuaOnActivate;
        copy->DropDownLink = DropDownLink;
        copy->AddressGroupHeader = AddressGroupHeader;
        copy->DontSave = DontSave;
        copy->Async = Async;
        copy->CustomTypeName = CustomTypeName;
        copy->LuaOnDeactivate = LuaOnDeactivate;
        for (const auto& child : Children)
        {
            auto cloned = child->Clone(nextId);
            cloned->Parent = copy.get();
            copy->Children.push_back(std::move(cloned));
        }
        return copy;
    }

    AddressList::AddressList() = default;

    MemoryRecord& AddressList::Add(std::unique_ptr<MemoryRecord> record, MemoryRecord* parent, int index)
    {
        if (record->Id <= 0 || FindById(record->Id))
            record->Id = NextId();
        NoteId(record->Id);
        record->Parent = parent;
        auto& siblings = parent ? parent->Children : m_roots;
        MemoryRecord& added = *record;
        if (index < 0 || index > static_cast<int>(siblings.size()))
            siblings.push_back(std::move(record));
        else
            siblings.insert(siblings.begin() + index, std::move(record));
        Touch();
        return added;
    }

    MemoryRecord& AddressList::AddValue(std::string description, std::string address, Scan::ValueType type, MemoryRecord* parent)
    {
        auto record = std::make_unique<MemoryRecord>();
        record->Description = std::move(description);
        record->Address = std::move(address);
        record->Type = type;
        return Add(std::move(record), parent);
    }

    MemoryRecord& AddressList::AddScript(std::string description, std::string script, MemoryRecord* parent)
    {
        auto record = std::make_unique<MemoryRecord>();
        record->Description = std::move(description);
        record->Kind = RecordKind::Script;
        record->Script = std::move(script);
        return Add(std::move(record), parent);
    }

    MemoryRecord& AddressList::AddGroup(std::string description, MemoryRecord* parent)
    {
        auto record = std::make_unique<MemoryRecord>();
        record->Description = std::move(description);
        record->Kind = RecordKind::Group;
        record->Options.HideChildren = false;
        return Add(std::move(record), parent);
    }

    std::vector<std::unique_ptr<MemoryRecord>>& AddressList::SiblingsOf(MemoryRecord& record)
    {
        return record.Parent ? record.Parent->Children : m_roots;
    }

    std::unique_ptr<MemoryRecord> AddressList::Take(MemoryRecord& record)
    {
        auto& siblings = SiblingsOf(record);
        for (auto it = siblings.begin(); it != siblings.end(); ++it)
        {
            if (it->get() == &record)
            {
                std::unique_ptr<MemoryRecord> taken = std::move(*it);
                siblings.erase(it);
                taken->Parent = nullptr;
                Touch();
                return taken;
            }
        }
        return nullptr;
    }

    void AddressList::Remove(MemoryRecord& record)
    {
        std::vector<int> removed;
        const std::function<void(const MemoryRecord&)> collect = [&](const MemoryRecord& node)
            {
                removed.push_back(node.Id);
                for (const auto& child : node.Children)
                    collect(*child);
            };
        collect(record);
        m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(), [&](int id) { return std::find(removed.begin(), removed.end(), id) != removed.end(); }),
            m_selection.end());
        Take(record);
    }

    bool AddressList::Move(MemoryRecord& record, MemoryRecord* newParent, int index)
    {
        if (newParent == &record || (newParent && newParent->IsDescendantOf(&record)))
            return false;
        auto& oldSiblings = SiblingsOf(record);
        int oldIndex = -1;
        for (std::size_t i = 0; i < oldSiblings.size(); ++i)
        {
            if (oldSiblings[i].get() == &record)
                oldIndex = static_cast<int>(i);
        }
        std::unique_ptr<MemoryRecord> taken = Take(record);
        if (!taken)
            return false;
        if (record.Parent == newParent && oldIndex >= 0 && index > oldIndex)
            --index;
        taken->Parent = newParent;
        auto& siblings = newParent ? newParent->Children : m_roots;
        if (index < 0 || index > static_cast<int>(siblings.size()))
            siblings.push_back(std::move(taken));
        else
            siblings.insert(siblings.begin() + index, std::move(taken));
        Touch();
        return true;
    }

    TableFile* AddressList::FindFile(std::string_view name)
    {
        for (TableFile& file : Files)
        {
            if (EqualsNoCase(file.Name, name))
                return &file;
        }
        return nullptr;
    }

    TableFile& AddressList::AddFile(std::string name)
    {
        if (TableFile* existing = FindFile(name))
            return *existing;
        TableFile file;
        file.Name = std::move(name);
        Files.push_back(std::move(file));
        Touch();
        return Files.back();
    }

    bool AddressList::RemoveFile(std::string_view name)
    {
        const auto before = Files.size();
        Files.erase(std::remove_if(Files.begin(), Files.end(), [&](const TableFile& file) { return EqualsNoCase(file.Name, name); }), Files.end());
        if (Files.size() != before)
            Touch();
        return Files.size() != before;
    }

    void AddressList::Clear()
    {
        m_roots.clear();
        m_selection.clear();
        TableLuaScript.clear();
        TableComment.clear();
        FilePath.clear();
        Files.clear();
        ExtraXml.clear();
        LoadedVersion = 0;
        m_nextId = 0;
        Touch();
        m_dirty = false;
    }

    MemoryRecord* AddressList::FindById(int id)
    {
        MemoryRecord* found = nullptr;
        ForEach([&](MemoryRecord& record)
            {
                if (!found && record.Id == id)
                    found = &record;
            });
        return found;
    }

    MemoryRecord* AddressList::FindByDescription(std::string_view description)
    {
        MemoryRecord* found = nullptr;
        ForEach([&](MemoryRecord& record)
            {
                if (!found && record.Description == description)
                    found = &record;
            });
        return found;
    }

    void AddressList::ForEach(const std::function<void(MemoryRecord&)>& visit)
    {
        const std::function<void(std::vector<std::unique_ptr<MemoryRecord>>&)> walk = [&](std::vector<std::unique_ptr<MemoryRecord>>& list)
            {
                for (std::size_t i = 0; i < list.size(); ++i)
                {
                    MemoryRecord& record = *list[i];
                    visit(record);
                    walk(record.Children);
                }
            };
        walk(m_roots);
    }

    void AddressList::ForEachVisible(const std::function<void(MemoryRecord&, int depth)>& visit)
    {
        const std::function<void(std::vector<std::unique_ptr<MemoryRecord>>&, int)> walk = [&](std::vector<std::unique_ptr<MemoryRecord>>& list, int depth)
            {
                for (std::size_t i = 0; i < list.size(); ++i)
                {
                    MemoryRecord& record = *list[i];
                    visit(record, depth);
                    if (record.ChildrenVisible())
                        walk(record.Children, depth + 1);
                }
            };
        walk(m_roots, 0);
    }

    std::vector<MemoryRecord*> AddressList::Flatten()
    {
        std::vector<MemoryRecord*> list;
        ForEach([&](MemoryRecord& record) { list.push_back(&record); });
        return list;
    }

    std::size_t AddressList::Count() const
    {
        std::size_t count = 0;
        const std::function<void(const std::vector<std::unique_ptr<MemoryRecord>>&)> walk = [&](const std::vector<std::unique_ptr<MemoryRecord>>& list)
            {
                for (const auto& record : list)
                {
                    ++count;
                    walk(record->Children);
                }
            };
        walk(m_roots);
        return count;
    }

    int AddressList::IndexOf(const MemoryRecord& record) const
    {
        const auto& siblings = record.Parent ? record.Parent->Children : m_roots;
        for (std::size_t i = 0; i < siblings.size(); ++i)
        {
            if (siblings[i].get() == &record)
                return static_cast<int>(i);
        }
        return -1;
    }

    void AddressList::NoteId(int id)
    {
        if (id >= m_nextId)
            m_nextId = id + 1;
    }

    bool AddressList::Selected(int id) const
    {
        return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
    }

    void AddressList::Select(int id, bool add)
    {
        if (!add)
            m_selection.clear();
        if (!Selected(id))
            m_selection.push_back(id);
    }

    const char* HotkeyActionName(HotkeyAction action)
    {
        switch (action)
        {
        case HotkeyAction::Toggle: return "Toggle freeze / activate";
        case HotkeyAction::ToggleAllowIncrease: return "Toggle freeze, allow increase";
        case HotkeyAction::ToggleAllowDecrease: return "Toggle freeze, allow decrease";
        case HotkeyAction::Activate: return "Activate / freeze";
        case HotkeyAction::Deactivate: return "Deactivate / unfreeze";
        case HotkeyAction::SetValue: return "Set value to";
        case HotkeyAction::Increase: return "Increase value by";
        case HotkeyAction::Decrease: return "Decrease value by";
        }
        return "?";
    }

    std::string KeysText(const std::vector<int>& keys)
    {
        std::string text;
        for (int key : keys)
        {
            if (!text.empty())
                text += "+";
            switch (key)
            {
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: text += "Ctrl"; continue;
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: text += "Shift"; continue;
            case VK_MENU: case VK_LMENU: case VK_RMENU: text += "Alt"; continue;
            default: break;
            }
            if (key >= VK_F1 && key <= VK_F24)
            {
                text += "F" + std::to_string(key - VK_F1 + 1);
                continue;
            }
            if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
            {
                text += "Num " + std::to_string(key - VK_NUMPAD0);
                continue;
            }
            const UINT scan = ::MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
            wchar_t name[64] = L"";
            if (scan && ::GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0)
                text += WideToUtf8(name);
            else
                text += "Key " + std::to_string(key);
        }
        return text;
    }

    namespace
    {
        struct OptionLabel
        {
            const char* Name;
            bool RecordOptions::*Field;
        };

        constexpr OptionLabel kOptionLabels[] = {
            { "moHideChildren", &RecordOptions::HideChildren },
            { "moActivateChildrenAsWell", &RecordOptions::ActivateChildren },
            { "moDeactivateChildrenAsWell", &RecordOptions::DeactivateChildren },
            { "moRecursiveSetValue", &RecordOptions::RecursiveSetValue },
            { "moAllowManualCollapseAndExpand", &RecordOptions::AllowManualExpand },
            { "moManualExpandCollapse", &RecordOptions::ManualExpand },
            { "moAlwaysHideChildren", &RecordOptions::AlwaysHideChildren },
        };
    }

    std::string DropDownText(const std::vector<DropDownEntry>& entries)
    {
        std::string text;
        for (const DropDownEntry& entry : entries)
        {
            text += entry.Value;
            text += ':';
            text += entry.Text;
            text += "\r\n";
        }
        return text;
    }

    std::vector<DropDownEntry> ParseDropDown(std::string_view text)
    {
        std::vector<DropDownEntry> entries;
        for (const std::string& line : SplitLines(text))
        {
            const std::string trimmed = Trim(line);
            if (trimmed.empty())
                continue;
            const std::size_t colon = trimmed.find(':');
            DropDownEntry entry;
            entry.Value = colon == std::string::npos ? trimmed : Trim(trimmed.substr(0, colon));
            entry.Text = colon == std::string::npos ? trimmed : Trim(trimmed.substr(colon + 1));
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    std::string OptionsText(const RecordOptions& options)
    {
        std::string text = "[";
        for (const OptionLabel& label : kOptionLabels)
        {
            if (!(options.*label.Field))
                continue;
            if (text.size() > 1)
                text += ',';
            text += label.Name;
        }
        return text + "]";
    }

    void ParseOptionsText(RecordOptions& options, std::string_view text)
    {
        for (const OptionLabel& label : kOptionLabels)
            options.*label.Field = false;
        std::string inner(TrimView(text));
        if (!inner.empty() && inner.front() == '[')
            inner.erase(0, 1);
        if (!inner.empty() && inner.back() == ']')
            inner.pop_back();
        for (const std::string& part : Split(inner, ','))
        {
            const std::string name = Trim(part);
            for (const OptionLabel& label : kOptionLabels)
            {
                if (EqualsNoCase(name, label.Name))
                    options.*label.Field = true;
            }
        }
    }
}
