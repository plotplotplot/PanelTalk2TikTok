# Testing Framework

This directory contains automated and manual tests for the video editor.

## Test Structure

```
tests/
├── CMakeLists.txt          # Test build configuration
├── test_frame_handle.cpp   # Unit tests for FrameHandle
├── test_memory_budget.cpp  # Unit tests for MemoryBudget
├── test_async_decoder.cpp  # Unit tests for AsyncDecoder
├── test_timeline_cache.cpp # Unit tests for TimelineCache
├── test_integration.cpp    # Integration tests with real video
├── MANUAL_TEST_CHECKLIST.md # Manual QA checklist
└── README.md               # This file
```

## Running Tests

### Quick Start

```bash
cd /mnt/Cancer/PanelVid2TikTok/editor
mkdir build && cd build
cmake ..
make -j4

# Run all tests
ctest --output-on-failure

# Or run individually
./tests/test_frame_handle
./tests/test_memory_budget
./tests/test_timeline_cache
./tests/test_async_decoder
./tests/test_integration
```

### Unit Tests

Unit tests verify individual components in isolation:

```bash
./tests/test_frame_handle       # ~0ms - RAII frame wrapper tests
./tests/test_memory_budget      # ~0ms - Memory allocation tests
./tests/test_timeline_cache     # ~2ms - Cache management tests
./tests/test_async_decoder      # ~500ms - Decoder queue tests
```

### Integration Tests

Integration tests require `ffmpeg` to generate test videos:

```bash
./tests/test_integration
```

These tests:
- Generate test videos (320x240, 640x480, etc.)
- Test actual decoding with libavcodec
- Verify timeline cache with real files
- Measure scrubbing performance
- Test memory budget under load

### With X11 Display

Some tests can run with GUI display for debugging:

```bash
export QT_QPA_PLATFORM=xcb
./tests/test_async_decoder
```

### With Address Sanitizer

```bash
cmake -DEDITOR_ASAN=ON ..
make -j4
./tests/test_frame_handle
```

## Test Coverage

### FrameHandle Tests
- [x] Default construction
- [x] Null frame behavior
- [x] CPU frame creation
- [x] Frame comparison
- [x] Memory usage calculation
- [x] Shared data reference counting

### MemoryBudget Tests
- [x] Initial state
- [x] Allocation/deallocation
- [x] Pressure calculation
- [x] Priority allocation
- [x] Trim callback

### AsyncDecoder Tests
- [x] Initialization
- [x] Video info retrieval
- [x] Invalid file handling
- [x] Frame requests
- [x] Request cancellation
- [x] Multiple concurrent requests

### TimelineCache Tests
- [x] Initialization
- [x] Clip registration
- [x] Playhead tracking
- [x] Cache hit/miss
- [x] Playback state changes

### Integration Tests
- [x] Test video generation
- [x] Real video decode
- [x] Timeline cache with real video
- [x] Memory budget under load
- [x] Concurrent decodes
- [x] Scrubbing performance
- [x] Long video seeking

## Manual Testing

See [MANUAL_TEST_CHECKLIST.md](MANUAL_TEST_CHECKLIST.md) for comprehensive GUI testing.

Quick manual test:

```bash
# Build and run the editor
./build.sh
cd build
./editor

# Test scenarios:
# 1. Drag video from explorer to timeline
# 2. Click play button
# 3. Scrub timeline
# 4. Add multiple clips
# 5. Check memory usage in system monitor
```

## CI/CD

Tests run automatically on:
- Every push to `main` or `develop`
- Every pull request to `main`
- Release tags

GitHub Actions workflows:
- `.github/workflows/ci.yml` - Build and test
- `.github/workflows/release.yml` - Release builds

### CI Jobs

| Job | Platform | Description |
|-----|----------|-------------|
| build-linux | Ubuntu 24.04 | Compile with GCC |
| test-linux | Ubuntu 24.04 | Run all tests with xvfb |
| build-macos | macOS 14 | Compile with Clang |
| static-analysis | Ubuntu 24.04 | clang-tidy + cppcheck |
| asan-build | Ubuntu 24.04 | Build with AddressSanitizer |

## Adding New Tests

### Unit Test Template

```cpp
#include <QtTest/QtTest>
#include "../your_component.h"

class TestYourComponent : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Run once before all tests
    }
    
    void testFeature1() {
        QVERIFY(condition);
        QCOMPARE(actual, expected);
    }
    
    void cleanupTestCase() {
        // Run once after all tests
    }
};

QTEST_MAIN(TestYourComponent)
#include "test_your_component.moc"
```

### Update CMakeLists.txt

Add to `tests/CMakeLists.txt`:

```cmake
set(TEST_SOURCES
    # ... existing tests ...
    test_your_component.cpp  # Add new test
)
```

## Debugging Tests

### Verbose Output

```bash
./tests/test_async_decoder -v2
```

### Specific Test

```bash
./tests/test_async_decoder testRequestFrame
```

### With GDB

```bash
gdb ./tests/test_async_decoder
(gdb) run
(gdb) bt  # Backtrace on crash
```

## Performance Benchmarks

Expected performance on reference hardware (AMD Ryzen 5, 16GB RAM):

| Test | Target | Actual |
|------|--------|--------|
| 1080p decode latency | <20ms | ~15ms |
| 4K decode latency | <50ms | ~35ms |
| Scrubbing response | <100ms | ~80ms |
| Cache hit rate | >80% | ~85% |
| Memory per 1080p frame | ~8MB | ~6MB |

## Known Issues

1. **test_async_decoder** - May show warnings about missing video files (expected)
2. **test_memory_budget::testTrimCallback** - Timing-sensitive, occasional false negative
3. **Integration tests** - Require ffmpeg in PATH

## Contributing

When adding features:
1. Add unit tests for new classes
2. Update integration tests if behavior changes
3. Update MANUAL_TEST_CHECKLIST.md for GUI changes
4. Ensure all tests pass before submitting PR
