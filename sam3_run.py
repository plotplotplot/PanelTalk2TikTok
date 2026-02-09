#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import sys
import time
import json

import cv2
import numpy as np
import torch
from PIL import Image

from huggingface_hub import login as hf_login
from sam3.model_builder import build_sam3_image_model, build_sam3_video_predictor
from sam3.model.sam3_image_processor import Sam3Processor

IMG_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}
VID_EXTS = {".mp4", ".mov", ".mkv", ".avi", ".webm", ".mpeg", ".mpg", ".m4v"}


def is_image(path: Path) -> bool:
    return path.suffix.lower() in IMG_EXTS


def is_video(path: Path) -> bool:
    return path.suffix.lower() in VID_EXTS


def read_hf_token():
    token_file = os.environ.get(
        "HF_TOKEN_FILE",
        str(Path(__file__).resolve().parent / "hftoken.txt"),
    )
    token_path = Path(token_file)
    if not token_path.exists():
        raise FileNotFoundError(f"HF token file not found: {token_path}")
    token = token_path.read_text(encoding="utf-8").strip()
    if not token:
        raise ValueError(f"HF token file is empty: {token_path}")
    return token


def ensure_hf_login():
    token = read_hf_token()
    os.environ["HUGGINGFACE_HUB_TOKEN"] = token
    hf_login(token=token, add_to_git_credential=False)


def overlay_masks(image_bgr: np.ndarray, masks: np.ndarray, alpha: float = 0.45) -> np.ndarray:
    if masks is None or len(masks) == 0:
        return image_bgr
    out = image_bgr.copy()
    rng = np.random.default_rng(0)
    colors = rng.integers(0, 255, size=(len(masks), 3), dtype=np.uint8)
    for i, mask in enumerate(masks):
        if mask.ndim == 3 and mask.shape[0] == 1:
            mask = mask[0]
        elif mask.ndim == 4 and mask.shape[1] == 1:
            mask = mask[0, 0]
        if mask.dtype != np.bool_:
            mask = mask.astype(bool)
        if mask.ndim != 2:
            raise ValueError(f"Unexpected mask shape: {mask.shape}")
        color = colors[i]
        out[mask] = (out[mask].astype(np.float32) * (1 - alpha) + color * alpha).astype(
            np.uint8
        )
    return out


def mask_center(mask: np.ndarray, method: str = "bbox"):
    if mask.dtype != np.bool_:
        mask = mask.astype(bool)
    if mask.ndim == 3 and mask.shape[0] == 1:
        mask = mask[0]
    elif mask.ndim == 3 and mask.shape[-1] == 1:
        mask = mask[:, :, 0]
    elif mask.ndim == 4 and mask.shape[1] == 1:
        mask = mask[0, 0]
    ys, xs = np.where(mask)
    if ys.size == 0:
        return None
    if method == "bbox":
        return float(xs.min() + xs.max()) / 2.0, float(ys.min() + ys.max()) / 2.0
    return float(xs.mean()), float(ys.mean())


def smooth_center(prev, cur, alpha):
    if prev is None:
        return cur
    if cur is None:
        return prev
    return (prev[0] * alpha + cur[0] * (1 - alpha), prev[1] * alpha + cur[1] * (1 - alpha))


def resolve_output_path(input_path: Path, out_arg: str | None, is_vid: bool) -> Path:
    if out_arg is None:
        suffix = ".mp4" if is_vid else ".png"
        return input_path.with_name(f"{input_path.stem}_sam3{suffix}")
    out_path = Path(out_arg)
    if out_path.is_dir() or out_arg.endswith(os.sep):
        suffix = ".mp4" if is_vid else ".png"
        return out_path / f"{input_path.stem}_sam3{suffix}"
    return out_path


