#!/usr/bin/env python3
import json
import sys
import argparse
from pathlib import Path


DEFAULT_KEYWORDS = ["adversity", "federal", "resilien", "communit", "action"]


def iter_matching_words_with_context(json_file, keywords, window=15):
    with open(json_file, "r", encoding="utf-8") as f:
        data = json.load(f)
    words = []
    for segment in data.get("segments", []):
        seg_speaker = segment.get("speaker", "")
        for w in segment.get("words", []) or []:
            word = (w.get("word") or "").strip()
            if not word:
                continue
            words.append(
                {
                    "start": w.get("start", ""),
                    "end": w.get("end", ""),
                    "speaker": w.get("speaker", "") or seg_speaker,
                    "word": word,
                }
            )

    match_indices = []
    for i, w in enumerate(words):
        lower_word = w["word"].lower()
        if any(k in lower_word for k in keywords):
            match_indices.append(i)

    if not match_indices:
        return

    # Build merged intervals of words to include (window around each match)
    intervals = []
    for idx in match_indices:
        start = max(0, idx - window)
        end = min(len(words) - 1, idx + window)
        match_word = words[idx]["word"]
        if not intervals:
            intervals.append([start, end, {match_word}])
            continue
        last_start, last_end, last_matches = intervals[-1]
        if start <= last_end + 1:
            intervals[-1][1] = max(last_end, end)
            last_matches.add(match_word)
        else:
            intervals.append([start, end, {match_word}])

    seen = set()
    for start, end, match_words in intervals:
        match_word_str = "|".join(sorted(match_words))
        for i in range(start, end + 1):
            w = words[i]
            key = (w["start"], w["end"], w["word"])
            if key in seen:
                continue
            seen.add(key)
            yield {
                "file": json_file.name,
                "start": w.get("start", ""),
                "end": w.get("end", ""),
                "speaker": w.get("speaker", ""),
                "word": w.get("word", ""),
                "match_word": match_word_str,
            }


def create_themes_csv(directory, keywords, output_path, window=15):
    directory = Path(directory)
    json_files = sorted(directory.glob("P10*.json"))
    rows = []
    for json_file in json_files:
        for row in iter_matching_words_with_context(json_file, keywords, window=window):
            row["x"] = 0.5
            row["y"] = 0.5
            row["zoom"] = 1
            rows.append(row)

    keys = ["file", "start", "end", "speaker", "word", "match_word", "x", "y", "zoom"]

    def render_value(v):
        return json.dumps(v, ensure_ascii=False)

    field_widths = {k: 0 for k in keys}
    for row in rows:
        for k in keys:
            field = f"\"{k}\": {render_value(row.get(k, ''))}"
            field_widths[k] = max(field_widths[k], len(field))

    with open(output_path, "w", encoding="utf-8") as out:
        for row in rows:
            fields = []
            for k in keys:
                field = f"\"{k}\": {render_value(row.get(k, ''))}"
                fields.append(field.ljust(field_widths[k]))
            if fields:
                line = "{ " + fields[0]
                for idx, f in enumerate(fields[:-1]):
                    pad = field_widths[keys[idx]] - len(f)
                    sep = "," + (" " * (pad + 1))
                    line += sep + fields[idx + 1]
                line += " }"
            else:
                line = "{ }"
            out.write(line + "\n")


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Scan P10*.json files for thematic keywords and print matching lines."
    )
    parser.add_argument(
        "--dir",
        type=str,
        default="CLIMATE EVENT BUTTERFLY EFFECT 2025 RAWS",
        help="Directory containing P10*.json files",
    )
    parser.add_argument(
        "--keywords",
        type=str,
        default=",".join(DEFAULT_KEYWORDS),
        help="Comma-separated keyword substrings to match",
    )
    parser.add_argument(
        "--output",
        type=str,
        default="themes_words.jsonl",
        help="Output JSONL file (default: themes_words.jsonl)",
    )
    parser.add_argument(
        "--window",
        type=int,
        default=15,
        help="Words of context before/after each match (default: 15)",
    )
    return parser.parse_args(argv)


if __name__ == "__main__":
    args = parse_args(sys.argv[1:])
    keywords = [k.strip().lower() for k in args.keywords.split(",") if k.strip()]
    create_themes_csv(args.dir, keywords, args.output, window=args.window)
