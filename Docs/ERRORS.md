# Error codes

Every failure Manifold Engine reports ends in a code like `ME-1006`. The number never changes its meaning between versions. Hover a coded message in the program to see the cause and what helps; a click copies it.

## Process

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-1001 | No process with this id or name runs: …. | The process ended, or the name was written differently. | Pick the process again from the process list. |
| ME-1002 | Windows refused access to the process: …. | The process runs with higher rights than this program, or it is protected. | Start Manifold Engine as administrator. Protected processes (anti-cheat, system services) cannot be opened at all. |
| ME-1003 | The process could not be opened: …. | OpenProcess failed for a reason other than missing rights. | Start the program again. If it stays, report the code with the log file from the logs folder next to the program. |
| ME-1004 | The process has ended. | The opened process closed or crashed. Its addresses mean nothing any more. | Start it again and open it from the process list. Tables keep their records. |
| ME-1005 | No process is open. | This needs a process to read from or write to. | Open a process first: the computer button at the top left, or Ctrl+P. |
| ME-1006 | Memory at … could not be read. | The address is not mapped in the process, or the page does not allow reading. | Check the address in the memory viewer: unreadable pages show as ??. |
| ME-1007 | Memory at … could not be written. | The address is not mapped, or the page could not be made writable. | Check the address in the memory viewer: unreadable pages show as ??. |
| ME-1008 | Memory could not be allocated in the process: …. | VirtualAllocEx found no free range of that size, or none near the wanted address. | Allocate less, or drop the near address so any free range will do. |
| ME-1009 | The protection of … could not be changed. | VirtualProtectEx refused: the page may be a guard page or belong to a mapped image that forbids it. | Check the address in the memory viewer: unreadable pages show as ??. |
| ME-1010 | Memory at … could not be freed. | The address was not the start of an allocation made with alloc, or it was freed already. | Free only what alloc returned. Disabling a script twice frees twice. |
| ME-1011 | The module list of the process could not be read. | The process is still starting, or a 64-bit process is examined from a 32-bit build. | Wait a moment and open the process again. |
| ME-1012 | A thread could not be started in the process: …. | CreateRemoteThread was refused. Some games block it. | Start Manifold Engine as administrator. Protected processes (anti-cheat, system services) cannot be opened at all. |
| ME-1013 | The library could not be loaded into the process: …. | LoadLibrary in the target failed: the file is missing, has the wrong bitness, or its imports are missing. | Use a library built for the same bitness as the target and keep its dependencies beside it. |
| ME-1014 | The module file on disk could not be read: …. | The file was moved or deleted after the process loaded it, or another program holds it locked. | Check that the file still exists at that path, then scan again. |
| ME-1015 | The module file does not match the loaded module: …. | The file on disk is not a valid program image, or it was replaced by a different version after the process started. | Restart the process so it loads the file on disk, then scan again. |
| ME-1016 | No module named … is loaded in the process. | The name matches none of the modules the process has loaded. | Check the spelling, or wait until the game has loaded the module. manifold.process.modules() lists them. |
| ME-1017 | No module contains the address …. | The address lies outside every loaded module: on the heap, on a stack, or in code the game allocated itself. | That is normal for objects and JIT code. Check the address in the memory viewer when a module was expected. |
| ME-1018 | A thread started in the process did not finish in time: …. | The code is still running or waits for something the game holds, for example while the game is paused in a debugger. | The memory the thread uses was kept. Check the code; a library's DllMain has to return quickly. |

