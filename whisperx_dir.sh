#!/usr/bin/env bash
# whisperx_dir.sh
#
# Usage:
#   ./whisperx_dir.sh "/path/to/dir"
#
# Runs whisperx_cache.sh on every *.mov / *.MOV file in the given directory.
# Skips files if output already exists next to the input file.

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <directory>" >&2
  exit 2
fi

DIR="$1"

if [[ ! -d "$DIR" ]]; then
  echo "ERROR: Not a directory: $DIR" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WHISPERX_SCRIPT="$SCRIPT_DIR/whisperx_cache.sh"

if [[ ! -x "$WHISPERX_SCRIPT" ]]; then
  echo "ERROR: whisperx_cache.sh not found or not executable at: $WHISPERX_SCRIPT" >&2
  echo "Fix: chmod +x \"$WHISPERX_SCRIPT\"" >&2
  exit 1
fi

# Find .mov files (case-insensitive) in the *top-level* of DIR (not recursive).
mapfile -d '' files < <(find "$DIR" -maxdepth 1 -type f \( -iname '*.mov' \) -print0 | sort -z)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "No .mov files found in: $DIR" >&2
  exit 0
fi

echo "Found ${#files[@]} .mov file(s) in: $DIR"
echo

output_exists() {
  local in="$1"
  local dir base stem

  dir="$(dirname "$in")"
  base="$(basename "$in")"
  stem="${base%.*}"  # strip extension

  # Common WhisperX output conventions (adjust as needed):
  # 1) <stem>.json
  # 2) <base>.json  (e.g., video.mov.json)
  if [[ -f "$dir/$stem.json" || -f "$dir/$base.json" ]]; then
    return 0
  fi

  # If your script creates different "done" artifacts, add checks here, e.g.:
  # [[ -f "$dir/$stem.srt" ]] && return 0
  # [[ -d "$dir/$stem" ]] && return 0

  return 1
}

for f in "${files[@]}"; do
  echo "=== whisperx: $(basename "$f") ==="

  if output_exists "$f"; then
    echo "SKIP: output already exists for $(basename "$f")"
    echo
    continue
  fi

  "$WHISPERX_SCRIPT" "$f"
  echo
done
