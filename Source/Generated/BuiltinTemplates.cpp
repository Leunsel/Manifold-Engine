#include "Templates/BuiltinTemplates.h"

#include <array>

namespace Manifold::Templates
{
    namespace
    {
        constexpr std::array<std::string_view, 1> kFile0 = {
            std::string_view(R"MTPL({ Game   : << Process >>
  Date   : << Date >>
  Author :

  Describe what this script changes.
}

[ENABLE]

aobscanmodule(<< HookName >>,<< Module >>,<< AoBStr >>) // should be unique
alloc(newmem,<< AllocationSize >>,<< HookName >>)

label(code)
label(return)

newmem:

code:
<< OriginalOpcodes >>
  jmp return

<< HookName >><< AoBOffset >>:
  << JumpType >> newmem
<% if NopPadding ~= "" then %>
  << trim(NopPadding) >>
<% end %>
return:
registersymbol(<< HookName >>)

[DISABLE]

<< HookName >><< AoBOffset >>:
  db << OriginalBytes >>

unregistersymbol(<< HookName >>)
dealloc(newmem)
<% if not isEmpty(CodeContext) then %>

{
// ORIGINAL CODE - INJECTION POINT: << Address >>

<< CodeContext >>
}
<% end %>
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile1 = {
            std::string_view(R"MTPL(return {
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
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile2 = {
            std::string_view(R"MTPL([ENABLE]
{$lua}
if syntaxcheck then return end
print("enabled")
{$asm}

[DISABLE]
{$lua}
if syntaxcheck then return end
print("disabled")
{$asm}
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile3 = {
            std::string_view(R"MTPL(return {
    SchemaVersion = 2,
    Id = "manifold.builtin.call-lua",
    Caption = "Call Lua",
    Description = "Runs Lua code when the script is switched on and off.",
    Category = "Manifold Engine",
    InSubMenu = false,
    Shortcut = "Ctrl+L",
    Order = 5,
    Tags = { "lua" },
}
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile4 = {
            std::string_view(R"MTPL([ENABLE]
// code from here to [DISABLE] runs when the script is switched on

[DISABLE]
// code from here to the end runs when the script is switched off
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile5 = {
            std::string_view(R"MTPL(return {
    SchemaVersion = 2,
    Id = "manifold.builtin.cheat-table-framework",
    Caption = "Cheat table framework",
    Description = "An empty script with an [ENABLE] and a [DISABLE] section.",
    Category = "Manifold Engine",
    InSubMenu = false,
    Shortcut = "Ctrl+Alt+T",
    Order = 4,
    Tags = { "framework" },
}
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile6 = {
            std::string_view(R"MTPL(alloc(newmem,<< AllocationSize >>,<< Address >>)
label(returnhere)
label(originalcode)
label(exit)

newmem:
  // your code here

originalcode:
<< OriginalOpcodes >>

exit:
  jmp returnhere

<< Address >>:
  << JumpType >> newmem
<% if NopPadding ~= "" then %>
  << trim(NopPadding) >>
<% end %>
returnhere:
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile7 = {
            std::string_view(R"MTPL(return {
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
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile8 = {
            std::string_view(R"MTPL(define(<< HookName >>,<< Address >>)

[ENABLE]
assert(<< HookName >>,<< OriginalBytes >>)
alloc(newmem,<< AllocationSize >>,<< HookName >>)

label(code)
label(return)

newmem:
  // your code here

code:
<< OriginalOpcodes >>
  jmp return

<< HookName >>:
  << JumpType >> newmem
<% if NopPadding ~= "" then %>
  << trim(NopPadding) >>
<% end %>
return:

[DISABLE]
<< HookName >>:
  db << OriginalBytes >>

dealloc(newmem)
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile9 = {
            std::string_view(R"MTPL(return {
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
)MTPL"),
        };
        constexpr std::array<std::string_view, 1> kFile10 = {
            std::string_view(R"MTPL({
   Process : << Process >>
   Module  : << Module >>
   Address : << Address >>
   Bytes   : << OriginalBytes >>
   Date    : << Date >>
   Author  :
}
)MTPL"),
        };

        const std::array<BuiltinFile, 11> kFiles = {
            BuiltinFile{ "AOB Injection.CEA", std::span<const std::string_view>(kFile0) },
            BuiltinFile{ "AOB Injection.Settings.lua", std::span<const std::string_view>(kFile1) },
            BuiltinFile{ "Call Lua.CEA", std::span<const std::string_view>(kFile2) },
            BuiltinFile{ "Call Lua.Settings.lua", std::span<const std::string_view>(kFile3) },
            BuiltinFile{ "Cheat Table Framework.CEA", std::span<const std::string_view>(kFile4) },
            BuiltinFile{ "Cheat Table Framework.Settings.lua", std::span<const std::string_view>(kFile5) },
            BuiltinFile{ "Code Injection.CEA", std::span<const std::string_view>(kFile6) },
            BuiltinFile{ "Code Injection.Settings.lua", std::span<const std::string_view>(kFile7) },
            BuiltinFile{ "Full Injection.CEA", std::span<const std::string_view>(kFile8) },
            BuiltinFile{ "Full Injection.Settings.lua", std::span<const std::string_view>(kFile9) },
            BuiltinFile{ "Header.CEA", std::span<const std::string_view>(kFile10) },
        };
    }

    std::string BuiltinFile::Text() const
    {
        std::string text;
        for (const std::string_view chunk : Chunks)
            text.append(chunk);
        return text;
    }

    std::span<const BuiltinFile> BuiltinFiles()
    {
        return kFiles;
    }
}