## Scanner

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-2001 | Enter a value to scan for. | This scan type compares memory with a value, and the value field is empty. | Type the value, or pick Unknown initial value for a first scan. |
| ME-2002 | "…" is not a valid value for this type. | The text does not parse as the chosen value type, or it does not fit its size. | Check the value type and the Hex box. Floats take a dot as decimal separator. |
| ME-2003 | The byte pattern is not valid: …. | Array of byte patterns are hex byte pairs separated by spaces; ?? or * stand for any byte. | Write the pattern like 48 8B ?? 10 FF. |
| ME-2004 | Scan results could not be stored: …. | The temporary folder for results is full or not writable. | Free disk space on the drive of the temp folder, or narrow the scan. |
| ME-2005 | No memory matches the scan options. | The start/stop range or the protection filters leave no memory to scan. | Widen the range, or set Writable/Executable/Copy on write to either. |
| ME-2006 | A scan is still running. | Only one scan runs at a time. | Wait for it, or cancel it with the button beside the progress bar. |
| ME-2007 | There is nothing to compare with yet. | A next scan needs the results of a first scan. | Run a first scan. |
| ME-2008 | No value type is chosen for All. | All scans the types ticked in Settings, and none of them is ticked, or the value fits none of them. | Tick the types under Settings, Scanner, Types for All, or check the value. |
| ME-2009 | The group is not valid: …. | A grouped scan takes values like 4:100 f:1.5 d:* separated by spaces. | Write each value as type:value. Types are 1, 2, 4, 8, f, d, p, s, u and skip; within:N lets the values after the first one be anywhere in N bytes. |
| ME-2010 | … is not available for this value type. | Grouped values and custom types compare their bytes only, so they have no bigger, smaller or increased. | Use Exact value, Unknown initial value, Changed, Unchanged or Same as first scan. |
| ME-2011 | There are no saved results named "…". | Saved results belong to the current scan and are gone after New scan. | Save the results under that name first, or pick another one. |
| ME-2012 | The saved results "…" have a different value type. | A next scan can compare only with results of the same value type and size. | Pick saved results of the same scan. |
| ME-2013 | The custom type "…" is not registered. | A script registers custom types, and this one is not known (any more). | Run the script that registers it, or pick another value type. |
| ME-2014 | The pointer scan covers too much memory: …. | A pointer scan handles up to 32 GB of memory and four billion pointers at once. | Leave read-only and mapped memory and the system modules out in the Memory menu. |
| ME-2015 | There is not enough memory for the pointer scan (…). | The pointer map needs eight bytes for every pointer the process holds, plus four while it searches. | Close other programs, or leave read-only and mapped memory out in the Memory menu. |
| ME-2016 | "…" is not a pointer scan file. | The file is missing, cut off, or was not written by Save results of the pointer scan. | Pick a .mptr file that the pointer scan saved. |
| ME-2017 | The pointer paths could not be written to "…". | The folder is not writable or the drive is full. | Save to another folder. |

## Assembler

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-3001 | Line …. | The line could not be read as an instruction. | Fix the line named in the message and run the script again. |
| ME-3002 | Line …. | The instruction name is unknown. | Fix the line named in the message and run the script again. |
| ME-3003 | Line …. | An operand is not valid for this instruction, or its size is unclear. | Write the size of memory operands: byte ptr, word ptr, dword ptr, qword ptr. |
| ME-3004 | Line …. | A name in the line is neither a label, a registered symbol, a module nor an export. | Declare labels with label(name), or register the symbol in the script that makes it. |
| ME-3005 | Line …. | A jump or memory reference is too far from the code. 64-bit code reaches only 2 GB around itself. | Allocate the code near its target: alloc(newmem,$1000,"game.exe"+1234). |
| ME-3006 | Line …. | The encoder found no form for this combination of operands. | Fix the line named in the message and run the script again. |
| ME-3101 | Line …. | The command is not an Auto Assembler command. | See Docs/AUTO-ASSEMBLER.md for the commands. |
| ME-3102 | Line …. | The command got the wrong number or kind of arguments. | See Docs/AUTO-ASSEMBLER.md for the arguments of the command. |
| ME-3103 | Line …. | A label or allocation name is declared or placed twice. | Rename one of them. |
| ME-3104 | Label … is declared but never placed. | label(name) declares a name; name: places it. One is missing. | Place the label with name: before the code it marks. |
| ME-3105 | Memory for … could not be allocated. | No free range was found for the allocation, also not near the wanted address. | Allocate less, or near another address. |
| ME-3106 | The pattern for … was not found. | aobscan found no match. The game was updated, or the bytes changed already (the script is active). | Disable the script first, or make the pattern from the current game version. |
| ME-3107 | The bytes at … are not the expected ones. | assert() compared memory with the bytes the script expects and they differ. | The game version differs from the one the script was written for. |
| ME-3108 | The script has no [ENABLE] and [DISABLE] sections. | A table script is switched on and off and needs both sections. | Add [ENABLE] and [DISABLE], or run it from the Auto Assembler window as a plain script. |
| ME-3109 | The script could not write its code to …. | The memory is not mapped or could not be made writable. | Check the address in the memory viewer: unreadable pages show as ??. |
| ME-3110 | The {$lua} block failed: …. | A Lua block in the script raised an error. | Run the block in the Lua engine window to see where it fails. |
| ME-3111 | The code layout did not settle. | Instruction sizes kept changing between passes, usually with labels that depend on each other's distance. | Report the script with the code. |
| ME-3112 | The included file … was not found. | include() looks next to the table and in the autorun folder. | Put the file there or write its full path. |
| ME-3113 | The symbol … is registered already. | registersymbol gave a name that another script registered and did not unregister. | Disable the other script, or rename the symbol. |
| ME-3114 | readmem could not read …. | The source of readmem is not readable. | Check the address in the memory viewer: unreadable pages show as ??. |

