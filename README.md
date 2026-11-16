# Manifold Engine

A memory scanner, debugger, assembler and Lua scripting tool for Windows processes, in the spirit of Cheat Engine. It is built on the Manifold UI (Dear ImGui with docking and multi-viewport on Direct3D 11), so every tool runs in its own window that can be moved to another screen, and windows can be docked into each other while Shift is held.

It reads and writes Cheat Engine tables (`.CT`): records, groups, pointers, scripts, hotkeys, drop-down lists, structures, registered symbols and the table's Lua script.

## What it does

- **Memory scanner**: first and next scans for byte, 2/4/8 bytes, float, double, string (UTF-8 or UTF-16, with or without case) and byte patterns with wildcards. Exact, bigger, smaller, between, unknown initial value, increased/decreased (by), changed, unchanged, same as first scan. Rounded, truncated or exact float compare. Writable/executable/copy-on-write filters, region types, start and stop address or one module, fast scan alignment, pausing the process. Scans run on several threads and keep their results in temporary files, so hundreds of millions of results do not fill the memory. Undo the last scan.
  - **All** scans byte, 2/4/8 bytes, float and double in one pass (Settings → Scanner → Types for All picks which); every result keeps its own type.
  - **Grouped** finds several values that lie next to each other, written as `type:value` separated by spaces: `4:100 f:1.5 d:*`. Types are `1`, `2`, `4`, `8` (bytes), `f` (float), `d` (double), `p` (pointer), `s` (text), `u` (UTF-16 text) and `skip:N` (N bytes in between). A value is a number, `*` for any value, or a range like `10..20`; text with spaces goes in quotes. With `within:N` the values after the first one may lie anywhere in the N bytes from the first, in any order. Adding a result puts each value into the address list under a group.
  - **Custom types** registered by scripts (`registerCustomTypeLua`) appear in the value type list; they scan for an exact value, unknown initial value, changed, unchanged and same as first scan.
  - **Compare to**: a next scan that looks at the earlier value (increased, decreased, changed, unchanged) compares with the previous scan, the first scan, or results saved under a name with the Save button beside it. Saved results stay until New scan.
