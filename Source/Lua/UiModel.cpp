#include "Lua/UiModel.h"

#include <algorithm>
#include <limits>

namespace Manifold::Lua
{
    namespace
    {
        constexpr std::pair<std::string_view, UiKind> kKinds[] = {
            { "window", UiKind::Window },
            { "row", UiKind::Row },
            { "column", UiKind::Column },
            { "section", UiKind::Section },
            { "tabs", UiKind::Tabs },
            { "tab", UiKind::Tab },
            { "split", UiKind::Split },
            { "text", UiKind::Text },
            { "heading", UiKind::Heading },
            { "separator", UiKind::Separator },
            { "spacer", UiKind::Spacer },
            { "button", UiKind::Button },
            { "checkbox", UiKind::Checkbox },
            { "input", UiKind::Input },
            { "number", UiKind::Number },
            { "slider", UiKind::Slider },
            { "combo", UiKind::Combo },
            { "table", UiKind::Table },
            { "tree", UiKind::Tree },
            { "progress", UiKind::Progress },
            { "image", UiKind::Image },
            { "canvas", UiKind::Canvas },
        };
        constexpr std::size_t kMaxEvents = 4096;
    }

    std::string_view UiKindName(UiKind kind)
    {
        for (const auto& [name, value] : kKinds)
        {
            if (value == kind)
                return name;
        }
        return "?";
    }

    std::optional<UiKind> UiKindFromName(std::string_view name)
    {
        for (const auto& [text, value] : kKinds)
        {
            if (text == name)
                return value;
        }
        return std::nullopt;
    }

    bool UiIsContainer(UiKind kind)
    {
        switch (kind)
        {
        case UiKind::Window:
        case UiKind::Row:
        case UiKind::Column:
        case UiKind::Section:
        case UiKind::Tabs:
        case UiKind::Tab:
        case UiKind::Split:
            return true;
        default:
            return false;
        }
    }

    int UiModel::Create(UiKind kind, int parent, std::string owner)
    {
        if (kind != UiKind::Window)
        {
            UiNode* above = Find(parent);
            if (!above || !UiIsContainer(above->Kind))
                return 0;
        }
        else
            parent = 0;
        const int id = m_next++;
        UiNode node;
        node.Id = id;
        node.Kind = kind;
        node.Parent = parent;
        node.Owner = std::move(owner);
        m_nodes.emplace(id, std::move(node));
        if (kind == UiKind::Window)
            m_windows.push_back(id);
        else if (UiNode* above = Find(parent))
        {
            above->Children.push_back(id);
            Touch(*above);
        }
        ++m_generation;
        return id;
    }

    UiNode* UiModel::Find(int id)
    {
        const auto found = m_nodes.find(id);
        return found == m_nodes.end() ? nullptr : &found->second;
    }

    const UiNode* UiModel::Find(int id) const
    {
        const auto found = m_nodes.find(id);
        return found == m_nodes.end() ? nullptr : &found->second;
    }

    void UiModel::Touch(UiNode& node)
    {
        ++node.Revision;
        ++m_generation;
    }

    void UiModel::RemoveTree(int id)
    {
        const auto found = m_nodes.find(id);
        if (found == m_nodes.end())
            return;
        const std::vector<int> children = found->second.Children;
        for (const int child : children)
            RemoveTree(child);
        m_nodes.erase(id);
        m_events.erase(std::remove_if(m_events.begin(), m_events.end(), [&](const UiEvent& event) { return event.Target == id; }), m_events.end());
    }

    bool UiModel::Remove(int id)
    {
        UiNode* node = Find(id);
        if (!node)
            return false;
        const int parent = node->Parent;
        const bool window = node->Kind == UiKind::Window;
        RemoveTree(id);
        if (window)
            m_windows.erase(std::remove(m_windows.begin(), m_windows.end(), id), m_windows.end());
        else if (UiNode* above = Find(parent))
        {
            above->Children.erase(std::remove(above->Children.begin(), above->Children.end(), id), above->Children.end());
            Touch(*above);
        }
        ++m_generation;
        return true;
    }

    void UiModel::Clear(int id)
    {
        UiNode* node = Find(id);
        if (!node)
            return;
        const std::vector<int> children = node->Children;
        for (const int child : children)
            RemoveTree(child);
        node = Find(id);
        if (node)
        {
            node->Children.clear();
            Touch(*node);
        }
    }

