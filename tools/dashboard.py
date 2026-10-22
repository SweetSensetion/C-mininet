#!/usr/bin/env python3
"""MiniNet 实时监控面板。

结构：
    echo_server (C++)  <-- 压测客户端 bench_client 循环施压
          ^
          | 读取 /proc/<pid>/status、/proc/<pid>/fd、/proc/net/tcp
    dashboard.py (Python 标准库)  --->  http://localhost:8080

为什么用旁路监控而不是让 C++ 服务端自己吐 HTTP：
    这个库刻意不做 HTTP（见设计文档 8.7），为了一个 demo 往里塞 HTTP 解析
    会污染核心代码。进程指标从 /proc 读，压测指标从客户端输出读，一样真实。
"""
import atexit
import json
import os
import re
import signal
import socket
import subprocess
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build")
SERVER_PORT = 19080
DASH_PORT = 8080
BENCH_LOG = "/tmp/minet_bench.log"

STATE = {
    "status": "starting",
    "uptime": 0,
    "conns": 0,
    "qps": 0,
    "total": 0,
    "rss_mb": 0.0,
    "threads": 0,
    "fds": 0,
    "bench_runs": 0,
}
HIST = deque(maxlen=60)
LOCK = threading.Lock()
START_TS = time.time()
STOP = threading.Event()

server_proc = None
bench_proc = None
bench_tail = 0


def read_proc(pid):
    rss = threads = 0
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1]) // 1024
                elif line.startswith("Threads:"):
                    threads = int(line.split()[1])
    except OSError:
        pass
    try:
        fds = len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        fds = 0
    return rss, threads, fds


def count_established(port):
    """/proc/net/tcp 里 st=01 且本地端口匹配的连接数，比跑 ss 便宜得多。"""
    hexport = f"{port:04X}"
    total = 0
    for path in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(path, encoding="utf-8") as f:
                next(f, None)
                for line in f:
                    parts = line.split()
                    if len(parts) < 4 or parts[3] != "01":
                        continue
                    local = parts[1]
                    if local.rsplit(":", 1)[-1].upper() == hexport:
                        total += 1
        except OSError:
            continue
    return total


def parse_bench():
    """从压测日志里取最近一次完整运行的结果。"""
    global bench_tail
    try:
        size = os.path.getsize(BENCH_LOG)
        if size < bench_tail:
            bench_tail = 0
        with open(BENCH_LOG, encoding="utf-8", errors="ignore") as f:
            f.seek(bench_tail)
            chunk = f.read()
            bench_tail = f.tell()
    except OSError:
        return None

    hits = re.findall(r"responses=(\d+) elapsed=([\d.]+)s qps=(\d+) failed_conns=(\d+)", chunk)
    if not hits:
        return None
    responses, elapsed, qps, failed = hits[-1]
    return int(responses), float(elapsed), int(qps), int(failed)


def launch_server():
    global server_proc
    log = open("/tmp/minet_server_dash.log", "wb")
    server_proc = subprocess.Popen(
        [os.path.join(BIN, "echo_server"), str(SERVER_PORT), "4"],
        stdout=log, stderr=subprocess.STDOUT,
    )
    return server_proc


def launch_bench():
    global bench_proc
    cmd = (
        f"while true; do "
        f"{os.path.join(BIN, 'bench_client')} 127.0.0.1 {SERVER_PORT} 4 8 3 64 >> {BENCH_LOG}; "
        f"sleep 0.2; done"
    )
    bench_proc = subprocess.Popen(["bash", "-c", cmd])


def collect():
    prev_total = 0
    while not STOP.is_set():
        try:
            if server_proc is not None and server_proc.poll() is not None:
                with LOCK:
                    STATE["status"] = "服务端已退出，正在重启"
                launch_server()

            pid = server_proc.pid if server_proc else 0
            rss, threads, fds = read_proc(pid) if pid else (0, 0, 0)
            conns = count_established(SERVER_PORT)
            parsed = parse_bench()

            with LOCK:
                STATE["uptime"] = int(time.time() - START_TS)
                STATE["rss_mb"] = round(rss, 1)
                STATE["threads"] = threads
                STATE["fds"] = fds
                STATE["conns"] = conns
                STATE["status"] = "running" if (server_proc and server_proc.poll() is None) else "degraded"
                if parsed:
                    responses, _elapsed, qps, failed = parsed
                    STATE["qps"] = qps
                    STATE["total"] += responses - prev_total if responses >= prev_total else responses
                    prev_total = responses
                    STATE["bench_runs"] += 1
                    if failed:
                        STATE["status"] = "压测有连接失败"
                HIST.append({"t": STATE["uptime"], "qps": STATE["qps"], "conns": conns})
        except Exception as exc:  # 监控本身不能把面板搞挂
            with LOCK:
                STATE["status"] = f"collect error: {exc}"
        STOP.wait(1.0)


