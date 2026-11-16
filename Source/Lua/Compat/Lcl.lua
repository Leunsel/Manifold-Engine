local classes = __ceClasses or {}
__ceClasses = nil

local lower = string.lower
local D = setmetatable({}, { __mode = "k" })
local Meta = {}
local Impl, Get, Set, Defaults, IndexGet, IndexSet, Init, Indexed = {}, {}, {}, {}, {}, {}, {}, {}
local forms = {}
local formNotifications = {}

local function touchMenus()
  local f = rawget(_G, "__manifoldMenusChanged")
  if f then
    f()
  end
end

local function touchForms()
  local f = rawget(_G, "__manifoldFormsChanged")
  if f then
    f()
  end
end

local layout = {}

local ExtraParents = {
  customcontrol = "wincontrol", customapplication = "component", tobject = "object", custompen = "object",
  custombrush = "object", customfont = "object", canvashelper = "object", customcanvas = "object",
  custombitmap = "rasterimage", handlestream = "stream", stream = "object", lclcomponent = "component",
  persistent = "object", graphicsobject = "object", phantom = "panel", mainform = "form",
  tfrmluaengine = "form", memoryview = "form", cefrm = "form", png = "bitmap", jpeg = "bitmap", icon = "bitmap",
  synedit = "memo", headercontrol = "panel", thread = "object", scrollbox = "scrollingwincontrol",
  pagecontrol = "wincontrol", tabsheet = "wincontrol", statusbar = "wincontrol", constraints = "object",
  borderspacing = "object", childsizing = "object", anchorside = "object", gutter = "object", point = "object",
}

local ClassNames = {
  object = "TObject", component = "TComponent", control = "TControl", wincontrol = "TWinControl", form = "TForm", cefrm = "TCEForm",
  mainform = "TMainForm", label = "TLabel", edit = "TEdit", memo = "TMemo", button = "TButton", checkbox = "TCheckBox",
  togglebox = "TToggleBox", combobox = "TComboBox", listbox = "TListBox", listview = "TListView", treeview = "TTreeView",
  panel = "TPanel", groupbox = "TGroupBox", radiogroup = "TRadioGroup", image = "TImage", paintbox = "TPaintBox",
  splitter = "TSplitter", progressbar = "TProgressBar", trackbar = "TTrackBar", mainmenu = "TMainMenu", popupmenu = "TPopupMenu",
  menuitem = "TMenuItem", imagelist = "TImageList", bitmap = "TBitmap", png = "TPortableNetworkGraphic", jpeg = "TJPEGImage",
  icon = "TIcon", picture = "TPicture", stringlist = "TStringList", strings = "TStrings", font = "TFont", canvas = "TCanvas",
  brush = "TBrush", pen = "TPen", opendialog = "TOpenDialog", savedialog = "TSaveDialog", selectdirectorydialog = "TSelectDirectoryDialog",
  colordialog = "TColorDialog", fontdialog = "TFontDialog", finddialog = "TFindDialog", listitems = "TListItems", listitem = "TListItem",
  listcolumns = "TListColumns", listcolumn = "TListColumn", treenodes = "TTreeNodes", treenode = "TTreeNode", application = "TApplication",
  thread = "TThread", phantom = "TControl", tfrmluaengine = "TfrmLuaEngine", memoryview = "TMemoryBrowser",
  disassemblerview = "TDisassemblerview", hexadecimal = "THexView", addresslist = "TAddresslist", synedit = "TSynEdit",
  headercontrol = "THeaderControl", scrollbox = "TScrollBox", pagecontrol = "TPageControl", tabsheet = "TTabSheet", statusbar = "TStatusBar",
}

local ObjectTypes = {
  font = "font", strings = "stringlist", stringlist = "stringlist", tstrings = "stringlist", canvas = "canvas", brush = "brush", pen = "pen",
  picture = "picture", listitems = "listitems", listcolumns = "listcolumns", treenodes = "treenodes", controlscrollbar = "controlscrollbar",
  bitmap = "bitmap", graphic = "bitmap", rasterimage = "bitmap", headersections = "headersections", constraints = "constraints",
  borderspacing = "borderspacing", gutter = "gutter", point = "point",
}

local Scalars = {
  string = "", integer = 0, number = 0, boolean = false, cardinal = 0, int64 = 0, word = 0, byte = 0, double = 0, single = 0,
  float = 0, ptruint = 0, qword = 0, dword = 0, longint = 0, smallint = 0, real = 0, int = 0, bool = false, ["string;"] = "",
  ["boolean;"] = false, ["integer;"] = 0, pointer = 0, rgbinteger = 0, modalresult = 0, imageindex = -1, height = 0, shortcut = "",
  alignmentoption = 0, alignment = 0, formstate = 0, panelbevel = 0, scrollstyle = 0, pixelformat = 0, checkboxstate = 0,
  duplicatestype = 0, displaytype = 0, troundingtype = 0, tfastscanmethod = 0, tscanoption = 0, scantype = 0, valuetype = 0,
  tvariabletype = 0, enum = 0, set = "[]", tscanregionpreference = 0, disassemblervaluetype = 0, colordefinition = 0x20000000,
}

local lineageCache = {}

local function parentOf(name)
  local info = classes[name]
  if info and info.parent and info.parent ~= "" and not info.parent:find(":") then
    return info.parent
  end
  return ExtraParents[name]
end

