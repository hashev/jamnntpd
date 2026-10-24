"""Reference interpreter for fixtures only; no inferred reverse mapping."""


def parse_byte(token):
    if token == r'\\':
        return 92
    if token == r'\0':
        return 0
    if token.startswith(r'\x'):
        value = int(token[2:], 16)
    elif token.startswith(r'\d'):
        value = int(token[2:], 10)
    elif len(token) == 1:
        value = ord(token)
    else:
        raise ValueError('invalid escape')
    if not 0 <= value <= 255:
        raise ValueError('byte outside range')
    return value


def read_table(path):
    lines = [line.split(';', 1)[0].strip()
             for line in path.read_text().splitlines()
             if not line.startswith(';')]
    # Blank lines in the supplied files are separators, not mapping rows.
    # An explicit deletion row uses \0 and is not blank.
    lines = [line for line in lines if line]
    if lines and lines[-1] == 'END':
        lines.pop()
    if len(lines) < 5:
        raise ValueError('truncated header')
    ident, version, level = map(int, lines[:3])
    if level not in (1, 2) or version < 0:
        raise ValueError('invalid header')
    base, count = (0, 256) if ident > 65535 else ((0, 128) if level == 1 else (128, 128))
    if len(lines) != 5 + count:
        raise ValueError('wrong row count')
    table = [bytes([i]) if i else b'' for i in range(256)]
    for index, line in enumerate(lines[5:], base):
        tokens = line.split()
        if len(tokens) > 4:
            raise ValueError('oversized expansion')
        table[index] = bytes(value for value in map(parse_byte, tokens) if value)
    return table


def translate(table, data):
    return b''.join(table[byte] for byte in data)


def pattern_matches(pattern, value):
    for index, char in enumerate(pattern.lower()):
        if char == '*':
            return True
        if index >= len(value):
            return False
        if char != '?' and char != value[index].lower():
            return False
    return len(value) == len(pattern)


def split_charset(text):
    first, _, second = text.partition(',')
    return first, second


def select_read(case):
    chrs, codepage = case['chrs'], case['codepage']
    group, global_default = case['group'], case['global']
    if group.startswith('!'):
        chrs, codepage = split_charset(group[1:])
    elif global_default.startswith('!'):
        chrs, codepage = split_charset(global_default[1:])
    if not chrs and group and not group.startswith('!'):
        chrs, codepage = split_charset(group)
    if not chrs and global_default and not global_default.startswith('!'):
        chrs, codepage = split_charset(global_default)
    for pattern, replacement in case['readaliases']:
        source, page = split_charset(pattern)
        if pattern_matches(source, chrs) and (',' not in pattern or pattern_matches(page, codepage)):
            chrs, codepage = split_charset(replacement)
            break
    return [chrs, codepage]


def select_post(case):
    source = case['source'] or case['defaultpost']
    for pattern, replacement in case['aliases']:
        if pattern_matches(pattern, source):
            source = replacement
            break
    destination = None
    if case['group'].startswith('!'):
        destination = case['group'][1:]
    elif case['global'].startswith('!'):
        destination = case['global'][1:]
    return source, destination
