#!/usr/bin/env python3
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = r"C:\Program Files\Cheat Engine 7.5\celua.txt"
OUT = os.path.join(ROOT, "Source", "Generated", "LuaDocs.cpp")

HEADER = re.compile(r"^([A-Za-z_]\w*)\s+[Cc]lass\s*:?\s*(?:\((?:\s*Inheritance\s*:)?\s*([^)]*)\))?\s*:?\s*$")
GLOBAL = re.compile(r"^([A-Za-z_][\w.]*)\s*\(")
MEMBER_CALL = re.compile(r"^(?: {1,3}|\t)([A-Za-z_]\w*)\s*\(")
MEMBER_FIELD = re.compile(r"^(?: {1,3}|\t)([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*:\s*(.*)$")
SECTION_WORDS = {"properties", "methods", "events", "note", "notes", "example", "examples", "remarks", "returns", "usage"}
MAX_TEXT = 220


def split_call(line, start):
    depth = 0
    for index in range(start, len(line)):
        if line[index] == "(":
            depth += 1
        elif line[index] == ")":
            depth -= 1
            if depth == 0:
                return line[start + 1:index], line[index + 1:]
    return line[start + 1:], ""


def clean_text(text):
    text = text.strip()
    while text[:1] in (":", "-"):
        text = text[1:].strip()
    text = re.sub(r"\s+", " ", text)
    if len(text) > MAX_TEXT:
        cut = text.rfind(" ", 0, MAX_TEXT)
        text = text[:cut if cut > 80 else MAX_TEXT].rstrip(" ,.;") + "..."
    return text


def clean_params(params):
    return re.sub(r"\s+", " ", params.strip())


def parse(path):
    classes = []
    known = set()
    entries = []
    seen = set()
    current = None
    for raw in open(path, encoding="latin-1"):
        line = raw.rstrip("\r\n")
        header = HEADER.match(line)
        if header:
            name = header.group(1)
            chain = [part.strip() for part in re.split(r"->|>", header.group(2) or "") if part.strip()]
            if chain and chain[0].lower() == name.lower():
                chain = chain[1:]
            current = name
            if name.lower() not in known:
                known.add(name.lower())
                classes.append((name, chain[0] if chain else ""))
            continue
        if not line.strip():
            continue
        if not line[0].isspace():
            match = GLOBAL.match(line)
            if match and "." not in match.group(1):
                params, rest = split_call(line, line.index("(", match.end(1)))
                add(entries, seen, "", match.group(1), clean_params(params), clean_text(rest), "Function")
            continue
        if current is None:
            continue
        call = MEMBER_CALL.match(line)
        if call:
            params, rest = split_call(line, line.index("(", call.end(1)))
            add(entries, seen, current, call.group(1), clean_params(params), clean_text(rest), "Method")
            continue
        field = MEMBER_FIELD.match(line)
        if field and field.group(1).lower() not in SECTION_WORDS:
            name = field.group(1) + ("[]" if field.group(2) else "")
            add(entries, seen, current, name, "", clean_text(field.group(3)), "Property")
    return classes, entries


def add(entries, seen, owner, name, params, text, kind):
    key = (owner.lower(), name.lower(), params.lower())
    if key in seen:
        return
    seen.add(key)
    entries.append((owner, name, params, text, kind))


def literal(text):
    text = text.encode("ascii", "replace").decode("ascii")
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("?", "\\?") + '"'


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    classes, entries = parse(source)
    with open(OUT, "w", encoding="utf-8", newline="\n") as out:
        out.write('#include "Lua/LuaDocs.h"\n\n')
        out.write("namespace Manifold::Lua\n{\n    namespace\n    {\n")
        out.write("        constexpr DocClass kClasses[] = {\n")
        for name, parent in classes:
            out.write(f"            {{ {literal(name)}, {literal(parent)} }},\n")
        out.write("        };\n\n        constexpr DocEntry kEntries[] = {\n")
        for owner, name, params, text, kind in entries:
            out.write(f"            {{ {literal(owner)}, {literal(name)}, {literal(params)}, {literal(text)}, DocKind::{kind} }},\n")
        out.write("        };\n    }\n\n")
        out.write("    std::span<const DocClass> DocClasses()\n    {\n        return kClasses;\n    }\n\n")
        out.write("    std::span<const DocEntry> DocEntries()\n    {\n        return kEntries;\n    }\n}\n")
    print(len(classes), "classes and", len(entries), "entries written to", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
