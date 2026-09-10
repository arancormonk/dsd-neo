// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_TEST_SUPPORT_QT_TEST_PATHS_H
#define DSD_NEO_TEST_SUPPORT_QT_TEST_PATHS_H

#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

// Qt's test mode uses ~/.qttest on Unix, which still needs a writable home.
// Keep all desktop test stores disposable, including fixed-name QSettings stores.
inline void
dsd_test_qt_isolate_paths() {
    static QTemporaryDir directory;
    if (!directory.isValid()) {
        qFatal("Could not create temporary Qt test storage");
    }
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    QStandardPaths::setTestModeEnabled(false);
    qputenv("XDG_DATA_HOME", directory.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
#else
    QStandardPaths::setTestModeEnabled(true);
#endif
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
}

#endif
