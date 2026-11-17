local native = ...

local SOURCE = debug.getinfo(1, "S").source
local API_VERSION = 4
local EMPTY = setmetatable({}, { __newindex = function() error("read-only", 2) end })
local RUNTIME_CODE = "ME-7002"

local LEVELS = { "debug", "info", "success", "warning", "error" }
local RANK = {}
for index, name in ipairs(LEVELS) do
  RANK[name] = index
end

local MEMORY_TYPES = { "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "float", "double", "pointer", "bytes", "raw", "string" }
local SIMPLE_READS = { i8 = true, u8 = true, i16 = true, u16 = true, i32 = true, u32 = true, i64 = true, u64 = true, float = true, double = true, pointer = true, string = true }
local INTEGER_RANGES = {
  i8 = { -0x80, 0x7F },
  u8 = { 0, 0xFF },
  i16 = { -0x8000, 0x7FFF },
  u16 = { 0, 0xFFFF },
  i32 = { -0x80000000, 0x7FFFFFFF },
  u32 = { 0, 0xFFFFFFFF },
}
local MAX_BYTES = 0x1000000
local MAX_STRING = 0x100000

local ErrorMeta = { __name = "manifold.error" }

local function errorText(object)
  local text = tostring(rawget(object, "message")) .. " (" .. tostring(rawget(object, "code")) .. ")"
  local where = rawget(object, "where")
  if type(where) == "string" and where ~= "" then
    text = where .. ": " .. text
  end
  return text
end
ErrorMeta.__tostring = errorText

local function isError(value)
  return type(value) == "table" and getmetatable(value) == ErrorMeta
end

local function callerFrame()
  for level = 2, 64 do
    local info = debug.getinfo(level, "Sln")
    if not info then
      return nil
    end
    if info.source ~= SOURCE and info.what ~= "C" then
      return info
    end
  end
  return nil
end

local function chunkName(info)
  local source = info.source or ""
  local first = source:sub(1, 1)
  if first == "@" then
    return source:match("([^\\/]+)$") or source:sub(2)
  end
  if first == "=" then
    return source:sub(2)
  end
  return info.short_src
end

local function whereOf(info)
  if not info then
    return ""
  end
  if info.currentline and info.currentline > 0 then
    return chunkName(info) .. ":" .. info.currentline
  end
  return chunkName(info)
end

local function makeError(code, message, where, traceback)
  return setmetatable({
    code = code,
    message = message,
    where = where or whereOf(callerFrame()),
    traceback = traceback,
  }, ErrorMeta)
end

local function result(value, code, message)
  if value == nil and code ~= nil then
    return nil, makeError(code, message)
  end
  return value
end

local function misuse(text)
  local code, message = native.fail("LuaApiMisuse", text)
  error(makeError(code, message), 0)
end

local function isInteger(value)
  return math.type(value) == "integer" or (math.type(value) == "float" and math.tointeger(value) ~= nil)
end

local CHECKS = {
  any = function() return true end,
  string = function(value) return type(value) == "string" end,
  number = function(value) return type(value) == "number" end,
  integer = isInteger,
  boolean = function(value) return type(value) == "boolean" end,
  table = function(value) return type(value) == "table" end,
  ["function"] = function(value) return type(value) == "function" end,
  address = function(value) return type(value) == "string" or isInteger(value) end,
}

local NOUNS = {
  any = "a value",
  string = "a string",
  number = "a number",
  integer = "an integer",
  boolean = "a boolean",
  table = "a table",
  ["function"] = "a function",
  address = "an address (an integer or an address expression)",
}

local function kindOf(value)
  if value == nil then
    return "nil"
  end
  if isError(value) then
    return "an error object"
  end
  if math.type(value) == "float" then
    return "the number " .. tostring(value)
  end
  return type(value)
end

local function distance(left, right)
  if left == right then
    return 0
  end
  local previous = {}
  for j = 0, #right do
    previous[j] = j
  end
  for i = 1, #left do
    local current = { [0] = i }
    local a = left:byte(i)
    for j = 1, #right do
      local cost = a == right:byte(j) and 0 or 1
      current[j] = math.min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost)
    end
    previous = current
  end
  return previous[#right]
end

local function suggestion(members, key)
  if type(key) ~= "string" then
    return nil
  end
  local wanted = key:lower()
  local best, bestScore
  for name in pairs(members) do
    if type(name) == "string" then
      local candidate = name:lower()
      local score = distance(wanted, candidate)
      if candidate:sub(1, #wanted) == wanted or wanted:sub(1, #candidate) == candidate then
        score = math.min(score, 1)
      end
      if not bestScore or score < bestScore or (score == bestScore and name < best) then
        best, bestScore = name, score
      end
    end
  end
  if best and bestScore <= math.max(2, #key // 3) then
    return best
  end
  return nil
end

local function sortedNames(members)
  local names = {}
  for name in pairs(members) do
    if type(name) == "string" then
      names[#names + 1] = name
    end
  end
  table.sort(names)
  return names
end

local function strict(path)
  local members = setmetatable({}, {
    __index = function(owned, key)
      local text = path .. "." .. tostring(key) .. " does not exist"
      local near = suggestion(owned, key)
      if near then
        text = text .. " (did you mean " .. path .. "." .. near .. "?)"
      else
        text = text .. " (" .. path .. " has " .. table.concat(sortedNames(owned), ", ") .. ")"
      end
      misuse(text)
    end,
  })
  local proxy = setmetatable({}, {
    __index = members,
    __newindex = function(_, key)
      misuse(path .. " is read-only, so " .. path .. "." .. tostring(key) .. " cannot be set")
    end,
    __pairs = function()
      return next, members, nil
    end,
    __tostring = function()
      return path
    end,
    __metatable = false,
  })
  return proxy, members
end

local function prepare(params)
  local prepared = {}
  for index, param in ipairs(params) do
    local types = {}
    for part in param.type:gmatch("[^|]+") do
      types[#types + 1] = part
    end
    local nouns = {}
    for _, kind in ipairs(types) do
      nouns[#nouns + 1] = NOUNS[kind]
    end
    local entry = {
      name = param.name,
      types = types,
      noun = table.concat(nouns, " or "),
      optional = param.optional,
      oneOf = param.oneOf,
      keys = param.keys,
    }
    if param.oneOf then
      entry.oneOfSet = {}
      for _, value in ipairs(param.oneOf) do
        entry.oneOfSet[value] = true
      end
    end
    if param.keys then
      entry.keyList = table.concat(sortedNames(param.keys), ", ")
    end
    prepared[index] = entry
  end
  return prepared
end

local function paramText(params, varargs)
  local names = {}
  for _, param in ipairs(params) do
    names[#names + 1] = param.name .. (param.optional and "?" or "")
  end
  if varargs then
    names[#names + 1] = "..."
  end
  return table.concat(names, ", ")
end

local function checkValue(path, index, param, value)
  local matched = false
  for _, kind in ipairs(param.types) do
    if CHECKS[kind](value) then
      matched = true
      break
    end
  end
  if not matched then
    misuse(("%s: argument %d (%s) must be %s, got %s"):format(path, index, param.name, param.noun, kindOf(value)))
  end
  if param.oneOfSet and not param.oneOfSet[value] then
    local quoted = {}
    for _, choice in ipairs(param.oneOf) do
      quoted[#quoted + 1] = '"' .. choice .. '"'
    end
    misuse(("%s: argument %d (%s) must be one of %s, got %q"):format(path, index, param.name, table.concat(quoted, ", "), tostring(value)))
  end
  if param.keys then
    for key, option in pairs(value) do
      local kind = type(key) == "string" and param.keys[key]
      if not kind then
        misuse(("%s: %s.%s is not an option (known options: %s)"):format(path, param.name, tostring(key), param.keyList))
      end
      if not CHECKS[kind](option) then
        misuse(("%s: %s.%s must be %s, got %s"):format(path, param.name, key, NOUNS[kind], kindOf(option)))
      end
    end
  end
end

local function guard(path, owner, params, varargs, body)
  local count = #params
  return function(...)
    local given = select("#", ...)
    if given > 0 and rawequal((...), owner) then
      misuse(path .. " is called with a dot, not a colon: " .. path .. "(" .. paramText(params, varargs) .. ")")
    end
    if not varargs and given > count then
      misuse(("%s takes %d argument%s, got %d"):format(path, count, count == 1 and "" or "s", given))
    end
    for index = 1, count do
      local param = params[index]
      local value = (select(index, ...))
      if value == nil then
        if not param.optional then
          misuse(("%s: argument %d (%s) is missing"):format(path, index, param.name))
        end
      else
        checkValue(path, index, param, value)
      end
    end
    return body(...)
  end
end

local entries = {}
local areas = {}
local manifold, rootMembers = strict("manifold")

native.docDefine(nil, "manifold", { kind = "table" })

local function area(name, summary)
  local proxy, members = strict("manifold." .. name)
  local section = { path = "manifold." .. name, name = name, proxy = proxy, members = members }
  areas[#areas + 1] = section
  rawset(rootMembers, name, proxy)
  native.docDefine(nil, section.path, { kind = "table", doc = summary })
  return section
end

local function bind(owner, path, members, name, spec, body, docPath)
  local params = prepare(spec.params or {})
  local wrapper = guard(path, owner, params, spec.varargs, body)
  rawset(members, name, wrapper)
  if docPath then
    native.docBind(wrapper, docPath)
  else
    native.docDefine(wrapper, path, spec)
  end
  return wrapper, params
end

local function define(section, name, spec, body)
  local path = section.path .. "." .. name
  bind(section.proxy, path, section.members, name, spec, body)
  entries[path:sub(#"manifold." + 1)] = true
end

local function constant(name, value, doc, shown)
  rawset(rootMembers, name, value)
  native.docDefine(nil, "manifold." .. name, { kind = "constant", value = shown or tostring(value), doc = doc })
  entries[name] = true
end

local function quote(text)
  return '"' .. (text:gsub('[\\"\n\r\t\0]', { ["\\"] = "\\\\", ['"'] = '\\"', ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t", ["\0"] = "\\0" })) .. '"'
end

local function keyText(key, render)
  if type(key) == "string" and key:match("^[%a_][%w_]*$") then
    return key
  end
  return "[" .. render(key) .. "]"
end

local function keyOrder(a, b)
  local ta, tb = type(a), type(b)
  if ta ~= tb then
    return ta < tb
  end
  if ta == "number" or ta == "string" then
    return a < b
  end
  return tostring(a) < tostring(b)
end

local MAX_ITEMS = 50

local function inspect(value, options)
  local maxDepth = options and options.depth or 3
  local hex = options and options.hex or false
  local seen = {}
  local function render(item, depth)
    local kind = type(item)
    if kind == "string" then
      return quote(item)
    end
    if kind == "number" then
      if hex and math.type(item) == "integer" then
        return ("0x%X"):format(item)
      end
      return tostring(item)
    end
    if kind == "nil" or kind == "boolean" then
      return tostring(item)
    end
    if kind == "function" then
      return "<function>"
    end
    if kind ~= "table" then
      return tostring(item)
    end
    local meta = getmetatable(item)
    if isError(item) or meta == false or (type(meta) == "table" and meta.__tostring) then
      return tostring(item)
    end
    if seen[item] then
      return "<cycle>"
    end
    if depth >= maxDepth then
      return "{...}"
    end
    seen[item] = true
    local parts = {}
    local length = rawlen(item)
    local skipped = 0
    for index = 1, length do
      if #parts >= MAX_ITEMS then
        skipped = skipped + 1
      else
        parts[#parts + 1] = render(rawget(item, index), depth + 1)
      end
    end
    local keys = {}
    for key in next, item do
      if not (math.type(key) == "integer" and key >= 1 and key <= length) then
        keys[#keys + 1] = key
      end
    end
    table.sort(keys, keyOrder)
    for _, key in ipairs(keys) do
      if #parts >= MAX_ITEMS then
        skipped = skipped + 1
      else
        parts[#parts + 1] = keyText(key, function(k) return render(k, depth + 1) end) .. " = " .. render(rawget(item, key), depth + 1)
      end
    end
    if skipped > 0 then
      parts[#parts + 1] = "... +" .. skipped
    end
    seen[item] = nil
    return "{" .. table.concat(parts, ", ") .. "}"
  end
  return render(value, 0)
end

local currentLevel = "info"

local function emit(level, channel, message, fields)
  if RANK[level] < RANK[currentLevel] then
    return
  end
  local text, traceback, code, where
  if isError(message) then
    text = tostring(rawget(message, "message"))
    traceback = rawget(message, "traceback")
    code = rawget(message, "code")
    where = rawget(message, "where")
  elseif type(message) == "string" then
    text = message
  else
    text = inspect(message)
  end
  local info = callerFrame()
  native.logRecord(RANK[level] - 1, text, channel or "", fields ~= nil and inspect(fields) or "", type(fields) == "table" and fields or nil,
    type(traceback) == "string" and traceback or "", code ~= nil and tostring(code) or "", type(where) == "string" and where or "",
    info and chunkName(info) or "", info and info.name or "", info and info.currentline or 0)
end

local LEVEL_DOCS = {
  debug = "Writes a debug line: details that only matter while working on a script. Hidden unless the level is `debug`.",
  info = "Writes an info line.",
  success = "Writes a success line, in the accent colour.",
  warning = "Writes a warning line, in the warning colour. It also goes to the program log.",
  error = "Writes an error line, in the error colour. It also goes to the program log. An error object as `message` adds its traceback.",
}

local MESSAGE_PARAMS = {
  { name = "message", type = "any" },
  { name = "fields", type = "table", optional = true },
}

local channels = {}

local function channelObject(name)
  local existing = channels[name]
  if existing then
    return existing
  end
  local prefix = ("channel(%q)"):format(name)
  local proxy, members = strict(prefix)
  for _, level in ipairs(LEVELS) do
    bind(proxy, prefix .. "." .. level, members, level, {
      params = MESSAGE_PARAMS,
    }, function(message, fields)
      emit(level, name, message, fields)
    end, "manifold.log.Channel." .. level)
  end
  bind(proxy, prefix .. ".channel", members, "channel", {
    params = { { name = "name", type = "string" } },
  }, function(sub)
    return channelObject(name .. "/" .. sub)
  end, "manifold.log.Channel.channel")
  rawset(members, "name", name)
  channels[name] = proxy
  return proxy
end

local log = area("log", "Coloured, levelled lines in the Lua engine output. Each line shows the time, the level and the channel; `fields` are appended as `{key = value}`. Warnings and errors also go to the program log (Log window and the log file in `logs\\`), with the script line that wrote them.")

for _, level in ipairs(LEVELS) do
  define(log, level, {
    params = MESSAGE_PARAMS,
    returns = "nothing",
    doc = LEVEL_DOCS[level],
  }, function(message, fields)
    emit(level, nil, message, fields)
  end)
end

define(log, "channel", {
  params = { { name = "name", type = "string" } },
  returns = "channel",
  doc = "A channel: an object with the same five level functions that tags every line with `[name]`. The same name gives the same object.",
}, function(name)
  if name == "" then
    misuse("manifold.log.channel: argument 1 (name) must not be empty")
  end
  return channelObject(name)
end)

define(log, "setLevel", {
  params = { { name = "level", type = "string", oneOf = LEVELS } },
  returns = "nothing",
  doc = "Drops lines below `level` for every script until the program closes. The default is `\"info\"`.",
}, function(level)
  currentLevel = level
end)

define(log, "getLevel", {
  returns = "level name",
  doc = "The current level.",
}, function()
  return currentLevel
end)

define(log, "inspect", {
  params = {
    { name = "value", type = "any", optional = true },
    { name = "options", type = "table", optional = true, keys = { depth = "integer", hex = "boolean" } },
  },
  returns = "string",
  doc = "One line of text for any value, the way log lines show `fields`: sorted keys, cycles marked, 50 items per table. `options.depth` (default 3) limits nesting; `options.hex` writes integers in hex.",
}, function(value, options)
  if options and options.depth and options.depth < 1 then
    misuse("manifold.log.inspect: options.depth must be 1 or more")
  end
  return inspect(value, options)
end)

native.docDefine(nil, "manifold.log.Channel", {
  kind = "class",
  title = "Channel objects",
  doc = "`manifold.log.channel(name)` returns an object that follows the same rules: dot calls only, read-only, unknown names raise.",
})
for _, level in ipairs(LEVELS) do
  native.docDefine(nil, "manifold.log.Channel." .. level, {
    params = MESSAGE_PARAMS,
    returns = "nothing",
    doc = "Like `manifold.log." .. level .. "`, tagged with `[name]`.",
  })
end
native.docDefine(nil, "manifold.log.Channel.channel", {
  params = { { name = "name", type = "string" } },
  returns = "channel",
  doc = "A sub-channel named `parent/name`.",
})
native.docDefine(nil, "manifold.log.Channel.name", { kind = "field", type = "string", doc = "The channel's name." })

local output = area("output", "The Lua engine window and its output. With *Show on print* on (the default), every printed or logged line opens the window when it is closed. The checkbox in the window is saved with the settings; a script can change it for the running session only.")

define(output, "show", {
  returns = "nothing",
  doc = "Opens the Lua engine window.",
}, function()
  native.showOutput()
end)

define(output, "clear", {
  returns = "nothing",
  doc = "Clears the output.",
}, function()
  native.clearOutput()
end)

define(output, "getShowOnPrint", {
  returns = "boolean",
  doc = "Whether printing opens the Lua engine window.",
}, function()
  return native.showOnPrint()
end)

define(output, "setShowOnPrint", {
  params = { { name = "enabled", type = "boolean" } },
  returns = "nothing",
  doc = "Turns *Show on print* on or off until the program closes. The saved setting stays as the user set it.",
}, function(enabled)
  native.setShowOnPrint(enabled)
end)

local process = area("process", "The opened process. Without one, every function that needs it returns `nil` and an `ME-1005` error.")

define(process, "isOpen", {
  returns = "boolean",
  doc = "Whether a process is open and still running.",
}, function()
  return native.processIsOpen()
end)

define(process, "info", {
  returns = "`{ id, name, path, is64Bit }`, or nil, error",
  doc = "The opened process.",
}, function()
  return result(native.processInfo())
end)

define(process, "open", {
  params = { { name = "target", type = "string|integer" } },
  returns = "`{ id, name, path, is64Bit }`, or nil, error",
  doc = "Opens a process by id or by executable name (`\"game.exe\"`, or the start of the name).",
}, function(target)
  return result(native.processOpen(target))
end)

define(process, "find", {
  params = { { name = "name", type = "string", optional = true } },
  returns = "list of `{ id, name, path, windowTitle, is64Bit }`",
  doc = "Running processes whose executable name contains `name` (any case); all of them without `name`.",
}, function(name)
  return native.processFind(name)
end)

define(process, "mainModule", {
  returns = "`{ name, base, size, path }`, or nil, error",
  doc = "The process's executable module.",
}, function()
  return result(native.mainModule())
end)

define(process, "module", {
  params = { { name = "name", type = "string" } },
  returns = "`{ name, base, size, path }`, or nil, error",
  doc = "A loaded module by file name, any case. `ME-1016` when it is not loaded.",
}, function(name)
  return result(native.module(name))
end)

define(process, "moduleAt", {
  params = { { name = "address", type = "address" } },
  returns = "`{ name, base, size, path }`, or nil, error",
  doc = "The module that contains the address. `ME-1017` for addresses outside every module: the heap, stacks and code the game allocated itself.",
}, function(address)
  return result(native.moduleAt(address))
end)

define(process, "modules", {
  returns = "list of `{ name, base, size, path }`, or nil, error",
  doc = "All loaded modules.",
}, function()
  return result(native.modules())
end)

define(process, "address", {
  params = { { name = "expression", type = "address" } },
  returns = "integer, or nil, error",
  doc = "Resolves an address expression such as `\"game.exe+10\"` or `\"[[base]+8]+4\"`. The error says which part failed.",
}, function(expression)
  return result(native.address(expression))
end)

define(process, "describe", {
  params = { { name = "address", type = "address" } },
  returns = "string, or nil, error",
  doc = "The address as `module.export+offset`, `module+offset` or hex.",
}, function(address)
  return result(native.describe(address))
end)

local memory = area("memory", "Typed reads and writes in the opened process. Types: `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` (integers; `u64` above 2^63 comes back negative, as Lua integers are signed), `float`, `double`, `pointer` (8 or 4 bytes by the process), `bytes` (a list of byte values), `raw` (the bytes as one Lua string, made for `string.unpack`) and `string` (UTF-8 in Lua, UTF-16 in memory with `wide`). A write checks that the value fits the type before it touches memory. Reads of the plain types and of `raw` take a fast path, so they suit loops over many objects.")

local function readOptions(kind, options)
  options = options or EMPTY
  if kind == "bytes" or kind == "raw" then
    if options.count == nil then
      misuse(("manifold.memory.read: type %q needs options.count"):format(kind))
    end
    if options.count < 1 or options.count > MAX_BYTES then
      misuse("manifold.memory.read: options.count must be 1 to " .. MAX_BYTES)
    end
  elseif options.count ~= nil then
    misuse("manifold.memory.read: options.count is for types \"bytes\" and \"raw\"")
  end
  if kind == "string" then
    if options.length ~= nil and (options.length < 1 or options.length > MAX_STRING) then
      misuse("manifold.memory.read: options.length must be 1 to " .. MAX_STRING)
    end
  elseif options.length ~= nil or options.wide ~= nil then
    misuse("manifold.memory.read: options.length and options.wide are for type \"string\"")
  end
  return options
end

define(memory, "read", {
  params = {
    { name = "address", type = "address" },
    { name = "type", type = "string", oneOf = MEMORY_TYPES },
    { name = "options", type = "table", optional = true, keys = { count = "integer", length = "integer", wide = "boolean" } },
  },
  returns = "value, or nil, error",
  doc = "Reads one value. `bytes` and `raw` need `options.count`; `string` reads up to `options.length` characters (default 256) and stops at the first zero.",
}, function(address, kind, options)
  options = readOptions(kind, options)
  return result(native.read(address, kind, options.count or 0, options.length or 256, options.wide or false))
end)

do
  local slow = rawget(memory.members, "read")
  local nativeRead = native.read
  local function fast(address, kind, options, extra)
    if extra == nil and math.type(address) == "integer" then
      if options == nil and SIMPLE_READS[kind] then
        local value, code, message = nativeRead(address, kind, 0, 256, false)
        if value == nil and code ~= nil then
          return nil, makeError(code, message)
        end
        return value
      end
      if kind == "raw" and type(options) == "table" and next(options, next(options)) == nil then
        local count = rawget(options, "count")
        if math.type(count) == "integer" and count >= 1 and count <= MAX_BYTES then
          local value, code, message = nativeRead(address, "raw", count, 0, false)
          if value == nil and code ~= nil then
            return nil, makeError(code, message)
          end
          return value
        end
      end
    end
    if extra == nil then
      return slow(address, kind, options)
    end
    return slow(address, kind, options, extra)
  end
  rawset(memory.members, "read", fast)
  native.docBind(fast, "manifold.memory.read")
end

local function checkWrite(kind, value, options)
  options = options or EMPTY
  local range = INTEGER_RANGES[kind]
  if range then
    if not isInteger(value) then
      misuse(("manifold.memory.write: argument 3 (value) must be an integer for %s, got %s"):format(kind, kindOf(value)))
    end
    if value < range[1] or value > range[2] then
      misuse(("manifold.memory.write: %s does not fit %s (%d to %d)"):format(tostring(value), kind, range[1], range[2]))
    end
  elseif kind == "i64" or kind == "u64" or kind == "pointer" then
    if not isInteger(value) then
      misuse(("manifold.memory.write: argument 3 (value) must be an integer for %s, got %s"):format(kind, kindOf(value)))
    end
    if kind == "pointer" and native.is64Bit() == false and (value < 0 or value > 0xFFFFFFFF) then
      misuse("manifold.memory.write: " .. tostring(value) .. " does not fit a pointer of a 32-bit process")
    end
  elseif kind == "float" or kind == "double" then
    if type(value) ~= "number" then
      misuse(("manifold.memory.write: argument 3 (value) must be a number for %s, got %s"):format(kind, kindOf(value)))
    end
  elseif kind == "bytes" then
    if type(value) == "table" then
      local count = rawlen(value)
      if count > MAX_BYTES then
        misuse("manifold.memory.write: more than " .. MAX_BYTES .. " bytes")
      end
      for index = 1, count do
        local byte = rawget(value, index)
        if not isInteger(byte) or byte < 0 or byte > 255 then
          misuse(("manifold.memory.write: byte %d is %s, not 0 to 255"):format(index, kindOf(byte) == "number" and tostring(byte) or kindOf(byte)))
        end
      end
    elseif type(value) ~= "string" then
      misuse("manifold.memory.write: argument 3 (value) must be a list of byte values or a string for bytes, got " .. kindOf(value))
    end
  elseif kind == "raw" then
    if type(value) ~= "string" then
      misuse("manifold.memory.write: argument 3 (value) must be a string for raw, got " .. kindOf(value))
    end
  elseif kind == "string" then
    if type(value) ~= "string" then
      misuse("manifold.memory.write: argument 3 (value) must be a string for string, got " .. kindOf(value))
    end
  end
  if kind ~= "string" and (options.wide ~= nil or options.terminate ~= nil) then
    misuse("manifold.memory.write: options.wide and options.terminate are for type \"string\"")
  end
  return options
end

define(memory, "write", {
  params = {
    { name = "address", type = "address" },
    { name = "type", type = "string", oneOf = MEMORY_TYPES },
    { name = "value", type = "any" },
    { name = "options", type = "table", optional = true, keys = { wide = "boolean", terminate = "boolean" } },
  },
  returns = "true, or nil, error",
  doc = "Writes one value. A `string` ends with a zero unless `options.terminate` is false.",
}, function(address, kind, value, options)
  options = checkWrite(kind, value, options)
  return result(native.writeMemory(address, kind, value, options.wide or false, options.terminate ~= false))
end)

define(memory, "scan", {
  params = {
    { name = "pattern", type = "string" },
    { name = "options", type = "table", optional = true, keys = { module = "string", executable = "boolean", writable = "boolean", alignment = "integer", limit = "integer" } },
  },
  returns = "list of integers, or nil, error",
  doc = "Finds a byte pattern such as `\"48 8B 05 ?? ?? ?? ??\"` (`??` is any byte) in the whole process, or only in `options.module`. `options.executable` and `options.writable` set to true keep only such memory, set to false only memory without it, and left out take both. `options.alignment` keeps addresses divisible by it; `options.limit` (1 to 1000000, default 100000) stops after that many hits. The list is in address order, so with a limit it holds the lowest hits. No hit is an empty list, not an error.",
}, function(pattern, options)
  options = options or EMPTY
  if options.alignment ~= nil and options.alignment < 1 then
    misuse("manifold.memory.scan: options.alignment must be 1 or more")
  end
  if options.limit ~= nil and (options.limit < 1 or options.limit > 1000000) then
    misuse("manifold.memory.scan: options.limit must be 1 to 1000000")
  end
  return result(native.scan(pattern, options.module, options.executable, options.writable, options.alignment or 0, options.limit or 100000))
end)

define(memory, "isReadable", {
  params = { { name = "address", type = "address" } },
  returns = "boolean",
  doc = "Whether the page at the address can be read. False without a process or for an address that does not resolve.",
}, function(address)
  return native.isReadable(address)
end)

local errors = area("error", "Error objects and protected calls. An error object is `{ code, message, where, traceback }`; `tostring` gives `where: message (code)`.")

local function fromRaised(problem, traceback)
  if isError(problem) then
    if rawget(problem, "traceback") == nil then
      problem.traceback = traceback
    end
    return problem
  end
  local text = type(problem) == "string" and problem or inspect(problem)
  local where, message = text:match("^([^\n]-:%d+): (.*)$")
  if where then
    where = where:match("([^\\/]+)$") or where
  end
  return makeError(RUNTIME_CODE, message or text, where or "", traceback)
end

local function tracebackOf(level)
  local kept = {}
  for line in debug.traceback("", level):gmatch("[^\n]+") do
    local frame = line:gsub("^%s+", "")
    if not (frame:find("^manifold:%d+:") or frame == "[C]: in function 'xpcall'" or frame == "(...tail calls...)") then
      kept[#kept + 1] = line
    end
  end
  return table.concat(kept, "\n")
end

define(errors, "create", {
  params = {
    { name = "message", type = "string" },
    { name = "options", type = "table", optional = true, keys = { code = "string" } },
  },
  returns = "error object",
  doc = "A new error object for the calling line. `options.code` sets the script's own code; the default is `ME-7002`.",
}, function(message, options)
  return makeError(options and options.code or RUNTIME_CODE, message)
end)

define(errors, "is", {
  params = { { name = "value", type = "any", optional = true } },
  returns = "boolean",
  doc = "Whether `value` is an error object.",
}, function(value)
  return isError(value)
end)

define(errors, "raise", {
  params = {
    { name = "problem", type = "string|table" },
    { name = "options", type = "table", optional = true, keys = { code = "string" } },
  },
  returns = "never returns",
  doc = "Raises an error object: `problem` itself when it is one, else a new one from the text.",
}, function(problem, options)
  if isError(problem) then
    error(problem, 0)
  end
  if type(problem) ~= "string" then
    misuse("manifold.error.raise: argument 1 (problem) must be a string or an error object, got table")
  end
  error(makeError(options and options.code or RUNTIME_CODE, problem), 0)
end)

define(errors, "try", {
  params = { { name = "fn", type = "function" } },
  varargs = true,
  returns = "true, results..., or false, error",
  doc = "Calls `fn(...)`. On an error it returns false and an error object with the line that raised it and the traceback, instead of stopping the script.",
}, function(fn, ...)
  return xpcall(fn, function(problem)
    return fromRaised(problem, tracebackOf(3))
  end, ...)
end)

define(errors, "check", {
  params = {
    { name = "value", type = "any", optional = true },
    { name = "problem", type = "any", optional = true },
  },
  varargs = true,
  returns = "its arguments",
  doc = "Passes its arguments through when `value` is neither nil nor false, else raises `problem`. Made for `nil, error` results: `local health = manifold.error.check(manifold.memory.read(address, \"i32\"))`.",
}, function(value, problem, ...)
  if value then
    return value, problem, ...
  end
  if isError(problem) then
    error(problem, 0)
  end
  if type(problem) == "string" then
    error(makeError(RUNTIME_CODE, problem), 0)
  end
  error(makeError(RUNTIME_CODE, "manifold.error.check got " .. tostring(value)), 0)
end)

local function raisedHandler(problem)
  return fromRaised(problem, tracebackOf(3))
end

local function protect(kind, fn)
  local reported = false
  local function finish(ok, first, ...)
    if ok then
      return first, ...
    end
    if not reported then
      reported = true
      emit("error", "manifold." .. kind, first)
    end
    return nil
  end
  return function(...)
    return finish(xpcall(fn, raisedHandler, ...))
  end
end

local function keyedRegistry(add, remove)
  local byKey = {}
  local keyOf = {}
  local function drop(id)
    local removed = remove(id) and true or false
    local key = keyOf[id]
    if key ~= nil then
      byKey[key] = nil
      keyOf[id] = nil
    end
    return removed
  end
  local function register(fn, options)
    local key = options and options.key
    if key ~= nil and byKey[key] ~= nil then
      drop(byKey[key])
    end
    local id = add(fn)
    if key ~= nil then
      byKey[key] = id
      keyOf[id] = key
    end
    return id
  end
  return register, drop
end

local KEY_OPTION = { name = "options", type = "table", optional = true, keys = { key = "string" } }
local KEY_TEXT = " `options.key` replaces an earlier registration with the same key, so a module that runs again does not register twice."

local ceSymbolLookup = rawget(_G, "registerSymbolLookupCallback")
local ceSymbolUnlookup = rawget(_G, "unregisterSymbolLookupCallback")
local ceNameLookup = rawget(_G, "registerStructureNameLookup")
local ceNameUnlookup = rawget(_G, "unregisterStructureNameLookup")
local ceDissector = rawget(_G, "registerStructureDissectOverride")
local ceUndissector = rawget(_G, "unregisterStructureDissectOverride")

local symbol = area("symbol", "Symbols that address expressions, the disassembler and Auto Assembler scripts know. A registered symbol belongs to the opened process: closing it or opening another one removes it, so a module registers its symbols again from a `processOpened` handler or when it resolves.")

define(symbol, "register", {
  params = {
    { name = "name", type = "string" },
    { name = "address", type = "address" },
  },
  returns = "true, or nil, error",
  doc = "Registers `name` for `address`, replacing an earlier symbol of that name.",
}, function(name, address)
  if name == "" then
    misuse("manifold.symbol.register: argument 1 (name) must not be empty")
  end
  return result(native.registerSymbol(name, address))
end)

define(symbol, "unregister", {
  params = { { name = "name", type = "string" } },
  returns = "true, or nil, error",
  doc = "Removes a registered symbol. `ME-8002` when there is none of that name.",
}, function(name)
  return result(native.unregisterSymbol(name))
end)

local addSymbolResolver, removeSymbolResolver = keyedRegistry(function(resolver)
  local guarded = protect("symbol", resolver)
  return ceSymbolLookup(function(name)
    local value = guarded(name)
    if math.type(value) == "integer" then
      return value
    end
    return nil
  end, 5)
end, ceSymbolUnlookup)

define(symbol, "addResolver", {
  params = { { name = "resolver", type = "function" }, KEY_OPTION },
  returns = "id",
  doc = "Calls `resolver(name)` for a name that is neither a module, an export nor a registered symbol; it returns an address or nil. Resolvers are asked in the order they were added, and the first address wins. An error in a resolver is logged once." .. KEY_TEXT,
}, function(resolver, options)
  return addSymbolResolver(resolver, options)
end)

define(symbol, "removeResolver", {
  params = { { name = "id", type = "integer" } },
  returns = "boolean",
  doc = "Removes a resolver; false when the id is unknown.",
}, function(id)
  return removeSymbolResolver(id)
end)

local EVENTS = { "processOpened", "processClosed" }
local subscribers = { processOpened = {}, processClosed = {} }
local subscriberNames = {}
local nextSubscriber = 1

local subscribe, unsubscribe = keyedRegistry(function(entry)
  local id = nextSubscriber
  nextSubscriber = nextSubscriber + 1
  entry.id = id
  local list = subscribers[entry.name]
  list[#list + 1] = entry
  subscriberNames[id] = entry.name
  return id
end, function(id)
  local name = subscriberNames[id]
  if not name then
    return false
  end
  subscriberNames[id] = nil
  local list = subscribers[name]
  for index, entry in ipairs(list) do
    if entry.id == id then
      table.remove(list, index)
      return true
    end
  end
  return false
end)

local event = area("event", "Notifications from the program. Handlers run in the order they subscribed; an error in one is logged with its traceback and does not stop the others. Handlers run while the program waits, so they should only reset state: long work belongs in a thread or happens on first use.")

define(event, "subscribe", {
  params = {
    { name = "name", type = "string", oneOf = EVENTS },
    { name = "handler", type = "function" },
    KEY_OPTION,
  },
  returns = "id",
  doc = "Calls `handler` on an event. `processOpened` passes the process as `manifold.process.info()` gives it, also when the same process is opened again; `processClosed` passes nothing and comes before the next `processOpened`." .. KEY_TEXT,
}, function(name, handler, options)
  return subscribe({ name = name, handler = handler }, options)
end)

define(event, "unsubscribe", {
  params = { { name = "id", type = "integer" } },
  returns = "boolean",
  doc = "Removes a handler; false when the id is unknown.",
}, function(id)
  return unsubscribe(id)
end)

debug.getregistry()["Manifold.Dispatch"] = function(name)
  local list = subscribers[name]
  if not list or #list == 0 then
    return
  end
  local argument = nil
  if name == "processOpened" then
    argument = native.processInfo()
  end
  local snapshot = table.move(list, 1, #list, 1, {})
  for _, entry in ipairs(snapshot) do
    local ok, problem = xpcall(entry.handler, raisedHandler, argument)
    if not ok then
      emit("error", "manifold.event", problem)
    end
  end
end

local structure = area("structure", "Hooks into Structure Dissect. Hooks are asked in the order they were added, and the first answer wins. An error in a hook is logged once and counts as no answer.")

local addNameResolver, removeNameResolver = keyedRegistry(function(resolver)
  return ceNameLookup(protect("structure", resolver))
end, ceNameUnlookup)

local addDissector, removeDissector = keyedRegistry(function(dissector)
  local guarded = protect("structure", dissector)
  return ceDissector(function(target, address)
    return guarded(target, address) == true
  end)
end, ceUndissector)

define(structure, "addNameResolver", {
  params = { { name = "resolver", type = "function" }, KEY_OPTION },
  returns = "id",
  doc = "Calls `resolver(address)` when Structure Dissect needs a name for a new structure or for a pointer it expands. It returns a name and, optionally, the address where the object starts, or nil." .. KEY_TEXT,
}, function(resolver, options)
  return addNameResolver(resolver, options)
end)

define(structure, "removeNameResolver", {
  params = { { name = "id", type = "integer" } },
  returns = "boolean",
  doc = "Removes a name resolver; false when the id is unknown.",
}, function(id)
  return removeNameResolver(id)
end)

define(structure, "addDissector", {
  params = { { name = "dissector", type = "function" }, KEY_OPTION },
  returns = "id",
  doc = "Calls `dissector(structure, address)` before Structure Dissect guesses fields. It fills the structure (the Cheat Engine structure object: `addElement`, `autoGuess`, …) and returns true, or returns false to leave it to the next dissector and the guess." .. KEY_TEXT,
}, function(dissector, options)
  return addDissector(dissector, options)
end)

define(structure, "removeDissector", {
  params = { { name = "id", type = "integer" } },
  returns = "boolean",
  doc = "Removes a dissector; false when the id is unknown.",
}, function(id)
  return removeDissector(id)
end)

define(structure, "resolveName", {
  params = { { name = "address", type = "address" } },
  returns = "name and start, nil when no resolver answers, or nil, error",
  doc = "Asks the name resolvers in order, as Structure Dissect does, and returns the first answer: the name and the address where the object starts (the given address when the resolver names none). Resolvers that Cheat Engine scripts registered are asked too; the program's own Mono lookup is not.",
}, function(address)
  local at, problem = result(native.address(address))
  if at == nil then
    return nil, problem
  end
  for _, hook in ipairs(native.structureHooks(0)) do
    local ok, name, start = pcall(hook, at)
    if ok and type(name) == "string" and name ~= "" then
      return name, math.type(start) == "integer" and start or at
    end
  end
  return nil
end)

define(structure, "dissect", {
  params = {
    { name = "structure", type = "any" },
    { name = "address", type = "address" },
  },
  returns = "boolean, or nil, error",
  doc = "Asks the dissectors in order, as Structure Dissect does before it guesses, to fill `structure` (a Cheat Engine structure object, e.g. from `createStructure`) for the object at `address`. True when one of them filled it.",
}, function(target, address)
  local at, problem = result(native.address(address))
  if at == nil then
    return nil, problem
  end
  for _, hook in ipairs(native.structureHooks(1)) do
    local ok, handled = pcall(hook, target, at)
    if ok and handled == true then
      return true
    end
  end
  return false
end)

local MISUSE_CODE = native.fail("LuaApiMisuse")

local function docResult(value, code, message)
  if value == nil and code == MISUSE_CODE then
    misuse(message)
  end
  return result(value, code, message)
end

local DOC_SPEC = {
  path = "string",
  kind = "string",
  summary = "string",
  doc = "string",
  title = "string",
  type = "string",
  value = "any",
  params = "table",
  varargs = "boolean",
  returns = "string",
  returnsDoc = "string",
  overloads = "table",
  deprecated = "any",
  hidden = "boolean",
  see = "table",
}

local doc = area("doc", "The documentation behind completion, signature help, hover tooltips and the docs panel. Manifold, Cheat Engine and Lua functions are documented already. A module documents its own functions with `register`, or writes LuaLS annotations (`---@param name type`) above them and calls `registerModule`. A script never replaces Manifold's own documentation.")

define(doc, "register", {
  params = {
    { name = "target", type = "function|string" },
    { name = "spec", type = "table", keys = DOC_SPEC },
  },
  returns = "true, or nil, error",
  doc = "Documents a function, or a name such as `\"MyModule.run\"`. `spec.doc` is the text, `spec.params` a list of `{ name, type, optional, doc, oneOf }`, `spec.returns` what comes back and `spec.overloads` more `{ params, returns }`. A function without `spec.path` is documented wherever the help reaches it.",
}, function(target, spec)
  return docResult(native.docRegister(target, spec))
end)

define(doc, "registerModule", {
  params = {
    { name = "path", type = "string" },
    { name = "module", type = "table" },
    { name = "options", type = "table", optional = true, keys = { doc = "string" } },
  },
  returns = "number of registered functions, or nil, error",
  doc = "Documents every function of a module table under `path` (such as `\"ManifoldUE\"`), from the LuaLS annotations above each function, or its parameter names when it has none. `options.doc` describes the module. Call it again after adding functions.",
}, function(path, module, options)
  return docResult(native.docRegisterModule(path, module, options))
end)

define(doc, "inherit", {
  params = {
    { name = "target", type = "function" },
    { name = "source", type = "function|string" },
  },
  returns = "true, or nil, error",
  doc = "Gives a wrapper the documentation of what it wraps, so a `function(...)` wrapper keeps the parameters and text of the original in the help.",
}, function(target, source)
  return docResult(native.docInherit(target, source))
end)

define(doc, "unregister", {
  params = { { name = "target", type = "function|string" } },
  returns = "true, or nil, error",
  doc = "Removes documentation that a script registered. Manifold's, Cheat Engine's and Lua's own documentation stays.",
}, function(target)
  return docResult(native.docUnregister(target))
end)

define(doc, "entry", {
  params = { { name = "target", type = "function|string" } },
  returns = "`{ path, name, kind, origin, summary, text, signatures, … }`, or nil, error",
  doc = "The documentation of a function or name, as the help shows it. `signatures` is a list of `{ text, params, returns }`; each param is `{ name, type, text, optional, variadic, choices }`. Without documentation the error is `ME-7008`.",
}, function(target)
  return docResult(native.docEntry(target))
end)

define(doc, "find", {
  params = {
    { name = "query", type = "string" },
    { name = "options", type = "table", optional = true, keys = { limit = "integer", kind = "string" } },
  },
  returns = "list of `{ path, kind, summary }`",
  doc = "Searches names and texts, best matches first. `options.limit` (default 50) caps the list; `options.kind` keeps one kind, such as `\"function\"` or `\"table\"`.",
}, function(query, options)
  options = options or EMPTY
  if options.limit ~= nil and options.limit < 0 then
    misuse("manifold.doc.find: options.limit must be 0 or more")
  end
  return docResult(native.docFind(query, options.limit or 50, options.kind or ""))
end)

define(doc, "annotations", {
  params = { { name = "fn", type = "function" } },
  returns = "`{ … }` like `entry`, or nil, error",
  doc = "Reads the LuaLS annotations above a function's definition without registering anything.",
}, function(fn)
  return docResult(native.docAnnotations(fn))
end)

define(doc, "markdown", {
  params = { { name = "prefix", type = "string" } },
  returns = "string, or nil, error",
  doc = "Markdown tables of everything documented under `prefix`, the way Docs/MANIFOLD-API.md is written.",
}, function(prefix)
  return docResult(native.docMarkdown(prefix))
end)

constant("apiVersion", API_VERSION, "The API version, an integer. It grows when functions are added; names keep their meaning.")
constant("version", native.version(), "The program version, such as `\"0.1.0\"`.", "string")

do
  local path = "manifold.has"
  local spec = {
    params = { { name = "name", type = "string" } },
    returns = "boolean",
    doc = "Whether the API has a name, such as `\"log.channel\"`, `\"memory\"` or `\"apiVersion\"`. Use it instead of reading a name that may not exist, which raises.",
  }
  bind(manifold, path, rootMembers, "has", spec, function(name)
    local short = name:gsub("^manifold%.", "")
    if entries[short] or short == "has" then
      return true
    end
    for _, section in ipairs(areas) do
      if section.name == short then
        return true
      end
    end
    return false
  end)
end

rawset(_G, "manifold", manifold)

local INTRO = [[
# Manifold Lua API

`manifold` is Manifold Engine's own Lua API. It lives beside the Cheat Engine functions in [LUA-API.md](LUA-API.md). Those keep Cheat Engine's names and habits for compatibility; `manifold` follows one fixed schema instead, and every call is checked against it.

This file is generated from the API itself (`Manifold-Engine.Tests.exe --manifold-markdown`), and the tests fail when it is out of date.
]]

local RULES = [[
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
]]

local function markdown()
  return table.concat({ INTRO, RULES, native.docMarkdown("manifold") }, "\n")
end

return markdown
