local native = ...

local SOURCE = debug.getinfo(1, "S").source
local API_VERSION = 6
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

local EVENTS = { "processOpened", "processClosed", "unrealReady" }
local subscribers = { processOpened = {}, processClosed = {}, unrealReady = {} }
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
  doc = "Calls `handler` on an event. `processOpened` passes the process as `manifold.process.info()` gives it, also when the same process is opened again; `processClosed` passes nothing and comes before the next `processOpened`; `unrealReady` passes nothing and follows when the Unreal tool has resolved the game." .. KEY_TEXT,
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

local unreal = area("unreal", "Unreal Engine 4 and 5 reflection, built into the program: it finds the game's name table, object array and reflection layout itself, without an SDK or game offsets, and only reads. Structure Dissect then knows Unreal objects (real property names, nested structs, bitfield bools, the native gaps), the custom types `UE FName`, `UE FString` and `UE Object Index` show names, strings and weak references, and `<Type>_<Property>` symbols such as `Actor_RootComponent` resolve to offsets in every session. It starts by itself when an Unreal game is opened, unless `configure{ autoStart = false }` says otherwise, and the window is in Extensions > Unreal Engine. The queries need it resolved and return `nil` and an `ME-4201` error before that. Types are given by name (`\"Actor\"`, without the U, A or F prefix) or by address.")

local function many(first, ...)
  if first == nil and select("#", ...) > 0 and (...) ~= nil then
    return result(first, ...)
  end
  return first, ...
end

local PATTERN_TEXT = "A pattern is an AOB string, or `{ pattern, offset, length, relative, add, module }`: the hit plus `offset` holds a 32-bit displacement relative to the end of the instruction (`length` bytes long, `offset + 4` by default), or with `relative = false` an absolute pointer; `add` is added to the result, and `module` (the executable by default) is where the pattern is searched."

define(unreal, "status", {
  returns = "`{ state, text, failure, objects, types, namePool, objectArray, objectArraySource, legacyNames, doubleVectors, seconds }`",
  doc = "Where the Unreal tool stands. `state` is `idle`, `notUnreal` (the opened game is no Unreal game), `resolving`, `ready`, `failed` (with `failure`) or `stopped`; `text` is the line the menu shows. The counts and addresses are there once it is ready.",
}, function()
  return native.unrealStatus()
end)

define(unreal, "start", {
  params = { { name = "options", type = "table", optional = true, keys = { wait = "boolean" } } },
  returns = "true, or nil, error",
  doc = "Resolves the opened process again, in any process, not only one that looks like Unreal. It runs in the background and the `unrealReady` event follows; with `options.wait` it resolves before it returns and gives the error when it fails. After `stop` this is what turns the tool on again.",
}, function(options)
  return result(native.unrealStart((options or EMPTY).wait == true))
end)

define(unreal, "stop", {
  returns = "true",
  doc = "Shuts the Unreal tool down: it drops what was resolved, its symbols and dissects stop, and nothing starts by itself again until `start`, also in the next game.",
}, function()
  return native.unrealStop()
end)

define(unreal, "configure", {
  params = {
    { name = "options", type = "table", keys = {
      autoStart = "boolean", dissect = "boolean", names = "boolean", symbols = "boolean", types = "boolean",
      namePool = "any", objectArray = "any", legacyNames = "boolean", layout = "any", patterns = "any",
    } },
  },
  returns = "true, or nil, error",
  doc = "Changes how the tool works; keys that are left out keep their value, and the settings last until the program closes, so an autorun script sets them once. `autoStart` (true): resolve when an Unreal game is opened. `dissect`, `names`, `symbols`, `types` (all true): Structure Dissect fields, structure names, the `<Type>_<Property>` and `GObjects` symbols, the custom types. For a game the search does not find: `namePool` (the FNamePool, or with `legacyNames = true` the TNameEntryArray of UE 4.22 and older) and `objectArray` (FUObjectArray or its ObjObjects) as addresses or expressions, `layout` as a table of offsets that replace what the probes find (`index`, `class`, `name`, `outer`, `super`, `childProperties`, `structSize`, `children`, `childNext`, `functionFlags`, `fField`, `fieldClass`, `fieldName`, `next`, `offset`, `size`, `extra`, `itemSize`, `itemOffset`), and `patterns = { names = {...}, objects = {...} }` searched before the built-in search. " .. PATTERN_TEXT .. " `false` clears an address, the layout or the patterns. A change to the addresses, layout or patterns applies at the next `start`.",
}, function(options)
  return result(native.unrealConfigure(options))
end)

define(unreal, "config", {
  returns = "`{ autoStart, dissect, names, symbols, types, namePool, objectArray, legacyNames, layout, patterns }`",
  doc = "The settings `configure` changes, as it would take them.",
}, function()
  return native.unrealConfig()
end)

define(unreal, "layout", {
  returns = "table of offsets, or nil, error",
  doc = "The layout the tool found: the object array items (`itemSize`, `itemOffset`, `chunked`), UObject (`index`, `class`, `name`, `outer`), UStruct (`super`, `childProperties`, `structSize`, `children`, `childNext`, `functionFlags`) and the properties (`fField`, `fieldClass`, `fieldName`, `next`, `offset`, `size`, `extra`, `component` 4 or 8 for float or double vectors). The keys are the ones `configure{ layout = ... }` takes.",
}, function()
  return result(native.unrealLayout())
end)

define(unreal, "count", {
  returns = "integer, or nil, error",
  doc = "How many slots the object array has in use.",
}, function()
  return result(native.unrealCount())
end)

define(unreal, "objectAt", {
  params = { { name = "index", type = "integer" } },
  returns = "address or nil, or nil, error",
  doc = "The object in a slot of the object array; nil for an empty slot.",
}, function(index)
  return many(native.unrealObjectAt(index))
end)

define(unreal, "isObject", {
  params = { { name = "address", type = "address" } },
  returns = "boolean",
  doc = "Whether a live UObject starts at the address: its index points back at it in the object array. False before the tool is ready.",
}, function(address)
  return native.unrealIsObject(address) == true
end)

define(unreal, "name", {
  params = { { name = "address", type = "address" } },
  returns = "string or nil, or nil, error",
  doc = "The FName stored at an address, with its number as `Name_1`.",
}, function(address)
  return many(native.unrealName(address))
end)

define(unreal, "nameOf", {
  params = { { name = "object", type = "address" } },
  returns = "string or nil, or nil, error",
  doc = "An object's name.",
}, function(object)
  return many(native.unrealNameOf(object))
end)

define(unreal, "fullName", {
  params = { { name = "object", type = "address" } },
  returns = "string, or nil, error",
  doc = "An object's path through its outers, such as `/Game/Maps/Level.Level.PersistentLevel.Pawn_1`.",
}, function(object)
  return result(native.unrealFullName(object))
end)

define(unreal, "classOf", {
  params = { { name = "object", type = "address" } },
  returns = "address or nil, or nil, error",
  doc = "An object's class.",
}, function(object)
  return many(native.unrealClassOf(object))
end)

define(unreal, "type", {
  params = { { name = "name", type = "string" } },
  returns = "address or nil, or nil, error",
  doc = "A class or struct by name, any case, without the U, A or F prefix.",
}, function(name)
  return many(native.unrealType(name))
end)

