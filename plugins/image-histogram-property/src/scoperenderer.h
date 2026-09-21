#pragma once

#include "imageanalysis.h"

#include <QByteArray>
#include <QImage>

struct RenderedWaveform {
    QImage image;
    QByteArray pngData;
};

struct RenderedVectorscope {
    QImage image;
    QByteArray pngData;
};

// Waveform rendering: horizontal axis = source horizontal position, vertical
// axis = luma level (black at the bottom).
RenderedWaveform renderWaveform(const imageanalysis::WaveformStats &stats);

// Vectorscope rendering: fixed Cb/Cr coordinates, square plot area, equal axis
// scales, neutral center.
RenderedVectorscope renderVectorscope(const imageanalysis::VectorscopeStats &stats);
