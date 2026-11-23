# Autorun modules

Manifold Engine runs the Lua scripts in the `autorun` folder next to the program at start (Settings → Scripts → *Run Autorun Modules*). **Extensions → Modules** lists them with their state, version, load time and errors, and switches, reloads and allows them. Scripts reach the same list through [`manifold.module`](MANIFOLD-API.md).

## The folder

```
autorun\
├─ MyScript.lua                 loose script: runs as it is, like in Cheat Engine
├─ custom\                      loose scripts too (Cheat Engine's custom folder)
├─ modules\
│  ├─ Engines\                  a category: any folder without an init.lua, one level
│  │  └─ Godot.lua              file module
│  └─ Tools\
│     └─ SigMaker\              folder module
│        ├─ init.lua            its entry
│        ├─ Manifold-SigMaker-Host.lua …   private files, loaded with require
│        └─ Manifold-Icons\
├─ lib\                         libraries any script can require; never run
├─ data\                        what modules write: data\<module name>\
└─ anything else\               left alone
```

The program decides what a file is by where it is and never by what is in it:

| What | Where | Entry |
| --- | --- | --- |
| Loose script | `autorun\*.lua`, `autorun\custom\*.lua` | the file |
| File module | `autorun\modules\[Category\]Name.lua` | the file |
| Folder module | `autorun\modules\[Category\]Name\init.lua` | `init.lua` |

