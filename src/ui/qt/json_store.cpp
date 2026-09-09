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
json_store_path(const QString& fileName) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QLatin1Char('/') + fileName;
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
json_store_save_array(const QString& fileName, const QJsonArray& array) {
    const QString path = json_store_path(fileName);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray data = QJsonDocument(array).toJson(QJsonDocument::Compact);
    return file.write(data) == data.size() && file.commit();
}

} // namespace dsd_qt
