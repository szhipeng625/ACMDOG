#!/usr/bin/env python3
"""ACMDOG Cloud Judge API - Lightweight HTTP bridge"""
import json, uuid, time, threading, ssl as _ssl
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from urllib.request import urlopen, Request
from urllib.error import HTTPError
import pika, re
from datetime import datetime

MQ_HOST = "127.0.0.1"
MQ_PORT = 5672
TASK_QUEUE = "oj.judge"
RESULT_QUEUE = "oj.judge.result"
MIDDLEWARE_URL = "https://127.0.0.1:8899"
LISTEN_PORT = 18081
TIMEOUT_SEC = 30

def _trigger_vh_init(username):
    """Trigger virtual human account creation for OJ user."""
    try:
        import urllib.request
        body = json.dumps({"username": username}).encode()
        req = urllib.request.Request("http://127.0.0.1:18082/api/ai/vh_info",
            data=body, method="POST",
            headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=5)
    except Exception:
        pass

_ssl_ctx = _ssl.create_default_context()
_ssl_ctx.check_hostname = False
_ssl_ctx.verify_mode = _ssl.CERT_NONE

_results = {}
_results_ts = {}
_results_lock = threading.Lock()
_task_meta = {}  # 持久保存 username/problem，避免被 SSE pop 消费掉

# ==================== 竞赛（Contest）功能 ====================
_CID_FONT = "STSong-Light"
_cid_font_ready = False


def _parse_ct_time(s):
    """解析竞赛时间字符串为时间戳（服务器本地时区），失败返回 None"""
    s = (s or "").strip()
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"):
        try:
            return datetime.strptime(s, fmt).timestamp()
        except Exception:
            pass
    return None


def _fetch_contests():
    """从中间件获取竞赛列表并解析 content 字段"""
    raw = json.loads(_proxy_get("/api/contests"))
    contests = []
    for item in raw:
        try:
            c = json.loads(item.get("content", "{}"))
        except Exception:
            continue
        c["id"] = item.get("id", c.get("id"))
        contests.append(c)
    return contests


def _contest_summary(c):
    return {
        "id": c.get("id"),
        "name": c.get("name", ""),
        "description": c.get("description", ""),
        "startTime": c.get("startTime", ""),
        "endTime": c.get("endTime", ""),
        "startTs": _parse_ct_time(c.get("startTime", "")),
        "endTs": _parse_ct_time(c.get("endTime", "")),
        "problems": c.get("problems", []),
    }


def _md_inline(text):
    """转义特殊字符并处理行内代码/粗体/斜体"""
    text = str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    text = re.sub(r"`([^`]+)`", r'<font backColor="#f0f0f0">\1</font>', text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"\*([^*\n]+)\*", r"<i>\1</i>", text)
    return text


def _md_to_flowables(md_text, styles):
    """把 Markdown 题面粗略转换为 reportlab 段落列表"""
    from reportlab.platypus import Paragraph, Preformatted
    lines = (md_text or "").replace("\r\n", "\n").replace("\r", "\n").split("\n")
    flow, para_buf, code_buf = [], [], []
    in_code = False

    def flush_para():
        if para_buf:
            t = " ".join(x.strip() for x in para_buf).strip()
            if t:
                flow.append(Paragraph(_md_inline(t), styles["body"]))
            para_buf.clear()

    for ln in lines:
        s = ln.strip()
        if s.startswith("```"):
            if in_code:
                flow.append(Preformatted("\n".join(code_buf) or " ", styles["code"]))
                code_buf = []
                in_code = False
            else:
                flush_para()
                in_code = True
            continue
        if in_code:
            code_buf.append(ln.rstrip())
            continue
        if not s:
            flush_para()
            continue
        if s.startswith("####"):
            flush_para(); flow.append(Paragraph(_md_inline(s[4:].strip()), styles["h4"]))
        elif s.startswith("###"):
            flush_para(); flow.append(Paragraph(_md_inline(s[3:].strip()), styles["h3"]))
        elif s.startswith("##"):
            flush_para(); flow.append(Paragraph(_md_inline(s[2:].strip()), styles["h2"]))
        elif s.startswith("#"):
            flush_para(); flow.append(Paragraph(_md_inline(s[1:].strip()), styles["h2"]))
        elif s.startswith("- ") or s.startswith("* "):
            flush_para(); flow.append(Paragraph("\u2022 " + _md_inline(s[2:].strip()), styles["body"]))
        else:
            para_buf.append(s)
    if in_code and code_buf:
        flow.append(Preformatted("\n".join(code_buf), styles["code"]))
    flush_para()
    return flow


