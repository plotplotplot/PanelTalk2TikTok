#pragma once

#include "qt_compat.h"  // Qt 6.4/GCC 13 compatibility
#include <QExplicitlySharedDataPointer>
#include <QImage>
#include <QSize>
#include <QString>
#include <atomic>
#include <cstdint>

// Forward declarations for QRhi
class QRhiTexture;
class QRhi;
struct AVFrame;

namespace editor {

// ============================================================================
// FrameData - Internal shared data for FrameHandle
// ============================================================================
class FrameData : public QSharedData {
public:
    FrameData() = default;
    ~FrameData();
    
    // Prevent copying (use shared data pattern)
    FrameData(const FrameData&) = delete;
    FrameData& operator=(const FrameData&) = delete;
    
    // CPU-side image data (if software decoded)
    QImage cpuImage;
    
    // GPU-side texture (if available)
    QRhiTexture* gpuTexture = nullptr;
    QRhi* rhiContext = nullptr;  // For texture cleanup
    AVFrame* hardwareFrame = nullptr;
    int hardwarePixelFormat = -1;
    int hardwareSwPixelFormat = -1;
    
    // Metadata
    int64_t frameNumber = -1;
    QString sourcePath;
    QSize size;
    qint64 decodeTimestamp = 0;  // When this frame was decoded
    std::atomic<int> gpuTextureOwned{0};  // 1 if we own the texture
    
    // Memory tracking
    size_t memoryUsage() const;
};

// ============================================================================
// FrameHandle - RAII wrapper for decoded frames
// 
// Thread-safe, reference-counted handle to a decoded frame.
// Automatically manages GPU texture lifecycle.
// ============================================================================
class FrameHandle {
public:
    FrameHandle();
    FrameData* data() const { return d.data(); }
    
    // Creation helpers
    static FrameHandle createCpuFrame(const QImage& image, int64_t frameNum, const QString& path);
    static FrameHandle createGpuFrame(QRhiTexture* texture, int64_t frameNum, const QString& path);
    static FrameHandle createHardwareFrame(const AVFrame* frame,
                                           int64_t frameNum,
                                           const QString& path,
                                           int swPixelFormat);
    
    // Validity
    bool isNull() const { return d.constData() == nullptr; }
    explicit operator bool() const { return !isNull(); }
    
    // Accessors
    int64_t frameNumber() const { return d ? d->frameNumber : -1; }
    QString sourcePath() const { return d ? d->sourcePath : QString(); }
    QSize size() const { return d ? d->size : QSize(); }
    bool hasCpuImage() const { return d && !d->cpuImage.isNull(); }
    bool hasGpuTexture() const { return d && d->gpuTexture != nullptr; }
    bool hasHardwareFrame() const { return d && d->hardwareFrame != nullptr; }
    
    QImage cpuImage() const { return d ? d->cpuImage : QImage(); }
    QRhiTexture* gpuTexture() const { return d ? d->gpuTexture : nullptr; }
    const AVFrame* hardwareFrame() const { return d ? d->hardwareFrame : nullptr; }
    int hardwarePixelFormat() const { return d ? d->hardwarePixelFormat : -1; }
    int hardwareSwPixelFormat() const { return d ? d->hardwareSwPixelFormat : -1; }
    
    size_t memoryUsage() const { return d ? d->memoryUsage() : 0; }
    size_t cpuMemoryUsage() const;
    size_t gpuMemoryUsage() const;
    
    // GPU texture upload (async)
    void uploadToGpu(QRhi* rhi);
    bool isGpuUploadPending() const { return m_gpuUploadPending; }
    
    // Comparison for caching
    bool operator==(const FrameHandle& other) const;
    bool operator!=(const FrameHandle& other) const { return !(*this == other); }
    
private:
    QExplicitlySharedDataPointer<FrameData> d;
    bool m_gpuUploadPending = false;
};

// ============================================================================
// FrameCacheKey - For hash-based frame lookup
// ============================================================================
struct FrameCacheKey {
    QString path;
    int64_t frameNumber;
    
    bool operator==(const FrameCacheKey& other) const {
        return frameNumber == other.frameNumber && path == other.path;
    }
};

} // namespace editor

// Make FrameCacheKey hashable
namespace std {
template<>
struct hash<editor::FrameCacheKey> {
    size_t operator()(const editor::FrameCacheKey& key) const {
        return qHash(key.path) ^ std::hash<int64_t>{}(key.frameNumber);
    }
};
} // namespace std

// Note: Q_DECLARE_METATYPE causes issues with Qt 6.4/GCC 13
// Use qRegisterMetaType<editor::FrameHandle>() in .cpp file instead
// Q_DECLARE_METATYPE(editor::FrameHandle)
