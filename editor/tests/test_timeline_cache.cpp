#include <QtTest/QtTest>
#include "../timeline_cache.h"
#include "../async_decoder.h"
#include "../memory_budget.h"

using namespace editor;

class TestTimelineCache : public QObject {
    Q_OBJECT

private slots:
    void testInitialization();
    void testClipRegistration();
    void testPlayheadTracking();
    void testCacheHitMiss();
    void testPlaybackState();
};

void TestTimelineCache::testInitialization() {
    AsyncDecoder decoder;
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    
    QCOMPARE(cache.totalCachedFrames(), 0);
    QCOMPARE(cache.totalMemoryUsage(), 0);
    QCOMPARE(cache.cacheHitRate(), 0.0);
}

void TestTimelineCache::testClipRegistration() {
    AsyncDecoder decoder;
    decoder.initialize();
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    
    cache.registerClip("clip1", "/tmp/test1.mp4", 0, 100);
    cache.registerClip("clip2", "/tmp/test2.mp4", 100, 200);
    
    // Check that clips are registered
    QVERIFY(!cache.isFrameCached("clip1", 0));
    QVERIFY(!cache.isFrameCached("clip2", 100));
}

void TestTimelineCache::testPlayheadTracking() {
    AsyncDecoder decoder;
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    
    QCOMPARE(cache.playheadFrame(), 0);
    
    cache.setPlayheadFrame(100);
    QCOMPARE(cache.playheadFrame(), 100);
    
    cache.setPlayheadFrame(500);
    QCOMPARE(cache.playheadFrame(), 500);
}

void TestTimelineCache::testCacheHitMiss() {
    AsyncDecoder decoder;
    decoder.initialize();
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    
    cache.registerClip("clip1", "/tmp/test.mp4", 0, 100);
    
    // Initially nothing should be cached
    QVERIFY(cache.getCachedFrame("clip1", 0).isNull());
    QVERIFY(!cache.isFrameCached("clip1", 0));
    
    // Request a frame (async, won't complete immediately in test)
    bool callbackCalled = false;
    cache.requestFrame("clip1", 0, [&callbackCalled](FrameHandle frame) {
        callbackCalled = true;
    });
    
    QVERIFY(!cache.isFrameCached("clip1", 0)); // Still not cached
}

void TestTimelineCache::testPlaybackState() {
    AsyncDecoder decoder;
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    
    cache.setPlaybackState(TimelineCache::PlaybackState::Stopped);
    cache.setPlaybackState(TimelineCache::PlaybackState::Playing);
    cache.setPlaybackState(TimelineCache::PlaybackState::Scrubbing);
    cache.setPlaybackState(TimelineCache::PlaybackState::Exporting);
    
    cache.setDirection(TimelineCache::Direction::Forward);
    cache.setDirection(TimelineCache::Direction::Backward);
    
    cache.setPlaybackSpeed(1.0);
    cache.setPlaybackSpeed(2.0);
    cache.setPlaybackSpeed(0.5);
}

QTEST_MAIN(TestTimelineCache)
#include "test_timeline_cache.moc"
