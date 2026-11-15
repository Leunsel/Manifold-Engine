#!/usr/bin/env python3
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "Templates")
OUT = os.path.join(ROOT, "Source", "Generated", "BuiltinTemplates.cpp")
CHUNK = 12000
DELIMITER = "MTPL"
EXTENSIONS = (".cea", ".lua")


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
    return parts or [""]


def collect():
    files = []
    for base, dirs, names in os.walk(SOURCE):
        dirs.sort()
        for name in sorted(names):
            if name.lower().endswith(EXTENSIONS):
                full = os.path.join(base, name)
                files.append(os.path.relpath(full, SOURCE).replace("\\", "/"))
    return files


def main():
    files = collect()
    lines = [
        '#include "Templates/BuiltinTemplates.h"',
        "",
        "#include <array>",
        "",
        "namespace Manifold::Templates",
        "{",
        "    namespace",
        "    {",
    ]
    for index, path in enumerate(files):
        with open(os.path.join(SOURCE, path), encoding="utf-8-sig") as handle:
            text = handle.read().replace("\r\n", "\n")
        if f"){DELIMITER}\"" in text:
            raise SystemExit(f"{path} contains the raw string delimiter")
        parts = chunks(text)
        lines.append(f"        constexpr std::array<std::string_view, {len(parts)}> kFile{index} = {{")
        for part in parts:
            lines.append(f'            std::string_view(R"{DELIMITER}({part}){DELIMITER}"),')
        lines.append("        };")
    lines.append("")
    lines.append(f"        const std::array<BuiltinFile, {len(files)}> kFiles = {{")
    for index, path in enumerate(files):
        lines.append(f'            BuiltinFile{{ "{path}", std::span<const std::string_view>(kFile{index}) }},')
    lines += [
        "        };",
        "    }",
        "",
        "    std::string BuiltinFile::Text() const",
        "    {",
        "        std::string text;",
        "        for (const std::string_view chunk : Chunks)",
        "            text.append(chunk);",
        "        return text;",
        "    }",
        "",
        "    std::span<const BuiltinFile> BuiltinFiles()",
        "    {",
        "        return kFiles;",
        "    }",
        "}",
        "",
    ]
    content = "\n".join(lines)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if os.path.isfile(OUT) and open(OUT, encoding="utf-8").read() == content:
        print("BuiltinTemplates.cpp is up to date")
        return
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)
    print(len(files), "template files embedded into", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
