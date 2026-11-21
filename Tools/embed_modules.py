#!/usr/bin/env python3
import hashlib
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "Autorun")
OUT = os.path.join(ROOT, "Source", "Generated", "ShippedModules.cpp")
HEAD = 16384


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(path):
    with open(path, "rb") as handle:
        return handle.read()


def header_name(path, fallback):
    text = read(path)[:HEAD].decode("utf-8", errors="replace").lstrip("﻿ \t\r\n")
    block = re.match(r"--\[(=*)\[(.*?)(?:\]\1\]|\Z)", text, re.S)
    if block:
        body = block.group(2)
    else:
        lines = []
        for line in text.splitlines():
            line = line.strip()
            if not line.startswith("--"):
                break
            lines.append(line.lstrip("-"))
        body = "\n".join(lines)
    for line in body.splitlines():
        found = re.match(r"\s*NAME\s*:\s*(.+?)\s*$", line)
        if found and not re.search(r'[\\/:*?"<>|]', found.group(1)):
            return found.group(1)
    return fallback


def is_lua(name):
    return name.lower().endswith(".lua")


def folder_hash(folder):
    files = []
    for base, _, names in os.walk(folder):
        for name in names:
            if is_lua(name):
                full = os.path.join(base, name)
                relative = os.path.relpath(full, folder).replace("\\", "/").lower()
                files.append((relative, sha(read(full))))
    files.sort()
    return sha("".join(f"{name}\n{digest}\n" for name, digest in files).encode("utf-8"))


def modules_in(folder, descend):
    found = []
    for name in sorted(os.listdir(folder)):
        full = os.path.join(folder, name)
        if os.path.isfile(full) and is_lua(name):
            found.append((header_name(full, os.path.splitext(name)[0]), sha(read(full))))
        elif os.path.isdir(full):
            init = os.path.join(full, "init.lua")
            if os.path.isfile(init):
                found.append((header_name(init, name), folder_hash(full)))
            elif descend:
                found += modules_in(full, False)
    return found


def shipped():
    found = []
    if not os.path.isdir(SOURCE):
        return found
    for folder in (SOURCE, os.path.join(SOURCE, "custom")):
        if os.path.isdir(folder):
            for name in sorted(os.listdir(folder)):
                full = os.path.join(folder, name)
                if os.path.isfile(full) and is_lua(name):
                    found.append((header_name(full, os.path.splitext(name)[0]), sha(read(full))))
    modules = os.path.join(SOURCE, "modules")
    if os.path.isdir(modules):
        found += modules_in(modules, True)
    return sorted((name.strip().lower(), digest) for name, digest in found)


def main():
    entries = shipped()
    lines = [
        '#include "Lua/AutorunModules.h"',
        "",
        "#include <array>",
        "",
        "namespace Manifold::Autorun",
        "{",
        "    namespace",
        "    {",
        f"        constexpr std::array<ShippedModule, {max(len(entries), 1)}> kShipped = {{",
    ]
    for name, digest in entries or [("", "")]:
        lines.append(f'            ShippedModule{{ "{name}", "{digest}" }},')
    lines += [
        "        };",
        "    }",
        "",
        "    std::span<const ShippedModule> ShippedModules()",
        "    {",
        f"        return std::span<const ShippedModule>(kShipped.data(), {len(entries)});",
        "    }",
        "}",
        "",
    ]
    text = "\n".join(lines)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if os.path.isfile(OUT) and open(OUT, encoding="utf-8").read() == text:
        return
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


if __name__ == "__main__":
    main()
