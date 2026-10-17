#!/usr/bin/env python3
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
BIN = os.path.join(REPO, "deploy", "lsm", "lsm_server")
CFG = os.path.join(REPO, "deploy", "lsm", "config.toml")
PORT = 6380

def cmd(sock, *args):
    parts = ["*%d" % len(args)]
    for a in args:
        parts.append("$%d" % len(a))
        parts.append(a)
    sock.sendall(("\r\n".join(parts) + "\r\n").encode())
    return sock.recv(65536)

def wait_port(proc, port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return True
        except OSError:
            time.sleep(0.5)
    return False

def main():
    work = tempfile.mkdtemp(prefix="lsm-smoke-")
    proc = None
    results = []

    def record(name, ok, detail=""):
        results.append((name, ok))
        status = "PASS" if ok else "FAIL"
        print("[%s] %-24s | %s | %s" % (time.strftime("%H:%M:%S"), name, status, str(detail)[:110]), flush=True)

    try:
        shutil.copy2(BIN, os.path.join(work, "lsm_server"))
        shutil.copy2(CFG, os.path.join(work, "config.toml"))
        os.chmod(os.path.join(work, "lsm_server"), 0o755)
        record("prepare_temp_dir", True, work)
        logpath = os.path.join(work, "server.log")
        logf = open(logpath, "wb")
        proc = subprocess.Popen([os.path.join(work, "lsm_server")], cwd=work, stdout=logf, stderr=subprocess.STDOUT)
        record("service_start", True, "pid=" + str(proc.pid))
        up = wait_port(proc, PORT, 25)
        record("listen_port_6380", up, "ok" if up else "25s not listening")
        if not up:
            try:
                txt = open(logpath, errors="replace").read()[-400:]
            except Exception:
                txt = ""
            record("server_log_tail", False, txt.replace(chr(10), " "))
            return results

        sock = socket.create_connection(("127.0.0.1", PORT), 5)
        sock.settimeout(5)
        record("tcp_connect", True, "connected")

        resp = cmd(sock, "PING")
        record("ping", b"PONG" in resp, resp[:40])

        key = "ci_smoke_key"
        val = "ci_smoke_value_123"
        resp = cmd(sock, "SET", key, val)
        record("set_key", resp.startswith(b"+OK"), resp[:40])

        resp = cmd(sock, "GET", key)
        record("get_key_match", val.encode() in resp, resp[:60])

        resp = cmd(sock, "DEL", key)
        record("del_key", resp.startswith(b":1") or resp.startswith(b"+OK"), resp[:40])

        resp = cmd(sock, "GET", key)
        record("get_after_del", val.encode() not in resp, resp[:40])
        sock.close()

    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        logf.close()
        shutil.rmtree(work, ignore_errors=True)
        gone = not os.path.exists(work)
        print("[%s] %-24s | %-4s | %s" % (time.strftime("%H:%M:%S"), "teardown", "PASS" if gone else "FAIL", "temp dir removed" if gone else "temp dir remains"), flush=True)

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
