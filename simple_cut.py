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

import extract_audio_segments
import segment_utils
import transcriptJson2csv


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

def render_grouped_subtitle_overlay(
    frame_shape,
    words,
    active_idx,
    font_scale=1.8,
    text_height_from_bottom=300,
):
    """
    Render grouped subtitle text once and return (overlay, mask).
    """
    height, width = frame_shape[:2]
    thickness = int(font_scale * 2)
    font = cv2.FONT_HERSHEY_SIMPLEX

    safe_words = []
    for w in words:
        if w is None:
            safe_words.append("")
        elif isinstance(w, float) and (np.isnan(w) or np.isinf(w)):
            safe_words.append("")
        else:
            safe_words.append(str(w))

    word_sizes = [cv2.getTextSize(w, font, font_scale, thickness)[0] for w in safe_words]
    space_width = cv2.getTextSize(" ", font, font_scale, thickness)[0][0]
    total_width = sum(w[0] for w in word_sizes) + space_width * max(0, len(safe_words) - 1)

    x = (width - total_width) // 2
    y = height - text_height_from_bottom

    overlay = np.zeros((height, width, 3), dtype=np.uint8)
    mask = np.zeros((height, width), dtype=np.uint8)

    shadow_offset = 3
    for i, word in enumerate(safe_words):
        if not word and i != len(safe_words) - 1:
            x += space_width
            continue
        color = (0, 255, 255) if i == active_idx else (255, 255, 255)

        # Shadow
        cv2.putText(
            overlay,
            word,
            (x + shadow_offset, y + shadow_offset),
            font,
            font_scale,
            (0, 0, 0),
            thickness,
            cv2.LINE_AA,
        )
        # Text
        cv2.putText(
            overlay,
            word,
            (x, y),
            font,
            font_scale,
            color,
            thickness,
            cv2.LINE_AA,
        )
        # Mask for text area (including shadow)
        cv2.putText(
            mask,
            word,
            (x + shadow_offset, y + shadow_offset),
            font,
            font_scale,
            255,
            thickness,
            cv2.LINE_AA,
        )
        cv2.putText(
            mask,
            word,
            (x, y),
            font,
            font_scale,
            255,
            thickness,
            cv2.LINE_AA,
        )
        x += word_sizes[i][0] + space_width

    return overlay, mask

def extract_video_segments(
    precomputed_segments,
    video_file,
    output_video_file,
    fps,
    font_scale=1.8,
    fade_frames=6,
    text_height=300,
    visualize=False,
    temp_codec="XVID",  # Changed from MJPG to XVID for faster writing
    crop_config=None,
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
    
    # Process each segment
    for seg in tqdm(precomputed_segments, desc="Processing video segments"):
        start_frame = seg["start_frame"]
        end_frame = seg["end_frame"]
        group_words = seg.get("group_words") or [seg.get("word", "")]
        active_idx = int(seg.get("active_idx", 0))
        
        # Create a cache key for this word group and active index
        cache_key = (tuple(group_words), active_idx, font_scale, text_height)
        
        # Get or create overlay and mask
        if cache_key in overlay_cache:
            overlay = overlay_cache[cache_key]
            mask = mask_cache[cache_key]
        else:
            overlay, mask = render_grouped_subtitle_overlay(
                (output_height, output_width, 3),  # frame shape
                group_words,
                active_idx,
                font_scale=font_scale,
                text_height_from_bottom=text_height,
            )
            overlay_cache[cache_key] = overlay
            mask_cache[cache_key] = mask
        
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
        default="output.mp4",
        help="Output video file (default: output.mp4)",
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
    
    args = parser.parse_args()

    # Load CSV/JSON data
    print("Loading segment data...")
    basefilename = ''.join(args.video.split('.')[:-1])
    jsonfilename = basefilename + ".json"
    audiofilename = basefilename + ".wav"

    if args.csv:
        word_segment_times = load_word_segments_from_csv(args.csv)
    elif args.use_csv:
        csvfilename = jsonfilename + ".csv"
        if not os.path.exists(csvfilename):
            transcriptJson2csv.export_words_to_csv(jsonfilename)
        word_segment_times = load_word_segments_from_csv(csvfilename)
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
    
    # Create temporary files
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
        extract_video_segments(
            precomputed_segments,
            args.video,
            temp_video,
            fps,
            font_scale=args.font_scale,
            fade_frames=args.fade_frames,
            text_height=args.text_height,
            visualize=args.visualize,
            temp_codec=args.temp_codec,
            crop_config=crop_config,
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
