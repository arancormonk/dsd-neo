// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <cerrno>
#include <memory>
#include <qflags.h>
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
#include <dsd-neo/core/managed_file.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/scan_options.h>

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

/* documentInspected()'s result. */
QVariantMap
inspection(const QString& kind, const QString& name) {
    return {{QStringLiteral("kind"), kind}, {QStringLiteral("name"), name}};
}

/* What a probe that ended with @p probed found: the document's kind once read, else why it was not. */
QString
inspection_kind(int probed, const ImportProbe& probe) {
    if (probed == DecoderHost::DocumentCopied) {
        return backup::incoming_kind_name(backup::classify_incoming(probe.header, probe.total));
    }
    return probed == DecoderHost::DocumentCancelled ? QStringLiteral("cancelled") : QStringLiteral("unreadable");
}

QVariantMap
busy_failure() {
    auto result = failure(QStringLiteral("busy"));
    result.insert(QStringLiteral("detail"), ImportedFilesModel::tr("Stop the decoder before replacing target files."));
    return result;
}

/* The managed-file lock (<dsd-neo/core/managed_file.h>) for one replacement of a row's file: the decoder rewrites and
   appends to the session's group list under the same lock, so its check and write never interleave with the move
   here. Holds the file operation only; nothing in it waits for the decoder. */
struct ManagedFileLock {
    ManagedFileLock() { dsd_managed_file_lock(); }

    ~ManagedFileLock() { dsd_managed_file_unlock(); }

    ManagedFileLock(const ManagedFileLock&) = delete;
    ManagedFileLock& operator=(const ManagedFileLock&) = delete;
};

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
 * A parser's reason names files by the paths it read in the request's staging folder: each of @p files (a staged
 * path and the name the user picked it by) is put back to that name. A reason still naming the staging folder (a path
 * no entry covers) is dropped, so the message falls back to the generic one rather than show an app-private path.
 */
QString
user_reason(QString detail, QList<std::pair<QString, QString>> files, const QString& stagingDir) {
    // Longer paths first, so no path is replaced inside a longer one.
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [path, name] : files) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.isEmpty() && canonical != path) {
            detail.replace(canonical, name);
        }
        detail.replace(path, name);
    }
    const QString canonicalStaging = QFileInfo(stagingDir).canonicalFilePath();
    if (detail.contains(stagingDir) || (!canonicalStaging.isEmpty() && detail.contains(canonicalStaging))) {
        return {};
    }
    return detail;
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
            result.insert(QStringLiteral("detail"), user_reason(detail, {{staged, name}}, job.stagingDir));
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
    if (!QDir().mkpath(dir.absolutePath())
        || !ImportService::publish(staged, dir, name, reserved_exports(), published)) {
        return QStringLiteral("local");
    }
    return {};
}

/*
 * Bundles (#832): a channel map or target list and its companions, built on the worker in a mirror of the library
 * inside the request's staging folder and moved into the library file by file at the commit.
 */

/* The staging folder's fetch: each document a bundle reads, copied by the request's transport into a fresh file. */
ImportFetch
make_fetch(const ImportJob& job) {
    const QString inbox = QDir(job.stagingDir).filePath(QStringLiteral("in"));
    auto next = std::make_shared<int>(0);
    return [job, inbox, next](const QString& reference, const QString& hint, qint64 maxBytes) {
        ImportFetchResult copy;
        copy.path = inbox + QLatin1Char('/') + QString::number((*next)++);
        copy.code = QDir().mkpath(inbox) ? job.transport->copy(job.id, reference, copy.path, maxBytes)
                                         : DecoderHost::DocumentLocalFailed;
        const ImportCopyProgress progress = job.transport->progress(job.id);
        copy.name = sanitized_import_name(progress.displayName, hint);
        copy.size = progress.total;
        copy.noSpace = progress.noSpace;
        copy.bytes = copy.code == DecoderHost::DocumentCopied ? QFileInfo(copy.path).size() : 0;
        return copy;
    };
}

