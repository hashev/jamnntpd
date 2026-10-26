"""Native lock interoperability; this does not exercise an NNTP server."""
import argparse
import contextlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


@contextlib.contextmanager
def byte_lock(stream):
    stream.seek(0)
    if os.name == 'nt':
        import msvcrt
        msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        try:
            yield
        finally:
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
    else:
        import fcntl
        fcntl.lockf(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB, 1, 0)
        try:
            yield
        finally:
            fcntl.lockf(stream.fileno(), fcntl.LOCK_UN, 1, 0)


class StorageLocks(unittest.TestCase):
    def test_companion_hardlink_alias_respects_lock_deadline_without_mutation(self):
        with tempfile.TemporaryDirectory(prefix='jam-alias-lock-') as temporary:
            base, alias = Path(temporary)/'base', Path(temporary)/'alias'
            before = {}
            for suffix in ['.jhr', '.jdt', '.jdx', '.jlr']:
                path = base.with_suffix(suffix)
                shutil.copyfile(OPTIONS.fixtures/('sample'+suffix), path)
                os.link(path, alias.with_suffix(suffix))
                before[suffix] = path.read_bytes()
            with base.with_suffix('.jhr').open('r+b', buffering=0) as header:
                with byte_lock(header):
                    start = time.monotonic()
                    result = subprocess.run([OPTIONS.probe, str(alias)], capture_output=True,
                                            text=True, timeout=4)
                    self.assertEqual(result.returncode, 3, result.stderr)
                    self.assertLess(time.monotonic()-start, 3)
                    self.assertIn('timeout', result.stderr)
            released = subprocess.run([OPTIONS.probe, str(alias)], capture_output=True,
                                      text=True, timeout=4)
            self.assertEqual(released.returncode, 0, released.stderr)
            for suffix, expected in before.items():
                self.assertEqual(base.with_suffix(suffix).read_bytes(), expected)
                self.assertEqual(alias.with_suffix(suffix).read_bytes(), expected)

    def test_first_byte_lock_conflicts_with_native_process(self):
        with tempfile.TemporaryDirectory(prefix='jamnntpd-lock-') as temporary:
            base = Path(temporary) / 'base'
            for extension in ['.jhr', '.jdt', '.jdx', '.jlr']:
                shutil.copyfile(OPTIONS.fixtures / ('sample' + extension),
                                base.with_suffix(extension))
            before = base.with_suffix('.jhr').read_bytes()
            with base.with_suffix('.jhr').open('r+b', buffering=0) as stream:
                with byte_lock(stream):
                    blocked = subprocess.run([OPTIONS.probe, str(base)], capture_output=True,
                                             text=True, timeout=5, check=False)
                    self.assertEqual(blocked.returncode, 3, blocked.stderr)
                released = subprocess.run([OPTIONS.probe, str(base)], capture_output=True,
                                          text=True, timeout=5, check=False)
                self.assertEqual(released.returncode, 0, released.stderr)
                self.assertEqual(released.stdout.strip(), '2 1')
            self.assertEqual(base.with_suffix('.jhr').read_bytes(), before)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    parser.add_argument('--fixtures', required=True, type=Path)
    OPTIONS, remainder = parser.parse_known_args()
    unittest.main(argv=[__file__, *remainder])
