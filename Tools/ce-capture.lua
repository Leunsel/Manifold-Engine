-- Cheat Engine reference capture: how CE's Lua disassemble() and splitDisassembledString() write instructions.
--
-- Run it in Cheat Engine's Lua engine (Table > Show Cheat Table Lua Script, or the Lua console) while CE is
-- attached to a 64-bit or a 32-bit process:
--   dofile([[<path to the repository>\Tools\ce-capture.lua]])
--
-- It allocates ONE block of its own in the process for test instructions and test data, writes only into that
-- block and frees it again at the end. Nothing else is written. Part 2 only reads the process's own code; set
-- CE_CAPTURE_CODE = false before dofile to leave it out.
--
-- Output: ce-capture-<process>.txt beside this script (or in CE_CAPTURE_OUT, a folder, when set), one
-- tab-separated line per instruction:
--   kind  address  16 raw bytes  CE size  disassemble()  extra  opcode  bytes  address
--
-- Tools/ce_capture_to_inc.py turns a 64-bit and a 32-bit capture into Tests/CheatEngineCapture.inc, the
-- regression test of suite "disasm". It takes the test block only; the process's code stays on your machine.

local function scriptFolder()
  local source = debug.getinfo(1, "S").source or ""
  local path = source:match("^@(.+)[\\/][^\\/]+$")
  return path or os.getenv("TEMP") or "."
end

local folder = CE_CAPTURE_OUT or scriptFolder()
local outPath = folder .. "\\ce-capture-" .. tostring(process or "unknown") .. ".txt"
local is64 = targetIs64Bit()
local lines = {}

local function hexBytes(list)
  local parts = {}
  for i, b in ipairs(list or {}) do parts[i] = string.format("%02X", b) end
  return table.concat(parts)
end

local function record(kind, address)
  local raw = readBytes(address, 16, true)
  local text = disassemble(address)
  local extra, opcode, bytes, at = splitDisassembledString(text)
  lines[#lines + 1] = table.concat({ kind, string.format("%X", address), hexBytes(raw), tostring(getInstructionSize(address)),
    tostring(text), tostring(extra), tostring(opcode), tostring(bytes), tostring(at) }, "\t")
end

