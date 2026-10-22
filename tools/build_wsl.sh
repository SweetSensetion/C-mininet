#!/usr/bin/env bash
# 在 WSL 中配置、编译并运行单元测试；完整日志留在 /tmp，终端只输出关键片段
cd /mnt/c/Code/Deepseek/C++ || exit 2

if [ "${1:-}" = "--clean" ]; then
  echo "removing $(pwd)/build"
  rm -rf build
fi

echo "== toolchain =="
g++ --version | head -1
cmake --version | head -1
echo "cpus=$(nproc)"

echo "== configure =="
cmake -B build -DCMAKE_BUILD_TYPE=Release > /tmp/minet_cfg.log 2>&1
CFG=$?
echo "configure exit=$CFG"
tail -n 25 /tmp/minet_cfg.log
if [ "$CFG" -ne 0 ]; then
  exit "$CFG"
fi

echo "== build =="
cmake --build build -j"$(nproc)" > /tmp/minet_build.log 2>&1
BLD=$?
echo "build exit=$BLD"
if [ "$BLD" -ne 0 ]; then
  echo "---- first errors ----"
  grep -n -E "error:|错误" /tmp/minet_build.log | head -n 40
  echo "---- tail ----"
  tail -n 30 /tmp/minet_build.log
  exit "$BLD"
fi
tail -n 3 /tmp/minet_build.log

echo "== ctest =="
ctest --test-dir build --output-on-failure > /tmp/minet_test.log 2>&1
TST=$?
echo "ctest exit=$TST"
cat /tmp/minet_test.log
exit "$TST"
