
def preprocess_injections(injections_list, output_width, output_height):
    """
    Preprocess injection images by loading them from disk and precomputing
    their scaled versions for the target output dimensions.
    This avoids repeated disk I/O and image processing during frame processing.
    """
    if not injections_list:
        return []
    
    processed_injections = []
    for inj in injections_list:
        filename = inj["filename"]
        if not os.path.exists(filename):
            print(f"⚠️ Warning: Injection image '{filename}' not found, skipping")
            continue
            
        # Load image once
        img = cv2.imread(filename, cv2.IMREAD_UNCHANGED)
        if img is None:
            print(f"⚠️ Warning: Could not load injection image '{filename}', skipping")
            continue
            
        # Ensure BGRA format
        if img.ndim == 2:
            img = cv2.cvtColor(img, cv2.COLOR_GRAY2BGRA)
        elif img.shape[2] == 3:
            bgr = img
            alpha = np.full((img.shape[0], img.shape[1], 1), 255, dtype=np.uint8)
            img = np.concatenate([bgr, alpha], axis=2)
        
        # Store the preloaded image and other metadata
        processed_inj = inj.copy()
        processed_inj["image"] = img
        processed_injections.append(processed_inj)
    
    return processed_injections


def overlay_injections_on_frame(frame, current_time, injections, fps=30.0):
    """
    Overlay any active injection images onto `frame` (BGR uint8) based on current_time.
    - injections: list of dicts produced by preprocess_injections (each has 'start','end','image')
    - fps: frames per second for fade calculations
    Returns a new BGR uint8 frame with overlays applied (alpha composited).
    """
    if not injections:
        return frame
    out = frame.astype(np.float32)
    h, w = frame.shape[:2]
    for inj in injections:
        start = inj.get("start")
        end = inj.get("end")
        if start is None or end is None:
            continue
        if current_time < start or current_time > end:
            continue
        
        # Use preloaded image instead of reading from disk
        img = inj.get("image")
        if img is None:
            continue

        # Calculate fade alpha based on fade_in_frames and fade_out_frames
        fade_in_frames = inj.get("fade_in_frames", 0)
        fade_out_frames = inj.get("fade_out_frames", 0)
        
        # Calculate fade alpha (0.0 to 1.0)
        fade_alpha = 1.0
        
        # Fade in calculation
        if fade_in_frames > 0 and current_time < start + (fade_in_frames / fps):
            fade_progress = (current_time - start) * fps / fade_in_frames
            fade_alpha = min(fade_alpha, max(0.0, fade_progress))
        
        # Fade out calculation
        if fade_out_frames > 0 and current_time > end - (fade_out_frames / fps):
            fade_progress = (end - current_time) * fps / fade_out_frames
            fade_alpha = min(fade_alpha, max(0.0, fade_progress))
        
        # Skip if completely faded out
        if fade_alpha <= 0.0:
            continue

        ih, iw = img.shape[:2]
        
        # Get positioning parameters (default to center if not specified)
        center_x = inj.get("center_x", w // 2)
        center_y = inj.get("center_y", h // 2)
        zoom = inj.get("zoom", 1.0)
        
        # Apply zoom factor to scale calculation
        base_scale = min(float(w) / float(iw), float(h) / float(ih))
        scale = base_scale * zoom
        
        new_w = max(1, int(round(iw * scale)))
        new_h = max(1, int(round(ih * scale)))
        interp = cv2.INTER_AREA if scale < 1.0 else cv2.INTER_LINEAR
        resized = cv2.resize(img, (new_w, new_h), interpolation=interp)

        # Create transparent canvas and position the resized image based on center_x, center_y
        canvas = np.zeros((h, w, 4), dtype=np.uint8)
        x = int(center_x - new_w // 2)
        y = int(center_y - new_h // 2)
        
        # Ensure the image stays within frame boundaries
        x = max(0, min(x, w - new_w))
        y = max(0, min(y, h - new_h))
        
        # Calculate the actual region where we can place the image
        # This handles cases where the resized image is larger than the available space
        canvas_y_start = max(0, y)
        canvas_y_end = min(h, y + new_h)
        canvas_x_start = max(0, x)
        canvas_x_end = min(w, x + new_w)
        
        # Calculate corresponding regions in the resized image
        resized_y_start = max(0, -y)
        resized_y_end = resized_y_start + (canvas_y_end - canvas_y_start)
        resized_x_start = max(0, -x)
        resized_x_end = resized_x_start + (canvas_x_end - canvas_x_start)
        
        # Ensure we don't exceed the resized image dimensions
        resized_y_end = min(new_h, resized_y_end)
        resized_x_end = min(new_w, resized_x_end)
        
        # Only place the portion of the resized image that fits in the canvas
        if (canvas_y_end > canvas_y_start and canvas_x_end > canvas_x_start and
            resized_y_end > resized_y_start and resized_x_end > resized_x_start):
            canvas[canvas_y_start:canvas_y_end, canvas_x_start:canvas_x_end, :] = \
                resized[resized_y_start:resized_y_end, resized_x_start:resized_x_end, :]

        # Apply alpha compositing with fade alpha
        bgr = canvas[..., :3].astype(np.float32)
        alpha = canvas[..., 3].astype(np.float32) / 255.0
        alpha = alpha[:, :, None] * fade_alpha  # Apply fade alpha
        out = bgr * alpha + out * (1.0 - alpha)
    out = np.clip(out, 0, 255).astype(np.uint8)
    return out


def extract_cropped_segment(
    frame,
    speaker_box,
    output_width,
    output_height,
    last_center=None,
    last_zoom=1.0,
    y_offset=100,
    zoom_speed=0.02,
):
    """
    Extract a cropped segment from the frame centered on the speaker's bounding box
    with optional smooth zooming toward the speaker.

    Behavior:
      - If `speaker_box` is provided, compute a desired zoom so that the speaker's
        bounding box occupies a fixed fraction of the output frame, then smoothly
        interpolate the zoom level from `last_zoom` toward `desired_zoom` using
        `zoom_speed`.
      - If `speaker_box` is None (non-speaker frame), do NOT zoom: zoom is reset
        immediately to 1.0 (no magnification).
      - Movement smoothing for the center is preserved (same small/medium/big
        thresholds as before).

    Returns:
        cropped_frame: The cropped image (may be smaller than output; caller should
                       resize to output_width/output_height)
        center: (center_x, center_y) used for the crop
        new_zoom: the updated zoom value to carry forward for the next frame
    """
    # Validate speaker_box presence (if empty or None we will not zoom)
    has_speaker = speaker_box is not None

    # Determine target center from speaker box if available
    if has_speaker:
        target_center_x = (speaker_box[0] + speaker_box[2]) / 2
        target_center_y = (speaker_box[1] + speaker_box[3]) / 2 + y_offset
    else:
        # If no speaker box provided, default target is last_center or center of frame
        frame_h, frame_w = frame.shape[:2]
        target_center_x = last_center[0] if last_center is not None else frame_w / 2
        target_center_y = last_center[1] if last_center is not None else frame_h / 2

    # Movement smoothing (reuse existing thresholds)
    screen_diagonal = np.sqrt(output_width**2 + output_height**2)
    small_movement_threshold = 0.01 * screen_diagonal  # 1% of diagonal
    big_movement_threshold = 0.20 * screen_diagonal  # 20% of diagonal

    if last_center is not None:
        last_center_x, last_center_y = last_center

        # Distance to target
        distance = np.sqrt(
            (target_center_x - last_center_x) ** 2
            + (target_center_y - last_center_y) ** 2
        )

        if distance > 0:
            if distance <= small_movement_threshold:
                center_x, center_y = target_center_x, target_center_y
            elif distance >= big_movement_threshold:
                center_x, center_y = target_center_x, target_center_y
            else:
                direction_x = (target_center_x - last_center_x) / distance
                direction_y = (target_center_y - last_center_y) / distance
                center_x = last_center_x + direction_x * small_movement_threshold
                center_y = last_center_y + direction_y * small_movement_threshold
        else:
            center_x, center_y = last_center_x, last_center_y
    else:
        center_x, center_y = target_center_x, target_center_y

    # Get frame dims
    frame_height, frame_width = frame.shape[:2]

    # Zoom computation
    if has_speaker:
        box_w = max(1.0, float(speaker_box[2] - speaker_box[0]))
        # fraction of output width we want the box to occupy (tweakable)
        target_fraction = 0.45
        # desired zoom = how much the final frame should magnify the box
        # S = output_width / crop_width and we want box_w / crop_width == target_fraction
        # => crop_width = box_w / target_fraction => S = output_width * target_fraction / box_w
        desired_zoom = (output_width * target_fraction) / box_w
        desired_zoom = float(max(1.0, min(desired_zoom, 3.0)))  # clamp reasonable zoom
        if last_zoom is None:
            last_zoom = 1.0
        # Smoothly interpolate zoom toward desired_zoom
        new_zoom = last_zoom + (desired_zoom - last_zoom) * float(zoom_speed)
    else:
        # Don't zoom on non-speakers: reset immediately
        new_zoom = 1.0

    # Compute crop dimensions based on zoom (smaller crop -> more zoom when resized to output)
    crop_w = int(round(output_width / new_zoom))
    crop_h = int(round(output_height / new_zoom))

    # Fallback minimums and maximums
    crop_w = max(2, min(crop_w, frame_width))
    crop_h = max(2, min(crop_h, frame_height))

    # Calculate crop box from center
    crop_x1 = int(center_x - crop_w / 2)
    crop_y1 = int(center_y - crop_h / 2)
    crop_x2 = crop_x1 + crop_w
    crop_y2 = crop_y1 + crop_h

    # Adjust crop boundaries if they go outside frame boundaries
    if crop_x1 < 0:
        crop_x2 -= crop_x1
        crop_x1 = 0
    if crop_y1 < 0:
        crop_y2 -= crop_y1
        crop_y1 = 0
    if crop_x2 > frame_width:
        crop_x1 -= crop_x2 - frame_width
        crop_x2 = frame_width
    if crop_y2 > frame_height:
        crop_y1 -= crop_y2 - frame_height
        crop_y2 = frame_height

    # Ensure boundaries are within frame
    crop_x1 = max(0, crop_x1)
    crop_y1 = max(0, crop_y1)
    crop_x2 = min(frame_width, crop_x2)
    crop_y2 = min(frame_height, crop_y2)

    # Extract cropped region
    cropped_frame = frame[crop_y1:crop_y2, crop_x1:crop_x2]

    return cropped_frame, (center_x, center_y), new_zoom
