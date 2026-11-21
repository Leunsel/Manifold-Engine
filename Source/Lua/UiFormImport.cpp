#include "Lua/UiFormImport.h"

#include "Core/Text.h"

#include <algorithm>
#include <initializer_list>
#include <set>

namespace Manifold::Lua
{
    namespace
    {
        using Table::CeComponent;
        using Table::CeValue;

        constexpr int kMaxDepth = 32;

        std::string Kind(const CeComponent& component)
        {
            std::string name = Lower(component.Class);
            if (name.rfind("tce", 0) == 0)
                name.erase(0, 3);
            else if (!name.empty() && name[0] == 't')
                name.erase(0, 1);
            return name;
        }

        std::string Who(const CeComponent& component)
        {
            return component.Class + " " + component.Name;
        }

        struct Box
        {
            const CeComponent* Item = nullptr;
            int Left = 0;
            int Top = 0;
            int Width = 0;
            int Height = 0;
            std::string Align;
        };

        Box BoxOf(const CeComponent& component)
        {
            Box box;
            box.Item = &component;
            box.Left = static_cast<int>(component.Integer("Left"));
            box.Top = static_cast<int>(component.Integer("Top"));
            box.Width = static_cast<int>(component.Integer("Width"));
            box.Height = static_cast<int>(component.Integer("Height"));
            box.Align = Lower(component.Text("Align", "alNone"));
            return box;
        }

        bool IsField(const std::string& kind)
        {
            static const std::set<std::string> kFields = { "edit", "labelededit", "maskedit", "combobox", "checkbox", "togglebox", "trackbar", "spinedit",
                "floatspinedit", "memo", "progressbar", "button", "bitbtn", "speedbutton", "radiogroup" };
            return kFields.count(kind) != 0;
        }

        bool IsLabel(const std::string& kind)
        {
            return kind == "label" || kind == "statictext";
        }

        struct Converter
        {
            FormImport& Out;
            FormDocument& Form;
            std::set<std::string> Used;

            void Note(std::string text)
            {
                if (std::find(Out.Notes.begin(), Out.Notes.end(), text) == Out.Notes.end())
                    Out.Notes.push_back(std::move(text));
            }

            int Make(UiKind kind, int parent, const CeComponent* from)
            {
                const UiNode* holder = Form.Model.Find(parent);
                if (!holder || !FormCanHold(holder->Kind, kind))
                    return 0;
                const int id = Form.Model.Create(kind, parent, {});
                if (from)
                {
                    ++Out.Converted;
                    if (FormIsIdentifier(from->Name))
                        Form.Meta[id].Name = FormUniqueName(Form, from->Name, id);
                    UiNode& node = *Form.Model.Find(id);
                    node.Visible = from->Flag("Visible", true);
                    node.Enabled = from->Flag("Enabled", true);
                    node.Tooltip = from->Text("Hint");
                }
                return id;
            }

            void Handler(int id, std::string_view key, const CeComponent& from, std::initializer_list<const char*> events)
            {
                for (const char* event : events)
                {
                    const CeValue* value = from.Find(event);
                    if (!value || value->Text.empty() || (value->Type != CeValue::Kind::Ident && value->Type != CeValue::Kind::String))
                        continue;
                    Used.insert(from.Name + "." + Lower(event));
                    if (!FormIsIdentifier(value->Text))
                    {
                        Note(Who(from) + ": " + event + " = " + value->Text + " is no plain function name and was left out.");
                        continue;
                    }
                    auto& handlers = Form.Meta[id].Handlers;
                    if (!handlers.count(std::string(key)))
                        handlers[std::string(key)] = value->Text;
                    else
                        Note(Who(from) + ": " + event + " (" + value->Text + ") was dropped, " + std::string(key) + " is " + handlers[std::string(key)] + ".");
                }
            }

