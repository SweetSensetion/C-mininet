#!/usr/bin/env python3
"""把 docs/src/part*.md 合并成完整文档，并渲染 DOCX。

用法：python tools/build_doc.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import md_to_docx  # noqa: E402


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src_dir = os.path.join(root, "docs", "src")
    parts = ["part1.md", "part2.md", "part3.md"]

    chunks = []
    for name in parts:
        path = os.path.join(src_dir, name)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        chunks.append(text.rstrip("\n"))

    merged = "\n\n".join(chunks) + "\n"
    md_path = os.path.join(root, "docs", "技术设计说明.md")
    with open(md_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(merged)

    docx_path = os.path.join(root, "docs", "高性能Reactor网络库技术设计说明.docx")
    doc = md_to_docx.convert(md_path, docx_path)
    print(f"markdown: {md_path} ({len(merged)} chars)")
    print(f"docx:     {docx_path}")
    print(f"paragraphs={len(doc.paragraphs)} tables={len(doc.tables)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
