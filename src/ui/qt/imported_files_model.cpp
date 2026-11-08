// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QIODevice>
#include <cerrno>
#include <memory>
#include <stddef.h>
#include <utility>
#include "imported_files_model.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileDevice> // IWYU pragma: keep
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QMap>
#include <QUuid>
#include <QVariant>

#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/scan_options.h>

#include <QSaveFile>
#include <QTemporaryFile>
#include <dsd-neo/core/dmr_key_map.h>
#include "backup_archive.h"
#include "csv_bundle_import.h"
#include "decoder_host.h"
#include "document_import_transport.h"
#include "import_names.h"
#include "import_service.h"
#include "json_store.h"
#include "realtime_clock.h"
#include "stored_int.h"

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "imported_files.json";

/* How many names a publish tries before it gives up: a free name can still be taken (by another process) before the
 * rename claims it. */
constexpr int kPublishAttempts = 16;

struct PrivateCsvBytes {
    QByteArray bytes;

    ~PrivateCsvBytes() {
        if (!bytes.isEmpty()) {
            DSD_SECURE_ZERO(bytes.data(), static_cast<size_t>(bytes.size()));
        }
    }
};

/* Every talkgroup-export destination this process handed out (Qt thread only). The engine may still write one after
   the export flow cancelled or retried, so no import may take its name. */
QSet<QString>&
reserved_exports() {
    static QSet<QString> paths;
    return paths;
}

QVariantMap
failure(const QString& error) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
}

QVariantMap
busy_failure(const QString& type) {
    auto result = failure(QStringLiteral("busy"));
    result.insert(QStringLiteral("detail"),
                  type == QLatin1String("trunkTargets")
                      ? ImportedFilesModel::tr("Stop the decoder before replacing target files.")
                      : ImportedFilesModel::tr("Stop the decoder before replacing a talkgroup list."));
    return result;
}

/* A copy's failure as an import result. */
QVariantMap
copy_failure(int code, const ImportCopyProgress& copy) {
    switch (code) {
        case DecoderHost::DocumentTooLarge: {
            auto result = failure(QStringLiteral("too_large"));
            result.insert(QStringLiteral("limitBytes"), backup::kMaxFileBytes);
            return result;
        }
        case DecoderHost::DocumentLocalFailed: {
            auto result = failure(QStringLiteral("local"));
            result.insert(QStringLiteral("noSpace"), copy.noSpace);
            return result;
        }
        case DecoderHost::DocumentCancelled: return failure(QStringLiteral("cancelled"));
        default: return failure(QStringLiteral("read"));
    }
}

/*
 * A single file's stage, on the import worker: copy the document into the request's staging folder and validate it
 * there. @p providerName names the copy when set (a generated file); a pick takes the name its provider reports, else
 * the picker's @p hint. On success the map carries "staged", the copy's path, for the commit to publish.
 */
QVariantMap
stage_single_file(const ImportJob& job, const QString& reference, const QString& providerName, const QString& hint,
                  const QString& type) {
    const QString staged = QDir(job.stagingDir).filePath(QStringLiteral("document"));
    const int code = job.transport->copy(job.id, reference, staged, backup::kMaxFileBytes);
    const ImportCopyProgress copy = job.transport->progress(job.id);
    const QString name = sanitized_import_name(providerName.isEmpty() ? copy.displayName : providerName, hint);
    QVariantMap result;
    if (code != DecoderHost::DocumentCopied) {
        result = copy_failure(code, copy);
    } else {
        int accepted = 0;
        int skipped = 0;
        QString detail;
        if (!ImportedFilesModel::validate(staged, type, &accepted, &skipped, &detail)) {
            result = failure(QStringLiteral("format"));
            result.insert(QStringLiteral("detail"), detail);
        } else {
            result = {{QStringLiteral("ok"), true},
                      {QStringLiteral("staged"), staged},
                      {QStringLiteral("accepted"), accepted},
                      {QStringLiteral("skipped"), skipped}};
        }
    }
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("type"), type);
    return result;
}

/*
 * Move @p staged into the library under a free form of @p name, never over an existing file. Returns the error ("" on
 * success) and the published path.
 */
QString
publish_new_file(const QString& staged, const QString& name, QString* published) {
    const QDir dir(ImportedFilesModel::libraryRoot());
    if (!QDir().mkpath(dir.absolutePath())) {
        return QStringLiteral("local");
    }
    QSet<QString> skipped = reserved_exports();
    for (int attempt = 0; attempt < kPublishAttempts; attempt++) {
        const QString destination = unique_import_destination(dir, name, skipped);
        if (destination.isEmpty()) {
            break;
        }
        int sourceLeft = 0;
        if (dsd_rename_noreplace(staged.toUtf8().constData(), destination.toUtf8().constData(), &sourceLeft) == 0) {
            *published = destination;
            return {};
        }
        if (errno != EEXIST) {
            break;
        }
        skipped.insert(dir.absoluteFilePath(QFileInfo(destination).fileName()));
    }
    return QStringLiteral("local");
}

} // namespace

ImportedFilesModel::ImportedFilesModel(DecoderHost* host, ImportService* imports, QObject* parent)
    : QAbstractListModel(parent), m_host(host), m_imports(imports) {
    load();
}

ImportedFilesModel::ImportedFilesModel(DecoderHost* host, QObject* parent) : QAbstractListModel(parent), m_host(host) {
    m_imports = new ImportService(host ? host->importTransport() : std::make_shared<DocumentImportTransport>(), this);
    load();
}

