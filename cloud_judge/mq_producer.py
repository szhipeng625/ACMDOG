#!/usr/bin/env python3
"""
ACMDOG Cloud Judge - RabbitMQ Producer (test/demo)
Sends a judge task to 'oj.judge' and waits for result on 'oj.judge.result'.
Usage: python3 mq_producer.py [code_file] [language]
"""
import json, sys, uuid, time
import pika

MQ_HOST = "127.0.0.1"
TASK_QUEUE = "oj.judge"
RESULT_QUEUE = "oj.judge.result"

# Default test: A+B problem
DEFAULT_CODE = """
#include <iostream>
using namespace std;
int main() {
    int a, b;
    cin >> a >> b;
    cout << a + b << endl;
    return 0;
}
"""

def main():
    code_file = sys.argv[1] if len(sys.argv) > 1 else None
    lang = sys.argv[2] if len(sys.argv) > 2 else "cpp"

    if code_file:
        with open(code_file) as f:
            code = f.read()
    else:
        code = DEFAULT_CODE

    task_id = f"test_{uuid.uuid4().hex[:10]}"
    task = {
        "task_id": task_id,
        "code": code,
        "language": lang,
        "stdin": "3 5\n",
        "expected_output": "8\n",
        "cpu_limit_ms": 1000,
        "mem_limit_bytes": 67108864,
    }

    conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST))
    ch = conn.channel()
    ch.queue_declare(queue=TASK_QUEUE, durable=True)
    ch.queue_declare(queue=RESULT_QUEUE, durable=True)

    # Publish task
    ch.basic_publish(exchange="", routing_key=TASK_QUEUE,
                     body=json.dumps(task),
                     properties=pika.BasicProperties(delivery_mode=2))
    print(f"[producer] sent task {task_id} to '{TASK_QUEUE}'")

    # Wait for result (up to 30s)
    deadline = time.time() + 30
    while time.time() < deadline:
        method, props, body = ch.basic_get(queue=RESULT_QUEUE, auto_ack=True)
        if method:
            result = json.loads(body)
            if result.get("task_id") == task_id:
                print(f"[producer] result: {json.dumps(result, indent=2)}")
                conn.close()
                return
        time.sleep(0.2)

    print(f"[producer] timeout waiting for {task_id}")
    conn.close()

if __name__ == "__main__":
    main()
