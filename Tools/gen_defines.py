#!/usr/bin/env python3
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = r"C:\Program Files\Cheat Engine 7.5\defines.lua"
OUT = os.path.join(ROOT, "Source", "Lua", "Compat", "Defines.lua")


def evaluate(expression, known):
    expression = expression.strip().rstrip(";").strip()
    expression = re.sub(r"--.*$", "", expression).strip()
    python = expression.replace("~", "^") if "~=" not in expression else None
    if python is None:
        return None
    try:
        return int(eval(python, {"__builtins__": {}}, dict(known)))
    except Exception:
        return None


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    known = {}
    order = []
    for raw in open(source, encoding="latin-1"):
        line = raw.strip()
        if not line or line.startswith("--"):
            continue
        guarded = re.match(r"^if\s+not\s+([A-Za-z_]\w*)\s+then\s+\1\s*=\s*(.+?)\s+end\s*$", line)
        if guarded:
            name, expression = guarded.group(1), guarded.group(2)
        else:
            plain = re.match(r"^([A-Za-z_]\w*)\s*=\s*(.+)$", line)
            if not plain:
                continue
            name, expression = plain.group(1), plain.group(2)
        value = evaluate(expression, known)
        if value is None:
            continue
        if name not in known:
            order.append(name)
        known[name] = value
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as file:
        file.write("local defines = {\n")
        for name in order:
            file.write(f"  {name} = {known[name]},\n")
        file.write("}\nfor name, value in pairs(defines) do\n  if rawget(_G, name) == nil then\n    rawset(_G, name, value)\n  end\nend\n")
    print(len(order), "constants written to", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
