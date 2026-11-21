--[[
    The one place the SigMaker version is written down.

    The README, the About entry and the line printed at startup all read it
    from here. The one other place is the VERSION line in the header of
    init.lua, which Manifold Engine reads for its module list without running
    anything, so the two are raised together.

    Changelog:
        ∂ v2.1.0 (2026-11-21)
            - an autorun module now (autorun\modules\Tools\SigMaker): the
              entry is init.lua, its files are found by the module's own
              require instead of a package.path entry, the icons through
              manifold.module.current().path, and the menu entries are taken
              down when the module is unloaded or switched off
        ∂ v2.0.0 (2026-11-17)
            - ported to Manifold Engine's manifold API 2; stock Cheat Engine is
              no longer supported
            - process, module, memory and scan calls go through manifold;
              uniqueness counts scan only the module instead of the process
            - the log is the manifold.log channel "SigMaker"
            - the actions and the menu entries catch errors with
              manifold.error.try and log them with their traceback
]]

local Version = {
    Major = 2,
    Minor = 1,
    Patch = 0,
    Name = "Manifold SigMaker"
}

function Version.String()
    return string.format("%d.%d.%d", Version.Major, Version.Minor, Version.Patch)
end

function Version.Full()
    return Version.Name .. " " .. Version.String()
end

return Version
