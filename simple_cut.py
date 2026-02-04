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
import subtitle_render


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


def precompute_segments(word_segment_times, fps, group_size=6):
    """
    Convert word segments to frame-aligned timing.
    """
    precomputed_segments = []

    total_words = len(word_segment_times)
    for idx, seg in enumerate(tqdm(word_segment_times, desc="Precomputing segment timings")):
        start_f = seg["start"] * fps
        end_f = seg["end"] * fps
        start_frame = math.floor(start_f)
        end_frame = math.floor(end_f)

        group_start = (idx // group_size) * group_size
        group_end = min(group_start + group_size, total_words)
        group_words = [w.get("word", "") for w in word_segment_times[group_start:group_end]]
        active_idx = idx - group_start

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

 

def extract_video_segments(
    precomputed_segments,
    video_file,
    output_video_file,
    fps,
    font_scale=1.8,
    fade_frames=6,
    text_height=300,
    text_height_2=360,
    subtitle_lines=1,
    visualize=False,
    temp_codec="XVID",  # Changed from MJPG to XVID for faster writing
    crop_config=None,
    font_ttf=None,
    subtitle_bg=False,
    subtitle_bg_color=(0, 0, 0),
    subtitle_bg_alpha=0.6,
    subtitle_bg_pad=8,
    subtitle_bg_height=0,
    subtitle_bg_offset_y=0,
    shadow_size=0,
    shadow_offset_x=0,
    shadow_offset_y=0,
    video_shift_seconds=0.0,
):
    """
    Extract video segments and stitch them together with text overlay.
    Optimized for faster video writing and reduced seeking.
    """
    print("Extracting video segments (optimized)...")
    
    cap = cv2.VideoCapture(video_file)
    if not cap.isOpened():
        print(f"Error: Could not open video file {video_file}")
        return
    
    # Get video properties
    input_fps = cap.get(cv2.CAP_PROP_FPS)
    input_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    input_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    
    print(f"Input video: {input_width}x{input_height}, {input_fps:.2f} fps")
    print(f"Using codec: {temp_codec} (optimized for speed)")

    output_width = input_width
    output_height = input_height
    if crop_config is not None:
        output_width = crop_config["output_width"]
        output_height = crop_config["output_height"]
    
    # Create output video writer with optimized codec
    fourcc = cv2.VideoWriter_fourcc(*temp_codec)
    out = cv2.VideoWriter(output_video_file, fourcc, fps, (output_width, output_height))
    
    if not out.isOpened():
        print(f"Warning: Could not open video writer with codec {temp_codec}, falling back to MJPG")
        fourcc = cv2.VideoWriter_fourcc(*'MJPG')
        out = cv2.VideoWriter(output_video_file, fourcc, fps, (output_width, output_height))
    
    if visualize:
        cv2.namedWindow("Video Extraction", cv2.WINDOW_NORMAL)
    
    total_frames_processed = 0
    current_frame_idx = int(cap.get(cv2.CAP_PROP_POS_FRAMES) or 0)
    
    # Cache for overlay/mask per word group to avoid recomputation
    overlay_cache = {}
    mask_cache = {}
    bg_mask_cache = {}
    
    # Process each segment
    video_shift_frames = int(round(video_shift_seconds * fps))
    for seg in tqdm(precomputed_segments, desc="Processing video segments"):
        start_frame = seg["start_frame"] + video_shift_frames
        end_frame = seg["end_frame"] + video_shift_frames
        if end_frame <= 0:
            continue
        if start_frame < 0:
            start_frame = 0
        group_words = seg.get("group_words") or [seg.get("word", "")]
        active_idx = int(seg.get("active_idx", 0))
        
        # Create a cache key for this word group and active index
        cache_key = (
            tuple(group_words),
            active_idx,
            font_scale,
            text_height,
            text_height_2,
            subtitle_lines,
            bool(font_ttf),
            subtitle_bg,
            subtitle_bg_color,
            subtitle_bg_alpha,
            subtitle_bg_pad,
            subtitle_bg_height,
            subtitle_bg_offset_y,
            shadow_size,
            shadow_offset_x,
            shadow_offset_y,
        )
        
        # Get or create overlay and mask
        if cache_key in overlay_cache:
            overlay = overlay_cache[cache_key]
            mask = mask_cache[cache_key]
            bg_mask = bg_mask_cache[cache_key]
        else:
            overlay, mask, bg_mask = subtitle_render.render_grouped_subtitle_overlay(
                (output_height, output_width, 3),  # frame shape
                group_words,
                active_idx,
                font_scale=font_scale,
                text_height_from_bottom=text_height,
                text_height_from_bottom_2=text_height_2,
                subtitle_lines=subtitle_lines,
                font_ttf=font_ttf,
                subtitle_bg=subtitle_bg,
                subtitle_bg_color=subtitle_bg_color,
                subtitle_bg_alpha=subtitle_bg_alpha,
                subtitle_bg_pad=subtitle_bg_pad,
                subtitle_bg_full_width=True,
                subtitle_bg_height=subtitle_bg_height,
                subtitle_bg_offset_y=subtitle_bg_offset_y,
                shadow_size=shadow_size,
                shadow_offset_x=shadow_offset_x,
                shadow_offset_y=shadow_offset_y,
            )
            overlay_cache[cache_key] = overlay
            mask_cache[cache_key] = mask
            bg_mask_cache[cache_key] = bg_mask
        
        # Optimized seeking: only seek if we're more than 50 frames away
        # (reduces expensive cv2.VideoCapture.set calls)
        if start_frame < current_frame_idx or start_frame > current_frame_idx + 50:
            cap.set(cv2.CAP_PROP_POS_FRAMES, start_frame)
            current_frame_idx = start_frame
        elif start_frame > current_frame_idx:
            # Skip frames by reading and discarding (faster than seeking for small gaps)
            gap = start_frame - current_frame_idx
            for _ in range(gap):
                ret, _ = cap.read()
                if not ret:
                    break
                current_frame_idx += 1
        
        # Process frames in this segment
        for frame_idx in range(start_frame, end_frame):
            ret, frame = cap.read()
            if not ret:
                break
            current_frame_idx += 1
            
            if crop_config is not None:
                x1, y1, x2, y2 = crop_config["crop_box"]
                frame = frame[y1:y2, x1:x2]
                if frame.size == 0:
                    continue
                if (x2 - x1) != output_width or (y2 - y1) != output_height:
                    frame = cv2.resize(frame, (output_width, output_height))
            
            # Apply subtitle background (alpha blend) before text
            if subtitle_bg and subtitle_bg_alpha > 0 and bg_mask is not None:
                alpha = max(0.0, min(1.0, subtitle_bg_alpha))
                ys, xs = np.where(bg_mask > 0)
                if ys.size:
                    y1, y2 = ys.min(), ys.max() + 1
                    x1, x2 = xs.min(), xs.max() + 1
                    roi = frame[y1:y2, x1:x2]
                    roi_bg = overlay[y1:y2, x1:x2]
                    frame[y1:y2, x1:x2] = cv2.addWeighted(roi, 1.0 - alpha, roi_bg, alpha, 0)

            # Apply text overlay using cached mask
            if mask is not None:
                # Use numpy indexing for faster overlay application
                mask_indices = mask > 0
                frame[mask_indices] = overlay[mask_indices]
            
            # Write frame to output
            out.write(frame)
            total_frames_processed += 1
            
            # Visualization
            if visualize:
                cv2.imshow("Video Extraction", frame)
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break
    
    cap.release()
    out.release()
    
    if visualize:
        cv2.destroyAllWindows()
    
    print(f"Video processing complete. Processed {total_frames_processed} frames.")
    
    # Verify output
    verify = cv2.VideoCapture(output_video_file)
    output_frames = int(verify.get(cv2.CAP_PROP_FRAME_COUNT))
    verify.release()
    print(f"Output video has {output_frames} frames")
    
    return True

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
        "--text-height-2",
        type=int,
        default=360,
        help="Height of top subtitle line from bottom (default: 360)",
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
        help="Background height in pixels (default: 0 = auto)",
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
        "--fade-frames",
        type=int,
        default=6,
        help="Crossfade length for video stitching (default: 6)"
    )
    parser.add_argument(
        "--fade-samples",
        type=int,
        default=300,
        help="Crossfade length for audio stitching in samples (default: 3000)"
    )
    parser.add_argument(
        "--group-size",
        type=int,
        default=6,
        help="Number of words per subtitle group (default: 6)",
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
        "--video-shift-seconds",
        type=float,
        default=0.0,
        help="Shift video segments by this many seconds relative to audio (default: 0.0)",
    )
    parser.add_argument(
        "--extend-ms",
        type=int,
        default=20,
        help="Extend each segment end by this many ms (default: 20)",
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

    if args.ig_standard:
        args.width = 1080
        args.height = 1920
        args.subtitle_lines = 2
        args.text_height = 600 + 170
        args.text_height_2 = 700 + 170
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
    basefilename = ''.join(args.video.split('.')[:-1])
    jsonfilename = basefilename + ".json"
    audiofilename = basefilename + ".wav"

    csvfilename = jsonfilename + ".csv"
    used_csv_path = None
    if args.csv:
        used_csv_path = args.csv
        word_segment_times = load_word_segments_from_csv(args.csv)
    elif os.path.exists(csvfilename):
        used_csv_path = csvfilename
        word_segment_times = load_word_segments_from_csv(csvfilename)
    elif os.path.exists(jsonfilename):
        # Export CSV for next time, then load JSON.
        transcriptJson2csv.export_words_to_csv(jsonfilename, source_video=args.video)
        word_segment_times = segment_utils.load_word_segments_from_json(jsonfilename)
    else:
        word_segment_times = segment_utils.load_word_segments_from_json(jsonfilename)

    sr = segment_utils.get_audio_sample_rate(audiofilename)
    word_segment_times = segment_utils.align_segments_to_sample_boundaries(
        word_segment_times, sr
    )
    word_segment_times = segment_utils.fix_zero_length_words(
        word_segment_times, sr, shift_seconds=args.shift_seconds
    )
    word_segment_times = segment_utils.extend_segments(
        word_segment_times, extend_ms=args.extend_ms
    )
    
    # Output naming (prefer CSV basename when available)
    if used_csv_path:
        csv_base = os.path.splitext(os.path.basename(used_csv_path))[0]
        out_dir = os.path.dirname(args.video)
        if out_dir == "":
            out_dir = "."
        if args.output is None:
            args.output = os.path.join(out_dir, f"{csv_base}.mp4")
        temp_video = os.path.join(out_dir, f"{csv_base}_temp.mp4")
        temp_audio = os.path.join(out_dir, f"{csv_base}_temp.wav")
    else:
        if args.output is None:
            args.output = "output.mp4"
        temp_video = f"{args.video}_temp.mp4"
        temp_audio = f"{args.video}_temp.wav"
    
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
        precomputed_segments = precompute_segments(
            word_segment_times, fps, group_size=args.group_size
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

        extract_video_segments(
            precomputed_segments,
            args.video,
            temp_video,
            fps,
            font_scale=args.font_scale,
            fade_frames=args.fade_frames,
            text_height=args.text_height,
            text_height_2=args.text_height_2,
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
            video_shift_seconds=args.video_shift_seconds,
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
