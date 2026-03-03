import csv
import math
import os
from subtitles import apply_subtitles, get_active_word_idx


def prepend_silence_to_wav(path, seconds):
    """
    Prepend silence to a WAV file in-place using a temp file.
    """
    import wave

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
    import wave

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
                start = _parse_timecode_to_seconds(row[0])
                end = _parse_timecode_to_seconds(row[1]) if len(row) > 1 else None
                if start is None or end is None:
                    continue
                if end <= start:
                    continue
                ranges.append((start, end))
    except FileNotFoundError:
        return []
    return _normalize_notch_ranges(ranges)


def _write_notch_csv(path, ranges):
    try:
        with open(path, "w", encoding="utf-8") as f:
            writer = csv.writer(f)
            for start, end in ranges:
                writer.writerow([f"{start:.3f}", f"{end:.3f}"])
    except OSError:
        return False
    return True


def _normalize_notch_ranges(ranges):
    cleaned = []
    for start, end in ranges:
        if end <= start:
            continue
        cleaned.append((float(start), float(end)))
    if not cleaned:
        return []
    cleaned.sort()
    merged = [cleaned[0]]
    for start, end in cleaned[1:]:
        last_start, last_end = merged[-1]
        if start <= last_end:
            merged[-1] = (last_start, max(last_end, end))
        else:
            merged.append((start, end))
    return merged