- **Address list**: groups and nested records, freezing (normal, allow increase, allow decrease), pointers with any number of offsets, hex and signed display, drop-down value lists, colors, hotkeys (toggle, activate, deactivate, set, increase, decrease), drag and drop, copy and paste as Cheat Engine XML, and the group options of Cheat Engine: hide children when deactivated, activate and deactivate children as well, set the value of children, always hide children, manual collapse.
- **Memory viewer** (as many windows as you like, each with its own File, Search, View, Debug and Tools menus like Cheat Engine's): Zydis disassembler with symbols, jump arrows and flow colors, assembling in place with NOP fill, replacing code with NOPs and restoring it, hex view with live change highlighting and in-place editing as bytes, integers, floats or doubles, go to address, byte search (the next match, or every match in a list that stays open and moves the viewer to the one you pick), bookmarks, undo of the window's memory edits, saving a region to a file and loading a file into memory, filling memory, creating a thread, allocating memory and injecting a DLL. Scripts from any instruction through templates. View → Show symbols switches names on or off: with it on, every place a symbol starts (a registered symbol, an export, or a Mono method) gets a header line, the instructions below it show their offset from that start (`+0`, `+3`, ...), call and jump targets show their symbol names (also `mov r11, <method>` before an indirect call), and addresses read as module+offset; when the listing starts inside a function, that function's name stays pinned as the first row while scrolling (a click goes to its start); off shows plain addresses. File → Load Mono runtime reads a Mono game's runtime without opening the Mono window.
- **Debugger**: software and hardware breakpoints, break, step into, step over, run until return, editing registers and flags, stack view, debug output of the process in the log. The register panel beside the code (its width can be dragged) marks values that changed since the last break, names the symbol a register points to, and shows XMM0–XMM15 (XMM0–XMM7 for 32-bit processes) and MXCSR as floats, doubles, integers or hex; a double click changes any register. Each flag explains itself in a tooltip (meaning, which instructions read it, its value at the last stop). When the instruction at RIP is a conditional jump, cmov or set, the flags it reads are outlined and the panel says whether it will jump, move or set. During a step the panel and the stack keep the last values instead of disappearing. The stack sits beside the hex view in the same column, with the offset from RSP, the value, the symbol it points to and the slot RBP points to.
  - **Conditional breakpoints and register changes**: a breakpoint can carry a condition and a set of register changes, both written as small Lua-like expressions over the registers (`RAX`, `EAX`, `AL`), the flags (`ZF`), memory reads (`readInteger(RCX+8)`, `[RSP]`) and symbols. The process stops only when the condition holds; a breakpoint can also just change registers (`EAX = 0`, `RIP = RIP + 5`) or flags and keep running, or fire only in one thread. Data breakpoints (break when an address is read or written) carry the same conditions. The conditions and changes run inside the target, so a breakpoint that rarely matches barely costs anything.
  - **Breakpoint list**: every breakpoint with its type, condition, changes and hit count in one window, to add, edit or remove.
  - **Thread list**: the threads of the process with their start address, TEB, priority, and whether a thread hides itself from debuggers.
  - **Break and trace**: set a breakpoint and record the instructions the thread runs from it, up to a chosen number of steps, stepping into or over calls, with an optional start and stop condition. Calls and returns are indented into a tree and each step keeps its registers.
  - **Comments**: a note on any instruction, shown in the disassembler's comment column and saved in the table.
- **Find out what…**: what accesses an address, what writes to it, what addresses an instruction accesses, and what calls an address. Each runs in its own window with hit counts and the registers of the last hit.
- **Structure dissect**: guesses pointers, floats, strings and integers, follows pointers into nested structures, compares several addresses side by side, edits names, types and values. Structures are saved in the table. When a Mono runtime is loaded (see Mono / .NET), dissecting the address of a Mono object names it by its class and fills in the real field names, offsets and types straight from the runtime, and following a pointer to another object does the same — no manual structure building.
- **Auto Assembler**: Cheat Engine's script language with `[ENABLE]`/`[DISABLE]`, `alloc`, `label`, `define`, `aobscan(module|region)`, `assert`, `registersymbol`, `readmem`, `reassemble`, `include`, `loadlibrary`, `createthread`, `{$lua}` blocks and more. See [Docs/AUTO-ASSEMBLER.md](Docs/AUTO-ASSEMBLER.md).
- **Script templates**: AOB injection, code injection, full injection and more as files in the `templates` folder, sorted into categories by folder or by their settings, with inputs, includes and a preview. Templates of the Manifold Template Loader for Cheat Engine work unchanged. See [Docs/TEMPLATES.md](Docs/TEMPLATES.md).
- **Lua**: Lua 5.4 with a Cheat-Engine-compatible API: memory, symbols, AOB scans, `autoAssemble`, the address list and its records, timers, hotkeys, `createThread` with a non-blocking `sleep`, debugger breakpoints with callbacks. Menu items that scripts add to the memory viewer show up in its context menu and menu bar, and items added to the main form's menu (like the Unreal Engine menu of Manifold-UE.lua) show up in the main window's menu bar or in its File, Edit, Table and Help menus, and windows that scripts build with `createForm` are drawn with Lazarus' layout rules: panels, labels, buttons, text fields, lists, tree views, splitters, menu bars, context menus and paint boxes, whose canvas drawing is replayed by the graphics card. See [Docs/LUA-API.md](Docs/LUA-API.md).
- **Mono / .NET**: for Mono-based Unity games (those with `mono-2.0-bdwgc.dll` or `mono.dll`) the classes, methods and field offsets are read from the runtime, and an address like `Assembly-CSharp:PlayerController:TakeDamage` resolves to the JIT-compiled code so scripts and the Auto Assembler can target a method without an AOB scan. This works without injecting a DLL: a small worker is written into an executable page and run on one thread attached to the Mono domain, and the `mono_*` functions are called through it. When the game has a Mono runtime, a Mono menu shows in the main window's title bar to load it, reload its classes and open the Mono window. The Lua side has `mono_isPresent`, `mono_enumClasses`, `mono_class_getMethods`, `mono_class_getFieldOffset` and `mono_getAddress`. IL2CPP games have no Mono runtime and are not covered.
- **Autorun**: every `.lua` file in the `autorun` folder next to the program runs at start. Processes can be opened automatically when they start (Settings).
- **Logs and error codes**: a log window with levels, search and the source line of each entry, also written to `Manifold-Engine.log`. Every failure ends in a code like `ME-1006`; hovering it shows the cause and what helps. The list is in [Docs/ERRORS.md](Docs/ERRORS.md) and in the program (Help → Error codes). A crash writes a minidump and a short report into `crashes`.
- **Keyboard shortcuts** for the features of every window, the same keys as Cheat Engine 7.5 where it has one (Ctrl+M memory viewer, Ctrl+Alt+A Auto Assembler, F5/F6 what accesses/writes, Ctrl+B browse, Ctrl+D disassemble, Ctrl+G go to, F7/F8/F9 stepping, Ctrl+I code injection template and so on) and own keys for the rest. Menus, context menus and button tooltips show them. Keys of the focused window win, so one key can mean the right thing in each window. Settings → Keyboard changes any of them (record new keys, add a second shortcut, remove or reset; it warns when two commands of one window share keys); changes go into `Manifold-Engine.ini` under `[Shortcuts]`. The default list is in [Docs/SHORTCUTS.md](Docs/SHORTCUTS.md), the current one in the program (Help → Keyboard shortcuts, F1).
- Symbols and modules (registered symbols, modules, exports), memory regions, settings.

## Building

Visual Studio 2022 with the C++ desktop workload and clang-cl (C++20). In the Visual Studio Installer, under individual components, add:

- C++ Clang Compiler for Windows
- MSBuild support for LLVM (clang-cl) toolset

1. Open `Manifold-Engine.sln`.
2. Build `Release|x64`.

Or from a developer prompt:

```bash
msbuild Manifold-Engine.sln /p:Configuration=Release /p:Platform=x64 /m
```

Everything lands in `Build\<Platform>\<Configuration>`, e.g. `Build\x64\Release`. All dependencies are in `Dependencies` and build with the solution: Dear ImGui 1.92 (docking), Zydis 5 (amalgamated), Lua 5.4. New source files are picked up by running `python Tools/gen_projects.py`, which writes the projects in `Projects` and the solution.

The solution also carries a `Win32` platform, but only the dependencies and the stand-alone `SyscallTest` project build as 32-bit; the app and the injected agent are x64-only.

Without clang-cl, build with the MSVC compiler instead; inline assembly then does not compile:

```bash
msbuild Manifold-Engine.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
```

`python Tools/gen_projects.py --msvc` writes projects that use MSVC by default.

## Tests

```bash
Build\x64\Release\Manifold-Engine.Tests.exe
```

The tests cover the assembler (557 cases), value parsing, XML and cheat table round trips, the error table, offline Auto Assembler runs, and an integration run against `TestTarget.exe` (built with the solution): scanning, pointers, freezing, a code injection script, the sample table `Tests/Target/TestTarget.CT`, Lua (records, timers, threads), all four "find out what" watches, breakpoints and stepping. `--errors-markdown` prints `Docs/ERRORS.md`, `--shortcuts-markdown` prints `Docs/SHORTCUTS.md`; the tests fail when either file is out of date, and when two commands of one window share a key.

## Use

- Many games run with administrator rights or protect their memory; start the program as administrator (Help → Restart as administrator) when a process cannot be opened. Protected processes and anti-cheat systems cannot be opened at all.
- `Manifold-Engine.exe game.CT --process=game.exe` opens a table and a process at start.
- Shortcuts: F1 lists all of them. The main window has the table, process and window menus (File, Edit, Process, Table, View, Tools, Help); code, memory and debugger tools sit in the memory viewer's own menus, as in Cheat Engine. Commands are defined once in `Source/App/Commands.cpp`; a window asks `Pressed(Command::…)` and shows the keys with `CommandItem`.
- Settings, window layout and the log are kept next to the program (`Manifold-Engine.ini`, `.layout.ini`, `.log`); what scripts store with `getSettings` goes into `Manifold-Engine.scripts.ini`.
- View → Compact mode shows only the address list, without the scanner.
- View → Release mode is for using a finished table: the address list shows only Active, Description and Value, the Tools menu and the technical menu entries are gone (Edit keeps only Settings), a double click turns a script on or off, and messages leave out their error codes. The same menu switches it back.
- Scripts that only run Lua, such as one that starts the game, work without an opened process. Scripts that need the game say so when they are turned on.
- When the program closes while scripts are active in the game, it asks whether to turn them off first, so nothing stays behind in the game. Settings → Scripts can make that choice permanent.

## Layout

| Folder | Content |
| --- | --- |
| `Source/App` | program, window host, settings, the engine that ties the parts together, crash handler |
| `Source/Core` | error codes, log, text helpers, XML |
| `Source/Target` | process access, process list, symbols and address expressions |
| `Source/Disasm`, `Source/Asm` | Zydis disassembler, assembler and Auto Assembler |
| `Source/Templates`, `Templates` | template engine, its values and the built-in template files (`Tools/embed_templates.py` puts them into `Source/Generated/BuiltinTemplates.cpp`) |
| `Source/Scan` | value types and the scanner |
| `Source/Debug` | debugger, breakpoints and watches |
| `Source/Table` | address list, cheat table files, structures |
| `Source/Lua` | Lua engine and API |
| `Source/UI` | main window and the tool windows |
| `Tests` | test suites and the test target with its table |
| `Dependencies`, `Licenses` | third-party code and its licenses |
