"""Check all owned C++ source, public headers and unit/probe files."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--formatter', default='clang-format')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
files = sorted(path for folder in ('src', 'tests/unit', 'tests/integration')
               for path in (root/folder).rglob('*') if path.suffix in ('.cpp', '.hpp'))
subprocess.run([args.formatter, '--dry-run', '--Werror', *map(str, files)], check=True)
print(f'{len(files)} owned C++ files pass formatting')
