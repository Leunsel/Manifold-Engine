local native = ...

local SOURCE = debug.getinfo(1, "S").source
local API_VERSION = 1
local RUNTIME_CODE = "ME-7002"

local LEVELS = { "debug", "info", "success", "warning", "error" }
local RANK = {}
for index, name in ipairs(LEVELS) do
  RANK[name] = index
end

local MEMORY_TYPES = { "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "float", "double", "pointer", "bytes", "string" }
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

local docs = setmetatable({}, { __mode = "k" })
local entries = {}
local areas = {}
local manifold, rootMembers = strict("manifold")

local root = { path = "manifold", functions = {}, constants = {} }

local function area(name, summary)
  local proxy, members = strict("manifold." .. name)
  local section = { path = "manifold." .. name, name = name, summary = summary, proxy = proxy, members = members, functions = {}, objects = {} }
  areas[#areas + 1] = section
  rawset(rootMembers, name, proxy)
  return section
end

local function bind(owner, path, members, name, spec, body)
  local params = prepare(spec.params or {})
  local wrapper = guard(path, owner, params, spec.varargs, body)
  rawset(members, name, wrapper)
  docs[wrapper] = { path = path, params = paramText(params, spec.varargs), returns = spec.returns, text = spec.doc }
  return wrapper, params
end

local function define(section, name, spec, body)
  local path = section.path .. "." .. name
  local _, params = bind(section.proxy, path, section.members, name, spec, body)
  local entry = { path = path, name = name, params = paramText(params, spec.varargs), returns = spec.returns, doc = spec.doc }
  section.functions[#section.functions + 1] = entry
  entries[path:sub(#"manifold." + 1)] = entry
end

local function constant(name, value, doc)
  rawset(rootMembers, name, value)
  root.constants[#root.constants + 1] = { path = "manifold." .. name, name = name, doc = doc, value = value }
  entries[name] = root.constants[#root.constants]
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
  local text, traceback
  if isError(message) then
    text = errorText(message)
    traceback = rawget(message, "traceback")
  elseif type(message) == "string" then
    text = message
  else
    text = inspect(message)
  end
  if fields ~= nil then
    text = text .. "  " .. inspect(fields)
  end
  if type(traceback) == "string" and traceback ~= "" then
    text = text .. "\n" .. traceback
  end
  local info = callerFrame()
  native.writeLine(RANK[level] - 1, text, channel or "", info and chunkName(info) or "", info and info.name or "", info and info.currentline or 0)
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
      returns = "nothing",
      doc = LEVEL_DOCS[level],
    }, function(message, fields)
      emit(level, name, message, fields)
    end)
  end
  bind(proxy, prefix .. ".channel", members, "channel", {
    params = { { name = "name", type = "string" } },
    returns = "channel",
    doc = "A sub-channel named `parent/name`.",
  }, function(sub)
    return channelObject(name .. "/" .. sub)
  end)
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

log.objects[#log.objects + 1] = {
  title = "Channel objects",
  summary = "`manifold.log.channel(name)` returns an object that follows the same rules: dot calls only, read-only, unknown names raise.",
  rows = {
    { signature = "channel.debug(message, fields?)` … `channel.error(message, fields?)", returns = "nothing", doc = "Like the `manifold.log` functions, tagged with `[name]`." },
    { signature = "channel.channel(name)", returns = "channel", doc = "A sub-channel named `parent/name`." },
    { signature = "channel.name", returns = "string", doc = "The channel's name." },
  },
}

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

local memory = area("memory", "Typed reads and writes in the opened process. Types: `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` (integers; `u64` above 2^63 comes back negative, as Lua integers are signed), `float`, `double`, `pointer` (8 or 4 bytes by the process), `bytes` (a list of byte values) and `string` (UTF-8 in Lua, UTF-16 in memory with `wide`). A write checks that the value fits the type before it touches memory.")

local function readOptions(kind, options)
  options = options or {}
  if kind == "bytes" then
    if options.count == nil then
      misuse("manifold.memory.read: type \"bytes\" needs options.count")
    end
    if options.count < 1 or options.count > MAX_BYTES then
      misuse("manifold.memory.read: options.count must be 1 to " .. MAX_BYTES)
    end
  elseif options.count ~= nil then
    misuse("manifold.memory.read: options.count is for type \"bytes\"")
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
  doc = "Reads one value. `bytes` needs `options.count`; `string` reads up to `options.length` characters (default 256) and stops at the first zero.",
}, function(address, kind, options)
  options = readOptions(kind, options)
  return result(native.read(address, kind, options.count or 0, options.length or 256, options.wide or false))
end)

local function checkWrite(kind, value, options)
  options = options or {}
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

constant("apiVersion", API_VERSION, "The API version, an integer. It grows when functions are added; names keep their meaning.")
constant("version", native.version(), "The program version, such as `\"0.1.0\"`.")

do
  local path = "manifold.has"
  local spec = {
    params = { { name = "name", type = "string" } },
    returns = "boolean",
    doc = "Whether the API has a name, such as `\"log.channel\"`, `\"memory\"` or `\"apiVersion\"`. Use it instead of reading a name that may not exist, which raises.",
  }
  local _, params = bind(manifold, path, rootMembers, "has", spec, function(name)
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
  root.functions[#root.functions + 1] = { path = path, name = "has", params = paramText(params), returns = spec.returns, doc = spec.doc }
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

local function cell(text)
  return (tostring(text or ""):gsub("|", "\\|"))
end

local function functionRows(out, functions)
  out[#out + 1] = "| Function | Returns | |"
  out[#out + 1] = "| --- | --- | --- |"
  for _, entry in ipairs(functions) do
    out[#out + 1] = ("| `%s(%s)` | %s | %s |"):format(entry.path, entry.params, cell(entry.returns), cell(entry.doc))
  end
end

local function markdown()
  local out = { INTRO, RULES, "## manifold", "" }
  out[#out + 1] = "| Name | Value | |"
  out[#out + 1] = "| --- | --- | --- |"
  for _, entry in ipairs(root.constants) do
    local value = entry.name == "version" and "string" or tostring(entry.value)
    out[#out + 1] = ("| `%s` | %s | %s |"):format(entry.path, value, cell(entry.doc))
  end
  for _, entry in ipairs(root.functions) do
    out[#out + 1] = ("| `%s(%s)` | %s | %s |"):format(entry.path, entry.params, cell(entry.returns), cell(entry.doc))
  end
  for _, section in ipairs(areas) do
    out[#out + 1] = ""
    out[#out + 1] = "## " .. section.path
    out[#out + 1] = ""
    out[#out + 1] = section.summary
    out[#out + 1] = ""
    functionRows(out, section.functions)
    for _, object in ipairs(section.objects) do
      out[#out + 1] = ""
      out[#out + 1] = "### " .. object.title
      out[#out + 1] = ""
      out[#out + 1] = object.summary
      out[#out + 1] = ""
      out[#out + 1] = "| Name | Returns | |"
      out[#out + 1] = "| --- | --- | --- |"
      for _, row in ipairs(object.rows) do
        out[#out + 1] = ("| `%s` | %s | %s |"):format(row.signature, cell(row.returns), cell(row.doc))
      end
    end
  end
  out[#out + 1] = ""
  return table.concat(out, "\n")
end

return docs, markdown