## Debugger

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-4001 | The debugger could not attach: …. | Another debugger is attached already, or the process forbids debugging. | Close other debuggers. Some games end themselves when a debugger attaches. |
| ME-4002 | The debugger is attached already. | It attaches once per process. | Nothing to do. |
| ME-4003 | All four hardware breakpoints are in use. | x86 has four debug registers; each watch or hardware breakpoint takes one. | Stop another watch or remove a breakpoint first. |
| ME-4004 | The breakpoint at … could not be set. | The code page could not be written, or a thread context could not be changed. | Check the address in the memory viewer: unreadable pages show as ??. |
| ME-4005 | The registers of thread … could not be read or written. | The thread ended or does not allow context access. | Usually harmless; the thread is skipped. |
| ME-4006 | The debugger could not detach: …. | DebugActiveProcessStop failed. The process ends when this program closes. | Start the program again. If it stays, report the code with the log file from the logs folder next to the program. |
| ME-4007 | The debugger is not attached. | This needs the debugger. | Attach it from the Debug menu, or start a watch: that attaches it. |
| ME-4008 | A hardware breakpoint cannot watch … bytes. | Debug registers watch 1, 2, 4 or 8 bytes at an address aligned to that size. | Watch fewer bytes, or an aligned address. |
| ME-4009 | The debugged process ended. | The process closed while the debugger was attached. | Open it again. |
| ME-4010 | The expression could not be read: …. | Conditions and register changes are small Lua expressions: registers (RAX, EAX, AL, ZF), numbers (decimal, or hex as 0x10, $10 or with letters like 7FF6A000), memory reads (readInteger(RCX+0x10), [RSP+8]), comparisons and and/or/not. | Fix the part the message names. Put module addresses in quotes, as in RIP == "game.exe+1234". |
| ME-4011 | The in-process debugger in this process is from another version of the program. | The process still has the debugger library of an earlier program version loaded, and the two cannot talk to each other. | Detach the in-process debugger in the old program, or restart the process. |
| ME-4012 | The trace could not be started: …. | A trace runs from a breakpoint and needs the debugger and a free breakpoint. | Attach the debugger, free a debug register or remove the breakpoint at that address first. |
| ME-4101 | No Mono or IL2CPP runtime was found in this process…. | The .NET features need a 64-bit Unity game: a Mono build (with mono-2.0-bdwgc.dll or mono.dll) or an IL2CPP build (with GameAssembly.dll). An IL2CPP build without il2cpp_image_get_class (before Unity 2018.3) or with renamed exports is not supported. | Open the game's process once it has finished loading, or check that it is a Unity game at all. |
| ME-4102 | A Mono call into the process failed: …. | The in-process helper could not call a mono function, or the call did not return. | The process may have closed, or the Mono runtime may be an unexpected version. |
| ME-4201 | Unreal Engine is not resolved: …. | The game's name table, object array or reflection layout was not found, it is still being read, or the Unreal tool is shut down. | Start it in Extensions > Unreal Engine once the game shows its menu. For a game the search does not find, manifold.unreal.configure takes the addresses, layout offsets or search patterns. |
| ME-4202 | No Unreal class or struct is named …. | The name is not among the game's reflected types. Type names have no U, A or F prefix: Actor, not AActor. | Look the type up in the Unreal Engine window (Extensions > Unreal Engine > Types, Properties and Who Owns an Address). |
| ME-4203 | No reflected property …. | The class or struct and its parents have no property of that name. Native members that are not reflected have no name. | Check the spelling in the Unreal Engine window; for an array element write Name[index]. |
| ME-4204 | No Unreal object contains …. | The address is not inside a live UObject: it is a struct on the heap, freed memory, or not Unreal data at all. | Use an address inside a live object, for example one from a dissect of the engine, the world or the pawn. |
| ME-4301 | No object with run-time type information at or before …. | No virtual function table pointer with MSVC run-time type information was found at the address or in the memory in front of it. The program may be built without RTTI (Unreal Engine games are), with GCC, or the address is not inside a C++ object with virtual functions. | Use an address inside an object of a class with virtual functions. |
| ME-4302 | No class … with a virtual function table was found. | The modules of the opened process hold no run-time type information for a class of that name. Names are case sensitive and written with their namespaces, as in Game::Player. | Check the spelling, or take the name from manifold.rtti.className on an object of the class. |

