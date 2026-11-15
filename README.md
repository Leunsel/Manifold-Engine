# Manifold Engine

A memory scanner, debugger, assembler and Lua scripting tool for Windows processes, in the spirit of Cheat Engine. It is built on the Manifold UI (Dear ImGui with docking and multi-viewport on Direct3D 11), so every tool runs in its own window that can be moved to another screen, and windows can be docked into each other while Shift is held.

It reads and writes Cheat Engine tables (`.CT`): records, groups, pointers, scripts, hotkeys, drop-down lists, structures, registered symbols and the table's Lua script.

## What it does

- **Memory scanner**: first and next scans for byte, 2/4/8 bytes, float, double, string (UTF-8 or UTF-16, with or without case) and byte patterns with wildcards. Exact, bigger, smaller, between, unknown initial value, increased/decreased (by), changed, unchanged, same as first scan. Rounded, truncated or exact float compare. Writable/executable/copy-on-write filters, region types, start and stop address or one module, fast scan alignment, pausing the process. Scans run on several threads and keep their results in temporary files, so hundreds of millions of results do not fill the memory. Undo the last scan.
- **Address list**: groups and nested records, freezing (normal, allow increase, allow decrease), pointers with any number of offsets, hex and signed display, drop-down value lists, colors, hotkeys (toggle, activate, deactivate, set, increase, decrease), drag and drop, copy and paste as Cheat Engine XML, and the group options of Cheat Engine: hide children when deactivated, activate and deactivate children as well, set the value of children, always hide children, manual collapse.
- **Memory viewer** (as many windows as you like): Zydis disassembler with symbols, jump arrows and flow colors, assembling in place with NOP fill, replacing code with NOPs and restoring it, hex view with live change highlighting and in-place editing as bytes, integers, floats or doubles, go to address, byte search, allocating memory and injecting a DLL. Script templates (AOB injection, code injection, full injection) from any instruction.
- **Debugger**: software and hardware breakpoints, break, step into, step over, run until return, editing registers and flags, stack view, debug output of the process in the log.
- **Find out what…**: what accesses an address, what writes to it, what addresses an instruction accesses, and what calls an address. Each runs in its own window with hit counts and the registers of the last hit.
- **Structure dissect**: guesses pointers, floats, strings and integers, follows pointers into nested structures, compares several addresses side by side, edits names, types and values. Structures are saved in the table.
- **Auto Assembler**: Cheat Engine's script language with `[ENABLE]`/`[DISABLE]`, `alloc`, `label`, `define`, `aobscan(module|region)`, `assert`, `registersymbol`, `readmem`, `reassemble`, `include`, `loadlibrary`, `createthread`, `{$lua}` blocks and more. See [Docs/AUTO-ASSEMBLER.md](Docs/AUTO-ASSEMBLER.md).
- **Lua**: Lua 5.4 with a Cheat-Engine-compatible API: memory, symbols, AOB scans, `autoAssemble`, the address list and its records, timers, hotkeys, `createThread` with a non-blocking `sleep`, debugger breakpoints with callbacks. See [Docs/LUA-API.md](Docs/LUA-API.md).
- **Autorun**: every `.lua` file in the `autorun` folder next to the program runs at start. Processes can be opened automatically when they start (Settings).
- **Logs and error codes**: a log window with levels, search and the source line of each entry, also written to `Manifold-Engine.log`. Every failure ends in a code like `ME-1006`; hovering it shows the cause and what helps. The list is in [Docs/ERRORS.md](Docs/ERRORS.md) and in the program (Help → Error codes). A crash writes a minidump and a short report into `crashes`.
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

Everything lands in `Build\Release`. All dependencies are in `Dependencies` and build with the solution: Dear ImGui 1.92 (docking), Zydis 5 (amalgamated), Lua 5.4. New source files are picked up by running `python Tools/gen_projects.py`, which writes the projects in `Projects` and the solution.

Without clang-cl, build with the MSVC compiler instead; inline assembly then does not compile:

```bash
msbuild Manifold-Engine.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
```

`python Tools/gen_projects.py --msvc` writes projects that use MSVC by default.

## Tests

```bash
Build\Release\Manifold-Engine.Tests.exe
```

The tests cover the assembler (557 cases), value parsing, XML and cheat table round trips, the error table, offline Auto Assembler runs, and an integration run against `TestTarget.exe` (built with the solution): scanning, pointers, freezing, a code injection script, the sample table `Tests/Target/TestTarget.CT`, Lua (records, timers, threads), all four "find out what" watches, breakpoints and stepping. `--errors-markdown` prints `Docs/ERRORS.md`.

## Use

- Many games run with administrator rights or protect their memory; start the program as administrator (Help → Restart as administrator) when a process cannot be opened. Protected processes and anti-cheat systems cannot be opened at all.
- `Manifold-Engine.exe game.CT --process=game.exe` opens a table and a process at start.
- Shortcuts: Ctrl+P process list, Ctrl+O open table, Ctrl+S save, Ctrl+M memory viewer, Ctrl+L Lua engine. In the address list: Space toggles, Enter changes the value, Del deletes, Ctrl+C/Ctrl+V copy and paste. In the memory viewer: Ctrl+G go to, Ctrl+F find, Enter assemble, Space follow, Backspace back, F5 breakpoint, F7 step into, F8 step over, Shift+F8 run until return, F9 run, Ctrl+A Auto Assembler.
- Settings, window layout and the log are kept next to the program (`Manifold-Engine.ini`, `.layout.ini`, `.log`).
- View → Compact mode shows only the address list, without the scanner.
- View → Release mode is for using a finished table: the address list shows only Active, Description and Value, the Edit, Tools and Debug menus and the technical menu entries are gone, a double click turns a script on or off, and messages leave out their error codes. The same menu switches it back.
- Scripts that only run Lua, such as one that starts the game, work without an opened process. Scripts that need the game say so when they are turned on.
- When the program closes while scripts are active in the game, it asks whether to turn them off first, so nothing stays behind in the game. Settings → Scripts can make that choice permanent.

## Layout

| Folder | Content |
| --- | --- |
| `Source/App` | program, window host, settings, the engine that ties the parts together, crash handler |
| `Source/Core` | error codes, log, text helpers, XML |
| `Source/Target` | process access, process list, symbols and address expressions |
| `Source/Disasm`, `Source/Asm` | Zydis disassembler, assembler, Auto Assembler and its templates |
| `Source/Scan` | value types and the scanner |
| `Source/Debug` | debugger, breakpoints and watches |
| `Source/Table` | address list, cheat table files, structures |
| `Source/Lua` | Lua engine and API |
| `Source/UI` | main window and the tool windows |
| `Tests` | test suites and the test target with its table |
| `Dependencies`, `Licenses` | third-party code and its licenses |
