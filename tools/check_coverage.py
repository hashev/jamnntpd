"""Gate combined LLVM source coverage; never exclude own error paths or headers."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--native-windows-evidence', action='store_true',
                    help='Require native Windows adapters in addition to aggregate coverage gates')
parser.add_argument('--llvm-cov', default='llvm-cov')
parser.add_argument('--llvm-profdata', default='llvm-profdata')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
profiles = sorted(build.glob('current-*.profraw'))
if not profiles:
    sys.exit('No current instrumented profiles; run CTest with LLVM_PROFILE_FILE=current-%p-%m.profraw')
merged = build / 'current.profdata'
subprocess.run([args.llvm_profdata, 'merge', '-sparse', *map(str, profiles), '-o', str(merged)], check=True)
objects = [build / name for name in ['jam_unit', 'jamnntpd', 'makechs', 'jam_storage_probe', 'jam_lifecycle_probe']]
if sys.platform == 'win32':
    objects = [obj.with_suffix('.exe') if obj.with_suffix('.exe').exists()
               else obj.parent / 'Debug' / (obj.name + '.exe') for obj in objects]
if args.native_windows_evidence and sys.platform != 'win32':
    sys.exit('Native Windows evidence must run on Windows')
command = [args.llvm_cov, 'export', str(objects[0]), '-instr-profile=' + str(merged)]
for obj in objects[1:]:
    command += ['-object', str(obj)]
report = json.loads(subprocess.check_output(command))
(build / 'current-coverage.json').write_text(json.dumps(report))
files = [entry for data in report['data'] for entry in data['files']
         if Path(entry['filename']).is_relative_to(root / 'src')]
expected = {path.resolve() for path in (root / 'src').rglob('*.cpp')}
represented = {Path(entry['filename']).resolve() for entry in files}
missing = expected - represented
if missing:
    sys.exit('Missing production translation units: ' + ', '.join(map(str, sorted(missing))))
totals = {metric: {'count': 0, 'covered': 0} for metric in ['lines', 'branches']}
for entry in sorted(files, key=lambda item: item['filename']):
    summary = entry['summary']
    print(Path(entry['filename']).relative_to(root),
          'lines=%.2f%% branches=%.2f%%' % (summary['lines']['percent'], summary['branches']['percent']))
    for metric in totals:
        for key in totals[metric]:
            totals[metric][key] += summary[metric][key]
passed = True
for metric, threshold in [('lines', 90), ('branches', 85)]:
    total = totals[metric]
    percent = 100 * total['covered'] / total['count']
    total['percent'] = percent
    print(f"{metric}: {total['covered']}/{total['count']} = {percent:.2f}% (required {threshold}%)")
    passed &= percent >= threshold
(build / 'current-summary.json').write_text(json.dumps(totals, indent=2))
if args.native_windows_evidence:
    adapters = {'network.cpp', 'storage.cpp', 'signals.cpp'}
    covered = {Path(entry['filename']).name for entry in files
               if entry['summary']['lines']['covered'] > 0}
    if not adapters <= covered:
        sys.exit('Missing native adapter execution')
    functions = [function for data in report['data'] for function in data['functions']]
    for name in ['console_control', 'native_lock_code', 'identify_open_file', '?create@NetworkRuntime']:
        if not any(name in function['name'] and function['count'] > 0 for function in functions):
            sys.exit('Missing native adapter function execution: ' + name)
sys.exit(0 if passed else 1)
