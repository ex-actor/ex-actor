#!/usr/bin/env python3

import subprocess
import sys
import time

bin = sys.argv[1]
node0 = subprocess.Popen([bin, "0"])
node1 = subprocess.Popen([bin, "1"])
time.sleep(5)  # Make sure the subprocess will be cleaned in any case.
if node0.poll() is None:
    node0.kill()
    node1.kill()
    raise SystemExit("Node0 timeout or exit by exception")
if node1.poll() is None:
    node1.kill()
    node0.kill()
    raise SystemExit("Node1 timeout or exit by exception")

if node0.returncode != 0:
    raise SystemExit(f"Node0 exit with {node0.returncode}")
if node1.returncode != 0:
    raise SystemExit(f"Node1 exit with {node1.returncode}")
