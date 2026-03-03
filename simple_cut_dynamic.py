#!/usr/bin/env python3
"""
Simple cut with dynamic zoom switching aligned to punctuation and face tracking.
Optional subtitle rendering.
"""
import argparse
import csv
import bisect
import json
import math
import os
import sys
import subprocess
import wave

import cv2
import numpy as np
from tqdm import tqdm
from PIL import ImageFont

import extract_audio_segments
import segment_utils
import transcriptJson2csv
from subtitles import apply_subtitles, build_subtitle_context, get_active_word_idx, parse_bgr, build_word_groups
from zoom_select import select_zoom_rects

# Reuse helpers from simple_cut where possible.
from simple_cut import (
    load_word_segments_from_csv,
    strip_standard_suffixes,
    postpend_silence_to_wav,
    combine_video_audio,
    reencode_video,
)


def prepend_silence_to_wav(path, seconds):
    """
    Prepend silence to a WAV file in-place using a temp file.
    """
    if seconds <= 0:
        return False
    tmp_path = path + ".silence_pre.tmp"
    with wave.open(path, "rb") as rf:
        params = rf.getparams()
        nchannels = params.nchannels
        sampwidth = params.sampwidth
        framerate = params.framerate
        silence_frames = int(round(seconds * framerate))
        if silence_frames <= 0:
            return False
        with wave.open(tmp_path, "wb") as wf:
            wf.setparams(params)
            silence_byte = b"\x80" if sampwidth == 1 else b"\x00"
            wf.writeframes(silence_byte * silence_frames * nchannels * sampwidth)
            while True:
                data = rf.readframes(16384)
                if not data:
                    break
                wf.writeframes(data)
    os.replace(tmp_path, path)
    return True


def trim_leading_audio_wav(path, seconds):
    """
    Trim leading audio from a WAV file in-place using a temp file.
    """
    if seconds <= 0:
        return False
    tmp_path = path + ".trim_pre.tmp"
    with wave.open(path, "rb") as rf:
        params = rf.getparams()
        nchannels = params.nchannels
        sampwidth = params.sampwidth
        framerate = params.framerate
        trim_frames = int(round(seconds * framerate))
        if trim_frames <= 0:
            return False
        with wave.open(tmp_path, "wb") as wf:
            wf.setparams(params)
            rf.readframes(trim_frames)
            while True:
                data = rf.readframes(16384)
                if not data:
                    break
                wf.writeframes(data)
    os.replace(tmp_path, path)
    return True


def _parse_timecode_to_seconds(value):
    s = str(value).strip()
    if not s:
        return None
    if ":" in s:
        parts = [p.strip() for p in s.split(":")]
        try:
            nums = [float(p) for p in parts]
        except ValueError:
            return None
        if len(nums) == 3:
            return nums[0] * 3600.0 + nums[1] * 60.0 + nums[2]
        if len(nums) == 2:
            return nums[0] * 60.0 + nums[1]
        if len(nums) == 1:
            return nums[0]
        return None
    try:
        return float(s)
    except ValueError:
        return None


def _read_notch_csv(path):
    ranges = []
    try:
        with open(path, "r", encoding="utf-8") as f:
            reader = csv.reader(f)
            for row in reader:
                if not row:
                    continue
                if len(row) >= 2:
                    start = _parse_timecode_to_seconds(row[0])
                    end = _parse_timecode_to_seconds(row[1])
                    if start is None or end is None:
                        continue
                    if end > start:
                        ranges.append((float(start), float(end)))
    except Exception as e:
        print(f"Warning: failed to read notch CSV {path}: {e}")
    return ranges


def _write_notch_csv(path, ranges):
    try:
        with open(path, "w", encoding="utf-8", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["start", "end"])
            for start, end in ranges:
                writer.writerow([f"{float(start):.3f}", f"{float(end):.3f}"])
    except Exception as e:
        print(f"Warning: failed to write notch CSV {path}: {e}")


def _normalize_notch_ranges(ranges):
    if not ranges:
        return []
    ranges = sorted([(float(s), float(e)) for s, e in ranges if e > s], key=lambda r: (r[0], r[1]))
    merged = [ranges[0]]
    for start, end in ranges[1:]:
        last_start, last_end = merged[-1]
        if start <= last_end:
            merged[-1] = (last_start, max(last_end, end))
        else:
            merged.append((start, end))
    return merged


def _build_keep_ranges(total_duration, notch_ranges):
    if total_duration is None or total_duration <= 0:
        return []
    notch_ranges = _normalize_notch_ranges(notch_ranges)
    if not notch_ranges:
        return [(0.0, float(total_duration))]
    clipped = []
    for start, end in notch_ranges:
        s = max(0.0, float(start))
        e = min(float(total_duration), float(end))
        if e > s:
            clipped.append((s, e))
    clipped = _normalize_notch_ranges(clipped)
    keep = []
    cursor = 0.0
    for start, end in clipped:
        if start > cursor:
            keep.append((cursor, start))
        cursor = max(cursor, end)
    if cursor < total_duration:
        keep.append((cursor, float(total_duration)))
    return keep


def _precompute_segments_from_ranges(ranges, fps):
    precomputed = []
    for start, end in ranges:
        start_f = float(start) * fps
        end_f = float(end) * fps
        start_frame = math.floor(start_f)
        end_frame = math.floor(end_f)
        if end_frame < start_frame:
            end_frame = start_frame
        precomputed.append(
            {
                "word": "",
                "start_frame": start_frame,
                "end_frame": end_frame,
                "start_sec": start_frame / fps,
                "end_sec": end_frame / fps,
            }
        )
    return precomputed


def _prompt_notch_ranges_gui(path):
    try:
        import tkinter as tk
        from tkinter import messagebox
        from tkinter.scrolledtext import ScrolledText
    except Exception as e:
        raise RuntimeError(
            f"GUI not available for notch input: {e}\n"
            "Install tkinter, e.g.:\n"
            "  pip install tk\n"
            "If you prefer system packages on Ubuntu:\n"
            "  sudo apt-get install python3-tk"
        )

    result = {"ranges": None}

    def _parse_lines(text):
        ranges = []
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 2:
                return None, f"Invalid line (need start,end): {line}"
            start = _parse_timecode_to_seconds(parts[0])
            end = _parse_timecode_to_seconds(parts[1])
            if start is None or end is None:
                return None, f"Invalid timecode in line: {line}"
            if end <= start:
                return None, f"End must be > start: {line}"
            ranges.append((float(start), float(end)))
        return ranges, None

    root = tk.Tk()
    root.title("Notch Ranges (Remove Sections)")
    root.geometry("520x360")

    msg = (
        "Enter ranges to remove from the final cut.\n"
        "Format: start,end (seconds or mm:ss or hh:mm:ss)\n"
        "Example:\n"
        "12.5,18.0\n"
        "01:02,01:15.5"
    )
    label = tk.Label(root, text=msg, justify="left")
    label.pack(padx=10, pady=8, anchor="w")

    text = ScrolledText(root, height=10)
    text.pack(fill="both", expand=True, padx=10, pady=6)

    btn_frame = tk.Frame(root)
    btn_frame.pack(pady=8)

    def on_save():
        ranges, err = _parse_lines(text.get("1.0", "end"))
        if err:
            messagebox.showerror("Invalid Input", err)
            return
        result["ranges"] = ranges
        _write_notch_csv(path, ranges)
        root.destroy()

    def on_skip():
        result["ranges"] = []
        root.destroy()

    tk.Button(btn_frame, text="Save", command=on_save, width=12).pack(side="left", padx=6)
    tk.Button(btn_frame, text="Skip", command=on_skip, width=12).pack(side="left", padx=6)

    root.mainloop()
    return result["ranges"] if result["ranges"] is not None else []


def _prompt_notch_ranges_cli():
    print("Enter ranges to remove (start,end). Use seconds or mm:ss or hh:mm:ss.")
    print("One range per line. Blank line to finish. Example: 12.5,18.0")
    ranges = []
    while True:
        try:
            line = input("notch> ").strip()
        except EOFError:
            break
        if not line:
            break
        if line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 2:
            print("Invalid line (need start,end).")
            continue
        start = _parse_timecode_to_seconds(parts[0])
        end = _parse_timecode_to_seconds(parts[1])
        if start is None or end is None:
            print("Invalid timecode.")
            continue
        if end <= start:
            print("End must be > start.")
            continue
        ranges.append((float(start), float(end)))
    return ranges


def _apply_notch_ranges(word_segment_times, ranges):
    if not ranges:
        return word_segment_times
    ranges = _normalize_notch_ranges(ranges)
    kept = []
    removed = 0
    for seg in word_segment_times:
        start = float(seg.get("start", 0.0))
        end = float(seg.get("end", start))
        overlap = False
        for r_start, r_end in ranges:
            if end <= r_start:
                break
            if start < r_end and end > r_start:
                overlap = True
                break
        if overlap:
            removed += 1
            continue
        kept.append(seg)
    print(f"Applied notch ranges: removed {removed} word segments, kept {len(kept)}")
    return kept


def _retime_segments_for_notches(word_segment_times, ranges):
    if not ranges:
        return word_segment_times
    ranges = _normalize_notch_ranges(ranges)
    retimed = []
    removed = 0
    for seg in word_segment_times:
        start = float(seg.get("start", 0.0))
        end = float(seg.get("end", start))
        overlap = False
        for r_start, r_end in ranges:
            if end <= r_start:
                break
            if start < r_end and end > r_start:
                overlap = True
                break
        if overlap:
            removed += 1
            continue
        shift = 0.0
        for r_start, r_end in ranges:
            if r_end <= start:
                shift += (r_end - r_start)
            else:
                break
        curr = dict(seg)
        curr["start"] = max(0.0, start - shift)
        curr["end"] = max(curr["start"], end - shift)
        retimed.append(curr)
    print(f"Retimed notch ranges: removed {removed} word segments, kept {len(retimed)}")
    return retimed


def _retime_segments_for_word_cuts(word_segment_times):
    if not word_segment_times:
        return word_segment_times
    retimed = []
    cursor = 0.0
    for seg in word_segment_times:
        start = float(seg.get("start", 0.0))
        end = float(seg.get("end", start))
        dur = max(0.0, end - start)
        curr = dict(seg)
        curr["start"] = cursor
        curr["end"] = cursor + dur
        retimed.append(curr)
        cursor += dur
    print(f"Retimed word segments to concatenated timeline ({cursor:.2f}s total).")
    return retimed


def _retime_segments_to_frame_timeline(word_segment_times, fps):
    if not word_segment_times or not fps:
        return word_segment_times
    retimed = []
    cursor_frames = 0
    for seg in word_segment_times:
        start = float(seg.get("start", 0.0))
        end = float(seg.get("end", start))
        start_frame = math.floor(start * fps)
        end_frame = math.floor(end * fps)
        if end_frame < start_frame:
            end_frame = start_frame
        dur_frames = max(0, end_frame - start_frame)
        curr = dict(seg)
        curr["start"] = cursor_frames / fps
        curr["end"] = (cursor_frames + dur_frames) / fps
        retimed.append(curr)
        cursor_frames += dur_frames
    print(f"Quantized word segments to frame timeline ({cursor_frames / fps:.2f}s total).")
    return retimed


def _retime_zoom_rects_for_notches(zoom_rects, ranges, fps):
    if not zoom_rects or not ranges or not fps:
        return zoom_rects
    ranges = _normalize_notch_ranges(ranges)
    retimed = []
    removed = 0
    for rec in zoom_rects:
        frame = rec.get("frame")
        if frame is None:
            retimed.append(rec)
            continue
        t = float(frame) / float(fps)
        overlap = False
        for r_start, r_end in ranges:
            if t < r_start:
                break
            if t >= r_start and t < r_end:
                overlap = True
                break
        if overlap:
            removed += 1
            continue
        shift = 0.0
        for r_start, r_end in ranges:
            if r_end <= t:
                shift += (r_end - r_start)
            else:
                break
        new_t = max(0.0, t - shift)
        new_frame = int(round(new_t * float(fps)))
        curr = dict(rec)
        curr["frame"] = new_frame
        retimed.append(curr)
    if removed:
        print(f"Retimed zoom centers for notches: dropped {removed} point(s), kept {len(retimed)}")
    return retimed


def precompute_segments_basic(word_segment_times, fps):
    precomputed = []
    for seg in tqdm(word_segment_times, desc="Precomputing segment timings"):
        start_f = seg["start"] * fps
        end_f = seg["end"] * fps
        start_frame = math.floor(start_f)
        end_frame = math.floor(end_f)
        if end_frame < start_frame:
            end_frame = start_frame
        precomputed.append(
            {
                "word": seg.get("word", ""),
                "start_frame": start_frame,
                "end_frame": end_frame,
                "start_sec": start_frame / fps,
                "end_sec": end_frame / fps,
            }
        )
    return precomputed


