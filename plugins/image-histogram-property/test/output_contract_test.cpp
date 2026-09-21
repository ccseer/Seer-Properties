#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "histogramhelper.h"

class OutputContractTest : public QObject {
    Q_OBJECT

private slots:
    void testOutputDirectoryCaseInsensitive();
    void testMissingInput();
    void testUnsupportedFormat();
    void testCorruptImage();
    void testOutputDirectoryFailure();
    void testValidImageAndExactJsonFields();
    void testRelativeAttachmentValue();
    void testResourceLimitRejection();
    void testRejectionOfWriteOutsideOutputDirectory();
    void testCommandLineParsing();
    void testCommandLineRejectsUnknownAndDuplicateOptions();
    void testDefaultProducesThreeScopes();
    void testScopesSelectorHistogramOnly();
    void testScopesSelectorRejectsUnknownAndDuplicates();
    void testEmptyScopesDefaultsToAll();
    void testThreePngsAreIndependentlyDecodable();
    void testAttachmentFailurePublishesNoResult();
    void testOnlyRequestedExtensionMatchesHostOutputDiscovery();
    void testAlphaImageProducesScopesAndAlphaStatistic();
    void testHighBitDepthInputIsAnalyzed();
    void testGrayRampStatistics();
    void testInternalDeadlinePublishesResult();
    void testPluginGroupHoldsEveryRowAndChart();

private:
    QString createValidImage(const QString &dirPath);
    QString createLargeDimensionBmp(const QString &dirPath);
    QJsonObject readEnvelope(const QString &jsonPath);
    QJsonObject readData(const QString &jsonPath);
};

// The whole `data` object. This plugin publishes exactly one key - its subgroup -
// so the helper is used to prove that nothing leaks to the section level.
QJsonObject OutputContractTest::readEnvelope(const QString &jsonPath)
{
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    return QJsonDocument::fromJson(file.readAll())
        .object()
        .value(QStringLiteral("data"))
        .toObject();
}

// Every flat row lives in the subgroup titled after the plugin, so row
// assertions read the group value.
// Every field of the subgroup - rows and charts alike - merged by key, for
// assertion reads that do not care about position.
QJsonObject OutputContractTest::readData(const QString &jsonPath)
{
    QJsonObject merged;
    const auto items = readEnvelope(jsonPath)
                           .value(imageScopesGroupTitle())
                           .toObject()
                           .value(QStringLiteral("value"))
                           .toArray();
    for (const auto &item : items) {
        const auto fields = item.toObject();
        for (auto field = fields.constBegin(); field != fields.constEnd();
             ++field) {
            merged.insert(field.key(), field.value());
        }
    }
    return merged;
}

QString OutputContractTest::createValidImage(const QString &dirPath)
{
    const QString filePath
        = QDir(dirPath).filePath(QStringLiteral("valid.png"));
    QImage img(2, 2, QImage::Format_RGB888);
    img.setPixelColor(0, 0, QColor(0, 0, 0));
    img.setPixelColor(1, 0, QColor(255, 255, 255));
    img.setPixelColor(0, 1, QColor(100, 150, 200));
    img.setPixelColor(1, 1, QColor(50, 50, 50));
    img.save(filePath, "PNG");
    return filePath;
}

