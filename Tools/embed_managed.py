#!/usr/bin/env python3
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(ROOT, "Managed", "ManifoldDumper", "ManifoldDumper.csproj")


def assembly_name():
    with open(PROJECT, encoding="utf-8") as handle:
        found = re.search(r"<AssemblyName>([^<]+)</AssemblyName>", handle.read())
    if not found:
        raise SystemExit(f"{PROJECT} names no assembly")
    return found.group(1)


IMAGE = os.path.join(ROOT, "Build", "Managed", assembly_name() + ".dll")
OUT = os.path.join(ROOT, "Source", "Generated", "ManagedDumperImage.cpp")
PER_LINE = 24


def render(data):
    lines = [
        '#include "Target/ManagedDumper.h"',
        "",
        "namespace Manifold::Target",
        "{",
        "    namespace",
        "    {",
    ]
    if data:
        lines.append(f"        alignas(16) constexpr std::uint8_t kImage[{len(data)}] = {{")
        for start in range(0, len(data), PER_LINE):
            chunk = data[start:start + PER_LINE]
            lines.append("            " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")
        lines.append("        };")
        size = "sizeof(kImage)"
    else:
        lines.append("        constexpr std::uint8_t kImage[1] = { 0 };")
        size = "0"
    lines += [
        "    }",
        "",
        "    std::span<const std::uint8_t> ManagedDumperImage()",
        "    {",
        f"        return std::span<const std::uint8_t>(kImage, {size});",
        "    }",
        "}",
        "",
    ]
    return "\n".join(lines)


def main(image=IMAGE, touch=False):
    data = b""
    if os.path.isfile(image):
        with open(image, "rb") as handle:
            data = handle.read()
    text = render(data)
    same = False
    if os.path.isfile(OUT):
        with open(OUT, encoding="utf-8") as handle:
            same = handle.read() == text
    if not same:
        os.makedirs(os.path.dirname(OUT), exist_ok=True)
        with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("wrote", os.path.relpath(OUT, ROOT), f"({len(data)} bytes)")
    if touch:
        folder = os.path.dirname(image)
        built = [os.path.join(folder, name) for name in os.listdir(folder) if name.lower().endswith(".dll")] if os.path.isdir(folder) else []
        for path in built + [OUT]:
            if os.path.isfile(path):
                os.utime(path)


if __name__ == "__main__":
    arguments = [a for a in sys.argv[1:] if a != "--touch"]
    main(arguments[0] if arguments else IMAGE, "--touch" in sys.argv[1:])
