#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <optional>

#include <windows.h>

namespace Manifold::UI
{
    namespace
    {
        constexpr float kLabelWidth = 104.0f;

        int TypeIndex(Scan::ValueType type)
        {
            for (int i = 0; i < static_cast<int>(std::size(Scan::kRecordTypes)); ++i)
            {
                if (Scan::kRecordTypes[i] == type)
                    return i;
            }
            return 2;
        }

        void BeginSection(const char* id, const char* icon, const char* title)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
            ImGui::TextUnformatted(icon);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, Px(8.0f));
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Card);
            ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::CardBorder);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(14.0f, 12.0f));
            ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }

        void EndSection()
        {
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0.0f, Px(8.0f)));
        }

        void FieldLabel(const char* text)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            ImGui::SameLine(Px(kLabelWidth));
            ImGui::SetNextItemWidth(-FLT_MIN);
        }

        void FieldNote(std::string_view text, ImVec4 color = ManifoldTheme::Muted)
        {
            ImGui::SetCursorPosX(Px(kLabelWidth));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }

        bool ToggleRow(const char* id, const char* title, const char* hint, bool* value)
        {
            ImGui::PushID(id);
            const bool changed = ToggleSwitch("##toggle", value);
            ImGui::PopID();
            ImGui::SameLine(0.0f, Px(10.0f));
            ImGui::BeginGroup();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(title);
            if (hint && *hint)
            {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.5f - Px(4.0f));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                ImGui::TextUnformatted(hint);
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }
            ImGui::EndGroup();
            return changed;
        }

        std::string Unquoted(std::string text)
        {
            text = Trim(text);
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                text = text.substr(1, text.size() - 2);
            return text;
        }

        struct PathStep
        {
            std::optional<std::uint64_t> Pointer;
            std::optional<std::uint64_t> Offset;
            std::optional<std::uint64_t> Address;
        };

        struct ResolvedPath
        {
            std::string Problem;
            std::string Relative;
            std::optional<std::uint64_t> Base;
            std::vector<PathStep> Steps;
        };

        ResolvedPath ResolvePath(Engine& engine, Table::MemoryRecord& record, const std::string& addressText, const std::vector<std::string>& offsets)
        {
            ResolvedPath path;
            path.Steps.resize(offsets.size());
            if (!engine.HasProcess())
            {
                path.Problem = "Open a process to see where this address points.";
                return path;
            }
            const std::string_view written = TrimView(addressText);
            if (written.empty())
            {
                path.Problem = "No address yet.";
                return path;
            }
            Status error;
            if (written.front() == '+' || written.front() == '-')
            {
                Table::MemoryRecord* parent = record.Parent;
                if (!parent)
                {
                    path.Problem = "An address that starts with + or - needs a parent record with an address.";
                    return path;
                }
                engine.ResolveRecord(*parent);
                if (!parent->AddressOk)
                {
                    path.Problem = "The parent \"" + parent->Description + "\" has no address" + (parent->AddressError.empty() ? std::string(".") : ": " + parent->AddressError);
                    return path;
                }
                const auto delta = engine.Evaluate(written.substr(1), &error);
                if (!delta)
                {
                    path.Problem = error.Text;
                    return path;
                }
                path.Base = written.front() == '+' ? parent->ResolvedAddress + *delta : parent->ResolvedAddress - *delta;
                path.Relative = "\"" + parent->Description + "\" " + Hex(parent->ResolvedAddress) + (written.front() == '+' ? " + " : " - ") + Hex(*delta);
            }
            else
            {
                path.Base = engine.Evaluate(addressText, &error);
                if (!path.Base)
                {
                    path.Problem = error.Text;
                    return path;
                }
            }
            std::optional<std::uint64_t> at = path.Base;
            for (std::size_t i = 0; i < offsets.size() && at; ++i)
            {
                PathStep& step = path.Steps[i];
                step.Pointer = engine.Process()->ReadPointer(*at);
                step.Offset = engine.Evaluate(Trim(offsets[i]).empty() ? "0" : offsets[i]);
                if (step.Pointer && step.Offset)
                    step.Address = *step.Pointer + *step.Offset;
                at = step.Address;
            }
            return path;
        }

        const char* KindText(const Table::MemoryRecord& record)
        {
            switch (record.Kind)
            {
            case Table::RecordKind::Script: return ICON_FA_CODE "  Script";
            case Table::RecordKind::Group: return record.AddressGroupHeader ? ICON_FA_FOLDER "  Group header with an address" : ICON_FA_FOLDER "  Group header";
            default: return record.IsPointer() ? ICON_FA_LINK "  Pointer" : ICON_FA_HASHTAG "  Value";
            }
        }
    }

    RecordEditor::RecordEditor(Workspace& workspace, int recordId) : m_workspace(workspace), m_engine(workspace.Core()), m_recordId(recordId)
    {
        if (const Table::MemoryRecord* record = m_engine.Table().FindById(recordId))
            Load(*record);
        m_title = ICON_FA_SLIDERS_H "  " + m_description + "###record" + std::to_string(recordId);
    }

    void RecordEditor::Load(const Table::MemoryRecord& record)
    {
        m_description = record.Description;
        m_address = record.Address;
        m_offsets = record.Offsets;
        m_type = TypeIndex(record.Type);
        m_length = record.Length;
        m_unicode = record.Unicode;
        m_zeroTerminate = record.ZeroTerminate;
        m_bitStart = record.BitStart;
        m_bitLength = record.BitLength;
        m_hex = record.ShowHex;
        m_signed = record.ShowSigned;
        m_options = record.Options;
        m_freeze = static_cast<int>(record.Freeze);
        m_dropDown.clear();
        for (const Table::DropDownEntry& entry : record.DropDown)
            m_dropDown += entry.Value + ":" + entry.Text + "\n";
        m_dropDownLink = Unquoted(record.DropDownLink);
        m_dropDownSource = m_dropDownLink.empty() ? 0 : 1;
        m_hasAddress = record.AddressGroupHeader;
        m_async = record.Options.AsyncScript;
        m_dropDownOnly = record.DropDownOnly;
        m_dropDownDescription = record.DropDownDescriptionOnly;
        m_hotkeys = record.Hotkeys;
        m_hasColor = record.HasColor;
        const ImVec4 color = RecordColor(record.Color);
        m_color[0] = color.x;
        m_color[1] = color.y;
        m_color[2] = color.z;
        m_comment = record.Comment;
    }

    void RecordEditor::Apply(Table::MemoryRecord& record)
    {
        record.Description = m_description;
        record.Comment = m_comment;
        record.Options = m_options;
        record.Hotkeys = m_hotkeys;
        record.HasColor = m_hasColor;
        record.Color = static_cast<std::uint32_t>(std::lround(m_color[0] * 255.0f)) | (static_cast<std::uint32_t>(std::lround(m_color[1] * 255.0f)) << 8) |
                       (static_cast<std::uint32_t>(std::lround(m_color[2] * 255.0f)) << 16);
        const bool addressed = record.Kind == Table::RecordKind::Value || (record.Kind == Table::RecordKind::Group && m_hasAddress);
        if (record.Kind == Table::RecordKind::Group)
            record.AddressGroupHeader = m_hasAddress;
        if (addressed)
        {
            record.Address = Trim(m_address);
            record.Offsets.clear();
            for (const std::string& offset : m_offsets)
                record.Offsets.push_back(Trim(offset).empty() ? "0" : Trim(offset));
        }
        if (record.Kind == Table::RecordKind::Value)
        {
            record.Type = Scan::kRecordTypes[m_type];
            record.Length = std::max(1, m_length);
            record.Unicode = m_unicode;
            record.ZeroTerminate = m_zeroTerminate;
            record.BitStart = std::clamp(m_bitStart, 0, 63);
            record.BitLength = std::clamp(m_bitLength, 1, 64 - record.BitStart);
            record.ShowHex = m_hex;
            record.ShowSigned = m_signed;
            record.Freeze = static_cast<Table::FreezeMode>(m_freeze);
        }
        if (record.Kind == Table::RecordKind::Value || record.Kind == Table::RecordKind::Group)
        {
            record.DropDown.clear();
            for (const std::string& line : SplitLines(m_dropDown))
            {
                const std::string trimmed = Trim(line);
                if (trimmed.empty())
                    continue;
                const std::size_t colon = trimmed.find(':');
                record.DropDown.push_back({ colon == std::string::npos ? trimmed : Trim(trimmed.substr(0, colon)), colon == std::string::npos ? trimmed : Trim(trimmed.substr(colon + 1)) });
            }
            record.DropDownLink = m_dropDownSource == 1 ? Trim(m_dropDownLink) : std::string();
            record.DropDownOnly = m_dropDownOnly;
            record.DropDownDescriptionOnly = m_dropDownDescription;
        }
        if (record.Kind == Table::RecordKind::Script)
            record.Options.AsyncScript = m_async;
        m_engine.Table().Touch();
        m_engine.RefreshValues(true);
    }

    void RecordEditor::CopyAddress(const char* id, std::uint64_t address)
    {
        const std::string text = Hex(address);
        const bool copied = m_copied == text && ImGui::GetTime() - m_copiedAt < 1.5;
        PushMono();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.07f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Text, copied ? ManifoldTheme::Accent : ManifoldTheme::AddressText);
        ImGui::PushID(id);
        if (ImGui::Button((text + "  " + (copied ? ICON_FA_CHECK : ICON_FA_COPY)).c_str()))
        {
            ImGui::SetClipboardText(text.c_str());
            m_copied = text;
            m_copiedAt = ImGui::GetTime();
        }
        ImGui::PopID();
        ImGui::PopStyleColor(4);
        PopMono();
        ItemTooltip(copied ? "Copied" : "Copy this address");
    }

    void RecordEditor::AddressSection(Table::MemoryRecord& record)
    {
        const bool group = record.Kind == Table::RecordKind::Group;
        if (record.Kind != Table::RecordKind::Value && !group)
            return;
        BeginSection("##address", ICON_FA_MAP_MARKER_ALT, "Address");
        if (group)
        {
            ToggleRow("hasaddress", "Has an address", "Children whose address starts with + or - are placed relative to this one.", &m_hasAddress);
            if (!m_hasAddress)
            {
                EndSection();
                return;
            }
            ImGui::Spacing();
        }
        FieldLabel("Address");
        InputTextWithHint("##address", "game.exe+1234, or +10 from the parent", m_address);
        const ResolvedPath path = ResolvePath(m_engine, record, m_address, m_offsets);
        ImGui::SetCursorPosX(Px(kLabelWidth));
        if (!path.Base)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            ImGui::TextUnformatted(path.Problem.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
        else
        {
            if (!path.Relative.empty())
            {
                ImGui::AlignTextToFramePadding();
                Muted(path.Relative + "  =");
                ImGui::SameLine(0.0f, Px(4.0f));
            }
            CopyAddress("base", *path.Base);
        }

        if (!m_offsets.empty())
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
            ImGui::TextUnformatted("Pointer path");
            ImGui::PopStyleColor();
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
            if (ImGui::BeginTable("##path", 4, flags))
            {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, Px(22.0f));
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, Px(96.0f));
                ImGui::TableSetupColumn("Read", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                int remove = -1;
                for (std::size_t i = 0; i < m_offsets.size(); ++i)
                {
                    const PathStep& step = path.Steps[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    Muted(std::to_string(i + 1));
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    InputTextWithHint("##offset", "0", m_offsets[i]);
                    ImGui::TableNextColumn();
                    if (step.Pointer && step.Offset)
                    {
                        PushMono();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(ManifoldTheme::Muted, "[%s] + %s", Hex(*step.Pointer).c_str(), Hex(*step.Offset).c_str());
                        PopMono();
                        ImGui::SameLine(0.0f, Px(4.0f));
                        ImGui::AlignTextToFramePadding();
                        Muted(ICON_FA_LONG_ARROW_ALT_RIGHT);
                        ImGui::SameLine(0.0f, Px(2.0f));
                        CopyAddress("step", *step.Address);
                    }
                    else if (path.Base)
                    {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(ManifoldTheme::Warning, "%s", step.Offset ? "the pointer here cannot be read" : "the offset is not a number or symbol");
                    }
                    ImGui::TableNextColumn();
                    if (IconButton(ICON_FA_TIMES, "Remove this offset"))
                        remove = static_cast<int>(i);
                    ImGui::PopID();
                }
                ImGui::EndTable();
                if (remove >= 0)
                    m_offsets.erase(m_offsets.begin() + remove);
            }
        }
        if (ImGui::SmallButton(ICON_FA_PLUS "  Pointer offset"))
            m_offsets.push_back("0");
        ItemTooltip("Turns this into a pointer: the address holds a pointer, and the offsets lead from it to the value. Offset 1 is added first.");
        EndSection();
    }

    void RecordEditor::ValueSection()
    {
        BeginSection("##value", ICON_FA_HASHTAG, "Value");
        FieldLabel("Type");
        if (ImGui::BeginCombo("##type", Scan::TypeName(Scan::kRecordTypes[m_type])))
        {
            for (int i = 0; i < static_cast<int>(std::size(Scan::kRecordTypes)); ++i)
            {
                if (ImGui::Selectable(Scan::TypeName(Scan::kRecordTypes[i]), i == m_type))
                    m_type = i;
            }
            ImGui::EndCombo();
        }
        const Scan::ValueType type = Scan::kRecordTypes[m_type];
        if (type == Scan::ValueType::String || type == Scan::ValueType::ByteArray)
        {
            FieldLabel("Length");
            ImGui::InputInt("##length", &m_length);
            m_length = std::clamp(m_length, 1, 4096);
            if (type == Scan::ValueType::String)
            {
                ImGui::SetCursorPosX(Px(kLabelWidth));
                ImGui::Checkbox("UTF-16", &m_unicode);
                ImGui::SameLine();
                ImGui::Checkbox("Zero terminated", &m_zeroTerminate);
            }
        }
        if (type == Scan::ValueType::Binary)
        {
            FieldLabel("First bit");
            ImGui::InputInt("##bitstart", &m_bitStart);
            FieldLabel("Bits");
            ImGui::InputInt("##bitlength", &m_bitLength);
        }
        FieldLabel("Show as");
        int display = m_hex ? 1 : 0;
        const float segment = std::min(Px(240.0f), ImGui::GetContentRegionAvail().x - Px(110.0f));
        if (Segmented("##display", &display, { "Decimal", "Hexadecimal" }, segment))
            m_hex = display == 1;
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::Checkbox("Signed", &m_signed);
        FieldLabel("Freeze");
        Segmented("##freeze", &m_freeze, { "Normal", "Allow increase", "Allow decrease" }, ImGui::GetContentRegionAvail().x);
        FieldNote("How a frozen value behaves: always written back, or only when the game lowers or raises it.");
        EndSection();
    }

    void RecordEditor::DropDownSection(Table::MemoryRecord& record)
    {
        BeginSection("##dropdown", ICON_FA_LIST_UL, "Drop-down list");
        FieldLabel("Source");
        Segmented("##source", &m_dropDownSource, { "Own list", "List of another record" }, ImGui::GetContentRegionAvail().x);
        if (m_dropDownSource == 0)
        {
            FieldNote("One entry per line, written value:description.");
            ImGui::SetCursorPosX(Px(kLabelWidth));
            InputMultiline("##dropdowntext", m_dropDown, ImVec2(-FLT_MIN, Px(110.0f)));
        }
        else
        {
            FieldLabel("Record");
            const std::string current = m_dropDownLink.empty() ? std::string("Pick a record") : m_dropDownLink;
            if (ImGui::BeginCombo("##link", current.c_str(), ImGuiComboFlags_HeightLarge))
            {
                for (const Table::MemoryRecord* other : m_engine.Table().Flatten())
                {
                    if (other == &record || other->DropDown.empty())
                        continue;
                    const std::string label = other->Description + "  (" + std::to_string(other->DropDown.size()) + ")";
                    ImGui::PushID(other->Id);
                    if (ImGui::Selectable(label.c_str(), EqualsNoCase(other->Description, m_dropDownLink)))
                        m_dropDownLink = other->Description;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            const Table::MemoryRecord* linked = m_dropDownLink.empty() ? nullptr : m_engine.Table().FindByDescription(m_dropDownLink);
            if (!linked)
                FieldNote(m_dropDownLink.empty() ? "The record whose list this one shows." : "No record with this description, so the own list is used.", m_dropDownLink.empty() ? ManifoldTheme::Muted : ManifoldTheme::Warning);
            else
            {
                std::string preview;
                const std::size_t shown = std::min<std::size_t>(linked->DropDown.size(), 6);
                for (std::size_t i = 0; i < shown; ++i)
                    preview += linked->DropDown[i].Value + " : " + linked->DropDown[i].Text + "\n";
                if (linked->DropDown.size() > shown)
                    preview += "... " + std::to_string(linked->DropDown.size() - shown) + " more";
                FieldNote(preview.empty() ? std::string("The linked record has no entries.") : preview);
            }
        }
        ImGui::SetCursorPosX(Px(kLabelWidth));
        ImGui::Checkbox("Only these values", &m_dropDownOnly);
        ItemTooltip("Changing the value offers only the entries of the list");
        ImGui::SameLine();
        ImGui::Checkbox("Show only the description", &m_dropDownDescription);
        ItemTooltip("The value column shows the description instead of the number");
        EndSection();
    }

    void RecordEditor::ChildrenSection(Table::MemoryRecord& record)
    {
        if (record.Children.empty() && record.Kind != Table::RecordKind::Group)
            return;
        BeginSection("##children", ICON_FA_SITEMAP, "Children");
        ToggleRow("activate", "Activate children as well", "Switching this on switches its children on, top to bottom.", &m_options.ActivateChildren);
        ToggleRow("deactivate", "Deactivate children as well", "Switching this off switches its children off first, bottom to top.", &m_options.DeactivateChildren);
        ToggleRow("hide", "Hide children while off", "The children show only while this record is on.", &m_options.HideChildren);
        ToggleRow("alwayshide", "Always hide children", nullptr, &m_options.AlwaysHideChildren);
        ToggleRow("manual", "Allow opening by hand", "The group can be opened and closed even when its children are hidden.", &m_options.AllowManualExpand);
        if (record.Kind != Table::RecordKind::Script)
            ToggleRow("recursive", "Set the value of children too", "Changing the value writes the same value to the children.", &m_options.RecursiveSetValue);
        EndSection();
    }

    void RecordEditor::HotkeySection()
    {
        BeginSection("##hotkeys", ICON_FA_KEYBOARD, "Hotkeys");
        if (m_capturing)
        {
            std::vector<int> down;
            for (int key = 0x08; key <= 0xFE; ++key)
            {
                if (key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU)
                    continue;
                if (::GetAsyncKeyState(key) & 0x8000)
                    down.push_back(key == VK_LCONTROL || key == VK_RCONTROL ? VK_CONTROL : key == VK_LSHIFT || key == VK_RSHIFT ? VK_SHIFT : key == VK_LMENU || key == VK_RMENU ? VK_MENU : key);
            }
            for (int key : down)
            {
                if (std::find(m_capture.begin(), m_capture.end(), key) == m_capture.end())
                    m_capture.push_back(key);
            }
            ImGui::TextColored(ManifoldTheme::Warning, "Press the keys... %s", Table::KeysText(m_capture).c_str());
            if (down.empty() && !m_capture.empty())
            {
                std::stable_sort(m_capture.begin(), m_capture.end(), [](int a, int b)
                    {
                        const auto rank = [](int key) { return key == VK_CONTROL ? 0 : key == VK_SHIFT ? 1 : key == VK_MENU ? 2 : 3; };
                        return rank(a) < rank(b);
                    });
                if (m_captureIndex >= 0 && m_captureIndex < static_cast<int>(m_hotkeys.size()))
                    m_hotkeys[static_cast<std::size_t>(m_captureIndex)].Keys = m_capture;
                m_capturing = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel"))
                m_capturing = false;
        }
        if (m_hotkeys.empty() && !m_capturing)
            Muted("No hotkeys yet.");
        for (std::size_t i = 0; i < m_hotkeys.size(); ++i)
        {
            Table::Hotkey& hotkey = m_hotkeys[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button((Table::KeysText(hotkey.Keys).empty() ? std::string("Set keys") : Table::KeysText(hotkey.Keys)).c_str(), Px(130.0f, 0.0f)))
            {
                m_capturing = true;
                m_captureIndex = static_cast<int>(i);
                m_capture.clear();
            }
            ItemTooltip("Click and press the new keys");
            ImGui::SameLine();
            const bool needsValue = hotkey.Action == Table::HotkeyAction::SetValue || hotkey.Action == Table::HotkeyAction::Increase || hotkey.Action == Table::HotkeyAction::Decrease;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - Px(8.0f) - (needsValue ? Px(98.0f) : 0.0f));
            int action = static_cast<int>(hotkey.Action);
            if (ImGui::Combo("##action", &action, "Toggle\0Toggle, allow increase\0Toggle, allow decrease\0Activate\0Deactivate\0Set value\0Increase by\0Decrease by\0"))
                hotkey.Action = static_cast<Table::HotkeyAction>(action);
            if (needsValue)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Px(90.0f));
                InputTextWithHint("##value", "value", hotkey.Value);
            }
            ImGui::SameLine();
            const bool remove = IconButton(ICON_FA_TIMES, "Remove this hotkey");
            ImGui::PopID();
            if (remove)
            {
                m_hotkeys.erase(m_hotkeys.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (ImGui::SmallButton(ICON_FA_PLUS "  Add hotkey"))
        {
            Table::Hotkey hotkey;
            hotkey.Id = static_cast<int>(m_hotkeys.size());
            m_hotkeys.push_back(hotkey);
            m_capturing = true;
            m_captureIndex = static_cast<int>(m_hotkeys.size()) - 1;
            m_capture.clear();
        }
        EndSection();
    }

    void RecordEditor::LookSection()
    {
        BeginSection("##look", ICON_FA_PALETTE, "Look and notes");
        ToggleRow("color", "Own color", nullptr, &m_hasColor);
        ImGui::SameLine(0.0f, Px(12.0f));
        ImGui::BeginDisabled(!m_hasColor);
        ImGui::ColorEdit3("##colorvalue", m_color, ImGuiColorEditFlags_NoInputs);
        ImGui::EndDisabled();
        ImGui::Spacing();
        FieldLabel("Comment");
        InputMultiline("##comment", m_comment, ImVec2(-FLT_MIN, Px(54.0f)));
        EndSection();
    }

    void RecordEditor::Draw()
    {
        if (!m_open)
            return;
        Table::MemoryRecord* record = m_engine.Table().FindById(m_recordId);
        if (!record)
        {
            m_open = false;
            return;
        }
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(m_title.c_str(), &m_open, ImVec2(560.0f, 720.0f), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(480.0f, 420.0f)))
        {
            EndToolWindow();
            return;
        }

        const float footer = ImGui::GetFrameHeight() + Px(18.0f);
        ImGui::BeginChild("##editorbody", ImVec2(0.0f, -footer), ImGuiChildFlags_None);
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
        ImGui::TextUnformatted(KindText(*record));
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        InputTextWithHint("##description", "Description", m_description);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));

        AddressSection(*record);
        if (record->Kind == Table::RecordKind::Value)
            ValueSection();
        if (record->Kind == Table::RecordKind::Value || record->Kind == Table::RecordKind::Group)
            DropDownSection(*record);
        if (record->Kind == Table::RecordKind::Script)
        {
            BeginSection("##script", ICON_FA_CODE, "Script");
            const std::size_t lines = static_cast<std::size_t>(std::count(record->Script.begin(), record->Script.end(), '\n')) + (record->Script.empty() ? 0 : 1);
            Muted(std::to_string(lines) + (lines == 1 ? " line" : " lines") + (record->Active ? "  ·  enabled" : ""));
            if (ImGui::Button(ICON_FA_CODE "  Edit script"))
                m_workspace.OpenScript(record->Script, record->Id);
            ImGui::Spacing();
            ToggleRow("async", "Marked as async", "Kept for Cheat Engine tables. This program switches every script in the background anyway.", &m_async);
            EndSection();
        }
        ChildrenSection(*record);
        HotkeySection();
        LookSection();
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(4.0f));
        if (AccentButton("OK", Px(96.0f, 0.0f)))
        {
            Apply(*record);
            m_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply", Px(96.0f, 0.0f)))
            Apply(*record);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(96.0f, 0.0f)))
            m_open = false;
        EndToolWindow();
    }
}
