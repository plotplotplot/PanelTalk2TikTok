#!/usr/bin/env python3
import argparse
import csv
import os
import subprocess
from typing import List


def _find_time_fields(fieldnames: List[str]):
    start_keys = []
    end_keys = []
    for name in fieldnames:
        lname = name.lower()
        if lname in ("start", "start_time", "starttime", "begin"):
            start_keys.append(name)
        if lname in ("end", "end_time", "endtime", "stop"):
            end_keys.append(name)
    # Prefer exact "start"/"end"
    def _prefer(keys, target):
        for k in keys:
            if k.lower() == target:
                return k
        return keys[0] if keys else None

    return _prefer(start_keys, "start"), _prefer(end_keys, "end")


def retime_csv(csv_path: str, start: float, end: float, out_csv: str):
    with open(csv_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames or []
        start_key, end_key = _find_time_fields(fieldnames)
        if not start_key or not end_key:
            raise ValueError("CSV missing start/end columns")
        rows = []
        for row in reader:
            try:
                s = float(row.get(start_key, 0.0))
            except Exception:
                s = 0.0
            try:
                e = float(row.get(end_key, s))
            except Exception:
                e = s
            if e < start or s > end:
                continue
            # clamp to clip window and retime
            s_clamped = max(start, min(end, s)) - start
            e_clamped = max(start, min(end, e)) - start
            row[start_key] = f"{s_clamped:.3f}"
            row[end_key] = f"{e_clamped:.3f}"
            rows.append(row)

    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def main():
    ap = argparse.ArgumentParser(description="Extract a clip and retime CSV if present.")
    ap.add_argument("video", help="Input video (e.g. BuiltWithCharm/section2.mp4)")
    ap.add_argument("--start", type=float, required=True, help="Start time in seconds")
    ap.add_argument("--end", type=float, required=True, help="End time in seconds")
    ap.add_argument("--out", type=str, required=True, help="Output basename (no extension)")
    args = ap.parse_args()

    if args.end <= args.start:
        raise SystemExit("--end must be greater than --start")

    out_dir = os.path.dirname(args.video) or "."
    out_mp4 = os.path.join(out_dir, args.out + ".mp4")
    out_csv = os.path.join(out_dir, args.out + ".csv")

    cmd = [
        "ffmpeg",
        "-y",
        "-ss",
        f"{args.start:.3f}",
        "-to",
        f"{args.end:.3f}",
        "-i",
        args.video,
        "-c",
        "copy",
        out_mp4,
    ]
    subprocess.run(cmd, check=False)
    print(f"Wrote clip: {out_mp4}")

    csv_path = os.path.splitext(args.video)[0] + ".json.csv"
    if os.path.exists(csv_path):
        try:
            retime_csv(csv_path, args.start, args.end, out_csv)
            print(f"Wrote retimed CSV: {out_csv}")
        except Exception as e:
            print(f"Warning: failed to retime CSV: {e}")
    else:
        print(f"No CSV found: {csv_path}")


if __name__ == "__main__":
    main()
