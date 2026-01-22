#!/usr/bin/env python3
"""
json2html_speakers.py

Read a diarized transcript JSON like the one you pasted and write an HTML file
where speaker changes delimit the text.

Usage:
  python json2html_speakers.py input.json output.html

Notes:
- Prefers per-word "speaker" labels when present (more accurate than segment-level).
- Collapses consecutive words by the same speaker into one block.
- Adds basic styling + timestamps for each block.
"""

from __future__ import annotations

import json
import html
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple, Optional


def die(msg: str, code: int = 1) -> None:
    print(msg, file=sys.stderr)
    raise SystemExit(code)


def fmt_ts(seconds: float) -> str:
    # hh:mm:ss.mmm (but omit hours if 0)
    if seconds < 0:
        seconds = 0.0
    ms = int(round(seconds * 1000.0))
    s, ms = divmod(ms, 1000)
    m, s = divmod(s, 60)
    h, m = divmod(m, 60)
    if h:
        return f"{h:02d}:{m:02d}:{s:02d}.{ms:03d}"
    return f"{m:02d}:{s:02d}.{ms:03d}"


def normalize_spacing(text: str) -> str:
    # Basic cleanup for word-joined text
    text = re.sub(r"\s+", " ", text).strip()

    # Remove spaces before punctuation
    text = re.sub(r"\s+([,.;:!?])", r"\1", text)

    # Fix space after opening quotes/parens (optional gentle)
    text = re.sub(r"([(\u201c\"'])\s+", r"\1", text)

    # Ensure a space after punctuation when followed by a word char
    text = re.sub(r"([,.;:!?])([A-Za-z0-9])", r"\1 \2", text)
    return text


