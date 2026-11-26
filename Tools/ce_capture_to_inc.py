"""Turns Cheat Engine capture files into Tests/CheatEngineCapture.inc and Tests/CheatEngineExtra.inc.

The capture files come from Tools/ce-capture.lua, run in Cheat Engine's Lua engine while CE is attached to a
64-bit or a 32-bit process. Only the test block goes into the includes: the instructions the script wrote into
memory it allocated itself. The lines from the process's own code ("code+..." kinds) stay out, so no code of the
process ends up in the repository.

CheatEngineCapture.inc holds CE's disassemble() text. Lines where Cheat Engine's own decoder is wrong are left out
there, because Manifold does not copy those faults (see issue #1): VEX/EVEX instructions, bytes CE cannot decode
(empty text or "db"), 32-bit-only opcodes in 64-bit code, and the missing/doubled commas CE writes for some SSE forms.

CheatEngineExtra.inc holds the fourth value of splitDisassembledString (issue #3) for every test-block line, with
the block's data area and the names CE gave values in the block, so the test can rebuild the memory CE read.
Captures from before the "data" and "name" lines are left out of it. The process's own module name becomes
"game.exe" there, so no game name ends up in the repository.

Usage:
    python Tools/ce_capture_to_inc.py capture-64.txt capture-32.txt [--out Tests/CheatEngineCapture.inc]
                                                                    [--extra-out Tests/CheatEngineExtra.inc]

The 64-bit capture comes first in the includes, whatever the order on the command line. The capture's "target"
line names its bitness; in older captures an address that does not fit 32 bits (the block's or the code's) does.
"""

import argparse
import pathlib
import re
import sys

HEADER = (
    "// Generated from Cheat Engine 7.6 captures the owner made (test-block instructions only, no game code).\n"
    "// One entry per instruction: 64-bit target, address, 16 bytes, CE's disassemble() text.\n"
    "// Left out on purpose, as Cheat Engine's own decoder is wrong there: VEX/EVEX, invalid bytes, comma faults.\n"
)

EXTRA_HEADER = (
    "// Generated from Cheat Engine 7.6 captures the owner made (test-block instructions only, no game code).\n"
    "// CE_EXTRA_LINE: 64-bit target, address, 16 bytes, CE's disassemble() text, splitDisassembledString's extra.\n"
    "// CE_EXTRA_DATA: the block's data area as CE left it. CE_EXTRA_NAME: getNameFromAddress for a value in the block.\n"
)

TEST_KINDS = re.compile(r"^(jcc8|jcc32|setcc|cmovcc|misc|extra2? .+)$")
ONLY_32BIT = re.compile(r"\b(push|pop) (es|cs|ss|ds)\b|\bpopad\b|\bpushad\b|\baa[adms]\b|\bdaa\b|\bdas\b|\binto\b|\bjmp far\b|\(invalid\)")
COMMA_FAULT = re.compile(r",,|\b(?:xmm|mm)\d+[0-9A-F]{2}\b|[a-z]xmm\d")
FIELDS = 9


def records(path):
    # Lua writes the file in text mode: every "\n" became "\r\n", also inside a text CE read from memory, whose
    # line breaks split a record over several lines.
    text = path.read_bytes().decode("utf-8", errors="replace").replace("\r\n", "\n")
    pending = None
    for line in text.split("\n"):
        if pending is not None:
            pending += "\n" + line
            if len(pending.split("\t")) >= FIELDS:
                yield pending.split("\t")
                pending = None
            continue
        parts = line.split("\t")
        if parts and TEST_KINDS.match(parts[0]) and len(parts) < FIELDS:
            pending = line
            continue
        yield parts


