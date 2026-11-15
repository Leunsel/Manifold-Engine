return {
    SchemaVersion = 2,
    Id = "manifold.builtin.full-injection",
    Caption = "Full injection",
    Description = "A code injection with [ENABLE] and [DISABLE] that checks the original bytes before patching.",
    Category = "Manifold Engine",
    InSubMenu = false,
    Shortcut = "Ctrl+Shift+F",
    Order = 3,
    Tags = { "injection" },
    Requires = { "Address", "HookName", "OriginalBytes", "OriginalOpcodes", "JumpType" },
    Memory = {
        AskForInjectionAddress = true,
        AskForHookName = true,
        DefaultHookName = "INJECT",
        AppendToHookName = "",
    },
}