            void Leftovers(const CeComponent& from)
            {
                for (const auto& [name, value] : from.Properties)
                {
                    if (name.size() < 3 || Lower(name.substr(0, 2)) != "on" || value.Text.empty())
                        continue;
                    if (value.Type != CeValue::Kind::Ident && value.Type != CeValue::Kind::String)
                        continue;
                    if (Used.count(from.Name + "." + Lower(name)))
                        continue;
                    Note(Who(from) + ": " + name + " (" + value.Text + ") has no manifold.ui counterpart and was left out.");
                }
            }

            void Fill(UiNode& node, const Box& box, bool fill)
            {
                if (fill)
                    node.Width = -1.0f;
                else if (box.Width > 0)
                    node.Width = static_cast<float>(box.Width);
            }

            bool Fills(const CeComponent& from, const Box& box)
            {
                if (box.Align == "altop" || box.Align == "albottom" || box.Align == "alclient")
                    return true;
                return from.HasName("Anchors", "akLeft") && from.HasName("Anchors", "akRight");
            }

            int Control(int parent, const CeComponent& from, int depth, bool inCell)
            {
                const std::string kind = Kind(from);
                const Box box = BoxOf(from);
                const bool fill = Fills(from, box) || inCell;
                int id = 0;
                if (IsLabel(kind))
                {
                    if (!from.Text("OnClick").empty())
                    {
                        id = Make(UiKind::Button, parent, &from);
                        if (id)
                        {
                            Form.Model.Find(id)->Text = from.Text("Caption");
                            Handler(id, "onClick", from, { "OnClick" });
                            Note(Who(from) + ": a clickable label became a button.");
                        }
                    }
                    else
                    {
                        const bool bold = from.HasName("Font.Style", "fsBold");
                        id = Make(bold ? UiKind::Heading : UiKind::Text, parent, &from);
                        if (id)
                        {
                            UiNode& node = *Form.Model.Find(id);
                            node.Text = from.Text("Caption");
                            node.Wrap = !bold && from.Flag("WordWrap", false);
                        }
                    }
                }
                else if (kind == "button" || kind == "bitbtn" || kind == "speedbutton")
                {
                    id = Make(UiKind::Button, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Text = from.Text("Caption");
                        if (fill)
                            node.Width = -1.0f;
                        Handler(id, "onClick", from, { "OnClick" });
                    }
                }
                else if (kind == "edit" || kind == "labelededit" || kind == "maskedit")
                {
                    id = Make(UiKind::Input, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.String = from.Text("Text");
                        node.Hint = from.Text("TextHint");
                        const std::string password = from.Text("PasswordChar");
                        node.Password = !password.empty() && password != std::string(1, '\0') && password != "#0";
                        if (kind == "labelededit")
                            node.Label = from.Text("EditLabel.Caption");
                        Fill(node, box, fill);
                        Handler(id, "onChange", from, { "OnChange" });
                        Handler(id, "onSubmit", from, { "OnEditingDone" });
                    }
                }
                else if (kind == "spinedit" || kind == "floatspinedit")
                {
                    id = Make(UiKind::Number, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        const CeValue* value = from.Find("Value");
                        node.Number = value ? (value->Type == CeValue::Kind::Real ? value->Real : static_cast<double>(value->Integer)) : 0.0;
                        node.Integer = kind == "spinedit";
                        if (from.Find("MinValue"))
                        {
                            node.HasMin = true;
                            node.Min = static_cast<double>(from.Integer("MinValue"));
                        }
                        if (from.Find("MaxValue"))
                        {
                            node.HasMax = true;
                            node.Max = static_cast<double>(from.Integer("MaxValue"));
                        }
                        Fill(node, box, fill);
                        Handler(id, "onChange", from, { "OnChange" });
                    }
                }
                else if (kind == "memo" || kind == "synedit")
                {
                    id = Make(UiKind::Input, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Multiline = true;
                        std::string text;
                        for (const std::string& line : from.Strings("Lines.Strings"))
                            text += (text.empty() ? "" : "\n") + line;
                        node.String = text;
                        node.Height = box.Align == "alclient" ? 0.0f : static_cast<float>(std::max(0, box.Height));
                        Fill(node, box, fill);
                        Handler(id, "onChange", from, { "OnChange" });
                    }
                }
                else if (kind == "checkbox" || kind == "togglebox" || kind == "radiobutton")
                {
                    id = Make(UiKind::Checkbox, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Label = from.Text("Caption");
                        node.Bool = from.Flag("Checked", false) || EqualsNoCase(from.Text("State"), "cbChecked");
                        Handler(id, "onChange", from, { "OnChange", "OnClick" });
                        if (kind == "radiobutton")
                            Note(Who(from) + ": a radio button became a checkbox; a Radio Group fits a set of choices better.");
                    }
                }
                else if (kind == "radiogroup")
                {
                    id = Make(UiKind::Radio, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Label = from.Text("Caption");
                        node.Items = from.Strings("Items.Strings");
                        node.Selected = static_cast<int>(from.Integer("ItemIndex", -1)) + 1;
                        node.Horizontal = from.Integer("Columns", 1) > 1;
                        Handler(id, "onChange", from, { "OnSelectionChanged", "OnClick" });
                    }
                }
                else if (kind == "combobox")
                {
                    id = Make(UiKind::Combo, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Items = from.Strings("Items.Strings");
                        node.Selected = static_cast<int>(from.Integer("ItemIndex", -1)) + 1;
                        Fill(node, box, fill);
                        Handler(id, "onChange", from, { "OnChange", "OnSelect" });
                    }
                }
                else if (kind == "listbox" || kind == "checklistbox")
                {
                    id = Make(UiKind::Table, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Columns = { UiColumn{} };
                        for (const std::string& item : from.Strings("Items.Strings"))
                            node.Rows.push_back({ item });
                        node.Selected = static_cast<int>(from.Integer("ItemIndex", -1)) + 1;
                        node.Height = static_cast<float>(std::max(0, box.Height));
                        Fill(node, box, fill);
                        Handler(id, "onSelect", from, { "OnSelectionChange", "OnClick" });
                        Handler(id, "onActivate", from, { "OnDblClick" });
                    }
                }
                else if (kind == "listview")
                {
                    id = Make(UiKind::Table, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        if (const CeValue* columns = from.Find("Columns"); columns && columns->Type == CeValue::Kind::Collection)
                        {
                            for (const auto& entry : columns->Entries)
                            {
                                UiColumn column;
                                for (const auto& [name, value] : entry)
                                {
                                    if (EqualsNoCase(name, "Caption"))
                                        column.Title = value.Text;
                                    else if (EqualsNoCase(name, "Width") && value.Type == CeValue::Kind::Integer)
                                        column.Width = static_cast<float>(value.Integer);
                                }
                                node.Columns.push_back(std::move(column));
                            }
                        }
                        node.Height = static_cast<float>(std::max(0, box.Height));
                        Fill(node, box, fill);
                        Handler(id, "onSelect", from, { "OnSelectItem", "OnClick" });
                        Handler(id, "onActivate", from, { "OnDblClick" });
                        Note(Who(from) + ": the rows of a list view come from the script (rows option or update).");
                    }
                }
                else if (kind == "treeview")
                {
                    id = Make(UiKind::Tree, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Height = static_cast<float>(std::max(0, box.Height));
                        Fill(node, box, fill);
                        Handler(id, "onSelect", from, { "OnSelectionChanged", "OnClick" });
                        Handler(id, "onActivate", from, { "OnDblClick" });
                        Note(Who(from) + ": the items of a tree view come from the script (items option or update).");
                    }
                }
                else if (kind == "progressbar")
                {
                    id = Make(UiKind::Progress, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        const double low = static_cast<double>(from.Integer("Min", 0));
                        const double high = static_cast<double>(from.Integer("Max", 100));
                        node.Fraction = high > low ? static_cast<float>(std::clamp((static_cast<double>(from.Integer("Position", 0)) - low) / (high - low), 0.0, 1.0)) : 0.0f;
                        Fill(node, box, fill);
                    }
                }
                else if (kind == "trackbar")
                {
                    id = Make(UiKind::Slider, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Integer = true;
                        node.HasMin = true;
                        node.HasMax = true;
                        node.Min = static_cast<double>(from.Integer("Min", 0));
                        node.Max = static_cast<double>(from.Integer("Max", 10));
                        node.Number = static_cast<double>(from.Integer("Position", 0));
                        Fill(node, box, fill);
                        Handler(id, "onChange", from, { "OnChange" });
                    }
                }
                else if (kind == "groupbox")
                {
                    id = Make(UiKind::Section, parent, &from);
                    if (id)
                    {
                        Form.Model.Find(id)->Title = from.Text("Caption");
                        Children(id, from, box.Width, depth + 1);
                    }
                }
                else if (kind == "panel" || kind == "scrollbox" || kind == "flowpanel" || kind == "tabsheet" || kind == "page")
                {
                    const bool empty = std::none_of(from.Children.begin(), from.Children.end(), [&](const CeComponent& child) { return Visual(child); });
                    if (empty)
                    {
                        const std::string caption = from.Text("Caption");
                        if (!from.Text("OnClick").empty())
                        {
                            id = Make(UiKind::Button, parent, &from);
                            if (id)
                            {
                                Form.Model.Find(id)->Text = caption.empty() ? from.Name : caption;
                                Handler(id, "onClick", from, { "OnClick" });
                                Note(Who(from) + ": a clickable panel became a button.");
                            }
                        }
                        else if (!caption.empty())
                        {
                            id = Make(UiKind::Text, parent, &from);
                            if (id)
                                Form.Model.Find(id)->Text = caption;
                        }
                    }
                    else
                    {
                        id = Make(kind == "flowpanel" ? UiKind::Row : UiKind::Column, parent, &from);
                        if (id)
                            Children(id, from, box.Width, depth + 1);
                    }
                }
                else if (kind == "pagecontrol" || kind == "notebook")
                {
                    id = Make(UiKind::Tabs, parent, &from);
                    if (id)
                    {
                        for (const CeComponent& page : from.Children)
                        {
                            const int tab = Make(UiKind::Tab, id, &page);
                            if (!tab)
                                continue;
                            Form.Model.Find(tab)->Title = page.Text("Caption", page.Name);
                            Children(tab, page, box.Width, depth + 1);
                            Leftovers(page);
                        }
                    }
                }
                else if (kind == "image")
                {
                    id = Make(UiKind::Image, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Width = static_cast<float>(std::max(0, box.Width));
                        node.Height = static_cast<float>(std::max(0, box.Height));
                        Note(Who(from) + ": the picture was not taken over; choose a file in the Image option.");
                    }
                }
                else if (kind == "paintbox")
                {
                    id = Make(UiKind::Canvas, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Height = static_cast<float>(std::max(40, box.Height));
                        Fill(node, box, fill);
                        Handler(id, "onClick", from, { "OnMouseDown", "OnClick" });
                        Note(Who(from) + ": a paint box became a canvas; draw with canvas.draw instead of OnPaint.");
                    }
                }
                else if (kind == "shape" || kind == "bevel")
                {
                    if (box.Height > 0 && box.Height <= 4)
                        id = Make(UiKind::Separator, parent, &from);
                }
                else if (kind == "splitter")
                {
                }
                else
                {
                    id = Make(UiKind::Text, parent, &from);
                    if (id)
                    {
                        UiNode& node = *Form.Model.Find(id);
                        node.Text = "[" + from.Class + " " + from.Name + "]";
                        node.Color = "muted";
                        Note(Who(from) + " has no manifold.ui counterpart; a placeholder text marks its place.");
                    }
                }
                Leftovers(from);
                return id;
            }

