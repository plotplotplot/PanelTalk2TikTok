#!/usr/bin/env python3
import argparse
import json
import math
from pathlib import Path

import cv2
import numpy as np
from grading import apply_grading

try:
    from stabilize_instance_continuation import assign_track_ids, merge_duplicate_centers
except Exception:
    def merge_duplicate_centers(centers):
        return centers, []

    def assign_track_ids(centers, prev_tracks, max_dist=120.0, frame_idx=0, max_gap=5):
        assigned_ids = list(range(len(centers)))
        return assigned_ids, {}


def load_detections(jsonl_path: Path):
    detections_by_frame = {}
    if not jsonl_path.exists():
        return detections_by_frame
    with jsonl_path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            frame = rec.get("frame", None)
            if frame is None:
                continue
            try:
                frame = int(frame)
            except (TypeError, ValueError):
                continue
            detections_by_frame.setdefault(frame, []).append(rec)
    return detections_by_frame


def _normalize_instance_id(instance_id):
    if isinstance(instance_id, str):
        s = instance_id.strip()
        if s.isdigit():
            try:
                return int(s)
            except Exception:
                return instance_id
        return instance_id
    return instance_id


def _instance_matches(rec_id, instance_id):
    if rec_id == instance_id:
        return True
    if isinstance(rec_id, str) and isinstance(instance_id, int):
        return rec_id.strip().isdigit() and int(rec_id.strip()) == instance_id
    if isinstance(rec_id, int) and isinstance(instance_id, str):
        return instance_id.strip().isdigit() and int(instance_id.strip()) == rec_id
    return False


def _prompt_for_instance(candidates, frame, last_frame, max_absence):
    if not candidates:
        return None, None
    ids = []
    for rec in candidates:
        rec_id = rec.get("instance_id")
        if rec_id is not None:
            ids.append(str(rec_id).strip())
    uniq_ids = sorted(set(ids))
    print(
        f"No continuation after {max_absence} frames at frame {frame} "
        f"(last detection frame {last_frame})."
    )
    if uniq_ids:
        print(f"Available instance_ids at frame {frame}:")
        rows = []
        for rec in candidates:
            rec_id = rec.get("instance_id")
            rx = rec.get("center_x")
            ry = rec.get("center_y")
            if rec_id is None or rx is None or ry is None:
                continue
            try:
                rx_f = float(rx)
                ry_f = float(ry)
            except (TypeError, ValueError):
                continue
            rows.append((rx_f, str(rec_id).strip(), ry_f))
        rows.sort(key=lambda r: r[0])
        for rx_f, rec_id, ry_f in rows:
            print(f"  - {rec_id}: ({rx_f:.2f}, {ry_f:.2f})")
    else:
        print(f"No instance_ids available at frame {frame}.")
    while True:
        user_val = input("Enter new instance_id to continue (blank to abort): ").strip()
        if not user_val:
            return None, None
        user_id = _normalize_instance_id(user_val)
        matches = [rec for rec in candidates if _instance_matches(rec.get("instance_id"), user_id)]
        if matches:
            return user_id, matches[0]
        print(f"instance_id {user_val} not found in frame {frame}. Try again.")


def _processed_frames_dir(video_path: Path) -> Path:
    return video_path.parent / f"{video_path.stem}_frames_out"


def _load_processed_frame(video_path: Path, frame_idx: int):
    frames_dir = _processed_frames_dir(video_path)
    if not frames_dir.exists():
        return None
    name = f"frame_{frame_idx + 1:06d}"
    for ext in ("png", "jpg"):
        path = frames_dir / f"{name}.{ext}"
        if path.exists():
            img = cv2.imread(str(path))
            if img is not None:
                return img
    return None


def _load_frame_from_video(video_path: Path, frame_idx: int):
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        return None
    try:
        cap.set(cv2.CAP_PROP_POS_FRAMES, float(frame_idx))
        ok, frame = cap.read()
        if not ok:
            return None
        return frame
    finally:
        cap.release()


