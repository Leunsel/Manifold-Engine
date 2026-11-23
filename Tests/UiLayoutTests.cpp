#include "TestKit.h"

#include "Layout/Layout.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        // ∑ an ImGui context without a GPU: the font atlas's textures are
        // marked as made the way a renderer backend would, so dynamic font
        // sizes (DPI scales) work
        class Headless
        {
        public:
            explicit Headless(float scale = 1.0f)
            {
                m_context = ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.LogFilename = nullptr;
                io.DisplaySize = ImVec2(1920.0f, 1200.0f);
                io.DeltaTime = 1.0f / 60.0f;
                io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
                io.Fonts->AddFontDefault();
                ImGui::GetStyle().FontScaleDpi = scale;
                ImGui::GetStyle().ScaleAllSizes(scale);
            }

            ~Headless()
            {
                for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
                {
                    if (texture->RefCount == 1)
                    {
                        texture->SetTexID(ImTextureID_Invalid);
                        texture->SetStatus(ImTextureStatus_Destroyed);
                    }
                }
                ImGui::DestroyContext(m_context);
            }

            Headless(const Headless&) = delete;
            Headless& operator=(const Headless&) = delete;

            template <class Draw>
            void Frame(Draw&& draw)
            {
                ImGui::NewFrame();
                Layout::NewFrame();
                draw();
                Layout::Audit::EndFrame();
                ImGui::Render();
                for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
                {
                    if (texture->Status == ImTextureStatus_WantCreate)
                    {
                        texture->SetTexID(static_cast<ImTextureID>(1));
                        texture->SetStatus(ImTextureStatus_OK);
                    }
                    else if (texture->Status == ImTextureStatus_WantUpdates)
                        texture->SetStatus(ImTextureStatus_OK);
                    else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0)
                    {
                        texture->SetTexID(ImTextureID_Invalid);
                        texture->SetStatus(ImTextureStatus_Destroyed);
                    }
                }
            }

            // a fixed window whose content area is width pixels wide
            template <class Draw>
            void Window(const char* name, float width, Draw&& draw, ImGuiWindowFlags flags = 0)
            {
                const ImGuiStyle& style = ImGui::GetStyle();
                ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
                ImGui::SetNextWindowSize(ImVec2(width + style.WindowPadding.x * 2.0f, 600.0f));
                ImGui::Begin(name, nullptr, flags | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar);
                draw();
                ImGui::End();
            }

        private:
            ImGuiContext* m_context = nullptr;
        };

        using Layout::Measure;
        using Layout::Size;

        bool Near(float a, float b, float tolerance = 0.51f)
        {
            return a > b - tolerance && a < b + tolerance;
        }

        void Distribution()
        {
            {
                const Size cells[] = { Size::Fill(), Size::Fill() };
                const Layout::Slots slots = Layout::Distribute(300.0f, 10.0f, cells, {}, 1.0f, false);
                CHECK(slots.Fits);
                CHECK(Near(slots.Widths[0], 145.0f) && Near(slots.Widths[1], 145.0f));
            }
            {
                const Size cells[] = { Size::Fixed(100.0f), Size::Fill() };
                const Layout::Slots slots = Layout::Distribute(300.0f, 0.0f, cells, {}, 1.0f, false);
                CHECK(Near(slots.Widths[0], 100.0f) && Near(slots.Widths[1], 200.0f));
            }
            {
                // a fill cell at its maximum hands the rest to the other
                const Size cells[] = { Size::Fill().Max(50.0f), Size::Fill() };
                const Layout::Slots slots = Layout::Distribute(300.0f, 0.0f, cells, {}, 1.0f, false);
                CHECK(Near(slots.Widths[0], 50.0f) && Near(slots.Widths[1], 250.0f));
            }
            {
                // weights
                const Size cells[] = { Size::Fill(1.0f), Size::Fill(3.0f) };
                const Layout::Slots slots = Layout::Distribute(400.0f, 0.0f, cells, {}, 1.0f, false);
                CHECK(Near(slots.Widths[0], 100.0f) && Near(slots.Widths[1], 300.0f));
            }
            {
                // minimums are kept even when they do not fit, and the row says so
                const Size cells[] = { Size::Fill().Min(200.0f), Size::Fill().Min(200.0f) };
                const Layout::Slots slots = Layout::Distribute(300.0f, 0.0f, cells, {}, 1.0f, false);
                CHECK(!slots.Fits);
                CHECK(Near(slots.Widths[0], 200.0f) && Near(slots.Widths[1], 200.0f));
                CHECK(Near(slots.Minimum, 400.0f));
            }
            {
                // a fit cell shrinks toward its content's minimum before anything overlaps
                const Size cells[] = { Size::Fit(), Size::Fixed(100.0f) };
                const Measure measured[] = { { 200.0f, 50.0f }, {} };
                const Layout::Slots slots = Layout::Distribute(200.0f, 0.0f, cells, measured, 1.0f, false);
                CHECK(slots.Fits);
                CHECK(Near(slots.Widths[0] + slots.Widths[1], 200.0f));
                CHECK(slots.Widths[0] >= 50.0f - 0.5f);
                CHECK(Near(slots.Natural, 300.0f));
            }
            {
                // wrapping keeps the cells' own sizes and breaks the line
                const Size cells[] = { Size::Fit(), Size::Fit(), Size::Fit() };
                const Measure measured[] = { { 120.0f, 40.0f }, { 120.0f, 40.0f }, { 120.0f, 40.0f } };
                const Layout::Slots slots = Layout::Distribute(300.0f, 10.0f, cells, measured, 1.0f, true);
                CHECK(slots.Fits);
                CHECK(slots.LineOf[0] == 0 && slots.LineOf[1] == 0 && slots.LineOf[2] == 1);
                CHECK(Near(slots.Widths[2], 120.0f));
                CHECK(Near(slots.Minimum, 40.0f));
            }
            {
                // points scale
                const Size cells[] = { Size::Fixed(100.0f), Size::Fill().Min(30.0f) };
                const Layout::Slots slots = Layout::Distribute(1000.0f, 0.0f, cells, {}, 2.0f, false);
                CHECK(Near(slots.Widths[0], 200.0f) && Near(slots.Widths[1], 800.0f));
            }
            // whatever the room, a row that fits never takes more than it has, and no cell goes below its minimum
            for (float available = 0.0f; available <= 900.0f; available += 13.0f)
            {
                for (const bool wrap : { false, true })
                {
                    const Size cells[] = { Size::Fill().Min(40.0f), Size::Fixed(90.0f), Size::Fit(), Size::Fill(2.0f).Max(160.0f), Size::Fit().Min(30.0f) };
                    const Measure measured[] = { { 100.0f, 50.0f }, {}, { 70.0f, 20.0f }, { 0.0f, 0.0f }, { 10.0f, 10.0f } };
                    const Layout::Slots slots = Layout::Distribute(available, 8.0f, cells, measured, 1.0f, wrap);
                    for (std::size_t line = 0; line < slots.LineUsed.size(); ++line)
                    {
                        if (slots.Fits && available > 0.0f)
                            CHECK_MSG(slots.LineUsed[line] <= available + 0.5f, "at " + std::to_string(available));
                    }
                    CHECK(slots.Widths[0] >= 40.0f - 0.5f && slots.Widths[4] >= 30.0f - 0.5f && slots.Widths[3] <= 160.0f + 0.5f);
                }
            }
        }

        void TextFitting()
        {
            Headless ui;
            ui.Frame([&] {
                ImFont* font = ImGui::GetFont();
                const float size = ImGui::GetFontSize();
                const std::string text = "Address of the player's health";
                const float full = ImGui::CalcTextSize(text.c_str()).x;
                const Layout::TextFit whole = Layout::FitText(font, size, text, full + 1.0f);
                CHECK(!whole.Truncated && whole.Bytes == text.size());
                const Layout::TextFit none = Layout::FitText(font, size, text, 0.0f);
                CHECK(none.Truncated && none.Bytes == 0);
                std::size_t previous = 0;
                const float dots = ImGui::CalcTextSize("...").x;
                for (float width = dots; width < full; width += 3.0f)
                {
                    const Layout::TextFit fit = Layout::FitText(font, size, text, width);
                    CHECK(fit.Truncated);
                    CHECK_MSG(fit.Width <= width + 0.01f, std::to_string(width));
                    CHECK(fit.Bytes >= previous || text[fit.Bytes] == ' ' || text[previous - 1] == ' ');
                    previous = std::max(previous, fit.Bytes);
                }
                // UTF-8 is cut between characters
                const std::string umlauts = "\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xA4\xC3\xB6\xC3\xBC";
                for (float width = dots; width < ImGui::CalcTextSize(umlauts.c_str()).x; width += 2.0f)
                    CHECK(Layout::FitText(font, size, umlauts, width).Bytes % 2 == 0);
            });
        }

        void TextStaysInside()
        {
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                Headless ui(scale);
                const std::string text = "A long description that cannot fit into a narrow cell of the layout";
                for (float width = 4.0f; width < 700.0f; width += 23.0f)
                {
                    bool truncated = false;
                    ImRect drawn;
                    float right = 0.0f;
                    float natural = 0.0f;
                    for (int frame = 0; frame < 2; ++frame)
                    {
                        ui.Frame([&] {
                            ui.Window("text", 800.0f, [&] {
                                Layout::RegionOptions options;
                                options.Width = width;
                                Layout::Region cell(options);
                                right = cell.Right();
                                natural = ImGui::CalcTextSize(text.c_str()).x;
                                truncated = Layout::Text(text);
                                drawn = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                                cell.End();
                            });
                        });
                    }
                    CHECK_MSG(drawn.Max.x <= right + 0.51f, "scale " + std::to_string(scale) + ", width " + std::to_string(width));
                    CHECK_MSG(truncated == (natural > width + 0.01f), "scale " + std::to_string(scale) + ", width " + std::to_string(width));
                }
            }
        }

        void AutoSizeKeepsText()
        {
            Headless ui;
            const std::string text = "A popup's text keeps its own width so the popup can grow to it";
            float width = 0.0f;
            bool truncated = true;
            for (int frame = 0; frame < 6; ++frame)
            {
                ui.Frame([&] {
                    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
                    ImGui::Begin("auto", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
                    truncated = Layout::Text(text);
                    width = ImGui::GetItemRectSize().x;
                    ImGui::End();
                });
            }
            CHECK(!truncated);
            CHECK(Near(width, ImGui::CalcTextSize(text.c_str()).x, 1.0f) || width > 0.0f);
        }

        struct CellBox
        {
            float Left = 0.0f;
            float Right = 0.0f;
            ImRect Content;
        };

        void RowsShareWithoutOverlap()
        {
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                Headless ui(scale);
                for (float width = 60.0f * scale; width < 900.0f * scale; width += 37.0f * scale)
                {
                    std::vector<CellBox> boxes;
                    Layout::Slots slots;
                    float rowLeft = 0.0f;
                    float rowRight = 0.0f;
                    for (int frame = 0; frame < 3; ++frame)
                    {
                        boxes.clear();
                        ui.Frame([&] {
                            ui.Window("row", width, [&] {
                                rowLeft = ImGui::GetCursorScreenPos().x;
                                rowRight = rowLeft + Layout::Avail();
                                Layout::Row row("##row", { Size::Fill(), Size::Fixed(80.0f), Size::Fit(), Size::Fit().Max(90.0f) });
                                const auto cell = [&](auto&& draw) {
                                    row.Cell([&] {
                                        CellBox box;
                                        box.Left = Layout::Region::Current()->Left();
                                        box.Right = Layout::Region::Current()->Right();
                                        draw();
                                        box.Content = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                                        boxes.push_back(box);
                                    });
                                };
                                cell([] { Layout::Text("Health of the player character"); });
                                cell([] { Layout::Button("Search"); });
                                cell([] { Layout::Button("Add Address Manually"); });
                                cell([] { Layout::Muted("A note that is longer than ninety points"); });
                                slots = row.Widths();
                                row.End();
                            });
                        });
                    }
                    const std::string where = "scale " + std::to_string(scale) + ", width " + std::to_string(width);
                    CHECK_MSG(boxes.size() == 4, where);
                    for (std::size_t i = 0; i < boxes.size(); ++i)
                    {
                        CHECK_MSG(boxes[i].Content.Min.x >= boxes[i].Left - 0.51f && boxes[i].Content.Max.x <= boxes[i].Right + 0.51f, where + ", cell " + std::to_string(i));
                        if (i > 0)
                            CHECK_MSG(boxes[i].Left >= boxes[i - 1].Right - 0.51f || boxes[i].Content.Min.y > boxes[i - 1].Content.Min.y, where + ", cell " + std::to_string(i));
                        if (slots.Fits)
                            CHECK_MSG(boxes[i].Right <= rowRight + 0.51f, where + ", cell " + std::to_string(i));
                    }
                    if (slots.Fits)
                        CHECK_MSG(boxes[3].Right - boxes[3].Left <= 90.0f * scale + 0.51f, where);
                }
            }
        }

        void ButtonsKeepTheirMinimum()
        {
            Headless ui;
            float width = 0.0f;
            float least = 0.0f;
            for (int frame = 0; frame < 2; ++frame)
            {
                ui.Frame([&] {
                    ui.Window("button", 400.0f, [&] {
                        Layout::RegionOptions options;
                        options.Width = 6.0f;
                        Layout::Region cell(options);
                        Layout::Button("Remove Selected Records");
                        width = ImGui::GetItemRectSize().x;
                        least = ImGui::GetFrameHeight();
                        cell.End();
                    });
                });
            }
            CHECK(width >= least - 0.5f);
        }

        void FieldsPlaceTheirLabels()
        {
            Headless ui;
            Layout::FieldLayout narrow;
            Layout::FieldLayout wide;
            ui.Frame([&] {
                ui.Window("fields", 700.0f, [&] {
                    {
                        Layout::RegionOptions options;
                        options.Width = 150.0f;
                        Layout::Region cell(options);
                        narrow = Layout::Field("A long label for a value");
                        static char buffer[16] = "";
                        ImGui::InputText("##narrow", buffer, sizeof(buffer));
                    }
                    {
                        Layout::RegionOptions options;
                        options.Width = 600.0f;
                        Layout::Region cell(options);
                        wide = Layout::Field("Address");
                        static char buffer[16] = "";
                        ImGui::InputText("##wide", buffer, sizeof(buffer));
                        CHECK(ImGui::GetItemRectMax().x <= cell.Right() + 0.51f);
                    }
                });
            });
            CHECK(narrow.Placement == Layout::LabelPlacement::Above);
            CHECK(wide.Placement == Layout::LabelPlacement::Left);
            CHECK(narrow.Width >= Layout::Pt(Layout::Current().MinFieldWidth) - 0.5f);
        }

        // ∑ a window's minimum comes from its content; if the content's
        // minimum followed the window's width (a key cap after a field that
        // fills the line, reported at the line's end), the window would grow
        // by measuring itself
        void MinimumDoesNotFollowWidth()
        {
            Headless ui;
            std::vector<float> minimums;
            for (const float width : { 320.0f, 820.0f })
            {
                float minimum = 0.0f;
                for (int frame = 0; frame < 3; ++frame)
                {
                    ui.Frame([&] {
                        ui.Window("minimum", 900.0f, [&] {
                            Layout::RegionOptions options;
                            options.Width = width;
                            Layout::Region root(options);
                            Layout::Text("Heading text that is long enough to be shortened");
                            Layout::FieldOptions field;
                            field.Reserve = Layout::KeyCapWidth("+ F5") + ImGui::GetStyle().ItemInnerSpacing.x;
                            Layout::Field("Health", field);
                            static char buffer[16] = "";
                            ImGui::InputText("##health", buffer, sizeof(buffer));
                            {
                                Layout::QuietReports quiet;
                                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                                Layout::KeyCap("+ F5");
                            }
                            Layout::Row row("##row", { Size::Fill(), Size::Fit() });
                            row.Cell([] { Layout::Muted("Status of the scan"); });
                            row.Cell([] { Layout::Button("Close"); });
                            row.End();
                            Layout::Card card("##card", { .Title = "A card" });
                            Layout::Text("Inside the card");
                            card.End();
                            root.End();
                            minimum = root.Reports().Minimum;
                        });
                    });
                }
                minimums.push_back(minimum);
            }
            CHECK_MSG(Near(minimums[0], minimums[1], 1.0f), std::to_string(minimums[0]) + " vs " + std::to_string(minimums[1]));
            CHECK(minimums[0] > 0.0f && minimums[0] < 320.0f);
        }

        void RegionsRestoreTheWindow()
        {
            Headless ui;
            ui.Frame([&] {
                ui.Window("restore", 500.0f, [&] {
                    const float before = ImGui::GetContentRegionAvail().x;
                    {
                        Layout::RegionOptions options;
                        options.Width = 120.0f;
                        Layout::Region cell(options);
                        CHECK(Near(Layout::Avail(), 120.0f));
                        Layout::Row row("##inner", { Size::Fill(), Size::Fill() });
                        row.Cell([] { CHECK(Layout::Avail() < 120.0f); });
                        row.Cell([] {});
                    }
                    CHECK(Near(ImGui::GetContentRegionAvail().x, before, 1.0f));
                    CHECK(!Layout::Region::Current());
                });
            });
        }

        void CardsClipTheirContent()
        {
            Headless ui;
            ImRect inner;
            ImRect text;
            for (int frame = 0; frame < 2; ++frame)
            {
                ui.Frame([&] {
                    ui.Window("card", 300.0f, [&] {
                        Layout::CardOptions options;
                        options.Title = "A card whose title is much longer than the card itself is wide";
                        options.Badge = "12 records";
                        options.Width = 200.0f;
                        Layout::Card card("##card", options);
                        Layout::Text("And content that is wider than the card as well, by a lot");
                        text = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                        inner = ImGui::GetCurrentWindow()->InnerClipRect;
                        card.End();
                    });
                });
            }
            CHECK(text.Max.x <= inner.Max.x + 0.51f);
        }

        void AuditFindsRawOverflow()
        {
            Headless ui;
            Layout::Audit::Clear();
            Layout::Audit::SetEnabled(true);
            for (int frame = 0; frame < 8; ++frame)
            {
                ui.Frame([&] {
                    ui.Window("raw", 200.0f, [&] { ImGui::Button("A raw button wider than its window", ImVec2(400.0f, 0.0f)); });
                    ImGui::SetNextWindowPos(ImVec2(500.0f, 20.0f));
                    ImGui::SetNextWindowSize(ImVec2(220.0f, 300.0f));
                    ImGui::Begin("kept", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
                    Layout::Text("A text that would be too wide for this window without the layout");
                    Layout::Row row("##row", { Size::Fill(), Size::Fit() });
                    row.Cell([] { Layout::Muted("Some more text in a filling cell"); });
                    row.Cell([] { Layout::Button("A Button"); });
                    row.End();
                    ImGui::End();
                    {
                        ImGui::SetNextWindowPos(ImVec2(900.0f, 20.0f));
                        ImGui::SetNextWindowSize(ImVec2(220.0f, 300.0f));
                        ImGui::Begin("leaking", nullptr, ImGuiWindowFlags_NoSavedSettings);
                        Layout::RegionOptions options;
                        options.Width = 60.0f;
                        Layout::Region cell(options);
                        ImGui::Button("raw", ImVec2(150.0f, 0.0f));
                        cell.End();
                        ImGui::End();
                    }
                });
            }
            bool clipped = false;
            bool kept = true;
            std::string seen;
            bool leaked = false;
            for (const Layout::Audit::Problem& problem : Layout::Audit::Problems())
            {
                clipped = clipped || (problem.WindowName == "raw" && problem.What == Layout::Audit::Kind::WindowClipped);
                kept = kept && problem.WindowName != "kept";
                seen += problem.WindowName + ": " + Layout::Audit::KindName(problem.What) + " (" + problem.Detail + "); ";
                leaked = leaked || (problem.WindowName == "leaking" && problem.What == Layout::Audit::Kind::CellOverflow);
            }
            CHECK_MSG(clipped, "a raw button wider than its window is reported as cut off");
            CHECK_MSG(kept, "a window built with the layout reports nothing: " + seen);
            CHECK_MSG(leaked, "raw content leaving its region is reported");
            Layout::Audit::SetEnabled(false);
        }

        // ∑ a table cell clips at its column, so raw text there leaves no trace
        //   in the window's size; the audit reads the columns' measured content
        void AuditFindsCutColumns()
        {
            Headless ui;
            Layout::Audit::Clear();
            Layout::Audit::SetEnabled(true);
            for (int frame = 0; frame < 8; ++frame)
            {
                ui.Frame([&] {
                    ui.Window("columns", 400.0f, [&] {
                        if (ImGui::BeginTable("##cut", 2, ImGuiTableFlags_BordersInnerV))
                        {
                            ImGui::TableSetupColumn("Raw", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableSetupColumn("Kept", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted("A raw text much longer than its column");
                            ImGui::TableNextColumn();
                            Layout::Text("A layout text much longer than its column");
                            ImGui::EndTable();
                        }
                    });
                });
            }
            bool raw = false;
            bool kept = true;
            std::string seen;
            for (const Layout::Audit::Problem& problem : Layout::Audit::Problems())
            {
                seen += problem.WindowName + ": " + Layout::Audit::KindName(problem.What) + " (" + problem.Detail + "); ";
                if (problem.What != Layout::Audit::Kind::ColumnClipped)
                    continue;
                raw = raw || problem.Detail.find("\"Raw\"") != std::string::npos;
                kept = kept && problem.Detail.find("\"Kept\"") == std::string::npos;
            }
            CHECK_MSG(raw, "raw text cut by its column is reported: " + seen);
            CHECK_MSG(kept, "a layout text in a fixed column given a width ends in an ellipsis instead: " + seen);
            Layout::Audit::SetEnabled(false);
        }

        // ∑ a tree node ends its label in "..." within its place and keeps the
        //   natural width for windows and columns that fit their content
        void TreeNodesFitTheirPlace()
        {
            {
                Headless ui;
                bool truncated = false;
                std::string shown;
                std::string whole;
                float used = 0.0f;
                for (int frame = 0; frame < 3; ++frame)
                {
                    ui.Frame([&] {
                        ui.Window("shorten", 400.0f, [&] {
                            whole = Layout::Shorten("short", 1000.0f);
                            shown = Layout::Shorten("A long label that will not fit", 60.0f, &truncated);
                            used = ImGui::CalcTextSize(shown.c_str()).x;
                        });
                    });
                }
                CHECK(whole == "short");
                CHECK(truncated);
                CHECK_MSG(used <= 60.0f + 0.5f, std::to_string(used));
                CHECK(shown.size() < std::string("A long label that will not fit").size());
            }
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                Headless ui(scale);
                ImRect node;
                ImRect bounds;
                for (int frame = 0; frame < 3; ++frame)
                {
                    ui.Frame([&] {
                        ui.Window("tree", 400.0f, [&] {
                            ImGui::BeginChild("##pane", ImVec2(150.0f * scale, 200.0f));
                            const ImGuiWindow* pane = ImGui::GetCurrentWindow();
                            bounds = pane->ContentRegionRect;
                            if (Layout::TreeNode("##folder", "x86/x64 - Teleporter Hooks and other long names", ImGuiTreeNodeFlags_SpanAvailWidth))
                                ImGui::TreePop();
                            node = ImRect(ImGui::GetItemRectMin(), ImVec2(pane->DC.CursorMaxPos.x, ImGui::GetItemRectMax().y));
                            ImGui::EndChild();
                        });
                    });
                }
                CHECK_MSG(node.Max.x <= bounds.Max.x + 1.0f, "scale " + std::to_string(scale) + ": the node's content reaches " + std::to_string(node.Max.x) + ", its pane ends at " +
                                                                std::to_string(bounds.Max.x));
            }
        }

        // ---- the lock on raw ImGui calls (Source/Layout/RawImGui.txt, Tools/check_ui_raw.py) ----

        struct RawRules
        {
            std::vector<std::string> Header;
            std::vector<std::string> Patterns;
            std::vector<std::string> Scopes;
            std::vector<std::string> Excluded;
            std::map<std::string, int> Counts;
            std::vector<std::string> Lines;   // every line as it is, for rewriting
        };

        std::filesystem::path RepositoryRoot()
        {
            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Source" / "Layout" / "RawImGui.txt"); ++i)
                at = at.parent_path();
            return std::filesystem::exists(at / "Source" / "Layout" / "RawImGui.txt") ? at : std::filesystem::path();
        }

        RawRules ReadRules(const std::filesystem::path& root)
        {
            RawRules rules;
            std::ifstream file(root / "Source" / "Layout" / "RawImGui.txt", std::ios::binary);
            std::string line;
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                rules.Lines.push_back(line);
                if (line.empty() || line[0] == '#')
                    continue;
                const std::size_t space = line.find(' ');
                const std::string word = line.substr(0, space);
                const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
                if (word == "pattern")
                    rules.Patterns.push_back(rest);
                else if (word == "scope")
                    rules.Scopes.push_back(rest);
                else if (word == "exclude")
                    rules.Excluded.push_back(rest);
                else if (const std::size_t last = line.rfind(' '); last != std::string::npos)
                    rules.Counts[line.substr(0, last)] = std::atoi(line.c_str() + last + 1);
            }
            return rules;
        }

        std::map<std::string, int> CountRaw(const std::filesystem::path& root, const RawRules& rules)
        {
            std::map<std::string, int> counts;
            for (const std::string& scope : rules.Scopes)
            {
                std::error_code error;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(root / scope, error))
                {
                    const std::string extension = entry.path().extension().string();
                    if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h"))
                        continue;
                    std::string relative = std::filesystem::relative(entry.path(), root).generic_string();
                    if (std::any_of(rules.Excluded.begin(), rules.Excluded.end(), [&](const std::string& prefix) { return relative.rfind(prefix, 0) == 0; }))
                        continue;
                    std::ifstream file(entry.path(), std::ios::binary);
                    std::string line;
                    int total = 0;
                    while (std::getline(file, line))
                    {
                        const std::size_t first = line.find_first_not_of(" \t");
                        if (first != std::string::npos && line.compare(first, 2, "//") == 0)
                            continue;
                        for (const std::string& pattern : rules.Patterns)
                        {
                            for (std::size_t at = line.find(pattern); at != std::string::npos; at = line.find(pattern, at + pattern.size()))
                                ++total;
                        }
                    }
                    counts[relative] = total;
                }
            }
            return counts;
        }

        void RawCallsStayLocked()
        {
            const std::filesystem::path root = RepositoryRoot();
            if (root.empty())
            {
                Skip("Source/Layout/RawImGui.txt not found above " + std::filesystem::current_path().string());
                return;
            }
            const RawRules rules = ReadRules(root);
            CHECK(!rules.Patterns.empty() && !rules.Scopes.empty());
            for (const auto& [file, used] : CountRaw(root, rules))
            {
                const auto known = rules.Counts.find(file);
                const int allowed = known == rules.Counts.end() ? 0 : known->second;
                CHECK_MSG(used <= allowed, file + " has " + std::to_string(used) + " raw ImGui calls where " + std::to_string(allowed) +
                                               " are allowed: draw text, buttons, fields and containers with the Layout components (Docs/UI-LAYOUT.md)");
            }
        }

        int RunUiLayoutTests()
        {
            const int before = Totals().Failures;
            Distribution();
            TextFitting();
            TextStaysInside();
            AutoSizeKeepsText();
            RowsShareWithoutOverlap();
            ButtonsKeepTheirMinimum();
            FieldsPlaceTheirLabels();
            MinimumDoesNotFollowWidth();
            RegionsRestoreTheWindow();
            CardsClipTheirContent();
            AuditFindsRawOverflow();
            AuditFindsCutColumns();
            TreeNodesFitTheirPlace();
            RawCallsStayLocked();
            return Totals().Failures - before;
        }
    }

    MANIFOLD_SUITE("ui-layout", "UI layout", 45, [](const SuiteContext&) { return RunUiLayoutTests(); });

    // ∑ lowers the numbers in Source/Layout/RawImGui.txt to what the files use now; it never raises one
    MANIFOLD_MODE("--ui-raw-baseline", [](const std::vector<std::string>&)
        {
            const std::filesystem::path root = RepositoryRoot();
            if (root.empty())
            {
                std::fputs("Source/Layout/RawImGui.txt was not found above the current folder\n", stderr);
                return 1;
            }
            const RawRules rules = ReadRules(root);
            const std::map<std::string, int> counts = CountRaw(root, rules);
            std::ostringstream out;
            int lowered = 0;
            for (const std::string& line : rules.Lines)
            {
                const std::size_t last = line.rfind(' ');
                const auto known = last == std::string::npos ? rules.Counts.end() : rules.Counts.find(line.substr(0, last));
                if (line.empty() || line[0] == '#' || known == rules.Counts.end())
                {
                    out << line << '\n';
                    continue;
                }
                const auto now = counts.find(known->first);
                const int used = now == counts.end() ? 0 : now->second;
                if (used >= known->second)
                {
                    out << line << '\n';
                    continue;
                }
                ++lowered;
                if (used > 0)
                    out << known->first << ' ' << used << '\n';
            }
            std::ofstream file(root / "Source" / "Layout" / "RawImGui.txt", std::ios::binary | std::ios::trunc);
            file << out.str();
            std::printf("%d files lowered in Source/Layout/RawImGui.txt\n", lowered);
            return 0;
        });
}