    std::vector<int> UiModel::Windows() const
    {
        return m_windows;
    }

    std::vector<int> UiModel::Owned(std::string_view owner) const
    {
        std::vector<int> found;
        for (const int id : m_windows)
        {
            if (const UiNode* node = Find(id); node && node->Owner == owner)
                found.push_back(id);
        }
        return found;
    }

    int UiModel::AddImage(UiImage image)
    {
        image.Id = m_next++;
        const int id = image.Id;
        m_images.emplace(id, std::move(image));
        ++m_generation;
        return id;
    }

    const UiImage* UiModel::FindImage(int id) const
    {
        const auto found = m_images.find(id);
        return found == m_images.end() ? nullptr : &found->second;
    }

    bool UiModel::RemoveImage(int id)
    {
        ++m_generation;
        return m_images.erase(id) != 0;
    }

    int UiModel::AddSlot(UiSlot slot)
    {
        slot.Id = m_next++;
        slot.Order = ++m_order;
        const int id = slot.Id;
        m_slots.emplace(id, std::move(slot));
        ++m_generation;
        return id;
    }

    UiSlot* UiModel::FindSlot(int id)
    {
        const auto found = m_slots.find(id);
        return found == m_slots.end() ? nullptr : &found->second;
    }

    bool UiModel::RemoveSlot(int id)
    {
        ++m_generation;
        return m_slots.erase(id) != 0;
    }

    std::vector<UiSlot> UiModel::Slots(UiSlotKind kind) const
    {
        std::vector<UiSlot> found;
        for (const auto& [id, slot] : m_slots)
        {
            if (slot.Kind == kind)
                found.push_back(slot);
        }
        std::sort(found.begin(), found.end(), [](const UiSlot& a, const UiSlot& b) { return a.Order < b.Order; });
        return found;
    }

    int UiModel::AddDialog(UiDialog dialog)
    {
        dialog.Id = m_next++;
        const int id = dialog.Id;
        m_dialogs.push_back(std::move(dialog));
        ++m_generation;
        return id;
    }

    bool UiModel::RemoveDialog(int id)
    {
        const auto found = std::find_if(m_dialogs.begin(), m_dialogs.end(), [&](const UiDialog& dialog) { return dialog.Id == id; });
        if (found == m_dialogs.end())
            return false;
        m_dialogs.erase(found);
        ++m_generation;
        return true;
    }

    void UiModel::Post(UiEvent event)
    {
        if (m_events.size() >= kMaxEvents)
            return;
        if (event.Name == "change" || event.Name == "hover" || event.Name == "view")
        {
            for (UiEvent& waiting : m_events)
            {
                if (waiting.Target == event.Target && waiting.Name == event.Name)
                {
                    waiting.Values = std::move(event.Values);
                    return;
                }
            }
        }
        m_events.push_back(std::move(event));
    }

    std::vector<UiEvent> UiModel::TakeEvents()
    {
        std::vector<UiEvent> taken;
        taken.swap(m_events);
        return taken;
    }

    double UiModel::NextUpdate() const
    {
        double next = std::numeric_limits<double>::infinity();
        for (const int id : m_windows)
        {
            if (const UiNode* node = Find(id); node && node->Updates && node->Open)
                next = std::min(next, node->NextUpdate);
        }
        return next;
    }

    void UiModel::ForgetOwner(std::string_view owner)
    {
        for (const int id : Owned(owner))
            Remove(id);
        std::vector<int> slots;
        for (const auto& [id, slot] : m_slots)
        {
            if (slot.Owner == owner)
                slots.push_back(id);
        }
        for (const int id : slots)
            RemoveSlot(id);
        m_dialogs.erase(std::remove_if(m_dialogs.begin(), m_dialogs.end(), [&](const UiDialog& dialog) { return dialog.Owner == owner; }), m_dialogs.end());
        std::vector<int> images;
        for (const auto& [id, image] : m_images)
        {
            if (image.Owner == owner)
                images.push_back(id);
        }
        for (const int id : images)
            RemoveImage(id);
        ++m_generation;
    }

    void UiModel::Reset()
    {
        m_nodes.clear();
        m_windows.clear();
        m_images.clear();
        m_slots.clear();
        m_dialogs.clear();
        m_events.clear();
        ++m_generation;
    }
}
