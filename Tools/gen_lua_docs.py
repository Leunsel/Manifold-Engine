#!/usr/bin/env python3
"""Turns Source/Lua/Docs/CheatEngine.txt into Source/Generated/LuaDocs.cpp.

The text file lists Cheat Engine's Lua API (classes, functions, methods, properties) with
descriptions written for this project; its header explains the format.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "Source", "Lua", "Docs", "CheatEngine.txt")
OUT = os.path.join(ROOT, "Source", "Generated", "LuaDocs.cpp")

CLASS = re.compile(r"^class\s+([A-Za-z_]\w*)(?:\s+:\s+([A-Za-z_]\w*))?$")
CALL = re.compile(r"^(function|method)\s+(?:([A-Za-z_]\w*)\.)?([A-Za-z_]\w*)\((.*)\)$")
PROPERTY = re.compile(r"^property\s+([A-Za-z_]\w*)\.([A-Za-z_]\w*(?:\[\])?)(?:\s+:\s+(.+))?$")


def fail(path, number, message):
    sys.exit(f"{os.path.relpath(path, ROOT)}({number}): {message}")


def parse(path):
    classes, entries, seen = [], [], set()
    current = None
    for number, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            current = None if not line.strip() else current
            continue
        if line.startswith("    "):
            if current is None or current[3]:
                fail(path, number, "a description belongs right under its entry, on one line")
            text = line.strip()
            if " - " in text or not text.isascii():
                fail(path, number, 'descriptions are ASCII and never contain " - "')
            current[3] = text
            continue
        match = CLASS.match(line)
        if match:
            classes.append((match.group(1), match.group(2) or ""))
            current = None
            continue
        match = CALL.match(line)
        if match:
            kind, owner, name, params = match.groups()
            if (kind == "method") != bool(owner):
                fail(path, number, "a method has a class (method Class.name), a function has none")
            current = [owner or "", name, params.strip(), "", "Method" if kind == "method" else "Function", ""]
        else:
            match = PROPERTY.match(line)
            if not match:
                fail(path, number, f"not an entry: {line}")
            current = [match.group(1), match.group(2), "", "", "Property", (match.group(3) or "").strip()]
        key = (current[0].lower(), current[1].lower(), current[2].lower())
        if key in seen:
            fail(path, number, f"{line} is listed twice")
        seen.add(key)
        entries.append(current)
    return classes, entries


def entry_text(owner, name, params, text, kind, kind_type):
    if kind != "Property" or not kind_type:
        return text
    if kind_type.startswith("'") and text:
        sys.exit(f"{owner}.{name}: a property with a list of values has no description")
    return f"{kind_type} - {text}" if text else kind_type


def literal(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("?", "\\?") + '"'


def main():
    classes, entries = parse(SOURCE)
    with open(OUT, "w", encoding="utf-8", newline="\n") as out:
        out.write('#include "Lua/LuaDocs.h"\n\n')
        out.write("namespace Manifold::Lua\n{\n    namespace\n    {\n")
        out.write("        constexpr DocClass kClasses[] = {\n")
        for name, parent in classes:
            out.write(f"            {{ {literal(name)}, {literal(parent)} }},\n")
        out.write("        };\n\n        constexpr DocEntry kEntries[] = {\n")
        for owner, name, params, text, kind, kind_type in entries:
            out.write(f"            {{ {literal(owner)}, {literal(name)}, {literal(params)}, {literal(entry_text(owner, name, params, text, kind, kind_type))}, DocKind::{kind} }},\n")
        out.write("        };\n    }\n\n")
        out.write("    std::span<const DocClass> DocClasses()\n    {\n        return kClasses;\n    }\n\n")
        out.write("    std::span<const DocEntry> DocEntries()\n    {\n        return kEntries;\n    }\n}\n")
    print(len(classes), "classes and", len(entries), "entries written to", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
