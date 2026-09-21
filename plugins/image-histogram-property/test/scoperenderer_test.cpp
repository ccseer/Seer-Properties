#include <QTest>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <array>
#include <QPoint>
#include <QRect>

#include "imageanalysis.h"
#include "scoperenderer.h"

using namespace imageanalysis;

namespace {

// Locates the brightest pixel of a rendered chart. Density cells are drawn in
// near-white, so they dominate the neutral background, grid, labels and
// reference markers.
QPoint brightestPixel(const QImage &image)
{
    int bestValue = -1;
    QPoint best(0, 0);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            const int value = qGray(c.rgb());
            if (value > bestValue) {
                bestValue = value;
                best = QPoint(x, y);
            }
        }
    }
    return best;
}

// Quadrant of the plotting area in which the brightest density pixel sits.
// Returns "TL", "TR", "BL" or "BR" relative to the image centre.
QString brightestQuadrant(const QImage &image)
{
    const QPoint p = brightestPixel(image);
    const bool left = p.x() < image.width() / 2;
    const bool top  = p.y() < image.height() / 2;
    if (top)
        return left ? QStringLiteral("TL") : QStringLiteral("TR");
    return left ? QStringLiteral("BL") : QStringLiteral("BR");
}

VectorscopeStats singleCell(int cr, int cb)
{
    VectorscopeStats stats;
    stats.density[static_cast<std::size_t>(cr) * VectorscopeStats::kGrid
                  + static_cast<std::size_t>(cb)] = 1;
    stats.samples                                     = 1;
    return stats;
}

}  // namespace

class ScopeRendererTest : public QObject {
    Q_OBJECT

private slots:
    void testWaveformDimensions();
    void testWaveformEmpty();
    void testVectorscopeSquare();
    void testVectorscopeEmpty();
    void testDeterministic();
    void testVectorscopeDensityOrientation();
    void testVectorscopeColorQuadrants();
    void testWaveformRenderingTracksStructure();
};

void ScopeRendererTest::testWaveformDimensions()
{
    WaveformStats stats{};
    stats.samples            = 10;
    stats.density[100 * WaveformStats::kBins + 50] = 10;

    const auto rendered = renderWaveform(stats);
    QVERIFY(!rendered.image.isNull());
    QVERIFY(!rendered.pngData.isEmpty());
    QCOMPARE(rendered.image.width(), 768);
    QCOMPARE(rendered.image.height(), 256);

    QBuffer buffer(const_cast<QByteArray *>(&rendered.pngData));
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    QVERIFY(reader.canRead());
    QCOMPARE(reader.size(), QSize(768, 256));
}

void ScopeRendererTest::testWaveformEmpty()
{
    WaveformStats stats{};
    const auto rendered = renderWaveform(stats);
    QVERIFY(!rendered.pngData.isEmpty());
}

void ScopeRendererTest::testVectorscopeSquare()
{
    VectorscopeStats stats{};
    stats.samples                  = 1;
    stats.density[128 * VectorscopeStats::kGrid + 128] = 1;

    const auto rendered = renderVectorscope(stats);
    QVERIFY(!rendered.image.isNull());
    QVERIFY(!rendered.pngData.isEmpty());
    QCOMPARE(rendered.image.width(), rendered.image.height());

    QBuffer buffer(const_cast<QByteArray *>(&rendered.pngData));
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    QVERIFY(reader.canRead());
    QCOMPARE(reader.size().width(), reader.size().height());
}

void ScopeRendererTest::testVectorscopeEmpty()
{
    VectorscopeStats stats{};
    const auto rendered = renderVectorscope(stats);
    QVERIFY(!rendered.pngData.isEmpty());
}

void ScopeRendererTest::testDeterministic()
{
    WaveformStats wave{};
    wave.density[10 * WaveformStats::kBins + 10]     = 5;
    wave.density[200 * WaveformStats::kBins + 240]   = 8;
    wave.samples                                      = 13;
    const auto r1 = renderWaveform(wave);
    const auto r2 = renderWaveform(wave);
    QCOMPARE(r1.pngData, r2.pngData);

    VectorscopeStats vec{};
    vec.density[10 * VectorscopeStats::kGrid + 240] = 3;
    vec.samples                                     = 3;
    const auto v1 = renderVectorscope(vec);
    const auto v2 = renderVectorscope(vec);
    QCOMPARE(v1.pngData, v2.pngData);
}

// Vertical orientation must be consistent between the accumulated density and
// the fixed reference markers: positive Cr points upwards, negative Cr points
// downwards. Regression guard for a vertically flipped density plot. Cells are
// sampled off the dashed centre axes so the probe pixel is never overdrawn.
void ScopeRendererTest::testVectorscopeDensityOrientation()
{
    const int last   = VectorscopeStats::kGrid - 1;
    const int middle = VectorscopeStats::kGrid / 2;
    const int off    = middle + 40;

    const QImage topCell    = renderVectorscope(singleCell(last, off)).image;
    const QImage bottomCell = renderVectorscope(singleCell(0, off)).image;
    QVERIFY(brightestPixel(topCell).y() < topCell.height() / 2);
    QVERIFY(brightestPixel(bottomCell).y() > bottomCell.height() / 2);

    const QImage leftCell  = renderVectorscope(singleCell(off, 0)).image;
    const QImage rightCell = renderVectorscope(singleCell(off, last)).image;
    QVERIFY(brightestPixel(leftCell).x() < leftCell.width() / 2);
    QVERIFY(brightestPixel(rightCell).x() > rightCell.width() / 2);
}

