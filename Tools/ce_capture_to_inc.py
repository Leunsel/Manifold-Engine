"""Turns Cheat Engine capture files into Tests/CheatEngineCapture.inc.

The capture files come from Tools/ce-capture.lua, run in Cheat Engine's Lua engine while CE is attached to a
64-bit or a 32-bit process. Only the test block goes into the include: the instructions the script wrote into
memory it allocated itself. The lines from the process's own code ("code+..." kinds) stay out, so no code of the
process ends up in the repository.

Lines where Cheat Engine's own decoder is wrong are left out as well, because Manifold does not copy those
faults (see issue #1): VEX/EVEX instructions, bytes CE cannot decode (empty text or "db"), 32-bit-only opcodes in
64-bit code, and the missing/doubled commas CE writes for some SSE forms.

Usage:
    python Tools/ce_capture_to_inc.py capture-64.txt capture-32.txt [--out Tests/CheatEngineCapture.inc]

The 64-bit capture comes first in the include, whatever the order on the command line. A capture is 64-bit when
one of its test-block addresses does not fit 32 bits.
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

TEST_KINDS = re.compile(r"^(jcc8|jcc32|setcc|cmovcc|misc|extra .+)$")
ONLY_32BIT = re.compile(r"\b(push|pop) (es|cs|ss|ds)\b|\bpopad\b|\bpushad\b|\baa[adms]\b|\bdaa\b|\bdas\b|\binto\b|\bjmp far\b|\(invalid\)")
COMMA_FAULT = re.compile(r",,|\b(?:xmm|mm)\d+[0-9A-F]{2}\b|[a-z]xmm\d")


def read_capture(path):
    entries = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split("\t")
        # Multi-line extra texts from the process's code continue on lines of their own; they never have a test kind.
        if len(parts) < 5 or not TEST_KINDS.match(parts[0]):
            continue
        kind, address, raw, _size, text = parts[:5]
        entries.append((kind, int(address, 16), raw.upper(), text))
    return entries


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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("captures", nargs="+", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent / "Tests" / "CheatEngineCapture.inc")
    args = parser.parse_args()

    captures = []
    for path in args.captures:
        entries = read_capture(path)
        if not entries:
            sys.exit(f"{path}: no test-block lines")
        captures.append((any(address > 0xFFFFFFFF for _, address, _, _ in entries), entries))
    captures.sort(key=lambda capture: not capture[0])

    out = [HEADER]
    kept = 0
    for is64, entries in captures:
        for _kind, address, raw, text in entries:
            if not keep(raw, text, is64):
                continue
            escaped = text.replace("\\", "\\\\").replace('"', '\\"')
            out.append(f'{{ {"true" if is64 else "false"}, 0x{address:X}ull, "{raw}", "{escaped}" }},\n')
            kept += 1
    args.out.write_bytes("".join(out).encode("utf-8"))
    print(f"{kept} lines written to {args.out}")


if __name__ == "__main__":
    main()