define(unreal, "types", {
  params = { { name = "filter", type = "string", optional = true } },
  returns = "list of `{ name, kind, address }`, or nil, error",
  doc = "Every class, struct and enum, sorted by name; with `filter`, the ones whose name contains it. `kind` is `class`, `struct` or `enum`.",
}, function(filter)
  return result(native.unrealTypes(filter))
end)

define(unreal, "super", {
  params = { { name = "type", type = "string|integer" } },
  returns = "address or nil, or nil, error",
  doc = "The parent of a class or struct.",
}, function(type)
  return many(native.unrealSuper(type))
end)

define(unreal, "size", {
  params = { { name = "type", type = "string|integer" } },
  returns = "integer or nil, or nil, error",
  doc = "The size of a class or struct in bytes.",
}, function(type)
  return many(native.unrealSize(type))
end)

define(unreal, "cppName", {
  params = { { name = "type", type = "string|integer" } },
  returns = "string, or nil, error",
  doc = "A type's C++ spelling: `F` for structs, `A` for actors, `U` for other classes.",
}, function(type)
  return result(native.unrealCppName(type))
end)

local FIELD_TEXT = "A field is `{ name, type, typeText, owner, offset, start, size, dim, flags, address, inner, metaClass, byteOffset, bitMask }`: `type` is the property class (`FloatProperty`), `typeText` the C++ type (`TArray<AActor*>`), `start` the offset of the first byte (a bitfield bool adds `byteOffset` and has `bitMask`), `inner` the struct, class or enum it refers to."

define(unreal, "fields", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "ownOnly", type = "boolean", optional = true },
  },
  returns = "list of fields, or nil, error",
  doc = "The reflected properties of a class or struct, its parents' included unless `ownOnly`, sorted by offset. " .. FIELD_TEXT,
}, function(type, ownOnly)
  return result(native.unrealFields(type, ownOnly == true))
end)

define(unreal, "field", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "name", type = "string" },
  },
  returns = "field, or nil, error",
  doc = "One property by name, any case, searched in the parents too; `ME-4203` when there is none. " .. FIELD_TEXT,
}, function(type, name)
  return result(native.unrealField(type, name))
end)

define(unreal, "offset", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "name", type = "string" },
  },
  returns = "symbol, offset, or nil, error",
  doc = "The offset symbol of a property (`Actor_RootComponent`) and its value. The symbol works in address expressions in every session, so records written with it keep working after a game update.",
}, function(type, name)
  return many(native.unrealSymbol(type, name))
end)

define(unreal, "functions", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "inherited", type = "boolean", optional = true },
  },
  returns = "list of `{ name, owner, address, flags, parameters }`, or nil, error",
  doc = "The UFunctions a class declares, and with `inherited` its parents' too. `parameters` are fields; the return value is the one with the return flag (`0x400` in `flags`).",
}, function(type, inherited)
  return result(native.unrealFunctions(type, inherited == true))
end)

define(unreal, "enum", {
  params = { { name = "enum", type = "string|integer" } },
  returns = "list of `{ name, value }`, or nil, error",
  doc = "The entries of an enum, by name or address.",
}, function(enum)
  return result(native.unrealEnum(enum))
end)

define(unreal, "instances", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "limit", type = "integer", optional = true },
  },
  returns = "list of addresses, or nil, error",
  doc = "The live objects of a class and its subclasses, without the class default objects. One pass over the object array; `limit` stops early.",
}, function(type, limit)
  return result(native.unrealInstances(type, limit))
end)

define(unreal, "find", {
  params = { { name = "name", type = "string" } },
  returns = "address or nil, or nil, error",
  doc = "The first object with that name, or with that full name when `name` has a dot in it.",
}, function(name)
  return many(native.unrealFind(name))
end)

define(unreal, "owner", {
  params = { { name = "address", type = "address" } },
  returns = "object, class, or nil, error",
  doc = "The object an address belongs to and its class; `ME-4204` when no live object holds it.",
}, function(address)
  return many(native.unrealOwner(address))
end)

define(unreal, "whois", {
  params = { { name = "address", type = "address" } },
  returns = "string, or nil, error",
  doc = "One line about an address: the object, its class, the offset, the property path down to the innermost field and that field's type, such as `BP_Pawn_C_1 (ABP_Pawn_C) +130: RootComponent  USceneComponent*`.",
}, function(address)
  return result(native.unrealWhois(address))
end)

define(unreal, "locate", {
  params = { { name = "address", type = "address" } },
  returns = "`{ object, class, offset, path, field, within, struct }`, or nil, error",
  doc = "What `whois` says, as a table: `path` is the property path as text, `field` the innermost field, `within` the offset inside it, and `struct` the outermost struct property that starts exactly at the address.",
}, function(address)
  return result(native.unrealLocate(address))
end)

define(unreal, "engine", {
  returns = "address or nil, or nil, error",
  doc = "The GameEngine object (or the Engine of an editor build).",
}, function()
  return many(native.unrealEngine())
end)

define(unreal, "world", {
  returns = "address or nil, or nil, error",
  doc = "The world the engine's game viewport shows; nil while no level is loaded.",
}, function()
  return many(native.unrealWorld())
end)

define(unreal, "controller", {
  returns = "address or nil, or nil, error",
  doc = "The first local player's PlayerController.",
}, function()
  return many(native.unrealController())
end)

define(unreal, "pawn", {
  returns = "address or nil, or nil, error",
  doc = "The pawn the local PlayerController possesses.",
}, function()
  return many(native.unrealPawn())
end)

define(unreal, "offline", {
  returns = "boolean or nil, or nil, error",
  doc = "true when the world has no net driver or one without connections, false when it is connected or hosts remote players, nil when it cannot tell. Writes in an online session can get the account banned.",
}, function()
  return many(native.unrealIsOffline())
end)

define(unreal, "walk", {
  params = {
    { name = "object", type = "address" },
    { name = "steps", type = "table" },
    { name = "root", type = "string", optional = true },
  },
  returns = "address, expression, field, or nil, error",
  doc = "Follows properties by name from an object: `walk(manifold.unreal.pawn(), { \"RootComponent\", \"RelativeLocation\" })`. Object properties are followed, structs entered, and `Name[i]` picks a TArray element. Gives the address of the last step, an address expression from `root` (the object's address by default) with offset symbols, such as `[GameEngine+GameEngine_GameViewport]+...`, and the last field.",
}, function(object, steps, root)
  return many(native.unrealWalk(object, steps, root))
end)

define(unreal, "dump", {
  params = { { name = "type", type = "string|integer" } },
  returns = "list of lines, or nil, error",
  doc = "A class or struct as C++-like lines: its name, parent and size, then one line per property with offset, type, name and the class that declares it.",
}, function(type)
  return result(native.unrealDump(type))
end)

define(unreal, "claims", {
  params = { { name = "address", type = "any" } },
  returns = "boolean",
  doc = "Whether the tool names the address itself (a live object or a remembered array), for other dissect scripts that step aside for Unreal objects. Never raises, false before it is ready.",
}, function(address)
  return native.unrealClaims(address) == true
end)

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

