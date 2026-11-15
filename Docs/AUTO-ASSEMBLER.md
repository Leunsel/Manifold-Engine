# Auto Assembler

Auto Assembler scripts write code and data into the opened process. A script in the address list has an `[ENABLE]` and a `[DISABLE]` section: switching the record on runs the first, switching it off runs the second. Text before `[ENABLE]` belongs to both. The Auto Assembler window runs scripts without sections as a whole.

Failures name the line and end in an `ME-3xxx` code; see [ERRORS.md](ERRORS.md).

## Syntax

- Numbers are **hexadecimal** unless written with `#` (decimal): `10` is sixteen, `#10` is ten. `0x` and `$` also mark hex.
- Comments: `// to the end of the line`, `{ a block }` and `/* a block */`.
- An address line ends in a colon and sets where the following lines go: `newmem:`, `game.exe+1234:`, `"game.exe"+1234:`, `INJECT+5:`. A declared label written this way is placed at the current address instead.
- Every other line is an instruction or data, Intel syntax: `mov eax,[rcx+10]`, `jmp return`, `db 90 90`, `dd (float)100.5`, `dq (double)1`, `db 'text',0`, `nop 5`.
- Names that work everywhere: labels and allocations of the script, `aobscan` results, registered symbols, module names (`game.exe`), exports (`kernel32.Sleep`, `kernel32!Sleep`).
- Jumps and calls pick the shortest form. In 64-bit code a target beyond 2 GB becomes a 14-byte absolute jump (16 for `call` and conditional jumps). Allocate near the code to keep the 5-byte form: `alloc(newmem,$1000,INJECT)`.

## Commands

| Command | What it does |
| --- | --- |
| `alloc(name, size[, near])` | Allocates executable memory, near the given address in 64-bit processes. Allocations with the same `near` share one block. |
| `globalalloc(name, size[, near])` | Like `alloc`, but registers the name and keeps the memory; a second run reuses it. |
| `dealloc(name)` | Frees an allocation, also one made by the `[ENABLE]` section of the same record. |
| `label(name, ...)` | Declares labels that are placed later with `name:`. |
| `define(name, text)` | Replaces the word `name` with `text` in every other line. |
| `registersymbol(name, ...)` | Makes a label, allocation or `aobscan` result usable by other scripts, by addresses in the table and by Lua. |
| `unregistersymbol(name, ...)` | Removes registered symbols. |
| `aobscan(name, bytes)` | Finds the first match of a byte pattern in the whole process. `??` or `*` matches any byte, `4?` matches any low nibble. |
| `aobscanmodule(name, module, bytes)` | Finds the pattern inside one module. |
| `aobscanregion(name, start, stop, bytes)` | Finds the pattern between two addresses. |
| `assert(address, bytes)` | Stops the script when the memory does not hold these bytes, before anything is written. |
| `readmem(address, size)` | Writes a copy of the bytes found at `address` into the current position. |
| `reassemble(address)` | Assembles the instruction found at `address` again at the current position, with relative jumps fixed. |
| `fullaccess(address, size)` | Makes memory readable, writable and executable. |
| `createthread(address)` / `createthreadandwait(address)` | Starts a thread in the process after the code is written. |
| `loadlibrary(path)` | Loads a DLL into the process before the code is assembled, so its exports can be used. |
| `include(file)` | Inserts another script file, looked up next to the table or in the autorun folder. |
| `luacall(code)` | Runs Lua code while the script runs. |
| `{$lua}` ... `{$asm}` | A Lua block. A string it returns is inserted as script text. The global `syntaxcheck` is true while a script is only checked. |

## Templates

The memory viewer's context menu (Create script) and the Template menu of the Auto Assembler window produce:

- **AOB injection**: finds the code by a unique byte pattern, so it survives most game updates.
- **Code injection** and **Full injection**: inject at a fixed `module+offset`; the full form checks the original bytes with `assert` and restores them on disable.
- **Cheat table framework** and **Call Lua**: empty `[ENABLE]`/`[DISABLE]` skeletons.

## Example

Health is written by `mov [rbx+150],eax`, six bytes. The script writes 1000 instead:

```asm
[ENABLE]
aobscanmodule(INJECT,game.exe,89 83 50 01 00 00 48 8B 5C 24 30) // should be unique
alloc(newmem,$1000,INJECT)
label(code)
label(return)

newmem:
  mov eax,#1000
code:
  mov [rbx+150],eax
  jmp return

INJECT:
  jmp newmem
  nop
return:
registersymbol(INJECT)

[DISABLE]
INJECT:
  db 89 83 50 01 00 00

unregistersymbol(INJECT)
dealloc(newmem)
```

`jmp newmem` takes five bytes; the `nop` fills the sixth, so `return` lands on the next whole instruction.

## Cheat Engine habits that work here

- `label(a b c)`, `registersymbol(a b)`, `unregistersymbol(a b)` and `dealloc(a b)` take names separated by commas or spaces.
- `unregistersymbol(*)` and `dealloc(*)` in `[DISABLE]` remove everything the `[ENABLE]` part of the same script registered or allocated.
- `registersymbol(name)` works for a `define`d name and registers the define's address.
- A `define` applies to the lines after it, so a command written in Lua (`registerAutoAssemblerCommand`) receives defined names already replaced, including names a command before it returned as `define(...)`.
- Anonymous labels: `@@:` places one, `@f` jumps to the next and `@b` to the previous.
- `(int)` next to `(float)` and `(double)`, and a memory operand without a size next to a number is a dword: `mov [rbx+FC],(int)0`.
- Module names with dashes need no quotes: `jmp qword ptr [Game-Win64-Shipping.exe+506]`.
