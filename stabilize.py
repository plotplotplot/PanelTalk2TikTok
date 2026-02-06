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


def preprocess_continuous(
    jsonl_path: Path,
    instance_id: int,
    out_path: Path,
    max_absence: int = 5,
    radius_per_frame: float = 50.0,
    average_frame_count: int = 7,
    lowpass_alpha: float = 0.25,
    fix_y: bool = False,
    time_shift: int = 0,
):
    detections_by_frame = load_detections(jsonl_path)
    if not detections_by_frame:
        raise RuntimeError("No detections found in JSONL.")

    frames = sorted(detections_by_frame.keys())
    first_frame = frames[0]
    first_frame_recs = detections_by_frame[first_frame]
    start_candidates = [
        rec for rec in first_frame_recs if rec.get("instance_id") == instance_id
    ]
    if not start_candidates:
        raise RuntimeError(
            f"No instance_id {instance_id} found in first frame ({first_frame})."
        )
    current = start_candidates[0]
    cx = current.get("center_x")
    cy = current.get("center_y")
    if cx is None or cy is None:
        raise RuntimeError("Starting detection missing center_x/center_y.")
    last_center = (float(cx), float(cy))
    last_frame = first_frame
    miss_count = 0

    output = [current]

    for frame in frames[1:]:
        candidates = detections_by_frame.get(frame, [])
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
            raise RuntimeError(
                f"No continuation after {max_absence} frames at frame {frame} "
                f"(last detection frame {last_frame})."
            )
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

    # Symmetric low-pass (forward-backward) to avoid phase delay
    alpha = float(lowpass_alpha)
    if alpha < 0.0:
        alpha = 0.0
    if alpha > 1.0:
        alpha = 1.0
    if alpha > 0.0 and len(smoothed) > 1:
        fwd = []
        prev_x = None
        prev_y = None
        for rec in smoothed:
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
        smoothed = combined

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
    parser.add_argument("--instance", type=int, default=1, help="Instance id to visualize.")
    parser.add_argument("--scale", type=float, default=0.5, help="Display scale factor.")
    parser.add_argument("--width", type=int, default=1080, help="Overlay box width in pixels.")
    parser.add_argument("--height", type=int, default=1920, help="Overlay box height in pixels.")
    parser.add_argument(
        "--radius",
        type=float,
        default=50.0,
        help="Continuation radius per frame (pixels).",
    )
    parser.add_argument(
        "--averageframecount",
        type=int,
        default=7,
        help="Centered moving average window size in frames.",
    )
    parser.add_argument(
        "--lowpass-alpha",
        type=float,
        default=0.25,
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
        default=0,
        help="Shift centroid time by N frames (positive = earlier).",
    )
    parser.add_argument(
        "--zoom",
        type=float,
        default=3.0,
        help="Zoom factor for overlay rectangle (shrinks by this factor).",
    )
    parser.add_argument(
        "--offset-x",
        type=float,
        default=0.0,
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
    args = parser.parse_args()

    video_path = Path(args.video)
    if not video_path.exists():
        raise FileNotFoundError(video_path)

    jsonl_path = video_path.with_suffix(".jsonl")
    grading_path = video_path.with_name(f"{video_path.stem}_grading.json")
    grading = load_or_create_grading(grading_path)
    preprocess_continuous(
        jsonl_path,
        instance_id=args.instance,
        radius_per_frame=args.radius,
        average_frame_count=args.averageframecount,
        lowpass_alpha=args.lowpass_alpha,
        fix_y=args.fix_y,
        time_shift=args.time_shift,
        out_path=jsonl_path.with_name("input_continuous.jsonl"),
    )
    continuous_path = jsonl_path.with_name("input_continuous.jsonl")
    continuous_by_frame = load_continuous_map(continuous_path)

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
    frame_idx = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        h, w = frame.shape[:2]
        graded = apply_grading(
            frame,
            shadows=tuple(grading.get("shadows", (0.0, 0.0, 0.0))),
            midtones=tuple(grading.get("midtones", (0.0, 0.0, 0.0))),
            highlights=tuple(grading.get("highlights", (0.0, 0.0, 0.0))),
            levels=tuple(grading.get("levels")) if grading.get("levels") is not None else None,
            brightness=float(grading.get("brightness", 0.0)),
            contrast=float(grading.get("contrast", 1.0)),
        )
        raw_frame = graded
        frame = graded.copy()
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
                crop = np.zeros((target_h, target_w, 3), dtype=raw_frame.dtype)
                if bx2c > bx1c and by2c > by1c:
                    src = raw_frame[by1c:by2c, bx1c:bx2c]
                    dx1 = max(0, -bx1)
                    dy1 = max(0, -by1)
                    dw = min(bx2c - bx1c, target_w - dx1)
                    dh = min(by2c - by1c, target_h - dy1)
                    dx2 = dx1 + dw
                    dy2 = dy1 + dh
                    crop[dy1:dy2, dx1:dx2] = src[:dh, :dw]
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

        resized = cv2.resize(frame, (int(w * scale), int(h * scale)), interpolation=cv2.INTER_AREA)
        cv2.imshow("stabilize", resized)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break
        frame_idx += 1

    cap.release()
    if out_writer is not None:
        out_writer.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