// Never calls the service: the app's is destroyed before the models.
ImportedFilesModel::~ImportedFilesModel() = default;

int
ImportedFilesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
ImportedFilesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
        case NameRole: return row.name;
        case PathRole: return row.path;
        case TypeRole: return row.type;
        case ImportedAtRole: return row.importedAt;
        case AcceptedRole: return row.accepted;
        case SkippedRole: return row.skipped;
        default: return provenanceRole(row, role);
    }
}

QVariant
ImportedFilesModel::provenanceRole(const Row& row, int role) {
    switch (role) {
        case OriginRole: return row.origin;
        case RrSidRole: return row.rrSid;
        case RrKindRole: return row.rrKind;
        case RrSiteIdsRole: return row.rrSiteIds;
        default: return QVariant();
    }
}

QHash<int, QByteArray>
ImportedFilesModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles.insert(NameRole, QByteArrayLiteral("name"));
    roles.insert(PathRole, QByteArrayLiteral("path"));
    roles.insert(TypeRole, QByteArrayLiteral("type"));
    roles.insert(ImportedAtRole, QByteArrayLiteral("importedAt"));
    roles.insert(AcceptedRole, QByteArrayLiteral("accepted"));
    roles.insert(SkippedRole, QByteArrayLiteral("skipped"));
    roles.insert(OriginRole, QByteArrayLiteral("origin"));
    roles.insert(RrSidRole, QByteArrayLiteral("rrSid"));
    roles.insert(RrKindRole, QByteArrayLiteral("rrKind"));
    roles.insert(RrSiteIdsRole, QByteArrayLiteral("rrSiteIds"));
    return roles;
}

QStringList
ImportedFilesModel::knownTypes() {
    // Keep in step with validate(), which refuses every other type.
    return {QStringLiteral("chan"),        QStringLiteral("group"),      QStringLiteral("keysDec"),
            QStringLiteral("keysHex"),     QStringLiteral("vertexKeys"), QStringLiteral("dmrTgKeys"),
            QStringLiteral("p25Bandplan"), QStringLiteral("src"),        QStringLiteral("trunkTargets")};
}

bool
ImportedFilesModel::validate(const QString& path, const QString& type, int* accepted, int* skipped, QString* detail) {
    if (detail) {
        detail->clear();
    }
    if (!knownTypes().contains(type)) {
        return false;
    }
    if (type == "trunkTargets") {
        char error[512] = {};
        *skipped = 0;
        const int result =
            dsd_app_trunk_scan_validate_bundle(path.toUtf8().constData(), accepted, nullptr, error, sizeof error);
        if (detail) {
            *detail = QString::fromUtf8(error);
        }
        return result == 0;
    }
    dsd_csv_validation counts = {0U, 0U, 0U};
    const QByteArray local = path.toUtf8();
    int rc = -1;
    if (type == QLatin1String("chan")) {
        rc = dsd_csv_validate_chan_file(local.constData(), &counts);
    } else if (type == QLatin1String("group")) {
        rc = dsd_csv_validate_group_file(local.constData(), &counts);
    } else if (type == QLatin1String("keysDec")) {
        rc = dsd_csv_validate_key_file_dec(local.constData(), &counts);
    } else if (type == QLatin1String("keysHex")) {
        rc = dsd_csv_validate_key_file_hex(local.constData(), &counts);
    } else if (type == QLatin1String("vertexKeys")) {
        rc = dsd_csv_validate_vertex_file(local.constData(), &counts);
    } else if (type == QLatin1String("dmrTgKeys")) {
        dsd_dmr_key_map map = {};
        rc = dsd_dmr_key_map_load(local.constData(), &map);
        counts.accepted = counts.total = static_cast<unsigned int>(map.count);
    } else if (type == QLatin1String("p25Bandplan")) {
        rc = dsd_csv_validate_p25_bandplan_file(local.constData(), &counts);
    } else if (type == QLatin1String("src")) {
        rc = dsd_csv_validate_src_file(local.constData(), &counts);
    }
    if (rc != 0) {
        return false;
    }
    *accepted = static_cast<int>(counts.accepted);
    *skipped = static_cast<int>(counts.skipped);
    return true;
}

QString
ImportedFilesModel::libraryRoot() {
    return json_store_path(QStringLiteral("imports"));
}

QString
ImportedFilesModel::newTalkgroupListPath() const {
    QDir dir(libraryRoot());
    if (!dir.mkpath(QStringLiteral("."))) {
        return {};
    }
    const QString path = dir.absoluteFilePath(
        QStringLiteral("talkgroups-%1.csv").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    reserved_exports().insert(path);
    return path;
}

bool
ImportedFilesModel::registerTalkgroupList(const QString& path) {
    const QFileInfo file(path);
    const QDir dir(libraryRoot());
    if (!file.isFile() || file.isSymLink()
        || file.canonicalPath() != QFileInfo(dir.absolutePath()).canonicalFilePath()) {
        return false;
    }
    if (rowForPath(path) >= 0) {
        return true;
    }
    Row row;
    row.path = path;
    row.name = file.fileName();
    row.type = QStringLiteral("group");
    row.importedAt = realtimeSecsSinceEpoch();
    // Unlike a staged import, failure must never delete the engine's active file.
    if (!validate(path, row.type, &row.accepted, &row.skipped)) {
        return false;
    }
    // Saved first: a row reported as added must still be listed after a restart.
    return appendRow(std::move(row));
}

bool
ImportedFilesModel::appendRow(Row row) {
    row.revision = m_revision + 1;
    auto next = m_rows;
    next.append(std::move(row));
    if (!saveRows(next)) {
        return false;
    }
    m_revision++;
    beginInsertRows(QModelIndex(), count(), count());
    m_rows = std::move(next);
    endInsertRows();
    Q_EMIT countChanged();
    Q_EMIT entriesChanged();
    return true;
}

static bool
writePrivateCsv(const QString& path, const QByteArray& data) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
           && file.write(data) == data.size() && file.commit();
}