def _select_instance_with_click(
    candidates,
    video_path: Path,
    frame_idx: int,
    default_rec=None,
    allow_keep: bool = False,
    last_center=None,
    current_center=None,
    click_max_dist: float = 30.0,
    manual_hold_frames: int = 100,
):
    img = _load_processed_frame(video_path, frame_idx)
    if img is None:
        img = _load_frame_from_video(video_path, frame_idx)
    if img is None:
        return None, None

    draw = img.copy()
    for rec in candidates:
        rx = rec.get("center_x")
        ry = rec.get("center_y")
        rec_id = rec.get("instance_id")
        if rx is None or ry is None:
            continue
        try:
            rx_f = float(rx)
            ry_f = float(ry)
        except (TypeError, ValueError):
            continue
        cv2.circle(draw, (int(rx_f), int(ry_f)), 10, (0, 255, 255), 2)
        cv2.putText(
            draw,
            str(rec_id),
            (int(rx_f) + 12, int(ry_f) - 6),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.6,
            (0, 255, 255),
            2,
            cv2.LINE_AA,
        )

    display_center = current_center if current_center is not None else last_center
    if display_center is not None:
        lx, ly = int(display_center[0]), int(display_center[1])
        cv2.circle(draw, (lx, ly), 8, (0, 0, 255), -1)
        cv2.putText(
            draw,
            "current",
            (lx + 10, ly + 10),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.6,
            (0, 0, 255),
            2,
            cv2.LINE_AA,
        )

    if default_rec is not None:
        dx = default_rec.get("center_x")
        dy = default_rec.get("center_y")
        if dx is not None and dy is not None:
            cv2.circle(draw, (int(dx), int(dy)), 14, (255, 0, 0), 2)

    instr = "Click to select instance (or empty space to track point)"
    if allow_keep:
        instr += " | Right arrow = keep center"
    cv2.putText(
        draw,
        instr,
        (20, 30),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )

    h, w = draw.shape[:2]
    max_dim = 1280
    scale = 1.0
    if max(h, w) > max_dim:
        scale = max_dim / float(max(h, w))
    scale *= 3.0
    if scale != 1.0:
        disp_w = max(1, int(round(w * scale)))
        disp_h = max(1, int(round(h * scale)))
        disp = cv2.resize(draw, (disp_w, disp_h), interpolation=cv2.INTER_AREA)
    else:
        disp_w, disp_h = w, h
        disp = draw

    win = "select instance"
    selected = {"rec": None}

    def _on_mouse(event, x, y, flags, param):
        if event != cv2.EVENT_LBUTTONDOWN:
            return
        ox = int(x / scale)
        oy = int(y / scale)
        best = None
        best_dist = None
        for rec in candidates:
            rx = rec.get("center_x")
            ry = rec.get("center_y")
            if rx is None or ry is None:
                continue
            try:
                rx_f = float(rx)
                ry_f = float(ry)
            except (TypeError, ValueError):
                continue
            dist = math.hypot(rx_f - ox, ry_f - oy)
            if best_dist is None or dist < best_dist:
                best_dist = dist
                best = rec
        if best is not None and best_dist is not None and best_dist <= float(click_max_dist):
            selected["rec"] = best
        else:
            selected["rec"] = {
                "center_x": float(ox),
                "center_y": float(oy),
                "instance_id": "manual",
                "_manual": True,
                "_manual_frames": int(manual_hold_frames),
            }

    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(win, disp_w, disp_h)
    cv2.setMouseCallback(win, _on_mouse)
    while True:
        cv2.imshow(win, disp)
        key = cv2.waitKey(20)
        if selected["rec"] is not None:
            cv2.destroyWindow(win)
            return "select", selected["rec"]
        if key in (13, 10):  # enter
            if allow_keep:
                cv2.destroyWindow(win)
                return "keep", default_rec
        if key in (83, 2555904):  # right arrow
            if allow_keep:
                cv2.destroyWindow(win)
                return "keep", default_rec
        if key in (27, ord("q")):
            cv2.destroyWindow(win)
            return "abort", None


