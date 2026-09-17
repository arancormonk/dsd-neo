// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <functional>
#include <iterator>
#include <stddef.h>
#include <utility>
#include "csv_bundle_import.h"
#include "decoder_host.h"
#include "json_store.h"

namespace dsd_qt {
namespace {
QString
documentReference(const QString& source) {
    // Library selections are stored paths; Android's host requires a URI. Do
    // this only at the copy boundary so opaque selection identities stay intact.
    return QFileInfo(source).isAbsolute() ? QUrl::fromLocalFile(source).toString() : source;
}

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
optionType(const QString& option) {
    if (option == "-K") {
        return "keysHex";
    }
    if (option == "-k") {
        return "keysDec";
    }
    return option == "-G" ? "group" : "dmrTgKeys";
}

QString
rewriteOptions(QString options, const std::function<QString(const QString&, const QString&)>& resolve) {
    options = unquote(options);
    static const QRegularExpression paths(
        QStringLiteral("(?:^|\\s)(-K|-k|-G|--dmr-tg-key-csv)(?:=|\\s+)(?:\"([^\"]+)\"|'([^']+)'|(\\S+))"));
    auto matches = paths.globalMatch(options);

    struct Edit {
        qsizetype at;
        qsizetype length;
        QString value;
    };

    QList<Edit> edits;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const int group = match.capturedStart(2) >= 0 ? 2 : match.capturedStart(3) >= 0 ? 3 : 4;
        const QString original = match.captured(group);
        edits.append({match.capturedStart(group), match.capturedLength(group),
                      resolve(original, optionType(match.captured(1)))});
    }
    for (auto i = edits.crbegin(); i != edits.crend(); ++i) {
        options.replace(i->at, i->length, i->value);
    }
    return options;
}

QString
rewriteMap(const QString& text, const std::function<QString(const QString&, const QString&)>& resolve) {
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
                    cells[column] = resolve(path, header == "keys_hex_csv" ? "keysHex" : "keysDec");
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

QString
csv_companion_type_name(const QString& type) {
    if (type == "keysHex") {
        return QObject::tr("hex keys");
    }
    if (type == "keysDec") {
        return QObject::tr("decimal keys");
    }
    if (type == "chan") {
        return QObject::tr("channel map");
    }
    if (type == "p25Bandplan") {
        return QObject::tr("P25 band plan");
    }
    if (type == "group") {
        return QObject::tr("talkgroups");
    }
    if (type == "src") {
        return QObject::tr("radio IDs");
    }
    if (type == "vertexKeys") {
        return QObject::tr("Vertex keystreams");
    }
    if (type == "trunkTargets") {
        return QObject::tr("scan targets");
    }
    return type == "dmrTgKeys" ? QObject::tr("DMR talkgroup key mapping") : type;
}

QString
csv_companion_type_error(const QString& name, const QString& actual, const QString& expected) {
    if (actual.isEmpty() || actual == expected) {
        return {};
    }
    if (actual == "keysHex" && expected == "keysDec") {
        return QObject::tr("%1 is a hex key file; this bundle expects decimal keys").arg(name);
    }
    if (actual == "keysDec" && expected == "keysHex") {
        return QObject::tr("%1 is a decimal key file; this bundle expects hex keys").arg(name);
    }
    return QObject::tr("%1 is imported as %2; this bundle expects %3")
        .arg(name, csv_companion_type_name(actual), csv_companion_type_name(expected));
}

static QString
libraryTypeError(const QString& source, const QString& type, const QVariantMap& libraryEntries) {
    const QUrl url(source);
    const QFileInfo file(url.isLocalFile() ? url.toLocalFile() : source);
    const auto entry = libraryEntries.value(file.canonicalFilePath()).toMap();
    return csv_companion_type_error(entry.value("name").toString(), entry.value("type").toString(), type);
}

static bool
rejectCompanionType(CsvBundleImport& result, const QString& key, const QString& source,
                    const QVariantMap& libraryEntries) {
    auto detail = result.requiredDetails.value(key).toMap();
    const QString warning = libraryTypeError(source, detail.value("type").toString(), libraryEntries);
    if (warning.isEmpty()) {
        return false;
    }
    detail.insert("selectionError", warning);
    result.requiredDetails.insert(key, detail);
    if (!result.required.contains(key)) {
        result.required.append(key);
    }
    return true;
}

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

static QString
channelReferenceKey(const QString& path, const QString& type) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{path, type}).toJson(QJsonDocument::Compact));
}

