#!/usr/bin/env python3

import subprocess
import sys
import time


def clear_nodes(node_list):
    for node in node_list:
        node.kill()
    for node in node_list:
        node.wait()


def check_nodes(node_list):
    time.sleep(15)
    for node in node_list:
        if node.poll() is None:
            clear_nodes(node_list)
            raise RuntimeError("There's node in the cluster timeout")
    time.sleep(5)
    for node in node_list:
        if node.returncode != 0:
            clear_nodes(node_list)
            raise RuntimeError("There's error in the cluster")


if __name__ == "__main__":
    bin = sys.argv[1]
    cluster_size = 8
    node_list = []
    for node_id in range(cluster_size):
        node_list.append(subprocess.Popen([bin, str(cluster_size), str(node_id)]))
    check_nodes(node_list)