QString OutputContractTest::createLargeDimensionBmp(const QString &dirPath)
{
    const QString filePath
        = QDir(dirPath).filePath(QStringLiteral("large.bmp"));
    QByteArray bmpData;
    // BMP Header (14 bytes)
    bmpData.append("BM");
    quint32 fileSize = 54;
    bmpData.append(reinterpret_cast<const char *>(&fileSize), 4);
    quint32 reserved = 0;
    bmpData.append(reinterpret_cast<const char *>(&reserved), 4);
    quint32 offset = 54;
    bmpData.append(reinterpret_cast<const char *>(&offset), 4);

    // DIB Header (BITMAPINFOHEADER: 40 bytes)
    quint32 headerSize = 40;
    bmpData.append(reinterpret_cast<const char *>(&headerSize), 4);
    qint32 width
        = 20000;  // 20,000 * 10,000 = 200,000,000 pixels > 100,000,000 limit
    qint32 height = 10000;
    bmpData.append(reinterpret_cast<const char *>(&width), 4);
    bmpData.append(reinterpret_cast<const char *>(&height), 4);
    quint16 planes = 1;
    bmpData.append(reinterpret_cast<const char *>(&planes), 2);
    quint16 bitCount = 24;
    bmpData.append(reinterpret_cast<const char *>(&bitCount), 2);
    quint32 compression = 0;
    bmpData.append(reinterpret_cast<const char *>(&compression), 4);
    quint32 imageSize = 0;
    bmpData.append(reinterpret_cast<const char *>(&imageSize), 4);
    qint32 xPels = 0;
    qint32 yPels = 0;
    bmpData.append(reinterpret_cast<const char *>(&xPels), 4);
    bmpData.append(reinterpret_cast<const char *>(&yPels), 4);
    quint32 clrUsed      = 0;
    quint32 clrImportant = 0;
    bmpData.append(reinterpret_cast<const char *>(&clrUsed), 4);
    bmpData.append(reinterpret_cast<const char *>(&clrImportant), 4);

    QFile file(filePath);
    file.open(QIODevice::WriteOnly);
    file.write(bmpData);
    file.close();
    return filePath;
}

void OutputContractTest::testOutputDirectoryCaseInsensitive()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const auto input = createValidImage(tempDir.path());
    const auto output = tempDir.filePath(QStringLiteral("result"));
    QCOMPARE(executeImageHistogram(input, output, tempDir.path().toUpper()), 0);
    QVERIFY(QFile::exists(output + QStringLiteral(".json")));
}

// A missing, unsupported or undecodable input is a domain outcome: exit 0 with
// a schema-1 result that states why, so the Inspector shows an explanation
// instead of nothing.
void OutputContractTest::testMissingInput()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input = tempDir.filePath(QStringLiteral("non_existent.png"));
    const QString outputBase = tempDir.filePath(QStringLiteral("output"));
    const int code = executeImageHistogram(input, outputBase, tempDir.path());
    QCOMPARE(code, 0);

    const auto data = readData(outputBase + QStringLiteral(".json"));
    QCOMPARE(data.value(QStringLiteral("Status")).toString(),
             QStringLiteral("ReadError"));
    QVERIFY(!data.value(QStringLiteral("Reason")).toString().isEmpty());
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
}

void OutputContractTest::testUnsupportedFormat()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input = tempDir.filePath(QStringLiteral("test.xyz"));
    QFile file(input);
    file.open(QIODevice::WriteOnly);
    file.write("sample text content");
    file.close();

    const QString outputBase = tempDir.filePath(QStringLiteral("output"));
    const int code = executeImageHistogram(input, outputBase, tempDir.path());
    QCOMPARE(code, 0);

    const auto data = readData(outputBase + QStringLiteral(".json"));
    QCOMPARE(data.value(QStringLiteral("Status")).toString(),
             QStringLiteral("UnsupportedOrMalformed"));
    QVERIFY(data.value(QStringLiteral("Reason")).toString().contains(
        QStringLiteral("xyz")));
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
}

void OutputContractTest::testCorruptImage()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input = tempDir.filePath(QStringLiteral("corrupt.png"));
    QFile file(input);
    file.open(QIODevice::WriteOnly);
    file.write("NOT_A_VALID_PNG_IMAGE_DATA_12345");
    file.close();

    const QString outputBase = tempDir.filePath(QStringLiteral("output"));
    const int code = executeImageHistogram(input, outputBase, tempDir.path());
    QCOMPARE(code, 0);

    const auto data = readData(outputBase + QStringLiteral(".json"));
    QCOMPARE(data.value(QStringLiteral("Status")).toString(),
             QStringLiteral("UnsupportedOrMalformed"));
    QVERIFY(!data.value(QStringLiteral("Reason")).toString().isEmpty());
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
}