def read_capture(path):
    entries, data, names = [], [], []
    bits = None
    wide = False
    for parts in records(path):
        if parts[0] == "target" and len(parts) >= 2:
            bits = parts[1].strip()
        elif len(parts) >= 2 and re.fullmatch(r"[0-9A-Fa-f]+", parts[1] or "") and int(parts[1], 16) > 0xFFFFFFFF and parts[0] not in ("data", "name"):
            wide = True
        if parts[0] == "data" and len(parts) >= 3:
            data.append((int(parts[1], 16), parts[2].strip().upper()))
        elif parts[0] == "name" and len(parts) >= 3:
            names.append((int(parts[1], 16), parts[2]))
        elif len(parts) >= 6 and TEST_KINDS.match(parts[0]):
            kind, address, raw, _size, text, extra = parts[:6]
            entries.append((kind, int(address, 16), raw.upper(), text, extra))
    # The block can lie below 4 GB in a 64-bit process; the script names the bitness, older captures only
    # show it in an address of the process's code or of the block.
    is64 = bits == "64" if bits else wide
    return is64, entries, data, names


def keep(raw, text, is64):
    opcode = text.split(" - ", 2)[2] if text.count(" - ") >= 2 else ""
    if raw[:2] in ("C4", "C5", "62"):
        return False
    if not opcode.strip() or opcode.startswith("db "):
        return False
    if is64 and ONLY_32BIT.search(opcode):
        return False
    if COMMA_FAULT.search(opcode):
        return False
    return True


def quote(text):
    out = []
    for c in text.encode("utf-8"):
        if c in (0x22, 0x5C):
            out.append("\\" + chr(c))
        elif c < 0x20 or c >= 0x7F:
            # An octal escape stops after three digits, so a following digit cannot join it.
            out.append(f"\\{c:03o}")
        else:
            out.append(chr(c))
    return '"' + "".join(out) + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("captures", nargs="+", type=pathlib.Path)
    tests = pathlib.Path(__file__).resolve().parent.parent / "Tests"
    parser.add_argument("--out", type=pathlib.Path, default=tests / "CheatEngineCapture.inc")
    parser.add_argument("--extra-out", type=pathlib.Path, default=tests / "CheatEngineExtra.inc")
    args = parser.parse_args()

    captures = []
    for path in args.captures:
        is64, entries, data, names = read_capture(path)
        if not entries:
            sys.exit(f"{path}: no test-block lines")
        process = path.stem[len("ce-capture-"):] if path.stem.startswith("ce-capture-") else ""
        if process:
            # No game names in the repository: CE names the process's module, the include calls it game.exe.
            neutral = lambda text: re.sub(re.escape(process), "game.exe", text, flags=re.IGNORECASE)
            names = [(value, neutral(name)) for value, name in names]
            entries = [(kind, address, raw, text, neutral(extra)) for kind, address, raw, text, extra in entries]
        captures.append((is64, entries, data, names))
    captures.sort(key=lambda capture: not capture[0])

    out = [HEADER]
    kept = 0
    for is64, entries, _data, _names in captures:
        for _kind, address, raw, text, _extra in entries:
            if not keep(raw, text, is64):
                continue
            escaped = text.replace("\\", "\\\\").replace('"', '\\"')
            out.append(f'{{ {"true" if is64 else "false"}, 0x{address:X}ull, "{raw}", "{escaped}" }},\n')
            kept += 1
    args.out.write_bytes("".join(out).encode("utf-8"))
    print(f"{kept} lines written to {args.out}")

    with_data = [capture for capture in captures if capture[2]]
    if not with_data:
        print(f"no capture has data lines: {args.extra_out} left as it is")
        return
    extra = [EXTRA_HEADER]
    count = 0
    for is64, entries, data, names in with_data:
        flag = "true" if is64 else "false"
        for address, hex_bytes in data:
            extra.append(f'CE_EXTRA_DATA({flag}, 0x{address:X}ull, "{hex_bytes}")\n')
        for value, name in names:
            extra.append(f"CE_EXTRA_NAME({flag}, 0x{value:X}ull, {quote(name)})\n")
        for _kind, address, raw, text, value in entries:
            extra.append(f"CE_EXTRA_LINE({flag}, 0x{address:X}ull, \"{raw}\", {quote(text)}, {quote(value)})\n")
            count += 1
    args.extra_out.write_bytes("".join(extra).encode("utf-8"))
    print(f"{count} extra lines written to {args.extra_out}")


if __name__ == "__main__":
    main()
