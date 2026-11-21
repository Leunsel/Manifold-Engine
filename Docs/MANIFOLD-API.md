# Manifold Lua API

`manifold` is Manifold Engine's own Lua API. It lives beside the Cheat Engine functions in [LUA-API.md](LUA-API.md). Those keep Cheat Engine's names and habits for compatibility; `manifold` follows one fixed schema instead, and every call is checked against it.

This file is generated from the API itself (`Manifold-Engine.Tests.exe --manifold-markdown`), and the tests fail when it is out of date.

## Schema

1. **One namespace.** Everything lives in the global table `manifold`. Its fields are areas (`manifold.log`, `manifold.process`, …), constants (`manifold.apiVersion`, `manifold.version`) and `manifold.has`. An area may group its functions into sub-areas one level down (`manifold.ui.window`, `manifold.ui.dialog`).
2. **Names.** Areas are lower-case nouns. Functions are lowerCamelCase: a lookup without side effects is named after what it returns (`process.module`, `log.channel`), everything else starts with a verb (`open`, `read`, `create`). Predicates start with `is` or `has` and return a boolean. Settings come in `get…`/`set…` pairs. The five log functions are named after their level. Constants are lowerCamelCase fields. There are no aliases and no capitalised duplicates.
3. **Calls use a dot.** `manifold.log.info("ready")`, never `manifold.log:info("ready")`. No function takes `self`, and the objects the API returns (channels) work the same way. A colon call raises an error that says so.
4. **Arguments.** Required arguments come first, optional ones after them, and settings go into one `options` table at the end. A `?` after a name marks it optional. A missing, extra or wrongly typed argument and an unknown option key raise `ME-7007`, naming the function and the argument. Addresses are integers or address expressions such as `"game.exe+10"`.
5. **Results.** A function that can fail while the script runs (no process, unreadable memory, unknown symbol) returns its value on success, or `true` when it has none, and `nil, error` on failure. It does not raise for those. Predicates never fail.
6. **Errors.** An error is a table `{ code, message, where, traceback }`: `code` is the `ME-` code from [ERRORS.md](ERRORS.md) or a script's own code, `message` the text, `where` the script line that made the call (`chunk:line`), `traceback` the stack when `manifold.error.try` caught it. `tostring(error)` gives `where: message (code)`. Using the API the wrong way (rules 3 and 4, unknown names) raises such an error instead of returning it.
7. **Strict tables.** Reading a name that does not exist raises `ME-7007` with the nearest name (`manifold.log.warn` → `manifold.log.warning`), and every table is read-only. `manifold.has("log.channel")` tests for a name without raising.
8. **Versions.** `manifold.apiVersion` grows when functions are added. Existing names keep their meaning.

```lua
local log = manifold.log.channel("Teleporter")
local game, problem = manifold.process.info()
if not game then
  log.warning("no game open", { reason = problem.message })
  return
end
local ok, failure = manifold.error.try(function()
  local base = manifold.error.check(manifold.process.address("game.exe+1A2B30"))
  local health = manifold.error.check(manifold.memory.read(base + 0x10, "i32"))
  log.info("player found", { base = base, health = health })
end)
if not ok then
  log.error(failure)
end
```

## manifold

| Name | Value | |
| --- | --- | --- |
| `manifold.apiVersion` | 13 | The API version, an integer. It grows when functions are added; names keep their meaning. |
| `manifold.version` | string | The program version, such as `"0.1.0"`. |
| `manifold.has(name)` | boolean | Whether the API has a name, such as `"log.channel"`, `"memory"` or `"apiVersion"`. Use it instead of reading a name that may not exist, which raises. |

## manifold.log

