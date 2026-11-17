--[[
    The one place the SigMaker version is written down.

    The README, the About entry and the line printed at startup all read it
    from here. No other file in the tree carries a version number of its own,
    so this is the only number that ever has to be raised.

    Changelog:
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
    Minor = 0,
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
