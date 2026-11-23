"""Warns when an interface file uses more raw ImGui calls than Source/Layout/RawImGui.txt allows.

The layout system (Docs/UI-LAYOUT.md) is the way to draw text, buttons, fields and containers; RawImGui.txt holds,
per file, how many raw calls are left from before it. This runs before every build of the program and only warns;
the tests (suite ui-layout) fail on the same numbers. `Manifold-Engine.Tests.exe --ui-raw-baseline` lowers them after
a file moved to the layout components. `--init` writes the file from the tree as it is (only to start it over).
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "Source", "Layout", "RawImGui.txt")


def read_baseline():
    patterns, scopes, excluded, counts, header = [], [], [], {}, []
    with open(BASELINE, encoding="utf-8") as file:
        for raw in file:
            line = raw.rstrip("\n").rstrip("\r")
            if not line.strip() or line.startswith("#"):
                if not counts:
                    header.append(line)
                continue
            word, _, rest = line.partition(" ")
            if word == "pattern":
                patterns.append(rest)
            elif word == "scope":
                scopes.append(rest)
            elif word == "exclude":
                excluded.append(rest)
            else:
                name, _, number = line.rpartition(" ")
                counts[name] = int(number)
    return patterns, scopes, excluded, counts, header


def files_in(scopes, excluded):
    found = []
    for scope in scopes:
        base = os.path.join(ROOT, scope)
        for folder, _, names in os.walk(base):
            for name in names:
                if not name.endswith(".cpp") and not name.endswith(".h"):
                    continue
                relative = os.path.relpath(os.path.join(folder, name), ROOT).replace("\\", "/")
                if any(relative.startswith(prefix) for prefix in excluded):
                    continue
                found.append(relative)
    return sorted(set(found))


def count(path, patterns):
    total = 0
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as file:
        for line in file:
            stripped = line.lstrip()
            if stripped.startswith("//"):
                continue
            for pattern in patterns:
                total += line.count(pattern)
    return total


def main():
    patterns, scopes, excluded, counts, header = read_baseline()
    if "--init" in sys.argv[1:]:
        lines = list(header)
        for name in [f"pattern {p}" for p in patterns] + [f"scope {s}" for s in scopes] + [f"exclude {e}" for e in excluded]:
            lines.append(name)
        lines.append("")
        for path in files_in(scopes, excluded):
            used = count(path, patterns)
            if used:
                lines.append(f"{path} {used}")
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as file:
            file.write("\n".join(lines) + "\n")
        print("wrote", os.path.relpath(BASELINE, ROOT))
        return 0
    for path in files_in(scopes, excluded):
        used = count(path, patterns)
        allowed = counts.get(path, 0)
        if used > allowed:
            print(f"{path.replace('/', os.sep)} : warning ME0004 : {used} raw ImGui calls where {allowed} are allowed; "
                  "draw text, buttons, fields and containers with the Layout components (Docs/UI-LAYOUT.md)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
