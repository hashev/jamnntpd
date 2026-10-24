import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest


class MakeChsCli(unittest.TestCase):
    def run_cli(self, *arguments):
        return subprocess.run([OPTIONS.executable, *map(str, arguments)],
                              capture_output=True, timeout=10, check=False)

    def test_utf8_generation_and_errors(self):
        with tempfile.TemporaryDirectory(prefix='jamnntpd-map-') as temporary:
            mapping = Path(temporary) / 'source.map'
            mapping.write_text('0x8D 0x041D\n0xFF 0x1F600\n')
            generated = self.run_cli('CP866', 'UTF-8', mapping)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            self.assertIn(b'100000\n0\n2\nCP866\nUTF-8\n', generated.stdout)
            self.assertIn(b'\\xF0\t\\x9F\t\\x98\t\\x80', generated.stdout)
            self.assertTrue(generated.stdout.endswith(b'END\n'))
            bad = self.run_cli('SOURCE', 'DEST', mapping.with_name('missing.map'))
            self.assertNotEqual(bad.returncode, 0)
            self.assertEqual(bad.stdout, b'')
            self.assertIn(b'Usage: makechs', self.run_cli().stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True)
    OPTIONS, remainder = parser.parse_known_args()
    unittest.main(argv=[__file__, *remainder])
