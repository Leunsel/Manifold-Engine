#include "Lua/UiModel.h"

#include "Core/Text.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
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
            { "grid", UiKind::Grid },
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
            { "radio", UiKind::Radio },
            { "color", UiKind::Color },
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
        case UiKind::Grid:
            return true;
        default:
            return false;
        }
    }

    std::pair<double, double> UiValueRange(const UiNode& node)
    {
        const bool slider = node.Kind == UiKind::Slider;
        constexpr double kNone = std::numeric_limits<double>::infinity();
        double low = node.HasMin && !std::isnan(node.Min) ? node.Min : slider ? 0.0 : -kNone;
        double high = node.HasMax && !std::isnan(node.Max) ? node.Max : slider ? (node.Integer ? 100.0 : 1.0) : kNone;
        if (low > high)
            std::swap(low, high);
        return { low, high };
    }

    double UiClampValue(const UiNode& node, double value)
    {
        const auto [low, high] = UiValueRange(node);
        return std::clamp(value, low, high);
    }

    double UiSnapValue(const UiNode& node, double value)
    {
        const auto [lowLimit, highLimit] = UiValueRange(node);
        if (std::isnan(value))
            return value;
        if (node.Integer)
        {
            const std::int64_t low = UiToInteger(lowLimit);
            const std::int64_t high = UiToInteger(highLimit);
            const std::int64_t whole = std::clamp(UiToInteger(value), low, high);
            if (!(node.Step > 1.0))
                return static_cast<double>(whole);
            // ∑ unsigned, as max - min can exceed the int64 range; rounds half
            // up like (offset + step / 2) / step * step did, which went past max
            const std::uint64_t step = static_cast<std::uint64_t>(UiToInteger(std::round(node.Step)));
            const std::uint64_t range = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
            const std::uint64_t offset = static_cast<std::uint64_t>(whole) - static_cast<std::uint64_t>(low);
            std::uint64_t snapped = offset / step * step;
            if (offset % step >= (step + 1) / 2)
                snapped = range - snapped < step ? range : snapped + step;
            return static_cast<double>(static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + snapped));
        }
        if (node.Step > 0.0 && std::isfinite(lowLimit))
            value = lowLimit + std::round((value - lowLimit) / node.Step) * node.Step;
        return std::clamp(value, lowLimit, highLimit);
    }

    std::int64_t UiToInteger(double value)
    {
        if (std::isnan(value))
            return 0;
        // 2^63 is the first double past the range; the lowest int64 is exactly -2^63
        if (value >= 9223372036854775808.0)
            return std::numeric_limits<std::int64_t>::max();
        if (value <= -9223372036854775808.0)
            return std::numeric_limits<std::int64_t>::min();
        return static_cast<std::int64_t>(value);
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
        m_watches.erase(std::remove_if(m_watches.begin(), m_watches.end(), [&](const UiWatch& watch) { return watch.Window == id; }), m_watches.end());
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

    void UiModel::PollHotkeys(const std::function<bool(const std::vector<int>&, bool&)>& pressed)
    {
        std::vector<int> fired;
        std::vector<std::pair<int, int>> moved; // a number, slider, combo or radio: +1 or -1
        // `live`: the window takes hotkeys now; keys of the others are still
        // asked, so a key held while a modal was up does not fire once it closes
        std::function<void(int, bool, bool, bool)> walk = [&](int id, bool enabled, bool shown, bool live) {
            UiNode* node = Find(id);
            if (!node)
                return;
            const bool on = enabled && node->Enabled;
            // a widget in a hidden row, section or tab is hidden too
            const bool visible = shown && node->Visible;
            const bool fires = on && visible && live;
            if (!node->HotkeyKeys.empty() && (node->Kind == UiKind::Checkbox || node->Kind == UiKind::Button))
            {
                // every key set is asked each time, so a held key fires once, also when it was held while disabled
                if (pressed(node->HotkeyKeys, node->HotkeyDown) && fires)
                    fired.push_back(id);
            }
            const bool stepper = node->Kind == UiKind::Number || node->Kind == UiKind::Slider || (node->Kind == UiKind::Combo && !node->Multi) || node->Kind == UiKind::Radio;
            if (stepper && !node->HotkeyMoreKeys.empty() && pressed(node->HotkeyMoreKeys, node->HotkeyMoreHeld) && fires)
                moved.push_back({ id, 1 });
            if (stepper && !node->HotkeyLessKeys.empty() && pressed(node->HotkeyLessKeys, node->HotkeyLessHeld) && fires)
                moved.push_back({ id, -1 });
            for (const int child : std::vector<int>(node->Children))
                walk(child, on, visible, live);
        };
        // ∑ an open modal holds the program: only the one in front (opened
        // last) takes hotkeys, the windows behind it wait like their mouse input
        std::erase_if(m_modalOrder, [&](int id) {
            const UiNode* node = Find(id);
            return !node || !node->Open || !node->Modal;
        });
        for (const int window : Windows())
        {
            const UiNode* node = Find(window);
            if (node && node->Open && node->Modal && std::find(m_modalOrder.begin(), m_modalOrder.end(), window) == m_modalOrder.end())
                m_modalOrder.push_back(window);
        }
        const int front = m_modalOrder.empty() ? 0 : m_modalOrder.back();
        for (const int window : Windows())
        {
            if (const UiNode* node = Find(window); node && node->Open)
                walk(window, true, true, front == 0 || window == front);
        }
        for (const int id : fired)
        {
            UiNode* node = Find(id);
            if (!node)
                continue;
            if (node->Kind == UiKind::Checkbox)
            {
                node->Bool = !node->Bool;
                Touch(*node);
                if (node->Handlers & UiHandler::Change)
                    Post(UiEvent{ id, "change", { node->Bool } });
            }
            else if (node->Confirm.empty() && (node->Handlers & UiHandler::Click))
                Post(UiEvent{ id, "click", {} });
        }
        for (const auto& [id, direction] : moved)
        {
            UiNode* node = Find(id);
            if (!node)
                continue;
            if (node->Kind == UiKind::Number || node->Kind == UiKind::Slider)
            {
                // ∑ one press moves it by hotkeyStep (else its step, else 1), within min and max
                const double step = node->HotkeyStep > 0.0 ? node->HotkeyStep : node->Step > 0.0 ? node->Step : 1.0;
                if (!std::isfinite(node->Number))
                    continue;
                double value = node->Number + direction * step;
                value = node->Integer ? std::round(value) : std::round(value * 1.0e9) / 1.0e9; // no 0.30000000000000004
                // the limits the slider is drawn with (0..100 or 0..1 without min and max), after rounding
                value = UiClampValue(*node, value);
                if (value == node->Number || !std::isfinite(value))
                    continue;
                node->Number = value;
                Touch(*node);
                if (node->Handlers & UiHandler::Change)
                    Post(UiEvent{ id, "change", { value } });
            }
            else
            {
                // the next or the previous item, round the end
                const int count = static_cast<int>(node->Items.size());
                if (count == 0)
                    continue;
                int chosen = node->Selected + direction;
                if (chosen < 1)
                    chosen = count;
                if (chosen > count)
                    chosen = 1;
                node->Selected = chosen;
                Touch(*node);
                if (node->Handlers & UiHandler::Change)
                    Post(UiEvent{ id, "change", { static_cast<double>(chosen) } });
            }
        }
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
            const UiNode* node = Find(id);
            if (node && node->Updates && node->Open)
                next = std::min(next, node->NextUpdate);
            if (node && node->Binds && node->Open)
                next = std::min(next, node->NextBind);
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

    std::uint64_t UiModel::FileStamp(std::string_view path)
    {
        std::error_code error;
        const std::filesystem::path file(Utf8ToWide(path));
        const auto written = std::filesystem::last_write_time(file, error);
        if (error)
            return 0;
        const auto size = std::filesystem::file_size(file, error);
        const std::uint64_t ticks = static_cast<std::uint64_t>(written.time_since_epoch().count());
        return (ticks ^ (static_cast<std::uint64_t>(error ? 0 : size) * 0x9E3779B97F4A7C15ull)) | 1u;
    }

    void UiModel::Watch(int window, std::string path)
    {
        Unwatch(window);
        UiWatch watch;
        watch.Window = window;
        watch.Stamp = FileStamp(path);
        watch.Path = std::move(path);
        m_watches.push_back(std::move(watch));
    }

    void UiModel::Unwatch(int window)
    {
        m_watches.erase(std::remove_if(m_watches.begin(), m_watches.end(), [&](const UiWatch& watch) { return watch.Window == window; }), m_watches.end());
    }

    int UiModel::Watching(std::string_view path) const
    {
        const auto normal = [](std::string_view text) { return Lower(WideToUtf8(std::filesystem::path(Utf8ToWide(text)).lexically_normal().generic_wstring())); };
        const std::string wanted = normal(path);
        int count = 0;
        for (const UiWatch& watch : m_watches)
        {
            if (normal(watch.Path) == wanted)
                ++count;
        }
        return count;
    }

    void UiModel::PollWatches(double now)
    {
        if (m_watches.empty() || now < m_nextPoll)
            return;
        m_nextPoll = now + 0.25;
        for (UiWatch& watch : m_watches)
        {
            const std::uint64_t stamp = FileStamp(watch.Path);
            if (stamp == 0 || stamp == watch.Stamp)
                continue;
            watch.Stamp = stamp;
            Post({ watch.Window, "reload", {} });
        }
    }

    void UiModel::Reset()
    {
        m_watches.clear();
        m_modalOrder.clear();
        m_nodes.clear();
        m_windows.clear();
        m_images.clear();
        m_slots.clear();
        m_dialogs.clear();
        m_events.clear();
        ++m_generation;
    }
}
