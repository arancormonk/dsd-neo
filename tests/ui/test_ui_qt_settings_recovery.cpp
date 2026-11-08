// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: a settings file Qt could not read whole is kept before Qt writes over it (issue #828). Covered, for
 * AppPrefs' INI file and, where it is an INI file, call history's: a malformed line in a section nothing has read yet;
 * a read error after a valid prefix; a malformed fallback file, which is not the settings file's problem; the original
 * kept byte-identical; a copy or a rewrite that fails, after which every QSettings of the format writes a scratch copy
 * (AppPrefs' constructor write, a deferred flush, the static helpers and destruction all leave the original alone); a
 * folder or FIFO at the path; the first call-history save after a malformed file; replacePortable() after a recovery;
 * and a pending restore that waits while the settings run on the scratch copy, then applies after a later start
 * recovers them. Registered only when the Qt frontend is enabled (DSD_ENABLE_QT_UI). */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>
#include <cstring>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <functional>
#include <stdio.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "backup_apply.h"
#include "call_history_model.h"
#include "json_store.h"
#include "settings_recovery.h"
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

using dsd_qt::AppPrefs;
using dsd_qt::SettingsRecovery;
using dsd_qt::SettingsRecoveryEnv;

#if !defined(Q_OS_WIN) && !defined(Q_OS_DARWIN)
#define HISTORY_SETTINGS_ARE_INI 1
#endif

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

void
expect(const QString& what, bool ok) {
    expect(qUtf8Printable(what), ok);
}

bool
write_file(const QString& path, const QByteArray& contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

QByteArray
read_file(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/* Where QSettings put each file before this test re-pointed anything, read while none exists (so Qt caches no parse). */
struct Paths {
    QString configBase; // the user path both formats share here
    QString prefs;      // AppPrefs' file
    QString orgPrefs;   // its organization fallback file
    QString history;    // call history's default QSettings file (an INI file only where HISTORY_SETTINGS_ARE_INI)
};

Paths g_paths;

QString
kept(const QString& path, int n) {
    return path + QStringLiteral(".unreadable-%1").arg(n);
}

/* A new start: the formats' user paths back on the real folder, as a new process has them. */
void
relaunch() {
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, g_paths.configBase);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, g_paths.configBase);
}

/* Removes @p path (a file, a FIFO or a folder) and every kept copy of it. */
void
remove_with_copies(const QString& path) {
    if (QFileInfo(path).isDir()) {
        QDir(path).removeRecursively();
    }
    QFile::remove(path);
    for (int n = 1; n <= 100; ++n) {
        QFile::remove(kept(path, n));
    }
}

/* Removes both settings files, AppPrefs' fallback and every kept copy, and starts again. The settings folders are left:
   call history's is the app data folder here too. */
void
reset() {
    relaunch();
    remove_with_copies(g_paths.prefs);
    remove_with_copies(g_paths.orgPrefs);
#ifdef HISTORY_SETTINGS_ARE_INI
    remove_with_copies(g_paths.history);
#endif
}

const dsd_qt::JsonStoreNotice*
notice_for(const QList<dsd_qt::JsonStoreNotice>& notices, const QString& fileName) {
    for (const dsd_qt::JsonStoreNotice& notice : notices) {
        if (notice.fileName == fileName) {
            return &notice;
        }
    }
    return nullptr;
}

/* What a fresh explicit-file QSettings parses from @p path, no fallbacks: its keys, and whether it parsed cleanly. */
QStringList
parsed_keys(const QString& path, bool* clean) {
    QSettings settings(path, QSettings::IniFormat);
    const QStringList keys = settings.allKeys();
    *clean = settings.status() == QSettings::NoError;
    return keys;
}

QString
prefs_file_now() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, AppPrefs::settingsOrganization(),
                     AppPrefs::settingsApplication())
        .fileName();
}

/* A file whose first section reads cleanly and whose later one, which nothing has read yet, holds a line Qt drops. */
constexpr char kUnreadSection[] = "[decode]\nextraArgs=-Z\n\n[location]\nlastFixAt=1\nlastLat=12.5\n\n"
                                  "[zz]\nkeep=1\nthis line has no equals sign\nafter=2\n";

