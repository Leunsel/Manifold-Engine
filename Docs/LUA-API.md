# Lua API

Manifold Engine runs Lua 5.4 with the standard libraries and the functions below. Names follow Cheat Engine where a function exists there, so most table scripts run unchanged. Addresses can be numbers or address expressions such as `"game.exe+10"` or `"[[base]+8]+4"`.

Scripts run on the program's main thread: in the Lua engine window (Ctrl+L), in `{$lua}` blocks of Auto Assembler scripts, from the table's Lua script, and from `.lua` files in the `autorun` folder at start. Errors end in `ME-7xxx`; see [ERRORS.md](ERRORS.md).

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
| `splitDisassembledString(text)` | `extra, opcode, bytes, address` |
| `getInstructionSize(address)` / `getPreviousOpcode(address)` | |

## Address list

`getAddressList()` and the global `AddressList` return the table's list: `Count`, `SelCount`, `[index]` and `MemoryRecord[index]` (0-based, all records in tree order), `getMemoryRecord(i)`, `getMemoryRecordByDescription(text)`, `getMemoryRecordByID(id)`, `createMemoryRecord()`, `getSelectedRecords()`, `SelectedRecord`, `setSelectedRecord(record)`, `disableAllWithoutExecute()`, `refresh()`.

Set `OnDescriptionChange`, `OnAddressChange`, `OnTypeChange`, `OnValueChange` or `OnAutoAssemblerEdit` to `function(addresslist, memrec)`. They run before the matching editor opens; return true to keep the editor closed.

A memory record has the fields `ID`, `Index`, `Description`, `Address`, `AddressString`, `CurrentAddress`, `Type`/`VarType` (`vtByte` .. `vtCustom`), `CustomTypeName`, `Value`, `NumericalValue`, `Active`, `Script`, `Count`, `Child[i]`, `[index]`, `Parent`, `IsGroupHeader`, `IsAddressGroupHeader`, `ShowAsHex`, `ShowAsSigned`, `Color`, `Options` (`"[moHideChildren,...]"`), `Collapsed`, `DontSave`, `Async`, `AllowIncrease`, `AllowDecrease`, `OffsetCount`, `Offset[i]` and `OffsetText[i]` (last offset first, like Cheat Engine), `String.Size`, `String.Unicode`, `Aob.Size`, `Binary.Startbit`, `Binary.Size`, `DropDownList` (a string list of `value:description` lines), `DropDownLinked`, `DropDownLinkedMemrec`, `DropDownReadOnly`, `DropDownDescriptionOnly`, `DropDownCount`, `DropDownValue[i]`, `DropDownDescription[i]`, `HotkeyCount`, `Hotkey[i]`, `Selected`, and the methods `delete()`, `setValue(v)`, `getValue()`, `setActive(on)`, `disableWithoutExecute()`, `getAddress()`, `setAddress(text, offsets)`, `getOffset(i)`, `setOffset(i, v)`, `appendToEntry(parent)`, `getChild(i)`, `getHotkey(i)`, `getHotkeyByID(id)`, `createHotkey(keys, action, value, description)`. Every field also has a `getX()`/`setX(v)` pair.

Fields a script sets that are not listed here stay on the record, so `memrec.MyFlag = true` works. Records have the events `OnActivate(memrec, before, currentstate)` (return false while `before` is true to stop the change), `OnDeactivate`, `OnGetDisplayValue(memrec, text)` (return `true, newText`), `OnValueChanged(memrec, old, new)` and `OnDestroy()`. The global functions `onMemRecPreExecute(memrec, newstate)` and `onMemRecPostExecute(memrec, newstate, succeeded)` run around every activation.

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

Timers, hotkeys and threads run between frames on the main thread. A timer that raises an error is switched off.

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