bool
ImportedFilesModel::decoderBusy() const {
    return m_host && (m_host->sessionActive() || m_host->transitioning());
}

QString
ImportedFilesModel::bundleReplacementError(int row, const QString& type) const {
    if (row < 0) {
        return {};
    }
    if (row >= count()) {
        return "removed";
    }
    if (m_rows[row].type != type) {
        return "format";
    }
    if (type == "trunkTargets" && decoderBusy()) {
        return "busy";
    }
    return {};
}

QVariantMap
ImportedFilesModel::companionChoices(const QVariantMap& requirement) const {
    auto detail = requirement;
    const QString type = detail.value("type").toString();
    const QString name = detail.value("name").toString();
    detail.insert("kind", csv_companion_type_name(type));
    QVariantList matching, others;
    QStringList warnings;
    if (!detail.value("selectionError").toString().isEmpty()) {
        warnings << detail.value("selectionError").toString();
    }
    // Numeric CSVs can fit several roles, and equal names can belong to different
    // systems. Library kind filters candidates; the user confirms their identity.
    for (const auto& row : m_rows) {
        const bool sameName = QFileInfo(row.name).fileName().compare(name, Qt::CaseInsensitive) == 0;
        if (row.type == type && QFileInfo(row.path).isFile()) {
            QVariantMap candidate{{"name", row.name},
                                  {"path", row.path},
                                  {"type", row.type},
                                  {"kind", csv_companion_type_name(row.type)},
                                  {"accepted", row.accepted}};
            (sameName ? matching : others).append(candidate);
        } else if (sameName) {
            const QString warning = csv_companion_type_error(row.name, row.type, type);
            if (!warning.isEmpty()) {
                warnings << warning;
            }
        }
    }
    detail.insert("preselected", matching.size() == 1 && others.isEmpty() ? matching.first().toMap().value("path")
                                                                          : QVariant(QString()));
    matching.append(others);
    detail.insert("candidates", matching);
    if (matching.size() > 1) {
        warnings.prepend(tr("Several imported files match this role. Choose the file for this system."));
    }
    warnings.removeDuplicates();
    detail.insert("warning", warnings.join('\n'));
    return detail;
}

QVariantMap
ImportedFilesModel::companionDetails(const CsvBundleImport& bundle) const {
    QVariantMap details;
    for (auto i = bundle.requiredDetails.cbegin(); i != bundle.requiredDetails.cend(); ++i) {
        details.insert(i.key(), companionChoices(i.value().toMap()));
    }
    return details;
}

QVariantMap
ImportedFilesModel::importBundle(const QString& reference, const QString& fileName, const QString& type,
                                 const QVariantMap& companions, int replaceRow) {
    if (type != "chan" && type != "trunkTargets") {
        if (replaceRow >= 0) {
            return updateFile(replaceRow, reference, fileName);
        }
        // A pick: the name its provider reports wins over the picker's hint, as in beginImport().
        ImportTarget target;
        const QVariantMap refusal = importRefusal(type, QString(), &target);
        return refusal.isEmpty() ? importNow(reference, QString(), fileName, type, target, QVariantMap()) : refusal;
    }
    const QString problem = bundleReplacementError(replaceRow, type);
    if (!problem.isEmpty()) {
        return {{"ok", false},
                {"error", problem},
                {"detail", problem == "busy" ? "Stop the decoder before replacing target files."
                                             : "Cannot replace this imported file."}};
    }
    QVariantMap libraryEntries;
    for (const auto& row : m_rows) {
        const QString path = QFileInfo(row.path).canonicalFilePath();
        if (!path.isEmpty()) {
            libraryEntries.insert(path, QVariantMap{{"type", row.type}, {"name", row.name}});
        }
    }
    const Row previous = replaceRow >= 0 ? m_rows[replaceRow] : Row();
    const auto bundle =
        type == "trunkTargets"
            ? stage_trunk_csv_bundle(m_host, reference, fileName, companions, previous.bundleRoot, libraryEntries)
            : stage_csv_bundle(m_host, reference, fileName, companions, previous.bundleRoot, libraryEntries);
    if (!bundle.error.isEmpty()) {
        return {{"ok", false},
                {"error", bundle.error},
                {"detail", bundle.detail},
                {"required", bundle.required},
                {"requiredLabels", bundle.requiredLabels},
                {"requiredDetails", companionDetails(bundle)}};
    }
    if (replaceRow < 0) {
        auto result = adoptStoredFile(bundle.path, type, {{"bundleRoot", bundle.root}, {"name", bundle.name}});
        if (!result.value("ok").toBool()) {
            discard_csv_bundle(bundle);
        }
        return result;
    }
    return replaceBundle(replaceRow, bundle, type);
}