def _build_contest_pdf(contest, problems):
    """生成竞赛全部题面合并后的 PDF 字节"""
    global _cid_font_ready
    from io import BytesIO
    from reportlab.lib.pagesizes import A4
    from reportlab.lib.units import mm
    from reportlab.lib.styles import ParagraphStyle
    from reportlab.lib.enums import TA_CENTER
    from reportlab.lib.fonts import addMapping
    from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, PageBreak, Preformatted
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.cidfonts import UnicodeCIDFont
    if not _cid_font_ready:
        pdfmetrics.registerFont(UnicodeCIDFont(_CID_FONT))
        addMapping(_CID_FONT, 0, 0, _CID_FONT)
        addMapping(_CID_FONT, 1, 0, _CID_FONT)
        addMapping(_CID_FONT, 0, 1, _CID_FONT)
        addMapping(_CID_FONT, 1, 1, _CID_FONT)
        _cid_font_ready = True
    styles = {
        "title": ParagraphStyle("ct", fontName=_CID_FONT, fontSize=22, leading=30, alignment=TA_CENTER, spaceAfter=8),
        "meta": ParagraphStyle("cm", fontName=_CID_FONT, fontSize=10.5, leading=15, alignment=TA_CENTER, textColor="#666666"),
        "ptitle": ParagraphStyle("pt", fontName=_CID_FONT, fontSize=16, leading=22, spaceBefore=4, spaceAfter=6),
        "h2": ParagraphStyle("h2", fontName=_CID_FONT, fontSize=13, leading=18, spaceBefore=10, spaceAfter=4),
        "h3": ParagraphStyle("h3", fontName=_CID_FONT, fontSize=12, leading=17, spaceBefore=8, spaceAfter=3),
        "h4": ParagraphStyle("h4", fontName=_CID_FONT, fontSize=11, leading=16, spaceBefore=6, spaceAfter=2),
        "body": ParagraphStyle("bd", fontName=_CID_FONT, fontSize=10.5, leading=16, spaceAfter=4),
        "code": ParagraphStyle("cd", fontName=_CID_FONT, fontSize=9.5, leading=13, backColor="#f5f5f5", borderPadding=6, leftIndent=6, spaceBefore=4, spaceAfter=8),
        "sample": ParagraphStyle("sp", fontName=_CID_FONT, fontSize=9.5, leading=13, backColor="#eef3ff", borderPadding=6, leftIndent=6, spaceBefore=2, spaceAfter=8),
    }
    buf = BytesIO()
    doc = SimpleDocTemplate(buf, pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm,
                            topMargin=18 * mm, bottomMargin=18 * mm,
                            title=str(contest.get("name", "contest")))
    story = [Paragraph(_md_inline(contest.get("name", "\u7ade\u8d5b\u9898\u9762")), styles["title"])]
    story.append(Paragraph(_md_inline("\u65f6\u95f4\uff1a%s ~ %s" % (contest.get("startTime", ""), contest.get("endTime", ""))), styles["meta"]))
    story.append(Spacer(1, 6 * mm))
    for idx, p in enumerate(problems, 1):
        if idx > 1:
            story.append(PageBreak())
        story.append(Paragraph(_md_inline("\u7b2c %d \u9898\uff1a%s" % (idx, p.get("title", ""))), styles["ptitle"]))
        story.append(Paragraph(_md_inline("\u65f6\u95f4\u9650\u5236 %s ms\u3000\u5185\u5b58\u9650\u5236 %s MB" % (p.get("timeMs", ""), p.get("memMb", ""))), styles["meta"]))
        story.append(Spacer(1, 2 * mm))
        story.extend(_md_to_flowables(p.get("description", ""), styles))
        si = (p.get("sampleIn") or "").strip()
        so = (p.get("sampleOut") or "").strip()
        if si:
            story.append(Paragraph("\u6837\u4f8b\u8f93\u5165", styles["h3"]))
            story.append(Preformatted(si, styles["sample"]))
        if so:
            story.append(Paragraph("\u6837\u4f8b\u8f93\u51fa", styles["h3"]))
            story.append(Preformatted(so, styles["sample"]))
    doc.build(story)
    return buf.getvalue()

