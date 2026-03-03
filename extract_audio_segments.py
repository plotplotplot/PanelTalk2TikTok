
import numpy as np
from pydub import AudioSegment
from tqdm import tqdm

def extract_audio_segments(
    word_segment_times, audio_file, output_audio_file, fade_samples: int = 3000
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

    # Precompute crossfade windows (expanded to match channel count)
    # t has shape (fade_samples, 1). We compute 1-D fades then expand to (fade_samples, ch)
    t = np.linspace(0.0, 1.0, fade_samples, dtype=np.float32)[:, None]
    fade_out_1d = np.cos(t * np.pi / 2.0)
    fade_in_1d = np.sin(t * np.pi / 2.0)
    # Expand fades to match number of channels so operations broadcast correctly
    # `ch` is number of audio channels (e.g., 2 for stereo)
    fade_out = np.repeat(fade_out_1d, ch, axis=1)
    fade_in = np.repeat(fade_in_1d, ch, axis=1)

    segments = []

    for i, row in enumerate(tqdm(word_segment_times, desc="Extracting audio")):
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

        prev = []
        if segments:
            prev = segments[-1]

        if len(prev) > fade_samples:
            overlap_in = frames[start - fade_samples : start] * fade_in
            prev[-fade_samples:] = prev[-fade_samples:] * fade_out + overlap_in

        segments.append(seg.copy())


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
    print(f"✅ Audio segments extracted to: {output_audio_file}")
    return output_audio_file
