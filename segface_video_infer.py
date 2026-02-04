#!/usr/bin/env python3
import sys
import os
import shutil
import subprocess
from pathlib import Path
import zipfile
import urllib.request
import importlib
import pkgutil

import cv2
import numpy as np
import torch
from huggingface_hub import hf_hub_download
from PIL import Image

# ----------------------------
# CONFIG
# ----------------------------
INPUT_RES = 512
INCLUDE_HAIR = False   # set True if you want hair counted as face
WEIGHTS_FILE = "swinb_lapa_512/model_299.pt"

# LaPa class indices (common convention)
FACE_IDXS = [1,2,3,4,5,6,7,8,9]  # skin + facial parts
HAIR_IDX = 10


def ensure_segface_checkout() -> Path:
    """
    Ensure SegFace repo is present. Prefer a persistent cache location.
    Works even if 'git' is not installed by downloading GitHub zip.
    """
    cache_root = Path(os.environ.get("SEGFACE_CACHE", "/data/.cache/segface")).resolve()
    repo_dir = cache_root / "SegFace"

    # If already present, done
    if (repo_dir / "network").exists():
        return repo_dir

    cache_root.mkdir(parents=True, exist_ok=True)

    git = shutil.which("git")
    if git:
        # Clone if missing
        print(f"[segface] cloning via git into {repo_dir} ...", flush=True)
        if repo_dir.exists():
            shutil.rmtree(repo_dir)
        subprocess.run([git, "clone", "--depth", "1", "https://github.com/Kartik-3004/SegFace.git", str(repo_dir)], check=True)
        return repo_dir

    # No git: download zip
    print("[segface] git not found; downloading GitHub zip...", flush=True)
    zip_url = "https://github.com/Kartik-3004/SegFace/archive/refs/heads/main.zip"
    zip_path = cache_root / "SegFace-main.zip"

    # Download (resume-safe enough for typical use)
    urllib.request.urlretrieve(zip_url, zip_path)

    # Extract
    if repo_dir.exists():
        shutil.rmtree(repo_dir)

    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(cache_root)

    extracted = cache_root / "SegFace-main"
    if not extracted.exists():
        raise RuntimeError("Zip extract failed: SegFace-main directory not found")

    extracted.rename(repo_dir)
    return repo_dir


def parse_args():
    # Minimal arg parsing: script.py input.mp4 [--no-view]
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print("Usage: segface_video_infer.py /data/input.mp4 [--no-view]")
        sys.exit(1)

    video = sys.argv[1]
    no_view = (len(sys.argv) == 3 and sys.argv[2] == "--no-view")
    if len(sys.argv) == 3 and not no_view:
        print("Unknown option:", sys.argv[2])
        sys.exit(1)
    return video, no_view


def preprocess(frame_bgr):
    img = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
    img = Image.fromarray(img)

    ow, oh = img.size
    scale = INPUT_RES / min(ow, oh)
    nw, nh = int(ow * scale), int(oh * scale)
    img = img.resize((nw, nh), Image.BILINEAR)

    left = (nw - INPUT_RES) // 2
    top = (nh - INPUT_RES) // 2
    img = img.crop((left, top, left + INPUT_RES, top + INPUT_RES))

    x = np.asarray(img).astype(np.float32) / 255.0
    mean = np.array([0.485, 0.456, 0.406], dtype=np.float32)
    std  = np.array([0.229, 0.224, 0.225], dtype=np.float32)
    x = (x - mean) / std
    x = np.transpose(x, (2, 0, 1))
    x = torch.from_numpy(x).unsqueeze(0)
    return x


