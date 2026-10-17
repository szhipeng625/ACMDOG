import json
import sys
import time
import pika
from urllib.request import urlopen

TERMINAL = ("AC","WA","CE","RE","TLE","MLE","SE","OLE","PE","DONE","FAILED")
tid = sys.argv[1]
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0

conn = pika.BlockingConnection(pika.ConnectionParameters("127.0.0.1", 5672))
ch = conn.channel()
ch.queue_declare(queue="oj.judge.result", durable=True)

deadline = time.time() + timeout
final = None
while time.time() < deadline:
    method, props, body = ch.basic_get(queue="oj.judge.result", auto_ack=True)
    if method:
        try:
            data = json.loads(body)
        except Exception:
            continue
        if data.get("task_id") != tid:
            continue
        st = str(data.get("status", "")).upper()
        print("event %s %s" % (st, data.get("phase", "")), flush=True)
        if st in TERMINAL:
            final = data
            break
        continue
    time.sleep(1)

if final is None:
    try:
        raw = urlopen("http://127.0.0.1:8890/status", timeout=5).read().decode()
        tasks = json.loads(raw)
        subs = [t for t in tasks if str(t.get("task_id", "")).startswith(tid + "_")]
        if subs and all(str(t.get("status", "")) == "finished" for t in subs):
            sts = [str((t.get("result") or {}).get("status", "")).upper() for t in subs]
            ok = all(s in TERMINAL for s in sts)
            final = {"task_id": tid, "status": "AC" if ok else sts[0], "total_cases": len(subs), "ac_count": sum(1 for s in sts if s == "AC"), "cases": [{"name": str(i + 1), "status": s} for i, s in enumerate(sts)], "source": "orchestrator_status"}
            print("fallback_used", flush=True)
    except Exception as e:
        print("fallback_error", e, flush=True)

print("FINAL_JSON " + json.dumps(final, ensure_ascii=False))
sys.exit(0 if final else 1)
