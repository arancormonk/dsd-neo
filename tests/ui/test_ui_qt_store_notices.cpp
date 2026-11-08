// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the files this run could not read, as Home's card lists them (issue #828). json_store's notices reach the
 * list on its own thread even when a worker posted them, and only once its event loop runs; a real load and save on a
 * worker go through unreadable to kept; call history's seen store is never listed; a dismissal holds for the notice as
 * it was, and a change to it shows again; a copy kept before is still named after the file is unreadable again; the
 * Signal search store's files are taken as reported; a settings file running on its scratch copy reads "unsaved", and
 * call history's settings file is named apart from AppPrefs'. Run under ThreadSanitizer with the tsan-debug preset.
 * Registered only when the Qt frontend is enabled (DSD_ENABLE_QT_UI). */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonValue>
#include <QLatin1String>
#include <QMap>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantMap>
#include <cstring>
#include <dsd-neo/core/safe_api.h>
#include <functional>
#include <stdio.h>
#include <thread>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "json_store.h"
#include "settings_recovery.h"
#include "store_notices.h"

namespace {

using dsd_qt::StoreNotices;

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* The shown notice for @p file, or an empty map. */
QVariantMap
shown(const QString& file) {
    for (const QVariant& value : StoreNotices::instance()->notices()) {
        const QVariantMap notice = value.toMap();
        if (notice.value(QStringLiteral("file")).toString() == file) {
            return notice;
        }
    }
    return {};
}

QString
field(const QString& file, const char* key) {
    return shown(file).value(QLatin1String(key)).toString();
}

void
post_from_worker(const QString& file, const QString& kept) {
    std::thread worker([file, kept] { dsd_qt::json_store_post_notice({file, QStringLiteral("test"), kept}); });
    worker.join();
}

/* A worker's post changes nothing until the list's own thread runs its events, and every change is made there. */
void
test_queued_from_a_worker() {
    StoreNotices* notices = StoreNotices::instance();
    QThread* changedOn = nullptr;
    int changes = 0;
    const QMetaObject::Connection connection = QObject::connect(notices, &StoreNotices::noticesChanged, [&] {
        changedOn = QThread::currentThread();
        changes++;
    });
    post_from_worker(QStringLiteral("saved_systems.json"), QString());
    expect("queued: nothing changes on the worker's thread", changes == 0 && shown("saved_systems.json").isEmpty());
    QCoreApplication::processEvents();
    expect("queued: the list changes once its events run", changes == 1);
    expect("queued: on the list's own thread", changedOn == QCoreApplication::instance()->thread());
    expect("queued: an unreadable notice", field("saved_systems.json", "state") == QLatin1String("unreadable"));
    expect("queued: named as its file", field("saved_systems.json", "store") == QLatin1String("saved_systems.json"));
    QObject::disconnect(connection);
}

/* Many workers posting at once while the GUI thread reads: every notice arrives (ThreadSanitizer checks the rest). */
void
test_many_workers() {
    std::thread workers[4];
    for (int i = 0; i < 4; ++i) {
        workers[i] = std::thread([i] {
            for (int n = 0; n < 25; ++n) {
                dsd_qt::json_store_post_notice(
                    {QStringLiteral("worker-%1-%2.json").arg(i).arg(n), QStringLiteral("test"), QString()});
            }
        });
    }
    for (int spin = 0; spin < 50; ++spin) {
        QCoreApplication::processEvents();
        (void)StoreNotices::instance()->notices();
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    QCoreApplication::processEvents();
    int found = 0;
    for (int i = 0; i < 4; ++i) {
        for (int n = 0; n < 25; ++n) {
            found += shown(QStringLiteral("worker-%1-%2.json").arg(i).arg(n)).isEmpty() ? 0 : 1;
        }
    }
    expect("many workers: every notice arrives", found == 100);
    for (int i = 0; i < 4; ++i) {
        for (int n = 0; n < 25; ++n) {
            StoreNotices::instance()->dismiss(QStringLiteral("worker-%1-%2.json").arg(i).arg(n));
        }
    }
}

/* A store loaded and saved on a worker, as call history's is: unreadable, then kept with its name. */
void
test_real_store_on_a_worker() {
    const QString file = QStringLiteral("call_history.json");
    const QString path = dsd_qt::json_store_path(file);
    QDir().mkpath(QFileInfo(path).absolutePath());
    {
        QFile junk(path);
        expect("store: written", junk.open(QIODevice::WriteOnly | QIODevice::Truncate) && junk.write("{not json") > 0);
    }
    std::thread([file] { (void)dsd_qt::json_store_load_array(file); }).join();
    QCoreApplication::processEvents();
    expect("store: unreadable after the load", field(file, "state") == QLatin1String("unreadable"));
    expect("store: nothing kept yet", field(file, "kept").isEmpty());
    std::thread([file] { (void)dsd_qt::json_store_save_array(file, QJsonArray{QJsonValue(1)}); }).join();
    QCoreApplication::processEvents();
    expect("store: kept after the save", field(file, "state") == QLatin1String("kept"));
    expect("store: names the kept file", field(file, "kept") == QLatin1String("call_history.json.unreadable-1"));
    expect("store: kept on disk", QFileInfo::exists(path + QStringLiteral(".unreadable-1")));
}

/* The seen store is the app's own bookkeeping. */
void
test_seen_store_hidden() {
    post_from_worker(QStringLiteral("call_history_seen.json"), QString());
    QCoreApplication::processEvents();
    expect("seen: never listed", shown(QStringLiteral("call_history_seen.json")).isEmpty());
}

void
test_dismiss_and_change() {
    const QString file = QStringLiteral("scan_lists.json");
    post_from_worker(file, QString());
    QCoreApplication::processEvents();
    expect("dismiss: shown", !shown(file).isEmpty());
    StoreNotices::instance()->dismiss(file);
    expect("dismiss: hidden", shown(file).isEmpty());
    post_from_worker(file, QString());
    QCoreApplication::processEvents();
    expect("dismiss: the same notice stays hidden", shown(file).isEmpty());
    post_from_worker(file, QStringLiteral("scan_lists.json.unreadable-1"));
    QCoreApplication::processEvents();
    expect("dismiss: kept after all shows again", field(file, "state") == QLatin1String("kept"));
    // Unreadable again after a clean save: the earlier copy is still on disk, so it is still named.
    post_from_worker(file, QString());
    QCoreApplication::processEvents();
    expect("earlier: unreadable again", field(file, "state") == QLatin1String("unreadable"));
    expect("earlier: names the copy kept before",
           field(file, "earlierKept") == QLatin1String("scan_lists.json.unreadable-1"));
}

/* Each shown notice is handed over for reading out once for the process; a dismissed one never, a changed one again. */
void
test_heard_once() {
    StoreNotices* notices = StoreNotices::instance();
    const auto taken = [notices](const QString& file) {
        for (const QVariant& value : notices->takeUnheard()) {
            if (value.toMap().value(QStringLiteral("file")).toString() == file) {
                return true;
            }
        }
        return false;
    };
    (void)notices->takeUnheard(); // what earlier cases left
    post_from_worker(QStringLiteral("imported_files.json"), QString());
    QCoreApplication::processEvents();
    expect("heard: a new notice is handed over", taken(QStringLiteral("imported_files.json")));
    expect("heard: once", notices->takeUnheard().isEmpty());
    post_from_worker(QStringLiteral("decryption_profiles.json"), QString());
    QCoreApplication::processEvents();
    notices->dismiss(QStringLiteral("decryption_profiles.json"));
    expect("heard: a dismissed notice is never handed over", notices->takeUnheard().isEmpty());
    post_from_worker(QStringLiteral("imported_files.json"), QStringLiteral("imported_files.json.unreadable-1"));
    QCoreApplication::processEvents();
    expect("heard: a changed notice is handed over again", taken(QStringLiteral("imported_files.json")));
}

void
test_signal_search_files() {
    StoreNotices* notices = StoreNotices::instance();
    notices->setSignalSearchFiles(
        {QVariantMap{{"file", "signal-search-history.v2"},
                     {"state", "kept"},
                     {"kept", "signal-search-history.v2.unreadable-1"}},
         QVariantMap{{"file", "signal-search-history.v1.avoids.json"}, {"state", ""}, {"kept", ""}},
         QVariantMap{{"file", "signal-search-history.v1"}, {"state", "leftInPlace"}, {"kept", ""}}});
    expect("search: the history kept", field("signal-search-history.v2", "state") == QLatin1String("kept"));
    expect("search: a file read whole is not listed", shown("signal-search-history.v1.avoids.json").isEmpty());
    expect("search: version 1 left in place",
           field("signal-search-history.v1", "state") == QLatin1String("leftInPlace"));
    notices->setSignalSearchFiles({});
    expect("search: a later report replaces it",
           shown("signal-search-history.v2").isEmpty() && shown("signal-search-history.v1").isEmpty());
}

/* A settings file that could not be kept runs on its scratch copy: this run's changes are not saved. */
void
test_settings_scratch() {
    const QString path = QSettings(QSettings::IniFormat, QSettings::UserScope, dsd_qt::AppPrefs::settingsOrganization(),
                                   dsd_qt::AppPrefs::settingsApplication())
                             .fileName();
    QDir().mkpath(QFileInfo(path).absolutePath());
    {
        QFile malformed(path);
        expect("settings: written", malformed.open(QIODevice::WriteOnly | QIODevice::Truncate)
                                        && malformed.write("[decode]\nextraArgs=-Z\n[zz]\nno equals sign here\n") > 0);
    }
    dsd_qt::SettingsRecoveryEnv env;
    env.writeFails = [](const char* step, const QString&) { return std::strcmp(step, "keep") == 0; };
    expect("settings: scratch",
           dsd_qt::recover_ini_settings(QSettings::IniFormat, dsd_qt::AppPrefs::settingsOrganization(),
                                        dsd_qt::AppPrefs::settingsApplication(), env)
               == dsd_qt::SettingsRecovery::Scratch);
    QCoreApplication::processEvents();
    const QString name = dsd_qt::settings_recovery_prefs_file_name();
    expect("settings: unsaved", field(name, "state") == QLatin1String("unsaved"));
    expect("settings: named Settings", field(name, "store") == name);
    // Call history's settings file is named after the application, whatever its extension.
    post_from_worker(QCoreApplication::applicationName() + QStringLiteral(".conf"), QString());
    QCoreApplication::processEvents();
    expect("settings: call history's apart",
           field(QCoreApplication::applicationName() + QStringLiteral(".conf"), "store")
               == QLatin1String("call-history-settings"));
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-store-notices-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    expect("made empty", StoreNotices::instance()->notices().isEmpty());
    test_queued_from_a_worker();
    test_many_workers();
    test_real_store_on_a_worker();
    test_seen_store_hidden();
    test_dismiss_and_change();
    test_heard_once();
    test_signal_search_files();
    test_settings_scratch();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
