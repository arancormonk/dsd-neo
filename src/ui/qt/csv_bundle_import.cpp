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
#include <QLatin1String>
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
#include <cerrno>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/scan_options.h>
#include <functional>
#include <iterator>
#include <qflags.h>
#include <stddef.h>
#include <utility>
#include "backup_archive.h"
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

/* A file operand for a spreadsheet-quoted options cell: as it is when it needs no grouping, else grouped by a
   doubled quote, or by single quotes when it holds a double quote. */
QByteArray
csvQuotedOperand(const QString& value) {
    QByteArray bytes = value.toUtf8();
    const bool plain = !bytes.isEmpty() && std::none_of(bytes.begin(), bytes.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '"' || c == '\'';
    });
    if (plain) {
        return bytes;
    }
    if (!bytes.contains('"')) {
        return "\"\"" + bytes + "\"\"";
    }
    return bytes.contains('\'') ? bytes : "'" + bytes + "'";
}

struct QuotedOperand {
    size_t offset = 0, length = 0;
    QString option, path;
    bool whole = false;
};

int
collectQuotedOperand(void* context, const char* option, const char* path, size_t offset, size_t length, int whole) {
    static_cast<QList<QuotedOperand>*>(context)->append(
        {offset, length, QString::fromUtf8(option), QString::fromUtf8(path), whole != 0});
    return 0;
}

/* A channel-map options cell a spreadsheet quoted (dsd_scan_options_cell_csv_quoted(), issue #637), read as the
   loader reads it: each file operand is rewritten in place, quoted for the cell, which keeps its own quotes. False when
   the cell is not one. */
bool
rewriteQuotedOptions(const QString& options, const std::function<QString(const QString&, const QString&)>& resolve,
                     QString& rewritten) {
    QByteArray cell = options.trimmed().toUtf8();
    if (cell.size() < 2 || cell.front() != '"' || cell.back() != '"') {
        return false;
    }
    const char* content = cell.constData() + 1;
    const size_t length = static_cast<size_t>(cell.size()) - 2;
    QList<QuotedOperand> operands;
    if (dsd_scan_options_cell_csv_quoted(cell.constData(), content, length) == 0
        || dsd_scan_options_visit_cell_files(cell.constData(), content, length, &operands, collectQuotedOperand,
                                             nullptr)
               != 0) {
        return false;
    }
    for (auto i = operands.crbegin(); i != operands.crend(); ++i) {
        const QByteArray value = csvQuotedOperand(resolve(i->path, optionType(i->option)));
        cell.replace(static_cast<qsizetype>(i->offset) + 1, static_cast<qsizetype>(i->length),
                     i->whole ? i->option.toUtf8() + "=" + value : value);
    }
    rewritten = QString::fromUtf8(cell);
    return true;
}

QString
rewriteOptions(QString options, const std::function<QString(const QString&, const QString&)>& resolve) {
    QString quoted;
    if (rewriteQuotedOptions(options, resolve, quoted)) {
        return quoted;
    }
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
    return !QFileInfo(root).isSymLink() && !QFileInfo(root).isJunction() && QFileInfo(root).absolutePath() == parent
           && QRegularExpression("^[A-Za-z0-9_-]{1,63}$").match(QFileInfo(root).fileName()).hasMatch();
}

/* Bytes that may hold key material (a channel map's options can carry -H), wiped when they go. */
struct PrivateBytes {
    QByteArray bytes;

    ~PrivateBytes() {
        if (!bytes.isEmpty()) {
            DSD_SECURE_ZERO(bytes.data(), static_cast<size_t>(bytes.size()));
        }
    }
};

/* Read @p path whole; false on any read error, so a failed read's valid prefix is never taken for the file (B9). */
bool
readWhole(const QString& path, QByteArray& bytes) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        return false;
    }
    bytes = input.readAll();
    return input.error() == QFileDevice::NoError;
}

QString
newUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool
revisionName(const QString& name) {
    static const QRegularExpression revision(QStringLiteral("^[0-9a-f-]{36}$"));
    return revision.match(name).hasMatch();
}

