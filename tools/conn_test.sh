#!/usr/bin/env bash
# 万级连接测试：同时建立 10000 条空闲连接，检查服务端 fd 数与 RSS
cd /mnt/c/Code/Deepseek/C++/build || exit 2

PORT=19001
CONNS=10000
ulimit -n 65535 2>/dev/null

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 不可用，跳过连接数测试"
  exit 3
fi

./echo_server "$PORT" 4 > /tmp/minet_conn_server.log 2>&1 &
SRV=$!
sleep 1
if ! kill -0 "$SRV" 2>/dev/null; then
  echo "!! server failed to start"
  cat /tmp/minet_conn_server.log
  exit 1
fi
echo "server pid=$SRV, soft fd limit=$(ulimit -n)"

python3 - "$PORT" "$CONNS" <<'PY' &
import socket, sys, time
port, n = int(sys.argv[1]), int(sys.argv[2])
socks = []
for _ in range(n):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setblocking(False)
    try:
        s.connect(("127.0.0.1", port))
    except BlockingIOError:
        pass
    except OSError:
        break
    socks.append(s)
print("client: initiated", len(socks), "connections", flush=True)
time.sleep(6)
print("client: holding", len(socks), "connections open", flush=True)
time.sleep(2)
PY
PY_PID=$!

sleep 4
echo "---- server state while holding ----"
echo "fds=$(ls /proc/$SRV/fd 2>/dev/null | wc -l)"
grep -E 'VmRSS|Threads' /proc/$SRV/status
if command -v ss >/dev/null 2>&1; then
  echo "established=$(ss -tan state established "( sport = :$PORT )" 2>/dev/null | tail -n +2 | wc -l)"
fi

wait $PY_PID
sleep 1
if kill -0 "$SRV" 2>/dev/null; then echo "server still alive after clients closed: yes"; else echo "server DIED"; fi
kill "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
echo "---- server log errors (if any) ----"
grep -iE 'error|fatal' /tmp/minet_conn_server.log | head -n 5 || true
