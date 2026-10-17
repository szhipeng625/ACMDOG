#!/usr/bin/env python3
"""
ACMDOG Cloud Judge - RabbitMQ Consumer via C++ Orchestrator
Full pipeline: MQ receive → middleware fetch (limits/generators/testcases)
→ generate data → sandbox judge → LRU cleanup by problem
"""
import json, os, subprocess, tempfile, time, uuid, signal, sys, threading
import shutil
import base64
from collections import OrderedDict
from urllib.request import urlopen
import pika
import heapq

# ==================== Cgroup Preemption for Data Prep ====================
def _detect_data_prep_cg():
    """Detect the data_prep cgroup path created by orchestrator."""
    candidates = [
        "/sys/fs/cgroup/system.slice/cloud_judge.service/data_prep",
        "/sys/fs/cgroup/data_prep",
    ]
    # Also try reading orchestrator's own cgroup to find sibling
    try:
        with open("/proc/self/cgroup") as f:
            for line in f:
                if "::" in line:
                    rel = line.strip().split("::")[-1]
                    parent = "/sys/fs/cgroup" + rel
                    candidate = parent + "/data_prep"
                    candidates.insert(0, candidate)
    except Exception:
        pass
    for c in candidates:
        if os.path.exists(c + "/cgroup.procs"):
            return c
    return None

_data_prep_cg_path = None
_original_cg_path = None

def _enter_data_prep_cg():
    """Move current process into data_prep cgroup (low CPU weight)."""
    global _data_prep_cg_path, _original_cg_path
    if _data_prep_cg_path is None:
        _data_prep_cg_path = _detect_data_prep_cg()
    if _data_prep_cg_path and os.path.exists(_data_prep_cg_path + "/cgroup.procs"):
        try:
            # Save original cgroup for restoration
            if _original_cg_path is None:
                with open("/proc/self/cgroup") as f:
                    for line in f:
                        if "::" in line:
                            rel = line.strip().split("::")[-1]
                            _original_cg_path = "/sys/fs/cgroup" + rel
                            break
            with open(_data_prep_cg_path + "/cgroup.procs", "w") as f:
                f.write(str(os.getpid()))
            return True
        except Exception as e:
            print(f"[preempt] enter data_prep failed: {e}", flush=True)
    return False

def _leave_data_prep_cg():
    """Move current process back to original cgroup."""
    global _original_cg_path
    if _original_cg_path and os.path.exists(_original_cg_path + "/cgroup.procs"):
        try:
            with open(_original_cg_path + "/cgroup.procs", "w") as f:
                f.write(str(os.getpid()))
            return True
        except Exception:
            pass
    return False

class DataPrepPreempt:
    """Context manager: enter data_prep cgroup on __enter__, leave on __exit__."""
    def __enter__(self):
        self._entered = _enter_data_prep_cg()
        if self._entered:
            print(f"[preempt] pid={os.getpid()} entered data_prep (low priority)", flush=True)
        return self
    def __exit__(self, *args):
        if self._entered:
            _leave_data_prep_cg()
            print(f"[preempt] pid={os.getpid()} left data_prep (normal priority)", flush=True)


ORCHESTRATOR_BIN = "/home/admin/eval-worker/build/orchestrator"
ORCH_HTTP_PORT = 8890
COMPILE_TIMEOUT = 30
MQ_HOST = "127.0.0.1"
MQ_PORT = 5672
TASK_QUEUE = "oj.judge"
RESULT_QUEUE = "oj.judge.result"
MIDDLEWARE_URL = "https://127.0.0.1:8899"


# ==================== Status Push ====================
def _publish_status(task_id, phase, extra=None):
    """Push task phase change to result queue so SSE can relay to frontend."""
    try:
        evt = {"task_id": task_id, "status": "judging", "phase": phase}
        if extra:
            evt.update(extra)
        conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST, MQ_PORT))
        ch = conn.channel()
        ch.basic_publish(exchange="", routing_key=RESULT_QUEUE,
                         body=json.dumps(evt),
                         properties=pika.BasicProperties(delivery_mode=2))
        conn.close()
    except Exception as e:
        print(f"[consumer] status push error: {e}", flush=True)

# LRU cache for problem data (testcases + compiled generators)
# Key: problem_id, Value: {"data_dir": path, "last_access": timestamp}
_problem_cache = OrderedDict()
_cache_lock = threading.Lock()
CACHE_MAX_PROBLEMS = 20          # max problems cached
CACHE_MAX_AGE_SEC = 3600         # max age per problem (1h)
CACHE_CLEANUP_INTERVAL = 300     # cleanup every 5 min
DATA_ROOT = "/tmp/cloud_judge_data"
CONTEST_DATA_ROOT = "/tmp/cloud_judge_contest"
_contest_data_lock = threading.Lock()
_contest_data_ready = set()

