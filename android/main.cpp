// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Qt entry point for the Android app.
 *
 * Owns nothing but the UI: the engine lives in the foreground service, which this
 * process reaches through DecoderHostAndroid. Android may tear this down (and with it
 * the whole Qt UI) while the service keeps decoding.
 */

#include <QGuiApplication>
#include <QLatin1Char>
#include <QQmlApplicationEngine>
#include <QString>
#include <memory>
#include <stdint.h>

#include <dsd-neo/runtime/decode_clock.h>

#include "decoder_host_android.h"
#include "diagnostics_log.h"
#include "qml_gc_policy.h"
#include "qt_ui.h"
#include "teardown_trace.h"

namespace {

void
log_teardown(const QString& line) {
    dsd_qt::DiagnosticsLog::submitHostDiagnostic(line);
}

/* CLOCK_MONOTONIC, which Android's SystemClock.uptimeMillis() reads too, so these line up with the Kotlin markers. */
const dsd_qt::TeardownTraceHooks kTeardownHooks{&log_teardown, &dsd_realtime_mono_ms};

QString
uptime_text(uint64_t ms) {
    return QStringLiteral("%1.%2 s").arg(ms / 1000).arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

} // namespace

int
main(int argc, char* argv[]) {
    dsd_qt::applyQmlGcPolicy();
    dsd_qt::ui_apply_style();

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DSD-neo"));
    app.setOrganizationName(QStringLiteral("dsd-neo"));

    // Held by pointer only so the teardown below can time each one's destruction; the order is unchanged.
    auto host = std::make_unique<dsd_android::DecoderHostAndroid>();
    auto engine = std::make_unique<QQmlApplicationEngine>();
    if (!dsd_qt::ui_load(*engine, host.get())) {
        return 1;
    }

    const int rc = app.exec();

    /* QtActivity's onDestroy() waits for this function to return, and a relaunch waits for this process (#782), so
     * each stage says how long it took: the engine (QML objects, then the models, which time their own destructors)
     * and the host. The bounded flush then makes these and DsdNeoActivity's markers durable before the process may
     * exit. */
    dsd_qt::teardownTraceInstall(&kTeardownHooks);
    log_teardown(
        QStringLiteral("teardown: Qt event loop returned at uptime %1").arg(uptime_text(dsd_realtime_mono_ms())));
    {
        const dsd_qt::TeardownStage stage("QML engine and its models");
        engine.reset();
    }
    {
        const dsd_qt::TeardownStage stage("decoder host");
        host.reset();
    }
    log_teardown(QStringLiteral("teardown: main() returning at uptime %1").arg(uptime_text(dsd_realtime_mono_ms())));
    (void)dsd_qt::DiagnosticsLog::flushDiagnostics(dsd_qt::DiagnosticsLog::kShutdownFlushMs);
    dsd_qt::teardownTraceInstall(nullptr);
    return rc;
}
