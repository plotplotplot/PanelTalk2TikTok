#include <QtTest/QtTest>
#include "../frame_handle.h"

using namespace editor;

class TestFrameHandle : public QObject {
    Q_OBJECT

private slots:
    void testDefaultConstruction();
    void testNullFrame();
    void testCpuFrameCreation();
    void testFrameComparison();
    void testMemoryUsage();
    void testSharedData();
};

void TestFrameHandle::testDefaultConstruction() {
    FrameHandle frame;
    QVERIFY(frame.isNull());
    QVERIFY(!frame);
    QCOMPARE(frame.frameNumber(), -1);
    QVERIFY(frame.sourcePath().isEmpty());
}

void TestFrameHandle::testNullFrame() {
    FrameHandle frame;
    QVERIFY(!frame.hasCpuImage());
    QVERIFY(!frame.hasGpuTexture());
    QCOMPARE(frame.memoryUsage(), 0);
}

void TestFrameHandle::testCpuFrameCreation() {
    QImage testImage(100, 100, QImage::Format_RGB32);
    testImage.fill(Qt::red);
    
    FrameHandle frame = FrameHandle::createCpuFrame(testImage, 42, "/path/to/video.mp4");
    
    QVERIFY(!frame.isNull());
    QVERIFY(frame.hasCpuImage());
    QVERIFY(!frame.hasGpuTexture());
    QCOMPARE(frame.frameNumber(), 42);
    QCOMPARE(frame.sourcePath(), QString("/path/to/video.mp4"));
    QCOMPARE(frame.size(), QSize(100, 100));
    QVERIFY(frame.memoryUsage() > 0);
}

void TestFrameHandle::testFrameComparison() {
    QImage testImage(100, 100, QImage::Format_RGB32);
    testImage.fill(Qt::red);
    
    FrameHandle frame1 = FrameHandle::createCpuFrame(testImage, 42, "/path/to/video.mp4");
    FrameHandle frame2 = FrameHandle::createCpuFrame(testImage, 42, "/path/to/video.mp4");
    FrameHandle frame3 = FrameHandle::createCpuFrame(testImage, 43, "/path/to/video.mp4");
    
    // Same frame number and path should be equal
    QVERIFY(frame1 == frame2);
    
    // Different frame number should not be equal
    QVERIFY(frame1 != frame3);
}

void TestFrameHandle::testMemoryUsage() {
    QImage smallImage(100, 100, QImage::Format_RGB32);
    QImage largeImage(1000, 1000, QImage::Format_RGB32);
    
    FrameHandle smallFrame = FrameHandle::createCpuFrame(smallImage, 1, "test.mp4");
    FrameHandle largeFrame = FrameHandle::createCpuFrame(largeImage, 1, "test.mp4");
    
    QVERIFY(largeFrame.memoryUsage() > smallFrame.memoryUsage());
}

void TestFrameHandle::testSharedData() {
    QImage testImage(100, 100, QImage::Format_RGB32);
    FrameHandle frame1 = FrameHandle::createCpuFrame(testImage, 1, "test.mp4");
    
    // Copy should share data
    FrameHandle frame2 = frame1;
    QVERIFY(frame1 == frame2);
    
    // Both should have same data pointer
    QCOMPARE(frame1.data(), frame2.data());
}

QTEST_MAIN(TestFrameHandle)
#include "test_frame_handle.moc"
