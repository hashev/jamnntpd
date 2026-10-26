import importlib.util
from pathlib import Path
import unittest
import subprocess
import sys
import tempfile

SPEC = importlib.util.spec_from_file_location('minimize', Path(__file__).resolve().parents[2]/
                                             'tools/minimize_case.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FailureMinimization(unittest.TestCase):
    def test_counterexample_is_reduced_without_losing_failure(self):
        source = b'padding CRASH more padding'
        predicate = lambda value: b'CRASH' in value
        self.assertEqual(MODULE.minimize(source, predicate), b'CRASH')
        self.assertEqual(MODULE.minimize(source, predicate), MODULE.minimize(source, predicate))

    def test_cli_preserves_the_replay_failure_exit_code(self):
        with tempfile.TemporaryDirectory() as folder:
            source, target = Path(folder)/'case.bin', Path(folder)/'min.bin'
            source.write_bytes(b'prefix CRASH suffix')
            command = [sys.executable, str(Path(__file__).resolve().parents[2]/'tools/minimize_case.py'),
                       '--input', str(source), '--output', str(target), '--', sys.executable, '-c',
                       "import os,pathlib,sys; sys.exit(7 if b'CRASH' in pathlib.Path(os.environ['JAM_REPLAY_FILE']).read_bytes() else 0)"]
            result = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(target.read_bytes(), b'CRASH')

    def test_non_reproducible_input_is_refused(self):
        with self.assertRaises(ValueError):
            MODULE.minimize(b'valid', lambda value: b'CRASH' in value)

    def test_empty_input_and_noncontiguous_predicates(self):
        self.assertEqual(MODULE.minimize(b'anything', lambda _: True), b'')
        value = MODULE.minimize(b'prefix A intervening B suffix',
                                lambda data: b'A' in data and b'B' in data)
        self.assertEqual(value, b'AB')
