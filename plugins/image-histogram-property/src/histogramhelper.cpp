#include "histogramhelper.h"

#include <qt_windows.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <cmath>

#include "histogram.h"
#include "histogramrenderer.h"
#include "imageanalysis.h"
#include "scoperenderer.h"

namespace {

QSet<ImageScope> kAllScopes()
{
    return {ImageScope::Histogram, ImageScope::Waveform, ImageScope::Vectorscope};
}

QSet<ImageScope> resolveScopes(QSet<ImageScope> scopes)
{
    return scopes.isEmpty() ? kAllScopes() : scopes;
}

// QDir::canonicalPath() does not traverse directory junctions on Windows, so a
// reparse point created inside the request directory could satisfy a purely
// lexical containment check. Probe the components below the request directory
// and reject reparse points outright. Ancestors of the request directory are
// deliberately not probed: they are chosen by the host, and rejecting them
// would fail the whole property result on hosts whose temp root is redirected.
bool isReparsePoint(const QString &absolute)
{
    const QString native = QDir::toNativeSeparators(absolute);
    const DWORD attrs    = GetFileAttributesW(
        reinterpret_cast<const wchar_t *>(native.utf16()));
    return attrs != INVALID_FILE_ATTRIBUTES
           && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool hasReparsePointBelow(const QString &canonicalDir,
                          const QString &canonicalFile)
{
    const QString relative
        = QDir(canonicalDir).relativeFilePath(canonicalFile);
    if (relative.isEmpty() || relative.startsWith(QStringLiteral(".."))) {
        return true;
    }
    QString accumulated = canonicalDir;
    const auto segments
        = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const auto &segment : segments) {
        accumulated += QLatin1Char('/') + segment;
        if (isReparsePoint(accumulated)) {
            return true;
        }
    }
    return false;
}

bool isContainedInDirectory(const QString &filePath, const QString &dirPath)
{
    const QDir dir(dirPath);
    if (!dir.exists()) {
        return false;
    }
    const QString cleanDir = QDir::cleanPath(dir.canonicalPath());
    if (cleanDir.isEmpty()) {
        return false;
    }
    // QDir::cleanPath keeps the trailing slash of a drive root ("C:/"), so the
    // child prefix must not append another one: "C:" + "/" would become "C://"
    // and every subdirectory check against a drive root would fail.
    const QString dirPrefix
        = cleanDir.endsWith(QLatin1Char('/')) ? cleanDir
                                              : cleanDir + QLatin1Char('/');

    const QFileInfo fileInfo(filePath);
    const QDir fileParentDir = fileInfo.dir();
    if (!fileParentDir.exists()) {
        return false;
    }
    const QString cleanParent = QDir::cleanPath(fileParentDir.canonicalPath());
    if (cleanParent.isEmpty()) {
        return false;
    }

    if (cleanParent.compare(cleanDir, Qt::CaseInsensitive) == 0) {
        return !hasReparsePointBelow(cleanDir, QDir::cleanPath(fileInfo.absoluteFilePath()));
    }
    if (cleanParent.startsWith(dirPrefix,
                               Qt::CaseInsensitive)) {
        return !hasReparsePointBelow(cleanDir, QDir::cleanPath(fileInfo.absoluteFilePath()));
    }
    return false;
}

bool writeAttachment(const QString &path, const QByteArray &pngData)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(pngData) != pngData.size()) {
        return false;
    }
    return file.commit();
}

bool writeJson(const QString &path, const QJsonObject &obj)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const auto bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        return false;
    }
    return file.commit();
}

QJsonObject imageField(const QString &value)
{
    QJsonObject obj;
    obj[QStringLiteral("type")]  = QStringLiteral("image");
    obj[QStringLiteral("value")] = value;
    return obj;
}

// The subgroup value is an array of one-key fields, so the array order - the
// order the fields are appended in - is the order the Inspector renders. A plain
// scalar field is a text row; a `{"type":"image"}` field is a chart.
void appendField(QJsonArray &fields, const QString &name,
                 const QJsonValue &value)
{
    fields.append(QJsonObject{{name, value}});
}

