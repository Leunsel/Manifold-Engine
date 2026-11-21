-- Manifold form: made with the Form Editor. manifold.ui.form.load builds the window from it.
return {
  form = 1,
  title = "Player Tools",
  icon = "user",
  width = 440,
  height = 470,
  key = "form demo",
  children = {
    {
      kind = "section",
      title = "Player",
      icon = "heart",
      children = {
        {
          kind = "grid",
          children = {
            { kind = "text", text = "Health" },
            { kind = "slider", name = "health", max = 100, value = 100, integer = true, onChange = "healthChanged" },
            { kind = "text", text = "Speed" },
            { kind = "slider", name = "speed", max = 5, value = 1, format = "%.1f x", onChange = "speedChanged" },
            { kind = "text", text = "Name" },
            { kind = "input", name = "name", value = "Hero", hint = "Player name" },
          },
        },
        { kind = "checkbox", name = "god", label = "God Mode", onChange = "godChanged" },
      },
    },
    { kind = "separator", text = "Look" },
    { kind = "radio", name = "difficulty", items = { "Easy", "Normal", "Hard" }, value = 2, horizontal = true, onChange = "difficultyChanged" },
    { kind = "color", name = "tint", label = "Tint", value = "#4C9AFF", onChange = "tintChanged" },
    {
      kind = "row",
      align = "right",
      children = {
        { kind = "button", name = "reset", text = "Reset", icon = "undo", onClick = "resetClicked" },
        { kind = "button", name = "apply", text = "Apply", icon = "check", style = "accent", onClick = "applyClicked" },
      },
    },
  },
}
