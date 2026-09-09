// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "saved_systems_model.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QMap>
#include <QMetaType>
#include <QSet>
#include <QUuid>
#include <QVariant>

#include "json_store.h"

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "saved_systems.json";

/**
 * @brief The bias-tee tri-state from a stored value, migrating legacy bools.
 *
 * Legacy stores saved the wizard's on/off switch as a bool. true was an
 * explicit choice; false was the untouched default, whose observed behavior
 * was to follow the app-wide pref — it must read as follow (-1), never as a
 * frozen off. Out-of-range ints collapse to follow as well.
 */
int
bias_tee_from_stored(const QVariant& value) {
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool() ? 1 : -1;
    }
    const int mode = value.toInt();
    return (mode >= -1 && mode <= 1) ? mode : -1;
}

} // namespace

SavedSystemsModel::SavedSystemsModel(QObject* parent) : QAbstractListModel(parent) { load(); }

SavedSystemsModel::~SavedSystemsModel() = default;

int
SavedSystemsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

/** @brief The identity half of the role switch; see tuningRoleValue(). */
QVariant
SavedSystemsModel::identityRoleValue(const Row& row, int role) {
    switch (role) {
        case UidRole: return row.uid;
        case EncKeyTypeRole: return row.encKeyType;
        case EncKeyValueRole: return row.encKeyValue;
        case EncForceKeyRole: return row.encForceKey;
        case RrSidRole: return row.rrSid;
        case RrSiteIdRole: return row.rrSiteId;
        case SiteNameRole: return row.siteName;
        case SiteLatRole: return row.siteLat;
        case SiteLonRole: return row.siteLon;
        case HasSitePosRole: return row.hasSitePos;
        case AvoidSiteRole: return row.avoidSite;
        case NameRole: return row.name;
        case SourceTypeRole: return row.sourceType;
        case HostRole: return row.host;
        case PortRole: return row.port;
        case FreqMhzRole: return row.freqMhz;
        case DecodeFlagRole: return row.decodeFlag;
        case FilePathRole: return row.filePath;
        default: return QVariant();
    }
}

/** @brief The tuning/recency half of the role switch, split to keep each simple. */
QVariant
SavedSystemsModel::tuningRoleValue(const Row& row, int role) {
    switch (role) {
        case TrunkingRole: return row.trunking;
        case GainDbRole: return row.gainDb;
        case PpmRole: return row.ppm;
        case BandwidthKhzRole: return row.bandwidthKhz;
        case BiasTeeRole: return row.biasTee;
        case ExtraArgsRole: return row.extraArgs;
        case LastHeardRole: return row.lastHeard;
        case ChanCsvPathRole: return row.chanCsvPath;
        case GroupCsvPathRole: return row.groupCsvPath;
        case KeyCsvPathRole: return row.keyCsvPath;
        case KeyCsvHexRole: return row.keyCsvHex;
        case P25BandplanCsvPathRole: return row.p25BandplanCsvPath;
        case SrcCsvPathRole: return row.srcCsvPath;
        default: return QVariant();
    }
}

QVariant
SavedSystemsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    const QVariant identity = identityRoleValue(row, role);
    return identity.isValid() ? identity : tuningRoleValue(row, role);
}

