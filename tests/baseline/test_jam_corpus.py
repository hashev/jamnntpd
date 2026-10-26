import json
from pathlib import Path
import tempfile
import unittest
import jam_corpus
import jam_oracle


class IndependentJamCorpus(unittest.TestCase):
    def test_recipes_have_independently_valid_records(self):
        for name, recipe in json.loads(jam_corpus.RECIPES.read_text()).items():
            with self.subTest(recipe=name):
                data = jam_corpus.generate(recipe)
                self.assertEqual(data, jam_corpus.generate(recipe))
                info = jam_oracle.base_header(data['.jhr'][:1024])
                indices = jam_oracle.indices(data['.jdx'])
                self.assertEqual(len(indices), recipe['messages'])
                live = 0
                for number, (_, offset) in enumerate(indices, 1):
                    if offset == 0xffffffff:
                        self.assertIn(number, recipe.get('deleted', []))
                        continue
                    live += 1
                    fixed = jam_oracle.message_header(data['.jhr'][offset:offset+76])
                    self.assertEqual(fixed[10], number)
                    fields = jam_oracle.subfields(data['.jhr'][offset+76:offset+76+fixed[0]])
                    codec = recipe.get('encodings', ['cp866'])[(number-1) % len(recipe.get('encodings', ['cp866']))]
                    self.assertEqual(jam_oracle.text_slice(fixed, data['.jdt']).decode(codec),
                                     f'Corpus {number}: Привет\r.line\r')
                    if recipe.get('unknown_subfield'):
                        self.assertIn((0x7777, 0xabcd, b'\x00\xffbinary\x00'), fields)
                self.assertEqual(info[2], live)
                if recipe.get('lastread'):
                    self.assertEqual([jam_oracle.uint(data['.jlr'], i, 4) for i in range(0, 16, 4)],
                                     [0x12345678, 42, 1, 3])
                else:
                    self.assertEqual(data['.jlr'], b'')

    def test_large_corpus_and_no_overwrite_contract(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder) / 'base'
            data = jam_corpus.generate({'messages': 0}, count=10000)
            self.assertEqual(len(jam_oracle.indices(data['.jdx'])), 10000)
            self.assertEqual(jam_oracle.base_header(data['.jhr'][:1024])[2], 10000)
            jam_corpus.write(base, data)
            with self.assertRaises(FileExistsError):
                jam_corpus.write(base, jam_corpus.generate({'messages': 0}))
            self.assertEqual(Path(str(base) + '.jhr').read_bytes(), data['.jhr'])
        with self.assertRaises(ValueError):
            jam_corpus.generate({'messages': 0}, -1)
        with self.assertRaises(ValueError):
            jam_corpus.generate({'messages': 0}, 100001)
