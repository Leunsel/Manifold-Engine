--[[
    The other direction: a signature comes in, an address comes out.

    Manifold-SigMaker-Pattern says what a pasted text means. This module
    decides what to do with it, and that is where the policy lives:

    A PATTERN THAT IS TOO SHORT is refused. Two fixed bytes match in tens of
    thousands of places in any real process, so the scan runs into its count
    limit, takes the interface with it for as long as the walk over memory
    lasts, and answers with a list nobody can use. On Cheat Engine it was
    worse, because AOBScan built the complete result list first, millions of
    entries, before a caller could look at any of it. The threshold is a
    setting, and the reason says how to lower it, because a deliberate two
    byte scan of a tiny process is a legitimate thing to want.

    THE SEARCH STARTS IN EXECUTABLE MEMORY. That is where code is, a signature
    from this tool describes code, and restricting the scan is what keeps it
    quick in a process with a gigabyte of heap. A signature that describes
    data finds nothing there, so a scan that comes back empty is widened to
    all memory once before it reports failure. Both halves are reported, so
    "not found" never hides which memory was actually searched.

    A HIT COUNT IS NOT A RESULT LIST. The scanner is asked for the addresses
    up to MaxResults, and the total it found regardless. A pattern with 40000
    matches then reports the 40000 rather than pretending there were 100.
    manifold.memory.scan stops counting at a limit of its own, CE.CountLimit
    in Manifold-SigMaker-CE, so a total that reaches it is reported as that
    many or more.

    Nothing here opens a window or moves the memory view. Scan answers what is
    there, and Manifold-SigMaker-Host is what does something with it.
]]

local Pattern = require("Manifold-SigMaker-Pattern")

local Find = {}
Find.__index = Find

function Find:New(deps)
    return setmetatable({
        CE = deps.CE,
        Log = deps.Log,
        Settings = deps.Settings
    }, Find)
end

--------------------------------------------------------
--                       Scanning                     --
--------------------------------------------------------

--
--- ∑ Reads a pasted signature and scans the attached process for it.
--- @param text string
--- @return table|nil, string|nil # { Pattern, Addresses, Count, Total,
---         Truncated, Capped, Protection, Widened, Elapsed } or nil and a
---         reason. Capped means Total is where the scanner stopped counting.
--
function Find:Scan(text)
    local ce, settings = self.CE, self.Settings
    local parsed, reason = Pattern.Parse(text)
    if not parsed then return nil, reason end

    local minimum = tonumber(settings.Find.MinFixedBytes) or 0
    if parsed.Fixed < minimum then
        -- No full stop at the end. Every caller writes one after the reason.
        return nil, string.format(
            "the pattern has %d fixed byte(s) and %d are needed. A pattern that short matches " ..
            "in thousands of places, and the scan counts up to %d of them before the first " ..
            "can be read. ManifoldSigMaker:SetFindMinimum(%d) allows it anyway",
            parsed.Fixed, minimum, ce.CountLimit, math.max(parsed.Fixed, 1))
    end
    if not ce:ProcessOpen() then return nil, "no process is attached" end

    local limit = tonumber(settings.Find.MaxResults)
    if limit and limit < 1 then limit = nil end
    local protection = settings.Find.Protection or ""
    local started = os.clock()

    local result, scanError = ce:ScanMatches(parsed.Pattern,
        { Protection = protection, Limit = limit })
    if not result then return nil, scanError end

    local widened = false
    if result.Count == 0 and settings.Find.Fallback and protection ~= "" then
        widened, protection = true, ""
        local second, secondError = ce:ScanMatches(parsed.Pattern,
            { Protection = protection, Limit = limit })
        if not second then return nil, secondError end
        result = second
    end

    return {
        Pattern = parsed,
        Addresses = result.Addresses,
        Count = result.Count,
        Total = result.Total,
        Truncated = result.Truncated,
        Capped = result.Capped,
        Protection = protection,
        Widened = widened,
        Elapsed = os.clock() - started
    }
end

--------------------------------------------------------
--                      Reporting                     --
--------------------------------------------------------

--- How many hits are listed in a log block before it says "and N more".
Find.ListedInBlock = 25

--- Where memory with these flags was searched, in words.
function Find.Where(protection)
    if protection == nil or protection == "" then return "all memory" end
    if protection == "+X" then return "executable memory" end
    return "memory matching " .. tostring(protection)
end

--
--- ∑ How one hit is written, both in the list of hits and in the log.
--- @param address number
--- @return string
--
function Find:Name(address)
    local hex = string.format("%X", address)
    local name = self.CE:AddressName(address)
    if name == hex then return hex end
    return string.format("%s  (%s)", name, hex)
end

--- The total as words: the count, or the count and "or more" when the
--- scanner stopped counting there.
local function totalOf(result)
    if result.Capped then return string.format("%d or more", result.Total) end
    return tostring(result.Total)
end

--- The lines of the list of hits, in the order the scanner found them.
function Find:Lines(result)
    local lines = {}
    for index, address in ipairs(result.Addresses) do
        lines[index] = string.format("%d.  %s", index, self:Name(address))
    end
    return lines
end

--
--- ∑ The title of the list of hits. The count comes first and the pattern
---   after it, because a title too long for its window loses its end.
--- @param result table
--- @return string
--
function Find:Title(result)
    local pattern = result.Pattern and result.Pattern.Pattern or ""
    if result.Total > result.Count then
        return string.format("The first %d of %s matches for %s",
            result.Count, totalOf(result), pattern)
    end
    return string.format("%d matches for %s", result.Count, pattern)
end

--
--- ∑ The rows of the log block for a finished scan.
--- @param result table
--- @return table
--
function Find:Rows(result)
    local parsed = result.Pattern
    local composition = string.format("%d, %d wildcarded", parsed.Tokens, parsed.Wildcards)
    if parsed.Partial > 0 then
        composition = composition .. string.format(", %d half wildcarded", parsed.Partial)
    end

    local hits
    if result.Total > result.Count then
        hits = string.format("%s, showing the first %d", totalOf(result), result.Count)
    elseif result.Capped then
        hits = totalOf(result)
    else
        hits = tostring(result.Count)
    end

    local rows = {
        { "Pattern", parsed.Pattern },
        { "Bytes", composition },
        { "Searched", Find.Where(result.Protection) },
        result.Widened and { "Widened", "nothing matched in executable memory, so the scan "
            .. "was repeated over all of it" } or false,
        { "Hits", hits },
        { "Time", string.format("%.2f s", result.Elapsed) }
    }
    for _, note in ipairs(parsed.Notes or {}) do
        rows[#rows + 1] = { "Note", note }
    end
    if result.Count > 0 then rows[#rows + 1] = "" end
    for index, address in ipairs(result.Addresses) do
        if index > Find.ListedInBlock then
            rows[#rows + 1] = string.format("... and %d more",
                result.Count - Find.ListedInBlock)
            break
        end
        rows[#rows + 1] = string.format("%d.  %s", index, self:Name(address))
    end
    return rows
end

--
--- ∑ Why a scan that worked found nothing, in one sentence that says where
---   it looked.
--- @param result table
--- @return string
--
function Find:NothingFound(result)
    if result.Widened then
        return "the pattern matches nowhere in this process, in executable memory or outside it"
    end
    return string.format("the pattern matches nowhere in %s", Find.Where(result.Protection))
end

return Find