QHash<int, QByteArray>
SavedSystemsModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles.insert(NameRole, QByteArrayLiteral("name"));
    roles.insert(SourceTypeRole, QByteArrayLiteral("sourceType"));
    roles.insert(HostRole, QByteArrayLiteral("host"));
    roles.insert(PortRole, QByteArrayLiteral("port"));
    roles.insert(FreqMhzRole, QByteArrayLiteral("freqMhz"));
    roles.insert(DecodeFlagRole, QByteArrayLiteral("decodeFlag"));
    roles.insert(TrunkingRole, QByteArrayLiteral("trunking"));
    roles.insert(GainDbRole, QByteArrayLiteral("gainDb"));
    roles.insert(PpmRole, QByteArrayLiteral("ppm"));
    roles.insert(BandwidthKhzRole, QByteArrayLiteral("bandwidthKhz"));
    roles.insert(BiasTeeRole, QByteArrayLiteral("biasTee"));
    roles.insert(ExtraArgsRole, QByteArrayLiteral("extraArgs"));
    roles.insert(FilePathRole, QByteArrayLiteral("filePath"));
    roles.insert(LastHeardRole, QByteArrayLiteral("lastHeard"));
    roles.insert(ChanCsvPathRole, QByteArrayLiteral("chanCsvPath"));
    roles.insert(GroupCsvPathRole, QByteArrayLiteral("groupCsvPath"));
    roles.insert(KeyCsvPathRole, QByteArrayLiteral("keyCsvPath"));
    roles.insert(KeyCsvHexRole, QByteArrayLiteral("keyCsvHex"));
    roles.insert(P25BandplanCsvPathRole, QByteArrayLiteral("p25BandplanCsvPath"));
    roles.insert(SrcCsvPathRole, QByteArrayLiteral("srcCsvPath"));
    roles.insert(UidRole, QByteArrayLiteral("uid"));
    roles.insert(EncKeyTypeRole, QByteArrayLiteral("encKeyType"));
    roles.insert(EncKeyValueRole, QByteArrayLiteral("encKeyValue"));
    roles.insert(EncForceKeyRole, QByteArrayLiteral("encForceKey"));
    roles.insert(RrSidRole, QByteArrayLiteral("rrSid"));
    roles.insert(RrSiteIdRole, QByteArrayLiteral("rrSiteId"));
    roles.insert(SiteNameRole, QByteArrayLiteral("siteName"));
    roles.insert(SiteLatRole, QByteArrayLiteral("siteLat"));
    roles.insert(SiteLonRole, QByteArrayLiteral("siteLon"));
    roles.insert(HasSitePosRole, QByteArrayLiteral("hasSitePos"));
    roles.insert(AvoidSiteRole, QByteArrayLiteral("avoidSite"));
    return roles;
}

namespace {

/**
 * @brief Overwrite @p target only when @p key is present; absent keys keep the base value.
 *
 * @p key is a QString so callers can pass QStringLiteral: building one from a
 * `const char*` here would heap-allocate on every field of every stored row.
 */
void
map_take_string(const QVariantMap& map, const QString& key, QString* target) {
    if (map.contains(key)) {
        *target = map.value(key).toString();
    }
}

void
map_take_int(const QVariantMap& map, const QString& key, int* target) {
    if (map.contains(key)) {
        *target = map.value(key).toInt();
    }
}

void
map_take_bool(const QVariantMap& map, const QString& key, bool* target) {
    if (map.contains(key)) {
        *target = map.value(key).toBool();
    }
}

} // namespace

SavedSystemsModel::Row
SavedSystemsModel::rowFromMap(const QVariantMap& map, const Row& base) {
    Row row = base;
    // An edit cannot change identity. Loading can adopt a valid persisted UUID;
    // add() replaces it afterwards so duplicating a row never duplicates identity.
    if (row.uid.isEmpty()) {
        const QUuid stored(map.value(QStringLiteral("uid")).toString());
        row.uid = (stored.isNull() ? QUuid::createUuid() : stored).toString(QUuid::WithoutBraces);
    }
    if (map.contains(QStringLiteral("encKeyType"))) {
        row.encKeyType = map.value(QStringLiteral("encKeyType")).toInt();
    }
    if (map.contains(QStringLiteral("encKeyValue"))) {
        row.encKeyValue = map.value(QStringLiteral("encKeyValue")).toString();
    }
    if (map.contains(QStringLiteral("encForceKey"))) {
        row.encForceKey = map.value(QStringLiteral("encForceKey")).toInt();
    }
    if (map.contains(QStringLiteral("rrSid"))) {
        row.rrSid = map.value(QStringLiteral("rrSid")).toInt();
    }
    if (map.contains(QStringLiteral("rrSiteId"))) {
        row.rrSiteId = map.value(QStringLiteral("rrSiteId")).toInt();
    }
    if (map.contains(QStringLiteral("siteName"))) {
        row.siteName = map.value(QStringLiteral("siteName")).toString();
    }
    if (map.contains(QStringLiteral("siteLat"))) {
        row.siteLat = map.value(QStringLiteral("siteLat")).toDouble();
    }
    if (map.contains(QStringLiteral("siteLon"))) {
        row.siteLon = map.value(QStringLiteral("siteLon")).toDouble();
    }
    if (map.contains(QStringLiteral("hasSitePos"))) {
        row.hasSitePos = map.value(QStringLiteral("hasSitePos")).toBool();
    }
    if (map.contains(QStringLiteral("avoidSite"))) {
        row.avoidSite = map.value(QStringLiteral("avoidSite")).toBool();
    }

    map_take_string(map, QStringLiteral("name"), &row.name);
    map_take_string(map, QStringLiteral("sourceType"), &row.sourceType);
    map_take_string(map, QStringLiteral("host"), &row.host);
    map_take_int(map, QStringLiteral("port"), &row.port);
    map_take_string(map, QStringLiteral("freqMhz"), &row.freqMhz);
    map_take_string(map, QStringLiteral("decodeFlag"), &row.decodeFlag);
    map_take_bool(map, QStringLiteral("trunking"), &row.trunking);
    map_take_int(map, QStringLiteral("gainDb"), &row.gainDb);
    map_take_string(map, QStringLiteral("ppm"), &row.ppm);
    map_take_int(map, QStringLiteral("bandwidthKhz"), &row.bandwidthKhz);
    if (map.contains(QStringLiteral("biasTee"))) {
        row.biasTee = bias_tee_from_stored(map.value(QStringLiteral("biasTee")));
    }
    map_take_string(map, QStringLiteral("extraArgs"), &row.extraArgs);
    map_take_string(map, QStringLiteral("filePath"), &row.filePath);
    if (map.contains(QStringLiteral("lastHeard"))) {
        row.lastHeard = map.value(QStringLiteral("lastHeard")).toLongLong();
    }
    map_take_string(map, QStringLiteral("chanCsvPath"), &row.chanCsvPath);
    map_take_string(map, QStringLiteral("groupCsvPath"), &row.groupCsvPath);
    map_take_string(map, QStringLiteral("keyCsvPath"), &row.keyCsvPath);
    map_take_bool(map, QStringLiteral("keyCsvHex"), &row.keyCsvHex);
    map_take_string(map, QStringLiteral("p25BandplanCsvPath"), &row.p25BandplanCsvPath);
    map_take_string(map, QStringLiteral("srcCsvPath"), &row.srcCsvPath);
    return row;
}

