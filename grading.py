#!/usr/bin/env python3
import argparse
from pathlib import Path
import json
import cv2
import numpy as np
from tqdm import tqdm

try:
    import motive2d_engine as _cg
except Exception:
    _cg = None
if _cg is None:
    try:
        import motive2d_color_grading as _cg
    except Exception:
        _cg = None


def parse_rgb(value: str):
    parts = [p.strip() for p in value.split(",")]
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("RGB value must be in form r,g,b")
    try:
        return tuple(float(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError("RGB value must be numeric (r,g,b)")


def parse_levels(value: str):
    parts = [p.strip() for p in value.split(",")]
    if len(parts) != 5:
        raise argparse.ArgumentTypeError(
            "Levels must be in form in_black,in_white,gamma,out_black,out_white"
        )
    try:
        ib, iw, gamma, ob, ow = (float(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError("Levels values must be numeric")
    if iw <= ib:
        raise argparse.ArgumentTypeError("in_white must be greater than in_black")
    if gamma <= 0:
        raise argparse.ArgumentTypeError("gamma must be > 0")
    return ib, iw, gamma, ob, ow


def parse_tone_edges(value: str):
    parts = [p.strip() for p in value.split(",")]
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("Tone edges must be in form shadow_end,highlight_start")
    try:
        sh_end, hi_start = (float(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError("Tone edges must be numeric")
    if not (0.0 <= sh_end < hi_start <= 1.0):
        raise argparse.ArgumentTypeError("Tone edges must satisfy 0 <= shadow_end < highlight_start <= 1")
    return sh_end, hi_start


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def apply_tonal_adjustment(frame_bgr, shadows, midtones, highlights, tone_edges):
    frame = frame_bgr.astype(np.float32)
    b, g, r = cv2.split(frame)
    luma = (0.0722 * b + 0.7152 * g + 0.2126 * r) / 255.0

    sh_end, hi_start = tone_edges
    w_sh = 1.0 - smoothstep(0.0, sh_end, luma)
    w_hi = smoothstep(hi_start, 1.0, luma)
    w_mid = 1.0 - (w_sh + w_hi)
    w_mid = np.clip(w_mid, 0.0, 1.0)

    sh = np.array(shadows, dtype=np.float32) * 255.0
    mi = np.array(midtones, dtype=np.float32) * 255.0
    hi = np.array(highlights, dtype=np.float32) * 255.0

    for chan, idx in ((b, 0), (g, 1), (r, 2)):
        chan += sh[idx] * w_sh + mi[idx] * w_mid + hi[idx] * w_hi

    out = cv2.merge((b, g, r))
    return np.clip(out, 0, 255).astype(np.uint8)


def apply_grading(
    frame_bgr,
    shadows=(0.0, 0.0, 0.0),
    midtones=(0.0, 0.0, 0.0),
    highlights=(0.0, 0.0, 0.0),
    levels=None,
    brightness=0.0,
    contrast=1.0,
    saturation=1.0,
    tone_edges=None,
):
    if _cg is not None:
        return _cg.apply_grading(
            frame_bgr,
            shadows,
            midtones,
            highlights,
            levels,
            float(brightness),
            float(contrast),
            float(saturation),
        )
    raise RuntimeError(
        "Color grading engine unavailable: motive2d_engine / motive2d_color_grading failed to import."
    )
    frame = frame_bgr.astype(np.float32)
    b, g, r = cv2.split(frame)
    # Luma for weighting
    luma = (0.0722 * b + 0.7152 * g + 0.2126 * r) / 255.0

    w_sh = 1.0 - smoothstep(0.0, 0.4, luma)
    w_hi = smoothstep(0.6, 1.0, luma)
    w_mid = 1.0 - (w_sh + w_hi)
    w_mid = np.clip(w_mid, 0.0, 1.0)

    sh = np.array(shadows, dtype=np.float32) * 255.0
    mi = np.array(midtones, dtype=np.float32) * 255.0
    hi = np.array(highlights, dtype=np.float32) * 255.0

    for chan, idx in ((b, 0), (g, 1), (r, 2)):
        chan += sh[idx] * w_sh + mi[idx] * w_mid + hi[idx] * w_hi

    out = cv2.merge((b, g, r))

    if levels is not None:
        ib, iw, gamma, ob, ow = levels
        ib /= 255.0
        iw /= 255.0
        ob /= 255.0
        ow /= 255.0

        v = out / 255.0
        v = (v - ib) / (iw - ib)
        v = np.clip(v, 0.0, 1.0)
        v = np.power(v, 1.0 / gamma)
        v = ob + v * (ow - ob)
        out = np.clip(v * 255.0, 0.0, 255.0)

    if brightness != 0.0 or contrast != 1.0:
        out = out * float(contrast) + float(brightness) * 255.0

    if saturation != 1.0:
        hsv = cv2.cvtColor(np.clip(out, 0, 255).astype(np.uint8), cv2.COLOR_BGR2HSV).astype(
            np.float32
        )
        hsv[:, :, 1] = np.clip(hsv[:, :, 1] * float(saturation), 0, 255)
        out = cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)

    return np.clip(out, 0, 255).astype(np.uint8)


def process_image(input_path: Path, output_path: Path, args):
    img = cv2.imread(str(input_path), cv2.IMREAD_COLOR)
    if img is None:
        raise RuntimeError(f"Failed to read image: {input_path}")
    out = apply_grading(
        img,
        shadows=args.shadows,
        midtones=args.midtones,
        highlights=args.highlights,
        levels=args.levels,
        brightness=args.brightness,
        contrast=args.contrast,
        saturation=args.saturation,
        tone_edges=args.tone_edges,
    )
    if not cv2.imwrite(str(output_path), out):
        raise RuntimeError(f"Failed to write image: {output_path}")


def process_video(input_path: Path, output_path: Path, args):
    cap = cv2.VideoCapture(str(input_path))
    if not cap.isOpened():
        raise RuntimeError(f"Failed to open video: {input_path}")

    fps = cap.get(cv2.CAP_PROP_FPS)
    if not fps or fps <= 0:
        fps = 30.0
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    writer = cv2.VideoWriter(str(output_path), fourcc, fps, (width, height))

    if args.visualize:
        cv2.namedWindow("grading_preview", cv2.WINDOW_NORMAL)

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    with tqdm(total=total_frames, desc="Grading video", unit="frame") as pbar:
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            out = apply_grading(
                frame,
                shadows=args.shadows,
                midtones=args.midtones,
                highlights=args.highlights,
                levels=args.levels,
                brightness=args.brightness,
                contrast=args.contrast,
                saturation=args.saturation,
                tone_edges=args.tone_edges,
            )
            if out is not None and out.ndim == 3 and out.shape[2] == 4:
                out = cv2.cvtColor(out, cv2.COLOR_BGRA2BGR)
            writer.write(out)
            if args.visualize:
                cv2.imshow("grading_preview", out)
                if cv2.waitKey(1) & 0xFF in (27, ord("q")):
                    break
            pbar.update(1)

    cap.release()
    writer.release()
    if args.visualize:
        cv2.destroyWindow("grading_preview")


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


def edit_grading(video_path: Path):
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise RuntimeError(f"Failed to open video: {video_path}")
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    if total_frames <= 0:
        total_frames = 1
    ok, frame = cap.read()
    if not ok or frame is None:
        cap.release()
        raise RuntimeError("Failed to read first frame.")

    grading_path = video_path.with_name(f"{video_path.stem}_grading.json")
    data = load_or_create_grading(grading_path)

    preview_win = "grading_preview"
    controls_win = "grading_controls"
    cv2.namedWindow(preview_win, cv2.WINDOW_NORMAL)
    cv2.namedWindow(controls_win, cv2.WINDOW_NORMAL)

    def to_tb(v):
        return int(round((float(v) + 1.0) * 100.0))

    def from_tb(v):
        return (float(v) / 100.0) - 1.0

    def to_gamma_tb(v):
        return int(round(float(v) * 100.0))

    def from_gamma_tb(v):
        return max(0.1, float(v) / 100.0)

    def to_contrast_tb(v):
        return int(round(float(v) * 100.0))

    def from_contrast_tb(v):
        return max(0.0, float(v) / 100.0)

    def to_saturation_tb(v):
        return int(round(float(v) * 100.0))

    def from_saturation_tb(v):
        return max(0.0, float(v) / 100.0)

    def noop(_):
        pass

    # Shadows
    cv2.createTrackbar("shad_r", controls_win, to_tb(data["shadows"][0]), 200, noop)
    cv2.createTrackbar("shad_g", controls_win, to_tb(data["shadows"][1]), 200, noop)
    cv2.createTrackbar("shad_b", controls_win, to_tb(data["shadows"][2]), 200, noop)
    # Midtones
    cv2.createTrackbar("mid_r", controls_win, to_tb(data["midtones"][0]), 200, noop)
    cv2.createTrackbar("mid_g", controls_win, to_tb(data["midtones"][1]), 200, noop)
    cv2.createTrackbar("mid_b", controls_win, to_tb(data["midtones"][2]), 200, noop)
    # Highlights
    cv2.createTrackbar("hi_r", controls_win, to_tb(data["highlights"][0]), 200, noop)
    cv2.createTrackbar("hi_g", controls_win, to_tb(data["highlights"][1]), 200, noop)
    cv2.createTrackbar("hi_b", controls_win, to_tb(data["highlights"][2]), 200, noop)
    # Levels
    cv2.createTrackbar("in_black", controls_win, int(data["levels"][0]), 255, noop)
    cv2.createTrackbar("in_white", controls_win, int(data["levels"][1]), 255, noop)
    cv2.createTrackbar("gamma", controls_win, to_gamma_tb(data["levels"][2]), 300, noop)
    cv2.createTrackbar("out_black", controls_win, int(data["levels"][3]), 255, noop)
    cv2.createTrackbar("out_white", controls_win, int(data["levels"][4]), 255, noop)
    # Brightness / contrast
    cv2.createTrackbar("brightness", controls_win, to_tb(data.get("brightness", 0.0)), 200, noop)
    cv2.createTrackbar("contrast", controls_win, to_contrast_tb(data.get("contrast", 1.0)), 300, noop)
    cv2.createTrackbar("saturation", controls_win, to_saturation_tb(data.get("saturation", 1.0)), 300, noop)
    # Save button
    cv2.createTrackbar("save", controls_win, 0, 1, noop)
    # Preview toggle (1 = graded, 0 = original)
    cv2.createTrackbar("preview", controls_win, 1, 1, noop)
    # Frame scrubber
    cv2.createTrackbar("frame", controls_win, 0, max(0, total_frames - 1), noop)

    # Put controls to the right of the preview window
    cv2.imshow(preview_win, frame)
    cv2.moveWindow(preview_win, 0, 0)
    cv2.moveWindow(controls_win, frame.shape[1] + 20, 0)

    last_save = 0
    last_frame_idx = 0
    while True:
        frame_idx = cv2.getTrackbarPos("frame", controls_win)
        if frame_idx != last_frame_idx:
            cap.set(cv2.CAP_PROP_POS_FRAMES, frame_idx)
            ok, new_frame = cap.read()
            if ok and new_frame is not None:
                frame = new_frame
                last_frame_idx = frame_idx

        sh = (
            from_tb(cv2.getTrackbarPos("shad_r", controls_win)),
            from_tb(cv2.getTrackbarPos("shad_g", controls_win)),
            from_tb(cv2.getTrackbarPos("shad_b", controls_win)),
        )
        mi = (
            from_tb(cv2.getTrackbarPos("mid_r", controls_win)),
            from_tb(cv2.getTrackbarPos("mid_g", controls_win)),
            from_tb(cv2.getTrackbarPos("mid_b", controls_win)),
        )
        hi = (
            from_tb(cv2.getTrackbarPos("hi_r", controls_win)),
            from_tb(cv2.getTrackbarPos("hi_g", controls_win)),
            from_tb(cv2.getTrackbarPos("hi_b", controls_win)),
        )
        levels = (
            float(cv2.getTrackbarPos("in_black", controls_win)),
            float(max(cv2.getTrackbarPos("in_white", controls_win), 1)),
            from_gamma_tb(cv2.getTrackbarPos("gamma", controls_win)),
            float(cv2.getTrackbarPos("out_black", controls_win)),
            float(cv2.getTrackbarPos("out_white", controls_win)),
        )
        brightness = from_tb(cv2.getTrackbarPos("brightness", controls_win))
        contrast = from_contrast_tb(cv2.getTrackbarPos("contrast", controls_win))
        saturation = from_saturation_tb(cv2.getTrackbarPos("saturation", controls_win))

        if cv2.getTrackbarPos("preview", controls_win) == 1:
            preview = apply_grading(
                frame,
                shadows=sh,
                midtones=mi,
                highlights=hi,
                levels=levels,
                brightness=brightness,
                contrast=contrast,
                saturation=saturation,
                tone_edges=None,
            )
        else:
            preview = frame
        cv2.imshow(preview_win, preview)

        save_pos = cv2.getTrackbarPos("save", controls_win)
        if save_pos == 1 and last_save == 0:
            payload = {
                "shadows": [float(x) for x in sh],
                "midtones": [float(x) for x in mi],
                "highlights": [float(x) for x in hi],
                "levels": [float(x) for x in levels],
                "brightness": float(brightness),
                "contrast": float(contrast),
                "saturation": float(saturation),
            }
            with grading_path.open("w", encoding="utf-8") as f:
                json.dump(payload, f, indent=2)
            cv2.setTrackbarPos("save", controls_win, 0)
        last_save = save_pos

        key = cv2.waitKey(10) & 0xFF
        if key in (27, ord("q")):
            break

    cap.release()
    cv2.destroyAllWindows()


def main():
    parser = argparse.ArgumentParser(description="Simple RGB grading with tonal controls.")
    parser.add_argument("input", help="Input image or video path")
    parser.add_argument("output", nargs="?", help="Output image or video path")
    parser.add_argument(
        "--gui",
        action="store_true",
        help="Open interactive grading GUI on the first frame.",
    )
    parser.add_argument(
        "--visualize",
        action="store_true",
        help="Show a live preview while rendering video output.",
    )
    parser.add_argument(
        "--shadows",
        type=parse_rgb,
        default=(0.0, 0.0, 0.0),
        help="RGB brightness in shadows (-1..1 per channel), e.g. 0.05,0,0",
    )
    parser.add_argument(
        "--midtones",
        type=parse_rgb,
        default=(0.0, 0.0, 0.0),
        help="RGB brightness in midtones (-1..1 per channel)",
    )
    parser.add_argument(
        "--highlights",
        type=parse_rgb,
        default=(0.0, 0.0, 0.0),
        help="RGB brightness in highlights (-1..1 per channel)",
    )
    parser.add_argument(
        "--levels",
        type=parse_levels,
        default=None,
        help="Levels curve: in_black,in_white,gamma,out_black,out_white (0-255)",
    )
    parser.add_argument(
        "--brightness",
        type=float,
        default=0.0,
        help="Brightness adjustment (-1..1).",
    )
    parser.add_argument(
        "--contrast",
        type=float,
        default=1.0,
        help="Contrast multiplier (>=0).",
    )
    parser.add_argument(
        "--saturation",
        type=float,
        default=1.0,
        help="Saturation multiplier (>=0).",
    )
    parser.add_argument(
        "--tone-edges",
        type=parse_tone_edges,
        default=None,
        help="Shadow/highlight boundaries (0..1 luma): shadow_end,highlight_start. "
        "If set, tonal weighting is applied after levels/gamma/brightness/contrast.",
    )
    args = parser.parse_args()
    if args.tone_edges is not None:
        raise RuntimeError(
            "--tone-edges is not supported in GPU-only mode (motive2d engine only)."
        )

    input_path = Path(args.input)
    output_path = Path(args.output) if args.output else None

    if args.gui:
        edit_grading(input_path)
        return

    img = cv2.imread(str(input_path), cv2.IMREAD_COLOR)
    if img is not None:
        if output_path is None:
            raise RuntimeError("Output path required for images.")
        process_image(input_path, output_path, args)
        return

    grading_path = input_path.with_name(f"{input_path.stem}_grading.json")
    if grading_path.exists():
        data = load_or_create_grading(grading_path)
        args.shadows = tuple(data.get("shadows", (0.0, 0.0, 0.0)))
        args.midtones = tuple(data.get("midtones", (0.0, 0.0, 0.0)))
        args.highlights = tuple(data.get("highlights", (0.0, 0.0, 0.0)))
        args.levels = tuple(data.get("levels")) if data.get("levels") is not None else None
        args.brightness = float(data.get("brightness", 0.0))
        args.contrast = float(data.get("contrast", 1.0))
        args.saturation = float(data.get("saturation", 1.0))

    if output_path is None:
        output_path = input_path.with_name(f"{input_path.stem}_graded.mp4")

    process_video(input_path, output_path, args)


if __name__ == "__main__":
    main()
