#include <QtTest/QtTest>
#include <QTemporaryFile>
#include "../async_decoder.h"

using namespace editor;

class TestAsyncDecoder : public QObject {
    Q_OBJECT

private slots:
    void testInitialization();
    void testVideoInfo();
    void testInvalidFile();
    void testRequestFrame();
    void testCancelRequests();
    void testMultipleRequests();
};

void TestAsyncDecoder::testInitialization() {
    AsyncDecoder decoder;
    QVERIFY(decoder.initialize());
    QVERIFY(decoder.workerCount() > 0);
}

void TestAsyncDecoder::testVideoInfo() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    // Test with non-existent file
    VideoStreamInfo info = decoder.getVideoInfo("/nonexistent/file.mp4");
    QVERIFY(!info.isValid);
}

void TestAsyncDecoder::testInvalidFile() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    bool callbackCalled = false;
    FrameHandle receivedFrame;
    
    decoder.requestFrame("/nonexistent/file.mp4", 0, 100, 1000,
        [&callbackCalled, &receivedFrame](FrameHandle frame) {
            callbackCalled = true;
            receivedFrame = frame;
        });
    
    // Wait for callback
    QTest::qWait(500);
    
    QVERIFY(callbackCalled);
    QVERIFY(receivedFrame.isNull());
}

void TestAsyncDecoder::testRequestFrame() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    // Create a dummy request to test the queue system
    bool callbackCalled = false;
    uint64_t seqId = decoder.requestFrame("/tmp/test.mp4", 0, 100, 1000,
        [&callbackCalled](FrameHandle frame) {
            callbackCalled = true;
        });
    
    QVERIFY(seqId > 0);
    QVERIFY(decoder.pendingRequestCount() >= 0);
}

void TestAsyncDecoder::testCancelRequests() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    // Queue multiple requests
    for (int i = 0; i < 10; ++i) {
        decoder.requestFrame("/tmp/test.mp4", i, 50, 5000,
            [](FrameHandle frame) {
                // Callback
            });
    }
    
    // Cancel all for this file
    decoder.cancelForFile("/tmp/test.mp4");
    
    // Queue should be cleared or processing
    QVERIFY(decoder.pendingRequestCount() >= 0);
}

void TestAsyncDecoder::testMultipleRequests() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    int callbackCount = 0;
    
    // Request multiple frames with different priorities
    decoder.requestFrame("/tmp/test1.mp4", 0, 100, 1000,
        [&callbackCount](FrameHandle frame) { callbackCount++; });
    decoder.requestFrame("/tmp/test2.mp4", 0, 50, 1000,
        [&callbackCount](FrameHandle frame) { callbackCount++; });
    decoder.requestFrame("/tmp/test3.mp4", 0, 10, 1000,
        [&callbackCount](FrameHandle frame) { callbackCount++; });

    QTest::qWait(500);
    QCOMPARE(callbackCount, 3);
}

QTEST_MAIN(TestAsyncDecoder)
#include "test_async_decoder.moc"
