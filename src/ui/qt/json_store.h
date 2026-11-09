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
 *
 * A store that could not be read whole is never overwritten (issue #828). The load marks it, and the first save
 * after that renames it aside to "<name>.unreadable-N" (N = 1..99, the first free one) before it puts the new list
 * in its place. Nothing moves on a run that does not save that store.
 */

#ifndef DSD_NEO_SRC_UI_QT_JSON_STORE_H_
#define DSD_NEO_SRC_UI_QT_JSON_STORE_H_

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <functional>

namespace dsd_qt {

/** @brief The app data directory every store and imported file lives under (AppDataLocation). */
QString json_store_root();

/** @brief Absolute path of @p fileName inside the app data directory. */
QString json_store_path(const QString& fileName);

/** @brief What reading a store found. */
enum class JsonStoreLoad {
    Missing,    ///< confirmed absent (json_store_definitely_missing())
    Ok,         ///< read whole, a JSON array
    Unreadable, ///< anything else: not a regular file, unopenable, a read error, not JSON, not an array
};

/** @brief One store read: its status, the array (empty unless Ok) and, when Unreadable, why (for logs). */
struct JsonStoreRead {
    JsonStoreLoad status = JsonStoreLoad::Missing;
    QJsonArray array;
    QString reason;
};

/**
 * @brief Read @p fileName without marking it.
 *
 * For readers that never save what they read (a backup snapshot, tests). A path that is not a regular file is
 * reported Unreadable without being opened, so a FIFO cannot hang the caller.
 */
JsonStoreRead json_store_read(const QString& fileName);

/**
 * @brief Read @p fileName as a JSON array, for a model that saves it later.
 *
 * An Unreadable file is marked and logged, and a notice is posted: the first json_store_save_array() of it keeps it
 * aside first. A later read that succeeds does not clear the mark.
 *
 * @return The array, or an empty one when the file is missing or unreadable.
 */
QJsonArray json_store_load_array(const QString& fileName);

/**
 * @brief json_store_load_array(), and how the read went.
 * @param status  Receives Ok, Missing (confirmed absent) or Unreadable (marked as above). Callers that delete anything
 *                on the strength of the store's contents must do nothing on Unreadable.
 * @return The array, empty unless @p status is Ok.
 */
QJsonArray json_store_load_array(const QString& fileName, JsonStoreLoad* status);

/**
 * @brief Atomically write @p array to @p fileName, creating directories as needed.
 *
 * A marked file is replaced in the order that keeps it: (1) write and sync a temporary file beside it, (2) rename the
 * file at the path (a regular file or a link, never followed) to its first free "<name>.unreadable-N", (3) replace
 * the path. A failure at (1) or (2) moves nothing and leaves the save owed. A path confirmed absent at (2) has nothing
 * to keep and clears the mark. A folder, FIFO or other special file there is never moved or replaced. Once kept, a
 * failure at (3) leaves only the write owed. Unmarked files are written through QSaveFile. Safe from any thread.
 *
 * @param bytes When non-null, receives the size of the serialized array (0 when the file could not be opened).
 * @return False when nothing was written, the save is then owed.
 */
bool json_store_save_array(const QString& fileName, const QJsonArray& array, qint64* bytes = nullptr);

/**
 * @brief Whether @p path is confirmed absent: its stat fails with ENOENT or ENOTDIR.
 *
 * Any other failure (a folder on the way that cannot be searched, an I/O error) is not an answer, and a caller that
 * would forget a file on "missing" keeps it.
 */
bool json_store_definitely_missing(const QString& path);

/** @brief A file that could not be read: its store file name, why, and the name it was kept as once moved. */
struct JsonStoreNotice {
    QString fileName;
    QString reason;
    QString keptName; ///< "<name>.unreadable-N" (no folder) once moved aside, empty until then
};

/** @brief Every notice posted in this process, oldest first, one per file name. Safe from any thread. */
QList<JsonStoreNotice> json_store_notices();

/**
 * @brief Post a notice for a file read outside json_store (a settings file), replacing the one with its file name.
 *
 * Calls the sink as the store's own notices do.
 */
void json_store_post_notice(const JsonStoreNotice& notice);

/**
 * @brief Set the function called after the notice list changes.
 *
 * Called with no json_store lock held, on the thread that loaded or saved (a call history worker among them), so it
 * may call json_store_notices() and should hand the change to its own thread. An empty function clears it.
 */
void json_store_set_notice_sink(std::function<void()> sink);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_JSON_STORE_H_ */