static bool
replacementBundleBytes(const CsvBundleImport& bundle, const QString& destination, QByteArray& bytes) {
    QFile staged(bundle.path);
    if (!staged.open(QIODevice::ReadOnly)) {
        return false;
    }
    bytes = staged.readAll();
    if (staged.error() != QFileDevice::NoError) {
        return false;
    }
    staged.close();
    if (!bundle.root.isEmpty()) {
        const QString prefix = QDir(QFileInfo(destination).absolutePath()).relativeFilePath(bundle.root);
        if (prefix != ".") {
            bytes.replace((bundle.revision + '/').toUtf8(), (prefix + '/' + bundle.revision + '/').toUtf8());
        }
    }
    return true;
}

QVariantMap
ImportedFilesModel::replaceBundle(int replaceRow, const CsvBundleImport& bundle, const QString& type) {
    const Row previous = m_rows.at(replaceRow);
    // Versioned companions are immutable. Only the existing primary path is
    // replaced, so every saved reference changes together after validation.
    PrivateCsvBytes storage;
    auto& bytes = storage.bytes;
    if (!replacementBundleBytes(bundle, previous.path, bytes)) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "open"}};
    }
    QTemporaryFile check(QFileInfo(previous.path).absolutePath() + "/.bundle-check-XXXXXX.csv");
    if (!check.open() || !check.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || check.write(bytes) != bytes.size() || !check.flush()) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "copy"}};
    }
    int accepted = 0, skipped = 0;
    QString detail;
    if (!validate(check.fileName(), type, &accepted, &skipped, &detail)) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "format"}, {"detail", detail}};
    }
    QFile old(previous.path);
    if (!old.open(QIODevice::ReadOnly)) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "open"}};
    }
    PrivateCsvBytes backupStorage;
    backupStorage.bytes = old.readAll();
    if (old.error() != QFileDevice::NoError) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "open"}};
    }
    const auto& backup = backupStorage.bytes;
    old.close();
    if (!writePrivateCsv(previous.path, bytes)) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "copy"}};
    }
    auto next = m_rows;
    next[replaceRow].accepted = accepted;
    next[replaceRow].skipped = skipped;
    next[replaceRow].bundleRoot = bundle.root.isEmpty() ? previous.bundleRoot : bundle.root;
    next[replaceRow].importedAt = realtimeSecsSinceEpoch();
    next[replaceRow].origin.clear();
    next[replaceRow].revision = m_revision + 1;
    if (!saveRows(next)) {
        if (writePrivateCsv(previous.path, backup)) {
            discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        }
        return {{"ok", false}, {"error", "copy"}};
    }
    QFile::remove(bundle.path);
    if (type == "trunkTargets") {
        clean_trunk_csv_revisions(previous.path, bundle.root);
    }
    applyReplacement(replaceRow, std::move(next));
    // Kept until the screens read the library's revision: they list entriesForType() again when the count changes.
    Q_EMIT countChanged();
    return {{"ok", true},           {"path", previous.path}, {"name", previous.name},           {"type", type},
            {"accepted", accepted}, {"skipped", skipped},    {"error", accepted ? "" : "empty"}};
}

QVariantMap
ImportedFilesModel::adoptStoredFile(const QString& path, const QString& type, const QVariantMap& origin) {
    int accepted = 0;
    int skipped = 0;
    QString detail;
    if (!validate(path, type, &accepted, &skipped, &detail)) {
        // The copy landed but cannot be parsed as this type; a library row for
        // it would just be a dead entry, so take the copy back out.
        QFile::remove(path);
        auto result = failure(QStringLiteral("open"));
        result.insert(QStringLiteral("detail"), detail);
        return result;
    }
    Row row = newRow(path, type, accepted, skipped, origin);
    if (type == "trunkTargets" && !origin.value("name").toString().isEmpty()) {
        row.name = origin.value("name").toString();
    }
    if (!appendRow(std::move(row))) {
        QFile::remove(path);
        return failure(QStringLiteral("open"));
    }
    return rowResult(m_rows.constLast());
}

ImportedFilesModel::Row
ImportedFilesModel::newRow(const QString& path, const QString& type, int accepted, int skipped,
                           const QVariantMap& origin) {
    Row row;
    row.path = path;
    row.name = QFileInfo(path).fileName();
    row.type = type;
    row.importedAt = realtimeSecsSinceEpoch();
    row.accepted = accepted;
    row.skipped = skipped;
    row.origin = origin.value(QStringLiteral("origin")).toString();
    row.rrSid = stored_int_or(origin.value(QStringLiteral("rrSid")), 0);
    row.rrKind = origin.value(QStringLiteral("rrKind")).toString();
    row.rrSiteIds = origin.value(QStringLiteral("rrSiteIds")).toString();
    row.rrPartialEnc = origin.value(QStringLiteral("rrPartialEnc"), true).toBool();
    row.bundleRoot = origin.value(QStringLiteral("bundleRoot")).toString();
    row.rrEncryptionPolicy =
        stored_int_or(origin.value(QStringLiteral("rrEncryptionPolicy")), row.rrPartialEnc ? 3 : 2);
    return row;
}

QVariantMap
ImportedFilesModel::rowResult(const Row& row) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), row.accepted == 0 ? QStringLiteral("empty") : QString());
    result.insert(QStringLiteral("path"), row.path);
    result.insert(QStringLiteral("name"), row.name);
    result.insert(QStringLiteral("type"), row.type);
    result.insert(QStringLiteral("accepted"), row.accepted);
    result.insert(QStringLiteral("skipped"), row.skipped);
    return result;
}