/* A bundle stage's answer: the result keys, plus "staged" (the mirror's primary) and "bundle" (its layout). */
QVariantMap
bundle_map(const CsvBundleImport& bundle, const QString& mirrorRoot) {
    QVariantMap result{{QStringLiteral("ok"), bundle.error.isEmpty()},
                       {QStringLiteral("error"), bundle.error},
                       {QStringLiteral("detail"), bundle.detail},
                       {QStringLiteral("name"), bundle.name},
                       {QStringLiteral("required"), bundle.required},
                       {QStringLiteral("requiredLabels"), bundle.requiredLabels},
                       {QStringLiteral("requiredDetails"), bundle.requiredDetails}};
    if (bundle.error == QLatin1String("too_large")) {
        result.insert(QStringLiteral("limitBytes"), bundle.limitBytes);
    } else if (bundle.error == QLatin1String("local")) {
        result.insert(QStringLiteral("noSpace"), bundle.noSpace);
    }
    result.insert(QStringLiteral("staged"), mirrorRoot + QLatin1Char('/') + bundle.primaryRelative);
    result.insert(QStringLiteral("bundle"), QVariantMap{{QStringLiteral("mirrorRoot"), mirrorRoot},
                                                        {QStringLiteral("primary"), bundle.primaryRelative},
                                                        {QStringLiteral("root"), bundle.rootRelative},
                                                        {QStringLiteral("revision"), bundle.revisionRelative},
                                                        {QStringLiteral("files"), bundle.stagedRelative},
                                                        {QStringLiteral("newRoot"), bundle.newRoot}});
    return result;
}

/* A bundle's stage, on the import worker: build it in the mirror, then validate the mirror's primary as the library
   will hold it. */
QVariantMap
stage_bundle(const ImportJob& job, const QVariantMap& request) {
    const QString type = request.value(QStringLiteral("type")).toString();
    const BundleTarget target{QDir(job.stagingDir).filePath(QStringLiteral("lib")),
                              request.value(QStringLiteral("primaryRelative")).toString(),
                              request.value(QStringLiteral("existingRootRelative")).toString(), kMaxBundleImportBytes,
                              request.value(QStringLiteral("confineCompanions")).toBool()};
    const ImportFetch fetch = make_fetch(job);
    const QString reference = request.value(QStringLiteral("reference")).toString();
    const QString fileName = request.value(QStringLiteral("fileName")).toString();
    const QVariantMap companions = request.value(QStringLiteral("companions")).toMap();
    const QVariantMap entries = request.value(QStringLiteral("libraryEntries")).toMap();
    const CsvBundleImport bundle = type == QLatin1String("trunkTargets")
                                       ? stage_trunk_csv_bundle(fetch, target, reference, fileName, companions, entries)
                                       : stage_csv_bundle(fetch, target, reference, fileName, companions, entries);
    QVariantMap result = bundle_map(bundle, target.mirrorRoot);
    result.insert(QStringLiteral("type"), type);
    QList<std::pair<QString, QString>> files;
    for (auto i = bundle.stagedNames.cbegin(); i != bundle.stagedNames.cend(); ++i) {
        files.append({target.mirrorRoot + QLatin1Char('/') + i.key(), i.value()});
    }
    if (!bundle.error.isEmpty()) {
        result.insert(QStringLiteral("detail"), user_reason(bundle.detail, files, job.stagingDir));
        return result;
    }
    int accepted = 0;
    int skipped = 0;
    QString detail;
    const QString staged = result.value(QStringLiteral("staged")).toString();
    if (!ImportedFilesModel::validate(staged, type, &accepted, &skipped, &detail)) {
        result.insert(QStringLiteral("ok"), false);
        result.insert(QStringLiteral("error"), QStringLiteral("format"));
        result.insert(QStringLiteral("detail"), user_reason(detail, files, job.stagingDir));
        return result;
    }
    result.insert(QStringLiteral("accepted"), accepted);
    result.insert(QStringLiteral("skipped"), skipped);
    return result;
}

/* Take back the directories a bundle commit made, and their journal entries. */
void
discard_created(const QStringList& created) {
    for (const QString& dir : created) {
        QDir(dir).removeRecursively();
    }
    ImportService::journalRemove(created);
}

