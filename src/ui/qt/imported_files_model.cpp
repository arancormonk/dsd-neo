// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "imported_files_model.h"

#include <QByteArray>
#include <QChar>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QMap>
#include <QVariant>

#include <dsd-neo/core/csv_validate.h>

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
        case OriginRole: return row.origin;
        case RrSidRole: return row.rrSid;
        case RrSiteNumberRole: return row.rrSiteNumber;
        case RrKindRole: return row.rrKind;
        case RrSiteNumbersRole: return row.rrSiteNumbers;
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
    roles.insert(RrSiteNumberRole, QByteArrayLiteral("rrSiteNumber"));
    roles.insert(RrKindRole, QByteArrayLiteral("rrKind"));
    roles.insert(RrSiteNumbersRole, QByteArrayLiteral("rrSiteNumbers"));
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
    }
    if (rc != 0) {
        return false;
    }
    *accepted = static_cast<int>(counts.accepted);
    *skipped = static_cast<int>(counts.skipped);
    return true;
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
    row.rrSiteNumber = origin.value(QStringLiteral("rrSiteNumber")).toInt();
    row.rrKind = origin.value(QStringLiteral("rrKind")).toString();
    row.rrSiteNumbers = origin.value(QStringLiteral("rrSiteNumbers")).toString();

    beginInsertRows(QModelIndex(), static_cast<int>(m_rows.size()), static_cast<int>(m_rows.size()));
    m_rows.append(row);
    endInsertRows();
    Q_EMIT countChanged();
    save();

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
ImportedFilesModel::commitReplacedRow(int row, int accepted, int skipped) {
    Row& stored = m_rows[row];
    stored.importedAt = QDateTime::currentSecsSinceEpoch();
    stored.accepted = accepted;
    stored.skipped = skipped;
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx);
    save();

    QVariantMap result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), accepted == 0 ? QStringLiteral("empty") : QString());
    result.insert(QStringLiteral("path"), stored.path);
    result.insert(QStringLiteral("name"), stored.name);
    result.insert(QStringLiteral("type"), stored.type);
    result.insert(QStringLiteral("accepted"), accepted);
    result.insert(QStringLiteral("skipped"), skipped);
    return result;
}

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
        // The staging name resolved to the row's own file, so it is already
        // committed and validated.
        return commitReplacedRow(row, accepted, skipped);
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
    return commitReplacedRow(row, accepted, skipped);
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
    const QString path = m_host->importLocalFile(sourcePath, snapshot.name, snapshot.path);
    if (path != snapshot.path || path.isEmpty()) {
        if (!path.isEmpty()) {
            QFile::remove(path);
        }
        return result;
    }
    return commitReplacedRow(row, accepted, skipped);
}

void
ImportedFilesModel::remove(int row) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }
    QFile::remove(m_rows.at(row).path);
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
    row.rrSiteNumber = map.value(QStringLiteral("rrSiteNumber")).toInt();
    row.rrKind = map.value(QStringLiteral("rrKind")).toString();
    row.rrSiteNumbers = map.value(QStringLiteral("rrSiteNumbers")).toString();
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
    map.insert(QStringLiteral("rrSiteNumber"), row.rrSiteNumber);
    map.insert(QStringLiteral("rrKind"), row.rrKind);
    map.insert(QStringLiteral("rrSiteNumbers"), row.rrSiteNumbers);
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

void
ImportedFilesModel::save() const {
    QJsonArray array;
    for (const Row& row : m_rows) {
        array.append(QJsonObject::fromVariantMap(mapFromRow(row)));
    }
    json_store_save_array(QLatin1String(kStoreFileName), array);
}

} // namespace dsd_qt
