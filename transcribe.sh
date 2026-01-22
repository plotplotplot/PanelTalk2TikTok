docker run --rm --gpus all \
  --ipc=host --ulimit memlock=-1 --ulimit stack=67108864 \
  -v "$PWD":/work -w /work \
  -v "$HOME/.cache/huggingface":/root/.cache/huggingface \
  -v "$HOME/.cache/ctranslate2":/root/.cache/ctranslate2 \
  -v "$HOME/.pytorch24":/root/.pytorch24 \
  nvcr.io/nvidia/pytorch:24.12-py3 \
  bash -c '
    set -euo pipefail

    export TARGET=/root/.pytorch24
    # SAFE: don’t assume PYTHONPATH is set
    export PYTHONPATH="${TARGET}${PYTHONPATH:+:$PYTHONPATH}"
    pip list

    # Installs (add --upgrade if you want to suppress "already exists" warnings)
    (python -m pip show whisperx >/dev/null 2>&1 || \
      pip install --no-deps --target="$TARGET" git+https://github.com/m-bain/whisperx.git)
      pip install --no-deps --target="$TARGET" \
        ctranslate2 faster_whisper av tokenizers==0.20.3 transformers==4.45.2 \
        pyannote.audio==3.1.1 pyannote.core==6.0.1 pyannote.database==6.1.0 pyannote.metrics==4.0.0 \
        pytorch_lightning torchmetrics torchaudio \
        huggingface_hub==0.23.2

    # Run transcribe.py, honoring .pth in TARGET for namespace packages (pyannote.*)
    python - <<PY "$@"
import site, runpy, sys
site.addsitedir("/root/.pytorch24")
sys.argv = ["transcribe.py"] + sys.argv[1:]
runpy.run_path("transcribe.py", run_name="__main__")
PY
  ' _ "$@"
