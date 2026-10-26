import importlib.util
from pathlib import Path
import socket
import time
import unittest

SPEC = importlib.util.spec_from_file_location('raw_client', Path(__file__).resolve().parents[1] /
                                             'integration/raw_client.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RawProtocolHarness(unittest.TestCase):
    def setUp(self):
        first, self.peer = socket.socketpair()
        self.client = MODULE.RawClient(first, timeout=.1)
        self.addCleanup(self.peer.close)
        self.addCleanup(self.client.close)

    def test_exact_multiline_preserves_stuffing_and_transcript(self):
        wire = b'220 Article\r\n..literal\r\nbody\r\n.\r\n'
        self.peer.sendall(wire)
        self.assertEqual(self.client.reply(220, multiline=True), wire)
        self.client.assert_quiet()
        self.client.send(b'HELP\n')
        self.assertEqual(self.peer.recv(64), b'HELP\n')
        self.assertEqual(self.client.transcript[-1], ('send_bytes', len(b'HELP\n')))

    def test_bare_lf_is_not_normalized(self):
        self.peer.sendall(b'200 Wrong\n')
        with self.assertRaisesRegex(AssertionError, 'CRLF'):
            self.client.reply(200)

    def test_unstuffed_dot_is_rejected(self):
        self.peer.sendall(b'220 Article\r\n.bad\r\n.\r\n')
        with self.assertRaisesRegex(AssertionError, 'Unstuffed'):
            self.client.reply(220, multiline=True)

    def test_missing_terminator_and_partial_eof_fail(self):
        self.peer.sendall(b'220 Article\r\nbody\r\n')
        self.peer.shutdown(socket.SHUT_WR)
        with self.assertRaises(EOFError):
            self.client.reply(220, multiline=True)

    def test_extra_bytes_are_detected(self):
        self.peer.sendall(b'205 Goodbye\r\nextra\r\n')
        self.client.reply(205)
        with self.assertRaisesRegex(AssertionError, 'Unexpected'):
            self.client.assert_quiet()

    def test_deadline_and_line_bound_are_enforced(self):
        start = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.client.line()
        self.assertLess(time.monotonic()-start, 1)
        self.peer.sendall(b'x'*1000)
        with self.assertRaisesRegex(AssertionError, 'limit'):
            self.client.line()

    def test_response_bound_and_reply_code_are_enforced(self):
        self.client.maximum_response = 20
        self.peer.sendall(b'220 Article\r\nlong body\r\n.\r\n')
        with self.assertRaisesRegex(AssertionError, 'bound'):
            self.client.reply(220, multiline=True)
