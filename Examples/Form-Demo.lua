-- The layout lives in Form-Demo.form.lua: open it in Tools > Form Editor to change it.
-- This script only holds the handlers; Export Script there writes a file like this one.
local ui = manifold.ui
local log = manifold.log.channel("Form demo")

local window, w
local handlers = {}

local function summary()
  return ("%s: health %d, speed %.1f x, %s"):format(w.name.get(), w.health.get(), w.speed.get(), w.god.get() and "god mode" or "mortal")
end

function handlers.healthChanged(value)
  w.god.set(value >= 100 and w.god.get())
end

function handlers.speedChanged(value)
  log.info(("Speed %.1f x"):format(value))
end

function handlers.godChanged(value)
  w.health.update({ enabled = not value })
end

function handlers.difficultyChanged(value)
  log.info("Difficulty " .. ({ "Easy", "Normal", "Hard" })[value])
end

function handlers.tintChanged(value)
  log.info("Tint " .. value)
end

function handlers.resetClicked()
  w.health.set(100)
  w.speed.set(1)
  w.god.set(false)
  w.health.update({ enabled = true })
end

function handlers.applyClicked()
  ui.notice.show(summary(), { level = "success" })
end

window, w = ui.form.load("Form-Demo.form.lua", handlers)
