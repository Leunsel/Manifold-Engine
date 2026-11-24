-- Theming with manifold.ui.theme: read the look, try a theme for this session,
-- change single colours and sizes, and save the result as a custom theme.
--
-- apply, override and restore only change the look until the program closes;
-- running this script again or stopping it gives your own theme back.
-- Only "Save as Theme" writes a file (into the themes folder).
local ui = manifold.ui
local theme = manifold.ui.theme
local log = manifold.log.channel("Theme demo")

local MODES = { "dark", "light", "system" }
local SWATCHES = { "accent", "success", "warning", "error", "text", "muted", "page", "card", "codeKeyword", "codeString" }

-- what this script changed on top of the theme it applied, for "Save as Theme"
local changes = { accent = nil, rounding = nil }
local w = {}

-- { r, g, b, a } from 0 to 1 -> "#rrggbb"
local function hex(color)
  local function byte(value)
    return math.floor(value * 255 + 0.5)
  end
  return ("#%02x%02x%02x"):format(byte(color.r), byte(color.g), byte(color.b))
end

local function themeNames()
  local names = {}
  for _, entry in ipairs(theme.list()) do
    names[#names + 1] = entry.custom and (entry.name .. " (custom)") or entry.name
  end
  return names
end

local function refresh()
  local look = theme.current()
  w.name.update({ text = look.custom and ("%s, a custom theme based on %s"):format(look.name, look.base) or (look.name .. " preset") })
  w.mode.update({ text = ("Mode %s, %s colours are showing"):format(look.mode, look.light and "light" or "dark") })
  w.scripted.update({
    text = look.scripted and "A script changed the look for this session." or "This is your own theme from the settings.",
    color = look.scripted and "warning" or "success",
  })
  local accent = theme.color("accent")
  if accent then
    w.accent.set(hex(accent))
  end
  w.themes.update({ items = themeNames() })

  -- swatches: canvas drawings take theme colour names, so they follow the theme by themselves
  local drawings = {}
  for index, name in ipairs(SWATCHES) do
    local x = (index - 1) % 5 * 112
    local y = (index - 1) // 5 * 44
    drawings[#drawings + 1] = { kind = "rect", x = x, y = y, width = 28, height = 28, filled = true, rounding = 4, color = name }
    drawings[#drawings + 1] = { kind = "rect", x = x, y = y, width = 28, height = 28, rounding = 4, color = "border" }
    local value = theme.color(name)
    drawings[#drawings + 1] = { kind = "text", x = x + 34, y = y + 1, text = name, color = "label" }
    drawings[#drawings + 1] = { kind = "text", x = x + 34, y = y + 15, text = value and hex(value) or "?", color = "muted" }
  end
  w.swatches.draw(drawings)
end

local window = ui.window.create("Theme demo", { icon = "palette", width = 600, height = 620, key = "manifold theme demo" })

-- 1. What is shown now
local now = window.add("section", { title = "Shown Now", icon = "eye" })
w.name = now.add("text", { text = "", bold = true })
w.mode = now.add("text", { text = "", color = "muted" })
w.scripted = now.add("text", { text = "" })
w.swatches = now.add("canvas", { height = 84 })

-- 2. Try a preset or a custom theme for this session
local try = window.add("section", { title = "Try a Theme", icon = "magic" })
w.themes = try.add("combo", { label = "Theme", items = themeNames(), value = 1 })
w.modes = try.add("radio", { label = "Mode", items = { "Dark", "Light", "System" }, value = 1, horizontal = true })
local buttons = try.add("row")
buttons.add("button", {
  text = "Apply for This Session",
  icon = "play",
  style = "accent",
  onClick = function()
    local entry = theme.list()[w.themes.get()]
    local ok, problem = theme.apply(entry.name, { mode = MODES[w.modes.get()] })
    if not ok then
      ui.notice.show(tostring(problem), { level = "error" })
      return
    end
    changes.accent, changes.rounding = nil, nil
    log.info("Applied " .. entry.name)
  end,
})
buttons.add("button", {
  text = "Restore My Theme",
  icon = "undo",
  onClick = function()
    if theme.restore() then
      changes.accent, changes.rounding = nil, nil
    else
      ui.notice.show("The look is already your own theme.")
    end
  end,
})

-- 3. Change single colours and sizes on top of what is shown
local tweak = window.add("section", { title = "Change Single Values", icon = "sliders-h" })
w.accent = tweak.add("color", {
  label = "Accent",
  value = "#3080e0",
  onChange = function(value)
    -- `colors` changes dark and light; `dark = {...}` or `light = {...}` change one mode
    local ok, problem = theme.override({ colors = { accent = value, accentHovered = value } })
    if ok then
      changes.accent = value
    else
      ui.notice.show(tostring(problem), { level = "error" })
    end
  end,
})
tweak.add("slider", {
  label = "Frame Rounding",
  value = 3,
  min = 0,
  max = 12,
  integer = true,
  onChange = function(value)
    -- the size names are listed by theme.sizes(): FrameRounding, ItemSpacing.x, ...
    if theme.override({ sizes = { FrameRounding = value } }) then
      changes.rounding = value
    end
  end,
})

-- 4. Save the look as a custom theme file
local save = window.add("section", { title = "Save as Theme", icon = "save" })
w.saveName = save.add("input", { label = "Name", value = "My Demo Theme", hint = "A new theme name" })
w.overwrite = save.add("checkbox", { label = "Replace my theme of that name" })
save.add("button", {
  text = "Save as Theme",
  icon = "save",
  style = "accent",
  onClick = function()
    -- start from what the shown theme changes from its preset, then add this script's changes
    local definition = assert(theme.definition(theme.current().name))
    if changes.accent then
      definition.dark.accent, definition.light.accent = changes.accent, changes.accent
      definition.dark.accentHovered, definition.light.accentHovered = changes.accent, changes.accent
    end
    if changes.rounding then
      definition.sizes.FrameRounding = changes.rounding
    end
    local name = w.saveName.get()
    local ok, problem = theme.save(name, definition, { overwrite = w.overwrite.get() })
    if ok then
      ui.notice.show("Saved " .. name .. ". Pick it in Settings > Appearance to keep it.", { level = "success" })
      w.themes.update({ items = themeNames() })
    elseif problem.code == "ME-5016" then
      ui.notice.show(name .. " exists already. Tick the box to replace it, or pick another name.", { level = "warning" })
    else
      ui.notice.show(tostring(problem), { level = "error" })
    end
  end,
})

-- the event comes after the user, Windows (System mode) or a script changed the look
manifold.event.subscribe("themeChanged", function(look)
  log.debug("Theme changed: " .. look.name .. " (" .. look.mode .. ")")
  refresh()
end, { key = "theme demo" })

refresh()
