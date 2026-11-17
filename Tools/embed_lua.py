#!/usr/bin/env python3
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "Source", "Lua", "Compat")
MANIFOLD = os.path.join(ROOT, "Source", "Lua", "Manifold")
OUT = os.path.join(ROOT, "Source", "Generated", "LuaCompatScripts.cpp")
ORDER = ("Defines.lua", "Classes.lua", "Lcl.lua")
CHUNK = 12000
DELIMITER = "MLUA"


def chunks(text):
    parts = []
    while text:
        if len(text) <= CHUNK:
            parts.append(text)
            break
        cut = text.rfind("\n", 0, CHUNK)
        if cut <= 0:
            cut = CHUNK
        parts.append(text[:cut + 1])
        text = text[cut + 1:]
    return parts


def script_names(folder, order=()):
    if not os.path.isdir(folder):
        return []
    names = [name for name in order if os.path.isfile(os.path.join(folder, name))]
    names += sorted(name for name in os.listdir(folder) if name.endswith(".lua") and name not in order)
    return names


def embed(lines, folder, names, prefix):
    entries = []
    for index, name in enumerate(names):
        text = open(os.path.join(folder, name), encoding="utf-8").read().replace("\r\n", "\n")
        if f"){DELIMITER}\"" in text:
            raise SystemExit(f"{name} contains the raw string delimiter")
        parts = chunks(text)
        lines.append(f"        constexpr std::array<std::string_view, {len(parts)}> k{prefix}{index} = {{")
        for part in parts:
            lines.append(f'            std::string_view(R"{DELIMITER}({part}){DELIMITER}"),')
        lines.append("        };")
        entries.append((name, index))
    return entries


def table(lines, name, prefix, entries):
    lines.append("")
    lines.append(f"        const std::array<CompatScript, {len(entries)}> {name} = {{")
    for script, index in entries:
        lines.append(f'            CompatScript{{ "{script}", std::span<const std::string_view>(k{prefix}{index}) }},')
    lines.append("        };")


def main():
    lines = [
        '#include "Lua/LuaCompatScripts.h"',
        "",
        "#include <array>",
        "",
        "namespace Manifold::Lua",
        "{",
        "    namespace",
        "    {",
    ]
    compat = embed(lines, SOURCE, script_names(SOURCE, ORDER), "Script")
    manifold = embed(lines, MANIFOLD, script_names(MANIFOLD), "Manifold")
    table(lines, "kScripts", "Script", compat)
    table(lines, "kManifoldScripts", "Manifold", manifold)
    lines += [
        "    }",
        "",
        "    std::string CompatScript::Text() const",
        "    {",
        "        std::string text;",
        "        for (std::string_view chunk : Chunks)",
        "            text += chunk;",
        "        return text;",
        "    }",
        "",
        "    std::span<const CompatScript> CompatScripts()",
        "    {",
        "        return kScripts;",
        "    }",
        "",
        "    std::span<const CompatScript> ManifoldScripts()",
        "    {",
        "        return kManifoldScripts;",
        "    }",
        "}",
        "",
    ]
    content = "\n".join(lines)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if os.path.isfile(OUT) and open(OUT, encoding="utf-8").read() == content:
        print("LuaCompatScripts.cpp is up to date")
        return
    with open(OUT, "w", encoding="utf-8", newline="\n") as file:
        file.write(content)
    print(len(compat) + len(manifold), "scripts embedded into", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
