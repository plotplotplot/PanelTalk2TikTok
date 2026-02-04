#!/usr/bin/env bash
# segface_video_docker.sh
#
# Usage:
#   ./segface_video_docker.sh /path/to/input.mp4
#
# Requirements:
#   - Docker installed
#   - segface_video_infer.py in current directory (PWD)
#   - Dockerfile.segface in current directory (PWD)
#
# Notes for GUI (cv2.imshow):
#   On the host, run:
#     xhost +local:docker
#   Then run this script.
#   Afterward:
#     xhost -local:docker

set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-segface:pt231-gui}"
DOCKERFILE="${DOCKERFILE:-Dockerfile.segface}"

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/input.mp4" >&2
  exit 2
fi

IN="$1"
IN_ABS="$(readlink -f "$IN")"
IN_DIR="$(dirname "$IN_ABS")"
IN_BASE="$(basename "$IN_ABS")"

if [[ ! -f "$IN_ABS" ]]; then
  echo "ERROR: Not a file: $IN_ABS" >&2
  exit 1
fi

APP_DIR="$(pwd)"

if [[ ! -f "$APP_DIR/segface_video_infer.py" ]]; then
  echo "ERROR: segface_video_infer.py not found in current directory: $APP_DIR" >&2
  exit 1
fi

if [[ ! -f "$APP_DIR/$DOCKERFILE" ]]; then
  echo "ERROR: $DOCKERFILE not found in current directory: $APP_DIR" >&2
  exit 1
fi

# Persistent caches (recommended)
HF_CACHE_HOST="$APP_DIR/.cache/huggingface"
PIP_CACHE_HOST="$APP_DIR/.cache/pip"
mkdir -p "$HF_CACHE_HOST" "$PIP_CACHE_HOST"

# Compute Dockerfile hash (used to detect "up to date")
if command -v sha256sum >/dev/null 2>&1; then
  DF_HASH="$(sha256sum "$APP_DIR/$DOCKERFILE" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
  DF_HASH="$(shasum -a 256 "$APP_DIR/$DOCKERFILE" | awk '{print $1}')"
else
  echo "ERROR: need sha256sum or shasum to hash Dockerfile" >&2
  exit 1
fi

need_build=0

# If image missing, build
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  need_build=1
else
  # Compare label to current hash
  IMG_HASH="$(docker image inspect "$IMAGE_NAME" --format '{{ index .Config.Labels "segface.build_hash" }}' 2>/dev/null || true)"
  if [[ "$IMG_HASH" != "$DF_HASH" ]]; then
    need_build=1
  fi
fi

if [[ "$need_build" -eq 1 ]]; then
  echo "[build] Building $IMAGE_NAME (Dockerfile hash: $DF_HASH)" >&2
  docker build \
    -t "$IMAGE_NAME" \
    -f "$APP_DIR/$DOCKERFILE" \
    --build-arg "BUILD_HASH=$DF_HASH" \
    "$APP_DIR"
else
  echo "[build] Image up to date: $IMAGE_NAME" >&2
fi

# X11 display passthrough if available
DISPLAY_ARGS=()
if [[ -n "${DISPLAY:-}" && -S /tmp/.X11-unix/X0 ]]; then
  DISPLAY_ARGS+=( -e "DISPLAY=$DISPLAY" -v /tmp/.X11-unix:/tmp/.X11-unix )
fi

docker run --rm -it --gpus all \
  "${DISPLAY_ARGS[@]}" \
  -v "$APP_DIR:/app" -w /app \
  -v "$IN_DIR:/data" \
  -v "$HF_CACHE_HOST:/root/.cache/huggingface" \
  -v "$PIP_CACHE_HOST:/root/.cache/pip" \
  "$IMAGE_NAME" \
  python /app/segface_video_infer.py "/data/$IN_BASE"