/* Make @p dir, which must not exist yet: an existing one is never taken for the commit's own. */
bool
make_new_dir(const QString& dir) {
    const QFileInfo info(dir);
    return QDir().mkpath(info.absolutePath())
           && QDir(info.absolutePath())
                  .mkdir(info.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

/*
 * The first part of a bundle commit: journal its new directory (a root, or a revision in an existing root), make it,
 * and move every companion from the mirror into it. Returns the error ("" on success); @p created then lists the
 * journaled directory. A bundle without companions has nothing to place.
 */
QString
place_bundle_files(const QVariantMap& staged, QStringList* created) {
    const QVariantMap layout = staged.value(QStringLiteral("bundle")).toMap();
    const QString revision = layout.value(QStringLiteral("revision")).toString();
    if (layout.value(QStringLiteral("root")).toString().isEmpty()) {
        return {};
    }
    const QString library = ImportedFilesModel::libraryRoot() + QLatin1Char('/');
    const bool newRoot = layout.value(QStringLiteral("newRoot")).toBool();
    const QString dir = library + (newRoot ? layout.value(QStringLiteral("root")).toString() : revision);
    if (!ImportService::journalAdd({dir})) {
        return QStringLiteral("local");
    }
    if (!make_new_dir(dir)) {
        ImportService::journalRemove({dir});
        return QStringLiteral("local");
    }
    *created = QStringList{dir};
    const QString mirror = layout.value(QStringLiteral("mirrorRoot")).toString() + QLatin1Char('/');
    bool placed = !newRoot || make_new_dir(library + revision);
    // Only the revision's files move here; the primary is published (or replaced) last.
    for (const QString& file : layout.value(QStringLiteral("files")).toStringList()) {
        if (placed && file.startsWith(revision + QLatin1Char('/'))) {
            int sourceLeft = 0;
            placed = dsd_rename_noreplace((mirror + file).toUtf8().constData(), (library + file).toUtf8().constData(),
                                          &sourceLeft)
                     == 0;
        }
    }
    if (!placed) {
        discard_created(*created);
        created->clear();
        return QStringLiteral("local");
    }
    return {};
}

/* A replace's move: the staged copy over the stored file, in one rename. */
int
replace_with_staged(const QString& staged, const QString& path) {
    return dsd_replace_file_with_temp(staged.toUtf8().constData(), path.toUtf8().constData());
}

} // namespace

ImportedFilesModel::ImportedFilesModel(DecoderHost* host, ImportService* imports, QObject* parent)
    : QAbstractListModel(parent), m_host(host), m_imports(imports), m_replaceFile(&replace_with_staged) {
    load();
}

ImportedFilesModel::ImportedFilesModel(DecoderHost* host, QObject* parent)
    : QAbstractListModel(parent), m_host(host), m_replaceFile(&replace_with_staged) {
    m_imports = new ImportService(host ? host->importTransport() : std::make_shared<DocumentImportTransport>(), this);
    load();
}

void
ImportedFilesModel::setReplaceFile(ReplaceFile replaceFile) {
    m_replaceFile = std::move(replaceFile);
}

void
ImportedFilesModel::setBeforeUserReplace(BeforeUserReplace hook) {
    m_beforeUserReplace = std::move(hook);
}

/*
 * The app's service is destroyed before the models, so this reaches only a service that outlives the model (or is its
 * child, not yet destroyed): no probe goes on reading for a model that is gone. Their commits find no model and
 * answer nothing.
 */
ImportedFilesModel::~ImportedFilesModel() {
    if (m_imports) {
        for (const qint64 id : std::as_const(m_probes)) {
            m_imports->cancel(id);
        }
    }
}

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

namespace {

/* Dry-run @p local with the parser of library type @p type (any type but trunkTargets); the parser's return code. */
int
validate_csv_kind(const QByteArray& local, const QString& type, dsd_csv_validation* counts) {
    if (type == QLatin1String("chan")) {
        return dsd_csv_validate_chan_file(local.constData(), counts);
    }
    if (type == QLatin1String("group")) {
        return dsd_csv_validate_group_file(local.constData(), counts);
    }
    if (type == QLatin1String("keysDec")) {
        return dsd_csv_validate_key_file_dec(local.constData(), counts);
    }
    if (type == QLatin1String("keysHex")) {
        return dsd_csv_validate_key_file_hex(local.constData(), counts);
    }
    if (type == QLatin1String("vertexKeys")) {
        return dsd_csv_validate_vertex_file(local.constData(), counts);
    }
    if (type == QLatin1String("dmrTgKeys")) {
        dsd_dmr_key_map map = {};
        const int rc = dsd_dmr_key_map_load(local.constData(), &map);
        counts->accepted = counts->total = static_cast<unsigned int>(map.count);
        return rc;
    }
    if (type == QLatin1String("p25Bandplan")) {
        return dsd_csv_validate_p25_bandplan_file(local.constData(), counts);
    }
    if (type == QLatin1String("src")) {
        return dsd_csv_validate_src_file(local.constData(), counts);
    }
    return -1;
}

} // namespace

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
    // The parsers give their reason for refusing a file only to the log: keep it for the import's answer.
    char reason[512] = {};
    dsd_neo_log_capture_error(reason, sizeof reason);
    const int rc = validate_csv_kind(path.toUtf8(), type, &counts);
    dsd_neo_log_capture_error(nullptr, 0);
    if (rc != 0) {
        if (detail) {
            *detail = QString::fromUtf8(reason).trimmed();
            if (!detail->isEmpty()) {
                (*detail)[0] = detail->at(0).toUpper();
            }
        }
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

bool
ImportedFilesModel::decoderBusy() const {
    return m_host && (m_host->sessionActive() || m_host->transitioning());
}

bool
ImportedFilesModel::cleanupBlocked() const {
    return m_host == nullptr || decoderBusy() || m_host->engineBusy();
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
ImportedFilesModel::companionDetails(const QVariantMap& requiredDetails) const {
    QVariantMap details;
    for (auto i = requiredDetails.cbegin(); i != requiredDetails.cend(); ++i) {
        details.insert(i.key(), companionChoices(i.value().toMap()));
    }
    return details;
}

QVariantMap
ImportedFilesModel::bundleRequest(const QString& reference, const QString& fileName, const QString& type,
                                  const QVariantMap& companions, const ImportTarget& target) const {
    QVariantMap libraryEntries;
    for (const auto& row : m_rows) {
        const QString path = QFileInfo(row.path).canonicalFilePath();
        if (!path.isEmpty()) {
            libraryEntries.insert(path, QVariantMap{{"type", row.type}, {"name", row.name}});
        }
    }
    // Read here, on the Qt thread, so the worker's stage never asks the host (issue #830: where DSD-neo browses the
    // whole device, a CSV finds companions by path only in its own folder tree).
    QVariantMap request{{QStringLiteral("reference"), reference},
                        {QStringLiteral("fileName"), fileName},
                        {QStringLiteral("type"), type},
                        {QStringLiteral("companions"), companions},
                        {QStringLiteral("libraryEntries"), libraryEntries},
                        {QStringLiteral("confineCompanions"), m_host && m_host->fileBrowserSupported()}};
    const int row = target.path.isEmpty() ? -1 : rowForPath(target.path);
    if (row >= 0) {
        // Where the replaced row's files lie in the library, so the stage builds the new ones at the same places.
        const QDir library(libraryRoot());
        request.insert(QStringLiteral("primaryRelative"), QDir::cleanPath(library.relativeFilePath(m_rows[row].path)));
        if (!m_rows[row].bundleRoot.isEmpty()) {
            request.insert(QStringLiteral("existingRootRelative"),
                           QDir::cleanPath(library.relativeFilePath(m_rows[row].bundleRoot)));
        }
    }
    return request;
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
    // A talkgroup list is not held back (issue #748): its move runs under the managed-file lock, the decoder refuses
    // to save over a list it did not reload, and the Imports screen asks a running session to reload the file.
    return type == QLatin1String("trunkTargets") && decoderBusy();
}

QVariantMap
ImportedFilesModel::importRefusal(const QString& type, const QString& replacePath, ImportTarget* target) const {
    const int row = replacePath.isEmpty() ? -1 : rowForPath(replacePath);
    QVariantMap result;
    if (!knownTypes().contains(type)) {
        result = failure(QStringLiteral("format"));
    } else if (!replacePath.isEmpty() && row < 0) {
        // The row the pick was asked for went while the picker was up (issue #829).
        result = failure(QStringLiteral("changed"));
        result.insert(QStringLiteral("detail"), tr("That file changed while you were picking. Try again."));
    } else if (!replacePath.isEmpty() && m_rows[row].type != type) {
        result = failure(QStringLiteral("path"));
        result.insert(QStringLiteral("detail"), tr("Cannot replace this imported file."));
    } else if (row >= 0 && replacementBusy(type)) {
        result = busy_failure();
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
    answerQueued(id, result);
    return id;
}

void
ImportedFilesModel::answerQueued(int id, const QVariantMap& result) {
    QMetaObject::invokeMethod(this, [this, id, result]() { Q_EMIT importFinished(id, result); }, Qt::QueuedConnection);
}

int
ImportedFilesModel::beginImport(const QString& reference, const QString& fileName, const QString& type,
                                const QVariantMap& companions, const QString& replacePath) {
    ImportTarget target;
    const QVariantMap refusal = importRefusal(type, replacePath, &target);
    if (!refusal.isEmpty()) {
        return answerLater(refusal);
    }
    if (!m_imports) {
        QVariantMap result = failure(QStringLiteral("local"));
        result.insert(QStringLiteral("type"), type);
        return answerLater(result);
    }
    // A channel map or a target list builds with its companions; any other type is one file.
    target.bundle = type == QLatin1String("chan") || type == QLatin1String("trunkTargets");
    ImportService::Stage stage = [reference, fileName, type](const ImportJob& job) {
        return stage_single_file(job, reference, QString(), fileName, type);
    };
    if (target.bundle) {
        stage = [request = bundleRequest(reference, fileName, type, companions, target)](const ImportJob& job) {
            return stage_bundle(job, request);
        };
    }
    const bool before = importing();
    const qint64 id =
        m_imports->submit(std::move(stage), [self = QPointer<ImportedFilesModel>(this),
                                             target](qint64 request, const QVariantMap& staged, bool cancelled) {
            if (self) {
                self->finishImport(request, staged, cancelled, target);
            }
        });
    m_inFlight.insert(id, InFlight{sanitized_import_name(QString(), fileName), type});
    updateImporting(before);
    return static_cast<int>(id);
}

void
ImportedFilesModel::cancelImport(int id) {
    const auto it = m_inFlight.constFind(id);
    if (!m_imports || it == m_inFlight.cend()) {
        return;
    }
    // The commit runs on this thread and has not run yet, so the outcome is settled now: the answer goes at once
    // rather than after a copy a provider may never let go of. The copy is still stopped where it can be.
    m_imports->cancel(id);
    QVariantMap result = failure(QStringLiteral("cancelled"));
    result.insert(QStringLiteral("name"), it->name);
    result.insert(QStringLiteral("type"), it->type);
    const bool before = importing();
    m_inFlight.erase(it);
    m_dropped.insert(id);
    answerQueued(id, result);
    updateImporting(before);
}

int
ImportedFilesModel::inspectDocument(const QString& reference) {
    if (!m_imports) {
        const auto id = static_cast<int>(ImportService::newRequestId());
        answerInspection(id, inspection(QStringLiteral("unreadable"), QString()));
        return id;
    }
    const qint64 id = m_imports->probeDocument(
        reference, backup::kMaxSealedBytes,
        [self = QPointer<ImportedFilesModel>(this)](qint64 request, int probed, const ImportProbe& probe) {
            if (self) {
                self->finishInspect(request, probed, probe);
            }
        });
    m_probes.insert(id);
    return static_cast<int>(id);
}

void
ImportedFilesModel::cancelInspect(int id) {
    if (!m_probes.remove(id)) {
        return;
    }
    // As cancelImport(): answered now, not after a read a provider may hold.
    if (m_imports) {
        m_imports->cancel(id);
    }
    answerInspection(id, inspection(QStringLiteral("cancelled"), QString()));
}

void
ImportedFilesModel::finishInspect(qint64 id, int probed, const ImportProbe& probe) {
    if (m_probes.remove(id)) {
        Q_EMIT documentInspected(static_cast<int>(id), inspection(inspection_kind(probed, probe), probe.displayName));
    }
}

void
ImportedFilesModel::answerInspection(int id, const QVariantMap& result) {
    QMetaObject::invokeMethod(
        this, [this, id, result]() { Q_EMIT documentInspected(id, result); }, Qt::QueuedConnection);
}

void
ImportedFilesModel::finishImport(qint64 id, const QVariantMap& staged, bool cancelled, const ImportTarget& target) {
    const bool before = importing();
    m_inFlight.remove(id);
    // Answered at its cancel: the commit only lets the staged files go.
    const bool dropped = m_dropped.remove(id);
    const QVariantMap result = commitStaged(staged, cancelled || dropped, target, QVariantMap());
    if (!dropped) {
        Q_EMIT importFinished(static_cast<int>(id), result);
    }
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
        if (target.bundle) {
            return commitBundle(staged, target);
        }
        return target.path.isEmpty() ? commitNewFile(staged, origin) : commitReplacement(staged, target);
    }
    // A request cancelled before its stage, or one with no staging folder, staged no name.
    QVariantMap result = cancelled ? failure(QStringLiteral("cancelled")) : staged;
    result.remove(QStringLiteral("staged"));
    result.remove(QStringLiteral("bundle"));
    if (result.contains(QStringLiteral("requiredDetails"))) {
        // The library's choices for each companion the bundle still needs.
        result.insert(QStringLiteral("requiredDetails"),
                      companionDetails(result.value(QStringLiteral("requiredDetails")).toMap()));
    }
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

int
ImportedFilesModel::currentRow(const ImportTarget& target) const {
    const int row = rowForPath(target.path);
    return row >= 0 && m_rows[row].revision == target.revision && m_rows[row].type == target.type ? row : -1;
}

QVariantMap
ImportedFilesModel::commitReplacement(const QVariantMap& staged, const ImportTarget& target) {
    const int row = currentRow(target);
    QVariantMap result = failure(QStringLiteral("stale"));
    if (row >= 0) {
        result = !target.refresh && replacementBusy(target.type) ? busy_failure()
                                                                 : replaceRowFile(row, staged, target.refresh);
    }
    if (!result.value(QStringLiteral("ok")).toBool()) {
        result.insert(QStringLiteral("name"), staged.value(QStringLiteral("name")).toString());
        result.insert(QStringLiteral("type"), target.type);
    }
    return result;
}

QVariantMap
ImportedFilesModel::commitBundle(const QVariantMap& staged, const ImportTarget& target) {
    const bool plain =
        staged.value(QStringLiteral("bundle")).toMap().value(QStringLiteral("root")).toString().isEmpty();
    if (target.path.isEmpty() && plain) {
        // A channel map that names no companion is one file, stored under a free name.
        return commitNewFile(staged, QVariantMap());
    }
    const int row = target.path.isEmpty() ? -1 : currentRow(target);
    QVariantMap result;
    if (!target.path.isEmpty() && row < 0) {
        result = failure(QStringLiteral("stale"));
    } else if (row >= 0 && replacementBusy(target.type)) {
        result = busy_failure();
    } else {
        QStringList created;
        const QString error = place_bundle_files(staged, &created);
        result = !error.isEmpty() ? failure(error)
                 : row < 0        ? publishBundle(staged, created)
                                  : replaceBundleRow(row, staged, created);
    }
    if (!result.value(QStringLiteral("ok")).toBool()) {
        result.insert(QStringLiteral("name"), staged.value(QStringLiteral("name")).toString());
        result.insert(QStringLiteral("type"), target.type);
    }
    return result;
}

QVariantMap
ImportedFilesModel::publishBundle(const QVariantMap& staged, const QStringList& created) {
    const QVariantMap layout = staged.value(QStringLiteral("bundle")).toMap();
    const QString type = staged.value(QStringLiteral("type")).toString();
    const QString path = libraryRoot() + QLatin1Char('/') + layout.value(QStringLiteral("primary")).toString();
    int sourceLeft = 0;
    // The root is new and the commit's own, so the primary's name is free there.
    if (dsd_rename_noreplace(staged.value(QStringLiteral("staged")).toString().toUtf8().constData(),
                             path.toUtf8().constData(), &sourceLeft)
        != 0) {
        discard_created(created);
        return failure(QStringLiteral("local"));
    }
    const QString root = libraryRoot() + QLatin1Char('/') + layout.value(QStringLiteral("root")).toString();
    Row row = newRow(path, type, staged.value(QStringLiteral("accepted")).toInt(),
                     staged.value(QStringLiteral("skipped")).toInt(), {{QStringLiteral("bundleRoot"), root}});
    if (type == QLatin1String("trunkTargets")) {
        // Every target list is stored as targets.csv in its own root; the row keeps the document's name.
        row.name = staged.value(QStringLiteral("name")).toString();
    }
    if (!appendRow(std::move(row))) {
        discard_created(created);
        return failure(QStringLiteral("save"));
    }
    ImportService::journalRemove(created);
    return rowResult(m_rows.constLast());
}

QVariantMap
ImportedFilesModel::replaceBundleRow(int row, const QVariantMap& staged, const QStringList& created) {
    QVariantMap replacement = staged;
    const QString root = staged.value(QStringLiteral("bundle")).toMap().value(QStringLiteral("root")).toString();
    if (!root.isEmpty()) {
        replacement.insert(QStringLiteral("bundleRoot"), libraryRoot() + QLatin1Char('/') + root);
    }
    QVariantMap result = replaceRowFile(row, replacement, false, created);
    if (result.value(QStringLiteral("ok")).toBool()) {
        const Row& stored = m_rows[row];
        if (!stored.bundleRoot.isEmpty()) {
            reclaimRevisions({BundleRevisions{stored.path, stored.bundleRoot, stored.type}});
        }
    }
    return result;
}

void
ImportedFilesModel::reclaimRevisions(const QList<BundleRevisions>& bundles) {
    // While the decoder runs it may still read an older revision's files (B5).
    if (bundles.isEmpty() || !m_imports || cleanupBlocked()) {
        return;
    }
    // Every row, not only these bundles': another row's CSV may still need a file in one of their revisions.
    const QList<LibraryFile> library = libraryFiles(m_rows);
    m_imports->submit(
        [bundles, library](const ImportJob&) {
            clean_bundle_revisions(bundles, library, &ImportService::journalEntries);
            return QVariantMap();
        },
        [](qint64, const QVariantMap&, bool) {});
}

QVariantMap
ImportedFilesModel::replaceRowFile(int row, const QVariantMap& staged, bool refresh, const QStringList& created) {
    // A RadioReference file replaced with the user's own bytes no longer describes the site the rows naming it came
    // from (a file the user picked never did, so its update asks nothing of them). Those rows let go of it first, as a
    // removal clears references before the file goes: a failure here changes nothing, and one after leaves the rows
    // cleared over the file's old bytes, which is safe.
    const Row& current = m_rows[row];
    const bool fromRadioReference = current.origin == QLatin1String("radioreference") || current.rrSid > 0;
    if (!refresh && fromRadioReference && m_beforeUserReplace && !m_beforeUserReplace(current.path)) {
        discard_created(created);
        return failure(QStringLiteral("save"));
    }
    auto next = m_rows;
    Row& stored = next[row];
    stored.importedAt = realtimeSecsSinceEpoch();
    stored.accepted = staged.value(QStringLiteral("accepted")).toInt();
    stored.skipped = staged.value(QStringLiteral("skipped")).toInt();
    stored.revision = m_revision + 1;
    if (staged.contains(QStringLiteral("bundleRoot"))) {
        stored.bundleRoot = staged.value(QStringLiteral("bundleRoot")).toString();
    }
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
        discard_created(created);
        return failure(QStringLiteral("save"));
    }
    int moved = -1;
    int moveErrno = 0;
    {
        // The move is one step for the decoder's group list writer: its check and rewrite of the session's list never
        // straddle it (issue #748).
        const ManagedFileLock lock;
        moved = m_replaceFile(staged.value(QStringLiteral("staged")).toString(), stored.path);
        moveErrno = errno;
        if (moved == 0) {
            dsd_managed_file_step_reached(DSD_MANAGED_FILE_STEP_INSTALLED);
        }
    }
    if (moved != 0) {
        const bool noSpace = moveErrno == ENOSPC;
        if (saveRows(m_rows)) {
            discard_created(created);
            QVariantMap result = failure(QStringLiteral("local"));
            result.insert(QStringLiteral("noSpace"), noSpace);
            return result;
        }
        // The index on disk now holds the new state, so the rows in memory follow it; the file keeps its old bytes.
        // The new directories stay journaled: the next start keeps the ones the index references.
        applyReplacement(row, std::move(next));
        QVariantMap result = failure(QStringLiteral("save"));
        result.insert(QStringLiteral("detail"),
                      tr("The file could not be replaced, and the library could not be put back as it was."));
        return result;
    }
    ImportService::journalRemove(created);
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
ImportedFilesModel::importGeneratedFile(const QString& sourcePath, const QString& fileName, const QString& type,
                                        const QVariantMap& origin) {
    return importNow(sourcePath, fileName, QFileInfo(sourcePath).fileName(), type,
                     ImportTarget{QString(), 0, type, false}, origin);
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
        remove_bundle_root(item.bundleRoot);
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

QVariantMap
ImportedFilesModel::groupListCounts(const QString& path) const {
    int accepted = 0;
    int skipped = 0;
    const bool ok = !path.isEmpty() && validate(path, QStringLiteral("group"), &accepted, &skipped);
    return {{"ok", ok}, {"accepted", ok ? accepted : 0}, {"skipped", ok ? skipped : 0}};
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
    JsonStoreLoad status = JsonStoreLoad::Unreadable;
    const QJsonArray array = json_store_load_array(QLatin1String(kStoreFileName), &status);
    QList<Row> stored;
    for (const auto& value : array) {
        if (value.isObject()) {
            stored.append(rowFromMap(value.toObject().toVariantMap()));
        }
    }
    // An index that cannot be read references nothing, which proves nothing about the journal. A running decoder may
    // still read a journaled revision: the journal then waits for a start that finds it stopped.
    if (status != JsonStoreLoad::Unreadable && !cleanupBlocked()) {
        recoverJournal(stored);
    }
    QList<Row> rows;
    QList<BundleRevisions> bundles;
    for (Row& row : stored) {
        // A copy Android or the user deleted behind the app's back must not
        // survive as a ghost row pointing nowhere. Only a confirmed absence
        // drops it: a folder that cannot be searched for now is no proof.
        if (row.path.isEmpty() || json_store_definitely_missing(row.path)) {
            if (!row.path.isEmpty() && !m_pruned_paths.contains(row.path)) {
                m_pruned_paths.append(row.path);
            }
            continue;
        }
        if (!row.bundleRoot.isEmpty()) {
            bundles.append(BundleRevisions{row.path, row.bundleRoot, row.type});
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
    reclaimRevisions(bundles);
}

namespace {
/* Whether a row of @p rows references the journaled directory @p dir: as its bundle root, or as a revision of its root
   that its primary reaches (or might: a primary that cannot be parsed keeps every revision). */
bool
journaled_dir_referenced(const QString& dir, const QList<BundleRevisions>& bundles) {
    for (const auto& bundle : bundles) {
        const QString root = QDir::cleanPath(bundle.root);
        if (dir == root) {
            return true;
        }
        QSet<QString> reached;
        if (QFileInfo(dir).absolutePath() == root
            && (!bundle_reached_revisions(bundle.path, root, bundle.type, &reached)
                || reached.contains(QFileInfo(dir).fileName()))) {
            return true;
        }
    }
    return false;
}
} // namespace

QList<LibraryFile>
ImportedFilesModel::libraryFiles(const QList<Row>& rows) {
    QList<LibraryFile> files;
    files.reserve(rows.size());
    for (const Row& row : rows) {
        files.append(LibraryFile{row.path, row.type});
    }
    return files;
}

void
ImportedFilesModel::recoverJournal(const QList<Row>& rows) {
    bool ok = false;
    const QStringList entries = ImportService::journalEntries(&ok);
    if (!ok) {
        // Nothing it names can be proven the app's now; it is set aside so later commits can journal again.
        ImportService::journalSetAside();
        return;
    }
    if (entries.isEmpty()) {
        return;
    }
    // A saved system, scan list or profile, or another row's CSV, may need a file in a journaled directory (a replace
    // whose entry outlived its commit leaves an old revision only they reach). A store or stored file that cannot be
    // read may need any of them: then nothing goes, and the journal keeps every entry for the next start.
    QStringList named;
    if (!kept_library_files(libraryFiles(rows), &named)) {
        return;
    }
    QList<BundleRevisions> bundles;
    for (const Row& row : rows) {
        if (!row.bundleRoot.isEmpty()) {
            bundles.append(BundleRevisions{row.path, row.bundleRoot, row.type});
        }
    }
    QStringList settled;
    for (const QString& dir : entries) {
        const bool owned = owned_bundle_root(dir) || owned_bundle_revision(dir);
        // A crash between a commit's journal entry and its index save left this; nothing names it, so it goes.
        const bool unnamed = !journaled_dir_referenced(dir, bundles) && !holds_named_file(dir, named);
        if (owned && unnamed && !QDir(dir).removeRecursively()) {
            continue;
        }
        settled.append(dir);
    }
    ImportService::journalRemove(settled);
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
