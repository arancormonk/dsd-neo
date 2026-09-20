// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <QVariantList>
#include <Qt>
#include <initializer_list>
#include <utility>
#include "json_store.h"
#include "scan_lists_model.h"

namespace dsd_qt {
namespace {
QString
uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QVariantMap
normalize(const QVariantMap& input, const QVariantMap& base = {}) {
    QVariantMap result =
        base.isEmpty()
            ? QVariantMap{{"name", ""},         {"sourceType", "usb"}, {"host", ""},         {"port", 1234},
                          {"gainDb", -1},       {"ppm", ""},           {"bandwidthKhz", -1}, {"biasTee", -1},
                          {"voiceOnly", false}, {"defaultDwellMs", 0}, {"defaultHoldMs", 0}, {"groupCsvPath", ""},
                          {"srcCsvPath", ""},   {"lastHeard", 0}}
            : base;
    const QStringList fields{"airspy",         "name",          "sourceType",   "host",       "port",
                             "gainDb",         "ppm",           "bandwidthKhz", "biasTee",    "voiceOnly",
                             "defaultDwellMs", "defaultHoldMs", "groupCsvPath", "srcCsvPath", "lastHeard",
                             "entries",        "isDraft"};
    for (const auto& field : fields) {
        if (input.contains(field)) {
            result[field] = input.value(field);
        }
    }
    if (!result.contains("uid")) {
        result["uid"] = input.value("uid", uuid());
    }
    if (!result.contains("isDraft")) {
        result["isDraft"] = false;
    }
    if (!result.contains("lastHeard")) {
        result["lastHeard"] = 0;
    }
    QVariantList entries;
    QSet<QString> ids;
    for (const auto& value : result.value("entries").toList()) {
        const auto old = value.toMap();
        QVariantMap entry{{"kind", "freq"}, {"enabled", true},  {"dwellMs", 0},
                          {"holdMs", 0},    {"modulation", ""}, {"gainDb", -1}};
        for (const auto& field : {"uid", "kind", "systemUid", "name", "protocol", "freqMhz", "enabled", "dwellMs",
                                  "holdMs", "modulation", "gainDb", "decryptionMode", "decryptionProfileUid"}) {
            if (old.contains(field)) {
                entry[field] = old.value(field);
            }
        }
        QString id = entry.value("uid").toString();
        if (id.isEmpty() || ids.contains(id)) {
            id = uuid();
        }
        ids.insert(id);
        entry["uid"] = id;
        if (!entry.contains("enabled")) {
            entry["enabled"] = true;
        }
        entries << entry;
    }
    result["entries"] = entries;
    return result;
}
} // namespace

ScanListsModel::ScanListsModel(QObject* parent) : QAbstractListModel(parent) {
    QSet<QString> ids;
    for (const auto& value : json_store_load_array(QStringLiteral("scan_lists.json"))) {
        if (!value.isObject()) {
            continue;
        }
        auto row = normalize(value.toObject().toVariantMap());
        auto id = row.value("uid").toString();
        if (id.isEmpty() || ids.contains(id)) {
            row["uid"] = id = uuid();
        }
        ids.insert(id);
        m_rows << row;
    }
}

int
ScanListsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray>
ScanListsModel::roleNames() const {
    return {{Qt::UserRole + 1, "name"},
            {Qt::UserRole + 2, "uid"},
            {Qt::UserRole + 3, "lastHeard"},
            {Qt::UserRole + 4, "entries"},
            {Qt::UserRole + 5, "isDraft"}};
}

QVariant
ScanListsModel::data(const QModelIndex& index, int role) const {
    return get(index.row()).value(QString::fromUtf8(roleNames().value(role)));
}

QVariantMap
ScanListsModel::get(int row) const {
    return row >= 0 && row < count() ? m_rows[row] : QVariantMap();
}

int
ScanListsModel::rowForUid(const QString& uid) const {
    for (int i = 0; i < count(); ++i) {
        if (m_rows[i].value("uid") == uid) {
            return i;
        }
    }
    return -1;
}

QVariantMap
ScanListsModel::newDraft() const {
    return normalize({{"isDraft", true}});
}

QString
ScanListsModel::newEntryId() const {
    QSet<QString> existing;
    for (const auto& list : m_rows) {
        for (const auto& entry : list.value("entries").toList()) {
            existing.insert(entry.toMap().value("uid").toString());
        }
    }
    QString id;
    do {
        id = uuid();
    } while (existing.contains(id));
    return id;
}

bool
ScanListsModel::add(const QVariantMap& list) {
    auto row = normalize(list);
    if (rowForUid(row.value("uid").toString()) >= 0) {
        row["uid"] = uuid();
    }
    row["lastHeard"] = 0;
    auto next = m_rows;
    next << row;
    if (!save(next)) {
        return false;
    }
    beginInsertRows({}, count(), count());
    m_rows = std::move(next);
    endInsertRows();
    Q_EMIT countChanged();
    return true;
}

bool
ScanListsModel::update(int row, const QVariantMap& list) {
    if (row < 0 || row >= count()) {
        return false;
    }
    auto next = m_rows;
    next[row] = normalize(list, next[row]);
    if (!save(next)) {
        return false;
    }
    m_rows = std::move(next);
    Q_EMIT dataChanged(index(row), index(row));
    return true;
}

bool
ScanListsModel::remove(int row) {
    if (row < 0 || row >= count()) {
        return false;
    }
    const QString uid = m_rows[row].value("uid").toString();
    auto next = m_rows;
    next.removeAt(row);
    if (!save(next)) {
        return false;
    }
    beginRemoveRows({}, row, row);
    m_rows = std::move(next);
    endRemoveRows();
    if (QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,63}$")).match(uid).hasMatch()) {
        QFile::remove(json_store_path(QStringLiteral("scan_lists/") + uid + QStringLiteral(".csv")));
    }
    Q_EMIT countChanged();
    return true;
}

void
ScanListsModel::touch(int row) {
    update(row, {{"lastHeard", QDateTime::currentSecsSinceEpoch()}});
}

bool
ScanListsModel::save(const QList<QVariantMap>& rows) const {
    QJsonArray array;
    for (const auto& row : rows) {
        array.append(QJsonObject::fromVariantMap(row));
    }
    return json_store_save_array(QStringLiteral("scan_lists.json"), array);
}
} // namespace dsd_qt