# 竞赛判题优先队列：最小堆维护竞赛任务，按用户提交次数排序（次数少在堆顶）
_pq_lock = threading.Lock()
_contest_pq = []            # 堆元素: (submit_count, seq, body)
_contest_seq = 0            # 入堆序号，保证次数相同按到达先后
_submit_count = {}          # Map: 用户 ID -> 提交次数
_pq_cv = threading.Condition(_pq_lock)

running = True
orch_proc = None

def stop(sig, frame):
    global running, orch_proc
    running = False
    print("[consumer] shutting down...", flush=True)
    if orch_proc:
        try:
            orch_proc.stdin.close()
            orch_proc.terminate()
            orch_proc.wait(timeout=5)
        except Exception:
            orch_proc.kill()

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)

# ==================== SSL context ====================
import ssl as _ssl
_ssl_ctx = _ssl.create_default_context()
_ssl_ctx.check_hostname = False
_ssl_ctx.verify_mode = _ssl.CERT_NONE

def _mw_get(path):
    """GET from middleware."""
    url = MIDDLEWARE_URL + path
    resp = urlopen(url, context=_ssl_ctx, timeout=10)
    return json.loads(resp.read().decode("utf-8"))

# ==================== LRU Cache ====================
def _cache_cleanup():
    """Evict stale or excess problem data dirs, skipping contest-protected problems."""
    now = time.time()
    with _cache_lock:
        # Remove expired entries (skip contest-protected)
        expired = [pid for pid, info in _problem_cache.items()
                   if now - info["last_access"] > CACHE_MAX_AGE_SEC]
        for pid in expired:
            if pid in _contest_data_ready:
                continue
            d = _problem_cache.pop(pid)
            _safe_rmdir(d["data_dir"])
            print(f"[lru] evicted problem {pid} (age > {CACHE_MAX_AGE_SEC}s)", flush=True)
        # Remove excess entries (LRU: pop oldest, skip contest-protected)
        while len(_problem_cache) > CACHE_MAX_PROBLEMS:
            pid, info = _problem_cache.popitem(last=False)
            if pid in _contest_data_ready:
                _problem_cache[pid] = info
                _problem_cache.move_to_end(pid)
                continue
            _safe_rmdir(info["data_dir"])
            print(f"[lru] evicted problem {pid} (cache full)", flush=True)

def _safe_rmdir(path):
    try:
        if path and os.path.isdir(path):
            shutil.rmtree(path)
    except Exception as e:
        print(f"[lru] rmdir error {path}: {e}", flush=True)

def _case_key(filename):
    """Numeric sort key for testcase files: 1.in, 2.in, ..., 10.in, 11.in."""
    base = filename[:-3] if filename.endswith(".in") else filename
    try:
        return (0, int(base), base)
    except ValueError:
        return (1, 0, base)

def sorted_case_files(data_dir):
    """Return .in files under data_dir in natural numeric order."""
    files = [f for f in os.listdir(data_dir) if f.endswith(".in")]
    files.sort(key=_case_key)
    return files

def _cache_touch(problem_id):
    """Mark problem as recently used (move to end of OrderedDict)."""
    with _cache_lock:
        if problem_id in _problem_cache:
            _problem_cache.move_to_end(problem_id)
            _problem_cache[problem_id]["last_access"] = time.time()

def _cache_get(problem_id):
    """Get cached problem data dir, or None."""
    with _cache_lock:
        if problem_id in _problem_cache:
            info = _problem_cache[problem_id]
            if time.time() - info["last_access"] <= CACHE_MAX_AGE_SEC:
                _problem_cache.move_to_end(problem_id)
                info["last_access"] = time.time()
                return info["data_dir"]
            else:
                _safe_rmdir(info["data_dir"])
                del _problem_cache[problem_id]
    return None

def _cache_put(problem_id, data_dir):
    """Store problem data dir in cache, evicting if needed (skipping contest-protected)."""
    with _cache_lock:
        if problem_id in _problem_cache:
            old = _problem_cache.pop(problem_id)
            _safe_rmdir(old["data_dir"])
        _problem_cache[problem_id] = {"data_dir": data_dir, "last_access": time.time()}
        # Evict excess (skip contest-protected)
        while len(_problem_cache) > CACHE_MAX_PROBLEMS:
            pid, info = _problem_cache.popitem(last=False)
            if pid in _contest_data_ready:
                _problem_cache[pid] = info
                _problem_cache.move_to_end(pid)
                continue
            _safe_rmdir(info["data_dir"])
            print(f"[lru] evicted problem {pid} (cache full on put)", flush=True)

def _cleanup_loop():
    """Background thread: periodic LRU cleanup."""
    while running:
        time.sleep(CACHE_CLEANUP_INTERVAL)
        try:
            _cache_cleanup()
        except Exception as e:
            print(f"[lru] cleanup error: {e}", flush=True)


