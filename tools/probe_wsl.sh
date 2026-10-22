#!/usr/bin/env bash
# 诊断 WSL 内的编译环境（只读，不安装任何东西）
echo "== user =="
id -un
echo "== distro =="
. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}"
echo "== kernel =="
uname -r
echo "== tools =="
for t in g++ gcc clang++ cmake make ninja; do
  if command -v "$t" >/dev/null 2>&1; then
    printf '%-10s %s\n' "$t" "$(command -v "$t")"
  else
    printf '%-10s MISSING\n' "$t"
  fi
done
echo "== mount =="
if ls -d "/mnt/c/Code/Deepseek/C++" >/dev/null 2>&1; then echo MOUNT_OK; else echo NO_MOUNT; fi
echo "== network =="
if getent hosts archive.ubuntu.com >/dev/null 2>&1; then echo DNS_OK; else echo DNS_FAIL; fi