bool
realDirectory(const QFileInfo& info) {
    return info.isDir() && !info.isSymLink() && !info.isJunction();
}

/* A path relative to imports/ that stays inside it: clean, not absolute, never "..". */
bool
insideRelative(const QString& relative) {
    return !relative.isEmpty() && !QDir::isAbsolutePath(relative) && QDir::cleanPath(relative) == relative
           && relative != QLatin1String("..") && !relative.startsWith(QLatin1String("../"));
}

QString
inLibrary(const QString& relative) {
    return QDir::cleanPath(json_store_path(QStringLiteral("imports")) + '/' + relative);
}

/* What a bundle may still copy: each copy at most 32 MiB and at most what is left of the bundle's budget. */
class CopyBudget {
  public:
    explicit CopyBudget(qint64 limit) : m_limit(limit) {}

    qint64
    cap() const {
        return std::min(backup::kMaxFileBytes, m_limit - m_copied);
    }

    /*
     * The limit a refused copy of a document of @p size bytes (-1: unknown) ran into: the bundle's once it is what held
     * the copy back, unless the document is over the per-file limit anyway.
     */
    qint64
    limitFor(qint64 allowed, qint64 size) const {
        return allowed < backup::kMaxFileBytes && size <= backup::kMaxFileBytes ? m_limit : backup::kMaxFileBytes;
    }

    qint64
    add(qint64 bytes) {
        m_copied += bytes;
        return m_copied;
    }

  private:
    qint64 m_limit;
    qint64 m_copied = 0;
};

void
tooLarge(CsvBundleImport& result, qint64 limit, const QString& name) {
    result.error = QStringLiteral("too_large");
    result.limitBytes = limit;
    result.name = name;
}

/*
 * Copy one document of the bundle into the staging folder, within @p budget: the copy's path, and its name in @p name.
 * Empty when it failed, with the failure in @p result, whose name then names the document that failed.
 */
QString
fetchDocument(const ImportFetch& fetch, CopyBudget& budget, const QString& source, const QString& hint,
              CsvBundleImport& result, QString* name) {
    const qint64 cap = budget.cap();
    if (cap <= 0) {
        tooLarge(result, budget.limitFor(cap, -1), hint);
        return {};
    }
    const ImportFetchResult copy = fetch(documentReference(source), hint, cap);
    switch (copy.code) {
        case DecoderHost::DocumentCopied:
            result.copiedBytes = budget.add(copy.bytes);
            *name = copy.name;
            return copy.path;
        case DecoderHost::DocumentTooLarge: tooLarge(result, budget.limitFor(cap, copy.size), copy.name); return {};
        case DecoderHost::DocumentLocalFailed:
            result.error = QStringLiteral("local");
            result.noSpace = copy.noSpace;
            break;
        case DecoderHost::DocumentCancelled: result.error = QStringLiteral("cancelled"); break;
        default: result.error = QStringLiteral("read"); break;
    }
    result.name = copy.name;
    return {};
}

/* Move a staged copy to @p relative in the mirror and list it; false (local) when it cannot go there. */
bool
placeInMirror(const QString& copy, const QString& mirrorRoot, const QString& relative, CsvBundleImport& result) {
    const QString destination = mirrorRoot + '/' + relative;
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::rename(copy, destination)) {
        result.error = QStringLiteral("local");
        return false;
    }
    result.stagedRelative.append(relative);
    return true;
}

/*
 * The bundle's layout: a new root (or the replaced row's), a new revision in it, and the primary's final place (the
 * replaced row's, else @p newPrimary in the new root). The mirror gets the revision directory.
 */
bool
layoutBundle(const BundleTarget& target, const QString& newPrimary, CsvBundleImport& result) {
    result.newRoot = target.existingRootRelative.isEmpty();
    result.rootRelative = result.newRoot ? QStringLiteral("bundles/") + newUuid() : target.existingRootRelative;
    result.revisionRelative = result.rootRelative + '/' + newUuid();
    result.primaryRelative =
        target.primaryRelative.isEmpty() ? result.rootRelative + '/' + newPrimary : target.primaryRelative;
    if (!insideRelative(result.rootRelative) || !ownedRoot(inLibrary(result.rootRelative))
        || !insideRelative(result.primaryRelative)) {
        result.error = QStringLiteral("path");
        return false;
    }
    if (!QDir().mkpath(target.mirrorRoot + '/' + result.revisionRelative)) {
        result.error = QStringLiteral("local");
        return false;
    }
    return true;
}
} // namespace

