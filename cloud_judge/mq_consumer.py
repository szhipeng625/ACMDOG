#!/usr/bin/env python3
"""
ACMDOG Cloud Judge - RabbitMQ Consumer
Listens on queue 'oj.judge', invokes eval_worker sandbox, publishes result to 'oj.judge.result'.
"""
import json, os, subprocess, tempfile, time, uuid, signal, sys
import pika

EVAL_WORKER = "/home/admin/eval-worker/build/eval_worker"
COMPILE_TIMEOUT = 30
MQ_HOST = "127.0.0.1"
MQ_PORT = 5672
TASK_QUEUE = "oj.judge"
RESULT_QUEUE = "oj.judge.result"

running = True
def stop(sig, frame):
    global running
    running = False
    print("[consumer] shutting down...")
signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)

def compile_code(code, lang, work_dir):
    src_map = {"c": "main.c", "cpp": "main.cpp", "cc": "main.cpp"}
    cmd_map = {
        "c":   ["gcc", "-O2", "-std=c17", "-o", "user_bin", "main.c", "-lm"],
        "cpp": ["g++", "-O2", "-std=c++17", "-o", "user_bin", "main.cpp", "-lm"],
        "cc":  ["g++", "-O2", "-std=c++17", "-o", "user_bin", "main.cpp", "-lm"],
    }
    ext = src_map.get(lang)
    if not ext:
        return None, f"Unsupported language: {lang}"
    with open(os.path.join(work_dir, ext), "w") as f:
        f.write(code)
    try:
        r = subprocess.run(cmd_map[lang], cwd=work_dir, capture_output=True, timeout=COMPILE_TIMEOUT)
        if r.returncode != 0:
            return None, r.stderr.decode("utf-8", errors="replace")[:4096]
        return os.path.join(work_dir, "user_bin"), None
    except subprocess.TimeoutExpired:
        return None, f"Compilation timed out ({COMPILE_TIMEOUT}s)"
    except Exception as e:
        return None, str(e)

def run_sandbox(task_id, binary, stdin_file, expected_file, cpu_ms, mem_bytes):
    payload = json.dumps({
        "task_id": task_id,
        "user_binary": binary,
        "stdin_file": stdin_file or "",
        "expected_output_file": expected_file or "",
        "cpu_limit_ms": cpu_ms,
        "mem_limit_bytes": mem_bytes,
        "max_pids": 32,
    })
    try:
        r = subprocess.run([EVAL_WORKER], input=payload + "\n",
                           capture_output=True, timeout=60, text=True)
        for line in r.stdout.strip().split("\n"):
            line = line.strip()
            if line.startswith("{"):
                return json.loads(line)
        return {"task_id": task_id, "status": "SE", "error": "no JSON output"}
    except Exception as e:
        return {"task_id": task_id, "status": "SE", "error": str(e)}

def process_task(body):
    req = json.loads(body)
    task_id = req.get("task_id", f"mq_{uuid.uuid4().hex[:10]}")
    code = req.get("code", "")
    lang = req.get("language", "cpp").lower()
    stdin_data = req.get("stdin", "")
    expected = req.get("expected_output", "")
    cpu_ms = min(int(req.get("cpu_limit_ms", 1000)), 10000)
    mem_bytes = min(int(req.get("mem_limit_bytes", 268435456)), 536870912)

    print(f"[consumer] task={task_id} lang={lang} cpu={cpu_ms}ms mem={mem_bytes}")

    work_dir = os.path.join(tempfile.gettempdir(), f"judge_{task_id}")
    os.makedirs(work_dir, exist_ok=True)

    # Compile
    binary, err = compile_code(code, lang, work_dir)
    if err:
        result = {"task_id": task_id, "status": "CE", "error": err}
        print(f"[consumer] {task_id} -> CE")
        return result

    # Prepare IO files
    stdin_file = ""
    if stdin_data:
        stdin_file = os.path.join(work_dir, "input.txt")
        with open(stdin_file, "w") as f:
            f.write(stdin_data)
    expected_file = ""
    if expected:
        expected_file = os.path.join(work_dir, "expected.txt")
        with open(expected_file, "w") as f:
            f.write(expected)

    # Run sandbox
    result = run_sandbox(task_id, binary, stdin_file, expected_file, cpu_ms, mem_bytes)
    print(f"[consumer] {task_id} -> {result.get('status', '?')}")
    return result

def main():
    print(f"[consumer] connecting to {MQ_HOST}:{MQ_PORT}...")
    conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST, MQ_PORT))
    ch = conn.channel()
    ch.queue_declare(queue=TASK_QUEUE, durable=True)
    ch.queue_declare(queue=RESULT_QUEUE, durable=True)
    ch.basic_qos(prefetch_count=1)

    def callback(ch, method, properties, body):
        try:
            result = process_task(body.decode("utf-8"))
            ch.basic_publish(exchange="", routing_key=RESULT_QUEUE,
                             body=json.dumps(result),
                             properties=pika.BasicProperties(delivery_mode=2))
        except Exception as e:
            err_result = {"task_id": "unknown", "status": "SE", "error": str(e)}
            ch.basic_publish(exchange="", routing_key=RESULT_QUEUE,
                             body=json.dumps(err_result),
                             properties=pika.BasicProperties(delivery_mode=2))
        finally:
            ch.basic_ack(delivery_tag=method.delivery_tag)

    ch.basic_consume(queue=TASK_QUEUE, on_message_callback=callback)
    print(f"[consumer] listening on '{TASK_QUEUE}', publishing to '{RESULT_QUEUE}'")

    while running:
        try:
            conn.process_data_events(time_limit=1)
        except Exception:
            break

    conn.close()
    print("[consumer] stopped.")

if __name__ == "__main__":
    main()
