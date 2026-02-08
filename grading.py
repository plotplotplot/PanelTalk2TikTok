#!/usr/bin/env python3
import argparse
from pathlib import Path
import json
import cv2
import numpy as np

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


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def apply_grading(
    frame_bgr,
    shadows=(0.0, 0.0, 0.0),
    midtones=(0.0, 0.0, 0.0),
    highlights=(0.0, 0.0, 0.0),
    levels=None,
    brightness=0.0,
    contrast=1.0,
    saturation=1.0,
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
        )
        writer.write(out)

    cap.release()
    writer.release()


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
    ok, frame = cap.read()
    cap.release()
    if not ok or frame is None:
        raise RuntimeError("Failed to read first frame.")

    grading_path = video_path.with_name(f"{video_path.stem}_grading.json")
    data = load_or_create_grading(grading_path)

    win = "grading"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

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
    cv2.createTrackbar("shad_r", win, to_tb(data["shadows"][0]), 200, noop)
    cv2.createTrackbar("shad_g", win, to_tb(data["shadows"][1]), 200, noop)
    cv2.createTrackbar("shad_b", win, to_tb(data["shadows"][2]), 200, noop)
    # Midtones
    cv2.createTrackbar("mid_r", win, to_tb(data["midtones"][0]), 200, noop)
    cv2.createTrackbar("mid_g", win, to_tb(data["midtones"][1]), 200, noop)
    cv2.createTrackbar("mid_b", win, to_tb(data["midtones"][2]), 200, noop)
    # Highlights
    cv2.createTrackbar("hi_r", win, to_tb(data["highlights"][0]), 200, noop)
    cv2.createTrackbar("hi_g", win, to_tb(data["highlights"][1]), 200, noop)
    cv2.createTrackbar("hi_b", win, to_tb(data["highlights"][2]), 200, noop)
    # Levels
    cv2.createTrackbar("in_black", win, int(data["levels"][0]), 255, noop)
    cv2.createTrackbar("in_white", win, int(data["levels"][1]), 255, noop)
    cv2.createTrackbar("gamma", win, to_gamma_tb(data["levels"][2]), 300, noop)
    cv2.createTrackbar("out_black", win, int(data["levels"][3]), 255, noop)
    cv2.createTrackbar("out_white", win, int(data["levels"][4]), 255, noop)
    # Brightness / contrast
    cv2.createTrackbar("brightness", win, to_tb(data.get("brightness", 0.0)), 200, noop)
    cv2.createTrackbar("contrast", win, to_contrast_tb(data.get("contrast", 1.0)), 300, noop)
    cv2.createTrackbar("saturation", win, to_saturation_tb(data.get("saturation", 1.0)), 300, noop)
    # Save button
    cv2.createTrackbar("save", win, 0, 1, noop)

    last_save = 0
    while True:
        sh = (
            from_tb(cv2.getTrackbarPos("shad_r", win)),
            from_tb(cv2.getTrackbarPos("shad_g", win)),
            from_tb(cv2.getTrackbarPos("shad_b", win)),
        )
        mi = (
            from_tb(cv2.getTrackbarPos("mid_r", win)),
            from_tb(cv2.getTrackbarPos("mid_g", win)),
            from_tb(cv2.getTrackbarPos("mid_b", win)),
        )
        hi = (
            from_tb(cv2.getTrackbarPos("hi_r", win)),
            from_tb(cv2.getTrackbarPos("hi_g", win)),
            from_tb(cv2.getTrackbarPos("hi_b", win)),
        )
        levels = (
            float(cv2.getTrackbarPos("in_black", win)),
            float(max(cv2.getTrackbarPos("in_white", win), 1)),
            from_gamma_tb(cv2.getTrackbarPos("gamma", win)),
            float(cv2.getTrackbarPos("out_black", win)),
            float(cv2.getTrackbarPos("out_white", win)),
        )
        brightness = from_tb(cv2.getTrackbarPos("brightness", win))
        contrast = from_contrast_tb(cv2.getTrackbarPos("contrast", win))
        saturation = from_saturation_tb(cv2.getTrackbarPos("saturation", win))

        preview = apply_grading(
            frame,
            shadows=sh,
            midtones=mi,
            highlights=hi,
            levels=levels,
            brightness=brightness,
            contrast=contrast,
            saturation=saturation,
        )
        cv2.imshow(win, preview)

        save_pos = cv2.getTrackbarPos("save", win)
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
            cv2.setTrackbarPos("save", win, 0)
        last_save = save_pos

        key = cv2.waitKey(10) & 0xFF
        if key in (27, ord("q")):
            break

    cv2.destroyAllWindows()


def main():
    parser = argparse.ArgumentParser(description="Simple RGB grading with tonal controls.")
    parser.add_argument("input", help="Input image or video path")
    parser.add_argument("output", nargs="?", help="Output image or video path")
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
    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output) if args.output else None

    img = cv2.imread(str(input_path), cv2.IMREAD_COLOR)
    if img is not None:
        if output_path is None:
            raise RuntimeError("Output path required for images.")
        process_image(input_path, output_path, args)
        return

    if output_path is None:
        edit_grading(input_path)
        return

    process_video(input_path, output_path, args)


if __name__ == "__main__":
    main()
