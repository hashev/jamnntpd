"""Seeded NNTP state model over the real executable, with credential-free replay artifacts."""
import argparse
import json
import os
from pathlib import Path
import random
import sys

import test_server

# Stable operation indices are part of the saved replay format. Append; do not reorder.
OPERATIONS = ['GROUP test', 'GROUP hidden', 'STAT', 'STAT 2', 'STAT 3',
              'STAT <1$test@JamNNTPd>', 'NEXT', 'LAST', 'AUTHINFO USER alice',
              'AUTHINFO USER bob', 'AUTHINFO PASS secret', 'AUTHINFO PASS password',
              'AUTHINFO PASS wrong', 'AUTHINFO USER /flowed=off', 'AUTHINFO USER /bad=on',
              'AUTHINFO PASS', 'AUTHINFO USER', 'XOVER 1-5', 'HELP', 'UNKNOWN']
LIVE = [1, 3, 5]


def session(client, operations):
    selected, current, readable, pending = False, 0, True, None
    for number, operation in enumerate(operations):
        command = OPERATIONS[operation]
        multiline = False
        if command == 'GROUP test':
            code = 211 if readable else 411
            if readable:
                selected, current = True, 1
        elif command == 'GROUP hidden':
            code = 411
        elif command == 'STAT <1$test@JamNNTPd>':
            code = 223 if readable else 430
        elif command.startswith('STAT'):
            code = 412 if not selected else 223
            if selected:
                if command == 'STAT 2':
                    code = 423
                elif command == 'STAT 3':
                    current = 3
        elif command in ['NEXT', 'LAST']:
            code = 412
            if selected:
                choices = [value for value in LIVE if (value > current if command == 'NEXT' else value < current)]
                if choices:
                    current = min(choices) if command == 'NEXT' else max(choices)
                    code = 223
                else:
                    code = 421 if command == 'NEXT' else 422
        elif command.startswith('AUTHINFO USER '):
            pending = command[len('AUTHINFO USER '):]
            code = 381
        elif command == 'AUTHINFO USER':
            code = 482
        elif command.startswith('AUTHINFO PASS'):
            password = command[len('AUTHINFO PASS'):].strip()
            code = 482
            if pending is not None and password:
                if pending == '/flowed=off':
                    code = 281
                elif pending == '/bad=on':
                    code = 482
                elif (pending, password) in [('alice', 'secret'), ('bob', 'password')]:
                    code = 281
                    readable = pending == 'alice'
                    if not readable:
                        selected, current = False, 0
                else:
                    code = 481
        elif command == 'XOVER 1-5':
            code = 224 if selected else 412
            multiline = selected
        elif command == 'HELP':
            code, multiline = 100, True
        else:
            code = 500
        client.send(command.encode()+b'\r\n')
        reply = client.reply(code, multiline=multiline)
        if command == 'GROUP test' and code == 211:
            assert reply == b'211 5 1 5 test Group selected\r\n', (number, operation)
        if command == 'XOVER 1-5' and multiline:
            rows = reply.split(b'\r\n')[1:-2]
            assert [int(row.split(b'\t', 1)[0]) for row in rows] == LIVE, (number, operation)
        # Independent observation after every operation checks otherwise invisible state changes.
        client.send(b'STAT\r\n')
        observed = client.reply(223 if selected else 412)
        expected = (f'223 {current} <{current}$test@JamNNTPd> Article retrieved\r\n'.encode()
                    if selected else b'412 No newsgroup selected\r\n')
        assert observed == expected, (number, operation, observed, expected)
    client.send(b'QUIT\r\n')
    client.reply(205)
    assert client.socket.recv(1) == b''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--sessions', type=int, default=int(os.environ.get('JAM_MODEL_SESSIONS', '100')))
    parser.add_argument('--seed', type=int, default=int(os.environ.get('JAM_TEST_SEED', '20261026')))
    parser.add_argument('--replay', action='store_true')
    args = parser.parse_args()
    if not 0 <= args.seed <= 0xffffffff:
        parser.error('Seed must fit an unsigned 32-bit integer')
    test_server.OPTIONS = args
    replay = None
    if args.replay:
        try:
            replay = json.loads(Path(os.environ['JAM_REPLAY_FILE']).read_text())
            assert replay['version'] == 1
            assert isinstance(replay['operations'], list) and len(replay['operations']) <= 200
            assert all(type(index) is int and 0 <= index < len(OPERATIONS) for index in replay['operations'])
        except (KeyError, AssertionError, ValueError, OSError):
            return 2
    elif not 100 <= args.sessions <= 1000:
        parser.error('Sessions must be in [100, 1000]')
    os.environ['JAM_TEST_SEED'] = str(args.seed)
    randomizer = random.Random(args.seed)
    with test_server.Server(recipe={'messages': 5, 'deleted': [2, 4]},
                            allow='127.0.0.1 a a\n', extra='max 32\n') as server:
        (server.root/'users').write_text('alice secret a a Alice\nbob password - - Bob\n')
        for index in range(1 if replay is not None else args.sessions):
            operations = replay['operations'] if replay is not None else [randomizer.randrange(len(OPERATIONS)) for _ in range(20)]
            try:
                with server.raw_client() as client:
                    session(client, operations)
            except (AssertionError, EOFError, TimeoutError) as error:
                saved = server.save_failure('generated-state')
                path = saved/'model-case.json'
                path.write_text(json.dumps({'version': 1, 'operations': operations})+'\n')
                print(f'State model failed: seed={args.seed} session={index} artifact={path}; {type(error).__name__}', file=sys.stderr)
                return 3
    print(f'{1 if replay else args.sessions} model sessions passed; seed={args.seed}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
