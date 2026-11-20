# Manifold Engine

A memory scanner, debugger and scripting tool for Windows games and programs, in the spirit of Cheat Engine. It reads and writes Cheat Engine tables (`.CT`), runs Cheat Engine's Auto Assembler and Lua scripts, and draws every tool in its own window on the graphics card.

## Highlights

- **Scanner**: all the usual value types, many threads, results on disk instead of in memory, an *All* scan over every number type at once, and grouped scans for values that sit next to each other.
- **Address list**: groups, pointers, freezing, hotkeys, drop-down values, and Cheat Engine's group options.
- **Memory viewer and debugger**: disassembler with symbols and jump arrows, a flow graph per function, write history with undo, conditional breakpoints that run inside the target, tracing, and "find out what accesses / writes".
- **Structure dissect** that names objects by their class in Mono, IL2CPP and Unreal Engine games, and C++ objects by their RTTI.
- **Game engines built in**: Mono and IL2CPP (Unity) with classes, methods and a scene explorer, and Unreal Engine 4 and 5 without an SDK or game offsets.
- **Scripting**: Cheat Engine's Auto Assembler, templates, and Lua 5.4 with the Cheat Engine API, plus the program's own `manifold` API with script windows drawn in the program's look.
- **Made for long sessions**: error codes that explain themselves, a log window for debugging, a profiler, and the keyboard shortcuts of Cheat Engine 7.5.

## Getting started

Build with Visual Studio 2022 and clang-cl (the C++ desktop workload plus *C++ Clang Compiler for Windows* and *MSBuild support for LLVM (clang-cl) toolset*):

```bash
msbuild Manifold-Engine.sln /p:Configuration=Release /p:Platform=x64 /m
```

The program lands in `Build\x64\Release\Manifold-Engine.exe`. Many games need the program to run as administrator (Help → Restart as administrator). Protected processes and anti-cheat systems are not supported.

## Documentation

| | |
| --- | --- |
| [Features](Docs/FEATURES.md) | everything the program does, window by window |
| [Development](Docs/DEVELOPMENT.md) | building, tests, project layout, CI |
| [Architecture](Docs/ARCHITECTURE.md) | layers, threads, and where new code goes |
| [Auto Assembler](Docs/AUTO-ASSEMBLER.md), [Templates](Docs/TEMPLATES.md) | the script language and the script templates |
| [Lua API](Docs/LUA-API.md), [manifold API](Docs/MANIFOLD-API.md), [Cheat Engine coverage](Docs/CE-COMPAT.md) | scripting |
| [Autorun modules](Docs/AUTORUN.md) | Godot and signature modules that run at start |
| [Shortcuts](Docs/SHORTCUTS.md), [Error codes](Docs/ERRORS.md) | reference |

## License

MIT, see [LICENSE](LICENSE). Third-party code (Dear ImGui, Zydis, Lua, stb, Font Awesome) keeps its own license, see [Licenses](Licenses).

Manifold Engine is an independent project. It is not affiliated with or endorsed by Cheat Engine or its author; the name Cheat Engine is used only to describe compatibility.
