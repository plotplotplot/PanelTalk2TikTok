#!/usr/bin/env python3
"""
Count total lines across all .cpp, .h, and .py files in the current directory (top-level only).
- Does NOT recurse.
- Skips directories (only counts regular files).
- Counts physical lines (including blanks/comments).
"""

from __future__ import annotations

from pathlib import Path

EXTS = {".cpp", ".h", ".hpp", ".py"}

def count_lines(p: Path) -> int:
    # Read as bytes and count b'\n' for speed/robustness; add 1 if file doesn't end with newline.
    data = p.read_bytes()
    if not data:
        return 0
    n = data.count(b"\n")
    return n if data.endswith(b"\n") else n + 1

def main() -> int:
    root = Path(".")
    per_ext = {ext: 0 for ext in sorted(EXTS)}
    per_file: list[tuple[str, int]] = []

    total = 0
    for p in root.iterdir():
        if not p.is_file():
            continue
        if p.suffix not in EXTS:
            continue

        lines = count_lines(p)
        total += lines
        per_ext[p.suffix] += lines
        per_file.append((p.name, lines))

    per_file.sort(key=lambda x: (-x[1], x[0]))

    for name, lines in per_file:
        print(f"{lines:8d}  {name}")

    if per_file:
        print("-" * 40)
        for ext in sorted(EXTS):
            print(f"{per_ext[ext]:8d}  *{ext}")
        print("=" * 40)

    print(f"{total:8d}  TOTAL")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
