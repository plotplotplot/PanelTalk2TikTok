#include "render_cpu_fallback.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace {

inline int clampByte(int value) {
    return value < 0 ? 0 : (value > 255 ? 255 : value);
}

}

bool fillNv12FrameFromImage(const QImage& image, AVFrame* frame) {
    if (!frame || frame->format != AV_PIX_FMT_NV12 || image.isNull()) {
        return false;
    }

    QImage argb = image;
    if (argb.format() != QImage::Format_ARGB32 && argb.format() != QImage::Format_ARGB32_Premultiplied) {
        argb = image.convertToFormat(QImage::Format_ARGB32);
    }
    if (argb.isNull()) {
        return false;
    }

    const int width = qMin(argb.width(), frame->width);
    const int height = qMin(argb.height(), frame->height);
    uint8_t* yPlane = frame->data[0];
    uint8_t* uvPlane = frame->data[1];
    const int yStride = frame->linesize[0];
    const int uvStride = frame->linesize[1];

    for (int y = 0; y < height; ++y) {
        const uint8_t* src = argb.constScanLine(y);
        uint8_t* dstY = yPlane + y * yStride;
        for (int x = 0; x < width; ++x) {
            const int b = src[x * 4 + 0];
            const int g = src[x * 4 + 1];
            const int r = src[x * 4 + 2];
            const int yValue = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            dstY[x] = static_cast<uint8_t>(clampByte(yValue));
        }
    }

    for (int y = 0; y < height; y += 2) {
        const uint8_t* src0 = argb.constScanLine(y);
        const uint8_t* src1 = argb.constScanLine(qMin(y + 1, height - 1));
        uint8_t* dstUV = uvPlane + (y / 2) * uvStride;
        for (int x = 0; x < width; x += 2) {
            int sumU = 0;
            int sumV = 0;
            int sampleCount = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const uint8_t* src = (dy == 0) ? src0 : src1;
                for (int dx = 0; dx < 2; ++dx) {
                    const int xx = qMin(x + dx, width - 1);
                    const int b = src[xx * 4 + 0];
                    const int g = src[xx * 4 + 1];
                    const int r = src[xx * 4 + 2];
                    const int uValue = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    const int vValue = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                    sumU += uValue;
                    sumV += vValue;
                    ++sampleCount;
                }
            }
            dstUV[x + 0] = static_cast<uint8_t>(clampByte(sumU / qMax(1, sampleCount)));
            if (x + 1 < uvStride) {
                dstUV[x + 1] = static_cast<uint8_t>(clampByte(sumV / qMax(1, sampleCount)));
            }
        }
    }

    return true;
}
