import json
import time

import cv2
import numpy as np


PREVIEW_SECONDS = 15.0
PREVIEW_MAX_FRAMES = 180
PREVIEW_MAX_FRAMES_TAIL = 90


def _normalize_rotation_degrees(angle):
    angle = float(angle or 0.0) % 360.0
    if angle > 180.0:
        angle -= 360.0
    if abs(angle) < 1e-9:
        return 0.0
    return angle


def _is_right_angle_rotation(angle):
    normalized = _normalize_rotation_degrees(angle)
    nearest = round(normalized / 90.0) * 90
    return abs(normalized - nearest) < 1e-6


def _rotate_frame(frame, angle):
    angle = _normalize_rotation_degrees(angle)
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
    h, w = frame.shape[:2]
    center = (w / 2.0, h / 2.0)
    matrix = cv2.getRotationMatrix2D(center, angle, 1.0)
    cos_v = abs(matrix[0, 0])
    sin_v = abs(matrix[0, 1])
    rotated_w = max(1, int(round(w * cos_v + h * sin_v)))
    rotated_h = max(1, int(round(h * cos_v + w * sin_v)))
    matrix[0, 2] += (rotated_w / 2.0) - center[0]
    matrix[1, 2] += (rotated_h / 2.0) - center[1]
    return cv2.warpAffine(
        frame,
        matrix,
        (rotated_w, rotated_h),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REPLICATE,
    )


def _read_frame_with_fallback(cap, frame_idx, total_frames):
    target = max(0, int(round(frame_idx)))
    candidates = [target]
    for delta in (1, 2, 3, 5, 10, 15, 30):
        if target - delta >= 0:
            candidates.append(target - delta)
        if total_frames > 0 and target + delta < total_frames:
            candidates.append(target + delta)
    seen = set()
    for idx in candidates:
        if idx in seen:
            continue
        seen.add(idx)
        cap.set(cv2.CAP_PROP_POS_FRAMES, float(idx))
        ok, frame = cap.read()
        if ok and frame is not None:
            return frame, idx
    return None, None


def _format_ms(seconds):
    return f"{seconds * 1000.0:.1f} ms"


def _write_zoom_rects(path, selections):
    with open(path, "w", encoding="utf-8") as f:
        for rec in selections:
            f.write(json.dumps(rec) + "\n")


def _compute_crop_box(center, input_width, input_height, output_width, output_height, zoom):
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


