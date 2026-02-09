import json
import numpy as np
from pydub import AudioSegment


def load_word_segments_from_json(json_path):
    """
    Load word-level segments from a Whisper-style JSON file.
    """
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    word_segment_times = []
    for segment in data.get("segments", []):
        for w in segment.get("words", []):
            try:
                start = float(w.get("start", 0.0))
            except Exception:
                start = 0.0
            try:
                end = float(w.get("end", start))
            except Exception:
                end = start
            word_segment_times.append(
                {
                    "speaker": w.get("speaker", "") if w.get("speaker") is not None else "",
                    "start": start,
                    "end": end,
                    "word": (w.get("word") or "").strip(),
                }
            )
    return word_segment_times


def get_audio_sample_rate(audio_file):
    """
    Load audio and return its sample rate.
    """
    audio = AudioSegment.from_file(audio_file)
    return audio.frame_rate


def align_segments_to_sample_boundaries(word_segment_times, sr):
    """
    Align word start/end times to audio sample boundaries.
    """
    aligned = []
    for w in word_segment_times:
        start = int(float(w.get("start", 0.0)) * sr) / sr
        end = int(float(w.get("end", start)) * sr) / sr
        new_w = dict(w)
        new_w["start"] = start
        new_w["end"] = end
        aligned.append(new_w)
    return aligned


def fix_zero_length_words(
    word_segment_times, sr, preappend_budget=0.2, shift_seconds=0.15
):
    """
    word_segment_times: list[dict], each dict at least has {"start": float, "end": float, ...}
    Mutates a copy of the list and returns it.
    """
    totaltime = 0
    preappend_budget = int(preappend_budget * sr) / sr
    if not word_segment_times:
        return []

    out = [dict(word_segment_times[0])]  # copy first row as-is
    for i in range(1, len(word_segment_times)):
        prev = out[-1]  # previous (already possibly adjusted)
        curr = dict(word_segment_times[i])  # copy current
        # apply shift
        curr["start"] = curr["start"] - shift_seconds

        # lengthening is ok
        if i < len(word_segment_times) - 1:
            curr["end"] = min(
                curr["end"], word_segment_times[i + 1]["start"] - shift_seconds
            )

        if curr["start"] < 0:
            continue

        curr_len = curr["end"] - curr["start"]
        if curr_len <= 0:
            # Compute allowable pull-back
            prev_dur = max(0.0, prev["end"] - prev["start"])
            halftime = 0.5 * prev_dur
            budget = min(preappend_budget, halftime)

            # Limit 1: at most `budget` earlier than nominal start
            candidate = curr["start"] - budget

            # Limit 2: do not advance earlier than the previous word's midpoint
            prev_midpoint_limit = prev["end"] - halftime  # == prev["start"] + halftime

            new_start = max(candidate, prev_midpoint_limit)

            # Ensure we don't go before the previous *start* (can happen if prev_dur == 0)
            new_start = max(new_start, prev["start"])

            # Adjust previous end only if we overlapped it
            if new_start < prev["end"]:
                prev["end"] = new_start

            # Commit the new start; clamp end if needed (still allow zero-length after move)
            curr["start"] = new_start
            if curr["end"] < curr["start"]:
                curr["end"] = curr["start"]
        totaltime += curr["end"] - curr["start"]
        out.append(curr)
    finalout = []
    for segment in out:
        if segment["end"] - segment["start"] > 0.01:
            finalout.append(segment)
        else:
            print(f"Segment too short {segment}")
    print(totaltime)
    return out


def extend_segments(word_segment_times, extend_ms=20):
    """
    Extend each segment end by extend_ms, capped to the next segment start.
    """
    if not word_segment_times or extend_ms <= 0:
        return list(word_segment_times)
    extend_sec = float(extend_ms) / 1000.0
    out = []
    for i, seg in enumerate(word_segment_times):
        curr = dict(seg)
        next_start = None
        if i + 1 < len(word_segment_times):
            next_start = word_segment_times[i + 1].get("start")
        new_end = curr.get("end", curr.get("start", 0.0)) + extend_sec
        if next_start is not None:
            try:
                new_end = min(new_end, float(next_start))
            except Exception:
                pass
        curr["end"] = max(curr.get("start", 0.0), new_end)
        out.append(curr)
    return out


def prepend_segments(word_segment_times, prepend_ms=20):
    """
    Move each segment start earlier by prepend_ms, capped to the previous segment end (and >= 0).
    """
    if not word_segment_times or prepend_ms <= 0:
        return list(word_segment_times)
    prepend_sec = float(prepend_ms) / 1000.0
    out = []
    prev_end = 0.0
    for i, seg in enumerate(word_segment_times):
        curr = dict(seg)
        start = float(curr.get("start", 0.0))
        candidate = max(0.0, start - prepend_sec)
        if i > 0:
            candidate = max(candidate, prev_end)
        curr["start"] = min(start, candidate)
        if curr.get("end", curr["start"]) < curr["start"]:
            curr["end"] = curr["start"]
        out.append(curr)
        prev_end = float(curr.get("end", curr["start"]))
    return out


def fill_gaps(word_segment_times, min_gap=0.0):
    """
    Insert blank word segments for gaps larger than min_gap (seconds).
    """
    if not word_segment_times:
        return []
    out = []
    prev = None
    for w in word_segment_times:
        if prev is not None:
            try:
                gap = float(w.get("start", 0.0)) - float(prev.get("end", 0.0))
            except Exception:
                gap = 0.0
            if gap > float(min_gap):
                out.append(
                    {
                        "speaker": prev.get("speaker", "") if prev.get("speaker") is not None else "",
                        "start": float(prev.get("end", 0.0)),
                        "end": float(w.get("start", 0.0)),
                        "word": "",
                    }
                )
        out.append(dict(w))
        prev = w
    return out
