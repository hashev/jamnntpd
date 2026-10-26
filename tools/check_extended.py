"""Validate native extended evidence without treating smoke runs or dirty builds as acceptance."""
import argparse
import json
from pathlib import Path
import re


def validate(reports, revision):
    errors, passed = [], set()
    for report in reports:
        if report.get('revision') != revision or not report.get('acceptance_profile'):
            continue
        key = (report.get('platform_key'), report.get('mode'))
        if key[0] not in ('linux', 'macos', 'windows') or key[1] not in ('load', 'idle'):
            errors.append(f'Invalid platform/mode: {key}')
            continue
        if report.get('status') != 'passed' or report.get('source_dirty') is not False:
            errors.append(f'Failed or dirty acceptance report: {key}')
            continue
        if key[1] == 'idle':
            if not 295 <= report.get('idle_seconds', 0) <= 315:
                errors.append(f'Invalid idle duration: {key}')
                continue
        else:
            phases = report.get('phases', [])
            if (report.get('messages_per_base') != 10000 or
                [phase.get('clients') for phase in phases] != [1, 5, 32] or
                any(phase.get('active_seconds', 0) < 600 or not phase.get('resources') or
                    not phase.get('operations') for phase in phases) or
                report.get('external_lock_iterations', 0) < 1 or
                report.get('shutdown_seconds', 99) >= 8):
                errors.append(f'Incomplete load profile: {key}')
                continue
            before, after = report.get('resources_before', {}), report.get('resources_after_load', {})
            cycled = report.get('resources_after_100_cycles', {})
            if (not before.get('native_handles') or not before.get('rss_bytes') or
                not after.get('rss_bytes') or
                after.get('native_handles', 10**9) > before['native_handles'] or
                cycled.get('native_handles', 10**9) > before['native_handles']):
                errors.append(f'Resource evidence missing or leaking: {key}')
                continue
        passed.add(key)
    expected = {(platform, mode) for platform in ('linux', 'macos', 'windows') for mode in ('load', 'idle')}
    for key in sorted(expected-passed):
        errors.append(f'Missing accepted native evidence: {key}')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--revision', required=True)
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{40}', args.revision):
        parser.error('Revision must be a complete commit SHA')
    reports = [json.loads(path.read_text()) for path in args.artifacts.rglob('report.json')]
    errors = validate(reports, args.revision)
    if errors:
        parser.exit(1, '\n'.join(errors)+'\n')
    print('Load and real-idle acceptance passed on Linux, macOS and Windows')


if __name__ == '__main__':
    main()