def _build_preview_frames(
    cap,
    target_idx,
    total_frames,
    fps,
    rotate_degrees,
    output_width,
    output_height,
    preview_win=None,
):
    preview_build_started = time.perf_counter()
    lead_frames = max(1, int(round(PREVIEW_SECONDS * max(fps, 1.0))))
    start_idx = max(0, int(round(target_idx)) - lead_frames)
    end_idx = max(start_idx, int(round(target_idx)))
    if total_frames > 0:
        end_idx = min(end_idx, total_frames - 1)
    span = max(1, end_idx - start_idx)
    effective_max_frames = PREVIEW_MAX_FRAMES
    if total_frames > 0 and end_idx >= total_frames - 1:
        effective_max_frames = min(effective_max_frames, PREVIEW_MAX_FRAMES_TAIL)
    step = max(1, span // effective_max_frames)
    preview_frames = []
    sample_indices = list(range(start_idx, end_idx, step))
    if not sample_indices or sample_indices[-1] != end_idx:
        sample_indices.append(end_idx)
    print(
        f"[centers] building preview for target frame {int(round(target_idx))}: "
        f"start={start_idx} end={end_idx} samples={len(sample_indices)} step={step} "
        f"max_samples={effective_max_frames} "
        f"window={PREVIEW_SECONDS:.1f}s"
    )
    final_frame = None
    final_actual_idx = end_idx
    seek_started = time.perf_counter()
    cap.set(cv2.CAP_PROP_POS_FRAMES, float(start_idx))
    seek_finished = time.perf_counter()
    print(
        f"[centers] preview sequential seek to frame {start_idx} "
        f"took {_format_ms(seek_finished - seek_started)}"
    )

    sample_positions = {sample_idx: sample_no for sample_no, sample_idx in enumerate(sample_indices, start=1)}
    decode_started = time.perf_counter()
    last_sample_finished = seek_finished
    for current_idx in range(start_idx, end_idx + 1):
        read_started = time.perf_counter()
        ok, frame = cap.read()
        read_finished = time.perf_counter()
        if not ok or frame is None:
            if final_frame is None:
                print(
                    f"[centers] sequential read failed at frame {current_idx} before any preview frame "
                    f"was decoded for target frame {int(round(target_idx))}."
                )
                return None, None, None
            print(
                f"[centers] sequential read failed at frame {current_idx}; "
                f"stopping preview at last good frame {final_actual_idx}."
            )
            break

        if current_idx not in sample_positions:
            continue

        sample_no = sample_positions[current_idx]
        print(
            f"[centers] preview sample {sample_no}/{len(sample_indices)} "
            f"sequential frame {current_idx} read in {_format_ms(read_finished - read_started)} "
            f"(+{_format_ms(read_started - last_sample_finished)} since previous sample)"
        )
        preview_frame = _fit_frame_to_canvas(
            _rotate_frame(frame, rotate_degrees),
            output_width,
            output_height,
        )
        preview_frames.append(preview_frame)
        final_frame = preview_frame
        final_actual_idx = current_idx
        last_sample_finished = read_finished
        if preview_win is not None:
            imshow_started = time.perf_counter()
            cv2.imshow(preview_win, preview_frame)
            imshow_finished = time.perf_counter()
            key = cv2.waitKey(1) & 0xFF
            waitkey_finished = time.perf_counter()
            print(
                f"[centers] preview sample {sample_no}/{len(sample_indices)} display "
                f"imshow={_format_ms(imshow_finished - imshow_started)} "
                f"waitKey={_format_ms(waitkey_finished - imshow_finished)}"
            )
            if key in (27, ord("q")):
                return None, None, None

    decode_finished = time.perf_counter()
    print(
        f"[centers] preview sequential decode for frames {start_idx}-{end_idx} "
        f"took {_format_ms(decode_finished - decode_started)}"
    )
    preview_build_finished = time.perf_counter()
    print(
        f"[centers] built preview for target frame {int(round(target_idx))} "
        f"with {len(preview_frames)} frames in {_format_ms(preview_build_finished - preview_build_started)}"
    )
    return preview_frames, final_frame, final_actual_idx


def _rotate_crop(crop, angle):
    angle = _normalize_rotation_degrees(angle)
    if angle == 0.0:
        return crop
    h, w = crop.shape[:2]
    center = (w / 2.0, h / 2.0)
    matrix = cv2.getRotationMatrix2D(center, angle, 1.0)
    return cv2.warpAffine(
        crop,
        matrix,
        (w, h),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REPLICATE,
    )


def _fit_frame_to_canvas(frame, canvas_width, canvas_height):
    canvas, _meta = _fit_frame_to_canvas_with_meta(frame, canvas_width, canvas_height)
    return canvas


def _fit_frame_to_canvas_with_meta(frame, canvas_width, canvas_height):
    if frame is None or frame.size == 0:
        return frame, None
    src_h, src_w = frame.shape[:2]
    if src_w <= 0 or src_h <= 0:
        return frame, None
    scale = min(float(canvas_width) / float(src_w), float(canvas_height) / float(src_h))
    resized_w = max(1, int(round(src_w * scale)))
    resized_h = max(1, int(round(src_h * scale)))
    resized = cv2.resize(frame, (resized_w, resized_h))
    canvas = np.zeros((canvas_height, canvas_width, 3), dtype=frame.dtype)
    x = max(0, (canvas_width - resized_w) // 2)
    y = max(0, (canvas_height - resized_h) // 2)
    canvas[y : y + resized_h, x : x + resized_w] = resized
    meta = {
        "scale": scale,
        "x_off": x,
        "y_off": y,
        "resized_w": resized_w,
        "resized_h": resized_h,
        "src_w": src_w,
        "src_h": src_h,
    }
    return canvas, meta


def _compose_editor_frame(frame, center, zoom, rotate, output_width, output_height, label, transition_hint):
    x1, y1, x2, y2 = _compute_crop_box(center, frame.shape[1], frame.shape[0], output_width, output_height, zoom)
    crop = frame[y1:y2, x1:x2]
    if crop.size != 0:
        crop = cv2.resize(crop, (output_width, output_height))
    else:
        crop = frame.copy()
    crop = _rotate_crop(crop, rotate)
    cv2.putText(
        crop,
        label,
        (20, 40),
        cv2.FONT_HERSHEY_SIMPLEX,
        1.0,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    cv2.putText(
        crop,
        f"zoom={zoom:.2f}",
        (20, 80),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.8,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    cv2.putText(
        crop,
        f"rotate={rotate:.2f}",
        (20, 120),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.8,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    cv2.putText(
        crop,
        transition_hint,
        (20, 160),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    cv2.putText(
        crop,
        "Click=center, wheel/+/−=zoom, [/] or ,/.=rotate 0.25 deg, Left=rewind 5s, Backspace=delete last, q=cancel",
        (20, 200),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.65,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    return crop


def _compose_selection_frame(frame, center, zoom, output_width, output_height, label):
    draw = frame.copy()
    x1, y1, x2, y2 = _compute_crop_box(center, draw.shape[1], draw.shape[0], output_width, output_height, zoom)
    cv2.rectangle(draw, (x1, y1), (x2, y2), (0, 255, 255), 2)
    cx = int(round(center[0]))
    cy = int(round(center[1]))
    cv2.drawMarker(draw, (cx, cy), (0, 255, 255), markerType=cv2.MARKER_CROSS, markerSize=20, thickness=2)
    cv2.putText(
        draw,
        label,
        (20, 40),
        cv2.FONT_HERSHEY_SIMPLEX,
        1.0,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    fitted, meta = _fit_frame_to_canvas_with_meta(draw, output_width, output_height)
    return fitted, meta


def _compose_keyframe_overview(indices, selections, current_idx, fps, width=1280, height=760):
    canvas = np.zeros((height, width, 3), dtype=np.uint8)
    header_h = 76
    footer_h = 44
    row_h = 26
    left = 16
    completed = len(selections)
    visible_rows = max(1, (height - header_h - footer_h - 8) // row_h)
    start_row = max(0, min(current_idx - visible_rows // 2, max(0, len(indices) - visible_rows)))
    end_row = min(len(indices), start_row + visible_rows)

    cv2.putText(canvas, "Zoom Keyframes", (left, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.95, (255, 255, 255), 2, cv2.LINE_AA)
    cv2.putText(
        canvas,
        "Click a completed row or the next unsaved row to jump there",
        (left, 60),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (190, 190, 190),
        1,
        cv2.LINE_AA,
    )

    cols = {
        "idx": 18,
        "target_t": 90,
        "target_f": 190,
        "sel_f": 320,
        "zoom": 440,
        "rot": 530,
        "transition": 620,
        "status": 800,
    }
    header_y = header_h
    cv2.line(canvas, (left, header_y), (width - left, header_y), (110, 110, 110), 1)
    headers = [
        ("#", cols["idx"]),
        ("Target s", cols["target_t"]),
        ("Target frame", cols["target_f"]),
        ("Saved frame", cols["sel_f"]),
        ("Zoom", cols["zoom"]),
        ("Rotate", cols["rot"]),
        ("Transition", cols["transition"]),
        ("Status", cols["status"]),
    ]
    for text, x in headers:
        cv2.putText(canvas, text, (x, header_y - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (220, 220, 220), 1, cv2.LINE_AA)

    row_bounds = []
    for row_idx, i in enumerate(range(start_row, end_row)):
        y1 = header_h + row_idx * row_h + 4
        y2 = y1 + row_h - 2
        bg = (28, 28, 28) if row_idx % 2 == 0 else (36, 36, 36)
        status = "pending"
        text_color = (190, 190, 190)
        if i < completed:
            bg = (26, 52, 32)
            status = "saved"
            text_color = (210, 240, 210)
        elif i == completed:
            bg = (36, 56, 76)
            status = "next"
            text_color = (210, 230, 255)
        if i == current_idx:
            bg = (70, 88, 120)
            text_color = (255, 255, 255)
        cv2.rectangle(canvas, (left, y1), (width - left, y2), bg, -1)
        cv2.rectangle(canvas, (left, y1), (width - left, y2), (60, 60, 60), 1)
        frame_no = int(indices[i])
        seconds = frame_no / fps if fps else 0.0
        sel = selections[i] if i < completed else None
        saved_frame = "" if sel is None else str(int(sel.get("frame", frame_no)))
        zoom_text = "" if sel is None else f"{float(sel.get('zoom', 0.0)):.2f}"
        rotate_text = "" if sel is None else f"{float(sel.get('rotate', 0.0)):.2f}"
        transition = "" if sel is None else str(sel.get("transition", ""))

        values = [
            (str(i + 1), cols["idx"]),
            (f"{seconds:.2f}", cols["target_t"]),
            (str(frame_no), cols["target_f"]),
            (saved_frame, cols["sel_f"]),
            (zoom_text, cols["zoom"]),
            (rotate_text, cols["rot"]),
            (transition, cols["transition"]),
            (status, cols["status"]),
        ]
        baseline_y = y1 + 18
        for text, x in values:
            cv2.putText(canvas, text, (x, baseline_y), cv2.FONT_HERSHEY_SIMPLEX, 0.48, text_color, 1, cv2.LINE_AA)
        row_bounds.append({"row_index": i, "y1": y1, "y2": y2})

    footer = (
        f"Current {current_idx + 1}/{len(indices)} | Completed {completed} | "
        f"Use [ ] or , . for 0.25 deg rotation | Left Arrow rewinds 5s"
    )
    cv2.putText(canvas, footer, (left, height - 16), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1, cv2.LINE_AA)
    return canvas, row_bounds


def select_zoom_rects(
    video_path,
    frame_idx,
    output_width,
    output_height,
    count,
    default_zoom,
    frame_indices=None,
    rotate_degrees=0.0,
    initial_selections=None,
    autosave_path=None,
):
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        print(f"Error: Could not open video file {video_path}")
        return None

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    indices = list(frame_indices) if frame_indices is not None else [frame_idx] * count
    selections = [dict(rec) for rec in (initial_selections or [])]
    idx = len(selections)

    try:
        keyframe_win = "zoom keyframes"
        cv2.namedWindow(keyframe_win, cv2.WINDOW_NORMAL)
        keyframe_state = {"row_bounds": [], "clicked_row": None}

        def _on_keyframe_mouse(event, x, y, flags, param):
            if event != cv2.EVENT_LBUTTONDOWN:
                return
            for row in param["row_bounds"]:
                if row["y1"] <= y <= row["y2"]:
                    param["clicked_row"] = row["row_index"]
                    return

        cv2.setMouseCallback(keyframe_win, _on_keyframe_mouse, keyframe_state)
        while idx < len(indices):
            target_idx = indices[idx]
            stage_started = time.perf_counter()
            print(
                f"[centers] stage {idx + 1}/{len(indices)} starting for requested frame {int(round(target_idx))}"
            )
            keyframe_view, keyframe_state["row_bounds"] = _compose_keyframe_overview(indices, selections, idx, fps)
            keyframe_state["clicked_row"] = None
            cv2.imshow(keyframe_win, keyframe_view)
            preview_win = "zoom preview"
            edit_win = "zoom editor"
            cv2.namedWindow(preview_win, cv2.WINDOW_NORMAL)
            preview_frames, final_frame, actual_idx = _build_preview_frames(
                cap,
                target_idx,
                total_frames,
                fps,
                rotate_degrees,
                output_width,
                output_height,
                preview_win=preview_win,
            )
            if final_frame is None:
                cv2.destroyWindow(preview_win)
                return None
            print(
                f"[centers] stage {idx + 1}/{len(indices)} preview ready for actual frame {actual_idx} "
                f"in {_format_ms(time.perf_counter() - stage_started)}"
            )

            h, w = final_frame.shape[:2]
            if selections:
                previous = selections[-1]
                center = (
                    float(previous.get("center_x", w / 2.0)),
                    float(previous.get("center_y", h / 2.0)),
                )
                zoom = float(previous.get("zoom", default_zoom))
                rotate = float(previous.get("rotate", 0.0))
            else:
                center = (w / 2.0, h / 2.0)
                zoom = float(default_zoom)
                rotate = 0.0
            reset_center = center
            reset_zoom = zoom
            reset_rotate = rotate
            pending = {"transition": None, "rewind": False, "delete_last": False, "cancel": False, "jump_to_row": False}

            cv2.destroyWindow(preview_win)
            print(
                f"[centers] stage {idx + 1}/{len(indices)} preview playback finished in "
                f"{_format_ms(time.perf_counter() - stage_started)}"
            )

            select_win = "zoom selection"
            preview_out_win = "zoom output"
            cv2.namedWindow(preview_out_win, cv2.WINDOW_NORMAL)
            cv2.namedWindow(select_win, cv2.WINDOW_NORMAL)

            def _on_mouse(event, x, y, flags, param):
                nonlocal center, zoom, rotate
                if event == cv2.EVENT_LBUTTONDOWN:
                    sel = param
                    scale = sel.get("scale")
                    x_off = sel.get("x_off", 0)
                    y_off = sel.get("y_off", 0)
                    resized_w = sel.get("resized_w", 0)
                    resized_h = sel.get("resized_h", 0)
                    if scale and resized_w > 0 and resized_h > 0:
                        if x_off <= x <= x_off + resized_w and y_off <= y <= y_off + resized_h:
                            ox = (float(x - x_off) / float(scale))
                            oy = (float(y - y_off) / float(scale))
                            center = (float(ox), float(oy))
                elif event == cv2.EVENT_MOUSEWHEEL:
                    if flags > 0:
                        zoom *= 1.05
                    else:
                        zoom /= 1.05
                    zoom = max(0.05, zoom)

            mouse_state = {
                "scale": 1.0,
                "x_off": 0,
                "y_off": 0,
                "resized_w": output_width,
                "resized_h": output_height,
            }
            cv2.setMouseCallback(select_win, _on_mouse, mouse_state)

            while True:
                selection_view, sel_meta = _compose_selection_frame(
                    final_frame,
                    center,
                    zoom,
                    output_width,
                    output_height,
                    f"Stage {idx + 1}/{len(indices)} | target frame {actual_idx}",
                )
                if sel_meta:
                    mouse_state.update(sel_meta)
                preview = _compose_editor_frame(
                    final_frame,
                    center,
                    zoom,
                    rotate,
                    output_width,
                    output_height,
                    "Preview (output)",
                    "Enter=hard jump | Space=zoom-time pan+zoom",
                )
                keyframe_view, keyframe_state["row_bounds"] = _compose_keyframe_overview(indices, selections, idx, fps)
                cv2.imshow(keyframe_win, keyframe_view)
                cv2.imshow(preview_out_win, preview)
                if selection_view is not None:
                    cv2.imshow(select_win, selection_view)
                key = cv2.waitKeyEx(20)
                key_low = key & 0xFF
                clicked_row = keyframe_state.get("clicked_row")
                keyframe_state["clicked_row"] = None
                if clicked_row is not None:
                    allowed_max = min(len(selections), idx)
                    if clicked_row <= allowed_max:
                        if clicked_row < len(selections):
                            selections = selections[:clicked_row]
                            if autosave_path:
                                _write_zoom_rects(autosave_path, selections)
                                print(f"[centers] autosaved {len(selections)} selection(s) to: {autosave_path}")
                        idx = clicked_row
                        pending["jump_to_row"] = True
                        print(f"[centers] jumped to keyframe row {clicked_row + 1}")
                        break
                if key in (13, 10) or key_low in (13, 10):
                    pending["transition"] = "jump"
                    break
                if key == 32 or key_low == 32:
                    pending["transition"] = "pan_zoom"
                    break
                if key in (8, 127) or key_low in (8, 127):
                    pending["delete_last"] = True
                    break
                if key in (27, ord("q")) or key_low in (27, ord("q")):
                    pending["cancel"] = True
                    break
                if key in (81, 2424832):
                    pending["rewind"] = True
                    break
                if key_low in (ord("+"), ord("=")):
                    zoom *= 1.05
                if key_low in (ord("-"), ord("_")):
                    zoom /= 1.05
                    zoom = max(0.05, zoom)
                if key_low in (ord("["), ord(",")):
                    rotate -= 0.25
                if key_low in (ord("]"), ord(".")):
                    rotate += 0.25
                if key_low in (ord("r"), ord("R")):
                    center = reset_center
                    zoom = reset_zoom
                    rotate = reset_rotate

            cv2.destroyWindow(select_win)
            cv2.destroyWindow(preview_out_win)
            if pending["cancel"]:
                return None
            if pending.get("jump_to_row"):
                continue
            if pending["rewind"]:
                rewind_frames = max(1, int(round(5.0 * fps)))
                indices[idx] = max(0, int(actual_idx) - rewind_frames)
                print(
                    f"[centers] rewound stage {idx + 1} target from frame {actual_idx} "
                    f"to {indices[idx]} (-5.0s)"
                )
                continue
            if pending["delete_last"]:
                if selections:
                    removed = selections.pop()
                    if autosave_path:
                        _write_zoom_rects(autosave_path, selections)
                        print(f"[centers] autosaved {len(selections)} selection(s) to: {autosave_path}")
                    idx = max(0, idx - 1)
                    indices[idx] = int(removed.get("frame", indices[idx]))
                    print(
                        f"[centers] deleted last saved selection and returned to stage {idx + 1} "
                        f"at frame {indices[idx]}"
                    )
                else:
                    print("[centers] no saved selection to delete")
                continue
            selections.append(
                {
                    "center_x": center[0],
                    "center_y": center[1],
                    "zoom": zoom,
                    "rotate": rotate,
                    "transition": pending["transition"] or "pan_zoom",
                    "frame": int(actual_idx),
                }
            )
            if autosave_path:
                _write_zoom_rects(autosave_path, selections)
                print(f"[centers] autosaved {len(selections)} selection(s) to: {autosave_path}")
            print(
                f"[centers] stage {idx + 1}/{len(indices)} saved selection for frame {actual_idx} "
                f"after {_format_ms(time.perf_counter() - stage_started)}"
            )
            idx += 1
    finally:
        cap.release()
        cv2.destroyWindow("zoom keyframes")

    return selections
