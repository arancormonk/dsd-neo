// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QIODevice>
#include <utility>
#include "imported_files_model.h"

#include <QByteArray>
#include <QChar>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QMap>
#include <QUuid>
#include <QVariant>

#include <dsd-neo/core/csv_validate.h>

#include <QSaveFile>
#include <QTemporaryFile>
#include <dsd-neo/core/dmr_key_map.h>
#include "csv_bundle_import.h"
#include "decoder_host.h"
#include "json_store.h"

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "imported_files.json";

} // namespace

ImportedFilesModel::ImportedFilesModel(DecoderHost* host, QObject* parent) : QAbstractListModel(parent), m_host(host) {
    load();
}

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

bool
ImportedFilesModel::validate(const QString& path, const QString& type, int* accepted, int* skipped) {
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
ImportedFilesModel::newTalkgroupListPath() const {
    QDir dir(json_store_path(QStringLiteral("imports")));
    if (!dir.mkpath(QStringLiteral("."))) {
        return {};
    }
    return dir.absoluteFilePath(
        QStringLiteral("talkgroups-%1.csv").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
}

bool
ImportedFilesModel::registerTalkgroupList(const QString& path) {
    const QFileInfo file(path);
    const QDir dir(json_store_path(QStringLiteral("imports")));
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
    row.importedAt = QDateTime::currentSecsSinceEpoch();
    // Unlike a staged import, failure must never delete the engine's active file.
    if (!validate(path, row.type, &row.accepted, &row.skipped)) {
        return false;
    }
    beginInsertRows({}, count(), count());
    m_rows.append(row);
    endInsertRows();
    Q_EMIT countChanged();
    save();
    return true;
}

static bool
writePrivateCsv(const QString& path, const QByteArray& data) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
           && file.write(data) == data.size() && file.commit();
}

QVariantMap
ImportedFilesModel::importBundle(const QString& reference, const QString& fileName, const QString& type,
                                 const QVariantMap& companions, int replaceRow) {
    if (type != "chan") {
        return replaceRow >= 0 ? updateFile(replaceRow, reference, fileName) : importFile(reference, fileName, type);
    }
    const Row previous = replaceRow >= 0 && replaceRow < count() ? m_rows[replaceRow] : Row();
    if (replaceRow >= 0 && previous.path.isEmpty()) {
        return {{"ok", false}, {"error", "removed"}};
    }
    const auto bundle = stage_csv_bundle(m_host, reference, fileName, companions, previous.bundleRoot);
    if (!bundle.error.isEmpty()) {
        return {{"ok", false}, {"error", bundle.error}, {"required", bundle.required}};
    }
    if (replaceRow < 0) {
        auto result = adoptStoredFile(bundle.path, type, {{"bundleRoot", bundle.root}});
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
    QByteArray bytes;
    if (!replacementBundleBytes(bundle, previous.path, bytes)) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "open"}};
    }
    QTemporaryFile check(QFileInfo(previous.path).absolutePath() + "/.bundle-check-XXXXXX.csv");
    if (!check.open() || check.write(bytes) != bytes.size() || !check.flush()) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "copy"}};
    }
    int accepted = 0, skipped = 0;
    if (!validate(check.fileName(), type, &accepted, &skipped)) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "format"}};
    }
    QFile old(previous.path);
    if (!old.open(QIODevice::ReadOnly)) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "open"}};
    }
    const QByteArray backup = old.readAll();
    old.close();
    if (!writePrivateCsv(previous.path, bytes)) {
        discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        return {{"ok", false}, {"error", "copy"}};
    }
    auto next = m_rows;
    next[replaceRow].accepted = accepted;
    next[replaceRow].skipped = skipped;
    next[replaceRow].bundleRoot = bundle.root.isEmpty() ? previous.bundleRoot : bundle.root;
    next[replaceRow].importedAt = QDateTime::currentSecsSinceEpoch();
    next[replaceRow].origin.clear();
    if (!saveRows(next)) {
        if (writePrivateCsv(previous.path, backup)) {
            discard_csv_bundle(bundle, !previous.bundleRoot.isEmpty());
        }
        return {{"ok", false}, {"error", "copy"}};
    }
    QFile::remove(bundle.path);
    m_rows = std::move(next);
    Q_EMIT dataChanged(index(replaceRow), index(replaceRow));
    Q_EMIT countChanged();
    return {{"ok", true},           {"path", previous.path}, {"name", previous.name},           {"type", type},
            {"accepted", accepted}, {"skipped", skipped},    {"error", accepted ? "" : "empty"}};
}

