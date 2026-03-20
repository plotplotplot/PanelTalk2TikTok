#include <memory>
#include "qt_compat.h"
// FFmpeg types - defined in async_decoder.cpp
extern "C" {
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFormatContext AVFormatContext;
typedef struct AVFrame AVFrame;
typedef struct AVPacket AVPacket;
typedef struct AVBufferRef AVBufferRef;
typedef int AVPixelFormat;  // Simplified for header
}
#include <QtCore/QMutex>
int main() { return 0; }