# ==================== Contest Data Management ====================
def _prepare_contest_data(contest_id):
    """Download all problem data for a contest into a dedicated directory."""
    print(f"[contest] preparing data for contest {contest_id}...", flush=True)
    try:
        contests_raw = _mw_get("/api/contests")
        target = None
        for item in contests_raw:
            try:
                c = json.loads(item.get("content", "{}"))
            except Exception:
                continue
            if str(c.get("id")) == str(contest_id):
                target = c
                break
        if not target:
            print(f"[contest] contest {contest_id} not found", flush=True)
            return
        problem_ids = target.get("problems", [])
        if not problem_ids:
            print(f"[contest] contest {contest_id} has no problems", flush=True)
            return
        contest_dir = os.path.join(CONTEST_DATA_ROOT, f"c{contest_id}")
        os.makedirs(contest_dir, exist_ok=True)
        with _contest_data_lock:
            _contest_data_ready.clear()
        for pid in problem_ids:
            try:
                time_ms, mem_mb, data_dir = _fetch_problem_data_inner(pid)
                dest = os.path.join(contest_dir, f"p{pid}")
                if os.path.exists(dest):
                    _safe_rmdir(dest)
                shutil.copytree(data_dir, dest)
                with _contest_data_lock:
                    _contest_data_ready.add(pid)
                print(f"[contest] contest {contest_id} problem {pid} data ready", flush=True)
            except Exception as e:
                print(f"[contest] contest {contest_id} problem {pid} fetch failed: {e}", flush=True)
        print(f"[contest] contest {contest_id} data preparation complete ({len(_contest_data_ready)} problems)", flush=True)
    except Exception as e:
        print(f"[contest] contest {contest_id} preparation error: {e}", flush=True)


def _cleanup_contest_data(contest_id):
    """Remove all contest-specific data directories and clear protection flags.
    比赛结束自动释放沙箱：清空优先队列中待判任务、重置全部用户提交计数。"""
    print(f"[contest] cleaning up data for contest {contest_id}...", flush=True)
    contest_dir = os.path.join(CONTEST_DATA_ROOT, f"c{contest_id}")
    _safe_rmdir(contest_dir)
    with _contest_data_lock:
        _contest_data_ready.clear()
    with _pq_cv:
        dropped = len(_contest_pq)
        _contest_pq.clear()
        _submit_count.clear()
        _pq_cv.notify_all()
    print(f"[contest] contest {contest_id} data cleaned up, priority queue released ({dropped} pending dropped)", flush=True)


def _enqueue_contest_task(body):
    """竞赛任务入堆：该用户提交次数 +1，连同判题信息一起压入最小堆（次数少在堆顶）。"""
    global _contest_seq
    try:
        req = json.loads(body)
    except Exception:
        req = {}
    uid = req.get("username") or "anonymous"
    with _pq_cv:
        _submit_count[uid] = _submit_count.get(uid, 0) + 1
        cnt = _submit_count[uid]
        _contest_seq += 1
        heapq.heappush(_contest_pq, (cnt, _contest_seq, body))
        print(f"[pq] enqueue user={uid} count={cnt} heap_size={len(_contest_pq)}", flush=True)
        _pq_cv.notify()


def _contest_worker():
    """竞赛判题工作线程：从堆顶弹出提交次数最少的任务执行判题，结果回传结果队列。"""
    while True:
        with _pq_cv:
            while not _contest_pq:
                _pq_cv.wait()
            cnt, seq, body = heapq.heappop(_contest_pq)
        try:
            req = json.loads(body)
            print(f"[pq] pop task={req.get('task_id','')} user={req.get('username','')} count={cnt}", flush=True)
            result = process_task(body)
        except Exception as e:
            result = {"task_id": "unknown", "status": "SE", "error": str(e)}
        try:
            conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST, MQ_PORT))
            ch = conn.channel()
            ch.queue_declare(queue=RESULT_QUEUE, durable=True)
            ch.basic_publish(exchange="", routing_key=RESULT_QUEUE,
                             body=json.dumps(result),
                             properties=pika.BasicProperties(delivery_mode=2))
            conn.close()
        except Exception as e:
            print(f"[pq] result publish error: {e}", flush=True)


def _get_contest_data_dir(problem_id):
    """Return contest data dir for a problem if contest data is ready, else None."""
    with _contest_data_lock:
        if problem_id not in _contest_data_ready:
            return None
    for entry in os.listdir(CONTEST_DATA_ROOT):
        contest_dir = os.path.join(CONTEST_DATA_ROOT, entry)
        if not os.path.isdir(contest_dir):
            continue
        candidate = os.path.join(contest_dir, f"p{problem_id}")
        if os.path.isdir(candidate):
            return candidate
    return None

# ==================== Problem Data Fetcher ====================
def fetch_problem_data(problem_id):
    """
    Fetch problem limits + testcases/generators from middleware.
    Returns (time_ms, mem_mb, data_dir) where data_dir contains:
      - N.in / N.out pairs (testcases)
      - or compiled generator binaries
    Uses LRU cache to avoid re-fetching.
    Enters data_prep cgroup (low CPU weight) during data preparation
    so that active judge processes can preempt CPU resources.
    """
    with DataPrepPreempt():
        return _fetch_problem_data_inner(problem_id)

