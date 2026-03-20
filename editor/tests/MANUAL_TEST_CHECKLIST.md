# Manual Test Checklist

Use this checklist for manual QA testing of the video editor GUI.

## Environment Setup

- [ ] OS: Ubuntu 24.04 (or your target platform)
- [ ] Qt Version: 6.4.2
- [ ] FFmpeg: 6.1.1
- [ ] Display: X11 or Wayland session active
- [ ] Test video files prepared (see below)

## Test Files Setup

Create test files in `/tmp/test_videos/`:
```bash
mkdir -p /tmp/test_videos

# Test 1: 1080p 30fps MP4
cd /tmp/test_videos
ffmpeg -f lavfi -i testsrc=duration=10:size=1920x1080:rate=30 -pix_fmt yuv420p test_1080p30.mp4

# Test 2: 4K 60fps MP4
ffmpeg -f lavfi -i testsrc=duration=5:size=3840x2160:rate=60 -pix_fmt yuv420p test_4k60.mp4

# Test 3: Image sequence
cd /tmp/test_videos
for i in {1..10}; do
    ffmpeg -f lavfi -i testsrc=duration=1:size=1280x720:rate=1 -vframes 1 image_$i.png
done

# Test 4: Long duration video (5 minutes)
ffmpeg -f lavfi -i testsrc=duration=300:size=1280x720:rate=30 -pix_fmt yuv420p test_long.mp4

# Test 5: Different aspect ratios
ffmpeg -f lavfi -i testsrc=duration=10:size=1920x1080:rate=30 -pix_fmt yuv420p test_16x9.mp4
ffmpeg -f lavfi -i testsrc=duration=10:size=1080x1920:rate=30 -pix_fmt yuv420p test_9x16.mp4
ffmpeg -f lavfi -i testsrc=duration=10:size=1080x1080:rate=30 -pix_fmt yuv420p test_1x1.mp4

# Test 6: Different codecs
ffmpeg -f lavfi -i testsrc=duration=10:size=1280x720:rate=30 -c:v libx265 -pix_fmt yuv420p test_hevc.mp4
ffmpeg -f lavfi -i testsrc=duration=10:size=1280x720:rate=30 -c:v libvpx-vp9 test_vp9.webm
```

---

## 1. Application Launch

### 1.1 Startup
- [ ] Application launches without console errors
- [ ] Window appears with title "QRhi Editor Professional"
- [ ] Initial size is 1500x900
- [ ] No crash on startup
- [ ] Background gradient renders correctly

### 1.2 Initial State
- [ ] Preview shows "No active clips at this frame"
- [ ] Timeline is empty
- [ ] Status shows "PAUSED | 0 clips"
- [ ] File explorer shows current directory
- [ ] Backend info shows "OpenGL" or "Vulkan"

---

## 2. File Explorer

### 2.1 File Tree Navigation
- [ ] Click "FILES" button opens folder picker
- [ ] Selecting folder updates file tree
- [ ] Tree shows folders and files
- [ ] Double-clicking folder expands/collapses
- [ ] Tree scrolls when content exceeds height

### 2.2 Drag and Drop from Explorer
- [ ] Drag video file to timeline - clip appears
- [ ] Drag image file to timeline - clip appears (90 frames default)
- [ ] Drag multiple files - multiple clips added
- [ ] Drop position determines start frame

---

## 3. Timeline Widget

### 3.1 Adding Clips
- [ ] Double-click video in explorer adds to timeline
- [ ] Clip appears with correct color
- [ ] Clip label shows filename
- [ ] Clip duration matches video length
- [ ] Multiple clips stack in rows

### 3.2 Clip Manipulation
- [ ] Click clip to select
- [ ] Drag clip to move start position
- [ ] Playhead follows dragged clip
- [ ] Clips snap to reasonable positions
- [ ] Context menu shows "Delete" and "Properties"
- [ ] Delete removes clip from timeline

### 3.3 Timeline Navigation
- [ ] Click timeline to move playhead
- [ ] Scroll wheel zooms timeline
- [ ] Ctrl+Scroll pans timeline
- [ ] Zoom maintains playhead position
- [ ] Time ruler shows minutes:seconds

### 3.4 Playhead
- [ ] Red playhead line visible
- [ ] Playhead moves during playback
- [ ] Playhead position matches preview
- [ ] Playhead has triangular handle

