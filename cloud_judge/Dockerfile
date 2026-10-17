# ---------- Stage 1: build C++ orchestrator & eval_worker ----------
FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y --no-install-recommends cmake make g++ pkg-config libseccomp-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY include ./include
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

# ---------- Stage 2: runtime (Python consumer + C++ binaries) ----------
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends python3 python3-pip g++ libseccomp2 ca-certificates && rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir --break-system-packages pika==1.4.4

WORKDIR /home/admin/eval-worker
COPY --from=builder /src/build/orchestrator build/orchestrator
COPY --from=builder /src/build/eval_worker build/eval_worker
COPY mq_consumer_orch.py mq_producer.py setup_cgroup.sh ./
RUN chmod +x setup_cgroup.sh build/orchestrator build/eval_worker

ENV ORCHESTRATOR_BIN=/home/admin/eval-worker/build/orchestrator ORCH_HTTP_PORT=8890 MQ_HOST=127.0.0.1 MQ_PORT=5672 TASK_QUEUE=oj.judge RESULT_QUEUE=oj.judge.result

CMD ["python3", "mq_consumer_orch.py"]
