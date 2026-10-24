import json
from pathlib import Path
import unittest

import chs_oracle as oracle

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures' / 'chs'


class ChsFixtures(unittest.TestCase):
    def setUp(self):
        self.expected = json.loads((FIXTURES / 'expected.json').read_text())

    def test_directional_expansion_deletion_and_escapes(self):
        table = oracle.read_table(FIXTURES / 'custom-forward.chs')
        result = oracle.translate(table, bytes.fromhex(self.expected['custom_input']))
        self.assertEqual(result.hex(), self.expected['custom_output'])
        reverse = oracle.read_table(FIXTURES / 'custom-reverse.chs')
        self.assertEqual(oracle.translate(reverse, bytes.fromhex(self.expected['reverse_input'])).hex(),
                         self.expected['reverse_output'])
        self.assertNotEqual(oracle.translate(reverse, result), bytes.fromhex(self.expected['custom_input']))

    def test_level_one_preserves_upper_half(self):
        table = oracle.read_table(FIXTURES / 'level-one.chs')
        self.assertEqual(oracle.translate(table, bytes.fromhex(self.expected['level_one_input'])).hex(),
                         self.expected['level_one_output'])

    def test_real_unicode_table_preserves_letters_and_box_drawing(self):
        table = oracle.read_table(FIXTURES / 'cp866-to-utf8.chs')
        for source, expected in self.expected['cp866_to_utf8'].items():
            with self.subTest(source=source):
                self.assertEqual(oracle.translate(table, bytes.fromhex(source)).hex(), expected)
        self.assertEqual(oracle.translate(table, b'\x8d').decode(), 'Н')

    def test_real_legacy_table(self):
        table = oracle.read_table(FIXTURES / 'cp866-to-koi8.chs')
        self.assertEqual(oracle.translate(table, b'\x80\xa0'), 'Аа'.encode('koi8-r'))
        self.assertEqual(oracle.translate(table, b'ASCII'), b'ASCII')

    def test_invalid_tables(self):
        for name in ['truncated.chs', 'invalid-escape.chs']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                oracle.read_table(FIXTURES / name)

    def test_selection_precedence(self):
        for case in json.loads((FIXTURES / 'selection.json').read_text()):
            with self.subTest(name=case['name']):
                self.assertEqual(oracle.select_read(case), case['expected'])

    def test_post_selection_precedence(self):
        for case in json.loads((FIXTURES / 'post-selection.json').read_text()):
            with self.subTest(name=case['name']):
                self.assertEqual(oracle.select_post(case),
                                 (case['expected_source'], case['expected_destination']))