PAGE = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>MiniNet 实时监控</title>
<style>
  :root{--bg:#0f1115;--card:#171a21;--line:#262b36;--txt:#e6e9ef;--dim:#8b93a7;--acc:#4ea1ff;--ok:#3ddc97}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--txt);font:14px/1.5 system-ui,-apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
  header{padding:22px 28px 8px;display:flex;align-items:baseline;gap:14px;flex-wrap:wrap}
  h1{font-size:20px;margin:0;font-weight:600}
  .dim{color:var(--dim);font-size:12px}
  .dot{width:8px;height:8px;border-radius:50%;background:var(--ok);display:inline-block;margin-right:6px}
  main{padding:12px 28px 28px;max-width:1180px;margin:0 auto}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(165px,1fr));gap:12px;margin:10px 0 18px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px}
  .k{color:var(--dim);font-size:12px;letter-spacing:.04em}
  .v{font-size:26px;font-weight:600;margin-top:6px;font-variant-numeric:tabular-nums}
  .unit{font-size:12px;color:var(--dim);margin-left:4px;font-weight:400}
  canvas{width:100%;height:280px;background:var(--card);border:1px solid var(--line);border-radius:12px;display:block}
  .row{display:flex;gap:12px;flex-wrap:wrap}
  footer{padding:0 28px 24px;color:var(--dim);font-size:12px;max-width:1180px;margin:0 auto}
  code{background:#1e222b;padding:2px 6px;border-radius:5px;color:#c8d2e6}
</style></head>
<body>
<header>
  <h1>MiniNet 实时监控</h1>
  <span class="dim"><span class="dot" id="dot"></span><span id="status">连接中…</span></span>
  <span class="dim">epoll + 主从 Reactor · C++11</span>
</header>
<main>
  <div class="grid">
    <div class="card"><div class="k">当前连接</div><div class="v" id="conns">–</div></div>
    <div class="card"><div class="k">QPS（近 3 秒均值）</div><div class="v" id="qps">–</div></div>
    <div class="card"><div class="k">累计响应</div><div class="v" id="total">–</div></div>
    <div class="card"><div class="k">服务端内存</div><div class="v"><span id="rss">–</span><span class="unit">MB</span></div></div>
    <div class="card"><div class="k">线程数</div><div class="v" id="threads">–</div></div>
    <div class="card"><div class="k">打开 fd</div><div class="v" id="fds">–</div></div>
  </div>
  <canvas id="chart" width="1120" height="280"></canvas>
  <p class="dim">压测：<code>bench_client 127.0.0.1 19080 4 8 3 64</code> 循环执行；服务端 <code>echo_server 19080 4</code>；数据每秒采样。</p>
</main>
<footer>页面由 <code>tools/dashboard.py</code> 提供 · 指标来自 /proc 与压测客户端输出 · 刷新即可重连</footer>
<script>
const $ = id => document.getElementById(id);
const fmt = n => (n||0).toLocaleString('en-US');
let hist = [];

function draw() {
  const c = $('chart'), ctx = c.getContext('2d');
  const W = c.width, H = c.height, pad = 36;
  ctx.clearRect(0, 0, W, H);
  const max = Math.max(1, ...hist.map(p => p.qps));
  ctx.strokeStyle = '#262b36'; ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = pad + (H - 2*pad) * i / 4;
    ctx.beginPath(); ctx.moveTo(pad, y); ctx.lineTo(W - pad, y); ctx.stroke();
    ctx.fillStyle = '#6d768a'; ctx.font = '11px system-ui';
    ctx.fillText(fmt(Math.round(max * (4 - i) / 4)), 4, y + 4);
  }
  if (!hist.length) return;
  const stepX = (W - 2*pad) / Math.max(1, hist.length - 1);
  const yOf = v => pad + (H - 2*pad) * (1 - v / max);
  ctx.beginPath();
  hist.forEach((p, i) => {
    const x = pad + i * stepX, y = yOf(p.qps);
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.strokeStyle = '#4ea1ff'; ctx.lineWidth = 2; ctx.stroke();
  ctx.lineTo(pad + (hist.length - 1) * stepX, H - pad);
  ctx.lineTo(pad, H - pad); ctx.closePath();
  ctx.fillStyle = 'rgba(78,161,255,.12)'; ctx.fill();
}

async function tick() {
  try {
    const r = await fetch('/stats', {cache: 'no-store'});
    const s = await r.json();
    $('conns').textContent = fmt(s.conns);
    $('qps').textContent = fmt(s.qps);
    $('total').textContent = fmt(s.total);
    $('rss').textContent = s.rss_mb.toFixed(1);
    $('threads').textContent = s.threads;
    $('fds').textContent = fmt(s.fds);
    $('status').textContent = s.status + ' · 已运行 ' + s.uptime + 's';
    $('dot').style.background = s.status === 'running' ? '#3ddc97' : '#f0a94c';
    hist = s.hist || [];
    draw();
  } catch (e) {
    $('status').textContent = '面板与服务端失联';
    $('dot').style.background = '#e05b5b';
  }
}
tick(); setInterval(tick, 1000);
</script>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.startswith("/stats"):
            with LOCK:
                payload = dict(STATE)
                payload["hist"] = list(HIST)
            body = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        body = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def cleanup(*_):
    STOP.set()
    for p in (bench_proc, server_proc):
        if p and p.poll() is None:
            try:
                p.terminate()
            except OSError:
                pass


def main():
    os.makedirs(BIN, exist_ok=True)
    for name in ("echo_server", "bench_client"):
        if not os.path.exists(os.path.join(BIN, name)):
            raise SystemExit(f"缺少 {BIN}/{name}，请先 cmake --build build")

    open(BENCH_LOG, "w").close()
    launch_server()
    time.sleep(0.8)
    launch_bench()
    atexit.register(cleanup)
    signal.signal(signal.SIGTERM, lambda *a: (cleanup(), os._exit(0)))

    threading.Thread(target=collect, daemon=True).start()
    httpd = ThreadingHTTPServer(("0.0.0.0", DASH_PORT), Handler)
    print(f"dashboard on http://0.0.0.0:{DASH_PORT}  (server pid={server_proc.pid})", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
