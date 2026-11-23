local manifold = manifold
local signature = manifold.signature

local SigMaker = {}
SigMaker.VERSION = "3.0.0"

local function text(problem, fallback)
  if type(problem) == "table" and problem.message then
    return problem.message
  end
  return problem ~= nil and tostring(problem) or fallback
end

local function selected()
  local ok, address = pcall(function() return getMemoryViewForm().DisassemblerView.SelectedAddress end)
  if ok and math.type(address) == "integer" and address ~= 0 then
    return address
  end
  return nil
end

function SigMaker:Make(address)
  address = address or selected()
  if address == nil then
    return nil, "no address was given"
  end
  local made, problem = signature.make(address)
  if not made then
    return nil, text(problem, "no signature")
  end
  return made
end

function SigMaker:Pattern(address)
  local made, problem = self:Make(address)
  if not made then
    return nil, problem
  end
  return made.pattern, made
end

function SigMaker:Copy(address)
  local made, problem = self:Make(address)
  if not made then
    return nil, problem
  end
  if type(writeToClipboard) == "function" then
    writeToClipboard(made.pattern)
  end
  return made.pattern
end

function SigMaker:Scan(pattern)
  local hits, problem = signature.find(pattern)
  if not hits then
    return nil, text(problem, "not a signature")
  end
  return hits
end

function SigMaker:Find(pattern)
  return self:Scan(pattern)
end

function SigMaker:Status()
  return "The signature maker is built into Manifold Engine " .. tostring(manifold.version) .. " (manifold.signature)."
end

rawset(_G, "ManifoldSigMaker", SigMaker)

manifold.doc.registerModule("ManifoldSigMaker", SigMaker, { doc = "The names of the Manifold-SigMaker autorun module, kept for tables and scripts written against it. The signature maker is built into the program now: Make, Pattern, Copy and Scan call manifold.signature with the settings under Settings > Signatures. Call them with a colon." })
