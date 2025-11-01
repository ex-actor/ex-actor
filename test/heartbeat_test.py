#!/usr/bin/env/python3

import sys
import subprocess

argv = sys.argv
node0 = subprocess.Popen([argv[1], "0"])
node1 = subprocess.Popen([argv[1], "1"])
node0.kill()
node0.wait(0.5)
try:
    node1.wait(1)
except subprocess.TimeoutExpired:
    print("Node0 is dead, but node1 is still running")
    raise
