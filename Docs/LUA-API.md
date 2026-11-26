# Lua API

Manifold Engine runs Lua 5.4 with the standard libraries and the functions below. Names follow Cheat Engine where a function exists there, so most table scripts run unchanged. Addresses can be numbers or address expressions such as `"game.exe+10"` or `"[[base]+8]+4"`.

Manifold Engine's own API, with one fixed naming and calling schema that every call is checked against, is the `manifold` namespace in [MANIFOLD-API.md](MANIFOLD-API.md): coloured log levels, process and typed memory helpers that report failures as error objects, and protected calls.

Scripts run on the program's main thread: in the Lua engine window (Ctrl+L), in `{$lua}` blocks of Auto Assembler scripts, from the table's Lua script, and from the scripts and modules in the `autorun` folder at start ([AUTORUN.md](AUTORUN.md)). Errors end in `ME-7xxx`; see [ERRORS.md](ERRORS.md).

## Process

| Function | Returns |
| --- | --- |
| `openProcess(pidOrName)` | true when the process was opened |
| `getOpenedProcessID()` | pid, or 0 |
| `getProcessIDFromProcessName(name)` | pid or nil |
| `getProcessList()` | table pid → name |
| `getForegroundProcess()` | pid of the foreground window |
| `targetIs64Bit()` | boolean |
| `enumModules()` | list of `{ Name, Address, Size, Is64Bit, PathToFile }` |
| `getModuleSize(name)` | size or nil |
| `onOpenProcess(pid)` | define this global function to be called after a process is opened |

## Addresses and symbols

| Function | Returns |
| --- | --- |
| `getAddress(expression)`, `getAddressFromName` | number; raises an error for unknown names |
| `getAddressSafe(expression)` | number or nil |
| `getNameFromAddress(address)` | `module.export+offset`, `module+offset` or hex |
| `registerSymbol(name, address)` / `unregisterSymbol(name)` | boolean |

## Memory

| Function | Notes |
| --- | --- |
| `readBytes(address, count, asTable)` | several values, or one table when `asTable` is true |
| `writeBytes(address, b1, b2, ...)` / `writeBytes(address, table)` | |
| `readByte`, `readSmallInteger(address, signed)`, `readInteger(address, signed)`, `readQword`, `readPointer`, `readFloat`, `readDouble` | nil when unreadable |
| `writeByte`, `writeSmallInteger`, `writeInteger`, `writeQword`, `writePointer`, `writeFloat`, `writeDouble` | true when written |
| `readString(address, maxLength, wide)` / `writeString(address, text, wide)` | UTF-8 in Lua, UTF-16 in memory when `wide` |
| `allocateMemory(size, near, protection)` / `deAlloc(address)` | |
| `fullAccess(address, size)` | |
| `executeCode(address, parameter)` | runs a thread in the process and waits for it |
| `injectDLL(path)` / `injectLibrary(path)` | |

## Scanning, code and scripts

| Function | Returns |
| --- | --- |
| `AOBScan(pattern, protection, alignmentType, alignmentParam)` | table with `Count`, entries `[0]`..`[Count-1]` as hex strings and `destroy()`, or nil. `protection` works like Cheat Engine's: `+X` executable only, `-W` not writable, `*C` copy-on-write does not matter, empty or nil for all memory. `alignmentType` 1 keeps addresses divisible by the hex `alignmentParam`, 2 keeps addresses that end in its digits. |
| `AOBScanUnique(pattern, protection, alignmentType, alignmentParam)` / `AOBScanModuleUnique(module, pattern, protection, ...)` | address when there is exactly one match, else nil |
| `autoAssemble(script, targetself, disableInfo)` | `true, disableInfo` after the `[ENABLE]` part (or the whole script); with a `disableInfo` it runs `[DISABLE]` with that script's allocations and symbols and returns `true`. On failure `false, error`; the error also goes to the log. `targetself` is not supported. |
| `autoAssembleCheck(script, enable)` | `success, error` without writing anything |
| `disassemble(address)` | `"address - bytes - opcode : extra"` |
| `disassembleBytes(bytes, address)` | the same line for the first instruction in `bytes` (a byte table or a hex string such as `"48 8B 4C 24 08"`) as if it stood at `address`; no process needed |
| `splitDisassembledString(text)` | `extra, opcode, bytes, address`. As in Cheat Engine, `extra` is not the text after ` : ` but a guess at the value the instruction refers to, read from the process: `[12345678]`, `[(float)1.5000]`, `[(double)0.0078]`, `["text"]`, `[module+offset]`, `[->module.export]` for a branch to a `jmp [pointer]`; empty without a process or when the address is not committed |
| `getInstructionSize(address)` / `getPreviousOpcode(address)` | |

## Address list

### The list

`getAddressList()` and the global `AddressList` return the table's list:

- `Count`, `SelCount`.
- `[index]` and `MemoryRecord[index]`: 0-based, all records in tree order.
- Lookups: `getMemoryRecord(i)`, `getMemoryRecordByDescription(text)`, `getMemoryRecordByID(id)`.
- `createMemoryRecord()`.
- The selection: `getSelectedRecords()`, `SelectedRecord`, `setSelectedRecord(record)`.
- `disableAllWithoutExecute()`, `refresh()`.

Set `OnDescriptionChange`, `OnAddressChange`, `OnTypeChange`, `OnValueChange` or `OnAutoAssemblerEdit` to `function(addresslist, memrec)`. They run before the matching editor opens; return true to keep the editor closed.

### Memory records

Every field also has a `getX()`/`setX(v)` pair. Fields a script sets that are not listed here stay on the record, so `memrec.MyFlag = true` works.

| Group | Fields |
| --- | --- |
| Identity | `ID`, `Index`, `Description`, `Selected` |
| Address | `Address`, `AddressString`, `CurrentAddress`, `OffsetCount`, `Offset[i]` and `OffsetText[i]` (last offset first, like Cheat Engine) |
| Type and value | `Type`/`VarType` (`vtByte` .. `vtCustom`), `CustomTypeName`, `Value`, `NumericalValue`, `String.Size`, `String.Unicode`, `Aob.Size`, `Binary.Startbit`, `Binary.Size` |
| State | `Active`, `Script`, `Async`, `AllowIncrease`, `AllowDecrease`, `DontSave` |
| Tree | `Count`, `Child[i]`, `[index]`, `Parent`, `IsGroupHeader`, `IsAddressGroupHeader`, `Options` (`"[moHideChildren,...]"`), `Collapsed` |
| Display | `ShowAsHex`, `ShowAsSigned`, `Color` |
| Drop-down list | `DropDownList` (a string list of `value:description` lines), `DropDownLinked`, `DropDownLinkedMemrec`, `DropDownReadOnly`, `DropDownDescriptionOnly`, `DropDownCount`, `DropDownValue[i]`, `DropDownDescription[i]` |
| Hotkeys | `HotkeyCount`, `Hotkey[i]` |

Methods:

- Value and state: `getValue()`, `setValue(v)`, `setActive(on)`, `disableWithoutExecute()`, `delete()`.
- Address: `getAddress()`, `setAddress(text, offsets)`, `getOffset(i)`, `setOffset(i, v)`.
- Tree: `appendToEntry(parent)`, `getChild(i)`.
- Hotkeys: `getHotkey(i)`, `getHotkeyByID(id)`, `createHotkey(keys, action, value, description)`.

Events:

- `OnActivate(memrec, before, currentstate)`: return false while `before` is true to stop the change.
- `OnDeactivate`.
- `OnGetDisplayValue(memrec, text)`: return `true, newText`.
- `OnValueChanged(memrec, old, new)`.
- `OnDestroy()`.

The global functions `onMemRecPreExecute(memrec, newstate)` and `onMemRecPostExecute(memrec, newstate, succeeded)` run around every activation.

### Record hotkeys

A record hotkey has `ID`, `Description`, `Keys`, `Action` (`mrhToggleActivation` .. `mrhDecreaseValue`), `Value`, `Owner`, `doHotkey()`, `getKeys()`, `setKeys(...)`, `destroy()`.

## Timers, hotkeys and threads

```lua
local t = createTimer(nil, false)
t.Interval = 100
t.OnTimer = function(sender) print("tick") end
t.Enabled = true
-- t.destroy()

createTimer(2000, function() print("once, after two seconds") end)

local key = createHotkey(function() print("F2 pressed") end, VK_F2)
-- key.destroy()

createThread(function()
  for i = 1, 5 do
    print(i)
    sleep(1000) -- yields; the window keeps running
  end
end)
```

Timers, hotkeys and threads run between frames on the main thread, also while the window is in the background. A timer that raises an error is switched off.

Callbacks that the program waits for (timers, hotkeys, menu and form events, symbol lookups, custom types, breakpoint functions) have a time limit, 2 seconds unless Settings → Scripts says otherwise. One that runs longer is stopped with an error instead of freezing the program. Long work belongs in `createThread`, the table script or the Lua engine window, which have no limit and can be stopped there.

Inside a thread, a table script or a form event, `sleep(ms)` and `processMessages()` give the frame back and continue later. Elsewhere, for example in the Lua engine window or a timer, `sleep` waits without the window being marked as not responding, and `processMessages()` runs the timers that are due, so a loop like `repeat processMessages() until done` ends when a timer sets `done`.

## Debugger

