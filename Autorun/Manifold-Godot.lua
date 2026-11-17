--[[
    Manifold-Godot.lua
    --------------------------------

    AUTHOR  : Leunsel
    VERSION : 2.0.0
    LICENSE : MIT
    CREATED : 2026-11-17
    UPDATED : 2026-11-17

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

    This file is part of the Manifold CE system, ported to Manifold Engine.

    CHANGELOG:
        ∂ v2.0.0 (2026-11-17)
            - ported to Manifold Engine's manifold API 2; stock Cheat Engine
              is no longer supported. Logging goes through manifold.log
              channels, memory through manifold.memory (raw blocks for the
              large scans), process changes through manifold.event, and the
              symbols, the symbol resolver and the dissect hooks through
              keyed manifold registrations. The engine drops registered
              symbols with their process, so the per-process symbol list is
              gone. ManifoldGodot calls log a failure and return nil and the
              error instead of raising.
        ∂ v1.0.0 (2026-11-17)
            - first release for Cheat Engine.

    Godot 4 engine data for Manifold Engine, without symbols, without game-specific
    offsets and without signatures. The engine's own bookkeeping is found in
    the running game: the StringName table, the ObjectDB slot array, ClassDB's
    class table and the SceneTree. Every member offset used here is probed in
    the live process, so a release or debug template, an MSVC or a MinGW build
    and the layout changes between 4.x releases are told apart by the data
    itself instead of by a version table.

    What it adds to Manifold Engine:
        - Structure Dissect knows Godot objects. "Define new structure" names
          the structure after the object's class and moves to the object's
          start; the fields are the Object and Node members the engine keeps
          (instance id, script instance, metadata, name, parent, owner,
          children, scene tree) and the native bytes up to the class's real
          size. Expanding the children or metadata list fills its entries,
          expanding a script instance shows its script and, for C#, the
          managed object handle.
        - Custom types "Godot StringName", "Godot String" and "Godot
          ObjectID" that show names, strings and object references as text.
        - Symbols: GodotStringNames, GodotObjectDB, GodotClassDB,
          GodotSceneTree, member offsets as Godot<Class>_<member>
          (GodotNode_parent, GodotObject_script_instance, ...) and nodes by
          path as GodotPath__root__Main__Player, so address list entries
          survive a restart of the game.
        - Engine method names in the disassembler (Node::get_name, ...), read
          from the method binds ClassDB keeps for every class.
        - A "Godot Engine" menu: dissects of the scene tree and its root, a
          node by path, the printed scene tree, nodes by name, instances of a
          class and "who owns this address".
        - ManifoldGodot, a Lua API for tables and the Lua console.

    Everything here only reads the game. Nothing is injected, hooked or called
    inside the target; writes happen only where the user edits a value.

    Supported: x64 Godot 4 games. Verified on 4.6.3 (.NET, clang-cl build)
    with version 1 of this script; the port keeps every probe, offset and
    threshold of that version. Godot 3 keeps its objects and names in
    different structures and is reported as not supported.

    Needs Manifold Engine with the manifold Lua API 2 or newer.
]]

-- ∑ The manifold API is the only host this version talks to. Without it
-- the script says so once and installs nothing, instead of half-working
-- through Cheat Engine fallbacks that no longer exist.
local manifold = rawget(_G, "manifold")
if type(manifold) ~= "table" or (tonumber(manifold.apiVersion) or 0) < 2 then
    print("Manifold-Godot needs Manifold Engine's manifold API 2 or newer")
    return
end

----------------------------------------
-- CONFIG
----------------------------------------
local RUNTIME_STATE_KEY = "__MANIFOLD_GODOT_RUNTIME__"
local REGISTRATION_KEY  = "Manifold-Godot"
local VERSION           = "2.0.0"
local MENU_CAPTION      = "Godot Engine"

local POINTER_MIN, POINTER_MAX = 0x10000, 0x7FFFFFFFFFFF

-- StringName::Table: TABLE_BITS has been 16 in every Godot 4 release.
local NAME_TABLE_SLOTS = 0x10000
local NAME_TABLE_MASK  = NAME_TABLE_SLOTS - 1
local NAME_TABLE_BYTES = NAME_TABLE_SLOTS * 8
local NAME_SAMPLES     = 1200
local STRING_LIMIT     = 4096

-- ObjectID: slot in bits 0..23, validator in bits 24..62, bit 63 for RefCounted.
local SLOT_BITS      = 24
local SLOT_MASK      = (1 << SLOT_BITS) - 1
local VALIDATOR_MASK = (1 << 39) - 1
local REF_BIT        = math.mininteger
local MAX_SLOTS      = 1 << 20
local ID_SCAN        = 0x100

-- HashMap<K, V>: elements, hashes, head, tail, capacity index, size. The
-- elements form a list in insertion order: next, prev, key, value.
local MAP_HEAD, MAP_TAIL, MAP_CAPACITY, MAP_SIZE = 0x10, 0x18, 0x20, 0x24
local ELEMENT_NEXT, ELEMENT_PREV, ELEMENT_KEY, ELEMENT_VALUE = 0x00, 0x08, 0x10, 0x18
local MAP_LIMIT = 200000

local CLASSINFO_SCAN = 0x400
local OBJECT_SCAN    = 0x400
local DEFAULT_SIZE   = 0x200
local SORTED_TTL     = 2000
local CHILD_LIMIT    = 100000
local ELEMENT_MEMORY = 20000
local ELEMENT_LIMIT  = 6000
local GAP_LIMIT      = 0x4000
local TREE_RETRY     = 3000

local CUSTOM_NAME   = "Godot StringName"
local CUSTOM_STRING = "Godot String"
local CUSTOM_ID     = "Godot ObjectID"

local NODE_SYMBOL_PREFIX = "GodotPath__"

-- Variable types as defines.lua publishes them; the literals are the fallback.
local VT = {
    Byte    = rawget(_G, "vtByte") or 0,
    Word    = rawget(_G, "vtWord") or 1,
    Dword   = rawget(_G, "vtDword") or 2,
    Qword   = rawget(_G, "vtQword") or 3,
    Single  = rawget(_G, "vtSingle") or 4,
    Double  = rawget(_G, "vtDouble") or 5,
    String  = rawget(_G, "vtString") or 6,
    Bytes   = rawget(_G, "vtByteArray") or 8,
    Pointer = rawget(_G, "vtPointer") or 12,
    Custom  = rawget(_G, "vtCustom") or 13,
}

local VARIANT_TYPES = {
    [0] = "Nil", "bool", "int", "float", "String", "Vector2", "Vector2i", "Rect2", "Rect2i",
    "Vector3", "Vector3i", "Transform2D", "Vector4", "Vector4i", "Plane", "Quaternion", "AABB",
    "Basis", "Transform3D", "Projection", "Color", "StringName", "NodePath", "RID", "Object",
    "Callable", "Signal", "Dictionary", "Array", "PackedByteArray", "PackedInt32Array",
    "PackedInt64Array", "PackedFloat32Array", "PackedFloat64Array", "PackedStringArray",
    "PackedVector2Array", "PackedVector3Array", "PackedColorArray", "PackedVector4Array",
}

-- ∑ Only what has to outlive one execution of this file lives outside the
-- chunk: the custom types (Manifold Engine registers a custom type once per
-- run of the program, so they dispatch through this table), the menu (a
-- Cheat Engine object that has to be destroyed before a new one is built)
-- and the user's dissect toggle. The resolver, the dissect hooks and the
-- event handlers are registered under REGISTRATION_KEY, so executing the
-- file again replaces them instead of stacking them, without ids kept here.
local State = rawget(_G, RUNTIME_STATE_KEY)
if type(State) ~= "table" then
    State = {}
    _G[RUNTIME_STATE_KEY] = State
end

ManifoldGodot = {}
local Godot = ManifoldGodot

-- ∑ The public functions are written into Api and handed out through
-- ManifoldGodot by one wrapper (see API); the menu calls Api directly, so its
-- own guard sees the error and can show it in a dialog.
local Api = {}

----------------------------------------
-- LOGGING
----------------------------------------

-- ∑ One channel for the script and a sub-channel per stage, so a line
-- reads [Godot/Resolve] like the [Resolve] tag of version 1 did.
local log = manifold.log.channel("Godot")
local Channel = {
    Resolve     = log.channel("Resolve"),
    Methods     = log.channel("Methods"),
    Dissect     = log.channel("Dissect"),
    Menu        = log.channel("Menu"),
    API         = log.channel("API"),
    CustomTypes = log.channel("CustomTypes"),
}

--
--- ∑ Resolve failures carry their own codes, so Ready tells the cases apart:
--- a process that is no 64-bit Godot 4 game is declined quietly (every other
--- game ends there, and Manifold-UE and Manifold-RTTI share the process), a
--- Godot 3 game gets a warning, and a Godot 4 game whose bookkeeping was not
--- found is an error that names the stage. Anything else is a bug of this
--- script and is logged with its traceback.
--
local FAIL = { declined = "GODOT-DECLINED", unsupported = "GODOT-UNSUPPORTED", stage = "GODOT-STAGE" }

local function Fail(code, message)
    manifold.error.raise(message, { code = code })
end

local function Hex(value)
    return value and string.format("%X", value) or "nil"
end

local function Ticks()
    return getTickCount()
end

----------------------------------------
-- MEMORY
----------------------------------------

-- ∑ The two manifold functions that run in loops are cached locals: the
-- plain typed reads take the API's fast path, and the method names are
-- registered by the thousand.
local read = manifold.memory.read
local registerSymbol = manifold.symbol.register

local function IsPtr(value)
    return math.type(value) == "integer" and value >= POINTER_MIN and value <= POINTER_MAX and value & 7 == 0
end

-- ∑ Reads hand back value or nil, error; the parentheses keep only the
-- value, so a failed read is a plain nil wherever these helpers are used as
-- the last argument of a call.
local function RP(address)
    if not address or address == 0 then return nil end
    return (read(address, "pointer"))
end

local function RPtr(address)
    local value = RP(address)
    return IsPtr(value) and value or nil
end

local function RQ(address)
    if not address or address == 0 then return nil end
    return (read(address, "u64"))
end

local function RU(address)
    if not address or address == 0 then return nil end
    return (read(address, "u32"))
end

local function RS(address, length)
    if not address or address == 0 then return nil end
    return (read(address, "string", { length = length }))
end

--
--- ∑ Raw bytes as a Lua string, decoded with string.unpack: far cheaper than
--- a read call per value. A raw read is all or nothing, like the readBytes
--- check of version 1, and takes at most 16 MB, so only larger blocks go in
--- pieces. The options table of each size is made once: the same few sizes
--- are read thousands of times in the scans, and the API's raw fast path
--- takes a table that holds nothing but count.
--
local RAW_PIECE = 0x1000000
local RAW_OPTIONS = setmetatable({}, { __index = function(cache, count)
    local options = { count = count }
    cache[count] = options
    return options
end })