def _prompt_notch_ranges_gui(path_hint):
    try:
        import tkinter as tk
        from tkinter import messagebox
        from tkinter.scrolledtext import ScrolledText
    except Exception as exc:
        raise RuntimeError(
            f"GUI not available for notch input: {exc}\n"
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
        root.destroy()

    def on_skip():
        result["ranges"] = []
        root.destroy()

    tk.Button(btn_frame, text="Save", command=on_save, width=12).pack(side="left", padx=6)
    tk.Button(btn_frame, text="Skip", command=on_skip, width=12).pack(side="left", padx=6)

    root.mainloop()
    return result["ranges"] if result["ranges"] is not None else []


def _prompt_notch_ranges_cli():
    print("Enter notch ranges to remove (comma-separated, e.g. 0:10-0:30,1:05-1:20).")
    raw = input("Notch ranges (blank to skip): ").strip()
    if not raw:
        return []
    ranges = []
    for chunk in raw.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        if "-" not in chunk:
            continue
        s, e = chunk.split("-", 1)
        start = _parse_timecode_to_seconds(s)
        end = _parse_timecode_to_seconds(e)
        if start is None or end is None or end <= start:
            continue
        ranges.append((start, end))
    return _normalize_notch_ranges(ranges)


def _build_keep_ranges(total_duration, notch_ranges):
    if total_duration <= 0:
        return []
    if not notch_ranges:
        return [(0.0, total_duration)]
    keep = []
    cursor = 0.0
    for start, end in notch_ranges:
        start = max(0.0, start)
        end = min(total_duration, end)
        if start > cursor:
            keep.append((cursor, start))
        cursor = max(cursor, end)
    if cursor < total_duration:
        keep.append((cursor, total_duration))
    return keep


def _precompute_segments_from_ranges(ranges, fps):
    precomputed = []
    for start, end in ranges:
        start_frame = int(math.floor(start * fps))
        end_frame = int(math.floor(end * fps))
        if end_frame < start_frame:
            end_frame = start_frame
        precomputed.append(
            {
                "start_frame": start_frame,
                "end_frame": end_frame,
                "start_sec": start,
                "end_sec": end,
                "word": "",
            }
        )
    return precomputed


def _retime_segments_for_notches(word_segment_times, ranges):
    if not ranges:
        return word_segment_times
    retimed = []
    removed = 0
    for seg in word_segment_times:
        start = float(seg.get("start", 0.0))
        end = float(seg.get("end", start))
        overlap = False
        for r_start, r_end in ranges:
            if end <= r_start or start >= r_end:
                continue
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
    if removed:
        print(f"Retimed notch ranges: removed {removed} word segments, kept {len(retimed)}")
    return retimed


def _retime_segments_for_word_cuts(word_segment_times):
    retimed = []
    cursor = 0.0
    for seg in word_segment_times:
        dur = float(seg.get("end", seg.get("start", 0.0))) - float(seg.get("start", 0.0))
        if dur < 0:
            dur = 0.0
        curr = dict(seg)
        curr["start"] = cursor
        curr["end"] = cursor + dur
        cursor = curr["end"]
        retimed.append(curr)
    print(f"Retimed word segments to concatenated timeline ({cursor:.2f}s total).")
    return retimed


def _retime_segments_to_frame_timeline(word_segment_times, fps):
    retimed = []
    cursor_frames = 0
    for seg in word_segment_times:
        dur = float(seg.get("end", seg.get("start", 0.0))) - float(seg.get("start", 0.0))
        if dur < 0:
            dur = 0.0
        frames = int(round(dur * fps))
        curr = dict(seg)
        curr["start"] = cursor_frames / fps if fps else 0.0
        curr["end"] = (cursor_frames + frames) / fps if fps else 0.0
        cursor_frames += frames
        retimed.append(curr)
    if fps:
        print(f"Quantized word segments to frame timeline ({cursor_frames / fps:.2f}s total).")
    return retimed


def _retime_zoom_rects_for_notches(zoom_rects, ranges, fps):
    if not ranges or not zoom_rects:
        return zoom_rects
    retimed = []
    removed = 0
    for rec in zoom_rects:
        frame = rec.get("frame")
        if frame is None:
            retimed.append(rec)
            continue
        t = float(frame) / float(fps) if fps else 0.0
        overlap = False
        for r_start, r_end in ranges:
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
    for seg in word_segment_times:
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


def merge_short_segments(word_segment_times, min_segment_seconds):
    if not word_segment_times or min_segment_seconds is None:
        return word_segment_times
    min_len = float(min_segment_seconds)
    if min_len <= 0:
        return word_segment_times
    merged = []
    i = 0
    while i < len(word_segment_times):
        curr = dict(word_segment_times[i])
        start = float(curr.get("start", 0.0))
        end = float(curr.get("end", start))
        if end < start:
            end = start
        # Merge forward if too short or if gap to next is tiny
        if i + 1 < len(word_segment_times):
            nxt = dict(word_segment_times[i + 1])
            n_start = float(nxt.get("start", end))
            n_end = float(nxt.get("end", n_start))
            if n_end < n_start:
                n_end = n_start
            gap = max(0.0, n_start - end)
            dur = max(0.0, end - start)
            if dur < min_len or gap < min_len:
                # merge with next
                curr["end"] = max(end, n_end)
                curr["word"] = f"{curr.get('word','')} {nxt.get('word','')}".strip()
                word_segment_times[i + 1] = curr
                i += 1
                continue
        curr["start"] = start
        curr["end"] = end
        merged.append(curr)
        i += 1
    if len(merged) != len(word_segment_times):
        print(f"Merged short segments: {len(word_segment_times)} -> {len(merged)}")
    return merged


def close_small_gaps(word_segment_times, max_gap_seconds):
    if not word_segment_times or max_gap_seconds is None:
        return word_segment_times
    max_gap = float(max_gap_seconds)
    if max_gap <= 0:
        return word_segment_times
    adjusted = [dict(word_segment_times[0])]
    for seg in word_segment_times[1:]:
        curr = dict(seg)
        prev = adjusted[-1]
        prev_end = float(prev.get("end", prev.get("start", 0.0)))
        curr_start = float(curr.get("start", prev_end))
        gap = curr_start - prev_end
        if gap > 0 and gap <= max_gap:
            # Extend previous segment to close the small gap.
            prev["end"] = curr_start
        adjusted.append(curr)
    return adjusted


def apply_bounded_prepend_extend(word_segment_times, prepend_ms, extend_ms):
    if not word_segment_times:
        return word_segment_times
    prepend = max(0.0, float(prepend_ms or 0.0) / 1000.0)
    extend = max(0.0, float(extend_ms or 0.0) / 1000.0)
    if prepend <= 0 and extend <= 0:
        return word_segment_times
    adjusted = []
    for i, seg in enumerate(word_segment_times):
        curr = dict(seg)
        start = float(curr.get("start", 0.0))
        end = float(curr.get("end", start))
        if end < start:
            end = start
        prev_end = None
        if i > 0:
            prev = word_segment_times[i - 1]
            prev_end = float(prev.get("end", prev.get("start", 0.0)))
        next_start = None
        if i + 1 < len(word_segment_times):
            nxt = word_segment_times[i + 1]
            next_start = float(nxt.get("start", end))
        if prepend > 0:
            new_start = start - prepend
            if prev_end is not None and new_start < prev_end:
                new_start = prev_end
            start = new_start
        if extend > 0:
            new_end = end + extend
            if next_start is not None and new_end > next_start:
                new_end = next_start
            end = new_end
        if start < 0:
            start = 0.0
        if end < 0:
            end = 0.0
        if end < start:
            end = start
        curr["start"] = start
        curr["end"] = end
        adjusted.append(curr)
    return adjusted


class SubtitleRenderer:
    def __init__(
        self,
        subtitles,
        subtitle_group_words,
        subtitle_group_for_idx,
        subtitle_start_times,
        subtitle_end_times,
        output_height,
        output_width,
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
        subtitle_cache_max=64,
    ):
        self.subtitles = subtitles
        self.subtitle_group_words = subtitle_group_words
        self.subtitle_group_for_idx = subtitle_group_for_idx or {}
        self.subtitle_start_times = subtitle_start_times or []
        self.subtitle_end_times = subtitle_end_times or []
        self.output_height = output_height
        self.output_width = output_width
        self.font_scale = font_scale
        self.text_height = text_height
        self.text_height_2 = text_height_2
        self.subtitle_lines = subtitle_lines
        self.font_ttf = font_ttf
        self.subtitle_bg = subtitle_bg
        self.subtitle_bg_color = subtitle_bg_color
        self.subtitle_bg_alpha = subtitle_bg_alpha
        self.subtitle_bg_pad = subtitle_bg_pad
        self.subtitle_bg_height = subtitle_bg_height
        self.subtitle_bg_offset_y = subtitle_bg_offset_y
        self.shadow_size = shadow_size
        self.shadow_offset_x = shadow_offset_x
        self.shadow_offset_y = shadow_offset_y
        self.subtitle_dilate = subtitle_dilate
        self.subtitle_dilate_size = subtitle_dilate_size
        self.subtitle_dilate_color = subtitle_dilate_color
        self.subtitle_text_color = subtitle_text_color
        self.subtitle_highlight_color = subtitle_highlight_color
        self.subtitle_shadow_color = subtitle_shadow_color
        self.subtitle_cache_max = max(0, int(subtitle_cache_max))
        self.last_sub_idx = None
        self.last_sub_time = None
        self.overlay_cache = {}
        self.mask_cache = {}
        self.bg_mask_cache = {}

    def render(self, frame, t_sub):
        if not self.subtitles or not self.subtitle_group_words:
            return frame
        if self.subtitle_cache_max == 0:
            self.overlay_cache.clear()
            self.mask_cache.clear()
            self.bg_mask_cache.clear()
        elif self.subtitle_cache_max > 0 and len(self.overlay_cache) > self.subtitle_cache_max:
            self.overlay_cache.clear()
            self.mask_cache.clear()
            self.bg_mask_cache.clear()
        active_idx = get_active_word_idx(t_sub, self.subtitle_start_times, self.subtitle_end_times)
        if active_idx is not None:
            self.last_sub_idx = active_idx
            self.last_sub_time = t_sub
            gi, pos = self.subtitle_group_for_idx.get(active_idx, (None, None))
            if gi is not None and gi < len(self.subtitle_group_words):
                group_words = self.subtitle_group_words[gi]
                frame = apply_subtitles(
                    frame,
                    (self.output_height, self.output_width, 3),
                    group_words,
                    pos,
                    self.overlay_cache,
                    self.mask_cache,
                    self.bg_mask_cache,
                    self.font_scale,
                    self.text_height,
                    self.text_height_2,
                    self.subtitle_lines,
                    self.font_ttf,
                    self.subtitle_bg,
                    self.subtitle_bg_color,
                    self.subtitle_bg_alpha,
                    self.subtitle_bg_pad,
                    self.subtitle_bg_height,
                    self.subtitle_bg_offset_y,
                    self.shadow_size,
                    self.shadow_offset_x,
                    self.shadow_offset_y,
                    self.subtitle_dilate,
                    self.subtitle_dilate_size,
                    self.subtitle_dilate_color,
                    self.subtitle_text_color,
                    self.subtitle_highlight_color,
                    self.subtitle_shadow_color,
                )
        return frame