def enforce_min_caption_gap(word_segment_times, min_gap_seconds):
    if not word_segment_times or min_gap_seconds is None:
        return word_segment_times
    min_gap = float(min_gap_seconds)
    if min_gap <= 0:
        return word_segment_times
    adjusted = [dict(word_segment_times[0])]
    for seg in word_segment_times[1:]:
        curr = dict(seg)
        prev = adjusted[-1]
        try:
            gap = float(curr.get("start", 0.0)) - float(prev.get("end", 0.0))
        except Exception:
            gap = 0.0
        if gap < min_gap:
            prev_end = float(prev.get("end", prev.get("start", 0.0)))
            prev["end"] = max(prev_end, float(curr.get("start", prev_end)))
        adjusted.append(curr)
    return adjusted


def build_punctuation_boundaries(word_segment_times, use_absolute_time=False, total_duration=None):
    groups = build_word_groups(
        word_segment_times, group_size=10_000_000, max_chars=None, subtitle_lines=1
    )

    if use_absolute_time:
        boundaries = []
        last_end = 0.0
        for g in groups:
            if not g:
                continue
            last_idx = g[-1]
            if last_idx < len(word_segment_times):
                end = float(word_segment_times[last_idx].get("end", 0.0))
                if end > 0:
                    boundaries.append(end)
                    last_end = max(last_end, end)
        if total_duration is None:
            total_duration = last_end
        if total_duration is None:
            total_duration = 0.0
        boundaries = sorted(set(b for b in boundaries if b > 0.0))
        if total_duration > 0 and (not boundaries or boundaries[-1] < total_duration):
            boundaries.append(total_duration)
        return boundaries, float(total_duration)

    durations = []
    for seg in word_segment_times:
        start = float(seg.get("start", 0.0))
        end = float(seg.get("end", start))
        durations.append(max(0.0, end - start))

    cum_end = []
    acc = 0.0
    for d in durations:
        acc += d
        cum_end.append(acc)
    total_out = acc

    boundaries = []
    for g in groups:
        if not g:
            continue
        last_idx = g[-1]
        if last_idx < len(cum_end):
            boundaries.append(cum_end[last_idx])

    if cum_end:
        boundaries.append(cum_end[-1])

    boundaries = sorted(set(b for b in boundaries if b > 0.0))
    if total_out > 0 and (not boundaries or boundaries[-1] < total_out):
        boundaries.append(total_out)
    return boundaries, total_out


def build_zoom_schedule(
    boundaries,
    total_out,
    zoom_in=1.25,
    zoom_out=1.0,
    zoom_seconds=10.0,
    align_window=3.0,
    min_segment=2.0,
    start_zoom_in=True,
):
    schedule = []
    if total_out <= 0:
        return schedule

    t = 0.0
    zoom_flag = bool(start_zoom_in)
    while t < total_out - 1e-6:
        target = t + float(zoom_seconds)
        candidate = None

        idx = bisect.bisect_left(boundaries, target)
        candidates = []
        if idx < len(boundaries):
            candidates.append(boundaries[idx])
        if idx > 0:
            candidates.append(boundaries[idx - 1])

        best = None
        best_dist = None
        for b in candidates:
            if b <= t + min_segment:
                continue
            dist = abs(b - target)
            if align_window is not None and dist > align_window:
                continue
            if best is None or dist < best_dist:
                best = b
                best_dist = dist
        candidate = best

        if candidate is None:
            # Fall back to first boundary after target, else end of output.
            for b in boundaries:
                if b > t + min_segment and b >= target:
                    candidate = b
                    break
        if candidate is None:
            candidate = total_out

        if candidate <= t:
            break

        schedule.append(
            {
                "start": t,
                "end": candidate,
                "zoom": zoom_in if zoom_flag else zoom_out,
            }
        )
        t = candidate
        zoom_flag = not zoom_flag

    if schedule and schedule[-1]["end"] < total_out:
        schedule.append(
            {
                "start": schedule[-1]["end"],
                "end": total_out,
                "zoom": zoom_in if zoom_flag else zoom_out,
            }
        )

    return schedule


def get_zoom_at_time(t, schedule, ramp_sec=0.0):
    if not schedule:
        return 1.0
    ramp = max(0.0, float(ramp_sec or 0.0))
    # Find current segment
    idx = 0
    while idx < len(schedule) and t >= schedule[idx]["end"]:
        idx += 1
    if idx >= len(schedule):
        idx = len(schedule) - 1
    seg = schedule[idx]
    z = float(seg.get("zoom", 1.0))
    if ramp <= 0:
        return z

    # Ramp in from previous segment
    if idx > 0:
        prev = schedule[idx - 1]
        t0 = float(seg.get("start", 0.0))
        if t < t0 + ramp:
            z0 = float(prev.get("zoom", z))
            alpha = max(0.0, min(1.0, (t - t0) / ramp))
            return z0 + (z - z0) * alpha

    # Ramp out to next segment
    if idx + 1 < len(schedule):
        next_seg = schedule[idx + 1]
        t1 = float(seg.get("end", 0.0))
        if t > t1 - ramp:
            z1 = float(next_seg.get("zoom", z))
            alpha = max(0.0, min(1.0, (t - (t1 - ramp)) / ramp))
            return z + (z1 - z) * alpha

    return z


def load_centers_jsonl(path):
    centers_by_frame = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            try:
                frame = int(rec.get("frame", -1))
            except Exception:
                continue
            cx = rec.get("center_x")
            cy = rec.get("center_y")
            if frame < 0 or cx is None or cy is None:
                continue
            centers_by_frame.setdefault(frame, []).append(
                {
                    "center": (float(cx), float(cy)),
                    "score": rec.get("score"),
                    "instance_id": rec.get("instance_id"),
                }
            )
    return centers_by_frame


