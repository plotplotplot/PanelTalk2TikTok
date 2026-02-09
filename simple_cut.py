#!/usr/bin/env python3
"""
Simplified script to extract video and audio segments based on time periods from a CSV file.
No cropping or face tracking - output is same size as input.
"""
import argparse
import csv
import json
import math
import os
import subprocess

import cv2
import numpy as np
from tqdm import tqdm
from PIL import ImageFont

import extract_audio_segments
import segment_utils
import transcriptJson2csv
from extract_video_segements import extract_video_segments


def load_word_segments_from_csv(csv_path):
    """
    Load word-level segments from a CSV with columns: speaker,start,end,word.
    """
    word_segment_times = []
    with open(csv_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                start = float(row.get("start", 0.0) or 0.0)
            except Exception:
                start = 0.0
            try:
                end = float(row.get("end", start) or start)
            except Exception:
                end = start
            word_segment_times.append(
                {
                    "speaker": row.get("speaker", "") if row.get("speaker") is not None else "",
                    "start": start,
                    "end": end,
                    "word": (row.get("word") or "").strip(),
                    "filename": (row.get("filename") or "").strip(),
                    "center_x": row.get("center_x"),
                    "center_y": row.get("center_y"),
                    "zoom": row.get("zoom"),
                }
            )
    return word_segment_times


def _line_length(words):
    total = 0
    for i, w in enumerate(words):
        wlen = len(w)
        total += wlen if i == 0 else wlen + 1
    return total


def _min_max_line_len(words):
    if len(words) <= 1:
        return _line_length(words)
    best = None
    for split in range(1, len(words)):
        left = _line_length(words[:split])
        right = _line_length(words[split:])
        m = max(left, right)
        if best is None or m < best:
            best = m
    return best if best is not None else 0


def _build_word_groups(words, group_size=6, max_chars=None, subtitle_lines=1):
    groups = []
    current = []
    current_len = 0
    for idx, w in enumerate(words):
        text = (w.get("word") or "").strip()
        word_len = len(text)
        extra = word_len if current_len == 0 else word_len + 1
        if max_chars is not None and current:
            if subtitle_lines <= 1:
                if (current_len + extra) > max_chars:
                    groups.append(current)
                    current = []
                    current_len = 0
            else:
                candidate_words = [
                    (words[i].get("word") or "").strip() for i in (current + [idx])
                ]
                if _min_max_line_len(candidate_words) > max_chars:
                    groups.append(current)
                    current = []
                    current_len = 0
        current.append(idx)
        current_len += word_len if current_len == 0 else word_len + 1
        if len(current) >= group_size:
            groups.append(current)
            current = []
            current_len = 0
            continue
        if text.endswith((",", ".", "?", "!", ";", ":")):
            groups.append(current)
            current = []
            current_len = 0
    if current:
        groups.append(current)
    return groups


def precompute_segments(word_segment_times, fps, group_size=6, max_chars=None, subtitle_lines=1):
    """
    Convert word segments to frame-aligned timing.
    """
    precomputed_segments = []

    total_words = len(word_segment_times)
    groups = _build_word_groups(
        word_segment_times,
        group_size=group_size,
        max_chars=max_chars,
        subtitle_lines=subtitle_lines,
    )
    group_for_idx = {}
    for gi, g in enumerate(groups):
        for pos, wi in enumerate(g):
            group_for_idx[wi] = (gi, pos)

    for idx, seg in enumerate(tqdm(word_segment_times, desc="Precomputing segment timings")):
        start_f = seg["start"] * fps
        end_f = seg["end"] * fps
        start_frame = math.floor(start_f)
        end_frame = math.floor(end_f)

        gi, pos = group_for_idx.get(idx, (idx // group_size, idx % group_size))
        group_indices = groups[gi] if gi < len(groups) else [idx]
        group_words = [word_segment_times[i].get("word", "") for i in group_indices]
        active_idx = pos

        precomputed_segments.append({
            "word": seg["word"],
            "group_words": group_words,
            "active_idx": active_idx,
            "start_frame": start_frame,
            "end_frame": end_frame,
            "start_sec": start_frame / fps,
            "end_sec": end_frame / fps,
        })
    
    return precomputed_segments

def add_text_with_shadow(frame, text, font_scale=1.8, text_height_from_bottom=300):
    """
    Add text with drop shadow to a frame.
    """
    if text is None:
        text = ""
    elif isinstance(text, float) and (np.isnan(text) or np.isinf(text)):
        text = ""
    else:
        text = str(text)
    
    height, width = frame.shape[:2]
    thickness = int(font_scale * 2)
    font = cv2.FONT_HERSHEY_SIMPLEX
    
    # Calculate text size
    (text_width, text_height), baseline = cv2.getTextSize(
        text, font, font_scale, thickness
    )
    
    # Position text at bottom center
    x = (width - text_width) // 2
    y = height - text_height_from_bottom
    
    # Add shadow
    shadow_offset = 3
    cv2.putText(
        frame,
        text,
        (x + shadow_offset, y + shadow_offset),
        font,
        font_scale,
        (0, 0, 0),
        thickness,
        cv2.LINE_AA,
    )
    
    # Add main text
    cv2.putText(
        frame, text, (x, y), font, font_scale, (255, 255, 255), thickness, cv2.LINE_AA
    )
    
    return frame

 


def combine_video_audio(video_file, audio_file, final_output):
    """
    Combine video and audio files using ffmpeg.
    """
    cmd = [
        "ffmpeg",
        "-i", video_file,
        "-i", audio_file,
        "-c:v", "libx264",
        "-c:a", "aac",
        "-strict", "experimental",
        "-y",
        final_output
    ]
    
    try:
        subprocess.run(cmd, check=True)
        print(f"Video and audio combined to: {final_output}")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error combining video and audio: {e}")
        return False

def reencode_video(video_file, final_output):
    """
    Re-encode video-only output using a standard codec.
    """
    cmd = [
        "ffmpeg",
        "-i", video_file,
        "-c:v", "libx264",
        "-y",
        final_output,
    ]
    try:
        subprocess.run(cmd, check=True)
        print(f"Video re-encoded to: {final_output}")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error re-encoding video: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(
        description="Extract video and audio segments based on CSV time periods"
    )
    parser.add_argument(
        "--video",
        type=str,
        required=True,
        help="Input video file"
    )
    parser.add_argument(
        "--font-scale",
        type=float,
        default=1.8,
        help="Font scale for text overlay (default: 1.8)"
    )
    parser.add_argument(
        "--text-height",
        type=int,
        default=300,
        help="Height of text from bottom (default: 300)"
    )
    parser.add_argument(
        "--subtitle-lines",
        type=int,
        default=1,
        help="Number of subtitle lines to render (default: 1)",
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
        default=36,
        help="TTF font size in pixels (default: 36)",
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
        default=0.6,
        help="Background opacity 0-1 (default: 0.6)",
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
        "--shadow-size",
        type=int,
        default=0,
        help="Subtitle shadow offset in pixels (default: 0)",
    )
    parser.add_argument(
        "--shadow-offset-x",
        type=int,
        default=0,
        help="Subtitle shadow X offset in pixels (default: 0). If both offsets are 0, uses --shadow-size.",
    )
    parser.add_argument(
        "--shadow-offset-y",
        type=int,
        default=0,
        help="Subtitle shadow Y offset in pixels (default: 0). If both offsets are 0, uses --shadow-size.",
    )
    parser.add_argument(
        "--subtitle-dilate",
        action="store_true",
        default=True,
        help="Use dilated outline instead of drop shadow.",
    )
    parser.add_argument(
        "--subtitle-dilate-size",
        type=int,
        default=10,
        help="Dilate pixel radius for subtitle outline (default: 10).",
    )
    parser.add_argument(
        "--subtitle-dilate-color",
        type=str,
        default="0,0,0",
        help="Dilate color as B,G,R (default: 0,0,0).",
    )
    parser.add_argument(
        "--subtitle-text-color",
        type=str,
        default="255,255,255",
        help="Subtitle text color as B,G,R (default: 255,255,255).",
    )
    parser.add_argument(
        "--subtitle-highlight-color",
        type=str,
        default="0,255,255",
        help="Subtitle highlight color as B,G,R (default: 0,255,255).",
    )
    parser.add_argument(
        "--subtitle-shadow-color",
        type=str,
        default="0,0,0",
        help="Subtitle shadow color as B,G,R (default: 0,0,0).",
    )
    parser.add_argument(
        "--brightness-mult",
        type=str,
        default="1.0,1.0,1.0",
        help="Multiply frame brightness per-channel as B,G,R (default: 1.0,1.0,1.0).",
    )
    parser.add_argument(
        "--fade-frames",
        type=int,
        default=6,
        help="Crossfade length for video stitching (default: 6)"
    )
    parser.add_argument(
        "--fade-samples",
        type=int,
        default=100,
        help="Crossfade length for audio stitching in samples (default: 3000)"
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
        "--temp-codec",
        type=str,
        default="mp4v",
        help="FourCC for temp video encoding (default: mp4v)",
    )
    parser.add_argument(
        "--shift-seconds",
        type=float,
        default=0.15,
        help="Shift all word timings by this many seconds (default: 0.15)",
    )
    parser.add_argument(
        "--preserve-word-gaps",
        action="store_true",
        help="Disable timing adjustments that reduce gaps between words (sets --shift-seconds 0 and --extend-ms 0)",
    )
    parser.add_argument(
        "--video-shift-frames",
        type=int,
        default=0,
        help="Shift video segments by N frames relative to audio (positive=advance, negative=delay).",
    )
    parser.add_argument(
        "--extend-ms",
        type=int,
        default=20,
        help="Extend each segment end by this many ms (default: 20)",
    )
    parser.add_argument(
        "--prepend-ms",
        type=int,
        default=20,
        help="Extend each segment start earlier by this many ms (default: 20)",
    )
    parser.add_argument(
        "--visualize",
        action="store_true",
        help="Show real-time visualization"
    )
    parser.add_argument(
        "--skip-video",
        action="store_true",
        help="Skip video processing"
    )
    parser.add_argument(
        "--skip-audio",
        action="store_true",
        help="Skip audio processing"
    )
    parser.add_argument(
        "--output",
        type=str,
        default=None,
        help="Output video file (default: output.mp4, or CSV basename if present)",
    )
    parser.add_argument(
        "--use-csv",
        action="store_true",
        help="Use JSON-derived CSV for word timings (create if missing)",
    )
    parser.add_argument(
        "--csv",
        type=str,
        default="",
        help="Path to a CSV file with word timings (speaker,start,end,word). Skips JSON.",
    )
    parser.add_argument(
        "--center-x",
        "--center_x",
        type=float,
        default=0.5,
        help="Crop center X as fraction of width (default: 0.5)",
    )
    parser.add_argument(
        "--center-y",
        "--center_y",
        type=float,
        default=0.5,
        help="Crop center Y as fraction of height (default: 0.5)",
    )
    parser.add_argument(
        "--width",
        type=int,
        default=0,
        help="Output width in pixels (enable cropping if > 0)",
    )
    parser.add_argument(
        "--height",
        type=int,
        default=0,
        help="Output height in pixels (enable cropping if > 0)",
    )
    parser.add_argument(
        "--zoom",
        type=float,
        default=1.0,
        help="Zoom factor for crop (default: 1.0). >1 zooms in.",
    )
    parser.add_argument(
        "--ig-standard",
        action="store_true",
        help="Apply IG standard settings (1080x1920, 2 subtitle lines, bg, etc.)",
    )
    
    args = parser.parse_args()
    text_height_2 = None

    if args.ig_standard:
        args.width = 1080
        args.height = 1920
        args.subtitle_lines = 2
        args.text_height = 600 + 170
        text_height_2 = None
        args.font_scale = 2
        args.subtitle_bg = True
        args.subtitle_bg_height = 300
        args.subtitle_bg_offset_y = 140
        args.subtitle_bg_alpha = 0.5
        args.subtitle_bg_alpha = 1
        if not args.csv:
            args.csv = "CLIMATE EVENT BUTTERFLY EFFECT 2025 RAWS/OrtizFull.csv"

    # Load CSV/JSON data
    print("Loading segment data...")
    basefilename = "".join(args.video.split(".")[:-1])
    def _strip_suffixes(name):
        for suf in ("_graded", "_stable"):
            if name.endswith(suf):
                name = name[: -len(suf)]
        return name

    json_base = _strip_suffixes(basefilename)
    jsonfilename = json_base + ".json"
    audiofilename = json_base + ".wav"

    csvfilename = jsonfilename + ".csv"
    used_csv_path = None
    if args.csv:
        used_csv_path = args.csv
        print(f"Using CSV timings: {args.csv}")
        word_segment_times = load_word_segments_from_csv(args.csv)
    elif os.path.exists(csvfilename):
        used_csv_path = csvfilename
        print(f"Using CSV timings: {csvfilename}")
        word_segment_times = load_word_segments_from_csv(csvfilename)
    elif os.path.exists(jsonfilename):
        print(f"Using JSON timings: {jsonfilename}")
        # Export CSV for next time, then load JSON.
        transcriptJson2csv.export_words_to_csv(jsonfilename, source_video=args.video)
        word_segment_times = segment_utils.load_word_segments_from_json(jsonfilename)
    else:
        print(f"Using JSON timings (missing, attempting anyway): {jsonfilename}")
        word_segment_times = segment_utils.load_word_segments_from_json(jsonfilename)

    sr = segment_utils.get_audio_sample_rate(audiofilename)
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
    
    # Output naming: default to {infile}_final.mp4 in same dir as input
    out_dir = os.path.dirname(args.video) or "."
    in_base = os.path.splitext(os.path.basename(args.video))[0]
    if args.output is None:
        args.output = os.path.join(out_dir, f"{in_base}_final.mp4")
    temp_video = os.path.join(out_dir, f"{in_base}_temp.mp4")
    temp_audio = os.path.join(out_dir, f"{in_base}_temp.wav")
    
    # Process audio
    if not args.skip_audio and os.path.exists(temp_audio):
        print(f"Temp audio exists, skipping audio processing: {temp_audio}")
    elif not args.skip_audio:
        print("\n=== Processing Audio ===")
        extract_audio_segments.extract_audio_segments(
            word_segment_times,
            audiofilename,
            temp_audio,
            fade_samples=args.fade_samples
        )
    
    # Process video
    if not args.skip_video:
        print("\n=== Processing Video ===")
        
        # Get video FPS and size
        cap = cv2.VideoCapture(args.video)
        fps = cap.get(cv2.CAP_PROP_FPS)
        input_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        input_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        cap.release()

        crop_config = None
        if args.width > 0 and args.height > 0:
            if args.zoom <= 0:
                raise ValueError("--zoom must be > 0")

            center_x = max(0.0, min(1.0, args.center_x))
            center_y = max(0.0, min(1.0, args.center_y))

            crop_w = max(1, int(round(args.width / args.zoom)))
            crop_h = max(1, int(round(args.height / args.zoom)))

            cx = int(round(center_x * input_width))
            cy = int(round(center_y * input_height))

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
                x1 -= (x2 - input_width)
                x2 = input_width
            if y2 > input_height:
                y1 -= (y2 - input_height)
                y2 = input_height

            x1 = max(0, x1)
            y1 = max(0, y1)
            x2 = min(input_width, x2)
            y2 = min(input_height, y2)

            if x2 <= x1 or y2 <= y1:
                raise ValueError("Invalid crop box; check --center-x/--center-y/--width/--height/--zoom")

            crop_config = {
                "output_width": args.width,
                "output_height": args.height,
                "crop_box": (x1, y1, x2, y2),
            }
        
        # Precompute segments
        max_chars = args.max_chars if args.max_chars and args.max_chars > 0 else None
        precomputed_segments = precompute_segments(
            word_segment_times,
            fps,
            group_size=args.group_size,
            max_chars=max_chars,
            subtitle_lines=args.subtitle_lines,
        )
        
        # Extract video segments
        font_ttf = None
        if args.font_file:
            if not os.path.exists(args.font_file):
                raise FileNotFoundError(f"Font file not found: {args.font_file}")
            effective_size = max(1, int(round(args.font_size * args.font_scale)))
            font_ttf = ImageFont.truetype(args.font_file, effective_size)

        try:
            bg_parts = [int(x) for x in args.subtitle_bg_color.split(",")]
            if len(bg_parts) != 3:
                raise ValueError
            subtitle_bg_color = (bg_parts[0], bg_parts[1], bg_parts[2])
        except Exception:
            raise ValueError("--subtitle-bg-color must be in 'B,G,R' format")

        try:
            dc_parts = [int(x) for x in args.subtitle_dilate_color.split(",")]
            if len(dc_parts) != 3:
                raise ValueError
            subtitle_dilate_color = (dc_parts[0], dc_parts[1], dc_parts[2])
        except Exception:
            raise ValueError("--subtitle-dilate-color must be in 'B,G,R' format")

        try:
            tc_parts = [int(x) for x in args.subtitle_text_color.split(",")]
            if len(tc_parts) != 3:
                raise ValueError
            subtitle_text_color = (tc_parts[0], tc_parts[1], tc_parts[2])
        except Exception:
            raise ValueError("--subtitle-text-color must be in 'B,G,R' format")

        try:
            hc_parts = [int(x) for x in args.subtitle_highlight_color.split(",")]
            if len(hc_parts) != 3:
                raise ValueError
            subtitle_highlight_color = (hc_parts[0], hc_parts[1], hc_parts[2])
        except Exception:
            raise ValueError("--subtitle-highlight-color must be in 'B,G,R' format")

        try:
            sc_parts = [int(x) for x in args.subtitle_shadow_color.split(",")]
            if len(sc_parts) != 3:
                raise ValueError
            subtitle_shadow_color = (sc_parts[0], sc_parts[1], sc_parts[2])
        except Exception:
            raise ValueError("--subtitle-shadow-color must be in 'B,G,R' format")

        try:
            bm_parts = [float(x) for x in args.brightness_mult.split(",")]
            if len(bm_parts) != 3:
                raise ValueError
            brightness_mult = np.array(bm_parts, dtype=np.float32).reshape(1, 1, 3)
        except Exception:
            raise ValueError("--brightness-mult must be in 'B,G,R' float format")

        includes = None
        includes_path = os.path.splitext(args.video)[0] + "_includes.json"
        if os.path.exists(includes_path):
            try:
                with open(includes_path, "r", encoding="utf-8") as f:
                    raw_includes = json.load(f)
                includes = []
                base_dir = os.path.dirname(includes_path)
                for rec in raw_includes:
                    fname = rec.get("filename", "")
                    if not fname:
                        continue
                    img_path = os.path.join(base_dir, fname)
                    img = cv2.imread(img_path, cv2.IMREAD_UNCHANGED)
                    if img is None:
                        continue
                    h = int(float(rec.get("height", 0)))
                    if h > 0:
                        scale = h / img.shape[0]
                        w = max(1, int(round(img.shape[1] * scale)))
                        img = cv2.resize(img, (w, h), interpolation=cv2.INTER_AREA)
                    pos = rec.get("position", [0.0, 0.0])
                    x = float(pos[0]) if len(pos) > 0 else 0.0
                    y = float(pos[1]) if len(pos) > 1 else 0.0
                    includes.append(
                        {
                            "image": img,
                            "x": x,
                            "y": y,
                            "start": float(rec.get("startTime", 0.0)),
                            "end": float(rec.get("endTime", 0.0)),
                        }
                    )
            except Exception as e:
                print(f"Warning: failed to load includes: {e}")

        extract_video_segments(
            precomputed_segments,
            args.video,
            temp_video,
            fps,
            font_scale=args.font_scale,
            fade_frames=args.fade_frames,
            text_height=args.text_height,
            text_height_2=text_height_2,
            subtitle_lines=args.subtitle_lines,
            visualize=args.visualize,
            temp_codec=args.temp_codec,
            crop_config=crop_config,
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
            brightness_mult=brightness_mult,
            video_shift_frames=args.video_shift_frames,
            includes=includes,
        )
    
    # Combine audio and video
    if not args.skip_audio and not args.skip_video:
        print("\n=== Combining Audio and Video ===")
        combine_video_audio(temp_video, temp_audio, args.output)
        
        # Clean up temporary files
        if os.path.exists(temp_video):
            os.remove(temp_video)
        if os.path.exists(temp_audio):
            os.remove(temp_audio)
        
        print(f"\nDone! Output saved to: {args.output}")
    elif not args.skip_video:
        # If only video was processed, re-encode temp file to output
        reencode_video(temp_video, args.output)
        print(f"\nDone! Output saved to: {args.output}")
    elif not args.skip_audio:
        # If only audio was processed, rename temp file to output
        os.rename(temp_audio, args.output.replace('.mp4', '.wav'))
        print(f"\nDone! Audio output saved to: {args.output.replace('.mp4', '.wav')}")

if __name__ == "__main__":
    main()
