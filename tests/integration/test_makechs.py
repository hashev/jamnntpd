import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'baseline'))
import chs_oracle
from junit_result import JUnitResult


class MakeChsCli(unittest.TestCase):
    def run_cli(self, *arguments):
        return subprocess.run([OPTIONS.executable, *map(str, arguments)],
                              capture_output=True, timeout=10, check=False)

    def test_generated_tables_are_verified_by_independent_parser(self):
        with tempfile.TemporaryDirectory(prefix='jam-map-oracle-') as temporary:
            root = Path(temporary)
            source, target, table = root/'source.map', root/'target.map', root/'out.chs'
            source.write_text('0x8D 0x041D\n0xFF 0x1F600\n')
            generated = self.run_cli('CP866', 'UTF-8', source)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            table.write_bytes(generated.stdout)
            mapping = chs_oracle.read_table(table)
            self.assertEqual(chs_oracle.translate(mapping, b'A\x8d\xff'), 'AН😀'.encode())
            source.write_text('0x41 0x0041\n0x42 0x0042\n')
            target.write_text('0x41 0x0042\n0x42 0x0041\n')
            generated = self.run_cli('SOURCE', 'DEST', source, target)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            table.write_bytes(generated.stdout)
            self.assertEqual(chs_oracle.translate(chs_oracle.read_table(table), b'AB'), b'BA')
            generated = self.run_cli('SOURCE', 'SOURCE', source, source)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            table.write_bytes(generated.stdout)
            self.assertEqual(chs_oracle.translate(chs_oracle.read_table(table), b'A\x80\xff'), b'A\x80\xff')
            for invalid in ['0x100 0x0041\n', '0x80 0x110000\n',
                            '0x80 0xD800\n', '0x80 garbage\n', 'incomplete\n']:
                with self.subTest(invalid=invalid):
                    source.write_text(invalid)
                    result = self.run_cli('SOURCE', 'UTF-8', source)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, b'')

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
    parser.add_argument('--junit', type=Path)
    OPTIONS, remainder = parser.parse_known_args()
    program = unittest.main(argv=[__file__, *remainder], exit=False,
                            testRunner=unittest.TextTestRunner(resultclass=JUnitResult))
    if OPTIONS.junit is not None:
        program.result.write(OPTIONS.junit)
    sys.exit(0 if program.result.wasSuccessful() else 1)
