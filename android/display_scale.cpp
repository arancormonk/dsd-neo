// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "display_scale.h"

#include <QGuiApplication>
#include <QList>
#include <QScreen>
#include <QWindow>
#include <cmath>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>

namespace dsd_android {

qreal
platform_density(const QScreen* screen) {
    const QPlatformScreen* platform = screen != nullptr ? screen->handle() : nullptr;
    if (platform == nullptr) {
        return 0;
    }
    const qreal base = platform->logicalBaseDpi().first;
    const qreal density = platform->logicalDpi().first / base;
    return std::isfinite(density) && density > 0 ? density : qreal(0);
}

void
resync_display_scale() {
    // The DPI first: its call updates Qt's scaling at once, so the conversions below use the new factor, and its
    // handler recomputes the screen's logical geometry and the device pixel ratio of every window on it.
    for (QScreen* screen : QGuiApplication::screens()) {
        const QPlatformScreen* platform = screen->handle();
        if (platform == nullptr) {
            continue;
        }
        const QDpi dpi = platform->logicalDpi();
        QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(screen, dpi.first, dpi.second);
    }
    // Every created window, children and popups included: the native geometry is unchanged, its logical size is not.
    for (QWindow* window : QGuiApplication::allWindows()) {
        const QPlatformWindow* platform = window->handle();
        if (platform == nullptr || window->screen() == nullptr) {
            continue;
        }
        QWindowSystemInterface::handleGeometryChange(window, platform->geometry());
        QWindowSystemInterface::handleSafeAreaMarginsChanged(window);
    }
}

} // namespace dsd_android