void OutputContractTest::testOutputDirectoryFailure()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input = createValidImage(tempDir.path());
    const QString nonExistentDir
        = tempDir.filePath(QStringLiteral("does_not_exist"));
    const QString outputBase
        = QDir(nonExistentDir).filePath(QStringLiteral("output"));

    const int code = executeImageHistogram(input, outputBase, nonExistentDir);
    QCOMPARE(code, 1);
}

void OutputContractTest::testValidImageAndExactJsonFields()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("result"));

    const int code = executeImageHistogram(input, outputBase, tempDir.path());
    QCOMPARE(code, 0);

    const QString jsonPath = tempDir.filePath(QStringLiteral("result.json"));
    QVERIFY(QFile::exists(jsonPath));

    QFile jsonFile(jsonPath);
    QVERIFY(jsonFile.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(jsonFile.readAll());
    QVERIFY(doc.isObject());

    const auto root = doc.object();
    QCOMPARE(root.value(QStringLiteral("result_schema")).toInt(), 1);
    QVERIFY(root.value(QStringLiteral("data")).isObject());

    const auto data = readData(tempDir.filePath(QStringLiteral("result.json")));
    QCOMPARE(data.value(QStringLiteral("Width")).toInteger(), 2);
    QCOMPARE(data.value(QStringLiteral("Height")).toInteger(), 2);
    QCOMPARE(data.value(QStringLiteral("Pixels")).toInteger(), 4);
    QCOMPARE(data.value(QStringLiteral("Alpha Pixels")).toInteger(), 0);

    QVERIFY(data.contains(QStringLiteral("Mean R")));
    QVERIFY(data.contains(QStringLiteral("Mean G")));
    QVERIFY(data.contains(QStringLiteral("Mean B")));

    QCOMPARE(data.value(QStringLiteral("Clipped R")).toInteger(), 1);
    QCOMPARE(data.value(QStringLiteral("Clipped G")).toInteger(), 1);
    QCOMPARE(data.value(QStringLiteral("Clipped B")).toInteger(), 1);

    QCOMPARE(data.value(QStringLiteral("Shadow R")).toInteger(), 1);
    QCOMPARE(data.value(QStringLiteral("Shadow G")).toInteger(), 1);
    QCOMPARE(data.value(QStringLiteral("Shadow B")).toInteger(), 1);

    QCOMPARE(data.value(QStringLiteral("Histogram Y Scale")).toString(),
             QStringLiteral("log1p"));

    const auto histImg
        = data.value(QStringLiteral("RGB Histogram")).toObject();
    QCOMPARE(histImg.value(QStringLiteral("type")).toString(),
             QStringLiteral("image"));
    QCOMPARE(histImg.value(QStringLiteral("value")).toString(),
             QStringLiteral("rgb-histogram.png"));

    const QString attachmentPath
        = tempDir.filePath(QStringLiteral("rgb-histogram.png"));
    QVERIFY(QFile::exists(attachmentPath));
    QImageReader reader(attachmentPath);
    QVERIFY(reader.canRead());
    QCOMPARE(reader.size(), QSize(768, 256));
}

void OutputContractTest::testRelativeAttachmentValue()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("test_att"));

    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path()), 0);

    const auto data
        = readData(tempDir.filePath(QStringLiteral("test_att.json")));
    const auto histImg = data.value(QStringLiteral("RGB Histogram")).toObject();

    const QString attachmentVal
        = histImg.value(QStringLiteral("value")).toString();
    QCOMPARE(attachmentVal, QStringLiteral("rgb-histogram.png"));
    QVERIFY(!attachmentVal.contains(QLatin1Char('/')));
    QVERIFY(!attachmentVal.contains(QLatin1Char('\\')));
}

void OutputContractTest::testResourceLimitRejection()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString largeBmp   = createLargeDimensionBmp(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("large_out"));

    // Rejection is a domain outcome: the image is never decoded, but the reason
    // is published.
    const int code
        = executeImageHistogram(largeBmp, outputBase, tempDir.path());
    QCOMPARE(code, 0);

    const auto data
        = readData(tempDir.filePath(QStringLiteral("large_out.json")));
    QCOMPARE(data.value(QStringLiteral("Status")).toString(),
             QStringLiteral("QueryError"));
    QVERIFY(data.value(QStringLiteral("Reason")).toString().contains(
        QStringLiteral("budget")));
    QVERIFY(
        !QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
}