def run_image(
    input_path: Path,
    prompt: str,
    out_path: Path,
    scale_width: int | None,
    centers_path: Path | None,
    produce_output_video: bool,
):
    model = build_sam3_image_model()
    processor = Sam3Processor(model)

    image = Image.open(input_path).convert("RGB")
    if scale_width is not None and scale_width > 0 and image.width > scale_width:
        new_h = int(round(image.height * (scale_width / image.width)))
        image = image.resize((scale_width, new_h), Image.BICUBIC)
    state = processor.set_image(image)
    state = processor.set_text_prompt(prompt=prompt, state=state)

    masks = state.get("masks")
    masks_np = masks.cpu().numpy() if masks is not None else np.zeros((0,), dtype=bool)

    img_bgr = cv2.cvtColor(np.array(image), cv2.COLOR_RGB2BGR)
    out_bgr = overlay_masks(img_bgr, masks_np)
    if produce_output_video:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        cv2.imwrite(str(out_path), out_bgr)
    if centers_path is not None and masks_np is not None:
        centers_path.parent.mkdir(parents=True, exist_ok=True)
        with centers_path.open("w", encoding="utf-8") as f:
            for i, mask in enumerate(masks_np):
                ctr = mask_center(mask)
                if ctr is None:
                    continue
                rec = {
                    "frame": 0,
                    "time_sec": None,
                    "instance_id": i,
                    "center_x": ctr[0],
                    "center_y": ctr[1],
                    "score": None,
                }
                f.write(json.dumps(rec) + "\n")


def run_video(
    input_path: Path,
    prompt: str,
    out_path: Path,
    max_frames: int | None,
    scale_width: int | None,
    centers_path: Path | None,
    produce_output_video: bool,
    centers_method: str,
    smooth_alpha: float | None,
    add_center: bool,
):
    predictor = build_sam3_video_predictor()
    source_path = input_path
    temp_scaled = None
    if scale_width is not None and scale_width > 0:
        temp_scaled = tempfile.NamedTemporaryFile(
            suffix=".mp4", prefix="sam3_scale_", delete=False
        )
        temp_scaled.close()
        scale_cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(input_path),
            "-vf",
            f"scale={scale_width}:-2",
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "18",
            "-c:a",
            "aac",
            "-b:a",
            "192k",
            temp_scaled.name,
        ]
        print("[scale] cmd:", " ".join(scale_cmd))
        subprocess.check_call(scale_cmd)
        source_path = Path(temp_scaled.name)

    resp = predictor.handle_request(
        {"type": "start_session", "resource_path": str(source_path)}
    )
    session_id = resp["session_id"]
    _ = predictor.handle_request(
        {"type": "add_prompt", "session_id": session_id, "frame_index": 0, "text": prompt}
    )

    cap = cv2.VideoCapture(str(source_path))
    if not cap.isOpened():
        raise RuntimeError(f"Failed to open video: {input_path}")

    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    writer = None
    if produce_output_video:
        fourcc = cv2.VideoWriter_fourcc(*"mp4v")
        out_path.parent.mkdir(parents=True, exist_ok=True)
        writer = cv2.VideoWriter(str(out_path), fourcc, fps, (width, height))

    stream = predictor.handle_stream_request(
        {
            "type": "propagate_in_video",
            "session_id": session_id,
            "propagation_direction": "forward",
            "start_frame_index": 0,
            "max_frame_num_to_track": max_frames,
        }
    )
    next_out = next(stream, None)
    frame_idx = 0

    centers_f = None
    smooth_state = {}
    existing_frames = set()
    if centers_path is not None:
        centers_path.parent.mkdir(parents=True, exist_ok=True)
        if centers_path.exists():
            with centers_path.open("r", encoding="utf-8") as f:
                for line in f:
                    try:
                        rec = json.loads(line)
                        existing_frames.add(int(rec.get("frame", -1)))
                    except Exception:
                        continue
        centers_f = centers_path.open("a", encoding="utf-8")

    if (not produce_output_video) and centers_path is not None:
        # If we already have centers for all frames, skip inference.
        total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
        if total_frames > 0 and len(existing_frames) >= total_frames:
            cap.release()
            predictor.handle_request({"type": "close_session", "session_id": session_id})
            if centers_f is not None:
                centers_f.close()
            if temp_scaled is not None:
                try:
                    os.unlink(temp_scaled.name)
                except OSError:
                    pass
            print(f"[skip] centers already exist for all frames: {centers_path}")
            return

    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if max_frames is not None and frame_idx >= max_frames:
            break

        masks_np = None
        while next_out is not None and next_out["frame_index"] < frame_idx:
            next_out = next(stream, None)
        centers = []
        if next_out is not None and next_out["frame_index"] == frame_idx:
            outputs = next_out["outputs"]
            masks_np = outputs.get("out_binary_masks", None)
            obj_ids = outputs.get("out_obj_ids", None)
            scores = outputs.get("out_probs", None)
            if masks_np is not None and centers_f is not None:
                for i, mask in enumerate(masks_np):
                    ctr = mask_center(mask, method=centers_method)
                    if ctr is None:
                        continue
                    rec = {
                        "frame": frame_idx,
                        "time_sec": (frame_idx / fps) if fps else None,
                        "instance_id": int(obj_ids[i]) if obj_ids is not None else i,
                        "center_x": ctr[0],
                        "center_y": ctr[1],
                        "score": float(scores[i]) if scores is not None else None,
                    }
                    centers.append(rec)
            next_out = next(stream, None)

        if produce_output_video:
            out_frame = overlay_masks(frame, masks_np) if masks_np is not None else frame
            if add_center and centers:
                for rec in centers:
                    cv2.circle(
                        out_frame,
                        (int(rec["center_x"]), int(rec["center_y"])),
                        8,
                        (0, 0, 255),
                        -1,
                    )
            writer.write(out_frame)
        if centers_f is not None:
            if centers:
                for rec in centers:
                    if rec["frame"] in existing_frames:
                        continue
                    if smooth_alpha is not None:
                        prev = smooth_state.get(rec["instance_id"])
                        cur = (rec["center_x"], rec["center_y"])
                        sm = smooth_center(prev, cur, smooth_alpha)
                        smooth_state[rec["instance_id"]] = sm
                        rec["center_x"], rec["center_y"] = sm
                    centers_f.write(json.dumps(rec) + "\n")
            else:
                # write empty frame record
                if frame_idx not in existing_frames:
                    centers_f.write(
                        json.dumps(
                            {
                                "frame": frame_idx,
                                "time_sec": (frame_idx / fps) if fps else None,
                                "instance_id": None,
                                "center_x": None,
                                "center_y": None,
                                "score": None,
                            }
                        )
                        + "\n"
                    )
        frame_idx += 1

    if writer is not None:
        writer.release()
    cap.release()
    predictor.handle_request({"type": "close_session", "session_id": session_id})
    if centers_f is not None:
        centers_f.close()
    if temp_scaled is not None:
        try:
            os.unlink(temp_scaled.name)
        except OSError:
            pass