QVariantMap
ImportedFilesModel::adoptStoredFile(const QString& path, const QString& type, const QVariantMap& origin) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), QStringLiteral("open"));

    int accepted = 0;
    int skipped = 0;
    if (!validate(path, type, &accepted, &skipped)) {
        // The copy landed but cannot be parsed as this type; a library row for
        // it would just be a dead entry, so take the copy back out.
        QFile::remove(path);
        return result;
    }

    Row row;
    row.path = path;
    row.name = path.section(QLatin1Char('/'), -1);
    row.type = type;
    row.importedAt = QDateTime::currentSecsSinceEpoch();
    row.accepted = accepted;
    row.skipped = skipped;
    row.origin = origin.value(QStringLiteral("origin")).toString();
    row.rrSid = origin.value(QStringLiteral("rrSid")).toInt();
    row.rrKind = origin.value(QStringLiteral("rrKind")).toString();
    row.rrSiteIds = origin.value(QStringLiteral("rrSiteIds")).toString();
    row.rrPartialEnc = origin.value(QStringLiteral("rrPartialEnc"), true).toBool();
    row.bundleRoot = origin.value(QStringLiteral("bundleRoot")).toString();
    row.rrEncryptionPolicy = origin.value(QStringLiteral("rrEncryptionPolicy"), row.rrPartialEnc ? 3 : 2).toInt();

    auto next = m_rows;
    next.append(row);
    if (!saveRows(next)) {
        QFile::remove(path);
        return result;
    }
    beginInsertRows(QModelIndex(), static_cast<int>(m_rows.size()), static_cast<int>(m_rows.size()));
    m_rows = std::move(next);
    endInsertRows();
    Q_EMIT countChanged();

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), accepted == 0 ? QStringLiteral("empty") : QString());
    result.insert(QStringLiteral("path"), row.path);
    result.insert(QStringLiteral("name"), row.name);
    result.insert(QStringLiteral("type"), row.type);
    result.insert(QStringLiteral("accepted"), accepted);
    result.insert(QStringLiteral("skipped"), skipped);
    return result;
}

QVariantMap
ImportedFilesModel::importFile(const QString& reference, const QString& fileName, const QString& type) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), QStringLiteral("open"));
    if (m_host == nullptr) {
        return result;
    }

    const QString path = m_host->importDocument(reference, fileName);
    if (path.isEmpty()) {
        return result;
    }
    return adoptStoredFile(path, type, QVariantMap());
}

QVariantMap
ImportedFilesModel::importGeneratedFile(const QString& sourcePath, const QString& fileName, const QString& type,
                                        const QVariantMap& origin) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), QStringLiteral("open"));
    if (m_host == nullptr) {
        return result;
    }

    const QString path = m_host->importLocalFile(sourcePath, fileName);
    if (path.isEmpty()) {
        return result;
    }
    return adoptStoredFile(path, type, origin);
}

/**
 * @brief Fill the result map from a row that was just replaced in place.
 */
QVariantMap
ImportedFilesModel::commitReplacedRow(int row, int accepted, int skipped, bool keepProvenance) {
    auto next = m_rows;
    Row& stored = next[row];
    stored.importedAt = QDateTime::currentSecsSinceEpoch();
    stored.accepted = accepted;
    stored.skipped = skipped;
    if (!keepProvenance) {
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
    if (!saveRows(next)) {
        return {{"ok", false}, {"error", "save"}};
    }
    m_rows = std::move(next);
    const Row& committed = m_rows[row];
    QVariantMap result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), accepted == 0 ? QStringLiteral("empty") : QString());
    result.insert(QStringLiteral("path"), committed.path);
    result.insert(QStringLiteral("name"), committed.name);
    result.insert(QStringLiteral("type"), committed.type);
    result.insert(QStringLiteral("accepted"), accepted);
    result.insert(QStringLiteral("skipped"), skipped);
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx);
    return result;
}

namespace {
struct CsvReplacementBackup {
    QString path;
    QByteArray bytes;
    bool existed, valid = true;

    explicit CsvReplacementBackup(const QString& destination) : path(destination), existed(QFile::exists(path)) {
        if (!existed) {
            return;
        }
        QFile file(path);
        valid = file.open(QIODevice::ReadOnly);
        if (valid) {
            bytes = file.readAll();
            valid = file.error() == QFileDevice::NoError;
        }
    }

    void
    restore() const {
        if (existed) {
            (void)writePrivateCsv(path, bytes);
        } else {
            QFile::remove(path);
        }
    }
};
} // namespace

