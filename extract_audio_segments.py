
import numpy as np
from pydub import AudioSegment
from tqdm import tqdm

def extract_audio_segments(
    word_segment_times,
    audio_file,
    output_audio_file,
    fade_samples: int = 3000,
    fade_mode: str = "sequence",
    preserve_length: bool = False,
    debug_lengths: bool = False,
):
    """
    Faster version of extract_audio_segments().
    Avoids repeated array reallocations and redundant conversions.
    """

    # Load and extract metadata
    audio = AudioSegment.from_file(audio_file)
    sr = audio.frame_rate
    ch = audio.channels
    sw = audio.sample_width  # bytes per sample (1,2,3,4)

    dtype = {1: np.int8, 2: np.int16, 3: np.int32, 4: np.int32}[min(sw, 4)]
    full_scale = float((1 << (8 * min(sw, 3) - 1)) - 1)

    # Zero-copy decode
    raw = np.frombuffer(audio.raw_data, dtype=dtype)
    frames = raw.reshape((-1, ch)).astype(np.float32) / full_scale

    fade_samples = max(0, int(fade_samples or 0))
    fade_mode = (fade_mode or "sequence").lower()
    if fade_mode not in ("sequence", "hold", "split"):
        fade_mode = "sequence"
    if fade_mode == "split" and fade_samples > 1:
        # Keep split fades symmetric so head/tail trim match.
        fade_samples = (fade_samples // 2) * 2

    # Precompute crossfade windows (expanded to match channel count)
    # t has shape (fade_len, 1). We compute 1-D fades then expand to (fade_len, ch)
    fade_len = fade_samples
    if fade_mode == "split":
        fade_len = fade_samples // 2
    t = np.linspace(0.0, 1.0, max(1, fade_len), dtype=np.float32)[:, None]
    fade_out_1d = np.cos(t * np.pi / 2.0)
    fade_in_1d = np.sin(t * np.pi / 2.0)
    # Expand fades to match number of channels so operations broadcast correctly
    # `ch` is number of audio channels (e.g., 2 for stereo)
    fade_out = np.repeat(fade_out_1d, ch, axis=1)
    fade_in = np.repeat(fade_in_1d, ch, axis=1)

    segments = []
    expected_total = 0
    actual_total = 0

    for i, row in enumerate(tqdm(word_segment_times, desc="Extracting audio")):
        if "start_sample" in row and "end_sample" in row:
            start = int(row["start_sample"])
            end = int(row["end_sample"])
        else:
            start = int(row["start"] * sr)
            end = int(row["end"] * sr)
        if start < 0:
            start = 0
        if end < 0:
            end = 0
        if end > len(frames):
            end = len(frames)
        if start >= end:
            # Skip invalid or empty segments after clamping.
            continue
        seg = frames[start:end]
        expected_len = max(0, end - start)
        expected_total += expected_len

        prev = []
        if segments:
            prev = segments[-1]

        if fade_len > 0 and len(prev) > fade_len and start >= fade_len:
            overlap_in = frames[start - fade_len : start] * fade_in
            prev[-fade_len:] = prev[-fade_len:] * fade_out + overlap_in
            # Trim the head of the new segment to match the overlap length.
            if not preserve_length:
                trim_len = fade_len
                if fade_mode == "split":
                    trim_len = fade_samples // 2
                if len(seg) > trim_len:
                    seg = seg[trim_len:]

        segments.append(seg.copy())
        actual_total += len(seg)
        if debug_lengths:
            actual_len = len(seg)
            print(f"[len][audio] seg={i+1} expected={expected_len} actual={actual_len} diff={actual_len - expected_len} samples")


    # Combine all at once
    out = np.concatenate(segments, axis=0)

    # Convert to int16
    out_i16 = np.clip(out * 32767.0, -32768, 32767).astype(np.int16)

    pcm = out_i16.reshape(-1)  # interleaved
    out_seg = AudioSegment(
        data=pcm.tobytes(),
        frame_rate=sr,
        sample_width=2,  # 16-bit
        channels=ch,
    )
    out_seg.export(output_audio_file, format="wav")
    if debug_lengths:
        print(
            f"[len][audio-total] expected={expected_total} actual={actual_total} "
            f"diff={actual_total - expected_total} samples"
        )
    print(f"✅ Audio segments extracted to: {output_audio_file}")
    return output_audio_file