## The program

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-5001 | The window class could not be registered. | Windows refused to register the program's window class. | Start the program again. If it stays, report the code with the log file from the logs folder next to the program. |
| ME-5002 | The window could not be created. | Windows did not create the main window. | Start the program again. If it stays, report the code with the log file from the logs folder next to the program. |
| ME-5003 | Direct3D 11 could not be started (…). | No graphics device with Direct3D 11 support was found. | Update the graphics driver. Over Remote Desktop, start the program on the computer itself once. |
| ME-5004 | The graphics device gave no DXGI factory (…). | The driver did not hand out the objects for drawing. | Update the graphics driver. Over Remote Desktop, start the program on the computer itself once. |
| ME-5005 | The swap chain could not be created (…). | The driver did not create the surface the window draws on. | Update the graphics driver. Over Remote Desktop, start the program on the computer itself once. |
| ME-5006 | The icon font is missing from the program. | The program file is damaged. | Download the program again. |
| ME-5007 | The fonts could not be prepared. | No usable font was found in the Windows fonts folder. | Repair the Windows fonts (Tahoma, Segoe UI, Consolas). |
| ME-5008 | The user interface could not be started. | The interface library did not start with the window or the device. | Start the program again. If it stays, report the code with the log file from the logs folder next to the program. |
| ME-5009 | The settings could not be saved to …. | The configs folder next to the program is not writable. | Move the program to a folder you can write to, or run it as administrator. |
| ME-5010 | The program crashed (…). | An unexpected error stopped the program. | Start the program again. If it stays, report the code with the log file from the logs folder next to the program. |
| ME-5011 | The last run crashed. A report is in …. | The program ended through an unexpected error last time. | Start the program again. If it stays, report the code with the log file from the logs folder next to the program. |
| ME-5012 | The program runs without administrator rights. | Many games run elevated or protect their memory, and then they cannot be opened. | Restart as administrator from the Help menu if a process cannot be opened. |
| ME-5013 | The diagnostics report could not be written to …. | The folder does not exist or is not writable. | Save it somewhere else, or copy it to the clipboard instead. |

## Cheat tables

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-6001 | The table … could not be read. | The file is missing or not readable. | Check the path and that the file is not open with exclusive access elsewhere. |
| ME-6002 | The table is not valid XML: …. | The file is damaged or not a cheat table. | Open it in a text editor and fix the line named in the message. |
| ME-6003 | The file is not a cheat table. | The XML has no CheatTable root element. | Open a .CT file made by Cheat Engine or by this program. |
| ME-6004 | The table could not be saved to …. | The folder is not writable or the disk is full. | Save it somewhere else. |
| ME-6005 | Entry … of the table is damaged and was skipped. | An entry misses required parts or has values that do not parse. | Check that entry in a text editor. |
| ME-6006 | The table was written by a newer version (…). | Parts of it may be unknown to this version and are kept as they are. | Usually fine. Check the records that do not work. |
| ME-6007 | The backup could not be written to …. | The backups folder is not writable or the disk is full. | Pick another folder in Settings, under Scripts, or turn the backups off there. |