void
test_missing() {
    reset();
    dsd_qt::recover_settings_files();
    expect("missing settings: nothing created",
           !QFileInfo::exists(g_paths.prefs) && !QFileInfo::exists(kept(g_paths.prefs, 1)));
    expect("missing settings: no scratch", !dsd_qt::settings_recovery_scratch());
    expect("missing settings: Missing",
           dsd_qt::recover_ini_settings(QSettings::IniFormat, AppPrefs::settingsOrganization(),
                                        AppPrefs::settingsApplication())
               == SettingsRecovery::Missing);
}

/* r1#8: Qt finds the malformed line only when its write parses every section, then writes without it. */
void
test_unread_section_kept() {
    reset();
    expect("unread section: written", write_file(g_paths.prefs, kUnreadSection));
    dsd_qt::recover_settings_files();
    // r1#8: replacePortable() read the FormatError its own write found as a failed restore. First, so no QSettings
    // has parsed the file before it.
    expect("unread section: replacePortable() works after the recovery",
           AppPrefs::replacePortable({{"decode/extraArgs", "-X"}}));
    expect("unread section: replacePortable() wrote", AppPrefs::readPortable().value("decode/extraArgs") == "-X");
    expect("unread section: kept byte-identical", read_file(kept(g_paths.prefs, 1)) == kUnreadSection);
    expect("unread section: no second copy", !QFileInfo::exists(kept(g_paths.prefs, 2)));
    expect("unread section: no scratch", !dsd_qt::settings_recovery_scratch());
    bool clean = false;
    const QStringList keys = parsed_keys(g_paths.prefs, &clean);
    expect("unread section: the rewrite parses cleanly", clean);
    expect("unread section: the rewrite holds every parsed key",
           keys.contains("decode/extraArgs") && keys.contains("zz/keep") && keys.contains("zz/after")
               && keys.contains("location/lastLat"));
    const dsd_qt::JsonStoreNotice* notice = notice_for(dsd_qt::json_store_notices(), "dsd-neo-app.ini");
    expect("unread section: a notice names the kept copy",
           notice != nullptr && notice->keptName == "dsd-neo-app.ini.unreadable-1" && !notice->reason.isEmpty());
    expect("unread section: the copy is untouched", read_file(kept(g_paths.prefs, 1)) == kUnreadSection);

    relaunch();
    expect("unread section: the next start finds it clean",
           dsd_qt::recover_ini_settings(QSettings::IniFormat, AppPrefs::settingsOrganization(),
                                        AppPrefs::settingsApplication())
               == SettingsRecovery::Clean);
    expect("unread section: the next start keeps nothing more", !QFileInfo::exists(kept(g_paths.prefs, 2)));
}

/* Every write QSettings of AppPrefs' format makes this run, none of which may reach @p original at the real path. */
void
write_through_appprefs(const QString& what, const QByteArray& original) {
    {
        AppPrefs prefs; // the location fix in the seed is a day old: the constructor removes it
        expect(what + ": the constructor removed the expired fix", qAbs(prefs.lastLat()) < 1e-9);
        QCoreApplication::processEvents(); // QSettings' deferred flush
        expect(what + ": the constructor write left the original alone", read_file(g_paths.prefs) == original);
        prefs.setAutoStartOnAttach(!prefs.autoStartOnAttach());
        QCoreApplication::processEvents();
        expect(what + ": a deferred flush left the original alone", read_file(g_paths.prefs) == original);
    }
    expect(what + ": destruction left the original alone", read_file(g_paths.prefs) == original);
    expect(what + ": replacePortable() writes the scratch copy",
           AppPrefs::replacePortable({{"decode/extraArgs", "-Y"}}));
    expect(what + ": forgetScanCompanions() writes the scratch copy", AppPrefs::forgetScanCompanions());
    expect(what + ": the static helpers left the original alone", read_file(g_paths.prefs) == original);
    expect(what + ": this run reads its own writes", AppPrefs::readPortable().value("decode/extraArgs") == "-Y");
    const QString scratch = prefs_file_now();
    expect(
        what + ": the format writes a scratch copy in the app cache",
        scratch != g_paths.prefs
            && scratch.startsWith(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1Char('/')));
    expect(what + ": the scratch copy holds the writes", read_file(scratch).contains("-Y"));
}