local ui = area("ui", "Windows, dialogs, notices, menu entries, toolbar buttons and status entries that Manifold draws itself, in its own look. A script describes what it wants (a window with a row of buttons, a table, a canvas) and keeps the handles it gets back; Manifold draws them every frame and calls the script's handlers when something happens. Colours are theme names (`accent`, `warning`, `muted`, ...), icons are Font Awesome names (`map-marker-alt`, `play`, ...). Everything a script made belongs to that script: when the same script runs again, its old windows, entries, notices and dialogs go away first.")

local function subarea(parent, name, summary)
  local path = parent.path .. "." .. name
  local proxy, members = strict(path)
  local section = { path = path, name = name, proxy = proxy, members = members }
  rawset(parent.members, name, proxy)
  native.docDefine(nil, path, { kind = "table", doc = summary })
  return section
end

local UI_COLORS = {
  "accent", "accentHovered", "success", "warning", "error", "text", "label", "muted", "changed",
  "page", "card", "cardBorder", "surface", "border", "rowSeparator", "frame", "button", "popup", "sidebar", "titleBar",
  "addressText", "bytesText", "codeKeyword", "codeRegister", "codeNumber", "codeString", "codeComment", "codeSection", "codeLabel",
  "flowJump", "flowCall", "flowReturn", "breakpoint", "currentLine", "shadow",
}
local UI_COLOR_SET = {}
for _, name in ipairs(UI_COLORS) do
  UI_COLOR_SET[name] = true
end

local UI_HANDLER_BITS = { onClick = 1, onChange = 2, onSelect = 4, onActivate = 8, onMenu = 16, onHover = 32, onSubmit = 64, onClose = 128, onView = 256 }
local UI_EVENTS = { click = "onClick", change = "onChange", select = "onSelect", activate = "onActivate", menu = "onMenu", hover = "onHover", submit = "onSubmit", close = "onClose", view = "onView" }

local COMMON_KEYS = { visible = "boolean", enabled = "boolean", tooltip = "string", width = "number", height = "number" }

local function withCommon(keys)
  local all = {}
  for key, kind in pairs(COMMON_KEYS) do
    all[key] = kind
  end
  for key, kind in pairs(keys) do
    all[key] = kind
  end
  return all
end

