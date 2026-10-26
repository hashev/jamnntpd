"""Opt-in native load, resource and real-idle acceptance against disposable JAM bases."""
import argparse
from collections import Counter, deque
import ctypes
import errno
import json
import os
from pathlib import Path
import platform
import random
import socket
import subprocess
import sys
import threading
import time

import test_server
from test_storage import byte_lock


def resources(pid):
    if sys.platform.startswith('linux'):
        lines = Path(f'/proc/{pid}/status').read_text().splitlines()
        rss = int(next(line.split()[1] for line in lines if line.startswith('VmRSS:')))*1024
        return {'rss_bytes': rss, 'native_handles': len(list(Path(f'/proc/{pid}/fd').iterdir()))}
    if sys.platform == 'darwin':
        rss = int(subprocess.check_output(['ps', '-o', 'rss=', '-p', str(pid)], timeout=3))*1024
        listing = subprocess.check_output(['lsof', '-a', '-p', str(pid), '-Ff'], timeout=3).decode()
        return {'rss_bytes': rss, 'native_handles': sum(line.startswith('f') and line[1:].isdigit()
                                                       for line in listing.splitlines())}
    if os.name == 'nt':
        from ctypes import wintypes
        class Counters(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('faults', wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in ['peak', 'working', 'peak_paged', 'paged',
                                                    'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile']]
        kernel, psapi = ctypes.WinDLL('kernel32', use_last_error=True), ctypes.WinDLL('psapi', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            memory, handles = Counters(), wintypes.DWORD()
            memory.cb = ctypes.sizeof(memory)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb) or not kernel.GetProcessHandleCount(handle, ctypes.byref(handles)):
                raise ctypes.WinError(ctypes.get_last_error())
            return {'rss_bytes': memory.working, 'native_handles': handles.value}
        finally:
            kernel.CloseHandle(handle)
    raise RuntimeError('No native resource observer for this platform')


def validate_base(server, name):
    data = server.snapshot(name)
    oracle = test_server.jam_oracle
    info = oracle.base_header(data['.jhr'][:1024])
    live = 0
    markers = set()
    for slot, (_, offset) in enumerate(oracle.indices(data['.jdx'])):
        if offset == 0xffffffff:
            continue
        fixed = oracle.message_header(data['.jhr'][offset:offset+76])
        assert fixed[10] == info[4]+slot, 'Number/index mismatch'
        fields = oracle.subfields(data['.jhr'][offset+76:offset+76+fixed[0]])
        body = oracle.text_slice(fixed, data['.jdt'])
        if fixed[11] & 0x80000000:
            continue
        live += 1
        first = body.split(b'\r', 1)[0]
        if first.startswith(b'load/'):
            markers.add(first.decode('ascii'))
        for low, _, value in fields:
            if low == 4:
                assert fixed[2] == oracle.crc32_ascii(value), 'Message CRC mismatch'
    assert live == info[2], 'Live count mismatch'
    assert len(data['.jlr']) % 16 == 0, 'Partial lastread record'
    return markers


def quiescent(server):
    # A final connection reaps completed worker entries in both observations.
    time.sleep(.2)
    with server.client() as client:
        assert client.command('QUIT') == b'205 Goodbye\r\n'
    time.sleep(.2)
    return resources(server.process.pid)


def locker(args):
    deadline = time.monotonic()+args.duration+180
    iterations = 0
    while not args.stop.exists() and time.monotonic() < deadline:
        try:
            with Path(str(args.base)+'.jhr').open('r+b', buffering=0) as stream, byte_lock(stream):
                iterations += 1
                args.ready.write_text(str(iterations))
                time.sleep(.05)
        except OSError as error:
            if error.errno not in (errno.EACCES, errno.EAGAIN):
                raise
        time.sleep(.5)
    return 0


def idle(args, report):
    with test_server.Server(extra='max 1\n') as server, server.raw_client() as client:
        before = server.snapshot()
        client.socket.settimeout(315)
        start = time.monotonic()
        assert client.socket.recv(1) == b'', 'Idle peer did not close'
        elapsed = time.monotonic()-start
        assert 295 <= elapsed <= 315, f'Unexpected idle deadline: {elapsed}'
        with server.client() as replacement:
            assert replacement.greeting.startswith(b'200'), 'Idle lease was not released'
        assert server.snapshot() == before
        report['idle_seconds'] = elapsed


def load(args, report, destination):
    with test_server.Server(recipe={'messages': args.messages}, extra='max 32\n') as server:
        baseline = quiescent(server)
        for _ in range(100):
            with server.client() as client:
                assert client.command('QUIT') == b'205 Goodbye\r\n'
        after_cycles = quiescent(server)
        assert after_cycles['native_handles'] <= baseline['native_handles'], (baseline, after_cycles)
        report.update(resources_before=baseline, resources_after_100_cycles=after_cycles, phases=[])
        successful = {'test': set(), 'local': set()}
        cancelled = {'test': set(), 'local': set()}
        # An independent writer imports once with all writers stopped; live contention below is a separate process.
        server.stop()
        subprocess.run([sys.executable, str(Path(__file__).with_name('fake_tosser.py')), str(server.root/'test')],
                       check=True, timeout=5)
        server.restart()
        starts = {name: len(test_server.jam_oracle.indices(server.snapshot(name)['.jdx'])) for name in successful}
        acknowledged = Counter()
        available = {name: deque() for name in successful}
        stop_path, ready_path = server.root/'locker.stop', server.root/'locker.ready'
        output = (destination/'locker-output.txt').open('wb')
        helper = subprocess.Popen([sys.executable, str(Path(__file__)), '--mode', 'locker',
                                   '--base', str(server.root/'test'), '--stop', str(stop_path),
                                   '--ready', str(ready_path), '--duration', str(args.duration)],
                                  stdout=output, stderr=output)
        try:
            deadline = time.monotonic()+5
            while not ready_path.exists():
                assert helper.poll() is None, 'External locker failed'
                assert time.monotonic() < deadline, 'External locker did not start'
                time.sleep(.02)
            mutex = threading.Lock()
            for phase, concurrency in enumerate((1, 5, 32)):
                stopped = threading.Event()
                errors, latencies, counters = [], [], Counter()
                def record(operation, code, elapsed):
                    with mutex:
                        counters[operation+':'+code] += 1
                        latencies.append(elapsed)
                def writer(worker):
                    rng = random.Random(args.seed+phase*100+worker)
                    try:
                        with server.client() as client:
                            assert client.greeting.startswith(b'200')
                            client.socket.settimeout(60)
                            assert client.login().startswith(b'281')
                            iteration = 0
                            while not stopped.is_set():
                                name = rng.choice(['test', 'local'])
                                choice, began = rng.randrange(100), time.monotonic()
                                iteration += 1
                                operation = 'mode'
                                if choice < 60:
                                    response = client.command('MODE READER')
                                    assert response.startswith(b'200')
                                elif choice < 80:
                                    operation = 'stat'
                                    response = client.command(f'STAT <1${name}@JamNNTPd>')
                                    assert response[:3] in (b'223', b'503')
                                elif choice < 95:
                                    operation = 'reply' if choice >= 90 else 'post'
                                    marker = f'load/{phase}/{worker}/{iteration}'
                                    response = client.post(test_server.article(marker, group=name,
                                        extra=f'References: <1${name}@JamNNTPd>\r\n' if operation == 'reply' else ''))
                                    assert response[:3] in (b'240', b'503')
                                    if response.startswith(b'240'):
                                        with mutex:
                                            acknowledged[name] += 1
                                            available[name].append(starts[name]+acknowledged[name])
                                            successful[name].add(marker)
                                else:
                                    operation = 'cancel'
                                    with mutex:
                                        number = available[name].popleft() if available[name] else None
                                    if number is None:
                                        continue
                                    response = client.post(test_server.article('', group=name,
                                        extra=f'Control: cancel <{number}${name}@JamNNTPd>\r\n'))
                                    assert response[:3] in (b'240', b'441', b'503')
                                    if response.startswith(b'240'):
                                        with mutex:
                                            cancelled[name].add(number)
                                record(operation, response[:3].decode('ascii'), time.monotonic()-began)
                    except Exception as error:
                        with mutex:
                            errors.append(type(error).__name__)
                        stopped.set()
                def slow_reader():
                    try:
                        with server.client() as client:
                            assert client.greeting.startswith(b'200')
                            client.socket.settimeout(60)
                            assert client.command('GROUP test').startswith(b'211')
                            client.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                            client.socket.sendall(f'XOVER 1-{args.messages}\r\n'.encode())
                            while not stopped.is_set():
                                value = client.socket.recv(1)
                                if not value:
                                    break
                                stopped.wait(.05)
                    except Exception as error:
                        if not stopped.is_set():
                            with mutex:
                                errors.append('slow:'+type(error).__name__)
                            stopped.set()
                slow_count = 0 if concurrency == 1 else (1 if concurrency == 5 else 2)
                threads = [threading.Thread(target=writer, args=(i,)) for i in range(concurrency-slow_count)]
                threads += [threading.Thread(target=slow_reader) for _ in range(slow_count)]
                start, samples = time.monotonic(), []
                for thread in threads:
                    thread.start()
                try:
                    while time.monotonic()-start < args.duration/3 and not stopped.is_set():
                        samples.append({'seconds': time.monotonic()-start, **resources(server.process.pid)})
                        assert helper.poll() is None, 'External locker exited early'
                        stopped.wait(min(5, max(.01, args.duration/3-(time.monotonic()-start))))
                finally:
                    stopped.set()
                    for thread in threads:
                        thread.join(timeout=65)
                    assert not any(thread.is_alive() for thread in threads), 'Load thread did not finish'
                assert not errors, errors
                ordered = sorted(latencies)
                assert ordered, 'No completed load operations'
                elapsed = time.monotonic()-start
                report['phases'].append({'clients': concurrency, 'slow_clients': slow_count,
                    'active_seconds': args.duration/3, 'elapsed_with_drain': elapsed,
                    'operations': dict(counters), 'throughput': len(ordered)/elapsed,
                    'latency_seconds': {str(percent): ordered[min(len(ordered)-1, int((len(ordered)-1)*percent/100))]
                                        for percent in (50, 95, 99)}, 'resources': samples})
                (destination/'report.json').write_text(json.dumps(report, indent=2))
        finally:
            stop_path.touch()
            try:
                helper.wait(timeout=3)
            except subprocess.TimeoutExpired:
                helper.kill()
                helper.wait(timeout=3)
                raise
            finally:
                output.close()
        assert helper.returncode == 0, 'External locker failed'
        report['external_lock_iterations'] = int(ready_path.read_text())
        after = quiescent(server)
        report['resources_after_load'] = after
        assert after['native_handles'] <= baseline['native_handles'], (baseline, after)
        start = time.monotonic()
        server.stop()
        report['shutdown_seconds'] = time.monotonic()-start
        assert report['shutdown_seconds'] < 8
        for name in successful:
            markers = validate_base(server, name)
            slots = len(test_server.jam_oracle.indices(server.snapshot(name)['.jdx']))
            assert slots == starts[name]+acknowledged[name], 'Unaccounted publication'
            assert len(markers) == len(successful[name])-len(cancelled[name]), 'Lost or partial committed article'
            assert markers <= successful[name], 'Unexpected published marker'
        server.restart()
        with server.client() as client:
            assert client.command('STAT <1$test@JamNNTPd>').startswith(b'223')
        report['acknowledged_posts'] = dict(acknowledged)
        report['successful_cancellations'] = {name: len(values) for name, values in cancelled.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['load', 'idle', 'locker'], required=True)
    parser.add_argument('--executable')
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--artifacts', type=Path, default=Path('build/acceptance-extended'))
    parser.add_argument('--duration', type=int, default=1800)
    parser.add_argument('--messages', type=int, default=10000)
    parser.add_argument('--seed', type=int, default=20261026)
    parser.add_argument('--base', type=Path)
    parser.add_argument('--stop', type=Path)
    parser.add_argument('--ready', type=Path)
    args = parser.parse_args()
    if args.mode == 'locker':
        if not all((args.base, args.stop, args.ready)):
            parser.error('Locker requires base/stop/ready')
        return locker(args)
    if not args.executable or args.fixtures is None:
        parser.error('Executable and fixtures are required')
    if not 3 <= args.duration <= 1800 or not 1 <= args.messages <= 10000:
        parser.error('Duration/count outside bounded profile')
    test_server.OPTIONS = args
    destination = args.artifacts.resolve()/(args.mode+'-'+str(time.time_ns()))
    destination.mkdir(parents=True)
    root = Path(__file__).resolve().parents[2]
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    changes = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=all'],
                                      cwd=root, text=True).splitlines()
    source_dirty = any(line[3:].startswith(('src/', 'tests/', 'tools/', 'CMake', '.github/')) for line in changes)
    platform_key = 'windows' if os.name == 'nt' else ('macos' if sys.platform == 'darwin' else 'linux')
    report = {'mode': args.mode, 'platform': platform.platform(), 'revision': revision,
              'platform_key': platform_key,
              'source_dirty': source_dirty,
              'seed': args.seed, 'messages_per_base': args.messages,
              'acceptance_profile': args.mode == 'idle' or (args.duration == 1800 and args.messages == 10000),
              'status': 'running'}
    try:
        if args.mode == 'idle':
            idle(args, report)
        else:
            load(args, report, destination)
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error_type'] = type(error).__name__
        raise
    finally:
        (destination/'report.json').write_text(json.dumps(report, indent=2))
        print(f'Extended report: {destination}/report.json', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
