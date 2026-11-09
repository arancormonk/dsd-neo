// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Re-announces a display density that Qt's Android plugin changed without telling QGuiApplication.
 *
 * Qt 6.11.3's plugin stores a new density for QAndroidPlatformScreen::logicalDpi() and raises no event, so Qt's scale
 * factor moves while the window keeps its old logical size and device pixel ratio, the screen its old logical
 * geometry, and QML its old safe-area margins. Uses Qt's platform API (QPA, Qt6::GuiPrivate): no public API raises
 * these events. Re-check on every Qt upgrade (android/README.md).
 */

#ifndef DSD_NEO_ANDROID_DISPLAY_SCALE_H_
#define DSD_NEO_ANDROID_DISPLAY_SCALE_H_

#include <QtGlobal>

class QScreen;

namespace dsd_android {

/**
 * @brief The scale Qt's platform plugin holds for @p screen: its logical DPI over its base DPI.
 *
 * On Android this is the density Qt last stored. It moves even while Qt's scaling is off (a start at density 1),
 * where QScreen::devicePixelRatio() stays 1. Returns 0 without a platform screen or for an unusable value.
 */
qreal platform_density(const QScreen* screen);

/**
 * @brief Has Qt re-read every screen's DPI (and with it its logical geometry and its windows' device pixel ratio),
 * and every created window's geometry and safe-area margins.
 *
 * GUI thread only. The events are queued, not delivered here, so it is safe inside a timer callback; their native to
 * logical conversion happens in this call, after the DPI announcement has updated Qt's scaling. A window gets a resize
 * only when its logical size changed. Never creates a platform window.
 */
void resync_display_scale();

} // namespace dsd_android

#endif // DSD_NEO_ANDROID_DISPLAY_SCALE_H_
