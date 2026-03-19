#!/usr/bin/env python3
#
# Heartbeat / failure-detection integration test.
#
# 1. Start two nodes (node0 = contact node, node1 = joining node).
# 2. Wait until both nodes have discovered each other via gossip.
#    Both directions must be confirmed because gossip propagation is
#    asynchronous — node0 knowing about node1 does NOT imply the reverse.
# 3. Wait for node1 to confirm it created the remote actor and started
#    pinging, then kill node0 to simulate a crash.
# 4. Verify that node1 detects node0's death (heartbeat timeout) and that
#    the in-flight ping RPC raises an exception on node1.

import sys
import subprocess
import time
import tempfile
import os

argv = sys.argv

ADDR0 = "tcp://127.0.0.1:5301"
ADDR1 = "tcp://127.0.0.1:5302"

# Step 1: Launch both nodes, capturing their logs to temp files.
log_file0 = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".log")
node0 = subprocess.Popen([argv[1], ADDR0, ADDR1], stdout=log_file0, stderr=subprocess.STDOUT)
log_file0.close()

log_file = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".log")
node1 = subprocess.Popen([argv[1], ADDR1, ADDR0, ADDR0], stdout=log_file, stderr=subprocess.STDOUT)
log_file.close()

# Step 2: Wait for bidirectional gossip discovery (up to 5 s).
# Node IDs are auto-generated, so we look for the generic gossip pattern.
deadline = time.monotonic() + 5
node0_found_peer = False
node1_found_peer = False
while time.monotonic() < deadline:
    if not node0_found_peer:
        with open(log_file0.name, "r") as f:
            if "[Gossip]" in f.read() and "found node" in open(log_file0.name).read():
                node0_found_peer = True
    if not node1_found_peer:
        with open(log_file.name, "r") as f:
            if "[Gossip]" in f.read() and "found node" in open(log_file.name).read():
                node1_found_peer = True
    if node0_found_peer and node1_found_peer:
        break
    time.sleep(0.1)

if not node0_found_peer or not node1_found_peer:
    missing = []
    if not node0_found_peer:
        missing.append("node0 -> peer")
    if not node1_found_peer:
        missing.append("node1 -> peer")
    print(f"FAIL: connection not established: {', '.join(missing)}")
    with open(log_file0.name, "r") as f:
        print("=== node0 log ===", flush=True)
        print(f.read(), end="", flush=True)
    with open(log_file.name, "r") as f:
        print("=== node1 log ===", flush=True)
        print(f.read(), end="", flush=True)
    node0.kill()
    node0.wait()
    node1.kill()
    node1.wait()
    os.unlink(log_file0.name)
    os.unlink(log_file.name)
    sys.exit(1)

# Step 3: Wait for node1 to confirm the remote actor is created and pinging
# has started, then kill node0 to simulate a crash.
deadline = time.monotonic() + 5
ping_started = False
while time.monotonic() < deadline:
    with open(log_file.name, "r") as f:
        if "remote actor created" in f.read():
            ping_started = True
            break
    time.sleep(0.1)

if not ping_started:
    print("FAIL: node1 did not start pinging node0")
    with open(log_file.name, "r") as f:
        print("=== node1 log ===", flush=True)
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

# Step 4: Poll node1's log for heartbeat-based death detection and the
# resulting RPC exception (up to 15 s, must exceed heartbeat_timeout_ms).
deadline = time.monotonic() + 15
death_detected = False
exception_caught = False
while time.monotonic() < deadline:
    with open(log_file.name, "r") as f:
        content = f.read()
    has_new_death_log = "lost connection to node" in content and "heartbeat timeout" in content
    if has_new_death_log:
        death_detected = True
    if "connection lost to node" in content:
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
        missing.append("connection lost to node")
    print(f"FAIL: node1 did not log: {', '.join(missing)}")
    with open(log_file.name, "r") as f:
        print(f.read(), end="", flush=True)
    node1.kill()
    node1.wait()
    os.unlink(log_file.name)
    sys.exit(1)

print("SUCCESS: node1 detected node0's death and caught ConnectionLost", flush=True)
node1.kill()
node1.wait()
os.unlink(log_file.name)
