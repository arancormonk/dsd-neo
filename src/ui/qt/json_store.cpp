// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "json_store.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

namespace dsd_qt {

QString
json_store_root() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString
json_store_path(const QString& fileName) {
    return json_store_root() + QLatin1Char('/') + fileName;
}

QJsonArray
json_store_load_array(const QString& fileName, JsonStoreLoad* status) {
    JsonStoreLoad result = JsonStoreLoad::Failed;
    QJsonArray array;
    QFile file(json_store_path(fileName));
    if (!file.exists()) {
        result = JsonStoreLoad::Absent;
    } else if (file.open(QIODevice::ReadOnly)) {
        const QByteArray data = file.readAll();
        QJsonParseError parse;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &parse);
        if (file.error() == QFileDevice::NoError && parse.error == QJsonParseError::NoError && doc.isArray()) {
            array = doc.array();
            result = JsonStoreLoad::Ok;
        }
    }
    if (status != nullptr) {
        *status = result;
    }
    return array;
}

QJsonArray
json_store_load_array(const QString& fileName) {
    return json_store_load_array(fileName, nullptr);
}

bool
json_store_save_array(const QString& fileName, const QJsonArray& array, qint64* bytes) {
    if (bytes != nullptr) {
        *bytes = 0;
    }
    const QString path = json_store_path(fileName);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return false;
    }
    const QByteArray data = QJsonDocument(array).toJson(QJsonDocument::Compact);
    if (bytes != nullptr) {
        *bytes = data.size();
    }
    return file.write(data) == data.size() && file.commit();
}

} // namespace dsd_qt
