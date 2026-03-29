#pragma once

#include "editor_shared.h"
#include "frame_handle.h"

#include <QHash>
#include <QSize>
#include <QString>
#include <QtGui/qopengl.h>

#include <cuda.h>

namespace editor {

struct GlTextureCacheEntry {
    GLuint textureId = 0;
    GLuint auxTextureId = 0;
    qint64 decodeTimestamp = 0;
    qint64 lastUsedMs = 0;
    QSize size;
    bool usesYuvTextures = false;
    CUgraphicsResource cudaYResource = nullptr;
    CUgraphicsResource cudaUvResource = nullptr;
};

bool frameUsesCudaZeroCopyCandidate(const FrameHandle& frame);
bool uploadCudaNv12FrameToTextures(const FrameHandle& frame, GLuint textureId, GLuint uvTextureId);

QString textureCacheKey(const FrameHandle& frame);
QString reusableTextureCacheKey(const FrameHandle& frame);
bool shouldUseReusableTextureCache(const FrameHandle& frame);

void destroyGlTextureEntry(GlTextureCacheEntry* entry);
bool uploadFrameToGlTextureEntry(const FrameHandle& frame, GlTextureCacheEntry* entry);
void trimGlTextureCache(QHash<QString, GlTextureCacheEntry>* cache, int maxEntries);

} // namespace editor
