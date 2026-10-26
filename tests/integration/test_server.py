"""Real executable NNTP tests against disposable JAM bases; no production data."""
import argparse
import base64
import quopri
import contextlib
from concurrent.futures import ThreadPoolExecutor
import email
import email.header
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'baseline'))
import jam_oracle
import jam_corpus
from raw_client import RawClient
from junit_result import JUnitResult
from test_storage import byte_lock

class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=4)
        self.file = self.socket.makefile('rb')
        self.greeting = self.line()
    def line(self):
        value = self.file.readline()
        if not value:
            raise EOFError('server closed connection')
        assert value.endswith(b'\r\n'), value
        return value
    def command(self, command):
        self.socket.sendall(command.encode() + b'\r\n')
        return self.line()
    def multiline(self, command):
        first = self.command(command)
        if not first.startswith((b'100', b'215', b'220', b'221', b'222', b'224', b'230', b'231')):
            return first, b''
        lines = []
        while True:
            line = self.line()
            if line == b'.\r\n': break
            lines.append(line[1:] if line.startswith(b'..') else line)
        return first, b''.join(lines)
    def post(self, article):
        first = self.command('POST')
        if not first.startswith(b'340'): return first
        stuffed = b'\r\n'.join(b'.' + line if line.startswith(b'.') else line for line in article.split(b'\r\n'))
        self.socket.sendall(stuffed + (b'' if stuffed.endswith(b'\r\n') else b'\r\n') + b'.\r\n')
        return self.line()
    def login(self, name='alice', password='secret'):
        assert self.command('AUTHINFO USER ' + name).startswith(b'381')
        return self.command('AUTHINFO PASS ' + password)
    def close(self):
        self.file.close()
        self.socket.close()
    def __enter__(self): return self
    def __exit__(self, *_): self.close()

class Server:
    def __init__(self, extra='', xlat=None, allow='127.0.0.1 ac ac\n', charset='CP866', console=False, recipe=None, force_charset=True, startup_timeout=10, strict_netmail=True, flags=None):
        self.temporary = tempfile.TemporaryDirectory(prefix='jamnntpd-e2e-')
        self.root = Path(self.temporary.name)
        self.clients = []
        try:
            for name in ['test', 'private', 'local', 'hidden']:
                if recipe is not None:
                    jam_corpus.write(self.root / name, jam_corpus.generate(recipe))
                else:
                    for suffix in ['.jhr', '.jdt', '.jdx', '.jlr']:
                        shutil.copyfile(OPTIONS.fixtures / 'jam' / ('sample' + suffix), self.root / (name + suffix))
            (self.root / 'groups').write_text(''.join(f'{prefix}{name} {access} 2:1/2 "{self.root / name}" {("!" if force_charset else "")}{charset}\n'
                for prefix,name,access in [('', 'test', 'a'), ('!', 'private', 'b'), ('$', 'local', 'c'), ('', 'hidden', 'd')]))
            (self.root / 'allow').write_text(allow)
            (self.root / 'users').write_text('alice secret abc abc Alice\nbob password abc abc Bob\n')
            (self.root / 'xlat').write_text(xlat if xlat is not None else f'defaultpost UTF-8\nread {charset} UTF-8 -builtin\npost UTF-8 {charset} -builtin\n')
            self.config = self.root / 'config'
            selected_flags = (['debug', 'loggroups'] + (['strictnetmail'] if strict_netmail else []) +
                              ([] if console else ['noecholog'])) if flags is None else flags
            self.config.write_text('port 0\nmax 8\n' + ''.join(flag+'\n' for flag in selected_flags) +
                ''.join(f'{option} "{self.root / value}"\n' for option,value in [('groups','groups'),('allow','allow'),('users','users'),('xlat','xlat'),('logfile','log')]) +
                (extra(self.root) if callable(extra) else extra))
            self.stdout = (self.root / 'stdout').open('wb')
            self.stderr = (self.root / 'stderr').open('wb')
            options = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {}
            self.process = subprocess.Popen([OPTIONS.executable, '-config', str(self.config)], stdout=self.stdout, stderr=self.stderr, **options)
            deadline = time.monotonic() + startup_timeout
            while time.monotonic() < deadline:
                if self.process.poll() is not None: raise RuntimeError((self.root / 'stderr').read_text())
                log = (self.root / 'log').read_text() if (self.root / 'log').exists() else ''
                log += (self.root / 'stdout').read_text(errors='replace')
                ready = re.search(r'Listening on port (\d+)', log)
                if ready:
                    self.port = int(ready.group(1)); break
                time.sleep(.02)
            else: raise TimeoutError('server did not become ready')
        except BaseException:
            try:
                self.save_failure('startup')
            finally:
                try:
                    self.abort()
                finally:
                    self.temporary.cleanup()
            raise
    def client(self): return Client(self.port)
    def raw_client(self):
        client = RawClient(socket.create_connection(('127.0.0.1', self.port), timeout=4))
        try:
            client.reply(200)
            self.clients.append(client)
            return client
        except BaseException:
            client.close()
            raise
    def snapshot(self, name='test'):
        return {suffix: (self.root / (name + suffix)).read_bytes() for suffix in ['.jhr','.jdt','.jdx','.jlr']}
    def message(self, number, name='test'):
        headers = (self.root / (name + '.jhr')).read_bytes()
        index = jam_oracle.indices((self.root / (name + '.jdx')).read_bytes())
        offset = index[number - 1][1]
        raw = headers[offset:offset+76]
        fixed = jam_oracle.message_header(raw)
        fields = jam_oracle.subfields(headers[offset+76:offset+76+fixed[0]])
        body = jam_oracle.text_slice(fixed, (self.root / (name + '.jdt')).read_bytes())
        return fixed, {low: data for low,_,data in fields if low != 2000}, body, [data for low,_,data in fields if low == 2000]
    def restart(self):
        self.stop()
        log_path = self.root / 'log'
        previous = len(log_path.read_text()) if log_path.exists() else 0
        self.stdout = (self.root / 'stdout').open('ab')
        self.stderr = (self.root / 'stderr').open('ab')
        options = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {}
        self.process = subprocess.Popen([OPTIONS.executable, '-config', str(self.config)],
                                        stdout=self.stdout, stderr=self.stderr, **options)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError('Server exited during restart')
            log = log_path.read_text()[previous:] if log_path.exists() else ''
            ready = re.search(r'Listening on port (\d+)', log)
            if ready:
                self.port = int(ready.group(1))
                return
            time.sleep(.02)
        raise TimeoutError('Server restart deadline')

    def save_failure(self, phase):
        destination = Path(os.environ.get('JAM_TEST_ARTIFACTS', 'build/acceptance-failures'))
        destination.mkdir(parents=True, exist_ok=True)
        target = Path(tempfile.mkdtemp(prefix='failure-', dir=destination))
        # Only disposable JAM data and already-redacted server diagnostics. Never copy users/config.
        for source in self.root.iterdir():
            if source.suffix in ('.jhr', '.jdt', '.jdx', '.jlr') or source.name in ('log', 'stdout', 'stderr'):
                shutil.copyfile(source, target / source.name)
        (target / 'context.json').write_text(json.dumps({
            'phase': phase, 'seed': os.environ.get('JAM_TEST_SEED', '0'),
            'transcripts': [client.transcript for client in self.clients]}, indent=2))
        return target
    def abort(self):
        process = getattr(self, 'process', None)
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=4)
        for name in ('stdout', 'stderr'):
            stream = getattr(self, name, None)
            if stream is not None:
                stream.close()
    def stop(self):
        try:
            if self.process.poll() is None:
                self.process.send_signal(signal.CTRL_BREAK_EVENT if os.name == 'nt' else signal.SIGTERM)
                try:
                    self.process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    self.save_failure('shutdown-timeout')
                    raise
            assert self.process.returncode == 0, (self.process.returncode, (self.root / 'stderr').read_text())
        finally:
            self.abort()
    def __enter__(self): return self
    def __exit__(self, kind, *_):
        try:
            if kind is not None:
                self.save_failure('test')
                self.abort()
            else:
                self.stop()
        finally:
            for client in self.clients:
                client.close()
            self.temporary.cleanup()

