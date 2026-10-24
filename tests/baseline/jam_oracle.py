"""Independent fixture decoder. Deliberately does not import production code."""


def uint(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('truncated record')
    return int.from_bytes(data[offset:offset + size], 'little')


def base_header(data):
    if len(data) != 1024 or data[:4] != b'JAM\0':
        raise ValueError('invalid base header')
    return [uint(data, offset, 4) for offset in range(4, 24, 4)]


def message_header(data):
    if len(data) != 76 or data[:4] != b'JAM\0' or uint(data, 4, 2) != 1:
        raise ValueError('invalid message header')
    return [uint(data, offset, 4) for offset in range(8, 76, 4)]


def indices(data):
    if len(data) % 8:
        raise ValueError('truncated index')
    return [(uint(data, offset, 4), uint(data, offset + 4, 4))
            for offset in range(0, len(data), 8)]


def subfields(data):
    result = []
    offset = 0
    while offset < len(data):
        low = uint(data, offset, 2)
        high = uint(data, offset + 2, 2)
        length = uint(data, offset + 4, 4)
        offset += 8
        if length > len(data) - offset:
            raise ValueError('truncated subfield')
        result.append((low, high, data[offset:offset + length]))
        offset += length
    return result


def text_slice(header, text):
    offset, length = header[13:15]
    if offset > len(text) or length > len(text) - offset:
        raise ValueError('invalid text extent')
    return text[offset:offset + length]


def crc32_ascii(text):
    crc = 0xffffffff
    for byte in text:
        if ord('A') <= byte <= ord('Z'):
            byte += 32
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xedb88320 if crc & 1 else 0)
    return crc