local function parse(text)
  local list = {}
  for hex in text:gmatch("%x%x") do list[#list + 1] = tonumber(hex, 16) end
  return list
end

local function le32(value)
  value = value & 0xFFFFFFFF
  return { value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF }
end

local function concat(...)
  local result = {}
  for _, list in ipairs({ ... }) do
    for _, b in ipairs(list) do result[#result + 1] = b end
  end
  return result
end

-- ModRM 05 is RIP-relative in 64-bit code and an absolute address in 32-bit code: the same head works for both.
local function memory(head, target, tail)
  tail = tail or {}
  return function(at)
    local length = #head + 4 + #tail
    local disp = is64 and (target - (at + length)) or target
    return concat(head, le32(disp), tail)
  end
end

-- Part 1: test instructions in a block of our own -------------------------------------------------
local block = allocateMemory(0x2000)
local data1 = block + 0x1800   -- dword 12345678
local data2 = block + 0x1810   -- qword 1122334455667788
local data3 = block + 0x1820   -- float 1.5 (3FC00000)
writeBytes(data1, { 0x78, 0x56, 0x34, 0x12 })
writeBytes(data2, { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 })
writeBytes(data3, { 0x00, 0x00, 0xC0, 0x3F })

local cases = {}
local function add(kind, bytes) cases[#cases + 1] = { kind, bytes } end

for i = 0, 15 do
  add("jcc8", { 0x70 + i, 0x10 })
  add("jcc32", { 0x0F, 0x80 + i, 0x10, 0x00, 0x00, 0x00 })
  add("setcc", { 0x0F, 0x90 + i, 0xC0 })
  add("cmovcc", { 0x0F, 0x40 + i, 0xC1 })
end
for _, text in ipairs({
  "8B 85 F0 FE FF FF",           -- mov eax,[rbp-110]  negative disp32
  "8B 45 80",                    -- mov eax,[rbp-80]   disp8 -128
  "66 B8 34 12",                 -- mov ax,1234
  "66 C7 01 34 12",              -- mov word ptr [rcx],1234
  "C6 01 01",                    -- mov byte ptr [rcx],1
  "48 C7 01 01 00 00 00",        -- mov qword ptr [rcx],1
  "48 C7 C0 FF FF FF FF",        -- mov rax,-1 (sign-extended imm32)
  "48 83 C4 F8",                 -- add rsp,-08
  "48 83 E0 F0",                 -- and rax,-10
  "6B C1 10",                    -- imul eax,ecx,10
  "69 C1 10 01 00 00",           -- imul eax,ecx,110
  "C1 E0 04",                    -- shl eax,04
  "D1 E0",                       -- shl eax,1
  "8B 04 85 10 00 00 00",        -- mov eax,[rax*4+10]  index without base
  "8B 04 88",                    -- mov eax,[rax+rcx*4]
  "8B 44 88 10",                 -- mov eax,[rax+rcx*4+10]
  "4E 8B 44 CA 20",              -- mov r8,[rdx+r9*8+20]
  "8B 04 24",                    -- mov eax,[rsp]
  "8B 45 00",                    -- mov eax,[rbp+00]
  "0F B6 01",                    -- movzx eax,byte ptr [rcx]
  "0F BF 01",                    -- movsx eax,word ptr [rcx]
  "0F B6 C1",                    -- movzx eax,cl
  "48 63 C1",                    -- movsxd rax,ecx
  "48 63 01",                    -- movsxd rax,dword ptr [rcx]
  "F3 AB",                       -- rep stosd
  "F3 48 AB",                    -- rep stosq
  "F3 A4",                       -- rep movsb
  "F3 A6",                       -- repe cmpsb
  "F2 AE",                       -- repne scasb
  "F0 0F B1 11",                 -- lock cmpxchg [rcx],edx
  "F0 FF 01",                    -- lock inc dword ptr [rcx]
  "65 48 8B 04 25 30 00 00 00",  -- mov rax,gs:[30]
  "64 A1 00 00 00 00",           -- mov eax,fs:[0] (32-bit)
  "64 8B 0D 30 00 00 00",        -- mov ecx,fs:[30]
  "C5 F8 28 C1",                 -- vmovaps xmm0,xmm1
  "C5 FC 28 C1",                 -- vmovaps ymm0,ymm1
  "C5 FA 10 01",                 -- vmovss xmm0,[rcx]
  "C4 E3 7D 18 C1 01",           -- vinsertf128 ymm0,ymm0,xmm1,01
  "C4 E2 79 18 01",              -- vbroadcastss xmm0,[rcx]
  "62 F1 7C 48 28 C1",           -- vmovaps zmm0,zmm1 (EVEX)
  "0F 29 74 24 20",              -- movaps [rsp+20],xmm6
  "F3 0F 2A 01",                 -- cvtsi2ss xmm0,dword ptr [rcx]
  "66 0F 6E C0",                 -- movd xmm0,eax
  "66 48 0F 6E C0",              -- movq xmm0,rax
  "66 0F 70 C0 1B",              -- pshufd xmm0,xmm0,1B
  "D9 45 F8",                    -- fld dword ptr [rbp-08]
  "DD 5D F0",                    -- fstp qword ptr [rbp-10]
  "D9 E8",                       -- fld1
  "55", "48 89 E5", "31 C0", "85 C0", "C9", "CC", "90",
  "0F 1F 00", "0F 1F 40 00", "66 0F 1F 84 00 00 00 00 00",
  "C2 08 00",                    -- ret 0008
  "6A 0A", "6A F6",              -- push 0A / push -0A
  "E9 10 00 00 00", "E8 F0 FF FF FF", "E2 F0", "E3 10",
  "FF E0", "FF D0", "FF 20", "FF 60 08",
  "0F 0B",                       -- ud2
  "0F 04", "FF FF", "06", "D6", "0F A2",
}) do add("misc", parse(text)) end

-- The extra field: instructions that name our own data with known values.
add("extra movss", memory({ 0xF3, 0x0F, 0x10, 0x05 }, data3))
add("extra movsd", memory({ 0xF2, 0x0F, 0x10, 0x05 }, data2))
add("extra mov32", memory({ 0x8B, 0x05 }, data1))
add("extra mov64", memory({ 0x48, 0x8B, 0x05 }, data2))
add("extra mov16", memory({ 0x66, 0x8B, 0x05 }, data1))
add("extra mov8", memory({ 0x8A, 0x05 }, data1))
add("extra movzx", memory({ 0x0F, 0xB6, 0x05 }, data1))
add("extra lea", memory({ 0x8D, 0x05 }, data1))
add("extra cmp", memory({ 0x83, 0x3D }, data1, { 0x05 }))
add("extra store", memory({ 0x89, 0x05 }, data1))
add("extra storeimm", memory({ 0xC7, 0x05 }, data1, le32(1)))
add("extra call", memory({ 0xFF, 0x15 }, data2))
add("extra fld", memory({ 0xD9, 0x05 }, data3))
add("extra cvt", memory({ 0xF3, 0x0F, 0x5A, 0x05 }, data3))

for i, case in ipairs(cases) do
  local at = block + (i - 1) * 0x20
  local bytes = type(case[2]) == "function" and case[2](at) or case[2]
  writeBytes(at, bytes)
  record(case[1], at)
end
deAlloc(block)

-- Part 2: the process's own code, 2500 instructions from each of five places in the main module --------
-- Only read. These lines are for comparing more real code locally; ce_capture_to_inc.py leaves them out.
if CE_CAPTURE_CODE ~= false then
  local base = getAddress(process)
  local size = getModuleSize(process)
  for _, offset in ipairs({ 0x1000, 0x10000, 0x100000, 0x400000, 0x1000000 }) do
    if base and size and offset < size then
      local at = base + offset
      local kind = string.format("code+%X", offset)
      for _ = 1, 2500 do
        record(kind, at)
        local length = getInstructionSize(at)
        at = at + ((length and length > 0) and length or 1)
        if at >= base + size then break end
      end
    end
  end
end

local file = assert(io.open(outPath, "w"))
file:write(table.concat(lines, "\n"), "\n")
file:close()
print(string.format("%d lines written to %s (%s, %d-bit)", #lines, outPath, tostring(process), is64 and 64 or 32))
