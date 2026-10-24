"""Real executable NNTP tests against disposable JAM bases; no production data."""
import argparse
import contextlib
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
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'baseline'))
import jam_oracle

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
    def __init__(self, extra='', xlat=None, allow='127.0.0.1 ac ac\n', charset='CP866', console=False):
        self.temporary = tempfile.TemporaryDirectory(prefix='jamnntpd-e2e-')
        self.root = Path(self.temporary.name)
        for name in ['test', 'private', 'local', 'hidden']:
            for suffix in ['.jhr', '.jdt', '.jdx', '.jlr']:
                shutil.copyfile(OPTIONS.fixtures / 'jam' / ('sample' + suffix), self.root / (name + suffix))
        (self.root / 'groups').write_text(''.join(f'{prefix}{name} {access} 2:1/2 "{self.root / name}" !{charset}\n'
            for prefix,name,access in [('', 'test', 'a'), ('!', 'private', 'b'), ('$', 'local', 'c'), ('', 'hidden', 'd')]))
        (self.root / 'allow').write_text(allow)
        (self.root / 'users').write_text('alice secret abc abc Alice\nbob password abc abc Bob\n')
        (self.root / 'xlat').write_text(xlat if xlat is not None else f'defaultpost UTF-8\nread {charset} UTF-8 -builtin\npost UTF-8 {charset} -builtin\n')
        self.config = self.root / 'config'
        self.config.write_text('port 0\nmax 8\ndebug\nloggroups\nstrictnetmail\n' + ('' if console else 'noecholog\n') +
            ''.join(f'{option} "{self.root / value}"\n' for option,value in [('groups','groups'),('allow','allow'),('users','users'),('xlat','xlat'),('logfile','log')]) + extra)
        self.stdout = (self.root / 'stdout').open('wb')
        self.stderr = (self.root / 'stderr').open('wb')
        options = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {}
        self.process = subprocess.Popen([OPTIONS.executable, '-config', str(self.config)], stdout=self.stdout, stderr=self.stderr, **options)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None: raise RuntimeError((self.root / 'stderr').read_text())
            log = (self.root / 'log').read_text() if (self.root / 'log').exists() else ''
            log += (self.root / 'stdout').read_text(errors='replace')
            ready = re.search(r'Listening on port (\d+)', log)
            if ready:
                self.port = int(ready.group(1)); break
            time.sleep(.02)
        else: self.stop(); raise TimeoutError('server did not become ready')
    def client(self): return Client(self.port)
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
    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.CTRL_BREAK_EVENT if os.name == 'nt' else signal.SIGTERM)
            try: self.process.wait(timeout=8)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait(); raise
        self.stdout.close(); self.stderr.close()
        assert self.process.returncode == 0, (self.process.returncode, (self.root / 'stderr').read_text())
    def __enter__(self): return self
    def __exit__(self, *_):
        try: self.stop()
        finally: self.temporary.cleanup()

def article(body='Н─│', group='test', subject='Re: Topic', extra=''):
    return (f'From: Alice <alice@example.org>\r\nNewsgroups: {group}\r\nSubject: {subject}\r\n'
            f'Content-Type: text/plain; charset=UTF-8\r\n{extra}\r\n{body}\r\n').encode('utf-8')

class NntpEndToEnd(unittest.TestCase):
    def test_independent_tosser_and_consistent_backup_restore(self):
        with Server() as server:
            server.stop()
            backup = server.snapshot()
            subprocess.run([sys.executable, str(Path(__file__).with_name('fake_tosser.py')),
                            str(server.root / 'test')], check=True, timeout=5)
            self.assertIn('Н─│'.encode('cp866'), server.message(3)[2])
            # Start the same executable/config against the independent imported files.
            def restart():
                (server.root / 'log').unlink(missing_ok=True)
                server.stdout = (server.root / 'stdout').open('wb')
                server.stderr = (server.root / 'stderr').open('wb')
                options = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {}
                server.process = subprocess.Popen([OPTIONS.executable, '-config', str(server.config)],
                    stdout=server.stdout, stderr=server.stderr, **options)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    self.assertIsNone(server.process.poll())
                    log = server.root / 'log'
                    ready = re.search(r'Listening on port (\d+)', log.read_text() if log.exists() else '')
                    if ready:
                        server.port = int(ready.group(1))
                        return
                    time.sleep(.02)
                self.fail('restart timed out')
            restart()
            with server.client() as client:
                self.assertTrue(client.command('GROUP test').startswith(b'211 3 1 3'))
                first, text = client.multiline('ARTICLE 3')
                self.assertTrue(first.startswith(b'220'))
                self.assertIn('Independent tosser: Н─│'.encode(), text)
                self.assertTrue(client.login().startswith(b'281'))
                self.assertTrue(client.post(article('after import')).startswith(b'240'))
                self.assertIn(b'after import', server.message(4)[2])
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
    OPTIONS, rest = parser.parse_known_args()
    unittest.main(argv=[__file__, *rest])
