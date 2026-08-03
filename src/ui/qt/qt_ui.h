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

class QQmlApplicationEngine;

namespace dsd_qt {

class DecoderHost;

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

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_QT_UI_H_ */
