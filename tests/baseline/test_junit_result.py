import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

SPEC = importlib.util.spec_from_file_location('junit_result', Path(__file__).resolve().parents[1]/
                                             'integration/junit_result.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PerMethodEvidence(unittest.TestCase):
    def test_failure_and_skip_are_preserved_without_recording_sensitive_assertion_text(self):
        class Examples(unittest.TestCase):
            def test_good(self):
                self.assertTrue(True)
            def test_subtests(self):
                for value in [False, True]:
                    with self.subTest(value=value):
                        self.assertTrue(value, 'sensitive-payload')
            @unittest.skip('Native-only mechanism')
            def test_skipped(self):
                pass
        result = unittest.TextTestRunner(stream=io.StringIO(), resultclass=MODULE.JUnitResult).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(Examples))
        self.assertFalse(result.wasSuccessful())
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'result.xml'
            result.write(path)
            records = {node.get('name'): node for node in ET.parse(path).iter('testcase')}
            self.assertIsNone(records['test_good'].find('failure'))
            self.assertIsNotNone(records['test_subtests'].find('failure'))
            self.assertIsNotNone(records['test_skipped'].find('skipped'))
            self.assertNotIn('sensitive-payload', path.read_text())