void OutputContractTest::testRejectionOfWriteOutsideOutputDirectory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString validImg = createValidImage(tempDir.path());

    const QString dirA = tempDir.filePath(QStringLiteral("dirA"));
    const QString dirB = tempDir.filePath(QStringLiteral("dirB"));
    QDir().mkpath(dirA);
    QDir().mkpath(dirB);

    // Target output inside dirB while output-dir is dirA
    const QString escapeOutput = QDir(dirB).filePath(QStringLiteral("escape"));
    const int codeEscape = executeImageHistogram(validImg, escapeOutput, dirA);
    QCOMPARE(codeEscape, 1);

    // Path traversal outside dirA
    const QString traversalOutput
        = QDir(dirA).filePath(QStringLiteral("../dirB/escape2"));
    const int codeTraversal
        = executeImageHistogram(validImg, traversalOutput, dirA);
    QCOMPARE(codeTraversal, 1);

    QVERIFY(!QFile::exists(QDir(dirB).filePath(QStringLiteral("escape.json"))));
    QVERIFY(
        !QFile::exists(QDir(dirB).filePath(QStringLiteral("escape2.json"))));
}

void OutputContractTest::testCommandLineParsing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString validImg   = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("cli_result"));

    const QStringList args = {QStringLiteral("image_histogram.exe"),
                              QStringLiteral("--input"),
                              validImg,
                              QStringLiteral("--output"),
                              outputBase,
                              QStringLiteral("--output-dir"),
                              tempDir.path()};

    const int code = runImageHistogram(args);
    QCOMPARE(code, 0);
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("cli_result.json"))));
    QVERIFY(
        QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
}

void OutputContractTest::testCommandLineRejectsUnknownAndDuplicateOptions()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString validImg = createValidImage(tempDir.path());
    const QString outputBase
        = tempDir.filePath(QStringLiteral("strict_result"));
    const QStringList prefix = {QStringLiteral("image_histogram.exe"),
                                QStringLiteral("--input"),
                                validImg,
                                QStringLiteral("--output"),
                                outputBase,
                                QStringLiteral("--output-dir"),
                                tempDir.path()};

    auto unknown = prefix;
    unknown.append({QStringLiteral("--unexpected"), QStringLiteral("value")});
    QCOMPARE(runImageHistogram(unknown), 1);

    auto duplicate = prefix;
    duplicate.insert(1, QStringLiteral("--input"));
    duplicate.insert(2, validImg);
    QCOMPARE(runImageHistogram(duplicate), 1);
}

void OutputContractTest::testDefaultProducesThreeScopes()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("result"));

    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path(), {}), 0);

    const auto data = readData(tempDir.filePath(QStringLiteral("result.json")));

    const auto histImg = data.value(QStringLiteral("RGB Histogram")).toObject();
    QCOMPARE(histImg.value(QStringLiteral("value")).toString(),
             QStringLiteral("rgb-histogram.png"));
    const auto waveImg = data.value(QStringLiteral("Waveform")).toObject();
    QCOMPARE(waveImg.value(QStringLiteral("value")).toString(),
             QStringLiteral("waveform.png"));
    const auto vecImg = data.value(QStringLiteral("Vectorscope")).toObject();
    QCOMPARE(vecImg.value(QStringLiteral("value")).toString(),
             QStringLiteral("vectorscope.png"));

    const QString pngPath = tempDir.path();
    QImageReader histReader(tempDir.filePath(QStringLiteral("rgb-histogram.png")));
    QVERIFY(histReader.canRead());
    QImageReader waveReader(tempDir.filePath(QStringLiteral("waveform.png")));
    QVERIFY(waveReader.canRead());
    QImageReader vecReader(tempDir.filePath(QStringLiteral("vectorscope.png")));
    QVERIFY(vecReader.canRead());
    (void)pngPath;
}

