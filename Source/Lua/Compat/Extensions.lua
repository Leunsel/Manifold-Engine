if string.split == nil then
  string.split = function(text, separator)
    text = tostring(text)
    if separator == nil or separator == "" then
      return text
    end
    local parts, start = {}, 1
    while true do
      local first, last = string.find(text, separator, start, true)
      if not first then
        parts[#parts + 1] = string.sub(text, start)
        break
      end
      parts[#parts + 1] = string.sub(text, start, first - 1)
      start = last + 1
    end
    return table.unpack(parts)
  end
end

if string.startsWith == nil then
  string.startsWith = function(text, prefix)
    return string.sub(tostring(text), 1, #prefix) == prefix
  end
end

if string.endsWith == nil then
  string.endsWith = function(text, suffix)
    return suffix == "" or string.sub(tostring(text), -#suffix) == suffix
  end
end

-- Pascal's Trim, as Cheat Engine's string.trim: drops every character up to and including space at both ends.
if string.trim == nil then
  string.trim = function(text)
    return (string.match(tostring(text), "^[%z\1- ]*(.-)[%z\1- ]*$"))
  end
end

-- ∑ Cheat Engine 7.x embeds Lua 5.3 without the 5.1 compatibility switches and adds the old names itself at
--   startup (LuaHandler.pas, InitializeLua: "loadstring=load", "unpack=table.unpack", "math.pow=...", and
--   frexp/cosh/sinh/tanh in Pascal). Tables use them, e.g. assert(loadstring(code)); Manifold had none of them
--   (owner report). The same definitions, same behaviour. bit32, table.getn, table.maxn and setfenv/getfenv
--   are absent in Cheat Engine too and stay absent.
if loadstring == nil then
  loadstring = load
end
if unpack == nil then
  unpack = table.unpack
end
if package.loaders == nil then
  package.loaders = package.searchers
end
if string.gfind == nil then
  string.gfind = string.gmatch
end
if math.log10 == nil then
  math.log10 = function(v) return math.log(v, 10) end
end
if math.pow == nil then
  math.pow = function(x, y) return x ^ y end
end
if math.atan2 == nil then
  math.atan2 = math.atan
end
if math.ldexp == nil then
  math.ldexp = function(x, exp) return x * 2.0 ^ exp end
end
if math.mod == nil then
  math.mod = math.fmod
end
if math.cosh == nil then
  math.cosh = function(x)
    local e = math.exp(x)
    return (e + 1 / e) / 2
  end
end
if math.sinh == nil then
  math.sinh = function(x)
    local e = math.exp(x)
    return (e - 1 / e) / 2
  end
end
if math.tanh == nil then
  math.tanh = function(x)
    if x > 20 then return 1.0 end
    if x < -20 then return -1.0 end
    local e = math.exp(2 * x)
    return (e - 1) / (e + 1)
  end
end
if math.frexp == nil then
  -- x = m * 2^e with 0.5 <= |m| < 1, as C's frexp; 0, infinities and NaN come back with exponent 0.
  math.frexp = function(x)
    x = x + 0.0
    if x == 0 or x ~= x or x == math.huge or x == -math.huge then
      return x, 0
    end
    local e = math.floor(math.log(math.abs(x), 2)) + 1
    local half = e // 2
    local m = x * 2.0 ^ -half * 2.0 ^ -(e - half)
    if math.abs(m) >= 1 then
      m, e = m / 2, e + 1
    elseif math.abs(m) < 0.5 then
      m, e = m * 2, e - 1
    end
    return m, math.tointeger(e) or e
  end
end

-- module() and package.seeall, which Cheat Engine's Lua DLL keeps (built with LUA_COMPAT_MODULE) and Lua 5.4
-- removed: module(name, ...) makes or reuses the module table, runs the options on it and makes it the
-- environment (_ENV) of the chunk that called it, as Lua 5.3's ll_module does.
if package.seeall == nil then
  package.seeall = function(module)
    local meta = getmetatable(module)
    if meta == nil then
      meta = {}
      setmetatable(module, meta)
    end
    meta.__index = _G
    return module
  end
end
if module == nil and debug ~= nil and debug.getinfo and debug.setupvalue then
  local debug = debug
  module = function(name, ...)
    local loaded = package.loaded
    local result = loaded[name]
    if type(result) ~= "table" then
      result = _G
      for part in string.gmatch(name, "[^%.]+") do
        local next = rawget(result, part)
        if next == nil then
          next = {}
          rawset(result, part, next)
        elseif type(next) ~= "table" then
          error("name conflict for module '" .. name .. "'", 2)
        end
        result = next
      end
      loaded[name] = result
    end
    if result._NAME == nil then
      result._M = result
      result._NAME = name
      result._PACKAGE = string.match(name, "^(.*%.)") or ""
    end
    local caller = debug.getinfo(2, "f")
    if caller and caller.func then
      for i = 1, math.huge do
        local upvalue = debug.getupvalue(caller.func, i)
        if upvalue == nil then
          break
        end
        if upvalue == "_ENV" then
          debug.setupvalue(caller.func, i, result)
          break
        end
      end
    end
    for _, option in ipairs({ ... }) do
      if type(option) == "function" then
        option(result)
      end
    end
    return result
  end
end
