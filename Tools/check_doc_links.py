#!/usr/bin/env python3
"""Verify that every relative Markdown link under Docs/ resolves to a real file.

Fenced code blocks are stripped before matching, because C++ such as
`operator[](usize index)` otherwise parses as a Markdown link and produces
false positives.

Usage:  check_doc_links.py [docs-root]     (default: Docs)
"""
from __future__ import annotations

import pathlib
import re
import sys

LINK = re.compile(r"\[([^\]]*)\]\(([^)]+)\)")
FENCE = re.compile(r"^\s*(```|~~~)")
EXTERNAL_PREFIXES = ("http://", "https://", "#", "mailto:")


def strip_code_blocks(text: str) -> str:
    """Replace fenced code block bodies with blank lines, preserving line numbers."""
    out, in_fence = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
            out.append("")
            continue
        out.append("" if in_fence else line)
    return "\n".join(out)


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "Docs")
    if not root.is_dir():
        print(f"not a directory: {root}")
        return 2

    broken, total, files = [], 0, 0
    for md in sorted(root.rglob("*.md")):
        files += 1
        text = strip_code_blocks(md.read_text(encoding="utf-8", errors="replace"))
        for lineno, line in enumerate(text.splitlines(), 1):
            for match in LINK.finditer(line):
                target = match.group(2).strip()
                if target.startswith(EXTERNAL_PREFIXES):
                    continue
                total += 1
                path_part = target.split("#")[0]
                if not path_part:
                    continue
                if not (md.parent / path_part).resolve().exists():
                    broken.append(f"{md.as_posix()}:{lineno} -> {target}")

    print(f"{total} relative links across {files} files")
    for entry in broken:
        print(f"  BROKEN {entry}")
    print("all links resolve" if not broken else f"{len(broken)} BROKEN LINK(S)")
    return 0 if not broken else 1


if __name__ == "__main__":
    sys.exit(main())
