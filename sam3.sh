#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$APP_DIR"
SAM3_DIR="$ROOT_DIR/sam3"
IMAGE_NAME="${IMAGE_NAME:-sam3:cu126}"
DOCKERFILE="${DOCKERFILE:-$SAM3_DIR/Dockerfile}"

usage() {
  cat >&2 <<'EOF'
Usage:
  ./sam3.sh <input_path> --prompt "text" [--out /path/to/output] [--max-frames N]
  ./sam3.sh <input_path> --prompt "text" [--out /path/to/output] [--max-frames N] --shell
  ./sam3.sh <input_path> --prompt "text" [--extract-frames] [--scale-width N] [--prescale-width N] [--intermediate-frames-format jpg|png]
  ./sam3.sh <input_path> --prompt "text" [--backend sam3|sam2|mobilesam]
  ./sam3.sh <input_path> --prompt "text" [--profile]
EOF
}

if [[ $# -lt 1 ]]; then
  usage
  exit 2
fi

INPUT=""
PROMPT=""
OUT_HOST=""
MAX_FRAMES=""
SHELL_MODE=0
PROFILE_MODE=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prompt)
      PROMPT="${2:-}"
      shift 2
      ;;
    --out)
      OUT_HOST="${2:-}"
      shift 2
      ;;
    --max-frames)
      MAX_FRAMES="${2:-}"
      shift 2
      ;;
    --shell)
      SHELL_MODE=1
      shift
      ;;
    --profile)
      PROFILE_MODE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      if [[ -z "$INPUT" ]]; then
        INPUT="$1"
        shift
      else
        if [[ "$1" == --* ]]; then
          if [[ $# -gt 1 && "${2:-}" != --* ]]; then
            EXTRA_ARGS+=("$1" "$2")
            shift 2
          else
            EXTRA_ARGS+=("$1")
            shift
          fi
        else
          echo "ERROR: Unknown arg: $1" >&2
          usage
          exit 2
        fi
      fi
      ;;
  esac
done

if [[ -z "$INPUT" || -z "$PROMPT" ]]; then
  if [[ -z "$INPUT" ]]; then
    usage
    exit 2
  fi
fi

if [[ -z "$PROMPT" ]]; then
  PROMPT="human head, face, hair, ears, hat"
  echo "[debug] Default prompt: \"$PROMPT\"" >&2
fi

has_arg() {
  local needle="$1"
  for a in "${EXTRA_ARGS[@]}"; do
    if [[ "$a" == "$needle" ]]; then
      return 0
    fi
  done
  return 1
}

if ! has_arg "--extract-frames"; then
  EXTRA_ARGS+=("--extract-frames")
  echo "[debug] Defaulting to --extract-frames" >&2
fi
if ! has_arg "--stream-extract"; then
  EXTRA_ARGS+=("--stream-extract")
  echo "[debug] Defaulting to --stream-extract" >&2
fi

IN_ABS="$(readlink -f "$INPUT" 2>/dev/null || true)"
if [[ -z "$IN_ABS" ]]; then
  IN_ABS="$INPUT"
fi
if [[ ! -e "$IN_ABS" ]]; then
  echo "ERROR: Input not found: $IN_ABS" >&2
  exit 1
fi

if [[ ! -f "$ROOT_DIR/hftoken.txt" ]]; then
  echo "ERROR: HF token file not found: $ROOT_DIR/hftoken.txt" >&2
  exit 1
fi

if [[ -f "$IN_ABS" ]]; then
  IN_DIR="$(dirname "$IN_ABS")"
  IN_CONTAINER="/data/$(basename "$IN_ABS")"
elif [[ -d "$IN_ABS" ]]; then
  IN_DIR="$IN_ABS"
  IN_CONTAINER="/data"
else
  echo "ERROR: Input is not a file or directory: $IN_ABS" >&2
  exit 1
fi

if [[ -z "$OUT_HOST" ]]; then
  OUT_HOST="$IN_DIR"
fi
mkdir -p "$OUT_HOST"

HF_CACHE_HOST="$ROOT_DIR/.cache/hf"
PIP_CACHE_HOST="$ROOT_DIR/.cache/pip"
mkdir -p "$HF_CACHE_HOST" "$PIP_CACHE_HOST"

# Prefer host code if available
EXTRA_MOUNTS=()
if [[ -d "$ROOT_DIR/sam3" ]]; then
  echo "[debug] Using host code override: $ROOT_DIR/sam3 -> /workspace/sam3" >&2
  EXTRA_MOUNTS+=( -v "$ROOT_DIR/sam3:/workspace/sam3" )
fi

is_video_ext() {
  local p="$1"
  local ext="${p##*.}"
  ext="${ext,,}"
  case "$ext" in
    mp4|mov|mkv|avi|webm|mpeg|mpg|m4v) return 0 ;;
    *) return 1 ;;
  esac
}

