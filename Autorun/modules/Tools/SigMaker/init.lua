--[[
    Manifold-SigMaker
    --------------------------------

    NAME        : Manifold-SigMaker
    VERSION     : 2.1.0
    AUTHOR      : Leunsel
    DESCRIPTION : Byte signatures for the selected instruction, checked for uniqueness in their module, and a search that finds them again.
    API         : 10
    LICENSE     : MIT
    CREATED     : 2026-09-03

    MIT License:
        Copyright (c) 2026 Leunsel

        Permission is hereby granted, free of charge, to any person obtaining a copy
        of this software and associated documentation files (the "Software"), to deal
        in the Software without restriction, including without limitation the rights
        to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
        copies of the Software, and to permit persons to whom the Software is
        furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be included in all
        copies or substantial portions of the Software.

        THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
        IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
        FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
        AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
        LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
        OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
        SOFTWARE.

    This is the entry point of an autorun module. Copy the whole SigMaker
    folder into autorun\modules next to Manifold-Engine.exe (a category
    folder in between, such as Tools, is fine). The next time Manifold Engine
    starts, the context menu in the disassembler carries two new entries,
    "Manifold: Copy Signature" and "Manifold: Find Signature", and the memory
    view's menu bar carries a small "Manifold" entry that holds the keyboard
    shortcut for the second one.

    Since 2.0.0 this runs on Manifold Engine only. Process, module, memory and
    scan calls go through its manifold API, and the log is the manifold.log
    channel "SigMaker". Stock Cheat Engine is no longer supported. Since
    2.1.0 it is an autorun module: the API line above is checked by the
    program before this file runs, and require finds the other files of this
    folder without a package.path entry.

    Everything those entries do is also available as a method.

        ManifoldSigMaker:Copy()
            Builds a signature for the address selected in the disassembler
            and puts it on the clipboard.

        ManifoldSigMaker:Pattern(0x14D762ED9)
            Returns only the scan pattern for the address it is given.

        ManifoldSigMaker:Find()
            Asks for a signature, offering the clipboard, scans the process
            for it and goes to where it matched. One hit is a jump. Several
            are a list that stays open, and a click on a line goes there.

        ManifoldSigMaker:Find("48 8B 4C 24 ? 48 83 EC 28")
            The same without the prompt.

        ManifoldSigMaker:Scan(pattern)
            The addresses on their own, with no prompt, list or jump.

        ManifoldSigMaker:Goto("game.exe+1A2B")
            Puts the memory view on an address.

        ManifoldSigMaker:Status()
            Reports what is loaded and how it is currently configured.

    Reloading the module (Extensions > Modules, or
    manifold.module.reload("Manifold-SigMaker")) is the normal way to pick up
    an edit. The menu entries and any open list of hits from the previous run
    are taken down before the new ones are built, and the files are read
    from disk again, so nothing accumulates and no old code stays behind.

    The masking policy and the search keep their defaults in
    Manifold-SigMaker-Settings.lua. They can be overridden below. The version
    number lives in Manifold-SigMaker-Version.lua.
]]

--
--- ∑ The program has checked the API line of the header before this file
---   runs, and it resolves require inside this folder and reads the files
---   from disk again on every reload. So there is no gate, no package.path
---   entry and no list of names to purge here any more. What is left is
---   asking where the folder is, because the icons are opened by path.
--
local here = manifold.error.check(manifold.module.current())

-- This has to stay the same as Manifold-SigMaker-Host.GlobalKey.
local HOST_KEY = "ManifoldSigMakerHost"

-- The same channel Manifold-SigMaker-Log writes to. A channel name always
-- gives back the same object, so this is not a second one.
local log = manifold.log.channel("SigMaker")

--
--- ∑ The generation before this one takes its entries down first. A failure
---   there must not stop the new generation from being built, because the
---   new one sweeps the menus for the same tag anyway, so it is logged and
---   the run goes on.
--
local previous = rawget(_G, HOST_KEY)
if type(previous) == "table" and type(previous.Uninstall) == "function" then
    local removed, problem = manifold.error.try(previous.Uninstall, previous)
    if not removed then log.warning(problem) end
end

-- Someone who copied this file on its own and left the rest behind gets one
-- readable line here. The alternative is a require traceback on every single
-- Manifold Engine start. The error is handed over as text and cut to its first
-- line, because require lists every path it tried below it, and an error
-- object would bring the traceback along as well.
local okHost, Host = manifold.error.try(require, "Manifold-SigMaker-Host")
if not okHost then
    log.error(string.format("The other files of the SigMaker were not found next to init.lua in %s. " ..
        "Copy the whole folder. (%s)", here.path, tostring(Host.message):match("^[^\r\n]*")))
    return nil
end
local Version = require("Manifold-SigMaker-Version")

local host = Host:New({
    Root = here.path,
    -- Anything put here overrides Manifold-SigMaker-Settings.Defaults, so:
    -- Settings = { Scope = "process", Mask = { Immediate = true } }
})

-- ∑ The menu entries and the hit lists are Cheat Engine objects, which the
--   program does not track, so the module takes them down itself when it is
--   unloaded or switched off.
manifold.module.onUnload(function()
    local removed, problem = manifold.error.try(host.Uninstall, host)
    if not removed then log.warning(problem) end
end)

_G[HOST_KEY] = host
_G[Host.FacadeKey] = host
if manifold.has("doc.registerModule") then
    manifold.doc.registerModule(Host.FacadeKey, host, { doc = "The signature maker: a unique byte pattern for an address, the hits of a pattern in the opened process, and copying and going to them. Call its functions with a colon." })
end
if type(registerLuaFunctionHighlight) == "function" then
    registerLuaFunctionHighlight(Host.FacadeKey)
end

-- At autorun time the memory view may not exist yet. Installing on demand
-- keeps the menu entry working without forcing that window open at startup.
local installed, reason = host:Install()
if not installed then
    host.Log:Debug("The disassembler menu entries are not installed yet: " .. tostring(reason) ..
        ". Call ManifoldSigMaker:Install() once the memory view is open.")
end

-- ∑ The status block is debug, not info: with Manifold Engine's "Show on
--   print" on, an info line at startup would open the Lua engine window every
--   time the program starts. manifold.log.setLevel("debug") shows it again.
host.Log:Debug(host.Log:Block(Version.Full() .. (previous and " re-executed" or " ready"),
    host:StatusRows()))

return host
