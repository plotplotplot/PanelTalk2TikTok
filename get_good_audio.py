#!/usr/bin/env python3
"""
extract_good_audio.py

Usage:
  python extract_good_audio.py <badclip> <goodmaster>

Example:
  python extract_good_audio.py camera_bad.wav event_good.wav
  -> writes: camera_bad_frommaster.wav

What it does:
  - Decodes BOTH files to mono float32 at a working sample rate (default 11025 Hz)
  - Computes simple spectral-band log-energy features over time
  - Uses FFT-based correlation to find where badclip occurs inside goodmaster
  - Extracts the matching time span from the ORIGINAL goodmaster (at 48 kHz WAV)

Requirements:
  - ffmpeg + ffprobe in PATH
  - pip install numpy scipy
"""

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.signal import fftconvolve


# -----------------------------
# Helpers: tools + decoding
# -----------------------------
def sh(cmd, *, capture=True, check=True):
    if capture:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if check and p.returncode != 0:
            raise RuntimeError(f"Command failed:\n{' '.join(cmd)}\n\nSTDERR:\n{p.stderr}")
        return p
    p = subprocess.run(cmd)
    if check and p.returncode != 0:
        raise RuntimeError(f"Command failed:\n{' '.join(cmd)}")
    return p


def need(bin_name: str):
    from shutil import which
    if which(bin_name) is None:
        print(f"ERROR: missing required tool: {bin_name}", file=sys.stderr)
        sys.exit(2)


def duration_seconds(path: str) -> float:
    p = sh(
        [
            "ffprobe", "-v", "error",
            "-show_entries", "format=duration",
            "-of", "default=noprint_wrappers=1:nokey=1",
            path
        ],
        capture=True,
        check=True
    )
    s = (p.stdout or "").strip()
    if not s:
        raise RuntimeError(f"ffprobe could not read duration for {path}")
    return float(s)