def main():
    in_video, no_view = parse_args()

    in_path = Path(in_video)
    if not in_path.exists():
        raise FileNotFoundError(in_path)

    # ----------------------------
    # Bootstrap SegFace (git OR zip)
    # ----------------------------
    segface_dir = ensure_segface_checkout()
    sys.path.insert(0, str(segface_dir))
    # Resolve build_model across possible module layouts
    try:
        from network import build_model  # type: ignore
        build_model_src = "network"
    except Exception:
        import network  # type: ignore

        def _find_build_model(pkg):
            if hasattr(pkg, "build_model"):
                return pkg.build_model, pkg.__name__
            if hasattr(pkg, "__path__"):
                for _, name, _ in pkgutil.iter_modules(pkg.__path__):
                    mod_name = f"{pkg.__name__}.{name}"
                    try:
                        mod = importlib.import_module(mod_name)
                    except Exception:
                        continue
                    if hasattr(mod, "build_model"):
                        return mod.build_model, mod.__name__
                    if hasattr(mod, "__path__"):
                        found = _find_build_model(mod)
                        if found:
                            return found
            return None

        found = _find_build_model(network)
        if not found:
            raise ImportError("Could not locate build_model inside SegFace network package")
        build_model, build_model_src = found
    print(f"[segface] build_model loaded from {build_model_src}", flush=True)

    # ----------------------------
    # Load model
    # ----------------------------
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    model = build_model(
        dataset="lapa",
        backbone="segface_lapa",
        model="swin_base",
        input_resolution=INPUT_RES,
    )

    # Put weights in HF cache (you already mount /root/.cache/huggingface)
    weights_path = hf_hub_download(
        repo_id="kartiknarayan/SegFace",
        filename=WEIGHTS_FILE,
    )

    ckpt = torch.load(weights_path, map_location="cpu")
    state = ckpt.get("model", ckpt)
    model.load_state_dict(state, strict=False)
    model.to(device).eval()
    print("SegFace loaded on", device, flush=True)

    # ----------------------------
    # Video IO
    # ----------------------------
    out_path = in_path.with_name(in_path.stem + "_facemap.mp4")

    cap = cv2.VideoCapture(str(in_path))
    if not cap.isOpened():
        raise RuntimeError("Could not open video")

    fps = cap.get(cv2.CAP_PROP_FPS)
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    if fps <= 0:
        fps = 30.0  # fallback

    writer = cv2.VideoWriter(
        str(out_path),
        cv2.VideoWriter_fourcc(*"mp4v"),
        fps,
        (w, h),
    )
    if not writer.isOpened():
        raise RuntimeError("Could not open VideoWriter (mp4v).")

    def infer_faceprob(x):
        with torch.no_grad():
            y = model(x)
            if isinstance(y, (list, tuple)):
                y = y[0]
            probs = torch.softmax(y, dim=1)

            ids = FACE_IDXS.copy()
            if INCLUDE_HAIR:
                ids.append(HAIR_IDX)

            faceprob = probs[:, ids].sum(dim=1)  # [1,H,W]
            return faceprob[0].detach().cpu().numpy().astype(np.float32)

    # ----------------------------
    # Main loop
    # ----------------------------
    print("Processing video...", flush=True)

    # If no_view, avoid any GUI calls
    if no_view:
        try:
            cv2.destroyAllWindows()
        except Exception:
            pass

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        x = preprocess(frame).to(device)
        faceprob = infer_faceprob(x)

        heat = cv2.resize(faceprob, (w, h), interpolation=cv2.INTER_LINEAR)
        heat_u8 = np.clip(heat * 255.0, 0, 255).astype(np.uint8)

        heat_color = cv2.applyColorMap(heat_u8, cv2.COLORMAP_JET)
        overlay = cv2.addWeighted(frame, 0.6, heat_color, 0.4, 0)

        writer.write(overlay)

        if not no_view:
            cv2.imshow("SegFace Face Probability", overlay)
            if (cv2.waitKey(1) & 0xFF) == 27:
                break

    cap.release()
    writer.release()
    if not no_view:
        cv2.destroyAllWindows()

    print("Done.")
    print("Wrote:", out_path, flush=True)


if __name__ == "__main__":
    main()
