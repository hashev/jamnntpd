"""Barrier-controlled NNTP mutations and shutdown under an independent byte lock."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import threading
from pathlib import Path
import time
import test_server
from test_storage import byte_lock
import jam_oracle

parser = argparse.ArgumentParser()
parser.add_argument('--executable', required=True)
parser.add_argument('--fixtures', required=True, type=Path)
parser.add_argument('--rounds', type=int, default=100)
options = parser.parse_args()
test_server.OPTIONS = options
for iteration in range(options.rounds):
    with test_server.Server() as server, server.client() as first, server.client() as second:
        assert first.login().startswith(b'281')
        assert second.login().startswith(b'281')
        barrier = threading.Barrier(2)
        def post(client, marker):
            barrier.wait(timeout=4)
            return client.post(test_server.article(marker))
        with ThreadPoolExecutor(max_workers=2) as workers:
            results = [workers.submit(post, client, f'iteration {iteration} client {number}')
                       for number, client in enumerate([first, second])]
            assert all(result.result(timeout=8) == b'240 Article posted\r\n' for result in results)
        assert first.command('STAT <3$test@JamNNTPd>').startswith(b'223')
        assert second.command('STAT <4$test@JamNNTPd>').startswith(b'223')
        assert first.post(test_server.article('', extra='Control: cancel <3$test@JamNNTPd>\r\n')) == b'240 Article cancelled\r\n'
        before = server.snapshot()
        # Both workers contend with another process; stopping must interrupt open as well as mutation waits.
        with (server.root / 'test.jhr').open('r+b', buffering=0) as header:
            with byte_lock(header):
                first.socket.sendall(b'GROUP test\r\n')
                assert second.command('POST').startswith(b'340')
                second.socket.sendall(test_server.article('must not commit') + b'.\r\n')
                beginning = time.monotonic()
                server.stop()
                assert time.monotonic() - beginning < 8
        assert server.snapshot() == before, 'shutdown published an uncommitted mutation'
        assert 'All workers joined' in (server.root / 'log').read_text()
        indices = jam_oracle.indices(before['.jdx'])
        assert len(indices) == 4
        assert indices[1][1] == 0xffffffff and indices[2][1] == 0xffffffff
        assert jam_oracle.base_header(before['.jhr'][:1024])[2] == 2
        for position, (_, offset) in enumerate(indices, start=1):
            if offset == 0xffffffff: continue
            raw = before['.jhr'][offset:offset+76]
            fixed = jam_oracle.message_header(raw)
            assert jam_oracle.uint(raw, 48, 4) == position
            fields = jam_oracle.subfields(before['.jhr'][offset+76:offset+76+fixed[0]])
            jam_oracle.text_slice(fixed, before['.jdt'])
            identifiers = [data for low, _, data in fields if low == 4]
            if identifiers: assert fixed[2] == jam_oracle.crc32_ascii(identifiers[0])
print(f'{options.rounds} contested read/post/cancel/shutdown rounds passed')
