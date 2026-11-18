import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FOLDERS = ['Source', 'Tests', 'Agent']
EXCLUDED = ['Source/Generated']
EXTENSIONS = 'c,cc,cpp,cxx,h,hh,hpp,hxx,inl'


def visual_studio_llvm():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if not vswhere.exists():
        return []
    found = subprocess.run([str(vswhere), '-all', '-products', '*', '-property', 'installationPath'], capture_output=True, text=True).stdout.split('\n')
    return [Path(line.strip()) / 'VC' / 'Tools' / 'Llvm' / folder for line in found if line.strip() for folder in ('x64/bin', 'bin')]


def find_tools():
    candidates = []
    if os.environ.get('CLANG_FORMAT'):
        candidates.append(Path(os.environ['CLANG_FORMAT']).parent)
    on_path = shutil.which('clang-format')
    if on_path:
        candidates.append(Path(on_path).parent)
    candidates += visual_studio_llvm()
    for folder in candidates:
        binary = folder / ('clang-format.exe' if os.name == 'nt' else 'clang-format')
        script = folder / 'git-clang-format'
        if binary.exists() and script.exists():
            return binary, script
    return None, None


def main():
    parser = argparse.ArgumentParser(description='Formats only the lines changed since a commit with the .clang-format of the repository. '
                                     'Without --apply it prints the changes it would make and touches no file, which is the safe choice '
                                     'while other sessions edit the same working tree.')
    parser.add_argument('files', nargs='*', help='files or folders to look at (default: Source, Tests and Agent, without Source/Generated)')
    parser.add_argument('--commit', default='HEAD', help='compare with this commit (default: HEAD)')
    parser.add_argument('--staged', action='store_true', help='look at the staged changes only')
    parser.add_argument('--apply', action='store_true', help='write the formatting into the files')
    arguments = parser.parse_args()

    binary, script = find_tools()
    if not binary:
        print('clang-format and git-clang-format were not found. Install the C++ Clang tools of Visual Studio, put clang-format on PATH, or set CLANG_FORMAT.', file=sys.stderr)
        return 2

    paths = arguments.files or FOLDERS
    paths = list(paths) + [f':(exclude){folder}' for folder in EXCLUDED]
    command = [sys.executable, str(script), '--binary', str(binary), '--extensions', EXTENSIONS]
    if arguments.staged:
        command.append('--staged')
    if not arguments.apply:
        command.append('--diff')
    command += [arguments.commit, '--'] + paths
    return subprocess.run(command, cwd=ROOT).returncode


if __name__ == '__main__':
    sys.exit(main())