bool
ImportedFilesModel::replacementBusy(const QString& type) const {
    return type == QLatin1String("trunkTargets") && decoderBusy();
}

QVariantMap
ImportedFilesModel::importRefusal(const QString& type, const QString& replacePath, ImportTarget* target) const {
    const int row = replacePath.isEmpty() ? -1 : rowForPath(replacePath);
    QVariantMap result;
    if (!knownTypes().contains(type)) {
        result = failure(QStringLiteral("format"));
    } else if (!replacePath.isEmpty() && (row < 0 || m_rows[row].type != type)) {
        result = failure(QStringLiteral("path"));
        result.insert(QStringLiteral("detail"), tr("Cannot replace this imported file."));
    } else if (row >= 0 && replacementBusy(type)) {
        result = busy_failure(type);
    } else {
        *target = row >= 0 ? ImportTarget{m_rows[row].path, m_rows[row].revision, type, false}
                           : ImportTarget{QString(), 0, type, false};
        return {};
    }
    result.insert(QStringLiteral("type"), type);
    return result;
}

int
ImportedFilesModel::answerLater(const QVariantMap& result) {
    const auto id = static_cast<int>(ImportService::newRequestId());
    QMetaObject::invokeMethod(this, [this, id, result]() { Q_EMIT importFinished(id, result); }, Qt::QueuedConnection);
    return id;
}

int
ImportedFilesModel::beginImport(const QString& reference, const QString& fileName, const QString& type,
                                const QVariantMap& companions, const QString& replacePath) {
    ImportTarget target;
    const QVariantMap refusal = importRefusal(type, replacePath, &target);
    if (!refusal.isEmpty()) {
        return answerLater(refusal);
    }
    if (type == QLatin1String("chan") || type == QLatin1String("trunkTargets")) {
        // Bundles still build on the Qt thread; their answer comes the way every other one does.
        return answerLater(
            importBundle(reference, fileName, type, companions, replacePath.isEmpty() ? -1 : rowForPath(replacePath)));
    }
    if (!m_imports) {
        QVariantMap result = failure(QStringLiteral("local"));
        result.insert(QStringLiteral("type"), type);
        return answerLater(result);
    }
    const bool before = importing();
    const qint64 id = m_imports->submit(
        [reference, fileName, type](const ImportJob& job) {
            return stage_single_file(job, reference, QString(), fileName, type);
        },
        [self = QPointer<ImportedFilesModel>(this), target](qint64 request, const QVariantMap& staged, bool cancelled) {
            if (self) {
                self->finishImport(request, staged, cancelled, target);
            }
        });
    m_inFlight.insert(id);
    updateImporting(before);
    return static_cast<int>(id);
}

void
ImportedFilesModel::cancelImport(int id) {
    if (m_imports && m_inFlight.contains(id)) {
        m_imports->cancel(id);
    }
}

void
ImportedFilesModel::finishImport(qint64 id, const QVariantMap& staged, bool cancelled, const ImportTarget& target) {
    const bool before = importing();
    m_inFlight.remove(id);
    const QVariantMap result = commitStaged(staged, cancelled, target, QVariantMap());
    Q_EMIT importFinished(static_cast<int>(id), result);
    updateImporting(before);
}

void
ImportedFilesModel::updateImporting(bool before) {
    if (importing() != before) {
        Q_EMIT importingChanged();
    }
}

QVariantMap
ImportedFilesModel::importNow(const QString& reference, const QString& providerName, const QString& hint,
                              const QString& type, const ImportTarget& target, const QVariantMap& origin) {
    QVariantMap result = failure(QStringLiteral("local"));
    if (m_imports) {
        m_imports->runNow(
            [&](const ImportJob& job) { return stage_single_file(job, reference, providerName, hint, type); },
            [&](qint64, const QVariantMap& staged, bool cancelled) {
                result = commitStaged(staged, cancelled, target, origin);
            });
    }
    return result;
}

QVariantMap
ImportedFilesModel::commitStaged(const QVariantMap& staged, bool cancelled, const ImportTarget& target,
                                 const QVariantMap& origin) {
    if (!cancelled && staged.value(QStringLiteral("ok")).toBool()) {
        return target.path.isEmpty() ? commitNewFile(staged, origin) : commitReplacement(staged, target);
    }
    // A request cancelled before its stage, or one with no staging folder, staged no name.
    QVariantMap result = cancelled ? failure(QStringLiteral("cancelled")) : staged;
    result.remove(QStringLiteral("staged"));
    result.insert(QStringLiteral("name"), staged.value(QStringLiteral("name")).toString());
    result.insert(QStringLiteral("type"), target.type);
    return result;
}

QVariantMap
ImportedFilesModel::commitNewFile(const QVariantMap& staged, const QVariantMap& origin) {
    const QString name = staged.value(QStringLiteral("name")).toString();
    const QString type = staged.value(QStringLiteral("type")).toString();
    QString path;
    QString error = publish_new_file(staged.value(QStringLiteral("staged")).toString(), name, &path);
    if (error.isEmpty()) {
        Row row = newRow(path, type, staged.value(QStringLiteral("accepted")).toInt(),
                         staged.value(QStringLiteral("skipped")).toInt(), origin);
        if (appendRow(std::move(row))) {
            return rowResult(m_rows.constLast());
        }
        // A file no row names would never be deleted.
        QFile::remove(path);
        error = QStringLiteral("save");
    }
    QVariantMap result = failure(error);
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("type"), type);
    return result;
}

