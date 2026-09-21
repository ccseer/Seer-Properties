#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

// Declaration order is the publish order: the scopes are appended to the result
// array in this sequence, and the Inspector renders them in that order. The
// enumerator values carry no meaning (the array position is what orders them).
enum class ImageScope { Histogram, Waveform, Vectorscope };

// Title of the subgroup that carries this plugin's whole result: the single key
// under `data` and the section title the Inspector shows. Tests and the README
// refer to the same name.
QString imageScopesGroupTitle();

QSet<ImageScope> allImageScopes();

// Internal deadline for the analysis. It stays below the manifest timeout_ms
// (30000) so an abandoned analysis can still publish a result before the host
// kills the helper and its process tree. 0 means the budget is already
// exhausted, so the analysis is abandoned at the first checkpoint (test hook);
// no value disables the deadline. The same convention is used by
// digital-signature-property.
constexpr unsigned kAnalysisDeadlineMs = 20000;

int runImageHistogram(const QStringList &arguments);
int executeImageHistogram(const QString &inputPath,
                          const QString &outputBasePath,
                          const QString &outputDirPath,
                          QSet<ImageScope> scopes = {},
                          unsigned analysisDeadlineMs = kAnalysisDeadlineMs);
