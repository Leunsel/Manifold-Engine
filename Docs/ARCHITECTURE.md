# Architecture

This page is for working on Manifold Engine itself: how the program is put together, which thread runs what, and where a new command, window, setting or Lua function goes. The user-facing documentation is in the other files of this folder.

## Layers

Lower layers never include higher ones. `Core` includes nothing from the other folders. Nothing in the engine core includes `App`, `Lua` or `UI`. Inside the engine core, code and target work together: the disassembler reads process memory and names, and the code map uses the disassembler.

| Layer | Folders | Holds |
| --- | --- | --- |
| Foundation | `Source/Core`, `Source/Text` | error codes and `Status`/`Result<T>`, the log, text helpers, ini and XML, paths, `BackgroundWork`, main-thread helpers, `CheckedMutex`, the profiler; JSON and other text formats |
| Engine core | `Source/Disasm`, `Source/Asm`, `Source/Syscalls`, `Source/Target`, `Source/Scan`, `Source/Debug`, `Source/Table` | Zydis disassembler, code dissect; the assembler and the Auto Assembler; direct system calls; process access, modules, symbols, address expressions and the code map; Mono/IL2CPP (`MonoBridge`, `RemoteCall`, `UnityExplorer`, the managed agent link); the scanner; the debugger, watches and tracer; the address list, cheat table files and structures |
| Application | `Source/App`, `Source/Templates` | `Engine` and its parts, settings, commands, diagnostics, the window host; the template engine |
| Scripting | `Source/Lua` | the Lua engine, the Cheat Engine compatible API, the `manifold.*` API and its documentation registry |
| Interface | `Source/UI` | `Workspace`, the main window and every tool window, widgets and theme |

Outside the program: `Agent` is the in-process debugger DLL (VEH) that is injected into the target; `Managed` is the .NET 3.5 dumper and scene agent that is loaded into a game's Mono root domain; `Source/Generated` is written by the scripts in `Tools`; `Tests` holds the test runner and `TestTarget`.

## The main objects

- **Application** (`App/Application`) runs the frame loop: `Engine::Tick`, then `Workspace::Draw`, then rendering.
- **Engine** (`App/Engine`) ties the parts together. It opens and closes the process, runs the tick, and posts notices. It owns:
  - **TargetSession**: the opened process, its generation number, symbols, PDB symbols, the Mono bridge and the code map.
  - **ManagedSession**: loads, reloads and shuts down the Mono or IL2CPP helper as background work, starts the scene explorer, and logs how a game ended while the agent was in it.
  - **UnrealSession**: resolves an Unreal Engine game as background work (`Target/UnrealRuntime`: name table, object array, reflection layout and the type index), keeps the `manifold.unreal` settings, registers `GObjects` and friends, answers `<Type>_<Property>` symbols, and provides the custom types `UE FName`, `UE FString` and `UE Object Index`. Structure Dissect asks it (`Table/UnrealStructures`) before Mono and before the Lua dissectors.
  - **TableRuntime**: the address list, structures, table symbols, record values, freezes, hotkeys, and table load and save.
  - **ActivationRunner**: turns scripts on and off on its worker thread, and runs Lua for it on the main thread.
  - **BackgroundWork**: jobs with progress and cancel. A job's `Finish` callback runs on the main thread in `Drain`, which the tick calls. Serial lanes run the jobs of one group one after another.
  - **NotificationCenter**: the notices.
  - **SettingsStore**: saves the settings shortly after the last change.
  - **LuaEngine**: the Lua state and everything scripts registered.
- **Workspace** (`UI/Workspace`) owns the windows. It opens them, routes the global shortcuts and draws notices and confirmations.

## Threads

