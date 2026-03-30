# RtAudio Backend Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ALSA-only AudioEngine with an RtAudio-based implementation that works on macOS (CoreAudio), Windows (WASAPI), and Linux (ALSA) — giving audio playback on all platforms.

**Architecture:** The current AudioEngine has 3 threads: decode (FFmpeg → float PCM cache), mix (compose timeline clips → int16 queue), and output (dequeue → `snd_pcm_writei`). RtAudio replaces the output thread with a pull callback. The decode and mix threads stay unchanged. The `snd_pcm_delay()` clock is replaced by `RtAudio::getStreamLatency()`. The public API (`start/stop/seek/currentSample/setTimelineClips/...`) stays identical — editor.cpp changes nothing.

**Tech Stack:** RtAudio 6.x (submodule at `editor/rtaudio`), CMake `add_subdirectory`, CoreAudio (macOS), WASAPI (Windows), ALSA (Linux)

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `editor/audio_engine.h` | **Rewrite** | Replace ALSA implementation with RtAudio. Keep all public API signatures identical. Remove `#ifdef EDITOR_HAS_ALSA` / stub split — RtAudio works on all platforms. |
| `editor/CMakeLists.txt` | **Modify** | Add `add_subdirectory(rtaudio)`, link `rtaudio`, remove ALSA pkg-config block |
| `editor/editor.cpp` | **No changes** | All call sites already use the public API |
| `editor/editor.h` | **No changes** | `std::unique_ptr<AudioEngine>` stays |

## Chunk 1: CMake integration and AudioEngine rewrite

### Task 1: Update CMakeLists.txt for RtAudio

**Files:**
- Modify: `editor/CMakeLists.txt`

- [ ] **Step 1: Add RtAudio subdirectory and remove ALSA**

Replace the ALSA conditional block and add RtAudio:

```cmake
# Remove this block:
# --- Conditional ALSA (Linux only) ---
# if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
#     pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
#     add_compile_definitions(EDITOR_HAS_ALSA=1)
# endif()

# Add instead:
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(rtaudio)
```

Link `rtaudio` to both `editor_lib` and `editor`:

```cmake
# In editor_lib target_link_libraries:
target_link_libraries(editor_lib PUBLIC ${EDITOR_COMMON_LIBS} rtaudio)

# In editor target_link_libraries — remove PkgConfig::ALSA block
# rtaudio is already linked transitively via editor_lib
```

Remove the Linux-only ALSA link blocks:
```cmake
# Remove:
# if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
#     target_link_libraries(editor_lib PUBLIC PkgConfig::ALSA)
# endif()
# if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
#     target_link_libraries(editor PRIVATE PkgConfig::ALSA)
# endif()
```

- [ ] **Step 2: Verify cmake configure succeeds**

Run: `cd editor && cmake -B build -DCMAKE_BUILD_TYPE=Release`
Expected: Configures successfully, finds CoreAudio on macOS

- [ ] **Step 3: Commit**

```bash
git add editor/CMakeLists.txt
git commit -m "build: replace ALSA with RtAudio subdirectory in CMake"
```

### Task 2: Rewrite AudioEngine with RtAudio backend

**Files:**
- Rewrite: `editor/audio_engine.h`

The rewrite replaces all ALSA-specific code while keeping the public API identical. Key architectural changes:

1. **Remove** `#ifdef EDITOR_HAS_ALSA` / `#else` stub split — single implementation for all platforms
2. **Remove** `m_outputWorker` thread and `m_pcmQueue` deque — replaced by RtAudio pull callback
3. **Replace** `snd_pcm_t*` with `std::unique_ptr<rt::audio::RtAudio>`
4. **Replace** `snd_pcm_delay()` with `rtaudio->getStreamLatency()`
5. **Keep** `m_decodeWorker` and `m_mixWorker` threads unchanged
6. **Keep** all mix logic (`mixChunk`, `decodeClipAudio`, `scheduleDecodesLocked`) unchanged
7. **Add** a ring buffer between mix thread and RtAudio callback (replaces `m_pcmQueue` deque)

The RtAudio callback signature (v6):
```cpp
int audioCallback(void* outputBuffer, void* /*inputBuffer*/,
                  unsigned int nFrames, double /*streamTime*/,
                  rt::audio::RtAudioStreamStatus status, void* userData)
```

The callback reads from a lock-free ring buffer that the mix thread writes to. This avoids mutex contention in the audio callback (which runs on a realtime OS thread).

- [ ] **Step 1: Write the new audio_engine.h**

