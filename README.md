# Manifold Engine

A memory scanner, debugger and scripting tool for Windows games and programs, in the spirit of Cheat Engine. It reads and writes Cheat Engine tables (`.CT`), runs Cheat Engine's Auto Assembler and Lua scripts, and draws every tool in its own window on the graphics card.

It is meant for people who already use Cheat Engine, or want to learn how games keep their data in memory. Tables, scripts, templates and most keyboard shortcuts carry over. On top of that it reads Unity (Mono and IL2CPP), .NET, Unreal Engine and C++ RTTI games by itself, and it has its own checked Lua API for scripts and script windows.

> [!NOTE]
> Many games need the program to run as administrator (Help → Restart as Administrator). Protected processes and anti-cheat systems are not supported.

## Contents

- [Highlights](#highlights)
- [Features](#features)
  - [Scanner](#scanner)
  - [Address list and tables](#address-list-and-tables)
  - [Memory viewer and disassembler](#memory-viewer-and-disassembler)
  - [Debugger](#debugger)
  - [Auto Assembler and templates](#auto-assembler-and-templates)
  - [Structure Dissect](#structure-dissect)
  - [Pointer scan and static pointers](#pointer-scan-and-static-pointers)
  - [Speedhack](#speedhack)
  - [Game engines and runtimes](#game-engines-and-runtimes)
  - [Lua scripting](#lua-scripting)
  - [Script windows and the Form Editor](#script-windows-and-the-form-editor)
  - [Autorun modules](#autorun-modules)
  - [Interface, themes and settings](#interface-themes-and-settings)
  - [Log, error codes and diagnostics](#log-error-codes-and-diagnostics)
  - [Safety with tables from elsewhere](#safety-with-tables-from-elsewhere)
- [Using the program](#using-the-program)
- [Building](#building)
- [Testing](#testing)
- [Installing](#installing)
- [Documentation](#documentation)
- [License](#license)

## Highlights

- **Scanner**: all the usual value types, many threads, results on disk instead of in memory, an *All* scan over every number type at once, and grouped scans for values that sit next to each other.
- **Address list**: groups, pointers, freezing, hotkeys, drop-down values, and Cheat Engine's group options.
- **Memory viewer and debugger**: disassembler with symbols and jump arrows, a flow graph per function, write history with undo, conditional breakpoints that run inside the target, tracing, and "find out what accesses / writes".
- **Structure Dissect** that names objects by their class in Mono, IL2CPP, .NET and Unreal Engine games, and C++ objects by their RTTI.
- **Game engines built in**: Mono and IL2CPP (Unity) with classes, methods and a scene explorer, .NET Framework and .NET games (such as Terraria) read from outside the game, and Unreal Engine 4 and 5 without an SDK or game offsets.
- **Scripting**: Cheat Engine's Auto Assembler, templates, and Lua 5.4 with the Cheat Engine API, plus the program's own `manifold` API with script windows drawn in the program's look and a Form Editor to lay them out.
- **Made for long sessions**: error codes that explain themselves, a log window for debugging, session backups, a profiler, and the keyboard shortcuts of Cheat Engine 7.5.

## Features

This is an overview. [Docs/FEATURES.md](Docs/FEATURES.md) describes every window in detail, and [Docs/SHORTCUTS.md](Docs/SHORTCUTS.md) lists every keyboard shortcut.

### Scanner

| | |
| --- | --- |
| Value types | byte, 2/4/8 bytes, float, double, string (UTF-8 or UTF-16, with or without case), byte patterns with wildcards, and custom types registered by scripts |
| Scan types | exact, bigger, smaller, between, unknown initial value, increased/decreased (by), changed, unchanged, same as first scan; rounded, truncated or exact float compare |
| **All** | scans byte, 2/4/8 bytes, float and double in one pass; every result keeps its own type |
| **Grouped** | finds several values next to each other, such as `4:100 f:1.5 d:*`, with ranges (`10..20`), gaps (`skip:N`) and `within:N` for any order |
| Compare to | the previous scan, the first scan, or results saved under a name |
| Filters | writable, executable, copy-on-write, region types, start and stop address or one module, fast scan alignment, pausing the process |

Scans run on several threads and keep their results in temporary files, so hundreds of millions of results do not fill the memory. `F3` runs the first or next scan, `Shift+F3` starts a new scan and `Ctrl+Alt+Z` undoes the last one.

### Address list and tables

- Groups and nested records, freezing (normal, allow increase, allow decrease), pointers with any number of offsets, hex and signed display, drop-down value lists, colours and hotkeys.
- Cheat Engine's group options, copy and paste as Cheat Engine XML, and drag and drop.
- Addresses are address expressions: `"game.exe"+1A2B30`, `[[base]+8]+4`, registered symbols, exports, and `mainmodule` or `$process` for the executable's base.
- Relative addresses (`+X`, `-X`) count from the nearest record above that has an address, as in Cheat Engine.
- **Table Files** (`Ctrl+Alt+F`): the files stored inside a table (Lua modules, scripts, images, sounds) with a preview, an editor, drafts, and import and export. The setting Table File Names writes them the Cheat Engine 7.5 or 7.6 way.
- **Address List Debug** (`Ctrl+Alt+Shift+D`): every record with its address chain step by step, a check of the whole table (broken links, Lua lookups that find nothing, duplicate paths, hotkey clashes), the last 500 activations with their writes, and the list's internals. Meant for table authors.
- **Session backups** of unsaved changes every 5 minutes into `backups`, with File → Backups and an offer to restore after a run that did not end properly.
- View → Compact Mode shows only the address list; View → Release Mode is for using a finished table.

### Memory viewer and disassembler

As many windows as you like (`Ctrl+M`), each with its own File, Search, View, Debug and Tools menus like Cheat Engine's.

- Zydis disassembler with symbols, jump arrows and flow colours; assembling in place with NOP fill, replacing code with NOPs and restoring it.
- Hex view with live change highlighting and editing as bytes, integers, floats or doubles.
- **Flow graph** of the selected function (`G`): blocks, taken and not-taken edges, loops, a minimap, and Copy as Mermaid.
- **Function bounds** from unwind data or the Mono or IL2CPP method: `Home` and `End` go to the start and end, `Ctrl+Shift+A` selects the function.
- **Write History** of every memory edit made in the viewers, with undo and redo per edit.
- Byte search, assembly search (`Ctrl+Shift+F`), ten bookmarks, references (`X`), copying with names or module offsets, saving and loading regions, filling memory, allocating memory, creating a thread and injecting a DLL.
- **Comments** on any instruction (`Ctrl+Shift+C`), saved in the table, and a **Comment List** (`Ctrl+Alt+Shift+C`).
- **Opcode Converter** (`Ctrl+Alt+O`): bytes to instructions and back, for x64 or x86, at an address.
- Tools → Scan for Patches compares each module's code with its file on disk and lists hooks, injected jumps and patched bytes.

### Debugger

For 64-bit and 32-bit (WOW64) processes, through the Windows debugger or in-process (Settings → Debugger).

| | |
| --- | --- |
| Breakpoints | software and hardware breakpoints, data breakpoints, the Breakpoint List (`Ctrl+Alt+B`) |
| Conditions | a condition and register changes written as small expressions (`RAX`, `ZF`, `readInteger(RCX+8)`, `EAX = 0`), run inside the target, so a breakpoint that rarely matches barely costs anything (`Shift+F5`) |
| Stepping | Run (`F9`), Step Into (`F7`), Step Over (`F8`), Step Out (`Shift+F8`), Break into Process (`F12`), Toggle Breakpoint (`F5`) |
| Registers | changed values marked, symbols named, XMM registers as floats, doubles, integers or hex, flags explained, the outcome of the next conditional jump |
| Break and Trace | records the instructions a thread runs from a breakpoint as a call tree with registers per step (`Ctrl+T`) |
| Find out what… | what accesses or writes an address (`F5` / `F6` in the address list), what addresses an instruction accesses, what calls an address |
| Thread List | start address, TEB, priority and threads that hide from debuggers (`Ctrl+Shift+T`) |

### Auto Assembler and templates

- Cheat Engine's script language with `[ENABLE]`/`[DISABLE]`, `alloc`, `label`, `define`, `aobscan`, `assert`, `registersymbol`, `readmem`, `reassemble`, `include`, `loadlibrary`, `createthread`, `{$lua}` blocks and more. See [Docs/AUTO-ASSEMBLER.md](Docs/AUTO-ASSEMBLER.md).
- Editors open as tabs (`Ctrl+Alt+A`), with syntax check, completion, Find (`Ctrl+F`) and Replace (`Ctrl+H`).
- **Templates**: AOB injection, code injection, full injection, cheat table framework and Call Lua are built in; your own go into the `templates` folder. The Templates window edits them with a highlighted preview. Templates of the Manifold Template Loader for Cheat Engine work unchanged. See [Docs/TEMPLATES.md](Docs/TEMPLATES.md).

### Structure Dissect

`Ctrl+Alt+D`, or Dissect Data on any address.

- Guesses pointers, floats, strings and integers, follows pointers into nested structures, and compares several addresses side by side.
- Names objects by their class and fills in real field names, offsets and types when a Mono, IL2CPP, .NET or Unreal runtime is loaded, or the program has C++ RTTI. Generic types, arrays and embedded structs are read too.
- A filter over name, offset, type or value; Go to Address (`Ctrl+G`); the pointer path of the selected field, ready to copy.
- Several windows, or tabs in one; structures are saved in the table.

### Pointer scan and static pointers

- **Pointer Scan** (`Ctrl+Alt+P`) finds chains like `[["game.exe"+offset]+18]+34` that lead to an address, up to 12 levels. **Rescan** after a restart keeps the paths that still lead to the value. Paths are saved as `.mptr` files and added to the address list with a double-click. 64-bit and 32-bit processes.
- **Find Static Pointers** (`Ctrl+Alt+K`) is a quick single-level search of the modules' writable sections for pointers to an address or a structure start.

### Speedhack

Makes the game's time run faster or slower: the switch is at the bottom of the scan panel (`Ctrl+Shift+H`), `Ctrl+Alt+Up` and `Ctrl+Alt+Down` step through the usual factors, and scripts use `speedhack_setSpeed` or `manifold.speed`.

It hooks `QueryPerformanceCounter`, `GetTickCount`, `GetTickCount64` and `timeGetTime` through the agent library, so clocks never jump or run backwards when the speed changes. The game goes back to normal speed by itself when Manifold Engine closes or crashes. Details are in [Docs/FEATURES.md](Docs/FEATURES.md).

### Game engines and runtimes

| Runtime | What you get | Where |
| --- | --- | --- |
| **Unity, Mono** | classes, methods and field offsets from the runtime; `Assembly-CSharp:Player:TakeDamage` resolves to the compiled method; a scene explorer and object inspector on the game's main thread | Extensions → Unity (Mono / IL2CPP), Mono / .NET window (`Ctrl+Alt+M`), `mono_*` Lua functions |
| **Unity, IL2CPP** | the same through the `il2cpp_*` functions: classes, methods with their code addresses, field offsets, and the scene explorer | the same menu, window and functions; `mono_isIl2Cpp()` tells them apart |
| **.NET** | .NET Framework and .NET 5 and later, read from outside the game by a helper process: classes, static fields as `Class:Field`, compiled methods, the GC heap and its objects | Extensions → .NET, the Mono / .NET window, `manifold.dotnet` |
| **Unreal Engine 4 and 5** | names, objects and reflection found in the running game without an SDK or offsets; real class layouts in Structure Dissect, `UE FName` and `UE FString` types, `Actor_RootComponent`-style symbols, a class browser | Extensions → Unreal Engine (`Ctrl+Alt+U`), `manifold.unreal` |
| **C++ RTTI** | class names of MSVC and clang-cl programs in Structure Dissect, `<Class>::vftable` symbols, base classes and instances | Extensions → C++ RTTI, `manifold.rtti` |
| **Godot 4** | StringNames, ObjectDB, ClassDB, the scene tree and method symbols | the `Manifold-Godot` autorun module |

All of them work in 64-bit and 32-bit games, and none needs a native DLL injected for reading.

### Lua scripting

Lua 5.4 with two APIs side by side:

- **The Cheat Engine API**: memory, symbols, AOB scans, `autoAssemble`, the address list and its records, timers, hotkeys, `createThread`, breakpoints with callbacks, menu items, and `createForm` windows drawn with Lazarus' layout rules. See [Docs/LUA-API.md](Docs/LUA-API.md). The global functions of Cheat Engine 7.5's Lua documentation are provided; [Docs/CE-COMPAT.md](Docs/CE-COMPAT.md) lists the exceptions that have no effect or are not available.
- **The `manifold` API**: the program's own namespace with one fixed, checked schema. Wrong calls raise an `ME-` error that names the argument, and failures return `nil, error` with the script line. It covers the log with channels, process and typed memory, records by id, description or path (`manifold.record.find({ "Player", "Stats", "Health" })`), symbols, structures, Unreal, RTTI, .NET, the speedhack, modules and the UI. See [Docs/MANIFOLD-API.md](Docs/MANIFOLD-API.md).

The Lua Engine window (`Ctrl+L`) has a console with coloured output, a script editor with completion, signature help and a documentation panel (`Ctrl+Shift+F`).

### Script windows and the Form Editor

- **`manifold.ui`** builds windows in Manifold's own look: rows, grids, cards, tabs, tables, trees, radio groups, colour pickers and canvases, plus dialogs, notices with progress, and entries in the Extensions menu, title bar and status bar. Layouts adapt to narrow sizes, and a script's windows go away when it runs again.
- **Form Editor** (`Ctrl+Alt+G`): lays out such a window by hand with a palette, an outline, a live preview and the handler functions shown as chips beside the form. Widgets can be bound to records of the table, follow a switch without code (Enabled When) and take hotkeys that work while the game has the focus.
- File → New Trainer from the Table turns the open table into a trainer form in one step, and File → Import Cheat Engine Form converts a Cheat Engine form.
- Examples: [Examples/Manifold-UI-Demo.lua](Examples/Manifold-UI-Demo.lua) shows the widgets, [Examples/Form-Demo.lua](Examples/Form-Demo.lua) loads [Examples/Form-Demo.form.lua](Examples/Form-Demo.form.lua), and [Examples/Manifold-Snake.lua](Examples/Manifold-Snake.lua) is a small game on a canvas.

### Autorun modules

The Lua scripts in the `autorun` folder next to the program run at start: files in its root as in Cheat Engine, and modules in `autorun\modules` with a header that names their version and requirements, so the load order follows from it. `require` finds a module's own files, other modules and `autorun\lib`.

- Extensions → Modules lists every module with its state, version, load time and errors, and switches, reloads and allows them; scripts use `manifold.module`.
- A script that is new in the folder is held until it is allowed, so a table cannot plant one unseen.
- Shipped modules: **Manifold-Godot** (Godot 4) and **Manifold-SigMaker** (byte signatures for the selected instruction).

See [Docs/AUTORUN.md](Docs/AUTORUN.md), and [Docs/AUTORUN-DESIGN.md](Docs/AUTORUN-DESIGN.md) for the design behind it.

### Interface, themes and settings

- Built on Dear ImGui with docking and multiple viewports on Direct3D 11: every tool window can move to another screen, and with Shift held it docks into another window or the main window.
- Themes **Standard**, **Red**, **Green** and **Blue**, each in light and dark, with a **System** mode that follows Windows; accent colours, colour overrides per theme and mode, and sizes (Settings → Appearance).
- Floating tool windows can be see-through, with one opacity while they have the focus and another while the game has it.
- Settings (`Ctrl+,`) has the pages General, Appearance, Scanner, Scripts, Debugger and Keyboard. Every shortcut can be changed there; Help → Keyboard Shortcuts (`F1`) lists them with a search.
- The open process shows as a chip with its icon in the title bar; script menus gather in the Extensions menu.

### Log, error codes and diagnostics

- **Log** (`Ctrl+Shift+L`): wrapping entries, repeats counted, filters by level, area and text, a details pane with milliseconds, thread and source line. Everything is also written to `logs\Manifold-Engine.log`.
- **Error codes**: every failure ends in a code like `ME-1006`; hovering it shows the cause and what helps. The list is in [Docs/ERRORS.md](Docs/ERRORS.md) and under Help → Error Codes.
- **Profiler and Diagnostics** (`Ctrl+Shift+F12`): frame graph, frame breakdown, threads, checks of the setup (rights, graphics driver, display refresh, overlays) and app info, with Copy Report. `--profile-report=report.txt` measures and writes a report from the command line.
- A crash writes a minidump and a short report into `crashes`.

### Safety with tables from elsewhere

- A table's Lua script runs only when you allow it (Settings → Scripts: ask, always, never). Until then its hotkeys cannot switch scripts.
- Starting programs, injecting DLLs and setting environment variables from a script are logged as warnings with the script line.
- A table is read with limits: groups nested deeper than 32 levels are cut off, and a file inside it may unpack to 256 MB at most (512 MB for all of them).
- Check in a Lua script window compiles the script without running any of it.
- When the program closes with scripts active in the game, it asks whether to turn them off first.

## Using the program

```powershell
Manifold-Engine.exe game.CT --process=game.exe
```

opens a table and a process at start. `--open=` opens windows at start, such as `--open=memory,lua,log`; [Docs/FEATURES.md](Docs/FEATURES.md) lists all names. Dropping a `.CT` file on the main window opens it, with Shift held it is merged into the current table.

The program keeps its files in folders next to it:

| Folder | Holds |
| --- | --- |
| `configs` | settings (`Manifold-Engine.ini`), the window layout and what scripts store with `getSettings` |
| `tables` | where the dialogs to open and save tables start |
| `autorun`, `templates` | autorun scripts and modules, your Auto Assembler templates |
| `sounds` | sound files that replace the built-in ones |
| `backups` | session backups |
| `logs`, `crashes` | the log of this run and the one before, crash reports |

Some useful keys: `Ctrl+P` opens a process, `Ctrl+O` a table, `Ctrl+M` the memory viewer, `Ctrl+Alt+A` the Auto Assembler, `Ctrl+L` the Lua Engine. The full list is in [Docs/SHORTCUTS.md](Docs/SHORTCUTS.md).

## Building

Visual Studio 2022 with the C++ desktop workload and, under individual components, *C++ Clang Compiler for Windows* and *MSBuild support for LLVM (clang-cl) toolset*. Python 3 and the .NET SDK should be on PATH: Python writes the generated sources and the build info, and `dotnet` builds the managed dumper for Mono games.

```powershell
msbuild Manifold-Engine.sln /p:Configuration=Release /p:Platform=x64 /m
```

The program lands in `Build\x64\Release\Manifold-Engine.exe`, next to the 32-bit helpers it needs for 32-bit games (`ManifoldAgent32.dll`, `ManifoldClr32.exe`). The program itself is x64-only and needs clang-cl; MSVC cannot build it. New source files are picked up with `python Tools/gen_projects.py`.

More in [Docs/DEVELOPMENT.md](Docs/DEVELOPMENT.md), and the code's layers and threads in [Docs/ARCHITECTURE.md](Docs/ARCHITECTURE.md).

## Testing

```powershell
Build\x64\Release\Manifold-Engine.Tests.exe
```

Run it from the repository folder. `--list` names the suites and `--filter=core,text` runs only those. The tests also check that the generated files `Docs/ERRORS.md`, `Docs/SHORTCUTS.md`, `Docs/MANIFOLD-API.md` and `Docs/CE-COMPAT.md` are up to date, so change the sources of those, not the files. CI builds and tests Release and Debug on every push to `main`.

## Installing

```powershell
.\Tools\Install.ps1
```

copies the Release build into `Install` (or `-Destination`). The program there keeps its `configs`, `tables`, `templates`, `logs` and the other folders apart from the build; later installs replace only the program files, and the script refuses while that copy of the program runs.

## Documentation

| File | Content |
| --- | --- |
| [Features](Docs/FEATURES.md) | everything the program does, window by window |
| [Shortcuts](Docs/SHORTCUTS.md) | every keyboard shortcut (generated) |
| [Error codes](Docs/ERRORS.md) | every `ME-` code with cause and help (generated) |
| [Auto Assembler](Docs/AUTO-ASSEMBLER.md) | the script language |
| [Templates](Docs/TEMPLATES.md) | Auto Assembler templates and their settings |
| [Lua API](Docs/LUA-API.md) | the Cheat Engine compatible Lua API |
| [manifold API](Docs/MANIFOLD-API.md) | the `manifold.*` namespace (generated) |
| [Cheat Engine coverage](Docs/CE-COMPAT.md) | Cheat Engine functions without effect or not available (generated) |
| [Autorun modules](Docs/AUTORUN.md), [design](Docs/AUTORUN-DESIGN.md) | the autorun folder, module headers, load order, `require` and the shipped modules |
| [Development](Docs/DEVELOPMENT.md) | building, tests, project layout, CI |
| [Architecture](Docs/ARCHITECTURE.md) | layers, threads, and where new code goes |
| [UI layout](Docs/UI-LAYOUT.md) | the layout system every window is drawn on, its audit and the lock on raw ImGui calls |

## License

MIT, see [LICENSE](LICENSE). Third-party code (Dear ImGui, Zydis, Lua, stb, Font Awesome) keeps its own license, see [Licenses](Licenses).

Manifold Engine is an independent project. It is not affiliated with or endorsed by Cheat Engine or its author; the name Cheat Engine is used only to describe compatibility.
