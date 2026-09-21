#include "scoperenderer.h"

#include <QBuffer>
#include <QFont>
#include <QPainter>
#include <QPen>
#include <algorithm>
#include <cmath>

namespace {

// Neutral, self-contained chart theme. The images are rendered without any
// host font or theme API so they stay reproducible outside Seer.
const QColor kBackground(30, 30, 32);
const QColor kBorder(70, 70, 78);
const QColor kGrid(55, 55, 60);
const QColor kLabel(160, 160, 170);
const QColor kTick(140, 140, 150);

QByteArray toPng(const QImage &image)
{
    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return pngData;
}

void setLabelFont(QPainter &painter)
{
    QFont f = painter.font();
    f.setPointSize(8);
    painter.setFont(f);
}

double maxLog1p(const std::vector<quint64> &density)
{
    double maxLog = 0.0;
    for (const auto count : density) {
        if (count > 0) {
            maxLog = std::max(maxLog, std::log1p(static_cast<double>(count)));
        }
    }
    return maxLog > 0.0 ? maxLog : 1.0;
}

// Density is displayed with the documented log1p scale: brighter cells mean
// more accumulated pixels, and the scale is global to the whole grid.
int densityLevel(quint64 count, double maxLog)
{
    const double norm = std::log1p(static_cast<double>(count)) / maxLog;
    return static_cast<int>(std::lround(40.0 + norm * 200.0));
}

// One-dimensional pixel span for a grid index, centred on its position and kept
// inside the plot area. Neighbouring spans touch without gaps, the outermost
// spans end exactly on the frame instead of overdraw-ing it, and a span thinner
// than one pixel is widened so sub-pixel cells (256 levels over 204 px) stay
// visible.
struct PixelSpan {
    double from = 0.0;
    double to   = 0.0;
    double length() const { return to - from; }
};

PixelSpan pixelSpan(double center, double half, double lo, double hi)
{
    PixelSpan span;
    span.from = center - half;
    span.to   = center + half;
    if (span.from < lo) {
        span.to += lo - span.from;
        span.from = lo;
    }
    if (span.to > hi) {
        span.from -= span.to - hi;
        span.to = hi;
    }
    if (span.length() < 1.0) {
        if (span.from <= lo) {
            span.from = lo;
            span.to   = std::min(hi, span.from + 1.0);
        }
        else {
            span.to   = std::min(hi, span.to + (1.0 - span.length()));
            span.from = std::max(lo, span.to - 1.0);
        }
    }
    return span;
}

}  // namespace