The full implementation. Key sections:

**Ring buffer** (simple power-of-2 SPSC ring buffer for int16 samples):
```cpp
struct AudioRingBuffer {
    static constexpr size_t kCapacity = 32768; // power of 2
    std::array<int16_t, kCapacity> buffer{};
    std::atomic<size_t> readPos{0};
    std::atomic<size_t> writePos{0};

    size_t available() const {
        return writePos.load(std::memory_order_acquire) - readPos.load(std::memory_order_relaxed);
    }
    size_t space() const { return kCapacity - available(); }

    size_t write(const int16_t* data, size_t count) {
        const size_t avail = space();
        count = std::min(count, avail);
        const size_t wp = writePos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i)
            buffer[(wp + i) & (kCapacity - 1)] = data[i];
        writePos.store(wp + count, std::memory_order_release);
        return count;
    }

    size_t read(int16_t* data, size_t count) {
        const size_t avail = available();
        count = std::min(count, avail);
        const size_t rp = readPos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i)
            data[i] = buffer[(rp + i) & (kCapacity - 1)];
        readPos.store(rp + count, std::memory_order_release);
        return count;
    }

    void clear() {
        readPos.store(0, std::memory_order_relaxed);
        writePos.store(0, std::memory_order_relaxed);
    }
};
```

**RtAudio callback** (static, called from OS audio thread):
```cpp
static int rtAudioCallback(void* outputBuffer, void* /*inputBuffer*/,
                           unsigned int nFrames, double /*streamTime*/,
                           rt::audio::RtAudioStreamStatus /*status*/, void* userData) {
    auto* engine = static_cast<AudioEngine*>(userData);
    auto* out = static_cast<int16_t*>(outputBuffer);
    const size_t samplesNeeded = nFrames * engine->m_channelCount;
    const size_t read = engine->m_ringBuffer.read(out, samplesNeeded);
    // Record clock position for samples just submitted
    if (read > 0) {
        engine->m_audioClockSample.store(
            engine->m_ringBufferEndSample.load(std::memory_order_acquire),
            std::memory_order_release);
    }
    // Fill remainder with silence if ring buffer underran
    if (read < samplesNeeded) {
        std::memset(out + read, 0, (samplesNeeded - read) * sizeof(int16_t));
        engine->m_underrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}
```

**Member variables** — replace ALSA-specific with:
```cpp
std::unique_ptr<rt::audio::RtAudio> m_rtaudio;
AudioRingBuffer m_ringBuffer;
std::atomic<int64_t> m_ringBufferEndSample{0}; // timeline sample at end of last written chunk
```

**Remove**: `snd_pcm_t* m_pcm`, `m_pcmMutex`, `m_pcmQueue`, `m_pcmChunkEndSamples`, `m_outputWorker`, `m_queueMutex`, `m_queueCondition`

**`initialize()`**: Create `RtAudio`, open output stream:
```cpp
bool initialize() {
    if (m_initialized) return true;
    try {
        m_rtaudio = std::make_unique<rt::audio::RtAudio>();
    } catch (...) {
        return false;
    }
    if (m_rtaudio->getDeviceCount() == 0) return false;

    rt::audio::RtAudio::StreamParameters params;
    params.deviceId = m_rtaudio->getDefaultOutputDevice();
    params.nChannels = m_channelCount;
    unsigned int bufferFrames = m_periodFrames;
    auto err = m_rtaudio->openStream(&params, nullptr,
        rt::audio::RTAUDIO_SINT16, m_sampleRate, &bufferFrames,
        &rtAudioCallback, this);
    if (err != rt::audio::RTAUDIO_NO_ERROR) return false;
    // Start decode and mix threads (same as before)
    m_running = true;
    m_decodeWorker = std::thread([this]() { decodeLoop(); });
    m_mixWorker = std::thread([this]() { mixLoop(); });
    m_initialized = true;
    return true;
}
```

**`start()`**: Clear ring buffer, set cursor, start stream:
```cpp
void start(int64_t startFrame) {
    if (!initialize()) return;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_timelineSampleCursor = timelineFrameToSamples(startFrame);
    m_audioClockSample.store(m_timelineSampleCursor, std::memory_order_release);
    m_ringBufferEndSample.store(m_timelineSampleCursor, std::memory_order_release);
    m_ringBuffer.clear();
    m_playing = true;
    m_stateCondition.notify_all();
    m_rtaudio->startStream();
}
```