            bool Visual(const CeComponent& component) const
            {
                static const std::set<std::string> kHidden = { "timer", "opendialog", "savedialog", "finddialog", "selectdirectorydialog", "mainmenu", "popupmenu",
                    "menuitem", "fontdialog", "colordialog", "imagelist", "actionlist" };
                return !kHidden.count(Kind(component));
            }

            void NonVisual(const CeComponent& from)
            {
                const std::string kind = Kind(from);
                if (kind == "timer")
                {
                    UiNode& window = *Form.Model.Find(Form.Window);
                    const std::string handler = from.Text("OnTimer");
                    auto& handlers = Form.Meta[Form.Window].Handlers;
                    if (!handler.empty() && FormIsIdentifier(handler) && !handlers.count("onUpdate"))
                    {
                        handlers["onUpdate"] = handler;
                        window.Interval = static_cast<double>(from.Integer("Interval", 1000)) / 1000.0;
                        Used.insert(from.Name + ".ontimer");
                        Note(Who(from) + ": its OnTimer (" + handler + ") became the window's onUpdate every " + std::to_string(from.Integer("Interval", 1000)) + " ms.");
                    }
                    else
                        Note(Who(from) + ": timers are no widgets; use the window's onUpdate or manifold timers in the script.");
                }
                else if (kind == "mainmenu" || kind == "popupmenu" || kind == "menuitem")
                    Note(Who(from) + ": menus are not converted; manifold.ui.menu.add or a widget's menu option replace them.");
                else if (kind.find("dialog") != std::string::npos)
                    Note(Who(from) + ": use manifold.ui.dialog.openFile or saveFile instead.");
                else
                    Note(Who(from) + " was left out.");
                Leftovers(from);
            }

            void Rows(int parent, std::vector<Box> boxes, int depth)
            {
                std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.Top != b.Top ? a.Top < b.Top : a.Left < b.Left; });
                std::vector<std::vector<Box>> rows;
                int bottom = 0;
                for (const Box& box : boxes)
                {
                    const int middle = box.Top + std::max(1, box.Height) / 2;
                    if (!rows.empty() && middle < bottom)
                    {
                        rows.back().push_back(box);
                        bottom = std::max(bottom, box.Top + box.Height);
                        continue;
                    }
                    rows.push_back({ box });
                    bottom = box.Top + std::max(1, box.Height);
                }
                for (auto& row : rows)
                    std::sort(row.begin(), row.end(), [](const Box& a, const Box& b) { return a.Left < b.Left; });
                const auto paired = [](const std::vector<Box>& row) {
                    return row.size() == 2 && IsLabel(Kind(*row[0].Item)) && row[0].Item->Text("OnClick").empty() && IsField(Kind(*row[1].Item));
                };
                for (std::size_t i = 0; i < rows.size();)
                {
                    std::size_t run = i;
                    while (run < rows.size() && paired(rows[run]))
                        ++run;
                    if (run - i >= 2)
                    {
                        const int grid = Make(UiKind::Grid, parent, nullptr);
                        for (std::size_t r = i; r < run; ++r)
                        {
                            Control(grid, *rows[r][0].Item, depth, false);
                            const int field = Control(grid, *rows[r][1].Item, depth, true);
                            if (UiNode* node = Form.Model.Find(field); node && node->Kind != UiKind::Checkbox && node->Kind != UiKind::Radio)
                                node->Label.clear();
                        }
                        i = run;
                        continue;
                    }
                    const std::vector<Box>& row = rows[i++];
                    if (row.size() == 1)
                    {
                        Control(parent, *row[0].Item, depth, false);
                        continue;
                    }
                    const int line = Make(UiKind::Row, parent, nullptr);
                    for (const Box& box : row)
                        Control(line, *box.Item, depth, false);
                }
            }

