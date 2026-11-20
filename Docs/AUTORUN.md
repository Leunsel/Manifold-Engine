# Autorun modules

The `Autorun` folder holds Lua modules that Manifold Engine runs at start when they sit in the `autorun` folder next to the program. They are built on the program's own [manifold API](MANIFOLD-API.md) (version 2 or newer) and do not run in stock Cheat Engine.

| File | Global | What it does |
| --- | --- | --- |
| `Manifold-Godot.lua` | `ManifoldGodot` | Godot 4 runtime: StringNames, ObjectDB, ClassDB, the scene tree and method symbols (`Class::method`); fills structures and adds a *Godot Engine* menu. Godot 3 is declined with a message. |
| `Manifold-SigMaker.lua` + `Manifold-SigMaker-Modules` | `ManifoldSigMaker` | Makes byte signatures for the selected instruction in the memory viewer, checks that they are unique in their module, and finds them again (Ctrl+Shift+F). |

Unreal Engine support and C++ run-time type information are built into the program (Extensions → Unreal Engine and Extensions → C++ RTTI, [`manifold.unreal`](MANIFOLD-API.md) and [`manifold.rtti`](MANIFOLD-API.md)). The program skips an old `Manifold-UE.lua` or `Manifold-RTTI.lua` in its `autorun` folder and logs it; those files can be deleted. The global `ManifoldRTTI` of the RTTI script is still there: its functions call `manifold.rtti` and return what the script returned, so tables and scripts written against it keep working, and `<Class>::vftable` symbols in saved tables resolve as before.

Building the program copies the folder into `autorun` next to the executable. Only files that are newer in the repository than the copies there are copied, so a module changed in the program's `autorun` folder after a build stays until the repository's file changes again. Other files there are left alone.

## How they use the API

- Output goes to log channels (`Godot`, `SigMaker`) with levels; listings the user asks for are info lines. Start-up lines are debug, so *Show on print* does not open the Lua engine at every start.
- Memory is read with `manifold.memory.read`, objects as one `raw` block unpacked with `string.unpack` instead of one read per field.
- Pattern scans use `manifold.memory.scan`, symbols `manifold.symbol`, Structure Dissect hooks `manifold.structure`, and process changes arrive through `manifold.event`. Every registration carries a key, so running a file again replaces its hooks instead of adding a second set.
- Failures come back as error objects with an `ME-` code or the module's own code (`GODOT-STAGE`, …); menu actions catch them with `manifold.error.try` and log them with the traceback.
- Forms, menus, dialogs, custom types and the structure objects that Structure Dissect hands to the hooks are still the Cheat Engine functions, since the API has no counterpart for them yet.

## Living together

The Unreal tool, Godot and the built-in RTTI stage never answer for each other's objects. The Unreal tool resolves only in a game that looks like Unreal (or when it is started by hand), Godot only behind its version literal, and RTTI asks the Unreal tool and `ManifoldGodot.claims` before it names an object; `manifold.rtti.claims` tells a script what RTTI names. Structure Dissect asks the built-in Unreal tool, then Mono, then RTTI, and only then the script hooks; script hooks are asked in the order they were registered, and the first answer wins.

## Checking them

`Manifold-Engine.Tests.exe` loads the folder twice without a process, opens and closes the test program and checks that nothing reports an error or a wrong API call, that neither the Unreal tool nor Godot claims the test program, and that `ManifoldRTTI` is documented and still embeds a member object. For a deeper look, the headless harness runs them against a process without writing to it:

```bash
Manifold-Engine.Tests.exe --compat Tests\Target\TestTarget.CT --autorun=Autorun --process=Game.exe --dry-run --home=<empty folder> --eval=<script.lua>
```

Checked so far, read-only:

- Crab Champions (UE 4.2x): the built-in Unreal tool resolves in 0.8 s and names and fills `GameEngine`, `World` and the pawn; RTTI named only the objects without reflection (`CSteamUserV021`), checked with the RTTI script the built-in stage replaced; Godot declines.
- Unrailed 2 (Godot 4.6.3 .NET): Godot resolves in 0.24 s with the measured layout, names and fills the scene root, and registers 14 611 method symbols in 0.26 s; UE declines; the RTTI script left the Godot objects to Godot.
- Headless against the test program: both modules; SigMaker with ten module-unique signatures. The Unreal tool and RTTI have their own test suites (`unreal` with a synthetic Unreal image, `rtti` with hand-built and compiler-made RTTI in the 64-bit and the 32-bit test program).

Not checked yet: UE5 and modular Unreal builds, other Godot 4 releases, the built-in RTTI on a real game (64-bit or 32-bit).