def infer_instance_id_from_jsonl(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            first_frame = None
            ids = []
            for line in f:
                line = line.strip()
                if not line:
                    continue
                rec = json.loads(line)
                frame = rec.get("frame")
                if frame is None:
                    continue
                try:
                    frame = int(frame)
                except Exception:
                    continue
                if first_frame is None:
                    first_frame = frame
                if frame != first_frame:
                    break
                rec_id = rec.get("instance_id")
                if rec_id is not None:
                    ids.append(rec_id)
            if not ids:
                return None
            # Prefer smallest numeric id if possible; otherwise first.
            numeric = []
            for rec_id in ids:
                try:
                    numeric.append(int(str(rec_id).strip()))
                except Exception:
                    pass
            if numeric:
                return str(min(numeric))
            return str(ids[0])
    except Exception:
        return None


def pick_center_from_frame(centers, last_center=None):
    if not centers:
        return None
    # Prefer highest score if provided, else nearest to last_center, else first.
    scored = [c for c in centers if c.get("score") is not None]
    if scored:
        best = max(scored, key=lambda c: float(c.get("score", 0.0)))
        return best["center"]
    if last_center is None:
        return centers[0]["center"]
    best = None
    best_dist = None
    for c in centers:
        cx, cy = c["center"]
        dist = (cx - last_center[0]) ** 2 + (cy - last_center[1]) ** 2
        if best is None or dist < best_dist:
            best = c
            best_dist = dist
    return best["center"] if best else None


def find_center_from_jsonl(
    centers_by_frame,
    frame_idx,
    input_width,
    input_height,
    last_center=None,
    smooth=0.2,
    y_offset=0.0,
    max_gap=3,
):
    if not centers_by_frame:
        return last_center
    centers = centers_by_frame.get(frame_idx)
    if centers is None and max_gap > 0:
        for delta in range(1, max_gap + 1):
            centers = centers_by_frame.get(frame_idx - delta)
            if centers:
                break
            centers = centers_by_frame.get(frame_idx + delta)
            if centers:
                break
    center = pick_center_from_frame(centers or [], last_center=last_center)
    if center is None:
        return last_center

    y_offset_px = float(y_offset) * float(input_height)
    cx = max(0.0, min(float(input_width), float(center[0])))
    cy = max(0.0, min(float(input_height), float(center[1]) + y_offset_px))

    if last_center is None:
        return (cx, cy)
    alpha = max(0.0, min(1.0, float(smooth)))
    sx = last_center[0] + (cx - last_center[0]) * alpha
    sy = last_center[1] + (cy - last_center[1]) * alpha
    return (sx, sy)


def compute_crop_box(center, input_width, input_height, output_width, output_height, zoom):
    zoom = max(0.01, float(zoom))
    crop_w = max(2, int(round(output_width / zoom)))
    crop_h = max(2, int(round(output_height / zoom)))
    crop_w = min(crop_w, input_width)
    crop_h = min(crop_h, input_height)

    cx, cy = center
    x1 = int(round(cx - crop_w / 2))
    y1 = int(round(cy - crop_h / 2))
    x2 = x1 + crop_w
    y2 = y1 + crop_h

    if x1 < 0:
        x2 -= x1
        x1 = 0
    if y1 < 0:
        y2 -= y1
        y1 = 0
    if x2 > input_width:
        x1 -= x2 - input_width
        x2 = input_width
    if y2 > input_height:
        y1 -= y2 - input_height
        y2 = input_height

    x1 = max(0, x1)
    y1 = max(0, y1)
    x2 = min(input_width, x2)
    y2 = min(input_height, y2)
    return x1, y1, x2, y2


def normalize_rotation_degrees(angle):
    angle = float(angle or 0.0) % 360.0
    if angle > 180.0:
        angle -= 360.0
    if abs(angle) < 1e-9:
        return 0.0
    return angle


def _is_right_angle_rotation(angle):
    normalized = normalize_rotation_degrees(angle)
    nearest = round(normalized / 90.0) * 90
    return abs(normalized - nearest) < 1e-6


def get_rotated_dimensions(width, height, angle):
    angle = normalize_rotation_degrees(angle)
    if angle == 0.0:
        return int(width), int(height)
    if _is_right_angle_rotation(angle):
        quarter_turns = int(round(angle / 90.0)) % 4
        if quarter_turns % 2:
            return int(height), int(width)
        return int(width), int(height)
    radians = math.radians(angle)
    cos_v = abs(math.cos(radians))
    sin_v = abs(math.sin(radians))
    rotated_w = max(1, int(round(width * cos_v + height * sin_v)))
    rotated_h = max(1, int(round(height * cos_v + width * sin_v)))
    return rotated_w, rotated_h


def build_rotation_matrix(width, height, angle):
    angle = normalize_rotation_degrees(angle)
    if angle == 0.0:
        return None, int(width), int(height)
    center = (width / 2.0, height / 2.0)
    matrix = cv2.getRotationMatrix2D(center, angle, 1.0)
    rotated_w, rotated_h = get_rotated_dimensions(width, height, angle)
    matrix[0, 2] += (rotated_w / 2.0) - center[0]
    matrix[1, 2] += (rotated_h / 2.0) - center[1]
    return matrix, rotated_w, rotated_h


def rotate_frame(frame, angle):
    angle = normalize_rotation_degrees(angle)
    if angle == 0.0:
        return frame
    if _is_right_angle_rotation(angle):
        quarter_turns = int(round(angle / 90.0)) % 4
        if quarter_turns == 1:
            return cv2.rotate(frame, cv2.ROTATE_90_COUNTERCLOCKWISE)
        if quarter_turns == 2:
            return cv2.rotate(frame, cv2.ROTATE_180)
        if quarter_turns == 3:
            return cv2.rotate(frame, cv2.ROTATE_90_CLOCKWISE)
        return frame
    height, width = frame.shape[:2]
    matrix, rotated_w, rotated_h = build_rotation_matrix(width, height, angle)
    return cv2.warpAffine(
        frame,
        matrix,
        (rotated_w, rotated_h),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REPLICATE,
    )


def rotate_point(point, width, height, angle):
    angle = normalize_rotation_degrees(angle)
    if point is None or angle == 0.0:
        return point
    x, y = point
    if _is_right_angle_rotation(angle):
        quarter_turns = int(round(angle / 90.0)) % 4
        if quarter_turns == 1:
            return (y, width - x)
        if quarter_turns == 2:
            return (width - x, height - y)
        if quarter_turns == 3:
            return (height - y, x)
        return point
    matrix, _, _ = build_rotation_matrix(width, height, angle)
    rx = matrix[0, 0] * x + matrix[0, 1] * y + matrix[0, 2]
    ry = matrix[1, 0] * x + matrix[1, 1] * y + matrix[1, 2]
    return (rx, ry)


def format_state_label(state, zoom_rects=None, zoomed_count=0):
    if not zoom_rects:
        return state
    if state == "zooming":
        phase_idx = min(max(zoomed_count, 0), len(zoom_rects) - 1)
        return f"{state} circle={phase_idx + 1}"
    if state == "zoomed":
        phase_idx = min(max(zoomed_count - 1, 0), len(zoom_rects) - 1)
        return f"{state} circle={phase_idx + 1}"
    return state


def format_transition_state_label(state, zoom_rects=None, zoomed_count=0):
    if state == "zoomed":
        return format_state_label(state, zoom_rects, zoomed_count + 1)
    return format_state_label(state, zoom_rects, zoomed_count)


def get_zoom_rect_transition(zoom_rects, phase_idx, default="pan_zoom"):
    if not zoom_rects:
        return default
    phase_idx = min(max(int(phase_idx), 0), len(zoom_rects) - 1)
    transition = zoom_rects[phase_idx].get("transition", default)
    if transition not in ("jump", "pan_zoom"):
        return default
    return transition


def get_zoom_rect_rotate(zoom_rects, phase_idx, default=0.0):
    if not zoom_rects:
        return float(default)
    phase_idx = min(max(int(phase_idx), 0), len(zoom_rects) - 1)
    try:
        return float(zoom_rects[phase_idx].get("rotate", default) or 0.0)
    except Exception:
        return float(default)


def lerp_scalar(start, end, alpha):
    return float(start) + (float(end) - float(start)) * float(alpha)


def lerp_point(start, end, alpha):
    return (
        lerp_scalar(start[0], end[0], alpha),
        lerp_scalar(start[1], end[1], alpha),
    )


def rotate_output_frame(frame, angle):
    angle = normalize_rotation_degrees(angle)
    if angle == 0.0:
        return frame
    height, width = frame.shape[:2]
    center = (width / 2.0, height / 2.0)
    matrix = cv2.getRotationMatrix2D(center, angle, 1.0)
    return cv2.warpAffine(
        frame,
        matrix,
        (width, height),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REPLICATE,
    )


def build_zoom_rect_plan(total_frames, fps, interval_seconds=15.0):
    if fps <= 0 or total_frames <= 0:
        return [0]
    step = max(1, int(round(float(interval_seconds) * float(fps))))
    frames = list(range(step, total_frames, step))
    last_frame = max(0, total_frames - 1)
    if not frames:
        frames = [last_frame]
    elif frames[-1] != last_frame and (last_frame - frames[-1]) > max(1, step // 3):
        frames.append(last_frame)
    return frames


def zoom_rects_have_timeline(zoom_rects):
    if not zoom_rects:
        return False
    for rec in zoom_rects:
        if "frame" not in rec:
            return False
    return True


def resolve_zoom_rect_state(t, fps, input_width, input_height, zoom_rects, zoom_time):
    base = {
        "center": (input_width / 2.0, input_height / 2.0),
        "zoom": 1.0,
        "rotate": 0.0,
        "label": "base",
    }
    if not zoom_rects:
        return base

    prev = dict(base)
    prev_frame = 0.0
    for idx, rec in enumerate(zoom_rects):
        target_frame = float(rec.get("frame", prev_frame))
        target_time = target_frame / fps if fps else 0.0
        target = {
            "center": (
                float(rec.get("center_x", prev["center"][0])),
                float(rec.get("center_y", prev["center"][1])),
            ),
            "zoom": float(rec.get("zoom", prev["zoom"])),
            "rotate": float(rec.get("rotate", prev["rotate"])),
            "label": f"point={idx + 1}",
        }
        transition = rec.get("transition", "pan_zoom")
        if transition not in ("jump", "pan_zoom"):
            transition = "pan_zoom"
        if transition == "jump":
            if t < target_time:
                return prev
            prev = target
            prev_frame = target_frame
            continue

        transition_dur = max(0.001, float(zoom_time or 0.0))
        transition_start = max(prev_frame / fps if fps else 0.0, target_time - transition_dur)
        if t < transition_start:
            return prev
        if t < target_time:
            alpha = (t - transition_start) / max(target_time - transition_start, 1e-6)
            alpha = max(0.0, min(1.0, alpha))
            return {
                "center": lerp_point(prev["center"], target["center"], alpha),
                "zoom": lerp_scalar(prev["zoom"], target["zoom"], alpha),
                "rotate": lerp_scalar(prev["rotate"], target["rotate"], alpha),
                "label": f"pan_to={idx + 1}",
            }
        prev = target
        prev_frame = target_frame
    return prev


def create_video_writer(output_video_file, fps, output_width, output_height, temp_codec):
    codec_candidates = []
    requested = (temp_codec or "").strip()
    if requested:
        codec_candidates.append(requested)
    if requested.lower() in ("", "h264", "avc1", "x264"):
        for candidate in ("avc1", "H264", "X264", "mp4v", "MJPG"):
            if candidate not in codec_candidates:
                codec_candidates.append(candidate)
    else:
        for candidate in ("mp4v", "MJPG"):
            if candidate not in codec_candidates:
                codec_candidates.append(candidate)

    for candidate in codec_candidates:
        fourcc = cv2.VideoWriter_fourcc(*candidate)
        out = cv2.VideoWriter(output_video_file, fourcc, fps, (output_width, output_height))
        if out.isOpened():
            print(f"Using temp video codec: {candidate}")
            return out, candidate
        out.release()

    return None, None


def count_required_zoom_rects(total_duration, static_seconds, zooming_duration, zoomed_duration):
    seq_states = ["static", "zooming", "zoomed", "static", "zoomed", "static", "zooming", "zoomed"]
    stage_times = []
    t_cursor = 0.0
    for state in seq_states:
        stage_times.append((state, t_cursor))
        if state == "static":
            t_cursor += float(static_seconds or 0.0)
        elif state == "zooming":
            t_cursor += float(zooming_duration or 0.0)
        else:
            t_cursor += float(zoomed_duration or 0.0)
    cycle_duration = t_cursor if t_cursor > 0 else 1.0
    zoomed_centers_times = [
        t + (float(zoomed_duration or 0.0) / 2.0) for state, t in stage_times if state == "zoomed"
    ]
    expanded_times = []
    t_cycle = 0.0
    while t_cycle < float(total_duration or 0.0) - 1e-6:
        for zt in zoomed_centers_times:
            curr_t = zt + t_cycle
            if curr_t < float(total_duration or 0.0) - 1e-6:
                expanded_times.append(curr_t)
        t_cycle += cycle_duration
    return len(expanded_times)


def select_zoom_point(video_path, frame_idx, rotate_degrees=0.0):
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        print(f"Error: Could not open video file {video_path}")
        return None
    try:
        cap.set(cv2.CAP_PROP_POS_FRAMES, float(frame_idx))
        ok, frame = cap.read()
        if not ok or frame is None:
            print("Error: Could not read frame for zoom selection.")
            return None
    finally:
        cap.release()

    frame = rotate_frame(frame, rotate_degrees)
    draw = frame.copy()
    h, w = draw.shape[:2]
    max_dim = 1280
    scale = 1.0
    if max(h, w) > max_dim:
        scale = max_dim / float(max(h, w))
    if scale != 1.0:
        disp_w = max(1, int(round(w * scale)))
        disp_h = max(1, int(round(h * scale)))
        disp = cv2.resize(draw, (disp_w, disp_h), interpolation=cv2.INTER_AREA)
    else:
        disp_w, disp_h = w, h
        disp = draw

    win = "select zoom point"
    selected = {"pt": None}

    def _on_mouse(event, x, y, flags, param):
        if event != cv2.EVENT_LBUTTONDOWN:
            return
        ox = int(x / scale)
        oy = int(y / scale)
        selected["pt"] = (float(ox), float(oy))

    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(win, disp_w, disp_h)
    cv2.setMouseCallback(win, _on_mouse)
    while True:
        cv2.imshow(win, disp)
        key = cv2.waitKey(20)
        if selected["pt"] is not None:
            cv2.destroyWindow(win)
            return selected["pt"]
        if key in (27, ord("q")):
            cv2.destroyWindow(win)
            return None


def _zoomed_phase_index(seq_index):
    # zoomed states occur at indices 2, 4, 7 in seq_states
    if seq_index == 2:
        return 0
    if seq_index == 4:
        return 1
    if seq_index == 7:
        return 2
    # zooming states map to the upcoming zoomed phase
    if seq_index == 1:
        return 0
    if seq_index == 6:
        return 2
    return None




def extract_video_segments_dynamic(
    precomputed_segments,
    video_file,
    output_video_file,
    fps,
    output_width,
    output_height,
    zoom_schedule,
    centers_by_frame=None,
    face_smooth=0.2,
    y_offset=0.0,
    zoom_ramp_sec=0.0,
    zoom_time=0.0,
    zoom_rate=0.0,
    max_zoom=8.0,
    visualize=False,
    preview_scale=1.0,
    right_trigger=0.0,
    static_seconds=0.0,
    zoom_in=1.25,
    zoom_seconds=10.0,
    zoom_point=None,
    zoom_rects=None,
    no_trigger=False,
    loop_sequence=False,
    no_punct_schedule=False,
    subtitles=False,
    subtitle_group_words=None,
    subtitle_group_for_idx=None,
    subtitle_start_times=None,
    subtitle_end_times=None,
    subtitle_lines=1,
    font_scale=1.8,
    text_height=650,
    text_height_2=None,
    font_ttf=None,
    subtitle_bg=False,
    subtitle_bg_color=(0, 0, 0),
    subtitle_bg_alpha=0.0,
    subtitle_bg_pad=8,
    subtitle_bg_height=0,
    subtitle_bg_offset_y=0,
    shadow_size=4,
    shadow_offset_x=2,
    shadow_offset_y=2,
    subtitle_dilate=False,
    subtitle_dilate_size=10,
    subtitle_dilate_color=(0, 0, 0),
    subtitle_text_color=(80, 80, 255),
    subtitle_highlight_color=(0, 255, 255),
    subtitle_shadow_color=(0, 0, 0),
    subtitle_cache_max=64,
    subtitle_hold_ms=120.0,
    max_seconds=None,
    temp_codec="avc1",
    fade_frames=0,
    video_shift_frames=0,
    fallback_center=None,
    rotate_degrees=0.0,
):
    print("Extracting video segments (dynamic zoom, face tracking)...")

    cap = cv2.VideoCapture(video_file)
    if not cap.isOpened():
        print(f"Error: Could not open video file {video_file}")
        return False

    input_fps = cap.get(cv2.CAP_PROP_FPS)
    total_frames_source = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    source_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    source_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    input_width, input_height = get_rotated_dimensions(source_width, source_height, rotate_degrees)

    total_duration = total_frames_source / input_fps if input_fps else 0.0
    print(f"Input video: {input_width}x{input_height}, {input_fps:.2f} fps, duration {total_duration:.2f}s")
    print(f"Output video: {output_width}x{output_height}")

    out, _actual_codec = create_video_writer(
        output_video_file,
        fps,
        output_width,
        output_height,
        temp_codec,
    )
    if out is None:
        print(f"Error: Could not open video writer for {output_video_file}")
        cap.release()
        return False

    if fallback_center is None:
        fallback_center = (input_width / 2.0, input_height / 2.0)

    out_frame_idx = 0
    current_frame_idx = int(cap.get(cv2.CAP_PROP_POS_FRAMES) or 0)
    last_center = None
    last_source_center = None
    last_timeline_label = None
    last_sub_idx = None
    last_sub_time = None
    last_sub_idx = None
    last_sub_time = None
    seq_states = ["static", "zooming", "zoomed", "static", "zoomed"]
    seq_active = False
    seq_index = 0
    seq_end = 0.0
    zoomed_count = 0
    zoomed_count = 0
    zoomed_count = 0
    zoomed_count = 0
    if no_trigger:
        seq_active = True
        seq_index = 0
        seq_end = float(static_seconds or 0.0)
        print(f"[state] t=0.00s -> {format_state_label(seq_states[seq_index], zoom_rects, zoomed_count)}")
        zoomed_count = 0
    zooming_duration = float(zoom_time) if zoom_time and zoom_time > 0 else float(zoom_ramp_sec or 0.0)
    overlay_cache = {}
    mask_cache = {}
    bg_mask_cache = {}
    subtitle_cache_max = max(0, int(subtitle_cache_max))

    video_shift_frames = int(video_shift_frames)

    total_frames = sum(
        max(0, seg["end_frame"] - seg["start_frame"]) for seg in precomputed_segments
    )
    if max_seconds is not None and max_seconds > 0 and fps:
        total_frames = min(total_frames, int(round(max_seconds * fps)))
    pbar = tqdm(total=total_frames, desc="Writing frames")

    fade_frames = max(0, int(fade_frames))
    prev_tail = []

    for seg in precomputed_segments:
        seg_frame_idx = 0
        prev_tail_snapshot = list(prev_tail) if fade_frames > 0 else []
        for frame_idx in range(seg["start_frame"], seg["end_frame"]):
            if max_seconds is not None and max_seconds > 0 and fps:
                if out_frame_idx >= int(round(max_seconds * fps)):
                    pbar.close()
                    cap.release()
                    out.release()
                    return True
            target_idx = frame_idx + video_shift_frames
            if abs(target_idx - current_frame_idx) > 10:
                cap.set(cv2.CAP_PROP_POS_FRAMES, target_idx)
                current_frame_idx = target_idx

            ret, frame = cap.read()
            if not ret:
                break
            current_frame_idx += 1
            frame = rotate_frame(frame, rotate_degrees)

            if centers_by_frame:
                source_center = find_center_from_jsonl(
                    centers_by_frame,
                    frame_idx,
                    source_width,
                    source_height,
                    last_center=last_source_center,
                    smooth=face_smooth,
                    y_offset=y_offset,
                )
                last_source_center = source_center
                last_center = rotate_point(source_center, source_width, source_height, rotate_degrees)
            if last_center is None:
                last_center = fallback_center

            out_time = out_frame_idx / fps if fps else 0.0
            if zoom_rects_have_timeline(zoom_rects):
                authored = resolve_zoom_rect_state(
                    out_time,
                    fps,
                    input_width,
                    input_height,
                    zoom_rects,
                    zoom_time if zoom_time and zoom_time > 0 else zoom_ramp_sec,
                )
                center = authored["center"]
                current_zoom = authored["zoom"]
                current_rotate = authored["rotate"]
                if authored["label"] != last_timeline_label:
                    print(f"[state] t={out_time:.2f}s -> {authored['label']}")
                    last_timeline_label = authored["label"]
            elif right_trigger and not seq_active and not no_trigger:
                dist_from_right = 1.0 - (last_center[0] / float(input_width))
                if dist_from_right > float(right_trigger):
                    seq_active = True
                    seq_index = 0
                    seq_end = out_time + float(static_seconds or 0.0)
            if not zoom_rects_have_timeline(zoom_rects) and seq_active:
                if out_time >= seq_end:
                    seq_index += 1
                    if seq_index >= len(seq_states):
                        if loop_sequence:
                            seq_active = True
                            seq_index = 0
                            seq_end = out_time + float(static_seconds or 0.0)
                            print(
                                f"[state] t={out_time:.2f}s -> "
                                f"{format_transition_state_label(seq_states[seq_index], zoom_rects, zoomed_count)}"
                            )
                        elif no_trigger:
                            seq_active = True
                            seq_index = len(seq_states) - 1
                            seq_end = 1e18
                        else:
                            seq_active = False
                    else:
                        state = seq_states[seq_index]
                        print(
                            f"[state] t={out_time:.2f}s -> "
                            f"{format_transition_state_label(state, zoom_rects, zoomed_count)}"
                        )
                        if state == "static":
                            seq_end = out_time + float(static_seconds or 0.0)
                        elif state == "zooming":
                            seq_end = out_time + float(zooming_duration)
                        else:
                            seq_end = out_time + float(zoom_seconds or 0.0)
                            zoomed_count += 1
                if seq_active:
                    state = seq_states[seq_index]
                    if state == "static":
                        current_zoom = 1.0
                        center = (input_width / 2.0, input_height / 2.0)
                        current_rotate = 0.0
                    elif state == "zooming":
                        dur = max(0.001, float(zooming_duration))
                        t0 = seq_end - dur
                        alpha = max(0.0, min(1.0, (out_time - t0) / dur))
                        if zoom_rects:
                            phase_idx = min(max(zoomed_count, 0), len(zoom_rects) - 1)
                            zoom_target = float(zoom_rects[phase_idx].get("zoom", zoom_in))
                            target_center = (
                                float(zoom_rects[phase_idx].get("center_x", last_center[0])),
                                float(zoom_rects[phase_idx].get("center_y", last_center[1])),
                            )
                            target_rotate = get_zoom_rect_rotate(zoom_rects, phase_idx)
                            transition_mode = get_zoom_rect_transition(zoom_rects, phase_idx)
                        else:
                            zoom_target = float(max_zoom) if (max_zoom and max_zoom > 0) else float(zoom_in)
                            target_center = zoom_point if zoom_point is not None else last_center
                            target_rotate = 0.0
                            transition_mode = "pan_zoom"
                        if transition_mode == "jump":
                            center = target_center
                            current_zoom = zoom_target
                            current_rotate = target_rotate
                        else:
                            start_center = (input_width / 2.0, input_height / 2.0)
                            center = lerp_point(start_center, target_center, alpha)
                            current_zoom = lerp_scalar(1.0, zoom_target, alpha)
                            current_rotate = lerp_scalar(0.0, target_rotate, alpha)
                    else:
                        if zoom_rects:
                            phase_idx = min(max(zoomed_count - 1, 0), len(zoom_rects) - 1)
                            current_zoom = float(zoom_rects[phase_idx].get("zoom", zoom_in))
                            center = (
                                float(zoom_rects[phase_idx].get("center_x", last_center[0])),
                                float(zoom_rects[phase_idx].get("center_y", last_center[1])),
                            )
                            current_rotate = get_zoom_rect_rotate(zoom_rects, phase_idx)
                        else:
                            current_zoom = float(max_zoom) if (max_zoom and max_zoom > 0) else float(zoom_in)
                            center = zoom_point if zoom_point is not None else last_center
                            current_rotate = 0.0
                else:
                    current_zoom = 1.0 if no_punct_schedule else get_zoom_at_time(out_time, zoom_schedule, zooming_duration)
                    if zoom_rate:
                        current_zoom += float(zoom_rate) * out_time
                        current_zoom = max(0.05, current_zoom)
                    if max_zoom and max_zoom > 0:
                        current_zoom = min(float(max_zoom), current_zoom)
                    if zoom_point is not None and current_zoom > 1.0:
                        center = zoom_point
                    else:
                        center = last_center
                    current_rotate = 0.0
            elif not zoom_rects_have_timeline(zoom_rects):
                current_zoom = 1.0 if no_punct_schedule else get_zoom_at_time(out_time, zoom_schedule, zooming_duration)
                if zoom_rate:
                    current_zoom += float(zoom_rate) * out_time
                    current_zoom = max(0.05, current_zoom)
                if max_zoom and max_zoom > 0:
                    current_zoom = min(float(max_zoom), current_zoom)
                if zoom_point is not None and current_zoom > 1.0:
                    center = zoom_point
                else:
                    center = last_center
                current_rotate = 0.0

            x1, y1, x2, y2 = compute_crop_box(
                center,
                input_width,
                input_height,
                output_width,
                output_height,
                current_zoom,
            )
            cropped = frame[y1:y2, x1:x2]
            if cropped.size == 0:
                continue
            if cropped.shape[1] != output_width or cropped.shape[0] != output_height:
                cropped = cv2.resize(cropped, (output_width, output_height))
            cropped = rotate_output_frame(cropped, current_rotate)
            if subtitles and subtitle_group_words:
                if subtitle_cache_max == 0:
                    overlay_cache.clear()
                    mask_cache.clear()
                    bg_mask_cache.clear()
                elif subtitle_cache_max > 0 and len(overlay_cache) > subtitle_cache_max:
                    overlay_cache.clear()
                    mask_cache.clear()
                    bg_mask_cache.clear()
                t_sub = out_time
                active_idx = get_active_word_idx(
                    t_sub, subtitle_start_times, subtitle_end_times
                )
                if active_idx is None and last_sub_idx is not None and subtitle_hold_ms and last_sub_time is not None:
                    if (t_sub - last_sub_time) <= (float(subtitle_hold_ms) / 1000.0):
                        active_idx = last_sub_idx
                if active_idx is not None:
                    last_sub_idx = active_idx
                    last_sub_time = t_sub
                if active_idx is None and last_sub_idx is not None and subtitle_hold_ms and last_sub_time is not None:
                    if (t_sub - last_sub_time) <= (float(subtitle_hold_ms) / 1000.0):
                        active_idx = last_sub_idx
                if active_idx is not None:
                    last_sub_idx = active_idx
                    last_sub_time = t_sub
                if active_idx is not None:
                    gi, pos = subtitle_group_for_idx.get(active_idx, (None, None))
                    if gi is not None and gi < len(subtitle_group_words):
                        group_words = subtitle_group_words[gi]
                        cropped = apply_subtitles(
                            cropped,
                            (output_height, output_width, 3),
                            group_words,
                            pos,
                            overlay_cache,
                            mask_cache,
                            bg_mask_cache,
                            font_scale,
                            text_height,
                            text_height_2,
                            subtitle_lines,
                            font_ttf,
                            subtitle_bg,
                            subtitle_bg_color,
                            subtitle_bg_alpha,
                            subtitle_bg_pad,
                            subtitle_bg_height,
                            subtitle_bg_offset_y,
                            shadow_size,
                            shadow_offset_x,
                            shadow_offset_y,
                            subtitle_dilate,
                            subtitle_dilate_size,
                            subtitle_dilate_color,
                            subtitle_text_color,
                            subtitle_highlight_color,
                            subtitle_shadow_color,
                        )

            if fade_frames > 0 and prev_tail_snapshot and seg_frame_idx < fade_frames:
                prev_idx = min(seg_frame_idx, len(prev_tail_snapshot) - 1)
                alpha = (seg_frame_idx + 1) / float(fade_frames + 1)
                blended = cv2.addWeighted(prev_tail_snapshot[prev_idx], 1.0 - alpha, cropped, alpha, 0)
                out.write(blended)
            else:
                out.write(cropped)
            if fade_frames > 0:
                if len(prev_tail) >= fade_frames:
                    prev_tail.pop(0)
                prev_tail.append(cropped.copy())

            if visualize:
                preview = cropped.copy()
                cv2.putText(
                    preview,
                    f"t={out_time:.2f}s zoom={current_zoom:.2f}",
                    (20, 40),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    1.0,
                    (255, 255, 255),
                    2,
                    cv2.LINE_AA,
                )
                if preview_scale and preview_scale > 0 and preview_scale != 1.0:
                    h, w = preview.shape[:2]
                    preview = cv2.resize(
                        preview,
                        (max(1, int(w * preview_scale)), max(1, int(h * preview_scale))),
                        interpolation=cv2.INTER_AREA,
                    )
                cv2.imshow("Dynamic Preview", preview)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    pbar.close()
                    cap.release()
                    out.release()
                    cv2.destroyAllWindows()
                    return True

            out_frame_idx += 1
            seg_frame_idx += 1
            pbar.update(1)

    pbar.close()
    cap.release()
    out.release()
    return True


def extract_video_dynamic_full(
    video_file,
    output_video_file,
    fps,
    output_width,
    output_height,
    zoom_schedule,
    centers_by_frame=None,
    face_smooth=0.2,
    y_offset=0.0,
    zoom_ramp_sec=0.0,
    zoom_time=0.0,
    zoom_rate=0.0,
    max_zoom=8.0,
    visualize=False,
    preview_scale=1.0,
    right_trigger=0.0,
    static_seconds=0.0,
    zoom_in=1.25,
    zoom_seconds=10.0,
    zoom_point=None,
    zoom_rects=None,
    no_trigger=False,
    loop_sequence=False,
    no_punct_schedule=False,
    subtitles=False,
    subtitle_group_words=None,
    subtitle_group_for_idx=None,
    subtitle_start_times=None,
    subtitle_end_times=None,
    subtitle_lines=1,
    font_scale=1.8,
    text_height=650,
    text_height_2=None,
    font_ttf=None,
    subtitle_bg=False,
    subtitle_bg_color=(0, 0, 0),
    subtitle_bg_alpha=0.0,
    subtitle_bg_pad=8,
    subtitle_bg_height=0,
    subtitle_bg_offset_y=0,
    shadow_size=4,
    shadow_offset_x=2,
    shadow_offset_y=2,
    subtitle_dilate=False,
    subtitle_dilate_size=10,
    subtitle_dilate_color=(0, 0, 0),
    subtitle_text_color=(80, 80, 255),
    subtitle_highlight_color=(0, 255, 255),
    subtitle_shadow_color=(0, 0, 0),
    subtitle_cache_max=64,
    subtitle_hold_ms=120.0,
    max_seconds=None,
    temp_codec="avc1",
    rotate_degrees=0.0,
):
    print("Extracting full video (dynamic zoom, face tracking)...")

    cap = cv2.VideoCapture(video_file)
    if not cap.isOpened():
        print(f"Error: Could not open video file {video_file}")
        return False

    input_fps = cap.get(cv2.CAP_PROP_FPS)
    source_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    source_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    input_width, input_height = get_rotated_dimensions(source_width, source_height, rotate_degrees)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    if max_seconds is not None and max_seconds > 0 and fps:
        total_frames = min(total_frames, int(round(max_seconds * fps)))

    total_duration = total_frames / fps if fps else 0.0
    print(f"Input video: {input_width}x{input_height}, {input_fps:.2f} fps, duration {total_duration:.2f}s")
    print(f"Output video: {output_width}x{output_height}")

    out, _actual_codec = create_video_writer(
        output_video_file,
        fps,
        output_width,
        output_height,
        temp_codec,
    )
    if out is None:
        print(f"Error: Could not open video writer for {output_video_file}")
        cap.release()
        return False

    last_center = None
    last_source_center = None
    last_timeline_label = None
    seq_states = ["static", "zooming", "zoomed", "static", "zoomed", "static", "zooming", "zoomed"]
    seq_active = False
    seq_index = 0
    seq_end = 0.0
    zoomed_count = 0
    if no_trigger:
        seq_active = True
        seq_index = 0
        seq_end = float(static_seconds or 0.0)
        print(f"[state] t=0.00s -> {format_state_label(seq_states[seq_index], zoom_rects, zoomed_count)}")
    zooming_duration = float(zoom_time) if zoom_time and zoom_time > 0 else float(zoom_ramp_sec or 0.0)
    overlay_cache = {}
    mask_cache = {}
    bg_mask_cache = {}
    pbar = tqdm(total=total_frames, desc="Writing frames")
    frame_idx = 0

    while True:
        if max_seconds is not None and max_seconds > 0 and fps:
            if frame_idx >= int(round(max_seconds * fps)):
                break
        ret, frame = cap.read()
        if not ret:
            break
        frame = rotate_frame(frame, rotate_degrees)

        if centers_by_frame:
            source_center = find_center_from_jsonl(
                centers_by_frame,
                frame_idx,
                source_width,
                source_height,
                last_center=last_source_center,
                smooth=face_smooth,
                y_offset=y_offset,
            )
            last_source_center = source_center
            last_center = rotate_point(source_center, source_width, source_height, rotate_degrees)
        if last_center is None:
            last_center = (input_width / 2.0, input_height / 2.0)

        t = frame_idx / fps if fps else 0.0
        if zoom_rects_have_timeline(zoom_rects):
            authored = resolve_zoom_rect_state(
                t,
                fps,
                input_width,
                input_height,
                zoom_rects,
                zoom_time if zoom_time and zoom_time > 0 else zoom_ramp_sec,
            )
            center = authored["center"]
            current_zoom = authored["zoom"]
            current_rotate = authored["rotate"]
            if authored["label"] != last_timeline_label:
                print(f"[state] t={t:.2f}s -> {authored['label']}")
                last_timeline_label = authored["label"]
        elif right_trigger and not seq_active and not no_trigger:
            dist_from_right = 1.0 - (last_center[0] / float(input_width))
            if dist_from_right > float(right_trigger):
                seq_active = True
                seq_index = 0
                seq_end = t + float(static_seconds or 0.0)

        if not zoom_rects_have_timeline(zoom_rects) and seq_active:
            if t >= seq_end:
                seq_index += 1
                if seq_index >= len(seq_states):
                    if loop_sequence:
                        seq_active = True
                        seq_index = 0
                        seq_end = t + float(static_seconds or 0.0)
                        print(
                            f"[state] t={t:.2f}s -> "
                            f"{format_transition_state_label(seq_states[seq_index], zoom_rects, zoomed_count)}"
                        )
                    elif no_trigger:
                        seq_active = True
                        seq_index = len(seq_states) - 1
                        seq_end = 1e18
                    else:
                        seq_active = False
                else:
                    state = seq_states[seq_index]
                    print(
                        f"[state] t={t:.2f}s -> "
                        f"{format_transition_state_label(state, zoom_rects, zoomed_count)}"
                    )
                    if state == "static":
                        seq_end = t + float(static_seconds or 0.0)
                    elif state == "zooming":
                        seq_end = t + float(zooming_duration)
                    else:
                        seq_end = t + float(zoom_seconds or 0.0)
                        zoomed_count += 1
            if seq_active:
                state = seq_states[seq_index]
                if state == "static":
                    current_zoom = 1.0
                    center = (input_width / 2.0, input_height / 2.0)
                    current_rotate = 0.0
                elif state == "zooming":
                    dur = max(0.001, float(zooming_duration))
                    t0 = seq_end - dur
                    alpha = max(0.0, min(1.0, (t - t0) / dur))
                    if zoom_rects:
                        phase_idx = min(max(zoomed_count, 0), len(zoom_rects) - 1)
                        zoom_target = float(zoom_rects[phase_idx].get("zoom", zoom_in))
                        target_center = (
                            float(zoom_rects[phase_idx].get("center_x", last_center[0])),
                            float(zoom_rects[phase_idx].get("center_y", last_center[1])),
                        )
                        target_rotate = get_zoom_rect_rotate(zoom_rects, phase_idx)
                        transition_mode = get_zoom_rect_transition(zoom_rects, phase_idx)
                    else:
                        zoom_target = float(max_zoom) if (max_zoom and max_zoom > 0) else float(zoom_in)
                        target_center = zoom_point if zoom_point is not None else last_center
                        target_rotate = 0.0
                        transition_mode = "pan_zoom"
                    if transition_mode == "jump":
                        center = target_center
                        current_zoom = zoom_target
                        current_rotate = target_rotate
                    else:
                        start_center = (input_width / 2.0, input_height / 2.0)
                        center = lerp_point(start_center, target_center, alpha)
                        current_zoom = lerp_scalar(1.0, zoom_target, alpha)
                        current_rotate = lerp_scalar(0.0, target_rotate, alpha)
                else:
                    if zoom_rects:
                        phase_idx = min(max(zoomed_count - 1, 0), len(zoom_rects) - 1)
                        current_zoom = float(zoom_rects[phase_idx].get("zoom", zoom_in))
                        center = (
                            float(zoom_rects[phase_idx].get("center_x", last_center[0])),
                            float(zoom_rects[phase_idx].get("center_y", last_center[1])),
                        )
                        current_rotate = get_zoom_rect_rotate(zoom_rects, phase_idx)
                    else:
                        current_zoom = float(max_zoom) if (max_zoom and max_zoom > 0) else float(zoom_in)
                        center = zoom_point if zoom_point is not None else last_center
                        current_rotate = 0.0
            else:
                current_zoom = 1.0 if no_punct_schedule else get_zoom_at_time(t, zoom_schedule, zooming_duration)
                if zoom_rate:
                    current_zoom += float(zoom_rate) * t
                    current_zoom = max(0.05, current_zoom)
                if max_zoom and max_zoom > 0:
                    current_zoom = min(float(max_zoom), current_zoom)
                if zoom_point is not None and current_zoom > 1.0:
                    center = zoom_point
                else:
                    center = last_center
                current_rotate = 0.0
        elif not zoom_rects_have_timeline(zoom_rects):
            current_zoom = 1.0 if no_punct_schedule else get_zoom_at_time(t, zoom_schedule, zooming_duration)
            if zoom_rate:
                current_zoom += float(zoom_rate) * t
                current_zoom = max(0.05, current_zoom)
            if max_zoom and max_zoom > 0:
                current_zoom = min(float(max_zoom), current_zoom)
            if zoom_point is not None and current_zoom > 1.0:
                center = zoom_point
            else:
                center = last_center
            current_rotate = 0.0

        x1, y1, x2, y2 = compute_crop_box(
            center,
            input_width,
            input_height,
            output_width,
            output_height,
            current_zoom,
        )
        cropped = frame[y1:y2, x1:x2]
        if cropped.size != 0:
            if cropped.shape[1] != output_width or cropped.shape[0] != output_height:
                cropped = cv2.resize(cropped, (output_width, output_height))
            cropped = rotate_output_frame(cropped, current_rotate)
            if subtitles and subtitle_group_words:
                if subtitle_cache_max == 0:
                    overlay_cache.clear()
                    mask_cache.clear()
                    bg_mask_cache.clear()
                elif subtitle_cache_max > 0 and len(overlay_cache) > subtitle_cache_max:
                    overlay_cache.clear()
                    mask_cache.clear()
                    bg_mask_cache.clear()
                t_sub = t
                active_idx = get_active_word_idx(
                    t_sub, subtitle_start_times, subtitle_end_times
                )
                if active_idx is not None:
                    gi, pos = subtitle_group_for_idx.get(active_idx, (None, None))
                    if gi is not None and gi < len(subtitle_group_words):
                        group_words = subtitle_group_words[gi]
                        cropped = apply_subtitles(
                            cropped,
                            (output_height, output_width, 3),
                            group_words,
                            pos,
                            overlay_cache,
                            mask_cache,
                            bg_mask_cache,
                            font_scale,
                            text_height,
                            text_height_2,
                            subtitle_lines,
                            font_ttf,
                            subtitle_bg,
                            subtitle_bg_color,
                            subtitle_bg_alpha,
                            subtitle_bg_pad,
                            subtitle_bg_height,
                            subtitle_bg_offset_y,
                            shadow_size,
                            shadow_offset_x,
                            shadow_offset_y,
                            subtitle_dilate,
                            subtitle_dilate_size,
                            subtitle_dilate_color,
                            subtitle_text_color,
                            subtitle_highlight_color,
                            subtitle_shadow_color,
                        )

            out.write(cropped)
            if visualize:
                preview = cropped.copy()
                cv2.putText(
                    preview,
                    f"t={t:.2f}s zoom={current_zoom:.2f}",
                    (20, 40),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    1.0,
                    (255, 255, 255),
                    2,
                    cv2.LINE_AA,
                )
                if preview_scale and preview_scale > 0 and preview_scale != 1.0:
                    h, w = preview.shape[:2]
                    preview = cv2.resize(
                        preview,
                        (max(1, int(w * preview_scale)), max(1, int(h * preview_scale))),
                        interpolation=cv2.INTER_AREA,
                    )
                cv2.imshow("Dynamic Preview", preview)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    pbar.close()
                    cap.release()
                    out.release()
                    cv2.destroyAllWindows()
                    return True

        frame_idx += 1
        pbar.update(1)

    pbar.close()
    cap.release()
    out.release()
    if visualize:
        cv2.destroyAllWindows()
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Extract video/audio segments with face tracking and dynamic zoom (no subtitles)"
    )
    parser.add_argument("--video", type=str, required=True, help="Input video file")
    parser.add_argument("--output", type=str, default=None, help="Output video file")
    parser.add_argument(
        "--csv",
        type=str,
        default="",
        help="Path to a CSV file with word timings (speaker,start,end,word). Skips JSON.",
    )
    parser.add_argument(
        "--use-csv",
        action="store_true",
        help="Use JSON-derived CSV for word timings (create if missing)",
    )
    parser.add_argument(
        "--width",
        type=int,
        default=1080,
        help="Output width in pixels (default: 1080)",
    )
    parser.add_argument(
        "--height",
        type=int,
        default=1920,
        help="Output height in pixels (default: 1920)",
    )
    parser.add_argument(
        "--rotate",
        type=float,
        default=0.0,
        help="Rotate frames counterclockwise by this many degrees before cropping; multiples of 90 use optimized rotation.",
    )
    parser.add_argument(
        "--zoom-in",
        type=float,
        default=1.25,
        help="Zoom factor for zoomed-in segments (default: 1.25)",
    )
    parser.add_argument(
        "--zoom-out",
        type=float,
        default=1.0,
        help="Zoom factor for zoomed-out segments (default: 1.0)",
    )
    parser.add_argument(
        "--zoom-seconds",
        type=float,
        default=10.0,
        help="Target duration for each zoom segment (default: 10s)",
    )
    parser.add_argument(
        "--subtitles",
        action="store_true",
        help="Enable subtitle rendering (default: off)",
    )
    parser.add_argument(
        "--no-subtitles",
        dest="subtitles",
        action="store_false",
        help="Disable subtitle rendering",
    )
    parser.set_defaults(subtitles=False)
    parser.add_argument(
        "--subtitle-lines",
        type=int,
        default=1,
        help="Number of subtitle lines (default: 1)",
    )
    parser.add_argument(
        "--group-size",
        type=int,
        default=6,
        help="Number of words per subtitle group (default: 6)",
    )
    parser.add_argument(
        "--max-chars",
        type=int,
        default=0,
        help="Max characters per subtitle line (0 = no limit).",
    )
    parser.add_argument(
        "--font-file",
        type=str,
        default="nofile.ttf",
        help="TTF font file to use for subtitles (default: nofile.ttf)",
    )
    parser.add_argument(
        "--font-size",
        type=int,
        default=50,
        help="TTF font size in pixels (default: 50)",
    )
    parser.add_argument(
        "--font-scale",
        type=float,
        default=1.8,
        help="Font scale multiplier (default: 1.8)",
    )
    parser.add_argument(
        "--text-height",
        type=int,
        default=650,
        help="Height of text from bottom (default: 650)",
    )
    parser.add_argument(
        "--subtitle-height",
        type=float,
        default=0.2,
        help="Subtitle height as fraction of output height (overrides --text-height when > 0).",
    )
    parser.add_argument(
        "--subtitle-text-color",
        type=str,
        default="80,80,255",
        help="Subtitle text color as B,G,R (default: 80,80,255)",
    )
    parser.add_argument(
        "--subtitle-highlight-color",
        type=str,
        default="0,255,255",
        help="Subtitle highlight color as B,G,R (default: 0,255,255)",
    )
    parser.add_argument(
        "--subtitle-cache-max",
        type=int,
        default=64,
        help="Max subtitle overlay cache entries (default: 64; 0 disables caching).",
    )
    parser.add_argument(
        "--subtitle-hold-ms",
        type=float,
        default=120.0,
        help="Hold last subtitle highlight for this many ms to reduce flicker (default: 120).",
    )
    parser.add_argument(
        "--subtitle-shadow-color",
        type=str,
        default="0,0,0",
        help="Subtitle shadow color as B,G,R (default: 0,0,0)",
    )
    parser.add_argument(
        "--shadow-size",
        type=int,
        default=4,
        help="Subtitle shadow offset size in pixels (default: 4)",
    )
    parser.add_argument(
        "--shadow-offset-x",
        type=int,
        default=2,
        help="Subtitle shadow X offset in pixels (default: 2)",
    )
    parser.add_argument(
        "--shadow-offset-y",
        type=int,
        default=2,
        help="Subtitle shadow Y offset in pixels (default: 2)",
    )
    parser.add_argument(
        "--subtitle-dilate",
        action="store_true",
        default=False,
        help="Use dilated outline instead of shadow (default: off)",
    )
    parser.add_argument(
        "--subtitle-dilate-size",
        type=int,
        default=30,
        help="Dilate pixel radius for subtitle outline (default: 10).",
    )
    parser.add_argument(
        "--subtitle-dilate-color",
        type=str,
        default="0,0,0",
        help="Dilate color as B,G,R (default: 0,0,0).",
    )
    parser.add_argument(
        "--subtitle-bg",
        action="store_true",
        help="Draw a background rectangle behind subtitles",
    )
    parser.add_argument(
        "--subtitle-bg-color",
        type=str,
        default="0,0,0",
        help="Background color as B,G,R (default: 0,0,0)",
    )
    parser.add_argument(
        "--subtitle-bg-alpha",
        type=float,
        default=0.0,
        help="Background opacity 0-1 (default: 0.0)",
    )
    parser.add_argument(
        "--subtitle-bg-height",
        type=int,
        default=0,
        help="Background height in pixels from bottom (default: 0 = auto)",
    )
    parser.add_argument(
        "--subtitle-bg-offset-y",
        type=int,
        default=0,
        help="Background vertical offset in pixels (default: 0)",
    )
    parser.add_argument(
        "--subtitle-bg-pad",
        type=int,
        default=8,
        help="Background padding in pixels (default: 8)",
    )
    parser.add_argument(
        "--align-window",
        type=float,
        default=3.0,
        help="Max seconds away from target to snap to punctuation (default: 3s)",
    )
    parser.add_argument(
        "--zoom-ramp",
        type=float,
        default=0.0,
        help="Seconds to ramp between zoom levels (default: 0, instant)",
    )
    parser.add_argument(
        "--zoom-time",
        type=float,
        default=0.0,
        help="Seconds to reach max zoom during zooming state (overrides --zoom-ramp when > 0).",
    )
    parser.add_argument(
        "--zoom-rate",
        type=float,
        default=0.0,
        help="Zoom change per second (positive=in, negative=out).",
    )
    parser.add_argument(
        "--min-segment",
        type=float,
        default=2.0,
        help="Minimum duration for a zoom segment in seconds (default: 2s)",
    )
    parser.add_argument(
        "--start-zoom-in",
        action="store_true",
        default=True,
        help="Start with zoom-in (default: true)",
    )
    parser.add_argument(
        "--start-zoom-out",
        dest="start_zoom_in",
        action="store_false",
        help="Start with zoom-out",
    )
    parser.add_argument(
        "--face-json",
        type=str,
        default="",
        help="Path to face tracking JSONL with per-frame centers",
    )
    parser.add_argument(
        "--run-stabilize",
        dest="run_stabilize",
        action="store_true",
        default=True,
        help="Run stabilize.py to build tracking JSONL (default: on)",
    )
    parser.add_argument(
        "--no-stabilize",
        dest="run_stabilize",
        action="store_false",
        help="Disable stabilize.py pre-pass",
    )
    parser.add_argument(
        "--stabilize-instance",
        type=str,
        default="",
        help="Instance id to pass to stabilize.py (default: stabilize.py default)",
    )
    parser.add_argument(
        "--stabilize-select-interval",
        type=int,
        default=0,
        help="Frames between stabilize.py re-selection prompts (0 = use stabilize default)",
    )
    parser.add_argument(
        "--stabilize-time-shift",
        type=int,
        default=0,
        help="Shift stabilize tracking by N frames (positive=earlier, negative=later).",
    )
    parser.add_argument(
        "--visualize",
        action="store_true",
        help="Enable dynamic preview (and stabilize visualization if stabilize is run)",
    )
    parser.add_argument(
        "--preview-scale",
        type=float,
        default=1.0,
        help="Scale factor for preview window (default: 1.0)",
    )
    parser.add_argument(
        "--face-smooth",
        type=float,
        default=0.2,
        help="Face center smoothing factor 0-1 (default: 0.2)",
    )
    parser.add_argument(
        "--y-offset",
        type=float,
        default=0.0,
        help="Vertical offset added to face center as fraction of height (default: 0.0)",
    )
    parser.add_argument(
        "--max-zoom",
        type=float,
        default=8.0,
        help="Maximum allowed zoom (default: 8.0)",
    )
    parser.add_argument(
        "--right-trigger",
        type=float,
        default=0.0,
        help="Trigger static when (1 - center_x_norm) exceeds this threshold (default: 0 = off).",
    )
    parser.add_argument(
        "--no-trigger",
        action="store_true",
        help="Disable right-trigger sequence entirely.",
    )
    parser.add_argument(
        "--static-seconds",
        type=float,
        default=10.0,
        help="Seconds to hold full-screen static after right-trigger (default: 10).",
    )
    parser.add_argument(
        "--fade-frames",
        type=int,
        default=6,
        help="Crossfade length for video stitching (default: 6, unused but kept for parity)",
    )
    parser.add_argument(
        "--fade-samples",
        type=int,
        default=100,
        help="Crossfade length for audio stitching in samples (default: 100)",
    )
    parser.add_argument(
        "--postpend-silence-seconds",
        type=float,
        default=0.5,
        help="Seconds of silence to postpend to temp audio (default: 0.5)",
    )
    parser.add_argument(
        "--postpend-silence",
        dest="postpend_silence",
        action="store_true",
        default=True,
        help="Postpend silence to the temp audio file (default: on)",
    )
    parser.add_argument(
        "--no-postpend-silence",
        dest="postpend_silence",
        action="store_false",
        help="Disable postpending silence to the temp audio file",
    )
    parser.add_argument(
        "--shift-seconds",
        type=float,
        default=0.15,
        help="Shift all word timings by this many seconds (default: 0.15). Deprecated; use --subtitles-delay-ms.",
    )
    parser.add_argument(
        "--subtitles-delay-ms",
        type=float,
        default=None,
        help="Shift subtitle word timings by this many ms (positive=delay, negative=advance).",
    )
    parser.add_argument(
        "--preserve-word-gaps",
        action="store_true",
        help="Disable timing adjustments that reduce gaps between words",
    )
    parser.add_argument(
        "--extend-ms",
        type=int,
        default=20,
        help="Extend each segment end by this many ms (default: 20)",
    )
    parser.add_argument(
        "--min-caption-gap",
        type=float,
        default=1.0,
        help="Minimum gap between captions in seconds (default: 1.0). Gaps below this are closed by extending the previous caption.",
    )
    parser.add_argument(
        "--prepend-ms",
        type=int,
        default=20,
        help="Extend each segment start earlier by this many ms (default: 20)",
    )
    parser.add_argument(
        "--audio-delay-ms",
        type=float,
        default=0.0,
        help="Shift all audio timings by this many ms (positive=delay)",
    )
    parser.add_argument(
        "--audio-only-delay-ms",
        type=float,
        default=0.0,
        help="Delay audio only by this many ms (positive=delay, negative=advance).",
    )
    parser.add_argument(
        "--temp-codec",
        type=str,
        default="avc1",
        help="Preferred FourCC for temp video encoding (default: avc1; falls back automatically).",
    )
    parser.add_argument(
        "--video-shift-frames",
        type=int,
        default=0,
        help="Shift video segments by N frames relative to audio (positive=advance, negative=delay)",
    )
    parser.add_argument(
        "--skip-video",
        action="store_true",
        help="Skip video processing",
    )
    parser.add_argument(
        "--skip-audio",
        action="store_true",
        help="Skip audio processing",
    )
    parser.add_argument(
        "--no-cut",
        action="store_true",
        help="Do not extract segments; apply dynamics to the full video",
    )
    parser.add_argument(
        "--cut",
        dest="force_cut",
        action="store_true",
        help="Force segment extraction (override --no-cut or standard-cut defaults).",
    )
    parser.add_argument(
        "--baltimoretechmeetup",
        action="store_true",
        help="Apply Baltimore Tech Meetup defaults.",
    )
    parser.add_argument(
        "--standard-cut",
        action="store_true",
        help="Apply standard dynamic cut defaults.",
    )
    parser.add_argument(
        "--loop-sequence",
        action="store_true",
        help="Loop the state sequence continuously (default: off).",
    )
    parser.add_argument(
        "--no-punct-schedule",
        action="store_true",
        help="Disable punctuation-based zoom schedule (state machine only).",
    )
    parser.add_argument(
        "--max-seconds",
        type=float,
        default=0.0,
        help="Limit output to the first N seconds (0 = full length).",
    )
    parser.add_argument(
        "--select-zoom-point",
        action="store_true",
        help="Select zoom rectangles (center + size) from a frame halfway through the video.",
    )

    args = parser.parse_args()
    explicit_subtitles = ("--subtitles" in sys.argv) or ("--no-subtitles" in sys.argv)
    argv_tokens = sys.argv[1:]

    def _arg_provided(*names):
        for name in names:
            if name in argv_tokens:
                return True
            prefix = name + "="
            for tok in argv_tokens:
                if tok.startswith(prefix):
                    return True
        return False

    if args.baltimoretechmeetup:
        args.no_cut = True
        args.zoom_ramp = 4
        args.preview_scale = 0.5
        args.width = 1920
        args.height = 1080
        args.zoom_rate = 0.1
        args.y_offset = 0.05
        args.stabilize_time_shift = -5
        args.max_zoom = 3
        args.right_trigger = 0.1
        args.static_seconds = 10
        args.subtitles = True
        args.max_chars = 40
        args.subtitle_lines = 2
        args.subtitle_height = 0.2
        args.subtitle_dilate = True
        args.subtitle_dilate_size = 14
        args.font_scale = 1.5
    if args.standard_cut:
        if not _arg_provided("--no-cut"):
            args.no_cut = True
        if not _arg_provided("--zoom-time"):
            args.zoom_time = 15
        if not _arg_provided("--subtitle-height"):
            args.subtitle_height = 0.15
        if not _arg_provided("--width"):
            args.width = 1920
        if not _arg_provided("--height"):
            args.height = 1080
        if not _arg_provided("--max-zoom"):
            args.max_zoom = 6
        if not _arg_provided("--font-scale"):
            args.font_scale = 1.3
        if not _arg_provided("--right-trigger"):
            args.right_trigger = 0.1
        if not _arg_provided("--static-seconds"):
            args.static_seconds = 10
        if not _arg_provided("--no-trigger"):
            args.no_trigger = True
        if not _arg_provided("--max-chars"):
            args.max_chars = 40
        if not _arg_provided("--subtitle-lines"):
            args.subtitle_lines = 2
        if not explicit_subtitles and not _arg_provided("--subtitles"):
            args.subtitles = False
        if not _arg_provided("--select-zoom-point"):
            args.select_zoom_point = True
        if not _arg_provided("--loop-sequence"):
            args.loop_sequence = True
        if not _arg_provided("--no-punct-schedule"):
            args.no_punct_schedule = True
        if not _arg_provided("--preview-scale"):
            args.preview_scale = 0.5
        if not _arg_provided("--stabilize-select-interval"):
            args.stabilize_select_interval = 800
        if not _arg_provided("--y-offset"):
            args.y_offset = 0.05
        if not _arg_provided("--stabilize-time-shift"):
            args.stabilize_time_shift = -5
        if not _arg_provided("--subtitle-dilate"):
            args.subtitle_dilate = True
        if not _arg_provided("--subtitle-dilate-size"):
            args.subtitle_dilate_size = 10
    if args.force_cut:
        args.no_cut = False

    # Load CSV/JSON data
    print("Loading segment data...")
    basefilename = os.path.splitext(args.video)[0]
    json_base = strip_standard_suffixes(basefilename)
    jsonfilename = json_base + ".json"
    audiofilename = json_base + ".wav"
    notch_csv = basefilename + "_notch.csv"

    csvfilename = jsonfilename + ".csv"
    used_csv_path = None
    if args.csv:
        used_csv_path = args.csv
        print(f"Using CSV timings: {args.csv}")
        word_segment_times = load_word_segments_from_csv(args.csv)
    elif os.path.exists(csvfilename) or args.use_csv:
        used_csv_path = csvfilename
        if os.path.exists(csvfilename):
            print(f"Using CSV timings: {csvfilename}")
        else:
            print(f"CSV missing, creating from JSON: {jsonfilename}")
        if not os.path.exists(csvfilename) and os.path.exists(jsonfilename):
            transcriptJson2csv.export_words_to_csv(jsonfilename, source_video=args.video)
        word_segment_times = load_word_segments_from_csv(csvfilename)
    elif os.path.exists(jsonfilename):
        print(f"Using JSON timings: {jsonfilename}")
        transcriptJson2csv.export_words_to_csv(jsonfilename, source_video=args.video)
        word_segment_times = segment_utils.load_word_segments_from_json(jsonfilename)
    else:
        print(f"Using JSON timings (missing, attempting anyway): {jsonfilename}")
        word_segment_times = segment_utils.load_word_segments_from_json(jsonfilename)

    sr = segment_utils.get_audio_sample_rate(audiofilename)
    if args.subtitles_delay_ms is not None:
        args.shift_seconds = float(args.subtitles_delay_ms) / 1000.0
    if args.preserve_word_gaps:
        args.shift_seconds = 0.0
        args.extend_ms = 0
        args.prepend_ms = 0
    word_segment_times = segment_utils.align_segments_to_sample_boundaries(
        word_segment_times, sr
    )
    word_segment_times = segment_utils.fix_zero_length_words(
        word_segment_times, sr, shift_seconds=args.shift_seconds
    )
    word_segment_times = segment_utils.extend_segments(
        word_segment_times, extend_ms=args.extend_ms
    )
    word_segment_times = segment_utils.prepend_segments(
        word_segment_times, prepend_ms=args.prepend_ms
    )
    if args.min_caption_gap is not None:
        word_segment_times = enforce_min_caption_gap(
            word_segment_times, args.min_caption_gap
        )
    if args.max_seconds and args.max_seconds > 0:
        limit = float(args.max_seconds)
        trimmed = []
        for seg in word_segment_times:
            start = float(seg.get("start", 0.0))
            end = float(seg.get("end", start))
            if start >= limit:
                break
            if end > limit:
                end = limit
            curr = dict(seg)
            curr["end"] = max(start, end)
            trimmed.append(curr)
        word_segment_times = trimmed
    if args.audio_delay_ms:
        delay_sec = float(args.audio_delay_ms) / 1000.0
        shifted = []
        for seg in word_segment_times:
            curr = dict(seg)
            start = float(curr.get("start", 0.0)) + delay_sec
            end = float(curr.get("end", start)) + delay_sec
            if end <= 0:
                continue
            if start < 0:
                start = 0.0
            curr["start"] = start
            curr["end"] = max(start, end)
            shifted.append(curr)
        word_segment_times = segment_utils.align_segments_to_sample_boundaries(
            shifted, sr
        )
        print(f"Applied audio delay: {args.audio_delay_ms:.1f} ms")
    word_segment_times_for_cut = word_segment_times

    notch_ranges = []
    cut_ranges = None
    cut_total = None
    if os.path.exists(notch_csv):
        notch_ranges = _read_notch_csv(notch_csv)
        if notch_ranges:
            print(f"Loaded notch ranges from: {notch_csv}")
    else:
        print(f"No notch file found at: {notch_csv}")
        notch_ranges = _prompt_notch_ranges_gui(notch_csv)
        if notch_ranges is None:
            notch_ranges = _prompt_notch_ranges_cli()
            if notch_ranges:
                _write_notch_csv(notch_csv, notch_ranges)
        if notch_ranges:
            print(f"Saved notch ranges to: {notch_csv}")
    if notch_ranges:
        cap = cv2.VideoCapture(args.video)
        if cap.isOpened():
            fps_for_cut = cap.get(cv2.CAP_PROP_FPS) or 30.0
            frames_for_cut = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
            cap.release()
            video_duration_for_cut = frames_for_cut / fps_for_cut if fps_for_cut else 0.0
        else:
            video_duration_for_cut = float(max((seg.get("end", 0.0) for seg in word_segment_times), default=0.0))
        if args.max_seconds and args.max_seconds > 0:
            video_duration_for_cut = min(video_duration_for_cut, float(args.max_seconds))
        cut_ranges = _build_keep_ranges(video_duration_for_cut, notch_ranges)
        cut_total = sum(max(0.0, e - s) for s, e in cut_ranges)
        print(f"Notch ranges detected; will remove sections from output ({cut_total:.2f}s kept).")
        word_segment_times = _retime_segments_for_notches(word_segment_times, notch_ranges)
    elif not args.no_cut:
        word_segment_times = _retime_segments_for_word_cuts(word_segment_times)

    word_segment_times_for_subs = word_segment_times

    subtitle_group_words = None
    subtitle_group_for_idx = None
    subtitle_start_times = None
    subtitle_end_times = None
    font_ttf = None
    subtitle_text_color = np.array([80, 80, 255]) * 0.5
    subtitle_highlight_color = np.array([0, 255, 255]) * 0.5
    subtitle_shadow_color = (0, 0, 0)
    subtitle_bg_color = (0, 0, 0)
    subtitle_dilate_color = (0, 0, 0)
    if args.subtitles:
        max_chars = args.max_chars if args.max_chars and args.max_chars > 0 else None
        subtitle_group_words, subtitle_group_for_idx, subtitle_start_times, subtitle_end_times = (
            build_subtitle_context(
                word_segment_times_for_subs,
                group_size=args.group_size,
                max_chars=max_chars,
                subtitle_lines=args.subtitle_lines,
            )
        )
        if args.font_file:
            if not os.path.exists(args.font_file):
                raise FileNotFoundError(f"Font file not found: {args.font_file}")
            effective_size = max(1, int(round(args.font_size * args.font_scale)))
            font_ttf = ImageFont.truetype(args.font_file, effective_size)

        subtitle_text_color = parse_bgr(args.subtitle_text_color, "--subtitle-text-color")
        subtitle_highlight_color = parse_bgr(
            args.subtitle_highlight_color, "--subtitle-highlight-color"
        )
        subtitle_shadow_color = parse_bgr(
            args.subtitle_shadow_color, "--subtitle-shadow-color"
        )
        subtitle_bg_color = parse_bgr(args.subtitle_bg_color, "--subtitle-bg-color")
        subtitle_dilate_color = parse_bgr(
            args.subtitle_dilate_color, "--subtitle-dilate-color"
        )

    subtitle_text_height = args.text_height
    if args.subtitle_height and args.subtitle_height > 0:
        try:
            subtitle_text_height = int(round(args.subtitle_height * args.height))
        except Exception:
            pass

    out_dir = os.path.dirname(args.video) or "."
    in_base = os.path.splitext(os.path.basename(args.video))[0]
    if args.output is None and used_csv_path:
        csv_root, _ = os.path.splitext(used_csv_path)
        args.output = f"{csv_root}_dynamic.mp4"
    if args.output is None:
        args.output = os.path.join(out_dir, f"{in_base}_dynamic.mp4")
    temp_video = os.path.join(out_dir, f"{in_base}_temp_dynamic.mp4")
    temp_audio = os.path.join(out_dir, f"{in_base}_temp_dynamic.wav")

    # Load face tracking centers (JSONL)
    centers_by_frame = None
    if args.face_json:
        if os.path.exists(args.face_json):
            centers_by_frame = load_centers_jsonl(args.face_json)
        else:
            raise FileNotFoundError(f"Face tracking JSONL not found: {args.face_json}")
    else:
        face_path = f"{basefilename}.jsonl"
        tracking_path = f"{basefilename}_tracking.jsonl"
        if args.run_stabilize:
            if os.path.exists(tracking_path):
                print(f"[stabilize] tracking JSONL already exists: {tracking_path}")
            elif os.path.exists(face_path):
                cmd = [
                    "python",
                    "stabilize.py",
                    args.video,
                    "--width",
                    "0",
                    "--height",
                    "0",
                ]
                if args.visualize:
                    cmd += ["--visualize"]
                else:
                    cmd += ["--no-visualize"]
                stabilize_instance = args.stabilize_instance
                if not stabilize_instance:
                    stabilize_instance = infer_instance_id_from_jsonl(face_path)
                    if stabilize_instance:
                        print(f"Auto-selecting stabilize instance: {stabilize_instance}")
                if stabilize_instance:
                    cmd += ["--instance", stabilize_instance]
                if args.stabilize_select_interval and args.stabilize_select_interval > 0:
                    cmd += ["--select-interval", str(int(args.stabilize_select_interval))]
                if args.stabilize_time_shift:
                    cmd += ["--time-shift", str(int(args.stabilize_time_shift))]
                print("Running stabilize.py to build tracking JSONL...")
                subprocess.run(cmd, check=False)
            else:
                print(f"Warning: stabilize skipped (missing JSONL): {face_path}")

        if os.path.exists(tracking_path):
            print(f"Using face tracking JSONL: {tracking_path}")
            centers_by_frame = load_centers_jsonl(tracking_path)
        elif os.path.exists(face_path):
            print(f"Using face tracking JSONL: {face_path}")
            centers_by_frame = load_centers_jsonl(face_path)
        else:
            print("Warning: no face tracking JSONL found. Falling back to center crop.")

    zoom_point = None
    zoom_rects = None
    planned_frames = None
    centers_path = f"{os.path.splitext(args.video)[0]}_centers.jsonl"
    print(f"[centers] zoom center file: {centers_path}")
    if os.path.exists(centers_path):
        try:
            zoom_rects = []
            with open(centers_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    rec = json.loads(line)
                    zoom_rects.append(rec)
            if zoom_rects:
                print(f"[centers] using saved zoom centers from: {centers_path}")
            else:
                print(f"[centers] zoom center file is empty: {centers_path}")
        except Exception:
            zoom_rects = None
            print(f"[centers] failed to read zoom center file: {centers_path}")
    else:
        print(f"[centers] no saved zoom center file found at: {centers_path}")
    if args.select_zoom_point and zoom_rects:
        cap = cv2.VideoCapture(args.video)
        if cap.isOpened():
            total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
            fps_sel = cap.get(cv2.CAP_PROP_FPS) or 30.0
            cap.release()
            planned_frames = build_zoom_rect_plan(total_frames, fps_sel, interval_seconds=15.0)
            if notch_ranges and zoom_rects_have_timeline(zoom_rects):
                zoom_rects = _retime_zoom_rects_for_notches(zoom_rects, notch_ranges, fps_sel)
            if not zoom_rects_have_timeline(zoom_rects):
                print(
                    f"[centers] saved zoom center file is missing authored timeline data "
                    f"or has incompatible entries. Regenerating."
                )
                zoom_rects = None
            elif len(zoom_rects) < len(planned_frames):
                print(
                    f"[centers] resuming saved zoom centers from: {centers_path} "
                    f"({len(zoom_rects)}/{len(planned_frames)} completed)"
                )
    need_zoom_selection = args.select_zoom_point and (
        not zoom_rects or (planned_frames is not None and len(zoom_rects) < len(planned_frames))
    )
    if need_zoom_selection:
        print(f"[centers] selecting new zoom centers; will save to: {centers_path}")
        cap = cv2.VideoCapture(args.video)
        if cap.isOpened():
            total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
            fps_sel = cap.get(cv2.CAP_PROP_FPS) or 30.0
            cap.release()
            frames = planned_frames or build_zoom_rect_plan(total_frames, fps_sel, interval_seconds=15.0)
            print("[centers] planned authored points:")
            for i, frame_no in enumerate(frames, start=1):
                t = frame_no / fps_sel if fps_sel else 0.0
                print(f"  point{i} t={t:.2f}s frame={frame_no}")
            zoom_rects = select_zoom_rects(
                args.video,
                frames[0],
                args.width,
                args.height,
                count=len(frames),
                default_zoom=args.max_zoom if args.max_zoom and args.max_zoom > 0 else args.zoom_in,
                frame_indices=frames,
                rotate_degrees=args.rotate,
                initial_selections=zoom_rects,
                autosave_path=centers_path,
            )
            if zoom_rects is None:
                print("Zoom rectangle selection cancelled.")
            else:
                try:
                    with open(centers_path, "w", encoding="utf-8") as f:
                        for rec in zoom_rects:
                            f.write(json.dumps(rec) + "\n")
                    print(f"Wrote zoom centers: {centers_path}")
                    for i, rec in enumerate(zoom_rects, start=1):
                        print(
                            f"[centers] point{i} frame={rec.get('frame')} "
                            f"center=({rec.get('center_x')},{rec.get('center_y')}) "
                            f"zoom={rec.get('zoom')} rotate={rec.get('rotate', 0.0)} "
                            f"transition={rec.get('transition', 'pan_zoom')}"
                        )
                except Exception as e:
                    print(f"Warning: failed to save zoom centers: {e}")
        else:
            print("Warning: could not open video for zoom point selection.")

    # Process audio
    audio_for_combine = temp_audio
    temp_audio_is_original = False
    need_temp_audio = False
    if cut_ranges:
        need_temp_audio = True
    elif args.no_cut:
        if args.max_seconds and args.max_seconds > 0:
            need_temp_audio = True
        elif args.audio_only_delay_ms and args.audio_only_delay_ms != 0:
            need_temp_audio = True
        else:
            audio_for_combine = audiofilename
            temp_audio_is_original = True
    if not args.skip_audio and os.path.exists(temp_audio) and not need_temp_audio:
        print(f"Temp audio exists, skipping audio processing: {temp_audio}")
    elif not args.skip_audio:
        print("\n=== Processing Audio ===")
        print(f"Audio source: {audiofilename}")
        if cut_ranges:
            source_audio = audiofilename if os.path.exists(audiofilename) else args.video
            if not os.path.exists(audiofilename):
                print(f"Warning: audio file not found: {audiofilename}; using video audio stream.")
            range_segments = [{"start": s, "end": e} for s, e in cut_ranges]
            extract_audio_segments.extract_audio_segments(
                range_segments,
                source_audio,
                temp_audio,
                fade_samples=args.fade_samples,
            )
            audio_for_combine = temp_audio
        elif args.no_cut:
            # Full-length or trimmed audio without segment extraction.
            if os.path.exists(audiofilename):
                cmd = ["ffmpeg", "-y", "-i", audiofilename]
            else:
                cmd = ["ffmpeg", "-y", "-i", args.video, "-vn"]
            if args.max_seconds and args.max_seconds > 0:
                cmd += ["-t", f"{float(args.max_seconds):.3f}"]
            cmd += [temp_audio]
            subprocess.run(cmd, check=False)
        else:
            if os.path.exists(audiofilename):
                extract_audio_segments.extract_audio_segments(
                    word_segment_times_for_cut,
                    audiofilename,
                    temp_audio,
                    fade_samples=args.fade_samples,
                )
            else:
                print(f"Warning: audio file not found: {audiofilename}")
        if args.audio_only_delay_ms and args.audio_only_delay_ms != 0:
            delay_sec = float(args.audio_only_delay_ms) / 1000.0
            try:
                if delay_sec > 0:
                    prepend_silence_to_wav(temp_audio, delay_sec)
                else:
                    trim_leading_audio_wav(temp_audio, abs(delay_sec))
                print(f"Applied audio-only delay: {args.audio_only_delay_ms:.1f} ms")
            except Exception as e:
                print(f"Warning: failed to shift audio: {e}")
        if args.postpend_silence and args.postpend_silence_seconds > 0:
            try:
                if postpend_silence_to_wav(temp_audio, args.postpend_silence_seconds):
                    print(
                        f"Postpended {args.postpend_silence_seconds:.3f}s of silence to: {temp_audio}"
                    )
            except Exception as e:
                print(f"Warning: failed to postpend silence to temp audio: {e}")

    # Process video
    if not args.skip_video:
        print("\n=== Processing Video ===")
        cap = cv2.VideoCapture(args.video)
        fps = cap.get(cv2.CAP_PROP_FPS)
        total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        cap.release()

        video_duration = total_frames / fps if fps else 0.0
        if not args.no_cut:
            word_segment_times_for_subs = _retime_segments_to_frame_timeline(word_segment_times_for_subs, fps)
            if args.subtitles:
                max_chars = args.max_chars if args.max_chars and args.max_chars > 0 else None
                subtitle_group_words, subtitle_group_for_idx, subtitle_start_times, subtitle_end_times = (
                    build_subtitle_context(
                        word_segment_times_for_subs,
                        group_size=args.group_size,
                        max_chars=max_chars,
                        subtitle_lines=args.subtitle_lines,
                    )
                )
        use_absolute = bool(args.no_cut) or bool(cut_ranges)
        total_duration_for_schedule = video_duration
        if cut_ranges and cut_total is not None:
            total_duration_for_schedule = cut_total
        schedule_segments = word_segment_times_for_subs if not args.no_cut else word_segment_times_for_cut
        boundaries, total_out = build_punctuation_boundaries(
            schedule_segments,
            use_absolute_time=use_absolute,
            total_duration=total_duration_for_schedule if use_absolute else None,
        )
        if total_out <= 0 and video_duration > 0:
            total_out = video_duration
        zoom_schedule = build_zoom_schedule(
            boundaries,
            total_out,
            zoom_in=args.zoom_in,
            zoom_out=args.zoom_out,
            zoom_seconds=args.zoom_seconds,
            align_window=args.align_window,
            min_segment=args.min_segment,
            start_zoom_in=args.start_zoom_in,
        )
        if not zoom_schedule:
            zoom_schedule = [{"start": 0.0, "end": total_out, "zoom": args.zoom_out}]

        if cut_ranges:
            precomputed_segments = _precompute_segments_from_ranges(cut_ranges, fps)
            extract_video_segments_dynamic(
                precomputed_segments,
                args.video,
                temp_video,
                fps,
                args.width,
                args.height,
                zoom_schedule,
                centers_by_frame=centers_by_frame,
                face_smooth=args.face_smooth,
                y_offset=args.y_offset,
                zoom_ramp_sec=args.zoom_ramp,
                zoom_time=args.zoom_time,
                zoom_rate=args.zoom_rate,
                max_zoom=args.max_zoom,
                visualize=args.visualize,
                preview_scale=args.preview_scale,
                right_trigger=args.right_trigger,
                static_seconds=args.static_seconds,
                zoom_in=args.zoom_in,
                zoom_seconds=args.zoom_seconds,
                subtitles=args.subtitles,
                subtitle_group_words=subtitle_group_words,
                subtitle_group_for_idx=subtitle_group_for_idx,
                subtitle_start_times=subtitle_start_times,
                subtitle_end_times=subtitle_end_times,
                subtitle_lines=args.subtitle_lines,
                font_scale=args.font_scale,
                text_height=subtitle_text_height,
                text_height_2=None,
                font_ttf=font_ttf,
                subtitle_bg=args.subtitle_bg,
                subtitle_bg_color=subtitle_bg_color,
                subtitle_bg_alpha=args.subtitle_bg_alpha,
                subtitle_bg_pad=args.subtitle_bg_pad,
                subtitle_bg_height=args.subtitle_bg_height,
                subtitle_bg_offset_y=args.subtitle_bg_offset_y,
                shadow_size=args.shadow_size,
                shadow_offset_x=args.shadow_offset_x,
                shadow_offset_y=args.shadow_offset_y,
                subtitle_dilate=args.subtitle_dilate,
                subtitle_dilate_size=args.subtitle_dilate_size,
                subtitle_dilate_color=subtitle_dilate_color,
                subtitle_text_color=subtitle_text_color,
                subtitle_highlight_color=subtitle_highlight_color,
                subtitle_shadow_color=subtitle_shadow_color,
                subtitle_cache_max=args.subtitle_cache_max,
                subtitle_hold_ms=args.subtitle_hold_ms,
                max_seconds=args.max_seconds if args.max_seconds and args.max_seconds > 0 else None,
                zoom_point=zoom_point,
                zoom_rects=zoom_rects,
                no_trigger=args.no_trigger,
                loop_sequence=args.loop_sequence,
                no_punct_schedule=args.no_punct_schedule,
                temp_codec=args.temp_codec,
                fade_frames=args.fade_frames,
                video_shift_frames=args.video_shift_frames,
                rotate_degrees=args.rotate,
            )
        elif args.no_cut:
            extract_video_dynamic_full(
                args.video,
                temp_video,
                fps,
                args.width,
                args.height,
                zoom_schedule,
                centers_by_frame=centers_by_frame,
                face_smooth=args.face_smooth,
                y_offset=args.y_offset,
                zoom_ramp_sec=args.zoom_ramp,
                zoom_time=args.zoom_time,
                zoom_rate=args.zoom_rate,
                max_zoom=args.max_zoom,
                visualize=args.visualize,
                preview_scale=args.preview_scale,
                right_trigger=args.right_trigger,
                static_seconds=args.static_seconds,
                zoom_in=args.zoom_in,
                zoom_seconds=args.zoom_seconds,
                subtitles=args.subtitles,
                subtitle_group_words=subtitle_group_words,
                subtitle_group_for_idx=subtitle_group_for_idx,
                subtitle_start_times=subtitle_start_times,
                subtitle_end_times=subtitle_end_times,
                subtitle_lines=args.subtitle_lines,
                font_scale=args.font_scale,
                text_height=subtitle_text_height,
                text_height_2=None,
                font_ttf=font_ttf,
                subtitle_bg=args.subtitle_bg,
                subtitle_bg_color=subtitle_bg_color,
                subtitle_bg_alpha=args.subtitle_bg_alpha,
                subtitle_bg_pad=args.subtitle_bg_pad,
                subtitle_bg_height=args.subtitle_bg_height,
                subtitle_bg_offset_y=args.subtitle_bg_offset_y,
                shadow_size=args.shadow_size,
                shadow_offset_x=args.shadow_offset_x,
                shadow_offset_y=args.shadow_offset_y,
                subtitle_dilate=args.subtitle_dilate,
                subtitle_dilate_size=args.subtitle_dilate_size,
                subtitle_dilate_color=subtitle_dilate_color,
                subtitle_text_color=subtitle_text_color,
                subtitle_highlight_color=subtitle_highlight_color,
                subtitle_shadow_color=subtitle_shadow_color,
                subtitle_cache_max=args.subtitle_cache_max,
                subtitle_hold_ms=args.subtitle_hold_ms,
                max_seconds=args.max_seconds if args.max_seconds and args.max_seconds > 0 else None,
                zoom_point=zoom_point,
                zoom_rects=zoom_rects,
                no_trigger=args.no_trigger,
                loop_sequence=args.loop_sequence,
                no_punct_schedule=args.no_punct_schedule,
                temp_codec=args.temp_codec,
                rotate_degrees=args.rotate,
            )
        else:
            precomputed_segments = precompute_segments_basic(word_segment_times_for_cut, fps)
            extract_video_segments_dynamic(
                precomputed_segments,
                args.video,
                temp_video,
                fps,
                args.width,
                args.height,
                zoom_schedule,
                centers_by_frame=centers_by_frame,
                face_smooth=args.face_smooth,
                y_offset=args.y_offset,
                zoom_ramp_sec=args.zoom_ramp,
                zoom_time=args.zoom_time,
                zoom_rate=args.zoom_rate,
                max_zoom=args.max_zoom,
                visualize=args.visualize,
                preview_scale=args.preview_scale,
                right_trigger=args.right_trigger,
                static_seconds=args.static_seconds,
                zoom_in=args.zoom_in,
                zoom_seconds=args.zoom_seconds,
                subtitles=args.subtitles,
                subtitle_group_words=subtitle_group_words,
                subtitle_group_for_idx=subtitle_group_for_idx,
                subtitle_start_times=subtitle_start_times,
                subtitle_end_times=subtitle_end_times,
                subtitle_lines=args.subtitle_lines,
                font_scale=args.font_scale,
                text_height=subtitle_text_height,
                text_height_2=None,
                font_ttf=font_ttf,
                subtitle_bg=args.subtitle_bg,
                subtitle_bg_color=subtitle_bg_color,
                subtitle_bg_alpha=args.subtitle_bg_alpha,
                subtitle_bg_pad=args.subtitle_bg_pad,
                subtitle_bg_height=args.subtitle_bg_height,
                subtitle_bg_offset_y=args.subtitle_bg_offset_y,
                shadow_size=args.shadow_size,
                shadow_offset_x=args.shadow_offset_x,
                shadow_offset_y=args.shadow_offset_y,
                subtitle_dilate=args.subtitle_dilate,
                subtitle_dilate_size=args.subtitle_dilate_size,
                subtitle_dilate_color=subtitle_dilate_color,
                subtitle_text_color=subtitle_text_color,
                subtitle_highlight_color=subtitle_highlight_color,
                subtitle_shadow_color=subtitle_shadow_color,
                subtitle_cache_max=args.subtitle_cache_max,
                subtitle_hold_ms=args.subtitle_hold_ms,
                max_seconds=args.max_seconds if args.max_seconds and args.max_seconds > 0 else None,
                zoom_point=zoom_point,
                zoom_rects=zoom_rects,
                no_trigger=args.no_trigger,
                loop_sequence=args.loop_sequence,
                no_punct_schedule=args.no_punct_schedule,
                temp_codec=args.temp_codec,
                video_shift_frames=args.video_shift_frames,
                rotate_degrees=args.rotate,
            )

    # Combine audio and video
    if not args.skip_audio and not args.skip_video:
        print("\n=== Combining Audio and Video ===")
        if not os.path.exists(audio_for_combine):
            print(f"Warning: audio file not found: {audio_for_combine}. Writing video only.")
            reencode_video(temp_video, args.output)
        else:
            combine_video_audio(temp_video, audio_for_combine, args.output)
        if os.path.exists(temp_video):
            os.remove(temp_video)
        if os.path.exists(temp_audio) and not temp_audio_is_original:
            os.remove(temp_audio)
        print(f"\nDone! Output saved to: {args.output}")
    elif not args.skip_video:
        reencode_video(temp_video, args.output)
        print(f"\nDone! Output saved to: {args.output}")
    elif not args.skip_audio:
        os.rename(temp_audio, args.output.replace(".mp4", ".wav"))
        print(f"\nDone! Audio output saved to: {args.output.replace('.mp4', '.wav')}")


if __name__ == "__main__":
    main()
