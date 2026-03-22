#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TOKEN_FILE="$PROJECT_ROOT/hftoken.txt"
CACHE_DIR="$PROJECT_ROOT/.cache"
DOCKER_INSTALL_URL="https://docs.docker.com/engine/install/"
NVIDIA_CONTAINER_URL="https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/1.12.0/install-guide.html"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <media-file>" >&2
  exit 1
fi

INPUT_PATH="$1"
if [[ "$INPUT_PATH" != /* ]]; then
  INPUT_PATH="$(cd "$(dirname "$INPUT_PATH")" && pwd)/$(basename "$INPUT_PATH")"
fi

if [[ ! -f "$INPUT_PATH" ]]; then
  echo "ERROR: input file not found: $INPUT_PATH" >&2
  exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: Docker is not installed or not on PATH." >&2
  echo "Install Docker first: $DOCKER_INSTALL_URL" >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "ERROR: Docker is installed, but the Docker daemon is not reachable." >&2
  echo "Start Docker and verify 'docker info' works, then retry." >&2
  echo "Docker install docs: $DOCKER_INSTALL_URL" >&2
  exit 1
fi

DOCKER_RUNTIMES="$(docker info --format '{{json .Runtimes}}' 2>/dev/null || true)"
HAS_NVIDIA_RUNTIME=0
if [[ "$DOCKER_RUNTIMES" == *'"nvidia"'* ]]; then
  HAS_NVIDIA_RUNTIME=1
fi
if command -v nvidia-container-toolkit >/dev/null 2>&1 || command -v nvidia-ctk >/dev/null 2>&1; then
  HAS_NVIDIA_RUNTIME=1
fi
if [[ $HAS_NVIDIA_RUNTIME -ne 1 ]]; then
  echo "ERROR: NVIDIA Container Toolkit does not appear to be installed or configured for Docker." >&2
  echo "Install or configure it here: $NVIDIA_CONTAINER_URL" >&2
  exit 1
fi

if [[ ! -f "$TOKEN_FILE" ]]; then
  echo "No Hugging Face token found at $TOKEN_FILE"
  printf "Enter Hugging Face token: " >&2
  read -r HF_TOKEN_INPUT
  if [[ -z "${HF_TOKEN_INPUT:-}" ]]; then
    echo "ERROR: token is required" >&2
    exit 1
  fi
  printf '%s\n' "$HF_TOKEN_INPUT" > "$TOKEN_FILE"
  chmod 600 "$TOKEN_FILE"
  echo "Saved token to $TOKEN_FILE"
fi

# read token and strip newline/carriage return
HF_TOKEN="$(tr -d '\r\n' < "$TOKEN_FILE")"

# ensure local cache dir exists (mapped into container)
mkdir -p "$CACHE_DIR"

# write outputs alongside the input file
INPUT_DIR="$(dirname "$INPUT_PATH")"
INPUT_BASENAME="$(basename "$INPUT_PATH")"
CONTAINER_MEDIA_DIR="/media"
CONTAINER_INPUT_PATH="$CONTAINER_MEDIA_DIR/$INPUT_BASENAME"
OUT_DIR="$CONTAINER_MEDIA_DIR"

DOCKER_TTY_ARGS=(-i)
if [[ -t 0 && -t 1 ]]; then
  DOCKER_TTY_ARGS+=(-t)
fi

# optional: avoid exposing token in ps output by not using env var inline on the docker command line
# (here we set it with -e so it is available inside the container)
docker run --gpus all "${DOCKER_TTY_ARGS[@]}" \
  --user "$(id -u):$(id -g)" \
  -v "$CACHE_DIR":/.cache \
  -v "$CACHE_DIR":/tmp/.cache \
  -v "$PROJECT_ROOT":/app -w /app \
  -v "$INPUT_DIR":"$CONTAINER_MEDIA_DIR" \
  -e HOME="/tmp" \
  -e HF_TOKEN="$HF_TOKEN" \
  -e MPLCONFIGDIR="/tmp/.cache/matplotlib" \
  -e HF_HOME="/tmp/.cache/huggingface" \
  -e HF_HUB_CACHE="/tmp/.cache/huggingface/hub" \
  -e TRANSFORMERS_CACHE="/tmp/.cache/huggingface/transformers" \
  -e TORCH_HOME="/tmp/.cache/torch" \
  ghcr.io/jim60105/whisperx:large-v3-tl-77e20c4 \
  whisperx "$CONTAINER_INPUT_PATH" \
    --output_dir "$OUT_DIR" \
    --output_format json \
    --diarize \
    --language en \
    --hf_token "$HF_TOKEN"

#-v "$(pwd)/.cache":/.cache/torch/hub/checkpoints \
