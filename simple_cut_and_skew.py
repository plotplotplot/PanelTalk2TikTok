#!/usr/bin/env python3
"""
Simplified script to extract video and audio segments based on time periods from a CSV file.
No cropping or face tracking - output is same size as input.
"""
import argparse
import os

import cv2
import numpy as np
from tqdm import tqdm

import extract_audio_segments
import segment_utils
import simple_cut
import transcriptJson2csv


SKIN_TEMPLATES = {
    "supply_chain_security": [
        "supply_chain_security.png",
        "supply_chain_security2.png",
        "supply_chain_security3.png",
    ],
}


def resolve_skin_template(video_file, target_size):
    """
    Pick a template image based on the video name and resize to match the frame size.
    """
    video_base = os.path.basename(video_file).lower()
    for key, template_paths in SKIN_TEMPLATES.items():
        if key in video_base:
            if isinstance(template_paths, str):
                template_paths = [template_paths]
            for template_path in template_paths:
                if not os.path.exists(template_path):
                    continue
                template = cv2.imread(template_path)
                if template is None:
                    continue
                if target_size is not None:
                    target_w, target_h = target_size
                    if template.shape[1] != target_w or template.shape[0] != target_h:
                        template = cv2.resize(template, (target_w, target_h), interpolation=cv2.INTER_AREA)
                return template
    return None


def frame_has_skin(frame, template, top_rows=25, left_cols=300, max_mean_diff=12.0):
    """
    Check if the top and left bands match the template closely.
    """
    if template is None:
        return False
    h, w = frame.shape[:2]
    top_rows = min(top_rows, h)
    left_cols = min(left_cols, w)
    if top_rows <= 0 or left_cols <= 0:
        return False
    if template.shape[:2] != (h, w):
        template = cv2.resize(template, (w, h), interpolation=cv2.INTER_AREA)
    top_diff = cv2.absdiff(frame[:top_rows, :], template[:top_rows, :])
    left_diff = cv2.absdiff(frame[:, :left_cols], template[:, :left_cols])
    return float(np.mean(top_diff)) <= max_mean_diff and float(np.mean(left_diff)) <= max_mean_diff


def remove_ppt_skin(frame, inset_width=239, inset_height=140, move_x=220, move_y=190):
    """
    Move the top-right inset inward and crop the frame to remove the PPT skin.
    """
    h, w = frame.shape[:2]
    crop_left = 150
    crop_right = 150
    crop_top = 120
    crop_bottom = 60

    if h <= (crop_top + crop_bottom) or w <= (crop_left + crop_right):
        return frame

    inset_h = min(inset_height, h)
    inset_w = min(inset_width, w)
    x1 = w - inset_w
    y1 = 0
    x2 = w
    y2 = inset_h

    inset = frame[y1:y2, x1:x2].copy()
    new_x1 = max(0, x1 - move_x)
    new_y1 = min(h - inset_h, y1 + move_y)
    new_x2 = new_x1 + inset_w
    new_y2 = new_y1 + inset_h

    if new_x2 <= w and new_y2 <= h:
        frame[new_y1:new_y2, new_x1:new_x2] = inset

    return frame[crop_top:h - crop_bottom, crop_left:w - crop_right]


