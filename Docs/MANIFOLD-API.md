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
| `manifold.apiVersion` | 1 | The API version, an integer. It grows when functions are added; names keep their meaning. |
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
| `channel.debug(message, fields?)` … `channel.error(message, fields?)` | nothing | Like the `manifold.log` functions, tagged with `[name]`. |
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
| `manifold.process.modules()` | list of `{ name, base, size, path }`, or nil, error | All loaded modules. |
| `manifold.process.address(expression)` | integer, or nil, error | Resolves an address expression such as `"game.exe+10"` or `"[[base]+8]+4"`. The error says which part failed. |
| `manifold.process.describe(address)` | string, or nil, error | The address as `module.export+offset`, `module+offset` or hex. |

## manifold.memory

Typed reads and writes in the opened process. Types: `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` (integers; `u64` above 2^63 comes back negative, as Lua integers are signed), `float`, `double`, `pointer` (8 or 4 bytes by the process), `bytes` (a list of byte values) and `string` (UTF-8 in Lua, UTF-16 in memory with `wide`). A write checks that the value fits the type before it touches memory.

| Function | Returns | |
| --- | --- | --- |
| `manifold.memory.read(address, type, options?)` | value, or nil, error | Reads one value. `bytes` needs `options.count`; `string` reads up to `options.length` characters (default 256) and stops at the first zero. |
| `manifold.memory.write(address, type, value, options?)` | true, or nil, error | Writes one value. A `string` ends with a zero unless `options.terminate` is false. |
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
