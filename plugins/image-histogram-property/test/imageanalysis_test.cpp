#include <QTest>
#include <QImage>
#include <QColor>
#include <cmath>

#include "imageanalysis.h"

using namespace imageanalysis;

class ImageAnalysisTest : public QObject {
    Q_OBJECT

private slots:
    void testNullImage();
    void testGrayRampSpatial();
    void testHorizontalGradient();
    void testVerticalGradient();
    void testVectorscopeNeutralCenter();
    void testVectorscopePrimaryColors();
    void testVectorscopeSecondaryColors();
    void testWaveformSamples();
    void testAnalysisImageAvoidsCopyForNativeFormats();
    void testPremultipliedInputIsUnpremultiplied();
    void testHighBitDepthInputIsNarrowed();
    void testWaveformTracksHorizontalStructure();
    void testVectorscopeKeepsFixedChromaExtent();
    void testAnalyzeScopesMatchesSingleScopeHelpers();
};

void ImageAnalysisTest::testNullImage()
{
    QImage img;
    const auto wave = computeWaveform(img);
    QCOMPARE(wave.samples, 0ULL);
    const auto vec = computeVectorscope(img);
    QCOMPARE(vec.samples, 0ULL);
}

void ImageAnalysisTest::testGrayRampSpatial()
{
    QImage img(256, 1, QImage::Format_RGB888);
    for (int x = 0; x < 256; ++x) {
        img.setPixelColor(x, 0, QColor(x, x, x));
    }
    const auto wave = computeWaveform(img);
    QCOMPARE(wave.samples, 256ULL);
    // For a left-to-right ramp, level==xBin column gets the sample.
    for (int i = 0; i < 256; ++i) {
        QCOMPARE(wave.at(i, i), 1ULL);
    }
}

void ImageAnalysisTest::testHorizontalGradient()
{
    // Horizontal structure changes luma along x: left dark, right bright.
    QImage img(64, 8, QImage::Format_RGB888);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int v = x * 255 / 63;
            img.setPixelColor(x, y, QColor(v, v, v));
        }
    }
    const auto wave = computeWaveform(img);
    QCOMPARE(wave.samples, 64ULL * 8);
    // The bright region (high x bins) must have high-luma samples; the dark
    // region (low x bins) must have low-luma samples.
    quint64 highLumaHighX = 0;
    for (int level = 200; level < 256; ++level) {
        for (int xb = 192; xb < 256; ++xb) {
            highLumaHighX += wave.at(level, xb);
        }
    }
    QVERIFY(highLumaHighX > 0);
}

void ImageAnalysisTest::testVerticalGradient()
{
    // Vertical gradient: luma only depends on y, not on x.
    QImage img(8, 64, QImage::Format_RGB888);
    for (int y = 0; y < 64; ++y) {
        const int v = y * 255 / 63;
        for (int x = 0; x < 8; ++x) {
            img.setPixelColor(x, y, QColor(v, v, v));
        }
    }
    const auto wave = computeWaveform(img);
    QCOMPARE(wave.samples, 64ULL * 8);
    // Every row spans all 8 columns, so each present luma level must appear
    // once per horizontal column (total of 8 samples across x bins).
    int presentLevels = 0;
    for (int level = 0; level < 256; ++level) {
        quint64 total = 0;
        for (int xb = 0; xb < 256; ++xb) {
            total += wave.at(level, xb);
        }
        if (total != 0) {
            ++presentLevels;
            QCOMPARE(total, 8ULL);
        }
    }
    // 64 rows of a vertical gradient hit ~64 distinct luma levels.
    QVERIFY(presentLevels >= 60 && presentLevels <= 65);
}

void ImageAnalysisTest::testVectorscopeNeutralCenter()
{
    QImage img(64, 64, QImage::Format_RGB888);
    img.fill(QColor(128, 128, 128));
    const auto vec = computeVectorscope(img);
    QCOMPARE(vec.samples, 64ULL * 64);
    // Neutral gray maps to Cb=Cr=0 => center of the grid.
    const int center = VectorscopeStats::kGrid / 2;
    QCOMPARE(vec.at(center, center), 64ULL * 64);
}

void ImageAnalysisTest::testVectorscopePrimaryColors()
{
    // Pure red: Cb negative (bluish-left), Cr strongly positive (up).
    // Pure blue: Cb strongly positive (right), Cr ~0 (neutral vertical).
    QImage img(1, 3, QImage::Format_RGB888);
    img.setPixelColor(0, 0, QColor(255, 0, 0));
    img.setPixelColor(0, 1, QColor(0, 255, 0));
    img.setPixelColor(0, 2, QColor(0, 0, 255));
    const auto vec = computeVectorscope(img);
    QCOMPARE(vec.samples, 3ULL);

    const int center = VectorscopeStats::kGrid / 2;
    // Red: (Cb<0, Cr>0)
    bool redFound = false;
    bool greenFound = false;
    bool blueFound = false;
    for (int cr = 0; cr < VectorscopeStats::kGrid; ++cr) {
        for (int cb = 0; cb < VectorscopeStats::kGrid; ++cb) {
            if (vec.at(cr, cb) == 0)
                continue;
            if (cb < center && cr > center)
                redFound = true;  // Red quadrant.
            if (cb < center && cr < center)
                greenFound = true;  // Green: both Cb and Cr negative.
            if (cb > center)
                blueFound = true;  // Blue: strong positive Cb.
        }
    }
    QVERIFY(redFound);
    QVERIFY(greenFound);
    QVERIFY(blueFound);
}

