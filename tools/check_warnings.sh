#!/usr/bin/env bash
# 统计编译日志中的告警与错误数量
log=/tmp/minet_build.log
if [ ! -f "$log" ]; then
  echo NO_LOG
  exit 2
fi
echo "warnings=$(grep -c 'warning:' "$log")"
echo "errors=$(grep -c 'error:' "$log")"
echo "---- warnings (first 20) ----"
grep -n 'warning:' "$log" | head -20
