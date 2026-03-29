#include "frame_handle.h"

#include <QtGui/private/qrhi_p.h>
#include <QDateTime>

extern "C" {
#include <libavutil/frame.h>
}

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
    if (hardwareFrame) {
        av_frame_free(&hardwareFrame);
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

static size_t hardwareFrameEstimateBytes(const FrameData* data) {
    if (!data || !data->hardwareFrame || !data->size.isValid()) {
        return 0;
    }

    const size_t width = static_cast<size_t>(qMax(0, data->size.width()));
    const size_t height = static_cast<size_t>(qMax(0, data->size.height()));
    if (width == 0 || height == 0) {
        return 0;
    }

    switch (data->hardwareSwPixelFormat) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_YUV420P:
            return (width * height * 3) / 2;
        case AV_PIX_FMT_P010:
        case AV_PIX_FMT_P016:
            return width * height * 3;
        default:
            return width * height * 4;
    }
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

FrameHandle FrameHandle::createHardwareFrame(const AVFrame* frame,
                                             int64_t frameNum,
                                             const QString& path,
                                             int swPixelFormat) {
    FrameHandle handle;
    if (!frame) {
        return handle;
    }

    AVFrame* cloned = av_frame_clone(frame);
    if (!cloned) {
        return handle;
    }

    handle.d = new FrameData();
    handle.d->hardwareFrame = cloned;
    handle.d->hardwarePixelFormat = frame->format;
    handle.d->hardwareSwPixelFormat = swPixelFormat;
    handle.d->frameNumber = frameNum;
    handle.d->sourcePath = path;
    handle.d->size = QSize(frame->width, frame->height);
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

size_t FrameHandle::cpuMemoryUsage() const {
    if (!d) {
        return 0;
    }
    return d->cpuImage.isNull() ? 0 : d->cpuImage.sizeInBytes();
}

size_t FrameHandle::gpuMemoryUsage() const {
    if (!d) {
        return 0;
    }

    size_t total = 0;
    if (d->gpuTexture) {
        QRhiTexture::Format fmt = d->gpuTexture->format();
        int bpp = 4;
        switch (fmt) {
            case QRhiTexture::RGBA8: bpp = 4; break;
            case QRhiTexture::BGRA8: bpp = 4; break;
            case QRhiTexture::R8: bpp = 1; break;
            case QRhiTexture::RG8: bpp = 2; break;
            default: bpp = 4; break;
        }
        total += static_cast<size_t>(d->size.width()) * static_cast<size_t>(d->size.height()) * static_cast<size_t>(bpp);
    }
    total += hardwareFrameEstimateBytes(d.data());
    return total;
}

} // namespace editor
