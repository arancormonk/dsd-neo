// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief One JSON-array-per-file persistence helper shared by the Qt models.
 *
 * Every store lives in the app data directory and is written through QSaveFile so
 * a mid-write kill (Android is fond of those) cannot half-truncate the only copy.
 */

#ifndef DSD_NEO_SRC_UI_QT_JSON_STORE_H_
#define DSD_NEO_SRC_UI_QT_JSON_STORE_H_

#include <QJsonArray>
#include <QString>
#include <QtGlobal>

namespace dsd_qt {

/** @brief The app data directory every store and imported file lives under (AppDataLocation). */
QString json_store_root();

/** @brief Absolute path of @p fileName inside the app data directory. */
QString json_store_path(const QString& fileName);

/**
 * @brief Read @p fileName as a JSON array.
 * @return The array, or an empty one when the file is missing or not an array.
 */
QJsonArray json_store_load_array(const QString& fileName);

/**
 * @brief Atomically write @p array to @p fileName, creating directories as needed.
 * @param bytes When non-null, receives the size of the serialized array (0 when the file could not be opened).
 */
bool json_store_save_array(const QString& fileName, const QJsonArray& array, qint64* bytes = nullptr);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_JSON_STORE_H_ */
