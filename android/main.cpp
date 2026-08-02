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
#include <QQmlApplicationEngine>

#include "decoder_host_android.h"
#include "qt_ui.h"

int
main(int argc, char* argv[]) {
    dsd_qt::ui_apply_style();

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DSD-neo"));
    app.setOrganizationName(QStringLiteral("dsd-neo"));

    dsd_android::DecoderHostAndroid host;
    QQmlApplicationEngine engine;
    if (!dsd_qt::ui_load(engine, &host)) {
        return 1;
    }

    return app.exec();
}
