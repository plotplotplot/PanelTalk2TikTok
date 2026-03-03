#!/usr/bin/env bash
set -euo pipefail

# fail early if token file missing
if [[ ! -f hftoken.txt ]]; then
  echo "ERROR: hftoken.txt not found" >&2
  exit 1
fi

# read token and strip newline/carriage return
HF_TOKEN="$(tr -d '\r\n' < hftoken.txt)"

# ensure local cache dir exists (mapped into container)
mkdir -p .cache

# write outputs alongside the input file
OUT_DIR="$(dirname "$1")"

# optional: avoid exposing token in ps output by not using env var inline on the docker command line
# (here we set it with -e so it is available inside the container)
docker run --gpus all -it \
  -v "$(pwd)/.cache":/.cache \
  -v "$(pwd)/.cache":/root/.cache \
  -v "$(pwd)":/app -w /app \
  -e HOME="/root" \
  -e HF_TOKEN="$HF_TOKEN" \
  -e MPLCONFIGDIR="/root/.cache/matplotlib" \
  -e HF_HOME="/root/.cache/huggingface" \
  -e HF_HUB_CACHE="/root/.cache/huggingface/hub" \
  -e TRANSFORMERS_CACHE="/root/.cache/huggingface/transformers" \
  -e TORCH_HOME="/root/.cache/torch" \
  ghcr.io/jim60105/whisperx:large-v3-tl-77e20c4 \
  whisperx "$1" \
    --output_dir "$OUT_DIR" \
    --output_format json \
    --diarize \
    --language en \
    --hf_token "$HF_TOKEN"

#-v "$(pwd)/.cache":/.cache/torch/hub/checkpoints \
