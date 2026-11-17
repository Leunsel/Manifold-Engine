# Manifold Lua API

`manifold` is Manifold Engine's own Lua API. It lives beside the Cheat Engine functions in [LUA-API.md](LUA-API.md). Those keep Cheat Engine's names and habits for compatibility; `manifold` follows one fixed schema instead, and every call is checked against it.

This file is generated from the API itself (`Manifold-Engine.Tests.exe --manifold-markdown`), and the tests fail when it is out of date.

## Schema

1. **One namespace.** Everything lives in the global table `manifold`. Its fields are areas (`manifold.log`, `manifold.process`, …), constants (`manifold.apiVersion`, `manifold.version`) and `manifold.has`.
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
| `manifold.apiVersion` | 4 | The API version, an integer. It grows when functions are added; names keep their meaning. |
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

The Lua engine window and its output. With *Show on print* on (the default), every printed or logged line opens the window when it is closed. The checkbox in the window is saved with the settings; a script can change it for the running session only.

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
| `manifold.process.find(name?)` | list of `{ id, name, path, windowTitle, is64Bit }` | Running processes whose executable name contains `name` (any case); all of them without `name`. |
| `manifold.process.mainModule()` | `{ name, base, size, path }`, or nil, error | The process's executable module. |
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
| `manifold.event.subscribe(name, handler, options?)` | id | Calls `handler` on an event. `processOpened` passes the process as `manifold.process.info()` gives it, also when the same process is opened again; `processClosed` passes nothing and comes before the next `processOpened`. `options.key` replaces an earlier registration with the same key, so a module that runs again does not register twice. |
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

## manifold.doc

The documentation behind completion, signature help, hover tooltips and the docs panel. Manifold, Cheat Engine and Lua functions are documented already. A module documents its own functions with `register`, or writes LuaLS annotations (`---@param name type`) above them and calls `registerModule`. A script never replaces Manifold's own documentation.

| Function | Returns | |
| --- | --- | --- |
| `manifold.doc.register(target, spec)` | true, or nil, error | Documents a function, or a name such as `"MyModule.run"`. `spec.doc` is the text, `spec.params` a list of `{ name, type, optional, doc, oneOf }`, `spec.returns` what comes back and `spec.overloads` more `{ params, returns }`. A function without `spec.path` is documented wherever the help reaches it. |
| `manifold.doc.registerModule(path, module, options?)` | number of registered functions, or nil, error | Documents every function of a module table under `path` (such as `"ManifoldUE"`), from the LuaLS annotations above each function, or its parameter names when it has none. `options.doc` describes the module. Call it again after adding functions. |
| `manifold.doc.inherit(target, source)` | true, or nil, error | Gives a wrapper the documentation of what it wraps, so a `function(...)` wrapper keeps the parameters and text of the original in the help. |
| `manifold.doc.unregister(target)` | true, or nil, error | Removes documentation that a script registered. Manifold's, Cheat Engine's and Lua's own documentation stays. |
| `manifold.doc.entry(target)` | `{ path, name, kind, origin, summary, text, signatures, … }`, or nil, error | The documentation of a function or name, as the help shows it. `signatures` is a list of `{ text, params, returns }`; each param is `{ name, type, text, optional, variadic, choices }`. Without documentation the error is `ME-7008`. |
| `manifold.doc.find(query, options?)` | list of `{ path, kind, summary }` | Searches names and texts, best matches first. `options.limit` (default 50) caps the list; `options.kind` keeps one kind, such as `"function"` or `"table"`. |
| `manifold.doc.annotations(fn)` | `{ … }` like `entry`, or nil, error | Reads the LuaLS annotations above a function's definition without registering anything. |
| `manifold.doc.markdown(prefix)` | string, or nil, error | Markdown tables of everything documented under `prefix`, the way Docs/MANIFOLD-API.md is written. |