            void Children(int parent, const CeComponent& from, int width, int depth)
            {
                if (depth > kMaxDepth)
                {
                    Note(Who(from) + ": the controls nest too deep; the rest was left out.");
                    return;
                }
                std::vector<Box> top, bottom, left, right, client, free;
                for (const CeComponent& child : from.Children)
                {
                    if (!Visual(child))
                    {
                        NonVisual(child);
                        continue;
                    }
                    if (Kind(child) == "splitter")
                        continue;
                    const Box box = BoxOf(child);
                    if (box.Align == "altop")
                        top.push_back(box);
                    else if (box.Align == "albottom")
                        bottom.push_back(box);
                    else if (box.Align == "alleft")
                        left.push_back(box);
                    else if (box.Align == "alright")
                        right.push_back(box);
                    else if (box.Align == "alclient")
                        client.push_back(box);
                    else
                        free.push_back(box);
                }
                const auto byTop = [](const Box& a, const Box& b) { return a.Top < b.Top; };
                const auto byLeft = [](const Box& a, const Box& b) { return a.Left < b.Left; };
                std::sort(top.begin(), top.end(), byTop);
                std::sort(bottom.begin(), bottom.end(), byTop);
                std::sort(left.begin(), left.end(), byLeft);
                std::sort(right.begin(), right.end(), byLeft);
                for (const Box& box : top)
                    Control(parent, *box.Item, depth, false);
                const bool sides = !left.empty() || !right.empty();
                if (sides && left.size() == 1 && right.empty())
                {
                    const int split = Make(UiKind::Split, parent, nullptr);
                    UiNode& node = *Form.Model.Find(split);
                    node.Ratio = width > 0 ? std::clamp(static_cast<float>(left[0].Width) / static_cast<float>(width), 0.1f, 0.9f) : 0.4f;
                    const int first = Make(UiKind::Column, split, nullptr);
                    Control(first, *left[0].Item, depth, true);
                    const int second = Make(UiKind::Column, split, nullptr);
                    Rows(second, free, depth);
                    for (const Box& box : client)
                        Control(second, *box.Item, depth, true);
                }
                else if (sides)
                {
                    const int line = Make(UiKind::Row, parent, nullptr);
                    for (const Box& box : left)
                        Control(line, *box.Item, depth, false);
                    if (!free.empty() || !client.empty())
                    {
                        const int middle = Make(UiKind::Column, line, nullptr);
                        Rows(middle, free, depth);
                        for (const Box& box : client)
                            Control(middle, *box.Item, depth, false);
                    }
                    for (const Box& box : right)
                        Control(line, *box.Item, depth, false);
                }
                else
                {
                    Rows(parent, free, depth);
                    for (const Box& box : client)
                        Control(parent, *box.Item, depth, true);
                }
                for (const Box& box : bottom)
                    Control(parent, *box.Item, depth, false);
            }
        };
    }

    FormImport FormFromCe(const Table::CeComponent& root)
    {
        FormImport out;
        out.Form = NewForm(root.Text("Caption", root.Name));
        Converter converter{ out, out.Form };
        UiNode& window = *out.Form.Model.Find(out.Form.Window);
        const std::int64_t width = root.Integer("ClientWidth", root.Integer("Width", 400));
        const std::int64_t height = root.Integer("ClientHeight", root.Integer("Height", 300));
        window.Width = static_cast<float>(std::clamp<std::int64_t>(width + 16, 160, 4000));
        window.Height = static_cast<float>(std::clamp<std::int64_t>(height + 40, 100, 4000));
        if (FormIsIdentifier(root.Name))
            out.Form.Key = root.Name;
        converter.Handler(out.Form.Window, "onClose", root, { "OnClose" });
        for (const char* event : { "OnCreate", "OnShow", "OnActivate" })
        {
            const std::string name = root.Text(event);
            if (!name.empty())
            {
                converter.Used.insert(root.Name + "." + Lower(event));
                converter.Note(std::string("The form's ") + event + " (" + name + ") runs nowhere now: call it in the script after manifold.ui.form.load.");
            }
        }
        converter.Children(out.Form.Window, root, static_cast<int>(width), 0);
        converter.Leftovers(root);
        if (out.Form.Meta.count(out.Form.Window) && out.Form.Meta[out.Form.Window].Name.empty() && out.Form.Meta[out.Form.Window].Handlers.empty())
            out.Form.Meta.erase(out.Form.Window);
        out.Form.Meta.erase(0);
        return out;
    }
}
