-- Snake on manifold.ui: a small game and a benchmark of the canvas.
-- Arrow keys or WASD steer, Space or P pauses, R or Enter starts again.
-- A click on the board turns the snake toward the click.
-- "Stress" adds bouncing dots that are drawn every frame, to see how many
-- drawings per frame the canvas and the Lua side keep up with.

local ui = manifold.ui
local log = manifold.log.channel("Snake")

local COLUMNS, ROWS, CELL = 24, 18, 16
local WIDTH, HEIGHT = COLUMNS * CELL, ROWS * CELL
local FIRST_STEP, FASTEST_STEP = 0.14, 0.055

local game = {}
local stats = { frames = 0, frameTime = 0, slowest = 0, drawings = 0, since = os.clock(), last = os.clock(), fps = 0, average = 0, worst = 0 }
local stress = { count = 0, dots = {} }
local best = 0
local widgets = {}
local entries = {}

local TURNS = {
  LeftArrow = { -1, 0 }, A = { -1, 0 },
  RightArrow = { 1, 0 }, D = { 1, 0 },
  UpArrow = { 0, -1 }, W = { 0, -1 },
  DownArrow = { 0, 1 }, S = { 0, 1 },
}

local function placeFood()
  local free = {}
  local taken = {}
  for _, part in ipairs(game.snake) do
    taken[part.y * COLUMNS + part.x] = true
  end
  for y = 0, ROWS - 1 do
    for x = 0, COLUMNS - 1 do
      if not taken[y * COLUMNS + x] then
        free[#free + 1] = { x = x, y = y }
      end
    end
  end
  game.food = #free > 0 and free[math.random(#free)] or nil
end

local function restart()
  game = {
    snake = { { x = 8, y = 9 }, { x = 7, y = 9 }, { x = 6, y = 9 } },
    direction = { 1, 0 },
    queue = {},
    grow = 0,
    score = 0,
    step = FIRST_STEP,
    clock = 0,
    state = "ready",
    pulse = 0,
  }
  placeFood()
end

local function turn(dx, dy)
  if game.state == "over" then
    return
  end
  local last = game.queue[#game.queue] or game.direction
  if (dx ~= 0 and dx == -last[1]) or (dy ~= 0 and dy == -last[2]) or (dx == last[1] and dy == last[2]) then
    return
  end
  if #game.queue < 2 then
    game.queue[#game.queue + 1] = { dx, dy }
  end
  if game.state == "paused" or game.state == "ready" then
    game.state = "running"
  end
end

local function advance()
  if #game.queue > 0 then
    game.direction = table.remove(game.queue, 1)
  end
  local head = game.snake[1]
  local x, y = head.x + game.direction[1], head.y + game.direction[2]
  if x < 0 or y < 0 or x >= COLUMNS or y >= ROWS then
    game.state = "over"
    return
  end
  local tail = #game.snake
  for index = 1, tail - (game.grow > 0 and 0 or 1) do
    local part = game.snake[index]
    if part.x == x and part.y == y then
      game.state = "over"
      return
    end
  end
  table.insert(game.snake, 1, { x = x, y = y })
  if game.food and x == game.food.x and y == game.food.y then
    game.score = game.score + 10
    game.grow = game.grow + 2
    game.step = math.max(FASTEST_STEP, game.step * 0.96)
    placeFood()
  end
  if game.grow > 0 then
    game.grow = game.grow - 1
  else
    table.remove(game.snake)
  end
  if game.state == "over" or not game.food then
    game.state = "over"
  end
  if game.score > best then
    best = game.score
  end
end

local function setStress(count)
  stress.count = count
  local dots = stress.dots
  for index = #dots + 1, count do
    local angle = math.random() * math.pi * 2
    local speed = 30 + math.random() * 120
    dots[index] = { x = math.random() * WIDTH, y = math.random() * HEIGHT, vx = math.cos(angle) * speed, vy = math.sin(angle) * speed, color = index % 3 }
  end
  for index = #dots, count + 1, -1 do
    dots[index] = nil
  end
end

local DOT_COLORS = { [0] = "accent", "warning", "success" }

local function draw(dt)
  local items = {
    { kind = "rect", x = 0, y = 0, width = WIDTH, height = HEIGHT, color = "card", filled = true, rounding = 6 },
  }
  local count = 1
  local function add(item)
    count = count + 1
    items[count] = item
  end

  for _, dot in ipairs(stress.dots) do
    dot.x, dot.y = dot.x + dot.vx * dt, dot.y + dot.vy * dt
    if dot.x < 0 or dot.x > WIDTH then
      dot.vx = -dot.vx
      dot.x = math.max(0, math.min(WIDTH, dot.x))
    end
    if dot.y < 0 or dot.y > HEIGHT then
      dot.vy = -dot.vy
      dot.y = math.max(0, math.min(HEIGHT, dot.y))
    end
    add({ kind = "circle", x = dot.x, y = dot.y, radius = 2, color = DOT_COLORS[dot.color], filled = true })
  end

  if game.food then
    local glow = 4 + math.sin(game.pulse * 6) * 1.5
    local fx, fy = game.food.x * CELL + CELL / 2, game.food.y * CELL + CELL / 2
    add({ kind = "circle", x = fx, y = fy, radius = glow + 3, color = "error", thickness = 1 })
    add({ kind = "circle", x = fx, y = fy, radius = glow, color = "error", filled = true })
  end

  local length = #game.snake
  for index = length, 1, -1 do
    local part = game.snake[index]
    local inset = index == 1 and 1 or 2
    add({ kind = "rect", x = part.x * CELL + inset, y = part.y * CELL + inset, width = CELL - inset * 2, height = CELL - inset * 2,
      color = index == 1 and "accent" or (index % 2 == 0 and "success" or "accentHovered"), filled = true, rounding = 4 })
  end
  local head = game.snake[1]
  if head then
    local cx, cy = head.x * CELL + CELL / 2, head.y * CELL + CELL / 2
    local dx, dy = game.direction[1], game.direction[2]
    for _, side in ipairs({ -1, 1 }) do
      add({ kind = "circle", x = cx + dx * 3 - dy * 3 * side, y = cy + dy * 3 + dx * 3 * side, radius = 1.6, color = "page", filled = true })
    end
  end

  add({ kind = "rect", x = 0, y = 0, width = WIDTH, height = HEIGHT, color = "cardBorder", rounding = 6, thickness = 1.5 })
  if game.state == "ready" then
    add({ kind = "text", x = WIDTH / 2 - 112, y = HEIGHT / 2 + 30, text = "Click here, then press an arrow key", color = "muted", size = 14 })
  elseif game.state == "paused" then
    add({ kind = "text", x = WIDTH / 2 - 34, y = HEIGHT / 2 - 10, text = "Paused", color = "text", size = 20 })
  elseif game.state == "over" then
    add({ kind = "text", x = WIDTH / 2 - 52, y = HEIGHT / 2 - 22, text = "Game over", color = "error", size = 22 })
    add({ kind = "text", x = WIDTH / 2 - 76, y = HEIGHT / 2 + 6, text = "R or Enter starts again", color = "muted", size = 13 })
  end
  widgets.board.draw(items)
  return count
end

local function showStats()
  widgets.score.update({ text = ("Score %d    Best %d    Length %d"):format(game.score, best, #game.snake) })
  widgets.speed.update({ text = ("%.1f steps per second"):format(1 / game.step) })
  widgets.fps.update({ text = ("%.0f frames per second"):format(stats.fps) })
  widgets.lua.update({ text = ("%.2f ms per frame in Lua (worst %.2f ms)"):format(stats.average * 1000, stats.worst * 1000) })
  widgets.drawings.update({ text = ("%d drawings per frame"):format(stats.drawings) })
  widgets.load.update({ value = math.min(1, stats.average * 60), text = ("%.0f %% of a 60 Hz frame"):format(stats.average * 60 * 100) })
end

local function frame()
  local now = os.clock()
  local dt = math.min(0.1, now - stats.last)
  stats.last = now
  if game.state == "running" then
    game.clock = game.clock + dt
    while game.clock >= game.step and game.state == "running" do
      game.clock = game.clock - game.step
      advance()
    end
  end
  game.pulse = game.pulse + dt
  stats.drawings = draw(dt)

  local spent = os.clock() - now
  stats.frames = stats.frames + 1
  stats.frameTime = stats.frameTime + spent
  stats.slowest = math.max(stats.slowest, spent)
  if now - stats.since >= 0.5 then
    stats.fps = stats.frames / (now - stats.since)
    stats.average = stats.frameTime / stats.frames
    stats.worst = stats.slowest
    stats.frames, stats.frameTime, stats.slowest, stats.since = 0, 0, 0, now
    showStats()
  end
end

local function onKey(key)
  local direction = TURNS[key]
  if direction then
    turn(direction[1], direction[2])
  elseif key == "Space" or key == "P" then
    if game.state == "running" then
      game.state = "paused"
    elseif game.state == "paused" then
      game.state = "running"
    end
  elseif key == "R" or key == "Enter" or key == "KeypadEnter" then
    restart()
  end
end

local function onClick(x, y)
  local head = game.snake[1]
  if not head or game.state == "over" or game.state == "ready" then
    return
  end
  local dx = x - (head.x * CELL + CELL / 2)
  local dy = y - (head.y * CELL + CELL / 2)
  if game.direction[1] ~= 0 then
    turn(0, dy < 0 and -1 or 1)
  else
    turn(dx < 0 and -1 or 1, 0)
  end
end

local window = ui.window.create("Snake", {
  icon = "gamepad",
  width = WIDTH + 330,
  height = HEIGHT + 120,
  key = "manifold snake",
  onUpdate = frame,
})

local layout = window.add("row")
widgets.board = layout.add("canvas", {
  width = WIDTH,
  height = HEIGHT,
  background = "surface",
  tooltip = "Arrow keys or WASD steer, Space pauses, R starts again. Click the board first so it has the keyboard.",
  onKey = onKey,
  onClick = onClick,
})

local side = layout.add("column", { width = 300 })
local scoreSection = side.add("section", { title = "Game", icon = "gamepad" })
widgets.score = scoreSection.add("text", { text = "", mono = true })
widgets.speed = scoreSection.add("text", { text = "", color = "muted" })
local buttons = scoreSection.add("row")
buttons.add("button", { text = "Pause", icon = "pause", onClick = function() onKey("P") end })
buttons.add("button", { text = "Restart", icon = "redo", style = "accent", onClick = function() restart() end })

local performance = side.add("section", { title = "Performance", icon = "tachometer-alt" })
widgets.fps = performance.add("text", { text = "", mono = true })
widgets.lua = performance.add("text", { text = "", mono = true })
widgets.drawings = performance.add("text", { text = "", mono = true })
widgets.load = performance.add("progress", { value = 0 })
performance.add("slider", {
  label = "Stress",
  value = 0,
  min = 0,
  max = 20000,
  integer = true,
  tooltip = "Bouncing dots drawn every frame on top of the game",
  onChange = function(value)
    setStress(math.floor(value))
  end,
})
performance.add("text", { text = "Lua time covers moving the dots, building the drawing list and handing it to the canvas; frames per second shows how often the window ran this script.", color = "muted", wrap = true })

entries.show = ui.menu.add("Snake/Show the game", { icon = "gamepad", onClick = function() window.focus() end })
entries.close = ui.menu.add("Snake/Close the game", {
  icon = "times",
  onClick = function()
    for _, entry in pairs(entries) do
      entry.remove()
    end
    window.remove()
  end,
})

math.randomseed(os.time())
restart()
showStats()
log.success("ready: click the board, then steer with the arrow keys or WASD")
