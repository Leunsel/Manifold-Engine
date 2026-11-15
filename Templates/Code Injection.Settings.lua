return {
    SchemaVersion = 2,
    Id = "manifold.builtin.code-injection",
    Caption = "Code injection",
    Description = "Jumps from the address into allocated memory and runs the original code there.",
    Category = "Manifold Engine",
    InSubMenu = false,
    Shortcut = "Ctrl+I",
    Order = 2,
    Tags = { "injection" },
    Requires = { "Address", "OriginalOpcodes", "JumpType" },
    Memory = {
        AskForInjectionAddress = true,
        AskForHookName = false,
        AppendToHookName = "",
    },
}