def apply_lowpass_filter(records, lowpass_alpha: float):
    # Symmetric low-pass (forward-backward) to avoid phase delay
    alpha = float(lowpass_alpha)
    if alpha < 0.0:
        alpha = 0.0
    if alpha > 1.0:
        alpha = 1.0
    if alpha <= 0.0 or len(records) <= 1:
        return records

    fwd = []
    prev_x = None
    prev_y = None
    for rec in records:
        x = rec.get("center_x")
        y = rec.get("center_y")
        if x is None or y is None:
            fwd.append(dict(rec))
            continue
        x = float(x)
        y = float(y)
        if prev_x is None:
            fx, fy = x, y
        else:
            fx = alpha * x + (1.0 - alpha) * prev_x
            fy = alpha * y + (1.0 - alpha) * prev_y
        prev_x, prev_y = fx, fy
        out_rec = dict(rec)
        out_rec["center_x"] = fx
        out_rec["center_y"] = fy
        fwd.append(out_rec)

    bwd = []
    prev_x = None
    prev_y = None
    for rec in reversed(fwd):
        x = rec.get("center_x")
        y = rec.get("center_y")
        if x is None or y is None:
            bwd.append(dict(rec))
            continue
        x = float(x)
        y = float(y)
        if prev_x is None:
            fx, fy = x, y
        else:
            fx = alpha * x + (1.0 - alpha) * prev_x
            fy = alpha * y + (1.0 - alpha) * prev_y
        prev_x, prev_y = fx, fy
        out_rec = dict(rec)
        out_rec["center_x"] = fx
        out_rec["center_y"] = fy
        bwd.append(out_rec)
    bwd.reverse()

    combined = []
    for a, b in zip(fwd, bwd):
        if a.get("center_x") is None or a.get("center_y") is None:
            combined.append(dict(a))
            continue
        out_rec = dict(a)
        out_rec["center_x"] = (float(a["center_x"]) + float(b["center_x"])) / 2.0
        out_rec["center_y"] = (float(a["center_y"]) + float(b["center_y"])) / 2.0
        combined.append(out_rec)
    return combined