| Function | Notes |
| --- | --- |
| `debugProcess()` | attaches the debugger |
| `debug_setBreakpoint(address[, size, trigger][, function])` | trigger: `bptExecute`, `bptWrite`, `bptAccess`. The function runs when it hits; registers are the globals `RAX`..`R15`, `RIP`, `EFLAGS` (`EAX`.. in 32-bit processes). Changed globals are written back. Return true to continue at once. |
| `debug_removeBreakpoint(address)` | |
| `debug_continueFromBreakpoint(method)` | `co_run`, `co_stepinto`, `co_stepover`, `co_runtill` |
| `debug_isBroken()`, `debug_isDebugging()`, `debug_getBreakpointList()` | |
| `debug_getContext(extraregs)` / `debug_setContext(extraregs)` | copies the registers of the stopped thread to the globals and back; with `extraregs` true also `XMM0`… (tables of 16 bytes) and `MXCSR` |
| `debug_breakThread()` | stops the process |

## Interface and helpers

| Area | Functions |
| --- | --- |
| Messages | `print(...)`, `showMessage(text)`, `messageDialog(text, type, buttons...)` (returns `mrYes`, `mrNo`, `mrOk`, `mrCancel`), `inputQuery(caption, prompt, default)` (text or nil) |
| Windows | `showMemoryViewer(address)`, `showStructureDissect(address)` |
| Time and keys | `sleep(ms)`, `getTickCount()`, `isKeyPressed(vk)` |
| Program | `getCheatEngineDir()`, `getAutorunPath()`, `getCEVersion()`, `getManifoldVersion()` |
| Threads | `inMainThread()`, `synchronize(f, ...)`, `queue(f, ...)` |
| Text | `intToHex(value, digits)` |

Byte tables: `byteTableToString`, `byteTableToWideString`, `stringToByteTable`, `wideStringToByteTable`, `wordToByteTable`, `dwordToByteTable`, `qwordToByteTable`, `floatToByteTable`, `doubleToByteTable`, `byteTableToWord`, `byteTableToDword`, `byteTableToQword`, `byteTableToFloat`, `byteTableToDouble`.

Bit helpers for old scripts: `bAnd`, `bOr`, `bXor`, `bNot`, `bShl`, `bShr` (Lua 5.4 also has `&`, `|`, `~`, `<<`, `>>`).

Old Lua names that Cheat Engine adds at startup, defined the same way: `loadstring` (= `load`), `unpack` (= `table.unpack`), `package.loaders` (= `package.searchers`), `string.gfind` (= `string.gmatch`), `math.pow`, `math.log10`, `math.ldexp`, `math.frexp`, `math.atan2`, `math.mod`, `math.cosh`, `math.sinh`, `math.tanh`, plus `module(name, ...)` with `package.seeall` for old-style modules and `string.trim`. Like in Cheat Engine there is no `bit32`, `table.getn`, `table.maxn`, `setfenv` or `getfenv`.

Key codes: `VK_F1`..`VK_F24`, `VK_0`..`VK_9`, `VK_A`..`VK_Z`, `VK_NUMPAD0`..`VK_NUMPAD9`, `VK_SHIFT`, `VK_CONTROL`, `VK_MENU`, `VK_SPACE`, `VK_RETURN`, `VK_ESCAPE`, arrows and the other common keys.

## Cheat Engine compatibility

Tables and autorun scripts written for Cheat Engine 7.5 run here. Beyond the functions above, the program provides what follows.

### Language and globals

- All constants of Cheat Engine's `defines.lua` (`vt*`, `so*`, `rt*`, `fsm*`, `mrh*`, `mr*`, `cl*`, `al*`, `bs*`, `ss*` and the rest).
- Member names in any case (`memrec.description` is `memrec.Description`), and methods called with a dot or a colon (`stream.readByte()` and `stream:readByte()`).
- Old `class_method` globals such as `memoryrecord_getDescription(mr)` or `component_getComponent(c, i)` call the method.
- The globals `process` (the module name of the opened process; `manifold.process.name()` reads the same name when it is called), `memrec` and `syntaxcheck` inside `{$lua}` blocks and Auto Assembler commands, `MainForm` and `AddressList`.
- Reads, writes and `enumModules()` without an opened process give nil, false or an empty table instead of an error.
- Module names with dashes in addresses: `Game-Win64-Shipping.exe+10` and `Game-Win64-Shipping+10`.
- The string helpers `split` (the parts as several values), `startsWith` and `endsWith`.

### Table files and streams

- Table files: `findTableFile(name)`, `createTableFile(name, path)`, with `Name`, `Stream`, `getData()`, `saveToFile(path)`, `delete()`. Files attached to a table are kept when it is saved.
- Streams: `createMemoryStream()`, `createStringStream(text)`, `createFileStream(path)`.
  - Properties: `Size`, `Position`, `Memory`, `DataString`.
  - Methods: `read(n)`, `write(bytes)`, `readByte` .. `readQword`, `writeByte` .. `writeQword`, `readString`, `writeString`, `readAnsiString`, `writeAnsiString`, `copyFrom`, `loadFromFile`, `saveToFile`, `clear`.
