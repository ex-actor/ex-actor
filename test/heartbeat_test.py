#!/usr/bin/env python3

import sys
import subprocess
import time
import tempfile
import os

argv = sys.argv

log_file0 = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".log")
node0 = subprocess.Popen([argv[1], "0"], stdout=log_file0, stderr=subprocess.STDOUT)
log_file0.close()

log_file = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".log")
node1 = subprocess.Popen([argv[1], "1"], stdout=log_file, stderr=subprocess.STDOUT)
log_file.close()

deadline = time.monotonic() + 5
connected = False
while time.monotonic() < deadline:
    with open(log_file0.name, "r") as f:
        if "[Gossip] Node 0 found node 1" in f.read():
            connected = True
            break
    time.sleep(0.1)

if not connected:
    print("FAIL: node0 never connected to node1")
    with open(log_file0.name, "r") as f:
        print(f.read(), end="", flush=True)
    node0.kill()
    node0.wait()
    node1.kill()
    node1.wait()
    os.unlink(log_file0.name)
    os.unlink(log_file.name)
    sys.exit(1)

node0.kill()
node0.wait(1)
os.unlink(log_file0.name)
print("node0 has been killed", flush=True)

deadline = time.monotonic() + 10
death_detected = False
exception_caught = False
while time.monotonic() < deadline:
    with open(log_file.name, "r") as f:
        content = f.read()
    if "detects that node" in content and "is dead" in content:
        death_detected = True
    if "caught exception during ping" in content:
        exception_caught = True
    if death_detected and exception_caught:
        print(content, end="", flush=True)
        break
    time.sleep(0.5)

if not death_detected or not exception_caught:
    missing = []
    if not death_detected:
        missing.append("death detection")
    if not exception_caught:
        missing.append("caught exception during ping")
    print(f"FAIL: node1 did not log: {', '.join(missing)}")
    with open(log_file.name, "r") as f:
        print(f.read(), end="", flush=True)
    node1.kill()
    node1.wait()
    os.unlink(log_file.name)
    sys.exit(1)

print("SUCCESS: node1 detected node0's death and caught ping exception", flush=True)
node1.kill()
node1.wait()
os.unlink(log_file.name)