// Integral overload: without it, an int argument would be ambiguous between the
// QJsonValue conversions (qint64 vs double) at every statistics call site.
void appendStat(QJsonArray &fields, const QString &name, qint64 value)
{
    fields.append(QJsonObject{{name, value}});
}

// The published `data` object for one analysis. Every field - flat statistics and
// the three typed chart fields alike - lives in the subgroup titled after the
// plugin, so the Inspector shows the plugin's whole result in one section. The
// host resolves `{"type":"image"}` attachments inside a subgroup as well.
QJsonObject groupedData(const QJsonArray &fields)
{
    QJsonObject data;
    data[imageScopesGroupTitle()]
        = QJsonObject{{QStringLiteral("value"), fields}};
    return data;
}

// Text rows are published as plain strings: the host recognises only
// `type: "image"` as a realised typed value and degrades any other type to a
// text row after a warning, so a typed text row would only add noise.

}  // namespace

QSet<ImageScope> allImageScopes()
{
    return kAllScopes();
}

QString imageScopesGroupTitle()
{
    return QStringLiteral("Image Scopes");
}

namespace {

// One analysis attempt. A domain outcome publishes a schema-1 result with exit
// 0; an invocation or publication failure publishes nothing and exits nonzero.
struct HistogramOutcome {
    int exitCode = 1;
    QJsonObject data;
};

HistogramOutcome domainOutcome(const QString &status, const QString &reason)
{
    QJsonArray fields;
    appendField(fields, QStringLiteral("Status"), status);
    appendField(fields, QStringLiteral("Reason"), reason);
    HistogramOutcome outcome;
    outcome.exitCode = 0;
    outcome.data     = groupedData(fields);
    return outcome;
}

// Every step that can be slow. deadlineMs bounds the whole analysis so a result
// is still published before the host reaches the manifest timeout. The checks
// sit between the steps: Qt offers no way to interrupt a decoder, so a step
// that overruns is abandoned rather than stopped.
HistogramOutcome analyzeImage(const QString &inputPath, const QDir &outDir,
                              const QSet<ImageScope> &scopes,
                              unsigned deadlineMs)
{
    QElapsedTimer timer;
    timer.start();
    // 0 is the shared "budget already exhausted" test hook, not a disabled
    // deadline: the first checkpoint then reports the analysis as abandoned.
    const auto overBudget = [&timer, deadlineMs]() {
        return deadlineMs == 0
               || static_cast<quint64>(timer.elapsed()) >= deadlineMs;
    };
    const auto deadlineOutcome = [deadlineMs]() {
        return domainOutcome(
            QStringLiteral("QueryError"),
            QStringLiteral("the analysis exceeded the internal deadline (%1 ms) "
                           "and was abandoned; no statistics are reported")
                .arg(deadlineMs));
    };

    const QFileInfo inputInfo(inputPath);
    if (!inputInfo.exists() || !inputInfo.isFile()) {
        return domainOutcome(QStringLiteral("ReadError"),
                             QStringLiteral("the input does not exist or is not "
                                            "a regular file"));
    }

    const QString suffix = inputInfo.suffix().toLower();
    static const QSet<QString> kSupportedExts = {
        QStringLiteral("png"), QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("tif"),
        QStringLiteral("tiff")};
    if (!kSupportedExts.contains(suffix)) {
        return domainOutcome(
            QStringLiteral("UnsupportedOrMalformed"),
            QStringLiteral("'%1' is not a supported image extension").arg(
                suffix));
    }

    QImageReader reader(inputPath);
    if (!reader.canRead()) {
        return domainOutcome(QStringLiteral("UnsupportedOrMalformed"),
                             QStringLiteral("the file cannot be decoded as an "
                                            "image"));
    }

    const QSize size = reader.size();
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return domainOutcome(QStringLiteral("UnsupportedOrMalformed"),
                             QStringLiteral("the image reports no usable "
                                            "dimensions"));
    }