RenderedWaveform renderWaveform(const imageanalysis::WaveformStats &stats)
{
    constexpr int kWidth  = 768;
    constexpr int kHeight = 256;
    constexpr int kLevels = imageanalysis::WaveformStats::kLevels;
    constexpr int kBins   = imageanalysis::WaveformStats::kBins;

    QImage image(kWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, kWidth, kHeight), kBackground);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const double plotLeft   = 44.0;
    const double plotTop    = 26.0;
    const double plotWidth  = static_cast<double>(kWidth) - 16.0 - plotLeft;
    const double plotHeight = static_cast<double>(kHeight) - 26.0 - plotTop;
    const double plotBottom = plotTop + plotHeight;
    const double plotRight  = plotLeft + plotWidth;

    const double maxLog = maxLog1p(stats.density);

    painter.setPen(QPen(kGrid, 1.0, Qt::DashLine));
    for (int step = 1; step <= 3; ++step) {
        const double y = plotTop + (plotHeight / 4.0) * step;
        painter.drawLine(QPointF(plotLeft, y), QPointF(plotRight, y));
    }
    for (int xb : {64, 128, 192}) {
        const double x = plotLeft + (static_cast<double>(xb) / (kBins - 1)) * plotWidth;
        painter.drawLine(QPointF(x, plotTop), QPointF(x, plotBottom));
    }

    // Frame before the trace, matching the established histogram renderer.
    // Drawing the frame last would overwrite the level-0 and level-255 bands,
    // which sit exactly on the plot edges.
    painter.setPen(QPen(kBorder, 1.0, Qt::SolidLine));
    painter.drawRect(QRectF(plotLeft, plotTop, plotWidth, plotHeight));

    // Density image. The horizontal axis is the source image's horizontal
    // position (not a histogram bin) and the vertical axis is luma intensity
    // with black at the bottom.
    const double halfX = plotWidth / (kBins - 1) / 2.0;
    const double halfY = plotHeight / (kLevels - 1) / 2.0;
    for (int level = 0; level < kLevels; ++level) {
        for (int xb = 0; xb < kBins; ++xb) {
            const quint64 count = stats.at(level, xb);
            if (count == 0) {
                continue;
            }
            const int lum = densityLevel(count, maxLog);
            if (lum <= 40) {
                continue;
            }
            const double centerX = plotLeft + (static_cast<double>(xb) / (kBins - 1)) * plotWidth;
            const double centerY = plotBottom - (static_cast<double>(level) / (kLevels - 1)) * plotHeight;
            const PixelSpan spanX = pixelSpan(centerX, halfX, plotLeft, plotRight);
            const PixelSpan spanY = pixelSpan(centerY, halfY, plotTop, plotBottom);
            painter.fillRect(QRectF(spanX.from, spanY.from, spanX.length(),
                                    spanY.length()),
                             QColor(200, 230, 255, qBound(0, lum, 255)));
        }
    }

    setLabelFont(painter);
    painter.setPen(kLabel);
    painter.drawText(QRectF(plotLeft, 4, 200, 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Luma Waveform (BT.709)"));
    painter.drawText(QRectF(plotRight - 90, 4, 90, 18),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("density log1p"));

    // Horizontal axis: source image x position, 0..255 label space.
    painter.setPen(kTick);
    const double tickY = plotBottom + 4;
    const double xScale = plotWidth / (kBins - 1);
    painter.drawText(QRectF(plotLeft - 15, tickY, 30, 16), Qt::AlignCenter,
                     QStringLiteral("0"));
    for (int xb : {64, 128, 192}) {
        painter.drawText(QRectF(plotLeft + xb * xScale - 15, tickY, 30, 16),
                         Qt::AlignCenter, QString::number(xb));
    }
    painter.drawText(QRectF(plotRight - 15, tickY, 30, 16), Qt::AlignCenter,
                     QStringLiteral("255"));

    // Vertical axis: luma level, black at the bottom, full-range 0..255.
    painter.drawText(QRectF(0, plotBottom - 8, 40, 16),
                     Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("0"));
    painter.drawText(QRectF(0, plotTop - 8, 40, 16),
                     Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("255"));

    painter.end();

    RenderedWaveform result;
    result.image   = image;
    result.pngData = toPng(image);
    return result;
}

RenderedVectorscope renderVectorscope(const imageanalysis::VectorscopeStats &stats)
{
    constexpr int kSize   = 560;
    constexpr int kMargin = 40;
    constexpr double kPlotSize = kSize - 2.0 * kMargin;
    constexpr int kGridSize = imageanalysis::VectorscopeStats::kGrid;

    QImage image(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, kSize, kSize), kBackground);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const double plotLeft   = kMargin;
    const double plotTop    = kMargin;
    const double plotRight  = kMargin + kPlotSize;
    const double plotBottom = kMargin + kPlotSize;

    const double maxLog = maxLog1p(stats.density);

    // Axis mapping shared by the density image and the reference markers: the
    // fixed [-0.5, 0.5] range spans the square plot area, Cb grows to the right
    // and Cr grows upwards with neutral colors exactly at the center.
    const auto cbToX = [&](double cb) {
        return plotLeft + (cb + 0.5) * kPlotSize;
    };
    const auto crToY = [&](double cr) {
        return plotBottom - (cr + 0.5) * kPlotSize;
    };

    // Frame and centre axes before the trace (see renderWaveform): drawing them
    // last would hide the most saturated cells, which sit on the plot edges and
    // on the neutral centre.
    painter.setPen(QPen(kBorder, 1.0, Qt::SolidLine));
    painter.drawRect(QRectF(plotLeft, plotTop, kPlotSize, kPlotSize));

    const double centerX = plotLeft + kPlotSize / 2.0;
    const double centerY = plotTop + kPlotSize / 2.0;
    painter.setPen(QPen(kGrid, 1.0, Qt::DashLine));
    painter.drawLine(QPointF(centerX, plotTop), QPointF(centerX, plotBottom));
    painter.drawLine(QPointF(plotLeft, centerY), QPointF(plotRight, centerY));

    for (int cr = 0; cr < kGridSize; ++cr) {
        for (int cb = 0; cb < kGridSize; ++cb) {
            const quint64 count = stats.at(cr, cb);
            if (count == 0) {
                continue;
            }
            const int lum = densityLevel(count, maxLog);
            if (lum <= 40) {
                continue;
            }
            const double halfCell = kPlotSize / (kGridSize - 1) / 2.0;
            const double centerX  = cbToX(static_cast<double>(cb) / (kGridSize - 1) - 0.5);
            const double centerY  = crToY(static_cast<double>(cr) / (kGridSize - 1) - 0.5);
            const PixelSpan spanX = pixelSpan(centerX, halfCell, plotLeft, plotRight);
            const PixelSpan spanY = pixelSpan(centerY, halfCell, plotTop, plotBottom);
            painter.fillRect(QRectF(spanX.from, spanY.from, spanX.length(),
                                    spanY.length()),
                             QColor(255, 255, 255, qBound(0, lum, 255)));
        }
    }

    // Fixed reference markers for the primary and secondary colors, drawn at
    // the same BT.709 projection used by the density image. Their positions are
    // never derived from the analyzed image, so comparing a low-saturation and
    // a high-saturation image stays meaningful.
    struct Marker {
        const char *label;
        double r, g, b;
        QColor color;
    };
    const Marker markers[] = {
        {"R", 1.0, 0.0, 0.0, QColor(255, 90, 90)},
        {"G", 0.0, 1.0, 0.0, QColor(90, 230, 90)},
        {"B", 0.0, 0.0, 1.0, QColor(100, 170, 255)},
        {"C", 0.0, 1.0, 1.0, QColor(90, 220, 220)},
        {"M", 1.0, 0.0, 1.0, QColor(230, 120, 230)},
        {"Y", 1.0, 1.0, 0.0, QColor(230, 230, 100)},
    };
    setLabelFont(painter);
    for (const auto &m : markers) {
        const double yv = 0.2126 * m.r + 0.7152 * m.g + 0.0722 * m.b;
        const double cb = (m.b - yv) / 1.8556;
        const double cr = (m.r - yv) / 1.5748;
        const double px = cbToX(cb);
        const double py = crToY(cr);
        painter.setPen(QPen(m.color, 1.8));
        painter.drawEllipse(QPointF(px, py), 4.0, 4.0);
        painter.drawText(QPointF(px + 7.0, py - 5.0), QLatin1String(m.label));
    }

    setLabelFont(painter);
    painter.setPen(kLabel);
    painter.drawText(QRectF(plotLeft, 8, 240, 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Cb/Cr Vectorscope (BT.709)"));
    painter.drawText(QRectF(plotLeft, kSize - 20, kPlotSize, 16),
                     Qt::AlignCenter,
                     QStringLiteral("density log1p; fixed axes, not rescaled"));

    // Fixed axis labels in the normalized [-0.5, 0.5] Cb/Cr range.
    painter.setPen(kTick);
    painter.drawText(QRectF(plotLeft - 34, centerY - 8, 32, 16),
                     Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("-0.5"));
    painter.drawText(QRectF(plotRight + 2, centerY - 8, 34, 16),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("+0.5"));
    painter.drawText(QRectF(centerX - 20, plotTop - 18, 40, 16),
                     Qt::AlignCenter, QStringLiteral("+0.5"));
    painter.drawText(QRectF(centerX - 20, plotBottom + 2, 40, 16),
                     Qt::AlignCenter, QStringLiteral("-0.5"));
    painter.drawText(QRectF(plotLeft, plotTop - 18, 80, 16),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Cr"));

    painter.end();

    RenderedVectorscope result;
    result.image   = image;
    result.pngData = toPng(image);
    return result;
}
