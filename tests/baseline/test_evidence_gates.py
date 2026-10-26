import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'tools'/(name+'.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


EXTENDED = load('check_extended')
COLLECT = load('collect_evidence')
SHA = 'a'*40


def native_reports():
    reports = []
    for platform in ['linux', 'macos', 'windows']:
        common = {'platform_key': platform, 'revision': SHA, 'acceptance_profile': True,
                  'source_dirty': False, 'status': 'passed'}
        resource = {'native_handles': 10, 'rss_bytes': 1000}
        reports.append({**common, 'mode': 'load', 'messages_per_base': 10000,
            'phases': [{'clients': clients, 'active_seconds': 600,
                        'resources': [resource], 'operations': {'post:240': 1}} for clients in [1, 5, 32]],
            'resources_before': dict(resource), 'resources_after_load': dict(resource),
            'resources_after_100_cycles': dict(resource), 'external_lock_iterations': 1,
            'shutdown_seconds': 1})
        reports.append({**common, 'mode': 'idle', 'idle_seconds': 300})
    return reports


class EvidenceGates(unittest.TestCase):
    def test_all_native_profiles_are_required(self):
        self.assertEqual(EXTENDED.validate(native_reports(), SHA), [])
        self.assertTrue(EXTENDED.validate(native_reports()[:-1], SHA))

    def test_dirty_stale_smoke_short_or_leaking_run_cannot_pass(self):
        mutations = [('source_dirty', True), ('acceptance_profile', False),
                     ('revision', 'b'*40), ('status', 'failed'), ('messages_per_base', 50),
                     ('shutdown_seconds', 8), ('external_lock_iterations', 0), ('phases', [])]
        for key, value in mutations:
            with self.subTest(key=key):
                reports = copy.deepcopy(native_reports())
                reports[0][key] = value
                self.assertTrue(EXTENDED.validate(reports, SHA))
        reports = copy.deepcopy(native_reports())
        reports[0]['resources_after_load']['native_handles'] = 100
        self.assertTrue(EXTENDED.validate(reports, SHA))

    def test_later_success_does_not_hide_failure_at_same_revision(self):
        reports = native_reports()
        failed = {**reports[0], 'status': 'failed'}
        self.assertTrue(EXTENDED.validate([failed, *reports], SHA))

    def test_wrong_idle_timeout_is_rejected(self):
        reports = native_reports()
        reports[1]['idle_seconds'] = 1
        self.assertTrue(EXTENDED.validate(reports, SHA))

    def test_junit_reports_failure_and_skip_without_promoting_them(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'junit.xml'
            root = ET.Element('testsuite')
            ET.SubElement(root, 'testcase', name='good')
            ET.SubElement(ET.SubElement(root, 'testcase', name='bad'), 'failure')
            ET.SubElement(ET.SubElement(root, 'testcase', name='absent'), 'skipped')
            ET.SubElement(root, 'testcase', name='bad')
            ET.ElementTree(root).write(path)
            self.assertEqual(COLLECT.tests(path), {'good': 'passed', 'bad': 'failed', 'absent': 'skipped'})
