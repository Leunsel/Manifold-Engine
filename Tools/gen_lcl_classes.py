#!/usr/bin/env python3
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = r"C:\Program Files\Cheat Engine 7.5\celua.txt"
OUT = os.path.join(ROOT, "Source", "Lua", "Compat", "Classes.lua")

HEADER = re.compile(r"^\s*([A-Za-z_]\w*)\s+[Cc]lass\s*:?\s*\((?:Inheritance\s*:)?\s*([^)]*)\)")
METHOD = re.compile(r"^\s+([A-Za-z_]\w*)\s*\(")
INDEXED = re.compile(r"^\s+([A-Za-z_]\w*)\s*\[[^\]]*\]\s*:?\s*(.*)$")
PROPERTY = re.compile(r"^\s+([A-Za-z_]\w*)\s*:\s*(.*)$")


def normalize(kind):
    kind = kind.strip()
    kind = re.split(r"\s+-\s+|\s-$", kind)[0].strip()
    token = re.split(r"[\s/,(\-.]", kind, maxsplit=1)[0].strip()
    return token.lower() if token else "unknown"


def parse(path):
    classes = {}
    current = None
    for raw in open(path, encoding="latin-1"):
        line = raw.rstrip("\n").replace("\t", "    ")
        header = HEADER.match(line)
        if header:
            name = header.group(1).lower()
            chain = [part.strip().lower() for part in re.split(r"->|>", header.group(2)) if part.strip()]
            if chain and chain[0] == name:
                chain = chain[1:]
            current = classes.setdefault(name, {"parent": chain[0] if chain else None, "props": {}, "indexed": {}, "methods": set()})
            continue
        if current is None:
            continue
        if line and not line.startswith(" "):
            continue
        method = METHOD.match(line)
        if method:
            current["methods"].add(method.group(1).lower())
            continue
        indexed = INDEXED.match(line)
        if indexed:
            current["indexed"].setdefault(indexed.group(1).lower(), normalize(indexed.group(2)))
            continue
        prop = PROPERTY.match(line)
        if prop and prop.group(1).lower() not in ("properties", "methods", "events", "note", "example"):
            current["props"].setdefault(prop.group(1).lower(), normalize(prop.group(2)))
    return classes


def lua_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    classes = parse(source)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as file:
        file.write("__ceClasses = {\n")
        for name in sorted(classes):
            data = classes[name]
            file.write(f"  [{lua_string(name)}] = {{\n")
            file.write(f"    parent = {lua_string(data['parent']) if data['parent'] else 'nil'},\n")
            file.write("    props = {")
            file.write(", ".join(f"[{lua_string(k)}] = {lua_string(v)}" for k, v in sorted(data["props"].items())))
            file.write("},\n    indexed = {")
            file.write(", ".join(f"[{lua_string(k)}] = {lua_string(v)}" for k, v in sorted(data["indexed"].items())))
            file.write("},\n    methods = {")
            file.write(", ".join(f"[{lua_string(k)}] = true" for k in sorted(data["methods"])))
            file.write("},\n  },\n")
        file.write("}\n")
    print(len(classes), "classes written to", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
