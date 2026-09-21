#include "imageanalysis.h"

#include <QColor>
#include <algorithm>
#include <cmath>

namespace imageanalysis {

namespace {

constexpr double kKr     = 0.2126;
constexpr double kKg     = 0.7152;
constexpr double kKb     = 0.0722;
constexpr double kCbScale = 1.8556;
constexpr double kCrScale = 1.5748;

int clampByte(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// Luma (BT.709 coefficients) from straight 8-bit RGB 0..255.
int lumaOf(int r, int g, int b)
{
    const double y = kKr * r + kKg * g + kKb * b;
    return clampByte(static_cast<int>(std::lround(y)));
}

// Both native analysis formats use the QRgb layout with straight (non
// premultiplied) channel values, so a whole-image conversion is only needed for
// every other format.
bool isAnalysisFormat(QImage::Format format)
{
    return format == QImage::Format_ARGB32 || format == QImage::Format_RGB32;
}

void accumulateWaveform(const QImage &image, WaveformStats &stats)
{
    const int width  = image.width();
    const int height = image.height();
    const int xDenominator = width > 1 ? width - 1 : 1;

    for (int y = 0; y < height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb px = line[x];
            const int luma
                = lumaOf(qRed(px), qGreen(px), qBlue(px));
            int xBin = static_cast<int>((static_cast<quint64>(x) * 255U)
                                        / static_cast<quint64>(xDenominator));
            if (xBin > WaveformStats::kBins - 1) {
                xBin = WaveformStats::kBins - 1;
            }
            ++stats.density[static_cast<std::size_t>(luma) * WaveformStats::kBins
                            + static_cast<std::size_t>(xBin)];
            ++stats.samples;
        }
    }
}

void accumulateVectorscope(const QImage &image, VectorscopeStats &stats)
{
    const int width  = image.width();
    const int height = image.height();
    const int last   = VectorscopeStats::kGrid - 1;

    for (int y = 0; y < height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb px = line[x];
            const double rn = qRed(px) / 255.0;
            const double gn = qGreen(px) / 255.0;
            const double bn = qBlue(px) / 255.0;
            const double yv = kKr * rn + kKg * gn + kKb * bn;
            const double cb = (bn - yv) / kCbScale;
            const double cr = (rn - yv) / kCrScale;

            // Map the fixed [-0.5, 0.5] axis range onto the grid. Cb is the
            // column index, Cr the row index; the renderer draws row 0 at the
            // bottom so that positive Cr points upwards.
            const int cbIndex = static_cast<int>(
                std::lround((cb + 0.5) * static_cast<double>(last)));
            const int crIndex = static_cast<int>(
                std::lround((cr + 0.5) * static_cast<double>(last)));
            const int cbC = cbIndex < 0 ? 0 : (cbIndex > last ? last : cbIndex);
            const int crC = crIndex < 0 ? 0 : (crIndex > last ? last : crIndex);
            ++stats.density[static_cast<std::size_t>(crC) * VectorscopeStats::kGrid
                            + static_cast<std::size_t>(cbC)];
            ++stats.samples;
        }
    }
}

}  // namespace

QImage toAnalysisImage(const QImage &source)
{
    if (source.isNull()) {
        return QImage();
    }
    if (isAnalysisFormat(source.format())) {
        return source;
    }
    return source.convertToFormat(QImage::Format_ARGB32);
}

ScopeStats analyzeScopes(const QImage &decoded)
{
    ScopeStats stats;
    const QImage image = toAnalysisImage(decoded);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return stats;
    }
    accumulateWaveform(image, stats.waveform);
    accumulateVectorscope(image, stats.vectorscope);
    return stats;
}

WaveformStats computeWaveform(const QImage &decoded)
{
    WaveformStats stats;
    const QImage image = toAnalysisImage(decoded);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return stats;
    }
    accumulateWaveform(image, stats);
    return stats;
}

VectorscopeStats computeVectorscope(const QImage &decoded)
{
    VectorscopeStats stats;
    const QImage image = toAnalysisImage(decoded);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return stats;
    }
    accumulateVectorscope(image, stats);
    return stats;
}

QString analysisDescription()
{
    return QStringLiteral(
        "8-bit decoded straight RGB; full range 0-255; BT.709 coefficients "
        "Y'=0.2126R'+0.7152G'+0.0722B', Cb=(B'-Y')/1.8556, Cr=(R'-Y')/1.5748. "
        "No subsampling: every decoded pixel contributes once and the source "
        "spatial distribution is preserved. First animation frame only; EXIF "
        "orientation applied by the decoder. Not an HDR, scene-linear, or "
        "fully color-managed measurement.");
}

}  // namespace imageanalysis
