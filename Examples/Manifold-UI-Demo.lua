local ui = manifold.ui
local log = manifold.log.channel("UI demo")

local DIFFICULTIES = { "Easy", "Normal", "Hard", "Nightmare" }
local WORLD_WIDTH = 400
local WORLD_HEIGHT = 300

local state = {
  health = 100,
  speed = 1.0,
  god = false,
  difficulty = 2,
  name = "Player",
  angle = 0,
  markers = {},
  hover = nil,
  scale = 1,
  progress = nil,
  sticky = nil,
  list = "modules",
  rows = {},
}

local widgets = {}
local entries = {}
local refreshSummary
local refreshList
local refreshStatus
local closeDemo

local function hex(value)
  return ("%X"):format(value)
end

local window = ui.window.create("UI demo", {
  icon = "magic",
  width = 660,
  height = 540,
  key = "manifold ui demo",
  interval = 1 / 30,
  onClose = function()
    ui.notice.show("The demo window is hidden. Extensions > UI demo > Show window brings it back.")
  end,
  onUpdate = function()
    state.angle = state.angle + 0.04 * state.speed
    widgets.drawMap()
    if state.progress then
      state.progress.value = math.min(1, state.progress.value + 1 / 90)
      if state.progress.value >= 1 then
        state.progress.notice.update({ text = "Scan finished", level = "success", progress = 1 })
        state.progress = nil
      else
        state.progress.notice.update({ text = ("Scanning %d%%"):format(math.floor(state.progress.value * 100)), progress = state.progress.value })
      end
    end
  end,
})

local tabs = window.add("tabs")

local valuesTab = tabs.add("tab", { title = "Values", icon = "sliders-h" })
local player = valuesTab.add("section", { title = "Player", icon = "user", badge = "demo" })
local firstRow = player.add("row")
widgets.health = firstRow.add("number", {
  label = "Health",
  value = state.health,
  min = 0,
  max = 999,
  step = 1,
  integer = true,
  width = 130,
  onChange = function(value)
    state.health = value
    refreshSummary()
  end,
})
widgets.speed = firstRow.add("slider", {
  label = "Speed",
  value = state.speed,
  min = 0.1,
  max = 5,
  format = "%.1fx",
  width = 170,
  onChange = function(value)
    state.speed = value
    refreshSummary()
  end,
})
local secondRow = player.add("row")
widgets.god = secondRow.add("checkbox", {
  label = "God mode",
  value = state.god,
  onChange = function(value)
    state.god = value
    refreshSummary()
  end,
})
widgets.difficulty = secondRow.add("combo", {
  label = "Difficulty",
  items = DIFFICULTIES,
  value = state.difficulty,
  width = 130,
  onChange = function(value)
    state.difficulty = value
    refreshSummary()
  end,
})
widgets.name = player.add("input", {
  label = "Name",
  value = state.name,
  hint = "Shown in the summary",
  width = 220,
  onChange = function(text)
    state.name = text
    refreshSummary()
  end,
  onSubmit = function(text)
    ui.notice.show("Hello, " .. text .. "!", { level = "success" })
  end,
})

local summary = valuesTab.add("section", { title = "Summary", icon = "info-circle" })
widgets.summary = summary.add("text", { wrap = true })
widgets.healthBar = summary.add("progress", { value = 0 })

local actions = valuesTab.add("row")
actions.add("button", {
  text = "Apply",
  icon = "check",
  style = "accent",
  onClick = function()
    ui.notice.show(("Applied %s: %d health at %.1fx speed"):format(state.name, state.health, state.speed), { level = "success" })
    log.info("applied", { name = state.name, health = state.health, speed = state.speed, god = state.god })
  end,
})
actions.add("button", {
  text = "Reset",
  icon = "undo",
  style = "danger",
  onClick = function()
    ui.dialog.confirm("Reset every value to its default?", {
      title = "Reset values",
      danger = true,
      onClose = function(answer)
        if not answer then
          return
        end
        state.health, state.speed, state.god, state.difficulty, state.name = 100, 1.0, false, 2, "Player"
        widgets.health.set(state.health)
        widgets.speed.set(state.speed)
        widgets.god.set(state.god)
        widgets.difficulty.set(state.difficulty)
        widgets.name.set(state.name)
        refreshSummary()
        ui.notice.show("Values reset")
      end,
    })
  end,
})

