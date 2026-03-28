#include "gl_frame_texture_shared.h"

#include <QDateTime>
#include <QImage>
#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include <cuda.h>
#include <cudaGL.h>

namespace editor {

bool frameUsesCudaZeroCopyCandidate(const FrameHandle& frame) {
    return frame.hasHardwareFrame() && frame.hardwareSwPixelFormat() == AV_PIX_FMT_NV12;
}

bool uploadCudaNv12FrameToTextures(const FrameHandle& frame, GLuint textureId, GLuint uvTextureId) {
    if (!textureId || !uvTextureId || !frameUsesCudaZeroCopyCandidate(frame)) {
        return false;
    }

    const AVFrame* hwFrame = frame.hardwareFrame();
    if (!hwFrame || !hwFrame->hw_frames_ctx) {
        return false;
    }

    auto* framesContext = reinterpret_cast<AVHWFramesContext*>(hwFrame->hw_frames_ctx->data);
    if (!framesContext || !framesContext->device_ctx) {
        return false;
    }
    auto* deviceContext = framesContext->device_ctx;
    if (!deviceContext || deviceContext->type != AV_HWDEVICE_TYPE_CUDA) {
        return false;
    }
    auto* cudaDeviceContext = reinterpret_cast<AVCUDADeviceContext*>(deviceContext->hwctx);
    if (!cudaDeviceContext) {
        return false;
    }

    if (cuCtxPushCurrent(cudaDeviceContext->cuda_ctx) != CUDA_SUCCESS) {
        return false;
    }

    auto restoreContext = []() {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
    };

    const int width = hwFrame->width;
    const int height = hwFrame->height;
    if (width <= 0 || height <= 0) {
        restoreContext();
        return false;
    }

    CUgraphicsResource yResource = nullptr;
    CUgraphicsResource uvResource = nullptr;
    CUresult result = cuGraphicsGLRegisterImage(&yResource, textureId, GL_TEXTURE_2D,
                                                CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD);
    if (result != CUDA_SUCCESS) {
        restoreContext();
        return false;
    }
    result = cuGraphicsGLRegisterImage(&uvResource, uvTextureId, GL_TEXTURE_2D,
                                       CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD);
    if (result != CUDA_SUCCESS) {
        cuGraphicsUnregisterResource(yResource);
        restoreContext();
        return false;
    }

    CUgraphicsResource resources[2] = { yResource, uvResource };
    result = cuGraphicsMapResources(2, resources, 0);
    if (result != CUDA_SUCCESS) {
        cuGraphicsUnregisterResource(uvResource);
        cuGraphicsUnregisterResource(yResource);
        restoreContext();
        return false;
    }

    CUarray yArray = nullptr;
    CUarray uvArray = nullptr;
    result = cuGraphicsSubResourceGetMappedArray(&yArray, yResource, 0, 0);
    if (result == CUDA_SUCCESS) {
        result = cuGraphicsSubResourceGetMappedArray(&uvArray, uvResource, 0, 0);
    }
    if (result == CUDA_SUCCESS) {
        CUDA_MEMCPY2D yCopy{};
        yCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        yCopy.srcDevice = reinterpret_cast<CUdeviceptr>(hwFrame->data[0]);
        yCopy.srcPitch = static_cast<size_t>(hwFrame->linesize[0]);
        yCopy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        yCopy.dstArray = yArray;
        yCopy.WidthInBytes = static_cast<size_t>(width);
        yCopy.Height = static_cast<size_t>(height);
        result = cuMemcpy2D(&yCopy);
    }
    if (result == CUDA_SUCCESS) {
        CUDA_MEMCPY2D uvCopy{};
        uvCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        uvCopy.srcDevice = reinterpret_cast<CUdeviceptr>(hwFrame->data[1]);
        uvCopy.srcPitch = static_cast<size_t>(hwFrame->linesize[1]);
        uvCopy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        uvCopy.dstArray = uvArray;
        uvCopy.WidthInBytes = static_cast<size_t>(qMax(1, width / 2) * 2);
        uvCopy.Height = static_cast<size_t>(qMax(1, height / 2));
        result = cuMemcpy2D(&uvCopy);
    }

    cuGraphicsUnmapResources(2, resources, 0);
    cuGraphicsUnregisterResource(uvResource);
    cuGraphicsUnregisterResource(yResource);
    restoreContext();
    return result == CUDA_SUCCESS;
}

QString textureCacheKey(const FrameHandle& frame) {
    return QStringLiteral("%1|%2").arg(frame.sourcePath()).arg(frame.frameNumber());
}

QString reusableTextureCacheKey(const FrameHandle& frame) {
    return QStringLiteral("%1|%2")
        .arg(frame.sourcePath())
        .arg(frameUsesCudaZeroCopyCandidate(frame) ? QStringLiteral("nv12") : QStringLiteral("rgba"));
}

bool shouldUseReusableTextureCache(const FrameHandle& frame) {
    return frame.hasHardwareFrame() || isImageSequencePath(frame.sourcePath());
}

void destroyGlTextureEntry(GlTextureCacheEntry* entry) {
    if (!entry) {
        return;
    }
    if (entry->textureId != 0) {
        glDeleteTextures(1, &entry->textureId);
        entry->textureId = 0;
    }
    if (entry->auxTextureId != 0) {
        glDeleteTextures(1, &entry->auxTextureId);
        entry->auxTextureId = 0;
    }
    entry->size = QSize();
    entry->usesYuvTextures = false;
}

bool uploadFrameToGlTextureEntry(const FrameHandle& frame, GlTextureCacheEntry* entry) {
    if (!entry) {
        return false;
    }

    const bool wantsYuv = frameUsesCudaZeroCopyCandidate(frame);
    if (wantsYuv) {
        const QSize frameSize = frame.size();
        if (!frameSize.isValid()) {
            return false;
        }
        const int width = frameSize.width();
        const int height = frameSize.height();
        const int uvWidth = qMax(1, (width + 1) / 2);
        const int uvHeight = qMax(1, (height + 1) / 2);
        const bool needsAllocate =
            entry->textureId == 0 ||
            entry->auxTextureId == 0 ||
            entry->size != frameSize ||
            !entry->usesYuvTextures;
        if (needsAllocate) {
            destroyGlTextureEntry(entry);
            glGenTextures(1, &entry->textureId);
            glBindTexture(GL_TEXTURE_2D, entry->textureId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

            glGenTextures(1, &entry->auxTextureId);
            glBindTexture(GL_TEXTURE_2D, entry->auxTextureId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uvWidth, uvHeight, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
            entry->size = frameSize;
            entry->usesYuvTextures = true;
        }
        if (!uploadCudaNv12FrameToTextures(frame, entry->textureId, entry->auxTextureId)) {
            destroyGlTextureEntry(entry);
            return false;
        }
        entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
        return true;
    }

    if (!frame.hasCpuImage()) {
        return false;
    }
    QImage uploadImage = frame.cpuImage();
    if (uploadImage.format() != QImage::Format_ARGB32_Premultiplied) {
        uploadImage = uploadImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    const bool needsAllocate =
        entry->textureId == 0 ||
        entry->usesYuvTextures ||
        entry->size != uploadImage.size();
    if (needsAllocate) {
        destroyGlTextureEntry(entry);
        glGenTextures(1, &entry->textureId);
        if (entry->textureId == 0) {
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, entry->textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA8,
                     uploadImage.width(),
                     uploadImage.height(),
                     0,
                     GL_BGRA,
                     GL_UNSIGNED_BYTE,
                     nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        entry->usesYuvTextures = false;
        entry->size = uploadImage.size();
    }

    glBindTexture(GL_TEXTURE_2D, entry->textureId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    uploadImage.width(),
                    uploadImage.height(),
                    GL_BGRA,
                    GL_UNSIGNED_BYTE,
                    uploadImage.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void trimGlTextureCache(QHash<QString, GlTextureCacheEntry>* cache, int maxEntries) {
    if (!cache || cache->size() <= maxEntries) {
        return;
    }

    QVector<QString> keys = cache->keys().toVector();
    std::sort(keys.begin(), keys.end(), [cache](const QString& a, const QString& b) {
        return cache->value(a).lastUsedMs < cache->value(b).lastUsedMs;
    });

    const int removeCount = cache->size() - maxEntries;
    for (int i = 0; i < removeCount; ++i) {
        GlTextureCacheEntry entry = cache->take(keys[i]);
        destroyGlTextureEntry(&entry);
    }
}

} // namespace editor