def _fetch_problem_data_inner(problem_id):
    """Inner implementation of fetch_problem_data (runs inside data_prep cgroup)."""
    cached = _cache_get(problem_id)
    if cached and os.path.isdir(cached):
        print(f"[fetch] problem {problem_id} cache hit", flush=True)
        # Still need limits from middleware (lightweight)
        try:
            pdata = _mw_get(f"/api/problem?id={problem_id}")
            return pdata.get("timeMs", 1000), pdata.get("memMb", 256), cached
        except Exception:
            return 1000, 256, cached

    print(f"[fetch] problem {problem_id} cache miss, fetching...", flush=True)
    pdata = _mw_get(f"/api/problem?id={problem_id}")
    time_ms = pdata.get("timeMs", 1000)
    mem_mb = pdata.get("memMb", 256)

    data_dir = os.path.join(DATA_ROOT, f"p{problem_id}")
    os.makedirs(data_dir, exist_ok=True)

    # 1. Try testcases from middleware
    try:
        tcs = _mw_get(f"/api/testcases?problemId={problem_id}")
        if isinstance(tcs, list) and len(tcs) > 0:
            for i, tc in enumerate(tcs, 1):
                inp = tc.get("input", "")
                out = tc.get("output", "")
                with open(os.path.join(data_dir, f"{i}.in"), "w") as f:
                    f.write(inp)
                with open(os.path.join(data_dir, f"{i}.out"), "w") as f:
                    f.write(out)
            print(f"[fetch] problem {problem_id}: {len(tcs)} testcases loaded", flush=True)
            _cache_put(problem_id, data_dir)
            return time_ms, mem_mb, data_dir
    except Exception as e:
        print(f"[fetch] testcases API error: {e}", flush=True)

    # 2. Try generators: compile and run to produce test data
    generators = pdata.get("generators", [])
    tc_idx = 1
    if generators:
        for gen in generators:
            gen_code = gen.get("code", "")
            gen_count = gen.get("genCount", 1)
            if not gen_code:
                continue
            # Compile generator
            gen_src = os.path.join(data_dir, f"gen_{gen['id']}.cpp")
            gen_bin = os.path.join(data_dir, f"gen_{gen['id']}")
            with open(gen_src, "w") as f:
                f.write(gen_code)
            try:
                r = subprocess.run(
                    ["g++", "-O2", "-std=c++17", "-o", gen_bin, gen_src],
                    capture_output=True, timeout=30
                )
                if r.returncode != 0:
                    print(f"[fetch] gen {gen['id']} compile failed: {r.stderr.decode()[:200]}", flush=True)
                    continue
            except Exception as e:
                print(f"[fetch] gen {gen['id']} compile error: {e}", flush=True)
                continue
            # Run generator: capture stdout → .in files
            for seed in range(1, gen_count + 1):
                out_name = f"{tc_idx}.in"
                out_path = os.path.join(data_dir, out_name)
                try:
                    with open(out_path, "wb") as outf:
                        r = subprocess.run(
                            [gen_bin, data_dir, str(seed)],
                            stdout=outf, stderr=subprocess.PIPE,
                            timeout=10, cwd=data_dir
                        )
                    if r.returncode != 0:
                        print(f"[fetch] gen {gen['id']} seed={seed} exit={r.returncode}", flush=True)
                        os.remove(out_path)
                    else:
                        tc_idx += 1
                except Exception as e:
                    print(f"[fetch] gen {gen['id']} seed={seed} error: {e}", flush=True)
            print(f"[fetch] gen {gen['id']} produced {gen_count} datasets", flush=True)

        # Count generated .in files
        in_files = sorted_case_files(data_dir)
        if in_files:
            # Compile stdCode to generate .out answer files
            std_code = pdata.get("stdCode", "")
            if std_code:
                std_src = os.path.join(data_dir, "std.cpp")
                std_bin = os.path.join(data_dir, "std_bin")
                with open(std_src, "w") as f:
                    f.write(std_code)
                try:
                    r = subprocess.run(
                        ["g++", "-O2", "-std=c++17", "-o", std_bin, std_src],
                        capture_output=True, timeout=30
                    )
                    if r.returncode == 0:
                        for inf in in_files:
                            out_name = inf.replace(".in", ".out")
                            out_path = os.path.join(data_dir, out_name)
                            in_path = os.path.join(data_dir, inf)
                            try:
                                with open(in_path, "rb") as inf_f, open(out_path, "wb") as outf_f:
                                    subprocess.run(
                                        [std_bin], stdin=inf_f, stdout=outf_f,
                                        stderr=subprocess.PIPE, timeout=30
                                    )
                            except Exception as e:
                                print(f"[fetch] std run {inf} error: {e}", flush=True)
                        print(f"[fetch] problem {problem_id}: {len(in_files)} answers generated via stdCode", flush=True)
                    else:
                        print(f"[fetch] stdCode compile failed: {r.stderr.decode()[:200]}", flush=True)
                except Exception as e:
                    print(f"[fetch] stdCode compile error: {e}", flush=True)
            print(f"[fetch] problem {problem_id}: {len(in_files)} generated test inputs", flush=True)
            _cache_put(problem_id, data_dir)
            return time_ms, mem_mb, data_dir

    # 3. Fallback: use sampleIn/sampleOut as single testcase
    sample_in = pdata.get("sampleIn", "")
    sample_out = pdata.get("sampleOut", "")
    if sample_in:
        with open(os.path.join(data_dir, "1.in"), "w") as f:
            f.write(sample_in.replace("\r\n", "\n"))
        if sample_out:
            with open(os.path.join(data_dir, "1.out"), "w") as f:
                f.write(sample_out.replace("\r\n", "\n"))
        print(f"[fetch] problem {problem_id}: using sample as testcase", flush=True)

    _cache_put(problem_id, data_dir)
    return time_ms, mem_mb, data_dir