local function lineage(name)
  local cached = lineageCache[name]
  if cached then
    return cached
  end
  local list, seen, current = {}, {}, name
  while current and not seen[current] do
    seen[current] = true
    list[#list + 1] = current
    current = parentOf(current)
  end
  if not seen.object then
    list[#list + 1] = "object"
  end
  lineageCache[name] = list
  return list
end

local function isObject(value)
  return type(value) == "table" and D[value] ~= nil
end

local function inherits(value, name)
  local d = type(value) == "table" and D[value]
  if not d then
    return false
  end
  for _, class in ipairs(lineage(d.class)) do
    if class == name then
      return true
    end
  end
  return false
end

local function bind(obj, fn)
  return function(first, ...)
    if rawequal(first, obj) then
      return fn(obj, ...)
    end
    return fn(obj, first, ...)
  end
end

local function call(fn, ...)
  if type(fn) == "function" then
    return fn(...)
  end
end

local function removeValue(list, value)
  for i = #list, 1, -1 do
    if rawequal(list[i], value) then
      table.remove(list, i)
      return i
    end
  end
end

local serial = 0

local function new(class, owner)
  local obj = setmetatable({}, Meta)
  serial = serial + 1
  local d = { class = class, props = {}, components = {}, controls = {}, serial = serial }
  D[obj] = d
  if isObject(owner) then
    d.owner = owner
    local list = D[owner].components
    list[#list + 1] = obj
  end
  local chain = lineage(class)
  for i = #chain, 1, -1 do
    local init = Init[chain[i]]
    if init then
      init(obj, d)
    end
  end
  if layout.created then
    layout.created(obj, d)
  end
  return obj
end

local function setParent(obj, parent)
  local d = D[obj]
  local old = d.parent
  if old and D[old] then
    removeValue(D[old].controls, obj)
  end
  d.parent = nil
  if isObject(parent) then
    d.parent = parent
    local list = D[parent].controls
    list[#list + 1] = obj
  end
  if layout.parentChanged and old ~= d.parent then
    layout.parentChanged(obj, old, d.parent)
  end
  touchForms()
end

local function findOwned(obj, name, deep)
  local d = D[obj]
  if not d then
    return nil
  end
  for _, child in ipairs(d.components) do
    local childName = D[child] and D[child].props.name
    if childName and lower(childName) == name then
      return child
    end
  end
  for _, child in ipairs(d.controls) do
    local childName = D[child] and D[child].props.name
    if childName and lower(childName) == name then
      return child
    end
  end
  if deep then
    for _, child in ipairs(d.components) do
      local found = findOwned(child, name, true)
      if found then
        return found
      end
    end
  end
  return nil
end

local noop = function() end
local genericCache = {}

local function generic(k)
  local cached = genericCache[k]
  if cached then
    return cached
  end
  local fn = noop
  local prefix = k:sub(1, 3)
  if #k > 3 and prefix == "get" then
    local prop = k:sub(4)
    fn = function(self, index)
      if index ~= nil then
        local holder = self[prop]
        if type(holder) == "table" then
          return holder[index]
        end
      end
      return self[prop]
    end
  elseif #k > 3 and prefix == "set" then
    local prop = k:sub(4)
    fn = function(self, a, b)
      if b ~= nil then
        local holder = self[prop]
        if type(holder) == "table" then
          holder[a] = b
          return
        end
      end
      self[prop] = a
    end
  elseif #k > 2 and k:sub(1, 2) == "is" then
    local prop = k:sub(3)
    fn = function(self)
      return self[prop] and true or false
    end
  end
  genericCache[k] = fn
  return fn
end

local knownMethods = {}
for _, info in pairs(classes) do
  for name in pairs(info.methods or {}) do
    knownMethods[name] = true
  end
end

local function indexer(obj, name, handler)
  return setmetatable({}, {
    __index = function(_, i)
      return handler.get(obj, D[obj], i)
    end,
    __newindex = function(_, i, v)
      if handler.set then
        handler.set(obj, D[obj], i, v)
      end
    end,
    __len = function()
      return handler.len and handler.len(obj, D[obj]) or 0
    end,
  })
end

local function typedDefault(obj, d, k, kind)
  local scalar = Scalars[kind]
  if scalar ~= nil then
    return scalar
  end
  if kind:find("color") then
    return 0x20000000
  end
  local objectClass = ObjectTypes[kind]
  if objectClass then
    local value = new(objectClass, obj)
    d.props[k] = value
    return value
  end
  return nil
end

Meta.__index = function(obj, key)
  local d = D[obj]
  if d == nil then
    return nil
  end
  local chain = lineage(d.class)
  if type(key) == "number" then
    for i = 1, #chain do
      local f = IndexGet[chain[i]]
      if f then
        return f(obj, d, key)
      end
    end
    return nil
  end
  if type(key) ~= "string" then
    return nil
  end
  local k = lower(key)
  local value = d.props[k]
  if value ~= nil then
    return value
  end
  for i = 1, #chain do
    local class = chain[i]
    local getter = Get[class] and Get[class][k]
    if getter then
      return getter(obj, d)
    end
    local method = Impl[class] and Impl[class][k]
    if method then
      return bind(obj, method)
    end
    local defaults = Defaults[class]
    if defaults then
      local default = defaults[k]
      if default ~= nil then
        if type(default) == "function" then
          local made = default(obj, d)
          d.props[k] = made
          return made
        end
        return default
      end
    end
    local handlers = Indexed[class] and Indexed[class][k]
    if handlers then
      return indexer(obj, k, handlers)
    end
  end
  local child = findOwned(obj, k, false)
  if child then
    return child
  end
  for i = 1, #chain do
    local info = classes[chain[i]]
    if info then
      local kind = info.props[k]
      if kind then
        return typedDefault(obj, d, k, kind)
      end
      if info.indexed[k] then
        return indexer(obj, k, { get = function() return nil end })
      end
    end
  end
  for i = 1, #chain do
    local info = classes[chain[i]]
    if info and info.methods[k] then
      return bind(obj, generic(k))
    end
  end
  if knownMethods[k] then
    return bind(obj, generic(k))
  end
  if k:sub(1, 2) == "on" then
    return nil
  end
  if key:match("^%u") then
    local phantom = new("phantom", obj)
    D[phantom].props.name = key
    d.props[k] = phantom
    return phantom
  end
  if #k > 3 and (k:sub(1, 3) == "get" or k:sub(1, 3) == "set") then
    return bind(obj, generic(k))
  end
  return nil
end

Meta.__newindex = function(obj, key, value)
  local d = D[obj]
  if d == nil then
    return
  end
  if d.class == "menuitem" then
    touchMenus()
  end
  touchForms()
  local chain = lineage(d.class)
  if type(key) == "number" then
    for i = 1, #chain do
      local f = IndexSet[chain[i]]
      if f then
        return f(obj, d, key, value)
      end
    end
    return
  end
  local k = lower(tostring(key))
  local handled = false
  for i = 1, #chain do
    local setter = Set[chain[i]] and Set[chain[i]][k]
    if setter then
      setter(obj, d, value)
      handled = true
      break
    end
  end
  if not handled then
    d.props[k] = value
  end
  if layout.changed then
    layout.changed(obj, d, k)
  end
end

Meta.__tostring = function(obj)
  local d = D[obj]
  return string.format("%s: %016X", ClassNames[d and d.class or "object"] or "TObject", 0x10000000 + (d and d.serial or 0) * 16)
end

Meta.__concat = function(a, b)
  return tostring(a) .. tostring(b)
end

local function className(d)
  return ClassNames[d.class] or ("T" .. d.class:sub(1, 1):upper() .. d.class:sub(2))
end

local function destroy(obj)
  local d = D[obj]
  if not d or d.destroyed then
    return
  end
  call(d.props.ondestroy, obj)
  d.destroyed = true
  if d.class == "canvas" and layout.canvasFreed then
    layout.canvasFreed(d)
  end
  for i = #d.components, 1, -1 do
    destroy(d.components[i])
  end
  if d.owner and D[d.owner] then
    removeValue(D[d.owner].components, obj)
  end
  if d.parent and D[d.parent] then
    removeValue(D[d.parent].controls, obj)
    if layout.parentChanged then
      layout.parentChanged(obj, d.parent, nil)
    end
  end
  if d.parentItem and D[d.parentItem] then
    removeValue(D[d.parentItem].items, obj)
    touchMenus()
  end
  removeValue(forms, obj)
  touchForms()
end

Impl.object = {
  getclassname = function(self) return className(D[self]) end,
  destroy = destroy,
  free = destroy,
  fieldaddress = function() return 0 end,
  methodaddress = function() return 0 end,
  methodname = function() return "" end,
  inheritsfrom = function(self, name) return inherits(self, lower(tostring(name)):gsub("^t", "")) end,
}
Get.object = {
  classname = function(self, d) return className(d) end,
}

Get.component = {
  componentcount = function(self, d) return #d.components end,
  owner = function(self, d) return d.owner end,
}
Indexed.component = {
  component = {
    get = function(self, d, i) return d.components[(tonumber(i) or -1) + 1] end,
    len = function(self, d) return #d.components end,
  },
  componentbyname = {
    get = function(self, d, name) return findOwned(self, lower(tostring(name)), false) end,
  },
}
Impl.component = {
  getcomponentcount = function(self) return #D[self].components end,
  getcomponent = function(self, i) return D[self].components[(tonumber(i) or -1) + 1] end,
  findcomponentbyname = function(self, name) return findOwned(self, lower(tostring(name or "")), false) end,
  getowner = function(self) return D[self].owner end,
}
Defaults.component = { name = "", tag = 0 }

local function controlText(self, d)
  local text = d.props.caption
  if text == nil then
    text = d.props.text
  end
  return text or ""
end

Get.control = {
  parent = function(self, d) return d.parent end,
  caption = controlText,
  text = controlText,
  clientwidth = function(self, d) return d.hostW or d.props.width or 100 end,
  clientheight = function(self, d) return d.hostH or d.props.height or 100 end,
  boundsrect = function(self, d)
    local left, top = d.props.left or 0, d.props.top or 0
    return { Left = left, Top = top, Right = left + (d.props.width or 100), Bottom = top + (d.props.height or 100) }
  end,
}
Set.control = {
  parent = function(self, d, value) setParent(self, value) end,
  caption = function(self, d, value) d.props.caption = value; d.props.text = nil end,
  text = function(self, d, value) d.props.caption = value; d.props.text = nil end,
}
Defaults.control = {
  visible = true, enabled = true, left = 0, top = 0, width = 100, height = 100, color = 0x20000000, align = 0, autosize = false,
  showhint = false, hint = "", cursor = 0, parentcolor = false, parentfont = true, parentshowhint = true, doublebuffered = false,
  borderwidth = 0, bevelouter = 0, bevelinner = 0, bevelwidth = 1, bevelcolor = 0x20000000, borderstyle = 0, transparent = false,
  alignment = 0, layout = 0, wordwrap = false, anchors = "[akTop,akLeft]", tabstop = true, taborder = 0, dragmode = 0,
  constraints = function(self) return new("constraints", self) end,
  borderspacing = function(self) return new("borderspacing", self) end,
  font = function(self) return new("font", self) end,
}
Impl.control = {
  getposition = function(self) local d = D[self] return d.props.left or 0, d.props.top or 0 end,
  setposition = function(self, x, y) local d = D[self] d.props.left, d.props.top = x, y if layout.boundsChanged then layout.boundsChanged(self, d) end end,
  getsize = function(self) local d = D[self] return d.props.width or 100, d.props.height or 100 end,
  setsize = function(self, w, h) local d = D[self] d.props.width, d.props.height = w, h if layout.boundsChanged then layout.boundsChanged(self, d) end end,
  doclick = function(self) return call(D[self].props.onclick, self) end,
  click = function(self) return call(D[self].props.onclick, self) end,
  clienttoscreen = function(self, x, y) return x or 0, y or 0 end,
  screentoclient = function(self, x, y) return x or 0, y or 0 end,
  canfocus = function() return true end,
  focused = function() return false end,
  show = function(self) D[self].props.visible = true if layout.changed then layout.changed(self, D[self], "visible") end end,
  hide = function(self) D[self].props.visible = false if layout.changed then layout.changed(self, D[self], "visible") end end,
  setbounds = function(self, l, t, w, h) local d = D[self] local p = d.props p.left, p.top, p.width, p.height = l, t, w, h if layout.boundsChanged then layout.boundsChanged(self, d) end end,
}

Get.wincontrol = {
  controlcount = function(self, d) return #d.controls end,
}
Indexed.wincontrol = {
  control = {
    get = function(self, d, i) return d.controls[(tonumber(i) or -1) + 1] end,
    len = function(self, d) return #d.controls end,
  },
}
Impl.wincontrol = {
  getcontrolcount = function(self) return #D[self].controls end,
  getcontrol = function(self, i) return D[self].controls[(tonumber(i) or -1) + 1] end,
  getcontrolatpos = function() return nil end,
}
Defaults.wincontrol = {
  canvas = function(self) return new("canvas", self) end,
  handle = 0,
}

local function splitLines(text)
  local items = {}
  text = tostring(text or "")
  if text == "" then
    return items
  end
  local position = 1
  while position <= #text do
    local a, b = text:find("\r?\n", position)
    if not a then
      a, b = text:find("\r", position)
    end
    if not a then
      items[#items + 1] = text:sub(position)
      break
    end
    items[#items + 1] = text:sub(position, a - 1)
    position = b + 1
  end
  return items
end

local function stringsChanged(self, d)
  touchForms()
  if d.onChange then
    d.onChange(self, d)
  end
  call(d.props.onchange, self)
end

Init.strings = function(self, d)
  d.items = {}
  d.data = {}
end
Get.strings = {
  count = function(self, d) return #d.items end,
  text = function(self, d)
    if #d.items == 0 then
      return ""
    end
    return table.concat(d.items, "\r\n") .. "\r\n"
  end,
  commatext = function(self, d) return table.concat(d.items, ",") end,
  linebreak = function() return "\r\n" end,
}
Set.strings = {
  text = function(self, d, value)
    d.items = splitLines(value)
    d.data = {}
    stringsChanged(self, d)
  end,
  commatext = function(self, d, value)
    d.items = {}
    for part in tostring(value or ""):gmatch("[^,]+") do
      d.items[#d.items + 1] = part
    end
    stringsChanged(self, d)
  end,
}
IndexGet.strings = function(self, d, i) return d.items[i + 1] end
IndexSet.strings = function(self, d, i, v)
  if i >= 0 and i < #d.items then
    d.items[i + 1] = tostring(v)
    stringsChanged(self, d)
  end
end
Indexed.strings = {
  string = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, set = function(self, d, i, v) IndexSet.strings(self, d, i, v) end, len = function(self, d) return #d.items end },
  strings = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, set = function(self, d, i, v) IndexSet.strings(self, d, i, v) end, len = function(self, d) return #d.items end },
  data = { get = function(self, d, i) return d.data[(tonumber(i) or -1) + 1] end, set = function(self, d, i, v) d.data[i + 1] = v end },
  objects = { get = function(self, d, i) return d.data[(tonumber(i) or -1) + 1] end, set = function(self, d, i, v) d.data[i + 1] = v end },
  values = {
    get = function(self, d, name)
      for _, line in ipairs(d.items) do
        local k, v = line:match("^([^=]*)=(.*)$")
        if k == name then
          return v
        end
      end
      return ""
    end,
    set = function(self, d, name, value)
      for i, line in ipairs(d.items) do
        if line:match("^([^=]*)=") == name then
          d.items[i] = name .. "=" .. tostring(value)
          return
        end
      end
      d.items[#d.items + 1] = name .. "=" .. tostring(value)
    end,
  },
  names = { get = function(self, d, i) local line = d.items[(tonumber(i) or -1) + 1] return line and line:match("^([^=]*)=") or "" end },
}
Impl.strings = {
  add = function(self, text)
    local d = D[self]
    d.items[#d.items + 1] = tostring(text == nil and "" or text)
    stringsChanged(self, d)
    return #d.items - 1
  end,
  addobject = function(self, text, data)
    local d = D[self]
    d.items[#d.items + 1] = tostring(text == nil and "" or text)
    d.data[#d.items] = data
    stringsChanged(self, d)
    return #d.items - 1
  end,
  addtext = function(self, text)
    local d = D[self]
    for _, line in ipairs(splitLines(text)) do
      d.items[#d.items + 1] = line
    end
    stringsChanged(self, d)
  end,
  addstrings = function(self, other)
    local d = D[self]
    local source = D[other]
    if source and source.items then
      for _, line in ipairs(source.items) do
        d.items[#d.items + 1] = line
      end
    elseif type(other) == "table" then
      for _, line in ipairs(other) do
        d.items[#d.items + 1] = tostring(line)
      end
    end
    stringsChanged(self, d)
  end,
  assign = function(self, other)
    local d = D[self]
    local source = D[other]
    d.items = {}
    d.data = {}
    if source and source.items then
      for i, line in ipairs(source.items) do
        d.items[i] = line
      end
    end
    stringsChanged(self, d)
  end,
  clear = function(self)
    local d = D[self]
    d.items = {}
    d.data = {}
    stringsChanged(self, d)
  end,
  delete = function(self, index)
    local d = D[self]
    index = tonumber(index) or -1
    if index >= 0 and index < #d.items then
      table.remove(d.items, index + 1)
      table.remove(d.data, index + 1)
      stringsChanged(self, d)
    end
  end,
  insert = function(self, index, text)
    local d = D[self]
    index = math.max(0, math.min(tonumber(index) or 0, #d.items))
    table.insert(d.items, index + 1, tostring(text))
    stringsChanged(self, d)
  end,
  remove = function(self, text)
    local d = D[self]
    for i, line in ipairs(d.items) do
      if line == text then
        table.remove(d.items, i)
        stringsChanged(self, d)
        return i - 1
      end
    end
    return -1
  end,
  indexof = function(self, text)
    for i, line in ipairs(D[self].items) do
      if line == text then
        return i - 1
      end
    end
    return -1
  end,
  indexofname = function(self, name)
    for i, line in ipairs(D[self].items) do
      if line:match("^([^=]*)=") == name then
        return i - 1
      end
    end
    return -1
  end,
  exchange = function(self, a, b)
    local items = D[self].items
    items[a + 1], items[b + 1] = items[b + 1], items[a + 1]
  end,
  sort = function(self) table.sort(D[self].items) end,
  gettext = function(self) return self.Text end,
  settext = function(self, text) self.Text = text end,
  getcount = function(self) return #D[self].items end,
  getstring = function(self, i) return D[self].items[(tonumber(i) or -1) + 1] end,
  setstring = function(self, i, v) IndexSet.strings(self, D[self], i, v) end,
  beginupdate = noop,
  endupdate = noop,
  loadfromfile = function(self, path)
    local file = io.open(path, "rb")
    if not file then
      error("Unable to open file \"" .. tostring(path) .. "\"")
    end
    local text = file:read("a")
    file:close()
    if text:sub(1, 3) == "\239\187\191" then
      text = text:sub(4)
    end
    self.Text = text
  end,
  savetofile = function(self, path)
    local file = io.open(path, "wb")
    if not file then
      error("Unable to create file \"" .. tostring(path) .. "\"")
    end
    file:write(self.Text)
    file:close()
  end,
}
ExtraParents.stringlist = "strings"
Defaults.stringlist = { sorted = false, duplicates = 0, casesensitive = false }

Defaults.memo = {
  lines = function(self, d)
    local lines = new("stringlist", self)
    D[lines].onChange = function()
      call(D[self].props.onchange, self)
    end
    return lines
  end,
  scrollbars = 0,
  readonly = false,
  wordwrap = true,
  wantreturns = true,
  wanttabs = false,
}
Get.memo = {
  text = function(self, d)
    local text = self.Lines.Text
    if text:sub(-2) == "\r\n" then
      text = text:sub(1, -3)
    end
    return text
  end,
  caption = function(self, d) return Get.memo.text(self, d) end,
}
Set.memo = {
  text = function(self, d, value) self.Lines.Text = value end,
  caption = function(self, d, value) self.Lines.Text = value end,
}
Impl.memo = {
  append = function(self, text) self.Lines.add(text) end,
  clear = function(self) self.Lines.clear() end,
  clearselection = noop,
  selectall = noop,
  copytoclipboard = noop,
  pastefromclipboard = noop,
}

Get.edit = {
  text = function(self, d) return d.props.text or "" end,
  caption = function(self, d) return d.props.text or "" end,
}
Set.edit = {
  text = function(self, d, value)
    d.props.text = value == nil and "" or tostring(value)
    call(d.props.onchange, self)
  end,
  caption = function(self, d, value) d.props.text = value == nil and "" or tostring(value) end,
}
Defaults.edit = { selstart = 0, sellength = 0, seltext = "", maxlength = 0, passwordchar = "", readonly = false, numbersonly = false, charcase = 0, texthint = "" }
Impl.edit = {
  clear = function(self) self.Text = "" end,
  selectall = noop,
  copytoclipboard = noop,
  cuttoclipboard = noop,
  pastefromclipboard = noop,
  clearselection = noop,
}

local function listText(self, d)
  local index = d.props.itemindex or -1
  local items = self.Items
  if index >= 0 and items[index] then
    return items[index]
  end
  return d.props.text or ""
end

Defaults.combobox = { items = function(self) return new("stringlist", self) end, itemindex = -1, style = 0, dropdowncount = 8, readonly = false }
Get.combobox = { text = listText, caption = listText }
Set.combobox = {
  text = function(self, d, value)
    d.props.text = tostring(value)
    local found = self.Items.indexOf(tostring(value))
    if found >= 0 then
      d.props.itemindex = found
    end
  end,
  itemindex = function(self, d, value)
    d.props.itemindex = tonumber(value) or -1
    d.props.text = nil
    call(d.props.onchange, self)
    call(d.props.onselect, self)
  end,
}
Impl.combobox = { clear = function(self) self.Items.clear() D[self].props.itemindex = -1 D[self].props.text = "" end }

Defaults.listbox = { items = function(self) return new("stringlist", self) end, itemindex = -1, multiselect = false }
Get.listbox = { text = listText, selcount = function(self, d) return (d.props.itemindex or -1) >= 0 and 1 or 0 end }
Indexed.listbox = {
  selected = {
    get = function(self, d, i) return (d.props.itemindex or -1) == i end,
    set = function(self, d, i, v) if v then d.props.itemindex = i end end,
  },
}
Impl.listbox = { clear = function(self) self.Items.clear() D[self].props.itemindex = -1 end, getitemindex = function(self) return D[self].props.itemindex or -1 end, setitemindex = function(self, v) D[self].props.itemindex = v end }

Defaults.radiogroup = { items = function(self) return new("stringlist", self) end, itemindex = -1, columns = 1 }
Defaults.checkbox = { checked = false, state = 0, allowgrayed = false }
Set.checkbox = {
  checked = function(self, d, value)
    local changed = (d.props.checked or false) ~= (value and true or false)
    d.props.checked = value and true or false
    d.props.state = value and 1 or 0
    if changed then
      call(d.props.onchange, self)
    end
  end,
  state = function(self, d, value)
    d.props.state = value
    d.props.checked = value == 1 or value == "cbChecked"
  end,
}
Defaults.button = { modalresult = 0, default = false, cancel = false }
Defaults.progressbar = { min = 0, max = 100, position = 0, step = 10 }
Impl.progressbar = { stepit = function(self) local d = D[self] d.props.position = (d.props.position or 0) + (d.props.step or 10) end, stepby = function(self, n) local d = D[self] d.props.position = (d.props.position or 0) + (n or 0) end }
Defaults.trackbar = { min = 0, max = 10, position = 0 }
Defaults.image = { picture = function(self) return new("picture", self) end, stretch = false, proportional = false, center = false, transparent = false }
Get.image = { canvas = function(self, d) return self.Picture.Bitmap.Canvas end }
Defaults.paintbox = { canvas = function(self) return new("canvas", self) end }
Impl.paintbox = { repaint = function(self) end }
Defaults.phantom = { canvas = function(self) return new("canvas", self) end, items = function(self) return new("stringlist", self) end }
Defaults.splitter = { resizestyle = 0, minsize = 30 }
Defaults.groupbox = {}
Defaults.panel = { bevelouter = 2, fullrepaint = true }

Defaults.font = { name = "Segoe UI", size = 9, height = -12, color = 0x20000000, style = "[]", charset = 1, quality = 0, pitch = 0, orientation = 0 }
Impl.font = { assign = function(self, other) local source = D[other] if source then for k, v in pairs(source.props) do D[self].props[k] = v end end end, isequal = function() return false end }
Defaults.brush = { color = 0xFFFFFF, style = 0 }
Defaults.gutter = { color = 0x20000000, width = 30, visible = true, autosize = true }
Defaults.point = { x = 0, y = 0 }
Defaults.borderspacing = { around = 0, left = 0, top = 0, right = 0, bottom = 0, innerborder = 0, cellalignhorizontal = 0, cellalignvertical = 0 }
Defaults.constraints = { minwidth = 0, minheight = 0, maxwidth = 0, maxheight = 0 }
Defaults.pen = { color = 0, width = 1, style = 0, mode = 0 }
Defaults.canvas = {
  pen = function(self) return new("pen", self) end,
  brush = function(self) return new("brush", self) end,
  font = function(self) return new("font", self) end,
  penposition = 0,
}
Get.canvas = {
  width = function(self, d) local owner = d.owner and D[d.owner] return owner and owner.props.width or 100 end,
  height = function(self, d) local owner = d.owner and D[d.owner] return owner and owner.props.height or 100 end,
}
local function canvasFont(self)
  local font = self.Font
  local size = tonumber(font.Size) or 9
  if size <= 0 then
    size = 9
  end
  local name = lower(tostring(font.Name or ""))
  local mono = name:find("consolas") ~= nil or name:find("courier") ~= nil or name:find("mono") ~= nil
  return size, lower(tostring(font.Style or "")):find("fsbold") ~= nil, mono
end
local function textWidth(self, text)
  local size, bold, mono = canvasFont(self)
  local native = rawget(_G, "__manifoldTextExtent")
  if native then
    local w = native(tostring(text or ""), size, bold, mono)
    if w then
      return w
    end
  end
  return math.floor(#tostring(text or "") * size * 0.75 + 0.5)
end
local function textHeight(self, text)
  local size, bold, mono = canvasFont(self)
  local native = rawget(_G, "__manifoldTextExtent")
  if native then
    local _, h = native(tostring(text or "Xg"), size, bold, mono)
    if h then
      return h
    end
  end
  return math.floor(size * 1.6 + 0.5)
end
Impl.canvas = {
  textwidth = textWidth,
  gettextwidth = textWidth,
  textheight = textHeight,
  gettextheight = textHeight,
  textextent = function(self, text) return textWidth(self, text), textHeight(self, text) end,
  getpixel = function() return 0 end,
  getpenposition = function() return 0, 0 end,
  handleallocated = function() return true end,
}
Defaults.graphic = { width = 0, height = 0, transparent = false, empty = true, modified = false }
Defaults.bitmap = { canvas = function(self) return new("canvas", self) end, pixelformat = 0, transparentcolor = 0x20000000, transparentmode = 0, handle = 0 }
Impl.bitmap = {
  setsize = function(self, w, h) local p = D[self].props p.width, p.height = w, h end,
  loadfromfile = function(self) D[self].props.empty = false end,
  loadfromstream = function(self) D[self].props.empty = false end,
  savetofile = noop,
  savetostream = noop,
  assign = function(self, other) local source = D[other] if source then D[self].props.width = source.props.width D[self].props.height = source.props.height end end,
  clear = noop,
  freeimage = noop,
}
Defaults.picture = {
  bitmap = function(self) return new("bitmap", self) end,
  png = function(self) return new("png", self) end,
  portablenetworkgraphic = function(self) return new("png", self) end,
  jpeg = function(self) return new("jpeg", self) end,
  icon = function(self) return new("icon", self) end,
  graphic = function(self) return new("bitmap", self) end,
}
Get.picture = {
  width = function(self) return self.Graphic.Width end,
  height = function(self) return self.Graphic.Height end,
}
Impl.picture = { loadfromfile = noop, loadfromstream = noop, savetofile = noop, assign = noop, clear = noop }

Init.imagelist = function(self, d) d.images = {} end
Defaults.imagelist = { width = 16, height = 16, masked = false, bkcolor = 0x1FFFFFFF }
Get.imagelist = { count = function(self, d) return #d.images end }
Impl.imagelist = {
  add = function(self, image) local d = D[self] d.images[#d.images + 1] = image or true return #d.images - 1 end,
  addmasked = function(self, image) local d = D[self] d.images[#d.images + 1] = image or true return #d.images - 1 end,
  addicon = function(self, image) local d = D[self] d.images[#d.images + 1] = image or true return #d.images - 1 end,
  insert = function(self, index, image) table.insert(D[self].images, (index or 0) + 1, image or true) end,
  delete = function(self, index) table.remove(D[self].images, (index or 0) + 1) end,
  clear = function(self) D[self].images = {} end,
  getbitmap = noop,
  draw = noop,
  replace = noop,
}

Init.menuitem = function(self, d) d.items = {} end
Defaults.menuitem = { caption = "", shortcut = "", imageindex = -1, checked = false, radioitem = false, autocheck = false, default = false, visible = true, enabled = true, groupindex = 0, hint = "" }
Get.menuitem = {
  count = function(self, d) return #d.items end,
  parent = function(self, d) return d.parentItem end,
  menuindex = function(self, d)
    local parent = d.parentItem and D[d.parentItem]
    if not parent then
      return -1
    end
    for i, item in ipairs(parent.items) do
      if rawequal(item, self) then
        return i - 1
      end
    end
    return -1
  end,
  menu = function(self, d)
    local current = self
    while D[current] and D[current].parentItem do
      current = D[current].parentItem
    end
    return D[current] and D[current].menu
  end,
}
Set.menuitem = {
  menuindex = function(self, d, value)
    local parent = d.parentItem and D[d.parentItem]
    if not parent then
      return
    end
    removeValue(parent.items, self)
    table.insert(parent.items, math.max(1, math.min((tonumber(value) or 0) + 1, #parent.items + 1)), self)
  end,
}
IndexGet.menuitem = function(self, d, i) return d.items[i + 1] end
Indexed.menuitem = {
  item = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.items end },
  items = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.items end },
}
local function attachItem(parent, item, index)
  local d = D[parent]
  local child = D[item]
  if not child then
    return
  end
  touchMenus()
  if child.parentItem and D[child.parentItem] then
    removeValue(D[child.parentItem].items, item)
  end
  child.parentItem = parent
  if index then
    table.insert(d.items, math.max(1, math.min(index + 1, #d.items + 1)), item)
  else
    d.items[#d.items + 1] = item
  end
end
Impl.menuitem = {
  add = function(self, item) attachItem(self, item) end,
  insert = function(self, index, item) attachItem(self, item, tonumber(index) or 0) end,
  delete = function(self, index)
    local d = D[self]
    local item = d.items[(tonumber(index) or -1) + 1]
    if item then
      table.remove(d.items, index + 1)
      D[item].parentItem = nil
      touchMenus()
    end
  end,
  remove = function(self, item)
    removeValue(D[self].items, item)
    if D[item] then
      D[item].parentItem = nil
    end
    touchMenus()
  end,
  clear = function(self)
    for _, item in ipairs(D[self].items) do
      D[item].parentItem = nil
    end
    D[self].items = {}
    touchMenus()
  end,
  indexof = function(self, item)
    for i, child in ipairs(D[self].items) do
      if rawequal(child, item) then
        return i - 1
      end
    end
    return -1
  end,
  find = function(self, caption)
    local wanted = tostring(caption or ""):gsub("&", "")
    for _, child in ipairs(D[self].items) do
      if tostring(child.Caption or ""):gsub("&", "") == wanted then
        return child
      end
    end
    return nil
  end,
  getitem = function(self, i) return D[self].items[(tonumber(i) or -1) + 1] end,
  getcount = function(self) return #D[self].items end,
  doclick = function(self) return call(D[self].props.onclick, self) end,
  click = function(self) return call(D[self].props.onclick, self) end,
}
Defaults.menu = {
  items = function(self)
    local root = new("menuitem", self)
    D[root].menu = self
    return root
  end,
  images = nil,
}
Get.menu = { images = function(self, d) return d.props.images end }
Impl.menu = { getitems = function(self) return self.Items end }
Impl.popupmenu = { popup = noop, close = noop }

Init.listitems = function(self, d) d.items = {} end
local function newListItem(owner)
  local item = new("listitem", owner)
  D[item].listOwner = owner
  return item
end
Get.listitems = { count = function(self, d) return #d.items end }
Set.listitems = {
  count = function(self, d, value)
    value = tonumber(value) or 0
    while #d.items < value do
      d.items[#d.items + 1] = newListItem(self)
    end
    while #d.items > value do
      table.remove(d.items)
    end
  end,
}
IndexGet.listitems = function(self, d, i) return d.items[i + 1] end
Indexed.listitems = { item = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.items end } }
Impl.listitems = {
  add = function(self) touchForms() local d = D[self] local item = newListItem(self) d.items[#d.items + 1] = item return item end,
  insert = function(self, index) touchForms() local d = D[self] local item = newListItem(self) table.insert(d.items, math.max(1, math.min((index or 0) + 1, #d.items + 1)), item) return item end,
  clear = function(self) touchForms() D[self].items = {} end,
  delete = function(self, index) touchForms() table.remove(D[self].items, (tonumber(index) or -1) + 1) end,
  beginupdate = noop,
  endupdate = noop,
  getitem = function(self, i) return D[self].items[(tonumber(i) or -1) + 1] end,
  getcount = function(self) return #D[self].items end,
}
Defaults.listitem = {
  caption = "",
  subitems = function(self) return new("stringlist", self) end,
  imageindex = -1,
  stateindex = -1,
  selected = false,
  checked = false,
  data = 0,
  focused = false,
}
Get.listitem = {
  index = function(self, d)
    local owner = d.listOwner and D[d.listOwner]
    if owner then
      for i, item in ipairs(owner.items) do
        if rawequal(item, self) then
          return i - 1
        end
      end
    end
    return -1
  end,
  owner = function(self, d) return d.listOwner end,
}
Impl.listitem = {
  delete = function(self) local d = D[self] if d.listOwner then removeValue(D[d.listOwner].items, self) end end,
  makevisible = noop,
  displayrect = function() return { Left = 0, Top = 0, Right = 0, Bottom = 0 } end,
}

Init.listcolumns = function(self, d) d.items = {} end
Get.listcolumns = { count = function(self, d) return #d.items end }
IndexGet.listcolumns = function(self, d, i) return d.items[i + 1] end
Impl.listcolumns = {
  add = function(self) local d = D[self] local column = new("listcolumn", self) d.items[#d.items + 1] = column return column end,
  clear = function(self) D[self].items = {} end,
  delete = function(self, index) table.remove(D[self].items, (tonumber(index) or -1) + 1) end,
  getcount = function(self) return #D[self].items end,
  getcolumn = function(self, i) return D[self].items[(tonumber(i) or -1) + 1] end,
  beginupdate = noop,
  endupdate = noop,
}
Defaults.listcolumn = { caption = "", width = 50, autosize = false, alignment = 0, visible = true, minwidth = 0, maxwidth = 0, imageindex = -1, tag = 0 }

Defaults.listview = {
  items = function(self) return new("listitems", self) end,
  columns = function(self) return new("listcolumns", self) end,
  itemindex = -1,
  viewstyle = 0,
  rowselect = false,
  readonly = false,
  multiselect = false,
  checkboxes = false,
  hideselection = true,
  gridlines = false,
  ownerdata = false,
  showcolumnheaders = true,
}
Get.listview = {
  selected = function(self, d)
    local index = d.props.itemindex or -1
    return index >= 0 and self.Items[index] or nil
  end,
  selcount = function(self, d) return (d.props.itemindex or -1) >= 0 and 1 or 0 end,
}
Set.listview = {
  selected = function(self, d, item)
    d.props.itemindex = item and item.Index or -1
  end,
}
Impl.listview = {
  clear = function(self) touchForms() self.Items.clear() D[self].props.itemindex = -1 end,
  getitemat = function() return nil end,
  beginupdate = noop,
  endupdate = noop,
}

Init.treenodes = function(self, d) d.nodes = {} d.roots = {} end
local function newNode(nodes, parent, text)
  touchForms()
  local node = new("treenode", nodes)
  local nd = D[node]
  nd.tree = nodes
  nd.children = {}
  nd.parentNode = parent
  nd.props.text = tostring(text or "")
  local list = parent and D[parent].children or D[nodes].roots
  list[#list + 1] = node
  local all = D[nodes].nodes
  all[#all + 1] = node
  return node
end
local function removeNode(node)
  local nd = D[node]
  if not nd then
    return
  end
  touchForms()
  local tree = nd.tree and D[nd.tree] and D[nd.tree].owner
  if tree and D[tree] and rawequal(D[tree].props.selected, node) then
    D[tree].props.selected = nil
  end
  for i = #nd.children, 1, -1 do
    removeNode(nd.children[i])
  end
  local tree = D[nd.tree]
  removeValue(tree.nodes, node)
  removeValue(nd.parentNode and D[nd.parentNode].children or tree.roots, node)
end
Get.treenodes = { count = function(self, d) return #d.nodes end }
IndexGet.treenodes = function(self, d, i) return d.nodes[i + 1] end
Indexed.treenodes = { item = { get = function(self, d, i) return d.nodes[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.nodes end } }
local function nodeArguments(first, second)
  if isObject(first) then
    return first, second
  end
  if first ~= nil and second == nil then
    return nil, first
  end
  return nil, second
end
local function insertNode(nodes, sibling, text, after)
  local node = newNode(nodes, sibling and D[sibling] and D[sibling].parentNode or nil, text)
  if sibling and D[sibling] then
    local nd = D[node]
    local list = nd.parentNode and D[nd.parentNode].children or D[nodes].roots
    removeValue(list, node)
    local at = #list + 1
    for i, other in ipairs(list) do
      if rawequal(other, sibling) then
        at = after and i + 1 or i
        break
      end
    end
    table.insert(list, at, node)
  end
  return node
end
Impl.treenodes = {
  add = function(self, a, b)
    local sibling, text = nodeArguments(a, b)
    local parent = sibling and D[sibling] and D[sibling].parentNode or nil
    return newNode(self, parent, text)
  end,
  addchild = function(self, parent, text) return newNode(self, isObject(parent) and parent or nil, text) end,
  addfirst = function(self, a, b)
    local sibling, text = nodeArguments(a, b)
    return Impl.treenodes.add(self, sibling, text)
  end,
  addchildfirst = function(self, parent, text)
    local node = newNode(self, isObject(parent) and parent or nil, text)
    local nd = D[node]
    local list = nd.parentNode and D[nd.parentNode].children or D[self].roots
    removeValue(list, node)
    table.insert(list, 1, node)
    return node
  end,
  insert = function(self, sibling, text) return insertNode(self, isObject(sibling) and sibling or nil, text, false) end,
  insertbehind = function(self, sibling, text) return insertNode(self, isObject(sibling) and sibling or nil, text, true) end,
  clear = function(self)
    local d = D[self]
    d.nodes = {}
    d.roots = {}
    local tree = d.owner
    if tree and D[tree] then
      D[tree].props.selected = nil
    end
    touchForms()
  end,
  delete = function(self, node) removeNode(node) end,
  getfirstnode = function(self) return D[self].roots[1] end,
  getcount = function(self) return #D[self].nodes end,
  getitem = function(self, i) return D[self].nodes[(tonumber(i) or -1) + 1] end,
  beginupdate = noop,
  endupdate = noop,
}
Defaults.treenode = { text = "", data = 0, imageindex = -1, selectedindex = -1, stateindex = -1, expanded = false, selected = false }
Get.treenode = {
  count = function(self, d) return #d.children end,
  parent = function(self, d) return d.parentNode end,
  haschildren = function(self, d) return #d.children > 0 or d.props.haschildren == true end,
  level = function(self, d)
    local level, current = 0, d.parentNode
    while current do
      level = level + 1
      current = D[current].parentNode
    end
    return level
  end,
  index = function(self, d)
    local list = d.parentNode and D[d.parentNode].children or D[d.tree].roots
    for i, node in ipairs(list) do
      if rawequal(node, self) then
        return i - 1
      end
    end
    return -1
  end,
  absoluteindex = function(self, d)
    for i, node in ipairs(D[d.tree].nodes) do
      if rawequal(node, self) then
        return i - 1
      end
    end
    return -1
  end,
  owner = function(self, d) return d.tree end,
}
IndexGet.treenode = function(self, d, i) return d.children[i + 1] end
Indexed.treenode = {
  items = { get = function(self, d, i) return d.children[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.children end },
  item = { get = function(self, d, i) return d.children[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.children end },
}
local function sibling(self, delta)
  local d = D[self]
  local list = d.parentNode and D[d.parentNode].children or D[d.tree].roots
  for i, node in ipairs(list) do
    if rawequal(node, self) then
      return list[i + delta]
    end
  end
end
Impl.treenode = {
  add = function(self, text) return newNode(D[self].tree, self, text) end,
  addchild = function(self, text) return newNode(D[self].tree, self, text) end,
  delete = function(self) removeNode(self) end,
  deletechildren = function(self) local d = D[self] for i = #d.children, 1, -1 do removeNode(d.children[i]) end end,
  expand = function(self, recursive)
    local function open(node, deep)
      D[node].props.expanded = true
      if deep then
        for _, child in ipairs(D[node].children) do
          open(child, true)
        end
      end
    end
    open(self, recursive ~= false)
    touchForms()
  end,
  collapse = function(self, recursive)
    local function close(node, deep)
      D[node].props.expanded = false
      if deep then
        for _, child in ipairs(D[node].children) do
          close(child, true)
        end
      end
    end
    close(self, recursive ~= false)
    touchForms()
  end,
  makevisible = noop,
  getfirstchild = function(self) return D[self].children[1] end,
  getlastchild = function(self) local c = D[self].children return c[#c] end,
  getnextsibling = function(self) return sibling(self, 1) end,
  getprevsibling = function(self) return sibling(self, -1) end,
  getnext = function(self) return D[self].children[1] or sibling(self, 1) end,
  displayrect = function() return { Left = 0, Top = 0, Right = 0, Bottom = 0 } end,
  getitem = function(self, i) return D[self].children[(tonumber(i) or -1) + 1] end,
  getcount = function(self) return #D[self].children end,
  getparent = function(self) return D[self].parentNode end,
}
Defaults.treeview = { items = function(self) return new("treenodes", self) end, readonly = false, multiselect = false, rightclickselect = false, hideselection = true, showroot = true, showlines = true }
Impl.treeview = {
  clear = function(self) self.Items.clear() D[self].props.selected = nil end,
  fullexpand = noop,
  fullcollapse = noop,
  getnodeat = function() return nil end,
  beginupdate = noop,
  endupdate = noop,
}

local function addForm(form)
  forms[#forms + 1] = form
  for _, entry in pairs(formNotifications) do
    call(entry, form)
  end
end

Defaults.form = {
  visible = false, caption = "", position = 0, borderstyle = 2, formstyle = 0, windowstate = 0, modalresult = 0, alphablend = false,
  alphablendvalue = 255, keypreview = false, showintaskbar = 0, designtimeppi = 96, width = 400, height = 300, popupparent = nil,
}
Set.form = {
  visible = function(self, d, value)
    d.props.visible = value and true or false
    touchForms()
  end,
}
Impl.form = {
  show = function(self)
    local d = D[self]
    if d.props.visible then
      d.raise = (d.raise or 0) + 1
    end
    d.props.visible = true
    touchForms()
    call(d.props.onshow, self)
  end,
  hide = function(self)
    D[self].props.visible = false
    touchForms()
  end,
  close = function(self)
    local d = D[self]
    local action = call(d.props.onclose, self)
    d.props.visible = false
    touchForms()
    if action == caFree then
      destroy(self)
    end
  end,
  showmodal = function(self)
    local d = D[self]
    call(d.props.onshow, self)
    if __manifoldHeadless then
      return d.props.modalresult ~= 0 and d.props.modalresult or mrCancel
    end
    return d.props.modalresult ~= 0 and d.props.modalresult or mrCancel
  end,
  centerscreen = noop,
  bringtofront = function(self)
    local d = D[self]
    d.raise = (d.raise or 0) + 1
    touchForms()
  end,
  sendtoback = noop,
  dragnow = noop,
  setfocus = noop,
  repaint = noop,
  update = noop,
  invalidate = noop,
  refresh = noop,
  loadformposition = function() return false end,
  saveformposition = noop,
  isforegroundwindow = function() return false end,
  registerclosecallback = noop,
  registercreatecallback = noop,
  registerfirstshowcallback = noop,
  fixdpi = noop,
  printtorasterimage = noop,
}

Defaults.filedialog = { filename = "", filter = "", filterindex = 1, initialdir = "", title = "", defaultext = "", options = "[]", files = function(self) return new("stringlist", self) end }
Impl.filedialog = {
  execute = function(self)
    if __manifoldHeadless then
      return false
    end
    local d = D[self]
    local save = d.class == "savedialog"
    local chosen = __manifoldFileDialog(save, self.Title, self.Filter, self.FileName, self.InitialDir, self.DefaultExt)
    if not chosen then
      return false
    end
    d.props.filename = chosen
    self.Files.clear()
    self.Files.add(chosen)
    return true
  end,
}
Impl.selectdirectorydialog = {
  execute = function(self)
    if __manifoldHeadless then
      return false
    end
    local chosen = __manifoldFolderDialog(self.Title ~= "" and self.Title or nil)
    if not chosen then
      return false
    end
    D[self].props.filename = chosen
    return true
  end,
}
Defaults.colordialog = { color = 0, title = "" }
Impl.colordialog = {
  execute = function(self)
    if __manifoldHeadless then
      return false
    end
    local chosen = __manifoldColorDialog(self.Color)
    if not chosen then
      return false
    end
    D[self].props.color = chosen
    return true
  end,
}
Impl.fontdialog = { execute = function() return false end }
Impl.finddialog = { execute = function() return false end, closedialog = noop }
Defaults.fontdialog = { font = function(self) return new("font", self) end }

Defaults.thread = { terminated = false, name = "", freeonterminate = true, finished = false }
Impl.thread = {
  synchronize = function(self, fn, ...) return call(fn, self, ...) end,
  queue = function(self, fn, ...) return call(fn, self, ...) end,
  terminate = function(self) D[self].props.terminated = true end,
  waitfor = noop,
  waittilldone = noop,
  freeonterminate = function(self, value) D[self].props.freeonterminate = value end,
  resume = noop,
  suspend = noop,
}

Defaults.disassemblerview = { selectedaddress2 = 0, topaddress = 0, showjumplines = true, hidefocusrect = false }
Get.disassemblerview = { selectedaddress = function() return __manifoldSelectedCodeAddress() end }
Set.disassemblerview = { selectedaddress = function(self, d, value) __manifoldSetSelectedCodeAddress(value) end }
Defaults.hexadecimal = { displaytype = 0 }
local function hexView(d, address)
  local f = rawget(_G, "__manifoldSetHexView")
  if f then
    f(address or 0, d.selStart or 0, d.selStop or 0)
  end
end
Get.hexadecimal = {
  address = function() return __manifoldSelectedHexAddress() end,
  topaddress = function() return __manifoldSelectedHexAddress() end,
  selectionstart = function(self, d) return d.selStart or 0 end,
  selectionstop = function(self, d) return d.selStop or 0 end,
}
Set.hexadecimal = {
  address = function(self, d, value)
    d.selStart, d.selStop = nil, nil
    hexView(d, tonumber(value))
  end,
  topaddress = function(self, d, value)
    d.selStart, d.selStop = nil, nil
    hexView(d, tonumber(value))
  end,
  selectionstart = function(self, d, value)
    d.selStart = tonumber(value) or 0
    if (d.selStop or 0) < d.selStart then
      d.selStop = d.selStart
    end
    hexView(d, 0)
  end,
  selectionstop = function(self, d, value)
    d.selStop = tonumber(value) or 0
    if (d.selStart or 0) == 0 or d.selStart > d.selStop then
      d.selStart = d.selStop
    end
    hexView(d, 0)
  end,
}

local function construct(class, owner)
  local obj = new(class, owner)
  if isObject(owner) and inherits(owner, "wincontrol") and inherits(obj, "control") then
    setParent(obj, owner)
  end
  return obj
end

local creators = {
  createLabel = "label", createEdit = "edit", createMemo = "memo", createButton = "button", createCheckBox = "checkbox",
  createToggleBox = "togglebox", createComboBox = "combobox", createListBox = "listbox", createListView = "listview",
  createTreeView = "treeview", createPanel = "panel", createGroupBox = "groupbox", createRadioGroup = "radiogroup",
  createImage = "image", createPaintBox = "paintbox", createSplitter = "splitter", createProgressBar = "progressbar",
  createTrackBar = "trackbar", createScrollBox = "scrollbox", createPageControl = "pagecontrol", createTabSheet = "tabsheet",
  createStatusBar = "statusbar", createSynEdit = "synedit", createCECustomButton = "cecustombutton",
}
for name, class in pairs(creators) do
  if rawget(_G, name) == nil then
    _G[name] = function(owner) return construct(class, owner) end
  end
end

local plain = {
  createMainMenu = "mainmenu", createPopupMenu = "popupmenu", createMenuItem = "menuitem", createImageList = "imagelist",
  createPicture = "picture", createFont = "font", createOpenDialog = "opendialog", createSaveDialog = "savedialog",
  createSelectDirectoryDialog = "selectdirectorydialog", createColorDialog = "colordialog", createFontDialog = "fontdialog",
  createFindDialog = "finddialog",
}
for name, class in pairs(plain) do
  if rawget(_G, name) == nil then
    _G[name] = function(owner)
      local obj = new(class, owner)
      if class == "mainmenu" and isObject(owner) then
        D[owner].props.menu = obj
      end
      return obj
    end
  end
end

createStringlist = createStringlist or function() return new("stringlist") end
createStringList = createStringList or createStringlist

local function graphic(class)
  return function(width, height)
    local obj = new(class)
    D[obj].props.width = tonumber(width) or 0
    D[obj].props.height = tonumber(height) or 0
    return obj
  end
end
createBitmap = createBitmap or graphic("bitmap")
createPNG = createPNG or graphic("png")
createJpeg = createJpeg or graphic("jpeg")
createIcon = createIcon or graphic("icon")

createForm = createForm or function(visible)
  local form = new("cefrm")
  D[form].script = true
  if visible == nil or visible then
    D[form].props.visible = true
  end
  addForm(form)
  touchForms()
  return form
end

local mainForm = new("mainform")
do
  local p = D[mainForm].props
  p.name = "MainForm"
  p.caption = "Cheat Engine 7.5"
  p.width, p.height = 900, 700
  p.visible = true
  p.onprocessopened = function() end
  local menu = new("mainmenu", mainForm)
  D[menu].props.name = "MainMenu1"
  p.menu = menu
  for _, entry in ipairs({ { "miFile", "&File" }, { "miEdit", "&Edit" }, { "miTable", "&Table" }, { "miD3D", "D3D" }, { "miHelp", "&Help" } }) do
    local item = new("menuitem", mainForm)
    D[item].props.name = entry[1]
    D[item].props.caption = entry[2]
    D[item].builtin = true
    menu.Items.add(item)
  end
  local tableMenu = findOwned(mainForm, "mitable", false)
  local fixed = {}
  for _, entry in ipairs({ { "miShowLuaScript", "Show Cheat Table Lua Script" }, { "miAddFile", "Add file" }, { "miTableSeparator", "-" } }) do
    local item = new("menuitem", mainForm)
    D[item].props.name = entry[1]
    D[item].props.caption = entry[2]
    D[item].builtin = true
    tableMenu.add(item)
    fixed[#fixed + 1] = item
  end
  local fileItems = {}
  D[tableMenu].props.onclick = function(sender)
    for _, item in ipairs(fileItems) do
      tableMenu.remove(item)
      destroy(item)
    end
    fileItems = {}
    for _, name in ipairs(__manifoldTableFileNames()) do
      local item = new("menuitem", mainForm)
      D[item].props.caption = name
      D[item].builtin = true
      tableMenu.add(item)
      fileItems[#fileItems + 1] = item
    end
  end
  for _, entry in ipairs({ { "Foundlist3", "listview" }, { "Panel4", "panel" }, { "Panel5", "panel" }, { "lblSigned", "label" }, { "Splitter1", "splitter" }, { "Panel1", "panel" }, { "btnMemoryView", "button" }, { "ProgressBar", "progressbar" } }) do
    local control = construct(entry[2], mainForm)
    D[control].props.name = entry[1]
  end
end
forms[#forms + 1] = mainForm
MainForm = mainForm

getMainForm = getMainForm or function() return mainForm end

local application = new("application")
do
  local p = D[application].props
  p.title = "Cheat Engine 7.5"
  p.mainform = mainForm
  p.icon = new("icon", application)
  p.exename = ""
end
Impl.application = { processmessages = function() if processMessages then return processMessages() end end, terminate = noop, minimize = noop, restore = noop, bringtofront = noop }
getApplication = getApplication or function() return application end

local luaEngineForm
getLuaEngine = getLuaEngine or function()
  if luaEngineForm then
    return luaEngineForm
  end
  luaEngineForm = new("tfrmluaengine")
  local p = D[luaEngineForm].props
  p.name = "frmLuaEngine"
  p.caption = "Lua Engine"
  local function child(name, class, parent)
    local control = new(class, luaEngineForm)
    setParent(control, parent or luaEngineForm)
    D[control].props.name = name
    return control
  end
  local panel1 = child("Panel1", "panel")
  child("Panel3", "panel")
  child("GroupBox1", "groupbox")
  child("Splitter1", "splitter")
  child("mOutput", "memo", panel1)
  child("mScript", "synedit")
  child("btnExecute", "button")
  local menu = new("mainmenu", luaEngineForm)
  D[luaEngineForm].props.menu = menu
  addForm(luaEngineForm)
  return luaEngineForm
end
createLuaEngine = createLuaEngine or function()
  local form = new("tfrmluaengine")
  local output = construct("memo", form)
  D[output].props.name = "mOutput"
  local script = construct("synedit", form)
  D[script].props.name = "mScript"
  addForm(form)
  return form
end

local memoryView
getMemoryViewForm = getMemoryViewForm or function()
  if memoryView then
    return memoryView
  end
  memoryView = new("memoryview")
  local p = D[memoryView].props
  p.name = "MemoryBrowser"
  p.caption = "Memory Viewer"
  local view = construct("disassemblerview", memoryView)
  D[view].props.name = "DisassemblerView"
  p.disassemblerview = view
  local hex = construct("hexadecimal", memoryView)
  D[hex].props.name = "HexadecimalView"
  p.hexadecimalview = hex
  local menu = new("mainmenu", memoryView)
  p.menu = menu
  local popup = new("popupmenu", memoryView)
  D[popup].props.name = "debuggerpopup"
  local images = new("imagelist", memoryView)
  D[images].props.name = "mvImageList"
  D[popup].props.images = images
  for _, name in ipairs({ "miDebugSetAddress", "Watchmemoryallocations1", "MenuItem14", "InjectDLL1", "CreateThread1", "AutoInject1" }) do
    local item = new("menuitem", memoryView)
    D[item].props.name = name
    D[item].props.caption = name
    D[item].builtin = true
    menu.Items.add(item)
  end
  addForm(memoryView)
  return memoryView
end

Get.memoryview = {
  visible = function()
    local f = rawget(_G, "__manifoldMemoryViewVisible")
    return f and f() or false
  end,
}
Set.memoryview = {
  visible = function(self, d, value)
    if value then
      local f = rawget(_G, "__manifoldShowMemoryView")
      if f then
        f()
      end
    end
  end,
}
Impl.memoryview = {
  show = function(self)
    local f = rawget(_G, "__manifoldShowMemoryView")
    if f then
      f()
    end
    call(D[self].props.onshow, self)
  end,
  bringtofront = function()
    local f = rawget(_G, "__manifoldShowMemoryView")
    if f then
      f()
    end
  end,
  hide = noop,
  close = noop,
}

local uids = setmetatable({}, { __mode = "v" })
local nextUid = 0

local function uidOf(obj)
  local d = D[obj]
  if not d.uid then
    nextUid = nextUid + 1
    d.uid = nextUid
    uids[nextUid] = obj
  end
  return d.uid
end

local function live(id)
  local obj = uids[tonumber(id) or -1]
  local d = obj and D[obj]
  if d and not d.destroyed then
    return obj, d
  end
  return nil
end

local function plainCaption(text)
  return (tostring(text or ""):gsub("&(.)", "%1"))
end

local function shortcutText(value)
  if type(value) == "number" then
    if value == 0 then
      return ""
    end
    local f = rawget(_G, "shortCutToText")
    if f then
      local ok, text = pcall(f, value)
      if ok and type(text) == "string" then
        return text
      end
    end
    return ""
  end
  return tostring(value or "")
end

local function menuEntries(parent, skipBuiltin)
  local out = {}
  local d = D[parent]
  if not d or not d.items then
    return out
  end
  for _, item in ipairs(d.items) do
    local child = D[item]
    if child and not child.destroyed and not (skipBuiltin and child.builtin) and item.Visible ~= false then
      out[#out + 1] = {
        id = uidOf(item),
        caption = plainCaption(item.Caption),
        shortcut = shortcutText(rawget(child.props, "shortcut")),
        enabled = item.Enabled ~= false,
        checked = item.Checked == true,
        children = menuEntries(item, false),
      }
    end
  end
  return out
end

local function scriptMenu(which)
  if type(which) == "number" then
    local menu = live(which)
    return menu
  end
  if which == "main" then
    return D[mainForm].props.menu
  end
  if not memoryView then
    return nil
  end
  if which == "popup" then
    return findOwned(memoryView, "debuggerpopup", false)
  end
  return D[memoryView].props.menu
end

local function mainMenuEntries(menu)
  local out = {}
  local d = D[menu.Items]
  if not d or not d.items then
    return out
  end
  for _, item in ipairs(d.items) do
    local child = D[item]
    if child and not child.destroyed and item.Visible ~= false then
      if child.builtin then
        local added = menuEntries(item, true)
        if #added > 0 then
          local name = tostring(rawget(child.props, "name") or "")
          out[#out + 1] = {
            id = uidOf(item),
            caption = plainCaption(item.Caption),
            merge = (string.lower(name):gsub("^mi", "")),
            enabled = true,
            checked = false,
            children = added,
          }
        end
      else
        out[#out + 1] = {
          id = uidOf(item),
          caption = plainCaption(item.Caption),
          shortcut = shortcutText(rawget(child.props, "shortcut")),
          enabled = item.Enabled ~= false,
          checked = item.Checked == true,
          children = menuEntries(item, false),
        }
      end
    end
  end
  return out
end

__manifoldMenuSnapshot = function(which)
  local menu = scriptMenu(which)
  if not menu then
    return {}
  end
  if which == "main" then
    return mainMenuEntries(menu)
  end
  return menuEntries(menu.Items, which == "menu")
end

__manifoldMenuPopup = function(which)
  local menu = scriptMenu(which)
  if menu then
    call(D[menu].props.onpopup, menu)
  end
end

__manifoldMenuClick = function(id)
  local item, d = live(id)
  if not item then
    return
  end
  if item.AutoCheck then
    item.Checked = not item.Checked
  end
  call(d.props.onclick, item)
end

local alignNames = { alnone = 0, altop = 1, albottom = 2, alleft = 3, alright = 4, alclient = 5, alcustom = 6 }
local bevelNames = { bvnone = 0, bvlowered = 1, bvraised = 2, bvspace = 3 }
local alignmentNames = { taleftjustify = 0, tarightjustify = 1, tacenter = 2 }
local layoutNames = { tltop = 0, tlcenter = 1, tlbottom = 2 }
local drawnKinds = {
  label = true, button = true, edit = true, memo = true, checkbox = true, togglebox = true, listbox = true,
  panel = true, groupbox = true, splitter = true, combobox = true, treeview = true, cecustombutton = true,
  paintbox = true, image = true, listview = true,
}
local defaultSizes = {
  panel = { 170, 50 }, label = { 65, 17 }, edit = { 80, 23 }, memo = { 150, 90 }, button = { 75, 25 },
  cecustombutton = { 75, 25 }, checkbox = { 90, 19 }, togglebox = { 90, 25 }, listbox = { 100, 100 },
  combobox = { 100, 23 }, groupbox = { 185, 105 }, radiogroup = { 185, 105 }, splitter = { 5, 100 },
  treeview = { 121, 97 }, listview = { 250, 150 }, image = { 90, 90 }, paintbox = { 105, 105 },
  progressbar = { 100, 20 }, trackbar = { 100, 25 }, scrollbox = { 150, 150 }, pagecontrol = { 200, 200 },
  tabsheet = { 200, 200 }, statusbar = { 100, 23 }, synedit = { 150, 150 },
}

local function enum(value, names, fallback)
  if type(value) == "number" then
    return value
  end
  if value == nil then
    return fallback
  end
  return names[lower(tostring(value))] or fallback
end

local function num(value)
  return tonumber(value) or 0
end

local function alignOf(control)
  return enum(D[control].props.align, alignNames, 0)
end

local function shown(control)
  local visible = D[control].props.visible
  return visible == nil or visible == true
end

local function centred(value)
  if type(value) == "number" then
    return value >= 4
  end
  return lower(tostring(value or "")):find("center") ~= nil
end

local function colorValue(value)
  local color = tonumber(value)
  if not color or color < 0 or color > 0xFFFFFF then
    return nil
  end
  return color
end

local function fontOf(control)
  local current = control
  while current and D[current] do
    local cd = D[current]
    local font = cd.props.font
    if font and D[font] and (D[font].changed or cd.props.parentfont == false) then
      return D[font].props
    end
    current = cd.parent
  end
  return {}
end

local function fontInfo(control)
  local props = fontOf(control)
  local size = tonumber(props.size) or 9
  if size <= 0 then
    size = 9
  end
  local name = lower(tostring(props.name or ""))
  return {
    size = size,
    bold = lower(tostring(props.style or "")):find("fsbold") ~= nil,
    mono = name:find("consolas") ~= nil or name:find("courier") ~= nil or name:find("mono") ~= nil or name:find("lucida console") ~= nil,
    color = colorValue(props.color),
  }
end

local function measure(control, text)
  local font = fontInfo(control)
  text = tostring(text or "")
  local native = rawget(_G, "__manifoldTextExtent")
  if native then
    local w, h = native(text, font.size, font.bold, font.mono)
    if w then
      return w, h
    end
  end
  local px = font.size * 96 / 72
  local longest, lines = 0, 0
  for line in (text .. "\n"):gmatch("(.-)\r?\n") do
    lines = lines + 1
    longest = math.max(longest, #line)
  end
  return math.floor(longest * px * (font.mono and 0.55 or 0.52) + 0.5), math.floor(math.max(1, lines) * px * 1.25 + 0.5)
end

local function isPanel(control, d)
  return d.class == "panel" or inherits(control, "panel")
end

local function insets(control, d)
  local n = num(d.props.borderwidth)
  if isPanel(control, d) then
    local width = tonumber(control.BevelWidth) or 1
    if enum(control.BevelOuter, bevelNames, 2) ~= 0 then
      n = n + width
    end
    if enum(control.BevelInner, bevelNames, 0) ~= 0 then
      n = n + width
    end
    return n, n, n, n
  end
  if d.class == "groupbox" or d.class == "radiogroup" then
    local _, h = measure(control, "Xg")
    return n + 2, n + h + 2, n + 2, n + 2
  end
  return n, n, n, n
end

local function clientSize(control, d)
  if inherits(control, "form") then
    return d.hostW or tonumber(d.props.clientwidth) or num(d.props.width), d.hostH or tonumber(d.props.clientheight) or num(d.props.height)
  end
  return num(d.props.width), num(d.props.height)
end

local function spacingOf(d)
  local bs = d.props.borderspacing
  local s = bs and D[bs] and D[bs].props or {}
  local around = num(s.around)
  return num(s.left) + around, num(s.top) + around, num(s.right) + around, num(s.bottom) + around
end

local function constrain(d, w, h)
  local cs = d.props.constraints
  local s = cs and D[cs] and D[cs].props
  if s then
    if num(s.maxwidth) > 0 then
      w = math.min(w, num(s.maxwidth))
    end
    if num(s.maxheight) > 0 then
      h = math.min(h, num(s.maxheight))
    end
    w = math.max(w, num(s.minwidth))
    h = math.max(h, num(s.minheight))
  end
  return math.max(0, math.floor(w + 0.5)), math.max(0, math.floor(h + 0.5))
end

local function autoSizeOf(control, d)
  local kind = d.class
  local auto = d.props.autosize
  if auto == nil then
    auto = kind == "label" or kind == "checkbox" or kind == "edit" or kind == "combobox"
  end
  if not auto then
    return nil, nil
  end
  if kind == "label" then
    local w, h = measure(control, controlText(control, d))
    if d.props.wordwrap then
      return nil, h
    end
    return w, h
  end
  if kind == "checkbox" then
    local w, h = measure(control, controlText(control, d))
    return w + 21, math.max(h, 17)
  end
  if kind == "edit" or kind == "combobox" then
    local _, h = measure(control, "Xg")
    return nil, h + 8
  end
  return nil, nil
end

local function insertBefore(c1, c2, align)
  local p1, p2 = D[c1].props, D[c2].props
  local b1, b2 = D[c1].base or {}, D[c2].base or {}
  if align == 1 then
    return num(p1.top) < num(p2.top) or (num(p1.top) == num(p2.top) and num(b1.top) < num(b2.top))
  elseif align == 3 then
    return num(p1.left) < num(p2.left) or (num(p1.left) == num(p2.left) and num(b1.left) < num(b2.left))
  elseif align == 2 then
    local e1, e2 = num(p1.top) + num(p1.height), num(p2.top) + num(p2.height)
    return e1 > e2 or (e1 == e2 and num(b1.top) + num(b1.height) > num(b2.top) + num(b2.height))
  elseif align == 4 then
    local e1, e2 = num(p1.left) + num(p1.width), num(p2.left) + num(p2.width)
    return e1 > e2 or (e1 == e2 and num(b1.left) + num(b1.width) > num(b2.left) + num(b2.width))
  end
  return false
end

local function alignList(parent, pd, align, start)
  local list = {}
  if start and D[start] and rawequal(D[start].parent, parent) and alignOf(start) == align and shown(start) then
    list[1] = start
  end
  for _, control in ipairs(pd.alignOrder or pd.controls) do
    local cd = D[control]
    if not rawequal(control, start) and cd and not cd.destroyed and rawequal(cd.parent, parent) and alignOf(control) == align and shown(control) then
      local at = 1
      while at <= #list and not insertBefore(control, list[at], align) do
        at = at + 1
      end
      table.insert(list, at, control)
    end
  end
  return list
end

local realign
local function place(control, d, align, rect)
  local p = d.props
  local sl, st, sr, sb = spacingOf(d)
  local w, h = num(p.width), num(p.height)
  local aw, ah = autoSizeOf(control, d)
  if align == 1 or align == 2 then
    w = rect.r - rect.l - sl - sr
    h = ah or h
  elseif align == 3 or align == 4 then
    h = rect.b - rect.t - st - sb
    w = aw or w
  else
    w = rect.r - rect.l - sl - sr
    h = rect.b - rect.t - st - sb
  end
  w, h = constrain(d, w, h)
  local l, t
  if align == 1 then
    l, t = rect.l + sl, rect.t + st
    rect.t = t + h + sb
  elseif align == 2 then
    l, t = rect.l + sl, rect.b - sb - h
    rect.b = t - st
  elseif align == 3 then
    l, t = rect.l + sl, rect.t + st
    rect.l = l + w + sr
  elseif align == 4 then
    l, t = rect.r - sr - w, rect.t + st
    rect.r = l - sl
  else
    l, t = rect.l + sl, rect.t + st
  end
  local resized = p.width ~= w or p.height ~= h
  p.left, p.top, p.width, p.height = l, t, w, h
  if resized then
    d.paintDirty = true
    d.resizePending = true
    realign(control, nil)
  end
end

local aligning = setmetatable({}, { __mode = "k" })
realign = function(parent, start)
  local pd = parent and D[parent]
  if not pd or pd.destroyed or aligning[parent] or #pd.controls == 0 then
    return
  end
  aligning[parent] = true
  local cw, ch = clientSize(parent, pd)
  local il, it, ir, ib = 0, 0, 0, 0
  if not inherits(parent, "form") then
    il, it, ir, ib = insets(parent, pd)
  end
  local rect = { l = il, t = it, r = math.max(il, cw - ir), b = math.max(it, ch - ib) }
  for align = 1, 5 do
    for _, control in ipairs(alignList(parent, pd, align, start)) do
      place(control, D[control], align, rect)
    end
  end
  for _, control in ipairs(pd.controls) do
    local cd = D[control]
    if cd and not cd.destroyed and shown(control) then
      local align = alignOf(control)
      if align == 0 or align == 6 then
        local aw, ah = autoSizeOf(control, cd)
        local w, h = constrain(cd, aw or num(cd.props.width), ah or num(cd.props.height))
        if w ~= cd.props.width or h ~= cd.props.height then
          cd.props.width, cd.props.height = w, h
          cd.paintDirty = true
          cd.resizePending = true
          realign(control, nil)
        end
      end
    end
  end
  aligning[parent] = nil
end

function layout.created(obj, d)
  if not inherits(obj, "control") or inherits(obj, "form") then
    return
  end
  local size = defaultSizes[d.class] or { 100, 100 }
  local p = d.props
  p.width = p.width or size[1]
  p.height = p.height or size[2]
  p.left = p.left or 0
  p.top = p.top or 0
  d.base = { left = p.left, top = p.top, width = p.width, height = p.height }
  if d.class == "splitter" and p.align == nil then
    p.align = "alLeft"
  end
end

function layout.parentChanged(obj, old, new)
  if old and D[old] then
    local od = D[old]
    if od.alignOrder then
      removeValue(od.alignOrder, obj)
    end
    realign(old, nil)
  end
  if new and D[new] then
    local nd = D[new]
    nd.alignOrder = nd.alignOrder or {}
    removeValue(nd.alignOrder, obj)
    table.insert(nd.alignOrder, 1, obj)
    realign(new, obj)
  end
end

local boundsKeys = { left = true, top = true, width = true, height = true }
local layoutKeys = {
  align = true, visible = true, autosize = true, caption = true, text = true, borderwidth = true, bevelouter = true,
  bevelinner = true, bevelwidth = true, wordwrap = true, clientwidth = true, clientheight = true, parentfont = true,
}
local ownBoundsKeys = {
  left = true, top = true, width = true, height = true, clientwidth = true, clientheight = true, bevelouter = true,
  bevelinner = true, bevelwidth = true, borderwidth = true,
}

function layout.changed(obj, d, k)
  local class = d.class
  if class == "borderspacing" or class == "constraints" then
    local owner = d.owner
    local od = owner and D[owner]
    if od then
      if od.parent then
        realign(od.parent, nil)
      end
      realign(owner, nil)
    end
    return
  end
  if class == "font" then
    d.changed = true
    local owner = d.owner
    local od = owner and D[owner]
    if od then
      if od.parent then
        realign(od.parent, nil)
      end
      realign(owner, nil)
    end
    return
  end
  if not (boundsKeys[k] or layoutKeys[k]) or not inherits(obj, "control") then
    return
  end
  if boundsKeys[k] then
    d.base = d.base or {}
    d.base[k] = num(d.props[k])
  end
  if k == "align" and d.parent then
    local pd = D[d.parent]
    pd.alignOrder = pd.alignOrder or {}
    removeValue(pd.alignOrder, obj)
    table.insert(pd.alignOrder, 1, obj)
  end
  if d.parent then
    realign(d.parent, obj)
  end
  if ownBoundsKeys[k] and inherits(obj, "wincontrol") then
    realign(obj, nil)
  end
end

function layout.boundsChanged(obj, d)
  d.base = d.base or {}
  for key in pairs(boundsKeys) do
    d.base[key] = num(d.props[key])
  end
  touchForms()
  if d.parent then
    realign(d.parent, obj)
  end
  realign(obj, nil)
end

layout.realign = realign

local penStyles = { pssolid = 0, psdash = 1, psdot = 2, psdashdot = 3, psdashdotdot = 4, psinsideframe = 5, pspattern = 6, psclear = 7 }
local brushStyles = { bssolid = 0, bsclear = 1 }

local function canvasPen(canvas, d)
  local pen = d.props.pen or canvas.Pen
  local p = D[pen].props
  return tonumber(p.color) or 0, math.max(1, tonumber(p.width) or 1), enum(p.style, penStyles, 0) == 7
end

local function canvasBrush(canvas, d)
  local brush = d.props.brush or canvas.Brush
  local p = D[brush].props
  return tonumber(p.color) or 0xFFFFFF, enum(p.style, brushStyles, 0) == 1
end

local function canvasFontInfo(canvas, d)
  local font = d.props.font or canvas.Font
  local p = D[font].props
  local size = tonumber(p.size) or 9
  if size <= 0 then
    size = 9
  end
  local name = lower(tostring(p.name or ""))
  local mono = name:find("consolas") ~= nil or name:find("courier") ~= nil or name:find("mono") ~= nil or name:find("lucida console") ~= nil
  return size, tonumber(p.color) or 0x20000000, lower(tostring(p.style or "")):find("fsbold") ~= nil, mono
end

local function canvasArea(d)
  local owner = d.owner and D[d.owner]
  if not owner then
    return 0, 0
  end
  return num(owner.props.width), num(owner.props.height)
end

local function canvasReset(d)
  local reset = rawget(_G, "__manifoldCanvasReset")
  if reset then
    reset(d.serial)
  end
end

local function canvasShape(canvas, op, x1, y1, x2, y2, usePen, useBrush, rx, ry)
  local native = rawget(_G, "__manifoldCanvasShape")
  if not native then
    return
  end
  local d = D[canvas]
  local pen, width, penClear = 0, 1, true
  if usePen then
    pen, width, penClear = canvasPen(canvas, d)
  end
  local brush, brushClear = 0xFFFFFF, true
  if useBrush then
    brush, brushClear = canvasBrush(canvas, d)
  end
  native(d.serial, op, num(x1), num(y1), num(x2), num(y2), pen, width, penClear, brush, brushClear, num(rx), num(ry))
end

local function pointList(points, ...)
  local flat = {}
  if type(points) == "table" then
    for _, point in ipairs(points) do
      if type(point) == "table" then
        flat[#flat + 1] = num(point.x or point.X or point[1])
        flat[#flat + 1] = num(point.y or point.Y or point[2])
      else
        flat[#flat + 1] = num(point)
      end
    end
  else
    for _, value in ipairs({ points, ... }) do
      flat[#flat + 1] = num(value)
    end
  end
  return flat
end

local function canvasPoly(canvas, closed, points, ...)
  local native = rawget(_G, "__manifoldCanvasPoly")
  if not native then
    return
  end
  local d = D[canvas]
  local pen, width, penClear = canvasPen(canvas, d)
  local brush, brushClear = canvasBrush(canvas, d)
  native(d.serial, closed, pointList(points, ...), pen, width, penClear, brush, closed and brushClear or true)
end

Impl.canvas.line = function(self, x1, y1, x2, y2)
  canvasShape(self, 1, x1, y1, x2, y2, true, false)
  local d = D[self]
  d.penX, d.penY = x2, y2
end
Impl.canvas.moveto = function(self, x, y)
  local d = D[self]
  d.penX, d.penY = x, y
end
Impl.canvas.lineto = function(self, x, y)
  local d = D[self]
  canvasShape(self, 1, d.penX or 0, d.penY or 0, x, y, true, false)
  d.penX, d.penY = x, y
end
Impl.canvas.rect = function(self, x1, y1, x2, y2) canvasShape(self, 2, x1, y1, x2, y2, true, true) end
Impl.canvas.rectangle = Impl.canvas.rect
Impl.canvas.framerect = function(self, x1, y1, x2, y2)
  local native = rawget(_G, "__manifoldCanvasShape")
  if native then
    local d = D[self]
    local brush = canvasBrush(self, d)
    native(d.serial, 2, num(x1), num(y1), num(x2), num(y2), brush, 1, false, 0, true, 0, 0)
  end
end
Impl.canvas.fillrect = function(self, x1, y1, x2, y2)
  local d = D[self]
  local w, h = canvasArea(d)
  if w > 0 and num(x1) <= 0 and num(y1) <= 0 and num(x2) >= w and num(y2) >= h then
    canvasReset(d)
  end
  canvasShape(self, 3, x1, y1, x2, y2, false, true)
end
Impl.canvas.clear = function(self)
  local d = D[self]
  canvasReset(d)
  local w, h = canvasArea(d)
  canvasShape(self, 3, 0, 0, w, h, false, true)
end
Impl.canvas.ellipse = function(self, x1, y1, x2, y2) canvasShape(self, 4, x1, y1, x2, y2, true, true) end
Impl.canvas.roundrect = function(self, x1, y1, x2, y2, rx, ry) canvasShape(self, 5, x1, y1, x2, y2, true, true, rx, ry or rx) end
Impl.canvas.setpixel = function(self, x, y, color)
  local native = rawget(_G, "__manifoldCanvasShape")
  if native then
    native(D[self].serial, 6, num(x), num(y), num(x) + 1, num(y) + 1, tonumber(color) or 0, 1, false, 0, true, 0, 0)
  end
end
Impl.canvas.polygon = function(self, points, ...) canvasPoly(self, true, points, ...) end
Impl.canvas.polyline = function(self, points, ...) canvasPoly(self, false, points, ...) end
Impl.canvas.textout = function(self, x, y, text)
  local native = rawget(_G, "__manifoldCanvasText")
  if not native then
    return
  end
  local d = D[self]
  local size, color, bold, mono = canvasFontInfo(self, d)
  local brush, brushClear = canvasBrush(self, d)
  native(d.serial, num(x), num(y), tostring(text or ""), size, color, bold, mono, brush, brushClear)
end
Impl.canvas.textrect = function(self, rect, x, y, text) Impl.canvas.textout(self, x, y, text) end
Impl.canvas.draw = function(self, x, y, graphic)
  local native = rawget(_G, "__manifoldCanvasDraw")
  if not native or not isObject(graphic) then
    return
  end
  local source = graphic.Canvas
  if source and D[source] then
    native(D[self].serial, D[source].serial, num(x), num(y), 0, 0)
  end
end
Impl.canvas.stretchdraw = function(self, rect, graphic)
  local native = rawget(_G, "__manifoldCanvasDraw")
  if not native or not isObject(graphic) or type(rect) ~= "table" then
    return
  end
  local source = graphic.Canvas
  if source and D[source] then
    local left, top = num(rect.Left or rect[1]), num(rect.Top or rect[2])
    native(D[self].serial, D[source].serial, left, top, num(rect.Right or rect[3]) - left, num(rect.Bottom or rect[4]) - top)
  end
end
Impl.canvas.copyrect = function(self, dest, source, sourceRect)
  if isObject(source) and D[source] and D[source].class == "canvas" then
    local native = rawget(_G, "__manifoldCanvasDraw")
    if native and type(dest) == "table" then
      native(D[self].serial, D[source].serial, num(dest.Left or dest[1]), num(dest.Top or dest[2]), 0, 0)
    end
  end
end

local function markPaint(self)
  local d = D[self]
  d.paintDirty = true
  touchForms()
end
Impl.paintbox = Impl.paintbox or {}
for _, name in ipairs({ "repaint", "invalidate", "refresh", "update" }) do
  Impl.paintbox[name] = markPaint
end

layout.canvasFreed = function(d)
  local free = rawget(_G, "__manifoldCanvasFree")
  if free then
    free(d.serial)
  end
end

local paintErrors = setmetatable({}, { __mode = "k" })
local function paintSurface(control, d)
  local canvas = control.Canvas
  local cd = canvas and D[canvas]
  if not cd then
    return 0
  end
  if d.class == "paintbox" and d.paintDirty ~= false then
    d.paintDirty = false
    canvasReset(cd)
    if type(d.props.onpaint) == "function" then
      local ok, err = pcall(d.props.onpaint, control)
      if not ok and paintErrors[control] ~= tostring(err) then
        paintErrors[control] = tostring(err)
        print("OnPaint failed: " .. tostring(err))
      end
    end
  end
  return cd.serial
end
layout.paintSurface = paintSurface

local function splitterTarget(splitter, sd)
  local parent = sd.parent
  if not parent or not D[parent] then
    return nil
  end
  local align = alignOf(splitter)
  local sp = sd.props
  local best, bestEdge
  for _, control in ipairs(D[parent].controls) do
    local cd = D[control]
    if not rawequal(control, splitter) and cd and not cd.destroyed and shown(control) and alignOf(control) == align then
      local p = cd.props
      local edge
      if align == 3 then
        edge = num(p.left) + num(p.width)
        if edge <= num(sp.left) + 1 and (not bestEdge or edge > bestEdge) then
          best, bestEdge = control, edge
        end
      elseif align == 4 then
        edge = num(p.left)
        if edge >= num(sp.left) + num(sp.width) - 1 and (not bestEdge or edge < bestEdge) then
          best, bestEdge = control, edge
        end
      elseif align == 1 then
        edge = num(p.top) + num(p.height)
        if edge <= num(sp.top) + 1 and (not bestEdge or edge > bestEdge) then
          best, bestEdge = control, edge
        end
      elseif align == 2 then
        edge = num(p.top)
        if edge >= num(sp.top) + num(sp.height) - 1 and (not bestEdge or edge < bestEdge) then
          best, bestEdge = control, edge
        end
      end
    end
  end
  return best
end

local function stringItems(list)
  local out = {}
  local d = list and D[list]
  if d and d.items then
    for i, text in ipairs(d.items) do
      out[i] = tostring(text)
    end
  end
  return out
end

local function nodeEntries(list)
  local out = {}
  for _, node in ipairs(list or {}) do
    local nd = D[node]
    if nd then
      out[#out + 1] = {
        id = uidOf(node),
        text = tostring(nd.props.text or ""),
        expanded = nd.props.expanded == true,
        haschildren = nd.props.haschildren == true,
        children = nodeEntries(nd.children),
      }
    end
  end
  return out
end

local function fires(p, ...)
  for _, name in ipairs({ ... }) do
    if type(p[name]) == "function" then
      return true
    end
  end
  return false
end

local function controlEntry(control, form)
  local d = D[control]
  local p = d.props
  if d.resizePending then
    d.resizePending = nil
    if type(p.onresize) == "function" then
      local ok, err = pcall(p.onresize, control)
      if not ok then
        print("OnResize failed: " .. tostring(err))
      end
    end
  end
  local font = fontInfo(control)
  local popup = p.popupmenu
  local entry = {
    id = uidOf(control),
    kind = d.class,
    x = num(p.left),
    y = num(p.top),
    w = num(p.width),
    h = num(p.height),
    enabled = p.enabled ~= false,
    color = (p.parentcolor ~= true and p.transparent ~= true) and colorValue(p.color) or nil,
    fontsize = font.size,
    fontcolor = font.color,
    bold = font.bold,
    mono = font.mono,
    cursor = tonumber(p.cursor) or 0,
    click = type(p.onclick) == "function",
    dblclick = type(p.ondblclick) == "function",
    hover = type(p.onmouseenter) == "function" or type(p.onmouseleave) == "function",
    popup = (popup and D[popup] and not D[popup].destroyed) and uidOf(popup) or nil,
    mouse = fires(p, "onmousedown", "onmouseup", "onmousemove"),
    wheel = fires(p, "onmousewheelup", "onmousewheeldown", "onmousewheel"),
    children = {},
  }
  if not drawnKinds[d.class] then
    entry.unsupported = ClassNames[d.class] or d.class
    form.unsupported = form.unsupported or entry.unsupported
  end
  local kind = d.class
  if isPanel(control, d) then
    entry.bevel = enum(control.BevelOuter, bevelNames, 2)
    entry.bevelinner = enum(control.BevelInner, bevelNames, 0)
    entry.bevelwidth = tonumber(control.BevelWidth) or 1
    entry.bevelcolor = colorValue(p.bevelcolor)
    entry.text = plainCaption(p.caption or "")
  elseif kind == "label" then
    entry.text = plainCaption(controlText(control, d))
    entry.alignment = enum(p.alignment, alignmentNames, 0)
    entry.layout = enum(p.layout, layoutNames, 0)
    entry.wordwrap = p.wordwrap == true
    entry.color = (p.parentcolor == false and p.transparent == false) and colorValue(p.color) or nil
  elseif kind == "listbox" or kind == "combobox" then
    entry.items = stringItems(control.Items)
    entry.itemindex = tonumber(p.itemindex) or -1
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
  elseif kind == "memo" or kind == "synedit" then
    entry.text = control.Text
    entry.readonly = control.ReadOnly == true
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
  elseif kind == "edit" then
    entry.text = control.Text
    entry.readonly = control.ReadOnly == true
    entry.hint = tostring(p.texthint or "")
    entry.password = p.passwordchar ~= nil and p.passwordchar ~= "" and p.passwordchar ~= "#0"
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
  elseif kind == "splitter" then
    local target = splitterTarget(control, d)
    entry.resizes = target and uidOf(target) or nil
    entry.minsize = tonumber(p.minsize) or 30
    entry.align = alignOf(control)
  elseif kind == "paintbox" or kind == "image" then
    entry.canvas = paintSurface(control, d)
  elseif kind == "treeview" then
    local items = p.items
    entry.nodes = (items and D[items]) and nodeEntries(D[items].roots) or {}
    local selected = p.selected
    entry.selected = (selected and D[selected] and not D[selected].destroyed) and uidOf(selected) or nil
    entry.rightclickselect = p.rightclickselect == true
    entry.autoexpand = p.autoexpand == true
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
  elseif kind == "listview" then
    entry.viewstyle = enum(p.viewstyle, { vsicon = 0, vssmallicon = 1, vslist = 2, vsreport = 3 }, 0)
    entry.itemindex = tonumber(p.itemindex) or -1
    entry.gridlines = p.gridlines == true
    entry.checkboxes = p.checkboxes == true
    entry.headers = p.showcolumnheaders ~= false
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
    entry.columns = {}
    for i, column in ipairs(D[control.Columns].items) do
      local cp = D[column].props
      entry.columns[i] = {
        caption = plainCaption(cp.caption or ""),
        width = tonumber(cp.width) or 50,
        alignment = enum(cp.alignment, alignmentNames, 0),
        visible = cp.visible ~= false,
      }
    end
    entry.rows = {}
    for i, item in ipairs(D[control.Items].items) do
      local ip = D[item].props
      entry.rows[i] = { id = uidOf(item), text = tostring(ip.caption or ""), sub = stringItems(ip.subitems), checked = ip.checked == true }
    end
  else
    entry.text = plainCaption(controlText(control, d))
  end
  if kind == "checkbox" or kind == "togglebox" then
    entry.checked = p.checked == true
  end
  for _, child in ipairs(d.controls) do
    local cd = D[child]
    if cd and not cd.destroyed and shown(child) then
      entry.children[#entry.children + 1] = controlEntry(child, form)
    end
  end
  return entry
end

__manifoldFormSnapshot = function()
  local out = {}
  for _, form in ipairs(forms) do
    local d = D[form]
    if d and d.script and not d.destroyed and d.props.visible then
      local p = d.props
      local constraints = p.constraints and D[p.constraints] and D[p.constraints].props or {}
      local menu = p.menu
      local font = fontInfo(form)
      local entry = {
        id = uidOf(form),
        caption = tostring(form.Caption or ""),
        left = num(p.left),
        top = num(p.top),
        width = tonumber(p.width) or 400,
        height = tonumber(p.height) or 300,
        clientwidth = tonumber(p.clientwidth),
        clientheight = tonumber(p.clientheight),
        center = centred(p.position),
        raise = d.raise or 0,
        minwidth = num(constraints.minwidth),
        minheight = num(constraints.minheight),
        menu = (menu and D[menu] and not D[menu].destroyed) and uidOf(menu) or nil,
        color = colorValue(p.color),
        keydown = type(p.onkeydown) == "function",
        fontsize = font.size,
        fontcolor = font.color,
        bold = font.bold,
        mono = font.mono,
        controls = {},
      }
      for _, child in ipairs(d.controls) do
        local cd = D[child]
        if cd and not cd.destroyed and shown(child) then
          entry.controls[#entry.controls + 1] = controlEntry(child, entry)
        end
      end
      out[#out + 1] = entry
    end
  end
  return out
end

__manifoldFormSet = function(id, field, value)
  local control, d = live(id)
  if not control then
    return
  end
  touchForms()
  if field == "itemindex" then
    d.props.itemindex = tonumber(value) or -1
    d.props.text = nil
  elseif field == "checked" then
    d.props.checked = value == "1"
    d.props.state = value == "1" and 1 or 0
  elseif field == "text" then
    if d.class == "memo" then
      local lines = control.Lines
      D[lines].items = splitLines(value)
    else
      d.props.text = tostring(value or "")
    end
  elseif field == "expanded" then
    d.props.expanded = value == "1"
  elseif field == "selected" then
    local node = live(tonumber(value))
    d.props.selected = node
    if node and d.props.autoexpand == true then
      D[node].props.expanded = true
    end
  elseif field == "width" or field == "height" then
    local size = math.max(0, tonumber(value) or 0)
    d.props[field] = size
    d.base = d.base or {}
    d.base[field] = size
    if d.parent then
      realign(d.parent, nil)
    end
    realign(control, nil)
  end
end

__manifoldFormFire = function(id, event, a, b, c)
  local control, d = live(id)
  if not control then
    return
  end
  local p = d.props
  if event == "select" then
    if d.class == "combobox" then
      call(p.onchange, control)
      call(p.onselect, control)
    else
      call(p.onclick, control)
      call(p.onselectionchange, control, true)
    end
  elseif event == "nodechange" then
    call(p.onchange, control, p.selected)
    call(p.onselectionchanged, control)
  elseif event == "itemselect" then
    local old = D[control.Items].items[(tonumber(a) or -1) + 1]
    if old then
      call(p.onselectitem, control, old, false)
    end
    local item = control.Selected
    if item then
      call(p.onselectitem, control, item, true)
      call(p.onchange, control, item, 2)
    end
  elseif event == "itemcheck" then
    call(p.onitemchecked, control, (live(a)))
  elseif event == "columnclick" then
    call(p.oncolumnclick, control, D[control.Columns].items[(tonumber(a) or -1) + 1])
  elseif event == "dblclick" then
    call(p.ondblclick, control)
  elseif event == "change" then
    call(p.onchange, control)
    if d.class == "checkbox" then
      call(p.onclick, control)
    end
  elseif event == "click" then
    call(p.onclick, control)
  elseif event == "mouseenter" then
    call(p.onmouseenter, control)
  elseif event == "mouseleave" then
    call(p.onmouseleave, control)
  elseif event == "moved" then
    call(p.onmoved, control)
  elseif event == "mousedown" then
    call(p.onmousedown, control, a, b, c)
  elseif event == "mouseup" then
    call(p.onmouseup, control, a, b, c)
  elseif event == "mousemove" then
    call(p.onmousemove, control, a, b)
  elseif event == "wheelup" or event == "wheeldown" then
    local named = event == "wheelup" and p.onmousewheelup or p.onmousewheeldown
    if type(named) == "function" then
      named(control, a, b)
    else
      call(p.onmousewheel, control, event == "wheelup" and 120 or -120, a, b)
    end
  elseif event == "keydown" then
    call(p.onkeydown, control, a)
  end
end

__manifoldFormClose = function(id)
  local form = live(id)
  if form then
    form.close()
  end
end

__manifoldFormBounds = function(id, left, top, width, height, clientWidth, clientHeight)
  local form, d = live(id)
  if not form then
    return
  end
  local p = d.props
  p.left, p.top = left, top
  p.width, p.height = width, height
  if p.clientwidth then
    p.clientwidth = clientWidth or width
  end
  if p.clientheight then
    p.clientheight = clientHeight or height
  end
  d.hostW, d.hostH = clientWidth, clientHeight
  realign(form, nil)
  touchForms()
end

getFormCount = getFormCount or function() return #forms end
getForm = getForm or function(index) return forms[(tonumber(index) or -1) + 1] end

registerFormAddNotification = registerFormAddNotification or function(fn)
  local handle = new("object")
  formNotifications[handle] = fn
  Impl.object.unregister = Impl.object.unregister or function(self) formNotifications[self] = nil end
  return handle
end
unregisterFormAddNotification = unregisterFormAddNotification or function(handle)
  if handle ~= nil then
    formNotifications[handle] = nil
  end
end

setMethodProperty = setMethodProperty or function(object, property, fn) object[property] = fn end
getMethodProperty = getMethodProperty or function(object, property) return object[property] end
setProperty = setProperty or function(object, property, value) object[property] = value end
getProperty = getProperty or function(object, property) return object[property] end

inheritsFromObject = inheritsFromObject or function(v) return isObject(v) or type(v) == "userdata" end
inheritsFromComponent = inheritsFromComponent or function(v) return inherits(v, "component") end
inheritsFromControl = inheritsFromControl or function(v) return inherits(v, "control") end
inheritsFromWinControl = inheritsFromWinControl or function(v) return inherits(v, "wincontrol") end

darkMode = darkMode or function() return true end
translate = translate or function(text) return text end
getTranslationFolder = getTranslationFolder or function() return "" end
loadPOFile = loadPOFile or function() return false end

showSelectionList = showSelectionList or function(title, caption, list, allowCustom)
  if __manifoldHeadless then
    return -1, ""
  end
  local items = {}
  if isObject(list) and D[list] and D[list].items then
    for i, text in ipairs(D[list].items) do
      items[i] = tostring(text)
    end
  elseif type(list) == "table" then
    for i, text in ipairs(list) do
      items[i] = tostring(text)
    end
  end
  return __manifoldShowSelectionList(title, caption, items, allowCustom)
end

__manifoldNewThreadObject = function()
  return new("thread")
end

local addressListObject = new("addresslist")
D[addressListObject].props.name = "Addresslist"
for _, entry in ipairs({ { "List", "treeview" }, { "Header", "headercontrol" } }) do
  local control = construct(entry[2], addressListObject)
  D[control].props.name = entry[1]
  D[addressListObject].props[lower(entry[1])] = control
end
D[mainForm].props.addresslist = addressListObject

__manifoldSetFallback("list", function(list, key)
  if type(key) ~= "string" then
    return nil
  end
  local value = addressListObject[key]
  if type(value) == "function" then
    return function(first, ...)
      if rawequal(first, list) or rawequal(first, addressListObject) then
        return value(addressListObject, ...)
      end
      return value(first, ...)
    end
  end
  return value
end)

local legacyClasses = {}
for name in pairs(classes) do
  legacyClasses[name] = true
end
for _, name in ipairs({ "memoryrecord", "addresslist", "timer", "memscan", "foundlist", "tablefile", "memorystream", "stream", "hotkey", "genericHotkey" }) do
  legacyClasses[lower(name)] = true
end
local globalMeta = getmetatable(_G) or {}
local previousIndex = globalMeta.__index
globalMeta.__index = function(globals, name)
  if type(name) == "string" then
    local class, method = name:match("^(%a+)_(%a[%w]*)$")
    if class and legacyClasses[lower(class)] and method:match("^%l") then
      local fn = function(object, ...)
        local member = object and object[method]
        if type(member) == "function" then
          return member(object, ...)
        end
        return member
      end
      rawset(globals, name, fn)
      return fn
    end
  end
  if type(previousIndex) == "function" then
    return previousIndex(globals, name)
  elseif type(previousIndex) == "table" then
    return previousIndex[name]
  end
  return nil
end
setmetatable(_G, globalMeta)

__manifoldLcl = {
  new = new,
  data = D,
  isObject = isObject,
  inherits = inherits,
  forms = forms,
}

getSettings = getSettings or function(path)
  local section = (path == nil or path == "") and "Cheat Engine" or tostring(path)
  local value = setmetatable({}, {
    __index = function(_, name)
      return __manifoldSettingRead(section, tostring(name))
    end,
    __newindex = function(_, name, v)
      __manifoldSettingWrite(section, tostring(name), v == nil and "" or tostring(v))
    end,
  })
  local settings = { Path = section, Value = value, ClassName = "TSettings" }
  settings.getValue = function(name) return value[name] end
  settings.setValue = function(name, v) value[name] = v end
  settings.destroy = noop
  return settings
end
