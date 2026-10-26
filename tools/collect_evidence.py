"""Record an exact revision, toolchain/dependency versions and required CTest results."""
import argparse
import json
from pathlib import Path
import platform
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

REQUIRED = {'server-stress', 'server-e2e', 'server-generated', 'server-load-smoke',
            'lifecycle-native', 'storage-process-lock', 'makechs-cli', 'unit',
            'storage-quarantine', 'storage-quarantine-reopen', 'storage-quarantine-resize',
            'timezone-native', 'baseline'}


def tests(path):
    tree = ET.parse(path)
    found = {}
    for item in tree.iter('testcase'):
        status = ('failed' if item.find('failure') is not None or item.find('error') is not None
                  else 'skipped' if item.find('skipped') is not None else 'passed')
        name = item.get('name')
        if status == 'failed' or found.get(name) not in ('failed', 'skipped'):
            found[name] = status
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--junit', default='current-junit.xml')
    parser.add_argument('--require-vcpkg', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    compiler = {}
    for path in build.glob('CMakeFiles/*/CMakeCXXCompiler.cmake'):
        for key in ('CMAKE_CXX_COMPILER', 'CMAKE_CXX_COMPILER_ID', 'CMAKE_CXX_COMPILER_VERSION'):
            match = re.search(r'set\('+key+r' "([^"]+)"', path.read_text())
            if match:
                compiler[key] = match[1]
    packages = []
    caches = (build/'CMakeCache.txt').read_text() if (build/'CMakeCache.txt').exists() else ''
    installed = re.search(r'^VCPKG_INSTALLED_DIR:[^=]+=(.+)$', caches, re.M)
    package_file = Path(installed[1])/'vcpkg/status' if installed else build/'vcpkg_installed/vcpkg/status'
    if package_file.exists():
        for stanza in package_file.read_text().split('\n\n'):
            fields = dict(line.split(': ', 1) for line in stanza.splitlines() if ': ' in line)
            if 'Package' in fields:
                packages.append(fields)
    junit = build/args.junit
    results = tests(junit) if junit.exists() else {}
    required = set(REQUIRED)
    if any(path.name == 'report.json' and json.loads(path.read_text()).get('acceptance_profile')
           for path in (build/'acceptance-extended').rglob('report.json')):
        required.update(('server-load-extended', 'server-idle-extended'))
    case_results = {}
    for suite in ('unit', 'server-e2e', 'makechs-cli'):
        case_file = build/(suite+'-junit.xml')
        case_results[suite] = tests(case_file) if case_file.exists() else {}
    for suite in ('storage-quarantine', 'storage-quarantine-reopen', 'storage-quarantine-resize', 'timezone-native'):
        case_file = build/(suite+'-junit.xml')
        if case_file.exists():
            case_results['unit'].update(tests(case_file))
    missing_case_reports = sorted(suite for suite, cases in case_results.items() if not cases)
    missing = sorted(required-results.keys())
    failed = sorted(name for name, status in results.items() if status != 'passed')
    status = 'passed' if not missing and not failed and not missing_case_reports and compiler and (packages or not args.require_vcpkg) else 'failed'
    evidence = {'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                'cmake': subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0],
                'profile': args.profile, 'platform': platform.platform(),
                'platform_key': 'windows' if sys.platform == 'win32' else ('macos' if sys.platform == 'darwin' else 'linux'),
                'python': sys.version,
                'compiler': compiler, 'vcpkg_manifest': json.loads((root/'vcpkg.json').read_text()),
                'installed_packages': packages, 'ctest': results, 'case_results': case_results, 'missing_case_reports': missing_case_reports, 'missing': missing,
                'failed': failed, 'status': status}
    (build/'source-evidence.json').write_text(json.dumps(evidence, indent=2)+'\n')
    print(f'{args.profile}: {status}; {len(results)} CTest groups, {len(packages)} package records')
    return 0 if status == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
