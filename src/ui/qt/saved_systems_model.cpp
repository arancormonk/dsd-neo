// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "saved_systems_model.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "saved_systems.json";

} // namespace

SavedSystemsModel::SavedSystemsModel(QObject* parent) : QAbstractListModel(parent) { load(); }

SavedSystemsModel::~SavedSystemsModel() = default;

int
SavedSystemsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
SavedSystemsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
        case NameRole: return row.name;
        case SourceTypeRole: return row.sourceType;
        case HostRole: return row.host;
        case PortRole: return row.port;
        case FreqMhzRole: return row.freqMhz;
        case DecodeFlagRole: return row.decodeFlag;
        case TrunkingRole: return row.trunking;
        case GainDbRole: return row.gainDb;
        case PpmRole: return row.ppm;
        case BandwidthKhzRole: return row.bandwidthKhz;
        case BiasTeeRole: return row.biasTee;
        case ExtraArgsRole: return row.extraArgs;
        case FilePathRole: return row.filePath;
        case LastHeardRole: return row.lastHeard;
        default: return QVariant();
    }
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
    return roles;
}

SavedSystemsModel::Row
SavedSystemsModel::rowFromMap(const QVariantMap& map, const Row& base) {
    Row row = base;
    if (map.contains(QStringLiteral("name"))) {
        row.name = map.value(QStringLiteral("name")).toString();
    }
    if (map.contains(QStringLiteral("sourceType"))) {
        row.sourceType = map.value(QStringLiteral("sourceType")).toString();
    }
    if (map.contains(QStringLiteral("host"))) {
        row.host = map.value(QStringLiteral("host")).toString();
    }
    if (map.contains(QStringLiteral("port"))) {
        row.port = map.value(QStringLiteral("port")).toInt();
    }
    if (map.contains(QStringLiteral("freqMhz"))) {
        row.freqMhz = map.value(QStringLiteral("freqMhz")).toString();
    }
    if (map.contains(QStringLiteral("decodeFlag"))) {
        row.decodeFlag = map.value(QStringLiteral("decodeFlag")).toString();
    }
    if (map.contains(QStringLiteral("trunking"))) {
        row.trunking = map.value(QStringLiteral("trunking")).toBool();
    }
    if (map.contains(QStringLiteral("gainDb"))) {
        row.gainDb = map.value(QStringLiteral("gainDb")).toInt();
    }
    if (map.contains(QStringLiteral("ppm"))) {
        row.ppm = map.value(QStringLiteral("ppm")).toString();
    }
    if (map.contains(QStringLiteral("bandwidthKhz"))) {
        row.bandwidthKhz = map.value(QStringLiteral("bandwidthKhz")).toInt();
    }
    if (map.contains(QStringLiteral("biasTee"))) {
        row.biasTee = map.value(QStringLiteral("biasTee")).toBool();
    }
    if (map.contains(QStringLiteral("extraArgs"))) {
        row.extraArgs = map.value(QStringLiteral("extraArgs")).toString();
    }
    if (map.contains(QStringLiteral("filePath"))) {
        row.filePath = map.value(QStringLiteral("filePath")).toString();
    }
    if (map.contains(QStringLiteral("lastHeard"))) {
        row.lastHeard = map.value(QStringLiteral("lastHeard")).toLongLong();
    }
    return row;
}

QVariantMap
SavedSystemsModel::mapFromRow(const Row& row) const {
    QVariantMap map;
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
    return map;
}

void
SavedSystemsModel::add(const QVariantMap& system) {
    beginInsertRows(QModelIndex(), static_cast<int>(m_rows.size()), static_cast<int>(m_rows.size()));
    m_rows.append(rowFromMap(system, Row()));
    endInsertRows();
    Q_EMIT countChanged();
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
    save();
}

QVariantMap
SavedSystemsModel::get(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return QVariantMap();
    }
    return mapFromRow(m_rows.at(row));
}

void
SavedSystemsModel::touch(int row) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }
    m_rows[row].lastHeard = QDateTime::currentSecsSinceEpoch();
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, {LastHeardRole});
    save();
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

QString
SavedSystemsModel::storePath() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QLatin1Char('/') + QLatin1String(kStoreFileName);
}

void
SavedSystemsModel::load() {
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        return;
    }
    QList<Row> rows;
    const QJsonArray array = doc.array();
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            continue;
        }
        rows.append(rowFromMap(value.toObject().toVariantMap(), Row()));
    }
    beginResetModel();
    m_rows = rows;
    endResetModel();
    Q_EMIT countChanged();
}

void
SavedSystemsModel::save() const {
    const QString path = storePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray array;
    for (const Row& row : m_rows) {
        array.append(QJsonObject::fromVariantMap(mapFromRow(row)));
    }
    // QSaveFile so a mid-write kill (Android is fond of those) cannot half-truncate
    // the only copy of the user's saved systems.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    (void)file.commit();
}

} // namespace dsd_qt