QVariantMap
ImportedFilesModel::commitReplacement(const QVariantMap& staged, const ImportTarget& target) {
    const int row = rowForPath(target.path);
    QVariantMap result = failure(QStringLiteral("stale"));
    if (row >= 0 && m_rows[row].revision == target.revision && m_rows[row].type == target.type) {
        result = !target.refresh && replacementBusy(target.type) ? busy_failure(target.type)
                                                                 : replaceRowFile(row, staged, target.refresh);
    }
    if (!result.value(QStringLiteral("ok")).toBool()) {
        result.insert(QStringLiteral("name"), staged.value(QStringLiteral("name")).toString());
        result.insert(QStringLiteral("type"), target.type);
    }
    return result;
}

QVariantMap
ImportedFilesModel::replaceRowFile(int row, const QVariantMap& staged, bool refresh) {
    auto next = m_rows;
    Row& stored = next[row];
    stored.importedAt = realtimeSecsSinceEpoch();
    stored.accepted = staged.value(QStringLiteral("accepted")).toInt();
    stored.skipped = staged.value(QStringLiteral("skipped")).toInt();
    stored.revision = m_revision + 1;
    if (!refresh) {
        // The bytes are the user's own now, so the row no longer came from
        // RadioReference. Keeping the provenance would leave "Refresh from
        // RadioReference" on offer for a file it would silently overwrite.
        stored.origin.clear();
        stored.rrSid = 0;
        stored.rrKind.clear();
        stored.rrSiteIds.clear();
        stored.rrPartialEnc = true;
        stored.rrEncryptionPolicy = 0;
    }
    // The index first, the file last: a crash between them leaves the index's counts ahead of a file that is still
    // whole, never a path that names nothing, and the old bytes never need restoring.
    if (!saveRows(next)) {
        return failure(QStringLiteral("save"));
    }
    const QString path = stored.path;
    if (dsd_replace_file_with_temp(staged.value(QStringLiteral("staged")).toString().toUtf8().constData(),
                                   path.toUtf8().constData())
        != 0) {
        const bool noSpace = errno == ENOSPC;
        if (saveRows(m_rows)) {
            QVariantMap result = failure(QStringLiteral("local"));
            result.insert(QStringLiteral("noSpace"), noSpace);
            return result;
        }
        // The index on disk now holds the new state, so the rows in memory follow it; the file keeps its old bytes.
        applyReplacement(row, std::move(next));
        QVariantMap result = failure(QStringLiteral("save"));
        result.insert(QStringLiteral("detail"),
                      tr("The file could not be replaced, and the library could not be put back as it was."));
        return result;
    }
    applyReplacement(row, std::move(next));
    return rowResult(m_rows[row]);
}

void
ImportedFilesModel::applyReplacement(int row, QList<Row> next) {
    m_revision++;
    m_rows = std::move(next);
    Q_EMIT dataChanged(index(row), index(row));
    Q_EMIT entriesChanged();
}

QVariantMap
ImportedFilesModel::importFile(const QString& reference, const QString& fileName, const QString& type) {
    if (type == "trunkTargets") {
        return importBundle(reference, fileName, type, {});
    }
    ImportTarget target;
    const QVariantMap refusal = importRefusal(type, QString(), &target);
    // The caller's name for the copy wins over the document's own (a blank one falls back to it).
    return refusal.isEmpty() ? importNow(reference, fileName, fileName, type, target, QVariantMap()) : refusal;
}

QVariantMap
ImportedFilesModel::importGeneratedFile(const QString& sourcePath, const QString& fileName, const QString& type,
                                        const QVariantMap& origin) {
    return importNow(sourcePath, fileName, QFileInfo(sourcePath).fileName(), type,
                     ImportTarget{QString(), 0, type, false}, origin);
}

QVariantMap
ImportedFilesModel::updateFile(int row, const QString& reference, const QString& fileName) {
    if (row < 0 || row >= m_rows.size()) {
        return failure(QStringLiteral("path"));
    }
    const Row snapshot = m_rows.at(row);
    if (snapshot.type == "trunkTargets") {
        return importBundle(reference, fileName, snapshot.type, {}, row);
    }
    ImportTarget target;
    const QVariantMap refusal = importRefusal(snapshot.type, snapshot.path, &target);
    return refusal.isEmpty() ? importNow(reference, QString(), fileName, snapshot.type, target, QVariantMap())
                             : refusal;
}

QVariantMap
ImportedFilesModel::refreshGeneratedFile(int row, const QString& sourcePath) {
    if (row < 0 || row >= m_rows.size()) {
        return failure(QStringLiteral("path"));
    }
    const Row& stored = m_rows.at(row);
    const ImportTarget target{stored.path, stored.revision, stored.type, true};
    // The path is preserved so saved systems referencing it stay valid, and the
    // copy is validated before the stored file is touched: a refresh that fetched
    // a fault page or a truncated body must leave the working copy as it was.
    return importNow(sourcePath, stored.name, QFileInfo(sourcePath).fileName(), stored.type, target, QVariantMap());
}

