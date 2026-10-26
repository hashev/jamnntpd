"""Validate traceability, selectors and evidence without promoting planned tests to pass."""
import argparse
import ast
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
STATES = {'planned', 'implemented', 'passed', 'failed', 'blocked', 'not-applicable'}


def inventory(root):
    features = {'command:' + key for key in json.loads(
        (root / 'tests/fixtures/protocol/contracts.json').read_text())}
    features.add('utility:makechs')
    config = (root / 'src/core/config.cpp').read_text()
    features.update('option:' + name for name in re.findall(r'\.name = "([a-z_]+)"', config))
    features.update('option:' + name for name in ('port', 'max', 'origin', 'guestsuffix',
                                                'conversion_errors', 'config', 'create',
                                                'help', 'smartquote'))
    features.update('option:def_' + name for name in
                    ('flowed', 'showto', 'nonbsp', 'delssq', 'addcr'))
    features.update('alias:' + short for short in re.findall(r'std::pair\{"([a-z])",', config))
    for spec in sorted((root / 'openspec/specs').glob('*/spec.md')):
        features.update('requirement:' + spec.parent.name + '/' + name for name in
                        re.findall(r'^### Requirement: (.+)$', spec.read_text(), re.M))
    return features


def selectors(root):
    result = set()
    for source in sorted((root / 'tests').rglob('*.py')):
        tree = ast.parse(source.read_text())
        for node in tree.body:
            if isinstance(node, ast.ClassDef):
                for method in node.body:
                    if isinstance(method, ast.FunctionDef) and method.name.startswith('test_'):
                        result.add(f'{source.relative_to(root).as_posix()}::{node.name}.{method.name}')
    for source in sorted((root / 'tests/unit').glob('*.cpp')):
        for match in re.finditer(r'TEST_CASE\s*\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)', source.read_text()):
            name = ''.join(json.loads(part) for part in
                           re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
            result.add(f'{source.relative_to(root).as_posix()}::{name}')
    return result


def validate(data, root=ROOT, final=False):
    errors, ids, covered = [], set(), set()
    available = selectors(root)
    if not isinstance(data, dict) or data.get('version') != 1:
        return ['Unsupported matrix schema']
    rows = data.get('scenarios')
    if not isinstance(rows, list) or not rows:
        return ['Matrix must contain scenarios']
    for row in rows:
        if not isinstance(row, dict):
            errors.append('Scenario must be an object')
            continue
        ident = row.get('id')
        if not isinstance(ident, str) or not ident or ident in ids:
            errors.append(f'Duplicate or invalid ID: {ident}')
        ids.add(str(ident))
        for field in ('variant', 'preconditions', 'input', 'expected', 'oracle', 'level'):
            if not isinstance(row.get(field), str) or not row[field].strip():
                errors.append(f'{ident}: missing {field}')
        for field in ('features', 'selectors', 'platforms'):
            if not isinstance(row.get(field), list) or not row[field] or not all(
                    isinstance(item, str) and item for item in row[field]):
                errors.append(f'{ident}: invalid {field}')
        covered.update(item for item in row.get('features', []) if isinstance(item, str))
        for selector in row.get('selectors', []):
            if selector not in available:
                errors.append(f'{ident}: unknown selector {selector}')
        status = row.get('status')
        if status not in STATES:
            errors.append(f'{ident}: invalid status {status}')
        if not isinstance(row.get('mandatory'), bool):
            errors.append(f'{ident}: mandatory must be boolean')
        if status == 'not-applicable' and (row.get('mandatory') or not row.get('reason')):
            errors.append(f'{ident}: mandatory skip or missing applicability reason')
        if final and row.get('mandatory') and status != 'passed':
            errors.append(f'{ident}: mandatory scenario has not passed')
        evidence = row.get('evidence', [])
        if not isinstance(evidence, list):
            errors.append(f'{ident}: invalid evidence')
            evidence = []
        if status == 'passed':
            platforms = set()
            for item in evidence:
                if not isinstance(item, dict):
                    errors.append(f'{ident}: invalid evidence record')
                    continue
                platforms.add(item.get('platform'))
                revision = item.get('revision', '')
                artifact = item.get('artifact', '')
                if not re.fullmatch(r'[0-9a-f]{40}', revision) or revision != data.get('revision'):
                    errors.append(f'{ident}: evidence revision mismatch')
                path = (root / artifact).resolve() if isinstance(artifact, str) else root
                if not artifact or not path.is_relative_to(root.resolve()) or not path.is_file():
                    errors.append(f'{ident}: evidence artifact missing or outside repository')
                if item.get('result') != 'passed':
                    errors.append(f'{ident}: evidence is not a pass')
                if artifact and path.is_file() and path.is_relative_to(root.resolve()):
                    try:
                        proof = json.loads(path.read_text())
                        if (proof.get('revision') != revision or proof.get('status') != 'passed' or
                            proof.get('platform_key') != item.get('platform')):
                            errors.append(f'{ident}: evidence content does not prove this result')
                        for selector in row.get('selectors', []):
                            filename, _, caption = selector.partition('::')
                            suite = 'unit' if filename.endswith('.cpp') else ('makechs-cli' if filename.endswith('test_makechs.py') else 'server-e2e')
                            case = caption if suite == 'unit' else caption.rsplit('.', 1)[-1]
                            if proof.get('case_results', {}).get(suite, {}).get(case) != 'passed':
                                errors.append(f'{ident}: selector has no executed passing evidence: {selector}')
                    except (ValueError, OSError, AttributeError):
                        errors.append(f'{ident}: invalid evidence contents')
            if not set(row.get('platforms', [])) <= platforms:
                errors.append(f'{ident}: passed without evidence for every platform')
    missing = inventory(root) - covered
    if missing:
        errors.append('Unmapped features: ' + ', '.join(sorted(missing)))
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--matrix', type=Path, default=ROOT / 'tests/fixtures/acceptance-matrix.json')
    parser.add_argument('--final', action='store_true')
    args = parser.parse_args()
    errors = validate(json.loads(args.matrix.read_text()), final=args.final)
    if errors:
        parser.exit(1, '\n'.join(errors) + '\n')
    print('Acceptance matrix is valid' + ('; all mandatory scenarios passed' if args.final else
                                         '; this is structural validation, not acceptance'))


if __name__ == '__main__':
    main()
