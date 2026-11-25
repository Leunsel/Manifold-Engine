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
| `manifold.apiVersion` | 1 | The API version, an integer. It grows when functions are added; names keep their meaning. |
| `manifold.version` | string | The program version, such as `"1.0.0"`. |
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

## manifold.record

The memory records of the open cheat table: the address list. A record is found by its id or its description and comes back as a record object; scripts and groups are switched on and off, value records also read and write their value as the address list does (its type, hex display and drop-down list apply). A record object stays bound to its record: when the record is deleted or the table is replaced (new, opened, restored), its functions return `nil` and an `ME-6009` error.

| Function | Returns | |
| --- | --- | --- |
| `manifold.record.find(record)` | record | The record with this id, the one record with this description (as written in the address list), or the one at this path: the descriptions from the top level down, `{ "Player", "Stats", "Health" }`, as `record.path()` gives them. Ids change when records are copied between tables and names repeat in many tables, but a path stays as long as the groups keep their names. A description or a path has to lead to exactly one record: `nil` and `ME-6011` with the count when several match (`findAll` lists the records with a description). `nil` and an `ME-6008` error when there is none. |
| `manifold.record.findAll(description)` | table | Every record with this description, in the address list's order; an empty list when there is none. |
| `manifold.record.all()` | table | Every record of the table, groups and their children included, in the address list's order from top to bottom. |
| `manifold.record.selected()` | table | The records selected in the address list, in the order they were selected. |
| `manifold.record.create(options)` | record | Adds a record at the end of the table, or below `options.parent` (a record object or id). `kind` is `value` (the default), `script` or `group`; a value record takes `address` (an address expression, like in the address list) and `type` (as `info` names them; `4bytes` by default), a script record takes `script` (Auto Assembler text with `[ENABLE]` and `[DISABLE]`). |

### Record objects

`find`, `all`, `selected` and `create` return record objects: dot calls only, read-only, unknown names raise. Two objects of the same record are the same object.

