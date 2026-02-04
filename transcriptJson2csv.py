#!/usr/bin/env python3
import json
import csv
import sys
import argparse

import segment_utils

def export_words_to_csv(json_file, fill_gaps=False, min_gap=0.0, source_video=None):
    csv_file = json_file + ".csv"
    # Load Whisper JSON
    with open(json_file, "r", encoding="utf-8") as f:
        data = json.load(f)

    # Collect words in chronological order
    words = segment_utils.load_word_segments_from_json(json_file)

    # Ensure chronological order by start time
    words.sort(key=lambda x: x["start"])

    # Open CSV for writing
    with open(csv_file, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["speaker", "start", "end", "filename", "center_x", "center_y", "zoom", "word"])  # header row

        export_words = words
        if fill_gaps:
            export_words = segment_utils.fill_gaps(words, min_gap=min_gap)
        for w in export_words:
            writer.writerow(
                [
                    w.get("speaker", ""),
                    "{:.3f}".format(w.get("start", 0.0)),
                    "{:.3f}".format(w.get("end", 0.0)),
                    source_video or "",
                    "0.5",
                    "0.5",
                    "1",
                    w.get("word", ""),
                ]
            )

    print(f"Exported word timings to {csv_file}")
    return csv_file

def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Export word-level timings from Whisper JSON to CSV."
    )
    parser.add_argument("json_file", help="Path to Whisper JSON file")
    parser.add_argument(
        "--fill-gaps",
        action="store_true",
        help="Insert blank rows for temporal gaps between consecutive words"
    )
    parser.add_argument(
        "--min-gap",
        type=float,
        default=0.0,
        help="Minimum gap (in seconds) required to insert a blank row (default: 0.0)"
    )
    parser.add_argument(
        "--source-video",
        type=str,
        default="",
        help="Source video filename to include in CSV rows",
    )
    return parser.parse_args(argv)

if __name__ == "__main__":
    args = parse_args(sys.argv[1:])
    export_words_to_csv(
        args.json_file,
        fill_gaps=args.fill_gaps,
        min_gap=args.min_gap,
        source_video=args.source_video or None,
    )