void OutputContractTest::testScopesSelectorHistogramOnly()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("hist_only"));

    const QSet<ImageScope> scopes{ImageScope::Histogram};
    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path(), scopes),
             0);

    const QString jsonPath = tempDir.filePath(QStringLiteral("hist_only.json"));
    QVERIFY(QFile::exists(jsonPath));
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("waveform.png"))));
    QVERIFY(
        !QFile::exists(tempDir.filePath(QStringLiteral("vectorscope.png"))));

    const auto data = readData(jsonPath);
    QVERIFY(data.contains(QStringLiteral("RGB Histogram")));
    QVERIFY(!data.contains(QStringLiteral("Waveform")));
    QVERIFY(!data.contains(QStringLiteral("Vectorscope")));
}

void OutputContractTest::testScopesSelectorRejectsUnknownAndDuplicates()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("sel_result"));

    const QStringList prefix = {QStringLiteral("image_histogram.exe"),
                                QStringLiteral("--input"),
                                input,
                                QStringLiteral("--output"),
                                outputBase,
                                QStringLiteral("--output-dir"),
                                tempDir.path()};

    auto unknown = prefix;
    unknown.append({QStringLiteral("--scopes"),
                    QStringLiteral("histogram,chroma")});
    QCOMPARE(runImageHistogram(unknown), 1);

    auto duplicate = prefix;
    duplicate.append({QStringLiteral("--scopes"),
                      QStringLiteral("waveform,waveform")});
    QCOMPARE(runImageHistogram(duplicate), 1);

    auto empty = prefix;
    empty.append({QStringLiteral("--scopes"), QStringLiteral("")});
    QCOMPARE(runImageHistogram(empty), 1);
}

void OutputContractTest::testEmptyScopesDefaultsToAll()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("empty_out"));
    // An empty scopes set means "all scopes" (default), not zero scopes.
    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path(), {}), 0);
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("empty_out.json"))));
    QVERIFY(
        QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("waveform.png"))));
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("vectorscope.png"))));
}

void OutputContractTest::testThreePngsAreIndependentlyDecodable()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("three"));

    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path(), {}), 0);

    // Three distinct, independently decodable PNGs plus exactly one JSON.
    QImageReader histReader(tempDir.filePath(QStringLiteral("rgb-histogram.png")));
    QVERIFY(histReader.canRead());
    QImageReader waveReader(tempDir.filePath(QStringLiteral("waveform.png")));
    QVERIFY(waveReader.canRead());
    QImageReader vecReader(tempDir.filePath(QStringLiteral("vectorscope.png")));
    QVERIFY(vecReader.canRead());
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("three.json"))));

    // The three images must differ visibly.
    const QImage hist = histReader.read();
    const QImage wave = waveReader.read();
    const QImage vec  = vecReader.read();
    QVERIFY(!hist.isNull());
    QVERIFY(!wave.isNull());
    QVERIFY(!vec.isNull());
    QVERIFY(hist != wave);
    QVERIFY(hist != vec);
    QVERIFY(wave != vec);
}

// An analysis that cannot finish inside the internal deadline publishes a
// bounded-timeout result instead of being killed by the host with nothing
// published.
void OutputContractTest::testInternalDeadlinePublishesResult()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const auto input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("deadline"));

    // A zero deadline expires at the first checkpoint, after the decode.
    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path(), {}, 0), 0);

    const auto data = readData(outputBase + QStringLiteral(".json"));
    QCOMPARE(data.value(QStringLiteral("Status")).toString(),
             QStringLiteral("QueryError"));
    QVERIFY(data.value(QStringLiteral("Reason")).toString().contains(
        QStringLiteral("deadline")));
    QVERIFY(
        !QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
}