refreshSummary = function()
  widgets.summary.update({
    text = ("%s plays on %s with %d health at %.1fx speed%s."):format(
      state.name ~= "" and state.name or "Nobody",
      DIFFICULTIES[state.difficulty] or "?",
      state.health,
      state.speed,
      state.god and ", and nothing can hurt them" or ""),
  })
  widgets.healthBar.update({ value = state.health / 999, text = ("%d / 999"):format(state.health) })
  if entries.god then
    entries.god.update({ checked = state.god })
  end
end

local listsTab = tabs.add("tab", { title = "Lists", icon = "list" })
local split = listsTab.add("split", { ratio = 0.32 })
widgets.tree = split.add("tree", {
  height = 330,
  value = state.list,
  items = {
    { text = "Process", icon = "microchip", id = "process", open = true, children = {
      { text = "Modules", icon = "cubes", id = "modules" },
      { text = "Overview", icon = "info-circle", id = "overview" },
    } },
    { text = "Demo data", icon = "table", id = "demo", open = true, children = {
      { text = "Items", icon = "cube", id = "items" },
      { text = "Enemies", icon = "skull", id = "enemies" },
    } },
  },
  onSelect = function(id)
    state.list = id
    refreshList()
  end,
})
local listColumn = split.add("column")
widgets.table = listColumn.add("table", {
  height = 300,
  menu = { "Show details", "-", "Remove row" },
  onSelect = function(row)
    widgets.detail.update({ text = table.concat(state.rows[row] or {}, "  |  ") })
  end,
  onActivate = function(row)
    ui.notice.show("Opened " .. tostring((state.rows[row] or {})[1]))
  end,
  onMenu = function(choice, text, row)
    if text == "Show details" then
      ui.dialog.choose("Row " .. row, state.rows[row] or {}, { title = "Details" })
    elseif text == "Remove row" then
      table.remove(state.rows, row)
      widgets.table.update({ rows = state.rows })
    end
  end,
})
widgets.detail = listColumn.add("text", { text = "Select a row; double-click opens it, right-click shows its menu.", color = "muted", wrap = true })

