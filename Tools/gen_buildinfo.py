import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*arguments):
    environment = dict(os.environ, GIT_OPTIONAL_LOCKS="0")
    try:
        result = subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, text=True, timeout=20, env=environment)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def main():
    if len(sys.argv) != 2:
        print("usage: gen_buildinfo.py <output header>", file=sys.stderr)
        return 2
    output = sys.argv[1]
    commit = git("rev-parse", "--short=7", "HEAD") or ""
    changes = git("status", "--porcelain", "--untracked-files=no") if commit else None
    dirty = 1 if changes else 0
    text = ("#pragma once\n\n"
            f"#define MANIFOLD_BUILD_COMMIT \"{commit}\"\n"
            f"#define MANIFOLD_BUILD_DIRTY {dirty}\n")
    try:
        with open(output, encoding="utf-8") as file:
            if file.read() == text:
                return 0
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
