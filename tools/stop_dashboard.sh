#!/usr/bin/env bash
# 停止监控面板及其拉起的服务端/压测进程
pkill -f "tools/dashboard.py" 2>/dev/null
sleep 0.3
pkill -f "bench_client 127.0.0.1 19080" 2>/dev/null
pkill -f "echo_server 19080" 2>/dev/null
rm -f /tmp/minet_dash.pid
echo "stopped"

# 确认没有残留进程
sleep 0.3
for pat in "tools/dashboard.py" "echo_server 19080" "bench_client"; do
  if pgrep -f "$pat" >/dev/null 2>&1; then
    echo "WARN: 仍有进程匹配 [$pat]"
  else
    echo "clean: [$pat]"
  fi
done
