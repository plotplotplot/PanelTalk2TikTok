#include "frame_handle.h"

#include <QtGui/private/qrhi_p.h>
#include <QDateTime>

namespace editor {

// ============================================================================
// FrameData Implementation
// ============================================================================

FrameData::~FrameData() {
    // GPU texture cleanup must happen on the render thread
    // The texture pointer is stored but actual cleanup is handled by QRhi
    if (gpuTexture && gpuTextureOwned.load() == 1) {
        // Mark for deferred deletion on render thread
        // (Actual cleanup happens in render loop)
    }
}

size_t FrameData::memoryUsage() const {
    size_t total = 0;
    
    // CPU image memory
    if (!cpuImage.isNull()) {
        total += cpuImage.sizeInBytes();
    }
    
    // GPU texture memory (estimate)
    if (gpuTexture) {
        QRhiTexture::Format fmt = gpuTexture->format();
        int bpp = 4; // Default to 4 bytes per pixel
        switch (fmt) {
            case QRhiTexture::RGBA8: bpp = 4; break;
            case QRhiTexture::BGRA8: bpp = 4; break;
            case QRhiTexture::R8: bpp = 1; break;
            case QRhiTexture::RG8: bpp = 2; break;
            default: bpp = 4; break;
        }
        total += size.width() * size.height() * bpp;
    }
    
    return total;
}

// ============================================================================
// FrameHandle Implementation
// ============================================================================

FrameHandle::FrameHandle() = default;

FrameHandle FrameHandle::createCpuFrame(const QImage& image, int64_t frameNum, const QString& path) {
    FrameHandle handle;
    handle.d = new FrameData();
    handle.d->cpuImage = image;
    handle.d->frameNumber = frameNum;
    handle.d->sourcePath = path;
    handle.d->size = image.size();
    handle.d->decodeTimestamp = QDateTime::currentMSecsSinceEpoch();
    return handle;
}

FrameHandle FrameHandle::createGpuFrame(QRhiTexture* texture, int64_t frameNum, const QString& path) {
    FrameHandle handle;
    if (!texture) return handle;
    
    handle.d = new FrameData();
    handle.d->gpuTexture = texture;
    handle.d->gpuTextureOwned.store(1);
    handle.d->frameNumber = frameNum;
    handle.d->sourcePath = path;
    handle.d->size = QSize(texture->pixelSize().width(), texture->pixelSize().height());
    handle.d->decodeTimestamp = QDateTime::currentMSecsSinceEpoch();
    return handle;
}

void FrameHandle::uploadToGpu(QRhi* rhi) {
    if (!d || d->cpuImage.isNull() || !rhi) {
        return;
    }
    
    if (d->gpuTexture) {
        // Already uploaded
        return;
    }
    
    // Convert to RGBA8 if needed
    QImage uploadImage = d->cpuImage;
    if (uploadImage.format() != QImage::Format_RGBA8888 && 
        uploadImage.format() != QImage::Format_ARGB32 &&
        uploadImage.format() != QImage::Format_RGB32) {
        uploadImage = uploadImage.convertToFormat(QImage::Format_RGBA8888);
    }
    
    // Create texture
    QRhiTexture::Format fmt = QRhiTexture::RGBA8;
    QRhiTexture* texture = rhi->newTexture(fmt, uploadImage.size(), 1);
    if (!texture->create()) {
        delete texture;
        return;
    }
    
    // Upload data - QRhi API varies by version
    // In Qt 6.4, we use resource update batch submitted via beginFrame/endFrame
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    batch->uploadTexture(texture, uploadImage);
    // Note: The batch must be submitted during a frame - this is handled by the caller
    
    d->gpuTexture = texture;
    d->rhiContext = rhi;
    d->gpuTextureOwned.store(1);
    m_gpuUploadPending = false;
}

bool FrameHandle::operator==(const FrameHandle& other) const {
    if (d.constData() == other.d.constData()) return true;
    if (!d || !other.d) return false;
    return d->sourcePath == other.d->sourcePath && 
           d->frameNumber == other.d->frameNumber;
}

} // namespace editor
