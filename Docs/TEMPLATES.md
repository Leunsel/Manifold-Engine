# Auto Assembler templates

Templates turn the instruction you picked into a finished Auto Assembler script. They are files, not part of the program: you can change the built-in ones, write your own, and sort them into categories. The format is the one of the Manifold Template Loader for Cheat Engine, so the same template files work in both programs.

## Where templates come from

- **Built-in templates** ship inside the program: AOB injection, code injection, full injection, cheat table framework and Call Lua. They sit at the top of the Template menu.
- **Your templates** live in the `templates` folder next to `Manifold-Engine.exe`. Template → Open template folder creates and opens it.
- A template of yours with the same `Id` as a built-in one replaces the built-in one. Delete your file to get the original back.

Changes in the folder are picked up on their own the next time a Template menu opens. Template → Reload templates loads everything again at once.

## Where templates appear

- The **Template** menu of an Auto Assembler window. The script is generated for the instruction selected last in a memory viewer, and added to the end of the script in the window.
- **Create script** in the right-click menu of the memory viewer's disassembly. The script is generated for the instruction under the mouse and opens in a new Auto Assembler window.

The same menu has two switches:

- **Use 14-byte jumps** makes `JumpType` `jmp far` and `MinJumpSize` 14, for targets out of reach of a 5-byte jump.
- **Preview before inserting** shows the generated script first. Nothing is inserted until you press Insert.

Templates that declare architectures are greyed out while a process of another architecture is open.

## The Templates window

Template → Manage templates... opens a window for all templates. Template → Save script as template... in an Auto Assembler window starts a new template from the script in it.

- **Left**: the templates by category, with a search field. Drag a template onto a category to move it there, or onto "Drop here: top level" below the list. Templates the program skipped are listed under "Skipped" with the reason.
- **Toolbar**: new template, duplicate, move to the recycle bin, check all templates, open the template folder, import templates from a folder. Importing copies `.CEA` and `.Settings.lua` files and leaves files that already exist alone.
- **Script**: the template with Auto Assembler highlighting. The list next to it holds every value, the inputs of the template and the helpers; a click inserts `<< Name >>` at the cursor, and hovering shows what the value is.
- **Settings**: the settings as a form, including the inputs. "Keep the category in the settings file" decides where the category lives: off, the file moves into the folder of its category when you save; on, the category is written into the settings and the file stays where it is.
- **Settings file**: the settings as Lua text. Edits there are saved as typed, comments included. Saving after a change in the form writes the file anew, and comments in it are lost; the form says so when the file has comments.
- **Preview**: renders the template as you type. With a process open and an instruction selected in a memory viewer it uses that instruction; otherwise it uses sample values for `Game.exe+1255B5B`, so the layout shows without a game. The preview never asks questions: the hook name and the address take their defaults, and the inputs of the template have their own fields above the preview. Problems of the template, such as unknown names, show above it, and an error marks the line in the editor.
- Ctrl+S saves. Leaving a template or closing the window with unsaved changes asks first.
- Built-in templates are read only. "Edit a copy" saves a copy in your folder that replaces the built-in template until you delete the copy.

## Files

A template is a `.CEA` file. Next to it, a `.Settings.lua` file with the same name can describe it:

```
templates/
├── Header.CEA                        rendered by << Header >>
├── Partials/                         fragments for include(), not listed in the menu
│   └── Notes.CEA
├── Pointer Hook.CEA
├── Pointer Hook.Settings.lua
└── Hooks/                            subfolders become categories
    └── Trampolines/
        └── My Detour.CEA             listed under Hooks > Trampolines
```

- A template without a settings file is listed by its file name, in the category of its folder (or `Templates` at the top).
- `Header.CEA` at the top of the folder and everything under `Partials` are not templates of their own.
- `include("Name")` looks in `Partials/Name.CEA`, then `Name.CEA` in the folder, then in the built-in templates.

## Template syntax

`<< expression >>` inserts the value of a Lua expression, where `nil` becomes nothing. `<% code %>` runs Lua statements and inserts nothing. All blocks of one template are one Lua chunk, so a `local` from one block is visible in the next.

```
<% for slot = 1, PointerCount do %>
  << PointerType >> 0 // slot << slot >>
<% end %>
```

A `<% %>` block alone on its line takes the line with it, so `<% if %>`, `<% else %>` and `<% end %>` leave no blank lines behind. Errors name the template line.

Helpers: `hex(v)`, `join(sep, ...)`, `default(v, fallback)`, `isEmpty(v)`, `byteCount(bytes)`, `bytesPrefix(bytes, count)`, `trim(s)` and `include(name)`.

## Settings

The settings file returns a table. It runs without `io`, `os` or the program's Lua API, and only sees `ipairs`, `pairs`, `tonumber`, `tostring` and copies of `math`, `string` and `table`.

```lua
return {
    SchemaVersion = 2,
    Id = "my.pointer.hook",
    Caption = "Pointer Hook",
    Description = "Stores the base pointer of the instruction.",
    Category = "Pointers > Hooks",
    CategoryOrder = 1,
    Order = 10,
    Tags = { "pointer" },
    Requires = { "AddressValue", "Module", "HookName", "AoBStr", "OriginalBytes", "OriginalOpcodes" },
    Optional = { "BaseAddressRegister" },
    Inputs = {
        { Name = "PointerCount", Type = "integer", Caption = "Pointer Count", Default = 2, Min = 1, Max = 16 },
        { Name = "Mode", Type = "enum", Caption = "Mode", Values = { "Player", "Entity" }, Default = "Player" },
    },
    Architectures = { "x64" },
    Memory = {
        AskForHookName = true,
        DefaultHookName = "Injection",
        AppendToHookName = "Hook",
        AskForInjectionAddress = false,
        AllocationSize = "$1000",
        AllocationNear = true,
    },
}
```

