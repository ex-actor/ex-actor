#!/usr/bin/env python3

import subprocess
import sys
import time

CLUSTER_SIZE = 8


def clear_nodes(node_list):
    for node in node_list:
        if node.poll() is None:
            node.kill()
    for node in node_list:
        node.wait()


def check_nodes(node_list):
    time.sleep(15)
    for node in node_list:
        if node.poll() is None:
            clear_nodes(node_list)
            raise RuntimeError("There is node in the cluster timeout")
    for node in node_list:
        if node.returncode != 0:
            clear_nodes(node_list)
            raise RuntimeError("There is error in the cluster")


def run_test(bin, test_type):
    node_list = []
    for node_id in range(CLUSTER_SIZE):
        node_list.append(
            subprocess.Popen([bin, str(CLUSTER_SIZE), str(node_id), test_type])
        )
    check_nodes(node_list)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise RuntimeError("usage: dynamic_connectivity_test.py <bin> <star|chain>")

    bin = sys.argv[1]
    type = sys.argv[2]
    if type not in {"star", "chain"}:
        raise RuntimeError(f"unsupported type: {type}")

    run_test(bin, type)