void ImageAnalysisTest::testVectorscopeSecondaryColors()
{
    // Cyan = (0,1,1): Cb positive, Cr negative.
    // Magenta = (1,0,1): Cb positive, Cr positive.
    // Yellow = (1,1,0): Cb negative, Cr ~0.
    QImage img(1, 3, QImage::Format_RGB888);
    img.setPixelColor(0, 0, QColor(0, 255, 255));
    img.setPixelColor(0, 1, QColor(255, 0, 255));
    img.setPixelColor(0, 2, QColor(255, 255, 0));
    const auto vec = computeVectorscope(img);
    QCOMPARE(vec.samples, 3ULL);

    const int center = VectorscopeStats::kGrid / 2;
    bool cyanFound = false;
    bool magentaFound = false;
    bool yellowFound = false;
    for (int cr = 0; cr < VectorscopeStats::kGrid; ++cr) {
        for (int cb = 0; cb < VectorscopeStats::kGrid; ++cb) {
            if (vec.at(cr, cb) == 0)
                continue;
            if (cb > center && cr < center)
                cyanFound = true;
            if (cb > center && cr > center)
                magentaFound = true;
            if (cb < center)
                yellowFound = true;
        }
    }
    QVERIFY(cyanFound);
    QVERIFY(magentaFound);
    QVERIFY(yellowFound);
}

void ImageAnalysisTest::testWaveformSamples()
{
    // 256-wide all-black image => luma 0 at every horizontal position.
    QImage img(256, 1, QImage::Format_RGB888);
    img.fill(QColor(0, 0, 0));
    const auto wave = computeWaveform(img);
    QCOMPARE(wave.samples, 256ULL);
    for (int xb = 0; xb < 256; ++xb) {
        QCOMPARE(wave.at(0, xb), 1ULL);
    }
}

// The two native 8-bit straight formats already satisfy the analysis
// convention, so normalization must not allocate another full-resolution image.
void ImageAnalysisTest::testAnalysisImageAvoidsCopyForNativeFormats()
{
    QImage argb(8, 8, QImage::Format_ARGB32);
    argb.fill(QColor(10, 20, 30, 200));
    const QImage argbAnalysis = toAnalysisImage(argb);
    QCOMPARE(argbAnalysis.format(), QImage::Format_ARGB32);
    QCOMPARE(argbAnalysis.constBits(), argb.constBits());

    QImage rgb32(8, 8, QImage::Format_RGB32);
    rgb32.fill(QColor(10, 20, 30));
    const QImage rgb32Analysis = toAnalysisImage(rgb32);
    QCOMPARE(rgb32Analysis.format(), QImage::Format_RGB32);
    QCOMPARE(rgb32Analysis.constBits(), rgb32.constBits());

    QVERIFY(toAnalysisImage(QImage()).isNull());

    QImage rgb888(8, 8, QImage::Format_RGB888);
    rgb888.fill(QColor(10, 20, 30));
    const QImage narrowed = toAnalysisImage(rgb888);
    QCOMPARE(narrowed.format(), QImage::Format_ARGB32);
    QCOMPARE(narrowed.pixelColor(0, 0), QColor(10, 20, 30));
}

// Premultiplied channel values must never be analyzed as if they were straight
// RGB: a 50% transparent white pixel still has luma of full white.
void ImageAnalysisTest::testPremultipliedInputIsUnpremultiplied()
{
    QImage img(1, 1, QImage::Format_ARGB32_Premultiplied);
    img.setPixelColor(0, 0, QColor(255, 255, 255, 128));

    const QImage analysis = toAnalysisImage(img);
    QCOMPARE(analysis.format(), QImage::Format_ARGB32);
    QVERIFY(analysis.pixelColor(0, 0).red() >= 254);
    QCOMPARE(analysis.pixelColor(0, 0).alpha(), 128);

    const auto wave = computeWaveform(img);
    QCOMPARE(wave.samples, 1ULL);
    // Full-white luma is 255; analyzing premultiplied 128 would give 128.
    QCOMPARE(wave.at(255, 0), 1ULL);

    const auto vec = computeVectorscope(img);
    QCOMPARE(vec.at(VectorscopeStats::kGrid / 2, VectorscopeStats::kGrid / 2),
             1ULL);
}

