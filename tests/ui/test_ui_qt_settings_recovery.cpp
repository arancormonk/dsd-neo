// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: a settings file Qt could not read whole is kept before Qt writes over it (issue #828). Covered, for
 * AppPrefs' INI file and, where it is an INI file, call history's: a malformed line in a section nothing has read yet;
 * a read error after a valid prefix; a malformed fallback file, which is not the settings file's problem; the original
 * kept byte-identical; a copy or a rewrite that fails, after which every QSettings of the format writes a scratch copy
 * (AppPrefs' constructor write, a deferred flush, the static helpers and destruction all leave the original alone); a
 * folder or FIFO at the path; a file gone before it is kept (missing: nothing kept or written, no notice); a failed
 * replacement after the rename; no folder for a scratch copy (defaults in memory, nothing written anywhere); a later
 * check forgetting an earlier scratch; the first call-history save after a malformed file; replacePortable() after a
 * recovery; and a pending restore that waits while the settings run on the scratch copy, then applies after a later
 * start recovers them. Registered only when the Qt frontend is enabled (DSD_ENABLE_QT_UI). */

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
    const QSettings settings(path, QSettings::IniFormat);
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

SettingsRecovery
recover_prefs(const SettingsRecoveryEnv& env = {}) {
    return dsd_qt::recover_ini_settings(QSettings::IniFormat, AppPrefs::settingsOrganization(),
                                        AppPrefs::settingsApplication(), env);
}

/* Takes every kept name of @p path, so nothing can be kept aside. */
void
take_every_kept_name(const QString& what, const QString& path) {
    bool written = true;
    for (int n = 1; n <= 99; ++n) {
        written = write_file(kept(path, n), QByteArray::number(n)) && written;
    }
    expect(what + ": every kept name taken", written);
}

bool
kept_names_untouched(const QString& path) {
    bool untouched = true;
    for (int n = 1; n <= 99; ++n) {
        untouched = untouched && read_file(kept(path, n)) == QByteArray::number(n);
    }
    return untouched && !QFileInfo::exists(kept(path, 100));
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
    expect("unread section: Kept", recover_prefs() == SettingsRecovery::Kept);
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
    expect("unread section: the next start finds it clean", recover_prefs() == SettingsRecovery::Clean);
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

/* r3#2: Qt's own load ignores a read error, and its write would replace the whole file with the prefix it read. The
   file is renamed aside unread, and the prefix that parsed takes its place. */
void
test_valid_prefix_read_error() {
    reset();
    const QByteArray original = "[decode]\nextraArgs=-Z\n\n[location]\nlastFixAt=1\nlastLat=12.5\n\n[zz]\nlater=1\n";
    expect("read error: written", write_file(g_paths.prefs, original));
    const qint64 prefix = original.indexOf("[zz]");
    SettingsRecoveryEnv env;
    env.readFailsAfter = [prefix](const QString& path) { return path == g_paths.prefs ? prefix : qint64(-1); };
    expect("read error: Kept", recover_prefs(env) == SettingsRecovery::Kept);
    expect("read error: no scratch", !dsd_qt::settings_recovery_scratch());
    expect("read error: the file is kept byte-identical, renamed unread",
           read_file(kept(g_paths.prefs, 1)) == original);
    bool clean = false;
    const QStringList keys = parsed_keys(g_paths.prefs, &clean);
    expect("read error: the prefix that parsed takes its place",
           clean && keys.contains("decode/extraArgs") && !keys.contains("zz/later"));
    const dsd_qt::JsonStoreNotice* notice = notice_for(dsd_qt::json_store_notices(), "dsd-neo-app.ini");
    expect("read error: a notice names the kept file",
           notice != nullptr && notice->keptName == "dsd-neo-app.ini.unreadable-1");
    expect("read error: the settings read the prefix", AppPrefs::readPortable().value("decode/extraArgs") == "-Z");

    // Nothing can be renamed aside: the file is left as it is, and the run writes a scratch copy of the prefix.
    reset();
    expect("read error, no free name: written", write_file(g_paths.prefs, original));
    take_every_kept_name("read error, no free name", g_paths.prefs);
    expect("read error, no free name: Scratch", recover_prefs(env) == SettingsRecovery::Scratch);
    expect("read error, no free name: the scratch copy is seeded with the prefix",
           AppPrefs::readPortable().value("decode/extraArgs") == "-Z");
    write_through_appprefs("read error, no free name", original);
    expect("read error, no free name: the taken names are untouched", kept_names_untouched(g_paths.prefs));

    // The clean write fails before the rename: nothing moves.
    reset();
    expect("read error, failed write: written", write_file(g_paths.prefs, original));
    SettingsRecoveryEnv failing = env;
    failing.writeFails = [](const char* step, const QString&) { return std::strcmp(step, "rewrite") == 0; };
    expect("read error, failed write: Scratch", recover_prefs(failing) == SettingsRecovery::Scratch);
    expect("read error, failed write: nothing kept", !QFileInfo::exists(kept(g_paths.prefs, 1)));
    write_through_appprefs("read error, failed write", original);

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
    // Both kinds of damage: a line a full parse drops, and a section name QSettings refuses as soon as it loads the
    // file, which sets FormatError on every QSettings that reads this fallback.
    const QByteArray brokenOrg = "[General]\nfromorg=1\n\n[x]\nbroken line\n\n[too/deep/section]\ny=1\n";
    expect("fallback: written", write_file(g_paths.prefs, healthy) && write_file(g_paths.orgPrefs, brokenOrg));
    expect("fallback: clean", recover_prefs() == SettingsRecovery::Clean);
    // AppPrefs reads no fallback file, so a broken one cannot fail a restore's settings write either. First, so nothing
    // has parsed the fallback before it.
    expect("fallback: replacePortable() works beside it", AppPrefs::replacePortable({{"decode/extraArgs", "-F"}}));
    expect("fallback: and wrote the settings file", read_file(g_paths.prefs).contains("-F"));
    {
        // What a check through QSettings' own fallbacks would have seen.
        const QSettings withFallbacks(QSettings::IniFormat, QSettings::UserScope, AppPrefs::settingsOrganization(),
                                      AppPrefs::settingsApplication());
        (void)withFallbacks.allKeys();
        expect("fallback: QSettings blames the settings file", withFallbacks.status() == QSettings::FormatError);
    }
    expect("fallback: the fallback is untouched", read_file(g_paths.orgPrefs) == brokenOrg);
    // A fallback that changes during the run is read again by the next QSettings (Qt keeps a read only while the file
    // is unchanged), and AppPrefs still does not see it.
    const QByteArray changedOrg = brokenOrg + "\n[other/deep/one]\nz=1\n";
    expect("fallback: changed", write_file(g_paths.orgPrefs, changedOrg));
    expect("fallback: replacePortable() works beside a fallback read again",
           AppPrefs::replacePortable({{"decode/extraArgs", "-G"}}) && read_file(g_paths.prefs).contains("-G"));
    expect("fallback: the changed fallback is untouched", read_file(g_paths.orgPrefs) == changedOrg);
    expect("fallback: nothing kept",
           !QFileInfo::exists(kept(g_paths.prefs, 1)) && !QFileInfo::exists(kept(g_paths.orgPrefs, 1)));
    expect("fallback: no scratch", !dsd_qt::settings_recovery_scratch());
}

/* r2#1: with no copy made, nothing may write the original; the next start tries again. */
void
test_preservation_failure() {
    reset();
    expect("no free name: written", write_file(g_paths.prefs, kUnreadSection));
    take_every_kept_name("no free name", g_paths.prefs);
    expect("no free name: Scratch", recover_prefs() == SettingsRecovery::Scratch);
    expect("no free name: scratch",
           dsd_qt::settings_recovery_scratch()
               && dsd_qt::settings_recovery_scratch(dsd_qt::settings_recovery_prefs_file_name()));
    // The seed is what parsed, not the malformed line: the first QSettings to parse all of it reads NoError.
    const QString scratch = prefs_file_now();
    expect("no free name: the seed leaves the malformed line out",
           !read_file(scratch).contains("no equals") && read_file(scratch).contains("after=2"));
    expect("no free name: the seed holds what parsed", AppPrefs::readPortable().value("decode/extraArgs") == "-Z");
    expect("no free name: the first full parse of the seed is clean",
           AppPrefs::replacePortable({{"decode/extraArgs", "-Z"}}));
    write_through_appprefs("no free name", kUnreadSection);
    expect("no free name: the taken names are untouched", kept_names_untouched(g_paths.prefs));

    relaunch();
    QFile::remove(kept(g_paths.prefs, 50));
    expect("no free name: a later start is Kept", recover_prefs() == SettingsRecovery::Kept);
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
        expect(what + ": Scratch", recover_prefs(env) == SettingsRecovery::Scratch);
        expect(what + ": scratch", dsd_qt::settings_recovery_scratch());
        expect(what + ": no copy left (the original is still in place)", !QFileInfo::exists(kept(g_paths.prefs, 1)));
        write_through_appprefs(what, kUnreadSection);
    }
}

/* A notice for AppPrefs' file that only a test posts: still there after a recovery means the recovery posted none. */
void
post_sentinel_notice() {
    dsd_qt::json_store_post_notice(
        {dsd_qt::settings_recovery_prefs_file_name(), QStringLiteral("sentinel"), QString()});
}

bool
sentinel_notice_stands() {
    const dsd_qt::JsonStoreNotice* notice =
        notice_for(dsd_qt::json_store_notices(), dsd_qt::settings_recovery_prefs_file_name());
    return notice != nullptr && notice->reason == QLatin1String("sentinel");
}

/* Puts AppPrefs' file on its scratch copy (no kept name free), then clears the taken names for the next case. */
void
leave_prefs_on_scratch(const QString& what) {
    reset();
    expect(what + ": written for the earlier check", write_file(g_paths.prefs, kUnreadSection));
    take_every_kept_name(what, g_paths.prefs);
    expect(what + ": the earlier check is Scratch", recover_prefs() == SettingsRecovery::Scratch);
    reset();
}

/* A file removed by someone else after it was read and before it was kept is a missing file, as if the check had found
   none: nothing kept, nothing written in its place, no notice, and nothing left noted from an earlier check. */
void
test_gone_before_kept() {
    const QByteArray original = "[decode]\nextraArgs=-Z\n\n[zz]\nlater=1\n";
    const QString prefsName = dsd_qt::settings_recovery_prefs_file_name();

    // Read error: the file goes as its rename aside starts.
    leave_prefs_on_scratch("gone before the rename");
    expect("gone before the rename: written", write_file(g_paths.prefs, original));
    SettingsRecoveryEnv renamed;
    renamed.readFailsAfter = [&original](const QString& path) {
        return path == g_paths.prefs ? original.indexOf("[zz]") : qint64(-1);
    };
    renamed.writeFails = [](const char* step, const QString& path) {
        if (std::strcmp(step, "keep") == 0 && path == g_paths.prefs) {
            QFile::remove(path);
        }
        return false;
    };
    post_sentinel_notice();
    expect("gone before the rename: Missing", recover_prefs(renamed) == SettingsRecovery::Missing);
    expect("gone before the rename: nothing written or kept",
           !QFileInfo::exists(g_paths.prefs) && !QFileInfo::exists(kept(g_paths.prefs, 1)));
    expect("gone before the rename: no notice", sentinel_notice_stands());
    expect("gone before the rename: not on scratch", !dsd_qt::settings_recovery_scratch(prefsName));
    expect("gone before the rename: the real file is used", prefs_file_now() == g_paths.prefs);

    // Read whole: the file goes after its copy was made, before the clean file replaces it.
    leave_prefs_on_scratch("gone before the replacement");
    expect("gone before the replacement: written", write_file(g_paths.prefs, kUnreadSection));
    SettingsRecoveryEnv copied;
    copied.writeFails = [](const char* step, const QString& path) {
        if (std::strcmp(step, "rewrite") == 0 && path == g_paths.prefs) {
            QFile::remove(path);
        }
        return false;
    };
    post_sentinel_notice();
    expect("gone before the replacement: Missing", recover_prefs(copied) == SettingsRecovery::Missing);
    expect("gone before the replacement: nothing written, the copy removed",
           !QFileInfo::exists(g_paths.prefs) && !QFileInfo::exists(kept(g_paths.prefs, 1)));
    expect("gone before the replacement: no notice", sentinel_notice_stands());
    expect("gone before the replacement: not on scratch", !dsd_qt::settings_recovery_scratch(prefsName));
}

/* A later check that finds no file forgets that an earlier one left the file on scratch. */
void
test_missing_forgets_scratch() {
    leave_prefs_on_scratch("missing after scratch");
    expect("missing after scratch: Missing", recover_prefs() == SettingsRecovery::Missing);
    expect("missing after scratch: no longer on scratch",
           !dsd_qt::settings_recovery_scratch(dsd_qt::settings_recovery_prefs_file_name())
               && !dsd_qt::settings_recovery_scratch());
}

/* Renamed aside, then the clean file could not take its place: the file is kept, nothing is at the path, and the run
   writes a scratch copy, so nothing writes the path this run either. */
void
test_replace_fails_after_rename() {
    reset();
    const QByteArray original = "[decode]\nextraArgs=-Z\n\n[zz]\nlater=1\n";
    expect("replace fails: written", write_file(g_paths.prefs, original));
    SettingsRecoveryEnv env;
    env.readFailsAfter = [&original](const QString& path) {
        return path == g_paths.prefs ? original.indexOf("[zz]") : qint64(-1);
    };
    env.writeFails = [](const char* step, const QString&) { return std::strcmp(step, "replace") == 0; };
    expect("replace fails: Scratch", recover_prefs(env) == SettingsRecovery::Scratch);
    expect("replace fails: kept byte-identical, nothing at the path",
           read_file(kept(g_paths.prefs, 1)) == original && !QFileInfo::exists(g_paths.prefs));
    const QString prefsName = dsd_qt::settings_recovery_prefs_file_name();
    expect("replace fails: on scratch, not in memory",
           dsd_qt::settings_recovery_scratch(prefsName) && !dsd_qt::settings_recovery_in_memory(prefsName));
    const dsd_qt::JsonStoreNotice* notice = notice_for(dsd_qt::json_store_notices(), prefsName);
    expect("replace fails: the notice names the kept file",
           notice != nullptr && notice->keptName == "dsd-neo-app.ini.unreadable-1");
    expect("replace fails: the scratch copy holds the prefix",
           prefs_file_now() != g_paths.prefs && AppPrefs::readPortable().value("decode/extraArgs") == "-Z");
    {
        AppPrefs prefs;
        prefs.setAutoStartOnAttach(!prefs.autoStartOnAttach());
        QCoreApplication::processEvents();
    }
    expect("replace fails: AppPrefs' writes leave the path empty", !QFileInfo::exists(g_paths.prefs));
    expect("replace fails: the kept file is untouched", read_file(kept(g_paths.prefs, 1)) == original);
}

/* With no folder for a scratch copy, nothing is written anywhere: the run holds default settings in memory, the file
   stays as it was, and the notice says so (settings_recovery_in_memory()). */
void
test_no_scratch_folder() {
    const QString prefsName = dsd_qt::settings_recovery_prefs_file_name();
    const QString cacheScratch =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/settings-scratch");
    reset();
    QDir(cacheScratch).removeRecursively();
    expect("no scratch folder: written", write_file(g_paths.prefs, kUnreadSection));
    take_every_kept_name("no scratch folder", g_paths.prefs);
    SettingsRecoveryEnv env;
    env.noScratchFolder = [] { return true; };
    expect("no scratch folder: Scratch", recover_prefs(env) == SettingsRecovery::Scratch);
    expect("no scratch folder: in memory",
           dsd_qt::settings_recovery_scratch(prefsName) && dsd_qt::settings_recovery_in_memory(prefsName));
    const QString inMemory = prefs_file_now();
    expect("no scratch folder: the format points where no folder can be made",
           inMemory != g_paths.prefs && !QDir().mkpath(QFileInfo(inMemory).absolutePath())
               && !QFileInfo::exists(QFileInfo(inMemory).absolutePath()));
    expect("no scratch folder: the settings are defaults", !AppPrefs::readPortable().contains("decode/extraArgs"));
    {
        AppPrefs prefs;
        QCoreApplication::processEvents();
        prefs.setAutoStartOnAttach(!prefs.autoStartOnAttach());
        QCoreApplication::processEvents();
    }
    (void)AppPrefs::replacePortable({{"decode/extraArgs", "-Y"}});
    expect("no scratch folder: the original is untouched", read_file(g_paths.prefs) == kUnreadSection);
    expect("no scratch folder: nothing written in the app cache", !QFileInfo::exists(cacheScratch));
    expect("no scratch folder: nothing written where the format points", !QFileInfo::exists(inMemory));
    expect("no scratch folder: the taken names are untouched", kept_names_untouched(g_paths.prefs));
    const dsd_qt::JsonStoreNotice* notice = notice_for(dsd_qt::json_store_notices(), prefsName);
    expect("no scratch folder: a notice, naming no kept file",
           notice != nullptr && notice->keptName.isEmpty() && !notice->reason.isEmpty());

    // Renamed aside before the replacement failed: still in memory, with the kept name in the notice.
    reset();
    const QByteArray original = "[decode]\nextraArgs=-Z\n\n[zz]\nlater=1\n";
    expect("no scratch folder, kept: written", write_file(g_paths.prefs, original));
    env.readFailsAfter = [&original](const QString& path) {
        return path == g_paths.prefs ? original.indexOf("[zz]") : qint64(-1);
    };
    env.writeFails = [](const char* step, const QString&) { return std::strcmp(step, "replace") == 0; };
    expect("no scratch folder, kept: Scratch", recover_prefs(env) == SettingsRecovery::Scratch);
    expect("no scratch folder, kept: in memory", dsd_qt::settings_recovery_in_memory(prefsName));
    notice = notice_for(dsd_qt::json_store_notices(), prefsName);
    expect("no scratch folder, kept: the notice names the kept file",
           notice != nullptr && notice->keptName == "dsd-neo-app.ini.unreadable-1"
               && read_file(kept(g_paths.prefs, 1)) == original);

    relaunch();
    dsd_qt::recover_settings_files();
    expect("no scratch folder: a later start forgets it", !dsd_qt::settings_recovery_in_memory(prefsName));
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
    expect("history settings, no copy: only that file runs on scratch",
           dsd_qt::settings_recovery_scratch(name)
               && !dsd_qt::settings_recovery_scratch(dsd_qt::settings_recovery_prefs_file_name()));
}
#endif

/* A committed restore whose settings hold decode/extraArgs = -R, the settings files removed after it. The backup is
   made on a clean start: settings an earlier case left on scratch would refuse it (issue #828). */
void
commit_restore(const QString& what) {
    reset();
    dsd_qt::recover_settings_files();
    expect(what + ": backup settings", AppPrefs::replacePortable({{"decode/extraArgs", "-R"}}));
    const auto built = dsd_qt::backup::backup_build_archive(dsd_qt::backup::backup_take_snapshot(), nullptr);
    expect(what + ": backup builds", built.error == dsd_qt::backup::BackupError::None);
    const QString root = dsd_qt::json_store_root();
    const dsd_qt::backup::ParseResult parsed = dsd_qt::backup::backup_parse_archive(built.archive, root);
    const QString staging = dsd_qt::backup::staging_dir_for_job("settings");
    QString error;
    expect(what + ": staged", dsd_qt::backup::stage_restore(root, parsed, built.archive, staging, &error));
    expect(what + ": committed", dsd_qt::backup::promote_staging(staging) && dsd_qt::backup::commit_pending());
    reset();
}

/* The restore waits: nothing it would write is touched, and it stays committed for a later start. */
void
expect_restore_waits(const QString& what, const QByteArray& settings) {
    const dsd_qt::backup::ApplyResult waited = dsd_qt::backup::apply_pending_restore({});
    expect(what + ": deferred while the settings run on the scratch copy",
           waited.outcome == dsd_qt::backup::ApplyOutcome::Deferred && !waited.reason.isEmpty());
    expect(what + ": still committed for the next start", dsd_qt::backup::pending_committed());
    expect(what + ": no apply started", !dsd_qt::backup::pending_apply_started());
    expect(what + ": the settings file is untouched", read_file(g_paths.prefs) == settings);
    expect(what + ": the scratch copy did not take the restore",
           AppPrefs::readPortable().value("decode/extraArgs") != "-R");
}

void
expect_restore_applies(const QString& what) {
    const dsd_qt::backup::ApplyResult applied = dsd_qt::backup::apply_pending_restore({});
    expect(what + ": the restore applies", applied.outcome == dsd_qt::backup::ApplyOutcome::Applied);
    expect(what + ": onto the settings file", prefs_file_now() == g_paths.prefs
                                                  && AppPrefs::readPortable().value("decode/extraArgs") == "-R"
                                                  && read_file(g_paths.prefs).contains("-R"));
    expect(what + ": nothing pending", !dsd_qt::backup::pending_exists());
    (void)dsd_qt::backup::clear_result_file();
}

/* Issue #828: settings on their scratch copy cannot go into a backup: refused by name until a later start keeps the
   file aside, then backed up as the clean file holds them. Call history's settings are never in a backup. */
void
test_backup_refused_on_scratch_settings() {
    const QStringList prefsName{dsd_qt::settings_recovery_prefs_file_name()};
    reset();
    expect("backup on scratch: written", write_file(g_paths.prefs, kUnreadSection));
    take_every_kept_name("backup on scratch", g_paths.prefs);
    expect("backup on scratch: Scratch", recover_prefs() == SettingsRecovery::Scratch);
    expect("backup on scratch: refused before the passphrase", dsd_qt::backup::backup_unreadable_files() == prefsName);
    const auto refused = dsd_qt::backup::backup_take_snapshot();
    expect("backup on scratch: the settings file is named", refused.unreadable == prefsName);
    const auto notBuilt = dsd_qt::backup::backup_build_archive(refused, nullptr);
    expect("backup on scratch: refused",
           notBuilt.error == dsd_qt::backup::BackupError::Unreadable && notBuilt.archive.isEmpty());

    relaunch();
    QFile::remove(kept(g_paths.prefs, 3));
    expect("backup on scratch: a later start is Kept", recover_prefs() == SettingsRecovery::Kept);
    const auto snapshot = dsd_qt::backup::backup_take_snapshot();
    expect("backup once kept: nothing unreadable", snapshot.unreadable.isEmpty());
    const auto built = dsd_qt::backup::backup_build_archive(snapshot, nullptr);
    const auto parsed = dsd_qt::backup::backup_parse_archive(built.archive, snapshot.root);
    expect("backup once kept: carries what parsed",
           built.error == dsd_qt::backup::BackupError::None && parsed.prefs.value("decode/extraArgs") == "-Z");

#ifdef HISTORY_SETTINGS_ARE_INI
    reset();
    expect("backup beside history scratch: written", write_file(g_paths.history, "[zz]\nno equals here\n"));
    take_every_kept_name("backup beside history scratch", g_paths.history);
    relaunch();
    dsd_qt::recover_settings_files();
    expect("backup beside history scratch: history on scratch",
           dsd_qt::settings_recovery_scratch(QFileInfo(g_paths.history).fileName()));
    expect("backup beside history scratch: not refused", dsd_qt::backup::backup_take_snapshot().unreadable.isEmpty()
                                                             && dsd_qt::backup::backup_unreadable_files().isEmpty());
#endif
}

/* r3#1: a restore that applied onto the scratch copy would lose its settings at the next start. */
void
test_restore_waits_for_settings() {
    const QByteArray old = "[decode]\nextraArgs=-OLD\n\n[zz]\nno equals here\n";
    commit_restore("restore");
    expect("restore: old settings written", write_file(g_paths.prefs, old));
    take_every_kept_name("restore", g_paths.prefs);
    expect("restore: Scratch", recover_prefs() == SettingsRecovery::Scratch);
    expect_restore_waits("restore", old);
    relaunch();
    QFile::remove(kept(g_paths.prefs, 7));
    dsd_qt::recover_settings_files();
    expect("restore: a later start recovers the settings",
           !dsd_qt::settings_recovery_scratch() && read_file(kept(g_paths.prefs, 7)) == old);
    expect_restore_applies("restore");

    // R1: a read error is renamed aside, so it does not hold the restore back.
    const QByteArray readable = "[decode]\nextraArgs=-OLD\n\n[zz]\nlater=1\n";
    SettingsRecoveryEnv readError;
    readError.readFailsAfter = [readable](const QString& path) {
        return path == g_paths.prefs ? readable.indexOf("[zz]") : qint64(-1);
    };
    commit_restore("restore after a read error");
    expect("restore after a read error: written", write_file(g_paths.prefs, readable));
    relaunch();
    dsd_qt::recover_settings_files(readError);
    expect("restore after a read error: kept, not scratch",
           !dsd_qt::settings_recovery_scratch() && read_file(kept(g_paths.prefs, 1)) == readable);
    expect_restore_applies("restore after a read error");

    commit_restore("restore, read error, no free name");
    expect("restore, read error, no free name: written", write_file(g_paths.prefs, readable));
    take_every_kept_name("restore, read error, no free name", g_paths.prefs);
    relaunch();
    dsd_qt::recover_settings_files(readError);
    expect("restore, read error, no free name: scratch", dsd_qt::settings_recovery_scratch());
    expect_restore_waits("restore, read error, no free name", readable);
    expect("restore, read error, no free name: discarded", dsd_qt::backup::discard_pending());

#ifdef HISTORY_SETTINGS_ARE_INI
    // Only AppPrefs' file holds what a restore writes: call history's settings on scratch do not hold it back.
    commit_restore("restore beside history scratch");
    const QByteArray history = "[zz]\nno equals here\n";
    expect("restore beside history scratch: written", write_file(g_paths.history, history));
    take_every_kept_name("restore beside history scratch", g_paths.history);
    relaunch();
    dsd_qt::recover_settings_files();
    expect("restore beside history scratch: history on scratch",
           dsd_qt::settings_recovery_scratch()
               && dsd_qt::settings_recovery_scratch(QFileInfo(g_paths.history).fileName()));
    expect_restore_applies("restore beside history scratch");
    expect("restore beside history scratch: history settings untouched", read_file(g_paths.history) == history);
#endif
}

#ifndef _WIN32
/* A link at the path: the copy holds what it points at, and the clean file replaces the link itself, never the target.
   A read error renames the link itself aside. */
void
test_link_at_the_path() {
    const QString targetDir = g_paths.configBase + QStringLiteral("/link-target");
    const QString target = targetDir + QStringLiteral("/prefs-target.ini");
    for (const bool readError : {false, true}) {
        const QString what = readError ? QStringLiteral("link, read error") : QStringLiteral("link");
        reset();
        QDir(targetDir).removeRecursively();
        expect(what + ": target written", write_file(target, kUnreadSection));
        expect(what + ": linked", QFile::link(target, g_paths.prefs));
        SettingsRecoveryEnv env;
        if (readError) {
            env.readFailsAfter = [](const QString& path) { return path == g_paths.prefs ? qint64(10) : qint64(-1); };
        }
        expect(what + ": Kept", recover_prefs(env) == SettingsRecovery::Kept);
        expect(what + ": the target is untouched", read_file(target) == kUnreadSection);
        struct stat st;
        expect(what + ": the path is a regular file now",
               lstat(QFile::encodeName(g_paths.prefs).constData(), &st) == 0 && S_ISREG(st.st_mode));
        const QFileInfo keptInfo(kept(g_paths.prefs, 1));
        if (readError) {
            expect(what + ": the link itself is kept", keptInfo.isSymLink() && keptInfo.symLinkTarget() == target);
        } else {
            expect(what + ": the copy holds the target's bytes",
                   !keptInfo.isSymLink() && read_file(kept(g_paths.prefs, 1)) == kUnreadSection);
        }
    }
    QDir(targetDir).removeRecursively();
}
#endif

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
    test_gone_before_kept();
    test_missing_forgets_scratch();
    test_replace_fails_after_rename();
    test_no_scratch_folder();
#ifndef _WIN32
    test_link_at_the_path();
#endif
#ifdef HISTORY_SETTINGS_ARE_INI
    test_call_history_settings();
#endif
    test_restore_waits_for_settings();
    test_backup_refused_on_scratch_settings();

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