QVariantMap
ImportedFilesModel::updateFile(int row, const QString& reference, const QString& fileName) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), QStringLiteral("open"));
    if (m_host == nullptr || row < 0 || row >= m_rows.size()) {
        return result;
    }

    const Row snapshot = m_rows.at(row);

    // Stage the pick beside the library instead of straight over the row's file.
    // Replacing first and validating afterwards has no rollback: a file that is
    // not parseable as this row's type would clobber a working CSV, leave the row
    // advertising stale counts, and hand every saved system pointing at that path
    // an unusable -G/-C.
    const QString staged = m_host->importDocument(reference, fileName);
    if (staged.isEmpty()) {
        return result;
    }

    int accepted = 0;
    int skipped = 0;
    if (!validate(staged, snapshot.type, &accepted, &skipped)) {
        QFile::remove(staged);
        return result;
    }

    if (staged == snapshot.path) {
        return commitReplacedRow(row, accepted, skipped, false);
    }

    CsvReplacementBackup backup(snapshot.path);
    if (!backup.valid) {
        QFile::remove(staged);
        return result;
    }
    const QString path = m_host->importLocalFile(staged, snapshot.name, snapshot.path);
    QFile::remove(staged);
    if (path != snapshot.path || path.isEmpty()) {
        // The host wrote somewhere other than the row's file (its replace target
        // was not usable), so the copy that just landed belongs to no row and
        // nothing else would ever delete it. Same rollback importFile() does.
        if (!path.isEmpty()) {
            QFile::remove(path);
        }
        return result;
    }
    result = commitReplacedRow(row, accepted, skipped, false);
    if (!result.value("ok").toBool()) {
        backup.restore();
    }
    return result;
}

QVariantMap
ImportedFilesModel::refreshGeneratedFile(int row, const QString& sourcePath) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), QStringLiteral("open"));
    if (m_host == nullptr || row < 0 || row >= m_rows.size()) {
        return result;
    }

    const Row snapshot = m_rows.at(row);
    int accepted = 0;
    int skipped = 0;
    // Validate the staging file first: a refresh that fetched a fault page or a
    // truncated body must leave the stored copy untouched.
    if (!validate(sourcePath, snapshot.type, &accepted, &skipped)) {
        return result;
    }

    // The path is preserved so saved systems referencing it stay valid.
    CsvReplacementBackup backup(snapshot.path);
    if (!backup.valid) {
        return result;
    }
    const QString path = m_host->importLocalFile(sourcePath, snapshot.name, snapshot.path);
    if (path != snapshot.path || path.isEmpty()) {
        if (!path.isEmpty()) {
            QFile::remove(path);
        }
        return result;
    }
    result = commitReplacedRow(row, accepted, skipped);
    if (!result.value("ok").toBool()) {
        backup.restore();
    }
    return result;
}

static void
appendChannelProfile(const dsd_csv_channel_profile* row, void* context) {
    auto* rows = static_cast<QVariantList*>(context);
    rows->append(QVariantMap{{"index", static_cast<int>(row->index)},
                             {"name", QString::fromUtf8(row->name)},
                             {"frequency", QString::number(row->frequency_hz / 1e6, 'f', 6)},
                             {"mode", QString::fromUtf8(row->mode)},
                             {"keySource", row->key_source},
                             {"force", row->force},
                             {"mappings", row->dmr_mapping_count},
                             {"profileRef", QString::fromUtf8(row->profile_ref)}});
}

QVariantMap
ImportedFilesModel::channelProfiles(int row) const {
    if (row < 0 || row >= count() || m_rows[row].type != "chan") {
        return {{"ok", false}, {"rows", QVariantList()}};
    }
    QVariantList rows;
    const bool ok =
        dsd_csv_inspect_channel_profiles(m_rows[row].path.toUtf8().constData(), &rows, appendChannelProfile) == 0;
    return {{"ok", ok}, {"rows", rows}};
}

void
ImportedFilesModel::remove(int row) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }
    const auto item = m_rows.at(row);
    QFile::remove(item.path);
    if (!item.bundleRoot.isEmpty()) {
        CsvBundleImport bundle;
        bundle.root = item.bundleRoot;
        discard_csv_bundle(bundle);
    }
    beginRemoveRows(QModelIndex(), row, row);
    m_rows.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
    save();
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
    row.accepted = map.value(QStringLiteral("accepted")).toInt();
    row.skipped = map.value(QStringLiteral("skipped")).toInt();
    row.origin = map.value(QStringLiteral("origin")).toString();
    row.rrSid = map.value(QStringLiteral("rrSid")).toInt();
    row.rrKind = map.value(QStringLiteral("rrKind")).toString();
    row.rrSiteIds = map.value(QStringLiteral("rrSiteIds")).toString();
    row.rrPartialEnc = map.value(QStringLiteral("rrPartialEnc"), true).toBool();
    row.bundleRoot = map.value(QStringLiteral("bundleRoot")).toString();
    row.rrEncryptionPolicy = map.value(QStringLiteral("rrEncryptionPolicy"), row.rrPartialEnc ? 3 : 2).toInt();
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
        rows.append(row);
    }
    const bool pruned = rows.size() != array.size();
    beginResetModel();
    m_rows = rows;
    endResetModel();
    Q_EMIT countChanged();
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
