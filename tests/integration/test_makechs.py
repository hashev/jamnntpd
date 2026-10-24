import argparse
import os
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
            newline = b'\r\n' if os.name == 'nt' else b'\n'
            self.assertIn(newline.join([b'100000',b'0',b'2',b'CP866',b'UTF-8',b'']),generated.stdout)
            self.assertIn(b'\\xF0\t\\x9F\t\\x98\t\\x80', generated.stdout)
            self.assertTrue(generated.stdout.endswith(b'END'+newline))
            unicode_mapping = mapping.with_name('карта.map')
            unicode_mapping.write_bytes(mapping.read_bytes())
            self.assertEqual(self.run_cli('CP866','UTF-8',unicode_mapping).stdout,generated.stdout)
            bad = self.run_cli('SOURCE', 'DEST', mapping.with_name('missing.map'))
            self.assertNotEqual(bad.returncode, 0)
            self.assertEqual(bad.stdout, b'')
            self.assertIn(b'Usage: makechs', self.run_cli().stderr)
            target = mapping.with_name('target.map')
            target.write_text('0x8D 0x041D\n')
            mapped = self.run_cli('CP866', 'CUSTOM', mapping, target)
            self.assertEqual(mapped.returncode, 0, mapped.stderr)
            self.assertIn(b'Warning:', mapped.stderr)
            self.assertNotEqual(self.run_cli('CP866','CUSTOM',mapping,target.with_name('missing.map')).returncode,0)
            self.assertNotEqual(self.run_cli('bad\nname','UTF-8',mapping).returncode,0)
            mapping.write_text('0xFF invalid\n')
            self.assertNotEqual(self.run_cli('CP866','UTF-8',mapping).returncode,0)



if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True)
    OPTIONS, remainder = parser.parse_known_args()
    unittest.main(argv=[__file__, *remainder])