def _mq_consume_results():
    while True:
        try:
            conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST, MQ_PORT))
            ch = conn.channel()
            ch.queue_declare(queue=RESULT_QUEUE, durable=True)
            def on_msg(ch, method, props, body):
                try:
                    r = json.loads(body)
                    tid = r.get("task_id", "")
                    if tid:
                        with _results_lock:
                            # Merge with persistent task meta (username, problem)
                            existing = _task_meta.get(tid, {})
                            print('[api] DEBUG mq_result: tid=' + str(tid) + ' status=' + str(r.get('status')) + ' username=' + str(r.get('username')), flush=True)
                            for k, v in existing.items():
                                if k not in r:
                                    r[k] = v
                            # Trigger AI encouragement on final result
                            status = r.get("status", "")
                            if status and status not in ("judging", "pending"):
                                try:
                                    username = r.get("username", "")
                                    problem = r.get("problem", "未知题目")
                                    if username:
                                        ai_body = json.dumps({"username": username, "problem": problem, "status": status}).encode()
                                        ai_req = Request("http://127.0.0.1:18082/api/ai/judge_event", data=ai_body, headers={"Content-Type": "application/json"})
                                        ai_resp = urlopen(ai_req, timeout=10)
                                        ai_data = json.loads(ai_resp.read().decode())
                                        if "reply" in ai_data:
                                            r["ai_message"] = {"reply": ai_data["reply"], "status": status}
                                except Exception as e:
                                    print(f'[api] AI trigger failed: {e}', flush=True)
                            # 竞赛提交：写入数据库（与客户端逻辑一致，维护榜单）
                            try:
                                if status and status not in ("judging", "pending"):
                                    cid = existing.get("contest_id", 0)
                                    tk = existing.get("token", "")
                                    if cid and tk:
                                        detail = (r.get("error") or r.get("stderr") or "")[:200]
                                        time_ms = int(r.get("cpu_ns", 0) / 1e6) if r.get("cpu_ns") else 0
                                        sb = json.dumps({"problemId": r.get("problem_id", 0), "contestId": cid,
                                                         "verdict": status, "detail": detail, "timeMs": time_ms}).encode()
                                        sreq = Request(MIDDLEWARE_URL + "/api/submit", data=sb, method="POST",
                                                       headers={"Content-Type": "application/json"})
                                        sreq.add_header("Authorization", f"Bearer {tk}")
                                        sresp = json.loads(urlopen(sreq, context=_ssl_ctx, timeout=10).read().decode())
                                        print(f'[api] contest submit recorded cid={cid} verdict={status} sid={sresp.get("sid")}', flush=True)
                            except Exception as e:
                                print(f'[api] contest submit record failed: {e}', flush=True)
                            r.pop("token", None)
                            r.pop("contest_id", None)
                            _results[tid] = r
                            _results_ts[tid] = time.time()
                except Exception:
                    pass
                finally:
                    ch.basic_ack(delivery_tag=method.delivery_tag)
            ch.basic_consume(queue=RESULT_QUEUE, on_message_callback=on_msg)
            print("[api] result consumer connected", flush=True)
            ch.start_consuming()
        except Exception as e:
            print(f"[api] result consumer error: {e}, retrying...", flush=True)
            time.sleep(3)

ANIME_TUNNEL = "http://127.0.0.1:18091"

def _tunnel_get(path):
    """GET 经 SSH 隧道访问动漫智能体服务。"""
    resp = urlopen(ANIME_TUNNEL + path, timeout=15)
    return resp.read().decode("utf-8")

def _tunnel_post(path, body, timeout=300):
    """POST 经 SSH 隧道访问动漫智能体服务。"""
    req = Request(ANIME_TUNNEL + path, data=body.encode("utf-8"), method="POST")
    req.add_header("Content-Type", "application/json")
    resp = urlopen(req, timeout=timeout)
    return resp.read().decode("utf-8")