Coloured, levelled lines in the Lua engine output. Each line shows the time, the level and the channel; `fields` are appended as `{key = value}`. Warnings and errors also go to the program log (Log window and the log file in `logs\`), with the script line that wrote them.

| Function | Returns | |
| --- | --- | --- |
| `manifold.log.debug(message, fields?)` | nothing | Writes a debug line: details that only matter while working on a script. Hidden unless the level is `debug`. |
| `manifold.log.info(message, fields?)` | nothing | Writes an info line. |
| `manifold.log.success(message, fields?)` | nothing | Writes a success line, in the accent colour. |
| `manifold.log.warning(message, fields?)` | nothing | Writes a warning line, in the warning colour. It also goes to the program log. |
| `manifold.log.error(message, fields?)` | nothing | Writes an error line, in the error colour. It also goes to the program log. An error object as `message` adds its traceback. |
| `manifold.log.channel(name)` | channel | A channel: an object with the same five level functions that tags every line with `[name]`. The same name gives the same object. |
| `manifold.log.setLevel(level)` | nothing | Drops lines below `level` for every script until the program closes. The default is `"info"`. |
| `manifold.log.getLevel()` | level name | The current level. |
| `manifold.log.inspect(value?, options?)` | string | One line of text for any value, the way log lines show `fields`: sorted keys, cycles marked, 50 items per table. `options.depth` (default 3) limits nesting; `options.hex` writes integers in hex. |

### Channel objects

`manifold.log.channel(name)` returns an object that follows the same rules: dot calls only, read-only, unknown names raise.

| Name | Returns | |
| --- | --- | --- |
| `channel.debug(message, fields?)` | nothing | Like `manifold.log.debug`, tagged with `[name]`. |
| `channel.info(message, fields?)` | nothing | Like `manifold.log.info`, tagged with `[name]`. |
| `channel.success(message, fields?)` | nothing | Like `manifold.log.success`, tagged with `[name]`. |
| `channel.warning(message, fields?)` | nothing | Like `manifold.log.warning`, tagged with `[name]`. |
| `channel.error(message, fields?)` | nothing | Like `manifold.log.error`, tagged with `[name]`. |
| `channel.channel(name)` | channel | A sub-channel named `parent/name`. |
| `channel.name` | string | The channel's name. |

## manifold.output

The Lua engine window and its output. With *Show on print* on (the default), every printed or logged line opens the window when it is closed. Set in the window's View menu, it is saved with the settings; a script can change it for the running session only.

| Function | Returns | |
| --- | --- | --- |
| `manifold.output.show()` | nothing | Opens the Lua engine window. |
| `manifold.output.clear()` | nothing | Clears the output. |
| `manifold.output.getShowOnPrint()` | boolean | Whether printing opens the Lua engine window. |
| `manifold.output.setShowOnPrint(enabled)` | nothing | Turns *Show on print* on or off until the program closes. The saved setting stays as the user set it. |

## manifold.process

The opened process. Without one, every function that needs it returns `nil` and an `ME-1005` error.

| Function | Returns | |
| --- | --- | --- |
| `manifold.process.isOpen()` | boolean | Whether a process is open and still running. |
| `manifold.process.info()` | `{ id, name, path, is64Bit }`, or nil, error | The opened process. |
| `manifold.process.open(target)` | `{ id, name, path, is64Bit }`, or nil, error | Opens a process by id or by executable name (`"game.exe"`, or the start of the name). |
| `manifold.process.name()` | string, or nil, error | The file name of the process's executable module, such as `"game.exe"`: the same name the Cheat Engine global `process` holds, read when it is asked for. Addresses name that module's base as `mainmodule` or, as in Cheat Engine, `$process`, so `"mainmodule+1A2B30"` works in memory records, pointers, Auto Assembler scripts and here. |
| `manifold.process.find(name?)` | list of `{ id, name, path, windowTitle, is64Bit }` | Running processes whose executable name contains `name` (any case); all of them without `name`. |
| `manifold.process.mainModule()` | `{ name, base, size, path }`, or nil, error | The process's executable module. Its base is what `mainmodule` and `$process` stand for in addresses. |
| `manifold.process.module(name)` | `{ name, base, size, path }`, or nil, error | A loaded module by file name, any case. `ME-1016` when it is not loaded. |
| `manifold.process.moduleAt(address)` | `{ name, base, size, path }`, or nil, error | The module that contains the address. `ME-1017` for addresses outside every module: the heap, stacks and code the game allocated itself. |
| `manifold.process.modules()` | list of `{ name, base, size, path }`, or nil, error | All loaded modules. |
| `manifold.process.address(expression)` | integer, or nil, error | Resolves an address expression such as `"game.exe+10"` or `"[[base]+8]+4"`. The error says which part failed. |
| `manifold.process.describe(address)` | string, or nil, error | The address as `module.export+offset`, `module+offset` or hex. |

## manifold.memory

Typed reads and writes in the opened process. Types: `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` (integers; `u64` above 2^63 comes back negative, as Lua integers are signed), `float`, `double`, `pointer` (8 or 4 bytes by the process), `bytes` (a list of byte values), `raw` (the bytes as one Lua string, made for `string.unpack`) and `string` (UTF-8 in Lua, UTF-16 in memory with `wide`). A write checks that the value fits the type before it touches memory. Reads of the plain types and of `raw` take a fast path, so they suit loops over many objects.

| Function | Returns | |
| --- | --- | --- |
| `manifold.memory.read(address, type, options?)` | value, or nil, error | Reads one value. `bytes` and `raw` need `options.count`; `string` reads up to `options.length` characters (default 256) and stops at the first zero. |
| `manifold.memory.write(address, type, value, options?)` | true, or nil, error | Writes one value. A `string` ends with a zero unless `options.terminate` is false. |
| `manifold.memory.scan(pattern, options?)` | list of integers, or nil, error | Finds a byte pattern such as `"48 8B 05 ?? ?? ?? ??"` (`??` is any byte) in the whole process, or only in `options.module`. `options.executable` and `options.writable` set to true keep only such memory, set to false only memory without it, and left out take both. `options.alignment` keeps addresses divisible by it; `options.limit` (1 to 1000000, default 100000) stops after that many hits. The list is in address order, so with a limit it holds the lowest hits. No hit is an empty list, not an error. |
| `manifold.memory.isReadable(address)` | boolean | Whether the page at the address can be read. False without a process or for an address that does not resolve. |

## manifold.error

Error objects and protected calls. An error object is `{ code, message, where, traceback }`; `tostring` gives `where: message (code)`.

| Function | Returns | |
| --- | --- | --- |
| `manifold.error.create(message, options?)` | error object | A new error object for the calling line. `options.code` sets the script's own code; the default is `ME-7002`. |
| `manifold.error.is(value?)` | boolean | Whether `value` is an error object. |
| `manifold.error.raise(problem, options?)` | never returns | Raises an error object: `problem` itself when it is one, else a new one from the text. |
| `manifold.error.try(fn, ...)` | true, results..., or false, error | Calls `fn(...)`. On an error it returns false and an error object with the line that raised it and the traceback, instead of stopping the script. |
| `manifold.error.check(value?, problem?, ...)` | its arguments | Passes its arguments through when `value` is neither nil nor false, else raises `problem`. Made for `nil, error` results: `local health = manifold.error.check(manifold.memory.read(address, "i32"))`. |

## manifold.symbol

Symbols that address expressions, the disassembler and Auto Assembler scripts know. A registered symbol belongs to the opened process: closing it or opening another one removes it, so a module registers its symbols again from a `processOpened` handler or when it resolves.

| Function | Returns | |
| --- | --- | --- |
| `manifold.symbol.register(name, address)` | true, or nil, error | Registers `name` for `address`, replacing an earlier symbol of that name. |
| `manifold.symbol.unregister(name)` | true, or nil, error | Removes a registered symbol. `ME-8002` when there is none of that name. |
| `manifold.symbol.addResolver(resolver, options?)` | id | Calls `resolver(name)` for a name that is neither a module, an export nor a registered symbol; it returns an address or nil. Resolvers are asked in the order they were added, and the first address wins. An error in a resolver is logged once. `options.key` replaces an earlier registration with the same key, so a module that runs again does not register twice. |
| `manifold.symbol.removeResolver(id)` | boolean | Removes a resolver; false when the id is unknown. |

## manifold.event

Notifications from the program. Handlers run in the order they subscribed; an error in one is logged with its traceback and does not stop the others. Handlers run while the program waits, so they should only reset state: long work belongs in a thread or happens on first use.

| Function | Returns | |
| --- | --- | --- |
| `manifold.event.subscribe(name, handler, options?)` | id | Calls `handler` on an event. `processOpened` passes the process as `manifold.process.info()` gives it, also when the same process is opened again; `processClosed` passes nothing and comes before the next `processOpened`; `unrealReady` passes nothing and follows when the Unreal tool has resolved the game. `options.key` replaces an earlier registration with the same key, so a module that runs again does not register twice. |
| `manifold.event.unsubscribe(id)` | boolean | Removes a handler; false when the id is unknown. |

## manifold.structure

Hooks into Structure Dissect. Hooks are asked in the order they were added, and the first answer wins. An error in a hook is logged once and counts as no answer.

| Function | Returns | |
| --- | --- | --- |
| `manifold.structure.addNameResolver(resolver, options?)` | id | Calls `resolver(address)` when Structure Dissect needs a name for a new structure or for a pointer it expands. It returns a name and, optionally, the address where the object starts, or nil. `options.key` replaces an earlier registration with the same key, so a module that runs again does not register twice. |
| `manifold.structure.removeNameResolver(id)` | boolean | Removes a name resolver; false when the id is unknown. |
| `manifold.structure.addDissector(dissector, options?)` | id | Calls `dissector(structure, address)` before Structure Dissect guesses fields. It fills the structure (the Cheat Engine structure object: `addElement`, `autoGuess`, …) and returns true, or returns false to leave it to the next dissector and the guess. `options.key` replaces an earlier registration with the same key, so a module that runs again does not register twice. |
| `manifold.structure.removeDissector(id)` | boolean | Removes a dissector; false when the id is unknown. |
| `manifold.structure.resolveName(address)` | name and start, nil when no resolver answers, or nil, error | Asks the name resolvers in order, as Structure Dissect does, and returns the first answer: the name and the address where the object starts (the given address when the resolver names none). Resolvers that Cheat Engine scripts registered are asked too; the program's own Mono lookup is not. |
| `manifold.structure.dissect(structure, address)` | boolean, or nil, error | Asks the dissectors in order, as Structure Dissect does before it guesses, to fill `structure` (a Cheat Engine structure object, e.g. from `createStructure`) for the object at `address`. True when one of them filled it. |

## manifold.unreal

Unreal Engine 4 and 5 reflection for 64-bit and 32-bit games, built into the program: it finds the game's name table, object array and reflection layout itself, without an SDK or game offsets, and only reads. Structure Dissect then knows Unreal objects (real property names, nested structs, bitfield bools, the native gaps), the custom types `UE FName`, `UE FString` and `UE Object Index` show names, strings and weak references, and `<Type>_<Property>` symbols such as `Actor_RootComponent` resolve to offsets in every session. It starts by itself when an Unreal game is opened, unless `configure{ autoStart = false }` says otherwise, and the window is in Extensions > Unreal Engine. The queries need it resolved and return `nil` and an `ME-4201` error before that. Types are given by name (`"Actor"`, without the U, A or F prefix) or by address.

| Function | Returns | |
| --- | --- | --- |
| `manifold.unreal.status()` | `{ state, text, failure, objects, types, namePool, objectArray, objectArraySource, legacyNames, doubleVectors, seconds }` | Where the Unreal tool stands. `state` is `idle`, `notUnreal` (the opened game is no Unreal game), `resolving`, `ready`, `failed` (with `failure`) or `stopped`; `text` is the line the menu shows. The counts and addresses are there once it is ready. |
| `manifold.unreal.start(options?)` | true, or nil, error | Resolves the opened process again, in any process, not only one that looks like Unreal. It runs in the background and the `unrealReady` event follows; with `options.wait` it resolves before it returns and gives the error when it fails. After `stop` this is what turns the tool on again. |
| `manifold.unreal.stop()` | true | Shuts the Unreal tool down: it drops what was resolved, its symbols and dissects stop, and nothing starts by itself again until `start`, also in the next game. |
| `manifold.unreal.configure(options)` | true, or nil, error | Changes how the tool works; keys that are left out keep their value, and the settings last until the program closes, so an autorun script sets them once. `autoStart` (true): resolve when an Unreal game is opened. `dissect`, `names`, `symbols`, `types` (all true): Structure Dissect fields, structure names, the `<Type>_<Property>` and `GObjects` symbols, the custom types. For a game the search does not find: `namePool` (the FNamePool, or with `legacyNames = true` the TNameEntryArray of UE 4.22 and older) and `objectArray` (FUObjectArray or its ObjObjects) as addresses or expressions, `layout` as a table of offsets that replace what the probes find (`index`, `class`, `name`, `outer`, `super`, `childProperties`, `structSize`, `children`, `childNext`, `functionFlags`, `fField`, `fieldClass`, `fieldName`, `next`, `offset`, `size`, `extra`, `itemSize`, `itemOffset`), and `patterns = { names = {...}, objects = {...} }` searched before the built-in search. A pattern is an AOB string, or `{ pattern, offset, length, relative, add, module }`: the hit plus `offset` holds a 32-bit displacement relative to the end of the instruction (`length` bytes long, `offset + 4` by default), or with `relative = false` an absolute pointer of the game's pointer size, which is what 32-bit code holds; `add` is added to the result, and `module` (the executable by default) is where the pattern is searched. `false` clears an address, the layout or the patterns. A change to the addresses, layout or patterns applies at the next `start`. |
| `manifold.unreal.config()` | `{ autoStart, dissect, names, symbols, types, namePool, objectArray, legacyNames, layout, patterns }` | The settings `configure` changes, as it would take them. |
| `manifold.unreal.layout()` | table of offsets, or nil, error | The layout the tool found: the object array items (`itemSize`, `itemOffset`, `chunked`), UObject (`index`, `class`, `name`, `outer`), UStruct (`super`, `childProperties`, `structSize`, `children`, `childNext`, `functionFlags`) and the properties (`fField`, `fieldClass`, `fieldName`, `next`, `offset`, `size`, `extra`, `component` 4 or 8 for float or double vectors). The keys are the ones `configure{ layout = ... }` takes. |
| `manifold.unreal.count()` | integer, or nil, error | How many slots the object array has in use. |
| `manifold.unreal.objectAt(index)` | address or nil, or nil, error | The object in a slot of the object array; nil for an empty slot. |
| `manifold.unreal.isObject(address)` | boolean | Whether a live UObject starts at the address: its index points back at it in the object array. False before the tool is ready. |
| `manifold.unreal.name(address)` | string or nil, or nil, error | The FName stored at an address, with its number as `Name_1`. |
| `manifold.unreal.nameOf(object)` | string or nil, or nil, error | An object's name. |
| `manifold.unreal.fullName(object)` | string, or nil, error | An object's path through its outers, such as `/Game/Maps/Level.Level.PersistentLevel.Pawn_1`. |
| `manifold.unreal.classOf(object)` | address or nil, or nil, error | An object's class. |
| `manifold.unreal.type(name)` | address or nil, or nil, error | A class or struct by name, any case, without the U, A or F prefix. |
| `manifold.unreal.types(filter?)` | list of `{ name, kind, address }`, or nil, error | Every class, struct and enum, sorted by name; with `filter`, the ones whose name contains it. `kind` is `class`, `struct` or `enum`. |
| `manifold.unreal.super(type)` | address or nil, or nil, error | The parent of a class or struct. |
| `manifold.unreal.size(type)` | integer or nil, or nil, error | The size of a class or struct in bytes. |
| `manifold.unreal.cppName(type)` | string, or nil, error | A type's C++ spelling: `F` for structs, `A` for actors, `U` for other classes. |
| `manifold.unreal.fields(type, ownOnly?)` | list of fields, or nil, error | The reflected properties of a class or struct, its parents' included unless `ownOnly`, sorted by offset. A field is `{ name, type, typeText, owner, offset, start, size, dim, flags, address, inner, metaClass, byteOffset, bitMask }`: `type` is the property class (`FloatProperty`), `typeText` the C++ type (`TArray<AActor*>`), `start` the offset of the first byte (a bitfield bool adds `byteOffset` and has `bitMask`), `inner` the struct, class or enum it refers to. |
| `manifold.unreal.field(type, name)` | field, or nil, error | One property by name, any case, searched in the parents too; `ME-4203` when there is none. A field is `{ name, type, typeText, owner, offset, start, size, dim, flags, address, inner, metaClass, byteOffset, bitMask }`: `type` is the property class (`FloatProperty`), `typeText` the C++ type (`TArray<AActor*>`), `start` the offset of the first byte (a bitfield bool adds `byteOffset` and has `bitMask`), `inner` the struct, class or enum it refers to. |
| `manifold.unreal.offset(type, name)` | symbol, offset, or nil, error | The offset symbol of a property (`Actor_RootComponent`) and its value. The symbol works in address expressions in every session, so records written with it keep working after a game update. |
| `manifold.unreal.functions(type, inherited?)` | list of `{ name, owner, address, flags, parameters }`, or nil, error | The UFunctions a class declares, and with `inherited` its parents' too. `parameters` are fields; the return value is the one with the return flag (`0x400` in `flags`). |
| `manifold.unreal.enum(enum)` | list of `{ name, value }`, or nil, error | The entries of an enum, by name or address. |
| `manifold.unreal.instances(type, limit?)` | list of addresses, or nil, error | The live objects of a class and its subclasses, without the class default objects. One pass over the object array; `limit` stops early. |
| `manifold.unreal.find(name)` | address or nil, or nil, error | The first object with that name, or with that full name when `name` has a dot in it. |
| `manifold.unreal.owner(address)` | object, class, or nil, error | The object an address belongs to and its class; `ME-4204` when no live object holds it. |
| `manifold.unreal.whois(address)` | string, or nil, error | One line about an address: the object, its class, the offset, the property path down to the innermost field and that field's type, such as `BP_Pawn_C_1 (ABP_Pawn_C) +130: RootComponent  USceneComponent*`. |
| `manifold.unreal.locate(address)` | `{ object, class, offset, path, field, within, struct }`, or nil, error | What `whois` says, as a table: `path` is the property path as text, `field` the innermost field, `within` the offset inside it, and `struct` the outermost struct property that starts exactly at the address. |
| `manifold.unreal.engine()` | address or nil, or nil, error | The GameEngine object (or the Engine of an editor build). |
| `manifold.unreal.world()` | address or nil, or nil, error | The world the engine's game viewport shows; nil while no level is loaded. |
| `manifold.unreal.controller()` | address or nil, or nil, error | The first local player's PlayerController. |
| `manifold.unreal.pawn()` | address or nil, or nil, error | The pawn the local PlayerController possesses. |
| `manifold.unreal.offline()` | boolean or nil, or nil, error | true when the world has no net driver or one without connections, false when it is connected or hosts remote players, nil when it cannot tell. Writes in an online session can get the account banned. |
| `manifold.unreal.walk(object, steps, root?)` | address, expression, field, or nil, error | Follows properties by name from an object: `walk(manifold.unreal.pawn(), { "RootComponent", "RelativeLocation" })`. Object properties are followed, structs entered, and `Name[i]` picks a TArray element. Gives the address of the last step, an address expression from `root` (the object's address by default) with offset symbols, such as `[GameEngine+GameEngine_GameViewport]+...`, and the last field. |
| `manifold.unreal.dump(type)` | list of lines, or nil, error | A class or struct as C++-like lines: its name, parent and size, then one line per property with offset, type, name and the class that declares it. |
| `manifold.unreal.claims(address)` | boolean | Whether the tool names the address itself (a live object or a remembered array), for other dissect scripts that step aside for Unreal objects. Never raises, false before it is ready. |

## manifold.rtti

C++ class names from the run-time type information of the opened process, built into the program. Every object of a C++ class with virtual functions starts with a pointer to its class's virtual function table, and in programs built with MSVC or clang-cl with RTTI on (the compiler's default) the slot in front of that table leads to the class's name and base classes. This area reads that in 64-bit and 32-bit processes; nothing is injected or called. Structure Dissect uses it to name structures and pointer fields and to embed member objects (Extensions > C++ RTTI turns that off), and `<Class>::vftable` and `<Class>::vftable@<Base>` symbols resolve in every session. Unreal Engine builds switch RTTI off and GCC builds use another format; there the lookups find nothing. Classes are given by name with their namespaces (`"Game::Player"`) or by decorated name (`".?AVPlayer@Game@@"`).

| Function | Returns | |
| --- | --- | --- |
| `manifold.rtti.className(address)` | string or nil, or nil, error | The class of the object whose virtual function table pointer is stored at the address; nil when there is none. |
| `manifold.rtti.object(address)` | `{ address, start, offset, class, decorated, vtable, module, viaVirtualBase }` or nil, or nil, error | The object whose virtual function table pointer is stored at the address. `start` is where the complete object starts, which is below the address when it holds a base class's table pointer; `viaVirtualBase` says the object starts with a virtual base table and was found through it. |
| `manifold.rtti.owner(address, window?)` | the table `object` gives or nil, or nil, error | The nearest object at or before the address, searched back over `window` bytes (0x4000 by default); `offset` is the address's offset in it. A member object with virtual functions of its own counts as an object, because RTTI does not say how large a class is. |
| `manifold.rtti.whois(address)` | string, or nil, error | Several lines about the object at or before an address: the class, the object's start, the decorated name, the virtual function table, the module and the base classes with their offsets. `ME-4301` when no object is found. |
| `manifold.rtti.bases(what)` | list of `{ name, decorated, depth, offset, virtual }`, or nil, error | A class and its base classes in the order of the hierarchy, the class itself first; `depth` is the level below it. `what` is a class name (a string is always a name, since names such as `Face` would also read as hex) or the address of an object. The offset of a virtual base is known only for an object and missing for a name. |
| `manifold.rtti.vtable(name, base?)` | address or nil, or nil, error | A class's virtual function table: the primary one, or the one for a base class part, given by the base's name or its offset in hex. The class is searched in the process's modules by name, without an index of the whole game; nil when it is not found. |
| `manifold.rtti.vtables(name)` | list of `{ address, offset, symbol, module }`, or nil, error | Every virtual function table of a class, ordered by the offset of the part it belongs to, with the symbol it is registered under. |
| `manifold.rtti.instances(name, limit?)` | list of addresses, or nil, error | The objects of exactly that class: one search of the process's writable memory for the class's primary table pointer, at most `limit` hits (100000 by default, 1000000 at most). Objects of derived classes have another table and are not found. |
| `manifold.rtti.classes()` | list of names, or nil, error | The classes met so far in this process, sorted. It is what the lookups have seen, not an index of the game. |
| `manifold.rtti.demangle(decorated)` | string | The readable name of a decorated type name: `.?AVPlayer@Game@@` gives `Game::Player`. Templates, nested and anonymous namespaces, local classes and lambdas are read; a name it cannot read comes back unchanged. Needs no process. |
| `manifold.rtti.mangle(name)` | list of decorated names, or nil | The decorated names a plain class name can have, as a class and as a struct: `Game::Player` gives `.?AVPlayer@Game@@` and `.?AUPlayer@Game@@`. nil for a name with template arguments or other characters. Needs no process. |
| `manifold.rtti.dissect(address)` | true, or nil, error | Opens Structure Dissect on the object at or before the address, with a structure named after its class; the structure is made, labelled and given its embedded members when none of that name exists. |
| `manifold.rtti.label(structure, address)` | number of labelled fields, or nil, error | What Structure Dissect does after its guess, for a structure that already exists (a Cheat Engine structure object): pointer fields without a name of their own are named `vftable`, `vftable (Base)`, `<Class> vftable`, `<Class>*` or `vbtable`, member objects with a table of another class become embedded structures named after that class, and a structure laid over a virtual function table gets `virtual N`. 0 when the address is neither an object's start nor a table. |
| `manifold.rtti.status()` | `{ text, dissect, process, is64Bit, modules, classes, vtables }` | What is known about the opened process: `text` is the line the menu shows, `dissect` whether Structure Dissect uses RTTI, and the counts are what the lookups have seen. Without a process only `text` and `dissect` are there. |
| `manifold.rtti.reset()` | true | Forgets what was learned about the process and takes back the `::vftable` symbols it registered. The next lookup starts fresh. |
| `manifold.rtti.claims(address)` | boolean | Whether Structure Dissect's RTTI stage names the address itself: an object or a virtual function table that neither the Unreal tool nor a Godot script claims. Never raises, false without a process or with the stage turned off. |

## manifold.speed

The speedhack: the opened process's clocks run faster or slower by a factor. Setting a speed loads a small library into the process and redirects its time functions (`QueryPerformanceCounter`, `GetTickCount`, `GetTickCount64`, `timeGetTime`) through it; sleeps and the wall clock stay as they are. A speed change never makes a clock jump or run backwards. `disable` goes back to normal speed and takes the hooks out when the clocks were never shifted; after a shift the hooks stay at normal speed so the game's time does not jump, and `remove` takes them out regardless. Works in 64-bit and 32-bit processes. If Manifold Engine ends while a speed is set, the process goes back to normal speed by itself.

| Function | Returns | |
| --- | --- | --- |
| `manifold.speed.get()` | number | The speed factor in use: 1 while the speedhack is off, 2 for double speed, 0.5 for half speed. |
| `manifold.speed.set(speed)` | true, or nil, error | Turns the speedhack on if needed and sets the factor, from 0 (time stands still) to 1000. `ME-1005` without a process, `ME-1019` when the time functions cannot be redirected. |
| `manifold.speed.isEnabled()` | boolean | Whether a speed is set. |
| `manifold.speed.disable()` | true, or nil, error | Goes back to normal speed. The hooks are taken out when the process's clocks still equal the real ones; otherwise they stay and pass time on at normal speed. |
| `manifold.speed.remove()` | true, or nil, error | Takes the hooks out and restores the time functions byte for byte. After a speed other than 1 the process's clocks jump to the real time at that moment, backwards if the game ran faster. |
| `manifold.speed.status()` | `{ enabled, speed, installed, clockShifted, hooks }` | The state of the speedhack: `installed` says whether the hooks are in the process, `clockShifted` whether its clocks differ from the real ones, and `hooks` lists `{ name, address }` for every redirected time function. |

## manifold.doc

The documentation behind completion, signature help, hover tooltips and the docs panel. Manifold, Cheat Engine and Lua functions are documented already. A module documents its own functions with `register`, or writes LuaLS annotations (`---@param name type`) above them and calls `registerModule`. A script never replaces Manifold's own documentation.

| Function | Returns | |
| --- | --- | --- |
| `manifold.doc.register(target, spec)` | true, or nil, error | Documents a function, or a name such as `"MyModule.run"`. `spec.doc` is the text, `spec.params` a list of `{ name, type, optional, doc, oneOf }`, `spec.returns` what comes back and `spec.overloads` more `{ params, returns }`. A function without `spec.path` is documented wherever the help reaches it. |
| `manifold.doc.registerModule(path, module, options?)` | number of registered functions, or nil, error | Documents every function of a module table under `path` (such as `"ManifoldGodot"`), from the LuaLS annotations above each function, or its parameter names when it has none. `options.doc` describes the module. Call it again after adding functions. |
| `manifold.doc.inherit(target, source)` | true, or nil, error | Gives a wrapper the documentation of what it wraps, so a `function(...)` wrapper keeps the parameters and text of the original in the help. |
| `manifold.doc.unregister(target)` | true, or nil, error | Removes documentation that a script registered. Manifold's, Cheat Engine's and Lua's own documentation stays. |
| `manifold.doc.entry(target)` | `{ path, name, kind, origin, summary, text, signatures, … }`, or nil, error | The documentation of a function or name, as the help shows it. `signatures` is a list of `{ text, params, returns }`; each param is `{ name, type, text, optional, variadic, choices }`. Without documentation the error is `ME-7008`. |
| `manifold.doc.find(query, options?)` | list of `{ path, kind, summary }` | Searches names and texts, best matches first. `options.limit` (default 50) caps the list; `options.kind` keeps one kind, such as `"function"` or `"table"`. |
| `manifold.doc.annotations(fn)` | `{ … }` like `entry`, or nil, error | Reads the LuaLS annotations above a function's definition without registering anything. |
| `manifold.doc.markdown(prefix)` | string, or nil, error | Markdown tables of everything documented under `prefix`, the way Docs/MANIFOLD-API.md is written. |

## manifold.ui

Windows, dialogs, notices, menu entries, toolbar buttons and status entries that Manifold draws itself, in its own look. A script describes what it wants (a window with a row of buttons, a table, a canvas) and keeps the handles it gets back; Manifold draws them every frame and calls the script's handlers when something happens. Colours are theme names (`accent`, `warning`, `muted`, ...), icons are Font Awesome names (`map-marker-alt`, `play`, ...). Everything a script made belongs to that script: when the same script runs again, its old windows, entries, notices and dialogs go away first.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.getMode()` | table | How the main window looks: `{ release = boolean, compact = boolean }`. *Release* shows three columns, hides the Edit, Tools and Debug menus and toggles records with a double click; *compact* hides the scanner and shows only the address list. View > Compact Mode and View > Release Mode (Ctrl+Shift+C, Ctrl+Shift+R) switch the same. |
| `manifold.ui.setMode(mode)` | nothing | Switches release and/or compact mode, like View > Release Mode and View > Compact Mode, and saves it with the settings. A key left out keeps its state, so `setMode{ compact = true }` changes only compact mode. |

### Menu, toolbar and status entries

`menu.add`, `toolbar.add` and `status.add` return an entry object; it takes the options its `add` took.

| Name | Returns | |
| --- | --- | --- |
| `entry.update(options)` | boolean | Changes the entry: the options its `add` took. |
| `entry.remove()` | boolean | Takes the entry out. |
| `entry.id` | integer | The entry's number. |

## manifold.ui.window

Tool windows that dock into the workspace like Manifold's own. A window holds widgets; containers (`row`, `column`, `grid`, `section`, `tabs` with `tab`, `split`) hold more widgets.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.window.create(title, options?)` | window | Opens a window and returns its handle. Options: `icon` (Font Awesome name), `width` and `height` (the first size, in points), `open` (false creates it hidden), `dock` (false keeps it out of the dock space), `opacity` (0.1 to 1, while the window has the focus) and `inactiveOpacity` (while another window has it; default `opacity`): without them the user's window opacity settings apply, and only a floating window can be see-through, a docked one stays opaque; `onClose(window)` when the user closes it (it is hidden, not removed; `show` brings it back), `onUpdate(window)` every `interval` seconds while it is open (default every frame), and `key`: a window created with the same key replaces the earlier one. |

### Window objects

`manifold.ui.window.create` returns a window object: dot calls only, read-only, unknown names raise. A call on a window that is gone (removed, or its script ran again) returns `nil, error` with code `ME-7010`.

| Name | Returns | |
| --- | --- | --- |
| `window.add(kind, options?)` | widget | Adds a widget at the end of the window and returns its handle. `kind` is one of `row`, `column`, `grid`, `section`, `tabs`, `split`, `text`, `heading`, `separator`, `spacer`, `button`, `checkbox`, `input`, `number`, `slider`, `combo`, `radio`, `color`, `table`, `tree`, `progress`, `image`, `canvas`; the options depend on it (see the widget object). |
| `window.clear()` | boolean | Removes every widget of the window. |
| `window.remove()` | boolean | Removes the window and its widgets for good. |
| `window.update(options)` | boolean | Changes the title, icon, opacity (`opacity`, `inactiveOpacity`), update interval or handlers. |
| `window.show()` | boolean | Shows the window again after the user or `hide` closed it. |
| `window.hide()` | boolean | Hides the window; `show` brings it back with its widgets. |
| `window.focus()` | boolean | Shows the window and brings it to the front. |
| `window.isOpen()` | boolean | Whether the window exists and is shown. |
| `window.id` | integer | The window's number. |
| `window.kind` | string | Always `window`. |

### Widget objects

`add` returns a widget object, with the same rules as a window object. Every widget takes `visible`, `enabled`, `tooltip`, `width` and `height` (points; 0 or missing means automatic; a negative `width` fills the rest of the line). Handlers get the widget as their last argument.
- `row`, `column`: lay out their widgets side by side or one below the other; a row's `align` (`left`, `center`, `right`) moves its widgets to that side of the line.
- `grid`: places its widgets in `columns` (2) cells per row, row by row. The first column fits its content and the others share the rest, so `text` then a field per row lines labels up; a field without a `label` fills its cell.
- `section`: a card with `title`, `icon`, `badge`; `collapsible` lets the user fold it, `open = false` starts it folded.
- `tabs` holds `tab` widgets (`title`, `icon`); `split` shows its first two widgets side by side (`vertical` stacks them) at `ratio` (0.5).
- `text` (`text`, `color`, `wrap`, `mono`), `heading` (`text`, `icon`), `separator` (`text` puts a caption into the line), `spacer`.
- `button`: `text`, `icon`, `style` (`default`, `accent`, `danger`), `onClick(widget)`.
- `checkbox` (`label`, boolean `value`), `input` (`label`, string `value`, `hint`, `multiline`, `password`, `onSubmit(text, widget)` on Enter), `number` (`label`, `value`, `min`, `max`, `step`, `integer`, `hex`, `format` like `%.2f`), `slider` (`label`, `value`, `min`, `max`, `integer`, `format`), `combo` (`label`, `items`, `value` = the chosen item's number), `radio` (the same as `combo`, drawn as one button per item; `horizontal` puts them in one line), `color` (`label`, `value` = `#RRGGBB`, `alpha` for `#RRGGBBAA` with an alpha channel): all call `onChange(value, widget)`.
- `table`: `columns` (titles, or `{ title, width, align }`), `rows` (lists of cells), `value` = the selected row's number, `onSelect(row, widget)`, `onActivate(row, widget)` on a double click, and `menu` (a list of entries; `-` is a separator) with `onMenu(choice, text, row, widget)`.
- `tree`: `items` (`{ text, icon, id, open, children }`, or plain strings), `value` = the selected item's `id`, `onSelect(id, widget)`, `onActivate(id, widget)`, `menu` with `onMenu(choice, text, id, widget)`.
- `progress` (`value` 0..1, `text`), `image` (`image` from `manifold.ui.image.load`).
- `canvas`: draws what `draw` gave it on the GPU. `background` is a colour, `zoom = true` lets the user zoom with the wheel and pan by dragging; `onClick(x, y, button, widget)`, `onHover(x, y, widget)`, `onView(x, y, scale, widget)` and `menu` with `onMenu(choice, text, x, y, widget)` get canvas coordinates; `onKey(key, widget)` gets every key pressed (and repeated while held) while the canvas's window has the focus, by name: `LeftArrow`, `W`, `Space`, `Enter`, `F1`… (Escape also closes the window).

| Name | Returns | |
| --- | --- | --- |
| `widget.add(kind, options?)` | widget | Adds a widget to a container (`row`, `column`, `section`, `tab`, `split`; `tabs` take `tab` widgets only). |
| `widget.clear()` | boolean | Removes a container's widgets, or a canvas's drawings. |
| `widget.remove()` | boolean | Removes the widget and everything in it. |
| `widget.update(options)` | boolean | Changes options of the widget: the same keys `add` takes for its kind, handlers included. |
| `widget.get()` | value | The widget's value: a checkbox's boolean, an input's text, a colour's `#RRGGBB`, a number, the chosen combo, radio or table row's number, the selected tree item's id, a progress fraction, otherwise the text. Returns nil when nothing is selected. |
| `widget.set(value?)` | boolean | Sets the widget's value (see `get`); nil clears a selection. It does not call `onChange`. |
| `widget.draw(drawings, options?)` | boolean | Replaces a canvas's drawings (`options.append` adds them instead). A drawing is a table with `kind`: `line` (`x`, `y`, `x2`, `y2`, `thickness`), `rect` (`x`, `y`, `width`, `height`, `filled`, `rounding`, `thickness`), `circle` (`x`, `y`, `radius`, `filled`, `thickness`), `polygon` (`points` = `{ x1, y1, x2, y2, ... }`, `filled`), `text` (`x`, `y`, `text`, `size`) or `image` (`image`, `x`, `y`, `width`, `height`); all but images take `color` (a theme name or `#RRGGBB`/`#RRGGBBAA`). |
| `widget.getView()` | table | A canvas's view `{ x, y, scale }`: the canvas point at its top left corner and the zoom. |
| `widget.setView(view)` | boolean | Moves or zooms a canvas's view. |
| `widget.id` | integer | The widget's number. |
| `widget.kind` | string | The kind it was added as. |

## manifold.ui.form

Windows described by a table, as the Form Editor (Tools > Form Editor) saves them in a `.form.lua` file. The table holds the layout and the names of the handlers; the script keeps the code and passes its handler functions in.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.form.build(form, handlers?)` | window | Builds a window from a form table and returns `window, widgets`: the window object, and a read-only table of the widgets that have a `name`, by name (an unknown name raises). The table has `form = 1`, `title`, the window options (`icon`, `width`, `key`, `onClose`, ...) and `children`, a list of widget tables: `kind`, an optional `name`, the options of that kind and their own `children`. Handler options hold a name, like `onClick = "save"`, and are looked up in `handlers` (`handlers.save`); a missing one raises. Without `handlers` the handlers are left out, which shows what the form looks like. |
| `manifold.ui.form.load(path, handlers?)` | window | Reads a `.form.lua` file and builds its window like `build`; returns `window, widgets`: the window object, and a read-only table of the widgets that have a `name`, by name (an unknown name raises). A relative path starts at the folder of the calling script (at the program folder for code without a file). Returns nil and an `ME-7014` error when the file cannot be read or does not return a table; a wrong form raises like `build`. |

## manifold.ui.dialog

Dialogs on top of the program. They do not block the script: each takes `onClose`, which gets the answer, or nil when the user cancelled.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.dialog.confirm(text, options?)` | dialog | Asks a yes/no question. `onClose(answer)` gets true or false; `danger = true` shows the yes button in the warning colour. |
| `manifold.ui.dialog.input(text, options?)` | dialog | Asks for a line of text, starting from `value`. `onClose(text)` gets the text, or nil. |
| `manifold.ui.dialog.choose(text, items, options?)` | dialog | Lets the user pick one of `items`. `onClose(number, text)` gets the chosen item's number and text, or nil. |
| `manifold.ui.dialog.openFile(options?)` | dialog | The Windows file dialog for opening a file. `filters` is a list of `{ "Lua scripts", "*.lua" }` pairs; `onClose(path)` gets the full path, or nil. |
| `manifold.ui.dialog.saveFile(options?)` | dialog | The Windows file dialog for saving, starting with `name`. `filters` is a list of `{ "Lua scripts", "*.lua" }` pairs; `onClose(path)` gets the full path, or nil. |

### Dialog objects

The dialog functions return at once; the dialog shows on top of the program and calls `onClose` with the answer when the user is done.

| Name | Returns | |
| --- | --- | --- |
| `dialog.close()` | boolean | Closes the dialog without an answer; `onClose` is not called. |
| `dialog.id` | integer | The dialog's number. |

## manifold.ui.notice

Short notices in the lower right corner, the same ones Manifold shows. A notice fades after a few seconds unless it is `sticky` or has a `progress`; a right click dismisses it.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.notice.show(text, options?)` | notice | Shows a notice. `level` is `info` (default), `success`, `warning` or `error`; `progress` (0..1) adds a bar and keeps the notice up until it reaches 1; `sticky` keeps it until it is removed or dismissed. |

### Notice objects

`manifold.ui.notice.show` returns a notice object.

| Name | Returns | |
| --- | --- | --- |
| `notice.update(options)` | boolean | Changes the text, level, progress or stickiness, and shows the notice for its full time again. |
| `notice.remove()` | boolean | Takes the notice down. |
| `notice.isShown()` | boolean | Whether the notice is still up. |
| `notice.id` | integer | The notice's number. |

## manifold.ui.menu

Entries in the main window's Extensions menu, beside the menus of Cheat Engine scripts. The menu shows as soon as a script adds one.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.menu.add(path, options?)` | entry | Adds an entry. `path` may name submenus with slashes (`Teleporter/Save position`); `-` as the last part is a separator. Options: `icon`, `enabled`, `checked` (shows a check mark), `onClick(entry)`. |

## manifold.ui.toolbar

Buttons in the main window's title bar, between the process and the Memory view button.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.toolbar.add(icon, options?)` | entry | Adds an icon button. Options: `text` beside the icon, `tooltip`, `enabled`, `onClick(entry)`. |

## manifold.ui.status

Entries in the main window's status bar, after the administrator note.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.status.add(text, options?)` | entry | Adds a status entry. Options: `icon`, `tooltip`, `color` (a theme name), `onClick(entry)`. |

## manifold.ui.image

Pictures for `image` widgets and canvas drawings.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.image.load(path)` | image | Loads a PNG, JPEG, BMP, TGA or GIF file (a relative path starts at the program folder) and returns an image object with `width`, `height` and `remove()`, or nil and an `ME-7009` error. |

### Image objects

`manifold.ui.image.load` returns an image object.

| Name | Returns | |
| --- | --- | --- |
| `image.remove()` | boolean | Frees the picture; widgets and drawings that show it go blank. |
| `image.id` | integer | The image's number. |
| `image.width` | integer | Width in pixels. |
| `image.height` | integer | Height in pixels. |

## manifold.ui.theme

The names `manifold.ui` takes for colours and icons.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.theme.colors()` | table | The theme colour names, which follow the theme and light or dark mode. |
| `manifold.ui.theme.icons(filter?)` | table | The Font Awesome icon names, all of them or those that contain `filter`. |
| `manifold.ui.theme.isIcon(name)` | boolean | Whether `name` is a Font Awesome icon name. |

## manifold.module

The autorun modules: the scripts in the `autorun` folder and the modules in `autorun\modules`. A module is named by the `NAME` line of its header, or by its file or folder name. Extensions > Modules shows the same list. [AUTORUN.md](AUTORUN.md) describes the folder layout, the header and what `require` finds.

| Function | Returns | |
| --- | --- | --- |
| `manifold.module.current()` | table, or nil, error | The module the calling script belongs to, as a table with `name`, `version`, `author`, `description`, `category`, `kind` (`loose`, `file` or `folder`), `origin` (`shipped` or `user`), `state` (`loaded`, `disabled`, `failed`, `blocked`, `held`, `superseded`, `builtIn` or `pending`), `reason`, `enabled`, `changed`, `path` (the module's folder), `entry` (its entry file), `dataPath` (its folder under `autorun\data`), `loadTime` in milliseconds and `requires` (a list of `{ name, version }`). `ME-7011` when the caller is not an autorun module, such as a table script or the console. |
| `manifold.module.ensureData()` | string, or nil, error | Creates the calling module's data folder, `autorun\data\<name>\`, when it is not there yet, and returns its path with a trailing backslash. A module keeps its settings and other files there: writing them does not count as a change to its code, and the module's own folder can be replaced as a whole. `ME-7011` when the caller is not an autorun module. |
| `manifold.module.list()` | table | Every script and module the autorun folder holds, in the order of the list in Extensions > Modules, each as the table `current` returns. |
| `manifold.module.info(name)` | table, or nil, error | One module by name, as the table `current` returns. `ME-7011` when there is none of that name. |
| `manifold.module.reload(name)` | true, or nil, error | Unloads a module and runs it again from disk, and after it the loaded modules that require it. The error is the reason the module did not load: `ME-7003` with the script error, `ME-7012` when it is blocked or disabled, `ME-7013` when it is held. |
| `manifold.module.enable(name)` | true, or nil, error | Switches a module on, loads it now and remembers it for the next start. The error is the reason it did not load, as for `reload`. |
| `manifold.module.disable(name)` | true, or nil, error | Switches a module off and remembers it for the next start. The module is unloaded, and so are the modules that require it. `info(name).enabled` tells which way the switch is. |
| `manifold.module.onUnload(handler)` | true, or nil, error | Calls `handler()` when the calling module is unloaded: before it is reloaded, when it is switched off, and before all modules run again. The program removes what the module registered through `manifold` on its own; the handler takes down the rest, such as Cheat Engine menu items, forms and timers. Handlers run in reverse order. `ME-7011` when the caller is not an autorun module. |