- Tables: `loadTable(file or stream, merge)` and `saveTable(file or stream)`.
  - With `merge` the table loads at once, otherwise on the next frame, so a script can replace the table it runs in.
  - A file that does not exist returns `nil` and the reason at once.
  - A stream's content is taken at the call, so the stream may be destroyed right after.

### Calls into the open process

| Function | |
| --- | --- |
| `executeCode(address, parameter)` | one parameter |
| `executeCodeEx(callmethod, timeout, address, ...)` | up to 32 parameters |
| `executeMethod(callmethod, timeout, address, instance, ...)` | a method of an object |

- `callmethod` is 0 for stdcall and 1 for cdecl (64-bit code has one convention).
- `timeout` is in milliseconds; `nil` waits without limit and 0 does not wait.
- A parameter is a number, a text (passed as a pointer to it), or `{type=, value=}` with type 0 integer, 1 float, 2 double, 3 text, 4 wide text.
- The instance is a number, or `{regnr=0..15, classinstance=}` to pass it in another register than ECX.
- They return the integer result, or `nil` and the reason.

### Memory

- `copyMemory(source, size, destination, method)`: method 0 target to target, 1 target to Manifold, 2 Manifold to target, 3 inside Manifold. Without a destination it allocates one and returns it.
- `compareMemory(a, b, size, method)`: `true`, or `false` and the offset of the first difference.
- `setMemoryProtection(address, size, {R=, W=, X=})`, `readRegionFromFile(file, address)`, `writeRegionToFile(file, address, size)`, `allocateSharedMemoryLocal(name, size)`, `createRemoteThread(address, parameter)`.
- Memory of the program itself: `readByteLocal` .. `readDoubleLocal`, `readStringLocal`, `readBytesLocal`, the matching `write*Local` functions, `executeCodeLocal(address, parameter)`, `executeCodeLocalEx(address, ...)`, and `getAddress(name, true)` for exports of loaded system DLLs (`"kernel32.GetTickCount"`).
- `getUniqueAOB(address)` returns a byte pattern that occurs once in all of the process's readable memory, which is what `AOBScan` searches, and the offset from the match to the address (`match + offset = address`). It uses the signature maker's measurement and masking: branch distances, displacements, addresses and large immediates are `??`, so the pattern survives a restart of the game. The pattern is at least 8 bytes long and may start at an instruction up to 128 bytes before or after the address, so the offset can be negative. It returns nil when no pattern within 96 bytes is unique. One call takes about 0.1 to 0.5 s in a game, because it reads all memory.

### Code and symbols

- `assemble(line, address)` returns the bytes of one instruction (or `nil, error`); `setAssemblerMode(0 or 1)` makes it assemble 32- or 64-bit code.
- `getComment(address)`, `setComment(address, text)`, `getHeader(address)`, `setHeader(address, text)` share the memory viewer's comments.
- `getSymbolInfo(name)` (`modulename`, `searchkey`, `address`, `size`) and `enumRegisteredSymbols()` (`symbolname`, `address`).
- `errorOnLookupFailure(state)`: when false, `getAddress` returns 0 for unknown names. Returns the old state.
- `reinitializeSymbolhandler()` reads the module list again.
- `getRTTIClassName(address)`: the MSVC class name of an object, `Namespace::Class`.
- Disassemblers: `createDisassembler()`, `getDefaultDisassembler()` and `getVisibleDisassembler()` give an object whose `disassemble(address)` returns the line and fills `LastDisassembleData` (`address`, `opcode`, `parameters`, `bytes`, `isJump`, `isCall`, `isRet`, `isConditionalJump`, `parameterValue` of jumps and calls). Disassemble overrides are not called.
- **Symbol lookup callbacks**: `registerSymbolLookupCallback(function(name) return address end, location)` resolves names the program does not know.
  - With `slStart` the function is asked before anything else; with any other location (`slFailure` is the default) only when every other lookup failed.
  - It is called on the Lua thread only, and not again while one of these functions runs.
  - Returns an ID for `unregisterSymbolLookupCallback(id)`.

### Auto Assembler commands and custom types

- `registerAutoAssemblerCommand(name, function(parameters, syntaxcheck))` returns the text that replaces the line, or `nil, error`; `unregisterAutoAssemblerCommand(name)` removes it. While it is registered, the script editors colour the command as a macro (Settings → Appearance → Code: Macros) where it starts a line, as the assembler only expands it there.
- `registerCustomTypeLua(name, bytecount, bytesToValue, valueToBytes, isFloat, isString)`, `getCustomType(name)`, `getCustomTypeList()`.
  - Records and structure elements whose type is a custom type read, show and write through the two functions.
  - With `isString` the first one returns text and the second one receives it.

