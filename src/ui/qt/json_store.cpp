// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "json_store.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
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
json_store_load_array(const QString& fileName) {
    QFile file(json_store_path(fileName));
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonArray();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isArray() ? doc.array() : QJsonArray();
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