---

## 4. Preview Window

### 4.1 Display
- [ ] Preview renders at 60fps during playback
- [ ] Aspect ratio maintained (letterbox if needed)
- [ ] Background gradient animates during playback
- [ ] Overlay shows "Preview | Active clips X"
- [ ] Frame number updates correctly

### 4.2 Multiple Clips
- [ ] Base layer (bottom clip) fills preview
- [ ] Overlay clips appear as thumbnails (top-right)
- [ ] Up to 3 overlays visible
- [ ] Overlays show clip labels
- [ ] Opacity effect on overlays visible

### 4.3 Performance
- [ ] 1080p video plays at 30fps without drops
- [ ] Scrubbing is responsive (<100ms latency)
- [ ] Memory usage stays under 1GB for 1080p
- [ ] GPU memory usage reasonable

---

## 5. Transport Controls

### 5.1 Playback
- [ ] Play button starts playback
- [ ] Pause button stops playback
- [ ] Status updates to "PLAYING"
- [ ] Playhead advances during playback
- [ ] Loops to start at end of timeline

### 5.2 Navigation
- [ ] Start button jumps to frame 0
- [ ] End button jumps to end
- [ ] Seek slider moves playhead
- [ ] Timecode displays correctly (MM:SS:FF)

### 5.3 Keyboard Shortcuts
- [ ] Spacebar toggles play/pause
- [ ] Left/Right arrows step frames
- [ ] Home key jumps to start
- [ ] End key jumps to end

---

## 6. Video Format Support

### 6.1 Codecs
- [ ] H.264 MP4 plays correctly
- [ ] H.265/HEVC plays (software fallback)
- [ ] VP9 WebM plays
- [ ] ProRes MOV plays (if supported)

### 6.2 Resolutions
- [ ] 720p video plays
- [ ] 1080p video plays
- [ ] 4K video plays (may need hardware accel)
- [ ] Vertical video (9:16) displays correctly
- [ ] Square video (1:1) displays correctly

### 6.3 Frame Rates
- [ ] 24fps video
- [ ] 30fps video
- [ ] 60fps video
- [ ] Variable frame rate handling

---

## 7. Memory and Performance

### 7.1 Memory Management
- [ ] Memory stays bounded during scrubbing
- [ ] Old frames are evicted from cache
- [ ] No memory leaks after 10 min usage
- [ ] Memory usage decreases when timeline cleared

### 7.2 Cache Behavior
- [ ] Recently viewed frames load instantly
- [ ] Forward playback prefetches next frames
- [ ] Backward playback prefetches correctly
- [ ] Cache hit rate > 80% during normal playback

---

## 8. Error Handling

### 8.1 Invalid Files
- [ ] Non-video files rejected gracefully
- [ ] Corrupted video shows "Frame loading..."
- [ ] Missing file doesn't crash app
- [ ] Permission denied handled gracefully

### 8.2 Edge Cases
- [ ] Empty timeline - no crash
- [ ] Single frame video works
- [ ] Very long video (>1 hour) doesn't freeze
- [ ] Unicode filenames work

---

## 9. State Persistence

### 9.1 Save/Restore
- [ ] Timeline state saves on exit
- [ ] Clips restore on relaunch
- [ ] Playhead position restores
- [ ] Explorer root path restores

### 9.2 State File
- [ ] `editor_state.json` created in app directory
- [ ] JSON is valid and readable
- [ ] State survives app crash

---

## 10. Stress Testing

### 10.1 Large Timeline
- [ ] 50+ clips on timeline
- [ ] Scrolling remains smooth
- [ ] Preview updates correctly
- [ ] No UI freezing

### 10.2 Rapid Operations
- [ ] Rapid scrubbing doesn't crash
- [ ] Rapid play/pause toggles work
- [ ] Rapid clip additions work
- [ ] Queue doesn't overflow

### 10.3 Long Duration Test
- [ ] App runs for 30 minutes without crash
- [ ] Memory usage stable over time
- [ ] No zombie threads

---

## Regression Tests

After any code change, verify:
1. [ ] Application still launches
2. [ ] Existing timeline loads
3. [ ] Basic playback works
4. [ ] No new warnings in console

---

## Test Sign-off

| Tester | Date | Version | Result |
|--------|------|---------|--------|
|        |      |         | ☐ PASS / ☐ FAIL |

Notes:
