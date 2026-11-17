--[[
    One place for this tool to log to.

    Every line goes through the manifold.log channel named "SigMaker". The
    lines then appear in the Lua engine output beside the other Manifold
    tools, coloured by level and tagged with the channel, and warnings and
    errors also reach the program log with the script line that wrote them.

    The channel is fetched once. manifold.log hands back the same object for
    the same name for as long as the program runs, so there is nothing to
    look up again on every call. The 1.x version had to, because the Manifold
    Logger it wrote through could be shut down and built again under it.

    The methods keep the names and the colon calls the other modules use.
    Only the channel underneath is called with a dot, which is the manifold
    API's rule.
]]

local Log = {}
Log.__index = Log

Log.ChannelName = "SigMaker"

function Log:New(options)
    options = options or {}
    return setmetatable({
        Channel = manifold.log.channel(options.Name or Log.ChannelName)
    }, Log)
end

--- Whether the lines reach a log channel. Always, since the channel is part
--- of the API this version requires.
function Log:Attached()
    return self.Channel ~= nil
end

--
--- ∑ Each level goes to the channel function of the same name. fields is an
---   optional table the channel appends as { key = value }, and an error
---   object passed as the message brings its script line and its traceback.
--- @param message any
--- @param fields table|nil
--
function Log:Debug(message, fields) self.Channel.debug(message, fields) end
function Log:Info(message, fields) self.Channel.info(message, fields) end
function Log:Warning(message, fields) self.Channel.warning(message, fields) end
function Log:Error(message, fields) self.Channel.error(message, fields) end

--
--- ∑ Renders a report of several rows as one log record. A row is either a
---   table holding a label and a value, or a bare string. A row written as
---   false is skipped, which lets a conditional row be written inline. A nil
---   row would end the walk early and lose every row after it.
---
---   The record goes out as one message of several lines, so a report is one
---   entry of the log with its rows under the title, and not one entry per
---   row.
--- @param title string
--- @param rows table
--- @return string
--
function Log:Block(title, rows)
    local width = 0
    for _, row in ipairs(rows) do
        if type(row) == "table" and row[1] ~= nil then
            width = math.max(width, #tostring(row[1]))
        end
    end
    local lines = { tostring(title) }
    for _, row in ipairs(rows) do
        if type(row) == "table" then
            lines[#lines + 1] = string.format("  %-" .. width .. "s : %s",
                tostring(row[1]), tostring(row[2]))
        elseif type(row) == "string" then
            lines[#lines + 1] = row == "" and "" or ("  " .. row)
        end
    end
    return table.concat(lines, "\n")
end

return Log
