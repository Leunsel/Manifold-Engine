# Auto Assembler

Auto Assembler scripts write code and data into the opened process. A script in the address list has an `[ENABLE]` and a `[DISABLE]` section: switching the record on runs the first, switching it off runs the second. Text before `[ENABLE]` belongs to both. The Auto Assembler window runs scripts without sections as a whole.

Failures name the line and end in an `ME-3xxx` code; see [ERRORS.md](ERRORS.md).

## Syntax

- Numbers are **hexadecimal** unless written with `#` (decimal): `10` is sixteen, `#10` is ten. `0x` and `$` also mark hex.
- Comments: `// to the end of the line`, `{ a block }` and `/* a block */`.
- An address line ends in a colon and sets where the following lines go: `newmem:`, `game.exe+1234:`, `"game.exe"+1234:`, `$process+1234:`, `INJECT+5:`. A declared label written this way is placed at the current address instead.
- Every other line is an instruction or data, Intel syntax: `mov eax,[rcx+10]`, `jmp return`, `db 90 90`, `dd (float)100.5`, `dq (double)1`, `db 'text',0`, `nop 5`.
- Names that work everywhere: labels and allocations of the script, `aobscan` results, registered symbols, module names (`game.exe`), the executable of the opened process as `mainmodule` or `$process` (as in Cheat Engine), exports (`kernel32.Sleep`, `kernel32!Sleep`).
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
| `aobscanmodule(name, module, bytes)` | Finds the pattern inside one module. `$process` or `mainmodule` as the module is the executable of the opened process. |
| `aobscanregion(name, start, stop, bytes)` | Finds the pattern between two addresses. |
| `resolvestatic(name, address[, options])` | Names the fixed address that the instruction at `address` refers to, such as the global behind `mov rax,[rip+...]`. See [Statics from code](#statics-from-code). |
| `assert(address, bytes)` | Stops the script when the memory does not hold these bytes, before anything is written. |
| `readmem(address, size)` | Writes a copy of the bytes found at `address` into the current position. The bytes are read after the `aobscan` commands and `alloc`, so `address` may be a symbol from a scan of the same script. |
| `reassemble(address)` | Assembles the instruction found at `address` again at the current position, with relative jumps fixed. |
| `fullaccess(address, size)` | Makes memory readable, writable and executable. |
| `createthread(address)` / `createthreadandwait(address)` | Starts a thread in the process after the code is written. |
| `loadlibrary(path)` | Loads a DLL into the process before the code is assembled, so its exports can be used. |
| `include(file)` | Inserts another script file, looked up next to the table or in the autorun folder. |
| `luacall(code)` | Runs Lua code while the script runs. |
| `{$lua}` ... `{$asm}` | A Lua block. A string it returns is inserted as script text. The global `syntaxcheck` is true while a script is only checked. |
| `{$log}` | Writes every step of this script to the log, see [Step log](#step-log). Cheat Engine reads it as a comment. |

## Step log

When a script does not do what it should, the step log shows what each command did. It is off unless you turn it on:

- **For every script**: Settings → Scripts → Log Auto Assembler Steps.
- **For one script**: put `{$log}` anywhere in it, outside comments and strings. It covers the `[ENABLE]` and the `[DISABLE]` part.

Each run writes **one** entry to the Log window (area Auto Assembler). Its line sums the run up, for example `"Infinite Health" [ENABLE]: 2 writes (19 bytes) · 1 allocation · registered INJECT · 9 steps`. A failed run is a warning that names the line and the error. Pick the entry and the details pane shows the steps as a table:

```
line  command                  result
   3  aobscanmodule(INJECT)    found at game.exe+1234 (7FF6...)
   4  alloc(newmem)            1F0000, 4096 bytes, 7FF... from game.exe+1234 (a 5-byte jump reaches)
  12  INJECT:                  code goes to game.exe+1234 (7FF6...)
  13  write                    5 bytes at game.exe+1234: E9 ...  (was 89 83 50 01 00)
```

- A script run from Lua (`autoAssemble`) is named after the record whose script called it.
- A syntax check that passes writes nothing to the log: tables check their scripts before every run, and those entries only buried the runs. A check that fails writes its warning like a run.
- `define`s that a command written in Lua returns are shown with that command, not a second time.
- The log file holds the same: the entry's line, and the table indented below it.
- Address List Debug → Trace keeps the steps of each switch, up to 400 per run.

| Command | What the log says |
| --- | --- |
| `aobscan`, `aobscanmodule`, `aobscanregion` | The pattern, where it was searched and the address it was found at, or that it was not found |
| `alloc`, `globalalloc` | The address, the shared block, the near address and whether a 5-byte jump reaches it; a `globalalloc` that reuses earlier memory says so |
| `dealloc` | The block it freed, a block that stays because other allocations still use it, or a name with nothing allocated |
| `define` | The value, and the one it replaces |
| `assert` | The address and the bytes it found, matching or not |
| `readmem`, `reassemble` | The bytes copied, or the instruction read and where it was placed |
| `resolvestatic` | The instruction it read, the kind of reference (rip-relative, absolute, branch target, number) and the address, plus the pointer with `pointer` |
| `label` and address lines | The address of each label, and where the lines after an address line go |
| Writes | Address, size, the new bytes and the bytes they replaced |
| `registersymbol`, `unregistersymbol` | The address registered, or whether the symbol was there to remove |
| `fullaccess`, `createthread`, `loadlibrary`, `include`, `luacall`, `{$lua}` | What ran, what it returned, the file that was read |
| Commands written in Lua | The parameters after `define`s and the text the command returned |

A syntax check logs too, and says what it leaves out (scans, asserts, Lua calls). When a run fails, the log shows what was put back and freed again before the error.

## Statics from code

A game's globals move with every update, but the code that reads them can be found with a pattern. `resolvestatic` decodes the instruction at an address and names the address that instruction refers to, so the global is found through its code:

```
aobscanmodule(PlayerRead,game.exe,48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 88)
resolvestatic(PlayerBase,PlayerRead)          // the address of the global
resolvestatic(Player,PlayerRead,pointer)      // the pointer stored in it
registersymbol(PlayerBase Player)
```

`PlayerBase` then works like an `aobscan` result: in the code of the script (`mov rax,[PlayerBase]`), in `registersymbol`, and as `alloc`'s near address.

What it reads, found from the decoded instruction and the opened process (a 32-bit game gets 32-bit decoding):

| Instruction | Gives |
| --- | --- |
| `mov rax,[rip+x]`, `lea rcx,[rip+x]`, `movss xmm0,[rip+x]`, `cmp [rip+x],0` | `x`, counted from the next instruction (64-bit) |
| `mov eax,[x]`, `mov eax,[x+ecx*4]`, `mov [x],ecx`, `A1`/`A3` moffs | `x`, written out in full (32-bit, and moffs in 64-bit) |
| `call [rip+x]`, `call [x]` | the import slot `x`; with `pointer` the function |
| `call x`, `jmp x`, `jcc x` | the branch target |
| `mov rax,x`, `push x`, `mov [esi+8],x` | the number `x`, when it points into a module |

The memory operand comes first, then a branch target, then a number. The options after the address are words, in any order:

| Option | Meaning |
| --- | --- |
| `memory`, `branch`, `immediate` | Take that kind of reference only, for example a number that points outside every module. |
| `pointer` (also `deref`, `value`) | Name the pointer stored at the address instead of the address. |
| `rip` / `absolute` | Insist that the reference is counted from the next instruction, or written out; a script that expects the other fails instead of reading a wrong address. |
| `first` | Accept an `aobscan` this command reads from even when its pattern matches more than one place. |
| two numbers | The byte offset of the number inside the instruction and the instruction's length, as `ManifoldResolveStatic` of the table-file framework takes them (decimal, `$` or `0x` for hex). They pick the field and are checked against the decoded instruction; they are never needed. |

It refuses, with the line and a code, instead of naming a wrong address:

- `ME-2019`: no instruction at the address (unreadable, or bytes that are no code).
- `ME-2020`: the instruction names no fixed address: no operand, a register-based one (`[rax+8]`), one counted from `fs:`/`gs:` (the thread block), or a number that is no address.
- `ME-2021`: several references of one kind; `ME-2022`: options the instruction does not have (`rip` on an absolute operand, a length the instruction is not).
- `ME-2023`: the address is not mapped memory; `ME-2025`: `pointer`, and the pointer cannot be read.
- `ME-2024`: the instruction is a `jmp` out of every module, a hook. That happens when the same script ran before and its `[DISABLE]` part did not: the "static" would be the hook's code cave.
- `ME-3115`: the `aobscan` it reads from matches a second place, which may hold another static. The scan looks for a second match only for this reason.

A syntax check reads nothing: the name is 0 until the script runs, like an `aobscan` result. In Lua the same is `manifold.memory.resolveStatic(address, options)`.

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
