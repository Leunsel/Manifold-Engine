return {
    SchemaVersion = 2,
    Id = "manifold.builtin.aob-injection",
    Caption = "AOB injection",
    Description = "Finds the code by a unique byte pattern, so the script survives updates that move it.",
    Category = "Manifold Engine",
    InSubMenu = false,
    Shortcut = "Ctrl+Shift+A",
    Order = 1,
    Tags = { "injection", "aob" },
    Requires = { "Process", "Module", "HookName", "AoBStr", "OriginalBytes", "OriginalOpcodes", "JumpType" },
    Optional = { "CodeContext" },
    Memory = {
        AskForInjectionAddress = true,
        AskForHookName = true,
        DefaultHookName = "INJECT",
        AppendToHookName = "",
    },
}