def extract_frames(
    input_path: Path,
    frames_dir: Path,
    scale_width: int | None,
    frames_format: str,
    extract_fps: float | None,
):
    frames_dir.mkdir(parents=True, exist_ok=True)
    vf = []
    if scale_width is not None and scale_width > 0:
        vf.append(f"scale={scale_width}:-2")
    if extract_fps is not None and extract_fps > 0:
        vf.append(f"fps={extract_fps}")
    vf_arg = ",".join(vf) if vf else "null"
    ext = "jpg" if frames_format == "jpg" else "png"
    existing = list(frames_dir.glob(f"frame_*.{ext}"))
    if existing:
        print(f"[frames] skipping extract; found {len(existing)} frames in {frames_dir}")
        return None

    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(input_path),
        "-vf",
        vf_arg,
        str(frames_dir / f"frame_%06d.{ext}"),
    ]
    print("[frames] extract cmd:", " ".join(cmd))
    subprocess.check_call(cmd)

    fps = None
    try:
        probe = subprocess.run(
            [
                "ffprobe",
                "-hide_banner",
                "-loglevel",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=avg_frame_rate",
                "-of",
                "default=nokey=1:noprint_wrappers=1",
                str(input_path),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if probe.stdout.strip():
            num, den = probe.stdout.strip().split("/")
            fps = float(num) / float(den)
    except Exception:
        fps = None
    return fps


def run_video_as_frames(
    input_path: Path,
    prompt: str,
    out_path: Path,
    scale_width: int | None,
    frames_dir: Path | None,
    frames_format: str,
    extract_fps: float | None,
    stream_extract: bool,
    centers_path: Path | None,
    produce_output_video: bool,
    centers_method: str,
    smooth_alpha: float | None,
    add_center: bool,
):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    frames_dir = frames_dir or (out_path.parent / f"{input_path.stem}_frames")
    out_frames_dir = out_path.parent / f"{input_path.stem}_frames_out"
    frames_dir.mkdir(parents=True, exist_ok=True)
    out_frames_dir.mkdir(parents=True, exist_ok=True)

    ext = "jpg" if frames_format == "jpg" else "png"

    model = build_sam3_image_model()
    processor = Sam3Processor(model)
    centers_f = None
    existing_frames = set()
    smooth_state = {}
    if centers_path is not None:
        centers_path.parent.mkdir(parents=True, exist_ok=True)
        if centers_path.exists():
            with centers_path.open("r", encoding="utf-8") as f:
                for line in f:
                    try:
                        rec = json.loads(line)
                        existing_frames.add(int(rec.get("frame", -1)))
                    except Exception:
                        continue
        centers_f = centers_path.open("a", encoding="utf-8")

    if stream_extract:
        vf = []
        if scale_width is not None and scale_width > 0:
            vf.append(f"scale={scale_width}:-2")
        if extract_fps is not None and extract_fps > 0:
            vf.append(f"fps={extract_fps}")
        vf_arg = ",".join(vf) if vf else "null"
        cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(input_path),
            "-vf",
            vf_arg,
            str(frames_dir / f"frame_%06d.{ext}"),
        ]
        existing = list(frames_dir.glob(f"frame_*.{ext}"))
        if existing:
            print(f"[frames] skipping extract; found {len(existing)} frames in {frames_dir}")
            proc = None
        else:
            print("[frames] extract cmd (stream):", " ".join(cmd))
            proc = subprocess.Popen(cmd)

        idx = 1
        while True:
            frame_path = frames_dir / f"frame_{idx:06d}.{ext}"
            if frame_path.exists():
                frame_idx = idx - 1
                if frame_idx in existing_frames and not produce_output_video:
                    print(f"[frames] skipping recorded {frame_path.name}")
                    idx += 1
                    continue
                print(f"[frames] processing {frame_path.name}")
                # wait for file size to stabilize
                last_size = -1
                for _ in range(5):
                    size = frame_path.stat().st_size
                    if size == last_size and size > 0:
                        break
                    last_size = size
                    time.sleep(0.05)

                image = Image.open(frame_path).convert("RGB")
                state = processor.set_image(image)
                state = processor.set_text_prompt(prompt=prompt, state=state)
                masks = state.get("masks")
                masks_np = (
                    masks.cpu().numpy() if masks is not None else np.zeros((0,), dtype=bool)
                )
                img_bgr = cv2.cvtColor(np.array(image), cv2.COLOR_RGB2BGR)
                out_bgr = overlay_masks(img_bgr, masks_np)
                centers = []
                for mask in masks_np:
                    ctr = mask_center(mask, method=centers_method)
                    if ctr is not None:
                        centers.append(ctr)
                if add_center and centers:
                    for ctr in centers:
                        cv2.circle(out_bgr, (int(ctr[0]), int(ctr[1])), 8, (0, 0, 255), -1)
                out_frame = out_frames_dir / frame_path.name
                cv2.imwrite(str(out_frame), out_bgr)
                if centers_f is not None:
                    if centers:
                        for i, ctr in enumerate(centers):
                            if smooth_alpha is not None:
                                prev = smooth_state.get(i)
                                cur = ctr
                                sm = smooth_center(prev, cur, smooth_alpha)
                                smooth_state[i] = sm
                                ctr = sm
                            rec = {
                                "frame": idx - 1,
                                "time_sec": None,
                                "instance_id": i,
                                "center_x": ctr[0],
                                "center_y": ctr[1],
                                "score": None,
                            }
                            if rec["frame"] in existing_frames:
                                continue
                            centers_f.write(json.dumps(rec) + "\n")
                    else:
                        if (idx - 1) not in existing_frames:
                            centers_f.write(
                                json.dumps(
                                    {
                                        "frame": idx - 1,
                                        "time_sec": None,
                                        "instance_id": None,
                                        "center_x": None,
                                        "center_y": None,
                                        "score": None,
                                    }
                                )
                                + "\n"
                            )
                idx += 1
                continue

            if proc is None:
                if not frame_path.exists():
                    break
            elif proc.poll() is not None:
                break
            time.sleep(0.05)
    else:
        fps = extract_frames(input_path, frames_dir, scale_width, frames_format, extract_fps)
        frame_files = sorted(frames_dir.glob(f"frame_*.{ext}"))
        if not frame_files:
            raise RuntimeError("No frames extracted.")

        for frame_path in frame_files:
            frame_idx = int(frame_path.stem.split("_")[-1]) - 1
            if frame_idx in existing_frames and not produce_output_video:
                continue
            print(f"[frames] processing {frame_path.name}")
            image = Image.open(frame_path).convert("RGB")
            state = processor.set_image(image)
            state = processor.set_text_prompt(prompt=prompt, state=state)
            masks = state.get("masks")
            masks_np = (
                masks.cpu().numpy() if masks is not None else np.zeros((0,), dtype=bool)
            )
            img_bgr = cv2.cvtColor(np.array(image), cv2.COLOR_RGB2BGR)
            out_bgr = overlay_masks(img_bgr, masks_np)
            centers = []
            for mask in masks_np:
                ctr = mask_center(mask, method=centers_method)
                if ctr is not None:
                    centers.append(ctr)
            if add_center and centers:
                for ctr in centers:
                    cv2.circle(out_bgr, (int(ctr[0]), int(ctr[1])), 8, (0, 0, 255), -1)
            out_frame = out_frames_dir / frame_path.name
            cv2.imwrite(str(out_frame), out_bgr)
            if centers_f is not None:
                if centers:
                    for i, ctr in enumerate(centers):
                        if smooth_alpha is not None:
                            prev = smooth_state.get(i)
                            cur = ctr
                            sm = smooth_center(prev, cur, smooth_alpha)
                            smooth_state[i] = sm
                            ctr = sm
                        rec = {
                            "frame": frame_idx,
                            "time_sec": None,
                            "instance_id": i,
                            "center_x": ctr[0],
                            "center_y": ctr[1],
                            "score": None,
                        }
                        if rec["frame"] in existing_frames:
                            continue
                        centers_f.write(json.dumps(rec) + "\n")
                else:
                    if frame_idx not in existing_frames:
                        centers_f.write(
                            json.dumps(
                                {
                                    "frame": frame_idx,
                                    "time_sec": None,
                                    "instance_id": None,
                                    "center_x": None,
                                    "center_y": None,
                                    "score": None,
                                }
                            )
                            + "\n"
                        )

    if produce_output_video:
        if stream_extract:
            fps_arg = str(extract_fps) if extract_fps else "30"
        else:
            fps_arg = str(extract_fps) if extract_fps else (str(fps) if fps else "30")
        stitch_cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-framerate",
            fps_arg,
            "-i",
            str(out_frames_dir / f"frame_%06d.{ext}"),
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            str(out_path),
        ]
        if out_path.exists():
            print(f"[frames] skipping stitch; output exists: {out_path}")
        else:
            print("[frames] stitch cmd:", " ".join(stitch_cmd))
            subprocess.check_call(stitch_cmd)
    else:
        print("[frames] skipping stitch; --produce-output-video not set")
    if centers_f is not None:
        centers_f.close()