// A failure while publishing any attachment must not leave a complete-looking
// result behind: the helper exits nonzero and writes no result JSON, so the
// host cleans the request directory instead of showing partial charts.
void OutputContractTest::testAttachmentFailurePublishesNoResult()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("blocked"));

    // A directory cannot be replaced by a file, so QSaveFile::commit() fails for
    // the waveform attachment while the histogram attachment succeeds first.
    const QString wavePath = tempDir.filePath(QStringLiteral("waveform.png"));
    QVERIFY(QDir().mkpath(wavePath));

    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path(), {}), 1);
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("blocked.json"))));

    // The same guard applies to the very first attachment.
    QVERIFY(QDir().rmdir(wavePath));
    const QString histPath
        = tempDir.filePath(QStringLiteral("rgb-histogram.png"));
    QVERIFY(QFile::remove(histPath));
    QVERIFY(QDir().mkpath(histPath));
    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path(), {}), 1);
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("blocked.json"))));
}

// The host discovers the result through "<output-base>.*" in the request
// directory, so the only file matching that pattern must be the single result
// JSON. Distinct attachment names never compete with it.
void OutputContractTest::testOnlyRequestedExtensionMatchesHostOutputDiscovery()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("abc123"));

    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path(), {}), 0);

    const QStringList discovered = QDir(tempDir.path()).entryList(
        QStringList{QStringLiteral("abc123.*")}, QDir::Files, QDir::Time);
    QCOMPARE(discovered, QStringList{QStringLiteral("abc123.json")});

    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("rgb-histogram.png"))));
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("waveform.png"))));
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("vectorscope.png"))));
}

void OutputContractTest::testAlphaImageProducesScopesAndAlphaStatistic()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input = tempDir.filePath(QStringLiteral("alpha.png"));
    QImage img(4, 4, QImage::Format_ARGB32);
    img.fill(QColor(0, 0, 0, 0));
    img.setPixelColor(0, 0, QColor(255, 0, 0, 128));
    QVERIFY(img.save(input, "PNG"));

    const QString outputBase = tempDir.filePath(QStringLiteral("alpha_out"));
    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path()), 0);

    QFile jsonFile(tempDir.filePath(QStringLiteral("alpha_out.json")));
    QVERIFY(jsonFile.open(QIODevice::ReadOnly));
    const auto data = readData(tempDir.filePath(QStringLiteral("alpha_out.json")));

    QCOMPARE(data.value(QStringLiteral("Pixels")).toInteger(), 16);
    QVERIFY(data.value(QStringLiteral("Alpha Pixels")).toInteger() > 0);
    // Transparent pixels keep contributing their straight RGB values, exactly
    // like the histogram statistics have always treated them.
    QVERIFY(data.value(QStringLiteral("Shadow R")).toInteger() < 16);
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("waveform.png"))));
    QVERIFY(QFile::exists(tempDir.filePath(QStringLiteral("vectorscope.png"))));
}

void OutputContractTest::testHighBitDepthInputIsAnalyzed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input = tempDir.filePath(QStringLiteral("deep.png"));
    QImage img(4, 4, QImage::Format_RGBA64);
    img.fill(QColor(64, 128, 192, 255));
    if (!img.save(input, "PNG")) {
        QSKIP("Qt PNG writer does not support 16-bit images here");
    }

    const QString outputBase = tempDir.filePath(QStringLiteral("deep_out"));
    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path()), 0);

    QFile jsonFile(tempDir.filePath(QStringLiteral("deep_out.json")));
    QVERIFY(jsonFile.open(QIODevice::ReadOnly));
    const auto data = readData(tempDir.filePath(QStringLiteral("deep_out.json")));

    QCOMPARE(data.value(QStringLiteral("Pixels")).toInteger(), 16);
    QVERIFY(data.value(QStringLiteral("Mean R")).toDouble() > 60.0);
    QVERIFY(data.value(QStringLiteral("Mean R")).toDouble() < 68.0);
    QVERIFY(data.value(QStringLiteral("Mean B")).toDouble() > 188.0);
    QVERIFY(data.value(QStringLiteral("Mean B")).toDouble() < 196.0);
}