QVariantMap
SavedSystemsModel::mapFromRow(const Row& row) const {
    QVariantMap map;
    map.insert(QStringLiteral("uid"), row.uid);
    map.insert(QStringLiteral("encKeyType"), row.encKeyType);
    map.insert(QStringLiteral("encKeyValue"), row.encKeyValue);
    map.insert(QStringLiteral("encForceKey"), row.encForceKey);
    map.insert(QStringLiteral("rrSid"), row.rrSid);
    map.insert(QStringLiteral("rrSiteId"), row.rrSiteId);
    map.insert(QStringLiteral("siteName"), row.siteName);
    map.insert(QStringLiteral("siteLat"), row.siteLat);
    map.insert(QStringLiteral("siteLon"), row.siteLon);
    map.insert(QStringLiteral("hasSitePos"), row.hasSitePos);
    map.insert(QStringLiteral("avoidSite"), row.avoidSite);

    map.insert(QStringLiteral("name"), row.name);
    map.insert(QStringLiteral("sourceType"), row.sourceType);
    map.insert(QStringLiteral("host"), row.host);
    map.insert(QStringLiteral("port"), row.port);
    map.insert(QStringLiteral("freqMhz"), row.freqMhz);
    map.insert(QStringLiteral("decodeFlag"), row.decodeFlag);
    map.insert(QStringLiteral("trunking"), row.trunking);
    map.insert(QStringLiteral("gainDb"), row.gainDb);
    map.insert(QStringLiteral("ppm"), row.ppm);
    map.insert(QStringLiteral("bandwidthKhz"), row.bandwidthKhz);
    map.insert(QStringLiteral("biasTee"), row.biasTee);
    map.insert(QStringLiteral("extraArgs"), row.extraArgs);
    map.insert(QStringLiteral("filePath"), row.filePath);
    map.insert(QStringLiteral("lastHeard"), row.lastHeard);
    map.insert(QStringLiteral("chanCsvPath"), row.chanCsvPath);
    map.insert(QStringLiteral("groupCsvPath"), row.groupCsvPath);
    map.insert(QStringLiteral("keyCsvPath"), row.keyCsvPath);
    map.insert(QStringLiteral("keyCsvHex"), row.keyCsvHex);
    map.insert(QStringLiteral("p25BandplanCsvPath"), row.p25BandplanCsvPath);
    map.insert(QStringLiteral("srcCsvPath"), row.srcCsvPath);
    return map;
}

void
SavedSystemsModel::add(const QVariantMap& system) {
    beginInsertRows(QModelIndex(), static_cast<int>(m_rows.size()), static_cast<int>(m_rows.size()));
    Row row = rowFromMap(system, Row());
    row.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_rows.append(row);
    endInsertRows();
    Q_EMIT countChanged();
    Q_EMIT mostRecentRowChanged();
    save();
}