def decode_f32_mono(path: str, sr: int) -> np.ndarray:
    cmd = [
        "ffmpeg", "-v", "error",
        "-i", path,
        "-vn",
        "-ac", "1",
        "-ar", str(sr),
        "-f", "f32le",
        "pipe:1"
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = p.communicate()
    if p.returncode != 0:
        raise RuntimeError(f"ffmpeg decode failed:\n{err.decode('utf-8', errors='replace')}")

    # frombuffer on bytes -> often read-only; copy to make writable
    y = np.frombuffer(out, dtype=np.float32).copy()

    if y.size:
        np.nan_to_num(y, copy=False)  # now safe (writable)
    return y


def extract_from_master(goodmaster: str, *, ss: float, t: float, out_path: str):
    """
    Extract matching segment from the original good master to a 48kHz mono PCM WAV.
    """
    cmd = [
        "ffmpeg", "-y", "-v", "error",
        "-ss", f"{ss:.6f}",
        "-t", f"{t:.6f}",
        "-i", goodmaster,
        "-vn",
        "-ac", "1",
        "-ar", "48000",
        "-c:a", "pcm_s16le",
        out_path
    ]
    p = sh(cmd, capture=True, check=False)
    if p.returncode != 0:
        raise RuntimeError(f"ffmpeg extract failed:\n{p.stderr}")


# -----------------------------
# Matching: FFT-ish features + correlation
# -----------------------------
def band_features(y: np.ndarray, *, n_fft: int, hop: int, n_bands: int) -> np.ndarray:
    """
    Make log band-energy features:
      - frame with hop
      - rfft -> power
      - pool bins into n_bands
      - log1p
    Returns: F shape (n_bands, n_frames)
    """
    if y.size < n_fft:
        y = np.pad(y, (0, n_fft - y.size), mode="constant")

    n_frames = 1 + (y.size - n_fft) // hop
    if n_frames <= 0:
        n_frames = 1

    frames = np.lib.stride_tricks.as_strided(
        y,
        shape=(n_frames, n_fft),
        strides=(y.strides[0] * hop, y.strides[0]),
        writeable=False
    )

    win = np.hanning(n_fft).astype(np.float32)
    X = np.fft.rfft(frames * win[None, :], axis=1)
    P = (X.real * X.real + X.imag * X.imag).astype(np.float32) + 1e-10

    n_bins = P.shape[1]
    start_bin = 1
    usable = max(1, n_bins - start_bin)
    bins_per_band = max(1, usable // n_bands)

    F = np.zeros((n_bands, n_frames), dtype=np.float32)
    for b in range(n_bands):
        a = start_bin + b * bins_per_band
        c = start_bin + (b + 1) * bins_per_band if b < n_bands - 1 else n_bins
        if a >= n_bins:
            break
        band_pow = P[:, a:c].sum(axis=1)
        F[b, :] = np.log1p(band_pow)

    # per-band z-normalize
    mu = F.mean(axis=1, keepdims=True)
    sig = F.std(axis=1, keepdims=True) + 1e-6
    F = (F - mu) / sig
    return F


def collapse(F: np.ndarray) -> np.ndarray:
    """
    Collapse bands -> 1D robust-ish time trajectory, normalize for correlation.
    """
    t = F.mean(axis=0).astype(np.float32)
    t -= t.mean()
    n = np.linalg.norm(t) + 1e-8
    return t / n


def find_offset_seconds(master_y: np.ndarray, clip_y: np.ndarray, *, sr: int, n_fft: int, hop: int, n_bands: int):
    """
    Returns:
      offset_seconds (float): start time in master where clip best matches
      score (float): correlation peak
      frame_index (int): peak index in feature frames
    """
    Fm = band_features(master_y, n_fft=n_fft, hop=hop, n_bands=n_bands)
    Fc = band_features(clip_y,   n_fft=n_fft, hop=hop, n_bands=n_bands)

    tm = collapse(Fm)
    tc = collapse(Fc)

    if tm.size < tc.size:
        raise RuntimeError("Master appears shorter than clip after feature extraction.")

    # correlation via FFT convolution: valid positions only
    corr = fftconvolve(tm, tc[::-1], mode="valid")
    idx = int(np.argmax(corr))
    offset_seconds = (idx * hop) / sr
    return offset_seconds, float(corr[idx]), idx


def main():
    need("ffmpeg")
    need("ffprobe")

    ap = argparse.ArgumentParser()
    ap.add_argument("badclip", help="Bad audio clip (camera audio)")
    ap.add_argument("goodmaster", help="Good full event audio (master)")
    ap.add_argument("--out", default=None, help="Output WAV (default: <badclip_stem>_frommaster.wav)")
    ap.add_argument("--sr", type=int, default=11025, help="Working sample rate for matching (smaller=faster)")
    ap.add_argument("--n_fft", type=int, default=2048, help="FFT size for features")
    ap.add_argument("--hop", type=int, default=512, help="Hop size for features")
    ap.add_argument("--bands", type=int, default=64, help="Number of spectral bands")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    bad = args.badclip
    good = args.goodmaster

    out_path = args.out
    if out_path is None:
        out_path = f"{Path(bad).stem}_frommaster.wav"

    bad_dur = duration_seconds(bad)
    good_dur = duration_seconds(good)

    if args.verbose:
        print(f"badclip:    {bad}  duration={bad_dur:.3f}s")
        print(f"goodmaster: {good} duration={good_dur:.3f}s")
        print(f"decoding both to sr={args.sr} mono float32 for matching...")

    clip_y = decode_f32_mono(bad, sr=args.sr)
    master_y = decode_f32_mono(good, sr=args.sr)

    if clip_y.size < args.n_fft:
        raise RuntimeError("Badclip too short to match reliably. Use a longer clip.")
    if master_y.size < clip_y.size:
        raise RuntimeError("Good master appears shorter than bad clip.")

    if args.verbose:
        print("matching (FFT-based correlation over spectral-band features)...")

    offset_s, score, _ = find_offset_seconds(
        master_y, clip_y,
        sr=args.sr, n_fft=args.n_fft, hop=args.hop, n_bands=args.bands
    )

    # clamp to valid range for extraction
    offset_s = max(0.0, min(offset_s, max(0.0, good_dur - bad_dur)))

    if args.verbose:
        print(f"best offset: {offset_s:.3f} s  (score={score:.4f})")
        print(f"extracting {bad_dur:.3f}s from master to: {out_path}")

    extract_from_master(good, ss=offset_s, t=bad_dur, out_path=out_path)
    print(out_path)


if __name__ == "__main__":
    main()