def fit_frame_to_output(frame, output_size):
    """
    Match output size with aspect-preserving resize and padding.
    """
    out_w, out_h = output_size
    h, w = frame.shape[:2]

    if w == 0 or h == 0 or out_w == 0 or out_h == 0:
        return frame

    scale = min(out_w / w, out_h / h)
    new_w = max(1, int(round(w * scale)))
    new_h = max(1, int(round(h * scale)))
    if new_w != w or new_h != h:
        frame = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_AREA)

    pad_left = max(0, (out_w - new_w) // 2)
    pad_right = max(0, out_w - new_w - pad_left)
    pad_top = max(0, (out_h - new_h) // 2)
    pad_bottom = max(0, out_h - new_h - pad_top)
    if pad_left or pad_right or pad_top or pad_bottom:
        frame = cv2.copyMakeBorder(
            frame, pad_top, pad_bottom, pad_left, pad_right, cv2.BORDER_CONSTANT, value=(0, 0, 0)
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
    visualize=False,
    temp_codec="XVID",  # Changed from MJPG to XVID for faster writing
    draw_border=False,
    draw_bar=False,
    bar_alpha=0.6,
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

    skin_template = resolve_skin_template(video_file, (input_width, input_height))
    if skin_template is not None:
        print("Skin template match enabled for this video.")
    
    # Create output video writer lazily once we know the processed frame size
    fourcc = cv2.VideoWriter_fourcc(*temp_codec)
    out = None
    output_size = None
    size_warning_printed = False
    
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

            skin_applied = False
            if skin_template is not None and frame_has_skin(frame, skin_template):
                frame = remove_ppt_skin(frame)
                skin_applied = True

            frame_h, frame_w = frame.shape[:2]
            if out is None:
                output_size = (frame_w, frame_h)
                out = cv2.VideoWriter(output_video_file, fourcc, fps, output_size)
                if not out.isOpened():
                    print(f"Warning: Could not open video writer with codec {temp_codec}, falling back to MJPG")
                    fallback_fourcc = cv2.VideoWriter_fourcc(*'MJPG')
                    out = cv2.VideoWriter(output_video_file, fallback_fourcc, fps, output_size)
            elif output_size != (frame_w, frame_h):
                if not size_warning_printed:
                    print("Warning: Frame size changed after skin removal; cropping/padding to match output size.")
                    size_warning_printed = True
                frame = fit_frame_to_output(frame, output_size)

            frame_h, frame_w = frame.shape[:2]
            if draw_bar:
                baseline_y = frame_h - text_height + 10
                bar_height = max(40, int(round(font_scale * 40)))
                bar_top = max(0, baseline_y - bar_height)
                bar_bottom = min(frame_h, baseline_y + int(round(font_scale * 10)))
                if bar_bottom > bar_top:
                    overlay_bar = frame.copy()
                    cv2.rectangle(
                        overlay_bar,
                        (0, bar_top),
                        (frame_w - 1, bar_bottom - 1),
                        (0, 0, 0),
                        -1,
                    )
                    alpha = float(bar_alpha)
                    frame[bar_top:bar_bottom, :] = cv2.addWeighted(
                        overlay_bar[bar_top:bar_bottom, :],
                        alpha,
                        frame[bar_top:bar_bottom, :],
                        1 - alpha,
                        0,
                    )
            cache_key = (frame_h, frame_w, tuple(group_words), active_idx, font_scale, text_height)
            if cache_key in overlay_cache:
                overlay = overlay_cache[cache_key]
                mask = mask_cache[cache_key]
            else:
                overlay, mask = simple_cut.render_grouped_subtitle_overlay(
                    (frame_h, frame_w, 3),
                    group_words,
                    active_idx,
                    font_scale=font_scale,
                    text_height_from_bottom=text_height,
                )
                overlay_cache[cache_key] = overlay
                mask_cache[cache_key] = mask
            
            # Apply text overlay using cached mask
            if mask is not None:
                # Use numpy indexing for faster overlay application
                mask_indices = mask > 0
                frame[mask_indices] = overlay[mask_indices]
            
            if draw_border:
                color = (0, 255, 0) if skin_applied else (0, 0, 255)
                cv2.rectangle(frame, (0, 0), (frame_w - 1, frame_h - 1), color, 6)

            # Write frame to output
            out.write(frame)
            total_frames_processed += 1
            
            # Visualization
            if visualize:
                cv2.imshow("Video Extraction", frame)
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break
    
    cap.release()
    if out is not None:
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
        "--no-csv",
        action="store_true",
        help="Use JSON directly (skip CSV generation)",
    )
    parser.add_argument(
        "--regen-csv",
        action="store_true",
        help="Regenerate CSV from JSON if missing (JSON read only when this is set)",
    )
    parser.add_argument(
        "--border",
        action="store_true",
        help="Draw green border when skin crop/copy is applied, red otherwise",
    )
    parser.add_argument(
        "--bar",
        action="store_true",
        help="Draw black bar behind subtitles across the full width",
    )
    parser.add_argument(
        "--bar-alpha",
        type=float,
        default=0.6,
        help="Opacity of subtitle bar (0.0-1.0, default: 0.6)",
    )
    
    args = parser.parse_args()

    # Load CSV/JSON data
    print("Loading segment data...")
    basefilename = os.path.splitext(args.video)[0]
    json_base = simple_cut.strip_standard_suffixes(basefilename)
    jsonfilename = json_base + ".json"
    audiofilename = json_base + ".wav"
    use_csv = not args.no_csv
    if use_csv or args.use_csv:
        csvfilename = jsonfilename + ".csv"
        if not os.path.exists(csvfilename):
            if args.regen_csv:
                transcriptJson2csv.export_words_to_csv(jsonfilename)
            else:
                raise FileNotFoundError(
                    f"CSV not found: {csvfilename}. Generate it first or use --regen-csv."
                )
        word_segment_times = simple_cut.load_word_segments_from_csv(csvfilename)
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
        
        # Get video FPS
        cap = cv2.VideoCapture(args.video)
        fps = cap.get(cv2.CAP_PROP_FPS)
        cap.release()
        
        # Precompute segments
        precomputed_segments = simple_cut.precompute_segments(
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
            draw_border=args.border,
            draw_bar=args.bar,
            bar_alpha=args.bar_alpha,
        )
    
    # Combine audio and video
    if not args.skip_audio and not args.skip_video:
        print("\n=== Combining Audio and Video ===")
        simple_cut.combine_video_audio(temp_video, temp_audio, args.output)
        
        # Clean up temporary files
        if os.path.exists(temp_video):
            os.remove(temp_video)
        if os.path.exists(temp_audio):
            os.remove(temp_audio)
        
        print(f"\nDone! Output saved to: {args.output}")
    elif not args.skip_video:
        # If only video was processed, re-encode temp file to output
        simple_cut.reencode_video(temp_video, args.output)
        print(f"\nDone! Output saved to: {args.output}")
    elif not args.skip_audio:
        # If only audio was processed, rename temp file to output
        os.rename(temp_audio, args.output.replace('.mp4', '.wav'))
        print(f"\nDone! Audio output saved to: {args.output.replace('.mp4', '.wav')}")

if __name__ == "__main__":
    main()
