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

#include <dsd-neo/runtime/git_ver.h>

#include "app_prefs.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "command_bridge.h"
#include "decoder_host.h"
#include "metrics_model.h"
#include "saved_systems_model.h"
#include "ui_controller.h"

namespace dsd_qt {

namespace {

/**
 * @brief Load one bundled face and hand its family name back.
 *
 * The whole UI sets its faces explicitly (IBM Plex Sans for text, IBM Plex Mono
 * for data); per-OS system-font fallback would break both the design's metrics
 * and the tabular-numeral alignment the monitor relies on.
 */
QString
load_font(const char* resource) {
    const int id = QFontDatabase::addApplicationFont(QLatin1String(resource));
    if (id < 0) {
        return QString();
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    return families.isEmpty() ? QString() : families.first();
}

} // namespace

void
ui_apply_style(void) {
    // Basic, not Material: every control the design needs is custom-drawn from the
    // token set, and the Material style would fight those with its own metrics,
    // ripples and theming.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
}

bool
ui_load(QQmlApplicationEngine& engine, DecoderHost* host) {
    // The weight variants register into the same family, so one name per family is
    // enough; Qt resolves font.weight against whichever variants are loaded.
    const QString sans_family = load_font(":/dsdneo/fonts/IBMPlexSans-Regular.ttf");
    (void)load_font(":/dsdneo/fonts/IBMPlexSans-SemiBold.ttf");
    (void)load_font(":/dsdneo/fonts/IBMPlexSans-Bold.ttf");
    const QString mono_family = load_font(":/dsdneo/fonts/IBMPlexMono-Regular.ttf");
    (void)load_font(":/dsdneo/fonts/IBMPlexMono-Medium.ttf");

    auto* metrics = new MetricsModel(&engine);
    auto* commands = new CommandBridge(&engine);
    auto* prefs = new AppPrefs(&engine);
    auto* systems = new SavedSystemsModel(&engine);
    auto* history = new CallHistoryModel(&engine);
    // Each view that shows the call log owns its filter state: the history tab's
    // search and pills must not silently filter the monitor's recent-calls pane.
    auto* historyView = new CallHistoryFilterModel(&engine);
    historyView->setSourceModel(history);
    auto* monitorView = new CallHistoryFilterModel(&engine);
    monitorView->setSourceModel(history);
    auto* controller = new UiController(host, metrics, history, &engine);

    QQmlContext* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("decoderHost"), host);
    context->setContextProperty(QStringLiteral("metrics"), metrics);
    context->setContextProperty(QStringLiteral("commands"), commands);
    context->setContextProperty(QStringLiteral("uiController"), controller);
    context->setContextProperty(QStringLiteral("prefs"), prefs);
    context->setContextProperty(QStringLiteral("savedSystems"), systems);
    context->setContextProperty(QStringLiteral("callHistory"), history);
    context->setContextProperty(QStringLiteral("historyView"), historyView);
    context->setContextProperty(QStringLiteral("monitorView"), monitorView);
    context->setContextProperty(QStringLiteral("sansFontFamily"),
                                sans_family.isEmpty() ? QStringLiteral("sans-serif") : sans_family);
    context->setContextProperty(QStringLiteral("monoFontFamily"),
                                mono_family.isEmpty() ? QStringLiteral("monospace") : mono_family);
    context->setContextProperty(QStringLiteral("appVersionText"), QString::fromUtf8(GIT_TAG));

    engine.load(QUrl(QStringLiteral("qrc:/dsdneo/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return false;
    }

    controller->start();
    return true;
}

} // namespace dsd_qt