def _proxy_get(path, token=None):
    url = MIDDLEWARE_URL + path
    req = Request(url, method="GET")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        resp = urlopen(req, context=_ssl_ctx, timeout=10)
        return resp.read().decode("utf-8")
    except HTTPError as e:
        return e.read().decode("utf-8")

def _proxy_post(path, body):
    from urllib.request import Request
    url = MIDDLEWARE_URL + path
    req = Request(url, data=body.encode("utf-8"), method="POST")
    req.add_header("Content-Type", "application/json")
    try:
        resp = urlopen(req, context=_ssl_ctx, timeout=10)
        return resp.read().decode("utf-8")
    except HTTPError as e:
        return e.read().decode("utf-8")

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print(f"[api] {fmt % args}", flush=True)

    def _send_json(self, code, data):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        from urllib.parse import urlparse, parse_qs
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        if parsed.path == "/api/cloud_problems":
            try:
                data = json.loads(_proxy_get("/api/problems"))
                public = [p for p in data if p.get("isPublic", False)]
                self._send_json(200, public)
            except Exception as e:
                self._send_json(502, {"error": str(e)})
        elif parsed.path == "/api/cloud_problem":
            try:
                pid = qs.get("id", [""])[0]
                raw = _proxy_get(f"/api/problem?id={pid}")
                body = raw.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)
            except Exception as e:
                self._send_json(502, {"error": str(e)})
        elif parsed.path == "/api/cloud_whoami":
            try:
                token = self.headers.get("Authorization", "")
                if token.startswith("Bearer "):
                    token = token[7:]
                raw = _proxy_get(f"/api/whoami?token={token}")
                body = raw.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)
            except Exception as e:
                self._send_json(401, {"error": str(e)})
        elif parsed.path == "/api/cloud_judge_stream":
            task_id = qs.get("task_id", [""])[0]
            if not task_id:
                self._send_json(400, {"error": "task_id required"})
                return
            with _results_lock:
                cached = _results.pop(task_id, None)
            # Always respond with SSE format so EventSource works correctly
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            # Send any cached events first (result may arrive before SSE connects)
            final_sent = False
            if cached:
                payload = json.dumps(cached, ensure_ascii=False)
                self.wfile.write(("data: " + payload + "\n\n").encode("utf-8"))
                self.wfile.flush()
                if cached.get("status") not in ("judging", "pending"):
                    final_sent = True
            else:
                self.wfile.write(b"data: {\"status\":\"pending\",\"phase\":\"queued\"}\n\n")
                self.wfile.flush()
            # Continue listening for more events until final result
            deadline = time.time() + TIMEOUT_SEC + 30
            while time.time() < deadline and not final_sent:
                with _results_lock:
                    evt = _results.pop(task_id, None)
                if evt:
                    if evt.get("status") not in ("judging",):
                        final_sent = True
                        # Trigger AI encouragement/comfort message before sending final result
                        username = evt.get("username", "")
                        status = evt.get("status", "")
                        if username and status:
                            try:
                                ai_body = json.dumps({
                                    "username": username,
                                    "problem": evt.get("problem", "未知题目"),
                                    "status": status
                                }).encode()
                                ai_req = Request("http://127.0.0.1:18082/api/ai/judge_event",
                                    data=ai_body,
                                    headers={"Content-Type": "application/json"})
                                ai_resp = urlopen(ai_req, timeout=10)
                                ai_data = json.loads(ai_resp.read().decode())
                                if "reply" in ai_data:
                                    evt["ai_message"] = {"reply": ai_data["reply"], "status": status}
                            except Exception as e:
                                print(f"[api] AI trigger failed: {e}", flush=True)
                    payload = json.dumps(evt, ensure_ascii=False)
                    self.wfile.write(("data: " + payload + "\n\n").encode("utf-8"))
                    self.wfile.flush()
                else:
                    time.sleep(0.2)
            if not final_sent:
                tp = json.dumps({"task_id": task_id, "status": "SE", "error": "stream timeout"})
                self.wfile.write(("data: " + tp + "\n\n").encode("utf-8"))
                self.wfile.flush()

        elif parsed.path == "/api/cloud_judge_result":
            task_id = qs.get("task_id", [""])[0]
            if not task_id:
                self._send_json(400, {"error": "task_id required"})
                return
            now = time.time()
            with _results_lock:
                evt = _results.get(task_id)
                stale = [k for k, t in _results_ts.items() if now - t > 600]
                for k in stale:
                    _results.pop(k, None)
                    _results_ts.pop(k, None)
            if evt is None:
                self._send_json(200, {"task_id": task_id, "status": "pending", "phase": "queued"})
            else:
                result = dict(evt)
                if evt.get("status") not in ("judging", "pending"):
                    username = evt.get("username", "")
                    status = evt.get("status", "")
                    if username and status:
                        try:
                            ai_body = json.dumps({"username": username,"problem": evt.get("problem","未知题目"),"status": status}).encode()
                            ai_req = Request("http://127.0.0.1:18082/api/ai/judge_event",data=ai_body,headers={"Content-Type":"application/json"})
                            ai_resp = urlopen(ai_req,timeout=10)
                            ai_data = json.loads(ai_resp.read().decode())
                            if "reply" in ai_data:
                                result["ai_message"] = {"reply":ai_data["reply"],"status":status}
                        except Exception as e:
                            print(f'[api] AI trigger failed: {e}',flush=True)
                self._send_json(200, result)
        elif parsed.path == "/api/cloud_contests":
            try:
                self._send_json(200, [_contest_summary(c) for c in _fetch_contests()])
            except Exception as e:
                self._send_json(502, {"error": str(e)})
        elif parsed.path == "/api/cloud_contest":
            try:
                cid = qs.get("id", [""])[0]
                if not cid:
                    self._send_json(400, {"error": "id required"})
                    return
                target = None
                for c in _fetch_contests():
                    if str(c.get("id")) == str(cid):
                        target = c
                        break
                if target is None:
                    self._send_json(404, {"error": "contest not found"})
                    return
                summary = _contest_summary(target)
                plist = json.loads(_proxy_get("/api/problems"))
                pmap = {str(p.get("id")): p for p in plist}
                probs = []
                for pid in summary["problems"]:
                    p = pmap.get(str(pid))
                    if p:
                        probs.append({"id": p.get("id"), "title": p.get("title", ""),
                                      "timeMs": p.get("timeMs"), "memMb": p.get("memMb")})
                    else:
                        probs.append({"id": pid, "title": "\u9898\u76ee %s" % pid,
                                      "timeMs": None, "memMb": None})
                summary["problemList"] = probs
                summary["serverTs"] = time.time()
                self._send_json(200, summary)
            except Exception as e:
                self._send_json(502, {"error": str(e)})
        elif parsed.path == "/api/cloud_contest_pdf":
            try:
                cid = qs.get("id", [""])[0]
                if not cid:
                    self._send_json(400, {"error": "id required"})
                    return
                target = None
                for c in _fetch_contests():
                    if str(c.get("id")) == str(cid):
                        target = c
                        break
                if target is None:
                    self._send_json(404, {"error": "contest not found"})
                    return
                probs = []
                for pid in target.get("problems", []):
                    try:
                        probs.append(json.loads(_proxy_get(f"/api/problem?id={pid}")))
                    except Exception:
                        probs.append({"id": pid, "title": "\u9898\u76ee %s" % pid, "description": ""})
                pdf = _build_contest_pdf(target, probs)
                self.send_response(200)
                self.send_header("Content-Type", "application/pdf")
                self.send_header("Content-Disposition", "attachment; filename=contest_%s.pdf" % cid)
                self.send_header("Content-Length", str(len(pdf)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(pdf)
            except Exception as e:
                self._send_json(500, {"error": str(e)})
        elif parsed.path == "/api/anime/status":
            try:
                raw = _tunnel_get("/api/anime/status")
                body = raw.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)
            except Exception as e:
                self._send_json(502, {"error": str(e)})
        elif parsed.path == "/api/cloud_contest_board":
            try:
                cid = qs.get("cid", [""])[0]
                if not cid:
                    self._send_json(400, {"error": "cid required"})
                    return
                raw = _proxy_get(f"/api/board/realtime?cid={cid}")
                body = raw.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)
            except Exception as e:
                self._send_json(502, {"error": str(e)})

    def do_POST(self):
        if self.path == "/api/cloud_login":
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8")
                raw = _proxy_post("/api/login", body)
                resp_body = raw.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(resp_body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(resp_body)
                # Auto-create virtual human account on successful login
                try:
                    login_data = json.loads(raw)
                    if login_data.get("ok") or "token" in login_data:
                        req_body = json.loads(body)
                        username = req_body.get("username", "")
                        if username:
                            threading.Thread(target=_trigger_vh_init, args=(username,), daemon=True).start()
                except Exception:
                    pass
            except Exception as e:
                self._send_json(401, {"error": str(e)})
        elif self.path == "/api/cloud_register":
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8")
                raw = _proxy_post("/api/register", body)
                resp_body = raw.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(resp_body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(resp_body)
                # Auto-create virtual human account on successful register
                try:
                    reg_data = json.loads(raw)
                    if reg_data.get("ok") or "token" in reg_data:
                        req_body = json.loads(body)
                        username = req_body.get("username", "")
                        if username:
                            threading.Thread(target=_trigger_vh_init, args=(username,), daemon=True).start()
                except Exception:
                    pass
            except Exception as e:
                self._send_json(400, {"error": str(e)})
        elif self.path == "/api/cloud_judge":
            try:
                # Token validation
                token = self.headers.get("Authorization", "")
                if token.startswith("Bearer "):
                    token = token[7:]
                if not token:
                    self._send_json(401, {"error": "missing token, please login first"})
                    return
                # Verify token via middleware whoami
                try:
                    whoami_check = _proxy_get("/api/whoami", token=token)
                    whoami_check_data = json.loads(whoami_check)
                    if not whoami_check_data.get("ok", True):
                        self._send_json(401, {"error": "invalid or expired token"})
                        return
                except Exception:
                    self._send_json(401, {"error": "invalid or expired token"})
                    return
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8")
                req = json.loads(body)
                code = req.get("code", "")
                lang = req.get("language", "cpp").lower()
                problem_id = req.get("problemId", 0)
                contest_id = req.get("contestId", 0)
                if not code or not problem_id:
                    self._send_json(400, {"error": "code and problemId required"})
                    return
                try:
                    pdata = json.loads(_proxy_get(f"/api/problem?id={problem_id}"))
                    cpu_ms = pdata.get("timeMs", 1000)
                    mem_mb = pdata.get("memMb", 256)
                except Exception:
                    cpu_ms, mem_mb = 1000, 256
                task_id = f"web_{uuid.uuid4().hex[:12]}"
                task = {
                    "task_id": task_id,
                    "code": code,
                    "language": lang,
                    "stdin": "",
                    "expected_output": "",
                    "cpu_limit_ms": cpu_ms,
                    "mem_limit_bytes": mem_mb * 1024 * 1024,
                    "problem_id": problem_id,
                    "contest_id": contest_id,
                }
                # Pre-store username and problem info for AI encouragement
                try:
                    whoami_raw = _proxy_get("/api/whoami", token=token)
                    whoami = json.loads(whoami_raw)
                    username = whoami.get("username", "")
                    print(f"[api] DEBUG whoami username={username}", flush=True)
                except Exception:
                    username = ""
                try:
                    problem_name = pdata.get("title", f"题目{problem_id}")
                except Exception:
                    problem_name = f"题目{problem_id}"
                task["username"] = username
                with _results_lock:
                    _task_meta[task_id] = {"username": username, "problem": problem_name, "contest_id": contest_id, "token": token}
                    _results[task_id] = {"status": "pending", "username": username, "problem": problem_name}
                    _results_ts[task_id] = time.time()
                conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST, MQ_PORT))
                ch = conn.channel()
                ch.queue_declare(queue=TASK_QUEUE, durable=True)
                ch.basic_publish(exchange="", routing_key=TASK_QUEUE,
                                 body=json.dumps(task),
                                 properties=pika.BasicProperties(delivery_mode=2))
                conn.close()
                print(f"[api] published {task_id} problem={problem_id}", flush=True)
                # Return immediately; client uses SSE to receive result
                self._send_json(200, {"task_id": task_id, "status": "pending"})
            except Exception as e:
                self._send_json(500, {"error": str(e)})
        elif self.path == "/api/anime/toggle":
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8") if length else "{}"
                raw = _tunnel_post("/api/anime/toggle", body, timeout=15)
                out = raw.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(out)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(out)
            except Exception as e:
                self._send_json(502, {"error": str(e)})
        elif self.path == "/api/anime/generate":
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8") if length else "{}"
                try:
                    raw = _tunnel_post("/api/anime/generate", body, timeout=300)
                except HTTPError as he:
                    raw = he.read().decode("utf-8")
                out = raw.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(out)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(out)
            except Exception as e:
                self._send_json(502, {"error": str(e)})
        elif self.path == "/api/cloud_contest_register":
            try:
                token = self.headers.get("Authorization", "")
                if token.startswith("Bearer "):
                    token = token[7:]
                if not token:
                    self._send_json(401, {"error": "missing token"})
                    return
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8")
                req = json.loads(body)
                contest_id = req.get("contestId", 0)
                if not contest_id:
                    self._send_json(400, {"error": "contestId required"})
                    return
                sb = json.dumps({"contestId": contest_id, "virtual": False}).encode()
                sreq = Request(MIDDLEWARE_URL + "/api/contest/register", data=sb, method="POST",
                               headers={"Content-Type": "application/json"})
                sreq.add_header("Authorization", f"Bearer {token}")
                sresp = json.loads(urlopen(sreq, context=_ssl_ctx, timeout=10).read().decode())
                self._send_json(200, sresp)
            except Exception as e:
                self._send_json(500, {"error": str(e)})
        elif self.path == "/api/cloud_contest_data_prepare":
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8")
                req = json.loads(body)
                contest_id = req.get("contestId", 0)
                if not contest_id:
                    self._send_json(400, {"error": "contestId required"})
                    return
                conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST, MQ_PORT))
                ch = conn.channel()
                ch.queue_declare(queue=TASK_QUEUE, durable=True)
                ch.basic_publish(exchange="", routing_key=TASK_QUEUE,
                                 body=json.dumps({"type": "contest_data_prepare", "contest_id": str(contest_id)}),
                                 properties=pika.BasicProperties(delivery_mode=2))
                conn.close()
                print(f"[api] contest_data_prepare sent cid={contest_id}", flush=True)
                self._send_json(200, {"sent": True})
            except Exception as e:
                self._send_json(500, {"error": str(e)})
        elif self.path == "/api/cloud_contest_data_cleanup":
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8")
                req = json.loads(body)
                contest_id = req.get("contestId", 0)
                if not contest_id:
                    self._send_json(400, {"error": "contestId required"})
                    return
                conn = pika.BlockingConnection(pika.ConnectionParameters(MQ_HOST, MQ_PORT))
                ch = conn.channel()
                ch.queue_declare(queue=TASK_QUEUE, durable=True)
                ch.basic_publish(exchange="", routing_key=TASK_QUEUE,
                                 body=json.dumps({"type": "contest_data_cleanup", "contest_id": str(contest_id)}),
                                 properties=pika.BasicProperties(delivery_mode=2))
                conn.close()
                print(f"[api] contest_data_cleanup sent cid={contest_id}", flush=True)
                try:
                    _proxy_post("/api/board/persist", json.dumps({"contestId": contest_id}))
                    print(f"[api] contest board persisted cid={contest_id}", flush=True)
                except Exception as pe:
                    print(f"[api] board persist failed: {pe}", flush=True)
                self._send_json(200, {"sent": True})
            except Exception as e:
                self._send_json(500, {"error": str(e)})
        else:
            self._send_json(404, {"error": "not found"})

if __name__ == "__main__":
    t = threading.Thread(target=_mq_consume_results, daemon=True)
    t.start()
    class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
        daemon_threads = True
    server = ThreadingHTTPServer(("127.0.0.1", LISTEN_PORT), Handler)
    print(f"[api] listening on 127.0.0.1:{LISTEN_PORT}", flush=True)
    server.serve_forever()

