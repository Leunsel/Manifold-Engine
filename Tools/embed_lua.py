#!/usr/bin/env python3
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "Source", "Lua", "Compat")
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


def main():
    names = [name for name in ORDER if os.path.isfile(os.path.join(SOURCE, name))]
    names += sorted(name for name in os.listdir(SOURCE) if name.endswith(".lua") and name not in ORDER)
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
    entries = []
    for index, name in enumerate(names):
        text = open(os.path.join(SOURCE, name), encoding="utf-8").read().replace("\r\n", "\n")
        if f"){DELIMITER}\"" in text:
            raise SystemExit(f"{name} contains the raw string delimiter")
        parts = chunks(text)
        lines.append(f"        constexpr std::array<std::string_view, {len(parts)}> kScript{index} = {{")
        for part in parts:
            lines.append(f'            std::string_view(R"{DELIMITER}({part}){DELIMITER}"),')
        lines.append("        };")
        entries.append((name, index))
    lines.append("")
    lines.append(f"        const std::array<CompatScript, {len(entries)}> kScripts = {{")
    for name, index in entries:
        lines.append(f'            CompatScript{{ "{name}", std::span<const std::string_view>(kScript{index}) }},')
    lines += [
        "        };",
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
    print(len(entries), "scripts embedded into", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