local function ReadRaw(address, size)
    if not address or not size or size <= 0 then return nil end
    if size <= RAW_PIECE then return (read(address, "raw", RAW_OPTIONS[size])) end
    local parts = {}
    for done = 0, size - 1, RAW_PIECE do
        local piece = read(address + done, "raw", RAW_OPTIONS[math.min(RAW_PIECE, size - done)])
        if not piece then return nil end
        parts[#parts + 1] = piece
    end
    return table.concat(parts)
end

-- Zero-based offsets into a raw block; the parentheses keep one value.
local function Q(raw, offset) return (string.unpack("<i8", raw, offset + 1)) end
local function D(raw, offset) return (string.unpack("<I4", raw, offset + 1)) end

local function BytesToInteger(bytes, first, count)
    local value = 0
    for i = count - 1, 0, -1 do value = (value << 8) | ((bytes[first + i] or 0) & 0xFF) end
    return value
end

----------------------------------------
-- SESSION
----------------------------------------

-- ∑ Everything learned about one process. The engine's processOpened and
-- processClosed events end the session (see MAIN), so attaching to another
-- game, or to the same one again, never reuses stale addresses. The symbols
-- registered for the old process need no list here: the engine drops every
-- registered symbol when its process closes or another one opens, so the
-- engine method names of one game (thousands of addresses) never name code
-- of the next game under Manifold-UE's and Manifold-RTTI's labels.
local S
local L = {}

local function Session()
    if not S then
        local process = manifold.process.info()
        S = {
            pid = process and process.id or 0, ready = false, failed = nil,
            images = {}, sections = {}, code = {},
            nameText = {}, nameCount = 0, typeNames = {}, vtClass = {},
            classes = {}, classList = {}, instLayouts = {},
            elements = {}, elementCount = 0,
        }
        L = {}
    end
    return S
end

local function EndSession()
    S, L = nil, {}
end

local function RegisterOwnSymbol(name, value)
    return registerSymbol(name, value) == true
end

-- ∑ The executable is the process's main module, not merely the first one
-- listed; it keeps its place in the sorted list the address checks search.
local function LoadImages()
    local list, main = {}, nil
    local executable = manifold.process.mainModule()
    for _, m in ipairs(manifold.process.modules() or {}) do
        local entry = { name = m.name, path = m.path, from = m.base, to = m.base + m.size }
        list[#list + 1] = entry
        if executable and m.base == executable.base then main = entry end
    end
    table.sort(list, function(a, b) return a.from < b.from end)
    S.images, S.main = list, main
end

local function ImageOf(address)
    local list = S.images
    local lo, hi = 1, #list
    while lo <= hi do
        local mid = (lo + hi) // 2
        local image = list[mid]
        if address < image.from then hi = mid - 1
        elseif address >= image.to then lo = mid + 1
        else return image end
    end
    return nil
end

local function InImage(address)
    return address ~= nil and ImageOf(address) ~= nil
end

-- A heap address: a pointer that is not inside the executable's image.
local function IsHeap(value)
    return math.type(value) == "integer" and value >= POINTER_MIN and value <= POINTER_MAX and value & 7 == 0
        and (value < S.main.from or value >= S.main.to)
end

local function LoadSections()
    local image = S.main
    local out, code = {}, {}
    local peOffset = RU(image.from + 0x3C)
    if peOffset then
        local pe = image.from + peOffset
        local count, optional = (read(pe + 6, "u16")), (read(pe + 0x14, "u16"))
        if count and optional then
            local first = pe + 0x18 + optional
            for i = 0, count - 1 do
                local section = first + i * 0x28
                local size, rva, flags = RU(section + 8), RU(section + 0xC), RU(section + 0x24)
                if size and rva and flags and size > 0 then
                    local entry = { from = image.from + rva, size = size, name = RS(section, 8) or "",
                        writable = flags & 0x80000000 ~= 0, executable = flags & 0x20000000 ~= 0 }
                    out[#out + 1] = entry
                    if entry.executable then code[#code + 1] = entry end
                end
            end
        end
    end
    S.sections, S.code = out, code
end

local function InCode(address)
    if not address then return false end
    for _, section in ipairs(S.code) do
        if address >= section.from and address < section.from + section.size then return true end
    end
    return false
end

-- The writable data sections of the executable, read once per resolve.
local function DataBlocks()
    local blocks = {}
    for _, section in ipairs(S.sections) do
        if section.writable and not section.executable and section.size >= 0x40 then
            local raw = ReadRaw(section.from, section.size - section.size % 8)
            if raw then blocks[#blocks + 1] = { from = section.from, raw = raw } end
        end
    end
    return blocks
end

----------------------------------------
-- STRINGS
----------------------------------------

--
--- ∑ Godot 4 strings are UTF-32 (CowData<char32_t>). The length sits in the
--- header in front of the data; its place differs between builds (64-bit at
--- -8 for MSVC, at -16 for MinGW's 16-byte alignment, 32-bit at -4 before
--- 4.3), so it is probed once and a scan for the terminator is the fallback.
--- Hashes are djb2 over the code points, which is how the engine hashes a
--- String, and they double as a check that a pointer really is a name.
--
local function DecodeUtf32(raw, count)
    local out, hash = {}, 5381
    for i = 0, count - 1 do
        local c = D(raw, i * 4)
        if c == 0 or c > 0x10FFFF or (c >= 0xD800 and c <= 0xDFFF) then return nil end
        out[i + 1] = c < 0x80 and string.char(c) or utf8.char(c)
        hash = (hash * 33 + c) & 0xFFFFFFFF
    end
    return table.concat(out), hash
end

local function ScanUtf32(ptr, limit)
    if not IsPtr(ptr) and not (math.type(ptr) == "integer" and ptr >= POINTER_MIN and ptr % 4 == 0) then return nil end
    limit = limit or 256
    local codes, at = {}, ptr
    while #codes < limit do
        local raw = ReadRaw(at, 256) or ReadRaw(at, 64)
        if not raw then return nil end
        for i = 0, #raw // 4 - 1 do
            local c = D(raw, i * 4)
            if c == 0 then
                local text = {}
                for k, v in ipairs(codes) do text[k] = v < 0x80 and string.char(v) or utf8.char(v) end
                return table.concat(text), #codes
            end
            if c > 0x10FFFF or (c >= 0xD800 and c <= 0xDFFF) or (c < 0x20 and c ~= 9 and c ~= 10 and c ~= 13) then return nil end
            codes[#codes + 1] = c
            if #codes >= limit then return nil end
        end
        at = at + #raw
    end
    return nil
end

local function StringAt(ptr)
    if ptr == nil then return nil end
    if ptr == 0 then return "" end
    if not IsPtr(ptr) then return nil end
    local size
    if L.strSize == 8 then size = RQ(ptr - 8)
    elseif L.strSize == 16 then size = RQ(ptr - 16)
    elseif L.strSize == 4 then size = RU(ptr - 4) end
    if size then
        if size < 1 or size > STRING_LIMIT + 1 then return nil end
        if size == 1 then return "" end
        local raw = ReadRaw(ptr, (size - 1) * 4)
        return raw and (DecodeUtf32(raw, size - 1))
    end
    return (ScanUtf32(ptr, STRING_LIMIT))
end

local function ByteHash(text)
    local hash = 5381
    for i = 1, #text do hash = (hash * 33 + text:byte(i)) & 0xFFFFFFFF end
    return hash
end

local function CodeHash(text)
    local hash = 5381
    for _, c in utf8.codes(text) do hash = (hash * 33 + c) & 0xFFFFFFFF end
    return hash
end

--
--- ∑ A StringName is a pointer to its _Data, which holds the text (a
--- String, or before 4.4 a C string for static names) and the hash. Texts
--- are cached per _Data together with that hash: dynamic names are freed
--- and their memory reused, and a changed hash shows a stale entry.
--
local function NameText(data, strict)
    if data == nil then return nil end
    if data == 0 then return "" end
    if not IsPtr(data) then return nil end
    local hash = RU(data + L.hash)
    if not hash then return nil end
    local cached = S.nameText[data]
    if cached and cached.hash == hash then return cached.text end
    local text
    if L.cname then
        local c = RP(data + L.cname)
        if c and c ~= 0 then text = RS(c, STRING_LIMIT) end
    end
    if not text then text = StringAt(RP(data + L.str)) end
    if text and strict and CodeHash(text) ~= hash and ByteHash(text) ~= hash then text = nil end
    if S.nameCount > 300000 then S.nameText, S.nameCount = {}, 0 end
    S.nameText[data] = { hash = hash, text = text }
    S.nameCount = S.nameCount + 1
    return text
end

local function NameAt(address)
    local data = RP(address)
    if data == nil then return nil end
    return NameText(data)
end

----------------------------------------
-- VERSION
----------------------------------------

local function Ascii(text)
    local parts = {}
    for i = 1, #text do parts[i] = string.format("%02X", text:byte(i)) end
    return table.concat(parts, " ")
end

-- ∑ Godot's Windows templates carry a "pck" section for an embedded pack,
-- whether a pack is embedded or not. It gates the fallback below for the
-- literal that is in the module twice, as it did in version 1.
local function HasPackSection()
    for _, section in ipairs(S.sections) do
        if section.name == "pck" then return true end
    end
    return false
end

--
--- ∑ One scan of the executable's module, any protection, at most two hits:
--- exactly one hit is the unique literal. A build that keeps two copies took
--- the first copy inside the module from a whole-process scan in version 1,
--- and only behind the pck gate; manifold.memory.scan can scan the module
--- itself, so the fallback rescans just the module for all its hits and
--- takes the lowest, which is the same answer without reading the rest of
--- the process. A game that is not Godot costs this one module scan.
--
local function ScanModule(text)
    local pattern, module = Ascii(text), S.main.name
    local hits = manifold.memory.scan(pattern, { module = module, limit = 2 }) or {}
    if #hits == 1 then return hits[1] end
    if #hits == 0 or not HasPackSection() then return nil end
    local first
    for _, hit in ipairs(manifold.memory.scan(pattern, { module = module }) or {}) do
        if not first or hit < first then first = hit end
    end
    return first
end

-- ∑ The version is a narrow literal in .rdata ("Godot Engine v4.6.3.stable
-- .mono.custom_build"). The major digit is part of the pattern so a Godot 3
-- game gets a clear answer instead of a failed layout probe.
local function FindVersion()
    local address = ScanModule("Godot Engine v4.")
    if not address then
        if ScanModule("Godot Engine v3.") then Fail(FAIL.unsupported, "this is a Godot 3 game; only Godot 4 is supported") end
        return nil
    end
    local text = RS(address, 160) or ""
    local major, minor, patch = text:match("^Godot Engine v(%d+)%.(%d+)%.?(%d*)")
    if not major then return nil end
    return { text = text:gsub("^Godot Engine v", ""), major = tonumber(major), minor = tonumber(minor),
        patch = tonumber(patch) or 0, address = address }
end

----------------------------------------
-- STRINGNAME TABLE
----------------------------------------

local HASH_CANDIDATES = { 0x10, 0x14, 0x18, 0x1C, 0x20 }

--
--- ∑ Every head in the table sits in the slot its own hash selects, so each
--- head votes for where the table must start. The hash offset is unknown
--- too; each candidate offset casts its own vote, and only the real pair
--- (offset, start) collects a majority.
--
local function VoteNameTable(block, runFrom, runTo)
    local raw, from = block.raw, block.from
    local lo, hi = math.max(0, runFrom - 0x1000), math.min(#raw - 8, runTo + 0x1000)
    local stride = math.max(1, ((hi - lo) // 8) // NAME_SAMPLES) * 8
    local votes, samples = {}, 0
    for pos = lo, hi - 8, stride do
        local p = Q(raw, pos)
        if IsHeap(p) then
            local head = ReadRaw(p, 0x30)
            if head then
                samples = samples + 1
                for _, h in ipairs(HASH_CANDIDATES) do
                    local start = pos - (D(head, h) & NAME_TABLE_MASK) * 8
                    if start >= 0 then
                        local key = h * 0x100000000 + start
                        votes[key] = (votes[key] or 0) + 1
                    end
                end
            end
        end
    end
    local best, count = nil, 0
    for key, n in pairs(votes) do if n > count then best, count = key, n end end
    if not best or samples < 32 or count < samples * 0.4 then return nil end
    local h, start = best // 0x100000000, best % 0x100000000
    if start + NAME_TABLE_BYTES > #raw then return nil end
    return { start = from + start, hash = h, votes = count, samples = samples }
end

-- ∑ 65536 slots of _Data pointers are by far the densest block of heap
-- pointers in the executable's data; only such runs are voted on.
local function FindNameTable(blocks)
    local best
    for _, block in ipairs(blocks) do
        local raw = block.raw
        local runFrom, runTo
        local function close()
            if runFrom and runTo - runFrom >= NAME_TABLE_BYTES // 2 then
                local candidate = VoteNameTable(block, runFrom, runTo)
                if candidate and (not best or candidate.votes > best.votes) then best = candidate end
            end
            runFrom, runTo = nil, nil
        end
        for at = 0, #raw - 4096, 4096 do
            local pointers, zeros = 0, 0
            for i = at, at + 4095, 8 do
                local v = Q(raw, i)
                if v == 0 then zeros = zeros + 1 elseif IsHeap(v) then pointers = pointers + 1 end
            end
            if pointers >= 64 and pointers + zeros >= 500 then
                runFrom = runFrom or at
                runTo = at + 4096
            else
                close()
            end
        end
        close()
    end
    return best
end

local function ProbeNameLayout()
    local raw = ReadRaw(S.nameTable, NAME_TABLE_BYTES)
    if not raw then Fail(FAIL.stage, "the StringName table at " .. Hex(S.nameTable) .. " cannot be read") end
    local heads = {}
    for slot = 0, NAME_TABLE_SLOTS - 1, 31 do
        local p = Q(raw, slot * 8)
        if p ~= 0 and IsHeap(p) then
            local block = ReadRaw(p, 0x30)
            if block then heads[#heads + 1] = { slot = slot, p = p, raw = block } end
            if #heads >= 300 then break end
        end
    end
    if #heads < 16 then Fail(FAIL.stage, "the StringName table has too few names (" .. #heads .. " sampled)") end
    local cname, str, sizes, decoded = 0, { [8] = 0, [0x10] = 0 }, { [4] = 0, [8] = 0, [16] = 0 }, 0
    for _, h in ipairs(heads) do
        local q8 = Q(h.raw, 8)
        if q8 ~= 0 and InImage(q8) then
            local text = RS(q8, 64)
            if text and text:find("^[%w_@]") and ByteHash(text) == D(h.raw, L.hash) then cname = cname + 1 end
        end
        for _, off in ipairs({ 8, 0x10 }) do
            local ptr = Q(h.raw, off)
            local text, count = ScanUtf32(ptr)
            if text and count > 0 and CodeHash(text) == D(h.raw, L.hash) then
                str[off] = str[off] + 1
                decoded = decoded + 1
                if RQ(ptr - 8) == count + 1 then sizes[8] = sizes[8] + 1 end
                if RQ(ptr - 16) == count + 1 then sizes[16] = sizes[16] + 1 end
                if RU(ptr - 4) == count + 1 then sizes[4] = sizes[4] + 1 end
            end
        end
    end
    L.cname = cname >= #heads * 0.1 and 8 or nil
    L.str = L.cname and 0x10 or (str[8] >= str[0x10] and 8 or 0x10)
    if cname + str[L.str] < #heads * 0.5 then
        Fail(FAIL.stage, string.format("StringName layout not recognised (%d of %d names decoded)", cname + str[L.str], #heads))
    end
    local bestSize, hits = nil, 0
    for width, n in pairs(sizes) do if n > hits then bestSize, hits = width, n end end
    L.strSize = decoded > 0 and hits >= decoded * 0.8 and bestSize or nil
    local links = {}
    for _, h in ipairs(heads) do
        for _, off in ipairs({ 0x18, 0x20, 0x28 }) do
            local q = Q(h.raw, off)
            if IsHeap(q) and ((RU(q + L.hash) or -1) & NAME_TABLE_MASK) == h.slot then links[off] = (links[off] or 0) + 1 end
        end
    end
    local bestLink, linkHits = nil, 0
    for off, n in pairs(links) do if n > linkHits then bestLink, linkHits = off, n end end
    L.next = bestLink or (L.cname and 0x28 or 0x20)
end

-- The _Data of a name, looked up like the engine does: hash, slot, chain.
local function FindName(text)
    if type(text) ~= "string" then return nil end
    local hash = CodeHash(text)
    local data = RP(S.nameTable + (hash & NAME_TABLE_MASK) * 8)
    local steps = 0
    while IsPtr(data) and steps < 4096 do
        if RU(data + L.hash) == hash and NameText(data) == text then return data end
        data = RP(data + L.next)
        steps = steps + 1
    end
    return nil
end

----------------------------------------
-- OBJECTDB
----------------------------------------

local function SlotCheck(slots)
    local raw = ReadRaw(slots, 16 * 16)
    if not raw then return 0 end
    local n = 0
    for i = 0, 15 do
        local obj = Q(raw, i * 16 + 8)
        if IsHeap(obj) then
            local vt = RP(obj)
            if vt and InImage(vt) then n = n + 1 end
        end
    end
    return n
end

-- ∑ Each live object stores its own ObjectID, which is the slot index
-- plus the slot's validator. Finding that value inside the objects both
-- proves the slot array and gives the offset of Object::_instance_id.
local function ProbeIdOffset(slots, max)
    local n = math.min(max, 1024)
    local raw = ReadRaw(slots, n * 16)
    if not raw then return nil end
    local hist, live = {}, 0
    for slot = 0, n - 1 do
        local word, obj = Q(raw, slot * 16), Q(raw, slot * 16 + 8)
        if obj ~= 0 and IsHeap(obj) and word & VALIDATOR_MASK ~= 0 then
            local id = ((word & VALIDATOR_MASK) << SLOT_BITS) | slot | (word & REF_BIT)
            local block = ReadRaw(obj, ID_SCAN)
            if block then
                live = live + 1
                for off = 8, ID_SCAN - 8, 8 do
                    if Q(block, off) == id then hist[off] = (hist[off] or 0) + 1 end
                end
            end
            if live >= 64 then break end
        end
    end
    local best, hits = nil, 0
    for off, count in pairs(hist) do if count > hits then best, hits = off, count end end
    if not best or live < 8 or hits < live * 0.9 then return nil end
    return best
end

local function PowerOfTwo(v) return v >= 16 and v <= MAX_SLOTS and v & (v - 1) == 0 end

local NEIGHBOURS = { -8, -4, 8, 12, -16, -12, 16, 20, -24, -20 }

local function HasSlotMax(raw, pos)
    for _, d in ipairs(NEIGHBOURS) do
        local at = pos + d
        if at >= 0 and at + 4 <= #raw and PowerOfTwo(D(raw, at)) then return true end
    end
    return false
end

-- ∑ slot_max is a power of two (the array doubles) next to the static
-- pointer; slot_count sits beside slot_max. The source fixes neither
-- position, so the neighbourhood is searched.
local function VerifyObjectDB(block, pos, slots)
    local raw = block.raw
    local max, maxAt
    for _, d in ipairs(NEIGHBOURS) do
        local at = pos + d
        if at >= 0 and at + 4 <= #raw then
            local v = D(raw, at)
            if PowerOfTwo(v) and (not max or v > max) and ReadRaw(slots + (v - 1) * 16, 16) then max, maxAt = v, at end
        end
    end
    if not max then return nil end
    local id = ProbeIdOffset(slots, max)
    if not id then return nil end
    local countAt
    for _, d in ipairs({ -4, 4 }) do
        local at = maxAt + d
        if at >= 0 and at + 4 <= #raw and (at < pos or at >= pos + 8) then
            local v = D(raw, at)
            if v > 0 and v <= max then countAt = at break end
        end
    end
    return { slotsAt = block.from + pos, maxAt = block.from + maxAt, countAt = countAt and block.from + countAt, id = id }
end

local function FindObjectDB(blocks)
    local tableFrom, tableTo = S.nameTable, S.nameTable + NAME_TABLE_BYTES
    local seen = {}
    for _, block in ipairs(blocks) do
        local raw, from = block.raw, block.from
        for pos = 0, #raw - 8, 8 do
            local at = from + pos
            if at < tableFrom or at >= tableTo then
                local v = Q(raw, pos)
                if v ~= 0 and v & 15 == 0 and not seen[v] and IsHeap(v) and HasSlotMax(raw, pos) then
                    seen[v] = true
                    if SlotCheck(v) >= 10 then
                        local found = VerifyObjectDB(block, pos, v)
                        if found then return found end
                    end
                end
            end
        end
    end
    return nil
end

local function SlotMax()
    return math.min(RU(S.db.maxAt) or 0, MAX_SLOTS)
end

local function EachObject(fn)
    local slots = RP(S.db.slotsAt)
    local max = SlotMax()
    if not IsPtr(slots) or max == 0 then return end
    local chunk = 4096
    for first = 0, max - 1, chunk do
        local n = math.min(chunk, max - first)
        local raw = ReadRaw(slots + first * 16, n * 16)
        if raw then
            for k = 0, n - 1 do
                local word, obj = Q(raw, k * 16), Q(raw, k * 16 + 8)
                if obj ~= 0 and IsPtr(obj) and word & VALIDATOR_MASK ~= 0 then
                    if fn(obj, first + k, word) == false then return end
                end
            end
        end
    end
end

local function IsObject(address)
    if not IsPtr(address) then return false end
    local vt = RP(address)
    if not vt or not InImage(vt) then return false end
    local id = RQ(address + L.id)
    if not id or id == 0 then return false end
    local slot = id & SLOT_MASK
    if slot >= SlotMax() then return false end
    local slots = RP(S.db.slotsAt)
    return slots ~= nil and RP(slots + slot * 16 + 8) == address
end

local function FromId(id)
    if math.type(id) ~= "integer" or id == 0 then return nil end
    local slot = id & SLOT_MASK
    if slot >= SlotMax() then return nil end
    local slots = RP(S.db.slotsAt)
    if not slots then return nil end
    local word = RQ(slots + slot * 16)
    if not word or word & VALIDATOR_MASK ~= (id >> SLOT_BITS) & VALIDATOR_MASK then return nil end
    return RPtr(slots + slot * 16 + 8)
end

local function Count()
    local n = 0
    EachObject(function() n = n + 1 end)
    return n
end

----------------------------------------
-- CLASSDB
----------------------------------------

--
--- ∑ Every element starts with next, prev, key and the first qword of its
--- value, 0x20 bytes. One raw read of them gives the link, the key and the
--- value word (the pointer in the method and children maps) instead of a
--- read each; this loop runs over every method of every class when the
--- method names are registered. fn gets the element, the key, the address
--- of the value and that value word.
--
local function MapEach(map, fn, limit)
    local e = RP(map + MAP_HEAD)
    local n, cap = 0, limit or MAP_LIMIT
    while IsPtr(e) and n < cap do
        local raw = ReadRaw(e, 0x20)
        if not raw then return end
        if fn(e, Q(raw, ELEMENT_KEY), e + ELEMENT_VALUE, Q(raw, ELEMENT_VALUE)) == false then return end
        e = Q(raw, ELEMENT_NEXT)
        n = n + 1
    end
end

local function Ptr(value) return IsPtr(value) and value or nil end

-- ∑ ClassDB::classes is the HashMap<StringName, ClassInfo> whose first
-- inserted class is "Object" (register_core_types starts with it) and that
-- holds the most entries of all such maps.
local function FindClassDB(blocks)
    local best
    for _, block in ipairs(blocks) do
        local raw, from = block.raw, block.from
        for pos = 0, #raw - 0x28, 8 do
            local count = D(raw, pos + MAP_SIZE)
            if count >= 100 and count <= 100000 and D(raw, pos + MAP_CAPACITY) < 40 then
                local head, tail = Q(raw, pos + MAP_HEAD), Q(raw, pos + MAP_TAIL)
                if IsHeap(head) and IsHeap(tail) and IsHeap(Q(raw, pos)) and IsHeap(Q(raw, pos + 8)) then
                    if NameText(RP(head + ELEMENT_KEY), true) == "Object" and (not best or count > best.count) then
                        best = { at = from + pos, count = count }
                    end
                end
            end
        end
    end
    return best
end

local function IndexClasses()
    S.classes, S.classList = {}, {}
    MapEach(S.classdb, function(_, key, value)
        local name = NameText(key)
        if name and name ~= "" then
            local info = { name = name, key = key, value = value }
            S.classes[name] = info
            S.classList[#S.classList + 1] = info
        end
    end)
    table.sort(S.classList, function(a, b) return a.name < b.name end)
end

local function InfoRaw(info) return ReadRaw(info.value, CLASSINFO_SCAN) end

local function DisassembledOpcode(text)
    local opcode = text and text:match("^.- %- .- %- (.*)$")
    if not opcode then return nil end
    return (opcode:gsub(" : .*$", ""))
end

--
--- ∑ ClassDB keeps no sizeof, but every creatable class has a creation
--- function that is memnew(T): the first argument loaded before the first
--- call is the allocation size. With the class size known, an address
--- inside an object finds its owner exactly and dissects end where the
--- object ends. The manifold API has no disassembler, so Manifold Engine's
--- Cheat Engine disassemble and getInstructionSize read the instructions;
--- they raise instead of returning nil (without a process, for one), and
--- the pcall turns that into the end of the walk it was in version 1. The
--- byte scan version 1 kept for hosts without a disassembler is gone:
--- Manifold Engine always has one.
--
local function CreatedSize(fn)
    if not fn or fn == 0 or not InCode(fn) then return nil end
    local at = fn
    for _ = 1, 16 do
        local ok, text = pcall(disassemble, at)
        local opcode = ok and DisassembledOpcode(text)
        if not opcode then break end
        local imm = opcode:match("^mov%s+ecx,%s*0?x?(%x+)h?$")
        if imm then
            local size = tonumber(imm, 16)
            if size and size >= 0x10 and size <= 0x1000000 then return size end
        end
        if opcode:match("^call") or opcode:match("^jmp") or opcode:match("^ret") then break end
        local fine, step = pcall(getInstructionSize, at)
        if not fine or not step or step <= 0 then break end
        at = at + step
    end
    return nil
end

local function ProbeClassInfo()
    local sample = {}
    local step = math.max(1, #S.classList // 40)
    for i = 1, #S.classList, step do sample[#sample + 1] = S.classList[i] end
    local hits = {}
    for _, info in ipairs(sample) do
        info.raw = InfoRaw(info)
        if info.raw then
            for off = 0, CLASSINFO_SCAN - 8, 8 do
                if Q(info.raw, off) == info.key then hits[off] = (hits[off] or 0) + 1 end
            end
        end
    end
    local best, count = nil, 0
    for off, n in pairs(hits) do if n > count or (n == count and off < best) then best, count = off, n end end
    if not best or count < #sample * 0.9 then Fail(FAIL.stage, "ClassInfo::name not found") end
    L.ciName = best

    local object, node, timer = S.classes.Object, S.classes.Node, S.classes.Timer
    if not object or not node then Fail(FAIL.stage, "ClassDB has no Object or Node class") end
    local rawObject, rawNode, rawTimer = InfoRaw(object), InfoRaw(node), timer and InfoRaw(timer)
    if not rawNode then Fail(FAIL.stage, "the Node ClassInfo cannot be read") end
    for off = 0, CLASSINFO_SCAN - 8, 8 do
        if off ~= L.ciName and NameText(Q(rawNode, off)) == "Object" and (not rawTimer or NameText(Q(rawTimer, off)) == "Node") then
            L.ciInherits = off
            break
        end
    end
    if not L.ciInherits then Fail(FAIL.stage, "ClassInfo::inherits not found") end

    -- The creation function: a code pointer in both classes whose decoded
    -- sizes grow from Node to Timer.
    for off = 0, CLASSINFO_SCAN - 8, 8 do
        local a = Q(rawNode, off)
        if InCode(a) then
            local nodeSize = CreatedSize(a)
            local timerSize = rawTimer and CreatedSize(Q(rawTimer, off))
            if nodeSize and (not rawTimer or (timerSize and timerSize > nodeSize)) then
                L.ciCreate = off
                break
            end
        end
    end

    -- method_map: a HashMap whose entries are method binds that carry their
    -- own name; that also gives MethodBind::name.
    for off = 0, L.ciName - 0x28, 8 do
        local head = Q(rawNode, off + MAP_HEAD)
        local size = D(rawNode, off + MAP_SIZE)
        if IsHeap(head) and size > 0 and size < 100000 and D(rawNode, off + MAP_CAPACITY) < 40 then
            local key, bind = RP(head + ELEMENT_KEY), RP(head + ELEMENT_VALUE)
            if IsHeap(bind) and InImage(RP(bind) or 0) and NameText(key, true) then
                for q = 8, 0x40, 8 do
                    if RP(bind + q) == key then L.ciMethods, L.mbName = off, q break end
                end
            end
        end
        if L.ciMethods then break end
    end
    if L.ciMethods then
        local code, binds = {}, 0
        MapEach(node.value + L.ciMethods, function(_, _, _, bind)
            local raw = IsHeap(bind) and ReadRaw(bind, 0x80)
            if raw then
                binds = binds + 1
                for off = L.mbName + 8, 0x78, 8 do
                    if InCode(Q(raw, off)) then code[off] = (code[off] or 0) + 1 end
                end
            end
            if binds >= 64 then return false end
        end)
        local bestCode, codeHits = nil, 0
        for off, n in pairs(code) do if n > codeHits or (n == codeHits and off < bestCode) then bestCode, codeHits = off, n end end
        if bestCode and codeHits >= binds * 0.6 then L.mbCode = bestCode end
    end
    for _, info in ipairs(sample) do info.raw = nil end
end

local function SuperName(info)
    if info.inherits == nil then
        local text = L.ciInherits and NameText(RP(info.value + L.ciInherits)) or nil
        info.inherits = (text and text ~= "") and text or false
    end
    return info.inherits or nil
end

local function Inherits(name, base)
    if not name or not base then return false end
    if name == base then return true end
    local info = S.classes[name]
    for _ = 1, 64 do
        if not info then return false end
        local super = SuperName(info)
        if not super then return false end
        if super == base then return true end
        info = S.classes[super]
    end
    return false
end

local function Ancestry(name)
    local list, info = { name }, S.classes[name]
    for _ = 1, 64 do
        local super = info and SuperName(info)
        if not super then break end
        list[#list + 1] = super
        info = S.classes[super]
    end
    return list
end

local function ClassSize(name)
    local info = S.classes[name]
    for _ = 1, 64 do
        if not info then return nil end
        if info.size == nil then
            local fn = L.ciCreate and RP(info.value + L.ciCreate)
            info.size = (fn and fn ~= 0 and CreatedSize(fn)) or false
        end
        if info.size then return info.size end
        local super = SuperName(info)
        info = super and S.classes[super]
    end
    return nil
end

local function IsVcallThunk(code)
    local raw = ReadRaw(code, 5)
    if not raw then return false end
    local a, b, c, d, e = raw:byte(1, 5)
    return a == 0x48 and b == 0x8B and c == 0x01 and d == 0xFF and (e == 0x60 or e == 0xA0 or e == 0x20)
end

local function OwnMethods(info)
    if info.methods then return info.methods end
    local list = {}
    if L.ciMethods then
        MapEach(info.value + L.ciMethods, function(_, key, _, bind)
            local name = NameText(key)
            if name and IsPtr(bind) then
                local code = L.mbCode and RP(bind + L.mbCode) or nil
                local virtual = false
                if code and code ~= 0 then
                    virtual = code & 1 == 1 or IsVcallThunk(code)
                else
                    code = nil
                end
                list[#list + 1] = { name = name, class = info.name, bind = bind, address = code, virtual = virtual }
            end
        end)
    end
    table.sort(list, function(a, b) return a.name < b.name end)
    info.methods = list
    return list
end

local function Methods(name, inherited)
    local out, seen = {}, {}
    local info = S.classes[name]
    for _ = 1, 64 do
        if not info then break end
        for _, m in ipairs(OwnMethods(info)) do
            if not seen[m.name] then
                seen[m.name] = true
                out[#out + 1] = m
            end
        end
        if not inherited then break end
        local super = SuperName(info)
        info = super and S.classes[super]
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

----------------------------------------
-- CLASS NAMES
----------------------------------------

local function Demangle(name)
    local body = name:match("^%.%?A[VU](.+)@@$")
    if not body then return name end
    if body:sub(1, 2) == "?$" then
        local template = body:match("^%?%$([^@]+)@")
        return template or body
    end
    local parts = {}
    for part in body:gmatch("[^@]+") do table.insert(parts, 1, part) end
    return table.concat(parts, "::")
end

local function ItaniumName(mangled)
    local s = mangled:match("^N(.+)E$") or mangled
    local parts, pos = {}, 1
    while pos <= #s do
        local len = s:match("^(%d+)", pos)
        if not len then break end
        pos = pos + #len
        parts[#parts + 1] = s:sub(pos, pos + tonumber(len) - 1)
        pos = pos + tonumber(len)
    end
    return #parts > 0 and table.concat(parts, "::") or nil
end

-- ∑ The C++ class from run-time type information: MSVC's complete object
-- locator or the Itanium type_info in front of the vtable. Only a fallback
-- for objects whose Godot type pointer is not set yet (construction).
local function VTableClass(vt)
    if not vt then return nil end
    local cached = S.vtClass[vt]
    if cached ~= nil then return cached or nil end
    local name
    local col = RP(vt - 8)
    local image = col and ImageOf(col)
    if image then
        if RU(col) == 1 then
            local td = RU(col + 12)
            local raw = td and RS(image.from + td + 0x10, 256)
            if raw and (raw:sub(1, 4) == ".?AV" or raw:sub(1, 4) == ".?AU") then name = Demangle(raw) end
        end
        if not name then
            local text = RP(col + 8)
            local raw = text and InImage(text) and RS(text, 256)
            if raw and raw:match("^N?%d") then name = ItaniumName(raw) end
        end
    end
    S.vtClass[vt] = name or false
    return name
end

--
--- ∑ Godot 4.6 objects point to their GDType (super, name, hierarchy);
--- 4.0 to 4.5 cache a pointer to the class's static StringName instead.
--- Both are probed: the offset whose pointer leads to a registered class
--- name for nearly every object wins. That names script-free extension
--- classes too, which run-time type information would call by their C++
--- wrapper.
--
local function ProbeTypePointer()
    local hits, objects = {}, 0
    EachObject(function(obj)
        local raw = ReadRaw(obj, 0x200)
        if not raw then return end
        objects = objects + 1
        for off = 8, 0x1F8, 8 do
            if off ~= L.id then
                local p = Q(raw, off)
                if IsPtr(p) then
                    local gd = NameText(RP(p + 8))
                    if gd and S.classes[gd] then hits["g" .. off] = (hits["g" .. off] or 0) + 1 end
                    local cn = NameText(RP(p))
                    if cn and S.classes[cn] then hits["c" .. off] = (hits["c" .. off] or 0) + 1 end
                end
            end
        end
        if objects >= 80 then return false end
    end)
    local best, count = nil, 0
    for key, n in pairs(hits) do if n > count then best, count = key, n end end
    if best and count >= objects * 0.7 then
        L.typeKind = best:sub(1, 1) == "g" and "gdtype" or "classname"
        L.type = tonumber(best:sub(2))
    end
end

local function ClassName(object)
    if L.type then
        local t = RP(object + L.type)
        if t and t ~= 0 then
            local cached = S.typeNames[t]
            if cached then return cached end
            local text = L.typeKind == "gdtype" and NameText(RP(t + 8)) or NameText(RP(t))
            if text and text ~= "" then
                S.typeNames[t] = text
                return text
            end
        end
    end
    local cpp = VTableClass(RP(object))
    if cpp then
        if S.classes[cpp] then return cpp end
        local short = cpp:match("([%w_]+)$")
        if short and S.classes[short] then return short end
        return cpp
    end
    return "Object"
end

local function IsNode(object)
    return L.name ~= nil and Inherits(ClassName(object), "Node")
end

----------------------------------------
-- SCRIPTS
----------------------------------------

-- ∑ A script instance points back to its owner; where exactly depends on
-- the instance class (C#, GDScript, placeholder), so the layout is learned
-- per vtable: the owner field and the Ref<Script> after it.
local function InstanceLayout(instance, owner)
    local vt = RP(instance)
    if not vt or not InImage(vt) then return nil end
    local known = S.instLayouts[vt]
    if known ~= nil then return known or nil end
    if not owner then return nil end
    local layout
    for _, k in ipairs({ 8, 0x10, 0x18 }) do
        if RP(instance + k) == owner then
            layout = { owner = k, class = VTableClass(vt) or "ScriptInstance" }
            for s = k + 8, k + 0x40, 8 do
                local p = RPtr(instance + s)
                if p and IsObject(p) and Inherits(ClassName(p), "Script") then layout.script = s break end
            end
            break
        end
    end
    S.instLayouts[vt] = layout or false
    return layout
end

local function ProbeScripts()
    local hits, checked = {}, 0
    EachObject(function(obj)
        local raw = ReadRaw(obj, 0x100)
        if not raw then return end
        checked = checked + 1
        for off = L.id + 8, 0xF8, 8 do
            if off ~= L.type then
                local p = Q(raw, off)
                if IsHeap(p) and InImage(RP(p) or 0) then
                    for _, k in ipairs({ 8, 0x10, 0x18 }) do
                        if RP(p + k) == obj then hits[off] = (hits[off] or 0) + 1 break end
                    end
                end
            end
        end
        local total = 0
        for _, n in pairs(hits) do total = total + n end
        if total >= 40 or checked >= 6000 then return false end
    end)
    local best, count = nil, 0
    for off, n in pairs(hits) do if n > count then best, count = off, n end end
    if best and count >= 2 then L.script = best end

    -- Resource::path_cache: the String that starts with res:// in scripts
    -- and other resources.
    local paths, resources = {}, 0
    EachObject(function(obj)
        if Inherits(ClassName(obj), "Resource") then
            local raw = ReadRaw(obj, OBJECT_SCAN)
            if raw then
                local found = false
                for off = 0x58, OBJECT_SCAN - 8, 8 do
                    local p = Q(raw, off)
                    if IsHeap(p) then
                        local text = StringAt(p)
                        if text and text:sub(1, 6) == "res://" then paths[off] = (paths[off] or 0) + 1 found = true end
                    end
                end
                if found then resources = resources + 1 end
            end
        end
        if resources >= 24 then return false end
    end)
    best, count = nil, 0
    for off, n in pairs(paths) do if n > count then best, count = off, n end end
    if best and count >= math.max(2, resources * 0.5) then L.resPath = best end
end

local function ScriptInstanceOf(object)
    if not L.script then return nil end
    local instance = RPtr(object + L.script)
    if not instance then return nil end
    return instance, InstanceLayout(instance, object)
end

local function ScriptOf(object)
    local instance, layout = ScriptInstanceOf(object)
    if not instance or not layout or not layout.script then return nil end
    return RPtr(instance + layout.script), instance, layout
end

local function ResourcePath(resource)
    if not L.resPath or not resource then return nil end
    local text = StringAt(RP(resource + L.resPath))
    return text ~= "" and text or nil
end

local function ScriptPath(object)
    return ResourcePath((ScriptOf(object)))
end

-- C#: CSharpInstance keeps a GCHandle right after its script; the handle
-- is a handle-table slot that holds the managed object.
local function ManagedObject(object)
    local _, instance, layout = ScriptOf(object)
    if not instance or not layout or not layout.class:find("CSharpInstance", 1, true) then return nil end
    local handle = RPtr(instance + layout.script + 8)
    return handle and RPtr(handle) or nil, handle
end

local function ScriptInstanceAt(address)
    if not L.script or not IsPtr(address) then return nil end
    local vt = RP(address)
    if not vt or not InImage(vt) then return nil end
    for _, k in ipairs({ 8, 0x10, 0x18 }) do
        local owner = RPtr(address + k)
        if owner and RP(owner + L.script) == address and IsObject(owner) then
            return InstanceLayout(address, owner), owner
        end
    end
    return nil
end

----------------------------------------
-- SCENE TREE
----------------------------------------

local function MapHeader(base, offset)
    local head = RP(base + offset + MAP_HEAD)
    local size, capacity = RU(base + offset + MAP_SIZE), RU(base + offset + MAP_CAPACITY)
    return head, size, capacity
end

--
--- ∑ Node::Data is found from the root window: it points to the scene tree,
--- its name is "root", its children map holds nodes that point to the same
--- tree, and those children point back to the root as their parent. owner
--- follows parent and scene_file_path starts the block three pointers
--- before it in every 4.x release.
--
local function ProbeTree(blocks)
    S.treeTried = Ticks()
    local tree
    EachObject(function(obj)
        if Inherits(ClassName(obj), "SceneTree") then tree = obj return false end
    end)
    if not tree then return false, "no SceneTree object yet" end
    local raw = ReadRaw(tree, math.min(ClassSize("SceneTree") or 0x800, 0x2000)) or ReadRaw(tree, 0x400)
    if not raw then return false, "the SceneTree cannot be read" end
    local root
    for off = 0x40, #raw - 8, 8 do
        local p = Q(raw, off)
        if IsHeap(p) and IsObject(p) and Inherits(ClassName(p), "Window") then
            local window = ReadRaw(p, 0x800)
            if window then
                for t = 0x80, 0x7F8, 8 do
                    if Q(window, t) == tree then
                        for n = t - 8, math.max(0x80, t - 0x40), -8 do
                            if NameText(Q(window, n)) == "root" then
                                L.root, L.tree, L.name, root = off, t, n, p
                                break
                            end
                        end
                    end
                    if root then break end
                end
            end
        end
        if root then break end
    end
    if not root then return false, "the root window was not found" end
    for c = L.name - 0x28, 0x80, -8 do
        local head, size, capacity = MapHeader(root, c)
        if IsHeap(head) and size and size > 0 and size < CHILD_LIMIT and capacity and capacity < 40 then
            local child = RPtr(head + ELEMENT_VALUE)
            if child and IsObject(child) and RP(child + L.tree) == tree and NameText(RP(head + ELEMENT_KEY)) == NameText(RP(child + L.name)) then
                L.children = c
                break
            end
        end
    end
    if not L.children then return false, "Node::children not found" end
    local votes = {}
    MapEach(root + L.children, function(_, _, _, word)
        local child = Ptr(word)
        if child then
            for p = L.children - 8, math.max(0x80, L.children - 0x40), -8 do
                if RP(child + p) == root and (RP(root + p) or 0) == 0 then votes[p] = (votes[p] or 0) + 1 end
            end
        end
    end, 64)
    local best, count = nil, 0
    for p, n in pairs(votes) do if n > count or (n == count and p < best) then best, count = p, n end end
    if not best then return false, "Node::parent not found" end
    L.parent = best
    if L.parent + 8 < L.children then L.owner = L.parent + 8 end
    if L.parent >= 0x18 then L.sceneFile = L.parent - 0x18 end
    S.tree, S.root = tree, root
    S.treeStatic = nil
    for _, block in ipairs(blocks or {}) do
        local packed = string.pack("<i8", tree)
        local at = 1
        while true do
            local found = block.raw:find(packed, at, true)
            if not found then break end
            if (found - 1) % 8 == 0 then S.treeStatic = block.from + found - 1 break end
            at = found + 1
        end
        if S.treeStatic then break end
    end
    return true
end

local function Tree()
    if S.tree and IsObject(S.tree) then return S.tree end
    if not S.treeTried or Ticks() - S.treeTried > TREE_RETRY or S.tree then
        S.tree = nil
        ProbeTree(nil)
    end
    return S.tree
end

local function Root()
    local tree = Tree()
    if not tree then return nil end
    local root = L.root and RPtr(tree + L.root)
    if root then S.root = root end
    return root
end

local function NodeName(node) return L.name and NameAt(node + L.name) or nil end
local function ParentOf(node) return L.parent and RPtr(node + L.parent) or nil end

local function Children(node)
    local list = {}
    if not L.children then return list end
    MapEach(node + L.children, function(_, _, _, word)
        local child = Ptr(word)
        if child then list[#list + 1] = child end
    end, CHILD_LIMIT)
    return list
end

local function ChildNamed(node, name)
    local found
    if not L.children then return nil end
    MapEach(node + L.children, function(_, key, _, word)
        if NameText(key) == name then found = Ptr(word) return false end
    end, CHILD_LIMIT)
    return found
end

local function PathOf(node)
    local parts, at = {}, node
    for _ = 1, 512 do
        if not at then break end
        table.insert(parts, 1, NodeName(at) or "?")
        at = ParentOf(at)
    end
    return "/" .. table.concat(parts, "/")
end

local function NodeAt(path)
    if type(path) ~= "string" or path == "" then return nil end
    local root = Root()
    if not root then return nil end
    local segments = {}
    for part in path:gmatch("[^/]+") do segments[#segments + 1] = part end
    if #segments == 0 then return root end
    local at, first = root, 1
    if segments[1] == (NodeName(root) or "root") then first = 2 end
    for i = first, #segments do
        at = ChildNamed(at, segments[i])
        if not at then return nil end
    end
    return at
end

local function EachNode(fn, from, limit)
    local start = from or Root()
    if not start then return end
    local queue, head, seen = { { start, 0 } }, 1, 0
    limit = limit or CHILD_LIMIT
    while head <= #queue and seen < limit do
        local node, depth = queue[head][1], queue[head][2]
        head = head + 1
        seen = seen + 1
        if fn(node, depth) == false then return end
        for _, child in ipairs(Children(node)) do queue[#queue + 1] = { child, depth + 1 } end
    end
end

local function Describe(object)
    local text = ClassName(object)
    local path = ScriptPath(object)
    if path then text = text .. ", " .. (path:match("[^/]+$") or path) end
    return text
end

local function TreeText(from, maxDepth, limit)
    local lines = {}
    local function walk(node, depth)
        if #lines >= limit then return end
        lines[#lines + 1] = string.rep("  ", depth) .. (NodeName(node) or "?") .. "  (" .. Describe(node) .. ")  " .. Hex(node)
        if depth >= maxDepth then
            local n = #Children(node)
            if n > 0 then lines[#lines + 1] = string.rep("  ", depth + 1) .. "... " .. n .. " children" end
            return
        end
        for _, child in ipairs(Children(node)) do walk(child, depth + 1) end
    end
    walk(from, 0)
    if #lines >= limit then lines[#lines + 1] = "... (stopped at " .. limit .. " lines)" end
    return table.concat(lines, "\n")
end

----------------------------------------
-- OBJECTS
----------------------------------------

local function ObjectSize(object)
    return ClassSize(ClassName(object)) or DEFAULT_SIZE
end

local function SortedObjects()
    local now = Ticks()
    if S.sorted and now - S.sortedAt < SORTED_TTL then return S.sorted end
    local list = {}
    EachObject(function(obj) list[#list + 1] = obj end)
    table.sort(list)
    S.sorted, S.sortedAt = list, now
    return list
end

-- ∑ Objects are found by their start; the sorted object list answers
-- "which object starts closest below this address" in a binary search, and
-- the class size decides whether the address is still inside it.
local function OwnerOf(address)
    if not IsPtr(address) and math.type(address) ~= "integer" then return nil end
    if IsObject(address) then return address end
    local list = SortedObjects()
    local lo, hi, found = 1, #list, nil
    while lo <= hi do
        local mid = (lo + hi) // 2
        if list[mid] <= address then found, lo = list[mid], mid + 1 else hi = mid - 1 end
    end
    if found and address - found < ObjectSize(found) and IsObject(found) then return found end
    return nil
end

local function Instances(class, limit, exact)
    local out = {}
    limit = limit or 1000
    EachObject(function(obj)
        local name = ClassName(obj)
        if name == class or (not exact and Inherits(name, class)) then out[#out + 1] = obj end
        if #out >= limit then return false end
    end)
    return out
end

local function Whois(address)
    local object = OwnerOf(address)
    if not object then
        local layout, owner = ScriptInstanceAt(address)
        if layout then return string.format("%X is the %s of %s %X", address, layout.class, ClassName(owner), owner), owner end
        return nil
    end
    local lines = { string.format("%X is %s+%X", address, ClassName(object), address - object) }
    lines[#lines + 1] = string.format("object %X, size %X, id %X", object, ObjectSize(object), RQ(object + L.id) or 0)
    if IsNode(object) then lines[#lines + 1] = "node " .. PathOf(object) end
    local path = ScriptPath(object)
    if path then lines[#lines + 1] = "script " .. path end
    return table.concat(lines, "\n"), object
end

----------------------------------------
-- RESOLVE
----------------------------------------

--
--- ∑ Every symbol of this script starts with "Godot" (method names aside,
--- which are Class::method). Manifold-UE answers any <Type>_<Property>, so
--- a plain Node_parent could have meant a property of an Unreal type called
--- Node; with the prefix the two namespaces cannot meet.
--
local OFFSET_SYMBOLS = {
    GodotObject_instance_id = "id", GodotObject_script_instance = "script", GodotObject_gdtype = "type",
    GodotObject_metadata = "meta", GodotNode_scene_file_path = "sceneFile", GodotNode_parent = "parent",
    GodotNode_owner = "owner", GodotNode_children = "children", GodotNode_name = "name", GodotNode_tree = "tree",
    GodotSceneTree_root = "root", GodotResource_path = "resPath",
}

local TREE_OFFSETS = { parent = true, owner = true, children = true, name = true, tree = true, root = true, sceneFile = true }

local function CoreSymbol(name)
    if name == "GodotStringNames" then return S.nameTable end
    if name == "GodotObjectDB" then return S.db and S.db.slotsAt end
    if name == "GodotClassDB" then return S.classdb end
    if name == "GodotSceneTree" then return S.treeStatic end
    return nil
end

local function RegisterCoreSymbols()
    for _, name in ipairs({ "GodotStringNames", "GodotObjectDB", "GodotClassDB", "GodotSceneTree" }) do
        local value = CoreSymbol(name)
        if value then RegisterOwnSymbol(name, value) end
    end
    for symbol, key in pairs(OFFSET_SYMBOLS) do
        if L[key] then RegisterOwnSymbol(symbol, L[key]) end
    end
end

--
--- ∑ metadata is the HashMap after the script instance, or after a Variant
--- holding the script in the releases that still keep one. Most objects have
--- no metadata, so the map is recognised by its shape: a capacity index
--- between 2 (the default) and 28, and a head exactly when it has entries.
--- A Variant at the same place fails that test, because its third qword
--- (the would-be capacity) is the second half of its data.
--
local function MapShape(raw, off)
    local head, capacity, size = Q(raw, off + MAP_HEAD), D(raw, off + MAP_CAPACITY), D(raw, off + MAP_SIZE)
    if capacity < 2 or capacity > 28 or size > 1000000 then return false end
    if size == 0 then return head == 0 end
    return IsHeap(head)
end

local function ProbeMetadata()
    if not L.script then return end
    local candidates = { L.script + 8, L.script + 0x20 }
    local good, checked = { 0, 0 }, 0
    EachObject(function(obj)
        local raw = ReadRaw(obj, L.script + 0x50)
        if not raw then return end
        checked = checked + 1
        for i, at in ipairs(candidates) do
            if MapShape(raw, at) then good[i] = good[i] + 1 end
        end
        if checked >= 200 then return false end
    end)
    for i, at in ipairs(candidates) do
        if checked >= 8 and good[i] >= checked * 0.95 then L.meta = at return end
    end
end

--
--- ∑ The whole resolution: version, StringName table, ObjectDB, ClassDB,
--- the type pointer, scripts and the scene tree. Every stage that fails
--- says which one, so a broken game is a one-line report, not a silent
--- "nothing found". The scene tree is optional: early in a game's start it
--- may not exist yet and is looked for again on demand.
--
local function Resolve()
    local started = os.clock()
    local timings, mark = {}, started
    local function stage(name)
        local now = os.clock()
        timings[#timings + 1] = string.format("%s %.2f s", name, now - mark)
        mark = now
    end
    LoadImages()
    if not S.main then Fail(FAIL.declined, "no process is opened") end
    local process = manifold.process.info()
    if not (process and process.is64Bit) then Fail(FAIL.declined, "only 64-bit games are supported") end
    LoadSections()
    S.version = FindVersion()
    if not S.version then Fail(FAIL.declined, "not a Godot 4 game (no \"Godot Engine v4\" in " .. S.main.name .. ")") end
    stage("version")

    local blocks = DataBlocks()
    if #blocks == 0 then Fail(FAIL.stage, "the executable has no readable data section") end
    local names = FindNameTable(blocks)
    if not names then Fail(FAIL.stage, "StringName table not found") end
    S.nameTable, L.hash = names.start, names.hash
    ProbeNameLayout()
    stage("names")

    local db = FindObjectDB(blocks)
    if not db then Fail(FAIL.stage, "ObjectDB slot array not found") end
    S.db, L.id = db, db.id
    stage("objects")

    local classdb = FindClassDB(blocks)
    if not classdb then Fail(FAIL.stage, "ClassDB class table not found") end
    S.classdb = classdb.at
    IndexClasses()
    ProbeClassInfo()
    ProbeTypePointer()
    stage("classes")

    ProbeScripts()
    ProbeMetadata()
    local treeOk, treeError = ProbeTree(blocks)
    S.treeError = not treeOk and treeError or nil
    stage("scene")

    S.ready, S.failed = true, nil
    RegisterCoreSymbols()
    S.seconds = os.clock() - started
    -- ∑ The three lines keep the text of version 1: they are what a game is
    -- checked against (the offsets measured in Unrailed 2 among them).
    Channel.Resolve.info(string.format("Godot %s | StringNames at %X | ObjectDB at %X (%d objects) | ClassDB at %X (%d classes) | %.1f s (%s)",
        S.version.text, S.nameTable, db.slotsAt, Count(), S.classdb, #S.classList, S.seconds, table.concat(timings, ", ")))
    Channel.Resolve.info(string.format("StringName hash %X text %X%s size %s | Object id %X type %s (%s) script %s meta %s | ClassInfo name %X inherits %X create %s methods %s | MethodBind name %s code %s",
        L.hash, L.str, L.cname and " cname 8" or "", L.strSize and Hex(L.strSize) or "scan",
        L.id, Hex(L.type), L.typeKind or "RTTI", Hex(L.script), Hex(L.meta),
        L.ciName, L.ciInherits, Hex(L.ciCreate), Hex(L.ciMethods), Hex(L.mbName), Hex(L.mbCode)))
    if treeOk then
        Channel.Resolve.info(string.format("SceneTree %X, root %X | Node parent %X owner %s children %X name %X tree %X | Resource path %s",
            S.tree, S.root, L.parent, Hex(L.owner), L.children, L.name, L.tree, Hex(L.resPath)))
    else
        Channel.Resolve.warning("Scene tree not resolved yet: " .. tostring(treeError) .. " (tried again on demand)")
    end
end

local function Ready(auto)
    Session()
    if S.ready then return true end
    if S.pid == 0 then return false end
    if S.failed then return false end
    if not auto then return false end
    local ok, err = manifold.error.try(Resolve)
    if not ok then
        S.ready, S.failed = false, tostring(err.message)
        if err.code == FAIL.declined then
            Channel.Resolve.debug("Godot not resolved: " .. S.failed)
        elseif err.code == FAIL.unsupported then
            Channel.Resolve.warning("Godot not resolved: " .. S.failed)
        elseif err.code == FAIL.stage then
            Channel.Resolve.error("Godot not resolved: " .. S.failed)
        else
            Channel.Resolve.error(err)
        end
        return false
    end
    return true
end

local function Require()
    if not Ready(true) then error("Godot is not resolved: " .. tostring(S and S.failed or "no process"), 2) end
end

local function RequireTree()
    Require()
    local root = Root()
    if not root then error("the scene tree is not resolved: " .. tostring(S.treeError or "no SceneTree object"), 2) end
    return root
end

----------------------------------------
-- CUSTOM TYPES
----------------------------------------

-- ∑ The process events end the session, so a ready session is always the
-- opened process's; the pid comparison of version 1 is no longer needed.
local function Live() return S ~= nil and S.ready end

State.CustomName = function(bytes)
    local data = BytesToInteger(bytes, 1, 8)
    if data == 0 then return "" end
    if not Live() then return "?" end
    return NameText(data) or "??"
end

State.CustomString = function(bytes)
    local ptr = BytesToInteger(bytes, 1, 8)
    if ptr == 0 then return "" end
    if not Live() then return "?" end
    return StringAt(ptr) or "??"
end

State.CustomId = function(bytes)
    local id = BytesToInteger(bytes, 1, 8)
    if id == 0 then return "null" end
    if not Live() then return "?" end
    local object = FromId(id)
    if not object then return string.format("#%d (freed)", id & SLOT_MASK) end
    local name = IsNode(object) and NodeName(object)
    return string.format("%s%s #%d", ClassName(object), name and (" '" .. name .. "'") or "", id & SLOT_MASK)
end

-- ∑ A custom type is asked for every value it shows, many times a second;
-- a value that cannot be decoded shows "??". pcall keeps that cheap where
-- manifold.error.try would build an error object and a traceback for every
-- such value.
local function CustomCaller(count, key)
    return function(...)
        local bytes = { ... }
        local ok, text = pcall(State[key], { table.unpack(bytes, 1, count) })
        return ok and text or "??"
    end
end

-- ∑ The manifold API has no custom types, so they stay Cheat Engine's
-- registerCustomTypeLua, registered once per run of the program; the
-- registered callers dispatch through State, which a new run of this file
-- refills.
local function RegisterCustomTypes()
    local function register(name, key)
        if getCustomType(name) then return end
        local ok, err = manifold.error.try(registerCustomTypeLua, name, 8, CustomCaller(8, key), function()
            error("this value is read only")
        end, false, true)
        if not ok then Channel.CustomTypes.warning("could not register " .. name .. ", it shows as a number", { reason = err.message }) end
    end
    register(CUSTOM_NAME, "CustomName")
    register(CUSTOM_STRING, "CustomString")
    register(CUSTOM_ID, "CustomId")
end

local function Custom(name)
    return getCustomType(name) or nil
end

----------------------------------------
-- STRUCTURE DISSECT
----------------------------------------

local function FindGlobalStructure(name)
    for i = 0, getStructureCount() - 1 do
        local structure = getStructure(i)
        if structure and structure.Name == name then return structure end
    end
end

local function Annotated(name, text)
    return text and text ~= "" and name .. "  (" .. text .. ")" or name
end

--
--- ∑ Map entries are heap blocks nothing describes. While an object is
--- filled, the entries of its children and metadata maps are remembered;
--- expanding one of their pointers then asks the dissect callbacks about
--- exactly that address, and they fill the entry from what it holds.
--
local function RememberMap(map, kind, label)
    if (S.elementCount or 0) >= ELEMENT_MEMORY then S.elements, S.elementCount = {}, 0 end
    local n = 0
    MapEach(map, function(e)
        if not S.elements[e] then S.elementCount = S.elementCount + 1 end
        S.elements[e] = { kind = kind, label = label }
        n = n + 1
    end, 4096)
    return n
end

local Filler = {}
Filler.__index = Filler

-- ∑ Elements are collected first and added in offset order at the end, so
-- the native gaps land where they belong and hosts that show elements in
-- insertion order still show them sorted.
local function NewFiller(structure, base)
    return setmetatable({ structure = structure, base = base, count = 0, specs = {} }, Filler)
end

function Filler:Add(offset, name, vartype, options)
    if self.count >= ELEMENT_LIMIT then return nil end
    options = options or {}
    self.count = self.count + 1
    local spec = { offset = offset, name = name, vartype = vartype, options = options, order = self.count,
        width = options.width or options.bytesize or 8 }
    self.specs[#self.specs + 1] = spec
    return spec
end

function Filler:Flush()
    table.sort(self.specs, function(a, b)
        if a.offset ~= b.offset then return a.offset < b.offset end
        return a.order < b.order
    end)
    local customs = {}
    for _, spec in ipairs(self.specs) do
        local options = spec.options
        local e = self.structure.addElement()
        e.Offset = spec.offset
        e.Name = spec.name
        local custom = options.custom and customs[options.custom]
        if options.custom and custom == nil then
            custom = Custom(options.custom) or false
            customs[options.custom] = custom
        end
        if custom then
            e.Vartype = VT.Custom
            e.CustomType = custom
        else
            e.Vartype = spec.vartype
            if options.bytesize then e.Bytesize = options.bytesize end
            if options.hex then e.DisplayMethod = "dtHexadecimal" elseif options.signed then e.DisplayMethod = "dtSignedInteger" end
        end
    end
    self.specs = {}
end

function Filler:Gaps(size)
    local ranges = {}
    for _, spec in ipairs(self.specs) do ranges[#ranges + 1] = { from = spec.offset, to = spec.offset + spec.width } end
    table.sort(ranges, function(a, b) return a.from < b.from end)
    local gaps, cursor = {}, 0
    for _, range in ipairs(ranges) do
        if range.from > cursor then gaps[#gaps + 1] = { from = cursor, to = range.from } end
        if range.to > cursor then cursor = range.to end
    end
    if size and size > cursor then gaps[#gaps + 1] = { from = cursor, to = size } end
    for _, gap in ipairs(gaps) do
        local at, stop = gap.from, math.min(gap.to, gap.from + GAP_LIMIT)
        while at < stop and self.count < ELEMENT_LIMIT do
            if at % 8 == 0 and at + 8 <= gap.to then
                local value = self.base and RP(self.base + at)
                local pointer = value and IsPtr(value)
                self:Add(at, "(native)", pointer and VT.Pointer or VT.Qword, { hex = not pointer, width = 8 })
                at = at + 8
            else
                local step = at % 4 == 0 and at + 4 <= gap.to and 4 or 1
                self:Add(at, "(native)", step == 4 and VT.Dword or VT.Byte, { hex = true, width = step })
                at = at + step
            end
        end
    end
end

function Filler:Map(offset, name, kind)
    self:Add(offset, name .. ".elements", VT.Pointer)
    self:Add(offset + 8, name .. ".hashes", VT.Pointer)
    self:Add(offset + MAP_HEAD, name .. ".head", VT.Pointer)
    self:Add(offset + MAP_TAIL, name .. ".tail", VT.Pointer)
    self:Add(offset + MAP_CAPACITY, name .. ".capacity_index", VT.Dword, { width = 4 })
    self:Add(offset + MAP_SIZE, name .. ".size", VT.Dword, { width = 4 })
    if self.base then RememberMap(self.base + offset, kind, name) end
end

function Filler:Variant(offset, name)
    local vtype = self.base and RU(self.base + offset) or nil
    local typeName = vtype and VARIANT_TYPES[vtype]
    self:Add(offset, Annotated(name .. ".type", typeName), VT.Dword, { width = 4 })
    local data = offset + 8
    if vtype == 1 then return self:Add(data, name, VT.Byte, { width = 1 }) end
    if vtype == 2 then return self:Add(data, name, VT.Qword, { signed = true }) end
    if vtype == 3 then return self:Add(data, name, VT.Double) end
    if vtype == 4 then return self:Add(data, name, VT.Pointer, { custom = CUSTOM_STRING }) end
    if vtype == 21 then return self:Add(data, name, VT.Pointer, { custom = CUSTOM_NAME }) end
    if vtype == 24 then
        self:Add(data, name .. ".id", VT.Qword, { custom = CUSTOM_ID, hex = true })
        return self:Add(data + 8, name .. ".object", VT.Pointer)
    end
    local floats = ({ [5] = 2, [7] = 4, [9] = 3, [12] = 4, [14] = 4, [15] = 4, [20] = 4 })[vtype]
    if floats then
        local axes = { "x", "y", "z", "w" }
        for i = 1, floats do self:Add(data + (i - 1) * 4, name .. "." .. axes[i], VT.Single, { width = 4 }) end
        return
    end
    local ints = ({ [6] = 2, [8] = 4, [10] = 3, [13] = 4 })[vtype]
    if ints then
        local axes = { "x", "y", "z", "w" }
        for i = 1, ints do self:Add(data + (i - 1) * 4, name .. "." .. axes[i], VT.Dword, { width = 4, signed = true }) end
        return
    end
    self:Add(data, name .. ".data", VT.Pointer)
    self:Add(data + 8, name .. ".data+8", VT.Qword, { hex = true })
end

function Filler:Object(object)
    self:Add(0, "VTable", VT.Pointer)
    self:Add(8, "_extension", VT.Pointer)
    self:Add(0x10, "_extension_instance", VT.Pointer)
    self:Add(L.id, "_instance_id", VT.Qword, { custom = CUSTOM_ID, hex = true })
    if L.type then self:Add(L.type, L.typeKind == "gdtype" and "_gdtype_ptr" or "_class_name_ptr", VT.Pointer) end
    if L.script then
        local _, layout = ScriptInstanceOf(object)
        local text = layout and layout.class or nil
        local path = ScriptPath(object)
        if path then text = (text and text .. ", " or "") .. path end
        self:Add(L.script, Annotated("script_instance", text), VT.Pointer)
    end
    if L.meta then self:Map(L.meta, "metadata", "variant") end
end

function Filler:Node(object)
    if L.sceneFile then
        self:Add(L.sceneFile, "scene_file_path", VT.Pointer, { custom = CUSTOM_STRING })
        self:Add(L.sceneFile + 8, "instance_state", VT.Pointer)
        self:Add(L.sceneFile + 0x10, "inherited_state", VT.Pointer)
    end
    local parent = ParentOf(object)
    self:Add(L.parent, Annotated("parent", parent and NodeName(parent)), VT.Pointer)
    if L.owner then
        local owner = RPtr(object + L.owner)
        self:Add(L.owner, Annotated("owner", owner and NodeName(owner)), VT.Pointer)
    end
    self:Map(L.children, "children", "node")
    self:Add(L.name, "name", VT.Pointer, { custom = CUSTOM_NAME })
    self:Add(L.tree, "tree", VT.Pointer)
end

-- ∑ Manifold Engine's structure objects have beginUpdate and endUpdate, so
-- they are called directly; a filling that stops is logged with its line
-- and traceback, and the update is closed either way.
local function Fill(structure, base, fn)
    local filler = NewFiller(structure, base)
    structure.beginUpdate()
    local ok, err = manifold.error.try(fn, filler)
    structure.endUpdate()
    if not ok then Channel.Dissect.error(err, { filling = Hex(base) }) end
    return filler.count
end

local function FillObject(structure, object)
    return Fill(structure, object, function(filler)
        filler:Object(object)
        if IsNode(object) then filler:Node(object) end
        local class = ClassName(object)
        if L.resPath and Inherits(class, "Resource") then filler:Add(L.resPath, "path", VT.Pointer, { custom = CUSTOM_STRING }) end
        if L.root and Inherits(class, "SceneTree") then filler:Add(L.root, "root", VT.Pointer) end
        filler:Gaps(ObjectSize(object))
        filler:Flush()
    end)
end

local function FillElement(structure, element, entry)
    return Fill(structure, element, function(filler)
        filler:Add(ELEMENT_NEXT, "next", VT.Pointer)
        filler:Add(ELEMENT_PREV, "prev", VT.Pointer)
        filler:Add(ELEMENT_KEY, "key", VT.Pointer, { custom = CUSTOM_NAME })
        if entry.kind == "node" then
            local child = RPtr(element + ELEMENT_VALUE)
            filler:Add(ELEMENT_VALUE, Annotated("node", child and ClassName(child)), VT.Pointer)
        else
            filler:Variant(ELEMENT_VALUE, "value")
        end
        filler:Flush()
    end)
end

local function FillInstance(structure, instance, layout, owner)
    return Fill(structure, instance, function(filler)
        filler:Add(0, "VTable", VT.Pointer)
        filler:Add(layout.owner, Annotated("owner", ClassName(owner)), VT.Pointer)
        if layout.script then
            filler:Add(layout.script, Annotated("script", ScriptPath(owner)), VT.Pointer)
            if layout.class:find("CSharpInstance", 1, true) then
                filler:Add(layout.script + 8, "gchandle.handle", VT.Pointer)
                filler:Add(layout.script + 0x10, "gchandle.type", VT.Dword, { width = 4 })
            end
        end
        filler:Gaps(layout.script and layout.script + 0x18 or layout.owner + 8)
        filler:Flush()
    end)
end

local function ElementAt(address)
    local entry = S.elements and S.elements[address]
    if not entry then return nil end
    if not NameText(RP(address + ELEMENT_KEY)) then return nil end
    return entry
end

-- ∑ The structure name resolver: the class names the structure, and the
-- object's real start replaces an address that points into its middle.
local function NameLookup(address)
    if not Ready(true) then return nil end
    local entry = ElementAt(address)
    if entry then return entry.label .. " entry", address end
    local object = OwnerOf(address)
    if object then return ClassName(object), object end
    local layout = ScriptInstanceAt(address)
    if layout then return layout.class, address end
    return nil
end

-- ∑ The dissector: objects, remembered map entries and script instances
-- are filled; anything else (nil) is left to the next dissector, such as
-- Manifold-RTTI's, and to Structure Dissect's own guess.
local function DissectOverride(structure, base)
    if not Ready(true) then return nil end
    local entry = ElementAt(base)
    if entry then return FillElement(structure, base, entry) > 0 end
    if IsObject(base) then return FillObject(structure, base) > 0 end
    local layout, owner = ScriptInstanceAt(base)
    if layout then return FillInstance(structure, base, layout, owner) > 0 end
    return nil
end

local function Dissect(address)
    Require()
    local object = OwnerOf(address)
    if not object then error(string.format("no Godot object contains %X", address), 2) end
    local name = ClassName(object)
    local structure = FindGlobalStructure(name)
    if not structure then
        structure = createStructure(name)
        FillObject(structure, object)
        structure.addToGlobalStructureList()
    end
    local form = createStructureForm(string.format("%X", object), "Group 1", name)
    local label = IsNode(object) and PathOf(object) or (ResourcePath(object) or "")
    if form and type(form) == "userdata" then pcall(function() form.Caption = "Structure dissect: " .. name .. " " .. label end) end
    return form, structure
end

----------------------------------------
-- SYMBOLS
----------------------------------------

-- GodotPath__root__Main__Player -> /root/Main/Player
local function NodeSymbol(node)
    local path = PathOf(node)
    if path:find("[^%w_/]") or path:find("__", 1, true) then return nil end
    return NODE_SYMBOL_PREFIX .. path:sub(2):gsub("/", "__")
end

--
--- ∑ Offsets and the core tables resolve on demand, so a table saved with
--- them works again in the next session without anyone registering the
--- symbols first. Node paths are looked up fresh every time and never
--- registered: the node is a new object after every scene change. Every
--- other name is not this script's and is answered with nil at once, before
--- anything resolves, so Manifold-UE's <Type>_<Property> names and any
--- other resolver keep their turn.
--
local function SymbolLookup(token)
    if type(token) ~= "string" or #token > 512 then return nil end
    local core = token == "GodotStringNames" or token == "GodotObjectDB" or token == "GodotClassDB" or token == "GodotSceneTree"
    local offset = OFFSET_SYMBOLS[token]
    local node = token:sub(1, #NODE_SYMBOL_PREFIX) == NODE_SYMBOL_PREFIX
    if not core and not offset and not node then return nil end
    if not Ready(true) then return nil end
    if core then return CoreSymbol(token) end
    if offset then
        if not L[offset] and TREE_OFFSETS[offset] then Tree() end
        return L[offset]
    end
    local path = "/" .. token:sub(#NODE_SYMBOL_PREFIX + 1):gsub("__", "/")
    return NodeAt(path)
end

--
--- ∑ The bulk registration (14611 names in Unrailed 2). registerSymbol is
--- the cached manifold.symbol.register, called directly in the loop, and
--- each class's "Class::" prefix is built once. The names are not kept
--- anywhere: the engine drops them when the process closes or another one
--- opens, and a later call registers them again, replacing each name.
--
local function NameMethods()
    Require()
    if not L.mbCode then error("the method bind layout was not resolved; method addresses are unknown") end
    local started, count, skipped = os.clock(), 0, 0
    for _, info in ipairs(S.classList) do
        local prefix = info.name .. "::"
        for _, m in ipairs(OwnMethods(info)) do
            -- ∑ Class::vftable belongs to Manifold-RTTI; no Godot method has that
            -- name, and the check keeps it so.
            if m.address and not m.virtual and m.name ~= "vftable" and InImage(m.address) then
                if registerSymbol(prefix .. m.name, m.address) then count = count + 1 else skipped = skipped + 1 end
            else
                skipped = skipped + 1
            end
        end
    end
    S.methodSymbols = count
    Channel.Methods.info(string.format("%d engine methods named in %.2f s (%d virtual or without code skipped)", count, os.clock() - started, skipped))
    return count
end

----------------------------------------
-- API
----------------------------------------

local function ToAddress(value)
    if math.type(value) == "integer" then return value end
    if type(value) == "number" then return math.floor(value) end
    if type(value) == "string" then
        local address = manifold.process.address(value)
        if address and address ~= 0 then return address end
        return tonumber(value, 16)
    end
    return nil
end

local function NodeArg(value)
    if type(value) == "string" and value:sub(1, 1) == "/" then
        local node = NodeAt(value)
        if not node then error("no node at " .. value, 3) end
        return node
    end
    local address = ToAddress(value)
    if not address then error("not a node or address: " .. tostring(value), 3) end
    return address
end

function Api.init(force)
    if force then S = nil end
    Ready(true)
    return S and S.ready or false
end

function Api.status()
    Session()
    if S.ready then
        local tree = S.tree and string.format(", SceneTree %X, root %X", S.tree, S.root or 0) or (", scene tree: " .. tostring(S.treeError or "not resolved"))
        return string.format("Godot %s, %d objects, %d classes%s", S.version.text, Count(), #S.classList, tree)
    end
    if S.failed then return "not resolved: " .. S.failed end
    return S.pid == 0 and "no process" or "not resolved yet"
end

function Api.version() Require(); return S.version.text, S.version.major, S.version.minor, S.version.patch end
function Api.layout() Require(); local copy = {}; for k, v in pairs(L) do copy[k] = v end; return copy end
function Api.count() Require(); return Count() end
function Api.objects(limit)
    Require()
    local out = {}
    EachObject(function(obj) out[#out + 1] = obj; if limit and #out >= limit then return false end end)
    return out
end
function Api.isObject(address) Require(); return IsObject(ToAddress(address)) end
function Api.fromId(id) Require(); return FromId(id) end
function Api.idOf(object) Require(); return RQ(ToAddress(object) + L.id) end
function Api.className(object) Require(); return ClassName(ToAddress(object)) end
function Api.name(address) Require(); return NameAt(ToAddress(address)) end
function Api.nameText(data) Require(); return NameText(ToAddress(data)) end
function Api.findName(text) Require(); return FindName(text) end
function Api.string(address) Require(); return StringAt(RP(ToAddress(address))) end
function Api.classes()
    Require()
    local out = {}
    for _, info in ipairs(S.classList) do out[#out + 1] = info.name end
    return out
end
function Api.class(name)
    Require()
    local info = S.classes[name]
    if not info then return nil end
    return { name = info.name, inherits = SuperName(info), ancestry = Ancestry(info.name), size = ClassSize(info.name),
        info = info.value, methods = #OwnMethods(info) }
end
function Api.inherits(name, base) Require(); return Inherits(name, base) end
function Api.methods(class, inherited)
    Require()
    local out = {}
    for _, m in ipairs(Methods(class, inherited)) do
        out[#out + 1] = { name = m.name, class = m.class, address = m.address, bind = m.bind, virtual = m.virtual }
    end
    return out
end
function Api.method(class, name)
    Require()
    for _, m in ipairs(Methods(class, true)) do
        if m.name == name then return m.address, m.bind end
    end
    return nil
end
function Api.instances(class, limit, exact) Require(); return Instances(class, limit, exact) end
function Api.size(classOrObject)
    Require()
    if type(classOrObject) == "string" and S.classes[classOrObject] then return ClassSize(classOrObject) end
    return ObjectSize(ToAddress(classOrObject))
end
function Api.tree() Require(); return Tree() end
function Api.root() Require(); return Root() end
function Api.node(path) RequireTree(); return NodeAt(path) end
function Api.nodeName(node) RequireTree(); return NodeName(NodeArg(node)) end
function Api.parent(node) RequireTree(); return ParentOf(NodeArg(node)) end
function Api.children(node) RequireTree(); return Children(NodeArg(node)) end
function Api.path(node) RequireTree(); return PathOf(NodeArg(node)) end
function Api.symbol(node) RequireTree(); return NodeSymbol(NodeArg(node)) end
function Api.find(text, limit)
    local root = RequireTree()
    local out, needle = {}, tostring(text):lower()
    EachNode(function(node)
        local name = NodeName(node)
        if name and name:lower():find(needle, 1, true) then out[#out + 1] = node end
        if #out >= (limit or 200) then return false end
    end, root)
    return out
end
function Api.printTree(node, maxDepth, limit)
    local root = RequireTree()
    local text = TreeText(node and NodeArg(node) or root, maxDepth or 64, limit or 4000)
    log.info(text)
    return text
end
function Api.script(object) Require(); local script = ScriptOf(ToAddress(object)); return script, ResourcePath(script) end
function Api.scriptPath(object) Require(); return ScriptPath(ToAddress(object)) end
function Api.managed(object) Require(); return ManagedObject(ToAddress(object)) end
function Api.resourcePath(resource) Require(); return ResourcePath(ToAddress(resource)) end
function Api.owner(address) Require(); return OwnerOf(ToAddress(address)) end

-- ∑ For other dissect scripts (Manifold-RTTI): true for every address this
-- script names itself, so they can step aside whichever callback runs
-- first. It resolves like a dissect would; a process that is not Godot
-- fails that once and answers false from then on.
function Api.claims(address)
    if not Ready(true) then return false end
    address = ToAddress(address)
    if not address then return false end
    return ElementAt(address) ~= nil or OwnerOf(address) ~= nil or ScriptInstanceAt(address) ~= nil
end
function Api.whois(address) Require(); return Whois(ToAddress(address)) end
function Api.dissect(address) return Dissect(NodeArg(address)) end
function Api.nameMethods() return NameMethods() end

--
--- ∑ ManifoldGodot is an entry point for the Lua console, tables and other
--- scripts. A failure no longer stops the caller: it is logged with the
--- call, the script line and the traceback, and the call returns nil and
--- the error object, the way manifold functions report a failure.
--
local function Finish(name, ok, ...)
    if ok then return ... end
    local err = ...
    Channel.API.error(err, { call = "ManifoldGodot." .. name })
    return nil, err
end

for name, fn in pairs(Api) do
    Godot[name] = function(...) return Finish(name, manifold.error.try(fn, ...)) end
end

-- ∑ claims keeps the contract Manifold-RTTI's EngineOwns relies on: a
-- boolean and never an error. RTTI asks it for every address it would name,
-- so a failure inside is only a debug line and answers false.
function Godot.claims(address)
    local ok, claimed = manifold.error.try(Api.claims, address)
    if ok then return claimed == true end
    Channel.API.debug("claims answered false after an error", { reason = claimed.message })
    return false
end

----------------------------------------
-- MENU
----------------------------------------

-- ∑ A menu action that fails is logged with its line and traceback and
-- shown in a dialog with the message alone.
local function Guarded(caption, fn)
    return function(...)
        local ok, err = manifold.error.try(fn, ...)
        if not ok then
            Channel.Menu.error(err, { item = caption })
            messageDialog(tostring(err.message), mtError, mbOK)
        end
    end
end

local function AskAddress(title)
    local text = inputQuery(title, "Address (number, symbol or expression):", "")
    if not text or text == "" then return nil end
    local address = ToAddress(text)
    if not address then error("not an address: " .. text) end
    return address
end

local function ListText(title, nodes)
    local lines = { title }
    for _, node in ipairs(nodes) do lines[#lines + 1] = string.format("  %X  %s  (%s)", node, PathOf(node), Describe(node)) end
    return table.concat(lines, "\n")
end

--
--- ∑ The two dissect hooks are keyed manifold registrations: running this
--- file again replaces them, so they never stack. The ids only serve the
--- checkbox, which removes them; a new run starts with no ids and its keyed
--- registration takes the place of the previous run's.
--
local hookIds = {}

local function SetDissectHooks(enabled)
    if hookIds.name then manifold.structure.removeNameResolver(hookIds.name) end
    if hookIds.dissector then manifold.structure.removeDissector(hookIds.dissector) end
    hookIds = {}
    State.HooksEnabled = enabled
    if not enabled then return end
    hookIds.name = manifold.structure.addNameResolver(NameLookup, { key = REGISTRATION_KEY })
    hookIds.dissector = manifold.structure.addDissector(DissectOverride, { key = REGISTRATION_KEY })
end

local function InstallMenu()
    if State.MenuRoot then
        pcall(function() State.MenuRoot.destroy() end)
        State.MenuRoot = nil
    end
    local mainForm = rawget(_G, "MainForm") or getMainForm()
    if not mainForm then return end
    local ok, err = manifold.error.try(function()
        local menu = mainForm.Menu
        local root = createMenuItem(menu)
        root.Caption = MENU_CAPTION
        menu.Items.add(root)
        State.MenuRoot = root
        local status
        local function item(caption, onClick)
            local entry = createMenuItem(menu)
            entry.Caption = caption
            if onClick then entry.OnClick = Guarded(caption, onClick) end
            root.add(entry)
            return entry
        end
        status = item("Status: not resolved yet")
        status.Enabled = false
        root.OnClick = function()
            local fine, text = manifold.error.try(Api.status)
            if fine then status.Caption = "Status: " .. text end
        end
        item("Resolve again", function()
            S = nil
            Api.init()
            status.Caption = "Status: " .. Api.status()
        end)
        item("-")
        item("Dissect SceneTree", function()
            Require()
            local tree = Tree()
            if not tree then error("no SceneTree object yet") end
            Dissect(tree)
        end)
        item("Dissect root window", function() Dissect(RequireTree()) end)
        item("Dissect node by path...", function()
            RequireTree()
            local path = inputQuery("Dissect Godot node", "Node path (e.g. /root/Main/Player):", "/root/")
            if not path or path == "" then return end
            local node = NodeAt(path)
            if not node then error("no node at " .. path) end
            Dissect(node)
        end)
        item("Dissect object at address...", function()
            local address = AskAddress("Dissect Godot object")
            if address then Dissect(address) end
        end)
        item("-")
        item("Print scene tree", function() Api.printTree() end)
        item("Find nodes by name...", function()
            local text = inputQuery("Find Godot nodes", "Part of the node name:", "")
            if not text or text == "" then return end
            local nodes = Api.find(text, 200)
            log.info(ListText(string.format("%d nodes named like '%s':", #nodes, text), nodes))
        end)
        item("Instances of a class...", function()
            Require()
            local class = inputQuery("Godot instances", "Class name (subclasses included):", "Node3D")
            if not class or class == "" then return end
            if not S.classes[class] then error("ClassDB has no class " .. class) end
            local list = Instances(class, 500)
            local lines = { string.format("%d instances of %s:", #list, class) }
            for _, obj in ipairs(list) do
                local where = IsNode(obj) and PathOf(obj) or (ResourcePath(obj) or "")
                lines[#lines + 1] = string.format("  %X  %s  %s", obj, Describe(obj), where)
            end
            log.info(table.concat(lines, "\n"))
        end)
        item("Who owns an address...", function()
            local address = AskAddress("Who owns this address?")
            if not address then return end
            Require()
            local text, object = Whois(address)
            if not text then
                messageDialog(string.format("No Godot object contains %X.", address), mtInformation, mbOK)
                return
            end
            log.info(text)
            if messageDialog(text .. "\n\nDissect the owner?", mtConfirmation, mbYes, mbNo) == mrYes then Dissect(object) end
        end)
        item("-")
        item("Name engine methods in the disassembler", function()
            local count = NameMethods()
            messageDialog(string.format("%d engine methods are named now (Class::method).", count), mtInformation, mbOK)
        end)
        local hooks = item("Structure Dissect uses Godot names", function() end)
        hooks.Checked = State.HooksEnabled ~= false
        hooks.OnClick = Guarded("Hooks", function()
            hooks.Checked = not hooks.Checked
            SetDissectHooks(hooks.Checked)
        end)
    end)
    if not ok then Channel.Menu.warning("could not build the menu", { reason = err.message }) end
end

----------------------------------------
-- MAIN
----------------------------------------

--
--- ∑ Everything is registered under REGISTRATION_KEY (the events under
--- their own sub-keys), so a second run of this file replaces the first
--- run's resolver, hooks and handlers. The handlers only end the session:
--- resolving stays lazy and happens on first use in the new process, which
--- also registers the core symbols for it again. The ready line is debug
--- chatter; with Show on print on, an info line at every program start
--- would open the Lua engine window.
--
local function Main()
    RegisterCustomTypes()
    SetDissectHooks(State.HooksEnabled ~= false)
    manifold.symbol.addResolver(SymbolLookup, { key = REGISTRATION_KEY })
    manifold.event.subscribe("processOpened", EndSession, { key = REGISTRATION_KEY .. ".opened" })
    manifold.event.subscribe("processClosed", EndSession, { key = REGISTRATION_KEY .. ".closed" })
    InstallMenu()
    log.debug("Version " .. VERSION .. " ready. Open a Godot 4 game and use Structure Dissect or the " .. MENU_CAPTION .. " menu.")
end

Main()

return Godot
