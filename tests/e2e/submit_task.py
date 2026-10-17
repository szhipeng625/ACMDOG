import json
import sys
import pika
code = open(sys.argv[1]).read()
problem_id = int(sys.argv[2])
task_id = sys.argv[3]
task = {"task_id": task_id, "code": code, "language": "cpp", "stdin": "", "expected_output": "", "cpu_limit_ms": 2000, "mem_limit_bytes": 268435456, "problem_id": problem_id, "contest_id": 0, "username": "ci_e2e"}
conn = pika.BlockingConnection(pika.ConnectionParameters("127.0.0.1", 5672))
ch = conn.channel()
ch.queue_declare(queue="oj.judge", durable=True)
ch.basic_publish(exchange="", routing_key="oj.judge", body=json.dumps(task), properties=pika.BasicProperties(delivery_mode=2))
conn.close()
print("published " + task_id)
