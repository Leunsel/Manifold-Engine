# Development

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

The program is x64-only. Every x64 build also builds the in-process debugger agent and the test target a second time as 32-bit, as `ManifoldAgent32.dll` and `TestTarget32.exe` next to the program: the program injects the 32-bit agent into 32-bit games, and the tests use the 32-bit target. The solution's `Win32` platform builds only the dependencies, these two projects and the stand-alone `SyscallTest` project.

The MSVC compiler (`v143`) cannot build the program: the direct system call stubs in `Source/Syscalls/Syscall.h` are naked functions with inline assembly, which only clang-cl compiles.

Before compiling, the projects run `Tools/embed_lua.py`, `Tools/embed_templates.py` and `Tools/gen_buildinfo.py` with the Python on PATH (without Python the build warns with ME0001 and uses the committed generated files). The last one writes the commit into the build, so the log, Help → About and the diagnostics report show the version with it, like `0.1.0 (3e90571)`; a `+` after the commit means the working tree had uncommitted changes.

## Tests

```bash
Build\x64\Release\Manifold-Engine.Tests.exe
```

The tests cover the assembler, value parsing, XML and cheat table round trips, the error table, offline Auto Assembler runs, the Lua APIs and the Autorun modules, and an integration run against `TestTarget.exe` (built with the solution): scanning, pointers, freezing, a code injection script, the sample table `Tests/Target/TestTarget.CT`, Lua (records, timers, threads), all four "find out what" watches, breakpoints, stepping and the direct system calls. The `wow64` suite does the same for a 32-bit game against `TestTarget32.exe`: pointers, a code injection script, Lua, both debuggers (watches, int3 and hardware breakpoints, conditions, stepping, Break, trace), and the fake Mono, IL2CPP and managed dumper runtimes of the target (`--wow64-managed-check` runs only those). `--errors-markdown` prints `Docs/ERRORS.md`, `--shortcuts-markdown` prints `Docs/SHORTCUTS.md` and `--manifold-markdown` prints `Docs/MANIFOLD-API.md`; the tests fail when one of them is out of date, and when two commands of one window share a key.

Run the tests from the repository folder or a folder below it, with `TestTarget.exe` next to them: the documentation checks look for `Docs` and `Source` above the working folder. A check that cannot run there counts as skipped, and skipped checks fail the run; `--allow-skips` accepts them. `--list` names the suites and `--filter=core,text` runs only those. A test run keeps its settings and logs in a folder of its own under `%TEMP%\Manifold-Engine-Tests`, so the `configs` folder next to the program stays as it is.

## Working on the code

- **Line endings and style**: the repository keeps text files with LF (`.gitattributes`; solution, project and batch files keep CRLF), and `.editorconfig` tells editors to use UTF-8, LF and four spaces. `.clang-format` describes the code style. `python Tools/format_changed.py` shows how clang-format would format the lines changed since `HEAD` and touches nothing; `--commit`, `--staged` and file names narrow it down, `--apply` writes the result. Only changed lines are formatted, so the rest of the code stays as it is. One place where clang-format differs from the existing code: it puts the braces of a lambda assigned to a variable at the level of the statement.
- **Commands and shortcuts** are defined once in `Source/App/Commands.cpp`; a window asks `Pressed(Command::…)` and shows the keys with `CommandItem`.
- **Cheat Engine API docs**: `Source/Lua/Docs/CheatEngine.txt` lists the classes, functions, methods and properties of Cheat Engine 7.5's Lua API with descriptions written for this project; `python Tools/gen_lua_docs.py` turns it into `Source/Generated/LuaDocs.cpp` for the documentation panel.
- **Live install**: `Build\x64\Release` is build output that every build replaces. `Tools\Install.ps1` copies a build into `Install` in the main checkout (or into `-Destination`), and the program there keeps its `configs`, `tables`, `sounds`, `templates`, `logs` and `crashes` apart from the build. The first install copies those folders from `Build\x64\Release` (or `-AdoptFrom`) and leaves the originals in place; later installs replace only the program files, add Autorun modules that are newer, and refuse while that copy of the program runs.
- **Worktrees**: parallel work goes into one git worktree each (`.claude/worktrees`) instead of the shared checkout, so no one commits someone else's half-done changes. Each worktree builds into its own `Build` folder, and `Tools\Install.ps1` run from any worktree installs into the main checkout's `Install`.
- **CI**: `.github/workflows/build.yml` builds `Release|x64` on `windows-2022` with clang-cl, checks that the generated files and projects are committed, runs the tests and builds `Debug|x64`, on every push to `main`, on pull requests and on demand. A change to `README.md` or the licenses alone does not start a run.
- **Architecture**: [ARCHITECTURE.md](ARCHITECTURE.md) describes the layers, the main objects, which thread runs what, and where a new command, window, setting, Lua function, error or test suite goes.

## Layout

| Folder | Content |
| --- | --- |
| `Source/App` | program, window host, settings, the engine and its parts (target session, managed session, table runtime, script activation), crash handler |
| `Source/Core` | error codes, log, text helpers, ini and XML, paths, background work, the frame profiler |
| `Source/Text` | JSON and other text formats |
| `Source/Target` | process access, process list, symbols and address expressions, Mono, IL2CPP and Unreal Engine |
| `Source/Disasm`, `Source/Asm` | Zydis disassembler, assembler and Auto Assembler |
| `Source/Syscalls` | direct system calls |
| `Source/Generated` | files written by the scripts in `Tools` (embedded Lua, built-in templates, icon names, Cheat Engine API docs) |
| `Agent` | the in-process debugger DLL |
| `Managed` | the .NET dumper and scene agent loaded into Mono games |
| `Autorun`, `Examples` | Lua modules that run at start, example scripts |
| `Tools` | project generator, embedding scripts, formatting, install |
| `Source/Templates`, `Templates` | template engine, its values and the built-in template files (`Tools/embed_templates.py` puts them into `Source/Generated/BuiltinTemplates.cpp`) |
| `Source/Scan` | value types and the scanner |
| `Source/Debug` | debugger, breakpoints and watches |
| `Source/Table` | address list, cheat table files, structures |
| `Source/Lua` | Lua engine and API; `Source/Lua/Docs` holds the documentation sources |
| `Source/UI` | main window and the tool windows |
| `Tests` | test suites and the test target with its table |
| `Dependencies`, `Licenses` | third-party code and its licenses |