    const quint64 w      = static_cast<quint64>(size.width());
    const quint64 h      = static_cast<quint64>(size.height());
    const quint64 pixels = w * h;
    if (pixels > 100000000ULL) {
        return domainOutcome(
            QStringLiteral("QueryError"),
            QStringLiteral("the image has %1 pixels, which exceeds the 100 "
                           "million pixel budget")
                .arg(pixels));
    }
    const quint64 estimatedBytes = pixels * 4ULL;
    if (estimatedBytes > 536870912ULL) {
        return domainOutcome(
            QStringLiteral("QueryError"),
            QStringLiteral("the decoded image would need about %1 MiB, which "
                           "exceeds the 512 MiB budget")
                .arg(estimatedBytes / (1024ULL * 1024ULL)));
    }

    // Decode once. The histogram keeps operating on the decoded image exactly as
    // before, and the two scope analyses share a single normalized 8-bit
    // straight-RGB buffer produced by analyzeScopes().
    QImage image;
    if (!reader.read(&image) || image.isNull()) {
        return domainOutcome(QStringLiteral("UnsupportedOrMalformed"),
                             QStringLiteral("the file could not be decoded "
                                            "into an image"));
    }
    if (overBudget()) {
        return deadlineOutcome();
    }

    const HistogramStats stats = computeHistogram(image);

    const bool hasHistogram   = scopes.contains(ImageScope::Histogram);
    const bool hasWaveform    = scopes.contains(ImageScope::Waveform);
    const bool hasVectorscope = scopes.contains(ImageScope::Vectorscope);

    // Only normalized when a scope actually needs it.
    const imageanalysis::ScopeStats scopeStats
        = (hasWaveform || hasVectorscope) ? imageanalysis::analyzeScopes(image)
                                          : imageanalysis::ScopeStats{};

    // Everything this plugin reports, published in one subgroup in the order it
    // should be rendered: the statistics fields, then the three typed charts.
    QJsonArray fields;
    appendStat(fields, QStringLiteral("Width"), image.width());
    appendStat(fields, QStringLiteral("Height"), image.height());
    appendStat(fields, QStringLiteral("Pixels"), stats.pixels);
    appendField(fields, QStringLiteral("Mean R"), std::round(stats.meanR * 10.0) / 10.0);
    appendField(fields, QStringLiteral("Mean G"), std::round(stats.meanG * 10.0) / 10.0);
    appendField(fields, QStringLiteral("Mean B"), std::round(stats.meanB * 10.0) / 10.0);
    appendStat(fields, QStringLiteral("Alpha Pixels"), stats.alphaPixels);
    appendStat(fields, QStringLiteral("Clipped R"), stats.clippedR);
    appendStat(fields, QStringLiteral("Clipped G"), stats.clippedG);
    appendStat(fields, QStringLiteral("Clipped B"), stats.clippedB);
    appendStat(fields, QStringLiteral("Shadow R"), stats.shadowR);
    appendStat(fields, QStringLiteral("Shadow G"), stats.shadowG);
    appendStat(fields, QStringLiteral("Shadow B"), stats.shadowB);

    if (overBudget()) {
        return deadlineOutcome();
    }

    // Publish attachments first (inside the request directory), then the single
    // result JSON last. No attachment ever appears next to the inspected file.
    if (hasHistogram) {
        const auto rendered = renderHistogram(stats);
        // The published scale comes from the renderer, so the row cannot drift
        // from what the chart actually draws.
        appendField(fields, QStringLiteral("Histogram Y Scale"),
                    rendered.yAxisScale);
        if (!writeAttachment(outDir.filePath(QStringLiteral("rgb-histogram.png")),
                             rendered.pngData)) {
            return HistogramOutcome{};
        }
        appendField(fields, QStringLiteral("RGB Histogram"),
                    imageField(QStringLiteral("rgb-histogram.png")));
    }

    if (hasWaveform) {
        if (overBudget()) {
            return deadlineOutcome();
        }
        const auto wave = renderWaveform(scopeStats.waveform);
        if (!writeAttachment(outDir.filePath(QStringLiteral("waveform.png")),
                             wave.pngData)) {
            return HistogramOutcome{};
        }
        appendField(fields, QStringLiteral("Waveform"),
                    imageField(QStringLiteral("waveform.png")));
    }

