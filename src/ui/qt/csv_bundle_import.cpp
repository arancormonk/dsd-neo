// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QtGlobal>
#include <algorithm>
#include <functional>
#include <iterator>
#include "csv_bundle_import.h"
#include "decoder_host.h"
#include "json_store.h"

namespace dsd_qt {
namespace {
QString
unquote(QString text) {
    text = text.trimmed();
    if (text.size() >= 2
        && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\''))) {
        text = text.mid(1, text.size() - 2);
    }
    return text;
}

QString
rewriteOptions(QString options, const std::function<QString(const QString&)>& resolve) {
    options = unquote(options);
    static const QRegularExpression paths(
        QStringLiteral("(?:^|\\s)(?:-K|-k|-G|--dmr-tg-key-csv)(?:=|\\s+)(?:\"([^\"]+)\"|'([^']+)'|(\\S+))"));
    auto matches = paths.globalMatch(options);

    struct Edit {
        qsizetype at;
        qsizetype length;
        QString value;
    };

    QList<Edit> edits;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const int group = match.capturedStart(1) >= 0 ? 1 : match.capturedStart(2) >= 0 ? 2 : 3;
        const QString original = match.captured(group);
        edits.append({match.capturedStart(group), match.capturedLength(group), resolve(original)});
    }
    for (auto i = edits.crbegin(); i != edits.crend(); ++i) {
        options.replace(i->at, i->length, i->value);
    }
    return options;
}

QString
rewriteMap(const QString& text, const std::function<QString(const QString&)>& resolve) {
    auto lines = text.split('\n');
    if (lines.isEmpty()) {
        return text;
    }
    auto headers = lines[0].split(',');
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](const QString& header) { return unquote(header).toLower(); });
    for (int row = 1; row < lines.size(); ++row) {
        if (lines[row].trimmed().isEmpty()) {
            continue;
        }
        auto cells = lines[row].split(',');
        for (int column = 0; column < cells.size() && column < headers.size(); ++column) {
            const QString& header = headers[column];
            if (header == "keys_hex_csv" || header == "keys_dec_csv") {
                const QString path = unquote(cells[column]);
                if (!path.isEmpty()) {
                    cells[column] = resolve(path);
                }
            } else if (header == "options" || header == "relevant_cli_switches") {
                cells[column] = rewriteOptions(cells[column], resolve);
            }
        }
        lines[row] = cells.join(',');
    }
    return lines.join('\n');
}

bool
writePrivate(const QString& path, const QByteArray& data) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
           && file.write(data) == data.size() && file.commit();
}

bool
ownedRoot(const QString& root) {
    const QString parent = QDir::cleanPath(json_store_path("imports/bundles"));
    return !QFileInfo(root).isSymLink() && QFileInfo(root).absolutePath() == parent
           && QRegularExpression("^[A-Za-z0-9_-]{1,63}$").match(QFileInfo(root).fileName()).hasMatch();
}
} // namespace

void
discard_csv_bundle(const CsvBundleImport& bundle, bool existingRoot) {
    if (!bundle.path.isEmpty()) {
        QFile::remove(bundle.path);
    }
    if (!bundle.root.isEmpty() && ownedRoot(bundle.root)) {
        if (!existingRoot) {
            QDir(bundle.root).removeRecursively();
        } else if (!bundle.revision.isEmpty()) {
            QDir(bundle.root + '/' + bundle.revision).removeRecursively();
        }
    }
}