static void
appendChannelProfile(const dsd_csv_channel_profile* row, void* context) {
    auto* rows = static_cast<QVariantList*>(context);
    rows->append(
        QVariantMap{{"index", static_cast<int>(row->index)},
                    {"name", QString::fromUtf8(row->name)},
                    {"frequency", QString::number(row->frequency_hz / 1e6, 'f', 6)},
                    {"mode", QString::fromUtf8(row->mode)},
                    {"keySource", row->key_source},
                    {"force", row->force},
                    {"mappings", row->dmr_mapping_count},
                    /* #521: invalid (not 0 or -1) when the row inherits the squelch. */
                    {"squelchDb", row->squelch_db_set ? QVariant(row->squelch_db) : QVariant()},
                    /* Issue #518 follow-up: the row's own auto or noise squelch margin, invalid for a level, and
                       whether it is the noise squelch's. */
                    {"squelchMarginDb", row->squelch_margin_db > 0 ? QVariant(row->squelch_margin_db) : QVariant()},
                    {"squelchNoise", row->squelch_mode == DSD_SQUELCH_MODE_NOISE},
                    /* #526: an analog (nfm or am) row's own channel width in Hz; invalid when it inherits
                                the configured width of its kind. */
                    {"bandwidthHz", row->bandwidth_hz >= 0 ? QVariant(row->bandwidth_hz) : QVariant()},
                    /* #527: an nfm row's own tone policy (dsd_tone_filter_mode) and its displayed list;
                                invalid when it runs the configured policy. */
                    {"toneFilter", row->tone_filter >= 0 ? QVariant(row->tone_filter) : QVariant()},
                    {"toneList", QString::fromUtf8(row->tone_list)},
                    {"receiveSummary", QString::fromUtf8(row->receive_summary)},
                    {"profileRef", QString::fromUtf8(row->profile_ref)}});
}

namespace {
/* The configured receive and tone policies a preview judges each row's service names against (issue #637). */
dsd_scan_option_baseline
configuredBaseline() {
    dsd_scan_option_baseline baseline{};
    dsd_scan_option_baseline_configured(dsd_app_get_latest_opts_snapshot(), dsd_app_get_latest_snapshot(), &baseline);
    return baseline;
}
} // namespace

QVariantMap
ImportedFilesModel::channelProfiles(int row) const {
    if (row < 0 || row >= count() || m_rows[row].type != "chan") {
        return {{"ok", false}, {"rows", QVariantList()}};
    }
    QVariantList rows;
    const dsd_scan_option_baseline baseline = configuredBaseline();
    const bool ok = dsd_csv_inspect_channel_profiles_against(m_rows[row].path.toUtf8().constData(), &baseline, &rows,
                                                             appendChannelProfile)
                    == 0;
    return {{"ok", ok}, {"rows", rows}};
}

bool
ImportedFilesModel::canRemove(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return false;
    }
    return m_rows[row].type != "trunkTargets" || !decoderBusy();
}

bool
ImportedFilesModel::remove(int row) {
    if (!canRemove(row)) {
        return false;
    }
    const auto item = m_rows.at(row);
    // The file goes before the row (issue #801). A file that will not go keeps its row, so the removal can be tried
    // again; a row whose index save fails after it names a file already gone, which a retry finishes and the next
    // load prunes. Dropping the row first could leave a copy, keys included, that no row names and nothing deletes.
    if (!QFile::remove(item.path) && QFileInfo::exists(item.path)) {
        return false;
    }
    if (!item.bundleRoot.isEmpty()) {
        CsvBundleImport bundle;
        bundle.root = item.bundleRoot;
        discard_csv_bundle(bundle);
    }
    auto next = m_rows;
    next.removeAt(row);
    if (!saveRows(next)) {
        return false;
    }
    m_revision++;
    beginRemoveRows(QModelIndex(), row, row);
    m_rows = std::move(next);
    endRemoveRows();
    Q_EMIT countChanged();
    Q_EMIT entriesChanged();
    return true;
}

namespace {
void
appendTargetPreview(const dsd_app_scan_csv_target* target, void* context) {
    static_cast<QVariantList*>(context)->append(QVariantMap{
        {"id", QString::fromUtf8(target->id)},
        {"type", QString::fromUtf8(target->type)},
        {"frequency", QString::number(target->frequency_hz / 1e6, 'f', 6)},
        {"dwellMs", target->dwell_ms},
        {"holdMs", target->hold_ms},
        {"gainDb", target->gain_db},
        /* #521: invalid (not 0 or -1) when the target inherits the squelch. */
        {"squelchDb", target->squelch_db_set ? QVariant(target->squelch_db) : QVariant()},
        {"squelchMarginDb", target->squelch_margin_db > 0 ? QVariant(target->squelch_margin_db) : QVariant()},
        {"squelchNoise", target->squelch_mode == DSD_SQUELCH_MODE_NOISE},
        /* #526: an analog (nfm- or am-conventional) target's own channel width in Hz; invalid when
                       it inherits the configured width of its kind. */
        {"bandwidthHz", target->bandwidth_hz >= 0 ? QVariant(target->bandwidth_hz) : QVariant()},
        /* #527: an nfm-conventional target's own tone policy (dsd_tone_filter_mode) and its displayed
                       list; invalid when it runs the configured policy. */
        {"toneFilter", target->tone_filter >= 0 ? QVariant(target->tone_filter) : QVariant()},
        {"toneList", QString::fromUtf8(target->tone_list)},
        {"receiveSummary", QString::fromUtf8(target->receive_summary)},
        {"modulation", QString::fromUtf8(target->modulation)}});
}
} // namespace

