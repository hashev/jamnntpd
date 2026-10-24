import json
from pathlib import Path
import unittest

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures' / 'protocol'


class ProtocolFixtures(unittest.TestCase):
    def test_transcripts_have_exact_wire_framing(self):
        for item in json.loads((FIXTURES / 'session.json').read_text()):
            with self.subTest(request=item['request']):
                self.assertTrue(item['request'].endswith('\r\n'))
                self.assertTrue(item['response'].endswith('\r\n'))
                self.assertRegex(item['response'], r'^[1-5][0-9]{2} ')
                self.assertNotIn('\n', item['response'].replace('\r\n', ''))
                if item['response'][:3] in ('215', '230', '231'):
                    self.assertTrue(item['response'].endswith('\r\n.\r\n'))

    def test_help_is_complete_multiline_fixture(self):
        response = (FIXTURES / 'help-response.txt').read_bytes()
        self.assertTrue(response.startswith(b'100 '))
        self.assertTrue(response.endswith(b'\r\n.\r\n'))
        self.assertIn(b'AUTHINFO\r\n', response)
        self.assertIn(b'XOVER ', response)
