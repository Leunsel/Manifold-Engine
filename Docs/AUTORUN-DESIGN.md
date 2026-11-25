# Autorun modules: design proposal

Status: approved as recommended and implemented on 2026-11-21. [AUTORUN.md](AUTORUN.md) describes what the program does; this file keeps the reasons and the rejected alternatives. Where the implementation differs, a note says so at the end.

## Today

- `LuaEngine::RunAutorun(folder)` runs every `*.lua` in the root of `autorun\` in name order, each under its own error handler, at start (`[Scripts] Autorun`) and from Tools → *Run autorun scripts again*. Subfolders are never looked at.
- Two files are skipped by a content marker because the program has them built in (`Manifold-UE.lua`, `Manifold-RTTI.lua`).
- The engine sets no `package.path`. `Manifold-SigMaker.lua` prepends its `Manifold-SigMaker-Modules` folder to the global `package.path` and clears its own names from `package.loaded` by hand to make a second run pick up edits.
- There is no list of what ran, no per-script switch, no versions, no dependencies. The only trace is a log line per file.
- The build (`xcopy /D /E`) and `Tools\Install.ps1` (`robocopy /E /XO`) copy `Autorun\` over `autorun\`, newer files only, and never remove anything.
- Scripts started from a table can write anywhere, including `autorun\`. Only `shellExecute`, `createProcess` and `injectDLL` are traced (Warning, channel `Script`). A planted file runs silently at every later start.
- Cheat Engine, as far as I know it (7.x, from memory, not re-read for this proposal): root `*.lua` in name order, then `autorun\custom\*.lua`; `forms`, `dlls`, `images`, `xml` are resource folders the scripts open themselves through `getAutorunPath()`.

## 1. What a module is

**Recommendation.** One concept, *module*, in three shapes. The loader tells them apart by position only and never guesses from content:

| Shape | Where | Entry |
| --- | --- | --- |
| Loose script | `autorun\*.lua`, `autorun\custom\*.lua` | the file |
| File module | `autorun\modules\[Category\]Name.lua` | the file |
| Folder module | `autorun\modules\[Category\]Name\init.lua` | `init.lua` |

Inside `modules\`, a folder with an `init.lua` is a module and is not searched further: everything else in it is the module's private business (libraries, icons, forms). A folder without `init.lua` is a category, one level deep. Libraries shared between modules live in `autorun\lib\`, data and config in `autorun\data\<Name>\`. Every other folder in the root (`Manifold-SigMaker-Modules`, CE's `forms`, `dlls`, …) is left alone, exactly as today.

A module's identity is its name: the header's `NAME`, else the file or folder name. The category is only for order on disk, so moving a module to another category keeps its state.

**Rejected.** Scanning all subfolders for `.lua` files (runs library files such as the SigMaker modules). A marker file per folder (`.module`, `.nolua`): one more thing to forget. No `modules\` container, with any root folder that has an `init.lua` counting as a module: a Lua library copied into the root (`penlight\init.lua`) would be run.

## 2. Folder layout

```
autorun\
├─ Manifold-Json.lua              loose script, runs as today
├─ custom\                        loose scripts (CE)
├─ modules\
│  ├─ Engines\                    category
│  │  └─ Godot.lua                file module
│  ├─ Tools\
│  │  └─ SigMaker\                folder module
│  │     ├─ init.lua
│  │     ├─ Decoder.lua …         private, loaded with require
│  │     └─ icons\
│  └─ Games\
│     └─ Isaac.lua
├─ lib\                           shared libraries, never run
│  └─ json.lua
├─ data\                          written by modules
│  └─ SigMaker\settings.json
└─ Manifold-SigMaker-Modules\     old resource folder: ignored
```

- **Shipped vs. own modules:** no separate folder. The build embeds the names and content hashes of the modules in the repository's `Autorun\`; a module on disk that matches is shown as *shipped*, anything else as *user*. Categories stay free for topics.
- **Data:** `data\<Name>\` is outside the module folder so that a module writing its config does not count as a change to its code (section 9) and so that a module folder can be replaced as a whole.
- **Disabled** is a stored state, not a place (section 6).

**Rejected.** `shipped\` and `user\` top-level folders: the owner writes both, and the split says nothing about what a module is for. A `disabled\` folder or a `_` name prefix: moving a file breaks relative paths, the next install copies a shipped module back, and it would be a second mechanism beside the switch in the list.

## 3. Load order and dependencies

**Recommendation.** One graph over all modules, loose scripts included.

1. Base order: root loose scripts by name, then `custom\`, then `modules\` by name (case-insensitive, category ignored). With no `REQUIRES` anywhere this is today's order.
2. `REQUIRES` moves a dependency in front of the module that names it.
3. A module does not run, with state *Blocked* and the reason, when a dependency is missing, disabled, held, failed, too old, or part of a cycle (the cycle is named), or when `API` asks for a newer `manifold.apiVersion`.
4. An error in one module marks it *Failed* with message and traceback. Its dependents become *Blocked*; everything else loads.

**Rejected.** A numeric priority: it hides the real reason for an order and ends in number races; dependencies say the reason. Name order alone: forces names like `00-Json.lua`. Running dependents of a failed module anyway: they fail later with a worse message.

## 4. How a module declares itself

**Recommendation.** `KEY : value` lines in the first block comment of the entry file, which is the header the shipped scripts already carry (`AUTHOR`, `VERSION`, `LICENSE`). The loader reads it as text before anything runs. All keys are optional; a file without a header is a valid module named after its file.

```lua
--[[
    SigMaker
    --------------------------------
    NAME        : SigMaker
    VERSION     : 2.1.0
    AUTHOR      : Leunsel
    DESCRIPTION : Unique byte signatures for the selected instruction.
    REQUIRES    : Json >= 1.2
    API         : 1
    LICENSE     : MIT
]]
```

Read keys: `NAME`, `VERSION`, `AUTHOR`, `DESCRIPTION`, `REQUIRES` (comma list, optional `>= version`), `API`. Unknown keys are kept and shown as text. `API` replaces the hand-written `manifold.apiVersion` gate at the top of each script.

**Rejected.** A manifest file beside the script (`module.json`): a one-file module cannot have one, and two files drift apart. A call such as `manifold.module.declare{…}`: the order must be known before any module runs. `---@` annotations: a second header style beside the existing one.

Declared settings (a schema in the header and a generated settings page) are left out of the first version; see the open decisions.

## 5. `require` rules

**Recommendation.** The global `require` is wrapped once. It looks at the file of the calling function and, when that file belongs to a module, resolves in this order:

1. the module's own folder: `require("Decoder")` → `<module>\Decoder.lua`, `require("ui.List")` → `<module>\ui\List.lua`; cached per module, so two modules may each have a `Settings.lua`;
2. another module by name: `require("Json")` returns what that module's entry returned (a Warning once when it is not in `REQUIRES`);
3. `autorun\lib\`;
4. the ordinary `package.path`, unchanged.

Callers outside a module (loose scripts, tables, the console) get steps 2 to 4. Nobody has to touch `package.path`, and a reload drops the module's private cache, which replaces SigMaker's hand-kept purge list.

**Rejected.** Adding every module folder to `package.path`: same-named files collide and the first folder wins. A separate environment per module: globals are how CE-style scripts and the owner's tables talk to modules (`ManifoldSigMaker`, `ManifoldGodot`).

## 6. Enabling, disabling, reload, state

**Recommendation.**

- State lives in `configs\Manifold-Engine.modules.ini`, one section per module name: `Enabled`, `Allowed`, `Hash`, `FirstSeen`. `[Scripts] Autorun` stays the master switch.
- Every `manifold` registration (events, symbol resolvers, dissect hooks, `manifold.ui` entries, docs) is tagged with the module whose file made the call. *Unload* removes them and calls the functions the module registered with `manifold.module.onUnload(f)`.
- **Reload** = unload, drop the private `require` cache, run the entry again. **Disable** = unload now and skip at start. A module that leaves things the program cannot track (CE menu items, forms, timers) and has no `onUnload` is shown as *disabled, active until restart*, which is the truth.
- New area `manifold.module`: `current()` (name, version, path, dataPath), `list()`, `info(name)`, `reload(name)`, `enable(name)`, `disable(name)`, `onUnload(handler)`.

**Rejected.** State inside the module's folder or header (a script could switch itself back on; shipped files would differ from the repository). State in `Manifold-Engine.ini` (the hashes would bloat the settings file).

## 7. What the user sees

**Recommendation.** A *Modules* window (Extensions → Modules; Tools keeps *Open autorun folder*, and *Run autorun scripts again* becomes *Reload all modules*).

- Table: switch, name, category, version, origin (shipped, user, loose), state, load time.
- States: Loaded, Disabled, Failed, Blocked, New (held), Changed, Superseded, Built in.
- Detail pane for the selected row: description, author, path, requirements, registrations it holds, the error with traceback, and the buttons Reload, Allow, Open folder, Open data folder.
- A notice at start only when something needs attention ("1 new module is held: Isaac"), with a button that opens the window.

Log, channel `Autorun`:

```
info   Loaded SigMaker 2.1.0 (Tools) in 14 ms
info   Skipped Manifold-SigMaker.lua: superseded by modules\Tools\SigMaker
error  Godot failed: init.lua:212: attempt to index a nil value (ME-7003)
error  Isaac is blocked: it requires Json >= 1.2, found 1.0 (ME-7012)
warn   New module Planted (modules\Planted.lua) is held until it is allowed
warn   Script wrote autorun\modules\Tools\SigMaker\init.lua  (Crab.CT:118)
```

**Rejected.** A section in Settings (no room for errors and details). A tab in the Lua engine window (the list is for users who never open the console too).

## 8. Legacy files and migration

- Root `*.lua` and `custom\*.lua` run as today, in the same order, with the same globals, `getAutorunPath()` and freedom to edit `package.path`. They appear in the list as *loose*, can be switched off, and may carry a header.
- The two built-in replacements keep being skipped (state *Built in*).
- **Name clash:** when a loose script and a module in `modules\` have the same name, the module wins and the loose script is *Superseded* and skipped. Nothing is deleted. This is what makes the migration safe: after an install, the old `Manifold-Godot.lua` and `Manifold-SigMaker.lua` in the owner's folder stop running without anyone touching them.
- Shipped scripts move in the repository: `Autorun\modules\Engines\Godot.lua` (header gains `NAME : Manifold-Godot` so it supersedes the old file), `Autorun\modules\Tools\SigMaker\init.lua` with its module files and icons beside it. SigMaker loses the `package.path` line, the purge list and the API gate, and finds its icons through `manifold.module.current().path`. Build and install copy steps stay as they are.
- `Manifold-Json.lua` in the owner's folder is not in the repository and is not touched; it keeps running as a loose script and other modules can name it in `REQUIRES`.

## 9. Trust

The folder stays writable. The design makes planting visible and stops the silent case; it is not a sandbox.

1. **Held until allowed:** a module whose name the state file has never seen does not run. It is listed as *New (held)* with a start notice; *Allow* runs it and remembers its hash. At the first start with the new loader, everything already in the folder is adopted as known. Shipped modules that match the embedded hashes are never held.
2. **Writes seen by the program:** `io.open` for writing, `os.rename`, `os.remove`, `lfs` and the CE file functions log a Warning when the target is inside `autorun\` (not for `data\`). When the target is a module's code, that module is held again until allowed, and the Warning names the writing script.
3. **Changes not seen:** a hash that differs at start without a traced write (the owner's editor, most likely) runs, with state *Changed* and one Warning. Holding these would punish editing one's own scripts.

Limits, stated plainly: a script can also write the state file, or go through `shellExecute` (already traced). `[Scripts] HoldNewModules=0` turns point 1 off.

**Rejected.** Making the folder read-only for table scripts (decided against by the owner). Holding every changed module. Signatures.

## 10. Loader steps

1. Scan: root `*.lua`, `custom\*.lua`, `modules\` (two levels). Read each entry's header.
2. Resolve names; mark duplicates (*Superseded*, or *Failed: duplicate name* for two modules in `modules\`) and built-in replacements.
3. Hash each module's code; compare with the state file and the embedded shipped hashes; set *New*, *Changed*, *Disabled*.
4. Build the order (section 3); mark *Blocked* modules with their reason.
5. For each runnable module in order: set it as owner, run the entry under the error handler, record time, result, return value; on error mark dependents *Blocked*.
6. Write the state file if it changed; log one summary line; raise the notice if anything is held, failed or blocked.

Steps 1 to 4 are plain C++ without Lua (`Source/Lua/AutorunModules`), so they can be tested on folders alone.

## 11. Tests

- Scan: a folder tree with loose scripts, file and folder modules, categories, `lib`, `data`, a resource folder with `.lua` files and a nested `init.lua` inside a module. Only the right entries are found.
- Header: owner-style header, no header, CRLF and BOM, unknown keys, malformed `REQUIRES`.
- Order: equals today's name order without `REQUIRES`; dependency before dependent; missing, disabled, too old, cycle, failed dependency; one failing module leaves the others loaded.
- `require`: private file, same file name in two modules, another module by name, `lib`, fallback to `package.path`, cache dropped on reload.
- Lifecycle: registrations removed on unload, `onUnload` called, reload does not double anything, disable survives a restart, "active until restart" for untracked leftovers.
- Clash: loose script superseded by a module; built-in replacements still skipped.
- Trust: first run adopts, new module held, allow runs it, script write into a module holds it, write into `data\` does not, outside edit gives *Changed*, shipped hash never held.
- The existing `autorun` suite on the migrated shipped modules (two runs, quiet log, documented globals), and the generated `manifold.module` docs.

## Open decisions

1. **Entry file name** of a folder module: `init.lua` (recommended, the Lua convention) or `<Folder>.lua` (clearer editor tabs)?
2. **New modules:** held until allowed (recommended), or run with only a notice?
3. **Categories for the shipped modules:** `Engines\Godot` and `Tools\SigMaker` as shown, or other names, or no categories for now?
4. **SigMaker file names** after the move: keep `Manifold-SigMaker-Decoder.lua` … (smallest change, recommended for this round) or shorten to `Decoder.lua` …?
5. **Scope of the first version:** leave out declared settings (a settings schema in the header with a generated page), per-process modules (`PROCESS : isaac-ng.exe`, loaded when that game opens) and a crash guard (a module that was loading when the program died is held at the next start)? Recommended: leave all three out now; the header and state file have room for them.

## What the implementation does differently

- `manifold.module.current()` does not create the data folder; `manifold.module.ensureData()` does and returns its path. Loading the repository's `Autorun` folder in the tests must not write into it.
- The codes are `ME-7011` (no such module), `ME-7012` (blocked) and `ME-7013` (held); `ME-7010` was taken.
- The shipped modules keep their names, `Manifold-Godot` and `Manifold-SigMaker`, in the `NAME` line, so that the old loose files are superseded; the folders are `Engines\Godot.lua` and `Tools\SigMaker`.
- Whether a disabled module is "active until restart" is judged by what changed while it loaded: timers, hotkeys, Cheat Engine menus, forms, custom types and Auto Assembler commands. Globals a module sets and symbols it registered are not removed.
- A script write holds a module only when it goes to the `.lua` files of another module. A module writing next to its own files is not reported, since existing scripts keep their settings there.
- All open decisions were taken as recommended: `init.lua`, new modules held, the categories `Engines` and `Tools`, SigMaker file names kept, and no declared settings, per-process modules or crash guard in this version.
- Script writes are seen at `io.open`, `os.rename` and `os.remove`. `lfs` and the Cheat Engine file functions (`writeRegionToFile`, stream and string-list saves) are not traced yet; what they change still shows up as a changed or new module at the next start.