local function moduleRows()
  local modules, problem = manifold.process.modules()
  if not modules then
    return { "Name", "Base", "Size" }, { { "No process open", "", "" } }, problem
  end
  table.sort(modules, function(a, b) return a.base < b.base end)
  local rows = {}
  for index = 1, math.min(#modules, 2000) do
    local module = modules[index]
    rows[index] = { module.name, hex(module.base), ("%d KB"):format(module.size // 1024) }
  end
  return { "Name", { title = "Base", width = 130, align = "right" }, { title = "Size", width = 90, align = "right" } }, rows
end

local function overviewRows()
  local info = manifold.process.info()
  if not info then
    return { "Field", "Value" }, { { "Process", "none" } }
  end
  local main = manifold.process.mainModule()
  return { { title = "Field", width = 110 }, "Value" }, {
    { "Name", info.name },
    { "Id", tostring(info.id) },
    { "64-bit", info.is64Bit and "yes" or "no" },
    { "Path", info.path or "" },
    { "Base", main and hex(main.base) or "?" },
  }
end

local DEMO_LISTS = {
  items = {
    columns = { "Item", { title = "Count", width = 70, align = "right" }, { title = "Value", width = 80, align = "right" } },
    rows = { { "Potion", "12", "25" }, { "Elixir", "3", "250" }, { "Iron sword", "1", "120" }, { "Arrows", "64", "1" }, { "Map", "1", "40" } },
  },
  enemies = {
    columns = { "Enemy", { title = "Level", width = 60, align = "right" }, "Weakness" },
    rows = { { "Slime", "1", "Fire" }, { "Goblin", "4", "Light" }, { "Wraith", "12", "Holy" }, { "Dragon", "40", "Ice" } },
  },
}

refreshList = function()
  local columns, rows
  if state.list == "modules" or state.list == "process" then
    columns, rows = moduleRows()
  elseif state.list == "overview" then
    columns, rows = overviewRows()
  else
    local list = DEMO_LISTS[state.list] or DEMO_LISTS.items
    columns, rows = list.columns, {}
    for index, row in ipairs(list.rows) do
      rows[index] = { table.unpack(row) }
    end
  end
  state.rows = rows
  widgets.table.update({ columns = columns, rows = rows, value = 0 })
  widgets.detail.update({ text = ("%d row%s. Select one; double-click opens it, right-click shows its menu."):format(#rows, #rows == 1 and "" or "s") })
end

local mapTab = tabs.add("tab", { title = "Map", icon = "map" })
mapTab.add("text", { text = "The wheel zooms, dragging pans, a left click sets a marker and a right click opens the menu.", color = "muted", wrap = true })
widgets.map = mapTab.add("canvas", {
  height = 320,
  background = "surface",
  zoom = true,
  menu = { "Marker here", "Center view", "-", "Clear markers" },
  onClick = function(x, y, button)
    if button == 1 then
      state.markers[#state.markers + 1] = { x = x, y = y }
      refreshStatus()
    end
  end,
  onHover = function(x, y)
    state.hover = { x = x, y = y }
    widgets.coordinates.update({ text = ("x %.0f  y %.0f"):format(x, y) })
  end,
  onView = function(_, _, scale)
    state.scale = scale
    widgets.zoom.update({ text = ("zoom %.2fx"):format(scale) })
  end,
  onMenu = function(_, text, x, y)
    if text == "Marker here" then
      state.markers[#state.markers + 1] = { x = x, y = y }
    elseif text == "Center view" then
      widgets.map.setView({ x = 0, y = 0, scale = 1 })
    elseif text == "Clear markers" then
      state.markers = {}
    end
    refreshStatus()
  end,
})
local mapInfo = mapTab.add("row")
widgets.coordinates = mapInfo.add("text", { text = "x -  y -", mono = true, color = "muted" })
widgets.zoom = mapInfo.add("text", { text = "zoom 1.00x", mono = true, color = "muted" })

widgets.drawMap = function()
  local drawings = {
    { kind = "rect", x = 0, y = 0, width = WORLD_WIDTH, height = WORLD_HEIGHT, color = "card", filled = true, rounding = 6 },
  }
  for x = 25, WORLD_WIDTH - 25, 25 do
    drawings[#drawings + 1] = { kind = "line", x = x, y = 0, x2 = x, y2 = WORLD_HEIGHT, color = "rowSeparator" }
  end
  for y = 25, WORLD_HEIGHT - 25, 25 do
    drawings[#drawings + 1] = { kind = "line", x = 0, y = y, x2 = WORLD_WIDTH, y2 = y, color = "rowSeparator" }
  end
  drawings[#drawings + 1] = { kind = "rect", x = 0, y = 0, width = WORLD_WIDTH, height = WORLD_HEIGHT, color = "cardBorder", rounding = 6, thickness = 1.5 }
  local cx, cy = WORLD_WIDTH / 2, WORLD_HEIGHT / 2
  for index = 1, 5 do
    local turn = state.angle * (0.4 + index * 0.15) + index * 1.3
    local distance = 40 + index * 22
    local ex, ey = cx + math.cos(turn) * distance, cy + math.sin(turn) * distance * 0.7
    drawings[#drawings + 1] = { kind = "circle", x = ex, y = ey, radius = 6, color = "error", filled = true }
    drawings[#drawings + 1] = { kind = "text", x = ex + 8, y = ey - 7, text = DEMO_LISTS.enemies.rows[(index - 1) % 4 + 1][1], color = "muted", size = 12 }
  end
  local a = state.angle
  local function point(length, offset)
    return cx + math.cos(a + offset) * length, cy + math.sin(a + offset) * length
  end
  local x1, y1 = point(16, 0)
  local x2, y2 = point(10, 2.5)
  local x3, y3 = point(4, math.pi)
  local x4, y4 = point(10, -2.5)
  drawings[#drawings + 1] = { kind = "circle", x = cx, y = cy, radius = 26, color = "accent", thickness = 1 }
  drawings[#drawings + 1] = { kind = "polygon", points = { x1, y1, x2, y2, x3, y3, x4, y4 }, color = "accent", filled = true }
  drawings[#drawings + 1] = { kind = "text", x = cx - 18, y = cy + 28, text = state.name, color = "text", size = 13 }
  for index, marker in ipairs(state.markers) do
    drawings[#drawings + 1] = { kind = "circle", x = marker.x, y = marker.y, radius = 5, color = "warning", thickness = 2 }
    drawings[#drawings + 1] = { kind = "text", x = marker.x + 7, y = marker.y - 7, text = "M" .. index, color = "warning", size = 12 }
  end
  if state.hover then
    drawings[#drawings + 1] = { kind = "line", x = state.hover.x - 6, y = state.hover.y, x2 = state.hover.x + 6, y2 = state.hover.y, color = "muted" }
    drawings[#drawings + 1] = { kind = "line", x = state.hover.x, y = state.hover.y - 6, x2 = state.hover.x, y2 = state.hover.y + 6, color = "muted" }
  end
  widgets.map.draw(drawings)
end

local dialogsTab = tabs.add("tab", { title = "Dialogs", icon = "comment-dots" })
local dialogs = dialogsTab.add("section", { title = "Dialogs", icon = "window-restore" })
local function answered(text)
  widgets.answer.update({ text = text })
  log.info(text)
end
local dialogRow = dialogs.add("row")
dialogRow.add("button", {
  text = "Confirm",
  icon = "question-circle",
  onClick = function()
    ui.dialog.confirm("Do you want to continue?", { title = "Confirm", onClose = function(answer) answered("Confirm: " .. tostring(answer)) end })
  end,
})
dialogRow.add("button", {
  text = "Input",
  icon = "user",
  onClick = function()
    ui.dialog.input("What is your name?", {
      title = "Name",
      value = state.name,
      onClose = function(text)
        answered("Input: " .. tostring(text))
        if text then
          state.name = text
          widgets.name.set(text)
          refreshSummary()
        end
      end,
    })
  end,
})
dialogRow.add("button", {
  text = "Choose",
  icon = "list",
  onClick = function()
    ui.dialog.choose("Pick a difficulty", DIFFICULTIES, {
      title = "Difficulty",
      onClose = function(number, text)
        answered("Choose: " .. tostring(text))
        if number then
          state.difficulty = number
          widgets.difficulty.set(number)
          refreshSummary()
        end
      end,
    })
  end,
})
local fileRow = dialogs.add("row")
fileRow.add("button", {
  text = "Open file",
  icon = "folder-open",
  onClick = function()
    ui.dialog.openFile({ title = "Open a Lua script", filters = { { "Lua scripts", "*.lua" }, { "All files", "*.*" } }, onClose = function(path) answered("Open: " .. tostring(path)) end })
  end,
})
fileRow.add("button", {
  text = "Save file",
  icon = "save",
  onClick = function()
    ui.dialog.saveFile({ title = "Save the demo values", name = "demo values.txt", filters = { { "Text files", "*.txt" } }, onClose = function(path) answered("Save: " .. tostring(path)) end })
  end,
})
widgets.answer = dialogs.add("text", { text = "Answers show up here.", color = "muted" })

local notices = dialogsTab.add("section", { title = "Notices", icon = "bell" })
local levelRow = notices.add("row")
for _, level in ipairs({ "info", "success", "warning", "error" }) do
  levelRow.add("button", {
    text = level:sub(1, 1):upper() .. level:sub(2),
    onClick = function()
      ui.notice.show("A notice at level " .. level, { level = level })
    end,
  })
end
local progressRow = notices.add("row")
progressRow.add("button", {
  text = "Progress",
  icon = "hourglass-half",
  onClick = function()
    if state.progress then
      return
    end
    state.progress = { value = 0, notice = ui.notice.show("Scanning 0%", { progress = 0 }) }
  end,
})
progressRow.add("button", {
  text = "Sticky",
  icon = "flag",
  onClick = function()
    if state.sticky and state.sticky.isShown() then
      return
    end
    state.sticky = ui.notice.show("This notice stays until it is removed or right-clicked", { level = "warning", sticky = true })
  end,
})
progressRow.add("button", {
  text = "Remove sticky",
  icon = "times",
  onClick = function()
    if state.sticky then
      state.sticky.remove()
      state.sticky = nil
    end
  end,
})

refreshStatus = function()
  entries.status.update({ text = ("UI demo: %d marker%s"):format(#state.markers, #state.markers == 1 and "" or "s") })
end

entries.show = ui.menu.add("UI demo/Show window", { icon = "window-restore", onClick = function() window.focus() end })
entries.god = ui.menu.add("UI demo/God mode", {
  icon = "heart",
  checked = state.god,
  onClick = function()
    state.god = not state.god
    widgets.god.set(state.god)
    refreshSummary()
  end,
})
entries.separator = ui.menu.add("UI demo/-")
entries.close = ui.menu.add("UI demo/Close the demo", { icon = "times", onClick = function() closeDemo() end })
entries.toolbar = ui.toolbar.add("magic", { tooltip = "Show the UI demo", onClick = function() window.focus() end })
entries.status = ui.status.add("UI demo: 0 markers", { icon = "map-marker-alt", color = "accent", tooltip = "Show the UI demo", onClick = function() window.focus() end })

local subscription = manifold.event.subscribe("processOpened", function()
  refreshList()
end, { key = "manifold ui demo" })

closeDemo = function()
  manifold.event.unsubscribe(subscription)
  if state.progress then
    state.progress.notice.remove()
  end
  if state.sticky then
    state.sticky.remove()
  end
  for _, entry in pairs(entries) do
    entry.remove()
  end
  window.remove()
  log.info("closed")
end

refreshSummary()
refreshList()
widgets.drawMap()
log.success("ready: Extensions > UI demo, the toolbar button and the status entry open it")
