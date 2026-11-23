# Development

## Building

Visual Studio 2022 with the C++ desktop workload and clang-cl (C++20). In the Visual Studio Installer, under individual components, add:

- C++ Clang Compiler for Windows
- MSBuild support for LLVM (clang-cl) toolset

The build also uses two tools from PATH:

- Python 3, for the scripts in `Tools` that write the generated sources and the build info.
- The .NET SDK (`dotnet`), to build the managed dumper for Mono games (`Managed`).

The program builds without either, with a warning, but then something is missing: without the .NET SDK the build has no managed dumper and the tests report that as skipped, which fails the run; without Python the generated sources are not refreshed and the build info has no commit.

1. Open `Manifold-Engine.sln`.
2. Build `Release|x64`.

Or from a developer prompt:

```bash
msbuild Manifold-Engine.sln /p:Configuration=Release /p:Platform=x64 /m
```

Everything lands in `Build\<Platform>\<Configuration>`, e.g. `Build\x64\Release`. All dependencies are in `Dependencies` and build with the solution: Dear ImGui 1.92 (docking), Zydis 5 (amalgamated), Lua 5.4. New source files are picked up by running `python Tools/gen_projects.py`, which writes the projects in `Projects` and the solution.

The program is x64-only. Every x64 build also builds the in-process debugger agent, the .NET helper and the test target a second time as 32-bit, as `ManifoldAgent32.dll`, `ManifoldClr32.exe` and `TestTarget32.exe` next to the program: the program injects the 32-bit agent into 32-bit games, reads 32-bit .NET games with the 32-bit helper, and the tests use the 32-bit target. The solution also lists a `Win32` platform, which that second build uses; building the whole solution as `Win32` is not something CI or the tests do.

The MSVC compiler (`v143`) cannot build the program: the direct system call stubs in `Source/Syscalls/Syscall.h` are naked functions with inline assembly, which only clang-cl compiles. `Tools/gen_projects.py --msvc` writes such projects all the same, for trying parts of the code with MSVC.

### Generated sources

- Before compiling, the projects run `Tools/embed_lua.py`, `Tools/embed_templates.py`, `Tools/embed_modules.py` and `Tools/gen_buildinfo.py` with the Python on PATH. A script that fails stops the build.
- `gen_buildinfo.py` writes the commit into the build, so the log, Help → About and the diagnostics report show the version with it, like `0.1.0 (3e90571)`. A `+` after the commit means the working tree had uncommitted changes.
- The `ManagedDumper` project builds `Managed` with `dotnet` and writes `Source/Generated/ManagedDumperImage.cpp`. It is the one generated source that is not committed, because it holds the built assembly.

The build warns when a tool is missing:

| Warning | Meaning |
| --- | --- |
| ME0001 | Python was not found: the committed generated sources are used as they are. |
| ME0002 | The .NET SDK was not found: the program is built without the managed dumper. |
| ME0003 | Python was not found and there is no `ManagedDumperImage.cpp` yet: an empty one is copied from `Tools/ManagedDumperImage.empty.cpp`. |

## Tests

```bash
Build\x64\Release\Manifold-Engine.Tests.exe
```

The run goes through these suites in order; `--list` names them and `--filter=core,text` runs only those:

| Suite | Covers |
| --- | --- |
| `assembler`, `core`, `text`, `work` | the assembler, value parsing, XML and cheat table round trips, the limits for tables from elsewhere, the error table, offline Auto Assembler runs, syntax highlighting, JSON and the editor's text buffer, background work |
| `disasm`, `flowgraph`, `relays` | the disassembler, function bounds and the flow graph, relays in a module header and symbol offsets |
| `unreal` | the Unreal Engine reader against a made-up name table, object array and reflection: in the tests' own process, and with the 64-bit and the 32-bit layout inside `TestTarget.exe --unreal` and `TestTarget32.exe --unreal` |
| `rtti` | the C++ run-time type information reader: the name demangler, hand-built RTTI (single, multiple and virtual inheritance, an embedded member, locators that must be rejected) and classes the compiler made, the Structure Dissect labels, the symbols, `manifold.rtti` and `ManifoldRTTI`: in the tests' own process and inside `TestTarget.exe --rtti` and `TestTarget32.exe --rtti` |
| `commands`, `docs`, `console`, `templates` | commands and shortcuts, the generated documentation, the Lua console, script templates |
| `compat`, `manifold-api`, `modules`, `autorun` | the Cheat Engine Lua API, the `manifold` API, the autorun module loader, the modules in `Autorun` |
| `managed` | the managed dumper: its data format, the embedded assembly, and a run on the .NET Framework |
| `clr` | the .NET reader: the metadata parser, the helper's protocol, and the whole path through `ManifoldClr.exe` (classes, statics, methods, the GC heap walk, objects, arrays and strings, a pointer from a static field in the address list, `manifold.dotnet` and `getDotNetDataCollector`) against `TestTarget.exe --clr` (the .NET Framework, also as 32-bit) and `TestTarget.exe --coreclr` (.NET 8 and the newest installed .NET); `--clr-check` runs only this |
| `integration` | a run against `TestTarget.exe` (built with the solution): scanning, pointers, freezing, a code injection script, the sample table `Tests/Target/TestTarget.CT`, Lua (records, timers, threads), the write history, all four "find out what" watches, breakpoints and stepping with the Windows debugger, the fake Mono, IL2CPP and managed dumper runtimes of the target, the direct system calls |
| `inprocess` | the in-process debugger in a 64-bit target: breakpoints with conditions and register changes, watches on one instruction, and the agent letting the target go when the program falls silent |
| `wow64` | the same as `integration` for a 32-bit game against `TestTarget32.exe`, with both debuggers and the fake Mono, IL2CPP and managed dumper runtimes (`--wow64-managed-check` runs only those) |

Four documentation files are generated by the tests program, and the tests fail when one of them is out of date: `--errors-markdown` prints `Docs/ERRORS.md`, `--shortcuts-markdown` prints `Docs/SHORTCUTS.md`, `--manifold-markdown` prints `Docs/MANIFOLD-API.md` and `--ce-compat-markdown` prints `Docs/CE-COMPAT.md`. They also fail when two commands of one window share a key.

Run the tests from the repository folder or a folder below it, with `TestTarget.exe` next to them (`--target=` names another one): the documentation checks look for `Docs` and `Source` above the working folder. A check that cannot run there counts as skipped, and skipped checks fail the run; `--allow-skips` accepts them.

A test run keeps its settings, its log (`logs\Manifold-Engine.log`) and a crash dump, should the tests program crash, in a folder of its own under `%TEMP%\Manifold-Engine-Tests`, so the `configs` folder next to the program stays as it is. The folder goes away when the run passes. After a failed run or a crash it stays for a week and the run prints where it is. The environment variable `MANIFOLD_TEST_DATA` names another place for these folders, and nothing is removed there; CI uses it to keep them.

`--crash-check` crashes the tests program on purpose, to see that the dump is written.

### Checks outside the default run

Some checks need a real game, a long run or a special situation and are not part of the default run or of CI. Each is started with its own option:

| Option | Notes |
| --- | --- |
| `--debugger-check` | with `--veh` for the in-process debugger |
| `--watch-arm-check`, `--watch-stress` | |
| `--mono-check`, `--mono-dump`, `--unity-dump`, `--il2cpp-check` | |
| `--unreal-check`, `--disasm-check`, `--relay-check` | |
| `--function-search-check` | with `--gaps` it looks at the code between the unwind entries of every loaded System32 DLL, and `--dump` prints the cases it got wrong or could not find |
| `--pointer-scan-bench --process=<name or id>` | builds the pointer map of a running process, searches three times, rescans, and prints the times, the pointer and path counts and the memory used; `--address=`, `--level=`, `--offset=`, `--results=`, `--threads=`, `--readonly`, `--mapped`, `--system` and `--any-offset` set the scan |
| `--compat` | runs a cheat table without a window |

With the environment variable `MANIFOLD_TEMPLATE_CHECK` set to a folder of templates, the `templates` suite also generates each of them for an instruction of the tests program itself, prints whether the result assembles and writes it into `_rendered` there. This prints only and fails nothing.

## Working on the code

- **Line endings**: the repository keeps text files with LF (`.gitattributes`; solution, project and batch files keep CRLF), and `.editorconfig` tells editors to use UTF-8, LF and four spaces.
- **Style**: `.clang-format` describes the code style.
  - `python Tools/format_changed.py` shows how clang-format would format the lines changed since `HEAD` and touches nothing; `--commit`, `--staged` and file names narrow it down, `--apply` writes the result.
  - Only changed lines are formatted, so the rest of the code stays as it is.
  - One place where clang-format differs from the existing code: it puts the braces of a lambda assigned to a variable at the level of the statement.