QVariantMap
ImportedFilesModel::targetPreview(const QString& path) const {
    const int row = rowForPath(path);
    if (row < 0 || m_rows[row].type != "trunkTargets") {
        return {{"ok", false}, {"rows", QVariantList()}, {"error", "Select an imported target CSV."}};
    }
    QVariantList rows;
    char error[512] = {};
    const dsd_scan_option_baseline baseline = configuredBaseline();
    const dsd_app_scan_csv_callbacks callbacks{appendTargetPreview, nullptr, &rows, &baseline};
    const bool ok =
        dsd_app_scan_csv_inspect(path.toUtf8().constData(), nullptr, 0, &callbacks, error, sizeof error) == 0;
    return {{"ok", ok}, {"rows", rows}, {"error", QString::fromUtf8(error)}};
}

QVariantMap
ImportedFilesModel::get(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return QVariantMap();
    }
    return mapFromRow(m_rows.at(row));
}

QVariantList
ImportedFilesModel::entriesForType(const QString& type) const {
    QVariantList entries;
    for (const Row& row : m_rows) {
        if (row.type == type) {
            entries.append(mapFromRow(row));
        }
    }
    return entries;
}

int
ImportedFilesModel::rowForPath(const QString& path) const {
    for (int i = 0; i < m_rows.size(); i++) {
        if (m_rows.at(i).path == path) {
            return i;
        }
    }
    return -1;
}

ImportedFilesModel::Row
ImportedFilesModel::rowFromMap(const QVariantMap& map) {
    Row row;
    row.name = map.value(QStringLiteral("name")).toString();
    row.path = map.value(QStringLiteral("path")).toString();
    row.type = map.value(QStringLiteral("type")).toString();
    row.importedAt = map.value(QStringLiteral("importedAt")).toLongLong();
    row.accepted = stored_int_or(map.value(QStringLiteral("accepted")), 0);
    row.skipped = stored_int_or(map.value(QStringLiteral("skipped")), 0);
    row.origin = map.value(QStringLiteral("origin")).toString();
    row.rrSid = stored_int_or(map.value(QStringLiteral("rrSid")), 0);
    row.rrKind = map.value(QStringLiteral("rrKind")).toString();
    row.rrSiteIds = map.value(QStringLiteral("rrSiteIds")).toString();
    row.rrPartialEnc = map.value(QStringLiteral("rrPartialEnc"), true).toBool();
    row.bundleRoot = map.value(QStringLiteral("bundleRoot")).toString();
    row.rrEncryptionPolicy = stored_int_or(map.value(QStringLiteral("rrEncryptionPolicy")), row.rrPartialEnc ? 3 : 2);
    return row;
}

QVariantMap
ImportedFilesModel::mapFromRow(const Row& row) {
    QVariantMap map;
    map.insert(QStringLiteral("name"), row.name);
    map.insert(QStringLiteral("path"), row.path);
    map.insert(QStringLiteral("type"), row.type);
    map.insert(QStringLiteral("importedAt"), row.importedAt);
    map.insert(QStringLiteral("accepted"), row.accepted);
    map.insert(QStringLiteral("skipped"), row.skipped);
    // This map feeds JSON persistence AND the QML-facing field maps get() and
    // entriesForType() return, so provenance reaches both from one place.
    map.insert(QStringLiteral("origin"), row.origin);
    map.insert(QStringLiteral("rrSid"), row.rrSid);
    map.insert(QStringLiteral("rrKind"), row.rrKind);
    map.insert(QStringLiteral("rrSiteIds"), row.rrSiteIds);
    map.insert(QStringLiteral("rrPartialEnc"), row.rrPartialEnc);
    map.insert(QStringLiteral("rrEncryptionPolicy"), row.rrEncryptionPolicy);
    map.insert(QStringLiteral("bundleRoot"), row.bundleRoot);
    return map;
}

void
ImportedFilesModel::load() {
    QList<Row> rows;
    const QJsonArray array = json_store_load_array(QLatin1String(kStoreFileName));
    for (const auto& value : array) {
        if (!value.isObject()) {
            continue;
        }
        Row row = rowFromMap(value.toObject().toVariantMap());
        // A copy Android or the user deleted behind the app's back must not
        // survive as a ghost row pointing nowhere.
        if (row.path.isEmpty() || !QFile::exists(row.path)) {
            if (!row.path.isEmpty() && !m_pruned_paths.contains(row.path)) {
                m_pruned_paths.append(row.path);
            }
            continue;
        }
        if (row.type == "trunkTargets" && m_host && !m_host->sessionActive() && !m_host->transitioning()) {
            clean_trunk_csv_revisions(row.path, row.bundleRoot);
        }
        row.revision = ++m_revision;
        rows.append(row);
    }
    const bool pruned = rows.size() != array.size();
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT entriesChanged();
    if (pruned) {
        // Persist the prune, or every launch re-stats files that are gone and
        // the index keeps advertising rows the library no longer has.
        save();
    }
}

QStringList
ImportedFilesModel::takePrunedPaths() {
    QStringList paths;
    paths.swap(m_pruned_paths);
    return paths;
}

bool
ImportedFilesModel::save() const {
    return saveRows(m_rows);
}

bool
ImportedFilesModel::saveRows(const QList<Row>& rows) const {
    QJsonArray array;
    for (const Row& row : rows) {
        array.append(QJsonObject::fromVariantMap(mapFromRow(row)));
    }
    return json_store_save_array(QLatin1String(kStoreFileName), array);
}

} // namespace dsd_qt
