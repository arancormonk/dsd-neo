// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Entry points a platform host uses to bring up the shared Qt Quick UI.
 */

#ifndef DSD_NEO_SRC_UI_QT_QT_UI_H_
#define DSD_NEO_SRC_UI_QT_QT_UI_H_

class QObject;
class QQmlApplicationEngine;

namespace dsd_qt {

class AppPrefs;
class DecoderHost;
class ImportedFilesModel;
class ListenPins;
class SavedSystemsModel;
class ScanListsModel;

/**
 * @brief Pin the look shared by every platform.
 *
 * Must run before the QML engine loads anything, and before a QGuiApplication is
 * required — style selection is process-global.
 */
void ui_apply_style(void);

/**
 * @brief Register QML types, expose @p host, and load the root QML document.
 * @param engine Engine owned by the caller; must outlive the UI.
 * @param host Decoder lifecycle implementation owned by the caller.
 * @return true when the root document instantiated a window.
 */
bool ui_load(QQmlApplicationEngine& engine, DecoderHost* host);

/**
 * @brief Clear what saved systems and scan lists name of library files that are gone.
 *
 * load_listen_pins() runs it, from ui_load(), once the models have loaded and the pins are built: the paths the
 * library pruned this load, and every path into ImportedFilesModel::libraryRoot() whose file no longer exists, so a
 * clear whose save failed at one launch is done at a later one. A path outside the library is left as it is.
 */
void prune_missing_csv_paths(ImportedFilesModel* importedFiles, SavedSystemsModel* systems, ScanListsModel* scanLists);

/**
 * @brief Build the shortcuts pinned to Listen over the loaded models, then run prune_missing_csv_paths().
 *
 * ui_load() publishes what this returns, owned by @p owner. The pins are built first so they see each saved row's
 * RadioReference group as stored: a system pin whose row the cleanup takes out of its group then moves to the group's
 * lead, as it does when the row is edited out of the group (issue #801).
 */
ListenPins* load_listen_pins(AppPrefs* prefs, ImportedFilesModel* importedFiles, SavedSystemsModel* systems,
                             ScanListsModel* scanLists, QObject* owner);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_QT_UI_H_ */