def preprocess_continuous(
    jsonl_path: Path,
    instance_id,
    out_path: Path,
    max_absence: int = 5,
    radius_per_frame: float = 50.0,
    average_frame_count: int = 7,
    lowpass_alpha: float = 0.25,
    fix_y: bool = False,
    time_shift: int = 0,
    start_frame: int | None = None,
    end_frame: int | None = None,
    select_interval: int = 300,
    manual_hold_frames: int = 100,
    click_max_dist: float = 30.0,
    video_path: Path | None = None,
):
    detections_by_frame = load_detections(jsonl_path)
    if not detections_by_frame:
        print(
            f"No detections found. JSONL path checked: {jsonl_path} (exists={jsonl_path.exists()})"
        )
        raise RuntimeError("No detections found in JSONL.")

    frames = sorted(detections_by_frame.keys())
    if start_frame is None:
        first_frame = frames[0]
    else:
        first_frame = int(start_frame)
        if first_frame not in detections_by_frame:
            raise RuntimeError(
                f"start_frame {first_frame} not found in detections (range {frames[0]}..{frames[-1]})."
            )
    if end_frame is not None:
        end_frame = int(end_frame)
        if end_frame < first_frame:
            raise RuntimeError(
                f"end_frame {end_frame} must be >= start_frame {first_frame}."
            )
    first_frame_recs = detections_by_frame[first_frame]
    start_candidates = [
        rec for rec in first_frame_recs if _instance_matches(rec.get("instance_id"), instance_id)
    ]
    default_rec = start_candidates[0] if start_candidates else None
    current = None
    if video_path is not None:
        action, rec = _select_instance_with_click(
            first_frame_recs,
            video_path,
            first_frame,
            default_rec=default_rec,
            allow_keep=default_rec is not None,
            last_center=(float(default_rec["center_x"]), float(default_rec["center_y"]))
            if default_rec and default_rec.get("center_x") is not None and default_rec.get("center_y") is not None
            else None,
            click_max_dist=click_max_dist,
            manual_hold_frames=manual_hold_frames,
        )
        if action == "select":
            current = rec
            instance_id = _normalize_instance_id(rec.get("instance_id"))
        elif action == "keep" and default_rec is not None:
            current = default_rec
        elif action == "abort":
            raise RuntimeError("Selection aborted.")
    if current is None:
        if not start_candidates:
            available_ids = []
            for rec in first_frame_recs:
                rec_id = rec.get("instance_id")
                if rec_id not in available_ids:
                    available_ids.append(rec_id)
            if len(available_ids) == 1 and first_frame_recs:
                chosen = first_frame_recs[0]
                instance_id = _normalize_instance_id(chosen.get("instance_id"))
                print(
                    f"instance_id {instance_id} auto-selected (only option in frame {first_frame})."
                )
                current = chosen
            else:
                raise RuntimeError(
                    f"No instance_id {instance_id} found in first frame ({first_frame})."
                )
        else:
            current = start_candidates[0]
    cx = current.get("center_x")
    cy = current.get("center_y")
    if cx is None or cy is None:
        raise RuntimeError("Starting detection missing center_x/center_y.")
    last_center = (float(cx), float(cy))
    last_frame = first_frame
    miss_count = 0
    frames_since_selection = 0
    manual_left = int(current.get("_manual_frames", 0)) if isinstance(current, dict) else 0

    output = [current]

    start_idx = frames.index(first_frame)
    if end_frame is not None:
        frames_iter = [f for f in frames[start_idx + 1:] if f <= end_frame]
    else:
        frames_iter = frames[start_idx + 1:]
    for frame in frames_iter:
        frames_since_selection += 1
        candidates = detections_by_frame.get(frame, [])

        if manual_left > 0:
            output.append(
                {
                    "frame": frame,
                    "instance_id": current.get("instance_id", instance_id),
                    "center_x": last_center[0],
                    "center_y": last_center[1],
                    "bbox": None,
                }
            )
            last_frame = frame
            miss_count = 0
            frames_since_selection = 0
            manual_left -= 1
            continue

        best_rec = None
        best_dist = None
        for rec in candidates:
            rx = rec.get("center_x")
            ry = rec.get("center_y")
            if rx is None or ry is None:
                continue
            dist = math.hypot(float(rx) - last_center[0], float(ry) - last_center[1])
            if best_dist is None or dist < best_dist:
                best_dist = dist
                best_rec = rec

        if video_path is not None and frames_since_selection >= int(select_interval):
            action, rec = _select_instance_with_click(
                candidates=candidates,
                video_path=video_path,
                frame_idx=frame,
                default_rec={
                    "center_x": last_center[0],
                    "center_y": last_center[1],
                    "instance_id": instance_id,
                },
                allow_keep=True,
                last_center=last_center,
                current_center=(
                    (float(best_rec["center_x"]), float(best_rec["center_y"]))
                    if best_rec is not None
                    else last_center
                ),
                click_max_dist=click_max_dist,
                manual_hold_frames=manual_hold_frames,
            )
            if action == "select" and rec is not None:
                instance_id = _normalize_instance_id(rec.get("instance_id"))
                current = rec
                last_center = (float(rec["center_x"]), float(rec["center_y"]))
                last_frame = frame
                miss_count = 0
                frames_since_selection = 0
                manual_left = int(rec.get("_manual_frames", 0)) if isinstance(rec, dict) else 0
                output.append(
                    {
                        "frame": frame,
                        "instance_id": instance_id,
                        "center_x": last_center[0],
                        "center_y": last_center[1],
                        "bbox": rec.get("bbox") if isinstance(rec, dict) else None,
                    }
                )
                continue
            elif action == "keep":
                output.append(
                    {
                        "frame": frame,
                        "instance_id": instance_id,
                        "center_x": last_center[0],
                        "center_y": last_center[1],
                        "bbox": None,
                    }
                )
                last_frame = frame
                miss_count = 0
                frames_since_selection = 0
                continue
            elif action == "abort":
                raise RuntimeError("Selection aborted.")
        f = frame - last_frame
        radius = float(radius_per_frame) * float(f)
        print(
            f"[frame {frame}] last=({last_center[0]:.2f},{last_center[1]:.2f}) "
            f"last_frame={last_frame} f={f} radius={radius:.2f}"
        )
        if candidates:
            print("  candidates:")
            for rec in candidates:
                rx = rec.get("center_x")
                ry = rec.get("center_y")
                if rx is None or ry is None:
                    print("    - center=None")
                    continue
                dist = math.hypot(float(rx) - last_center[0], float(ry) - last_center[1])
                print(
                    f"    - center=({float(rx):.2f},{float(ry):.2f}) "
                    f"dist={dist:.2f}"
                )
        else:
            print("  candidates: none")

        if best_rec is not None:
            if best_dist is not None and best_dist <= radius:
                print(
                    f"  -> accepted: dist={best_dist:.2f} <= radius={radius:.2f}"
                )
                last_center = (float(best_rec["center_x"]), float(best_rec["center_y"]))
                last_frame = frame
                miss_count = 0
                output.append(best_rec)
                continue
            if best_dist is not None:
                print(
                    f"  -> rejected: dist={best_dist:.2f} > radius={radius:.2f}"
                )

        miss_count += 1
        print(f"  -> miss_count={miss_count}")
        if miss_count > max_absence:
            if not candidates:
                output.append(
                    {
                        "frame": frame,
                        "instance_id": instance_id,
                        "center_x": last_center[0],
                        "center_y": last_center[1],
                        "bbox": None,
                    }
                )
                last_frame = frame
                miss_count = 0
                continue
            new_id = None
            new_rec = None
            if video_path is not None:
                action, rec = _select_instance_with_click(
                    candidates,
                    video_path,
                    frame,
                    default_rec=None,
                    allow_keep=True,
                    last_center=last_center,
                    current_center=(
                        (float(best_rec["center_x"]), float(best_rec["center_y"]))
                        if best_rec is not None
                        else last_center
                    ),
                    click_max_dist=click_max_dist,
                    manual_hold_frames=manual_hold_frames,
                )
                if action == "keep":
                    output.append(
                        {
                            "frame": frame,
                            "instance_id": instance_id,
                            "center_x": last_center[0],
                            "center_y": last_center[1],
                            "bbox": None,
                        }
                    )
                    last_frame = frame
                    miss_count = 0
                    continue
                if action == "select":
                    new_rec = rec
                    new_id = _normalize_instance_id(rec.get("instance_id"))
                if action == "abort":
                    raise RuntimeError("Selection aborted.")
            if new_rec is None:
                new_id, new_rec = _prompt_for_instance(
                    candidates, frame, last_frame, max_absence
                )
            if new_rec is None:
                output.append(
                    {
                        "frame": frame,
                        "instance_id": instance_id,
                        "center_x": last_center[0],
                        "center_y": last_center[1],
                        "bbox": None,
                    }
                )
                last_frame = frame
                miss_count = 0
                continue
            instance_id = new_id
            current = new_rec
            last_center = (float(new_rec["center_x"]), float(new_rec["center_y"]))
            last_frame = frame
            miss_count = 0
            frames_since_selection = 0
            manual_left = int(new_rec.get("_manual_frames", 0)) if isinstance(new_rec, dict) else 0
            output.append(new_rec)
            continue
        output.append(
            {
                "frame": frame,
                "instance_id": instance_id,
                "center_x": None,
                "center_y": None,
                "bbox": None,
            }
        )

    # Interpolate missing centers (linear; edge gaps are forward/back filled)
    known_idxs = [
        i
        for i, rec in enumerate(output)
        if rec.get("center_x") is not None and rec.get("center_y") is not None
    ]
    if known_idxs:
        first_known = known_idxs[0]
        last_known = known_idxs[-1]
        # Backfill leading missing
        for i in range(0, first_known):
            output[i]["center_x"] = output[first_known]["center_x"]
            output[i]["center_y"] = output[first_known]["center_y"]
        # Forward fill trailing missing
        for i in range(last_known + 1, len(output)):
            output[i]["center_x"] = output[last_known]["center_x"]
            output[i]["center_y"] = output[last_known]["center_y"]
        # Interpolate interior gaps
        for idx_pos in range(len(known_idxs) - 1):
            a = known_idxs[idx_pos]
            b = known_idxs[idx_pos + 1]
            frame_a = output[a].get("frame", a)
            frame_b = output[b].get("frame", b)
            xa = float(output[a]["center_x"])
            ya = float(output[a]["center_y"])
            xb = float(output[b]["center_x"])
            yb = float(output[b]["center_y"])
            span = float(frame_b - frame_a) if frame_b != frame_a else 1.0
            for i in range(a + 1, b):
                frame_i = output[i].get("frame", i)
                t = float(frame_i - frame_a) / span
                output[i]["center_x"] = xa + (xb - xa) * t
                output[i]["center_y"] = ya + (yb - ya) * t

    # Preserve un-averaged, un-filtered centers
    for rec in output:
        rec["raw_center_x"] = rec.get("center_x")
        rec["raw_center_y"] = rec.get("center_y")

    # Centered moving average of centers (no temporal delay)
    if average_frame_count < 1:
        average_frame_count = 1
    half = average_frame_count // 2
    smoothed = []
    for i in range(len(output)):
        sx = 0.0
        sy = 0.0
        count = 0
        start = max(0, i - half)
        end = min(len(output) - 1, i + half)
        for j in range(start, end + 1):
            rx = output[j].get("center_x")
            ry = output[j].get("center_y")
            if rx is None or ry is None:
                continue
            sx += float(rx)
            sy += float(ry)
            count += 1
        out_rec = dict(output[i])
        if count > 0:
            out_rec["center_x"] = sx / count
            out_rec["center_y"] = sy / count
        smoothed.append(out_rec)

    # Preserve pre-lowpass (moving-averaged) centers
    for rec in smoothed:
        rec["avg_center_x"] = rec.get("center_x")
        rec["avg_center_y"] = rec.get("center_y")

    # Optional: fix Y to average across all frames
    if fix_y and smoothed:
        ys = [rec.get("center_y") for rec in smoothed if rec.get("center_y") is not None]
        if ys:
            avg_y = sum(float(y) for y in ys) / float(len(ys))
            for rec in smoothed:
                if rec.get("center_y") is not None:
                    rec["center_y"] = avg_y

    # Optional: shift centers by N frames (positive = shift earlier, negative = later)
    if time_shift != 0 and smoothed:
        shifted = [dict(rec) for rec in smoothed]
        n = len(smoothed)
        for i in range(n):
            src = i + time_shift
            if 0 <= src < n:
                shifted[i]["center_x"] = smoothed[src].get("center_x")
                shifted[i]["center_y"] = smoothed[src].get("center_y")
            else:
                shifted[i]["center_x"] = None
                shifted[i]["center_y"] = None
        smoothed = shifted

    with out_path.open("w", encoding="utf-8") as f:
        for rec in smoothed:
            if end_frame is not None:
                try:
                    if int(rec.get("frame", -1)) > end_frame:
                        break
                except (TypeError, ValueError):
                    pass
            out_rec = dict(rec)
            out_rec["instance_id"] = instance_id
            f.write(json.dumps(out_rec) + "\n")


