#!/usr/bin/env python3
import json
import os
import socket
import subprocess
import sys
import time
import uuid

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
JUDGE_IMAGE = "cloud-judge:localtest"
MQ_PORT = 5672
LSM_PORT = 6380
MW_PORT = 8898
ORCH_PORT = 8890
DB_PORT = 3306
TERMINAL = set(["AC","WA","CE","RE","TLE","MLE","SE","OLE","PE","DONE","FAILED"])

results = []

def record(name, ok, detail="", began=None):
    dur = "" if began is None else "%.1fs" % (time.time() - began)
    results.append((name, ok))
    print("[%s] %-26s | %-4s | %6s | %s" % (time.strftime("%H:%M:%S"), name, "PASS" if ok else "FAIL", dur, str(detail)[:110]), flush=True)

def port_open(port, host="127.0.0.1", timeout=2.0):
    try:
        s = socket.create_connection((host, port), timeout)
        s.close()
        return True
    except OSError:
        return False

def sh(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "timeout"

PROBLEM_ID = 111
SOLUTION_PATH = os.path.join(REPO, "tests", "e2e", "solution.cpp")

def main():
    t0 = time.time()
    record("mq_port_5672", port_open(MQ_PORT), "queue", t0)

    t1 = time.time()
    record("lsm_port_6380", port_open(LSM_PORT), "storage", t1)

    t2 = time.time()
    record("middleware_8898", port_open(MW_PORT), "business middleware", t2)

    t3 = time.time()
    record("orchestrator_8890", port_open(ORCH_PORT), "judge orchestrator", t3)

    t4 = time.time()
    rc, out = sh("docker exec e2e-judge sh -c \"command -v g++\"", 30)
    record("compiler_in_judge", rc == 0, out.strip()[:60], t4)

    t5 = time.time()
    code = open(SOLUTION_PATH).read()
    task_id = "ci_e2e_" + uuid.uuid4().hex[:8]
    payload = {
        "task_id": task_id, "code": code, "language": "cpp",
        "stdin": "", "expected_output": "", "cpu_limit_ms": 2000,
        "mem_limit_bytes": 268435456, "problem_id": PROBLEM_ID,
        "contest_id": 0, "username": "ci_e2e",
    }
    pub = ("import json,pika,sys\n"
           "c=pika.BlockingConnection(pika.ConnectionParameters(\"127.0.0.1\",5672))\n"
           "ch=c.channel()\n"
           "ch.queue_declare(queue=\"oj.judge\",durable=True)\n"
           "ch.basic_publish(exchange=\"\",routing_key=\"oj.judge\",body=sys.stdin.read(),properties=pika.BasicProperties(delivery_mode=2))\n"
           "print(\"published\")\n")
    rc, out = sh("docker run --rm --network host -v " + os.path.join(REPO, "tests", "e2e") + ":/e2e --entrypoint python3 " + JUDGE_IMAGE + " /e2e/submit_task.py /e2e/solution.cpp " + str(PROBLEM_ID) + " " + task_id, 90)
    record("submit_task", rc == 0 and "published" in out, task_id, t5)

    t6 = time.time()
    rc, out = sh("docker run --rm --network host -v " + os.path.join(REPO, "tests", "e2e") + ":/e2e --entrypoint python3 " + JUDGE_IMAGE + " /e2e/wait_result.py " + task_id + " 300", 360)
    final = None
    for line in out.splitlines():
        if line.startswith("FINAL_JSON "):
            try:
                final = json.loads(line[len("FINAL_JSON "):])
            except Exception:
                final = None
    if not final:
        record("judge_verdict", False, "no terminal verdict: " + out.replace(chr(10), " ")[:90], t6)
        return results
    status = str(final.get("status", "")).upper()
    record("judge_verdict", status in TERMINAL, status, t6)

    ac = final.get("ac_count")
    total = final.get("total_cases")
    record("case_counts", isinstance(ac, int) and isinstance(total, int) and total >= 1, "ac=%s total=%s" % (ac, total))

    cases = final.get("cases") or []
    ok_cases = len(cases) >= 1 and all(str(c.get("status", "")).upper() == "AC" for c in cases)
    record("all_cases_ac", ok_cases, "cases=%d" % len(cases))

    cpu = final.get("cpu_ns")
    record("resource_reported", isinstance(cpu, int) and cpu >= 0, "cpu_ns=%s" % cpu)
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