// The six reference colors must land where the BT.709 Cb/Cr projection puts
// them with positive Cr upwards.
void ScopeRendererTest::testVectorscopeColorQuadrants()
{
    const auto renderedFor = [](const QColor &color) {
        QImage img(1, 1, QImage::Format_RGB888);
        img.setPixelColor(0, 0, color);
        return renderVectorscope(computeVectorscope(img)).image;
    };

    const QImage red     = renderedFor(QColor(255, 0, 0));
    const QImage green   = renderedFor(QColor(0, 255, 0));
    const QImage cyan    = renderedFor(QColor(0, 255, 255));
    const QImage magenta = renderedFor(QColor(255, 0, 255));

    QCOMPARE(brightestQuadrant(red), QStringLiteral("TL"));
    QCOMPARE(brightestQuadrant(green), QStringLiteral("BL"));
    QCOMPARE(brightestQuadrant(cyan), QStringLiteral("BR"));
    QCOMPARE(brightestQuadrant(magenta), QStringLiteral("TR"));

    // Blue and yellow are the Cb extrema: nearly neutral Cr, saturated Cb.
    const QImage blue   = renderedFor(QColor(0, 0, 255));
    const QImage yellow = renderedFor(QColor(255, 255, 0));
    const QPoint bluePoint   = brightestPixel(blue);
    const QPoint yellowPoint = brightestPixel(yellow);
    QVERIFY(bluePoint.x() > blue.width() / 2);
    QVERIFY(qAbs(bluePoint.y() - blue.height() / 2) < 40);
    QVERIFY(yellowPoint.x() < yellow.width() / 2);
    QVERIFY(qAbs(yellowPoint.y() - yellow.height() / 2) < 40);
}

void ScopeRendererTest::testWaveformRenderingTracksStructure()
{
    // Solid half-image blocks: same histogram, mirrored horizontal layout.
    QImage leftDark(256, 8, QImage::Format_RGB888);
    QImage leftBright(256, 8, QImage::Format_RGB888);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 256; ++x) {
            const bool firstHalf = x < 128;
            leftDark.setPixelColor(x, y, firstHalf ? QColor(0, 0, 0)
                                                   : QColor(255, 255, 255));
            leftBright.setPixelColor(x, y, firstHalf ? QColor(255, 255, 255)
                                                     : QColor(0, 0, 0));
        }
    }

    const QImage dark  = renderWaveform(computeWaveform(leftDark)).image;
    const QImage light = renderWaveform(computeWaveform(leftBright)).image;

    // Only the trace is counted: the graticule and captions stay below 100
    // gray, while accumulated density reaches roughly 212, so the threshold
    // isolates the actual plotted pixels.
    const QRect plot(46, 24, 704, 208);
    const auto traceContent = [&plot](const QImage &image) {
        const int splitX = plot.left() + plot.width() / 2;
        const int splitY = plot.top() + plot.height() / 2;
        double topLeft = 0.0, topRight = 0.0, bottomLeft = 0.0, bottomRight = 0.0;
        for (int y = plot.top(); y < plot.bottom(); ++y) {
            for (int x = plot.left(); x < plot.right(); ++x) {
                const double above = qGray(image.pixel(x, y)) - 100.0;
                if (above <= 0.0) {
                    continue;
                }
                if (y < splitY) {
                    (x < splitX ? topLeft : topRight) += above;
                }
                else {
                    (x < splitX ? bottomLeft : bottomRight) += above;
                }
            }
        }
        return std::array<double, 4>{topLeft, topRight, bottomLeft, bottomRight};
    };

    const auto darkContent  = traceContent(dark);
    const auto lightContent = traceContent(light);

    const std::string numbers
        = "dark tl=" + std::to_string(darkContent[0])
          + " tr=" + std::to_string(darkContent[1])
          + " bl=" + std::to_string(darkContent[2])
          + " br=" + std::to_string(darkContent[3])
          + " | light tl=" + std::to_string(lightContent[0])
          + " tr=" + std::to_string(lightContent[1])
          + " bl=" + std::to_string(lightContent[2])
          + " br=" + std::to_string(lightContent[3]);

    // Trace must actually be present in the expected quadrants.
    QVERIFY2(darkContent[1] > 0.0 && darkContent[2] > 0.0, numbers.c_str());
    QVERIFY2(lightContent[0] > 0.0 && lightContent[3] > 0.0, numbers.c_str());

    // leftDark: luma 0 accumulates on the left (bottom), luma 255 on the right
    // (top). A horizontally stretched histogram could not do this.
    QVERIFY2(darkContent[0] < darkContent[1] * 0.25, numbers.c_str());
    QVERIFY2(darkContent[3] < darkContent[2] * 0.25, numbers.c_str());
    // leftBright is the mirror image.
    QVERIFY2(lightContent[1] < lightContent[0] * 0.25, numbers.c_str());
    QVERIFY2(lightContent[2] < lightContent[3] * 0.25, numbers.c_str());

    // The two renders are mirrors of each other.
    QVERIFY2(qAbs(darkContent[1] - lightContent[0])
                 < darkContent[1] * 0.15,
             numbers.c_str());
}

QTEST_MAIN(ScopeRendererTest)
#include "scoperenderer_test.moc"
