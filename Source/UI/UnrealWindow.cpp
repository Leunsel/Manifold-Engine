#include "UI/UnrealWindow.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <cfloat>

namespace Manifold::UI
{
    namespace
    {
        constexpr const char* kSearchGroup = "unrealsearch";
        constexpr std::size_t kNone = static_cast<std::size_t>(-1);
        constexpr std::size_t kInstanceLimit = 500;
        constexpr std::size_t kHitLimit = 2000;

        const char* KindBadge(Target::UnrealKind kind)
        {
            switch (kind)
            {
            case Target::UnrealKind::Struct: return "struct";
            case Target::UnrealKind::Enum: return "enum";
            default: return "";
            }
        }
    }

    void UnrealWindow::ShowType(const std::string& name)
    {
        m_pendingType = name;
        Show();
    }

    void UnrealWindow::ShowAddress(std::uint64_t address)
    {
        m_whoisText = Hex(address);
        m_whoisResult.clear();
        m_whoisObject = 0;
        if (m_runtime)
        {
            m_whoisResult = m_runtime->Whois(address).value_or("No Unreal object contains " + Hex(address) + ".");
            if (const auto owner = m_runtime->OwnerOf(address))
                m_whoisObject = owner->first;
        }
        Show();
    }

    void UnrealWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_CUBES "  Unreal Engine###unreal", ImVec2(1040.0f, 660.0f), ImGuiWindowFlags_MenuBar, ImVec2(660.0f, 360.0f)))
            return;
        Engine& engine = m_workspace.Core();
        MenuBar();
        if (!engine.HasProcess())
        {
            m_runtime.reset();
            m_workspace.NoProcessState(ICON_FA_CUBES, "Open an Unreal Engine game to see its classes, structs and properties.");
            EndToolWindow();
            return;
        }
        UnrealSession& session = engine.Unreal();
        const auto runtime = session.Runtime();
        if (runtime != m_runtime)
        {
            m_runtime = runtime;
            m_view = TypeView{};
            m_appliedFilter = "\x01";
            m_appliedKind = -1;
            m_selected = kNone;
            m_instances.clear();
            m_instancesOf = 0;
            m_hits.clear();
            m_index.reset();
            m_whoisResult.clear();
            m_whoisObject = 0;
            ++m_whoisSerial;
        }
        if (const std::shared_ptr<const Target::UnrealTypeIndex> index = runtime ? runtime->Index() : nullptr; index != m_index)
        {
            const std::uint64_t selected = m_index && m_selected < m_index->List.size() ? m_index->List[m_selected].Object : 0;
            m_index = index;
            m_selected = kNone;
            for (std::size_t i = 0; index && selected && i < index->List.size(); ++i)
            {
                if (index->List[i].Object == selected)
                {
                    m_selected = i;
                    break;
                }
            }
            m_appliedFilter = "\x01";
            m_appliedKind = -1;
            m_hits.clear();
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(runtime ? ManifoldTheme::Success : session.State() == UnrealState::Failed ? ManifoldTheme::Warning : ManifoldTheme::Muted, ICON_FA_CIRCLE);
        ImGui::SameLine();
        TextFitted(session.StatusText(), ImGui::GetContentRegionAvail().x, ManifoldTheme::Label);

        if (!runtime)
        {
            if (session.Resolving())
            {
                ImGui::Spacing();
                Spinner(Px(14.0f), ManifoldTheme::Accent);
                ImGui::SameLine();
                Muted("Reading the game's name table, objects and reflection...");
            }
            else
            {
                const char* title = "Unreal Engine is not resolved";
                std::string text = "Start reads the game's name table, object array and reflection layout. It starts by itself when an Unreal game is opened.";
                if (session.State() == UnrealState::NotUnreal)
                {
                    title = "This does not look like an Unreal Engine game";
                    text = "No CoreUObject module or package was found in it. Start tries anyway.";
                }
                else if (session.State() == UnrealState::Failed)
                {
                    title = "The game did not resolve";
                    text = session.Failure() + "\n\nFor a game the search does not find, manifold.unreal.configure takes the name table and object array addresses, layout offsets or search patterns.";
                }
                else if (session.Stopped())
                {
                    title = "The Unreal tool is shut down";
                    text = "Nothing is read until it is started again.";
                }
                if (EmptyState(ICON_FA_CUBES, title, text, { { .Label = ICON_FA_PLAY "  Start", .Accent = true } }, false) == 0)
                    session.Start(false);
            }
            EndToolWindow();
            return;
        }

        Header(*runtime);
        Whois(*runtime);
        ImGui::Spacing();

        const float gap = Px(8.0f);
        const float minList = Px(240.0f);
        const float minDetail = Px(380.0f);
        const float total = ImGui::GetContentRegionAvail().x;
        m_listWidth = std::clamp(m_listWidth <= 0.0f ? Px(300.0f) : m_listWidth, minList, std::max(minList, total - minDetail - gap));
        const float height = ImGui::GetContentRegionAvail().y;
        const ImVec2 start = ImGui::GetCursorPos();
        ImGui::BeginChild("##unrealleft", ImVec2(m_listWidth, height), ImGuiChildFlags_Borders);
        TypeList(*runtime, height);
        ImGui::EndChild();
        float left = m_listWidth;
        float right = total - left - gap;
        ImGui::SetCursorPos(start);
        if (Splitter("##unrealsplit", true, gap, &left, &right, minList, minDetail, height))
            m_listWidth = left;
        ImGui::SetCursorPos(ImVec2(start.x + m_listWidth + gap, start.y));
        ImGui::BeginChild("##unrealright", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
        Details(*runtime);
        ImGui::EndChild();
        EndToolWindow();
    }

    void UnrealWindow::Dissect(std::uint64_t address, const char* what)
    {
        if (!address)
        {
            m_workspace.Core().Notify(LogLevel::Info, std::string(what) + " was not found. Is a level loaded?");
            return;
        }
        m_workspace.OpenStructure(address);
    }

    void UnrealWindow::Header(const Target::UnrealRuntime& runtime)
    {
        const GhostButtons ghost;
        ImGui::AlignTextToFramePadding();
        Muted("Dissect");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COGS "  Engine"))
            Dissect(runtime.Engine(), "The engine object");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_GLOBE "  World"))
            Dissect(runtime.World(), "The world");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_GAMEPAD "  Player Controller"))
            Dissect(runtime.Controller(), "The local player controller");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_USER "  Pawn"))
            Dissect(runtime.Pawn(), "The local pawn");
        ItemTooltip("Opens Structure Dissect on the object, with its class's properties.");
    }

    void UnrealWindow::Whois(const Target::UnrealRuntime& runtime)
    {
        const float button = ImGui::CalcTextSize(ICON_FA_SEARCH "  Who Owns It").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(Px(260.0f));
        const bool enter = InputTextWithHint("##unrealwhois", "Address or expression", m_whoisText, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button(ICON_FA_SEARCH "  Who Owns It", ImVec2(button, 0.0f)) || enter) && !Trim(m_whoisText).empty())
        {
            (void)runtime;
            Status error;
            m_whoisObject = 0;
            const std::uint64_t serial = ++m_whoisSerial;
            if (const auto address = m_workspace.Core().Evaluate(m_whoisText, &error))
            {
                m_whoisResult = "Looking through the objects...";
                m_workspace.Core().Work().Start({ .Title = "Finding the owner of an address", .Group = "unrealwhois", .Cancellable = true, .Quiet = true },
                    [this, found = m_runtime, where = *address, serial](WorkProgress&) -> BackgroundWork::Finish
                    {
                        std::string text = found->Whois(where).value_or("No Unreal object contains " + Hex(where) + ".");
                        const auto owner = found->OwnerOf(where);
                        return [this, found, serial, text = std::move(text), object = owner ? owner->first : 0]()
                            {
                                if (serial != m_whoisSerial || found != m_runtime)
                                    return;
                                m_whoisResult = text;
                                m_whoisObject = object;
                            };
                    });
            }
            else
            {
                m_whoisResult = error.Text;
            }
        }
        ItemTooltip("The object an address belongs to, its class and the property path down to the field.");
        if (!m_whoisResult.empty())
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            PushMono();
            TextFitted(m_whoisResult, ImGui::GetContentRegionAvail().x - (m_whoisObject ? Px(90.0f) : 0.0f), ManifoldTheme::Text);
            PopMono();
            if (m_whoisObject)
            {
                ImGui::SameLine();
                const GhostButtons ghost;
                if (ImGui::SmallButton(ICON_FA_SITEMAP "  Dissect"))
                    Dissect(m_whoisObject, "The owner");
            }
        }
    }

    void UnrealWindow::RefreshFilter(const Target::UnrealRuntime& runtime)
    {
        if (m_filter == m_appliedFilter && m_kind == m_appliedKind)
            return;
        m_appliedFilter = m_filter;
        m_appliedKind = m_kind;
        m_visible.clear();
        const std::string filter = Trim(m_filter);
        if (filter.size() > 2 && filter.front() == '.')
        {
            SearchProperties(m_runtime);
            return;
        }
        m_workspace.Core().Work().CancelGroup(kSearchGroup);
        m_searching = false;
        m_hits.clear();
        (void)runtime;
        const auto& types = m_index->List;
        for (std::size_t i = 0; i < types.size(); ++i)
        {
            const Target::UnrealKind kind = types[i].Kind;
            if ((m_kind == 1 && kind != Target::UnrealKind::Class) || (m_kind == 2 && kind != Target::UnrealKind::Struct) || (m_kind == 3 && kind != Target::UnrealKind::Enum))
                continue;
            if (!filter.empty() && !ContainsNoCase(types[i].Name, filter))
                continue;
            m_visible.push_back(i);
        }
    }

    void UnrealWindow::SearchProperties(const std::shared_ptr<const Target::UnrealRuntime>& runtime)
    {
        BackgroundWork& work = m_workspace.Core().Work();
        work.CancelGroup(kSearchGroup);
        work.CompleteGroup(kSearchGroup);
        m_hits.clear();
        m_searching = true;
        const std::string needle = Trim(m_filter).substr(1);
        const std::string filter = m_filter;
        work.Start({ .Title = "Searching the Unreal properties", .Group = kSearchGroup, .Cancellable = true, .Quiet = true },
            [this, runtime, needle, filter, index = m_index](WorkProgress& progress) -> BackgroundWork::Finish
            {
                std::vector<Hit> hits;
                const auto& types = index->List;
                for (std::size_t i = 0; i < types.size() && hits.size() < kHitLimit && !progress.Cancelled(); ++i)
                {
                    if (types[i].Kind == Target::UnrealKind::Enum)
                        continue;
                    for (const Target::UnrealField& field : runtime->Fields(types[i].Object, true))
                    {
                        if (ContainsNoCase(field.Name, needle))
                            hits.push_back({ types[i].Name + "." + field.Name + "  " + runtime->TypeText(field), i });
                    }
                }
                return [this, hits = std::move(hits), filter, runtime, index]()
                    {
                        if (filter != m_filter || runtime != m_runtime || index != m_index)
                            return;
                        m_hits = hits;
                        m_searching = false;
                    };
            });
    }

    void UnrealWindow::TypeList(const Target::UnrealRuntime& runtime, float height)
    {
        (void)height;
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputTextWithHint("##unrealfilter", ICON_FA_SEARCH "  Type name, or .name to search properties", m_filter);
        Segmented("##unrealkind", &m_kind, { "All", "Classes", "Structs", "Enums" }, ImGui::GetContentRegionAvail().x);
        RefreshFilter(runtime);
        if (!m_pendingType.empty() && !m_refreshing)
        {
            const auto& types = m_index->List;
            bool found = false;
            for (std::size_t i = 0; i < types.size() && !found; ++i)
            {
                if (EqualsNoCase(types[i].Name, m_pendingType))
                {
                    m_selected = i;
                    m_tab = types[i].Kind == Target::UnrealKind::Enum ? Tab::Entries : Tab::Properties;
                    found = true;
                }
            }
            if (found || m_pendingRead)
            {
                m_pendingType.clear();
                m_pendingRead = false;
            }
            else
            {
                m_pendingRead = true;
                ReadTypesAgain(false);
            }
        }
        const bool properties = Trim(m_filter).size() > 2 && Trim(m_filter).front() == '.';
        ImGui::AlignTextToFramePadding();
        if (properties)
            Muted(m_searching ? std::string("Searching...") : Format("%zu properties%s", m_hits.size(), m_hits.size() >= kHitLimit ? " (first ones)" : ""));
        else
            Muted(m_refreshing ? std::string("Reading the types again...") : Format("%zu of %zu types", m_visible.size(), m_index->List.size()));
        ImGui::BeginChild("##unrealtypes", ImVec2(0.0f, 0.0f));
        const auto& types = m_index->List;
        const auto row = [&](std::size_t type, const std::string& label, int index)
            {
                const bool selected = type == m_selected;
                const char* badge = KindBadge(types[type].Kind);
                const float badgeWidth = *badge ? ImGui::CalcTextSize(badge).x : 0.0f;
                const float left = ImGui::GetCursorPosX();
                const float width = ImGui::GetContentRegionAvail().x;
                if (ImGui::Selectable(("##" + std::to_string(index)).c_str(), selected))
                {
                    m_selected = type;
                    m_tab = types[type].Kind == Target::UnrealKind::Enum ? Tab::Entries : Tab::Properties;
                }
                ImGui::SameLine(left);
                TextFitted(label, width - (*badge ? badgeWidth + Px(14.0f) : Px(4.0f)), ManifoldTheme::Text);
                if (*badge)
                {
                    ImGui::SameLine(left + width - badgeWidth - Px(4.0f));
                    Muted(badge);
                }
            };
        if (properties)
            ClippedRows(static_cast<int>(m_hits.size()), [&](int index) { row(m_hits[static_cast<std::size_t>(index)].Type, m_hits[static_cast<std::size_t>(index)].Text, index); });
        else
            ClippedRows(static_cast<int>(m_visible.size()), [&](int index) { row(m_visible[static_cast<std::size_t>(index)], types[m_visible[static_cast<std::size_t>(index)]].Name, index); });
        ImGui::EndChild();
    }

    void UnrealWindow::ReadTypesAgain(bool report)
    {
        if (m_refreshing || !m_runtime)
            return;
        m_refreshing = true;
        m_workspace.Core().Work().Start({ .Title = "Reading the Unreal types again", .Group = "unrealtypes", .Cancellable = false, .Quiet = true },
            [this, runtime = m_runtime, report](WorkProgress&) -> BackgroundWork::Finish
            {
                const bool changed = runtime->RefreshTypes();
                return [this, runtime, report, changed]()
                    {
                        m_refreshing = false;
                        if (!report || runtime != m_runtime)
                            return;
                        m_workspace.Core().Notify(LogLevel::Info, changed ? Format("The game has %zu types now", runtime->Index()->List.size()) : std::string("The types of the game have not changed"));
                    };
            });
    }

    void UnrealWindow::Details(const Target::UnrealRuntime& runtime)
    {
        const auto& types = m_index->List;
        if (m_selected >= types.size())
        {
            EmptyState(ICON_FA_CUBES, "Pick a type", "Classes, structs and enums of the game are on the left. Type a name to filter them, or .name to find a property in all of them.", {}, false);
            return;
        }
        const Target::UnrealType& entry = types[m_selected];
        const std::uint64_t type = entry.Object;
        const bool isEnum = entry.Kind == Target::UnrealKind::Enum;
        if (m_view.Runtime != &runtime || m_view.Type != type)
        {
            m_view = TypeView{};
            m_view.Runtime = &runtime;
            m_view.Type = type;
            m_view.Super = isEnum ? 0 : runtime.SuperOf(type);
            m_view.Name = runtime.CppName(type);
            m_view.SuperName = m_view.Super ? runtime.CppName(m_view.Super) : std::string();
            m_view.Size = isEnum ? std::nullopt : runtime.StructSize(type);
            m_view.Where = runtime.FullName(type);
        }
        const std::uint64_t super = m_view.Super;
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        ImGui::TextUnformatted(m_view.Name.c_str());
        ImGui::PopFont();
        if (super)
        {
            ImGui::SameLine();
            Muted(":");
            ImGui::SameLine();
            if (ImGui::SmallButton((m_view.SuperName + "##super").c_str()))
                m_pendingType = runtime.NameOf(super).value_or("");
            ItemTooltip("Shows the parent type");
        }
        ImGui::SameLine();
        const std::optional<int> size = m_view.Size;
        const std::string& where = m_view.Where;
        if (isEnum)
            Muted(Format("enum  ·  %s", where.c_str()));
        else if (size)
            Muted(Format("size 0x%X  ·  %s", static_cast<unsigned>(*size), where.c_str()));
        else
            Muted(where);
        {
            const GhostButtons ghost;
            if (entry.Kind == Target::UnrealKind::Class)
            {
                if (ImGui::Button(ICON_FA_SITEMAP "  Dissect an Instance"))
                {
                    m_workspace.Core().Work().Start({ .Title = "Finding an instance of " + entry.Name, .Group = "unrealinstance", .Cancellable = true, .Quiet = true },
                        [this, found = m_runtime, type, name = entry.Name](WorkProgress&) -> BackgroundWork::Finish
                        {
                            const std::vector<std::uint64_t> objects = found->Instances(type, 1);
                            return [this, found, name, object = objects.empty() ? 0 : objects.front()]()
                                {
                                    if (found == m_runtime)
                                        Dissect(object, ("A live " + name).c_str());
                                };
                        });
                }
                ImGui::SameLine();
            }
            if (entry.Kind != Target::UnrealKind::Enum && ImGui::Button(ICON_FA_COPY "  Copy as C++"))
            {
                ImGui::SetClipboardText(Dump(runtime, type).c_str());
                m_workspace.Core().Notify(LogLevel::Info, "Copied " + runtime.CppName(type) + " as C++-like lines");
            }
        }
        std::vector<const char*> tabs;
        std::vector<Tab> ids;
        if (entry.Kind == Target::UnrealKind::Enum)
        {
            tabs = { "Entries" };
            ids = { Tab::Entries };
        }
        else if (entry.Kind == Target::UnrealKind::Class)
        {
            tabs = { "Properties", "Functions", "Instances" };
            ids = { Tab::Properties, Tab::Functions, Tab::Instances };
        }
        else
        {
            tabs = { "Properties" };
            ids = { Tab::Properties };
        }
        int current = 0;
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == m_tab)
                current = static_cast<int>(i);
        }
        if (tabs.size() > 1 && Segmented("##unrealtab", &current, tabs, Px(320.0f)))
            m_tab = ids[static_cast<std::size_t>(current)];
        m_tab = ids[static_cast<std::size_t>(std::clamp(current, 0, static_cast<int>(ids.size()) - 1))];
        ImGui::Spacing();
        switch (m_tab)
        {
        case Tab::Functions: Functions(runtime, type); break;
        case Tab::Instances: Instances(runtime, type); break;
        case Tab::Entries: Entries(runtime, type); break;
        default: Properties(runtime, type); break;
        }
    }

    void UnrealWindow::FieldMenu(const Target::UnrealRuntime& runtime, const Target::UnrealField& field)
    {
        const std::string symbol = Target::UnrealRuntime::OffsetSymbol(field.Owner, field.Name);
        ImGui::TextDisabled("%s", symbol.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Offset Symbol", symbol.c_str()))
            ImGui::SetClipboardText(symbol.c_str());
        ItemTooltip("The symbol resolves to the offset in every session, so address list entries written with it survive game updates.");
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Offset", Hex(static_cast<std::uint64_t>(field.Start())).c_str()))
            ImGui::SetClipboardText(Hex(static_cast<std::uint64_t>(field.Start())).c_str());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Name"))
            ImGui::SetClipboardText(field.Name.c_str());
        if (field.Inner)
        {
            ImGui::Separator();
            const std::string inner = runtime.NameOf(field.Inner).value_or("");
            if (!inner.empty() && ImGui::MenuItem((ICON_FA_ARROW_RIGHT "  Show " + runtime.CppName(field.Inner)).c_str()))
                m_pendingType = inner;
        }
    }

    void UnrealWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        Engine& engine = m_workspace.Core();
        UnrealSession& session = engine.Unreal();
        if (ImGui::BeginMenu("Unreal"))
        {
            if (session.Resolving())
                ImGui::MenuItem(ICON_FA_SPINNER "  Reading the Reflection...", nullptr, false, false);
            else if (ImGui::MenuItem(session.Runtime() ? ICON_FA_SYNC "  Resolve Again" : ICON_FA_PLAY "  Start", nullptr, false, engine.HasProcess()))
                session.Start(false);
            ItemTooltip("Reads the game's name table, object array and reflection layout, also in a game that does not look like Unreal.");
            if (ImGui::MenuItem(ICON_FA_LIST "  Read the Types Again", nullptr, false, m_runtime && m_index && !m_refreshing))
                ReadTypesAgain(true);
            ItemTooltip("Classes a game loads later, such as the Blueprints of a level, are listed after this.");
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_POWER_OFF "  Shut Down", nullptr, false, !session.Stopped()))
                session.Stop();
            ItemTooltip("Drops what was resolved; dissects, symbols and types stop until Start, also in the next game.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem(ICON_FA_FILTER "  Properties Declared Here Only", nullptr, &m_ownOnly);
            ItemTooltip("Properties: leave out the ones the type inherits from its parents.");
            ImGui::MenuItem(ICON_FA_LAYER_GROUP "  Functions of the Parents Too", nullptr, &m_inherited);
            ItemTooltip("Functions: also list the ones the parent types declare.");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void UnrealWindow::Properties(const Target::UnrealRuntime& runtime, std::uint64_t type)
    {
        if (m_view.Runtime != &runtime || m_view.Type != type || !m_view.HasFields || m_view.OwnOnly != m_ownOnly)
        {
            m_view.Fields = runtime.Fields(type, m_ownOnly);
            m_view.OwnOnly = m_ownOnly;
            m_view.HasFields = true;
        }
        const std::vector<Target::UnrealField>& fields = m_view.Fields;
        if (fields.empty())
        {
            Muted("No reflected properties.");
            return;
        }
        if (!BeginDataTable("##unrealfields", { { "Offset", 1.0f, 70.0f }, { "Name", 1.6f }, { "Type", 1.6f }, { "Declared in", 1.0f } }))
            return;
        ClippedRows(static_cast<int>(fields.size()), [&](int index)
            {
                const Target::UnrealField& f = fields[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                PushMono();
                const std::string offset = Format("%04X", static_cast<unsigned>(f.Start()));
                ImGui::Selectable((offset + "##row").c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                PopMono();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && f.Inner)
                    m_pendingType = runtime.NameOf(f.Inner).value_or("");
                if (ImGui::BeginPopupContextItem("##fieldmenu"))
                {
                    FieldMenu(runtime, f);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                std::string name = f.Name + (f.Dim > 1 ? "[" + std::to_string(f.Dim) + "]" : std::string());
                if (f.MaskedBit())
                    name += Format("  (mask %02X)", f.Mask);
                ImGui::TextUnformatted(name.c_str());
                ImGui::TableNextColumn();
                PushMono();
                ImGui::TextColored(f.Inner ? ManifoldTheme::Accent : ManifoldTheme::Text, "%s", runtime.TypeText(f).c_str());
                PopMono();
                ImGui::TableNextColumn();
                Muted(f.Owner);
            });
        ImGui::EndTable();
    }

    void UnrealWindow::Functions(const Target::UnrealRuntime& runtime, std::uint64_t type)
    {
        if (m_view.Runtime != &runtime || m_view.Type != type || !m_view.HasFunctions || m_view.Inherited != m_inherited)
        {
            m_view.Functions = runtime.Functions(type, m_inherited);
            m_view.Inherited = m_inherited;
            m_view.HasFunctions = true;
        }
        const std::vector<Target::UnrealFunction>& functions = m_view.Functions;
        if (functions.empty())
        {
            Muted(runtime.Layout().Children < 0 ? "The function list layout was not found in this game." : "No functions.");
            return;
        }
        if (!BeginDataTable("##unrealfunctions", { { "Name", 1.2f }, { "Parameters", 2.4f }, { "Flags", 1.0f, 86.0f }, { "Declared in", 1.0f } }))
            return;
        ClippedRows(static_cast<int>(functions.size()), [&](int index)
            {
                const Target::UnrealFunction& f = functions[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Selectable((f.Name + "##fn").c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::BeginPopupContextItem("##fnmenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Name"))
                        ImGui::SetClipboardText(f.Name.c_str());
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the UFunction Address"))
                        ImGui::SetClipboardText(Hex(f.Address).c_str());
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                std::vector<std::string> parameters;
                for (const Target::UnrealField& p : f.Parameters)
                    parameters.push_back(runtime.TypeText(p) + " " + p.Name);
                PushMono();
                TextFitted("(" + Join(parameters, ", ") + ")", ImGui::GetContentRegionAvail().x, ManifoldTheme::Text);
                PopMono();
                ImGui::TableNextColumn();
                PushMono();
                Muted(Format("%08X", f.Flags));
                PopMono();
                ImGui::TableNextColumn();
                Muted(f.Owner);
            });
        ImGui::EndTable();
    }

    void UnrealWindow::FindInstances(const std::shared_ptr<const Target::UnrealRuntime>& runtime, std::uint64_t type)
    {
        m_findingInstances = true;
        m_instancesOf = type;
        m_instances.clear();
        m_workspace.Core().Work().Start({ .Title = "Finding the instances", .Group = "unrealinstances", .Cancellable = true, .Quiet = true },
            [this, runtime, type](WorkProgress&) -> BackgroundWork::Finish
            {
                std::vector<std::pair<std::uint64_t, std::string>> found;
                for (const std::uint64_t object : runtime->Instances(type, kInstanceLimit))
                    found.push_back({ object, runtime->FullName(object) });
                return [this, runtime, type, found = std::move(found)]()
                    {
                        if (runtime != m_runtime || type != m_instancesOf)
                            return;
                        m_instances = found;
                        m_findingInstances = false;
                    };
            });
    }

    void UnrealWindow::Instances(const Target::UnrealRuntime& runtime, std::uint64_t type)
    {
        (void)runtime;
        if (m_instancesOf != type)
            FindInstances(m_runtime, type);
        {
            const GhostButtons ghost;
            ImGui::BeginDisabled(m_findingInstances);
            if (ImGui::Button(ICON_FA_SYNC "  Find Again"))
                FindInstances(m_runtime, type);
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (m_findingInstances)
        {
            Spinner(Px(12.0f), ManifoldTheme::Accent);
            ImGui::SameLine();
            Muted("Looking through the object array...");
            return;
        }
        Muted(m_instances.empty() ? std::string("No live instance, class default objects left out") :
                                    Format("%zu live %s%s", m_instances.size(), m_instances.size() == 1 ? "instance" : "instances", m_instances.size() >= kInstanceLimit ? " (the first ones)" : ""));
        if (m_instances.empty() || !BeginDataTable("##unrealinstances", { { "Address", 1.0f, 130.0f }, { "Full name", 3.0f } }))
            return;
        ClippedRows(static_cast<int>(m_instances.size()), [&](int index)
            {
                const auto& [object, name] = m_instances[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                PushMono();
                ImGui::Selectable((Hex(object) + "##instance").c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                PopMono();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    Dissect(object, "The object");
                ItemTooltip("Double click: Structure Dissect");
                if (ImGui::BeginPopupContextItem("##instancemenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_SITEMAP "  Dissect"))
                        Dissect(object, "The object");
                    if (ImGui::MenuItem(ICON_FA_TH "  Show in the Hex View"))
                        m_workspace.RevealInMemoryViewer(object, 8, false, true);
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Address"))
                        ImGui::SetClipboardText(Hex(object).c_str());
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Full Name"))
                        ImGui::SetClipboardText(name.c_str());
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name.c_str());
            });
        ImGui::EndTable();
    }

    void UnrealWindow::Entries(const Target::UnrealRuntime& runtime, std::uint64_t type)
    {
        const auto entries = runtime.EnumEntries(type);
        if (entries.empty())
        {
            Muted("No entries were found.");
            return;
        }
        if (!BeginDataTable("##unrealentries", { { "Name", 2.0f }, { "Value", 1.0f, 120.0f } }))
            return;
        ClippedRows(static_cast<int>(entries.size()), [&](int index)
            {
                const Target::UnrealEnumEntry& entry = entries[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.Name.c_str());
                ImGui::TableNextColumn();
                PushMono();
                ImGui::Text("%lld", static_cast<long long>(entry.Value));
                PopMono();
            });
        ImGui::EndTable();
    }

    std::string UnrealWindow::Dump(const Target::UnrealRuntime& runtime, std::uint64_t type) const
    {
        const std::uint64_t super = runtime.SuperOf(type);
        std::string text = Format("%s%s  // size 0x%X\n", runtime.CppName(type).c_str(), super ? (" : " + runtime.CppName(super)).c_str() : "",
            static_cast<unsigned>(runtime.StructSize(type).value_or(0)));
        for (const Target::UnrealField& f : runtime.Fields(type))
        {
            const std::string mask = f.MaskedBit() ? Format(" : 1 (mask %02X)", f.Mask) : std::string();
            text += Format("  +%04X  %-32s %s%s%s  (%s)\n", static_cast<unsigned>(f.Start()), runtime.TypeText(f).c_str(), f.Name.c_str(),
                f.Dim > 1 ? ("[" + std::to_string(f.Dim) + "]").c_str() : "", mask.c_str(), f.Owner.c_str());
        }
        return text;
    }
}
