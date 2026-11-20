local manifold = manifold
local rtti = manifold.rtti

local RTTI = {}
RTTI.VERSION = "3.0.0"

local function ready()
  if not manifold.process.isOpen() then
    error("no process is opened", 3)
  end
end

local function toAddress(value)
  if math.type(value) == "integer" then
    return value
  end
  if type(value) == "number" then
    return math.tointeger(math.floor(value))
  end
  if type(value) == "string" and value ~= "" then
    local address = manifold.process.address(value)
    if address and address ~= 0 then
      return address
    end
  end
  return nil
end

function RTTI.demangle(decorated)
  if type(decorated) ~= "string" then
    return nil
  end
  return rtti.demangle(decorated)
end

function RTTI.mangle(name)
  if type(name) ~= "string" then
    return nil
  end
  return rtti.mangle(name)
end

function RTTI.className(address)
  ready()
  local at = toAddress(address)
  return at and (rtti.className(at)) or nil
end

function RTTI.object(address)
  ready()
  local at = toAddress(address)
  return at and (rtti.object(at)) or nil
end

function RTTI.owner(address, window)
  ready()
  local at = toAddress(address)
  if not at then
    return nil
  end
  return (rtti.owner(at, math.type(window) == "integer" and window or nil))
end

function RTTI.whois(address)
  ready()
  local at = toAddress(address)
  return at and (rtti.whois(at)) or nil
end

function RTTI.bases(what)
  ready()
  if type(what) ~= "string" then
    what = toAddress(what)
  end
  return what and (rtti.bases(what)) or nil
end

function RTTI.vtable(name, base)
  ready()
  if type(name) ~= "string" then
    return nil
  end
  return (rtti.vtable(name, type(base) == "string" and base or nil))
end

function RTTI.vtables(name)
  ready()
  if type(name) ~= "string" then
    return nil
  end
  return (rtti.vtables(name))
end

function RTTI.instances(name, limit)
  ready()
  if type(name) ~= "string" then
    return nil
  end
  local count = type(limit) == "number" and math.tointeger(math.floor(limit)) or nil
  return (rtti.instances(name, count))
end

function RTTI.dissect(address)
  ready()
  local at = toAddress(address)
  local ok = at and rtti.dissect(at)
  if not ok then
    error(string.format("no object with run-time type information at %X", at or 0), 2)
  end
  return true
end

function RTTI.label(structure, address)
  ready()
  local at = toAddress(address)
  return at and (rtti.label(structure, at)) or 0
end

function RTTI.classes()
  ready()
  return (rtti.classes())
end

function RTTI.status()
  return rtti.status().text
end

function RTTI.reset()
  rtti.reset()
end

rawset(_G, "ManifoldRTTI", RTTI)

manifold.doc.registerModule("ManifoldRTTI", RTTI, { doc = "The names of the Manifold-RTTI autorun script, kept for tables and scripts written against it. The run-time type information is built into the program now: these functions call manifold.rtti and return what the script returned (nil where it found nothing, an error without a process)." })