| Name | Returns | |
| --- | --- | --- |
| `record.info()` | table | What the record is: `{ id, description, kind, active, children, hotkeys, parent, comment }`, `kind` being `value`, `script` or `group`; a value record adds `type` (`byte`, `2bytes`, `4bytes`, `8bytes`, `float`, `double`, `string`, `bytes`, `binary`, `pointer` or its custom type's name), `address` (the expression as written), `offsets` (how many pointer offsets), `resolved` (the address it points to now, when it resolves) and `hex`. |
| `record.getDescription()` | string | The record's description, as the address list shows it. |
| `record.setDescription(description)` | boolean | Changes the record's description. |
| `record.getValue()` | value | Reads the record's value: a number for the number types (an integer for the integer types, also when the address list shows it as hex or as a drop-down entry), otherwise the text as the address list shows it (a string, bytes). The text as the address list shows it comes second, and third the description of the drop-down entry the value matches, when the record has a drop-down list. `nil` and an error when the address does not resolve or cannot be read; `ME-6010` for scripts and groups. |
| `record.setValue(value)` | boolean | Writes the record's value, as typing it into the address list does: a number, or a text in the record's own form (hex digits for a hex record, a drop-down entry's value or description, a string). A frozen record keeps the new value. `nil` and an error when it cannot be written. |
| `record.isActive()` | boolean | Whether the record is switched on: an enabled script, a frozen value, an active group. |
| `record.setActive(active)` | boolean | Switches the record on or off, as its box in the address list does: a script runs its `[ENABLE]` or `[DISABLE]` part, a value freezes or thaws, a group follows its options for its children. `true` once the record is in that state. Inside a script's coroutine with asynchronous activation on, it waits for the activation without blocking the program. Asked from inside the record's own switch (from its script or one of its handlers), the new switch runs after the current one and counts as done. `nil` and an error when the record did not end in that state: the script's error, the reason when `onMemRecPreExecute` or the record's `OnActivate` or `OnDeactivate` handler refuses the switch, `ME-6009` when Lua of the switch deleted the record. |
| `record.toggle()` | boolean | Switches the record to the other state and returns the new one; `nil` and an error when that fails, as for `setActive`. |
| `record.getAddress()` | integer | The address the record's value is at now (pointers followed). `nil` and an error when it does not resolve; `ME-6010` for scripts and groups. |
| `record.children()` | table | The records directly below this one, in the address list's order. |
| `record.parent()` | record | The record this one is below, or nil at the top level. |
| `record.find(record)` | record | A record below this one: by its description at any depth (`script.find("Multiplier:")`), or by a path that starts at this record's children (`script.find({ "Type:", "Multiplier:" })`). Tables repeat names below every script, so this finds a script's own child however often the name occurs elsewhere. Exactly one record must match: `nil` and `ME-6011` with the count when several do, `ME-6008` when none does. |
| `record.path()` | table | The descriptions from the top level down to this record, its own last: the path `manifold.record.find` takes. It stays the same when the record's id changes, as long as the groups above it keep their names. |
| `record.remove()` | boolean | Deletes the record and the records below it from the table. Every record of it that is on is switched off first, the deepest first, so scripts take out their hooks and allocations; each removed record's `OnDestroy` handler runs. `nil` and an error, the record staying in the table, while a record of it is being switched, when one of its scripts fails to switch off or stays on, or when Lua of a switch turned one of its scripts on again; `ME-6009` when that Lua deleted it or replaced the table. |
| `record.id` | integer | The record's id, the number Cheat Engine tables store with it. |

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
| `manifold.event.subscribe(name, handler, options?)` | id | Calls `handler` on an event. `processOpened` passes the process as `manifold.process.info()` gives it, also when the same process is opened again; `processClosed` passes nothing and comes before the next `processOpened`; `unrealReady` passes nothing and follows when the Unreal tool has resolved the game; `themeChanged` passes the look as `manifold.ui.theme.current()` gives it, after the user, Windows (System mode) or a script changed it, at most once a frame. `options.key` replaces an earlier registration with the same key, so a module that runs again does not register twice. |
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

## manifold.signature

Byte signatures: a pattern for an address that matches nowhere else, so a script or an `aobscanmodule` finds the place again after the game is updated. `make` grows the pattern from the address one byte at a time, with the numbers that move between builds (addresses, jump distances, structure offsets, large constants) as wildcards, until no other place in the module (or the process) matches it. The defaults are the ones under Settings > Signatures; the options of a call override them for that call. `parse` reads signatures in the shapes other tools write, and `find` searches for one. Copy Signature and the Signature Maker in the memory viewer use the same functions.

| Function | Returns | |
| --- | --- | --- |
| `manifold.signature.make(address, options?)` | `{ pattern, mask, code, array, autoAssembler, address, start, offset, length, wildcards, instructions, module, scope, uniqueIn, verified, functionEnd, seconds, bytes }`, or nil, error | Makes the shortest pattern from the address that matches only there. `pattern` is it as an AoB (`"48 8B 05 ?? ?? ?? ?? 8B 88"`), `mask` the matching `x`/`?` string, `code`, `array` and `autoAssembler` the other ways to write it; `bytes` lists `{ value, wild, role }` per byte. With `options.offset` the pattern may start at an instruction up to `maxOffset` bytes before or after the address when that saves `minSaving` bytes: `start` is where it begins and `offset` is `address - start`, so the address is a scan hit plus `offset`. `options.scope` is `"module"` (unique in the address's module, like `aobscanmodule`) or `"process"`; `codeOnly` counts executable memory only. `branches` and `offsets` take `"keep"`, `"long"` (32-bit ones only) or `"all"`; `immediates` takes `"keep"`, `"large"` (from `immediateThreshold` up) or `"all"`; `maskAddresses` false keeps RIP-relative and absolute addresses. `registers` takes `"keep"`, `"sib"` (the base and index registers of memory operands, the default) or `"all"` (also ModRM bytes whose reg field is a register, never an opcode extension). `maxBytes` (up to 1024) and `minBytes` bound the length, `stopAtFunctionEnd` never goes past a ret, jmp or int3. `wildcard` (`"??"`, `"?"`, `"**"`, `"*"`), `spaces` and `label` shape the text. `functionEnd` is set when the pattern reaches past the end of the function. `ME-2018` when no pattern within `maxBytes` is unique; a module-wide search takes milliseconds, a process-wide one reads all memory. |
| `manifold.signature.parse(text)` | `{ pattern, length, fixed, wildcards, partial, source, module, notes }`, or nil, error | Reads a signature as other tools write it: `"48 8B ?? ??"`, `"48 8B ? ?"`, `"48 8B ** *"`, `"488B????"`, the C string with its mask (`"\x48\x8B\x00", "xx?"`), a C array (`{ 0x48, 0x8B }`), nibble wildcards (`"4?"`) or a whole `aobscanmodule(name,module,bytes)` line, with comment and header lines around it. `pattern` is it written as `manifold.memory.scan` takes it, `partial` counts the half-known bytes, `source` says which shape it was, `module` is the module of an `aobscanmodule` line, and `notes` lists what was read in a way that may surprise (a C string without a mask keeps its zeros). `ME-2003` when it does not read as a pattern. |
| `manifold.signature.find(text, options?)` | list of integers, or nil, error | Reads a signature like `parse` and returns where it matches, in address order: in `options.module`, else in the module of an `aobscanmodule` line, else in the whole process. `options.executable` true searches code only; `options.limit` (1 to 100000, default 100) stops after that many hits. No hit is an empty list. |

## manifold.rebase

Rebasing shows a module at another base, the way IDA's Rebase Program does: at the image base its PE header asks for (the addresses IDA and Ghidra show for the file, whatever ASLR picked this time), at 0 for RVAs, or at a base of your own. Only the addresses on screen change (the disassembler, the hex view, Copy and Go to Address); the process, the instructions and everything written stay at the real addresses, and symbol expressions keep reading real addresses. Rules are kept by module name in the settings, so they hold for the next start of the game. The memory viewer and Symbols and Modules > Modules use the same rules.

| Function | Returns | |
| --- | --- | --- |
| `manifold.rebase.set(module, base)` | true | Shows the module at `base`: `"image"` for the image base in its PE header, or a number (0 shows RVAs). The module need not be loaded yet; the rule holds whenever a module of that name is. `clear` takes it back. |
| `manifold.rebase.get(module)` | entry, or nil | The rebase of the module, or nil when it is shown where it is loaded. An entry is `{ module, rule, base }`: `rule` is `"image"` or the chosen base, `base` the base the module is shown at now (nil while it is not loaded). |
| `manifold.rebase.clear(module)` | true | Shows the module at its real addresses again. |
| `manifold.rebase.list()` | list of entries | Every rebased module, by name. An entry is `{ module, rule, base }`: `rule` is `"image"` or the chosen base, `base` the base the module is shown at now (nil while it is not loaded). |
| `manifold.rebase.shown(address)` | integer, or nil, error | The address as it is shown: rebased when it lies in a rebased module, else the address itself. |
| `manifold.rebase.real(shown)` | integer, or nil, error | Where a shown address really is, such as an address copied from IDA: the real address when it lies in a rebased module as shown, else the number itself. With rebased modules shown over each other the first in the module list wins. |
| `manifold.rebase.imageBase(module)` | integer, or nil, error | The image base in the PE header of a loaded module: where it asks to be loaded, and where `"image"` shows it. `ME-1016` when no module has the name. |

## manifold.codefilter

The code filter: finds code by what it does instead of by a value. `prepare` collects the function starts of the game's modules (the unwind table of 64-bit modules, the exports and checked call targets), and every round puts a one-time int3 on each one that is left, inside the process through the agent library: the first thread that runs into one puts the byte back and runs on. A `"filterOut"` round drops what ran during it (let the game run without doing the thing), a `"keepOnly"` round keeps only what ran (do the thing once), so a few rounds leave the code behind an action. While a round runs every read through Manifold shows the original bytes, and a write over an armed byte takes its int3 back first. Tools > Code Filter is the same filter: the window and these functions share one state. Games that check their own code or run an anti-cheat notice the int3s.

| Function | Returns | |
| --- | --- | --- |
| `manifold.codefilter.prepare(options?)` | `{ total, seconds, modules }`, or nil, error | Collects the candidates and starts a new filter (the rounds and the undo steps before are gone). `options.modules` lists module names, by default every module that is not part of Windows or Manifold's own library; `unwind`, `exports` and `pointers` (all on by default) and `calls` (on by default for 32-bit, from Settings for 64-bit) choose the sources; `pointers` takes the functions that are only reached through a pointer (virtual methods from their vtables, callbacks, function tables), from the module's relocations and, with `calls`, the code's own address references. `modules` in the result lists `{ name, functions, unwind, exports, calls, pointers, imported, rejected }` per module. Call targets take a code dissect of each module, which takes a while in a large game. `ME-1016` for a module that is not loaded, `ME-1020` when nothing can be collected. |
| `manifold.codefilter.start(kind)` | true, or nil, error | Starts a round on what is left: `"filterOut"` or `"keepOnly"`. Addresses the agent cannot arm (system code, an int3 already there, a page that cannot be written) are dropped from the list. `ME-1020` when `prepare` was not called, a round already runs or nothing is left. |
| `manifold.codefilter.stop()` | `{ kind, before, after, hits, skipped, seconds }`, or nil, error | Ends the round and applies it: `filterOut` drops every candidate that ran, `keepOnly` keeps only those. Returns the round's record. `ME-1020` when no round runs. |
| `manifold.codefilter.cancel()` | true, or nil, error | Ends the round without changing the list. Nothing happens when no round runs. |
| `manifold.codefilter.undo()` | boolean | Goes back one step (a round or a refine), up to 32 steps. False when there is nothing to undo or a round runs. |
| `manifold.codefilter.reset()` | true | Ends a running round and forgets the candidates, the rounds and the undo steps. |
| `manifold.codefilter.refine(maxFunctions?)` | integer, or nil, error | Splits every function that is left into its basic blocks, from its flow graph, so the next rounds show which part of it ran; blocks have the source `"block"` and their `function`. Returns how many candidates there are now. `ME-1020` when more than `maxFunctions` (default 256) functions are left or every candidate is a block already. |
| `manifold.codefilter.import(addresses, options?)` | `{ imported, lines, duplicates, rejected, total, problems, seconds }`, or nil, error | Takes the candidates from a list of addresses instead of collecting them, like Filter > Import Addresses: a text with one address per line (hex, `module+offset`, a symbol or any address expression; empty lines and lines starting with `#`, `;` or `//` are skipped; a line with more words uses its first), or a table of addresses and texts. An address must be executable code where an instruction starts; code outside a module (JIT code) is fine. By default the list replaces the candidates and starts a new filter; `options.add` adds it to the collected list instead, which only works before the first round. `problems` holds the first 20 rejected lines with the reason. `ME-1020` when no line is an address in code, or `add` after a round. |
| `manifold.codefilter.export()` | string | What is left as text for `import` or Filter > Import Addresses: a comment header, then one `module+offset` per line, which still points at the same code after the game restarted. |
| `manifold.codefilter.candidates(limit?)` | list of `{ address, name, function, sources }` | What is left, in address order, at most `limit` (default 1000, up to 1000000). `name` is the address as the disassembler names it, `sources` lists where it came from (`"unwind"`, `"export"`, `"call"`, `"pointer"`, `"import"`, `"block"`), `function` is set for blocks. |
| `manifold.codefilter.status()` | `{ prepared, running, kind, remaining, total, liveHits, seconds }` | The filter's state: whether candidates are collected, whether a round runs and which, how many are left of how many collected, and for a running round how many candidates ran so far and for how long. |
| `manifold.codefilter.history()` | list of `{ kind, before, after, hits, skipped, seconds }` | The steps since `prepare`, oldest first: rounds (`"filterOut"`, `"keepOnly"`) and refines (`"refine"`). |

## manifold.crash

The crash watch: finds the script behind a crash of the game. Every Auto Assembler run that changes the game notes what it put there (its allocations and the bytes it patched), and once a script has written code the agent library notes every exception that ends a process, in memory Manifold Engine owns. When the game ends with a crash, the last exception is matched against the scripts: in a script's code, in or right behind its patch, in its memory after it was switched off, or with its code on the stack. The result goes into the log and a notification, and `last` returns it. Settings: [Crashes] Watch.

| Function | Returns | |
| --- | --- | --- |
| `manifold.crash.last()` | `{ process, headline, details, exitCode, exact, watched, code, address, accessed, thread, secondsBeforeEnd, scripts, switchedOn, lastSwitched, lastSwitchedSeconds }`, or nil | The report of the last crash in this session, or `nil` when none crashed. `scripts` lists the scripts involved, most likely first, as `{ script, recordId, link, where, address }`; `link` is `"code"` (the failing instruction is in its code), `"patch"` (in bytes it patched), `"behindPatch"` (right behind its patch), `"freed"` (in its memory after it was switched off), `"unbalanced"` (its hook leaves the stack unbalanced, from the Auto Assembler's stack check), `"nearPatch"` (the game failed shortly behind its patch),`"accessed"` (the failing access reached its memory), `"stack"` (its code is on the stack) or `"earlier"` (an exception in its code shortly before). `exact` is true when the exit code is the noted exception's code. `code`, `address` (the instruction) and `accessed` (an access violation's address) are missing when no exception was noted. |
| `manifold.crash.watching()` | boolean | Whether the agent watches the open process for crashes. |
| `manifold.crash.start()` | true, or nil, error | Starts the crash watch in the open process now, without waiting for a script to write code. Loads the agent library into the game. `ME-1021` when it cannot. |
| `manifold.crash.scripts()` | list of `{ script, recordId, on, ranges, warnings }` | What the scripts put into the open process, as the crash watch matches it: per run that changed the game its label (the record's description, or for a script run from Lua the record whose Lua ran it), record id, whether it is still on, `ranges` of `{ address, size, kind, name }` with `kind` `"alloc"` or `"patch"`, and `warnings` of `{ line, text }`: what the Auto Assembler's stack check found, such as a push without its pop before the jump back. |

## manifold.dotnet

The .NET runtime of a .NET game (.NET Framework or .NET 5 and later, 64-bit and 32-bit), read from outside the game by Manifold's .NET helper through the runtime's own data access library: nothing runs inside the game. `load` reads the classes, as Extensions > .NET does; the other functions need that and return `nil` and an `ME-4104` error before it. A class is named `"Namespace.Class"`, a nested one `"Namespace.Outer+Inner"`, and `"Class"` alone works when it is unique. A field or method of a class is written `"Class:Name"`, which also resolves as an address everywhere. Objects move when the garbage collector runs, so an object address is good for the moment; a path from a static field (`value`, `address`) finds the object again each time. Only classes the runtime has loaded are listed, and only methods it has compiled have an address.

| Function | Returns | |
| --- | --- | --- |
| `manifold.dotnet.load()` | true, or nil, error | Starts the .NET helper and reads the classes of the opened process's .NET runtime, unless that is done already. Takes a moment for a large game. `ME-4101` when the process has no .NET runtime, `ME-4104` when a Mono or IL2CPP runtime is loaded instead, `ME-4103` when the helper fails. |
| `manifold.dotnet.unload()` | true | Ends the .NET helper and forgets the classes. Nothing happens when no .NET runtime is loaded. |
| `manifold.dotnet.isLoaded()` | boolean | Whether a .NET runtime is loaded for the opened process. |
| `manifold.dotnet.status()` | `{ loaded, detected, runtime, module, classes, pointerSize, processId, generation }` | Where the .NET reader stands: `detected` says the opened process has a .NET runtime at all, `loaded` that its classes are read. `runtime` (such as `".NET Framework 4.8.9363.0"`), `module`, `classes`, `pointerSize`, `processId` and `generation` are there once it is loaded. `processId` is the game's process id; `generation` grows each time the classes are read again or forgotten. Together they tell a restarted game, which has the same module and often the same class count, from the one before: a cache of class handles keyed on them is never used for the wrong game. |
| `manifold.dotnet.modules()` | list of `{ handle, name, path, base, size, dynamic }`, or nil, error | The loaded assemblies' modules: the runtime's handle for each, its name without the extension, its file, and where its image is. `dynamic` marks a module made at run time. |
| `manifold.dotnet.classes(filter?)` | list of `{ name, namespace, image, handle, module, token, generic }`, or nil, error | The loaded classes, or those whose full name contains `filter` (ignoring case). `handle` is the class's method table, the pointer every object of it starts with; `module` and `token` say where it is defined. |
| `manifold.dotnet.class(name)` | `{ name, namespace, image, parent, handle, module, token, generic, fields, statics, methods }` or nil, or nil, error | A class with its members. `fields` lists the instance fields declared by the class (not its parents') as `{ name, type, typeCode, offset, valueSize, typeHandle }`, the offset counted from the object's start; `statics` lists the static fields as `{ name, type, typeCode, address }`, without `address` while the runtime has made no storage for it; `methods` lists `{ name, address, handle, token, params, static }`, `address` only for a method the runtime has compiled. nil when no class has the name. |
| `manifold.dotnet.classAt(address)` | `{ class, address, offset }` or nil, or nil, error | The object an address is in: its class, where it starts and the address's offset in it. An address within a few KB after an object's start finds it, the way Structure Dissect does. nil when no object is there. |
| `manifold.dotnet.object(address)` | `{ address, class, handle, kind, size, length, elementClass, elementType, elementSize, data, rank }` or nil, or nil, error | What the runtime says about the object that starts at the address. `kind` is `object`, `string`, `array` or `free` (unused heap space); `length` is there for a string (its characters) and an array (its elements), and an array also has its element class, element type code, element size, `data` (the first element) and `rank`. nil when no object starts there. |
| `manifold.dotnet.fields(address)` | list of `{ name, type, typeCode, offset, valueSize, typeHandle }`, or nil, error | The instance fields of the object at the address, its parents' included, sorted by offset. `ME-4105` when no .NET object is there. |
| `manifold.dotnet.methodAt(address)` | `{ name, start, size }` or nil, or nil, error | The compiled method whose code holds the address, as `"Namespace.Class:Method"`, with its code's start and size. nil for an address in no managed code. |
| `manifold.dotnet.methodAddress(class, method)` | address or nil, or nil, error | The code of a method once the runtime has compiled it, the same address `"Class:Method"` resolves to; with overloads, the first one that has code. nil when no such method has run yet. |
| `manifold.dotnet.staticAddress(class, field)` | address or nil, or nil, error | Where a static field's value is, the same address `"Class:Field"` resolves to. A static field of a reference type holds a reference there, a static struct a reference to a boxed copy. nil when the class has no such field or no storage for it yet. |
| `manifold.dotnet.fieldOffset(class, field)` | integer or nil, or nil, error | The offset of an instance field from the start of an object of the class, inherited fields included. An auto-property's backing field is also found by the property's name. nil when there is no such field. |
| `manifold.dotnet.value(base, path?)` | number, boolean, string, address or nil, or nil, error | Reads a value by following a path. `base` is an object address or a static field as `"Class:Field"`; `path` names fields by name and array elements by index, as in `"Players[0].statLife"` or `".Inventory[2].Name"`. A number, a bool, a char's code and a string's text come back as Lua values, another object as its address, null as nil, an inline struct as its address. A step that does not exist, or a null on the way, gives `ME-4105`. The path is followed again on every call, so it keeps working while the garbage collector moves objects. |
| `manifold.dotnet.address(base, path?)` | address, or nil, error | Where the value a path reaches is stored, for `manifold.memory.read` and `write` or the address list; the path works as for `value`. Without a path it is the base itself (the object, or the static field's storage). Good until the garbage collector moves the object; follow the path again for the next use. |
| `manifold.dotnet.text(address)` | string or nil, or nil, error | The text of the `System.String` object at the address, at most a million characters; nil when no string is there. |
| `manifold.dotnet.heap()` | `{ objects, bytes, segments, seconds, types }`, or nil, error | Walks the whole GC heap and counts the objects per type: `types` lists `{ name, handle, count, bytes }`, most bytes first. The game keeps running and the walk takes a few seconds for a large heap, during which the script waits. |
| `manifold.dotnet.instances(class, limit?)` | list of addresses, or nil, error | The objects of exactly one class on the GC heap (objects of derived classes have another class), at most `limit` (100000 by default, 1000000 at most). The class is given by name or by its handle. Walks the whole heap like `heap`; an object found here may already be dead and not collected yet. |

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
| `manifold.ui.window.create(title, options?)` | window | Opens a window and returns its handle. Options: `icon` (Font Awesome name), `width` and `height` (the first size, in points), `open` (false creates it hidden), `dock` (false keeps it out of the dock space), `opacity` (0.1 to 1, while the window has the focus) and `inactiveOpacity` (while another window has it; default `opacity`): without them the user's window opacity settings apply, and only a floating window can be see-through, a docked one stays opaque; `onClose(window)` when the user closes it (it is hidden, not removed; `show` brings it back), `onUpdate(window)` every `interval` seconds while it is open (default every frame), and `key`: a window created with the same key replaces the earlier one, and it keeps its place, size and dock spot from one run to the next. Size and behaviour: `minWidth`, `minHeight`, `maxWidth`, `maxHeight` (points) limit what the user can drag it to, `resizable = false` fixes the size, `autoSize = true` fits the window to its widgets, `scrollbar = false` hides the scroll bar, `topmost = true` keeps a floating window above other windows (it does not dock). `modal = true` shows the window as a dialog in front of the program: the rest of the program waits until it is closed (its close button, Escape or `hide`), a modal opened from a modal stacks on top of it, and closing one closes the ones opened from it; with `open = false` it waits for `show`. `position` places it the first time: `"center"` (of the main window) or `"mouse"`; `x` and `y` (points from the main window's corner) place it exactly. |

### Window objects

`manifold.ui.window.create` returns a window object: dot calls only, read-only, unknown names raise. A call on a window that is gone (removed, or its script ran again) returns `nil, error` with code `ME-7010`.

| Name | Returns | |
| --- | --- | --- |
| `window.add(kind, options?)` | widget | Adds a widget at the end of the window and returns its handle. `kind` is one of `row`, `column`, `grid`, `section`, `tabs`, `split`, `text`, `heading`, `separator`, `spacer`, `button`, `checkbox`, `input`, `number`, `slider`, `combo`, `radio`, `color`, `table`, `tree`, `progress`, `image`, `canvas`; the options depend on it (see the widget object). |
| `window.clear()` | boolean | Removes every widget of the window. |
| `window.remove()` | boolean | Removes the window and its widgets for good. |
| `window.update(options)` | boolean | Changes the title, icon, opacity (`opacity`, `inactiveOpacity`), size limits and behaviour (`minWidth`, `minHeight`, `maxWidth`, `maxHeight`, `resizable`, `autoSize`, `scrollbar`, `topmost`, `modal`), update interval or handlers. |
| `window.show()` | boolean | Shows the window again after the user or `hide` closed it. |
| `window.hide()` | boolean | Hides the window; `show` brings it back with its widgets. |
| `window.focus()` | boolean | Shows the window and brings it to the front. |
| `window.isOpen()` | boolean | Whether the window exists and is shown. |
| `window.id` | integer | The window's number. |
| `window.kind` | string | Always `window`. |

### Widget objects

`add` returns a widget object, with the same rules as a window object. Every widget takes `visible`, `enabled`, `tooltip`, `width` and `height` (points; 0 or missing means automatic; a negative `width` fills the rest of the line); `minWidth` and `maxWidth` keep fields, buttons and lists within that range, and a text wraps at `maxWidth`. Nothing is drawn outside its place: a text, heading or button wider than its place ends in `...` and shows all of it as a tooltip; a text or heading with `overflow = "wrap"` breaks into lines instead (text only), `"clip"` is cut at the edge, `"visible"` keeps its own width. A window cannot be made narrower than its widgets need (their smallest sizes: a field 80 points, a button its padding and a few characters). Handlers get the widget as their last argument.
- Labels of fields (`input`, `number`, `slider`, `combo`, `radio`, `color`): `labelPosition` is `left` (the default: the labels of one column, section or window share a column and the fields line up after it, and a checkbox among them lines up with the fields), `above` or `right` (after the field). Set on a window, column, section, tab or split it is the default for the fields inside. When none is set and a column is too narrow for the labels beside the fields, the labels go above them instead. A field fills the rest of its line unless it has a `width`, so no label is cut off; a negative `height` makes a table, tree, canvas, scrolling column or tab, multi-line input or section fill the height that is left.
- `row`, `column`: lay out their widgets side by side or one below the other; a row's `align` (`left`, `center`, `right`) moves its widgets to that side of the line. In a row, fields, bars, cards, lists and columns without a `width` share the free width equally (a negative `width` asks for a share too), the others get what their content needs, every widget stays inside its share, a `spacer` without a width takes a share too (so what follows it goes to the right), and `wrap = true` moves a widget that does not fit to the next line instead of narrowing the widgets. `gap` (points) sets the space between the widgets of a row, column, section or tab; a column or tab with `scroll = true` scrolls inside its `height` (200).
- `grid`: places its widgets in `columns` (2) cells per row, row by row. The first column fits its content and the others share the rest, so `text` then a field per row lines labels up; a field without a `label` fills its cell.
- `section`: a card with `title`, `icon`, `badge`; `collapsible` lets the user fold it, `open = false` starts it folded.
- `tabs` holds `tab` widgets (`title`, `icon`); `split` shows its first two widgets side by side (`vertical` stacks them) at `ratio` (0.5).
- `text` (`text`, `color`, `wrap`, `mono`, `bold`, `size` in points, `align` = `left`, `center` or `right` for text that does not wrap), `heading` (`text`, `icon`, `color`, `size`, `align`), `separator` (`text` puts a caption into the line), `spacer`.
- `button`: `text`, `icon` (an icon without text makes an icon button), `style` (`default`, `accent`, `danger`), `onClick(widget)`; `shortcut` (like `Ctrl+S` or `F5`) clicks it while its window has the focus, `confirm` asks that text before the click goes through; `hotkey` (like `F5` or `Ctrl+F2`) clicks it from anywhere, also while the game has the focus, as long as its window is open (not a button with `confirm`).
- `checkbox` (`label`, boolean `value`; `style = "switch"` draws it as a switch; `hotkey` like `F1` or `Ctrl+F2` switches it from anywhere, also while the game has the focus, as long as its window is open and the box is enabled, and shows the key beside it), `input` (`label`, string `value`, `hint`, `multiline`, `password`, `readOnly`, `maxLength` in characters, `onSubmit(text, widget)` on Enter; `focus = true` puts the cursor there when the window opens, also on a `number`), `number` (`label`, `value`, `min`, `max`, `step`, `integer`, `hex`, `format` like `%.2f`; a typed value is reported when Enter is pressed or the field is left, a click on its +/- buttons at once), `slider` (`label`, `value`, `min`, `max`, `step`, `integer`, `format`; the value stays within `min` and `max`, also when typed with Ctrl+click; a `number` or `slider` takes `hotkeyIncrease` and `hotkeyDecrease`, keys that move it by `hotkeyStep` (else its `step`, else 1) from anywhere while its window is open, within `min` and `max`), `combo` (`label`, `items`, `hint` shown while nothing is chosen, `value` = the chosen item's number; with `multi = true` several items can be ticked and `value` is a list of their numbers, like `{ 1, 3 }`), `radio` (the same as `combo`, drawn as one button per item; `horizontal` puts them in one line; a `combo` or `radio` takes `hotkeyNext` and `hotkeyPrevious`, keys that choose the next or previous item, round the end), `color` (`label`, `value` = `#RRGGBB`, `alpha` for `#RRGGBBAA` with an alpha channel): all call `onChange(value, widget)`.
- `table`: `columns` (titles, or `{ title, width, align }`), `rows` (lists of cells), `value` = the selected row's number, `onSelect(row, widget)`, `onActivate(row, widget)` on a double click, and `menu` (a list of entries; `-` is a separator) with `onMenu(choice, text, row, widget)`.
- `tree`: `items` (`{ text, icon, id, open, children }`, or plain strings), `value` = the selected item's `id`, `onSelect(id, widget)`, `onActivate(id, widget)`, `menu` with `onMenu(choice, text, id, widget)`.
- `progress` (`value` 0..1, `text`), `image` (`image` from `manifold.ui.image.load`).
- `canvas`: draws what `draw` gave it on the GPU. `background` is a colour, `zoom = true` lets the user zoom with the wheel and pan by dragging; `onClick(x, y, button, widget)`, `onHover(x, y, widget)`, `onView(x, y, scale, widget)` and `menu` with `onMenu(choice, text, x, y, widget)` get canvas coordinates; `onKey(key, widget)` gets every key pressed (and repeated while held) while the canvas's window has the focus, by name: `LeftArrow`, `W`, `Space`, `Enter`, `F1`… (Escape also closes the window).

| Name | Returns | |
| --- | --- | --- |
| `widget.add(kind, options?)` | widget | Adds a widget to a container (`row`, `column`, `section`, `tab`, `split`; `tabs` take `tab` widgets only). |
| `widget.clear()` | boolean | Removes a container's widgets, or a canvas's drawings. |
| `widget.remove()` | boolean | Removes the widget and everything in it. |
| `widget.update(options)` | boolean | Changes options of the widget: the same keys `add` takes for its kind, handlers included, and nothing else (an unknown key raises and names the known ones). Keys that every widget takes: `visible`, `enabled` (false draws it dimmed and takes no input), `tooltip`, `width`, `height`, `minWidth`, `maxWidth`. By kind: a `text` or `heading` takes `text`; a `button` `text`, `icon`, `style`; a `checkbox` `label`, `value`, `style`; an `input` `label`, `value`, `hint`, `readOnly`; a `number` or `slider` `label`, `value`, `min`, `max`, `format`; a `combo` or `radio` `items` and `value` (new items keep the chosen number while it exists); a `progress` `value` (0..1) and `text`; a `table` `columns`, `rows`, `value`; a `tree` `items`, `value`; a `section` or `tab` `title`, `icon`, `badge`. A new `value` does not call `onChange`. Example: `w.speed.update({ value = 2, enabled = god.get() })`, `w.status.update({ text = "Attached", color = "success" })`. |
| `widget.get()` | value | The widget's value: a checkbox's boolean, an input's text, a colour's `#RRGGBB`, a number, the chosen combo, radio or table row's number (a combo with `multi` returns the list of chosen numbers), the selected tree item's id, a progress fraction, otherwise the text. Returns nil when nothing is selected. |
| `widget.set(value?)` | boolean | Sets the widget's value (see `get`); nil clears a selection. It does not call `onChange`. |
| `widget.draw(drawings, options?)` | boolean | Replaces a canvas's drawings (`options.append` adds them instead). A drawing is a table with `kind`: `line` (`x`, `y`, `x2`, `y2`, `thickness`), `rect` (`x`, `y`, `width`, `height`, `filled`, `rounding`, `thickness`), `circle` (`x`, `y`, `radius`, `filled`, `thickness`), `polygon` (`points` = `{ x1, y1, x2, y2, ... }`, `filled`), `text` (`x`, `y`, `text`, `size`) or `image` (`image`, `x`, `y`, `width`, `height`); all but images take `color` (a theme name or `#RRGGBB`/`#RRGGBBAA`). |
| `widget.getView()` | table | A canvas's view `{ x, y, scale }`: the canvas point at its top left corner and the zoom. |
| `widget.setView(view)` | boolean | Moves or zooms a canvas's view. |
| `widget.getRows()` | table | A table's rows, each a list of its cells' texts, and as a second value the column titles. Example: `local rows, columns = w.list.getRows()`, `rows[2][1]` is the first cell of the second row. |
| `widget.getRow(row?)` | table | One row of a table as a list of its cells' texts, or nil when there is no such row or nothing is selected. `w.list.getRow()` reads the selected row. |
| `widget.addRow(cells, row?)` | boolean | Adds a row to a table, at the end or before `row`. |
| `widget.setRow(row, cells)` | boolean | Replaces a row of a table. |
| `widget.removeRow(row)` | boolean | Removes a row of a table; the rows after it move up. |
| `widget.getItems()` | table | A combo's or radio's items as a list of texts (`get` gives the chosen one's number), or a tree's items as tables with `id`, `text`, `icon`, `open` and `children`. |
| `widget.id` | integer | The widget's number. |
| `widget.kind` | string | The kind it was added as. |

## manifold.ui.form

Windows described by a table, as the Form Editor (Tools > Form Editor) saves them in a `.form.lua` file. The table holds the layout and the names of the handlers; the script keeps the code and passes its handler functions in.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.form.build(form, handlers?, options?)` | window | Builds a window from a form table and returns `window, widgets, modals`: the window object, a read-only table of the widgets that have a `name`, by name (the modals' widgets included; an unknown name raises), and a read-only table of the modal windows by name. The table has `form = 1`, `title`, the window options (`icon`, `width`, `key`, `onClose`, ...) and `children`, a list of widget tables: `kind`, an optional `name`, the options of that kind and their own `children`. Handler options hold a name, like `onClick = "save"`, and are looked up in `handlers` (`handlers.save`); a missing one raises. Without `handlers` the handlers are left out, which shows what the form looks like. `modals` lists modal forms: window tables like the form itself with a `name`, each made as a modal window (`modal = true`) that waits hidden; `modals.<name>.show()` opens it. A button's `opens = "<name>"` opens that modal when clicked and `closes = true` hides the window it is in; both run before the button's `onClick`. Any widget can take `enabledWhen`, which enables it only while another widget of the form says so, without code: `"god"` (that checkbox or switch is on, or that combo or radio has a choice), `"not god"`, or `"mode = Multiplier, Double"` (one of these items of that combo or radio is chosen). It follows the user's changes; a script's `set` does not update it. A checkbox, button, number, slider, input, text, combo or radio can take `record`: a memory record's description or id (see `manifold.record`), which binds the widget to it without code. A checkbox shows and switches whether the record is on, a button switches it, a number or slider shows and writes its value, an input writes when Enter is pressed, a text shows the value as the address list does, a combo or radio writes the chosen item's text (a drop-down entry of the record). The window refreshes them ten times a second while it is open, the script's own handlers still run after the write, and a widget whose record is not in the table is disabled until it is. A description binds only when exactly one record has it: with several, the widget stays disabled and a warning names them, until the descriptions differ or the form names the record by id. The whole form is checked first, so a wrong one raises without making a window. An `image` widget takes a file name as `image`, relative to the calling script's folder; a picture that cannot be read leaves the widget empty. A script file that holds its form as `local form = { form = 1, ... }` (as the Form Editor writes it) gets a live reload like `load`: saving the script rebuilds the window's widgets from the table in the file, with the same handlers; the handlers' own code changes only when the script runs again. `options.reload = false` switches that off, `options.onReload(window, widgets, modals)` runs after each rebuild. |
| `manifold.ui.form.load(path, handlers?, options?)` | window | Reads a `.form.lua` file and builds its window like `build`; returns `window, widgets, modals`: the window object, a read-only table of the widgets that have a `name`, by name (the modals' widgets included; an unknown name raises), and a read-only table of the modal windows by name. A relative path starts at the folder of the calling script (at the program folder for code without a file). Returns nil and an `ME-7014` error when the file cannot be read or does not return a table; a wrong form raises like `build`. Picture files of `image` widgets are relative to the form file's folder. Live reload: while the window is open, saving the file (in the Form Editor or elsewhere) rebuilds its widgets in the same window, with the same handlers; the window, the `widgets` and `modals` tables stay the same objects (a modal keeps its window by name), widgets that keep their name and kind keep their value, and a wrong file only logs a warning. `options.reload = false` switches that off; `options.onReload(window, widgets, modals)` runs after each rebuild, to fill what the script put in (table rows, tree items, drawings). |

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

The theme: the names `manifold.ui` takes for colours and icons, the look shown now, and custom themes. The presets are Standard, Red, Green and Blue; a custom theme is an .ini file in the themes folder that builds on one of them. `apply`, `override` and `restore` change the look for this session only, and it comes back when the script runs again or stops; `save` writes a theme file.

| Function | Returns | |
| --- | --- | --- |
| `manifold.ui.theme.colors()` | table | The theme colour names, which follow the theme and light or dark mode. |
| `manifold.ui.theme.icons(filter?)` | table | The Font Awesome icon names, all of them or those that contain `filter`. |
| `manifold.ui.theme.isIcon(name)` | boolean | Whether `name` is a Font Awesome icon name. |
| `manifold.ui.theme.sizes()` | table | The size names a theme can set, such as `FrameRounding` or `ItemSpacing.x`, as Settings > Appearance lists them. |
| `manifold.ui.theme.current()` | table | The look shown now: `{ name, custom, base, mode, light, scripted }`. `name` is the preset's or the custom theme's name, `custom` whether it is a custom theme, `base` the preset it builds on (`standard`, `red`, `green` or `blue`), `mode` `dark`, `light` or `system`, `light` whether light colours show, `scripted` whether a script changed the look for this session. |
| `manifold.ui.theme.list()` | table | The themes to pick from: the presets first, then the custom themes by name, each `{ name, custom, base }`. |
| `manifold.ui.theme.color(name)` | table, or nil, error | A colour of the look shown now as `{ r, g, b, a }` from 0 to 1. `name` is one of `colors()` or an ImGui colour as `imgui.WindowBg`; an unknown name gives `ThemeInvalid`. |
| `manifold.ui.theme.definition(name)` | table, or nil, error | What a theme changes from its preset: `{ base, dark = { name = colour }, light = { ... }, sizes = { name = number } }`, to copy, change and `save`. A preset changes nothing, so its tables are empty; colours are `{ r, g, b, a }`. An unknown name gives `ThemeNotFound`. |
| `manifold.ui.theme.apply(name, options?)` | true, or nil, error | Shows a preset or a custom theme for this session; the user's choice in the settings stays. `options.mode` is `dark`, `light` or `system` (default: the user's mode). An unknown name gives `ThemeNotFound`. |
| `manifold.ui.theme.override(changes)` | true, or nil, error | Changes single colours or sizes of the look shown, for this session. `colors` applies to dark and light, `dark` and `light` to one mode; a colour is `"#rrggbb"`, `"#rrggbbaa"` or `{ r, g, b, a }` from 0 to 1. `sizes` takes the names of `sizes()`. A bad name or value gives `ThemeInvalid` and changes nothing. |
| `manifold.ui.theme.restore()` | boolean | Undoes this script's `apply` and `override`, so the user's theme shows again; false when the look was not this script's. |
| `manifold.ui.theme.save(name, definition, options?)` | true, or nil, error | Writes a custom theme file in the themes folder, as `definition()` describes one; `base` defaults to `standard`. It does not select the theme. A name that is taken gives `ThemeExists` unless `options.overwrite` replaces your own theme of that name; preset names are always taken. |

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
