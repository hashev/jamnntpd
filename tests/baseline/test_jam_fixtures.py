import json
from pathlib import Path
import unittest

import jam_oracle as oracle

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures' / 'jam'


class JamFixtures(unittest.TestCase):
    def data(self, name):
        return (FIXTURES / name).read_bytes()

    def test_fixed_base_header(self):
        self.assertEqual(oracle.base_header(self.data('base-header.bin')),
                         [0x80000000, 7, 1, 0xffffffff, 1])

    def test_message_high_bits(self):
        record = oracle.message_header(self.data('message-header.bin'))
        self.assertEqual(record[2:4], [0x80000000, 0xffffffff])
        self.assertEqual(record[10:13], [1, 0x01000001, 0])
        self.assertEqual(record[15], 0xffffffff)

    def test_index_deleted_sentinel(self):
        self.assertEqual(oracle.indices(self.data('index.bin')),
                         [(0xffffffff, 1024), (0xffffffff, 0xffffffff)])

    def test_lastread_layout(self):
        data = self.data('lastread.bin')
        self.assertEqual([oracle.uint(data, i, 4) for i in range(0, 16, 4)],
                         [0x12345678, 0xffffffff, 1, 0x80000000])

    def test_complete_base(self):
        expected = json.loads((FIXTURES / 'expected.json').read_text())
        header_file = self.data('sample.jhr')
        oracle.base_header(header_file[:1024])
        _, offset = oracle.indices(self.data('sample.jdx'))[0]
        header = oracle.message_header(header_file[offset:offset + 76])
        fields = oracle.subfields(header_file[offset + 76:offset + 76 + header[0]])
        self.assertEqual([[low, data.decode()] for low, _, data in fields],
                         expected['subfields'])
        self.assertEqual(oracle.text_slice(header, self.data('sample.jdt')).hex(),
                         expected['text_hex'])

    def test_invalid_records_rejected(self):
        for decoder, name in [(oracle.base_header, 'bad-signature.bin'),
                              (oracle.message_header, 'truncated-header.bin'),
                              (oracle.indices, 'truncated-index.bin'),
                              (oracle.subfields, 'overflow-subfield.bin')]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                decoder(self.data(name))
        with self.assertRaises(ValueError):
            oracle.text_slice(oracle.message_header(self.data('overflow-text-header.bin')),
                              self.data('sample.jdt'))

    def test_crc_vectors(self):
        expected = json.loads((FIXTURES / 'expected.json').read_text())
        self.assertEqual(expected['jam_crc32']['123456789'], 0x340bc6d9)
        for text, crc in expected['jam_crc32'].items():
            with self.subTest(text=text):
                self.assertEqual(oracle.crc32_ascii(text.encode()), crc)
