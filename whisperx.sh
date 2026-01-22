#!/usr/bin/env bash
set -euo pipefail

# fail early if token file missing
if [[ ! -f hftoken.txt ]]; then
  echo "ERROR: hftoken.txt not found" >&2
  exit 1
fi

# read token and strip newline/carriage return
HF_TOKEN="$(tr -d '\r\n' < hftoken.txt)"

# optional: avoid exposing token in ps output by not using env var inline on the docker command line
# (here we set it with -e so it is available inside the container)
docker run --gpus all -it \
  -v "$(pwd)":/app -w /app \
  -e HF_TOKEN="$HF_TOKEN" \
  ghcr.io/jim60105/whisperx:large-v3-tl-77e20c4 \
  whisperx "$1" \
    --output_dir /app \
    --output_format json \
    --diarize \
    --language en \
    --hf_token "$HF_TOKEN"

#-v "$(pwd)/.cache":/.cache/torch/hub/checkpoints \