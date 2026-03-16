#!/usr/bin/env python3

import sys
import subprocess
import time
import tempfile
import os

TIMEOUT = 5
SUCCESS_LOG = "All work done"

argv = sys.argv

ADDR0 = "tcp://127.0.0.1:5301"
ADDR1 = "tcp://127.0.0.1:5302"

log_file0 = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".log")
node0 = subprocess.Popen([argv[1], ADDR0], stdout=log_file0, stderr=subprocess.STDOUT)
log_file0.close()

log_file1 = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".log")
node1 = subprocess.Popen([argv[1], ADDR1, ADDR0], stdout=log_file1, stderr=subprocess.STDOUT)
log_file1.close()


def cleanup(kill=True):
    for node in [node0, node1]:
        if kill:
            node.kill()
        node.wait()
    for path in [log_file0.name, log_file1.name]:
        os.unlink(path)


def check_exit_codes():
    for i, node in enumerate([node0, node1]):
        code = node.poll()
        if code is not None and code != 0:
            print(f"FAIL: node{i} exited with non-zero exit code {code}", flush=True)
            for path in [log_file0.name, log_file1.name]:
                with open(path, "r") as f:
                    print(f.read(), end="", flush=True)
            cleanup(kill=True)
            sys.exit(1)


deadline = time.monotonic() + TIMEOUT
done = [False, False]

while time.monotonic() < deadline:
    check_exit_codes()

    for i, path in enumerate([log_file0.name, log_file1.name]):
        if not done[i]:
            with open(path, "r") as f:
                content = f.read()
            if SUCCESS_LOG in content:
                done[i] = True
                print(f"node{i} printed '{SUCCESS_LOG}'", flush=True)

    if all(done):
        break

    time.sleep(0.5)

if not all(done):
    missing = [i for i, d in enumerate(done) if not d]
    print(f"FAIL: node(s) {missing} did not print '{SUCCESS_LOG}' within {TIMEOUT}s", flush=True)
    for i, path in enumerate([log_file0.name, log_file1.name]):
        print(f"--- node{i} log ---", flush=True)
        with open(path, "r") as f:
            print(f.read(), end="", flush=True)
    cleanup(kill=True)
    sys.exit(1)

print("SUCCESS: both nodes completed work", flush=True)
cleanup(kill=True)
