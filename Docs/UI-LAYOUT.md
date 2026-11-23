# UI Layout

Every window of the program and every script window (`manifold.ui`) is drawn on one layout system in `Source/Layout`. It exists so that cut-off texts, overlapping widgets, uneven gaps and windows squeezed into slivers stop being something each window has to guard against by hand. Dear ImGui lets any call put anything anywhere; the layout system decides widths, keeps content inside its place and makes the safe behaviour the default.

## Contents

- [The rules in short](#the-rules-in-short)
- [The four layers](#the-four-layers)
- [Writing a window](#writing-a-window)
- [Script windows: UiHost](#script-windows-uihost)
- [The audit](#the-audit)
- [The sweep over all windows](#the-sweep-over-all-windows)
- [The lock on raw ImGui calls](#the-lock-on-raw-imgui-calls)
- [Tests](#tests)

## The rules in short

- **Text ends in `...`** when its place is too narrow, and its tooltip shows all of it. Wrapping, clipping or keeping the full width are chosen explicitly. In places without a bound (an auto-sizing popup, an auto-fitting table column) text keeps its width, so the place can grow to it.
- **Nothing leaves its place.** Every cell of a row, every column and every card is a region: ImGui widgets inside take its width, and a clip rect keeps drawing and mouse hover inside it.
- **Nothing collapses.** A field is at least 80 points wide, a button at least its padding and a few characters, a card 120 × 40 points, a text three characters and the ellipsis. Below that a row wraps (if it may) or reports that it is too narrow.
- **One set of measures.** Gaps, paddings, roundings and minimums come from `Layout::Rules`, in points; the components scale them by the monitor's DPI.
- **Windows know their content's minimum.** Script windows cannot be made narrower than their widgets need.

## The four layers

| Layer | Files | What it does |
| --- | --- | --- |
| Rules | `Layout/Rules.h` | `Rules` holds every measure in points (spacing scale `Space::Xs` … `Space::Xl` = 2/4/8/12/16, card and panel padding, minimums, the label column's share of a line, when labels go above their fields). `Defaults()` for the program, `RulesScope` to draw a part with other rules, `Pt()` and `Gap()` to read them scaled. |
| Core | `Layout/Size.h`, `Layout/Region.h`, `Layout/Containers.h` | `Size` (`Fixed`, `Fit`, `Fill` with `Min`/`Max`) says what a cell asks for; `Distribute` turns that and what the cells measured last frame into widths. `Region` bounds content (work rect, clip rect) and collects what it reports. `Row` and `Stack` are built from them. |
| Components | `Layout/Typography.h`, `Layout/Components.h` | `Text` (and `Muted`, `Label`, `Heading`, `Colored`, `Wrapped`, `Shorten` for labels a widget draws itself), `Button`, `IconButton`, `Field`, `LabelColumn`, `Card`/`BeginCard`, `Panel`, `TitleLine`, `KeyCap`, `Badge`, `RowSelectable`, `TreeNode` (its label ends in `...` within its place). Each reports its natural and minimum width to the region it is in. |
| Audit | `Layout/Audit.h`, `UI/LayoutAuditWindow` | Finds what slipped through and shows it (see below). |

`Layout/Layout.h` includes all of it.

### How widths are found

1. A `Row` reads what each cell measured in the last frame (its content's natural and minimum width, kept in the window's ImGui storage).
2. `Distribute` gives fixed cells their width, fit cells their content's width, and shares what is left among the fill cells by weight; a cell at its maximum hands its share on.
3. When the room is short, fit and fixed cells shrink toward their content's minimum (texts and buttons then end in `...`). A wrapping row breaks the line first.
4. When even the minimums do not fit, the cells keep their minimums, the row clips at its edge and the audit reports it.
5. Every cell passes its minimum up, so a window ends up knowing the least width its content can take.

## Writing a window

Derive from `ToolWindowBase` as before and draw the content with the components:

```cpp
#include "Layout/Layout.h"

void FindWindow::Draw()
{
    if (!BeginWindow(ICON_FA_SEARCH "  Find###find", ImVec2(520.0f, 300.0f)))
        return;
    {
        Layout::Row bar("##bar", { Layout::Size::Fill(), Layout::Size::Fit(), Layout::Size::Fit() });
        bar.Cell([&] {
            Layout::Field("Text", { .Placement = Layout::LabelPlacement::Auto });
            InputText("##text", m_text);
        });
        bar.Cell([&] { if (Layout::Button(ICON_FA_SEARCH "  Find", Layout::ButtonKind::Accent)) Find(); });
        bar.Cell([&] { if (Layout::Button("Close")) m_open = false; });
    }
    Layout::Card results("##results", { .Title = "Results", .Badge = m_badge, .Height = -FLT_MIN });
    for (const Hit& hit : m_hits)
        Layout::Text(hit.Line);
    results.End();
    EndToolWindow();
}
```

- Lay out side by side with `Row`, not `SameLine`; use `Layout::Toolbar()` as the row's options for a toolbar that wraps.
- Draw text with `Layout::Text` and its presets, not `TextUnformatted`. Choose `Overflow::Wrap` for paragraphs.
- Size fields with `Layout::Field` (it draws the label and sets the width), not `SetNextItemWidth`.
- Buttons are `Layout::Button`/`IconButton`; give an `IconButton` an id of its own, never just the glyph.
- Cards and panels are `Layout::Card`/`Panel` rather than a styled `BeginChild`.
- Trees are `Layout::TreeNode(id, label, flags)`: a tree in a narrow pane shortens its labels instead of running out of the pane.
- A table row's name goes over `Layout::RowSelectable` as `Layout::Text`, not into the selectable's label, which ImGui cuts at the column's edge.
- A pannable canvas (a child whose content moves with its own view) sets its content size back to nothing before `EndChild`, so the audit does not count what lies off its view.
- Raw ImGui is still fine for what the components do not cover (tables, menus, combo boxes, custom drawing): inside a region those take the region's width too.

## Script windows: UiHost

`UI/UiHost` draws every `manifold.ui` window through the same components, so scripts follow the rules without asking for them:

- A window's content is one region; its reported minimum (plus padding and scrollbar, at most 80 % of the monitor) is the window's minimum width, also when it is docked.
- A `row` is a `Layout::Row`: fields, bars, sections, tables, trees, canvases, columns and spacers without a `width` fill; widgets with a `width` are fixed; the rest fit their content. `minWidth`/`maxWidth` bound the cell (for fields: the field). `wrap = true` breaks the line, `align` moves the cells.
- Texts and headings are `Layout::Text`; `overflow = "ellipsis"` (default), `"wrap"`, `"clip"` or `"visible"` chooses what happens when they are too wide (API 27). `wrap = true` and `maxWidth` still wrap.
- Sections are `Layout::Card`, buttons `Layout::Button`, fields `Layout::Field`, hotkey caps `Layout::KeyCap`, table cells `Layout::Text`.
- `UiHost::Rules()` are the rules its windows are drawn with.

## The audit

Settings → Diagnostics → **Layout Checks** (or the start option `--layout-audit`) turns it on. Then:

- content that leaves its cell (raw calls inside a region), rows whose minimums do not fit, any ImGui window whose content is wider than the window without a horizontal scrollbar (so it is cut off), and table columns whose content is wider than the column (ImGui cuts it at the column's edge) are outlined on screen in red, with the place they should have kept to in amber;
- **Layout Problems** (Ctrl+Shift+F11, also in the Help menu while the checks are on) lists them per window, with how often they were seen; hovering a row outlines it;
- each new problem is written once to the log (area `Layout`).

The window scan finds problems in windows that do not use the layout system yet, which makes it the guide for moving them over.

## The sweep over all windows

The audit only sees what is on screen. The sweep opens every window for it:

```powershell
Manifold-Engine.exe --layout-sweep=sweep.txt --process=TestTarget.exe --table=some.CT
```

- It runs in a cloaked main window without platform windows, reads and writes no layout file and saves no settings, so nothing appears on screen, nothing takes the focus and the program's own setup stays as it was. The program closes when the report is written.
- Every window `--open` knows (`Workspace::WindowNames()`) is opened alone, undocked, at its minimum, a medium (760 × 540) and a large (1280 × 860) size, and on every tab of its tab bars; the main window at its minimum, 1280 × 800 and 1920 × 1080. All of it at 100 % and 150 % unless `--sweep-scales=100,125,150` says otherwise.
- `--sweep-windows=templates,forms` limits it to those windows; `--sweep-windows=+templates:<Id>` adds a window that needs an argument to show its content.
- The report lists each problem once per window and place, with the worst amount, one detail, every scale, size and tab it was seen at, and the names that opened it.
- What the sweep cannot reach: content that only some data brings (long names, deep groups), so a process (`--process=`) and a real table (`--table=`) make it see more, and panes a person resized by hand. The components cover those cases by shortening, not the sweep.

## The lock on raw ImGui calls

`Source/Layout/RawImGui.txt` lists, per file in `Source/UI` and `Source/App`, how many raw calls of the kinds the components replace are left (`Button`, `SmallButton`, `Text…`, `TextUnformatted`, `SetNextItemWidth`, `PushItemWidth`, `BeginChild`, `SameLine`, `CalcTextSize`). `Source/Layout` itself is exempt.

- The build warns (`ME0004`) when a file uses more than its number (`Tools/check_ui_raw.py`, run before every build of the program).
- The tests fail on the same numbers (suite `ui-layout`), locally and in CI. A file that is not listed may use none, so every new window starts on the layout system.
- After moving a file to the components, `Manifold-Engine.Tests.exe --ui-raw-baseline` lowers its number; it never raises one.
- A raised number is a deliberate exception: change it by hand and say why on a comment line above it.

## Tests

The suite `ui-layout` (`Tests/UiLayoutTests.cpp`) runs Dear ImGui headless, without a GPU, at DPI scales 1, 1.5 and 2:

- `Distribute`: shares, weights, maximums handing on, minimums kept, fit cells shrinking first, wrapping, scaling, and a sweep over widths that no line ever takes more than it has;
- `FitText`: whole text, nothing, monotonic cuts, UTF-8 boundaries;
- texts never drawing past their region over a sweep of widths, and keeping their width in auto-sizing windows;
- rows over a sweep of widths: every cell's content inside its cell, no overlap, fixed and maximum widths held;
- buttons at their minimum, fields placing their labels above when narrow, regions restoring the window, cards clipping their content;
- the audit finding a raw window that is cut off, raw content leaving a region and raw text cut by its table column, and nothing in a window built with the components;
- tree nodes and `Shorten` staying within their place at every scale;
- the lock against the real tree.
