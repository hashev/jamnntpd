"""Independent JAM writer used only with disposable interoperability fixtures."""
import argparse
import os
from pathlib import Path
import struct
from test_storage import byte_lock


def append(base):
    paths = {suffix: Path(str(base) + suffix) for suffix in ['.jhr', '.jdt', '.jdx']}
    with paths['.jhr'].open('r+b', buffering=0) as headers, byte_lock(headers):
        info = headers.read(1024)
        assert info[:4] == b'JAM\0'
        number = paths['.jdx'].stat().st_size // 8 + int.from_bytes(info[20:24], 'little')
        body = 'Independent tosser: Н─│\r'.encode('cp866')
        fields = b''.join(struct.pack('<HHI', low, 0, len(value)) + value for low, value in
                          [(2, b'Tosser'), (3, b'All'), (6, b'Independent import'), (2000, b'CHRS: CP866 2')])
        values = [len(fields), 0, 0xffffffff, 0xffffffff, 0, 0, 0,
                  1700000000, 1700000000, 0, number, 0x01000000, 0,
                  paths['.jdt'].stat().st_size, len(body), 0xffffffff, 0]
        headers.seek(0, 2)
        offset = headers.tell()
        with paths['.jdt'].open('ab', buffering=0) as text, paths['.jdx'].open('ab', buffering=0) as index:
            text.write(body)
            headers.write(struct.pack('<4sHH17I', b'JAM\0', 1, 0, *values) + fields)
            index.write(struct.pack('<II', 0xffffffff, offset))
            headers.seek(8)
            headers.write(struct.pack('<II', int.from_bytes(info[8:12], 'little') + 1,
                                      int.from_bytes(info[12:16], 'little') + 1))
            for stream in [text, index, headers]:
                stream.flush()
                os.fsync(stream.fileno())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('base', type=Path)
    append(parser.parse_args().base)
