#!/usr/bin/env python3
"""Verify that every relative Markdown link under Docs/ resolves to a real file.

Fenced code blocks are stripped before matching, because C++ such as
`operator[](usize index)` otherwise parses as a Markdown link and produces
false positives.

Usage:  check_doc_links.py [path ...]      (default: Docs)

Each path may be a directory, which is searched recursively for *.md, or a single
Markdown file. Relative links are resolved against the file that contains them.
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


def collect(paths: list[str]) -> list[pathlib.Path]:
    """Expand each argument into Markdown files, preserving order and de-duplicating."""
    found: list[pathlib.Path] = []
    for raw in paths:
        p = pathlib.Path(raw)
        if p.is_dir():
            found.extend(sorted(p.rglob("*.md")))
        elif p.is_file():
            found.append(p)
        else:
            print(f"no such file or directory: {p}")
            return []
    seen, unique = set(), []
    for f in found:
        key = f.resolve()
        if key not in seen:
            seen.add(key)
            unique.append(f)
    return unique


def main() -> int:
    targets = sys.argv[1:] or ["Docs"]
    documents = collect(targets)
    if not documents:
        return 2

    broken, total, files = [], 0, 0
    for md in documents:
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