**`stop()`**: Stop stream, clear buffer:
```cpp
void stop() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_playing = false;
        m_stateCondition.notify_all();
    }
    if (m_rtaudio && m_rtaudio->isStreamRunning())
        m_rtaudio->stopStream();
    m_ringBuffer.clear();
}
```

**`seek()`**: Same as stop + reposition + restart if was playing:
```cpp
void seek(int64_t frame) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    const int64_t sample = timelineFrameToSamples(frame);
    m_timelineSampleCursor = sample;
    m_audioClockSample.store(sample, std::memory_order_release);
    m_ringBufferEndSample.store(sample, std::memory_order_release);
    m_ringBuffer.clear();
    m_stateCondition.notify_all();
}
```

**`currentSample()`**: Use `getStreamLatency()`:
```cpp
int64_t currentSample() const {
    const int64_t submitted = m_audioClockSample.load(std::memory_order_acquire);
    long latencyFrames = 0;
    if (m_rtaudio && m_rtaudio->isStreamOpen())
        latencyFrames = m_rtaudio->getStreamLatency();
    return qMax<int64_t>(0, submitted - latencyFrames * m_channelCount);
}
```
Wait — `getStreamLatency()` returns frames, not samples. And `m_audioClockSample` is in timeline samples (mono). So:
```cpp
return qMax<int64_t>(0, submitted - latencyFrames * kSamplesPerFrame / m_periodFrames);
```
Actually, the units need careful thought. `m_audioClockSample` in the original code is a **timeline sample position** (at 48kHz). `getStreamLatency()` returns audio frames (at 48kHz). Since stereo, 1 audio frame = 2 samples in the buffer but = 1 sample position in timeline terms. So:
```cpp
return qMax<int64_t>(0, submitted - latencyFrames);
```

**`mixLoop()`** — change the output target from `m_pcmQueue` to `m_ringBuffer`:
The loop stays almost identical. Instead of pushing int16 chunks to `m_pcmQueue` and recording end samples in `m_pcmChunkEndSamples`, it writes to `m_ringBuffer` and updates `m_ringBufferEndSample`. The low-water check changes from `m_pcmQueue.size()` to `m_ringBuffer.available()`.

**`shutdown()`**: Stop stream, close stream, join threads:
```cpp
void shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_running = false;
        m_playing = false;
        m_stateCondition.notify_all();
        m_decodeCondition.notify_all();
    }
    if (m_decodeWorker.joinable()) m_decodeWorker.join();
    if (m_mixWorker.joinable()) m_mixWorker.join();
    if (m_rtaudio) {
        if (m_rtaudio->isStreamRunning()) m_rtaudio->stopStream();
        if (m_rtaudio->isStreamOpen()) m_rtaudio->closeStream();
        m_rtaudio.reset();
    }
    m_initialized = false;
}
```

- [ ] **Step 2: Build**

Run: `cd editor && ./build_mac.sh`
Expected: Compiles and links. RtAudio auto-detects CoreAudio on macOS.

- [ ] **Step 3: Smoke test**

Run the editor, load a project with audio clips, press Play.
Expected: Audio plays through CoreAudio. Timeline stays in sync.

- [ ] **Step 4: Commit**

```bash
git add editor/audio_engine.h
git commit -m "feat: replace ALSA with RtAudio for cross-platform audio playback"
```

### Task 3: Run tests

- [ ] **Step 1: Run unit tests**

```bash
cd editor/build/tests
QT_QPA_PLATFORM=offscreen ./test_frame_handle
QT_QPA_PLATFORM=offscreen ./test_memory_budget
QT_QPA_PLATFORM=offscreen ./test_timeline_cache
QT_QPA_PLATFORM=offscreen ./test_async_decoder
QT_QPA_PLATFORM=offscreen ./test_integration
```
Expected: All 39 tests pass. AudioEngine is not directly tested by the unit tests (they test timeline cache, frame handle, etc.), but linking must succeed.

- [ ] **Step 2: Commit all changes**

```bash
git add -A
git commit -m "feat: RtAudio cross-platform audio backend

Replace ALSA-only AudioEngine with RtAudio 6.x (submodule).
Supports CoreAudio (macOS), WASAPI (Windows), ALSA (Linux).
Public API unchanged — no editor.cpp modifications needed.

Key changes:
- Output thread replaced by RtAudio pull callback
- snd_pcm_delay() clock replaced by getStreamLatency()
- Lock-free SPSC ring buffer between mix thread and callback
- EDITOR_HAS_ALSA removed — single implementation for all platforms"
```
