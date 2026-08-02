// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "qt_ui.h"

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "command_bridge.h"
#include "decoder_host.h"
#include "event_log_model.h"
#include "metrics_model.h"
#include "ui_controller.h"

namespace dsd_qt {

namespace {

/**
 * @brief Load the bundled monospace face and hand its family back.
 *
 * The event log lays out in columns; per-OS system-font fallback breaks both the
 * alignment and the metrics the layout assumes.
 */
QString
load_mono_font(void) {
    const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/dsdneo/fonts/DejaVuSansMono.ttf"));
    if (id < 0) {
        return QString();
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    return families.isEmpty() ? QString() : families.first();
}

} // namespace

void
ui_apply_style(void) {
    // One style on every platform: the shared UI must not inherit the per-platform
    // Qt Quick Controls default (Material on Android, Fusion on Linux, ...).
    QQuickStyle::setStyle(QStringLiteral("Material"));
}

bool
ui_load(QQmlApplicationEngine& engine, DecoderHost* host) {
    const QString mono_family = load_mono_font();

    auto* metrics = new MetricsModel(&engine);
    auto* events = new EventLogModel(&engine);
    auto* commands = new CommandBridge(&engine);
    auto* controller = new UiController(host, metrics, events, &engine);

    QQmlContext* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("decoderHost"), host);
    context->setContextProperty(QStringLiteral("metrics"), metrics);
    context->setContextProperty(QStringLiteral("eventLog"), events);
    context->setContextProperty(QStringLiteral("commands"), commands);
    context->setContextProperty(QStringLiteral("uiController"), controller);
    context->setContextProperty(QStringLiteral("monoFontFamily"),
                                mono_family.isEmpty() ? QStringLiteral("monospace") : mono_family);

    engine.load(QUrl(QStringLiteral("qrc:/dsdneo/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return false;
    }

    controller->start();
    return true;
}

} // namespace dsd_qt