static QMap<QString, QString>
resolveSources(const QString& reference, const QStringList& references, const QVariantMap& companions,
               const QString& existingRoot, QStringList& required) {
    QMap<QString, QString> sources;
    const QUrl original(reference);
    const QString local = original.isLocalFile() ? original.toLocalFile() : reference;
    for (const auto& path : references) {
        QString source = companions.value(path).toString();
        if (source.isEmpty() && QFileInfo(local).isFile()) {
            const QString candidate = QFileInfo(path).isAbsolute() ? path : QFileInfo(local).dir().filePath(path);
            if (QFileInfo(candidate).isFile()) {
                source = QUrl::fromLocalFile(candidate).toString();
            }
        }
        if (source.isEmpty() && !existingRoot.isEmpty()) {
            const QString candidate = QDir(existingRoot).filePath(path);
            if (QFileInfo(candidate).isFile()) {
                source = QUrl::fromLocalFile(candidate).toString();
            }
        }
        if (source.isEmpty()) {
            required.append(path);
        } else {
            sources.insert(path, source);
        }
    }
    return sources;
}

static QMap<QString, QString>
copyCompanions(DecoderHost* host, const QStringList& references, const QMap<QString, QString>& sources,
               const CsvBundleImport& result) {
    QMap<QString, QString> replacements;
    for (int i = 0; i < references.size(); ++i) {
        const QString relative = result.revision + '/' + QString::number(i) + ".csv";
        const QString staged =
            host->importDocument(sources.value(references[i]), QFileInfo(references[i]).fileName(), QString());
        if (staged.isEmpty() || !QFile::rename(staged, result.root + '/' + relative)) {
            if (!staged.isEmpty()) {
                QFile::remove(staged);
            }
            return {};
        }
        replacements.insert(references[i], relative);
    }
    return replacements;
}

CsvBundleImport
stage_csv_bundle(DecoderHost* host, const QString& reference, const QString& name, const QVariantMap& companions,
                 const QString& existingRoot) {
    CsvBundleImport result;
    if (!host) {
        result.error = "open";
        return result;
    }
    const QString copied = host->importDocument(reference, name, QString());
    if (copied.isEmpty()) {
        result.error = "open";
        return result;
    }
    QFile input(copied);
    if (!input.open(QIODevice::ReadOnly)) {
        QFile::remove(copied);
        result.error = "open";
        return result;
    }
    auto bytes = input.readAll();
    input.close();
    if (bytes.contains('\0')) {
        QFile::remove(copied);
        result.error = "format";
        return result;
    }
    const QString text = QString::fromUtf8(bytes);
    QStringList references;
    (void)rewriteMap(text, [&](const QString& path) {
        if (!references.contains(path)) {
            references.append(path);
        }
        return path;
    });
    if (references.isEmpty()) {
        result.path = copied;
        return result;
    }
    const auto sources = resolveSources(reference, references, companions, existingRoot, result.required);
    if (!result.required.isEmpty()) {
        QFile::remove(copied);
        result.error = "companions";
        return result;
    }
    const QString root = existingRoot.isEmpty()
                             ? json_store_path("imports/bundles/" + QUuid::createUuid().toString(QUuid::WithoutBraces))
                             : existingRoot;
    if (!ownedRoot(root)) {
        QFile::remove(copied);
        result.error = "path";
        return result;
    }
    result.root = root;
    result.revision = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString directory = root + '/' + result.revision;
    if (!QDir().mkpath(directory)) {
        QFile::remove(copied);
        result.error = "copy";
        return result;
    }
    const auto replacements = copyCompanions(host, references, sources, result);
    if (replacements.size() != references.size()) {
        QFile::remove(copied);
        result.error = "copy";
        discard_csv_bundle(result, !existingRoot.isEmpty());
        return result;
    }
    const QString rewritten = rewriteMap(text, [&](const QString& path) { return replacements.value(path, path); });
    result.path =
        root + '/' + (existingRoot.isEmpty() ? QFileInfo(copied).fileName() : ".staged-" + result.revision + ".csv");
    QFile::remove(copied);
    if (!writePrivate(result.path, rewritten.toUtf8())) {
        result.error = "copy";
        discard_csv_bundle(result, !existingRoot.isEmpty());
        result.path.clear();
    }
    return result;
}
} // namespace dsd_qt
