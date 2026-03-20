#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QDir>

#include "../async_decoder.h"
#include "../timeline_cache.h"
#include "../memory_budget.h"

using namespace editor;

class TestIntegration : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;
    QString m_testVideoPath;
    
    bool generateTestVideo(const QString& path, int width, int height, 
                           int durationSec, int fps);

private slots:
    void initTestCase();
    void testVideoGeneration();
    void testDecodeRealVideo();
    void testMemoryBudgetUnderLoad();
    void testConcurrentDecodes();
    void testScrubbingPerformance();
    void testLongVideoSeeking();
    void cleanupTestCase();
};

bool TestIntegration::generateTestVideo(const QString& path, int width, int height,
                                        int durationSec, int fps) {
    QStringList args;
    args << "-f" << "lavfi"
         << "-i" << QString("testsrc=duration=%1:size=%2x%3:rate=%4")
                   .arg(durationSec).arg(width).arg(height).arg(fps)
         << "-pix_fmt" << "yuv420p"
         << "-y"  // Overwrite output
         << path;
    
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", args);
    ffmpeg.waitForFinished(30000);
    
    return ffmpeg.exitCode() == 0 && QFile::exists(path);
}

void TestIntegration::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    m_testVideoPath = m_tempDir.filePath("test_video.mp4");
    
    // Check if ffmpeg is available
    QProcess checkFfmpeg;
    checkFfmpeg.start("ffmpeg", QStringList() << "-version");
    checkFfmpeg.waitForFinished(2000);
    
    if (checkFfmpeg.exitCode() != 0) {
        QSKIP("ffmpeg not available - skipping integration tests");
    }
}

void TestIntegration::testVideoGeneration() {
    QVERIFY(generateTestVideo(m_testVideoPath, 320, 240, 2, 30));
    QVERIFY(QFile::exists(m_testVideoPath));
    QVERIFY(QFileInfo(m_testVideoPath).size() > 0);
}

void TestIntegration::testDecodeRealVideo() {
    QVERIFY(generateTestVideo(m_testVideoPath, 320, 240, 2, 30));
    
    AsyncDecoder decoder;
    QVERIFY(decoder.initialize());
    
    // Get video info
    VideoStreamInfo info = decoder.getVideoInfo(m_testVideoPath);
    QVERIFY(info.isValid);
    QCOMPARE(info.frameSize, QSize(320, 240));
    QVERIFY(info.fps > 0);
    QVERIFY(info.durationFrames > 0);
    
    // Request a frame
    bool callbackCalled = false;
    FrameHandle receivedFrame;
    
    uint64_t seqId = decoder.requestFrame(m_testVideoPath, 0, 100, 5000,
        [&callbackCalled, &receivedFrame](FrameHandle frame) {
            callbackCalled = true;
            receivedFrame = frame;
        });
    
    QVERIFY(seqId > 0);
    
    // Wait for decode (up to 5 seconds)
    QElapsedTimer timer;
    timer.start();
    while (!callbackCalled && timer.elapsed() < 5000) {
        QTest::qWait(100);
    }
    
    QVERIFY(callbackCalled);
    QVERIFY(!receivedFrame.isNull());
    QVERIFY(receivedFrame.hasCpuImage());
    QCOMPARE(receivedFrame.frameNumber(), 0);
    QCOMPARE(receivedFrame.sourcePath(), m_testVideoPath);
}

void TestIntegration::testMemoryBudgetUnderLoad() {
    MemoryBudget budget;
    budget.setMaxCpuMemory(10 * 1024 * 1024);  // 10MB
    budget.setMaxGpuMemory(10 * 1024 * 1024);  // 10MB
    
    bool trimCalled = false;
    budget.setTrimCallback([&trimCalled]() {
        trimCalled = true;
    });
    
    // Allocate until pressure
    size_t allocated = 0;
    const size_t chunk = 1024 * 1024;  // 1MB
    
    while (allocated < 15 * 1024 * 1024) {  // Try to allocate 15MB
        if (!budget.allocateCpu(chunk, MemoryBudget::Priority::Normal)) {
            break;  // Allocation failed - budget full
        }
        allocated += chunk;
    }
    
    // Should have hit memory limit
    QVERIFY(budget.isCpuUnderPressure() || allocated >= 10 * 1024 * 1024);
    QVERIFY(allocated <= 12 * 1024 * 1024);  // Shouldn't exceed by much
}

void TestIntegration::testConcurrentDecodes() {
    QVERIFY(generateTestVideo(m_testVideoPath, 320, 240, 2, 30));
    
    AsyncDecoder decoder;
    QVERIFY(decoder.initialize());
    
    const int numRequests = 20;
    int completedRequests = 0;
    
    // Request multiple frames concurrently
    for (int i = 0; i < numRequests; ++i) {
        decoder.requestFrame(m_testVideoPath, i, 50, 10000,
            [&completedRequests](FrameHandle frame) {
                Q_UNUSED(frame)
                completedRequests++;
            });
    }
    
    // Wait for all to complete
    QElapsedTimer timer;
    timer.start();
    while (completedRequests < numRequests && timer.elapsed() < 15000) {
        QTest::qWait(100);
    }
    
    QCOMPARE(completedRequests, numRequests);
}

void TestIntegration::testScrubbingPerformance() {
    QVERIFY(generateTestVideo(m_testVideoPath, 640, 480, 5, 30));
    
    AsyncDecoder decoder;
    QVERIFY(decoder.initialize());
    
    // Simulate rapid scrubbing - request frames 0, 10, 5, 15, 2, etc.
    QList<int> scrubSequence = {0, 10, 5, 15, 2, 20, 8, 12, 3, 18};
    
    for (int frameNum : scrubSequence) {
        bool callbackCalled = false;
        
        decoder.requestFrame(m_testVideoPath, frameNum, 100, 2000,
            [&callbackCalled](FrameHandle frame) {
                Q_UNUSED(frame)
                callbackCalled = true;
            });
        
        // Wait briefly (simulating user scrubbing)
        QTest::qWait(50);
        
        // Cancel pending to simulate scrubbing past
        decoder.cancelForFile(m_testVideoPath);
    }
    
    // Should not crash or hang
    QVERIFY(true);
}

void TestIntegration::testLongVideoSeeking() {
    // Generate a longer video (10 seconds)
    QString longVideoPath = m_tempDir.filePath("long_video.mp4");
    QVERIFY(generateTestVideo(longVideoPath, 320, 240, 10, 30));
    
    AsyncDecoder decoder;
    QVERIFY(decoder.initialize());
    
    // Test seeking to various positions
    QList<int> seekPositions = {0, 100, 200, 250, 100, 50, 225};
    
    for (int frameNum : seekPositions) {
        bool callbackCalled = false;
        FrameHandle frame;
        
        decoder.requestFrame(longVideoPath, frameNum, 100, 5000,
            [&callbackCalled, &frame](FrameHandle f) {
                callbackCalled = true;
                frame = f;
            });
        
        // Wait for decode
        QElapsedTimer timer;
        timer.start();
        while (!callbackCalled && timer.elapsed() < 5000) {
            QTest::qWait(50);
        }
        
        QVERIFY2(callbackCalled, QString("Seek to frame %1 timed out").arg(frameNum).toLatin1());
        QVERIFY2(!frame.isNull(), QString("Frame %1 is null").arg(frameNum).toLatin1());
    }
}

void TestIntegration::cleanupTestCase() {
    // Cleanup handled by QTemporaryDir destructor
}

QTEST_MAIN(TestIntegration)
#include "test_integration.moc"