// Higher-bit-depth inputs are narrowed to 8 bits per channel explicitly.
void ImageAnalysisTest::testHighBitDepthInputIsNarrowed()
{
    QImage img(2, 1, QImage::Format_RGBA64);
    img.setPixelColor(0, 0, QColor(255, 128, 64, 255));
    img.setPixelColor(1, 0, QColor(0, 0, 0, 255));

    const QImage analysis = toAnalysisImage(img);
    QCOMPARE(analysis.format(), QImage::Format_ARGB32);
    QCOMPARE(analysis.width(), 2);

    const QColor first = analysis.pixelColor(0, 0);
    QCOMPARE(first.red(), 255);
    QVERIFY(qAbs(first.green() - 128) <= 1);
    QVERIFY(qAbs(first.blue() - 64) <= 1);
    QCOMPARE(analysis.pixelColor(1, 0), QColor(0, 0, 0, 255));

    const auto vec = computeVectorscope(img);
    QCOMPARE(vec.samples, 2ULL);
}

// Two images with the same histogram but mirrored spatial layout must produce
// different waveforms; a horizontally stretched histogram would not.
void ImageAnalysisTest::testWaveformTracksHorizontalStructure()
{
    QImage leftDark(64, 1, QImage::Format_RGB888);
    QImage leftBright(64, 1, QImage::Format_RGB888);
    for (int x = 0; x < 64; ++x) {
        const bool firstHalf = x < 32;
        leftDark.setPixelColor(x, 0,
                               firstHalf ? QColor(0, 0, 0)
                                         : QColor(255, 255, 255));
        leftBright.setPixelColor(x, 0,
                                 firstHalf ? QColor(255, 255, 255)
                                           : QColor(0, 0, 0));
    }

    const auto a = computeWaveform(leftDark);
    const auto b = computeWaveform(leftBright);
    QCOMPARE(a.samples, 64ULL);
    QCOMPARE(b.samples, 64ULL);

    // Aggregate over the two horizontal halves: the waveform must follow the
    // source layout, so the dark samples only appear on the left for the first
    // image and only on the right for the mirrored one.
    quint64 darkLeft = 0, darkRight = 0, brightLeft = 0, brightRight = 0;
    for (int xb = 0; xb < 128; ++xb) {
        darkLeft += a.at(0, xb);
        brightLeft += b.at(0, xb);
    }
    for (int xb = 128; xb < 256; ++xb) {
        darkRight += a.at(0, xb);
        brightRight += b.at(0, xb);
    }
    QCOMPARE(darkLeft, 32ULL);
    QCOMPARE(darkRight, 0ULL);
    QCOMPARE(brightLeft, 0ULL);
    QCOMPARE(brightRight, 32ULL);

    quint64 brightALeft = 0, brightARight = 0;
    for (int xb = 0; xb < 128; ++xb) {
        brightALeft += a.at(255, xb);
    }
    for (int xb = 128; xb < 256; ++xb) {
        brightARight += a.at(255, xb);
    }
    QCOMPARE(brightALeft, 0ULL);
    QCOMPARE(brightARight, 32ULL);
}

// The chroma extent is never rescaled to fill the plot: a low-saturation image
// keeps its samples close to the neutral center while a fully saturated one does
// not.
void ImageAnalysisTest::testVectorscopeKeepsFixedChromaExtent()
{
    const int center = VectorscopeStats::kGrid / 2;

    const auto distanceFromCenter = [&](const VectorscopeStats &stats) {
        int best = -1;
        for (int cr = 0; cr < VectorscopeStats::kGrid; ++cr) {
            for (int cb = 0; cb < VectorscopeStats::kGrid; ++cb) {
                if (stats.at(cr, cb) == 0)
                    continue;
                best = qMax(best, qMax(qAbs(cb - center), qAbs(cr - center)));
            }
        }
        return best;
    };

    QImage lowSat(8, 8, QImage::Format_RGB888);
    lowSat.fill(QColor(200, 190, 190));
    const auto low = computeVectorscope(lowSat);

    QImage highSat(8, 8, QImage::Format_RGB888);
    highSat.fill(QColor(255, 0, 0));
    const auto high = computeVectorscope(highSat);

    const int lowExtent  = distanceFromCenter(low);
    const int highExtent = distanceFromCenter(high);
    QVERIFY(lowExtent >= 0);
    QVERIFY(highExtent > lowExtent);
    QVERIFY(highExtent > center / 2);
}

void ImageAnalysisTest::testAnalyzeScopesMatchesSingleScopeHelpers()
{
    QImage img(32, 16, QImage::Format_RGB888);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 32; ++x) {
            img.setPixelColor(x, y, QColor(x * 8, y * 8, 128));
        }
    }

    const auto scopes = analyzeScopes(img);
    const auto wave   = computeWaveform(img);
    const auto vec    = computeVectorscope(img);

    QCOMPARE(scopes.waveform.samples, wave.samples);
    QCOMPARE(scopes.vectorscope.samples, vec.samples);
    QCOMPARE(scopes.waveform.density.size(), wave.density.size());
    QCOMPARE(scopes.vectorscope.density.size(), vec.density.size());
    for (std::size_t i = 0; i < wave.density.size(); ++i) {
        QCOMPARE(scopes.waveform.density[i], wave.density[i]);
    }
    for (std::size_t i = 0; i < vec.density.size(); ++i) {
        QCOMPARE(scopes.vectorscope.density[i], vec.density[i]);
    }
}

QTEST_MAIN(ImageAnalysisTest)
#include "imageanalysis_test.moc"
