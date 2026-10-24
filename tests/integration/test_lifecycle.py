"""Independent native termination checks with bounded child cleanup."""
import argparse
import os
import queue
import signal
import subprocess
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument("--probe", required=True)
args = parser.parse_args()
for termination in ([signal.CTRL_BREAK_EVENT] if os.name == "nt" else [signal.SIGINT, signal.SIGTERM]):
    options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {}
    process = subprocess.Popen([args.probe], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **options)
    ready = queue.Queue()
    reader = threading.Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True)
    reader.start()
    try:
        assert ready.get(timeout=3) == "READY\n", "probe failed before readiness"
        beginning = time.monotonic()
        process.send_signal(termination)
        stdout, stderr = process.communicate(timeout=5)
        reader.join(timeout=1)
        assert process.returncode == 0, (process.returncode, stderr)
        assert time.monotonic() - beginning < 5
        assert not stderr, stderr
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)
        reader.join(timeout=1)
