local lcl = __manifoldLcl
local D = lcl.data
local new, construct, classes, Impl = lcl.new, lcl.construct, lcl.classes, lcl.impl

local function define(name, fn)
  if rawget(_G, name) == nil then
    rawset(_G, name, fn)
  end
end

local function fill(list, items)
  list.clear()
  for _, text in ipairs(items) do
    list.add(text)
  end
  return list
end

define("getProcesslist", function(list)
  local processes = getProcessList()
  if list == nil then
    return processes
  end
  local ids = {}
  for pid in pairs(processes) do
    ids[#ids + 1] = pid
  end
  table.sort(ids)
  local items = {}
  for i, pid in ipairs(ids) do
    items[i] = string.format("%08X-%s", pid, processes[pid])
  end
  return fill(list, items)
end)

define("getWindowlist", function(list)
  local windows = __manifoldWindowList()
  if list == nil then
    local result = {}
    for _, window in ipairs(windows) do
      local captions = result[window.pid] or {}
      captions[#captions + 1] = window.caption
      result[window.pid] = captions
    end
    return result
  end
  local items = {}
  for i, window in ipairs(windows) do
    items[i] = string.format("%08X-%s", window.pid, window.caption)
  end
  return fill(list, items)
end)

define("getThreadlist", function(list)
  local threads = __manifoldThreadList()
  if list == nil then
    return threads
  end
  local items = {}
  for i, id in ipairs(threads) do
    items[i] = string.format("%X", id)
  end
  return fill(list, items)
end)

define("readShortInteger", function(address, signed)
  local value = readByte(address)
  if value and signed and value >= 0x80 then
    value = value - 0x100
  end
  return value
end)

define("writeShortInteger", function(address, value)
  return writeByte(address, value)
end)

local refs, nextRef = {}, 0
define("createRef", function(...)
  nextRef = nextRef + 1
  refs[nextRef] = table.pack(...)
  return nextRef
end)
define("getRef", function(id)
  local values = refs[id]
  if values then
    return table.unpack(values, 1, values.n)
  end
end)
define("destroyRef", function(id)
  refs[id] = nil
end)

define("setGlobalVariable", function(name, value)
  rawset(_G, name, value)
end)
define("getGlobalVariable", function(name)
  return rawget(_G, name)
end)

define("getNextAllocNumber", function(script)
  local text = lcl.isObject(script) and script.Text or tostring(script or "")
  local highest
  for number in text:lower():gmatch("alloc%s*%(%s*newmem(%d*)") do
    local value = tonumber(number) or 1
    if highest == nil or value > highest then
      highest = value
    end
  end
  return highest and highest + 1 or 0
end)

define("translateID", function(id, original)
  if original ~= nil then
    return original
  end
  return id
end)

define("loadModule", function(path)
  return injectDLL(path)
end)

define("loadTable", function(source, merge)
  if merge then
    return __manifoldLoadTable(source, true)
  end
  local path, temporary = __manifoldStageTable(source)
  if not path then
    return nil, temporary
  end
  createTimer(1, function()
    __manifoldLoadTable(path, false, temporary)
  end)
  return true
end)

define("createThreadSuspended", function(fn, ...)
  local thread = createThread(function(self, ...)
    while D[self] and D[self].suspended and not self.Terminated do
      sleep(10)
    end
    if not self.Terminated then
      return fn(self, ...)
    end
  end, ...)
  if D[thread] then
    D[thread].suspended = true
  end
  return thread
end)

define("createThreadNewState", function(script)
  local chunk, problem = load(script, "=new state", "t")
  if not chunk then
    error(problem, 2)
  end
  return createThread(function(self)
    return chunk(self)
  end)
end)

local function current()
  return (coroutine.running())
end

local function waitUntil(test, timeout)
  if test() then
    return true
  end
  if not __manifoldCanWait() then
    return false
  end
  timeout = tonumber(timeout)
  local deadline = timeout and timeout >= 0 and timeout < 0xFFFFFFFF and getTickCount() + timeout or nil
  repeat
    sleep(1)
    if test() then
      return true
    end
  until deadline and getTickCount() >= deadline
  return false
end

Impl.criticalsection = {
  enter = function(self)
    local d, me = D[self], current()
    waitUntil(function() return d.holder == nil or d.holder == me end)
    d.holder = me
    d.depth = (d.depth or 0) + 1
  end,
  tryenter = function(self)
    local d, me = D[self], current()
    if d.holder ~= nil and d.holder ~= me then
      return false
    end
    d.holder = me
    d.depth = (d.depth or 0) + 1
    return true
  end,
  leave = function(self)
    local d = D[self]
    if (d.depth or 0) > 0 then
      d.depth = d.depth - 1
      if d.depth == 0 then
        d.holder = nil
      end
    end
  end,
}
define("createCriticalSection", function()
  return new("criticalsection")
end)

Impl.event = {
  setevent = function(self) D[self].signaled = true end,
  resetevent = function(self) D[self].signaled = false end,
  waitfor = function(self, timeout)
    local d = D[self]
    if waitUntil(function() return d.signaled end, timeout) then
      if not d.manualReset then
        d.signaled = false
      end
      return wrSignaled or 0
    end
    return wrTimeout or 1
  end,
}
define("createEvent", function(manualReset, initialState)
  local event = new("event")
  D[event].manualReset = manualReset and true or false
  D[event].signaled = initialState and true or false
  return event
end)

Impl.semaphore = {
  acquire = function(self)
    local d = D[self]
    waitUntil(function() return d.count > 0 end)
    d.count = d.count - 1
  end,
  release = function(self, count)
    local d = D[self]
    d.count = d.count + (tonumber(count) or 1)
  end,
}
define("createSemaphore", function(count)
  local semaphore = new("semaphore")
  D[semaphore].count = tonumber(count) or 1
  return semaphore
end)

Impl.multireadexclusivewritesynchronizer = {
  beginread = function(self)
    local d, me = D[self], current()
    waitUntil(function() return d.writer == nil or d.writer == me end)
    d.readers = (d.readers or 0) + 1
  end,
  endread = function(self)
    local d = D[self]
    d.readers = math.max((d.readers or 0) - 1, 0)
  end,
  beginwrite = function(self)
    local d, me = D[self], current()
    waitUntil(function() return (d.writer == nil or d.writer == me) and (d.readers or 0) == 0 end)
    d.writer = me
    d.writes = (d.writes or 0) + 1
    return true
  end,
  endwrite = function(self)
    local d = D[self]
    d.writes = (d.writes or 1) - 1
    if d.writes <= 0 then
      d.writer, d.writes = nil, 0
    end
  end,
}
define("createMultiReadExclusiveWriteSynchronizer", function()
  return new("multireadexclusivewritesynchronizer")
end)

local function decode(address)
  if type(address) == "string" then
    address = getAddress(address)
  end
  local text = disassemble(address)
  local _, opcode, bytes = splitDisassembledString(text)
  local mnemonic, parameters = tostring(opcode or ""):match("^%s*(%S*)%s*(.-)%s*$")
  local m = (mnemonic or ""):lower()
  local byteTable = {}
  for hex in tostring(bytes or ""):gmatch("%x%x") do
    byteTable[#byteTable + 1] = tonumber(hex, 16)
  end
  local isJump = m:sub(1, 1) == "j" or m:sub(1, 4) == "loop"
  local data = {
    address = address,
    opcode = mnemonic or "",
    parameters = parameters or "",
    description = "",
    commentsoverride = "",
    bytes = byteTable,
    modrmValueType = dvtNone or 0,
    modrmValue = 0,
    parameterValueType = dvtNone or 0,
    parameterValue = 0,
    isJump = isJump,
    isCall = m == "call",
    isRet = m:sub(1, 3) == "ret",
    isRep = m:sub(1, 3) == "rep",
    isConditionalJump = (isJump and m ~= "jmp"),
  }
  if (isJump or data.isCall) and parameters ~= "" then
    local target = getAddressSafe(parameters)
    if target then
      data.parameterValue = target
      data.parameterValueType = dvtAddress or 1
    end
  end
  return text, data
end

Impl.disassembler = {
  disassemble = function(self, address)
    local text, data = decode(address)
    D[self].props.lastdisassembledata = data
    return text
  end,
  getlastdisassembledata = function(self)
    return D[self].props.lastdisassembledata or {}
  end,
  decodelastparameterstostring = function()
    return ""
  end,
}
define("createDisassembler", function()
  return new("disassembler")
end)
local defaultDisassembler
define("getDefaultDisassembler", function()
  defaultDisassembler = defaultDisassembler or new("disassembler")
  return defaultDisassembler
end)
local visibleDisassembler
define("getVisibleDisassembler", function()
  visibleDisassembler = visibleDisassembler or new("disassembler")
  return visibleDisassembler
end)

define("createMemoryView", function()
  return getMemoryViewForm()
end)

define("createAutoAssemblerForm", function(script)
  __manifoldOpenAutoAssembler(script or "")
  return new("tfrmautoinject")
end)

define("createCalendar", function(owner) return construct("calendar", owner) end)
define("createColorBox", function(owner) return construct("combobox", owner) end)
define("createDiagram", function(owner) return construct("diagram", owner) end)
define("createVirtualStringTree", function(owner) return construct("customcontrol", owner) end)
define("createRegion", function() return new("region") end)

local function classKey(name)
  if lcl.isObject(name) then
    return D[name].class
  end
  local key = tostring(name or ""):lower():gsub("^t", "")
  return key
end

local special = {
  form = function() return createForm(false) end,
  ceform = function() return createForm(false) end,
  stringlist = function() return createStringlist() end,
  memorystream = function() return createMemoryStream() end,
  stringstream = function() return createStringStream("") end,
  timer = function(owner) return createTimer(owner, false) end,
}

define("createClass", function(name)
  local key = classKey(name)
  if special[key] then
    return special[key]()
  end
  if classes[key] then
    return new(key)
  end
  return nil
end)

define("createComponentClass", function(name, owner)
  local key = classKey(name)
  if special[key] then
    return special[key](owner)
  end
  if classes[key] then
    return construct(key, owner)
  end
  return nil
end)

define("getPropertyList", function(class)
  local key = classKey(class)
  local names, seen, visited = {}, {}, {}
  while key and classes[key] and not visited[key] do
    visited[key] = true
    for prop in pairs(classes[key].props or {}) do
      if not seen[prop] then
        seen[prop] = true
        names[#names + 1] = prop
      end
    end
    key = classes[key].parent
  end
  table.sort(names)
  return fill(createStringlist(), names)
end)

local autoAttach, autoAttachTimer
local function autoAttachTick()
  local wanted = D[autoAttach].items
  if #wanted == 0 or getOpenedProcessID() ~= 0 then
    return
  end
  local processes = getProcessList()
  for _, name in ipairs(wanted) do
    local lowered = tostring(name):lower()
    for pid, process in pairs(processes) do
      if tostring(process):lower() == lowered then
        openProcess(pid)
        return
      end
    end
  end
end
define("getAutoAttachList", function()
  if not autoAttach then
    autoAttach = createStringlist()
    autoAttachTimer = createTimer(nil, false)
    autoAttachTimer.Interval = 1000
    autoAttachTimer.OnTimer = autoAttachTick
    D[autoAttach].onChange = function(_, d)
      autoAttachTimer.Enabled = #d.items > 0
    end
  end
  return autoAttach
end)

local settingsForm
define("getSettingsForm", function()
  if not settingsForm then
    settingsForm = new("form")
    D[settingsForm].props.name = "frmSettings"
    D[settingsForm].props.caption = "Settings"
  end
  return settingsForm
end)

local timers = {}
local function standInTimer(index)
  local timer = timers[index]
  if not timer then
    timer = new("timer")
    D[timer].props.enabled = true
    timers[index] = timer
  end
  D[timer].props.interval = select(index, __manifoldTimerIntervals())
  return timer
end
define("getFreezeTimer", function() return standInTimer(1) end)
define("getUpdateTimer", function() return standInTimer(2) end)

local globalMeta = getmetatable(_G) or {}
local previousIndex = globalMeta.__index
local compatStub = __manifoldCompatStub
globalMeta.__index = function(globals, name)
  local value
  if type(previousIndex) == "function" then
    value = previousIndex(globals, name)
  elseif type(previousIndex) == "table" then
    value = previousIndex[name]
  end
  if value == nil and type(name) == "string" then
    value = compatStub(name)
  end
  return value
end
setmetatable(_G, globalMeta)
