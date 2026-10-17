#!/usr/bin/env python3
"""Cloud judge orchestrator business smoke test."""
import json
import os
import random
import socket
import subprocess
import sys
import time
from urllib.request import urlopen
from urllib.error import URLError

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
BIN = os.path.join(REPO, "build", "cloud_judge", "orchestrator")
PORT = random.randint(18800, 18899)

results = []

def record(name, ok, detail=""):
    results.append((name, ok))
    print("[%s] %-26s | %-4s | %s" % (time.strftime("%H:%M:%S"), name, "PASS" if ok else "FAIL", str(detail)[:110]), flush=True)

def http_get(path, timeout=5):
    url = "http://127.0.0.1:%d%s" % (PORT, path)
    with urlopen(url, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8", "replace")

def wait_ready(proc, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            socket.create_connection(("127.0.0.1", PORT), 1).close()
            return True
        except OSError:
            time.sleep(0.3)
    return False

def main():
    if not os.path.exists(BIN):
        record("orchestrator_binary", False, "missing: " + BIN)
        return results
    record("orchestrator_binary", True, os.path.basename(BIN))

    proc = None
    try:
        proc = subprocess.Popen([BIN, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        record("service_start", True, "pid=" + str(proc.pid) + " port=" + str(PORT))
        up = wait_ready(proc, 20)
        record("http_ready", up, "port %d" % PORT if up else "20s not ready")
        if not up:
            return results

        code, body = http_get("/")
        ok = code == 200 and "/status" in body
        record("endpoint_index", ok, body[:90])

        code, body = http_get("/status")
        ok = code == 200 and isinstance(json.loads(body), list)
        record("endpoint_status", ok, body[:90])

        code, body = http_get("/preempt")
        data = json.loads(body)
        ok = code == 200 and "active_judges" in data and "high_watermark" in data
        record("endpoint_preempt", ok, body[:90])

        code, body = http_get("/task/not-exist-ci")
        data = json.loads(body)
        ok = code == 200 and data.get("error") == "not found"
        record("endpoint_task_missing", ok, body[:90])
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        alive = subprocess.run(["pgrep", "-f", "orchestrator %d" % PORT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        gone = alive.returncode != 0
        print("[%s] %-26s | %-4s | %s" % (time.strftime("%H:%M:%S"), "teardown", "PASS" if gone else "FAIL", "service stopped" if gone else "process remains"), flush=True)
    return results

if __name__ == "__main__":
    res = main()
    total = len(res)
    ok = sum(1 for _, v in res if v)
    print("")
    print("=== SUMMARY ===")
    print("total=%d passed=%d failed=%d" % (total, ok, total - ok))
    if ok != total:
        print("failed cases: " + ", ".join(n for n, v in res if not v))
        print("result: FAILED")
        sys.exit(1)
    print("result: ALL PASSED")
    sys.exit(0)