A folder module is not searched further: everything else in it belongs to the module. Every other folder in the root of `autorun` (resource folders of older scripts, Cheat Engine's `forms` and `dlls`) is never run.

## The header

A module may describe itself in the first block comment of its entry file (or in the `--` lines at its top). The program reads these lines as text before anything runs. All of them are optional, so a plain script is a valid module named after its file.

```lua
--[[
    NAME        : SigMaker
    VERSION     : 2.1.0
    AUTHOR      : Leunsel
    DESCRIPTION : Unique byte signatures for the selected instruction.
    REQUIRES    : Json >= 1.2, Helpers
    API         : 10
]]
```

| Line | Meaning |
| --- | --- |
| `NAME` | The module's name and identity. Without it the file name (or the folder name) is the name. Names ignore case. |
| `VERSION` | Shown in the list; compared by `REQUIRES` of other modules, number by number. |
| `AUTHOR`, `DESCRIPTION` | Shown in the list. |
| `REQUIRES` | Modules that have to be loaded first, separated by commas, each with an optional `>= version`. |
| `API` | The `manifold.apiVersion` the module needs at least. |

Other `KEY : value` lines (`LICENSE`, `CREATED`, …) are shown as they are.

## Load order

Loose scripts in the root run first in name order, then `custom\`, then the modules in name order (the category does not count). `REQUIRES` moves a required module in front of the one that names it; without any `REQUIRES` the order is the plain name order.

A module does not run and is shown as **Blocked**, with the reason, when a module it requires is missing, switched off, held, failed or too old, when requirements form a circle, when the header cannot be read, or when `API` asks for a newer program. A script error marks the module **Failed** with the message and traceback; the modules that require it are blocked, and all others load.

When a loose script and a module in `modules\` have the same name, the module runs and the loose script is **Superseded** and skipped. Nothing is deleted. Two modules in `modules\` with the same name are an error for the second one.

## require

`require` looks at the file that calls it:

1. In a folder module, the module's own folder comes first: `require("Decoder")` is `<module>\Decoder.lua` or `<module>\Decoder\init.lua`, `require("ui.List")` is `<module>\ui\List.lua`. These are kept per module, so two modules may both have a `Settings.lua`, and a reload reads them from disk again.
2. The name of another module returns what that module's entry file returned (`true` when it returned nothing). A module that is not loaded raises an error. A module that requires another one without naming it in `REQUIRES` gets a warning once, because the load order is then not guaranteed.
3. `autorun\lib\?.lua` and `autorun\lib\?\init.lua`.
4. The ordinary `package.path`.

Table scripts and the console get steps 2 to 4. No module has to change `package.path`.

## Switching, reloading, unloading

The switch in the list (or `manifold.module.enable` and `disable`) is remembered in `configs\Manifold-Engine.modules.ini`. Switching a module off unloads it and the modules that require it; switching it on loads it at once. *Reload* unloads a module, reads it from disk and runs it again, and after it the loaded modules that require it. *Reload all modules* reads the whole folder again.

Unloading removes what the module registered through `manifold` (event handlers, symbol resolvers, Structure Dissect hooks, `manifold.ui` windows and entries) and calls the handlers it gave to `manifold.module.onUnload`. The program cannot track Cheat Engine menu items, forms, timers and custom types, and globals stay as well; a module that makes such things takes them down in an `onUnload` handler. A module that made them and has no handler is shown as **Disabled, active until restart**. Symbols registered with `manifold.symbol.register` stay until the process is closed.

`manifold.module.current()` tells a module its name, folder and data folder; `manifold.module.ensureData()` creates `autorun\data\<name>\` and returns its path. Settings and other files belong there: writing them does not count as a change to the module's code, and the module's folder can be replaced as a whole.

## New and changed modules

Scripts of a table can write files, also into the `autorun` folder. So that such a file cannot run unseen at every start:

- A script or module the program has not seen before is **held**: it is listed, a notice appears at start, and it runs once *Allow* is chosen in the list. Settings → Scripts → *Hold New Modules* turns this off. At the first start with this version everything already in the folder counts as seen. The modules that come with the program are never held while their files are unchanged.
- When a script writes into the `autorun` folder (`io.open` for writing, `os.rename`, `os.remove`), the log gets a Warning on the channel `Autorun` that names the file and the script line. Writing to another module's `.lua` files holds that module from its next load until it is allowed again. Writes into `autorun\data` and a module writing its own files are not reported.
- A module whose code differs from the last run without such a write (edited in an editor, most likely) runs, with a Warning in the log and *Loaded, changed* in the list for that session.

This makes planting visible; it is not a sandbox. A script can still write the state file itself or start another program (`shellExecute` and `createProcess` are logged as before).

## The modules that come with the program

| Module | Where | Global | What it does |
| --- | --- | --- | --- |
| `Manifold-Godot` | `modules\Engines\Godot.lua` | `ManifoldGodot` | Godot 4 runtime: StringNames, ObjectDB, ClassDB, the scene tree and method symbols (`Class::method`); fills structures and adds a *Godot Engine* menu. Godot 3 is declined with a message. |

It is built on the program's own [manifold API](MANIFOLD-API.md) and does not run in stock Cheat Engine. Up to version 2.0.0 it was `Manifold-Godot.lua` in the root of `autorun`; such an old copy is superseded by the module and can be deleted, and so can an old `Manifold-SigMaker.lua` with its `Manifold-SigMaker-Modules` folder.

Unreal Engine support, C++ run-time type information and the signature maker are built into the program (Extensions → Unreal Engine, Extensions → C++ RTTI and Copy Signature / Signature Maker in the memory viewer; [`manifold.unreal`](MANIFOLD-API.md), [`manifold.rtti`](MANIFOLD-API.md) and [`manifold.signature`](MANIFOLD-API.md)). An installed copy of the old `Manifold-SigMaker` module is shown as built in and no longer runs; `ManifoldSigMaker` keeps working for scripts that call it.

- The program skips an old `Manifold-UE.lua` or `Manifold-RTTI.lua` in its `autorun` folder (state *Built in*); those files can be deleted.
- The global `ManifoldRTTI` of the RTTI script is still there: its functions call `manifold.rtti` and return what the script returned, so tables and scripts written against it keep working.
- `<Class>::vftable` symbols in saved tables resolve as before.

Building the program copies the repository's `Autorun` folder into `autorun` next to the executable, and `Tools\Install.ps1` does the same for the install folder. Only files that are newer in the repository are copied, and nothing is removed. The build also embeds the names and content hashes of these modules (`Tools/embed_modules.py`), which is how the program knows a shipped module from another one.

## How they use the API

- Output goes to log channels (`Godot`) with levels; listings the user asks for are info lines. Start-up lines are debug, so *Show on print* does not open the Lua engine at every start.
- Memory is read with `manifold.memory.read`, objects as one `raw` block unpacked with `string.unpack` instead of one read per field.
- Pattern scans use `manifold.memory.scan`, symbols `manifold.symbol`, Structure Dissect hooks `manifold.structure`, and process changes arrive through `manifold.event`. Every registration carries a key, so running a file again replaces its hooks instead of adding a second set.
- Failures come back as error objects with an `ME-` code or the module's own code (`GODOT-STAGE`, …); menu actions catch them with `manifold.error.try` and log them with the traceback.
- Forms, menus, dialogs, custom types and the structure objects that Structure Dissect hands to the hooks are still the Cheat Engine functions, since the API has no counterpart for them yet. Both modules take their menus down in an `onUnload` handler.

## Living together

The Unreal tool, Godot and the built-in RTTI stage never answer for each other's objects:

- The Unreal tool resolves only in a game that looks like Unreal (or when it is started by hand).
- Godot resolves only behind its version literal.
- RTTI asks the Unreal tool and `ManifoldGodot.claims` before it names an object; `manifold.rtti.claims` tells a script what RTTI names.

Structure Dissect asks the built-in Unreal tool, then Mono, then RTTI, and only then the script hooks. Script hooks are asked in the order they were registered, and the first answer wins.

## Checking them

`Manifold-Engine.Tests.exe --filter=modules,autorun`:

1. checks the loader on folders it builds itself: what is found, headers, order, `require`, unloading, the held and changed states;
2. loads the repository's `Autorun` folder twice without a process, switches both modules off and on, and opens and closes the test program;
3. checks that nothing reports an error or a wrong API call, that neither the Unreal tool nor Godot claims the test program, and that the shipped modules match the embedded hashes.

For a deeper look, the headless harness runs them against a process without writing to it:

```bash
Manifold-Engine.Tests.exe --compat Tests\Target\TestTarget.CT --autorun=Autorun --process=Game.exe --dry-run --home=<empty folder> --eval=<script.lua>
```

The harness keeps no state file, so nothing is held there.

Checked so far, read-only, with the scripts as they were before they became modules:

- Crab Champions (UE 4.2x): the built-in Unreal tool resolves in 0.8 s and names and fills `GameEngine`, `World` and the pawn; RTTI named only the objects without reflection (`CSteamUserV021`), checked with the RTTI script the built-in stage replaced; Godot declines.
- Unrailed 2 (Godot 4.6.3 .NET): Godot resolves in 0.24 s with the measured layout, names and fills the scene root, and registers 14 611 method symbols in 0.26 s; UE declines; the RTTI script left the Godot objects to Godot.
- Headless against the test program: the Godot module. The Unreal tool, RTTI and the signature maker have their own test suites (`signatures` checks the uniqueness search against a brute-force search), (`unreal` with a synthetic Unreal image, `rtti` with hand-built and compiler-made RTTI in the 64-bit and the 32-bit test program).

Not checked yet: the two modules in a real game since the move to `modules\`, UE5 and modular Unreal builds, other Godot 4 releases, the built-in RTTI on a real game (64-bit or 32-bit).