void
SavedSystemsModel::update(int row, const QVariantMap& system) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }
    m_rows[row] = rowFromMap(system, m_rows.at(row));
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx);
    save();
}

void
SavedSystemsModel::remove(int row) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), row, row);
    m_rows.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
    Q_EMIT mostRecentRowChanged();
    save();
}

QVariantMap
SavedSystemsModel::get(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return QVariantMap();
    }
    return mapFromRow(m_rows.at(row));
}

int
SavedSystemsModel::rowForUid(const QString& uid) const {
    if (!uid.isEmpty()) {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i).uid == uid) {
                return i;
            }
        }
    }
    return -1;
}

QVariantMap
SavedSystemsModel::getByUid(const QString& uid) const {
    return get(rowForUid(uid));
}

void
SavedSystemsModel::touch(int row) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }
    m_rows[row].lastHeard = QDateTime::currentSecsSinceEpoch();
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, {LastHeardRole});
    Q_EMIT mostRecentRowChanged();
    save();
}

QStringList
SavedSystemsModel::systemsReferencingPath(const QString& path) const {
    QStringList names;
    if (path.isEmpty()) {
        return names;
    }
    for (const Row& row : m_rows) {
        if (row.chanCsvPath == path || row.groupCsvPath == path || row.keyCsvPath == path
            || row.p25BandplanCsvPath == path || row.srcCsvPath == path) {
            names.append(row.name);
        }
    }
    return names;
}

void
SavedSystemsModel::clearCsvPath(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    bool changed = false;
    for (int i = 0; i < m_rows.size(); i++) {
        Row& row = m_rows[i];
        bool rowChanged = false;
        if (row.chanCsvPath == path) {
            row.chanCsvPath.clear();
            rowChanged = true;
        }
        if (row.groupCsvPath == path) {
            row.groupCsvPath.clear();
            rowChanged = true;
        }
        if (row.keyCsvPath == path) {
            row.keyCsvPath.clear();
            rowChanged = true;
        }
        if (row.p25BandplanCsvPath == path) {
            row.p25BandplanCsvPath.clear();
            rowChanged = true;
        }
        if (row.srcCsvPath == path) {
            row.srcCsvPath.clear();
            rowChanged = true;
        }
        if (rowChanged) {
            const QModelIndex idx = index(i);
            Q_EMIT dataChanged(idx, idx);
            changed = true;
        }
    }
    if (changed) {
        save();
    }
}

int
SavedSystemsModel::mostRecentRow() const {
    int best = 0;
    qint64 bestHeard = -1;
    for (int i = 0; i < m_rows.size(); i++) {
        if (m_rows.at(i).lastHeard > bestHeard) {
            bestHeard = m_rows.at(i).lastHeard;
            best = i;
        }
    }
    return best;
}

void
SavedSystemsModel::load() {
    QList<Row> rows;
    QSet<QString> seen;
    bool migrated = false;
    const QJsonArray array = json_store_load_array(QLatin1String(kStoreFileName));
    // auto, not QJsonValue: iterating a QJsonArray yields a QJsonValueConstRef
    // proxy, and binding that to a QJsonValue reference converts — copying every
    // element into a temporary the loop then reads through.
    for (const auto& value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QVariantMap stored = value.toObject().toVariantMap();
        Row row = rowFromMap(stored, Row());
        if (seen.contains(row.uid)) {
            row.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        seen.insert(row.uid);
        migrated |= stored.value(QStringLiteral("uid")).toString() != row.uid;
        // The P25 Simulcast chip used to carry "-f1 -mq", which pinned Phase 1 only;
        // -mq alone keeps the engine's default decode set (Phase 1 + Phase 2).
        if (row.decodeFlag == QLatin1String("-f1 -mq")) {
            row.decodeFlag = QStringLiteral("-mq");
        }
        rows.append(row);
    }
    beginResetModel();
    m_rows = rows;
    endResetModel();
    if (migrated) {
        save(); // Persist now: a read-only visit must not generate a new identity next launch.
    }
    Q_EMIT countChanged();
    Q_EMIT mostRecentRowChanged();
}

void
SavedSystemsModel::save() const {
    QJsonArray array;
    for (const Row& row : m_rows) {
        array.append(QJsonObject::fromVariantMap(mapFromRow(row)));
    }
    json_store_save_array(QLatin1String(kStoreFileName), array);
}

} // namespace dsd_qt