- **Commands and shortcuts** are defined once in `Source/App/Commands.cpp`; a window asks `Pressed(Command::…)` and shows the keys with `CommandItem`.
- **Cheat Engine API docs**: `Source/Lua/Docs/CheatEngine.txt` lists the classes, functions, methods and properties of Cheat Engine 7.5's Lua API with descriptions written for this project; `python Tools/gen_lua_docs.py` turns it into `Source/Generated/LuaDocs.cpp` for the documentation panel, and `Tools/gen_projects.py` runs it too, so CI notices a `LuaDocs.cpp` that was not regenerated.
- **Live install**: `Build\x64\Release` is build output that every build replaces. `Tools\Install.ps1` copies a build into `Install` in the main checkout (or into `-Destination`), and the program there keeps its `configs`, `tables`, `sounds`, `templates`, `logs` and `crashes` apart from the build. The first install copies those folders from `Build\x64\Release` (or `-AdoptFrom`) and leaves the originals in place; later installs replace only the program files, add Autorun modules that are newer, and refuse while that copy of the program runs.
- **Worktrees**: parallel work goes into one git worktree each (`.claude/worktrees`) instead of the shared checkout, so no one commits someone else's half-done changes. Each worktree builds into its own `Build` folder, and `Tools\Install.ps1` run from any worktree installs into the main checkout's `Install`.
- **CI**: `.github/workflows/build.yml` runs on `windows-2022` with clang-cl on every push to `main`, on pull requests and on demand. It checks that Python and the .NET SDK are there, builds `Release|x64`, checks that the generated files and projects are committed, and runs all tests. Then it builds `Debug|x64` and runs all tests again with that build. When a step fails, the run keeps the test folders with their logs and crash dumps as the artifact `test-data` for a week. A change to `README.md` or the licenses alone does not start a run.
- **Architecture**: [ARCHITECTURE.md](ARCHITECTURE.md) describes the layers, the main objects, which thread runs what, and where a new command, window, setting, Lua function, error or test suite goes.
- **Interface**: windows are drawn with the layout system in `Source/Layout` ([UI-LAYOUT.md](UI-LAYOUT.md)). `Source/Layout/RawImGui.txt` caps the raw ImGui calls per file: the build warns (`ME0004`) and the suite `ui-layout` fails above it; `Manifold-Engine.Tests.exe --ui-raw-baseline` lowers the numbers after a file moved to the components. Settings → Diagnostics → Layout Checks (or `--layout-audit`) outlines what is cut off or leaves its place and lists it under Layout Problems; `Manifold-Engine.exe --layout-sweep=<report>` runs the checks over every window, size, tab and scale without showing anything.

## Layout

| Folder | Content |
| --- | --- |
| `Source/App` | program, window host, settings, the engine and its parts (target session, managed session, table runtime, script activation), crash handler |
| `Source/Core` | error codes, log, text helpers, ini and XML, paths, background work, the frame profiler |
| `Source/Text` | JSON and other text formats |
| `Source/Target` | process access, process list, symbols and address expressions, Mono, IL2CPP, .NET, Unreal Engine and C++ RTTI |
| `Source/Disasm`, `Source/Asm` | Zydis disassembler, assembler and Auto Assembler |
| `Source/Syscalls` | direct system calls |
| `Source/Generated` | files written by the scripts in `Tools` (embedded Lua, built-in templates, icon names, Cheat Engine API docs) |
| `Agent` | the in-process debugger DLL |
| `ClrHelper` | `ManifoldClr.exe` / `ManifoldClr32.exe`, which read a .NET game from outside through the runtime's data access library |
| `Managed` | the .NET dumper and scene agent loaded into Mono games, and the fake .NET game the tests read |
| `Autorun`, `Examples` | Lua modules that run at start, example scripts |
| `Tools` | project generator, embedding scripts, formatting, install |
| `Source/Templates`, `Templates` | template engine, its values and the built-in template files (`Tools/embed_templates.py` puts them into `Source/Generated/BuiltinTemplates.cpp`) |
| `Source/Scan` | value types and the scanner |
| `Source/Debug` | debugger, breakpoints and watches |
| `Source/Table` | address list, cheat table files, structures |
| `Source/Lua` | Lua engine and API; `Source/Lua/Docs` holds the documentation sources |
| `Source/Layout` | the layout system: rules, rows and regions, text, buttons, fields, cards, the audit, and `RawImGui.txt` |
| `Source/UI` | main window and the tool windows |
| `Tests` | test suites and the test target with its table |
| `Dependencies`, `Licenses` | third-party code and its licenses |
