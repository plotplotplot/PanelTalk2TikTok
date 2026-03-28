#pragma once

#include <QImage>

struct AVFrame;

bool fillNv12FrameFromImage(const QImage& image, AVFrame* frame);