ratio_to_decimal() {
  local value="${1:-0/1}"
  awk -v r="$value" 'BEGIN {
    n = split(r, a, "/");
    if (n == 2 && a[2] + 0 != 0) {
      printf "%.6f", (a[1] + 0) / (a[2] + 0);
    } else {
      printf "%.6f", r + 0;
    }
  }'
}

check_video_timing() {
  local input_path="${1:?}"
  if ! command -v ffprobe >/dev/null 2>&1; then
    echo "[warn] ffprobe not found; skipping VFR/sync preflight checks" >&2
    return 0
  fi

  local stream_info
  stream_info="$(ffprobe -hide_banner -loglevel error \
    -select_streams v:0 \
    -show_entries stream=r_frame_rate,avg_frame_rate,time_base,nb_frames,duration \
    -of default=nokey=0:noprint_wrappers=1 \
    "$input_path" 2>/dev/null || true)"
  if [[ -z "$stream_info" ]]; then
    echo "[warn] Unable to read video timing metadata from: $input_path" >&2
    return 0
  fi

  local r_frame_rate avg_frame_rate v_duration nb_frames
  r_frame_rate="$(awk -F= '/^r_frame_rate=/{print $2}' <<<"$stream_info")"
  avg_frame_rate="$(awk -F= '/^avg_frame_rate=/{print $2}' <<<"$stream_info")"
  v_duration="$(awk -F= '/^duration=/{print $2}' <<<"$stream_info")"
  nb_frames="$(awk -F= '/^nb_frames=/{print $2}' <<<"$stream_info")"

  local r_fps avg_fps fps_delta_pct
  r_fps="$(ratio_to_decimal "${r_frame_rate:-0/1}")"
  avg_fps="$(ratio_to_decimal "${avg_frame_rate:-0/1}")"
  fps_delta_pct="$(awk -v r="$r_fps" -v a="$avg_fps" 'BEGIN {
    if (r <= 0 || a <= 0) {
      printf "%.3f", 0;
    } else {
      d = r - a;
      if (d < 0) d = -d;
      printf "%.3f", (d / r) * 100.0;
    }
  }')"

  echo "[timing] video fps nominal=$r_fps avg=$avg_fps frames=${nb_frames:-unknown} duration=${v_duration:-unknown}s" >&2
  if awk -v d="$fps_delta_pct" 'BEGIN { exit !(d > 0.2) }'; then
    echo "[warn] Video looks VFR-ish or timestamp-irregular: nominal/avg fps differ by ${fps_delta_pct}%." >&2
    echo "[warn] If frame/audio sync matters, consider remuxing or re-encoding with regenerated timestamps first." >&2
  fi

  local a_duration duration_delta
  a_duration="$(ffprobe -hide_banner -loglevel error \
    -select_streams a:0 \
    -show_entries stream=duration \
    -of default=nokey=1:noprint_wrappers=1 \
    "$input_path" 2>/dev/null | head -n 1 || true)"
  if [[ -n "${a_duration:-}" && -n "${v_duration:-}" ]]; then
    duration_delta="$(awk -v a="$a_duration" -v v="$v_duration" 'BEGIN {
      d = a - v;
      if (d < 0) d = -d;
      printf "%.3f", d;
    }')"
    echo "[timing] audio duration=${a_duration}s delta_vs_video=${duration_delta}s" >&2
    if awk -v d="$duration_delta" 'BEGIN { exit !(d > 0.2) }'; then
      echo "[warn] Audio/video stream durations differ by ${duration_delta}s. Sync-sensitive frame extraction may drift." >&2
    fi
  fi

  local pkt_sample pkt_issues
  pkt_sample="$(ffprobe -hide_banner -loglevel error \
    -select_streams v:0 \
    -show_packets \
    -show_entries packet=dts_time \
    -of csv=p=0 \
    "$input_path" 2>/dev/null | head -n 2000 || true)"
  if [[ -n "$pkt_sample" ]]; then
    pkt_issues="$(awk -F, '
      BEGIN { prev = ""; dup = 0; nonmono = 0; count = 0 }
      NF {
        val = $NF + 0;
        if (prev != "") {
          if (val == prev) dup++;
          if (val < prev) nonmono++;
        }
        prev = val;
        count++;
      }
      END { printf "%d %d %d", count, dup, nonmono }
    ' <<<"$pkt_sample")"
    local pkt_count pkt_dup pkt_nonmono
    read -r pkt_count pkt_dup pkt_nonmono <<<"$pkt_issues"
    if [[ "${pkt_dup:-0}" -gt 0 || "${pkt_nonmono:-0}" -gt 0 ]]; then
      echo "[warn] Sampled packet timestamps show duplicate/non-monotonic DTS (sample=${pkt_count:-0}, duplicate=${pkt_dup:-0}, nonmonotonic=${pkt_nonmono:-0})." >&2
      echo "[warn] This input can trigger ffmpeg image extraction warnings and unstable frame numbering." >&2
    fi
  fi
}