| Field | Meaning |
| --- | --- |
| `SchemaVersion` | `2` for the fields below. Files without it are read as schema 1, the fields of Template Loader 2.x |
| `Id` | Stable identity, letters, digits, `.`, `_` and `-`. Without one the template is `legacy.<file-name>` |
| `Caption`, `Description` | Menu text and its tooltip |
| `Category`, `CategoryOrder` | Category path, `>` nests. `SubMenuName` and a `[n]` prefix work as in schema 1 |
| `InSubMenu` | `false` lists the template at the top of the menu |
| `Order` | Order inside the category (`MenuOrder` in schema 1) |
| `Requires` | Values the template cannot do without. Generation stops with an explanation when one of them comes out empty |
| `Optional` | Values the template reads if they can be found; a failure turns into an empty value |
| `Inputs` | Typed values asked in one dialog before generating: `string`, `boolean`, `integer`, `number`, `enum` |
| `Architectures` | `x86`, `x64` or both |
| `Capabilities` | `Manifold.AssemblerCommands`, `Manifold.Trampolines`: a warning in the log when the Manifold Framework commands are not loaded |
| `AllowUnsafeGlobals` | `true` lets a schema-2 template read the program's Lua globals. Schema-1 templates always can |
| `Memory` | The hook-name and allocation options below. The flat schema-1 fields are read as well |

Every other field stays readable in the template as `TemplateSettings.<Field>`.

Schema-1 templates without `Requires` first ask for the address, check the module and ask for the hook name, in that order, as the Template Loader 2.x did.

## Values

Values are worked out only when a template uses them, and each one once per generation. A value that cannot be determined reliably is empty rather than guessed.

| Group | Values |
| --- | --- |
| Program | `Version`, `Date`, `Time`, `DateTime`, `Header`, `MonoSupportStatus` |
| Process | `Process`, `ProcessBase`, `IsTarget64Bit`, `PointerType` (`dq`/`dd`), `PointerSize`, `DefaultPointerBytes`, `Module`, `ModuleBase` |
| Instruction | `Address`, `AddressValue`, `Is14ByteJump`, `MinJumpSize`, `JumpType`, `JumpSize` (whole instructions, never part of one), `SelectionSize`, `OriginalInstruction`, `OriginalOpcodes`, `OriginalBytes`, `NopPadding`, `NopBytes`, `BaseAddressRegister`, `BaseAddressOffset` |
| Hook | `HookName`, `HookNameParsed`, `Alloc`, `GlobalAlloc`, `AoBStr`, `AoBOffset`, `InjectionInfo`, `InjInfoLineCount`, `InjInfoRemoveSpaces`, `InjInfoAddTabs`, `AppendToHookName`, `AskForHookName`, `AskForInjectionAddress`, `AllocationSize`, `AllocationNear` |
| Manifold Framework | `HasManifoldCommands`, `HasManifoldTrampolines`, `ScanModule`, `AssertBytes`, `FrameworkWarning`, `TrampolineWarning` |
| Manifold Engine | `CodeContext`: the code around the injection point with markers, as in the AOB injection template |
| Mono | `MonoAvailable`, `MonoIsIl2Cpp`, `MonoIsJitted`, `MonoNamespace`, `MonoClass`, `MonoClassFullName`, `MonoMethod`, `MonoImage`, `MonoDescriptor`, `MonoMethodEntry`, `MonoMethodSize`, `MonoMethodOffset`, `MonoHookName`, `MonoResolve` |

Notes:

- `AoBStr` is a byte pattern of at least 8 bytes that occurs once in the module. When the bytes at the address are not unique, the pattern starts up to 8 instructions earlier and `AoBOffset` says how far, as in `+1C`.
- `BaseAddressRegister` comes from a memory operand like `[rax]` or `[rbx+30]`. For anything else the program asks for the register.
- `HasManifoldCommands` is true when `ManifoldScanModule` is registered as an Auto Assembler command, so it is exact rather than a guess. `ScanModule` and `AssertBytes` fall back to `aobScanModule` and `assert` without the framework.
- Module names with characters the address parser does not read, such as `-`, are written in quotes in `Address` and `InjectionInfo`.
- `MonoAvailable` is true while a Mono or IL2CPP runtime is loaded (Extensions → Unity, or a `mono_*` Lua call). `MonoIsIl2Cpp` is true for an IL2CPP build, from the loaded runtime or, before it is loaded, from `GameAssembly.dll` in the module list.
- The method values (`MonoClass`, `MonoMethod`, `MonoDescriptor`, `MonoResolve` and the others) are still empty, so templates that require `MonoDescriptor` say so instead of generating.

## When a generation fails

Nothing is written into a script until the template rendered completely. A cancelled question ends the generation without a message. Anything else shows the template, the file and line, the value involved and the reason, with an `ME-9xxx` code in the log.
