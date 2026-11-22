-- Cheat Engine's .NET data collector, on top of manifold.dotnet. Cheat Engine
-- names a class by its module handle and TypeDef token; here those are the
-- handles and tokens manifold.dotnet.classes reports. The collector loads the
-- .NET runtime on first use. Methods take the collector as an optional first
-- argument, so both dc.enumDomains() and dc:enumDomains() work.

local collector
local warnedAllObjects = false

local function dotnet()
  local api = manifold.dotnet
  if not api.isLoaded() and not api.load() then
    return nil
  end
  return api
end

local byToken
local generation

-- The classes by "module:token", rebuilt when the class list changes. The
-- process id and the generation (one per reading of the classes) tell a
-- restarted game from the one before, whose module and count are the same.
local function classByToken(api, module, token)
  local status = api.status()
  local key = tostring(status.processId) .. ":" .. tostring(status.generation) .. ":" .. tostring(status.module) .. ":" .. tostring(status.classes)
  if not byToken or generation ~= key then
    byToken = {}
    for _, class in ipairs(api.classes() or {}) do
      if class.token ~= 0 then
        byToken[class.module .. ":" .. class.token] = class
      end
    end
    generation = key
  end
  return byToken[tostring(module) .. ":" .. tostring(token)]
end

local function instanceFields(api, name)
  local fields = {}
  local seen = 0
  while name and name ~= "" and seen < 48 do
    local class = api.class(name)
    if not class then break end
    for _, field in ipairs(class.fields) do
      fields[#fields + 1] = { Offset = field.offset, FieldType = field.typeCode, Name = field.name, FieldTypeClassName = field.type, IsStatic = false }
    end
    name = class.parent
    seen = seen + 1
  end
  table.sort(fields, function(a, b) return a.Offset < b.Offset end)
  return fields
end

local function layout(api, address, start, className)
  local pointer = api.status().pointerSize or 8
  local data = { StartAddress = start, ClassName = className, ObjectType = 0x12, ElementType = 0, ElementSize = 0, CountOffset = 0, FirstElementOffset = 0 }
  local object = api.object(start)
  if object and object.kind == "array" then
    data.ObjectType = 0x1D
    data.ElementType = object.elementType
    data.ElementSize = object.elementSize
    data.CountOffset = pointer
    data.FirstElementOffset = object.data - start
  elseif object and object.kind == "string" then
    data.ObjectType = 0x0E
    data.ElementType = 0x03
    data.ElementSize = 2
    data.CountOffset = pointer
    data.FirstElementOffset = pointer + 4
  end
  data.Fields = instanceFields(api, className)
  return data
end

local function method(name, body)
  return function(first, ...)
    if first == collector then
      return body(...)
    end
    return body(first, ...)
  end
end

collector = {
  enumDomains = method("enumDomains", function()
    if not dotnet() then return {} end
    return { { DomainHandle = 1, Name = "DefaultDomain" } }
  end),

  enumModuleList = method("enumModuleList", function()
    local api = dotnet()
    if not api then return {} end
    local list = {}
    for _, module in ipairs(api.modules() or {}) do
      list[#list + 1] = { ModuleHandle = module.handle, BaseAddress = module.base, Name = module.path ~= "" and module.path or module.name }
    end
    return list
  end),

  enumTypeDefs = method("enumTypeDefs", function(moduleHandle)
    local api = dotnet()
    if not api then return {} end
    local list = {}
    for _, class in ipairs(api.classes() or {}) do
      if class.module == moduleHandle and class.token ~= 0 then
        list[#list + 1] = { TypeDefToken = class.token, Name = class.name, Flags = 0, Extends = 0 }
      end
    end
    return list
  end),

  getTypeDefMethods = method("getTypeDefMethods", function(moduleHandle, token)
    local api = dotnet()
    local class = api and classByToken(api, moduleHandle, token)
    local details = class and api.class(class.name)
    if not details then return {} end
    local list = {}
    for _, m in ipairs(details.methods) do
      list[#list + 1] = { Name = m.name, MethodToken = m.token, Attributes = m.static and 0x10 or 0, ImplementationFlags = 0, ILCode = 0,
        NativeCode = m.address or 0, SecondaryNativeCode = {} }
    end
    return list
  end),

  getMethodParameters = method("getMethodParameters", function(moduleHandle, methodToken)
    local api = dotnet()
    if not api then return {} end
    for _, class in ipairs(api.classes() or {}) do
      if class.module == moduleHandle then
        local details = api.class(class.name)
        for _, m in ipairs(details and details.methods or {}) do
          if m.token == methodToken then
            local list = {}
            for i = 1, m.params do
              list[i] = { Name = "arg" .. i, CType = 0 }
            end
            return list
          end
        end
      end
    end
    return {}
  end),

  getTypeDefParent = method("getTypeDefParent", function(moduleHandle, token)
    local api = dotnet()
    local class = api and classByToken(api, moduleHandle, token)
    local details = class and api.class(class.name)
    local parent = details and details.parent ~= "" and api.class(details.parent)
    if not parent then return nil end
    return { ModuleHandle = parent.module, TypedefToken = parent.token }
  end),

  getTypeDefData = method("getTypeDefData", function(moduleHandle, token)
    local api = dotnet()
    local class = api and classByToken(api, moduleHandle, token)
    if not class then return nil end
    return { ClassName = class.name, ObjectType = 0x12, ElementType = 0, ElementSize = 0, CountOffset = 0, FirstElementOffset = 0,
      Fields = instanceFields(api, class.name) }
  end),

  getAddressData = method("getAddressData", function(address)
    local api = dotnet()
    local found = api and api.classAt(address)
    if not found then return nil end
    return layout(api, address, found.address, found.class)
  end),

  enumAllObjectsOfType = method("enumAllObjectsOfType", function(moduleHandle, token)
    local api = dotnet()
    local class = api and classByToken(api, moduleHandle, token)
    if not class then return {} end
    return api.instances(class.handle) or {}
  end),

  enumAllObjects = method("enumAllObjects", function()
    -- Every object of a game is millions of entries; the counts per type and
    -- the objects of one type are what manifold.dotnet.heap and instances give.
    if not warnedAllObjects then
      warnedAllObjects = true
      manifold.log.warning("enumAllObjects is not built in: use manifold.dotnet.heap() for the counts per type and enumAllObjectsOfType or manifold.dotnet.instances for the objects of one type")
    end
    return nil
  end),
}

rawset(_G, "getDotNetDataCollector", function()
  return collector
end)
