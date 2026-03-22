#!/usr/bin/env python3

import os
import signal
import subprocess
import sys
import tempfile
import time

IS_WINDOWS = sys.platform == "win32"
CLUSTER_SIZE = 8
WORK_TIMEOUT = 15
SUCCESS_LOG = "All work done"


def kill_nodes(node_list):
    for node in node_list:
        if node.poll() is None:
            node.kill()
    for node in node_list:
        node.wait()


def run_test(bin, test_type):
    log_files = []
    node_list = []
    for node_id in range(CLUSTER_SIZE):
        log_file = tempfile.NamedTemporaryFile(
            mode="w", delete=False, suffix=".log"
        )
        log_files.append(log_file)
        node_list.append(
            subprocess.Popen(
                [bin, str(CLUSTER_SIZE), str(node_id), test_type],
                stdout=log_file,
                stderr=subprocess.STDOUT,
            )
        )
        log_file.close()

    def cleanup(kill=True):
        for node in node_list:
            if kill and node.poll() is None:
                node.kill()
            node.wait()
        for log_file in log_files:
            os.unlink(log_file.name)

    def print_all_logs():
        for i, log_file in enumerate(log_files):
            print(f"--- node{i} log ---", flush=True)
            with open(log_file.name, "r") as f:
                print(f.read(), end="", flush=True)

    # Wait until all nodes print the success log, or timeout.
    done = [False] * CLUSTER_SIZE
    deadline = time.monotonic() + WORK_TIMEOUT
    while time.monotonic() < deadline:
        for i, node in enumerate(node_list):
            code = node.poll()
            if code is not None and code != 0:
                print(f"FAIL: node{i} exited prematurely with code {code}", flush=True)
                print_all_logs()
                cleanup(kill=True)
                sys.exit(1)

        for i, log_file in enumerate(log_files):
            if not done[i]:
                with open(log_file.name, "r") as f:
                    if SUCCESS_LOG in f.read():
                        done[i] = True

        if all(done):
            break
        time.sleep(0.5)

    if not all(done):
        missing = [i for i, d in enumerate(done) if not d]
        print(f"FAIL: node(s) {missing} did not print '{SUCCESS_LOG}' within {WORK_TIMEOUT}s", flush=True)
        print_all_logs()
        cleanup(kill=True)
        sys.exit(1)

    # All nodes finished work; terminate them.
    # On Linux, terminate() sends SIGTERM which the C++ signal handler catches for graceful shutdown.
    # On Windows, terminate() calls TerminateProcess(pid, 1) — no signal handler is invoked.
    for node in node_list:
        if node.poll() is None:
            node.terminate()

    for node in node_list:
        try:
            node.wait(timeout=5)
        except subprocess.TimeoutExpired:
            node.kill()
            node.wait()

    if IS_WINDOWS:
        # TerminateProcess sets exit code to 1
        acceptable_codes = {0, 1}
    else:
        acceptable_codes = {0, -signal.SIGTERM}
    for i, node in enumerate(node_list):
        if node.returncode not in acceptable_codes:
            print(f"FAIL: node{i} exited with unexpected code {node.returncode}", flush=True)
            print_all_logs()
            cleanup(kill=False)
            sys.exit(1)

    print(f"SUCCESS: all {CLUSTER_SIZE} nodes completed work", flush=True)
    for log_file in log_files:
        os.unlink(log_file.name)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise RuntimeError("usage: dynamic_connectivity_test.py <bin> <star|chain>")

    bin = sys.argv[1]
    type = sys.argv[2]
    if type not in {"star", "chain"}:
        raise RuntimeError(f"unsupported type: {type}")

    run_test(bin, type)