def run_video_chunked(
    input_path: Path,
    prompt: str,
    out_path: Path,
    chunk_seconds: int,
    keep_chunks: bool = True,
    chunks_dir: Path | None = None,
    scale_width: int | None = None,
    centers_path: Path | None = None,
    produce_output_video: bool = False,
    centers_method: str = "bbox",
    smooth_alpha: float | None = None,
    add_center: bool = False,
):
    if chunk_seconds <= 0:
        raise ValueError("--chunk-seconds must be > 0")
    if keep_chunks:
        base_dir = chunks_dir or (out_path.parent / f"{input_path.stem}_chunks")
        chunk_dir_p = base_dir / "input"
        chunk_out_p = base_dir / "output"
        chunk_dir_p.mkdir(parents=True, exist_ok=True)
        chunk_out_p.mkdir(parents=True, exist_ok=True)
    else:
        tmp_in = tempfile.TemporaryDirectory(prefix="sam3_chunks_")
        tmp_out = tempfile.TemporaryDirectory(prefix="sam3_chunk_out_")
        chunk_dir_p = Path(tmp_in.name)
        chunk_out_p = Path(tmp_out.name)

    source_path = input_path
    scaled_path = None
    if scale_width is not None and scale_width > 0:
        scaled_path = (chunk_dir_p / "scaled_input.mp4") if keep_chunks else Path(
            tempfile.NamedTemporaryFile(suffix=".mp4", prefix="sam3_scale_", delete=False).name
        )
        scale_cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(input_path),
            "-vf",
            f"scale={scale_width}:-2",
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "18",
            "-c:a",
            "aac",
            "-b:a",
            "192k",
            str(scaled_path),
        ]
        print("[scale] cmd:", " ".join(scale_cmd))
        subprocess.check_call(scale_cmd)
        source_path = Path(scaled_path)

    cap = cv2.VideoCapture(str(source_path))
    fps = cap.get(cv2.CAP_PROP_FPS) or 0.0
    frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    cap.release()
    if fps <= 0:
        fps = 30.0
    gop = max(1, int(round(fps * chunk_seconds)))

    print("[chunk] ffprobe input:")
    try:
        subprocess.run(
            [
                "ffprobe",
                "-hide_banner",
                "-loglevel",
                "error",
                "-show_format",
                "-show_streams",
                str(input_path),
            ],
            check=False,
            text=True,
        )
    except FileNotFoundError:
        print("[chunk] ffprobe not found (skipping)")

    split_cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-fflags",
        "+genpts",
        "-avoid_negative_ts",
        "make_zero",
        "-i",
        str(source_path),
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-crf",
        "18",
        "-g",
        str(gop),
        "-keyint_min",
        str(gop),
        "-sc_threshold",
        "0",
        "-force_key_frames",
        f"expr:gte(t,n_forced*{chunk_seconds})",
        "-c:a",
        "aac",
        "-b:a",
        "192k",
        "-f",
        "segment",
        "-segment_time",
        str(chunk_seconds),
        "-reset_timestamps",
        "1",
        str(chunk_dir_p / "chunk_%04d.mp4"),
    ]
    print("[chunk] split cmd:", " ".join(split_cmd))
    try:
        subprocess.check_call(split_cmd)
    except subprocess.CalledProcessError as err:
        print(f"[chunk] split failed: {err}. Retrying with vsync=0")
        split_cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-fflags",
            "+genpts",
            "-avoid_negative_ts",
            "make_zero",
            "-vsync",
            "0",
            "-i",
            str(source_path),
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "18",
            "-g",
            str(gop),
            "-keyint_min",
            str(gop),
            "-sc_threshold",
            "0",
            "-force_key_keys",
            f"expr:gte(t,n_forced*{chunk_seconds})",
            "-c:a",
            "aac",
            "-b:a",
            "192k",
            "-f",
            "segment",
            "-segment_time",
            str(chunk_seconds),
            "-reset_timestamps",
            "1",
            str(chunk_dir_p / "chunk_%04d.mp4"),
        ]
        print("[chunk] split cmd (retry):", " ".join(split_cmd))
        subprocess.check_call(split_cmd)
    print(f"[chunk] chunks dir: {chunk_dir_p}")

    chunks = sorted(chunk_dir_p.glob("chunk_*.mp4"))
    if not chunks:
        raise RuntimeError("No chunks created by ffmpeg.")
    if len(chunks) == 1 and frame_count > int(fps * chunk_seconds * 1.5):
        raise RuntimeError(
            "Chunking produced a single segment; input timestamps may be invalid. "
            "Try remuxing the input to fix timestamps before chunking."
        )

    out_files = []
    centers_records = []
    frame_offset = 0
    for idx, chunk in enumerate(chunks):
        out_file = chunk_out_p / f"out_{idx:04d}.mp4"
        chunk_centers = chunk_out_p / f"centers_{idx:04d}.jsonl"
        # Run each chunk in a fresh process so CUDA memory is fully released between chunks.
        cmd = [
            sys.executable,
            str(Path(__file__).resolve()),
            "--input",
            str(chunk),
            "--prompt",
            prompt,
            "--output",
            str(out_file),
            "--child-run",
            "--centers-json",
            str(chunk_centers),
        ]
        cmd += ["--centers-method", centers_method]
        if smooth_alpha is not None:
            cmd += ["--smooth-alpha", str(smooth_alpha)]
        if add_center:
            cmd.append("--add-center")
        if produce_output_video:
            cmd.append("--produce-output-video")
        subprocess.check_call(cmd)
        out_files.append(out_file)

        if centers_path is not None and chunk_centers.exists():
            with chunk_centers.open("r", encoding="utf-8") as f:
                for line in f:
                    rec = json.loads(line)
                    rec["frame"] = rec.get("frame", 0) + frame_offset
                    centers_records.append(rec)
            cap = cv2.VideoCapture(str(chunk))
            frame_offset += int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
            cap.release()

    list_file = chunk_out_p / "concat.txt"
    with list_file.open("w", encoding="utf-8") as f:
        for fpath in out_files:
            f.write(f"file '{fpath}'\n")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    concat_cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "concat",
        "-safe",
        "0",
        "-i",
        str(list_file),
        "-c",
        "copy",
        str(out_path),
    ]
    if produce_output_video:
        subprocess.check_call(concat_cmd)
    if centers_path is not None:
        centers_path.parent.mkdir(parents=True, exist_ok=True)
        with centers_path.open("w", encoding="utf-8") as f:
            for rec in centers_records:
                f.write(json.dumps(rec) + "\n")

    if not keep_chunks:
        tmp_in.cleanup()
        tmp_out.cleanup()
        if scaled_path is not None:
            try:
                os.unlink(str(scaled_path))
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(description="SAM3 text-prompted segmentation.")
    parser.add_argument("--input", required=True, help="Input image or video file.")
    parser.add_argument("--prompt", required=True, help='Text prompt, e.g. "a person".')
    parser.add_argument("--output", default=None, help="Output file path or directory.")
    parser.add_argument("--max-frames", type=int, default=None, help="Limit frames for video.")
    parser.add_argument("--chunk-seconds", type=int, default=None, help="Chunk video for OOM safety.")
    parser.add_argument(
        "--scale-width",
        type=int,
        default=None,
        help="Resize input video/image to this width before processing.",
    )
    parser.add_argument(
        "--extract-frames",
        action="store_true",
        help="Extract video to frames and run image model per frame (no tracking).",
    )
    parser.add_argument(
        "--frames-dir",
        default=None,
        help="Directory to store extracted frames (default: <out_dir>/<input>_frames).",
    )
    parser.add_argument(
        "--frames-format",
        choices=["jpg", "png"],
        default="jpg",
        help="Frame image format when using --extract-frames (default: jpg).",
    )
    parser.add_argument(
        "--extract-fps",
        type=float,
        default=None,
        help="When using --extract-frames, only extract this many frames per second.",
    )
    parser.add_argument(
        "--stream-extract",
        action="store_true",
        help="Stream extraction and run inference while frames are being written.",
    )
    parser.add_argument(
        "--centers-json",
        default=None,
        help="Write per-frame centers to JSONL (one record per instance per frame).",
    )
    parser.add_argument(
        "--produce-output-video",
        action="store_true",
        help="Write output video (otherwise only JSONL centers are produced).",
    )
    parser.add_argument(
        "--centers-method",
        choices=["bbox", "centroid"],
        default="bbox",
        help="How to compute centers from masks (default: bbox).",
    )
    parser.add_argument(
        "--smooth-alpha",
        type=float,
        default=None,
        help="EMA smoothing factor for centers (0-1, higher = smoother).",
    )
    parser.add_argument(
        "--follow-speed",
        type=float,
        default=None,
        help="Alias for --smooth-alpha (higher = smoother).",
    )
    parser.add_argument(
        "--add-center",
        action="store_true",
        help="Draw centerpoints on output frames/video.",
    )
    parser.add_argument(
        "--no-keep-chunks",
        dest="keep_chunks",
        action="store_false",
        help="Delete intermediate chunks after stitching.",
    )
    parser.add_argument(
        "--chunks-dir",
        default=None,
        help="Directory to store intermediate chunks (default: <out_dir>/<input>_chunks).",
    )
    parser.add_argument(
        "--child-run",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    args = parser.parse_args()

    if args.follow_speed is not None:
        args.smooth_alpha = args.follow_speed

    if not torch.cuda.is_available():
        raise RuntimeError("GPU is required but torch.cuda.is_available() is False.")

    ensure_hf_login()

    input_path = Path(args.input)
    if not input_path.exists():
        raise FileNotFoundError(f"Input not found: {input_path}")

    if is_image(input_path):
        out_path = resolve_output_path(input_path, args.output, is_vid=False)
        centers_path = (
            Path(args.centers_json)
            if args.centers_json
            else (input_path.with_suffix(".jsonl"))
        )
        if args.produce_output_video and out_path.exists() and centers_path.exists():
            print(f"[skip] output exists: {out_path}")
            print(f"[skip] centers exists: {centers_path}")
            return
        run_image(
            input_path,
            args.prompt,
            out_path,
            args.scale_width,
            centers_path,
            args.produce_output_video,
        )
        print(f"[image] {input_path} -> {out_path}")
    elif is_video(input_path):
        out_path = resolve_output_path(input_path, args.output, is_vid=True)
        centers_path = (
            Path(args.centers_json)
            if args.centers_json
            else (input_path.with_suffix(".jsonl"))
        )
        if args.produce_output_video and out_path.exists() and centers_path.exists():
            print(f"[skip] output exists: {out_path}")
            print(f"[skip] centers exists: {centers_path}")
            return
        if args.extract_frames and not args.child_run:
            frames_dir = Path(args.frames_dir) if args.frames_dir else None
            run_video_as_frames(
                input_path,
                args.prompt,
                out_path,
                args.scale_width,
                frames_dir,
                args.frames_format,
                args.extract_fps,
                args.stream_extract,
                centers_path,
                args.produce_output_video,
                args.centers_method,
                args.smooth_alpha,
                args.add_center,
            )
        elif args.chunk_seconds is not None and not args.child_run:
            chunks_dir = Path(args.chunks_dir) if args.chunks_dir else None
            run_video_chunked(
                input_path,
                args.prompt,
                out_path,
                args.chunk_seconds,
                keep_chunks=args.keep_chunks,
                chunks_dir=chunks_dir,
                scale_width=args.scale_width,
                centers_path=centers_path,
                produce_output_video=args.produce_output_video,
                centers_method=args.centers_method,
                smooth_alpha=args.smooth_alpha,
                add_center=args.add_center,
            )
        else:
            run_video(
                input_path,
                args.prompt,
                out_path,
                args.max_frames,
                args.scale_width,
                centers_path,
                args.produce_output_video,
                args.centers_method,
                args.smooth_alpha,
                args.add_center,
            )
        print(f"[video] {input_path} -> {out_path}")
    else:
        raise ValueError(f"Unsupported input type: {input_path}")


if __name__ == "__main__":
    main()