`print(...)`, `showMessage(text)`, `messageDialog(text, type, buttons...)` (returns `mrYes`, `mrNo`, `mrOk`, `mrCancel`), `inputQuery(caption, prompt, default)` (text or nil), `sleep(ms)`, `getTickCount()`, `isKeyPressed(vk)`, `showMemoryViewer(address)`, `showStructureDissect(address)`, `getCheatEngineDir()`, `getAutorunPath()`, `getCEVersion()`, `getManifoldVersion()`, `inMainThread()`, `synchronize(f, ...)`, `queue(f, ...)`, `intToHex(value, digits)`.

Byte tables: `byteTableToString`, `byteTableToWideString`, `stringToByteTable`, `wideStringToByteTable`, `wordToByteTable`, `dwordToByteTable`, `qwordToByteTable`, `floatToByteTable`, `doubleToByteTable`, `byteTableToWord`, `byteTableToDword`, `byteTableToQword`, `byteTableToFloat`, `byteTableToDouble`.

Bit helpers for old scripts: `bAnd`, `bOr`, `bXor`, `bNot`, `bShl`, `bShr` (Lua 5.4 also has `&`, `|`, `~`, `<<`, `>>`).

Key codes: `VK_F1`..`VK_F24`, `VK_0`..`VK_9`, `VK_A`..`VK_Z`, `VK_NUMPAD0`..`VK_NUMPAD9`, `VK_SHIFT`, `VK_CONTROL`, `VK_MENU`, `VK_SPACE`, `VK_RETURN`, `VK_ESCAPE`, arrows and the other common keys.

## Cheat Engine compatibility

Tables and autorun scripts written for Cheat Engine 7.5 run here. Beyond the functions above, the program provides:

- All constants of Cheat Engine's `defines.lua` (`vt*`, `so*`, `rt*`, `fsm*`, `mrh*`, `mr*`, `cl*`, `al*`, `bs*`, `ss*` and the rest).
- Member names in any case (`memrec.description` is `memrec.Description`), and methods called with a dot or a colon (`stream.readByte()` and `stream:readByte()`). Old `class_method` globals such as `memoryrecord_getDescription(mr)` or `component_getComponent(c, i)` call the method.
- The globals `process` (the module name of the opened process), `memrec` and `syntaxcheck` inside `{$lua}` blocks and Auto Assembler commands, `MainForm` and `AddressList`.
- Reads, writes and `enumModules()` without an opened process give nil, false or an empty table instead of an error.
- Module names with dashes in addresses: `Game-Win64-Shipping.exe+10` and `Game-Win64-Shipping+10`.
- Table files: `findTableFile(name)`, `createTableFile(name, path)`, with `Name`, `Stream`, `getData()`, `saveToFile(path)`, `delete()`. Files attached to a table are kept when it is saved.
- Streams: `createMemoryStream()`, `createStringStream(text)`, `createFileStream(path)` with `Size`, `Position`, `Memory`, `DataString`, `read(n)`, `write(bytes)`, `readByte` .. `readQword`, `writeByte` .. `writeQword`, `readString`, `writeString`, `readAnsiString`, `writeAnsiString`, `copyFrom`, `loadFromFile`, `saveToFile`, `clear`.
- Memory of the program itself: `readByteLocal` .. `readDoubleLocal`, `readStringLocal`, `readBytesLocal`, the matching `write*Local` functions, `executeCodeLocal(address, parameter)`, `executeCodeLocalEx(address, ...)`, and `getAddress(name, true)` for exports of loaded system DLLs (`"kernel32.GetTickCount"`).
- Auto Assembler commands from Lua: `registerAutoAssemblerCommand(name, function(parameters, syntaxcheck))` returns the text that replaces the line, or `nil, error`. `unregisterAutoAssemblerCommand(name)`.
- Custom types: `registerCustomTypeLua(name, bytecount, bytesToValue, valueToBytes, isFloat, isString)`, `getCustomType(name)`, `getCustomTypeList()`. Records and structure elements whose type is a custom type read, show and write through the two functions. With `isString` the first one returns text and the second one receives it.
- Structures: `getStructureCount()`, `getStructure(i)`, `createStructure(name)`, `createStructureForm(address, group, name)` (`name` selects that structure), with `Name`, `Size`, `Count`, `Element[i]`, `getElement(i)`, `getElementByOffset(o)`, `addElement()`, `autoGuess(address, offset, size)` (fills `size` bytes from `offset` on the way Structure Dissect guesses, replacing the elements in that range), `addToGlobalStructureList()`, `removeFromGlobalStructureList()`, `destroy()`, and elements with `Offset`, `Name`, `Vartype`, `Bytesize`, `ChildStruct`, `DisplayMethod`, `CustomType`, `CustomTypeName`.
- Structure Dissect callbacks: `registerStructureNameLookup(function(address) return name, start end)` names a new structure and may move its start; it is also asked when a pointer is expanded, and a structure of that name is then reused. `registerStructureDissectOverride(function(structure, address) return true end)` fills a new structure instead of the guess. Both return an ID for `unregisterStructureNameLookup(id)` and `unregisterStructureDissectOverride(id)`.
- Symbol lookup callbacks: `registerSymbolLookupCallback(function(name) return address end, location)` resolves names the program does not know. With `slStart` the function is asked before anything else, with any other location (`slFailure` is the default) only when every other lookup failed. It is called on the Lua thread only, and not again while one of these functions runs. Returns an ID for `unregisterSymbolLookupCallback(id)`.
- Scanning from Lua: `createMemScan()` with `firstScan`, `nextScan`, `waitTillDone`, `setOnlyOneResult`, `getOnlyResult`, `FoundList`, and `createFoundList(memscan)` with `Count`, `Address[i]`, `Value[i]`.
  - `firstScan` takes `vtAll` (the types ticked under Settings → Scanner → Types for All), `vtGrouped` (the value is a group like `4:100 f:1.5 d:*`, see the README) and `vtCustom` (set `memscan.CustomType` to a custom type or its name first).
  - `saveCurrentResults(name)` keeps the current results under a name and `getSavedResultList()` returns the names. `nextScan` compares with saved results when its 10th argument (after `ispercentagescan`) is such a name.
