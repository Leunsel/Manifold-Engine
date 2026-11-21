#include "UI/AddressActions.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui.h"

namespace Manifold::UI
{
    namespace
    {
        constexpr double kReevaluateSeconds = 0.25;
    }

    std::string AddressText(Engine& engine, std::uint64_t address)
    {
        if (engine.HasProcess())
        {
            if (const auto module = engine.Process()->ModuleAt(address))
                return module->Name + "+" + Hex(address - module->Base);
        }
        return Hex(address);
    }

    bool AddressMenuItems(Workspace& workspace, std::uint64_t address, unsigned actions, int size)
    {
        Engine& engine = workspace.Core();
        bool clicked = false;
        const auto item = [&](unsigned action, const char* label)
            {
                if (!(actions & action) || !ImGui::MenuItem(label))
                    return false;
                clicked = true;
                return true;
            };
        if (item(ShowCode, ICON_FA_CODE "  Show in disassembler"))
            workspace.OpenMemoryViewer(address, false);
        if (item(ShowHex, ICON_FA_MEMORY "  Show in hex view"))
            workspace.OpenMemoryViewer(address, true);
        if (item(DissectHere, ICON_FA_SITEMAP "  Dissect data at this address"))
            workspace.OpenStructure(address);
        if (item(DissectHere, ICON_FA_CLONE "  Dissect in a new window"))
            workspace.OpenStructure(address, {}, true);
        if (item(DissectHere, ICON_FA_THUMBTACK "  Find static pointers to this address"))
            workspace.FindStaticPointers(address);
        if (item(DissectHere, ICON_FA_ROUTE "  Pointer scan for this address"))
            workspace.OpenPointerScan(address);
        if ((actions & (WatchAccesses | WatchWrites)) && (actions & (ShowCode | ShowHex | DissectHere)))
            ImGui::Separator();
        if (item(WatchAccesses, ICON_FA_EYE "  Find out what accesses this address"))
            workspace.StartWatch(Debug::WatchKind::Accesses, address, size);
        if (item(WatchWrites, ICON_FA_PEN "  Find out what writes to this address"))
            workspace.StartWatch(Debug::WatchKind::Writes, address, size);
        if (item(AddToList, ICON_FA_PLUS "  Add to the address list"))
            workspace.AddAddress(address, size == 8 ? Scan::ValueType::EightBytes : size == 2 ? Scan::ValueType::TwoBytes : size == 1 ? Scan::ValueType::Byte : Scan::ValueType::FourBytes);
        if ((actions & (CopyAddress | CopyModuleOffset)) && (actions & ~(CopyAddress | CopyModuleOffset)))
            ImGui::Separator();
        if (item(CopyAddress, ICON_FA_COPY "  Copy address"))
            ImGui::SetClipboardText(Hex(address).c_str());
        if ((actions & CopyModuleOffset) && engine.HasProcess() && engine.Process()->ModuleAt(address))
        {
            if (item(CopyModuleOffset, ICON_FA_COPY "  Copy as module + offset"))
                ImGui::SetClipboardText(engine.Symbols().ModuleOffset(address).c_str());
        }
        return clicked;
    }

    bool AddressCell(Workspace& workspace, std::uint64_t address, bool selected, unsigned actions, bool code, const std::function<void()>& extra)
    {
        Engine& engine = workspace.Core();
        const char* key = reinterpret_cast<const char*>(&address);
        ImGui::PushID(key, key + sizeof(address));
        PushMono();
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
        const bool clicked = ImGui::Selectable(AddressText(engine, address).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
        ImGui::PopStyleColor();
        PopMono();
        if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            workspace.OpenMemoryViewer(address, !code);
        if (ImGui::BeginPopupContextItem("##addressmenu"))
        {
            if (extra)
            {
                extra();
                ImGui::Separator();
            }
            AddressMenuItems(workspace, address, actions);
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return clicked;
    }

    const std::optional<std::uint64_t>& Resolve(Engine& engine, AddressField& field)
    {
        const std::uint64_t generation = engine.Symbols().Generation() * 1000003ull + engine.ProcessGeneration();
        const double now = engine.Now();
        if (field.Text != field.Evaluated || field.Generation != generation || now - field.At > kReevaluateSeconds)
        {
            field.Evaluated = field.Text;
            field.Generation = generation;
            field.At = now;
            field.Error.clear();
            field.Value.reset();
            if (!Trim(field.Text).empty())
            {
                Status error;
                field.Value = engine.Evaluate(field.Text, &error);
                if (!field.Value)
                    field.Error = error.Text;
            }
        }
        return field.Value;
    }

    bool AddressInput(const char* id, Engine& engine, AddressField& field, float width, const char* hint)
    {
        const std::optional<std::uint64_t> before = field.Value;
        ImGui::SetNextItemWidth(width);
        InputTextWithHint(id, hint, field.Text);
        Resolve(engine, field);
        if (!field.Error.empty())
        {
            ImGui::SameLine(0.0f, Px(2.0f));
            ImGui::TextColored(ManifoldTheme::Error, ICON_FA_EXCLAMATION_TRIANGLE);
            ErrorTooltip(field.Error);
        }
        return field.Value != before;
    }
}