if [[ -f "$IN_ABS" ]] && is_video_ext "$IN_ABS"; then
  check_video_timing "$IN_ABS"
fi

if command -v sha256sum >/dev/null 2>&1; then
  BUILD_HASH="$(
    sha256sum "$DOCKERFILE" "$SAM3_DIR/pyproject.toml" "$SAM3_DIR/requirements.txt" | sha256sum | awk '{print $1}'
  )"
elif command -v shasum >/dev/null 2>&1; then
  BUILD_HASH="$(
    shasum -a 256 "$DOCKERFILE" "$SAM3_DIR/pyproject.toml" "$SAM3_DIR/requirements.txt" | shasum -a 256 | awk '{print $1}'
  )"
else
  echo "ERROR: need sha256sum or shasum to hash Dockerfile inputs" >&2
  exit 1
fi

need_build=0
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  need_build=1
else
  IMG_HASH="$(docker image inspect "$IMAGE_NAME" --format '{{ index .Config.Labels "sam3.build_hash" }}' 2>/dev/null || true)"
  if [[ "$IMG_HASH" != "$BUILD_HASH" ]]; then
    need_build=1
  fi
fi

if [[ "$need_build" -eq 1 ]]; then
  echo "[build] Building $IMAGE_NAME (hash: $BUILD_HASH)" >&2
  docker build \
    -t "$IMAGE_NAME" \
    -f "$DOCKERFILE" \
    --build-arg "BUILD_HASH=$BUILD_HASH" \
    ${BASE_IMAGE:+--build-arg "BASE_IMAGE=$BASE_IMAGE"} \
    "$SAM3_DIR"
else
  echo "[build] Image up to date: $IMAGE_NAME" >&2
fi

if ! command -v nvidia-smi >/dev/null 2>&1; then
  echo "ERROR: nvidia-smi not found. GPU is required for this container." >&2
  exit 1
fi

run_container() {
  local in_container="${1:?}"
  local out_container="${2:?}"
  local max_frames="${3-}"
  local input_base
  input_base="$(basename "$IN_ABS")"
  local input_stem="${input_base%.*}"
  local profile_container="$out_container/${input_stem}_cprofile.prof"
  local python_bin="python"
  local py_args="/workspace/sam3_run.py --input \"$in_container\" --prompt \"$PROMPT\" --output \"$out_container\""
  if [[ -n "$max_frames" ]]; then
    py_args="$py_args --max-frames \"$max_frames\""
  fi
  if [[ "${#EXTRA_ARGS[@]}" -gt 0 ]]; then
    for a in "${EXTRA_ARGS[@]}"; do
      py_args="$py_args \"$a\""
    done
  fi
  if [[ "${#EXTRA_ARGS[@]}" -gt 0 ]]; then
    echo "[debug] Forwarding extra args to python: ${EXTRA_ARGS[*]}" >&2
  fi
  if [[ "$PROFILE_MODE" -eq 1 ]]; then
    python_bin="python -m cProfile -o \"$profile_container\""
    echo "[profile] Writing cProfile output to: $OUT_HOST/${input_stem}_cprofile.prof" >&2
  fi
  local cmd="PYTHONPATH=/workspace/sam3${PYTHONPATH:+:$PYTHONPATH} $python_bin $py_args"

  if [[ "$SHELL_MODE" -eq 1 ]]; then
    echo "Run inside container:"
    echo "$cmd"
    docker run --rm -it \
      --gpus all \
      -v "$IN_DIR:/data" \
      -v "$OUT_HOST:/out" \
      -v "$HF_CACHE_HOST:/cache/hf" \
      -v "$PIP_CACHE_HOST:/root/.cache/pip" \
      -v "$ROOT_DIR:/workspace" \
      "${EXTRA_MOUNTS[@]}" \
      -e HF_TOKEN_FILE=/workspace/hftoken.txt \
      "$IMAGE_NAME" \
      /bin/bash
  else
    docker run --rm -it \
      --gpus all \
      -v "$IN_DIR:/data" \
      -v "$OUT_HOST:/out" \
      -v "$HF_CACHE_HOST:/cache/hf" \
      -v "$PIP_CACHE_HOST:/root/.cache/pip" \
      -v "$ROOT_DIR:/workspace" \
      "${EXTRA_MOUNTS[@]}" \
      -e HF_TOKEN_FILE=/workspace/hftoken.txt \
      "$IMAGE_NAME" \
      /bin/bash -lc "$cmd"
  fi
}

run_container "$IN_CONTAINER" "/out" "$MAX_FRAMES"
