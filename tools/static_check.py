#!/usr/bin/env python3
"""静态自检：在无法编译的环境下，尽可能早地发现低级错误。

检查项：
  1. #include "net/X.h" 是否能解析到真实文件；
  2. 头文件是否有 include guard 且配对；
  3. 去掉注释与字符串后，{} 与 () 是否配平；
  4. CMakeLists 里列出的源文件是否存在，src 下的 .cc 是否都被收录；
  5. 是否存在 TODO/FIXME 或明显的占位符。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDE_DIR = os.path.join(ROOT, "include")
SRC_DIR = os.path.join(ROOT, "src")

problems = []


def strip_code(text):
    """移除注释与字符串/字符字面量，保留换行以便定位。"""
    out = []
    i, n = 0, len(text)
    state = None  # None, 'line', 'block', 'str', 'chr'
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "line"
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                i += 2
                continue
            if c == '"':
                state = "str"
                i += 1
                out.append('"')
                continue
            if c == "'":
                state = "chr"
                i += 1
                out.append("'")
                continue
            out.append(c)
            i += 1
        elif state == "line":
            if c == "\n":
                state = None
                out.append(c)
            i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                state = None
                i += 2
                continue
            if c == "\n":
                out.append(c)
            i += 1
        else:  # str / chr
            if c == "\\":
                i += 2
                continue
            if (state == "str" and c == '"') or (state == "chr" and c == "'"):
                state = None
                out.append(c)
                i += 1
                continue
            if c == "\n":
                out.append(c)
            i += 1
    return "".join(out)


def check_includes():
    pattern = re.compile(r'#include\s+"(net/[^"]+)"')
    for dirpath, _, files in os.walk(ROOT):
        if "docs" in dirpath or "build" in dirpath:
            continue
        for name in files:
            if not name.endswith((".h", ".cc")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as f:
                for lineno, line in enumerate(f, 1):
                    m = pattern.search(line)
                    if not m:
                        continue
                    target = os.path.join(INCLUDE_DIR, *m.group(1).split("/"))
                    if not os.path.isfile(target):
                        problems.append(f"{path}:{lineno}: 找不到头文件 {m.group(1)}")


def check_guards():
    for dirpath, _, files in os.walk(INCLUDE_DIR):
        for name in files:
            if not name.endswith(".h"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            ifndef = re.search(r"#ifndef\s+(\w+)", text)
            define = re.search(r"#define\s+(\w+)", text)
            endif_count = len(re.findall(r"^#endif", text, re.M))
            if not ifndef or not define:
                problems.append(f"{path}: 缺少 include guard")
            elif ifndef.group(1) != define.group(1):
                problems.append(f"{path}: include guard 名不一致")
            elif endif_count != 1:
                problems.append(f"{path}: #endif 数量为 {endif_count}")


def check_balance():
    for dirpath, _, files in os.walk(ROOT):
        if any(part in dirpath for part in ("docs", "build", ".git")):
            continue
        for name in files:
            if not name.endswith((".h", ".cc")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as f:
                raw = f.read()
            code = strip_code(raw)
            for open_ch, close_ch in (("{", "}"), ("(", ")")):
                depth = 0
                line = 1
                bad = None
                for ch in code:
                    if ch == "\n":
                        line += 1
                    elif ch == open_ch:
                        depth += 1
                    elif ch == close_ch:
                        depth -= 1
                        if depth < 0 and bad is None:
                            bad = line
                if bad is not None:
                    problems.append(f"{path}:{bad}: 多余的 {close_ch}")
                elif depth != 0:
                    problems.append(f"{path}: {open_ch}{close_ch} 不配平，差值 {depth}")


def check_cmake():
    cmake_path = os.path.join(ROOT, "CMakeLists.txt")
    with open(cmake_path, encoding="utf-8") as f:
        cmake = f.read()
    listed = set(re.findall(r"(src/[\w.]+\.cc)", cmake))
    actual = {f"src/{n}" for n in os.listdir(SRC_DIR) if n.endswith(".cc")}
    for missing in sorted(listed - actual):
        problems.append(f"CMakeLists.txt: 列出的 {missing} 不存在")
    for unlisted in sorted(actual - listed):
        problems.append(f"CMakeLists.txt: 漏掉源文件 {unlisted}")


def check_placeholders():
    for dirpath, _, files in os.walk(ROOT):
        if any(part in dirpath for part in ("docs", "build", ".git")):
            continue
        for name in files:
            if not name.endswith((".h", ".cc", ".txt", ".md")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as f:
                for lineno, line in enumerate(f, 1):
                    if re.search(r"\b(TODO|FIXME|XXX)\b", line):
                        problems.append(f"{path}:{lineno}: 存在未完成标记 -> {line.strip()[:80]}")


def main():
    check_includes()
    check_guards()
    check_balance()
    check_cmake()
    check_placeholders()

    if problems:
        print(f"发现 {len(problems)} 个问题：")
        for p in problems:
            print("  -", p)
        return 1
    print("静态自检通过：include 可解析、guard 配对、括号配平、CMake 源文件完整、无未完成标记")
    return 0


if __name__ == "__main__":
    sys.exit(main())