def load_centers(jsonl_path: Path):
    centers_by_frame = {}
    bboxes_by_frame = {}
    if not jsonl_path.exists():
        return centers_by_frame, bboxes_by_frame
    with jsonl_path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            frame = int(rec.get("frame", -1))
            cx = rec.get("center_x", None)
            cy = rec.get("center_y", None)
            bbox = rec.get("bbox", None)
            if frame >= 0 and cx is not None and cy is not None:
                centers_by_frame.setdefault(frame, []).append((float(cx), float(cy)))
            if frame >= 0 and bbox is not None:
                bboxes_by_frame.setdefault(frame, []).append(bbox)
    return centers_by_frame, bboxes_by_frame


def load_continuous_map(jsonl_path: Path):
    by_frame = {}
    if not jsonl_path.exists():
        return by_frame
    with jsonl_path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            frame = rec.get("frame", None)
            if frame is None:
                continue
            try:
                frame = int(frame)
            except (TypeError, ValueError):
                continue
            by_frame[frame] = rec
    return by_frame


def load_or_create_grading(json_path: Path):
    defaults = {
        "shadows": [0.0, 0.0, 0.0],
        "midtones": [0.0, 0.0, 0.0],
        "highlights": [0.0, 0.0, 0.0],
        "levels": [0.0, 255.0, 1.0, 0.0, 255.0],
        "brightness": 0.0,
        "contrast": 1.0,
        "saturation": 1.0,
    }
    if not json_path.exists():
        with json_path.open("w", encoding="utf-8") as f:
            json.dump(defaults, f, indent=2)
        return defaults
    with json_path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    for key in ("shadows", "midtones", "highlights", "levels"):
        if key not in data:
            data[key] = defaults[key]
    return data


