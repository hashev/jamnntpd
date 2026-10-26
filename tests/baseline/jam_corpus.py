"""Deterministic independent JAM fixtures; never writes to an existing base."""
import argparse
import json
from pathlib import Path
import struct

SUFFIXES = ('.jhr', '.jdt', '.jdx', '.jlr')
RECIPES = Path(__file__).resolve().parents[1] / 'fixtures/jam/corpus-recipes.json'


def generate(recipe, count=None):
    size = recipe['messages'] if count is None else count
    if not 0 <= size <= 100000:
        raise ValueError('Corpus count must be in [0, 100000]')
    dead = set(recipe.get('deleted', []))
    encodings = recipe.get('encodings', ['cp866'])
    headers, texts, indices = [], [], []
    header_offset, text_offset, active = 1024, 0, 0
    for number in range(1, size + 1):
        encoding = encodings[(number - 1) % len(encodings)]
        body = recipe.get('body', f'Corpus {number}: Привет\r.line\r').encode(encoding)
        label = {'cp1251': 'WINDOWS-1251', 'koi8_r': 'KOI8-R'}.get(encoding, encoding.upper())
        if recipe.get('labels'):
            label = recipe['labels'][(number-1) % len(recipe['labels'])]
        entries = [(2, 0, b'Alice'), (3, 0, b'All'), (6, 0, f'Corpus {number}'.encode()),
                   (2000, 0, f'CHRS: {label} 2'.encode())]
        if recipe.get('origin'):
            entries.append((0, 0, recipe['origin'].encode('ascii')))
        if recipe.get('unknown_subfield'):
            entries.append((0x7777, 0xabcd, b'\x00\xffbinary\x00'))
        fields = b''.join(struct.pack('<HHI', low, high, len(data)) + data
                          for low, high, data in entries)
        deleted = number in dead
        active += not deleted
        values = [len(fields), 0, 0xffffffff, 0xffffffff, 0, 0, 0,
                  1700000000, 1700000000, 0, number,
                  recipe.get('attributes', 0x01000000) | (0x80000000 if deleted else 0), 0,
                  text_offset, len(body), 0xffffffff, 0]
        record = struct.pack('<4sHH17I', b'JAM\0', 1, 0, *values) + fields
        headers.append(record)
        texts.append(body)
        indices.append(struct.pack('<II', 0xffffffff, 0xffffffff if deleted else header_offset))
        header_offset += len(record)
        text_offset += len(body)
    base = struct.pack('<4s5I1000s', b'JAM\0', 1700000000, 0, active, 0xffffffff, 1, b'')
    lastread = b''
    if recipe.get('lastread') and size:
        lastread = struct.pack('<4I', 0x12345678, 42, 1, size)
    return dict(zip(SUFFIXES, (base + b''.join(headers), b''.join(texts),
                               b''.join(indices), lastread)))


def write(base, content):
    paths = {suffix: Path(str(base) + suffix) for suffix in SUFFIXES}
    if any(path.exists() for path in paths.values()):
        raise FileExistsError('Refusing to overwrite an existing base')
    # Caller owns a new temporary directory; no use on live bases is supported.
    for suffix, path in paths.items():
        with path.open('xb') as output:
            output.write(content[suffix])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--recipe', default='mixed')
    parser.add_argument('--count', type=int)
    args = parser.parse_args()
    recipe = json.loads(RECIPES.read_text())[args.recipe]
    write(args.base, generate(recipe, args.count))
