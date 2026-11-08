// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Keeping a settings file Qt could not read whole before Qt writes over it (issue #828).
 *
 * QSettings parses an INI file's sections lazily, ignores a read error, and on its next write rewrites the whole file
 * from what it parsed, dropping every line it could not. Two INI files hold the app's settings: AppPrefs'
 * (IniFormat) and call history's default QSettings (NativeFormat, an INI file on Linux and Android; the Windows
 * registry and Apple plists are left alone). recover_settings_files() checks both before anything else opens them:
 *  - It reads the exact file, checking the read, and parses that copy apart from QSettings' fallback files.
 *  - A file read whole that parses cleanly is left alone, as is a missing one.
 *  - Otherwise the file's bytes are copied to "<file>.unreadable-N" (N = 1..99, the first free one; created, never
 *    replaced, and synced), and a clean file holding what Qt could parse takes its place, which is what Qt's next write
 *    would have produced.
 *  - When that cannot be done (a read error, no free name, a failed write), nothing may write the file: every QSettings
 *    of that format writes a scratch copy in the app cache for the rest of the run, seeded with the bytes that could be
 *    read.
 *    The file stays as it was, and the next start tries again.
 * Each such file gets a JsonStoreNotice (json_store_post_notice()): its fileName is the settings file's own name
 * ("dsd-neo-app.ini" for AppPrefs, "<application>.conf" for call history, "DSD-neo.conf" in the app) and its keptName
 * the copy's name, which stays empty only for a file running on the scratch copy.
 */

#ifndef DSD_NEO_SRC_UI_QT_SETTINGS_RECOVERY_H_
#define DSD_NEO_SRC_UI_QT_SETTINGS_RECOVERY_H_

#include <QSettings>
#include <QString>
#include <QtGlobal>
#include <functional>

namespace dsd_qt {

/** @brief What a test makes a settings file's steps do. An empty function takes the default: no fault. */
struct SettingsRecoveryEnv {
    /** @brief For the settings file at @p path: the bytes read before a read error, or a negative value for none. */
    std::function<qint64(const QString& path)> readFailsAfter;
    /** @brief Whether writing @p path's kept copy ("keep") or its clean file ("rewrite") fails. */
    std::function<bool(const char* step, const QString& path)> writeFails;
};

/** @brief What checking one settings file found. */
enum class SettingsRecovery {
    Missing, ///< no file (confirmed absent), or no organization or application name to find it by
    Clean,   ///< read whole and parsed cleanly; left alone
    Kept,    ///< kept as "<file>.unreadable-N" and replaced by a clean file
    Scratch, ///< could not be kept: QSettings of its format write a scratch copy for the rest of the run
};

/**
 * @brief Check the settings file QSettings(@p format, UserScope, @p organization, @p application) opens.
 *
 * Call before any QSettings of @p format exists: a Scratch outcome re-points that format's user path, which reaches
 * only the QSettings made after it.
 */
SettingsRecovery recover_ini_settings(QSettings::Format format, const QString& organization, const QString& application,
                                      const SettingsRecoveryEnv& env = {});

/**
 * @brief Check AppPrefs' settings file and, where it is an INI file, call history's.
 *
 * The first thing ui_load() does, before a restore applies or any model opens its settings. Forgets what an earlier
 * call found (tests call it once per simulated start, after pointing the formats' user paths back themselves).
 */
void recover_settings_files(const SettingsRecoveryEnv& env = {});

/**
 * @brief Whether a settings file runs on its scratch copy this run: what is changed now is lost at the next start.
 *
 * A pending restore waits while it is true, and a backup cannot include the settings. Independent of QSettings'
 * status, which reads NoError from the scratch copy. Safe from any thread.
 */
bool settings_recovery_scratch();

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SETTINGS_RECOVERY_H_ */