void OutputContractTest::testGrayRampStatistics()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input = tempDir.filePath(QStringLiteral("ramp.png"));
    QImage img(256, 1, QImage::Format_RGB888);
    for (int x = 0; x < 256; ++x) {
        img.setPixelColor(x, 0, QColor(x, x, x));
    }
    QVERIFY(img.save(input, "PNG"));

    const QString outputBase = tempDir.filePath(QStringLiteral("ramp_out"));
    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path()), 0);

    QFile jsonFile(tempDir.filePath(QStringLiteral("ramp_out.json")));
    QVERIFY(jsonFile.open(QIODevice::ReadOnly));
    const auto data = readData(tempDir.filePath(QStringLiteral("ramp_out.json")));

    QCOMPARE(data.value(QStringLiteral("Width")).toInteger(), 256);
    QCOMPARE(data.value(QStringLiteral("Height")).toInteger(), 1);
    QCOMPARE(data.value(QStringLiteral("Pixels")).toInteger(), 256);
    QCOMPARE(data.value(QStringLiteral("Mean R")).toDouble(), 127.5);
    QCOMPARE(data.value(QStringLiteral("Mean G")).toDouble(), 127.5);
    QCOMPARE(data.value(QStringLiteral("Mean B")).toDouble(), 127.5);
    QCOMPARE(data.value(QStringLiteral("Shadow R")).toInteger(), 1);
    QCOMPARE(data.value(QStringLiteral("Clipped R")).toInteger(), 1);
}

// The plugin's whole result - statistics rows and the three charts alike - lives
// in the single subgroup titled after the plugin, and nothing leaks to the top
// level of `data`.
void OutputContractTest::testPluginGroupHoldsEveryRowAndChart()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString input      = createValidImage(tempDir.path());
    const QString outputBase = tempDir.filePath(QStringLiteral("group"));

    QCOMPARE(executeImageHistogram(input, outputBase, tempDir.path()), 0);

    const QString jsonPath = tempDir.filePath(QStringLiteral("group.json"));
    const auto envelope    = readEnvelope(jsonPath);
    QVERIFY(envelope.contains(imageScopesGroupTitle()));
    // `data` carries exactly one key: the plugin's own subgroup.
    QVERIFY(envelope.size() == 1);

    // The subgroup value is an array of one-key fields in the plugin's own
    // order - statistics, then the three charts. That sequence is the contract.
    const auto sequence
        = envelope.value(imageScopesGroupTitle())
              .toObject()
              .value(QStringLiteral("value"))
              .toArray();
    QStringList keys;
    for (const auto &item : sequence) {
        keys.append(item.toObject().keys());
    }
    QCOMPARE(keys.join(QLatin1Char(',')),
             QStringLiteral("Width,Height,Pixels,Mean R,Mean G,Mean B,"
                            "Alpha Pixels,Clipped R,Clipped G,Clipped B,"
                            "Shadow R,Shadow G,Shadow B,Histogram Y Scale,"
                            "RGB Histogram,Waveform,Vectorscope,"
                            "Image Scopes Analysis Convention"));

    const auto fields = readData(jsonPath);
    QVERIFY(fields.contains(QStringLiteral("Width")));
    QVERIFY(fields.contains(QStringLiteral("Pixels")));
    QVERIFY(fields.contains(QStringLiteral("Histogram Y Scale")));
    QVERIFY(fields.contains(QStringLiteral("Image Scopes Analysis Convention")));

    QCOMPARE(fields.value(QStringLiteral("RGB Histogram"))
                 .toObject()
                 .value(QStringLiteral("type"))
                 .toString(),
             QStringLiteral("image"));
    QCOMPARE(fields.value(QStringLiteral("Waveform"))
                 .toObject()
                 .value(QStringLiteral("type"))
                 .toString(),
             QStringLiteral("image"));
    QCOMPARE(fields.value(QStringLiteral("Vectorscope"))
                 .toObject()
                 .value(QStringLiteral("type"))
                 .toString(),
             QStringLiteral("image"));
}

QTEST_MAIN(OutputContractTest)
#include "output_contract_test.moc"