- Files and system: `lfs` (`attributes`, `dir`, `mkdir`, `rmdir`, `currentdir`, `chdir`, `touch`), `getFileList`, `getDirectoryList`, `extractFileName`, `extractFileNameWithoutExt`, `extractFilePath`, `extractFileExt`, `getFileVersion`, `md5file`, `md5memory`, `stringToMD5String`, `writeToClipboard`, `readFromClipboard`, `getInternet()` with `getURL(url)` and `postURL(url, data)`, `shellExecute(command, parameters, folder, show)` (also spelled `ShellExecute`), `getScreenWidth`, `getScreenHeight`, `getWorkAreaWidth`, `getWorkAreaHeight`, `getScreenDPI`, `getMousePos`, `getTempFolder`, `cheatEngineIs64Bit`, `pause`, `unpause`, `closeCE`, `enumMemoryRegions`, `inModule`, `inSystemModule`, `deleteAllRegisteredSymbols`.
- The string helpers `split` (the parts as several values), `startsWith` and `endsWith`.
- Stored settings: `getSettings(path)` returns an object whose `Value[name]` reads and writes text, `""` when nothing is stored. Cheat Engine keeps these in the registry; here they go into `Manifold-Engine.scripts.ini` next to the program, one section per path.

### Forms

The form functions of Cheat Engine exist: `createForm`, `createLabel`, `createEdit`, `createMemo`, `createButton`, `createCheckBox`, `createComboBox`, `createListBox`, `createListView`, `createTreeView`, `createPanel`, `createGroupBox`, `createRadioGroup`, `createImage`, `createPaintBox`, `createSplitter`, `createProgressBar`, `createTrackBar`, `createMainMenu`, `createPopupMenu`, `createMenuItem`, `createImageList`, `createBitmap`, `createPNG`, `createPicture`, `createStringlist`, `createFont`, the dialogs, `getMainForm()`, `getLuaEngine()`, `getMemoryViewForm()`, `getForm(i)`, `getFormCount()`, `getApplication()`, `registerFormAddNotification`, `setMethodProperty`, `inheritsFromObject` and the other `inheritsFrom*` checks.

