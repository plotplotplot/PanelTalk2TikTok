import cv2
import numpy as np
from tqdm import tqdm

import subtitle_render


def extract_video_segments(
    precomputed_segments,
    video_file,
    output_video_file,
    fps,
    font_scale=1.8,
    fade_frames=6,
    text_height=300,
    text_height_2=None,
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
    subtitle_dilate=True,
    subtitle_dilate_size=10,
    subtitle_dilate_color=(0, 0, 0),
    subtitle_text_color=(255, 255, 255),
    subtitle_highlight_color=(0, 255, 255),
    subtitle_shadow_color=(0, 0, 0),
    brightness_mult=None,
    video_shift_frames=0,
    includes=None,
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
        fourcc = cv2.VideoWriter_fourcc(*"MJPG")
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
    video_shift_frames = int(video_shift_frames)
    fade_frames = max(0, int(fade_frames))
    # output_history removed; fades are now per-include only
    total_output_frames = sum(
        max(0, seg["end_frame"] - seg["start_frame"]) for seg in precomputed_segments
    )
    output_duration_s = total_output_frames / fps if fps else 0.0
    if includes and output_duration_s > 0:
        for inc in includes:
            try:
                start = float(inc.get("start", 0.0))
            except Exception:
                start = 0.0
            try:
                end = float(inc.get("end", 0.0))
            except Exception:
                end = 0.0
            if start < 0:
                start = output_duration_s + start
            if end < 0:
                end = output_duration_s + end
            start = max(0.0, min(output_duration_s, start))
            end = max(0.0, min(output_duration_s, end))
            if start > end:
                print(
                    f"Warning: include start > end after normalization "
                    f"({start:.3f}s > {end:.3f}s). Swapping."
                )
                start, end = end, start
            inc["start"] = start
            inc["end"] = end

    def _prepare_frame(frame, frame_idx):
        if crop_config is not None:
            x1, y1, x2, y2 = crop_config["crop_box"]
            frame = frame[y1:y2, x1:x2]
            if frame.size == 0:
                return None
            if (x2 - x1) != output_width or (y2 - y1) != output_height:
                frame = cv2.resize(frame, (output_width, output_height))

        if brightness_mult is not None:
            mult = brightness_mult
            frame = np.clip(frame.astype(np.float32) * mult, 0, 255).astype(frame.dtype)

        return frame

    def _apply_includes(frame, out_frame_idx):
        if not includes:
            return frame
        t = out_frame_idx / fps if fps else 0.0
        for inc in includes:
            if t < inc["start"] or t > inc["end"]:
                continue
            fade_dur = 1.0
            alpha_mult = 1.0
            fade_override = None
            if inc.get("fade") is not None:
                try:
                    fade_override = max(0.0, float(inc.get("fade", 0.0)))
                except Exception:
                    fade_override = None
            fade_in = None
            fade_out = None
            if inc.get("fade_in") is not None:
                try:
                    fade_in = max(0.0, float(inc.get("fade_in", 0.0)))
                except Exception:
                    fade_in = None
            if inc.get("fade_out") is not None:
                try:
                    fade_out = max(0.0, float(inc.get("fade_out", 0.0)))
                except Exception:
                    fade_out = None
            if fade_override is not None and fade_override >= 0:
                fade_in = fade_out = fade_override
            use_custom_fade = (fade_in is not None) or (fade_out is not None)
            if not use_custom_fade:
                if t < inc["start"] + fade_dur:
                    alpha_mult = max(0.0, min(1.0, (t - inc["start"]) / fade_dur))
                elif t > inc["end"] - fade_dur:
                    alpha_mult = max(0.0, min(1.0, (inc["end"] - t) / fade_dur))
            else:
                if fade_in and fade_in > 0:
                    if t < inc["start"] + fade_in:
                        alpha_mult = max(0.0, min(1.0, (t - inc["start"]) / fade_in))
                if fade_out and fade_out > 0:
                    if t > inc["end"] - fade_out:
                        alpha_mult = max(0.0, min(1.0, (inc["end"] - t) / fade_out))
            if inc.get("text"):
                # Lazy-render text include once per output size.
                if "text_overlay" not in inc:
                    text = str(inc.get("text", ""))
                    words = text.splitlines() if text else [""]
                    color = inc.get("color", [255, 255, 255])
                    if isinstance(color, (list, tuple)) and len(color) >= 3:
                        r, g, b = int(color[0]), int(color[1]), int(color[2])
                    else:
                        r, g, b = 255, 255, 255
                    # Render via the same subtitle renderer for consistent font/style.
                    overlay_full, mask_full, _ = subtitle_render.render_grouped_subtitle_overlay(
                        frame_shape=(output_height, output_width, 3),
                        words=words,
                        active_idx=-1,
                        font_scale=font_scale,
                        text_height_from_bottom=int(round(output_height / 2)),
                        text_height_from_bottom_2=None,
                        subtitle_lines=1,
                        font_ttf=font_ttf,
                        subtitle_bg=False,
                        subtitle_bg_color=(0, 0, 0),
                        subtitle_bg_alpha=0.0,
                        subtitle_bg_pad=0,
                        subtitle_bg_full_width=False,
                        subtitle_bg_height=0,
                        subtitle_bg_offset_y=0,
                        shadow_size=0,
                        shadow_offset_x=0,
                        shadow_offset_y=0,
                        dilate_size=subtitle_dilate_size,
                        dilate_color=subtitle_dilate_color,
                        text_color=(b, g, r),
                        highlight_color=(b, g, r),
                        shadow_color=(0, 0, 0),
                    )
                    ys, xs = np.where(mask_full > 0)
                    if ys.size == 0 or xs.size == 0:
                        inc["text_overlay"] = None
                        inc["text_mask"] = None
                    else:
                        y1, y2 = ys.min(), ys.max() + 1
                        x1, x2 = xs.min(), xs.max() + 1
                        inc["text_overlay"] = overlay_full[y1:y2, x1:x2]
                        inc["text_mask"] = mask_full[y1:y2, x1:x2]

                img = inc.get("text_overlay")
                mask = inc.get("text_mask")
                if img is None or mask is None:
                    continue
                ih, iw = img.shape[:2]
            else:
                img = inc.get("image")
                if img is None:
                    continue
                ih, iw = img.shape[:2]

            x = int(round(inc["x"] * output_width - iw / 2.0))
            y = int(round(inc["y"] * output_height - ih / 2.0))
            x2 = min(output_width, x + iw)
            y2 = min(output_height, y + ih)
            if x2 <= 0 or y2 <= 0 or x >= output_width or y >= output_height:
                continue
            sx1 = max(0, -x)
            sy1 = max(0, -y)
            sx2 = sx1 + (x2 - max(0, x))
            sy2 = sy1 + (y2 - max(0, y))
            dx1 = max(0, x)
            dy1 = max(0, y)
            dx2 = dx1 + (sx2 - sx1)
            dy2 = dy1 + (sy2 - sy1)
            inc_overlay = img[sy1:sy2, sx1:sx2]

            if inc.get("text"):
                inc_mask = mask[sy1:sy2, sx1:sx2]
                if inc_mask.ndim == 2:
                    inc_mask = inc_mask[:, :, None] / 255.0
                alpha = inc_mask * alpha_mult
                base = frame[dy1:dy2, dx1:dx2]
                frame[dy1:dy2, dx1:dx2] = (
                    alpha * inc_overlay + (1 - alpha) * base
                ).astype(base.dtype)
                continue

            if inc_overlay.shape[2] == 4:
                alpha = (inc_overlay[:, :, 3:4] / 255.0) * alpha_mult
                base = frame[dy1:dy2, dx1:dx2]
                frame[dy1:dy2, dx1:dx2] = (
                    alpha * inc_overlay[:, :, :3] + (1 - alpha) * base
                ).astype(base.dtype)
            else:
                if alpha_mult >= 0.999:
                    frame[dy1:dy2, dx1:dx2] = inc_overlay
                else:
                    base = frame[dy1:dy2, dx1:dx2]
                    frame[dy1:dy2, dx1:dx2] = (
                        alpha_mult * inc_overlay + (1 - alpha_mult) * base
                    ).astype(base.dtype)
        return frame

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
            subtitle_dilate,
            subtitle_dilate_size,
            subtitle_dilate_color,
            subtitle_text_color,
            subtitle_highlight_color,
            subtitle_shadow_color,
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
                dilate_size=subtitle_dilate_size if subtitle_dilate else 0,
                dilate_color=subtitle_dilate_color,
                text_color=subtitle_text_color,
                highlight_color=subtitle_highlight_color,
                shadow_color=subtitle_shadow_color,
            )
            overlay_cache[cache_key] = overlay
            mask_cache[cache_key] = mask
            bg_mask_cache[cache_key] = bg_mask

        # Optimized seeking: only seek if we're more than 50 frames away
        # (reduces expensive cv2.VideoCapture.set calls)
        if start_frame < current_frame_idx or start_frame > current_frame_idx + 50:
            cap.set(cv2.CAP_PROP_POS_FRAMES, start_frame)
            current_frame_idx = start_frame
            # Segment jump
        elif start_frame > current_frame_idx:
            # Skip frames by reading and discarding (faster than seeking for small gaps)
            gap = start_frame - current_frame_idx
            for _ in range(gap):
                ret, skipped = cap.read()
                if not ret:
                    break
                current_frame_idx += 1

        seg_len = max(0, end_frame - start_frame)
        if seg_len == 0:
            continue

        # No global/segment fade; include fades handled in _apply_includes

        # Process frames in this segment
        for frame_idx in range(start_frame, end_frame):
            ret, frame = cap.read()
            if not ret:
                break
            current_frame_idx += 1

            frame = _prepare_frame(frame, frame_idx)
            if frame is None:
                continue

            # Apply subtitle background (alpha blend) before text
            if subtitle_bg and subtitle_bg_alpha > 0 and bg_mask is not None:
                alpha_bg = max(0.0, min(1.0, subtitle_bg_alpha))
                ys, xs = np.where(bg_mask > 0)
                if ys.size:
                    y1, y2 = ys.min(), ys.max() + 1
                    x1, x2 = xs.min(), xs.max() + 1
                    roi = frame[y1:y2, x1:x2]
                    roi_bg = overlay[y1:y2, x1:x2]
                    frame[y1:y2, x1:x2] = cv2.addWeighted(
                        roi, 1.0 - alpha_bg, roi_bg, alpha_bg, 0
                    )

            # Apply text overlay using cached mask
            if mask is not None:
                mask_indices = mask > 0
                frame[mask_indices] = overlay[mask_indices]
            frame = _apply_includes(frame, total_frames_processed)
            out.write(frame)
            total_frames_processed += 1
            # No global/segment fade history

            # Visualization
            if visualize:
                cv2.imshow("Video Extraction", frame)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break

        # Keep output history across segments for consistent output-timeline fades

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
