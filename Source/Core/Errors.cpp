#include "Core/Errors.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace Manifold
{
    namespace
    {
        constexpr std::string_view kPrefix = "ME-";
        constexpr std::string_view kLegacyPrefix = "MCE-";
        constexpr std::string_view kPlace = "{}";

        constexpr std::string_view kRunAsAdmin = "Start Manifold Engine as administrator. Protected processes (anti-cheat, system services) cannot be opened at all.";
        constexpr std::string_view kCheckAddress = "Check the address in the memory viewer: unreadable pages show as ??.";
        constexpr std::string_view kCheckScript = "Fix the line named in the message and run the script again.";
        constexpr std::string_view kDriver = "Update the graphics driver. Over Remote Desktop, start the program on the computer itself once.";
        constexpr std::string_view kReport = "Start the program again. If it stays, report the code with the log file from the logs folder next to the program.";

        struct Area
        {
            int First;
            std::string_view Name;
        };

        constexpr std::array<Area, 8> kAreas{ {
            { 1, "Process" },
            { 2, "Scanner" },
            { 3, "Assembler" },
            { 4, "Debugger" },
            { 5, "The program" },
            { 6, "Cheat tables" },
            { 7, "Lua" },
            { 8, "Addresses and symbols" },
        } };

        constexpr ErrorInfo kErrors[] = {
            { ErrorCode::ProcessNotFound, "No process with this id or name runs: {}.",
                "The process ended, or the name was written differently.",
                "Pick the process again from the process list." },
            { ErrorCode::ProcessAccessDenied, "Windows refused access to the process: {}.",
                "The process runs with higher rights than this program, or it is protected.",
                kRunAsAdmin },
            { ErrorCode::ProcessOpenFailed, "The process could not be opened: {}.",
                "OpenProcess failed for a reason other than missing rights.",
                kReport },
            { ErrorCode::ProcessExited, "The process has ended.",
                "The opened process closed or crashed. Its addresses mean nothing any more.",
                "Start it again and open it from the process list. Tables keep their records." },
            { ErrorCode::NoProcess, "No process is open.",
                "This needs a process to read from or write to.",
                "Open a process first: the computer button at the top left, or Ctrl+P." },
            { ErrorCode::ReadFailed, "Memory at {} could not be read.",
                "The address is not mapped in the process, or the page does not allow reading.",
                kCheckAddress },
            { ErrorCode::WriteFailed, "Memory at {} could not be written.",
                "The address is not mapped, or the page could not be made writable.",
                kCheckAddress },
            { ErrorCode::AllocFailed, "Memory could not be allocated in the process: {}.",
                "VirtualAllocEx found no free range of that size, or none near the wanted address.",
                "Allocate less, or drop the near address so any free range will do." },
            { ErrorCode::ProtectFailed, "The protection of {} could not be changed.",
                "VirtualProtectEx refused: the page may be a guard page or belong to a mapped image that forbids it.",
                kCheckAddress },
            { ErrorCode::FreeFailed, "Memory at {} could not be freed.",
                "The address was not the start of an allocation made with alloc, or it was freed already.",
                "Free only what alloc returned. Disabling a script twice frees twice." },
            { ErrorCode::ModulesUnreadable, "The module list of the process could not be read.",
                "The process is still starting, or a 64-bit process is examined from a 32-bit build.",
                "Wait a moment and open the process again." },
            { ErrorCode::ThreadCreateFailed, "A thread could not be started in the process: {}.",
                "CreateRemoteThread was refused. Some games block it.",
                kRunAsAdmin },
            { ErrorCode::InjectFailed, "The library could not be loaded into the process: {}.",
                "LoadLibrary in the target failed: the file is missing, has the wrong bitness, or its imports are missing.",
                "Use a library built for the same bitness as the target and keep its dependencies beside it." },
            { ErrorCode::ModuleFileUnreadable, "The module file on disk could not be read: {}.",
                "The file was moved or deleted after the process loaded it, or another program holds it locked.",
                "Check that the file still exists at that path, then scan again." },
            { ErrorCode::ModuleFileDifferent, "The module file does not match the loaded module: {}.",
                "The file on disk is not a valid program image, or it was replaced by a different version after the process started.",
                "Restart the process so it loads the file on disk, then scan again." },
            { ErrorCode::ModuleNotFound, "No module named {} is loaded in the process.",
                "The name matches none of the modules the process has loaded.",
                "Check the spelling, or wait until the game has loaded the module. manifold.process.modules() lists them." },
            { ErrorCode::NoModuleAt, "No module contains the address {}.",
                "The address lies outside every loaded module: on the heap, on a stack, or in code the game allocated itself.",
                "That is normal for objects and JIT code. Check the address in the memory viewer when a module was expected." },
            { ErrorCode::ThreadTimedOut, "A thread started in the process did not finish in time: {}.",
                "The code is still running or waits for something the game holds, for example while the game is paused in a debugger.",
                "The memory the thread uses was kept. Check the code; a library's DllMain has to return quickly." },

            { ErrorCode::ScanNoValue, "Enter a value to scan for.",
                "This scan type compares memory with a value, and the value field is empty.",
                "Type the value, or pick Unknown initial value for a first scan." },
            { ErrorCode::ScanBadValue, "\"{}\" is not a valid value for this type.",
                "The text does not parse as the chosen value type, or it does not fit its size.",
                "Check the value type and the Hex box. Floats take a dot as decimal separator." },
            { ErrorCode::ScanBadPattern, "The byte pattern is not valid: {}.",
                "Array of byte patterns are hex byte pairs separated by spaces; ?? or * stand for any byte.",
                "Write the pattern like 48 8B ?? 10 FF." },
            { ErrorCode::ScanStoreFailed, "Scan results could not be stored: {}.",
                "The temporary folder for results is full or not writable.",
                "Free disk space on the drive of the temp folder, or narrow the scan." },
            { ErrorCode::ScanRangeEmpty, "No memory matches the scan options.",
                "The start/stop range or the protection filters leave no memory to scan.",
                "Widen the range, or set Writable/Executable/Copy on write to either." },
            { ErrorCode::ScanBusy, "A scan is still running.",
                "Only one scan runs at a time.",
                "Wait for it, or cancel it with the button beside the progress bar." },
            { ErrorCode::ScanNothingToCompare, "There is nothing to compare with yet.",
                "A next scan needs the results of a first scan.",
                "Run a first scan." },
            { ErrorCode::ScanNoTypes, "No value type is chosen for All.",
                "All scans the types ticked in Settings, and none of them is ticked, or the value fits none of them.",
                "Tick the types under Settings, Scanner, Types for All, or check the value." },
            { ErrorCode::ScanGroupSyntax, "The group is not valid: {}.",
                "A grouped scan takes values like 4:100 f:1.5 d:* separated by spaces.",
                "Write each value as type:value. Types are 1, 2, 4, 8, f, d, p, s, u and skip; within:N lets the values after the first one be anywhere in N bytes." },
            { ErrorCode::ScanCompareUnavailable, "{} is not available for this value type.",
                "Grouped values and custom types compare their bytes only, so they have no bigger, smaller or increased.",
                "Use Exact value, Unknown initial value, Changed, Unchanged or Same as first scan." },
            { ErrorCode::ScanSavedMissing, "There are no saved results named \"{}\".",
                "Saved results belong to the current scan and are gone after New scan.",
                "Save the results under that name first, or pick another one." },
            { ErrorCode::ScanSavedDifferent, "The saved results \"{}\" have a different value type.",
                "A next scan can compare only with results of the same value type and size.",
                "Pick saved results of the same scan." },
            { ErrorCode::ScanCustomMissing, "The custom type \"{}\" is not registered.",
                "A script registers custom types, and this one is not known (any more).",
                "Run the script that registers it, or pick another value type." },

            { ErrorCode::AsmSyntax, "Line {}.",
                "The line could not be read as an instruction.",
                kCheckScript },
            { ErrorCode::AsmUnknownInstruction, "Line {}.",
                "The instruction name is unknown.",
                kCheckScript },
            { ErrorCode::AsmBadOperand, "Line {}.",
                "An operand is not valid for this instruction, or its size is unclear.",
                "Write the size of memory operands: byte ptr, word ptr, dword ptr, qword ptr." },
            { ErrorCode::AsmUnknownSymbol, "Line {}.",
                "A name in the line is neither a label, a registered symbol, a module nor an export.",
                "Declare labels with label(name), or register the symbol in the script that makes it." },
            { ErrorCode::AsmOutOfRange, "Line {}.",
                "A jump or memory reference is too far from the code. 64-bit code reaches only 2 GB around itself.",
                "Allocate the code near its target: alloc(newmem,$1000,\"game.exe\"+1234)." },
            { ErrorCode::AsmEncodeFailed, "Line {}.",
                "The encoder found no form for this combination of operands.",
                kCheckScript },
            { ErrorCode::AaUnknownCommand, "Line {}.",
                "The command is not an Auto Assembler command.",
                "See Docs/AUTO-ASSEMBLER.md for the commands." },
            { ErrorCode::AaBadArguments, "Line {}.",
                "The command got the wrong number or kind of arguments.",
                "See Docs/AUTO-ASSEMBLER.md for the arguments of the command." },
            { ErrorCode::AaLabelTwice, "Line {}.",
                "A label or allocation name is declared or placed twice.",
                "Rename one of them." },
            { ErrorCode::AaLabelNotPlaced, "Label {} is declared but never placed.",
                "label(name) declares a name; name: places it. One is missing.",
                "Place the label with name: before the code it marks." },
            { ErrorCode::AaAllocFailed, "Memory for {} could not be allocated.",
                "No free range was found for the allocation, also not near the wanted address.",
                "Allocate less, or near another address." },
            { ErrorCode::AaPatternNotFound, "The pattern for {} was not found.",
                "aobscan found no match. The game was updated, or the bytes changed already (the script is active).",
                "Disable the script first, or make the pattern from the current game version." },
            { ErrorCode::AaAssertFailed, "The bytes at {} are not the expected ones.",
                "assert() compared memory with the bytes the script expects and they differ.",
                "The game version differs from the one the script was written for." },
            { ErrorCode::AaNoSections, "The script has no [ENABLE] and [DISABLE] sections.",
                "A table script is switched on and off and needs both sections.",
                "Add [ENABLE] and [DISABLE], or run it from the Auto Assembler window as a plain script." },
            { ErrorCode::AaWriteFailed, "The script could not write its code to {}.",
                "The memory is not mapped or could not be made writable.",
                kCheckAddress },
            { ErrorCode::AaLuaFailed, "The {$lua} block failed: {}.",
                "A Lua block in the script raised an error.",
                "Run the block in the Lua engine window to see where it fails." },
            { ErrorCode::AaLayoutUnstable, "The code layout did not settle.",
                "Instruction sizes kept changing between passes, usually with labels that depend on each other's distance.",
                "Report the script with the code." },
            { ErrorCode::AaIncludeMissing, "The included file {} was not found.",
                "include() looks next to the table and in the autorun folder.",
                "Put the file there or write its full path." },
            { ErrorCode::AaSymbolTaken, "The symbol {} is registered already.",
                "registersymbol gave a name that another script registered and did not unregister.",
                "Disable the other script, or rename the symbol." },
            { ErrorCode::AaReadMemFailed, "readmem could not read {}.",
                "The source of readmem is not readable.",
                kCheckAddress },

            { ErrorCode::DebugAttachFailed, "The debugger could not attach: {}.",
                "Another debugger is attached already, or the process forbids debugging.",
                "Close other debuggers. Some games end themselves when a debugger attaches." },
            { ErrorCode::DebugAlreadyAttached, "The debugger is attached already.",
                "It attaches once per process.",
                "Nothing to do." },
            { ErrorCode::DebugNoFreeSlot, "All four hardware breakpoints are in use.",
                "x86 has four debug registers; each watch or hardware breakpoint takes one.",
                "Stop another watch or remove a breakpoint first." },
            { ErrorCode::DebugBreakpointFailed, "The breakpoint at {} could not be set.",
                "The code page could not be written, or a thread context could not be changed.",
                kCheckAddress },
            { ErrorCode::DebugContextFailed, "The registers of thread {} could not be read or written.",
                "The thread ended or does not allow context access.",
                "Usually harmless; the thread is skipped." },
            { ErrorCode::DebugDetachFailed, "The debugger could not detach: {}.",
                "DebugActiveProcessStop failed. The process ends when this program closes.",
                kReport },
            { ErrorCode::DebugNotAttached, "The debugger is not attached.",
                "This needs the debugger.",
                "Attach it from the Debug menu, or start a watch: that attaches it." },
            { ErrorCode::DebugBadSize, "A hardware breakpoint cannot watch {} bytes.",
                "Debug registers watch 1, 2, 4 or 8 bytes at an address aligned to that size.",
                "Watch fewer bytes, or an aligned address." },
            { ErrorCode::DebugTargetExited, "The debugged process ended.",
                "The process closed while the debugger was attached.",
                "Open it again." },
            { ErrorCode::DebugBadExpression, "The expression could not be read: {}.",
                "Conditions and register changes are small Lua expressions: registers (RAX, EAX, AL, ZF), numbers (decimal, or hex as 0x10, $10 or with letters like 7FF6A000), memory reads (readInteger(RCX+0x10), [RSP+8]), comparisons and and/or/not.",
                "Fix the part the message names. Put module addresses in quotes, as in RIP == \"game.exe+1234\"." },
            { ErrorCode::DebugAgentVersion, "The in-process debugger in this process is from another version of the program.",
                "The process still has the debugger library of an earlier program version loaded, and the two cannot talk to each other.",
                "Detach the in-process debugger in the old program, or restart the process." },
            { ErrorCode::DebugTraceFailed, "The trace could not be started: {}.",
                "A trace runs from a breakpoint and needs the debugger and a free breakpoint.",
                "Attach the debugger, free a debug register or remove the breakpoint at that address first." },
            { ErrorCode::MonoNotFound, "No Mono or IL2CPP runtime was found in this process{}.",
                "The .NET features need a 64-bit Unity game: a Mono build (with mono-2.0-bdwgc.dll or mono.dll) or an IL2CPP build (with GameAssembly.dll). "
                "An IL2CPP build without il2cpp_image_get_class (before Unity 2018.3) or with renamed exports is not supported.",
                "Open the game's process once it has finished loading, or check that it is a Unity game at all." },
            { ErrorCode::MonoCallFailed, "A Mono call into the process failed: {}.",
                "The in-process helper could not call a mono function, or the call did not return.",
                "The process may have closed, or the Mono runtime may be an unexpected version." },

            { ErrorCode::WindowClassFailed, "The window class could not be registered.",
                "Windows refused to register the program's window class.",
                kReport },
            { ErrorCode::WindowFailed, "The window could not be created.",
                "Windows did not create the main window.",
                kReport },
            { ErrorCode::DeviceFailed, "Direct3D 11 could not be started ({}).",
                "No graphics device with Direct3D 11 support was found.",
                kDriver },
            { ErrorCode::FactoryFailed, "The graphics device gave no DXGI factory ({}).",
                "The driver did not hand out the objects for drawing.",
                kDriver },
            { ErrorCode::SwapChainFailed, "The swap chain could not be created ({}).",
                "The driver did not create the surface the window draws on.",
                kDriver },
            { ErrorCode::IconFontMissing, "The icon font is missing from the program.",
                "The program file is damaged.",
                "Download the program again." },
            { ErrorCode::FontAtlasFailed, "The fonts could not be prepared.",
                "No usable font was found in the Windows fonts folder.",
                "Repair the Windows fonts (Tahoma, Segoe UI, Consolas)." },
            { ErrorCode::ImGuiFailed, "The user interface could not be started.",
                "The interface library did not start with the window or the device.",
                kReport },
            { ErrorCode::SettingsNotSaved, "The settings could not be saved to {}.",
                "The configs folder next to the program is not writable.",
                "Move the program to a folder you can write to, or run it as administrator." },
            { ErrorCode::Crashed, "The program crashed ({}).",
                "An unexpected error stopped the program.",
                kReport },
            { ErrorCode::LastRunCrashed, "The last run crashed. A report is in {}.",
                "The program ended through an unexpected error last time.",
                kReport },
            { ErrorCode::NotElevated, "The program runs without administrator rights.",
                "Many games run elevated or protect their memory, and then they cannot be opened.",
                "Restart as administrator from the Help menu if a process cannot be opened." },
            { ErrorCode::ReportNotSaved, "The diagnostics report could not be written to {}.",
                "The folder does not exist or is not writable.",
                "Save it somewhere else, or copy it to the clipboard instead." },

            { ErrorCode::TableNotRead, "The table {} could not be read.",
                "The file is missing or not readable.",
                "Check the path and that the file is not open with exclusive access elsewhere." },
            { ErrorCode::TableNotXml, "The table is not valid XML: {}.",
                "The file is damaged or not a cheat table.",
                "Open it in a text editor and fix the line named in the message." },
            { ErrorCode::TableNotCheatTable, "The file is not a cheat table.",
                "The XML has no CheatTable root element.",
                "Open a .CT file made by Cheat Engine or by this program." },
            { ErrorCode::TableNotSaved, "The table could not be saved to {}.",
                "The folder is not writable or the disk is full.",
                "Save it somewhere else." },
            { ErrorCode::TableEntryDamaged, "Entry {} of the table is damaged and was skipped.",
                "An entry misses required parts or has values that do not parse.",
                "Check that entry in a text editor." },
            { ErrorCode::TableTooNew, "The table was written by a newer version ({}).",
                "Parts of it may be unknown to this version and are kept as they are.",
                "Usually fine. Check the records that do not work." },

            { ErrorCode::LuaSyntax, "Lua syntax error: {}.",
                "The script does not compile.",
                kCheckScript },
            { ErrorCode::LuaRuntime, "Lua error: {}.",
                "The script raised an error while running.",
                kCheckScript },
            { ErrorCode::LuaAutorunFailed, "The autorun script {} failed.",
                "A script in the autorun folder raised an error at start.",
                "Fix or remove the script. The log names the line." },
            { ErrorCode::LuaTimerFailed, "A Lua timer was stopped after an error: {}.",
                "The timer's function raised an error, so the timer was disabled to stop the error repeating.",
                "Fix the function and enable the timer again." },
            { ErrorCode::LuaTableScriptFailed, "The table's Lua script failed: {}.",
                "The script stored in the table raised an error when the table was loaded.",
                "Open Table > Show table Lua script and fix it." },
            { ErrorCode::LuaBadArgument, "Lua: {}.",
                "A Manifold function got an argument of the wrong type or value.",
                "See Docs/LUA-API.md for the arguments." },
            { ErrorCode::LuaApiMisuse, "The manifold API was called the wrong way: {}.",
                "A manifold function was called with a colon, got a missing, extra or wrong argument or an unknown option, or a name that does not exist was read.",
                "The message names the function and the argument. Docs/MANIFOLD-API.md has the rules and every function." },
            { ErrorCode::LuaDocMissing, "There is no documentation for {}.",
                "Nothing documents the function or name: no Manifold, Cheat Engine or Lua entry, no manifold.doc.register call and no LuaLS annotation above its definition.",
                "Check the name, or document it with manifold.doc.register, or with ---@param annotations and manifold.doc.registerModule." },
            { ErrorCode::LuaImageUnreadable, "The image {} could not be read.",
                "manifold.ui.image.load reads PNG, JPEG, BMP, TGA and GIF files; the file is missing, unreadable or in another format.",
                "Check the path (relative paths start at the program folder) and that the file opens in an image viewer." },
            { ErrorCode::LuaUiGone, "The {} is gone.",
                "The window, widget, menu entry or dialog was removed, closed for good, or its script ran again, which removes everything the script made before.",
                "Keep the new handle the script's next run returns, or test with isOpen before using an old one." },

            { ErrorCode::ExprSyntax, "\"{}\" is not a valid address.",
                "The address expression could not be read.",
                "Write addresses like game.exe+1234, [game.exe+10]+8, or a hex number." },
            { ErrorCode::ExprUnknownSymbol, "Unknown symbol \"{}\".",
                "The name is neither a module, an export nor a registered symbol.",
                "Check the spelling. Symbols of scripts exist only while the script is active." },
            { ErrorCode::ExprPointerUnreadable, "The pointer at {} could not be read.",
                "A [ ] part of the address points to unreadable memory.",
                "The pointer path is wrong or the object does not exist right now." },
            { ErrorCode::ExprDivideByZero, "Division by zero in \"{}\".",
                "The address expression divides by zero.",
                "Fix the expression." },
            { ErrorCode::SymbolNameTaken, "The name {} is taken.",
                "Another symbol with this name exists.",
                "Pick another name." },

            { ErrorCode::TemplateSyntax, "The template has a syntax error: {}.",
                "A << >> or <% %> block in the template or its settings file is not valid Lua, or a block is not closed.",
                "Fix the template at the reported line." },
            { ErrorCode::TemplateRender, "The template stopped with an error: {}.",
                "Lua code inside the template raised an error while the script was generated.",
                "Fix the template at the reported line." },
            { ErrorCode::TemplateRequirement, "The template needs {}, which could not be determined.",
                "The template lists the value under Requires, and it came out empty for the selected address.",
                "Pick an address the template supports, or remove the value from Requires." },
            { ErrorCode::TemplateContext, "{}",
                "A value the template uses could not be worked out, or a question was cancelled.",
                "Open a process and select an instruction in the memory viewer first." },
            { ErrorCode::TemplateNotFound, "The template file {} was not found.",
                "include() looks in the Partials folder, then in the template folder, then in the built-in templates.",
                "Put the file there or correct the name." },
            { ErrorCode::TemplateIncludeCycle, "{}.",
                "Templates include each other in a circle, or the includes nest more than 16 levels deep.",
                "Remove the include that leads back." },
            { ErrorCode::TemplateArchitecture, "{}",
                "The template declares the architectures it supports, and the attached process is another one.",
                "Use a template for this architecture." },
            { ErrorCode::TemplateWrite, "{}",
                "A template file could not be written, moved or removed, or the action is not possible for this template.",
                "Check that the templates folder is writable and the file is not open elsewhere. Built-in templates change only through a copy." },
        };

        static_assert(std::size(kErrors) == std::size(kErrorCodes));

        std::string Number(ErrorCode code)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "%04u", static_cast<unsigned>(code));
            return text;
        }

        std::string Fill(std::string_view message, std::string_view detail)
        {
            std::string result(message);
            const std::size_t at = result.find(kPlace);
            if (at != std::string::npos)
                result.replace(at, kPlace.size(), detail.empty() ? std::string_view("?") : detail);
            return result;
        }

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }
    }

    std::span<const ErrorInfo> ErrorTable()
    {
        return kErrors;
    }

    const ErrorInfo* FindError(ErrorCode code)
    {
        for (const ErrorInfo& info : kErrors)
        {
            if (info.Code == code)
                return &info;
        }
        return nullptr;
    }

    const ErrorInfo* FindError(std::string_view written)
    {
        for (const std::string_view prefix : { kPrefix, kLegacyPrefix })
        {
            if (written.size() < prefix.size())
                continue;
            std::string upper(written.substr(0, prefix.size()));
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
            if (upper == prefix)
            {
                written.remove_prefix(prefix.size());
                break;
            }
        }
        if (written.empty() || written.size() > 5 || !std::all_of(written.begin(), written.end(), IsDigit))
            return nullptr;
        unsigned value = 0;
        for (char c : written)
            value = value * 10 + static_cast<unsigned>(c - '0');
        return FindError(static_cast<ErrorCode>(value));
    }

    const ErrorInfo* ErrorOf(std::string_view message)
    {
        const std::size_t open = message.rfind("(" + std::string(kPrefix));
        if (open == std::string_view::npos)
            return nullptr;
        const std::size_t close = message.find(')', open);
        if (close == std::string_view::npos)
            return nullptr;
        return FindError(message.substr(open + 1, close - open - 1));
    }

    std::string ErrorName(ErrorCode code)
    {
        return std::string(kPrefix) + Number(code);
    }

    std::string_view ErrorArea(ErrorCode code)
    {
        const int area = static_cast<int>(code) / 1000;
        for (const Area& entry : kAreas)
        {
            if (entry.First == area)
                return entry.Name;
        }
        return "Other";
    }

    std::string ErrorMessage(ErrorCode code, std::string_view detail)
    {
        const ErrorInfo* info = FindError(code);
        if (!info)
            return std::string(detail.empty() ? std::string_view("Unknown error") : detail) + " (" + ErrorName(code) + ")";
        return Fill(info->Message, detail) + " (" + ErrorName(code) + ")";
    }

    std::string ErrorsMarkdown()
    {
        std::string text = "# Error codes\n\n"
                           "Every failure Manifold Engine reports ends in a code like `ME-1006`. "
                           "The number never changes its meaning between versions. "
                           "Hover a coded message in the program to see the cause and what helps; a click copies it.\n";
        std::string_view area;
        for (const ErrorInfo& info : kErrors)
        {
            if (ErrorArea(info.Code) != area)
            {
                area = ErrorArea(info.Code);
                text += "\n## " + std::string(area) + "\n\n| Code | Message | Cause | Help |\n| --- | --- | --- | --- |\n";
            }
            std::string message = Fill(info.Message, "…");
            for (std::size_t at = message.find('|'); at != std::string::npos; at = message.find('|', at + 2))
                message.replace(at, 1, "\\|");
            text += "| " + ErrorName(info.Code) + " | " + message + " | " + std::string(info.Cause) + " | " + std::string(info.Help) + " |\n";
        }
        return text;
    }
}