# ==================== Orchestrator Management ====================
def start_orchestrator():
    global orch_proc
    print(f"[consumer] starting orchestrator on port {ORCH_HTTP_PORT}...", flush=True)
    orch_proc = subprocess.Popen(
        ["/home/admin/eval-worker/build/orchestrator", str(ORCH_HTTP_PORT)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    def drain():
        for line in orch_proc.stdout:
            print(f"[orch] {line.rstrip()}", flush=True)
    threading.Thread(target=drain, daemon=True).start()
    time.sleep(0.5)
    print(f"[consumer] orchestrator PID={orch_proc.pid}", flush=True)

def submit_to_orchestrator(task_json):
    global orch_proc
    if not orch_proc or orch_proc.poll() is not None:
        start_orchestrator()
    try:
        orch_proc.stdin.write(task_json + "\n")
        orch_proc.stdin.flush()
        return True
    except Exception as e:
        print(f"[consumer] orchestrator stdin error: {e}, restarting...", flush=True)
        start_orchestrator()
        try:
            orch_proc.stdin.write(task_json + "\n")
            orch_proc.stdin.flush()
            return True
        except Exception as e2:
            print(f"[consumer] orchestrator restart failed: {e2}", flush=True)
            return False

def poll_result(task_id, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            url = f"http://127.0.0.1:{ORCH_HTTP_PORT}/task/{task_id}"
            resp = urlopen(url, timeout=2)
            data = json.loads(resp.read())
            status = data.get("status", "")
            if status == "finished":
                result = data.get("result", {})
                if isinstance(result, str):
                    result = json.loads(result)
                return result
            elif status == "error":
                result = data.get("result", {})
                if isinstance(result, str):
                    try: result = json.loads(result)
                    except: result = {"task_id": task_id, "status": "SE", "error": result}
                return result
        except Exception:
            pass
        time.sleep(0.3)
    return {"task_id": task_id, "status": "SE", "error": "orchestrator poll timeout"}

# ==================== Compile User Code ====================
def compile_code(code, lang, work_dir):
    """Compile user code inside data_prep cgroup to yield CPU to judges."""
    with DataPrepPreempt():
        return _compile_code_inner(code, lang, work_dir)

def _compile_code_inner(code, lang, work_dir):
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

# ==================== 函数式判题（编译用户代码为共享库）====================
def compile_shared(code, lang, work_dir):
    """Compile user code to shared library (functional judging: solve interface)."""
    with DataPrepPreempt():
        return _compile_shared_inner(code, lang, work_dir)

def _compile_shared_inner(code, lang, work_dir):
    src_map = {"c": "solve.c", "cpp": "solve.cpp", "cc": "solve.cpp"}
    cmd_map = {
        "c":   ["gcc", "-O2", "-std=c17", "-shared", "-fPIC", "-o", "user.so", "solve.c", "-lm"],
        "cpp": ["g++", "-O2", "-std=c++17", "-shared", "-fPIC", "-o", "user.so", "solve.cpp", "-lm"],
        "cc":  ["g++", "-O2", "-std=c++17", "-shared", "-fPIC", "-o", "user.so", "solve.cpp", "-lm"],
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
        return os.path.join(work_dir, "user.so"), None
    except subprocess.TimeoutExpired:
        return None, f"Compilation timed out ({COMPILE_TIMEOUT}s)"
    except Exception as e:
        return None, str(e)

# ==================== 判题辅助 ====================
def _b64decode(s):
    try:
        return base64.b64decode(s)
    except Exception:
        return b""

def _normalize(raw):
    """与客户端 judge.cpp normalize 一致：去 BOM、去 \r、去行尾空白、去末尾空行。"""
    s = raw
    if isinstance(s, bytes):
        s = s.decode("utf-8", errors="replace")
    if s.startswith("\ufeff"):
        s = s[1:]
    s = s.replace("\r", "")
    lines = s.split("\n")
    lines = [l.rstrip(" \t") for l in lines]
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines)

def _get_problem_meta(problem_id):
    """拉取题目完整元数据（含 spj/entry/judgeMode），失败返回空 dict。"""
    try:
        return _mw_get(f"/api/problem?id={problem_id}")
    except Exception as e:
        print(f"[consumer] problem meta error: {e}", flush=True)
        return {}

def _prepare_checker(pdata, data_dir):
    """
    准备特殊判题器 spj 与函数式判题入口 entry 的二进制。
    返回 (spj_binary, entry_binary, judge_mode)；二进制为空表示不存在/编译失败。
    """
    judge_mode = 1 if pdata.get("judgeMode") else 0
    spj_binary = ""
    spj_code = pdata.get("spjCode", "") or ""
    spj_exe = pdata.get("spjExe", "") or ""
    if spj_exe:
        spj_binary = os.path.join(data_dir, "spj_bin")
        with open(spj_binary, "wb") as f:
            f.write(_b64decode(spj_exe))
        os.chmod(spj_binary, 0o755)
    elif spj_code:
        spj_src = os.path.join(data_dir, "spj.cpp")
        spj_binary = os.path.join(data_dir, "spj_bin")
        with open(spj_src, "w") as f:
            f.write(spj_code)
        r = subprocess.run(["g++", "-O2", "-std=c++17", "-o", spj_binary, spj_src],
                           capture_output=True, timeout=30)
        if r.returncode != 0:
            print(f"[consumer] spj compile failed: {r.stderr.decode()[:200]}", flush=True)
            spj_binary = ""

    entry_binary = ""
    if judge_mode:
        entry_code = pdata.get("entryCode", "") or ""
        entry_exe = pdata.get("entryExe", "") or ""
        if entry_exe:
            entry_binary = os.path.join(data_dir, "entry_bin")
            with open(entry_binary, "wb") as f:
                f.write(_b64decode(entry_exe))
            os.chmod(entry_binary, 0o755)
        elif entry_code:
            entry_src = os.path.join(data_dir, "entry.cpp")
            entry_binary = os.path.join(data_dir, "entry_bin")
            with open(entry_src, "w") as f:
                f.write(entry_code)
            r = subprocess.run(["g++", "-O2", "-std=c++17", "-o", entry_binary, entry_src],
                               capture_output=True, timeout=30)
            if r.returncode != 0:
                print(f"[consumer] entry compile failed: {r.stderr.decode()[:200]}", flush=True)
                entry_binary = ""
    return spj_binary, entry_binary, judge_mode

def _run_orch(orch_task, sub_task_id, timeout):
    """提交任务到 orchestrator 并等待结果，返回 result dict（status/cpu_ns/mem_bytes/...）。"""
    if not submit_to_orchestrator(json.dumps(orch_task)):
        return {"task_id": sub_task_id, "status": "SE", "error": "orchestrator submit failed"}
    return poll_result(sub_task_id, timeout=timeout)

# ==================== Core: Process Task ====================
def process_task(body):
    req = json.loads(body)
    task_id = req.get("task_id", f"mq_{uuid.uuid4().hex[:10]}")
    code = req.get("code", "")
    lang = req.get("language", "cpp").lower()
    problem_id = req.get("problem_id", 0)

    print(f"[consumer] task={task_id} problem={problem_id} lang={lang}", flush=True)

    _publish_status(task_id, "pending")
    # 1. Fetch problem data (limits + testcases) via middleware, with LRU cache
    #    Contest data takes priority if pre-downloaded
    try:
        contest_dir = _get_contest_data_dir(problem_id)
        if contest_dir:
            pdata = _mw_get(f"/api/problem?id={problem_id}")
            time_ms = pdata.get("timeMs", 1000)
            mem_mb = pdata.get("memMb", 256)
            data_dir = contest_dir
            print(f"[consumer] task={task_id} using contest data for problem {problem_id}", flush=True)
        else:
            time_ms, mem_mb, data_dir = fetch_problem_data(problem_id)
        _publish_status(task_id, "fetched")
    except Exception as e:
        print(f"[consumer] {task_id} fetch error: {e}", flush=True)
        return {"task_id": task_id, "status": "SE", "error": f"fetch problem data: {e}"}

    # 1b. 拉取题目元数据（judgeMode / spj / entry），准备判定器与入口
    pdata = _get_problem_meta(problem_id)
    spj_binary, entry_binary, judge_mode = _prepare_checker(pdata, data_dir)

    cpu_ms = min(time_ms, 10000)
    mem_bytes = min(mem_mb * 1024 * 1024, 536870912)

    # 2. Compile user code（函数式判题编译为共享库 user.so）
    work_dir = os.path.join(tempfile.gettempdir(), f"judge_{task_id}")
    os.makedirs(work_dir, exist_ok=True)
    _publish_status(task_id, "compiling")
    if judge_mode:
        if not entry_binary:
            shutil.rmtree(work_dir, ignore_errors=True)
            return {"task_id": task_id, "status": "SE", "error": "题目入口缺失（entry 不存在或编译失败）"}
        binary, err = compile_shared(code, lang, work_dir)
    else:
        binary, err = compile_code(code, lang, work_dir)
    if err:
        print(f"[consumer] {task_id} -> CE", flush=True)
        shutil.rmtree(work_dir, ignore_errors=True)
        return {"task_id": task_id, "status": "CE", "error": err}

    # 3. Find testcase pairs
    in_files = sorted_case_files(data_dir)
    if not in_files:
        print(f"[consumer] {task_id} no testcases found", flush=True)
        shutil.rmtree(work_dir, ignore_errors=True)
        return {"task_id": task_id, "status": "SE", "error": "no testcases"}

    _publish_status(task_id, "running", {"total_cases": len(in_files)})
    # 4. Judge each testcase through orchestrator → eval_worker
    total_cpu_ns = 0
    max_mem = 0
    worst_status = "AC"
    first_error = ""
    ac_count = 0
    cases_out = []   # 逐测试点结果（客户端按点展示）

    for in_file in in_files:
        tc_name = in_file.replace(".in", "")
        out_file = os.path.join(data_dir, tc_name + ".out")
        expected_file = out_file if os.path.exists(out_file) else ""
        stdin_file = os.path.join(data_dir, in_file)
        sub_task_id = f"{task_id}_{tc_name}"

        st = "SE"
        case_err = ""
        case_cpu = 0
        case_mem = 0

        if judge_mode:
            # 函数式判题：entry <user.so> <输入> <输出> <时间>
            # 输出/时间文件放 /tmp（世界可写）：沙箱以 nobody 身份运行 entry，需能写这些文件。
            out_tmp = f"/tmp/judge_{task_id}_{tc_name}.out"
            time_tmp = f"/tmp/judge_{task_id}_{tc_name}.time"
            orch_task = {
                "task_id": sub_task_id,
                "user_binary": entry_binary,
                "args": [binary, stdin_file, out_tmp, time_tmp],
                "stdin_file": "",
                "expected_output_file": "",
                "cpu_limit_ms": cpu_ms,
                "mem_limit_bytes": mem_bytes,
                "max_pids": 32,
            }
            result = _run_orch(orch_task, sub_task_id, timeout=cpu_ms // 1000 * 3 + 10)
            st = result.get("status", "SE")
            case_cpu = result.get("cpu_ns", 0)
            case_mem = result.get("mem_bytes", 0)
            if st == "AC":
                try:
                    with open(out_tmp, "rb") as f:
                        user_out = f.read()
                except Exception:
                    user_out = b""
                if expected_file:
                    try:
                        with open(expected_file, "rb") as f:
                            exp_out = f.read()
                    except Exception:
                        exp_out = b""
                    if _normalize(user_out) == _normalize(exp_out):
                        st = "AC"
                    else:
                        st = "WA"
                        case_err = "输出与标准答案不一致"
                else:
                    st = "WA"
                    case_err = "输出与标准答案不一致"
            elif st == "TLE":
                case_err = "运行超时"
            elif st == "MLE":
                case_err = "内存超限"
            elif st == "RE":
                case_err = result.get("error", "") or result.get("stderr", "") or "运行时错误"
            elif st == "SE":
                case_err = result.get("error", "") or "系统错误"

        elif spj_binary:
            # 特殊判题：先跑用户程序（stdin → stdout），再跑 spj <用户输出> <标准输出> <输入>
            user_out_path = f"/tmp/eval_{sub_task_id}/stdout.txt"
            orch_task = {
                "task_id": sub_task_id,
                "user_binary": binary,
                "stdin_file": stdin_file,
                "expected_output_file": "",
                "cpu_limit_ms": cpu_ms,
                "mem_limit_bytes": mem_bytes,
                "max_pids": 32,
            }
            result = _run_orch(orch_task, sub_task_id, timeout=cpu_ms // 1000 * 3 + 10)
            st = result.get("status", "SE")
            case_cpu = result.get("cpu_ns", 0)
            case_mem = result.get("mem_bytes", 0)
            if st == "AC":
                spj_task_id = f"{sub_task_id}_spj"
                spj_task = {
                    "task_id": spj_task_id,
                    "user_binary": spj_binary,
                    "args": [user_out_path, expected_file, stdin_file],
                    "stdin_file": "",
                    "expected_output_file": "",
                    "checker": "exitcode",
                    "cpu_limit_ms": 5000,
                    "mem_limit_bytes": mem_bytes,
                    "max_pids": 32,
                }
                spj_result = _run_orch(spj_task, spj_task_id, timeout=15)
                case_cpu += spj_result.get("cpu_ns", 0)
                case_mem = max(case_mem, spj_result.get("mem_bytes", 0))
                sst = spj_result.get("status", "SE")
                if sst == "AC":
                    st = "AC"
                elif sst == "WA":
                    st = "WA"
                    case_err = spj_result.get("error", "") or "special judge 未通过"
                else:
                    st = "SE"
                    case_err = "special judge 运行失败"
            elif st == "TLE":
                case_err = "运行超时"
            elif st == "MLE":
                case_err = "内存超限"
            elif st == "RE":
                case_err = result.get("error", "") or result.get("stderr", "") or "运行时错误"
            elif st == "SE":
                case_err = result.get("error", "") or "系统错误"

        else:
            # 普通判题：eval_worker 直接比对 stdout 与期望输出
            orch_task = {
                "task_id": sub_task_id,
                "user_binary": binary,
                "stdin_file": stdin_file,
                "expected_output_file": expected_file,
                "cpu_limit_ms": cpu_ms,
                "mem_limit_bytes": mem_bytes,
                "max_pids": 32,
            }
            result = _run_orch(orch_task, sub_task_id, timeout=cpu_ms // 1000 * 3 + 10)
            st = result.get("status", "SE")
            case_cpu = result.get("cpu_ns", 0)
            case_mem = result.get("mem_bytes", 0)
            if st == "WA":
                case_err = "输出与标准答案不一致"
            elif st == "TLE":
                case_err = "运行超时"
            elif st == "MLE":
                case_err = "内存超限"
            elif st == "RE":
                case_err = result.get("error", "") or result.get("stderr", "") or "运行时错误"
            elif st == "SE":
                case_err = result.get("error", "") or "系统错误"

        total_cpu_ns += case_cpu
        max_mem = max(max_mem, case_mem)
        if st == "AC":
            ac_count += 1
        else:
            if worst_status == "AC":
                worst_status = st
            if not first_error:
                first_error = case_err or st
        cases_out.append({"name": tc_name, "status": st,
                          "cpu_ms": int(case_cpu // 1000000), "error": case_err})

        print(f"[consumer] {task_id} tc={tc_name} -> {st}", flush=True)
        _publish_status(task_id, "running", {"ac_count": ac_count, "done_cases": in_files.index(in_file)+1, "total_cases": len(in_files)})

    # 5. Cleanup work dir (keep problem data in LRU cache)
    shutil.rmtree(work_dir, ignore_errors=True)

    final = {
        "task_id": task_id,
        "problem_id": problem_id,
        "status": worst_status,
        "ac_count": ac_count,
        "total_cases": len(in_files),
        "cpu_ns": total_cpu_ns,
        "mem_bytes": max_mem,
        "cases": cases_out,
    }
    if first_error and worst_status != "AC":
        final["error"] = first_error[:500]

    print(f"[consumer] {task_id} -> {worst_status} ({ac_count}/{len(in_files)} AC)", flush=True)
    return final

# ==================== Main ====================
def main():
    os.makedirs(DATA_ROOT, exist_ok=True)
    start_orchestrator()

    # Start LRU cleanup thread
    threading.Thread(target=_cleanup_loop, daemon=True).start()
    # Start contest priority-queue judge worker
    threading.Thread(target=_contest_worker, daemon=True).start()
    print(f"[pq] contest judge worker started (min-heap by submit count)", flush=True)
    print(f"[lru] cleanup thread started (interval={CACHE_CLEANUP_INTERVAL}s, max={CACHE_MAX_PROBLEMS} problems, max_age={CACHE_MAX_AGE_SEC}s)", flush=True)

    print(f"[consumer] connecting to {MQ_HOST}:{MQ_PORT}...", flush=True)
    conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST, MQ_PORT))
    ch = conn.channel()
    ch.queue_declare(queue=TASK_QUEUE, durable=True)
    ch.queue_declare(queue=RESULT_QUEUE, durable=True)
    ch.basic_qos(prefetch_count=1)

    def callback(ch, method, properties, body):
        try:
            raw = body.decode("utf-8")
            req = json.loads(raw)
            msg_type = req.get("type", "")
            if msg_type == "contest_data_prepare":
                cid = req.get("contest_id", "")
                print(f"[consumer] contest_data_prepare contest={cid}", flush=True)
                threading.Thread(target=_prepare_contest_data, args=(cid,), daemon=True).start()
            elif msg_type == "contest_data_cleanup":
                cid = req.get("contest_id", "")
                print(f"[consumer] contest_data_cleanup contest={cid}", flush=True)
                threading.Thread(target=_cleanup_contest_data, args=(cid,), daemon=True).start()
            else:
                if req.get("contest_id", 0):
                    _enqueue_contest_task(raw)
                else:
                    result = process_task(raw)
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
    print(f"[consumer] listening on '{TASK_QUEUE}' → orchestrator → eval_worker", flush=True)
    print(f"[consumer] data pipeline: middleware fetch → LRU cache → generate → judge → cleanup", flush=True)

    while running:
        try:
            conn.process_data_events(time_limit=1)
        except Exception:
            break

    conn.close()
    if orch_proc:
        try:
            orch_proc.stdin.close()
            orch_proc.terminate()
            orch_proc.wait(timeout=5)
        except Exception:
            orch_proc.kill()
    print("[consumer] stopped.", flush=True)

if __name__ == "__main__":
    main()
