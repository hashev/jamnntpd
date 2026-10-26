"""Self-tests for acceptance bookkeeping; no server state is modified."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('acceptance_validator', ROOT / 'tools/check_acceptance.py')
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class AcceptanceMatrix(unittest.TestCase):
    def setUp(self):
        self.matrix = json.loads((ROOT / 'tests/fixtures/acceptance-matrix.json').read_text())
        self.matrix = copy.deepcopy(self.matrix)
        for row in self.matrix['scenarios']:
            row.update(status='planned', evidence=[])

    def errors(self):
        return '\n'.join(VALIDATOR.validate(self.matrix))

    def test_structural_inventory_is_complete_but_not_final_acceptance(self):
        self.assertEqual(self.errors(), '')
        self.assertTrue(VALIDATOR.validate(self.matrix, final=True))
        self.assertEqual(len([x for x in VALIDATOR.inventory(ROOT)
                              if x.startswith('requirement:')]), 15)
        self.assertEqual(len([x for x in VALIDATOR.inventory(ROOT)
                              if x.startswith('command:')]), 18)

    def test_duplicate_ids_are_rejected(self):
        self.matrix['scenarios'].append(copy.deepcopy(self.matrix['scenarios'][0]))
        self.assertIn('Duplicate', self.errors())

    def test_unknown_selector_is_rejected(self):
        self.matrix['scenarios'][0]['selectors'] = ['tests/unit/none.cpp::not a test']
        self.assertIn('unknown selector', self.errors())

    def test_unmapped_feature_is_rejected(self):
        self.matrix['scenarios'].pop()
        self.assertIn('Unmapped features', self.errors())

    def test_mandatory_skip_is_rejected(self):
        self.matrix['scenarios'][0].update(status='not-applicable', reason='Cannot run here')
        self.assertIn('mandatory skip', self.errors())

    def test_pass_requires_every_platform_evidence(self):
        self.matrix['scenarios'][0]['status'] = 'passed'
        self.assertIn('passed without evidence', self.errors())

    def test_mismatched_revision_and_missing_artifact_are_rejected(self):
        self.matrix['revision'] = 'a' * 40
        self.matrix['scenarios'][0].update(status='passed', evidence=[
            {'platform': platform, 'revision': 'b' * 40,
             'artifact': 'does-not-exist', 'result': 'passed'}
            for platform in ('linux', 'macos', 'windows')])
        errors = self.errors()
        self.assertIn('revision mismatch', errors)
        self.assertIn('artifact missing', errors)

    def test_failed_result_and_outside_artifact_are_rejected(self):
        self.matrix['revision'] = 'a' * 40
        self.matrix['scenarios'][0].update(status='passed', evidence=[
            {'platform': platform, 'revision': 'a' * 40,
             'artifact': '../outside-evidence', 'result': 'failed'}
            for platform in ('linux', 'macos', 'windows')])
        errors = self.errors()
        self.assertIn('outside repository', errors)
        self.assertIn('not a pass', errors)

    def test_pass_requires_actual_matching_case_results_in_the_artifact(self):
        (ROOT/'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as folder:
            path = Path(folder)/'proof.json'
            row = self.matrix['scenarios'][0]
            caption = row['selectors'][0].split('::')[1]
            proof = {'status': 'passed', 'revision': 'a'*40, 'platform_key': 'macos',
                     'case_results': {'unit': {caption: 'passed'}}}
            path.write_text(json.dumps(proof))
            self.matrix['revision'] = 'a'*40
            row.update(status='passed', platforms=['macos'], evidence=[{
                'platform': 'macos', 'revision': 'a'*40, 'result': 'passed',
                'artifact': str(path.relative_to(ROOT))}])
            self.assertEqual(self.errors(), '')
            proof['case_results'] = {}
            path.write_text(json.dumps(proof))
            self.assertIn('no executed passing evidence', self.errors())

    def test_malformed_schema_and_scenario_fail_safely(self):
        self.assertTrue(VALIDATOR.validate([]))
        self.matrix['scenarios'] = ['not a scenario']
        self.assertIn('Scenario must be an object', self.errors())
