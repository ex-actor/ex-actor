#!/usr/bin/env/python3

import sys
import subprocess
import time

argv = sys.argv
node0 = subprocess.Popen([argv[1], "0"])
node1 = subprocess.Popen([argv[1], "1"])
time.sleep(0.5)
node0.kill()
node0.wait(1)
print("node0 has been killed", flush=True)
try:
    node1.wait(10)
except subprocess.TimeoutExpired:
    print("Node0 is dead, but node1 is still running")
    raise