Their objects behave like Cheat Engine's: properties, string lists, menus, list and tree items, canvases, owners and parents, `findComponentByName`, events. `tostring(object)` gives the class and a number unique to the object (`TPanel: 0000000010000A40`), so modules can key tables by it. The main form has Cheat Engine's menu, and its Table menu lists the table's files when clicked.

Forms made with `createForm` are drawn as their own windows. Controls are laid out the way Lazarus does it: `Align` (`alTop`, `alBottom`, `alLeft`, `alRight`, `alClient`), `BorderSpacing` (`Around` plus each side), `Constraints`, the bevel of panels, and `AutoSize` for labels, edits, combo boxes and check boxes. Among controls with the same `Align` and the same position, the one that was added or aligned last comes first, so alTop panels created in a loop end up in reverse order, as in Cheat Engine. Scripts read the resulting `Left`, `Top`, `Width` and `Height`, and the layout follows when the window is resized.

Drawn are labels (colour, `Font` name, size, colour and bold, `Alignment`, `Layout`, `WordWrap`), panels (colour, bevel, caption), buttons, toggle and check boxes, edits (`TextHint`, `PasswordChar`, `ReadOnly`, `BorderStyle`), memos, list boxes, combo boxes, group boxes, splitters (dragging resizes the control next to them) tree views (nodes, expanding, selection, the arrow keys, `Home`, `End` and `Enter`) and list views (`vsReport` with columns, their `Width`, `Alignment` and `Visible`, `GridLines`, `CheckBoxes`, `ShowColumnHeaders` and sub-items; the other styles as a plain list), which fire `OnSelectItem`, `OnChange`, `OnItemChecked` and `OnColumnClick`. A form's `Menu` appears as its menu bar, and a control's `PopupMenu` (or its parent's) opens on a right click, with `OnPopup` first. `OnClick`, `OnDblClick`, `OnChange`, `OnSelectionChange`, `OnSelectionChanged`, `OnMouseEnter`, `OnMouseLeave`, `OnMoved` and `OnClose` fire as in Cheat Engine, and `Cursor = crHandPoint` shows the hand. Paint boxes and images draw what scripts paint on their `Canvas`: `line`, `moveTo`/`lineTo`, `rect`, `fillRect`, `frameRect`, `ellipse`, `roundRect`, `polygon`, `polyline`, `setPixel`, `textOut` (on an opaque box unless `Brush.Style` is `bsClear`), `clear`, `draw` and `stretchDraw` of a bitmap, with `Pen` (colour, width, `psClear`), `Brush` (colour, `bsClear`) and `Font`. The calls are recorded and replayed by the graphics card every frame, so a bitmap filled in a timer and drawn in `OnPaint` costs no copy. `OnPaint` runs when the paint box is shown, resized or asked to with `repaint()`/`invalidate()`; an opaque `fillRect` from the top left corner that covers the last frame starts a new one. Paint boxes and other controls also get `OnMouseDown`, `OnMouseUp` (button, x, y), `OnMouseMove` (x, y), `OnMouseWheelUp`/`OnMouseWheelDown` (or `OnMouseWheel`) and `OnResize`, and a form with `OnKeyDown` gets the virtual key code while it has the focus.

Other controls (page controls, track bars) keep their place as a marked empty box, and the log names them once.

By default these windows take the look of the rest of the program: the colours, fonts and font sizes that scripts set are left out (what they paint on a canvas keeps its colours), and panels and group boxes are drawn as cards. Settings → Scripts → "Script windows in the app's look" turns this off and draws them with the script's own colours and fonts. `showModal()` does not open a window yet.

The memory viewer is `getMemoryViewForm()`:

- `DisassemblerView.SelectedAddress` is the selected instruction of the memory viewer used last; setting it moves that viewer there without raising it.
- `HexadecimalView.Address` moves the hex view; `SelectionStart` and `SelectionStop` mark a range of bytes.
- `show()` and `bringToFront()` open or raise the viewer, and `Visible` says whether one is open.
- Menu items added to the `debuggerpopup` menu appear at the end of the disassembler's context menu; its `OnPopup` runs before the menu opens. Root items added to the form's `Menu` appear in the viewer's menu bar after Tools, with their sub-items. `Caption` (`-` is a separator), `Visible`, `Enabled`, `Checked`, `AutoCheck`, `Shortcut` and `OnClick` are honoured; a root item's `OnClick` runs when its menu opens, as in Cheat Engine.
- The same goes for `MainForm.Menu`: root items that scripts add appear in the main window's menu bar before Help, and items added to Cheat Engine's own menus (`MainForm.miFile`, `miEdit`, `miTable`, `miHelp`) appear at the end of Manifold's File, Edit, Table and Help menus. Their shortcuts work in the main window unless one of its own commands uses the same keys.
- The `Shortcut` of a menu bar item works while the memory viewer has the focus. When the key is one of the viewer's own keys that Cheat Engine 7.5 also uses, the viewer's command keeps it and the log says so; over the viewer's other keys (such as `Ctrl+Shift+F` for Find assembly code) the script's item wins, as it would in Cheat Engine.

## Mono / .NET

For a Mono-based Unity game (one with `mono-2.0-bdwgc.dll` or `mono.dll`), the runtime is read through a small in-process worker, without injecting a DLL. IL2CPP games have no Mono runtime and report as absent.

| Function | Result |
| --- | --- |
| `mono_isPresent()` | `true` when a Mono runtime is attached. |
| `mono_initialize()` | Finds and attaches the Mono runtime of the open process; `true` on success. The other functions call it on demand, so it rarely needs calling directly. |
| `mono_refresh()` | Reads the classes again (after the game has loaded more assemblies). |
| `mono_enumClasses()` | A list of `{ namespace, name, fullname, image }`, one per class. |
| `mono_class_getMethods(class)` | A list of the method names of the class (`"Player"` or `"Game.Player"`), or `nil` when the class is unknown. |
| `mono_class_getFieldOffset(class, field)` | The byte offset of a field, or `nil`. |
| `mono_getAddress(class, method)` or `mono_getAddress("Class:Method")` | The JIT-compiled address of the method (compiling it if needed), or `nil`. The same names resolve in address expressions and the Auto Assembler, e.g. `Assembly-CSharp:Player:TakeDamage`. |
| `mono_showDissector()` | Opens the Mono / .NET window (class list, methods with their address, field offsets), so an autorun script can offer it from its own menu the way the Unreal Engine script does. |

When Mono is loaded, Structure Dissect uses it automatically: dissecting the address of a Mono object names the structure by its class and fills the fields (names, offsets and types) from the runtime, and following a pointer to another object looks that one up too. No script or manual structure is needed.

## Checking a table without the program window

The test program runs a table the way the program does, without a window, and prints every Lua error:

```
Manifold-Engine.Tests.exe --compat "Game.CT" --autorun="C:\path\to\autorun" --check-scripts --seconds=5
```

`--autorun` runs the `.lua` files of a folder first. `--process=name.exe` opens a process. `--check-scripts` checks every Auto Assembler script of the table without writing to the process. `--eval` runs Lua code, or a `.lua` file, at the end. Files the scripts write under the user profile go to a separate folder (`--home=`).

`--dry-run` lets scripts run for real against the opened process without changing it: scans and reads go to the process, while writes, allocations, protection changes, threads, library loads and pausing only happen in a copy that later reads see. At the end the program prints what it held back. With it, records can be switched on and off from `--eval` to see whether a table works with the running game.

Without a window, message boxes print their text and answer No or Cancel when the box offers it, input boxes answer nothing, and `os.execute`, `io.popen`, `shellExecute` and `os.exit` only print what they would have done.
