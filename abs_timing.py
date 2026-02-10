#!/usr/bin/env python3
from __future__ import annotations

import csv
import sys
from pathlib import Path
from typing import Dict, List, Tuple


REQUIRED = {"speaker", "start", "end", "filename"}


def read_rows(path: Path) -> Tuple[List[str], List[dict], int]:
    """
    Returns (fieldnames, rows, extra_cells_count)
    extra_cells_count counts how many rows had spillover columns (None key).
    """
    with path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(
            f,
            skipinitialspace=False,
        )
        fieldnames = reader.fieldnames
        if not fieldnames:
            raise ValueError("CSV has no header row.")

        rows: List[dict] = []
        extra_rows = 0

        for r in reader:
            # If a row has more columns than headers, DictReader stores extras under key None.
            if None in r:
                extra_rows += 1
                # Drop the spillover so DictWriter won't choke.
                r.pop(None, None)

            rows.append(r)

    missing = REQUIRED - set(fieldnames)
    if missing:
        raise ValueError(f"Missing required columns: {sorted(missing)}")

    return fieldnames, rows, extra_rows


def validate_times(rows: List[dict]) -> None:
    for r in rows:
        fn = r.get("filename", "")
        try:
            s = float(r["start"])
            e = float(r["end"])
        except Exception as ex:
            raise ValueError(f"Bad start/end in file={fn!r}: start={r.get('start')!r}, end={r.get('end')!r}") from ex
        if e < s:
            raise ValueError(f"End < start in file={fn!r}: start={s}, end={e}")


def make_gapless(rows: List[dict]) -> List[dict]:
    # Group rows by filename
    by_file: Dict[str, List[dict]] = {}
    for r in rows:
        by_file.setdefault(r["filename"], []).append(r)

    out: List[dict] = []

    for fn, items in by_file.items():
        # Sort by original start time (stable)
        items_sorted = sorted(items, key=lambda r: float(r["start"]))

        cursor = 0.0
        for r in items_sorted:
            s = float(r["start"])
            e = float(r["end"])
            dur = e - s

            r2 = dict(r)
            r2["start"] = f"{cursor:.3f}"
            r2["end"] = f"{(cursor + dur):.3f}"
            cursor += dur

            # Safety: ensure no spillover key survives
            r2.pop(None, None)

            out.append(r2)

    return out


def write_csv(out_path: Path, fieldnames: List[str], rows: List[dict]) -> None:
    with out_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=fieldnames,
            quoting=csv.QUOTE_MINIMAL,
            extrasaction="ignore",  # ignore any unexpected keys just in case
        )
        writer.writeheader()
        for r in rows:
            writer.writerow(r)


def main(argv: List[str]) -> int:
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        print(f"Usage: {Path(argv[0]).name} <input.csv>", file=sys.stderr)
        return 2

    in_path = Path(argv[1]).expanduser().resolve()
    if not in_path.exists():
        print(f"Input not found: {in_path}", file=sys.stderr)
        return 2

    fieldnames, rows, extra_rows = read_rows(in_path)
    if not rows:
        print("Input CSV has no data rows.", file=sys.stderr)
        return 2

    validate_times(rows)
    gapless_rows = make_gapless(rows)

    out_path = in_path.with_name(f"{in_path.stem}_abs{in_path.suffix}")
    write_csv(out_path, fieldnames, gapless_rows)

    if extra_rows:
        print(f"Wrote: {out_path}  (note: {extra_rows} row(s) had extra columns; they were ignored)")
    else:
        print(f"Wrote: {out_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
