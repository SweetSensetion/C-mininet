#!/usr/bin/env bash
# 启动监控面板（后台常驻）。面板自己会拉起 echo_server 与压测循环。
cd /mnt/c/Code/Deepseek/C++ || exit 2

# 清掉上一次的实例，避免端口/进程堆积
pkill -f "tools/dashboard.py" 2>/dev/null
pkill -f "echo_server 19080" 2>/dev/null
pkill -f "bench_client 127.0.0.1 19080" 2>/dev/null
sleep 0.5

nohup python3 tools/dashboard.py > /tmp/minet_dashboard.log 2>&1 &
echo $! > /tmp/minet_dash.pid
sleep 2

echo "dashboard_pid=$(cat /tmp/minet_dash.pid)"
echo "---- dashboard log ----"
cat /tmp/minet_dashboard.log 2>/dev/null

python3 - <<'PY'
import json, urllib.request
try:
    with urllib.request.urlopen('http://127.0.0.1:8080/stats', timeout=3) as r:
        d = json.load(r)
    keep = {k: d.get(k) for k in ('status', 'conns', 'qps', 'total', 'rss_mb', 'threads', 'fds')}
    print('stats:', json.dumps(keep, ensure_ascii=False))
except Exception as e:
    print('stats failed:', e)
PY