## Lua

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-7001 | Lua syntax error: …. | The script does not compile. | Fix the line named in the message and run the script again. |
| ME-7002 | Lua error: …. | The script raised an error while running. | Fix the line named in the message and run the script again. |
| ME-7003 | The autorun script … failed. | A script in the autorun folder raised an error at start. | Fix or remove the script. The log names the line. |
| ME-7004 | A Lua timer was stopped after an error: …. | The timer's function raised an error, so the timer was disabled to stop the error repeating. | Fix the function and enable the timer again. |
| ME-7005 | The table's Lua script failed: …. | The script stored in the table raised an error when the table was loaded. | Open Table > Table Lua Script and fix it. |
| ME-7006 | Lua: …. | A Manifold function got an argument of the wrong type or value. | See Docs/LUA-API.md for the arguments. |
| ME-7007 | The manifold API was called the wrong way: …. | A manifold function was called with a colon, got a missing, extra or wrong argument or an unknown option, or a name that does not exist was read. | The message names the function and the argument. Docs/MANIFOLD-API.md has the rules and every function. |
| ME-7008 | There is no documentation for …. | Nothing documents the function or name: no Manifold, Cheat Engine or Lua entry, no manifold.doc.register call and no LuaLS annotation above its definition. | Check the name, or document it with manifold.doc.register, or with ---@param annotations and manifold.doc.registerModule. |
| ME-7009 | The image … could not be read. | manifold.ui.image.load reads PNG, JPEG, BMP, TGA and GIF files; the file is missing, unreadable or in another format. | Check the path (relative paths start at the program folder) and that the file opens in an image viewer. |
| ME-7010 | The … is gone. | The window, widget, menu entry or dialog was removed, closed for good, or its script ran again, which removes everything the script made before. | Keep the new handle the script's next run returns, or test with isOpen before using an old one. |
| ME-7011 | There is no autorun module named …. | No script or module of that name is in the autorun folder, or the script that asked does not belong to a module. | Extensions > Modules lists the names. A module's name is its NAME header line, or its file or folder name. |
| ME-7012 | The autorun module …. | A module it requires is missing, switched off, held, failed or too old, its requirements form a circle, it asks for a newer manifold API, or the module cannot be switched in its state. | The message names the reason. Fix the REQUIRES or API line in the module's header, or add or enable the module it names. |
| ME-7013 | The autorun module … is held until it is allowed. | The module is new in the autorun folder, or a script wrote to its files, so it does not run on its own. | Look at it in Extensions > Modules and choose Allow if it is yours. |

## Addresses and symbols

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-8001 | "…" is not a valid address. | The address expression could not be read. | Write addresses like game.exe+1234, [game.exe+10]+8, or a hex number. |
| ME-8002 | Unknown symbol "…". | The name is neither a module, an export nor a registered symbol. | Check the spelling. Symbols of scripts exist only while the script is active. |
| ME-8003 | The pointer at … could not be read. | A [ ] part of the address points to unreadable memory. | The pointer path is wrong or the object does not exist right now. |
| ME-8004 | Division by zero in "…". | The address expression divides by zero. | Fix the expression. |
| ME-8005 | The name … is taken. | Another symbol with this name exists. | Pick another name. |

## Other

| Code | Message | Cause | Help |
| --- | --- | --- | --- |
| ME-9001 | The template has a syntax error: …. | A << >> or <% %> block in the template or its settings file is not valid Lua, or a block is not closed. | Fix the template at the reported line. |
| ME-9002 | The template stopped with an error: …. | Lua code inside the template raised an error while the script was generated. | Fix the template at the reported line. |
| ME-9003 | The template needs …, which could not be determined. | The template lists the value under Requires, and it came out empty for the selected address. | Pick an address the template supports, or remove the value from Requires. |
| ME-9004 | … | A value the template uses could not be worked out, or a question was cancelled. | Open a process and select an instruction in the memory viewer first. |
| ME-9005 | The template file … was not found. | include() looks in the Partials folder, then in the template folder, then in the built-in templates. | Put the file there or correct the name. |
| ME-9006 | …. | Templates include each other in a circle, or the includes nest more than 16 levels deep. | Remove the include that leads back. |
| ME-9007 | … | The template declares the architectures it supports, and the attached process is another one. | Use a template for this architecture. |
| ME-9008 | … | A template file could not be written, moved or removed, or the action is not possible for this template. | Check that the templates folder is writable and the file is not open elsewhere. Built-in templates change only through a copy. |