| Thread | Runs | Rules |
| --- | --- | --- |
| Main | the frame loop: Engine tick, Lua, ImGui, every `Finish` callback | ImGui and Lua run here only. `OnMainThread()` tells, and `MANIFOLD_ASSERT_MAIN()` logs a violation once per place |
| Background work | the job functions of `BackgroundWork` | they return a `Finish` lambda for anything that touches the UI, Lua or the engine's state |
| Script activation | the Auto Assembler for a script record | Lua runs through `RunOnMainAndWait` |
| Debugger | `Debug::Debugger::Loop`, every debug event | the breakpoint and watch state is under the debugger's lock |
| Scanner, PDB loader, Unity explorer jobs, sounds | their own work | they publish results under their own locks |
| RemoteCall worker (in the game) | Mono and IL2CPP calls, one at a time | every call holds `MonoBridge::m_lock`, a `CheckedMutex`. `RemoteCall` asserts it, and the integration tests fail when a call ran without it. Pages handed to the worker are never freed while it can still use them |
| Game main thread | the managed agent (`Managed/ManifoldDumper/Agent.cs`), from the game's frame callback | every Unity engine call happens here. The host only writes requests into the mailbox, and `StopNow` hands over to `Tick` when the agent is busy |

## Frame order

`Engine::Tick` saves due settings, drains finished background work, polls the scanner and checks that the process is alive. It then opens processes that are waiting to be opened, writes frozen values and checks hotkeys. Next it refreshes the address list values, runs Lua timers and tasks, and pumps script activations. Last it handles a debugger break. After the tick, `Workspace::Draw` handles requests from scripts and the engine, runs the shortcuts and draws every window.

## Adding things

- **A command:**
  - Add a row to the table in `App/Commands.cpp` with its scope, name and keys.
  - Handle it in `Workspace::Run` (scope Global) or `MainWindow::Run` (scope Main), with its enable rule in the matching `CanRun`. Other windows handle their own scope in their `RunCommand`.
  - Menus draw the entry with `MainWindow::Item`, or with `CommandItem` in other windows, so the keys and the enabled state come from the same place as the shortcut.
- **A window:**
  - Put the class in its own header next to its `.cpp` and derive it from `ToolWindowBase`. That gives it `m_open`, bringing it to the front, and `BeginWindow`.
  - Add a member and an `Open…` method to `Workspace`.
- **A setting:** add the field to `Settings` and a row to `kFields` in `App/Settings.cpp`. Load and save follow from the row, and the test `SettingsTable` covers the new field by itself.
- **A Lua function:**
  - Cheat Engine compatible functions are a table row in `Lua/LuaApi.cpp`, `LuaObjects.cpp` or `LuaCompat.cpp`. They take their arguments with the helpers in `Lua/LuaArgs.h`.
  - `manifold.*` functions need three things: a native in `Lua/LuaManifold.cpp` with its row in `natives[]`, a `define()` in `Lua/Manifold/Manifold.lua`, and a regenerated `Docs/MANIFOLD-API.md` (`Manifold-Engine.Tests.exe --manifold-markdown`).
  - Failures go back to Lua as `nil, code, message` through `PushFailure`.
- **An error:** add a row to the table in `Core/Errors.cpp`; the number never changes its meaning. `Docs/ERRORS.md` comes from `--errors-markdown`.
- **A test suite:** write `int RunSomethingTests()` and register it at the end of its file with `MANIFOLD_SUITE("something", "Something", order, ...)`. A separate tool run (a harness against a live process, say) registers with `MANIFOLD_MODE("--flag", ...)`.

## Generated files

- `Source/Generated` holds the embedded Lua scripts, the built-in templates and the Font Awesome names. The pre-build step refreshes them with `Tools/embed_lua.py` and `Tools/embed_templates.py`, and they are committed.
- The managed dumper is embedded by `Tools/embed_managed.py`; that file is not committed.
- `Projects/*.vcxproj` and `Manifold-Engine.sln` come from `Tools/gen_projects.py`, which takes every file on disk. Run it after adding or removing a source file.
- CI fails when a generated file differs from what is committed.

## Tests

`Manifold-Engine.Tests.exe` runs every registered suite in order:
- `--list` names the suites.
- `--filter=core,text` runs a part of them.
- `--allow-skips` accepts checks that could not run.

The run writes settings, logs and crash reports into its own folder under `%TEMP%\Manifold-Engine-Tests` and removes it at the end, so the configs folder next to the program is never touched. The integration suite starts `TestTarget.exe`, a small program with known values, code and a fake Mono and IL2CPP runtime. It never attaches to a real game.