local WIDGET_KEYS = {
  row = withCommon({}),
  column = withCommon({}),
  section = withCommon({ title = "string", icon = "string", badge = "string", collapsible = "boolean" }),
  tabs = withCommon({}),
  tab = withCommon({ title = "string", icon = "string" }),
  split = withCommon({ vertical = "boolean", ratio = "number" }),
  text = withCommon({ text = "string", color = "string", wrap = "boolean", mono = "boolean" }),
  heading = withCommon({ text = "string", icon = "string" }),
  separator = withCommon({}),
  spacer = withCommon({}),
  button = withCommon({ text = "string", icon = "string", style = "string", onClick = "function" }),
  checkbox = withCommon({ label = "string", value = "boolean", onChange = "function" }),
  input = withCommon({ label = "string", value = "string", hint = "string", multiline = "boolean", password = "boolean", onChange = "function", onSubmit = "function" }),
  number = withCommon({ label = "string", value = "number", min = "number", max = "number", step = "number", integer = "boolean", hex = "boolean", format = "string", onChange = "function" }),
  slider = withCommon({ label = "string", value = "number", min = "number", max = "number", integer = "boolean", format = "string", onChange = "function" }),
  combo = withCommon({ label = "string", items = "table", value = "integer", onChange = "function" }),
  table = withCommon({ columns = "table", rows = "table", value = "integer", menu = "table", onSelect = "function", onActivate = "function", onMenu = "function" }),
  tree = withCommon({ items = "table", value = "string", menu = "table", onSelect = "function", onActivate = "function", onMenu = "function" }),
  progress = withCommon({ value = "number", text = "string" }),
  image = withCommon({ image = "table" }),
  canvas = withCommon({ background = "string", zoom = "boolean", menu = "table", onClick = "function", onHover = "function", onMenu = "function", onView = "function" }),
}
local WIDGET_KINDS = {}
for kind in pairs(WIDGET_KEYS) do
  WIDGET_KINDS[#WIDGET_KINDS + 1] = kind
end
table.sort(WIDGET_KINDS)
local CONTAINERS = { row = true, column = true, section = true, tabs = true, tab = true, split = true }
local BUTTON_STYLES = { default = true, accent = true, danger = true }

local DRAW_KEYS = {
  line = { kind = true, x = true, y = true, x2 = true, y2 = true, color = true, thickness = true },
  rect = { kind = true, x = true, y = true, width = true, height = true, x2 = true, y2 = true, color = true, filled = true, rounding = true, thickness = true },
  circle = { kind = true, x = true, y = true, radius = true, color = true, filled = true, thickness = true },
  polygon = { kind = true, points = true, color = true, filled = true, thickness = true },
  text = { kind = true, x = true, y = true, text = true, color = true, size = true },
  image = { kind = true, image = true, x = true, y = true, width = true, height = true, x2 = true, y2 = true },
}

local uiEntries = {}
local uiOwned = {}
local imageIds = setmetatable({}, { __mode = "k" })

local function ownerSource()
  local info = callerFrame()
  return info and info.source or ""
end

local function remember(owner, key, remove)
  local list = uiOwned[owner]
  if not list then
    list = {}
    uiOwned[owner] = list
  end
  list[key] = remove
end

local function forget(owner, key)
  local list = uiOwned[owner]
  if list then
    list[key] = nil
  end
end

local function gone(what)
  return nil, makeError("ME-7010", "The " .. what .. " is gone")
end

local function checkColor(path, name, value)
  if value == nil then
    return
  end
  if not UI_COLOR_SET[value] and not value:match("^#%x%x%x%x%x%x$") and not value:match("^#%x%x%x%x%x%x%x%x$") then
    misuse(("%s: %s %q is no theme colour (%s) and no #RRGGBB or #RRGGBBAA"):format(path, name, value, table.concat(UI_COLORS, ", ")))
  end
end

local function checkIcon(path, value)
  if value ~= nil and value ~= "" and not native.uiIsIcon(value) then
    local near = native.uiIcons(value:sub(1, math.min(#value, 4)))
    local hint = #near > 0 and (" (similar: " .. table.concat(near, ", ", 1, math.min(#near, 6)) .. ")") or ""
    misuse(("%s: icon %q is no Font Awesome icon%s; manifold.ui.theme.icons lists them"):format(path, value, hint))
  end
end

local function checkOptions(path, keys, options)
  if options == nil then
    return {}
  end
  if type(options) ~= "table" then
    misuse(("%s: options must be a table, got %s"):format(path, kindOf(options)))
  end
  local names = nil
  for key, value in pairs(options) do
    local kind = type(key) == "string" and keys[key]
    if not kind then
      names = names or table.concat(sortedNames(keys), ", ")
      misuse(("%s: options.%s is not an option (known options: %s)"):format(path, tostring(key), names))
    end
    if not CHECKS[kind](value) then
      misuse(("%s: options.%s must be %s, got %s"):format(path, key, NOUNS[kind], kindOf(value)))
    end
  end
  checkColor(path, "options.color", options.color)
  checkColor(path, "options.background", options.background)
  checkIcon(path, options.icon)
  if options.style ~= nil and not BUTTON_STYLES[options.style] then
    misuse(('%s: options.style must be one of "default", "accent", "danger", got %q'):format(path, options.style))
  end
  return options
end

local function handlerBits(options)
  local bits = 0
  for name, bit in pairs(UI_HANDLER_BITS) do
    if options[name] ~= nil then
      bits = bits | bit
    end
  end
  return bits
end

local function props(options)
  local copy = {}
  for key, value in pairs(options) do
    if type(value) ~= "function" then
      copy[key] = value
    end
  end
  if options.image ~= nil then
    copy.image = imageIds[options.image]
    if not copy.image then
      misuse("options.image must be an image from manifold.ui.image.load")
    end
  end
  copy.handlers = handlerBits(options)
  return copy
end

local function keepHandlers(entry, options)
  for name in pairs(UI_HANDLER_BITS) do
    if options[name] ~= nil then
      entry[name] = options[name]
    end
  end
end

local function dropEntry(id)
  local entry = uiEntries[id]
  if not entry then
    return
  end
  uiEntries[id] = nil
  for child in pairs(entry.children) do
    dropEntry(child)
  end
  local parent = entry.parent and uiEntries[entry.parent]
  if parent then
    parent.children[id] = nil
  end
end

local widgetObject

local function addTo(path, parentEntry, kind, options)
  if not WIDGET_KEYS[kind] then
    misuse(('%s: argument 1 (kind) must be one of "%s", got %q'):format(path, table.concat(WIDGET_KINDS, '", "'), kind))
  end
  if parentEntry.kind ~= "window" and not CONTAINERS[parentEntry.kind] then
    misuse(("%s: a %s holds no widgets; add to a window, row, column, section, tab or split"):format(path, parentEntry.kind))
  end
  if parentEntry.kind == "tabs" and kind ~= "tab" then
    misuse(path .. ": tabs hold tab widgets only")
  end
  if kind == "tab" and parentEntry.kind ~= "tabs" then
    misuse(path .. ": a tab goes into a tabs widget")
  end
  options = checkOptions(path .. "(" .. kind .. ")", WIDGET_KEYS[kind], options)
  if not native.uiExists(parentEntry.id) then
    return gone(parentEntry.kind)
  end
  local id, code, message = native.uiCreate(kind, parentEntry.id, parentEntry.owner, props(options))
  if not id then
    return result(nil, code, message)
  end
  local entry = { id = id, kind = kind, owner = parentEntry.owner, parent = parentEntry.id, children = {} }
  keepHandlers(entry, options)
  uiEntries[id] = entry
  parentEntry.children[id] = true
  entry.handle = widgetObject(entry)
  return entry.handle
end

local function liveEntry(entry)
  return uiEntries[entry.id] == entry and native.uiExists(entry.id)
end

local function updateEntry(path, entry, keys, options)
  options = checkOptions(path, keys, options)
  if not liveEntry(entry) then
    return gone(entry.kind)
  end
  keepHandlers(entry, options)
  local copy = props(options)
  local bits = 0
  for name, bit in pairs(UI_HANDLER_BITS) do
    if entry[name] ~= nil then
      bits = bits | bit
    end
  end
  copy.handlers = bits
  return result(native.uiUpdate(entry.id, copy))
end

local WIDGET_DOC = "manifold.ui.window.Widget."

widgetObject = function(entry)
  local prefix = ("widget(%s %d)"):format(entry.kind, entry.id)
  local proxy, members = strict(prefix)
  local function method(name, spec, body)
    bind(proxy, prefix .. "." .. name, members, name, spec, body, WIDGET_DOC .. name)
  end
  method("add", { params = { { name = "kind", type = "string" }, { name = "options", type = "table", optional = true } } }, function(kind, options)
    return addTo(prefix .. ".add", entry, kind, options)
  end)
  method("clear", {}, function()
    if not liveEntry(entry) then
      return gone(entry.kind)
    end
    for child in pairs(entry.children) do
      dropEntry(child)
    end
    entry.children = {}
    return result(native.uiClear(entry.id))
  end)
  method("remove", {}, function()
    if uiEntries[entry.id] ~= entry then
      return false
    end
    dropEntry(entry.id)
    return native.uiRemove(entry.id)
  end)
  method("update", { params = { { name = "options", type = "table" } } }, function(options)
    return updateEntry(prefix .. ".update", entry, WIDGET_KEYS[entry.kind], options)
  end)
  method("get", {}, function()
    if not liveEntry(entry) then
      return gone(entry.kind)
    end
    return native.uiGet(entry.id)
  end)
  method("set", { params = { { name = "value", type = "any", optional = true } } }, function(value)
    if not liveEntry(entry) then
      return gone(entry.kind)
    end
    return result(native.uiSet(entry.id, value))
  end)
  method("draw", { params = { { name = "drawings", type = "table" }, { name = "options", type = "table", optional = true, keys = { append = "boolean" } } } }, function(drawings, options)
    if entry.kind ~= "canvas" then
      misuse(prefix .. ".draw: only a canvas draws")
    end
    if not liveEntry(entry) then
      return gone("canvas")
    end
    local prepared = {}
    for index, drawing in ipairs(drawings) do
      if type(drawing) ~= "table" then
        misuse(("%s.draw: drawing %d must be a table, got %s"):format(prefix, index, kindOf(drawing)))
      end
      local allowed = DRAW_KEYS[drawing.kind]
      if not allowed then
        misuse(('%s.draw: drawing %d: kind must be one of "circle", "image", "line", "polygon", "rect", "text", got %s'):format(prefix, index, tostring(drawing.kind)))
      end
      for key in pairs(drawing) do
        if not allowed[key] then
          misuse(("%s.draw: drawing %d (%s) has no %s"):format(prefix, index, drawing.kind, tostring(key)))
        end
      end
      if drawing.color ~= nil then
        checkColor(prefix .. ".draw", "drawing " .. index .. " color", drawing.color)
      end
      if drawing.kind == "image" then
        local copy = {}
        for key, value in pairs(drawing) do
          copy[key] = value
        end
        copy.image = imageIds[drawing.image]
        if not copy.image then
          misuse(("%s.draw: drawing %d needs an image from manifold.ui.image.load"):format(prefix, index))
        end
        drawing = copy
      end
      prepared[index] = drawing
    end
    return result(native.uiDraw(entry.id, prepared, options and options.append))
  end)
  method("getView", {}, function()
    if entry.kind ~= "canvas" then
      misuse(prefix .. ".getView: only a canvas has a view")
    end
    if not liveEntry(entry) then
      return gone("canvas")
    end
    return native.uiView(entry.id)
  end)
  method("setView", { params = { { name = "view", type = "table", keys = { x = "number", y = "number", scale = "number" } } } }, function(view)
    if entry.kind ~= "canvas" then
      misuse(prefix .. ".setView: only a canvas has a view")
    end
    if not liveEntry(entry) then
      return gone("canvas")
    end
    return result(native.uiSetView(entry.id, view))
  end)
  rawset(members, "id", entry.id)
  rawset(members, "kind", entry.kind)
  return proxy
end

local WINDOW_KEYS = { icon = "string", width = "number", height = "number", open = "boolean", dock = "boolean", interval = "number", key = "string", onClose = "function", onUpdate = "function" }
local WINDOW_UPDATE_KEYS = { title = "string", icon = "string", interval = "number", onClose = "function", onUpdate = "function" }
local WINDOW_DOC = "manifold.ui.window.Window."
local windowKeys = {}

local function windowObject(entry)
  local prefix = ("window(%q)"):format(entry.title)
  local proxy, members = strict(prefix)
  local function method(name, spec, body)
    bind(proxy, prefix .. "." .. name, members, name, spec, body, WINDOW_DOC .. name)
  end
  method("add", { params = { { name = "kind", type = "string" }, { name = "options", type = "table", optional = true } } }, function(kind, options)
    return addTo(prefix .. ".add", entry, kind, options)
  end)
  method("clear", {}, function()
    if not liveEntry(entry) then
      return gone("window")
    end
    for child in pairs(entry.children) do
      dropEntry(child)
    end
    entry.children = {}
    return result(native.uiClear(entry.id))
  end)
  method("remove", {}, function()
    if uiEntries[entry.id] ~= entry then
      return false
    end
    dropEntry(entry.id)
    forget(entry.owner, entry.id)
    if entry.key and windowKeys[entry.key] == entry then
      windowKeys[entry.key] = nil
    end
    return native.uiRemove(entry.id)
  end)
  method("update", { params = { { name = "options", type = "table" } } }, function(options)
    options = checkOptions(prefix .. ".update", WINDOW_UPDATE_KEYS, options)
    if not liveEntry(entry) then
      return gone("window")
    end
    if options.onClose ~= nil then
      entry.onClose = options.onClose
    end
    if options.onUpdate ~= nil then
      entry.onUpdate = options.onUpdate
    end
    local copy = { title = options.title, icon = options.icon, interval = options.interval, handlers = entry.onClose and UI_HANDLER_BITS.onClose or 0 }
    if options.onUpdate ~= nil then
      copy.updates = true
    end
    return result(native.uiUpdate(entry.id, copy))
  end)
  method("show", {}, function()
    if not liveEntry(entry) then
      return gone("window")
    end
    return result(native.uiShow(entry.id, true, false))
  end)
  method("hide", {}, function()
    if not liveEntry(entry) then
      return gone("window")
    end
    return result(native.uiShow(entry.id, false, false))
  end)
  method("focus", {}, function()
    if not liveEntry(entry) then
      return gone("window")
    end
    return result(native.uiShow(entry.id, true, true))
  end)
  method("isOpen", {}, function()
    return liveEntry(entry) and native.uiIsOpen(entry.id)
  end)
  rawset(members, "id", entry.id)
  rawset(members, "kind", "window")
  return proxy
end

local window = subarea(ui, "window", "Tool windows that dock into the workspace like Manifold's own. A window holds widgets; containers (`row`, `column`, `section`, `tabs` with `tab`, `split`) hold more widgets.")

define(window, "create", {
  params = {
    { name = "title", type = "string" },
    { name = "options", type = "table", optional = true, keys = WINDOW_KEYS },
  },
  returns = "window",
  doc = "Opens a window and returns its handle. Options: `icon` (Font Awesome name), `width` and `height` (the first size, in points), `open` (false creates it hidden), `dock` (false keeps it out of the dock space), `onClose(window)` when the user closes it (it is hidden, not removed; `show` brings it back), `onUpdate(window)` every `interval` seconds while it is open (default every frame), and `key`: a window created with the same key replaces the earlier one.",
}, function(title, options)
  options = options or {}
  checkIcon("manifold.ui.window.create", options.icon)
  local owner = ownerSource()
  if options.key and windowKeys[options.key] then
    local earlier = windowKeys[options.key]
    if uiEntries[earlier.id] == earlier then
      earlier.handle.remove()
    end
  end
  local id = native.uiCreate("window", 0, owner, {
    title = title,
    icon = options.icon,
    width = options.width,
    height = options.height,
    dock = options.dock,
    interval = options.interval,
    updates = options.onUpdate ~= nil,
    handlers = options.onClose and UI_HANDLER_BITS.onClose or 0,
  })
  if options.open == false then
    native.uiShow(id, false, false)
  end
  local entry = { id = id, kind = "window", owner = owner, children = {}, title = title, key = options.key, onClose = options.onClose, onUpdate = options.onUpdate }
  uiEntries[id] = entry
  entry.handle = windowObject(entry)
  if options.key then
    windowKeys[options.key] = entry
  end
  remember(owner, id, entry.handle.remove)
  return entry.handle
end)

native.docDefine(nil, "manifold.ui.window.Window", {
  kind = "class",
  title = "Window objects",
  doc = "`manifold.ui.window.create` returns a window object: dot calls only, read-only, unknown names raise. A call on a window that is gone (removed, or its script ran again) returns `nil, error` with code `ME-7010`.",
})
local WINDOW_METHOD_DOCS = {
  add = { params = { { name = "kind", type = "string" }, { name = "options", type = "table", optional = true } }, returns = "widget", doc = "Adds a widget at the end of the window and returns its handle. `kind` is one of `row`, `column`, `section`, `tabs`, `split`, `text`, `heading`, `separator`, `spacer`, `button`, `checkbox`, `input`, `number`, `slider`, `combo`, `table`, `tree`, `progress`, `image`, `canvas`; the options depend on it (see the widget object)." },
  clear = { returns = "boolean", doc = "Removes every widget of the window." },
  remove = { returns = "boolean", doc = "Removes the window and its widgets for good." },
  update = { params = { { name = "options", type = "table", keys = WINDOW_UPDATE_KEYS } }, returns = "boolean", doc = "Changes the title, icon, update interval or handlers." },
  show = { returns = "boolean", doc = "Shows the window again after the user or `hide` closed it." },
  hide = { returns = "boolean", doc = "Hides the window; `show` brings it back with its widgets." },
  focus = { returns = "boolean", doc = "Shows the window and brings it to the front." },
  isOpen = { returns = "boolean", doc = "Whether the window exists and is shown." },
}
for _, name in ipairs({ "add", "clear", "remove", "update", "show", "hide", "focus", "isOpen" }) do
  native.docDefine(nil, WINDOW_DOC .. name, WINDOW_METHOD_DOCS[name])
end
native.docDefine(nil, WINDOW_DOC .. "id", { kind = "field", type = "integer", doc = "The window's number." })
native.docDefine(nil, WINDOW_DOC .. "kind", { kind = "field", type = "string", doc = "Always `window`." })

native.docDefine(nil, "manifold.ui.window.Widget", {
  kind = "class",
  title = "Widget objects",
  doc = table.concat({
    "`add` returns a widget object, with the same rules as a window object. Every widget takes `visible`, `enabled`, `tooltip`, `width` and `height` (points; 0 or missing means automatic). Handlers get the widget as their last argument.",
    "- `row`, `column`: lay out their widgets side by side or one below the other.",
    "- `section`: a card with `title`, `icon`, `badge`; `collapsible` lets the user fold it.",
    "- `tabs` holds `tab` widgets (`title`, `icon`); `split` shows its first two widgets side by side (`vertical` stacks them) at `ratio` (0.5).",
    "- `text` (`text`, `color`, `wrap`, `mono`), `heading` (`text`, `icon`), `separator`, `spacer`.",
    "- `button`: `text`, `icon`, `style` (`default`, `accent`, `danger`), `onClick(widget)`.",
    "- `checkbox` (`label`, boolean `value`), `input` (`label`, string `value`, `hint`, `multiline`, `password`, `onSubmit(text, widget)` on Enter), `number` (`label`, `value`, `min`, `max`, `step`, `integer`, `hex`, `format` like `%.2f`), `slider` (`label`, `value`, `min`, `max`, `integer`, `format`), `combo` (`label`, `items`, `value` = the chosen item's number): all call `onChange(value, widget)`.",
    "- `table`: `columns` (titles, or `{ title, width, align }`), `rows` (lists of cells), `value` = the selected row's number, `onSelect(row, widget)`, `onActivate(row, widget)` on a double click, and `menu` (a list of entries; `-` is a separator) with `onMenu(choice, text, row, widget)`.",
    "- `tree`: `items` (`{ text, icon, id, open, children }`, or plain strings), `value` = the selected item's `id`, `onSelect(id, widget)`, `onActivate(id, widget)`, `menu` with `onMenu(choice, text, id, widget)`.",
    "- `progress` (`value` 0..1, `text`), `image` (`image` from `manifold.ui.image.load`).",
    "- `canvas`: draws what `draw` gave it on the GPU. `background` is a colour, `zoom = true` lets the user zoom with the wheel and pan by dragging; `onClick(x, y, button, widget)`, `onHover(x, y, widget)`, `onView(x, y, scale, widget)` and `menu` with `onMenu(choice, text, x, y, widget)` get canvas coordinates.",
  }, "\n"),
})
local WIDGET_METHOD_DOCS = {
  add = { params = { { name = "kind", type = "string" }, { name = "options", type = "table", optional = true } }, returns = "widget", doc = "Adds a widget to a container (`row`, `column`, `section`, `tab`, `split`; `tabs` take `tab` widgets only)." },
  clear = { returns = "boolean", doc = "Removes a container's widgets, or a canvas's drawings." },
  remove = { returns = "boolean", doc = "Removes the widget and everything in it." },
  update = { params = { { name = "options", type = "table" } }, returns = "boolean", doc = "Changes options of the widget: the same keys `add` takes for its kind, handlers included." },
  get = { returns = "value", doc = "The widget's value: a checkbox's boolean, an input's text, a number, the chosen combo item's or table row's number, the selected tree item's id, a progress fraction, otherwise the text. Returns nil when nothing is selected." },
  set = { params = { { name = "value", type = "any", optional = true } }, returns = "boolean", doc = "Sets the widget's value (see `get`); nil clears a selection. It does not call `onChange`." },
  draw = { params = { { name = "drawings", type = "table" }, { name = "options", type = "table", optional = true, keys = { append = "boolean" } } }, returns = "boolean", doc = "Replaces a canvas's drawings (`options.append` adds them instead). A drawing is a table with `kind`: `line` (`x`, `y`, `x2`, `y2`, `thickness`), `rect` (`x`, `y`, `width`, `height`, `filled`, `rounding`, `thickness`), `circle` (`x`, `y`, `radius`, `filled`, `thickness`), `polygon` (`points` = `{ x1, y1, x2, y2, ... }`, `filled`), `text` (`x`, `y`, `text`, `size`) or `image` (`image`, `x`, `y`, `width`, `height`); all but images take `color` (a theme name or `#RRGGBB`/`#RRGGBBAA`)." },
  getView = { returns = "table", doc = "A canvas's view `{ x, y, scale }`: the canvas point at its top left corner and the zoom." },
  setView = { params = { { name = "view", type = "table", keys = { x = "number", y = "number", scale = "number" } } }, returns = "boolean", doc = "Moves or zooms a canvas's view." },
}
for _, name in ipairs({ "add", "clear", "remove", "update", "get", "set", "draw", "getView", "setView" }) do
  native.docDefine(nil, WIDGET_DOC .. name, WIDGET_METHOD_DOCS[name])
end
native.docDefine(nil, WIDGET_DOC .. "id", { kind = "field", type = "integer", doc = "The widget's number." })
native.docDefine(nil, WIDGET_DOC .. "kind", { kind = "field", type = "string", doc = "The kind it was added as." })

local function plainObject(entry, prefix, docPrefix, methods, fields)
  local proxy, members = strict(prefix)
  for name, spec in pairs(methods) do
    bind(proxy, prefix .. "." .. name, members, name, spec, spec.body, docPrefix .. name)
  end
  rawset(members, "id", entry.id)
  for key, value in pairs(fields or {}) do
    rawset(members, key, value)
  end
  return proxy
end

local function dialogObject(entry)
  return plainObject(entry, ("dialog(%d)"):format(entry.id), "manifold.ui.dialog.Dialog.", {
    close = {
      body = function()
        if uiEntries[entry.id] ~= entry then
          return false
        end
        uiEntries[entry.id] = nil
        forget(entry.owner, entry.id)
        return native.uiDialogClose(entry.id)
      end,
    },
  })
end

native.docDefine(nil, "manifold.ui.dialog.Dialog", {
  kind = "class",
  title = "Dialog objects",
  doc = "The dialog functions return at once; the dialog shows on top of the program and calls `onClose` with the answer when the user is done.",
})
native.docDefine(nil, "manifold.ui.dialog.Dialog.close", { returns = "boolean", doc = "Closes the dialog without an answer; `onClose` is not called." })
native.docDefine(nil, "manifold.ui.dialog.Dialog.id", { kind = "field", type = "integer", doc = "The dialog's number." })

local dialog = subarea(ui, "dialog", "Dialogs on top of the program. They do not block the script: each takes `onClose`, which gets the answer, or nil when the user cancelled.")

local function openDialog(kind, props, onClose)
  local owner = ownerSource()
  local id = native.uiDialog(kind, owner, props)
  local entry = { id = id, kind = "dialog", owner = owner, children = {}, onDialog = onClose }
  uiEntries[id] = entry
  entry.handle = dialogObject(entry)
  remember(owner, id, entry.handle.close)
  return entry.handle
end

local DIALOG_KEYS = { title = "string", onClose = "function" }

define(dialog, "confirm", {
  params = {
    { name = "text", type = "string" },
    { name = "options", type = "table", optional = true, keys = { title = "string", danger = "boolean", onClose = "function" } },
  },
  returns = "dialog",
  doc = "Asks a yes/no question. `onClose(answer)` gets true or false; `danger = true` shows the yes button in the warning colour.",
}, function(text, options)
  options = options or {}
  return openDialog("confirm", { text = text, title = options.title, danger = options.danger }, options.onClose)
end)

define(dialog, "input", {
  params = {
    { name = "text", type = "string" },
    { name = "options", type = "table", optional = true, keys = { title = "string", value = "string", onClose = "function" } },
  },
  returns = "dialog",
  doc = "Asks for a line of text, starting from `value`. `onClose(text)` gets the text, or nil.",
}, function(text, options)
  options = options or {}
  return openDialog("input", { text = text, title = options.title, value = options.value }, options.onClose)
end)

define(dialog, "choose", {
  params = {
    { name = "text", type = "string" },
    { name = "items", type = "table" },
    { name = "options", type = "table", optional = true, keys = DIALOG_KEYS },
  },
  returns = "dialog",
  doc = "Lets the user pick one of `items`. `onClose(number, text)` gets the chosen item's number and text, or nil.",
}, function(text, items, options)
  options = options or {}
  local names = {}
  for index, item in ipairs(items) do
    names[index] = tostring(item)
  end
  return openDialog("choose", { text = text, title = options.title, items = names }, options.onClose)
end)

local FILE_KEYS = { title = "string", filters = "table", name = "string", onClose = "function" }
local FILE_TEXT = " `filters` is a list of `{ \"Lua scripts\", \"*.lua\" }` pairs; `onClose(path)` gets the full path, or nil."

local function flatFilters(path, filters)
  if filters == nil then
    return nil
  end
  local flat = {}
  for index, pair in ipairs(filters) do
    if type(pair) ~= "table" or type(pair[1]) ~= "string" or type(pair[2]) ~= "string" then
      misuse(("%s: filter %d must be { description, pattern }"):format(path, index))
    end
    flat[#flat + 1] = pair[1]
    flat[#flat + 1] = pair[2]
  end
  return flat
end

define(dialog, "openFile", {
  params = { { name = "options", type = "table", optional = true, keys = FILE_KEYS } },
  returns = "dialog",
  doc = "The Windows file dialog for opening a file." .. FILE_TEXT,
}, function(options)
  options = options or {}
  return openDialog("openFile", { title = options.title, value = options.name, filters = flatFilters("manifold.ui.dialog.openFile", options.filters) }, options.onClose)
end)

define(dialog, "saveFile", {
  params = { { name = "options", type = "table", optional = true, keys = FILE_KEYS } },
  returns = "dialog",
  doc = "The Windows file dialog for saving, starting with `name`." .. FILE_TEXT,
}, function(options)
  options = options or {}
  return openDialog("saveFile", { title = options.title, value = options.name, filters = flatFilters("manifold.ui.dialog.saveFile", options.filters) }, options.onClose)
end)

local NOTICE_LEVELS = { "info", "success", "warning", "error" }
local NOTICE_KEYS = { level = "string", progress = "number", sticky = "boolean" }

local notice = subarea(ui, "notice", "Short notices in the lower right corner, the same ones Manifold shows. A notice fades after a few seconds unless it is `sticky` or has a `progress`; a right click dismisses it.")

local function checkLevel(path, level)
  if level ~= nil then
    local known = false
    for _, name in ipairs(NOTICE_LEVELS) do
      known = known or name == level
    end
    if not known then
      misuse(('%s: options.level must be one of "info", "success", "warning", "error", got %q'):format(path, level))
    end
  end
end

local function noticeObject(entry)
  local state = entry.state
  return plainObject(entry, ("notice(%d)"):format(entry.id), "manifold.ui.notice.Notice.", {
    update = {
      params = { { name = "options", type = "table", keys = { text = "string", level = "string", progress = "number", sticky = "boolean" } } },
      body = function(options)
        checkLevel("notice.update", options.level)
        state.text = options.text or state.text
        state.level = options.level or state.level
        if options.progress ~= nil then
          state.progress = options.progress
        end
        if options.sticky ~= nil then
          state.sticky = options.sticky
        end
        local sticky = state.sticky or (state.progress >= 0 and state.progress < 1)
        if not native.uiNoticeUpdate(entry.id, state.text, state.level, state.progress, sticky) then
          return gone("notice")
        end
        return true
      end,
    },
    remove = {
      body = function()
        forget(entry.owner, "notice" .. entry.id)
        return native.uiNoticeRemove(entry.id)
      end,
    },
    isShown = {
      body = function()
        return native.uiNoticeExists(entry.id)
      end,
    },
  })
end

define(notice, "show", {
  params = {
    { name = "text", type = "string" },
    { name = "options", type = "table", optional = true, keys = NOTICE_KEYS },
  },
  returns = "notice",
  doc = "Shows a notice. `level` is `info` (default), `success`, `warning` or `error`; `progress` (0..1) adds a bar and keeps the notice up until it reaches 1; `sticky` keeps it until it is removed or dismissed.",
}, function(text, options)
  options = options or {}
  checkLevel("manifold.ui.notice.show", options.level)
  local progress = options.progress or -1
  local sticky = options.sticky or (progress >= 0 and progress < 1)
  local owner = ownerSource()
  local id = native.uiNotice(text, options.level or "info", progress, sticky)
  local entry = { id = id, owner = owner, state = { text = text, level = options.level or "info", progress = progress, sticky = options.sticky or false } }
  local handle = noticeObject(entry)
  remember(owner, "notice" .. id, handle.remove)
  return handle
end)

native.docDefine(nil, "manifold.ui.notice.Notice", { kind = "class", title = "Notice objects", doc = "`manifold.ui.notice.show` returns a notice object." })
native.docDefine(nil, "manifold.ui.notice.Notice.update", { params = { { name = "options", type = "table", keys = { text = "string", level = "string", progress = "number", sticky = "boolean" } } }, returns = "boolean", doc = "Changes the text, level, progress or stickiness, and shows the notice for its full time again." })
native.docDefine(nil, "manifold.ui.notice.Notice.remove", { returns = "boolean", doc = "Takes the notice down." })
native.docDefine(nil, "manifold.ui.notice.Notice.isShown", { returns = "boolean", doc = "Whether the notice is still up." })
native.docDefine(nil, "manifold.ui.notice.Notice.id", { kind = "field", type = "integer", doc = "The notice's number." })

local ENTRY_DOC = "manifold.ui.Entry."

local function entryObject(entry, keys)
  return plainObject(entry, ("%s(%d)"):format(entry.kind, entry.id), ENTRY_DOC, {
    update = {
      params = { { name = "options", type = "table" } },
      body = function(options)
        options = checkOptions(entry.kind .. ".update", keys, options)
        if uiEntries[entry.id] ~= entry then
          return gone("entry")
        end
        if options.onClick ~= nil then
          entry.onClick = options.onClick
        end
        local copy = {}
        for key, value in pairs(options) do
          if type(value) ~= "function" then
            copy[key] = value
          end
        end
        return result(native.uiSlotUpdate(entry.id, copy))
      end,
    },
    remove = {
      body = function()
        if uiEntries[entry.id] ~= entry then
          return false
        end
        uiEntries[entry.id] = nil
        forget(entry.owner, entry.id)
        return native.uiSlotRemove(entry.id)
      end,
    },
  })
end

local function addEntry(kind, keys, values, options)
  options = checkOptions("manifold.ui." .. kind .. ".add", keys, options)
  local owner = ownerSource()
  local copy = {}
  for key, value in pairs(values) do
    copy[key] = value
  end
  for key, value in pairs(options) do
    if type(value) ~= "function" then
      copy[key] = value
    end
  end
  local id = native.uiSlotAdd(kind, owner, copy)
  local entry = { id = id, kind = kind, owner = owner, children = {}, onClick = options.onClick }
  uiEntries[id] = entry
  entry.handle = entryObject(entry, keys)
  remember(owner, id, entry.handle.remove)
  return entry.handle
end

native.docDefine(nil, "manifold.ui.Entry", { kind = "class", title = "Menu, toolbar and status entries", doc = "`menu.add`, `toolbar.add` and `status.add` return an entry object; it takes the options its `add` took." })
native.docDefine(nil, ENTRY_DOC .. "update", { params = { { name = "options", type = "table" } }, returns = "boolean", doc = "Changes the entry: the options its `add` took." })
native.docDefine(nil, ENTRY_DOC .. "remove", { returns = "boolean", doc = "Takes the entry out." })
native.docDefine(nil, ENTRY_DOC .. "id", { kind = "field", type = "integer", doc = "The entry's number." })

local MENU_KEYS = { icon = "string", enabled = "boolean", checked = "boolean", onClick = "function" }
local menu = subarea(ui, "menu", "Entries in the main window's Extensions menu, beside the menus of Cheat Engine scripts. The menu shows as soon as a script adds one.")

define(menu, "add", {
  params = {
    { name = "path", type = "string" },
    { name = "options", type = "table", optional = true, keys = MENU_KEYS },
  },
  returns = "entry",
  doc = "Adds an entry. `path` may name submenus with slashes (`Teleporter/Save position`); `-` as the last part is a separator. Options: `icon`, `enabled`, `checked` (shows a check mark), `onClick(entry)`.",
}, function(path, options)
  local text = path:match("([^/]+)$") or path
  return addEntry("menu", MENU_KEYS, { path = path, text = text }, options)
end)

local TOOLBAR_KEYS = { text = "string", tooltip = "string", enabled = "boolean", onClick = "function", icon = "string" }
local toolbar = subarea(ui, "toolbar", "Buttons in the main window's title bar, between the process and the Memory view button.")

define(toolbar, "add", {
  params = {
    { name = "icon", type = "string" },
    { name = "options", type = "table", optional = true, keys = TOOLBAR_KEYS },
  },
  returns = "entry",
  doc = "Adds an icon button. Options: `text` beside the icon, `tooltip`, `enabled`, `onClick(entry)`.",
}, function(icon, options)
  checkIcon("manifold.ui.toolbar.add", icon)
  return addEntry("toolbar", TOOLBAR_KEYS, { icon = icon }, options)
end)

local STATUS_KEYS = { icon = "string", tooltip = "string", color = "string", onClick = "function", text = "string" }
local status = subarea(ui, "status", "Entries in the main window's status bar, after the administrator note.")

define(status, "add", {
  params = {
    { name = "text", type = "string" },
    { name = "options", type = "table", optional = true, keys = STATUS_KEYS },
  },
  returns = "entry",
  doc = "Adds a status entry. Options: `icon`, `tooltip`, `color` (a theme name), `onClick(entry)`.",
}, function(text, options)
  return addEntry("status", STATUS_KEYS, { text = text }, options)
end)

local image = subarea(ui, "image", "Pictures for `image` widgets and canvas drawings.")

define(image, "load", {
  params = { { name = "path", type = "string" } },
  returns = "image",
  doc = "Loads a PNG, JPEG, BMP, TGA or GIF file (a relative path starts at the program folder) and returns an image object with `width`, `height` and `remove()`, or nil and an `ME-7009` error.",
}, function(path)
  local owner = ownerSource()
  local id, width, height = native.uiImageLoad(path, owner)
  if not id then
    return result(nil, width, height)
  end
  local entry = { id = id, owner = owner }
  local handle = plainObject(entry, ("image(%d)"):format(id), "manifold.ui.image.Image.", {
    remove = {
      body = function()
        forget(owner, "image" .. id)
        return native.uiImageRemove(id)
      end,
    },
  }, { width = width, height = height })
  imageIds[handle] = id
  remember(owner, "image" .. id, handle.remove)
  return handle
end)

native.docDefine(nil, "manifold.ui.image.Image", { kind = "class", title = "Image objects", doc = "`manifold.ui.image.load` returns an image object." })
native.docDefine(nil, "manifold.ui.image.Image.remove", { returns = "boolean", doc = "Frees the picture; widgets and drawings that show it go blank." })
native.docDefine(nil, "manifold.ui.image.Image.id", { kind = "field", type = "integer", doc = "The image's number." })
native.docDefine(nil, "manifold.ui.image.Image.width", { kind = "field", type = "integer", doc = "Width in pixels." })
native.docDefine(nil, "manifold.ui.image.Image.height", { kind = "field", type = "integer", doc = "Height in pixels." })

local theme = subarea(ui, "theme", "The names `manifold.ui` takes for colours and icons.")

define(theme, "colors", {
  returns = "table",
  doc = "The theme colour names, which follow the theme and light or dark mode.",
}, function()
  return table.move(UI_COLORS, 1, #UI_COLORS, 1, {})
end)

define(theme, "icons", {
  params = { { name = "filter", type = "string", optional = true } },
  returns = "table",
  doc = "The Font Awesome icon names, all of them or those that contain `filter`.",
}, function(filter)
  return native.uiIcons(filter)
end)

define(theme, "isIcon", {
  params = { { name = "name", type = "string" } },
  returns = "boolean",
  doc = "Whether `name` is a Font Awesome icon name.",
}, function(name)
  return native.uiIsIcon(name)
end)

local function forgetSource(source)
  local list = uiOwned[source]
  if list then
    uiOwned[source] = nil
    for _, remove in pairs(list) do
      pcall(remove)
    end
  end
  native.uiForgetOwner(source)
  for key, entry in pairs(windowKeys) do
    if entry.owner == source then
      windowKeys[key] = nil
    end
  end
end

debug.getregistry()["Manifold.ChunkStarting"] = forgetSource

local function callHandler(handler, ...)
  local ok, problem = xpcall(handler, raisedHandler, ...)
  if not ok then
    emit("error", "manifold.ui", problem)
  end
  return ok
end

debug.getregistry()["Manifold.UiTick"] = function(now)
  for _, id in ipairs(native.uiDue(now)) do
    local entry = uiEntries[id]
    if entry and entry.onUpdate and not callHandler(entry.onUpdate, entry.handle) then
      native.uiUpdate(id, { updates = false })
    end
  end
  for _, event in ipairs(native.uiTakeEvents()) do
    local entry = uiEntries[event.target]
    if entry then
      if event.name == "dialog" then
        uiEntries[event.target] = nil
        forget(entry.owner, entry.id)
        if entry.onDialog then
          callHandler(entry.onDialog, table.unpack(event, 1, event.n))
        end
      else
        local handler = entry[UI_EVENTS[event.name] or ""]
        if handler then
          local values = table.move(event, 1, event.n, 1, {})
          values[event.n + 1] = entry.handle
          callHandler(handler, table.unpack(values, 1, event.n + 1))
        end
      end
    end
  end
end

rawset(_G, "manifold", manifold)

local INTRO = [[
# Manifold Lua API

`manifold` is Manifold Engine's own Lua API. It lives beside the Cheat Engine functions in [LUA-API.md](LUA-API.md). Those keep Cheat Engine's names and habits for compatibility; `manifold` follows one fixed schema instead, and every call is checked against it.

This file is generated from the API itself (`Manifold-Engine.Tests.exe --manifold-markdown`), and the tests fail when it is out of date.
]]

local RULES = [[
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
]]

local function markdown()
  return table.concat({ INTRO, RULES, native.docMarkdown("manifold") }, "\n")
end

return markdown