/* r3#2: Qt's own load ignores a read error, and its write would replace the whole file with the prefix it read. */
void
test_valid_prefix_read_error() {
    reset();
    const QByteArray original = "[decode]\nextraArgs=-Z\n\n[location]\nlastFixAt=1\nlastLat=12.5\n\n[zz]\nlater=1\n";
    expect("read error: written", write_file(g_paths.prefs, original));
    const qint64 prefix = original.indexOf("[zz]");
    SettingsRecoveryEnv env;
    env.readFailsAfter = [prefix](const QString& path) { return path == g_paths.prefs ? prefix : qint64(-1); };
    dsd_qt::recover_settings_files(env);
    expect("read error: scratch", dsd_qt::settings_recovery_scratch());
    expect("read error: nothing kept", !QFileInfo::exists(kept(g_paths.prefs, 1)));
    expect("read error: the scratch copy is seeded with the prefix",
           AppPrefs::readPortable().value("decode/extraArgs") == "-Z");
    const dsd_qt::JsonStoreNotice* notice = notice_for(dsd_qt::json_store_notices(), "dsd-neo-app.ini");
    expect("read error: a notice with no kept name", notice != nullptr && notice->keptName.isEmpty());
    write_through_appprefs("read error", original);
    expect("read error: still nothing kept", !QFileInfo::exists(kept(g_paths.prefs, 1)));

    relaunch();
    dsd_qt::recover_settings_files();
    expect("read error: the next start reads it whole and finds it clean",
           !dsd_qt::settings_recovery_scratch() && read_file(g_paths.prefs) == original
               && !QFileInfo::exists(kept(g_paths.prefs, 1)));
    expect("read error: the next start reads the real file",
           prefs_file_now() == g_paths.prefs && AppPrefs::readPortable().value("decode/extraArgs") == "-Z");
}

/* r3#2: a malformed organization file is read as a fallback; the settings file itself is fine. */
void
test_fallback_ignored() {
    reset();
    const QByteArray healthy = "[decode]\nextraArgs=-Z\n";
    const QByteArray brokenOrg = "[General]\nfromorg=1\n\n[x]\nbroken line\n";
    expect("fallback: written", write_file(g_paths.prefs, healthy) && write_file(g_paths.orgPrefs, brokenOrg));
    {
        // What a check through QSettings' own fallbacks would have seen.
        QSettings withFallbacks(QSettings::IniFormat, QSettings::UserScope, AppPrefs::settingsOrganization(),
                                AppPrefs::settingsApplication());
        (void)withFallbacks.allKeys();
        expect("fallback: QSettings blames the settings file", withFallbacks.status() == QSettings::FormatError);
    }
    expect("fallback: clean", dsd_qt::recover_ini_settings(QSettings::IniFormat, AppPrefs::settingsOrganization(),
                                                           AppPrefs::settingsApplication())
                                  == SettingsRecovery::Clean);
    expect("fallback: the settings file is untouched", read_file(g_paths.prefs) == healthy);
    expect("fallback: the fallback is untouched", read_file(g_paths.orgPrefs) == brokenOrg);
    expect("fallback: nothing kept",
           !QFileInfo::exists(kept(g_paths.prefs, 1)) && !QFileInfo::exists(kept(g_paths.orgPrefs, 1)));
    expect("fallback: no scratch", !dsd_qt::settings_recovery_scratch());
}

/* r2#1: with no copy made, nothing may write the original; the next start tries again. */
void
test_preservation_failure() {
    reset();
    expect("no free name: written", write_file(g_paths.prefs, kUnreadSection));
    for (int n = 1; n <= 99; ++n) {
        write_file(kept(g_paths.prefs, n), QByteArray::number(n));
    }
    dsd_qt::recover_settings_files();
    expect("no free name: scratch", dsd_qt::settings_recovery_scratch());
    expect("no free name: the seed holds what parsed", AppPrefs::readPortable().value("decode/extraArgs") == "-Z");
    write_through_appprefs("no free name", kUnreadSection);
    bool untouched = true;
    for (int n = 1; n <= 99; ++n) {
        untouched = untouched && read_file(kept(g_paths.prefs, n)) == QByteArray::number(n);
    }
    expect("no free name: the taken names are untouched", untouched && !QFileInfo::exists(kept(g_paths.prefs, 100)));

    relaunch();
    QFile::remove(kept(g_paths.prefs, 50));
    dsd_qt::recover_settings_files();
    expect("no free name: a later start keeps it in the freed name",
           !dsd_qt::settings_recovery_scratch() && read_file(kept(g_paths.prefs, 50)) == kUnreadSection);
    bool clean = false;
    (void)parsed_keys(g_paths.prefs, &clean);
    expect("no free name: and rewrites it cleanly", clean);
    expect("no free name: the next start's writes reach it",
           AppPrefs::replacePortable({{"decode/extraArgs", "-W"}}) && read_file(g_paths.prefs).contains("-W"));

    for (const char* step : {"keep", "rewrite"}) {
        const QString what = QStringLiteral("failed %1").arg(QLatin1String(step));
        reset();
        expect(what + ": written", write_file(g_paths.prefs, kUnreadSection));
        SettingsRecoveryEnv env;
        env.writeFails = [step](const char* failing, const QString&) { return std::strcmp(failing, step) == 0; };
        dsd_qt::recover_settings_files(env);
        expect(what + ": scratch", dsd_qt::settings_recovery_scratch());
        expect(what + ": no copy left (the original is still in place)", !QFileInfo::exists(kept(g_paths.prefs, 1)));
        write_through_appprefs(what, kUnreadSection);
    }
}