static QMap<QString, QString>
resolveSources(const QString& reference, const QStringList& references, const QVariantMap& companions,
               const QString& existingRoot, CsvBundleImport& result) {
    QMap<QString, QString> sources;
    const QUrl original(reference);
    const QString local = original.isLocalFile() ? original.toLocalFile() : reference;
    for (const auto& key : references) {
        const QString path = result.requiredDetails.value(key).toMap().value("reference").toString();
        // Retain the original channel import API's explicit path-keyed choices.
        QString source = companions.value(key, companions.value(path)).toString();
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
            result.required.append(key);
        } else {
            sources.insert(key, source);
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
        const QString staged = host->importDocument(
            documentReference(sources.value(references[i])),
            result.requiredDetails.value(references[i]).toMap().value("name").toString(), QString());
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

static bool
channelSourcesReady(const QMap<QString, QString>& sources, const QVariantMap& libraryEntries, CsvBundleImport& result) {
    for (auto i = sources.cbegin(); i != sources.cend(); ++i) {
        rejectCompanionType(result, i.key(), i.value(), libraryEntries);
    }
    if (!result.required.isEmpty()) {
        result.error = "companions";
        return false;
    }
    return true;
}

CsvBundleImport
stage_csv_bundle(DecoderHost* host, const QString& reference, const QString& name, const QVariantMap& companions,
                 const QString& existingRoot, const QVariantMap& libraryEntries) {
    CsvBundleImport result;
    if (!host) {
        result.error = "open";
        return result;
    }
    const QString copied = host->importDocument(documentReference(reference), name, QString());
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
    (void)rewriteMap(text, [&](const QString& path, const QString& type) {
        const QString key = channelReferenceKey(path, type);
        if (!references.contains(key)) {
            references.append(key);
        }
        result.requiredLabels.insert(key, path);
        result.requiredDetails.insert(
            key, QVariantMap{{"name", QFileInfo(path).fileName()}, {"type", type}, {"reference", path}});
        return path;
    });
    if (references.isEmpty()) {
        result.path = copied;
        return result;
    }
    const auto sources = resolveSources(reference, references, companions, existingRoot, result);
    if (!channelSourcesReady(sources, libraryEntries, result)) {
        QFile::remove(copied);
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
    const QString rewritten = rewriteMap(text, [&](const QString& path, const QString& type) {
        return replacements.value(channelReferenceKey(path, type), path);
    });
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

namespace {
QString
targetReferenceType(dsd_app_scan_file_kind kind) {
    switch (kind) {
        case DSD_APP_SCAN_FILE_CHANNEL: return "chan";
        case DSD_APP_SCAN_FILE_BANDPLAN: return "p25Bandplan";
        case DSD_APP_SCAN_FILE_KEYS_HEX: return "keysHex";
        case DSD_APP_SCAN_FILE_KEYS_DEC: return "keysDec";
        case DSD_APP_SCAN_FILE_GROUP: return "group";
        case DSD_APP_SCAN_FILE_DMR_MAP: return "dmrTgKeys";
    }
    return {};
}

struct TargetReference {
    size_t index = 0;
    dsd_app_scan_file_kind kind = DSD_APP_SCAN_FILE_CHANNEL;
    QString path, resolved;
};

void
collectTargetReference(const dsd_app_scan_csv_reference* ref, void* context) {
    static_cast<QList<TargetReference>*>(context)->append(
        {ref->index, ref->kind, QString::fromUtf8(ref->path), QString::fromUtf8(ref->resolved_path)});
}

struct StagedCopies {
    QStringList paths;

    ~StagedCopies() {
        for (const auto& path : paths) {
            QFile::remove(path);
        }
    }
};

struct TargetDocument {
    QString source, label;
    bool channel = false;
    bool primary = false;
};

class TrunkBundleStager {
  public:
    TrunkBundleStager(DecoderHost* h, CsvBundleImport& out, const QVariantMap& selections, const QString& oldRoot,
                      const QString& fileName, const QVariantMap& entries)
        : host(h), result(out), companions(selections), libraryEntries(entries), existingRoot(oldRoot), name(fileName) {
    }

    QString
    stageDocument(const TargetDocument& input) {
        const QString identity = input.source + (input.channel ? "#channel" : "#target");
        if (stored.contains(identity)) {
            return stored.value(identity);
        }
        const QString copy = host->importDocument(documentReference(input.source),
                                                  input.primary ? name : QFileInfo(input.label).fileName());
        if (copy.isEmpty()) {
            result.error = "open";
            return {};
        }
        copies.paths << copy;
        if (input.primary) {
            result.name = QFileInfo(copy).fileName();
        }
        const QUrl url(input.source);
        const QString original = url.isLocalFile() ? url.toLocalFile() : input.source;
        const bool local = QFileInfo(original).isFile();
        const QByteArray base = local ? original.toUtf8() : QByteArray("<document>");
        QList<TargetReference> refs;
        char error[512] = {};
        const dsd_app_scan_csv_callbacks callbacks{nullptr, collectTargetReference, &refs};
        if (dsd_app_scan_csv_inspect(copy.toUtf8().constData(), base.constData(), input.channel, &callbacks, error,
                                     sizeof error)) {
            result.error = "format";
            result.detail = QString::fromUtf8(error);
            return {};
        }
        QString destination = input.primary ? result.path : nextPath();
        QList<QByteArray> replacements;
        QList<dsd_app_scan_csv_replacement> mapping;
        if (!stageReferences(input, local, refs, destination, replacements, mapping)) {
            return {};
        }
        for (qsizetype i = 0; i < mapping.size(); ++i) {
            mapping[i].relative_path = replacements[i].constData();
        }
        if (dsd_app_scan_csv_rewrite(copy.toUtf8().constData(), base.constData(), input.channel,
                                     destination.toUtf8().constData(), mapping.constData(),
                                     static_cast<size_t>(mapping.size()), error, sizeof error)) {
            result.error = "format";
            result.detail = QString::fromUtf8(error);
            return {};
        }
        stored.insert(identity, destination);
        return destination;
    }

  private:
    QString
    nextPath() {
        return result.root + '/' + result.revision + '/' + QString::number(nextFile++) + ".csv";
    }

    QString
    selectedSource(const TargetReference& ref, const QString& key, bool local) {
        QString selected = companions.value(key).toString();
        if (selected.isEmpty() && local && QFileInfo(ref.resolved).isFile()) {
            selected = QUrl::fromLocalFile(ref.resolved).toString();
        }
        // Detached copies of the stored primary may reuse revision companions,
        // but arbitrary names must not bind to the bundle's own targets.csv.
        static const QRegularExpression revisionFile(QStringLiteral("^[0-9a-f-]{36}/[0-9]+\\.csv$"));
        if (selected.isEmpty() && !existingRoot.isEmpty() && revisionFile.match(ref.path).hasMatch()) {
            const QString candidate = QDir::cleanPath(QDir(existingRoot).filePath(ref.path));
            if (candidate.startsWith(existingRoot + '/') && QFileInfo(candidate).isFile()) {
                selected = QUrl::fromLocalFile(candidate).toString();
            }
        }
        if (rejectCompanionType(result, key, selected, libraryEntries)) {
            return {};
        }
        return selected;
    }

    QString
    stageLeaf(const QString& source, const QString& label) {
        const QUrl url(source);
        const QString canonical = QFileInfo(url.isLocalFile() ? url.toLocalFile() : source).canonicalFilePath();
        const QString identity = (canonical.isEmpty() ? source : canonical) + "#leaf";
        if (stored.contains(identity)) {
            return stored.value(identity);
        }
        const QString copy = host->importDocument(documentReference(source), QFileInfo(label).fileName());
        if (copy.isEmpty()) {
            result.error = "open";
            return {};
        }
        copies.paths << copy;
        QString destination = nextPath();
        if (!QFile::rename(copy, destination)) {
            result.error = "copy";
            return {};
        }
        stored.insert(identity, destination);
        return destination;
    }

    bool
    stageReferences(const TargetDocument& input, bool local, const QList<TargetReference>& refs,
                    const QString& destination, QList<QByteArray>& replacements,
                    QList<dsd_app_scan_csv_replacement>& mapping) {
        for (const auto& ref : refs) {
            // Opaque, unambiguous identities are separate from the display label.
            const QString key = QString::fromUtf8(QJsonDocument(QJsonArray{input.primary ? QString() : input.source,
                                                                           ref.path, targetReferenceType(ref.kind)})
                                                      .toJson(QJsonDocument::Compact));
            const QString label = input.primary ? ref.path : input.label + QStringLiteral(" → ") + ref.path;
            result.requiredDetails.insert(
                key, QVariantMap{{"name", QFileInfo(ref.path).fileName()}, {"type", targetReferenceType(ref.kind)}});
            result.requiredLabels.insert(key, label);
            const QString selected = selectedSource(ref, key, local);
            if (selected.isEmpty()) {
                if (!result.required.contains(key)) {
                    result.required << key;
                }
                continue;
            }
            const QString companion = ref.kind == DSD_APP_SCAN_FILE_CHANNEL
                                          ? stageDocument({selected, label, true, false})
                                          : stageLeaf(selected, ref.path);
            if (!result.error.isEmpty()) {
                return false;
            }
            if (companion.isEmpty()) {
                continue;
            }
            replacements << QDir(QFileInfo(destination).absolutePath()).relativeFilePath(companion).toUtf8();
            mapping << dsd_app_scan_csv_replacement{ref.index, nullptr};
        }
        return result.required.isEmpty();
    }

    DecoderHost* host;
    CsvBundleImport& result;
    const QVariantMap& companions;
    const QVariantMap& libraryEntries;
    QString existingRoot, name;
    StagedCopies copies;
    QMap<QString, QString> stored;
    int nextFile = 0;
};
} // namespace

CsvBundleImport
stage_trunk_csv_bundle(DecoderHost* host, const QString& reference, const QString& name, const QVariantMap& companions,
                       const QString& existingRoot, const QVariantMap& libraryEntries) {
    CsvBundleImport result;
    if (!host) {
        result.error = "open";
        return result;
    }
    result.root = existingRoot.isEmpty()
                      ? json_store_path("imports/bundles/" + QUuid::createUuid().toString(QUuid::WithoutBraces))
                      : existingRoot;
    result.revision = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!ownedRoot(result.root) || !QDir().mkpath(result.root + '/' + result.revision)) {
        result.error = "copy";
        return result;
    }
    result.path = result.root + '/'
                  + (existingRoot.isEmpty() ? QStringLiteral("targets.csv") : ".staged-" + result.revision + ".csv");
    TrunkBundleStager stager(host, result, companions, existingRoot, name, libraryEntries);
    stager.stageDocument({reference, QString(), false, true});
    if (result.error.isEmpty() && !result.required.isEmpty()) {
        result.error = "companions";
    }
    if (result.error.isEmpty()) {
        char error[512] = {};
        if (dsd_app_trunk_scan_validate_bundle(result.path.toUtf8().constData(), nullptr, nullptr, error,
                                               sizeof error)) {
            result.error = "format";
            result.detail = QString::fromUtf8(error);
        }
    }
    if (!result.error.isEmpty()) {
        discard_csv_bundle(result, !existingRoot.isEmpty());
        result.path.clear();
    }
    return result;
}

static bool
collectReachableReferences(const QString& path, QList<TargetReference>& refs) {
    const dsd_app_scan_csv_callbacks callbacks{nullptr, collectTargetReference, &refs};
    if (dsd_app_scan_csv_inspect(path.toUtf8().constData(), nullptr, 0, &callbacks, nullptr, 0)) {
        return false;
    }
    QList<TargetReference> nested;
    const dsd_app_scan_csv_callbacks nestedCallbacks{nullptr, collectTargetReference, &nested};
    const bool failed = std::any_of(refs.cbegin(), refs.cend(), [&nestedCallbacks](const TargetReference& ref) {
        return ref.kind == DSD_APP_SCAN_FILE_CHANNEL
               && dsd_app_scan_csv_inspect(ref.resolved.toUtf8().constData(), nullptr, 1, &nestedCallbacks, nullptr, 0);
    });
    if (failed) {
        return false;
    }
    refs.append(nested);
    return true;
}

void
clean_trunk_csv_revisions(const QString& path, const QString& root) {
    if (!ownedRoot(root) || QFileInfo(path).absolutePath() != root
        || dsd_app_trunk_scan_validate_bundle(path.toUtf8().constData(), nullptr, nullptr, nullptr, 0)) {
        return;
    }
    QList<TargetReference> refs;
    if (!collectReachableReferences(path, refs)) {
        return;
    }
    QSet<QString> used;
    for (const auto& ref : refs) {
        const QString relative = QDir(root).relativeFilePath(ref.resolved);
        used.insert(relative.section('/', 0, 0));
    }
    QDir directory(root);
    const QRegularExpression revision(QStringLiteral("^[0-9a-f-]{36}$"));
    for (const auto& name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (revision.match(name).hasMatch() && !used.contains(name)
            && !QFileInfo(directory.filePath(name)).isSymLink()) {
            QDir(directory.filePath(name)).removeRecursively();
        }
    }
}
} // namespace dsd_qt