    if (hasVectorscope) {
        if (overBudget()) {
            return deadlineOutcome();
        }
        const auto vec = renderVectorscope(scopeStats.vectorscope);
        if (!writeAttachment(outDir.filePath(QStringLiteral("vectorscope.png")),
                             vec.pngData)) {
            return HistogramOutcome{};
        }
        appendField(fields, QStringLiteral("Vectorscope"),
                    imageField(QStringLiteral("vectorscope.png")));
    }

    // The plugin-qualified name keeps this field's identity stable.
    appendField(fields, QStringLiteral("Image Scopes Analysis Convention"),
                imageanalysis::analysisDescription());

    HistogramOutcome outcome;
    outcome.exitCode = 0;
    outcome.data     = groupedData(fields);
    return outcome;
}

}  // namespace

int executeImageHistogram(const QString &inputPath,
                          const QString &outputBasePath,
                          const QString &outputDirPath,
                          QSet<ImageScope> scopes,
                          unsigned analysisDeadlineMs)
{
    scopes = resolveScopes(scopes);

    const QDir outDir(outputDirPath);
    if (!outDir.exists()) {
        return 1;
    }

    QString outputJsonPath = outputBasePath;
    if (!outputJsonPath.endsWith(QStringLiteral(".json"),
                                 Qt::CaseInsensitive)) {
        outputJsonPath += QStringLiteral(".json");
    }

    if (!isContainedInDirectory(outputJsonPath, outputDirPath)) {
        return 1;
    }

    const HistogramOutcome outcome
        = analyzeImage(inputPath, outDir, scopes, analysisDeadlineMs);
    if (outcome.exitCode != 0) {
        return outcome.exitCode;
    }

    QJsonObject rootObj;
    rootObj[QStringLiteral("result_schema")] = 1;
    rootObj[QStringLiteral("data")]          = outcome.data;

    return writeJson(outputJsonPath, rootObj) ? 0 : 1;
}

int runImageHistogram(const QStringList &arguments)
{
    QSet<ImageScope> scopes = allImageScopes();

    // Parsing rule: --input/--output/--output-dir consume the next token.
    // --scopes consumes a comma-separated list. Default is all three scopes.
    QString input;
    QString output;
    QString outputDir;
    bool inputSeen     = false;
    bool outputSeen    = false;
    bool outputDirSeen = false;
    bool scopesSeen    = false;

    for (int i = 1; i < arguments.size();) {
        const auto &option = arguments[i];
        if (option == QStringLiteral("--scopes")) {
            if (scopesSeen || i + 1 >= arguments.size()) {
                return 1;
            }
            const QString value = arguments[i + 1];
            if (value.isEmpty()) {
                return 1;
            }
            const QStringList items
                = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
            QSet<ImageScope> parsed;
            for (const auto &item : items) {
                const QString name = item.trimmed().toLower();
                if (name == QStringLiteral("histogram")) {
                    parsed.insert(ImageScope::Histogram);
                }
                else if (name == QStringLiteral("waveform")) {
                    parsed.insert(ImageScope::Waveform);
                }
                else if (name == QStringLiteral("vectorscope")) {
                    parsed.insert(ImageScope::Vectorscope);
                }
                else {
                    return 1;  // Unknown scope name.
                }
            }
            if (parsed.isEmpty() || parsed.size() != items.size()) {
                return 1;  // Empty selection or duplicate entries.
            }
            scopes    = parsed;
            scopesSeen = true;
            i += 2;
            continue;
        }

        if (i + 1 >= arguments.size()) {
            return 1;
        }
        const auto &value = arguments[i + 1];
        if (value.isEmpty()) {
            return 1;
        }
        if (option == QStringLiteral("--input") && !inputSeen) {
            input     = value;
            inputSeen = true;
        }
        else if (option == QStringLiteral("--output") && !outputSeen) {
            output     = value;
            outputSeen = true;
        }
        else if (option == QStringLiteral("--output-dir") && !outputDirSeen) {
            outputDir     = value;
            outputDirSeen = true;
        }
        else {
            return 1;
        }
        i += 2;
    }

    if (!inputSeen || !outputSeen || !outputDirSeen) {
        return 1;
    }

    return executeImageHistogram(input, output, outputDir, scopes);
}