/* A folder or a FIFO at the path is neither read nor written, and QSettings never opens it this run. */
void
test_not_a_file() {
    reset();
    QDir().mkpath(g_paths.prefs);
    dsd_qt::recover_settings_files();
    expect("folder: scratch", dsd_qt::settings_recovery_scratch());
    { AppPrefs prefs; }
    expect("folder: still a folder, nothing kept",
           QFileInfo(g_paths.prefs).isDir() && !QFileInfo::exists(kept(g_paths.prefs, 1)));
#ifndef _WIN32
    reset();
    expect("FIFO: made", mkfifo(QFile::encodeName(g_paths.prefs).constData(), 0600) == 0);
    dsd_qt::recover_settings_files(); // would wait for a writer if it opened the FIFO
    expect("FIFO: scratch", dsd_qt::settings_recovery_scratch());
    { AppPrefs prefs; } // so would QSettings
    struct stat st;
    expect("FIFO: still a FIFO, nothing kept", lstat(QFile::encodeName(g_paths.prefs).constData(), &st) == 0
                                                   && S_ISFIFO(st.st_mode)
                                                   && !QFileInfo::exists(kept(g_paths.prefs, 1)));
#endif
}

#ifdef HISTORY_SETTINGS_ARE_INI
/* r2#2: call history's own QSettings writes its counters at its first save. */
void
test_call_history_settings() {
    reset();
    const QByteArray original = "[callHistory]\nnextUid=41\n\n[zz]\nkeep=1\nno equals here\n";
    expect("history settings: written", write_file(g_paths.history, original));
    dsd_qt::recover_settings_files();
    expect("history settings: kept byte-identical", read_file(kept(g_paths.history, 1)) == original);
    expect("history settings: no scratch", !dsd_qt::settings_recovery_scratch());
    const QString name = QFileInfo(g_paths.history).fileName();
    const dsd_qt::JsonStoreNotice* notice = notice_for(dsd_qt::json_store_notices(), name);
    expect("history settings: a notice names the kept copy",
           notice != nullptr && notice->keptName == name + QStringLiteral(".unreadable-1"));
    {
        dsd_qt::CallHistoryModel history;
        history.beginSession();
        history.clearAll(); // writes the next uid
    }
    bool clean = false;
    const QStringList keys = parsed_keys(g_paths.history, &clean);
    expect("history settings: the first save writes a clean file with every parsed key",
           clean && keys.contains("zz/keep") && keys.contains("callHistory/session")
               && keys.contains("callHistory/nextUid"));
    expect("history settings: the copy is untouched", read_file(kept(g_paths.history, 1)) == original);

    reset();
    expect("history settings: written again", write_file(g_paths.history, original));
    SettingsRecoveryEnv env;
    env.writeFails = [](const char* step, const QString& path) {
        return std::strcmp(step, "keep") == 0 && path == g_paths.history;
    };
    dsd_qt::recover_settings_files(env);
    expect("history settings, no copy: scratch", dsd_qt::settings_recovery_scratch());
    {
        dsd_qt::CallHistoryModel history;
        history.beginSession();
        history.clearAll();
        QCoreApplication::processEvents();
        expect("history settings, no copy: the save left the original alone", read_file(g_paths.history) == original);
    }
    expect("history settings, no copy: destruction left the original alone", read_file(g_paths.history) == original);
    expect("history settings, no copy: AppPrefs' file was not redirected", prefs_file_now() == g_paths.prefs);
}
#endif