def build_speaker_blocks(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    """
    Returns blocks like:
      { speaker, start, end, text }
    using per-word speaker labels when available.
    """
    segments = data.get("segments")
    if not isinstance(segments, list):
        die("Input JSON must have a top-level key 'segments' that is a list.")

    blocks: List[Dict[str, Any]] = []

    cur_speaker: Optional[str] = None
    cur_words: List[str] = []
    cur_start: Optional[float] = None
    cur_end: Optional[float] = None

    def flush() -> None:
        nonlocal cur_speaker, cur_words, cur_start, cur_end
        if cur_speaker is None or not cur_words or cur_start is None or cur_end is None:
            cur_speaker, cur_words, cur_start, cur_end = None, [], None, None
            return
        text = normalize_spacing(" ".join(cur_words))
        blocks.append(
            {
                "speaker": cur_speaker,
                "start": float(cur_start),
                "end": float(cur_end),
                "text": text,
            }
        )
        cur_speaker, cur_words, cur_start, cur_end = None, [], None, None

    for seg in segments:
        words = seg.get("words")
        if isinstance(words, list) and words:
            # Word-level diarization
            for w in words:
                spk = w.get("speaker") or seg.get("speaker") or "UNKNOWN"
                word = w.get("word", "")
                if not isinstance(word, str):
                    continue
                word = word.strip()
                if not word:
                    continue

                w_start = float(w.get("start", seg.get("start", 0.0) or 0.0))
                w_end = float(w.get("end", seg.get("end", w_start) or w_start))

                if cur_speaker is None:
                    cur_speaker = str(spk)
                    cur_start = w_start
                    cur_end = w_end
                    cur_words = [word]
                elif str(spk) == cur_speaker:
                    cur_words.append(word)
                    cur_end = w_end
                else:
                    flush()
                    cur_speaker = str(spk)
                    cur_start = w_start
                    cur_end = w_end
                    cur_words = [word]
        else:
            # Fallback to segment text
            spk = seg.get("speaker") or "UNKNOWN"
            text = seg.get("text", "")
            if not isinstance(text, str):
                continue
            text = normalize_spacing(text)
            if not text:
                continue

            s_start = float(seg.get("start", 0.0) or 0.0)
            s_end = float(seg.get("end", s_start) or s_start)

            if cur_speaker is None:
                cur_speaker = str(spk)
                cur_start = s_start
                cur_end = s_end
                cur_words = [text]  # treat as one "chunk"
            elif str(spk) == cur_speaker:
                cur_words.append(text)
                cur_end = s_end
            else:
                flush()
                cur_speaker = str(spk)
                cur_start = s_start
                cur_end = s_end
                cur_words = [text]

    flush()
    return blocks


def render_html(blocks: List[Dict[str, Any]], title: str = "Transcript") -> str:
    # Build speaker list for stable coloring via CSS classes
    speakers = sorted({b["speaker"] for b in blocks})
    speaker_class = {s: f"spk-{i}" for i, s in enumerate(speakers)}

    # Simple color palette via HSL; no JS required
    speaker_css_lines = []
    for i, s in enumerate(speakers):
        hue = (i * 47) % 360
        cls = speaker_class[s]
        speaker_css_lines.append(
            f""".{cls} .speaker-badge {{
  background: hsl({hue} 70% 45% / 0.18);
  border-color: hsl({hue} 70% 45% / 0.55);
  color: hsl({hue} 70% 30%);
}}"""
        )

    rows = []
    for b in blocks:
        spk = b["speaker"]
        cls = speaker_class.get(spk, "spk-x")
        start = fmt_ts(float(b["start"]))
        end = fmt_ts(float(b["end"]))
        text = html.escape(str(b["text"]))
        spk_label = html.escape(str(spk))

        rows.append(
            f"""
      <div class="block {cls}">
        <div class="meta">
          <span class="speaker-badge">{spk_label}</span>
          <span class="ts">{start} → {end}</span>
        </div>
        <div class="text">{text}</div>
      </div>
""".rstrip()
        )

    css = "\n".join(speaker_css_lines)

    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>{html.escape(title)}</title>
  <style>
    :root {{
      --bg: #0b0c10;
      --card: #11131a;
      --text: #e8eaf0;
      --muted: #aeb3c2;
      --border: rgba(255,255,255,0.10);
    }}
    body {{
      margin: 0;
      background: var(--bg);
      color: var(--text);
      font: 14px/1.5 system-ui, -apple-system, Segoe UI, Roboto, Ubuntu, Cantarell, Noto Sans, Helvetica, Arial;
    }}
    .wrap {{
      max-width: 980px;
      margin: 0 auto;
      padding: 28px 18px 60px;
    }}
    h1 {{
      font-size: 20px;
      margin: 0 0 14px;
      letter-spacing: 0.2px;
    }}
    .hint {{
      color: var(--muted);
      margin: 0 0 18px;
      font-size: 13px;
    }}
    .block {{
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 14px;
      padding: 12px 14px;
      margin: 10px 0;
    }}
    .meta {{
      display: flex;
      align-items: center;
      gap: 10px;
      margin-bottom: 8px;
    }}
    .speaker-badge {{
      display: inline-block;
      padding: 3px 10px;
      border-radius: 999px;
      border: 1px solid var(--border);
      font-weight: 650;
      letter-spacing: 0.2px;
    }}
    .ts {{
      color: var(--muted);
      font-variant-numeric: tabular-nums;
      font-size: 12px;
    }}
    .text {{
      white-space: pre-wrap;
      word-wrap: break-word;
      font-size: 15px;
    }}

    {css}
  </style>
</head>
<body>
  <div class="wrap">
    <h1>{html.escape(title)}</h1>
    <p class="hint">Speaker changes create new blocks. Timestamps are based on word timing when available.</p>
    {"".join(rows)}
  </div>
</body>
</html>
"""


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        die("Usage: python json2html_speakers.py input.json [output.html]")

    in_path = Path(argv[1])
    out_path = Path(argv[2]) if len(argv) >= 3 else in_path.with_suffix(".html")

    if not in_path.exists():
        die(f"Input file not found: {in_path}")

    data = json.loads(in_path.read_text(encoding="utf-8"))
    blocks = build_speaker_blocks(data)
    title = in_path.stem.replace("_", " ")
    html_doc = render_html(blocks, title=title)

    out_path.write_text(html_doc, encoding="utf-8")
    print(f"Wrote {out_path} ({len(blocks)} speaker blocks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

