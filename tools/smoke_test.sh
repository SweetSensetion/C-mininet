#!/usr/bin/env bash
# 端到端冒烟测试：启动 echo_server，跑压测客户端，然后关掉服务器
cd /mnt/c/Code/Deepseek/C++/build || exit 2

PORT=19000
BODY=64
SECONDS_TO_RUN=5

ulimit -n 65535 2>/dev/null

./echo_server "$PORT" 4 > /tmp/minet_server.log 2>&1 &
SRV=$!
sleep 1

if ! kill -0 "$SRV" 2>/dev/null; then
  echo "!! server failed to start"
  cat /tmp/minet_server.log
  exit 1
fi

echo "server pid=$SRV port=$PORT"
echo "---- bench ----"
./bench_client 127.0.0.1 "$PORT" 4 8 "$SECONDS_TO_RUN" "$BODY"
RC=$?
echo "client exit=$RC"

kill "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null

echo "---- server log (first 6 lines) ----"
head -n 6 /tmp/minet_server.log
exit "$RC"