/* r3#1: a restore that applied onto the scratch copy would lose its settings at the next start. */
void
test_restore_waits_for_settings() {
    reset();
    expect("restore: backup settings", AppPrefs::replacePortable({{"decode/extraArgs", "-R"}}));
    const auto built = dsd_qt::backup::backup_build_archive(dsd_qt::backup::backup_take_snapshot(), nullptr);
    expect("restore: backup builds", built.error == dsd_qt::backup::BackupError::None);
    const QString root = dsd_qt::json_store_root();
    const dsd_qt::backup::ParseResult parsed = dsd_qt::backup::backup_parse_archive(built.archive, root);
    const QString staging = dsd_qt::backup::staging_dir_for_job("settings");
    QString error;
    expect("restore: staged", dsd_qt::backup::stage_restore(root, parsed, built.archive, staging, &error));
    expect("restore: committed", dsd_qt::backup::promote_staging(staging) && dsd_qt::backup::commit_pending());

    const QByteArray old = "[decode]\nextraArgs=-OLD\n\n[zz]\nno equals here\n";
    expect("restore: old settings written", write_file(g_paths.prefs, old));
    for (int n = 1; n <= 99; ++n) {
        write_file(kept(g_paths.prefs, n), QByteArray::number(n));
    }
    dsd_qt::recover_settings_files();
    expect("restore: scratch", dsd_qt::settings_recovery_scratch());
    const dsd_qt::backup::ApplyResult waited = dsd_qt::backup::apply_pending_restore({});
    expect("restore: deferred while the settings run on the scratch copy",
           waited.outcome == dsd_qt::backup::ApplyOutcome::Deferred && !waited.reason.isEmpty());
    expect("restore: still committed for the next start", dsd_qt::backup::pending_committed());
    expect("restore: no apply started", !dsd_qt::backup::pending_apply_started());
    expect("restore: the settings file is untouched", read_file(g_paths.prefs) == old);
    expect("restore: the scratch copy did not take the restore",
           AppPrefs::readPortable().value("decode/extraArgs") == "-OLD");

    relaunch();
    QFile::remove(kept(g_paths.prefs, 7));
    dsd_qt::recover_settings_files();
    expect("restore: a later start recovers the settings",
           !dsd_qt::settings_recovery_scratch() && read_file(kept(g_paths.prefs, 7)) == old);
    const dsd_qt::backup::ApplyResult applied = dsd_qt::backup::apply_pending_restore({});
    expect("restore: then applies", applied.outcome == dsd_qt::backup::ApplyOutcome::Applied);
    expect("restore: onto the settings file", prefs_file_now() == g_paths.prefs
                                                  && AppPrefs::readPortable().value("decode/extraArgs") == "-R"
                                                  && read_file(g_paths.prefs).contains("-R"));
    expect("restore: nothing pending", !dsd_qt::backup::pending_exists());
    (void)dsd_qt::backup::clear_result_file();
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-settings-recovery-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QDir().mkpath(dataDir);

    // Read while no settings file exists, so QSettings caches no parse of one.
    g_paths.prefs = prefs_file_now();
    g_paths.configBase = QFileInfo(QFileInfo(g_paths.prefs).absolutePath()).absolutePath();
    g_paths.orgPrefs =
        QSettings(QSettings::IniFormat, QSettings::UserScope, AppPrefs::settingsOrganization()).fileName();
    g_paths.history = QSettings().fileName();
    if (QFileInfo::exists(g_paths.prefs) || g_paths.configBase.size() < 2) {
        DSD_FPRINTF(stderr, "the settings folder is not an empty test folder\n");
        return 1;
    }

    test_missing();
    test_unread_section_kept();
    test_valid_prefix_read_error();
    test_fallback_ignored();
    test_preservation_failure();
    test_not_a_file();
#ifdef HISTORY_SETTINGS_ARE_INI
    test_call_history_settings();
#endif
    test_restore_waits_for_settings();

    reset();
    dsd_app_signal_search_store_close();
    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "UI_QT_SETTINGS_RECOVERY: OK\n");
    return 0;
}
