# Autorun modules

The `Autorun` folder holds Lua modules that Manifold Engine runs at start when they sit in the `autorun` folder next to the program. They are built on the program's own [manifold API](MANIFOLD-API.md) (version 2 or newer) and no longer run in stock Cheat Engine; the Cheat Engine versions they were ported from stay unchanged in the CheatEngineLua repository.

| File | Global | What it does |
| --- | --- | --- |
| `Manifold-RTTI.lua` | `ManifoldRTTI` | Names C++ objects from MSVC RTTI in Structure Dissect and registers `<Class>::vftable` symbols; finds the owner of an address and instances of a class. |
| `Manifold-Godot.lua` | `ManifoldGodot` | Godot 4 runtime: StringNames, ObjectDB, ClassDB, the scene tree and method symbols (`Class::method`); fills structures and adds a *Godot Engine* menu. Godot 3 is declined with a message. |
| `Manifold-SigMaker.lua` + `Manifold-SigMaker-Modules` | `ManifoldSigMaker` | Makes byte signatures for the selected instruction in the memory viewer, checks that they are unique in their module, and finds them again (Ctrl+Shift+F). |

Unreal Engine support was `Manifold-UE.lua` here and is built into the program now (Extensions → Unreal Engine, [`manifold.unreal`](MANIFOLD-API.md)). The program skips an old copy of that file in its `autorun` folder and logs it; the copy can be deleted.

Building the program copies the folder into `autorun` next to the executable. Only files that are newer in the repository than the copies there are copied, so a module changed in the program's `autorun` folder after a build stays until the repository's file changes again. Other files there, such as `Manifold-Json.lua`, are left alone.

## How they use the API

- Output goes to log channels (`RTTI`, `Godot`, `SigMaker`) with levels; listings the user asks for are info lines. Start-up lines are debug, so *Show on print* does not open the Lua engine at every start.
- Memory is read with `manifold.memory.read`, objects as one `raw` block unpacked with `string.unpack` instead of one read per field.
- Pattern scans use `manifold.memory.scan`, symbols `manifold.symbol`, Structure Dissect hooks `manifold.structure`, and process changes arrive through `manifold.event`. Every registration carries a key, so running a file again replaces its hooks instead of adding a second set.
- Failures come back as error objects with an `ME-` code or the module's own code (`GODOT-STAGE`, …); menu actions catch them with `manifold.error.try` and log them with the traceback.
- Forms, menus, dialogs, custom types and the structure objects that Structure Dissect hands to the hooks are still the Cheat Engine functions, since the API has no counterpart for them yet.

## Living together

The Unreal tool, Godot and RTTI never answer for each other's objects. The Unreal tool resolves only in a game that looks like Unreal (or when it is started by hand), Godot only behind its version literal, and RTTI asks `manifold.unreal.claims` and `ManifoldGodot.claims` before naming an address. Structure Dissect asks the built-in Unreal tool (and Mono) before any script hook; script hooks are asked in the order they were registered, and the first answer wins.

## Checking them

`Manifold-Engine.Tests.exe` loads the folder twice without a process, opens and closes the test program and checks that nothing reports an error or a wrong API call and that neither the Unreal tool nor Godot claims the test program. For a deeper look, the headless harness runs them against a process without writing to it:

```bash
Manifold-Engine.Tests.exe --compat Tests\Target\TestTarget.CT --autorun=Autorun --process=Game.exe --dry-run --home=<empty folder> --eval=<script.lua>
```

Checked so far, read-only:

- Crab Champions (UE 4.2x): the built-in Unreal tool resolves in 0.8 s and names and fills `GameEngine`, `World` and the pawn; RTTI names only the objects without reflection (`CSteamUserV021`); Godot declines.
- Unrailed 2 (Godot 4.6.3 .NET): Godot resolves in 0.24 s with the measured layout, names and fills the scene root, and registers 14 611 method symbols in 0.26 s; UE declines; RTTI leaves the Godot objects to Godot.
- Headless against the test program: all three; RTTI against the test program's own RTTI classes and synthetic ones; SigMaker with ten module-unique signatures. The Unreal tool has its own test suite (`unreal`) with a synthetic Unreal image.

Not checked yet: UE5 and modular Unreal builds, other Godot 4 releases, RTTI on a 32-bit program.