QStringList
csv_companion_references(const QString& text) {
    QStringList references;
    (void)rewriteMap(text, [&references](const QString& path, const QString&) {
        if (!references.contains(path)) {
            references.append(path);
        }
        return path;
    });
    return references;
}

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

bool
owned_bundle_root(const QString& dir) {
    return ownedRoot(dir) && realDirectory(QFileInfo(dir));
}

bool
owned_bundle_revision(const QString& dir) {
    const QFileInfo info(dir);
    return revisionName(info.fileName()) && realDirectory(info) && owned_bundle_root(info.absolutePath());
}

void
remove_bundle_root(const QString& root) {
    if (owned_bundle_root(root)) {
        QDir(root).removeRecursively();
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

/* Copy each selected companion into the mirror's revision directory; the map from each reference to the path the
   primary will name it by, or empty with the failure in @p result. */
static QMap<QString, QString>
copyCompanions(const ImportFetch& fetch, CopyBudget& budget, const BundleTarget& target, const QStringList& references,
               const QMap<QString, QString>& sources, CsvBundleImport& result) {
    QMap<QString, QString> replacements;
    const QDir primaryDir(QFileInfo(target.mirrorRoot + '/' + result.primaryRelative).absolutePath());
    for (int i = 0; i < references.size(); ++i) {
        const QString relative = result.revisionRelative + '/' + QString::number(i) + ".csv";
        QString name;
        const QString copy =
            fetchDocument(fetch, budget, sources.value(references[i]),
                          result.requiredDetails.value(references[i]).toMap().value("name").toString(), result, &name);
        if (copy.isEmpty() || !placeInMirror(copy, target.mirrorRoot, relative, result)) {
            return {};
        }
        result.stagedNames.insert(relative, name);
        replacements.insert(references[i], primaryDir.relativeFilePath(target.mirrorRoot + '/' + relative));
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

/* The channel map's companion references, each once, with their labels and roles in @p result. */
static QStringList
channelReferences(const QString& text, CsvBundleImport& result) {
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
    return references;
}

/* The rewritten primary, written to its place in the mirror; false with the failure in @p result. */
static bool
writeChannelPrimary(const BundleTarget& target, const QString& text, const QMap<QString, QString>& replacements,
                    CsvBundleImport& result) {
    PrivateBytes rewritten;
    rewritten.bytes = rewriteMap(text, [&](const QString& path, const QString& type) {
                          return replacements.value(channelReferenceKey(path, type), path);
                      }).toUtf8();
    if (rewritten.bytes.size() > backup::kMaxFileBytes) {
        tooLarge(result, backup::kMaxFileBytes, result.name);
        return false;
    }
    errno = 0;
    if (!writePrivate(target.mirrorRoot + '/' + result.primaryRelative, rewritten.bytes)) {
        result.error = QStringLiteral("local");
        result.noSpace = errno == ENOSPC;
        return false;
    }
    result.stagedRelative.append(result.primaryRelative);
    result.stagedNames.insert(result.primaryRelative, result.name);
    return true;
}

CsvBundleImport
stage_csv_bundle(const ImportFetch& fetch, const BundleTarget& target, const QString& reference, const QString& name,
                 const QVariantMap& companions, const QVariantMap& libraryEntries) {
    CsvBundleImport result;
    CopyBudget budget(target.budget);
    const QString copied = fetchDocument(fetch, budget, reference, name, result, &result.name);
    if (copied.isEmpty()) {
        return result;
    }
    PrivateBytes bytes;
    if (!readWhole(copied, bytes.bytes)) {
        result.error = QStringLiteral("local");
        return result;
    }
    if (bytes.bytes.contains('\0')) {
        result.error = QStringLiteral("format");
        return result;
    }
    const QString text = QString::fromUtf8(bytes.bytes);
    const QStringList references = channelReferences(text, result);
    if (references.isEmpty()) {
        // A plain map: its own bytes, at the replaced row's place or under its own name.
        result.primaryRelative = target.primaryRelative.isEmpty() ? result.name : target.primaryRelative;
        if (insideRelative(result.primaryRelative)) {
            if (placeInMirror(copied, target.mirrorRoot, result.primaryRelative, result)) {
                result.stagedNames.insert(result.primaryRelative, result.name);
            }
        } else {
            result.error = QStringLiteral("path");
        }
        return result;
    }
    const QString existingRoot =
        target.existingRootRelative.isEmpty() ? QString() : inLibrary(target.existingRootRelative);
    const auto sources = resolveSources(reference, references, companions, existingRoot, result);
    if (!channelSourcesReady(sources, libraryEntries, result) || !layoutBundle(target, result.name, result)) {
        return result;
    }
    const auto replacements = copyCompanions(fetch, budget, target, references, sources, result);
    if (replacements.size() == references.size()) {
        (void)writeChannelPrimary(target, text, replacements, result);
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

struct TargetDocument {
    QString source, label;
    bool channel = false;
    bool primary = false;
};

class TrunkBundleStager {
  public:
    TrunkBundleStager(const ImportFetch& f, const BundleTarget& t, CsvBundleImport& out, const QVariantMap& selections,
                      const QString& oldRoot, const QString& fileName, const QVariantMap& entries)
        : fetch(f), target(t), result(out), companions(selections), libraryEntries(entries), existingRoot(oldRoot),
          name(fileName), budget(t.budget) {}

    QString
    stageDocument(const TargetDocument& input) {
        const QString identity = input.source + (input.channel ? "#channel" : "#target");
        if (stored.contains(identity)) {
            return stored.value(identity);
        }
        QString copied;
        const QString copy = fetchDocument(fetch, budget, input.source,
                                           input.primary ? name : QFileInfo(input.label).fileName(), result, &copied);
        if (copy.isEmpty()) {
            return {};
        }
        if (input.primary) {
            result.name = copied;
        }
        const QUrl url(input.source);
        const QString original = url.isLocalFile() ? url.toLocalFile() : input.source;
        const bool local = QFileInfo(original).isFile();
        const QByteArray base = local ? original.toUtf8() : QByteArray("<document>");
        QList<TargetReference> refs;
        char error[512] = {};
        const dsd_app_scan_csv_callbacks callbacks{nullptr, collectTargetReference, &refs, nullptr};
        if (dsd_app_scan_csv_inspect(copy.toUtf8().constData(), base.constData(), input.channel, &callbacks, error,
                                     sizeof error)) {
            result.error = "format";
            result.detail = QString::fromUtf8(error);
            return {};
        }
        const QString relative = input.primary ? result.primaryRelative : nextRelative();
        QString destination = target.mirrorRoot + '/' + relative;
        QList<QByteArray> replacements;
        QList<dsd_app_scan_csv_replacement> mapping;
        if (!stageReferences(input, local, refs, destination, replacements, mapping)) {
            return {};
        }
        for (qsizetype i = 0; i < mapping.size(); ++i) {
            mapping[i].relative_path = replacements[i].constData();
        }
        if (!rewrite(copy, base, input.channel, destination, mapping)) {
            return {};
        }
        result.stagedRelative.append(relative);
        result.stagedNames.insert(relative, copied);
        stored.insert(identity, destination);
        return destination;
    }

  private:
    QString
    nextRelative() {
        return result.revisionRelative + '/' + QString::number(nextFile++) + ".csv";
    }

    /* Rewrite @p copy's references into @p destination: a write that fails is this device's (local), an input the
       rewrite refuses a format failure, and an output over 32 MiB too large. */
    bool
    rewrite(const QString& copy, const QByteArray& base, bool channel, const QString& destination,
            const QList<dsd_app_scan_csv_replacement>& mapping) {
        char error[512] = {};
        errno = 0;
        const int rc = dsd_app_scan_csv_rewrite(copy.toUtf8().constData(), base.constData(), channel,
                                                destination.toUtf8().constData(), mapping.constData(),
                                                static_cast<size_t>(mapping.size()), error, sizeof error);
        if (rc == DSD_APP_SCAN_CSV_WRITE_FAILED) {
            result.error = "local";
            result.noSpace = errno == ENOSPC;
            return false;
        }
        if (rc != 0) {
            result.error = "format";
            result.detail = QString::fromUtf8(error);
            return false;
        }
        if (QFileInfo(destination).size() > backup::kMaxFileBytes) {
            tooLarge(result, backup::kMaxFileBytes, result.name);
            return false;
        }
        return true;
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
        QString copied;
        const QString copy = fetchDocument(fetch, budget, source, QFileInfo(label).fileName(), result, &copied);
        const QString relative = nextRelative();
        if (copy.isEmpty() || !placeInMirror(copy, target.mirrorRoot, relative, result)) {
            return {};
        }
        QString destination = target.mirrorRoot + '/' + relative;
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

    const ImportFetch& fetch;
    const BundleTarget& target;
    CsvBundleImport& result;
    const QVariantMap& companions;
    const QVariantMap& libraryEntries;
    QString existingRoot, name;
    CopyBudget budget;
    QMap<QString, QString> stored;
    int nextFile = 0;
};
} // namespace

CsvBundleImport
stage_trunk_csv_bundle(const ImportFetch& fetch, const BundleTarget& target, const QString& reference,
                       const QString& name, const QVariantMap& companions, const QVariantMap& libraryEntries) {
    CsvBundleImport result;
    if (!layoutBundle(target, QStringLiteral("targets.csv"), result)) {
        return result;
    }
    const QString existingRoot = result.newRoot ? QString() : inLibrary(result.rootRelative);
    TrunkBundleStager stager(fetch, target, result, companions, existingRoot, name, libraryEntries);
    stager.stageDocument({reference, QString(), false, true});
    if (result.error.isEmpty() && !result.required.isEmpty()) {
        result.error = "companions";
    }
    return result;
}

static bool
collectReachableReferences(const QString& path, QList<TargetReference>& refs) {
    const dsd_app_scan_csv_callbacks callbacks{nullptr, collectTargetReference, &refs, nullptr};
    if (dsd_app_scan_csv_inspect(path.toUtf8().constData(), nullptr, 0, &callbacks, nullptr, 0)) {
        return false;
    }
    QList<TargetReference> nested;
    const dsd_app_scan_csv_callbacks nestedCallbacks{nullptr, collectTargetReference, &nested, nullptr};
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

namespace {
bool
collectChannelReferences(const QString& path, QList<TargetReference>& refs) {
    QList<TargetReference> found;
    const dsd_app_scan_csv_callbacks callbacks{nullptr, collectTargetReference, &found, nullptr};
    if (dsd_app_scan_csv_inspect(path.toUtf8().constData(), nullptr, 1, &callbacks, nullptr, 0) != 0) {
        return false;
    }
    refs.append(found);
    return true;
}
} // namespace

bool
bundle_reached_files(const QString& path, const QString& root, const QString& type, QSet<QString>* files) {
    QList<TargetReference> refs;
    if (type == QLatin1String("trunkTargets")) {
        // A target list reaches its channel maps' companions too; one that no longer validates keeps everything.
        if (dsd_app_trunk_scan_validate_bundle(path.toUtf8().constData(), nullptr, nullptr, nullptr, 0)
            || !collectReachableReferences(path, refs)) {
            return false;
        }
    } else if (!collectChannelReferences(path, refs)) {
        return false;
    }
    const QString prefix = QDir::cleanPath(root) + '/';
    for (const auto& ref : refs) {
        const QString resolved = QDir::cleanPath(ref.resolved);
        if (resolved.startsWith(prefix)) {
            files->insert(resolved);
        }
    }
    return true;
}

bool
bundle_reached_revisions(const QString& path, const QString& root, const QString& type, QSet<QString>* names) {
    QSet<QString> files;
    if (!bundle_reached_files(path, root, type, &files)) {
        return false;
    }
    const qsizetype skip = QDir::cleanPath(root).size() + 1;
    for (const auto& file : files) {
        names->insert(file.mid(skip).section('/', 0, 0));
    }
    return true;
}

namespace {
/* @p path as the file system resolves it, through every link (/data/user/0 and /data/data are one folder); its clean
   absolute spelling when it does not exist. */
QString
resolvedPath(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}
} // namespace

bool
holds_named_file(const QString& dir, const QStringList& named) {
    // Either spelling of the folder: a file that is gone is kept by its spelling alone.
    const QString resolved = resolvedPath(dir) + '/';
    const QString clean = QDir::cleanPath(dir) + '/';
    return std::any_of(named.cbegin(), named.cend(), [&resolved, &clean](const QString& path) {
        return path.startsWith(resolved) || path.startsWith(clean);
    });
}

namespace {
/* Add the files @p file's stored CSV reaches to @p reached; false when it cannot be read or parsed. */
bool
addReachedFiles(const LibraryFile& file, QStringList* reached) {
    const bool targets = file.type == QLatin1String("trunkTargets");
    // Only channel maps and target lists name other files; a file that is gone names nothing (load() prunes its row).
    if ((!targets && file.type != QLatin1String("chan")) || !QFileInfo::exists(file.path)) {
        return true;
    }
    QList<TargetReference> refs;
    if (!(targets ? collectReachableReferences(file.path, refs) : collectChannelReferences(file.path, refs))) {
        return false;
    }
    for (const auto& ref : refs) {
        reached->append(QDir::cleanPath(ref.resolved));
    }
    return true;
}
} // namespace

bool
kept_library_files(const QList<LibraryFile>& library, QStringList* kept) {
    bool ok = false;
    QStringList files = backup::typed_file_paths(&ok);
    const auto add = [&files](const LibraryFile& file) { return addReachedFiles(file, &files); };
    if (!ok || !std::all_of(library.cbegin(), library.cend(), add)) {
        return false;
    }
    // Compared as the file system resolves them, so a reference through a link still keeps its file.
    std::transform(files.begin(), files.end(), files.begin(), resolvedPath);
    files.removeDuplicates();
    *kept = std::move(files);
    return true;
}

namespace {
/* The revision directories of @p root: owned names, real directories, no link. */
QStringList
revisionDirectories(const QString& root) {
    QStringList dirs;
    const QDir directory(root);
    for (const auto& name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (revisionName(name) && realDirectory(QFileInfo(directory.filePath(name)))) {
            dirs.append(name);
        }
    }
    return dirs;
}

void
cleanBundle(const BundleRevisions& bundle, const QStringList& kept,
            const std::function<QStringList(bool* ok)>& journal) {
    const QString root = QDir::cleanPath(bundle.root);
    if (!owned_bundle_root(root)) {
        return;
    }
    // Listed first, then the journal, then the primary: a directory listed here was journaled before it was made, and
    // its commit drops it from the journal only once the primary names it.
    const QStringList revisions = revisionDirectories(root);
    if (revisions.isEmpty()) {
        return;
    }
    bool ok = false;
    const QStringList inFlight = journal(&ok);
    QSet<QString> reached;
    if (!ok || inFlight.contains(root) || !bundle_reached_revisions(bundle.path, root, bundle.type, &reached)) {
        return;
    }
    for (const auto& name : revisions) {
        const QString dir = root + '/' + name;
        if (!reached.contains(name) && !inFlight.contains(dir) && !holds_named_file(dir, kept)) {
            QDir(dir).removeRecursively();
        }
    }
}
} // namespace

void
clean_bundle_revisions(const QList<BundleRevisions>& bundles, const QList<LibraryFile>& library,
                       const std::function<QStringList(bool* ok)>& journal) {
    QStringList kept;
    if (bundles.isEmpty() || !kept_library_files(library, &kept)) {
        return;
    }
    for (const auto& bundle : bundles) {
        cleanBundle(bundle, kept, journal);
    }
}
} // namespace dsd_qt
