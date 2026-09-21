#pragma once

#include <cstdint>
#include <vector>
#include <QtGlobal>
#include <QImage>
#include <QString>

namespace imageanalysis {

// Unified 8-bit decoded-RGB analysis convention.
//
// Every scope analysis in this plugin works on pixels that were decoded once
// and normalized to a non-premultiplied 8-bit RGB(A) buffer:
//
//   * premultiplied formats are un-premultiplied before analysis, so channel
//     values are straight (non-premultiplied) RGB;
//   * higher-bit-depth formats (for example 16-bit PNG) are narrowed to 8 bits
//     per channel by Qt's format conversion;
//   * transparent pixels contribute their straight RGB values, exactly as the
//     histogram analysis has always treated them;
//   * luma uses the BT.709 coefficients
//     Y' = 0.2126 R' + 0.7152 G' + 0.0722 B' over full-range 0..255 values.
//
// This is deliberately NOT an HDR, scene-linear, or fully color-managed
// measurement system, and the chroma projection is a BT.709-coefficient
// projection under the convention above rather than a broadcast-compliant
// HDR measurement.

struct WaveformStats {
    static constexpr int kLevels = 256;  // luma levels, 0..255
    static constexpr int kBins   = 256;  // horizontal source-position bins

    // density[level * kBins + xBin] accumulates pixel density, not a
    // horizontally stretched histogram.
    std::vector<quint64> density = std::vector<quint64>(
        static_cast<std::size_t>(kLevels) * kBins, 0);
    quint64 samples = 0;

    quint64 at(int level, int xBin) const
    {
        return density[static_cast<std::size_t>(level) * kBins + xBin];
    }
};

struct VectorscopeStats {
    // Fixed Cb/Cr coordinate system; the per-image chroma extent is never
    // rescaled to fill the plot. Cb is horizontal, Cr is vertical, and neutral
    // colors stay at the grid center. The axis range is fixed to [-0.5, 0.5].
    // Cb = (B'-Y')/1.8556, Cr = (R'-Y')/1.5748
    static constexpr int kGrid = 256;

    // density[cr * kGrid + cb]; row 0 is the most negative Cr.
    std::vector<quint64> density = std::vector<quint64>(
        static_cast<std::size_t>(kGrid) * kGrid, 0);
    quint64 samples = 0;

    quint64 at(int cr, int cb) const
    {
        return density[static_cast<std::size_t>(cr) * kGrid + cb];
    }
};

struct ScopeStats {
    WaveformStats waveform;
    VectorscopeStats vectorscope;
};

// Normalizes a decoded image into the single non-premultiplied 8-bit RGB(A)
// buffer shared by the scope analyses. QImage::Format_ARGB32 and
// QImage::Format_RGB32 already satisfy the convention and are returned as-is
// with no copy; every other format is converted once. Returns a null image when
// the source is null or cannot be normalized.
QImage toAnalysisImage(const QImage &source);

// Shared analysis entry point: the decoded image is normalized exactly once
// and both scope grids accumulate from that single buffer. No full-resolution
// copy is created per scope.
ScopeStats analyzeScopes(const QImage &decoded);

// Single-scope analysis. Each call normalizes on its own; the helper uses
// analyzeScopes() instead so the decoded image is only normalized once.
WaveformStats computeWaveform(const QImage &decoded);
VectorscopeStats computeVectorscope(const QImage &decoded);

// Human-readable convention summary published as a result row.
QString analysisDescription();

}  // namespace imageanalysis