### Structures

- `getStructureCount()`, `getStructure(i)`, `createStructure(name)`, `createStructureForm(address, group, name)` (`name` selects that structure).
- A structure has `Name`, `Size`, `Count`, `Element[i]`, `getElement(i)`, `getElementByOffset(o)`, `addElement()`, `addToGlobalStructureList()`, `removeFromGlobalStructureList()`, `destroy()`, and `autoGuess(address, offset, size)`, which fills `size` bytes from `offset` on the way Structure Dissect guesses, replacing the elements in that range.
- An element has `Offset`, `Name`, `Vartype`, `Bytesize`, `ChildStruct`, `DisplayMethod`, `CustomType`, `CustomTypeName`.

**Structure Dissect callbacks**:

- `registerStructureNameLookup(function(address) return name, start end)` names a new structure and may move its start. It is also asked when a pointer is expanded, and a structure of that name is then reused.
- `registerStructureDissectOverride(function(structure, address) return true end)` fills a new structure instead of the guess.
- Both return an ID for `unregisterStructureNameLookup(id)` and `unregisterStructureDissectOverride(id)`.
- They run in the background like a thread, spread over frames, so a slow lookup does not hold the window: Structure Dissect opens at once and fills in the name and the fields when the callback returns.

### Scanning

- `createMemScan()` with `firstScan`, `nextScan`, `waitTillDone`, `setOnlyOneResult`, `getOnlyResult`, `FoundList`, and `createFoundList(memscan)` with `Count`, `Address[i]`, `Value[i]`.
- `firstScan` takes `vtAll` (the types ticked under Settings → Scanner → Types for All), `vtGrouped` (the value is a group like `4:100 f:1.5 d:*`, see [Grouped](FEATURES.md#grouped)) and `vtCustom` (set `memscan.CustomType` to a custom type or its name first).
- `saveCurrentResults(name)` keeps the current results under a name and `getSavedResultList()` returns the names. `nextScan` compares with saved results when its 10th argument (after `ispercentagescan`) is such a name.

### Files, system and processes

| Area | Functions |
| --- | --- |
| Files | `lfs` (`attributes`, `dir`, `mkdir`, `rmdir`, `currentdir`, `chdir`, `touch`), `getFileList`, `getDirectoryList`, `extractFileName`, `extractFileNameWithoutExt`, `extractFilePath`, `extractFileExt`, `getFileVersion`, `fileExists(path)`, `deleteFile(path)` (files only), `getTempFolder` |
| Hashes | `md5file`, `md5memory`, `stringToMD5String` |
| Clipboard and internet | `writeToClipboard`, `readFromClipboard`, `getInternet()` with `getURL(url)` and `postURL(url, data)` |
| Screen | `getScreenWidth`, `getScreenHeight`, `getWorkAreaWidth`, `getWorkAreaHeight`, `getScreenDPI`, `getMousePos`, `getSystemMetrics(index)` |
| Programs | `shellExecute(command, parameters, folder, show)` (also spelled `ShellExecute`), `runCommand(exe, parameters or {list}, folder)` (output and exit code, waits for the program), `createProcess(path, parameters, debug)` (starts and opens the process; `debug` attaches the debugger afterwards, `breakonentrypoint` is not supported) |
| The opened process | `pause`, `unpause`, `enumMemoryRegions`, `inModule`, `inSystemModule`, `deleteAllRegisteredSymbols`, `getOpenedProcessHandle()`, `getPointerSize()` (of the opened process, 8 without one), `targetIsX86()`, `targetIsArm()`, `targetIsAndroid()` |
| The program | `cheatEngineIs64Bit`, `closeCE`, `getCheatEngineFileVersion()` (7.5.0.7431, the version the API follows), `getOperatingSystem()` (0) |
| Machine | `getCPUCount()`, `cpuid(eax, ecx)` (`EAX`..`EDX`, also lower case) |
| Windows | `getUserRegistryEnvironmentVariable(name)` / `setUserRegistryEnvironmentVariable(name, value)`, `sendMessage(hwnd, msg, wparam, lparam)` (waits at most five seconds for the other window and returns 0 when it does not answer), `outputDebugString(text)` |
| Lists | `getProcesslist([strings])`, `getWindowlist([strings])` (a table `pid → {captions}` without a list), `getThreadlist([list])` (thread IDs of the opened process); with a string list they fill it in Cheat Engine's `%08X-name` / `%X` form |
| Auto attach | `getAutoAttachList()`: names added to it are opened as soon as such a process runs and none is open, as with Cheat Engine's auto attach. The list lasts for the session and is not added to the settings |

**Stored settings**: `getSettings(path)` returns an object whose `Value[name]` reads and writes text, `""` when nothing is stored. Cheat Engine keeps these in the registry; here they go into `configs\Manifold-Engine.scripts.ini` next to the program, one section per path.

### Input and sound

- Mouse: `setMousePos(x, y)`, `mouse_event(flags, x, y, data, extra)`, `getPixel(x, y)`.
- Keys: `keyDown(vk)`, `keyUp(vk)`, `doKeyPress(vk)` (holds the key 50 ms; inside `createThread` the thread waits instead of the program).
- Sound: `beep()`, `playSound(stream or tablefile or path, wait)` (WAV), `speak(text, wait or flags)` and `speakEnglish(text, wait)` (Windows speech, in order, one after the other).
- Controllers: `getXBox360ControllerState(id)` with Cheat Engine's fields, `setXBox360ControllerVibration(id, left, right)`.

### Conversions

- `ansiToUtf8`, `utf8ToAnsi`.
- `byteTableToExtended`, `extendedToByteTable` (80-bit floats).
- `signExtend(value, bit)`, `readShortInteger`/`writeShortInteger` (8 bit), `invertColor(color)`.
- `convertKeyComboToString(keys)` (like the hotkey list), `shortCutToText(shortcut)` and `textToShortCut(text)` (Lazarus shortcut numbers: key + `0x2000` Shift, `0x4000` Ctrl, `0x8000` Alt).

### Objects and threads

- References: `createRef(...)`, `getRef(id)`, `destroyRef(id)`; `setGlobalVariable`/`getGlobalVariable`.
- Synchronisation: `createCriticalSection`, `createEvent(manualReset, signalled)` (`waitFor(timeout)` returns `wrSignaled` or `wrTimeout`), `createSemaphore(count)`, `createMultiReadExclusiveWriteSynchronizer()`. Scripts share one Lua state, so these only wait inside `createThread`; elsewhere a wait that cannot succeed returns at once.
- Threads: `createThreadSuspended(f, ...)` starts on `thread.resume()`, and `createThreadNewState(script)` runs the script as a thread in the same state.

### Scripts and windows

- `getNextAllocNumber(script)`, `translateID(id, text)` (returns the text).
- `loadModule(path)` injects the DLL.
- `createAutoAssemblerForm(script)` opens an Auto Assembler window with the script; `createMemoryView()` is the memory viewer.
- `createClass(name)` and `createComponentClass(name, owner)` (`"TButton"`, `"TStringList"` and the other classes), `getPropertyList(class)`.

### What is missing

[CE-COMPAT.md](CE-COMPAT.md) lists the rest of Cheat Engine's functions:

- Some exist without an effect, for example `waitForExports`, `setProgressState`, `hideAllCEWindows`, stand-ins from `getSettingsForm` and `getFreezeTimer`, and placeholder controls such as `createCalendar`.
- The others are not available: kernel driver, DBVM, compilers, SQL, XML and a few more. Calling one of those does nothing, returns nil (or false or an id, as listed), and writes one warning per function to the Lua output, with the script and line. They are not real globals, so `rawget(_G, name)` stays nil for scripts that check before they call.

The documentation panel of the Lua engine window marks both kinds.

### Forms

The form functions of Cheat Engine exist:

- Controls: `createForm`, `createLabel`, `createEdit`, `createMemo`, `createButton`, `createCheckBox`, `createComboBox`, `createListBox`, `createListView`, `createTreeView`, `createPanel`, `createGroupBox`, `createRadioGroup`, `createImage`, `createPaintBox`, `createSplitter`, `createProgressBar`, `createTrackBar`.
- Menus: `createMainMenu`, `createPopupMenu`, `createMenuItem`.
- Resources: `createImageList`, `createBitmap`, `createPNG`, `createPicture`, `createStringlist`, `createFont`, and the dialogs.
- Lookups: `getMainForm()`, `getLuaEngine()`, `getMemoryViewForm()`, `getForm(i)`, `getFormCount()`, `getApplication()`.
- Others: `registerFormAddNotification`, `setMethodProperty`, `inheritsFromObject` and the other `inheritsFrom*` checks.

Their objects behave like Cheat Engine's: properties, string lists, menus, list and tree items, canvases, owners and parents, `findComponentByName`, events.

- `tostring(object)` gives the class and a number unique to the object (`TPanel: 0000000010000A40`), so modules can key tables by it.
- The main form has Cheat Engine's menu, and its Table menu lists the table's files when clicked.

#### Layout

Forms made with `createForm` are drawn as their own windows. Controls are laid out the way Lazarus does it:

- `Align` (`alTop`, `alBottom`, `alLeft`, `alRight`, `alClient`), `BorderSpacing` (`Around` plus each side), `Constraints`, the bevel of panels, and `AutoSize` for labels, edits, combo boxes and check boxes.
- Among controls with the same `Align` and the same position, the one that was added or aligned last comes first, so alTop panels created in a loop end up in reverse order, as in Cheat Engine.
- Scripts read the resulting `Left`, `Top`, `Width` and `Height`, and the layout follows when the window is resized.

#### Controls that are drawn

| Control | What is honoured |
| --- | --- |
| Labels | colour, `Font` name, size, colour and bold, `Alignment`, `Layout`, `WordWrap` |
| Panels | colour, bevel, caption |
| Buttons, toggle and check boxes | |
| Edits | `TextHint`, `PasswordChar`, `ReadOnly`, `BorderStyle` |
| Memos, list boxes, combo boxes, group boxes | |
| Splitters | dragging resizes the control next to them |
| Tree views | nodes, expanding, selection, the arrow keys, `Home`, `End` and `Enter` |
| List views | `vsReport` with columns, their `Width`, `Alignment` and `Visible`, `GridLines`, `CheckBoxes`, `ShowColumnHeaders` and sub-items; the other styles as a plain list. They fire `OnSelectItem`, `OnChange`, `OnItemChecked` and `OnColumnClick` |
| Paint boxes, images | what scripts paint on their `Canvas` (below) |

Other controls (page controls, track bars) keep their place as a marked empty box, and the log names them once.

#### Menus and events

- A form's `Menu` appears as its menu bar, and a control's `PopupMenu` (or its parent's) opens on a right click, with `OnPopup` first.
- `OnClick`, `OnDblClick`, `OnChange`, `OnSelectionChange`, `OnSelectionChanged`, `OnMouseEnter`, `OnMouseLeave`, `OnMoved` and `OnClose` fire as in Cheat Engine, and `Cursor = crHandPoint` shows the hand.
- Paint boxes and other controls also get `OnMouseDown`, `OnMouseUp` (button, x, y), `OnMouseMove` (x, y), `OnMouseWheelUp`/`OnMouseWheelDown` (or `OnMouseWheel`) and `OnResize`.
- A form with `OnKeyDown` gets the virtual key code while it has the focus.

#### Canvas

- Drawing: `line`, `moveTo`/`lineTo`, `rect`, `fillRect`, `frameRect`, `ellipse`, `roundRect`, `polygon`, `polyline`, `setPixel`, `textOut` (on an opaque box unless `Brush.Style` is `bsClear`), `clear`, `draw` and `stretchDraw` of a bitmap.
- With `Pen` (colour, width, `psClear`), `Brush` (colour, `bsClear`) and `Font`.
- The calls are recorded and replayed by the graphics card every frame, so a bitmap filled in a timer and drawn in `OnPaint` costs no copy.
- `OnPaint` runs when the paint box is shown, resized or asked to with `repaint()`/`invalidate()`. An opaque `fillRect` from the top left corner that covers the last frame starts a new one.

#### Look

By default these windows take the look of the rest of the program: the colours, fonts and font sizes that scripts set are left out (what they paint on a canvas keeps its colours), and panels and group boxes are drawn as cards. Settings → Scripts → "Script Windows in App Style" turns this off and draws them with the script's own colours and fonts.

`showModal()` does not open a window yet.

#### The memory viewer form

The memory viewer is `getMemoryViewForm()`:

- `DisassemblerView.SelectedAddress` is the selected instruction of the memory viewer used last; setting it moves that viewer there without raising it.
- `HexadecimalView.Address` moves the hex view; `SelectionStart` and `SelectionStop` mark a range of bytes.
- `show()` and `bringToFront()` open or raise the viewer, and `Visible` says whether one is open.
- Menu items added to the `debuggerpopup` menu appear at the end of the disassembler's context menu; its `OnPopup` runs before the menu opens.
- Root items added to the form's `Menu` appear in the viewer's menu bar after Tools, with their sub-items. `Caption` (`-` is a separator), `Visible`, `Enabled`, `Checked`, `AutoCheck`, `Shortcut` and `OnClick` are honoured; a root item's `OnClick` runs when its menu opens, as in Cheat Engine.
- The `Shortcut` of a menu bar item works while the memory viewer has the focus. When the key is one of the viewer's own keys that Cheat Engine 7.5 also uses, the viewer's command keeps it and the log says so. Over the viewer's other keys (such as `Ctrl+Shift+F` for Find Assembly Code) the script's item wins, as it would in Cheat Engine.

#### The main form

The same goes for `MainForm.Menu`:

- Root items that scripts add appear in the main window's Extensions menu.
- Items added to Cheat Engine's own menus (`MainForm.miFile`, `miEdit`, `miTable`, `miHelp`) appear at the end of Manifold's File, Edit, Table and Help menus.
- Their shortcuts work in the main window unless one of its own commands uses the same keys.

#### The Lua engine form

The Lua engine is `getLuaEngine()`: `cbShowOnPrint.Checked` (and `State`) is the window's *Show on Print* switch. Setting it from a script, as many tables do to keep the window closed, lasts until the program closes; the saved setting stays as the user left it.

## Mono / .NET

For a Unity game, Mono-based (one with `mono-2.0-bdwgc.dll` or `mono.dll`) or IL2CPP (one with `GameAssembly.dll`), the runtime is read through a small in-process worker, without injecting a native DLL. On Mono the class list comes from a small managed dumper that Manifold loads into the game's root domain (it stays loaded until the game ends), with the `mono_*` C functions as the fallback. As in Cheat Engine, the same `mono_*` functions serve both runtimes. On IL2CPP these functions read metadata only: there is no JIT, every method already has its code in `GameAssembly.dll`, and they run no managed code in the game.

| Function | Result |
| --- | --- |
| `mono_isPresent()` | `true` when a Mono or IL2CPP runtime is attached. |
| `mono_isIl2Cpp()` | `true` when the game is an IL2CPP build: the attached runtime, or before one is attached, `GameAssembly.dll` in the module list. Starts nothing in the game. |
| `mono_initialize()` | Finds and attaches the runtime of the open process (IL2CPP first when the game has `GameAssembly.dll`, then Mono); `true` on success. The other functions call it on demand, so it rarely needs calling directly. |
| `mono_refresh()` | Reads the classes again (after the game has loaded more assemblies). |
| `mono_enumClasses()` | A list of `{ namespace, name, fullname, image }`, one per class. Image names come without `.dll`. |
| `mono_class_getMethods(class)` | A list of the method names of the class (`"Player"`, `"Game.Player"` or `"Assembly-CSharp:Game.Player"`), or `nil` when the class is unknown. |
| `mono_class_getFieldOffset(class, field)` | The byte offset of an instance field (from the object's start), or `nil`. |
| `mono_getAddress(class, method)` or `mono_getAddress("Class:Method")` | The address of the method's code, or `nil`: compiled on first use for Mono, read from the method for IL2CPP (`nil` for a method without code of its own, such as an abstract one). The same names resolve in address expressions and the Auto Assembler, also with the assembly in front: `Assembly-CSharp:Player:TakeDamage`. |
| `mono_showDissector([class])` | Opens the Mono / .NET window (scene and object explorer, class list, inspector), so an autorun script can offer it from its own menu the way the Unreal Engine script does. With a class name (`"Player"`, `"Game.Player"` or `"Assembly-CSharp:Game.Player"`) it opens on that class in the Classes tab. |
| `mono_shutdown()` | Ends the helper thread in the game and takes the Unity explorer's per-frame callback out, without closing the process. Mono names stop resolving until `mono_initialize()` (or the menu's Load) runs again. |

When Mono or IL2CPP is loaded, Structure Dissect uses it automatically: dissecting the address of a managed object names the structure by its class and fills the fields (names, offsets and types) from the runtime, and following a pointer to another object looks that one up too. No script or manual structure is needed. On IL2CPP an object of a class that no assembly lists (a generic instance such as `List<int>`) is named from its class's memory only and its fields are guessed, because such a class pointer is never handed to the runtime.

The scene explorer, the inspector and `mono_showDissector()`'s Scenes and Objects tabs run managed code on the game's main thread. On Mono they go through Manifold's managed agent, which hangs on the game's per-frame callback (`Application.onBeforeRender`) and answers requests a few milliseconds per frame at most. On IL2CPP a small native stub sits on a per-frame Unity method instead and runs the requests as `il2cpp_runtime_invoke` calls. Shut down takes either back out.

## Checking a table without the program window

The test program runs a table the way the program does, without a window, and prints every Lua error:

```
Manifold-Engine.Tests.exe --compat "Game.CT" --autorun="C:\path\to\autorun" --check-scripts --seconds=5
```

`--autorun` loads a folder as the autorun folder first (loose scripts and `modules\`, without a state file, so nothing is held). `--process=name.exe` opens a process. `--check-scripts` checks every Auto Assembler script of the table without writing to the process. `--eval` runs Lua code, or a `.lua` file, at the end. Files the scripts write under the user profile go to a separate folder (`--home=`).

`--dry-run` lets scripts run for real against the opened process without changing it: scans and reads go to the process, while writes, allocations, protection changes, threads, library loads and pausing only happen in a copy that later reads see. At the end the program prints what it held back. With it, records can be switched on and off from `--eval` to see whether a table works with the running game.

Without a window, message boxes print their text and answer No or Cancel when the box offers it, input boxes answer nothing, and `os.execute`, `io.popen`, `shellExecute`, `os.exit`, `runCommand`, `createProcess`, `createRemoteThread`, the key, mouse, sound and speech functions, `setXBox360ControllerVibration`, `setUserRegistryEnvironmentVariable`, `deleteFile` and `saveTable` only print what they would have done.
