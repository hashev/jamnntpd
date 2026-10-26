"""Reduce a saved generated case while preserving its replay command's failing exit code."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def minimize(data, interesting):
    if not interesting(data):
        raise ValueError('Initial case does not reproduce the failure')
    partitions = 2
    while data:
        width = max(1, (len(data)+partitions-1)//partitions)
        for start in range(0, len(data), width):
            candidate = data[:start]+data[start+width:]
            if interesting(candidate):
                data = candidate
                partitions = max(2, partitions-1)
                break
        else:
            if width == 1:
                return data
            partitions = min(len(data), partitions*2)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('Provide a replay command after --')
    data = args.input.read_bytes()
    if len(data) > 1024*1024:
        parser.error('Input exceeds 1 MiB bound')
    with tempfile.TemporaryDirectory(prefix='jam-reduce-') as folder:
        path = Path(folder)/'case.bin'
        def replay(candidate):
            path.write_bytes(candidate)
            result = subprocess.run(command, env={**os.environ, 'JAM_REPLAY_FILE': str(path)},
                                    capture_output=True, timeout=10)
            return result.returncode
        expected = replay(data)
        if expected == 0:
            parser.error('Replay succeeds; failure not reproduced')
        reduced = minimize(data, lambda candidate: replay(candidate) == expected)
    with args.output.open('xb') as output:
        output.write(reduced)
    print(f'Reduced {len(data)} bytes to {len(reduced)}; exit code {expected}')


if __name__ == '__main__':
    main()