def build_tracked_centers(centers_by_frame, max_dist=120.0, max_gap=5):
    tracked = {}
    prev_tracks = {}
    for frame in sorted(centers_by_frame.keys()):
        centers = centers_by_frame[frame]
        centers, _ = merge_duplicate_centers(centers)
        assigned_ids, prev_tracks = assign_track_ids(
            centers, prev_tracks, max_dist=max_dist, frame_idx=frame, max_gap=max_gap
        )
        frame_map = {}
        for i, ctr in enumerate(centers):
            frame_map[assigned_ids[i]] = ctr
        tracked[frame] = {
            "ids": assigned_ids,
            "centers": centers,
            "map": frame_map,
        }
    return tracked


def main():
    parser = argparse.ArgumentParser(description="Visualize centerpoints for an instance.")
    parser.add_argument("video", help="Input video (e.g. sankofa.MOV)")
    parser.add_argument(
        "--instance",
        type=str,
        default="3",
        help="Instance id to visualize (number or string label).",
    )
    parser.add_argument("--scale", type=float, default=0.5, help="Display scale factor.")
    parser.add_argument("--width", type=int, default=1080, help="Overlay box width in pixels.")
    parser.add_argument("--height", type=int, default=1920, help="Overlay box height in pixels.")
    parser.add_argument(
        "--radius",
        type=float,
        default=30.0,
        help="Continuation radius per frame (pixels).",
    )
    parser.add_argument(
        "--averageframecount",
        type=int,
        default=5,
        help="Centered moving average window size in frames.",
    )
    parser.add_argument(
        "--select-interval",
        type=int,
        default=300,
        help="Frames between interactive re-selection prompts (default: 300).",
    )
    parser.add_argument(
        "--lowpass-alpha",
        type=float,
        default=0.45,
        help="Symmetric low-pass alpha (0-1).",
    )
    parser.add_argument(
        "--fix-y",
        action="store_true",
        help="Fix all centroid Y values to their average across frames.",
    )
    parser.add_argument(
        "--time-shift",
        type=int,
        default=5,
        help="Shift centroid time by N frames (positive = earlier).",
    )
    parser.add_argument(
        "--zoom",
        type=float,
        default=1.7,
        help="Zoom factor for overlay rectangle (shrinks by this factor).",
    )
    parser.add_argument(
        "--offset-x",
        type=float,
        default=0,
        help="Horizontal offset (pixels) applied to overlay rectangle center.",
    )
    parser.add_argument(
        "--offset-y",
        type=float,
        default=0.0,
        help="Vertical offset (pixels) applied to overlay rectangle center.",
    )
    parser.add_argument(
        "--center-type",
        choices=["centroid", "bbox"],
        default="centroid",
        help="Center for overlay box (default: centroid).",
    )
    parser.add_argument(
        "--no-grading",
        action="store_true",
        help="Disable color grading (use raw frame).",
    )
    parser.add_argument(
        "--start-frame",
        type=int,
        default=0,
        help="Start processing at this frame index (default: 0).",
    )
    parser.add_argument(
        "--end-frame",
        type=int,
        default=None,
        help="Stop processing at this frame index (inclusive).",
    )
    vis_group = parser.add_mutually_exclusive_group()
    vis_group.add_argument(
        "--visualize",
        action="store_true",
        help="Show live preview window (default: on).",
    )
    vis_group.add_argument(
        "--no-visualize",
        action="store_true",
        help="Disable live preview window.",
    )
    args = parser.parse_args()

    video_path = Path(args.video)
    if not video_path.exists():
        raise FileNotFoundError(video_path)

    jsonl_path = video_path.with_suffix(".jsonl")
    if not jsonl_path.exists() and video_path.stem.endswith("_graded"):
        # Fall back to ungraded JSONL if the input video is a graded variant.
        ungraded_stem = video_path.stem[: -len("_graded")]
        fallback_jsonl = video_path.with_name(f"{ungraded_stem}.jsonl")
        if fallback_jsonl.exists():
            jsonl_path = fallback_jsonl

    if jsonl_path.exists():
        print(f"Found JSONL: {jsonl_path}")
    else:
        print(f"JSONL not found: {jsonl_path}")
    grading_path = video_path.with_name(f"{video_path.stem}_grading.json")
    grading = load_or_create_grading(grading_path)
    instance_id = _normalize_instance_id(args.instance)
    continuous_path = jsonl_path.with_name(f"{video_path.stem}_tracking.jsonl")
    if continuous_path.exists():
        print(f"[tracking] using existing {continuous_path}")
    else:
        preprocess_continuous(
            jsonl_path,
            instance_id=instance_id,
            radius_per_frame=args.radius,
            average_frame_count=args.averageframecount,
            lowpass_alpha=args.lowpass_alpha,
            fix_y=args.fix_y,
            time_shift=args.time_shift,
            start_frame=args.start_frame,
            end_frame=args.end_frame,
            select_interval=args.select_interval,
            out_path=continuous_path,
            video_path=video_path,
        )
    continuous_by_frame = load_continuous_map(continuous_path)
    if args.lowpass_alpha and continuous_by_frame:
        ordered_frames = sorted(continuous_by_frame.keys())
        ordered_recs = [continuous_by_frame[f] for f in ordered_frames]
        ordered_recs = apply_lowpass_filter(ordered_recs, args.lowpass_alpha)
        continuous_by_frame = {
            int(rec.get("frame", ordered_frames[i])): rec
            for i, rec in enumerate(ordered_recs)
        }

    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise RuntimeError(f"Failed to open video: {video_path}")

    fps = cap.get(cv2.CAP_PROP_FPS)
    if not fps or fps <= 0:
        fps = 30.0

    out_path = video_path.with_name(f"{video_path.stem}_stable.mp4")
    out_writer = None
    target_w = None
    target_h = None
    if args.width > 0 and args.height > 0:
        zoom = args.zoom if args.zoom > 0 else 1.0
        target_w = int(round(args.width / zoom))
        target_h = int(round(args.height / zoom))
        if target_w <= 0 or target_h <= 0:
            target_w = None
            target_h = None

    scale = args.scale
    visualize = True if not args.no_visualize else False
    start_frame = max(0, int(args.start_frame))
    if start_frame > 0:
        cap.set(cv2.CAP_PROP_POS_FRAMES, start_frame)
    frame_idx = start_frame
    end_frame = int(args.end_frame) if args.end_frame is not None else None
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if end_frame is not None and frame_idx > end_frame:
            break
        print(f"[frame] {frame_idx}")
        h, w = frame.shape[:2]
        if args.no_grading:
            graded = frame
        else:
            graded = apply_grading(
                frame,
                shadows=tuple(grading.get("shadows", (0.0, 0.0, 0.0))),
                midtones=tuple(grading.get("midtones", (0.0, 0.0, 0.0))),
                highlights=tuple(grading.get("highlights", (0.0, 0.0, 0.0))),
                levels=tuple(grading.get("levels")) if grading.get("levels") is not None else None,
                brightness=float(grading.get("brightness", 0.0)),
                contrast=float(grading.get("contrast", 1.0)),
                saturation=float(grading.get("saturation", 1.0)),
            )
        raw_frame = graded
        frame = graded.copy()
        chan_count = frame.shape[2] if frame is not None and frame.ndim == 3 else 3
        rec = continuous_by_frame.get(frame_idx, None)
        center = None
        bbox = None
        if rec is not None:
            cx = rec.get("center_x", None)
            cy = rec.get("center_y", None)
            if cx is not None and cy is not None:
                center = (float(cx), float(cy))
                cv2.circle(frame, (int(center[0]), int(center[1])), 12, (0, 0, 255), -1)
            bbox = rec.get("bbox", None)
            if bbox:
                x1, y1, x2, y2 = bbox
                cv2.rectangle(frame, (int(x1), int(y1)), (int(x2), int(y2)), (255, 0, 0), 2)

        # draw overlay box centered on averaged centroid or bbox center
        if args.width > 0 and args.height > 0:
            zoom = args.zoom if args.zoom > 0 else 1.0
            if center is not None:
                cx, cy = center
            elif args.center_type == "bbox" and bbox:
                x1, y1, x2, y2 = bbox
                cx = (x1 + x2) / 2.0
                cy = (y1 + y2) / 2.0
            else:
                cx, cy = w / 2.0, h / 2.0
            cx += args.offset_x
            cy += args.offset_y
            hx = (args.width / zoom) / 2.0
            hy = (args.height / zoom) / 2.0
            bx1 = int(round(cx - hx))
            by1 = int(round(cy - hy))
            bx2 = int(round(cx + hx))
            by2 = int(round(cy + hy))
            cv2.rectangle(frame, (bx1, by1), (bx2, by2), (0, 255, 255), 2)

            # write cropped output
            bx1c = max(0, min(bx1, w))
            by1c = max(0, min(by1, h))
            bx2c = max(0, min(bx2, w))
            by2c = max(0, min(by2, h))
            if target_w and target_h:
                if out_writer is None:
                    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
                    out_writer = cv2.VideoWriter(str(out_path), fourcc, fps, (target_w, target_h))
                crop = np.zeros((target_h, target_w, chan_count), dtype=raw_frame.dtype)
                if bx2c > bx1c and by2c > by1c:
                    src = raw_frame[by1c:by2c, bx1c:bx2c]
                    dx1 = max(0, -bx1)
                    dy1 = max(0, -by1)
                    dw = min(bx2c - bx1c, target_w - dx1)
                    dh = min(by2c - by1c, target_h - dy1)
                    dx2 = dx1 + dw
                    dy2 = dy1 + dh
                    crop[dy1:dy2, dx1:dx2] = src[:dh, :dw]
                if chan_count == 4:
                    crop = cv2.cvtColor(crop, cv2.COLOR_BGRA2BGR)
                out_writer.write(crop)
        else:
            cv2.putText(
                frame,
                f"no center for instance {args.instance}",
                (20, 40),
                cv2.FONT_HERSHEY_SIMPLEX,
                1.0,
                (0, 0, 255),
                2,
                cv2.LINE_AA,
            )

        if visualize:
            resized = cv2.resize(
                frame, (int(w * scale), int(h * scale)), interpolation=cv2.INTER_AREA
            )
            cv2.imshow("stabilize", resized)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
        frame_idx += 1

    cap.release()
    if out_writer is not None:
        out_writer.release()
    if visualize:
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
