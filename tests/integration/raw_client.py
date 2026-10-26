"""Bounded byte-exact NNTP observer; deliberately does not repair server output."""
import select
import socket
import time


class RawClient:
    def __init__(self, connection, timeout=4, limit=1000, maximum_response=1024*1024):
        self.socket = connection
        self.timeout = timeout
        self.limit = limit
        self.maximum_response = maximum_response
        self.buffer = bytearray()
        self.transcript = []

    def send(self, data):
        if not isinstance(data, bytes):
            raise TypeError('Raw requests must be bytes')
        deadline = time.monotonic() + self.timeout
        remaining = memoryview(data)
        while remaining:
            self.socket.settimeout(max(0.001, deadline-time.monotonic()))
            if time.monotonic() >= deadline:
                raise TimeoutError('Raw send deadline')
            count = self.socket.send(remaining)
            if not count:
                raise EOFError('No write progress')
            remaining = remaining[count:]
        # Requests may contain fragmented AUTHINFO credentials; never retain request bytes.
        self.transcript.append(('send_bytes', len(data)))

    def line(self, deadline=None):
        deadline = time.monotonic() + self.timeout if deadline is None else deadline
        while True:
            end = self.buffer.find(b'\n')
            if end >= 0:
                result = bytes(self.buffer[:end+1])
                del self.buffer[:end+1]
                if len(result) > self.limit:
                    raise AssertionError('Wire line exceeds limit')
                if not result.endswith(b'\r\n') or b'\r' in result[:-2]:
                    raise AssertionError('Invalid CRLF framing')
                self.transcript.append(('receive', result.hex()))
                return result
            if len(self.buffer) >= self.limit:
                raise AssertionError('Wire line exceeds limit')
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Raw read deadline')
            self.socket.settimeout(remaining)
            data = self.socket.recv(min(4096, self.limit-len(self.buffer)))
            if not data:
                raise EOFError('EOF before complete response')
            self.buffer.extend(data)

    def reply(self, code, multiline=False):
        deadline = time.monotonic() + self.timeout
        first = self.line(deadline)
        if not first.startswith(str(code).encode() + b' '):
            raise AssertionError(f'Expected {code}, received {first!r}')
        result = bytearray(first)
        if multiline:
            while True:
                line = self.line(deadline)
                result.extend(line)
                if len(result) > self.maximum_response:
                    raise AssertionError('Response exceeds bound')
                if line == b'.\r\n':
                    break
                if line.startswith(b'.') and not line.startswith(b'..'):
                    raise AssertionError('Unstuffed dot in multiline response')
        return bytes(result)

    def assert_quiet(self, timeout=0.03):
        if self.buffer:
            raise AssertionError('Unexpected bytes after response')
        ready, _, _ = select.select([self.socket], [], [], timeout)
        if ready and self.socket.recv(1):
            raise AssertionError('Unexpected bytes after response')

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
