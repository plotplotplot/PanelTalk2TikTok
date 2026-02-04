#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <input_media_file>" >&2
  exit 1
fi

if [[ ! -f hftoken.txt ]]; then
  echo "ERROR: hftoken.txt not found" >&2
  exit 1
fi

HF_TOKEN="$(tr -d '\r\n' < hftoken.txt)"

INPUT="$1"
INPUT_ABS="$(readlink -f "$INPUT")"
INPUT_DIR="$(dirname "$INPUT_ABS")"
INPUT_BASE="$(basename "$INPUT_ABS")"

CACHE_ROOT="$(pwd)/.cache"
mkdir -p "$CACHE_ROOT"/{huggingface,torch,whisperx,matplotlib,xdg-config,home}

docker run --gpus all -it --rm \
  --user "$(id -u)":"$(id -g)" \
  -v "$(pwd)":/app -w /app \
  -v "$INPUT_DIR":/in \
  -v "$CACHE_ROOT/huggingface":/cache/huggingface \
  -v "$CACHE_ROOT/torch":/cache/torch \
  -v "$CACHE_ROOT/whisperx":/cache/whisperx \
  -v "$CACHE_ROOT/matplotlib":/cache/matplotlib \
  -v "$CACHE_ROOT/xdg-config":/cache/config \
  -v "$CACHE_ROOT/home":/cache/home \
  -e HF_TOKEN="$HF_TOKEN" \
  -e HF_HOME=/cache/huggingface \
  -e TORCH_HOME=/cache/torch \
  -e XDG_CACHE_HOME=/cache \
  -e XDG_CONFIG_HOME=/cache/config \
  -e HOME=/cache/home \
  -e MPLCONFIGDIR=/cache/matplotlib \
  ghcr.io/jim60105/whisperx:large-v3-tl-77e20c4 \
  whisperx "/in/$INPUT_BASE" \
    --output_dir "/in" \
    --output_format json \
    --diarize \
    --language en \
    --hf_token "$HF_TOKEN"
