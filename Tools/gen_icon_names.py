#!/usr/bin/env python3
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "Source", "Generated", "IconsFontAwesome5.h")
OUT = os.path.join(ROOT, "Source", "Generated", "IconNames.cpp")
PATTERN = re.compile(r'#define ICON_FA_([A-Z0-9_]+)\s+"((?:\\x[0-9a-fA-F]{2})+)"')


def collect():
    icons = {}
    with open(HEADER, encoding="utf-8") as handle:
        for line in handle:
            found = PATTERN.match(line.strip())
            if found:
                icons[found.group(1).lower().replace("_", "-")] = found.group(2)
    return sorted(icons.items())


def main():
    icons = collect()
    lines = [
        '#include "Core/IconNames.h"',
        "",
        "#include <algorithm>",
        "#include <array>",
        "",
        "namespace Manifold",
        "{",
        "    namespace",
        "    {",
        f"        constexpr std::array<IconName, {len(icons)}> kIcons = {{ {{",
    ]
    for name, glyph in icons:
        lines.append(f'            {{ "{name}", "{glyph}" }},')
    lines += [
        "        } };",
        "    }",
        "",
        "    std::span<const IconName> IconNames()",
        "    {",
        "        return kIcons;",
        "    }",
        "",
        "    std::string_view IconGlyph(std::string_view name)",
        "    {",
        "        const auto found = std::lower_bound(kIcons.begin(), kIcons.end(), name, [](const IconName& icon, std::string_view key) { return icon.Name < key; });",
        "        return found != kIcons.end() && found->Name == name ? found->Glyph : std::string_view();",
        "    }",
        "}",
        "",
    ]
    text = "\n".join(lines)
    if os.path.isfile(OUT):
        with open(OUT, encoding="utf-8") as handle:
            if handle.read() == text:
                return
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("wrote", os.path.relpath(OUT, ROOT), f"({len(icons)} icons)")


if __name__ == "__main__":
    main()