def article(body='Н─│', group='test', subject='Re: Topic', extra=''):
    return (f'From: Alice <alice@example.org>\r\nNewsgroups: {group}\r\nSubject: {subject}\r\n'
            f'Content-Type: text/plain; charset=UTF-8\r\n{extra}\r\n{body}\r\n').encode('utf-8')

class NntpEndToEnd(unittest.TestCase):
    def test_harness_preserves_evidence_and_cleans_up_on_startup_timeout(self):
        launched = []
        original = subprocess.Popen
        def launch(*args, **kwargs):
            process = original(*args, **kwargs)
            launched.append(process)
            return process
        with tempfile.TemporaryDirectory() as artifacts:
            with mock.patch.dict(os.environ, JAM_TEST_ARTIFACTS=artifacts, JAM_TEST_SEED='123'), \
                 mock.patch.object(subprocess, 'Popen', side_effect=launch), \
                 mock.patch.object(re, 'search', return_value=None):
                with self.assertRaises(TimeoutError):
                    Server(startup_timeout=.05)
            self.assertEqual(len(launched), 1)
            self.assertIsNotNone(launched[0].poll())
            saved = list(Path(artifacts).iterdir())
            self.assertEqual(len(saved), 1)
            self.assertTrue((saved[0] / 'test.jhr').is_file())
            context = json.loads((saved[0] / 'context.json').read_text())
            self.assertEqual(context['seed'], '123')
            self.assertEqual(context['phase'], 'startup')
            self.assertFalse((saved[0] / 'users').exists())
            self.assertFalse((saved[0] / 'config').exists())

    def test_harness_cleans_up_exception_and_redacts_fragmented_requests(self):
        with tempfile.TemporaryDirectory() as artifacts:
            with mock.patch.dict(os.environ, JAM_TEST_ARTIFACTS=artifacts, JAM_TEST_SEED='456'):
                with self.assertRaisesRegex(RuntimeError, 'test failure'):
                    with Server() as server:
                        client = server.raw_client()
                        client.send(b'AUTHINFO USER alice\r\nAUTHINFO PA')
                        client.send(b'SS PRIVATE-HARNESS-SENTINEL\r\n')
                        client.reply(381)
                        client.reply(481)
                        raise RuntimeError('test failure')
            self.assertIsNotNone(server.process.poll())
            self.assertFalse(server.root.exists())
            with socket.socket() as connection:
                self.assertNotEqual(connection.connect_ex(('127.0.0.1', server.port)), 0)
            saved = next(Path(artifacts).iterdir())
            context = json.loads((saved / 'context.json').read_text())
            self.assertTrue(context['transcripts'])
            self.assertEqual(context['phase'], 'test')
            self.assertEqual(context['seed'], '456')
            for path in saved.iterdir():
                self.assertNotIn(b'PRIVATE-HARNESS-SENTINEL', path.read_bytes())

    def test_all_commands_and_article_states_over_raw_tcp(self):
        with Server() as server, server.raw_client() as client:
            def single(command, code, exact=None):
                client.send(command.encode() + b'\r\n')
                value = client.reply(code)
                if exact is not None:
                    self.assertEqual(value, exact)
                return value
            for command in ['ARTICLE', 'HEAD', 'BODY', 'STAT', 'NEXT', 'LAST', 'XOVER']:
                single(command, 412, b'412 No newsgroup selected\r\n')
            single('GROUP', 501, b'501 No group specified\r\n')
            single('GROUP unknown', 411, b'411 No such newsgroup\r\n')
            single('gRoUp test', 211, b'211 2 1 2 test Group selected\r\n')
            for command, code in [('ARTICLE', 220), ('HEAD', 221), ('BODY', 222)]:
                for argument in ['', ' 1', ' <1$test@JamNNTPd>']:
                    client.send((command+argument+'\r\n').encode())
                    result = client.reply(code, multiline=True)
                    if command != 'HEAD':
                        self.assertIn(b'Hello\r\n..World\r\n', result)
                    if command != 'BODY':
                        self.assertIn(b'Subject: Fixture\r\n', result)
            for argument in ['', ' 1', ' <1$test@JamNNTPd>']:
                single('STAT'+argument, 223,
                       b'223 1 <1$test@JamNNTPd> Article retrieved\r\n')
            for command in ['ARTICLE', 'HEAD', 'BODY', 'STAT']:
                for argument, code in [(' 2', 423), (' <2$test@JamNNTPd>', 430),
                                       (' <1$hidden@JamNNTPd>', 430), (' -1', 501),
                                       (' 4294967296', 501), (' 1x', 501)]:
                    single(command+argument, code)
            single('NEXT', 421)
            single('LAST', 422)
            for argument in ['', ' 1', ' 1-1', ' 1-', ' 1-2']:
                client.send(('XOVER'+argument+'\r\n').encode())
                overview = client.reply(224, multiline=True)
                self.assertIn(b'1\tFixture\t', overview)
                self.assertNotIn(b'\r\n2\t', overview)
            single('XOVER 3-1', 420)
            single('XOVER 4294967296-', 501)
            for argument in ['', ' ACTIVE', ' NEWSGROUPS', ' OVERVIEW.FMT']:
                client.send(('LIST'+argument+'\r\n').encode())
                listing = client.reply(215, multiline=True)
                self.assertNotIn(b'hidden', listing)
                if argument != ' OVERVIEW.FMT':
                    self.assertNotIn(b'private', listing)
            single('LIST NEWSGROUPS *', 501)
            single('LIST unknown', 501)
            client.send(b'HELP\r\n')
            self.assertEqual(client.reply(100, multiline=True),
                             (OPTIONS.fixtures / 'protocol/help-response.txt').read_bytes())
            for command, code, multiline in [('IHAVE <id>', 435, False),
                                            ('NEWGROUPS', 231, True), ('NEWNEWS', 230, True),
                                            ('SLAVE', 202, False), ('MODE READER', 200, False)]:
                client.send((command+'\r\n').encode())
                client.reply(code, multiline=multiline)
            single('UNKNOWN', 500, b'500 Unknown command\r\n')
            client.send(b' \r\n')
            client.assert_quiet()
            single('AUTHINFO PASS secret', 482)
            single('AUTHINFO USER alice', 381)
            single('AUTHINFO PASS wrong', 481)
            single('AUTHINFO PASS secret', 281)
            single('POST', 340)
            client.send(article('..raw body') + b'.\r\n')
            client.reply(240)
            single('STAT 3', 223, b'223 3 <3$test@JamNNTPd> Article retrieved\r\n')
            single('LAST', 223, b'223 1 <1$test@JamNNTPd> Article retrieved\r\n')
            single('NEXT', 223, b'223 3 <3$test@JamNNTPd> Article retrieved\r\n')
            single('QUIT', 205, b'205 Goodbye\r\n')
            self.assertEqual(client.socket.recv(1), b'')

    def test_empty_group_current_article_and_session_isolation(self):
        with Server(recipe={'messages': 0}) as server, server.raw_client() as client:
            client.send(b'GROUP test\r\n')
            self.assertEqual(client.reply(211), b'211 0 0 0 test Group selected\r\n')
            for command in ['ARTICLE', 'HEAD', 'BODY', 'STAT', 'NEXT', 'LAST', 'XOVER']:
                client.send((command+'\r\n').encode())
                client.reply(420)
        with Server() as server, server.client() as first, server.client() as second:
            self.assertTrue(first.command('GROUP test').startswith(b'211'))
            self.assertEqual(second.command('STAT'), b'412 No newsgroup selected\r\n')
            self.assertTrue(first.login().startswith(b'281'))
            self.assertTrue(first.command('GROUP private').startswith(b'211'))
            self.assertEqual(second.command('GROUP private'), b'411 No such newsgroup\r\n')
            self.assertEqual(first.login(password='wrong'), b'481 Authentication rejected\r\n')
            self.assertTrue(first.command('STAT <1$private@JamNNTPd>').startswith(b'223'))
            self.assertTrue(first.login(name='/flowed=off').startswith(b'281'))
            self.assertTrue(first.command('STAT <1$private@JamNNTPd>').startswith(b'223'))

    def test_reauthentication_losing_permissions_invalidates_current_group(self):
        with Server() as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            self.assertTrue(client.command('GROUP private').startswith(b'211'))
            self.assertTrue(client.command('STAT').startswith(b'223'))
            for name in ['/unknown=on', '/flowed=invalid']:
                self.assertTrue(client.login(name=name).startswith(b'482'))
                self.assertTrue(client.command('STAT').startswith(b'223'))
            (server.root / 'users').write_text('alice secret a a Alice\n')
            self.assertEqual(client.login(password='wrong'), b'481 Authentication rejected\r\n')
            self.assertTrue(client.command('STAT').startswith(b'223'))
            self.assertEqual(client.login(), b'281 Authentication accepted\r\n')
            self.assertEqual(client.command('STAT'), b'412 No newsgroup selected\r\n')
            self.assertEqual(client.command('GROUP private'), b'411 No such newsgroup\r\n')
            self.assertEqual(client.command('STAT <1$private@JamNNTPd>'), b'430 No such article found\r\n')
            before = server.snapshot('private')
            self.assertEqual(client.post(article(group='private')), b'441 Posting failed\r\n')
            self.assertEqual(server.snapshot('private'), before)
            self.assertTrue(client.command('GROUP test').startswith(b'211'))
            self.assertTrue(client.command('STAT').startswith(b'223'))

    def test_guest_read_and_post_permissions_are_independent_and_first_match_wins(self):
        for read, post in [('a', '-'), ('-', 'a'), ('-', '-'), ('a', 'a')]:
            with self.subTest(read=read, post=post), \
                 Server(allow=f'127.0.0.* {read} {post}\n* abc abc\n') as server, server.client() as client:
                before = server.snapshot()
                code = b'211' if read == 'a' else b'411'
                self.assertTrue(client.command('GROUP test').startswith(code))
                first, listing = client.multiline('LIST')
                self.assertTrue(first.startswith(b'215'))
                self.assertEqual(b'test ' in listing, read == 'a')
                if read == '-':
                    for command in ['ARTICLE', 'HEAD', 'BODY', 'STAT']:
                        self.assertEqual(client.command(command+' <1$test@JamNNTPd>'),
                                         b'430 No such article found\r\n')
                result = client.post(article())
                self.assertTrue(result.startswith(b'240' if post == 'a' else b'440'))
                if post == '-':
                    self.assertEqual(server.snapshot(), before)

    def test_raw_command_boundaries_fragmentation_and_coalescing(self):
        for length in [997, 998, 999]:
            with self.subTest(length=length), Server() as server, server.raw_client() as client:
                client.send(b'X'*length + b'\r\n')
                if length <= 998:
                    self.assertEqual(client.reply(500), b'500 Unknown command\r\n')
                    client.send(b'MODE READER\r\n')
                    client.reply(200)
                else:
                    self.assertEqual(client.socket.recv(1), b'')
        with Server() as server, server.raw_client() as client:
            for fragment in [b'G', b'RO', b'UP te', b'st\r', b'\n']:
                client.send(fragment)
            client.reply(211)
            client.send(b'STAT\r\nLAST\r\nNEXT\r\nMODE READER\r\n')
            for code in [223, 422, 421, 200]:
                client.reply(code)
            client.assert_quiet()
            client.send(b'MODE READER\n')
            client.reply(200)
        for bad in [b'MODE\x00 READER\r\n', b'X'*999+b'\r\nPOST\r\n']:
            with self.subTest(bad=bad[:20]), Server() as server, server.raw_client() as client:
                before = server.snapshot()
                client.send(bad)
                if b'\x00' in bad:
                    self.assertEqual(client.reply(500), b'500 Invalid command line\r\n')
                self.assertEqual(client.socket.recv(1), b'')
                self.assertEqual(server.snapshot(), before)

    def test_post_aggregate_boundaries_and_exact_dot_unstuffing(self):
        headers = article('').split(b'\r\n\r\n', 1)[0] + b'\r\n\r\n'
        for size in [19999, 20000, 20001]:
            body = bytearray()
            remaining = size-len(headers)
            while remaining:
                width = min(remaining, 902)
                if remaining-width == 1:
                    width -= 1
                body.extend(b'x'*(width-2)+b'\r\n')
                remaining -= width
            raw = headers + bytes(body)
            self.assertEqual(len(raw), size)
            with self.subTest(size=size), Server() as server, server.client() as client:
                self.assertTrue(client.login().startswith(b'281'))
                before = server.snapshot()
                response = client.post(raw)
                self.assertTrue(response.startswith(b'240' if size <= 20000 else b'441'))
                if size > 20000:
                    self.assertEqual(server.snapshot(), before)
        with Server() as server, server.raw_client() as client:
            client.send(b'POST\r\n')
            client.reply(340)
            client.send(headers + b'..one\r\n...two\r\n.\r\n')
            client.reply(240)
            self.assertIn(b'.one\r..two\r', server.message(3)[2])
            client.send(b'BODY <3$test@JamNNTPd>\r\n')
            response = client.reply(222, multiline=True)
            self.assertIn(b'..one\r\n...two\r\n', response)

    def test_eof_at_each_incomplete_post_phase_never_mutates(self):
        headers = article('').split(b'\r\n\r\n', 1)[0] + b'\r\n\r\n'
        for partial in [b'', b'From: Al', headers, headers+b'body\r\n', headers+b'.']:
            with self.subTest(partial=partial[-10:]), Server() as server:
                before = server.snapshot()
                with server.raw_client() as client:
                    client.send(b'POST\r\n')
                    client.reply(340)
                    client.send(partial)
                    client.socket.shutdown(socket.SHUT_WR)
                    self.assertEqual(client.socket.recv(1), b'')
                server.stop()
                self.assertEqual(server.snapshot(), before)

    def test_netmail_matrix_for_sender_recipient_and_unrelated_user(self):
        for strict in [False, True]:
            with self.subTest(strict=strict), Server(strict_netmail=strict) as server:
                with (server.root / 'users').open('a') as users:
                    users.write('eve password abc abc Eve\n')
                with server.client() as alice:
                    self.assertTrue(alice.login().startswith(b'281'))
                    self.assertEqual(alice.post(article('To: Bob, 2:1/3\r\n\r\nprivate sentinel',
                                                        group='private')), b'240 Article posted\r\n')
                before = server.snapshot('private')
                for name, password, visible in [('alice', 'secret', [1, 3]),
                                                 ('bob', 'password', [3]), ('eve', 'password', [])]:
                    with self.subTest(user=name), server.client() as client:
                        self.assertTrue(client.login(name, password).startswith(b'281'))
                        group = client.command('GROUP private')
                        count, low, high = (len(visible), min(visible, default=0), max(visible, default=0)) if strict else (3, 1, 3)
                        self.assertEqual(group, f'211 {count} {low} {high} private Group selected\r\n'.encode())
                        for number in [1, 2, 3]:
                            for command, code in [('ARTICLE', 220), ('HEAD', 221), ('BODY', 222), ('STAT', 223)]:
                                for selector in [str(number), f'<{number}$private@JamNNTPd>']:
                                    request = command+' '+selector
                                    if number in visible:
                                        if command == 'STAT':
                                            response = client.command(request)
                                        else:
                                            response, _ = client.multiline(request)
                                        self.assertTrue(response.startswith(str(code).encode()))
                                    else:
                                        expected = b'430' if selector.startswith('<') else b'423'
                                        self.assertTrue(client.command(request).startswith(expected))
                        first, overview = client.multiline('XOVER 1-3')
                        self.assertTrue(first.startswith(b'420' if strict and not visible else b'224'))
                        self.assertEqual([int(line.split(b'\t', 1)[0]) for line in overview.splitlines()], visible)
                        _, listing = client.multiline('LIST ACTIVE')
                        self.assertIn(f'private {high} {low} y\r\n'.encode(), listing)
                        if name != 'alice':
                            self.assertEqual(client.post(article('', group='private',
                                extra='Control: cancel <3$private@JamNNTPd>\r\n')),
                                b'441 Posting failed\r\n')
                self.assertEqual(server.snapshot('private'), before)

    def test_post_validation_and_transfer_encodings_preserve_storage_contract(self):
        with Server() as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            valid = article()
            rejected = [valid.replace(b'From: Alice <alice@example.org>\r\n', b''),
                        valid.replace(b'Newsgroups: test\r\n', b''),
                        valid.replace(b'Newsgroups: test', b'Newsgroups: test,local'),
                        valid.replace(b'text/plain', b'text/html'),
                        valid.replace(b'text/plain', b'multipart/mixed'),
                        article(extra='Control: unsupported\r\n'),
                        article(extra='References: <999$test@JamNNTPd>\r\n')]
            headers = article('').split(b'\r\n\r\n', 1)[0]
            rejected += [headers+b'\r\nContent-Transfer-Encoding: base64\r\n\r\n%%%\r\n',
                         headers+b'\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n=QZ\r\n']
            before = server.snapshot()
            for raw in rejected:
                with self.subTest(raw=raw[:70]):
                    self.assertEqual(client.post(raw), b'441 Posting failed\r\n')
                    self.assertEqual(server.snapshot(), before)
            for index, (transfer, encoded) in enumerate([
                    ('8bit', 'Н─│'.encode()), ('base64', base64.b64encode('Н─│'.encode())),
                    ('quoted-printable', quopri.encodestring('Н─│'.encode())), ('7bit', b'ASCII')], 3):
                wire = headers + f'\r\nContent-Transfer-Encoding: {transfer}\r\n\r\n'.encode() + encoded + b'\r\n'
                self.assertEqual(client.post(wire), b'240 Article posted\r\n')
                _, body = client.multiline(f'BODY <{index}$test@JamNNTPd>')
                expected = b'ASCII' if transfer == '7bit' else 'Н─│'.encode()
                self.assertIn(expected, body)
                stored = server.message(index)[2]
                self.assertIn(b'ASCII' if transfer == '7bit' else 'Н─│'.encode('cp866'), stored)
                info = jam_oracle.base_header(server.snapshot()['.jhr'][:1024])
                self.assertEqual(info[2], index-1)
            client.close()
            accepted = server.snapshot()
            server.restart()
            with server.client() as reopened:
                for number in range(3, 7):
                    self.assertTrue(reopened.command(f'STAT <{number}$test@JamNNTPd>').startswith(b'223'))
            self.assertEqual(server.snapshot(), accepted)

    def test_executable_cli_aliases_boundaries_and_immediate_create(self):
        with tempfile.TemporaryDirectory(prefix='jam-cli-') as folder:
            root = Path(folder)
            output = root / 'конфигурация with spaces.cfg'
            def run(*args):
                return subprocess.run([OPTIONS.executable, *map(str, args)], cwd=root,
                                      capture_output=True, timeout=4)
            cases = [(alias, value) for alias, value in [('p', '0'), ('p', '65535'),
                     ('m', '0'), ('m', '4294967295')]]
            cases += [(alias, root / 'путь with spaces') for alias in ['a', 'g', 'u', 'x', 'l']]
            for alias, value in cases:
                with self.subTest(alias=alias, value=value):
                    output.unlink(missing_ok=True)
                    result = run('-'+alias, value, '-create', output, '-unknown-after-create')
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('port 5000', output.read_text())
            for args in [('-p', '65536'), ('-m', '4294967296'), ('-p', '-1'),
                         ('-m', '1junk'), ('-unknown', 'x'), ('-p',), ('-config',), ('-create',)]:
                with self.subTest(arguments=args):
                    output.unlink(missing_ok=True)
                    self.assertNotEqual(run(*args).returncode, 0)
                    self.assertFalse(output.exists())
            for help_option in ['?', '-h', '--help']:
                self.assertEqual(run(help_option).returncode, 0)
            self.assertNotEqual(run('-help').returncode, 0)
            self.assertNotEqual(run('-config', root/'missing.cfg').returncode, 0)

    def test_cancel_attribute_policy_and_repeated_cancel(self):
        for attributes, allowed in [(0x01000001, True), (0x01000011, False),
                                    (0x41000001, False), (0x01000000, False)]:
            with self.subTest(attributes=attributes), \
                 Server(recipe={'messages': 1, 'attributes': attributes}) as server, server.client() as client:
                self.assertTrue(client.login().startswith(b'281'))
                before = server.snapshot()
                control = article('', extra='Control: cancel <1$test@JamNNTPd>\r\n')
                response = client.post(control)
                self.assertEqual(response, b'240 Article cancelled\r\n' if allowed else b'441 Posting failed\r\n')
                if allowed:
                    self.assertEqual(jam_oracle.indices(server.snapshot()['.jdx'])[0][1], 0xffffffff)
                    cancelled = server.snapshot()
                    self.assertEqual(client.post(control), b'441 Posting failed\r\n')
                    self.assertEqual(server.snapshot(), cancelled)
                else:
                    self.assertEqual(server.snapshot(), before)
        with Server(extra='nocancel\n') as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            self.assertTrue(client.post(article()).startswith(b'240'))
            before = server.snapshot()
            self.assertEqual(client.post(article('', extra='Control: cancel <3$test@JamNNTPd>\r\n')),
                             b'441 Posting failed\r\n')
            self.assertEqual(server.snapshot(), before)

    def test_reply_chain_cancel_middle_first_last_over_tcp(self):
        with Server() as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            self.assertEqual(client.post(article('root')), b'240 Article posted\r\n')
            for number in [4, 5, 6]:
                self.assertEqual(client.post(article(f'reply {number}',
                    extra='References: <3$test@JamNNTPd>\r\n')), b'240 Article posted\r\n')
                self.assertEqual(server.message(number)[0][4], 3)
            self.assertEqual(server.message(3)[0][5], 4)
            self.assertEqual(server.message(4)[0][6], 5)
            self.assertEqual(server.message(5)[0][6], 6)
            for cancelled, first_reply in [(5, 4), (4, 6), (6, 0)]:
                self.assertEqual(client.post(article('',
                    extra=f'Control: cancel <{cancelled}$test@JamNNTPd>\r\n')),
                    b'240 Article cancelled\r\n')
                self.assertEqual(server.message(3)[0][5], first_reply)
                self.assertEqual(jam_oracle.indices(server.snapshot()['.jdx'])[cancelled-1][1], 0xffffffff)
            self.assertEqual(jam_oracle.base_header(server.snapshot()['.jhr'][:1024])[2], 2)

    def test_every_user_option_default_and_login_override_changes_observable_output(self):
        for option in ['flowed', 'showto', 'nonbsp', 'delssq', 'addcr', 'squote']:
            for enabled in [False, True]:
                for source in ['default', 'login']:
                    setting = 'on' if enabled else 'off'
                    extra = f'def_{option} {setting}\n' if source == 'default' else ''
                    if source == 'default' and option == 'squote':
                        extra = 'smartquote\n' if enabled else ''
                    with self.subTest(option=option, setting=setting, source=source), \
                         Server(extra=extra) as server, server.client() as client:
                        username = 'alice' + (f'/{option}={setting}' if source == 'login' else '')
                        self.assertTrue(client.login(username).startswith(b'281'))
                        body = {'nonbsp': ' Н', 'delssq': '>  quoted', 'squote': '> text'}.get(option, 'Body')
                        raw = article(body, extra='References: <1$test@JamNNTPd>\r\n' if option == 'squote' else '')
                        if option == 'delssq':
                            raw = raw.replace(b'charset=UTF-8', b'charset=UTF-8; format=flowed')
                        self.assertEqual(client.post(raw), b'240 Article posted\r\n')
                        stored = server.message(3)[2]
                        _, headers = client.multiline('HEAD <3$test@JamNNTPd>')
                        if option == 'flowed':
                            self.assertIn(b'format=flowed' if enabled else b'format=fixed', headers)
                        elif option == 'showto':
                            from_line = next(line for line in headers.splitlines() if line.startswith(b'From:'))
                            self.assertEqual(b' -> All' in from_line, enabled)
                        elif option == 'nonbsp':
                            self.assertTrue(stored.startswith(b' \x8d' if enabled else b'\xff\x8d'))
                        elif option == 'delssq':
                            self.assertTrue(stored.startswith(b'> quoted\r' if enabled else b'>  quoted\r'))
                        elif option == 'addcr':
                            self.assertTrue(stored.startswith(b'Body\r\r---' if enabled else b'Body\r---'))
                        elif option == 'squote':
                            self.assertTrue(stored.startswith(b' A> text\r' if enabled else b'> text\r'))

    def test_boolean_flags_pairwise_have_real_wire_storage_and_log_effects(self):
        names = ['debug', 'noecholog', 'nostripre', 'notearline', 'nocancel',
                 'strictnetmail', 'readorigin', 'noreplyaddr', 'noencode', 'notzutc',
                 'loggroups', 'note', 'smartquote']
        combinations = [tuple(bool((row & column).bit_count() % 2) for column in range(1, 14))
                        for row in range(16)]
        # The 16-row binary orthogonal array covers each pair's four settings.
        for left in range(len(names)):
            for right in range(left):
                self.assertEqual({(row[left], row[right]) for row in combinations},
                                 {(False, False), (False, True), (True, False), (True, True)})
        for settings in combinations:
            enabled = {name for name, state in zip(names, settings) if state}
            with self.subTest(flags=sorted(enabled)), \
                 Server(flags=sorted(enabled), extra=lambda root: f'origin "Test origin"\nechomailjam "{root / "notify"}"\n') as server, server.client() as client:
                self.assertTrue(client.login().startswith(b'281'))
                self.assertTrue(client.command('GROUP test').startswith(b'211'))
                self.assertEqual(client.post(article(subject='Re: Тема', extra='User-Agent: TestReader\r\n')),
                                 b'240 Article posted\r\n')
                _, fields, stored, kludges = server.message(3)
                subject = ('Re: Тема' if 'nostripre' in enabled else 'Тема').encode('cp866')
                self.assertEqual(fields[6], subject)
                self.assertEqual(any(line.startswith(b'REPLYADDR ') for line in kludges), 'noreplyaddr' not in enabled)
                self.assertEqual(any(line.startswith(b'TZUTC: ') for line in kludges), 'notzutc' not in enabled)
                self.assertEqual(b'NOTE: TestReader' in kludges, 'note' in enabled)
                self.assertIn(b'---\r' if 'notearline' in enabled else b'--- TestReader\r', stored)
                self.assertIn(b' * Origin: Test origin (2:1/2)\r', stored)
                self.assertIn(b'CHRS: CP866 2', kludges)
                self.assertTrue(fields[4])  # independent stored MSGID
                self.assertEqual(client.post(article('> quoted', extra='References: <3$test@JamNNTPd>\r\n')),
                                 b'240 Article posted\r\n')
                self.assertTrue(server.message(4)[2].startswith(b' A> quoted\r' if 'smartquote' in enabled else b'> quoted\r'))
                _, headers = client.multiline('HEAD <3$test@JamNNTPd>')
                subject_line = next(line for line in headers.splitlines() if line.startswith(b'Subject:'))
                self.assertEqual(b'=?' in subject_line, 'noencode' not in enabled)
                self.assertEqual(client.post(article('', extra='Control: cancel <3$test@JamNNTPd>\r\n')),
                                 b'441 Posting failed\r\n' if 'nocancel' in enabled else b'240 Article cancelled\r\n')
                client.close()
                server.stop()
                self.assertIn('test 3', (server.root/'notify').read_text())
                log = (server.root/'log').read_text()
                self.assertEqual('AUTHINFO [redacted]' in log, 'debug' in enabled)
                self.assertEqual('Group selected: test' in log, 'loggroups' in enabled)
                self.assertEqual(bool((server.root/'stdout').read_bytes()), 'noecholog' not in enabled)

    def test_readorigin_guest_suffix_and_local_nochrs_group_flags(self):
        recipe = {'messages': 1, 'origin': '2:1/2',
                  'body': 'Body\r * Origin: Named (2:9/9)\r'}
        for enabled in [False, True]:
            with self.subTest(readorigin=enabled), Server(recipe=recipe,
                 extra='readorigin\n' if enabled else '') as server, server.client() as client:
                _, headers = client.multiline('HEAD <1$test@JamNNTPd>')
                from_line = next(line for line in headers.splitlines() if line.startswith(b'From:'))
                self.assertIn(b'<0@9.9.2>' if enabled else b'<0@2.1.2>', from_line)
        with Server(extra='guestsuffix " [Guest]"\n') as server, server.client() as client:
            self.assertEqual(client.post(article()), b'240 Article posted\r\n')
            self.assertEqual(server.message(3)[1][2], b'Alice [Guest]')
        with Server() as server:
            groups = server.root/'groups'
            groups.write_text(groups.read_text().replace('!CP866', '!CP866 -nochrs'))
            with server.client() as client:
                self.assertTrue(client.login().startswith(b'281'))
                self.assertEqual(client.post(article(group='local')), b'240 Article posted\r\n')
                fixed, _, body, kludges = server.message(3, 'local')
                self.assertTrue(fixed[11] & 0x00800000)
                self.assertFalse(any(value.startswith(b'CHRS:') for value in kludges))
                self.assertNotIn(b' * Origin:', body)

    def test_subject_final_byte_boundary_and_independent_mime_decode(self):
        for length in [70, 71, 72]:
            with self.subTest(length=length), Server() as server, server.client() as client:
                self.assertTrue(client.login().startswith(b'281'))
                before = server.snapshot()
                response = client.post(article(subject='Н'*length))
                if length > 71:
                    self.assertEqual(response, b'441 Posting failed\r\n')
                    self.assertEqual(server.snapshot(), before)
                else:
                    self.assertEqual(response, b'240 Article posted\r\n')
                    self.assertEqual(server.message(3)[1][6], b'\x8d'*length)
                    _, headers = client.multiline('HEAD <3$test@JamNNTPd>')
                    message = email.message_from_bytes(headers)
                    parts = email.header.decode_header(message['Subject'])
                    decoded = ''.join(data.decode(charset or 'ascii') if isinstance(data, bytes) else data
                                      for data, charset in parts)
                    self.assertEqual(decoded, 'Н'*length)
                    for word in re.findall(rb'=\?[^?]+\?[BQ]\?[^?]*\?=', headers):
                        self.assertLessEqual(len(word), 75)

    def test_storage_lock_timeout_is_503_and_article_recovers_without_mutation(self):
        with Server() as server, server.raw_client() as client:
            before = server.snapshot()
            client.timeout = 8
            with (server.root/'test.jhr').open('r+b', buffering=0) as header, byte_lock(header):
                began = time.monotonic()
                client.send(b'STAT <1$test@JamNNTPd>\r\n')
                self.assertEqual(client.reply(503), b'503 Local error: Could not access messagebase\r\n')
                self.assertGreaterEqual(time.monotonic()-began, 4.5)
                self.assertLess(time.monotonic()-began, 8)
            self.assertEqual(server.snapshot(), before)
            client.send(b'STAT <1$test@JamNNTPd>\r\n')
            self.assertEqual(client.reply(223), b'223 1 <1$test@JamNNTPd> Article retrieved\r\n')

    def test_truncated_text_base_rejects_post_without_reviving_old_article(self):
        with Server() as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            (server.root/'test.jdt').write_bytes(b'')
            self.assertEqual(client.command('STAT <1$test@JamNNTPd>'),
                             b'503 Local error: Could not access messagebase\r\n')
            before = server.snapshot()
            self.assertEqual(client.post(article('Replacement payload')),
                             b'503 Local error: Could not access messagebase\r\n')
            self.assertEqual(server.snapshot(), before)
            self.assertEqual(client.command('STAT <1$test@JamNNTPd>'),
                             b'503 Local error: Could not access messagebase\r\n')

    def test_alias_automatic_conversion_and_forced_group_precedence_over_tcp(self):
        recipe = {'messages': 1, 'encodings': ['cp1251'], 'labels': ['PRIVATE1251']}
        for forced in [False, True]:
            with self.subTest(forced=forced), Server(recipe=recipe, force_charset=forced,
                 xlat='readalias PRIVATE1251 WINDOWS-1251\n') as server, server.client() as client:
                before = server.snapshot()
                first, body = client.multiline('BODY <1$test@JamNNTPd>')
                self.assertTrue(first.startswith(b'222'))
                expected = 'Привет' if not forced else 'Привет'.encode('cp1251').decode('cp866')
                self.assertIn(expected.encode(), body)
                self.assertEqual(server.snapshot(), before)
        with Server(recipe=recipe, force_charset=False, xlat='') as server, server.client() as client:
            before = server.snapshot()
            self.assertEqual(client.command('BODY <1$test@JamNNTPd>'), b'430 No such article found\r\n')
            self.assertEqual(server.snapshot(), before)

    def test_admission_at_max_minus_one_max_and_max_plus_one(self):
        for maximum in [0, 1, 2, 8]:
            with self.subTest(maximum=maximum), Server(extra=f'max {maximum}\n') as server:
                with contextlib.ExitStack() as stack:
                    accepted = []
                    for _ in range(maximum):
                        client = stack.enter_context(server.client())
                        self.assertTrue(client.greeting.startswith(b'200'))
                        accepted.append(client)
                    with server.client() as rejected:
                        self.assertTrue(rejected.greeting.startswith(b'502'))
                    for client in accepted:
                        self.assertEqual(client.command('QUIT'), b'205 Goodbye\r\n')
                if maximum:
                    deadline = time.monotonic()+2
                    while True:
                        with server.client() as replacement:
                            if replacement.greeting.startswith(b'200'):
                                break
                        self.assertLess(time.monotonic(), deadline, 'Session lease did not release')
                        time.sleep(.01)

    def test_parallel_writes_to_one_and_two_bases_keep_records_and_values_distinct(self):
        for second_group in ['test', 'local']:
            with self.subTest(second_group=second_group), Server() as server, \
                 server.client() as first, server.client() as second:
                self.assertTrue(first.login().startswith(b'281'))
                self.assertTrue(second.login().startswith(b'281'))
                gate = threading.Barrier(2)
                def post(client, group, prefix):
                    for index in range(4):
                        gate.wait(timeout=4)
                        result = client.post(article(f'{prefix}-{index}', group=group))
                        if result != b'240 Article posted\r\n':
                            raise AssertionError(result)
                with ThreadPoolExecutor(max_workers=2) as pool:
                    tasks = [pool.submit(post, first, 'test', 'first'),
                             pool.submit(post, second, second_group, 'second')]
                    for task in tasks:
                        task.result(timeout=12)
                messages, identifiers = [], []
                for group, count in [('test', 8 if second_group == 'test' else 4),
                                     ('local', 0 if second_group == 'test' else 4)]:
                    snapshot = server.snapshot(group)
                    self.assertEqual(len(jam_oracle.indices(snapshot['.jdx'])), 2+count)
                    self.assertEqual(jam_oracle.base_header(snapshot['.jhr'][:1024])[2], 1+count)
                    for number in range(3, 3+count):
                        _, fields, body, _ = server.message(number, group)
                        messages.append(body.split(b'\r', 1)[0])
                        identifiers.append(fields[4])
                self.assertEqual(set(messages), {f'{name}-{i}'.encode() for name in ['first', 'second'] for i in range(4)})
                self.assertEqual(len(set(identifiers)), 8)

    def test_native_shutdown_in_connection_read_post_and_external_lock_phases(self):
        signals = [signal.CTRL_BREAK_EVENT] if os.name == 'nt' else [signal.SIGINT, signal.SIGTERM]
        for termination in signals:
            for phase in ['connection', 'read', 'post', 'lock']:
                with self.subTest(signal=termination, phase=phase), Server() as server:
                    before = server.snapshot()
                    with (server.root/'test.jhr').open('r+b', buffering=0) as header:
                        with byte_lock(header) if phase == 'lock' else contextlib.nullcontext():
                            if phase == 'connection':
                                connection = socket.create_connection(('127.0.0.1', server.port), timeout=4)
                                client = RawClient(connection)
                            else:
                                client = server.raw_client()
                            try:
                                if phase == 'read':
                                    client.send(b'GROUP test\r\nHEAD 1\r\n')
                                elif phase in ['post', 'lock']:
                                    client.send(b'POST\r\n')
                                    client.reply(340)
                                    client.send(article('uncommitted') + (b'.\r\n' if phase == 'lock' else b''))
                                    if phase == 'lock':
                                        client.assert_quiet(.05)
                                beginning = time.monotonic()
                                server.process.send_signal(termination)
                                server.process.wait(timeout=8)
                                self.assertLess(time.monotonic()-beginning, 8)
                                server.stop()
                            finally:
                                client.close()
                    self.assertEqual(server.snapshot(), before)
                    self.assertIn('All workers joined; server stopped', (server.root/'log').read_text())

    def test_independent_corpus_variants_over_real_tcp(self):
        for name, recipe in json.loads(jam_corpus.RECIPES.read_text()).items():
            with self.subTest(recipe=name), Server(recipe=recipe, force_charset=False) as server, server.raw_client() as client:
                before = server.snapshot()
                client.send(b'GROUP test\r\n')
                client.reply(211)
                for number in range(1, recipe['messages']+1):
                    client.send(f'ARTICLE {number}\r\n'.encode())
                    if number in recipe.get('deleted', []):
                        client.reply(423)
                    else:
                        response = client.reply(220, multiline=True)
                        self.assertIn(f'Corpus {number}: Привет'.encode(), response)
                        self.assertIn(b'..line\r\n', response)
                self.assertEqual(server.snapshot(), before)
                client.send(b'QUIT\r\n')
                client.reply(205)
                self.assertEqual(client.socket.recv(1), b'')

    def test_independent_tosser_and_consistent_backup_restore(self):
        with Server() as server:
            server.stop()
            backup = server.snapshot()
            subprocess.run([sys.executable, str(Path(__file__).with_name('fake_tosser.py')),
                            str(server.root / 'test')], check=True, timeout=5)
            self.assertIn('Н─│'.encode('cp866'), server.message(3)[2])
            imported_lastread = backup['.jlr'] + b''.join(value.to_bytes(4, 'little')
                for value in [0x12345678, 42, 3, 3])
            (server.root/'test.jlr').write_bytes(imported_lastread)
            # Start the same executable/config against the independent imported files.
            restart = server.restart
            restart()
            with server.client() as client:
                self.assertTrue(client.command('GROUP test').startswith(b'211 3 1 3'))
                first, text = client.multiline('ARTICLE 3')
                self.assertTrue(first.startswith(b'220'))
                self.assertIn('Independent tosser: Н─│'.encode(), text)
                self.assertTrue(client.login().startswith(b'281'))
                self.assertTrue(client.post(article('after import')).startswith(b'240'))
                self.assertIn(b'after import', server.message(4)[2])
                self.assertEqual((server.root/'test.jlr').read_bytes(), imported_lastread)
                self.assertEqual([jam_oracle.uint(imported_lastread[-16:], i, 4)
                                  for i in range(0, 16, 4)], [0x12345678, 42, 3, 3])
            server.stop()
            for suffix, data in backup.items():
                (server.root / ('test' + suffix)).write_bytes(data)
            restart()
            with server.client() as client:
                self.assertTrue(client.command('GROUP test').startswith(b'211 2 1 2'))
                self.assertTrue(client.command('STAT 3').startswith(b'423'))
            self.assertEqual(server.snapshot(), backup)

    def test_admission_and_configuration_failures_are_bounded(self):
        with Server(extra='max 1\n') as server, server.client() as admitted:
            with server.client() as rejected:
                self.assertTrue(rejected.greeting.startswith(b'502'))
            self.assertTrue(admitted.command('HELP').startswith(b'100'))
        with Server(allow='10.0.0.1 a a\n') as server, server.client() as client:
            self.assertTrue(client.greeting.startswith(b'502'))
        for filename in ['groups','xlat']:
            with Server() as server:
                (server.root / filename).unlink()
                with server.client() as client:
                    self.assertTrue(client.greeting.startswith(b'503'))
        with tempfile.TemporaryDirectory(prefix='jamnntpd-startup-') as temporary:
            root = Path(temporary)
            unicode_config = root / 'конфигурация.config'
            created = subprocess.run([OPTIONS.executable,'-create',str(unicode_config)],
                capture_output=True,timeout=5)
            self.assertEqual(created.returncode,0,created.stderr)
            self.assertTrue(unicode_config.is_file())
            for arguments in [['-logfile',str(root / 'missing' / 'log')],
                              ['-create',str(root / 'missing' / 'config')]]:
                result = subprocess.run([OPTIONS.executable,*arguments],capture_output=True,timeout=5)
                self.assertNotEqual(result.returncode,0)
            with socket.socket() as occupied:
                occupied.bind(('0.0.0.0',0))
                occupied.listen()
                result = subprocess.run([OPTIONS.executable,'-port',str(occupied.getsockname()[1]),
                    '-logfile',str(root / 'log'),'-noecholog'],capture_output=True,timeout=5)
                self.assertNotEqual(result.returncode,0)

    def test_ibmpc_codepage_and_textual_diagnostic_headers(self):
        rules = 'defaultpost UTF-8\nread IBMPC,866 IBMPC,866\npost UTF-8 IBMPC,866 -builtin\n'
        with Server(extra='note\n',charset='IBMPC,866',xlat=rules) as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            self.assertTrue(client.post(article(extra='X-Newsreader: Reader Н\r\n')).startswith(b'240'))
            self.assertIn(b'CODEPAGE: 866',server.message(3)[3])
            self.assertTrue(client.command('GROUP test').startswith(b'211'))
            first,data = client.multiline('ARTICLE 3')
            self.assertTrue(first.startswith(b'220'))
            message = email.message_from_bytes(data)
            self.assertEqual(message.get_content_charset(),'cp866')
            self.assertIn('Н─│'.encode('cp866'),message.get_payload(decode=True))
            diagnostics = [str(email.header.make_header(email.header.decode_header(value)))
                           for value in message.get_all('X-JAM-FTSKLUDGE',[])]
            self.assertIn('NOTE: Reader Н',diagnostics)

    def test_read_post_reply_cancel_and_independent_storage(self):
        with Server() as server, server.client() as client:
            self.assertTrue(client.greeting.startswith(b'200'))
            self.assertTrue(client.command('GROUP test').startswith(b'211 2 1 2'))
            first, data = client.multiline('ARTICLE 1')
            self.assertTrue(first.startswith(b'220'))
            self.assertIn(b'Hello\r\n.World\r\n', data)
            self.assertTrue(client.login().startswith(b'281'))
            self.assertEqual(client.post(article()), b'240 Article posted\r\n')
            fixed, fields, body, kludges = server.message(3)
            self.assertEqual(fields[2], b'Alice')
            self.assertEqual(fields[6], b'Topic')
            self.assertIn('Н─│'.encode('cp866'), body)
            self.assertIn(b'CHRS: CP866 2', kludges)
            self.assertEqual(client.post(article('reply', extra='References: <3$test@JamNNTPd>\r\n')), b'240 Article posted\r\n')
            self.assertEqual(server.message(3)[0][5], 4)
            self.assertEqual(server.message(4)[0][4], 3)
            first, data = client.multiline('ARTICLE 3')
            self.assertTrue(first.startswith(b'220'))
            self.assertIn('Н─│'.encode(), data)
            self.assertEqual(client.post(article('', extra='Control: cancel <3$test@JamNNTPd>\r\n')), b'240 Article cancelled\r\n')
            self.assertTrue(client.command('STAT 3').startswith(b'423'))
            self.assertEqual(server.message(4)[0][4], 3) # stored ancestry is retained
            self.assertTrue(client.command('QUIT').startswith(b'205'))
    def test_strict_failures_never_mutate_and_debug_never_contains_credentials(self):
        sentinel = 'UNIQUE-PASSWORD-SENTINEL-84721'
        with Server() as server:
            with server.client() as client:
                self.assertTrue(client.login(password=sentinel).startswith(b'481'))
                self.assertTrue(client.login().startswith(b'281'))
                before = server.snapshot()
                for raw in [article('😀'), article(group='test,hidden'), article(group='hidden'), article('x').replace(b'Content-Type: text/plain', b'Content-Type: text/html'), article('x').replace(b'x\r\n', b'\xff\r\n')]:
                    self.assertTrue(client.post(raw).startswith(b'441'), raw)
                    self.assertEqual(server.snapshot(), before)
            server.stop()
            for name in ['log','stdout','stderr']:
                self.assertNotIn(sentinel, (server.root / name).read_text())
            self.assertIn('AUTHINFO [redacted]', (server.root / 'log').read_text())
    def test_netmail_visibility_and_cancellation_ownership(self):
        with Server() as server:
            with server.client() as alice:
                self.assertTrue(alice.command('GROUP private').startswith(b'411'))
                self.assertTrue(alice.login().startswith(b'281'))
                self.assertEqual(alice.post(article('To: Bob, 2:1/3\r\n\r\nprivate text', group='private')), b'240 Article posted\r\n')
                self.assertEqual(alice.post(article()), b'240 Article posted\r\n')
            with server.client() as bob:
                self.assertTrue(bob.login('bob', 'password').startswith(b'281'))
                self.assertTrue(bob.command('GROUP private').startswith(b'211 1 3 3'))
                self.assertTrue(bob.command('STAT 1').startswith(b'423'))
                self.assertTrue(bob.command('STAT <1$private@JamNNTPd>').startswith(b'430'))
                self.assertTrue(bob.command('STAT 3').startswith(b'223'))
                before = server.snapshot()
                self.assertTrue(bob.post(article('', extra='Control: cancel <3$test@JamNNTPd>\r\n')).startswith(b'441'))
                self.assertEqual(server.snapshot(), before)
    def test_standard_encodings_read_post_reply_and_special_symbols(self):
        cases = [('CP437','cp437','é£µ─│'), ('CP850','cp850','é£µ─│'),
                 ('CP865','cp865','é£µ─│'), ('CP866','cp866','Ня─│ '),
                 ('KOI8-R','koi8_r','Ня─│'), ('windows-1251','cp1251','Ня№ '),
                 ('windows-1252','cp1252','é€£ '), ('ISO-8859-1','latin1','é£µ '),
                 ('ISO-8859-15','iso8859_15','é€Œ '), ('UTF-8','utf8','Н─│é€😀 ')]
        for charset, codec, text in cases:
            with self.subTest(charset=charset), Server(charset=charset) as server, server.client() as client:
                self.assertTrue(client.login().startswith(b'281'))
                self.assertEqual(client.post(article(text, subject=text)), b'240 Article posted\r\n')
                fixed, fields, body, kludges = server.message(3)
                self.assertEqual(fields[6], text.encode(codec))
                self.assertIn(text.encode(codec), body)
                self.assertIn(('CHRS: ' + charset + (' 4' if charset == 'UTF-8' else ' 2')).encode(), kludges)
                first, data = client.multiline('ARTICLE <3$test@JamNNTPd>')
                self.assertTrue(first.startswith(b'220'))
                self.assertIn(text.encode(), data)
                parsed = email.message_from_bytes(data)
                subject = ''.join(part.decode(encoding or 'ascii') if isinstance(part, bytes) else part
                                  for part,encoding in email.header.decode_header(parsed['Subject']))
                self.assertEqual(subject, text)
                self.assertEqual(client.post(article(text, extra='References: <3$test@JamNNTPd>\r\n')), b'240 Article posted\r\n')
                self.assertEqual(server.message(4)[0][4], 3)
    def test_custom_chs_and_legacy_label_only_rules(self):
        root = Path(__file__).resolve().parents[2]
        read_table = OPTIONS.fixtures / 'chs/cp866-to-koi8.chs'
        post_table = root / 'xlat/koi_866.chs'
        xlat = f'read CP866 koi8-r "{read_table}" -keepsoftcr\npost koi8-r CP866 "{post_table}"\n'
        with Server(xlat=xlat) as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            raw = article('Ня').decode().replace('charset=UTF-8', 'charset=koi8-r').encode('koi8_r')
            self.assertEqual(client.post(raw), b'240 Article posted\r\n')
            self.assertIn('Ня'.encode('cp866'), server.message(3)[2])
            first, body = client.multiline('BODY <3$test@JamNNTPd>')
            self.assertTrue(first.startswith(b'222'))
            self.assertIn('Ня'.encode('koi8_r'), body)
            raw_reply = raw.replace(b'From:', b'References: <3$test@JamNNTPd>\r\nFrom:', 1)
            self.assertEqual(client.post(raw_reply), b'240 Article posted\r\n')
        xlat = 'read CP866 UTF-8\npost UTF-8 CP866\n'
        with Server(xlat=xlat) as server, server.client() as client:
            # Explicit legacy rules relabel bytes; they must not trigger a standard conversion.
            self.assertTrue(client.login().startswith(b'281'))
            self.assertEqual(client.post(article('Н')), b'240 Article posted\r\n')
            self.assertIn('Н'.encode('utf8'), server.message(3)[2])
    def test_replacement_flowed_nbsp_and_rejected_incomplete_post(self):
        with Server(extra='conversion_errors replace\ndef_nonbsp on\n') as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            raw = article(' Н \r\nnext 😀', extra='').replace(b'charset=UTF-8', b'charset=UTF-8; format=flowed')
            self.assertEqual(client.post(raw), b'240 Article posted\r\n')
            self.assertIn('Н next ?'.encode('cp866'), server.message(3)[2])
            before = server.snapshot()
            self.assertTrue(client.command('POST').startswith(b'340'))
            client.socket.sendall(b'From: unfinished\r\n')
            client.close()
            server.stop()
            self.assertIn('replaced unrepresentable', (server.root / 'log').read_text() if (server.root / 'log').exists() else '')
            self.assertEqual(server.snapshot(), before)
    def test_cli_and_startup_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for alias in ['?', '-h', '--help']:
                result = subprocess.run([OPTIONS.executable, alias], capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0)
                self.assertIn(b'Usage: jamnntpd', result.stdout)
            created = root / 'new.cfg'
            result = subprocess.run([OPTIONS.executable, '-create', str(created)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertIn('port 5000', created.read_text())
            for args in [['-port', '-1'], ['-port', '65536'], ['-max', '-1'], ['-port'],
                         ['-config', str(root / 'missing')], ['-create', str(root / 'absent/file')],
                         ['-logfile', str(root)]]:
                result = subprocess.run([OPTIONS.executable, *args], capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0, args)
    def test_protocol_limits_disconnect_without_injected_commands(self):
        with Server() as server:
            before = server.snapshot()
            with server.client() as client:
                client.socket.sendall(b'A' * 1001 + b'\r\nPOST\r\n')
                with self.assertRaises((EOFError, ConnectionError, OSError)):
                    client.line()
            with server.client() as client:
                self.assertTrue(client.command('POST').startswith(b'340'))
                try:
                    client.socket.sendall(article('line\r\n' * 5000) + b'.\r\nPOST\r\n')
                    response = client.line()
                    self.assertTrue(response.startswith(b'441'))
                except (EOFError, ConnectionError, OSError): pass
            with server.client() as client:
                self.assertTrue(client.command('GROUP test').startswith(b'211 2 1 2'))
            self.assertEqual(server.snapshot(), before)
    def test_redaction_on_success_and_failure_in_file_and_console(self):
        with Server(console=True) as server, server.client() as client:
            success = 'SUCCESS-PASSWORD-SENTINEL-94173'
            failed = 'FAILED-PASSWORD-SENTINEL-94174'
            (server.root / 'users').write_text(f'alice {success} abc abc Alice\n')
            self.assertTrue(client.login(password=failed).startswith(b'481'))
            self.assertTrue(client.login(password=success).startswith(b'281'))
            self.assertTrue(client.command('GROUP test').startswith(b'211'))
            self.assertTrue(client.command('QUIT').startswith(b'205'))
            server.stop()
            for name in ['log', 'stdout', 'stderr']:
                text = (server.root / name).read_text()
                self.assertNotIn(success, text)
                self.assertNotIn(failed, text)
            text = (server.root / 'log').read_text()
            self.assertIn('AUTHINFO [redacted]', text)
            self.assertIn('Group selected: test', text)
            self.assertIn('All workers joined', text)
    @unittest.skipUnless(Path('/dev/full').exists(), 'native /dev/full unavailable; injected sink test remains mandatory')
    def test_full_runtime_sink_has_bounded_fallback_and_keeps_serving(self):
        with Server(extra='logfile /dev/full\n', console=True) as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            self.assertTrue(client.command('STAT <1$test@JamNNTPd>').startswith(b'223'))
            server.stop()
            self.assertEqual((server.root / 'stderr').read_text().count('JamNNTPd: logging destination failed'), 1)
    def test_post_storage_failure_is_503_and_rejected_article_is_441(self):
        with Server() as server, server.client() as client:
            self.assertTrue(client.login().startswith(b'281'))
            text_path = server.root / 'test.jdt'
            saved_text = text_path.read_bytes()
            text_path.unlink()
            remaining = {suffix: (server.root / ('test' + suffix)).read_bytes()
                         for suffix in ['.jhr', '.jdx', '.jlr']}
            self.assertEqual(client.post(article()),
                             b'503 Local error: Could not access messagebase\r\n')
            self.assertFalse(text_path.exists())
            self.assertEqual(remaining, {suffix: (server.root / ('test' + suffix)).read_bytes()
                                       for suffix in remaining})
            text_path.write_bytes(saved_text)
            before = server.snapshot()
            self.assertEqual(client.post(article(extra='Control: unsupported\r\n')),
                             b'441 Posting failed\r\n')
            self.assertEqual(server.snapshot(), before)
            self.assertEqual(client.post(article()), b'240 Article posted\r\n')

    def test_protocol_characterization_transcripts(self):
        with Server(allow='127.0.0.1 "" ""\n') as server, server.client() as client:
            transcripts = json.loads((OPTIONS.fixtures / 'protocol/session.json').read_text())
            for case in transcripts:
                request = case['request'].strip()
                expected = case['response'].encode()
                if request == 'QUIT': continue
                if request.startswith('AUTHINFO PASS secret'):
                    self.assertTrue(client.command('AUTHINFO USER alice').startswith(b'381'))
                if expected.endswith(b'.\r\n'):
                    first, body = client.multiline(request)
                    actual = first + body + b'.\r\n'
                else: actual = client.command(request)
                self.assertEqual(actual, expected, case)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--junit', type=Path)
    OPTIONS, rest = parser.parse_known_args()
    program = unittest.main(argv=[__file__, *rest], exit=False,
                            testRunner=unittest.TextTestRunner(resultclass=JUnitResult))
    if OPTIONS.junit is not None:
        program.result.write(OPTIONS.junit)
    sys.exit(0 if program.result.wasSuccessful() else 1)
