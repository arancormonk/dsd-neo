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
 * replacement after the rename; no folder for a scratch copy (defaults in memory, nothing written anywhere); no
 * system temp folder (Android's /tmp) and no app cache for the check, parsed in place through the file read (a FIFO
 * swapped in, a path Qt cannot read, two files of one size and modification time parsed one after the other); an
 * app cache that cannot take the scratch copy (defaults in memory, nothing written outside the cache); leftover work
 * folders and cache scratch copies; a later check forgetting an earlier scratch; the first call-history save after a
 * malformed file; replacePortable() after a recovery; every copy owner-only under umask 022; and a pending restore
 * that waits while the settings run on the scratch copy, then applies after a later start recovers them.
 * Registered only when the Qt frontend is enabled (DSD_ENABLE_QT_UI). */

#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QCoreApplication>
#include <QDateTime>
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
#include <QTemporaryDir>
#include <QTimeZone>
#include <QVariant>
#include <QtGlobal>
#include <cstring>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <functional>
#include <qflags.h>
#include <qsystemdetection.h>
#include <qtenvironmentvariables.h>
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
#include <sys/types.h>
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

/* Issue #828: Qt finds the malformed line only when its write parses every section, then writes without it. */
void
test_unread_section_kept() {
    reset();
    expect("unread section: written", write_file(g_paths.prefs, kUnreadSection));
    expect("unread section: Kept", recover_prefs() == SettingsRecovery::Kept);
    // replacePortable() used to read the FormatError its own write found as a failed restore. First, so no QSettings
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

/* Issue #828: Qt's own load ignores a read error, and its write would replace the whole file with the prefix it read.
   The file is renamed aside unread, and the prefix that parsed takes its place. */
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

/* Issue #828: a malformed organization file is read as a fallback; the settings file itself is fine. */
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

/* Issue #828: with no copy made, nothing may write the original; the next start tries again. */
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

#ifndef _WIN32
/* Renamed aside, then a FIFO took the path before the clean file could: the FIFO is never replaced (a rename over it
   would delete it), the file stays kept, and the run writes a scratch copy, so nothing opens the FIFO either. */
void
test_fifo_before_replacement() {
    reset();
    const QByteArray original = "[decode]\nextraArgs=-Z\n\n[zz]\nlater=1\n";
    expect("FIFO before the replacement: written", write_file(g_paths.prefs, original));
    SettingsRecoveryEnv env;
    env.readFailsAfter = [&original](const QString& path) {
        return path == g_paths.prefs ? original.indexOf("[zz]") : qint64(-1);
    };
    env.writeFails = [](const char* step, const QString& path) {
        if (std::strcmp(step, "replace") == 0 && path == g_paths.prefs) {
            expect("FIFO before the replacement: made", mkfifo(QFile::encodeName(path).constData(), 0600) == 0);
        }
        return false;
    };
    expect("FIFO before the replacement: Scratch", recover_prefs(env) == SettingsRecovery::Scratch);
    { AppPrefs prefs; } // would wait for a writer if it opened the FIFO
    struct stat st;
    expect("FIFO before the replacement: still a FIFO",
           lstat(QFile::encodeName(g_paths.prefs).constData(), &st) == 0 && S_ISFIFO(st.st_mode));
    expect("FIFO before the replacement: kept byte-identical", read_file(kept(g_paths.prefs, 1)) == original);
    expect("FIFO before the replacement: on scratch",
           dsd_qt::settings_recovery_scratch(dsd_qt::settings_recovery_prefs_file_name()));
}
#endif

#ifndef _WIN32
/* The permission bits of @p path, never following a link at it; -1 when they cannot be read. */
int
mode_of(const QString& path) {
    struct stat st;
    return lstat(QFile::encodeName(path).constData(), &st) == 0 ? static_cast<int>(st.st_mode & 07777) : -1;
}

/* Every copy of a settings file, which can hold RadioReference keys, is the owner's alone whatever the umask: the
   parse's work folder and the files parsed there, and the scratch copy and every folder of it made for it, also after
   QSettings' own writes to the copy. */
void
test_settings_copies_owner_only() {
    const mode_t umaskBefore = umask(022);
    const QString scratch =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/settings-scratch");
    QDir(scratch).removeRecursively();
    reset();
    expect("owner only: written", write_file(g_paths.prefs, kUnreadSection));
    expect("owner only: the original is the owner's",
           QFile::setPermissions(g_paths.prefs, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    take_every_kept_name("owner only", g_paths.prefs);
    QMap<QString, int> work;
    SettingsRecoveryEnv env;
    env.workFolderWritten = [&work](const QString& folder) {
        work.insert(folder, mode_of(folder));
        for (const char* name : {"snapshot.ini", "rewritten.ini"}) {
            const QString file = folder + QLatin1Char('/') + QLatin1String(name);
            work.insert(file, mode_of(file));
        }
    };
    expect("owner only: Scratch", recover_prefs(env) == SettingsRecovery::Scratch);
    expect("owner only: on a scratch copy",
           !dsd_qt::settings_recovery_in_memory(dsd_qt::settings_recovery_prefs_file_name()));
    expect("owner only: the work folder was seen", work.size() == 3);
    for (auto it = work.cbegin(); it != work.cend(); ++it) {
        const bool folder = !it.key().endsWith(QLatin1String(".ini"));
        expect(QStringLiteral("owner only: %1 is %2, not %3")
                   .arg(QFileInfo(it.key()).fileName(), QString::number(folder ? 0700 : 0600, 8),
                        QString::number(it.value(), 8)),
               it.value() == (folder ? 0700 : 0600));
    }
    const QString copy = prefs_file_now();
    expect("owner only: the copy is in the cache", copy.startsWith(scratch + QStringLiteral("/ini/")));
    for (QString folder = QFileInfo(copy).absolutePath(); folder.startsWith(scratch);
         folder = QFileInfo(folder).absolutePath()) {
        expect(QStringLiteral("owner only: folder %1 is 700, not %2")
                   .arg(folder.mid(scratch.size()), QString::number(mode_of(folder), 8)),
               mode_of(folder) == 0700);
    }
    expect(QStringLiteral("owner only: the copy is 600, not %1").arg(QString::number(mode_of(copy), 8)),
           mode_of(copy) == 0600);
    {
        AppPrefs prefs;
        prefs.setAutoStartOnAttach(!prefs.autoStartOnAttach());
        QCoreApplication::processEvents();
    }
    expect("owner only: QSettings wrote the copy", AppPrefs::readPortable().contains("decode/extraArgs"));
    expect(
        QStringLiteral("owner only: still 600 after QSettings' writes, not %1").arg(QString::number(mode_of(copy), 8)),
        mode_of(copy) == 0600);
    umask(umaskBefore);
    relaunch();
    reset();
}
#endif

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

/* Sets an environment variable for this process while it lives, then puts back what was there. */
class ScopedEnv {
  public:
    ScopedEnv(const char* name, const QByteArray& value)
        : m_name(name), m_wasSet(qEnvironmentVariableIsSet(name)), m_old(qgetenv(name)) {
        qputenv(m_name, value);
    }

    ~ScopedEnv() {
        if (m_wasSet) {
            qputenv(m_name, m_old);
        } else {
            qunsetenv(m_name);
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

  private:
    const char* m_name;
    bool m_wasSet;
    QByteArray m_old;
};

/* A regular file to point a folder variable at: nothing can be made under it (ENOTDIR), root included. */
QString
not_a_folder() {
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/not-a-folder");
    expect("not a folder: written", write_file(path, "x"));
    return path;
}

/* A healthy file is left alone with no notice and no scratch, whatever folders the check could or could not have. */
void
expect_healthy_left_alone(const QString& what) {
    const QByteArray healthy = "[decode]\nextraArgs=-Z\n\n[location]\nlastFixAt=1\n";
    reset();
    expect(what + ": written", write_file(g_paths.prefs, healthy));
    post_sentinel_notice();
    expect(what + ": Clean", recover_prefs() == SettingsRecovery::Clean);
    expect(what + ": not on scratch", !dsd_qt::settings_recovery_scratch(dsd_qt::settings_recovery_prefs_file_name()));
    expect(what + ": no notice", sentinel_notice_stands());
    expect(what + ": left alone", read_file(g_paths.prefs) == healthy && !QFileInfo::exists(kept(g_paths.prefs, 1)));
    expect(what + ": the real file is used", prefs_file_now() == g_paths.prefs);
}

#ifndef Q_OS_WIN
/* Android has no system temp folder an app may write (QDir::tempPath() is /tmp, which refuses it): the check needs
   none, so a healthy file is left alone at every start and one that is not clean is still kept, never "could not be
   checked" and run on a scratch copy. TMPDIR names a file here. */
void
test_no_system_temp_folder() {
    const QString blocker = not_a_folder();
    {
        const ScopedEnv tmp("TMPDIR", QFile::encodeName(blocker));
        expect("no temp folder: the system temp folder takes nothing", !QTemporaryDir().isValid());
        expect_healthy_left_alone("no temp folder, healthy");

        reset();
        expect("no temp folder, not clean: written", write_file(g_paths.prefs, kUnreadSection));
        expect("no temp folder, not clean: Kept", recover_prefs() == SettingsRecovery::Kept);
        expect("no temp folder, not clean: kept byte-identical", read_file(kept(g_paths.prefs, 1)) == kUnreadSection);
        expect("no temp folder, not clean: not on scratch", !dsd_qt::settings_recovery_scratch());
        relaunch();

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        // No app cache either: the file is parsed where it is, which is enough to leave a healthy one alone.
        const ScopedEnv cache("XDG_CACHE_HOME", QFile::encodeName(blocker + QStringLiteral("/cache")));
        expect("no folder at all: the app cache cannot be made",
               !QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)));
        expect_healthy_left_alone("no folder at all, healthy");
        // One that is not clean gets no clean file and has no folder for a scratch copy: defaults in memory.
        reset();
        expect("no folder at all, not clean: written", write_file(g_paths.prefs, kUnreadSection));
        expect("no folder at all, not clean: Scratch", recover_prefs() == SettingsRecovery::Scratch);
        expect("no folder at all, not clean: in memory",
               dsd_qt::settings_recovery_in_memory(dsd_qt::settings_recovery_prefs_file_name()));
        expect("no folder at all, not clean: the original is untouched", read_file(g_paths.prefs) == kUnreadSection);
        relaunch();
        // Parsed in place, the file is the one the check read, through its descriptor: a FIFO swapped in after the
        // read is never opened, so the start cannot wait on it (it would wait for a writer for ever).
        reset();
        expect("swapped for a FIFO: written", write_file(g_paths.prefs, "[decode]\nextraArgs=-Z\n"));
        SettingsRecoveryEnv swapped;
        swapped.readFailsAfter = [](const QString& path) {
            if (path == g_paths.prefs) {
                QFile::remove(path);
                expect("swapped for a FIFO: made", mkfifo(QFile::encodeName(path).constData(), 0600) == 0);
            }
            return qint64(-1);
        };
        expect("swapped for a FIFO: Clean, judged from the file read",
               recover_prefs(swapped) == SettingsRecovery::Clean);
        expect("swapped for a FIFO: the FIFO is left",
               QFileInfo::exists(g_paths.prefs) && !QFileInfo(g_paths.prefs).isFile());
        QFile::remove(g_paths.prefs);
        relaunch();
        // A parse in place Qt cannot read reads as an empty file, which would pass as clean: the file is not checked.
        reset();
        expect("in place unreadable: written", write_file(g_paths.prefs, kUnreadSection));
        SettingsRecoveryEnv unreachable;
        unreachable.inPlaceSource = [&blocker](const QString&) { return blocker + QStringLiteral("/no-such-file"); };
        post_sentinel_notice();
        expect("in place unreadable: not Clean", recover_prefs(unreachable) == SettingsRecovery::Scratch);
        const dsd_qt::JsonStoreNotice* notice =
            notice_for(dsd_qt::json_store_notices(), dsd_qt::settings_recovery_prefs_file_name());
        expect("in place unreadable: could not be checked",
               notice != nullptr && notice->reason.startsWith(QLatin1String("could not be checked")));
        expect("in place unreadable: the original is untouched", read_file(g_paths.prefs) == kUnreadSection);
        relaunch();
#endif
    }
    reset();
    QFile::remove(blocker);
}
#endif

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
/* No app cache, but a system temp folder anyone could write: nothing of the settings goes there, neither a work folder
   nor a scratch copy. A healthy file is checked in place and left alone; one that is not clean runs on defaults in
   memory. */
void
test_no_cache_writes_nothing_in_temp() {
    const QString blocker = not_a_folder();
    const QString temp =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/tmp-open");
    expect("no cache, open temp: an empty temp folder", QDir().mkpath(temp));
    {
        const ScopedEnv tmp("TMPDIR", QFile::encodeName(temp));
        const ScopedEnv cache("XDG_CACHE_HOME", QFile::encodeName(blocker + QStringLiteral("/cache")));
        expect("no cache, open temp: the temp folder takes a folder", QTemporaryDir().isValid());
        expect("no cache, open temp: no app cache",
               !QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)));
        expect_healthy_left_alone("no cache, open temp, healthy");
        reset();
        expect("no cache, open temp, not clean: written", write_file(g_paths.prefs, kUnreadSection));
        take_every_kept_name("no cache, open temp, not clean", g_paths.prefs);
        expect("no cache, open temp, not clean: Scratch", recover_prefs() == SettingsRecovery::Scratch);
        expect("no cache, open temp, not clean: in memory",
               dsd_qt::settings_recovery_in_memory(dsd_qt::settings_recovery_prefs_file_name()));
        {
            AppPrefs prefs;
            prefs.setAutoStartOnAttach(!prefs.autoStartOnAttach());
            QCoreApplication::processEvents();
        }
        expect("no cache, open temp, not clean: the original is untouched", read_file(g_paths.prefs) == kUnreadSection);
        expect("no cache, open temp: nothing written in the temp folder",
               QDir(temp).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty());
    }
    relaunch();
    reset();
    QDir(temp).removeRecursively();
    QFile::remove(blocker);
}
#endif

#if defined(HISTORY_SETTINGS_ARE_INI) && !defined(Q_OS_MACOS)
/* Gives @p path the modification time @p when, to the millisecond. */
bool
set_modified(const QString& path, const QDateTime& when) {
    QFile file(path);
    return file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)
           && file.setFileTime(when, QFileDevice::FileModificationTime);
}

/* No app cache, so both settings files are parsed in place, one after the other, through the descriptor each check
   opened. QSettings keeps a parse by its path and takes it again unread for a file at that path with the same size and
   modification time: a malformed call history file parsed through the number AppPrefs' clean file had just used would
   pass as clean, and its next save would drop the malformed lines unkept. Each parse reads through its own path. */
void
test_in_place_parses_apart() {
    const QString blocker = not_a_folder();
    const QByteArray clean = "[decode]\nextraArgs=-Z\n";
    const QByteArray malformed = "[zz]\nno equals sign 2\n";
    expect("in place apart: the files are the same size", clean.size() == malformed.size());
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(Q_INT64_C(1700000000123), QTimeZone::UTC);
    {
        const ScopedEnv cache("XDG_CACHE_HOME", QFile::encodeName(blocker + QStringLiteral("/cache")));
        expect("in place apart: no app cache",
               !QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)));
        reset();
        expect("in place apart: AppPrefs' file written", write_file(g_paths.prefs, clean));
        expect("in place apart: history written", write_file(g_paths.history, malformed));
        expect("in place apart: one modification time",
               set_modified(g_paths.prefs, when) && set_modified(g_paths.history, when));
        const QString name = QFileInfo(g_paths.history).fileName();
        post_sentinel_notice();
        dsd_qt::json_store_post_notice({name, QStringLiteral("sentinel"), QString()});
        dsd_qt::recover_settings_files();
        expect("in place apart: AppPrefs' file is clean",
               !dsd_qt::settings_recovery_scratch(dsd_qt::settings_recovery_prefs_file_name())
                   && sentinel_notice_stands() && read_file(g_paths.prefs) == clean);
        expect("in place apart: the history file is not clean", dsd_qt::settings_recovery_in_memory(name));
        const dsd_qt::JsonStoreNotice* notice = notice_for(dsd_qt::json_store_notices(), name);
        expect("in place apart: the history file's notice",
               notice != nullptr && notice->reason.startsWith(QLatin1String("not valid INI")));
        {
            dsd_qt::CallHistoryModel history;
            history.beginSession();
            history.clearAll(); // writes the next uid
            QCoreApplication::processEvents();
        }
        expect("in place apart: the history file is untouched", read_file(g_paths.history) == malformed);
    }
    relaunch();
    reset();
    QFile::remove(blocker);
}
#endif

/* The app cache cannot take the scratch copy (a file stands where its folder would go): the run holds defaults in
   memory and says so, and no copy of the settings is written anywhere else, the system temp folder included. Never
   "on scratch" with nothing in the copy. */
void
test_scratch_copy_not_in_cache() {
    const QString prefsName = dsd_qt::settings_recovery_prefs_file_name();
    const QString cacheScratch =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/settings-scratch");
    const QString temp =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/tmp-empty");
    QDir(cacheScratch).removeRecursively();
    expect("scratch not in cache: a file where the folder goes", write_file(cacheScratch, "not a folder"));
    expect("scratch not in cache: an empty temp folder", QDir().mkpath(temp));
    reset();
    expect("scratch not in cache: written", write_file(g_paths.prefs, kUnreadSection));
    take_every_kept_name("scratch not in cache", g_paths.prefs);
    {
        const ScopedEnv tmp("TMPDIR", QFile::encodeName(temp));
        post_sentinel_notice();
        expect("scratch not in cache: Scratch", recover_prefs() == SettingsRecovery::Scratch);
        expect("scratch not in cache: in memory", dsd_qt::settings_recovery_in_memory(prefsName));
        const dsd_qt::JsonStoreNotice* notice = notice_for(dsd_qt::json_store_notices(), prefsName);
        expect("scratch not in cache: the notice says so",
               notice != nullptr && notice->reason.endsWith(QLatin1String("; no scratch copy could be written")));
        expect("scratch not in cache: the settings are defaults",
               !AppPrefs::readPortable().contains("decode/extraArgs"));
        {
            AppPrefs prefs;
            prefs.setAutoStartOnAttach(!prefs.autoStartOnAttach());
            QCoreApplication::processEvents();
        }
        expect("scratch not in cache: the original is untouched", read_file(g_paths.prefs) == kUnreadSection);
        expect("scratch not in cache: nothing written in the temp folder",
               QDir(temp).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty());
        expect("scratch not in cache: the taken names are untouched", kept_names_untouched(g_paths.prefs));
    }
    QFile::remove(cacheScratch);
    QDir(temp).removeRecursively();
    relaunch();
    reset();
}

/* A check that never finished (a crash) left its work folder in the app cache, with a copy of the settings, which can
   hold credentials: the next start removes it, and nothing else. A link inside goes, never what it points to. A link
   with a work folder's name stays, and so does all of a work folder holding a folder: nothing in it goes. The shared
   temp folder is never touched, whatever is in it. */
void
test_leftover_work_folders() {
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString temp = data + QStringLiteral("/tmp-for-sweep");
    const QString outside = data + QStringLiteral("/outside");
    expect("leftovers: the outside file", write_file(outside + QStringLiteral("/target.ini"), "secret"));
    const QString leftover = cache + QStringLiteral("/settings-check-AbC123");
    expect("leftovers: a work folder's copy", write_file(leftover + QStringLiteral("/snapshot.ini"), "secret"));
    expect("leftovers: its clean file", write_file(leftover + QStringLiteral("/rewritten.ini"), "secret"));
    const QStringList kept{cache + QStringLiteral("/settings-checkup/file"),
                           cache + QStringLiteral("/other/file"),
                           cache + QStringLiteral("/settings-check-Nested/snapshot.ini"),
                           cache + QStringLiteral("/settings-check-Nested/inner/file"),
                           temp + QStringLiteral("/settings-check-Q1w2E3/file"),
                           temp + QStringLiteral("/dsd-neo-settings-check-Xy9Z01/snapshot.ini"),
                           temp + QStringLiteral("/dsd-neo-settings-scratch-Sc1234/ini/dsd-neo/dsd-neo-app.ini")};
    for (const QString& file : kept) {
        expect("leftovers: an unrelated file", write_file(file, "keep"));
    }
#ifndef Q_OS_WIN
    expect("leftovers: a link inside a work folder",
           QFile::link(outside + QStringLiteral("/target.ini"), leftover + QStringLiteral("/link.ini")));
    expect("leftovers: a link named as a work folder",
           QFile::link(outside, cache + QStringLiteral("/settings-check-Link00")));
#endif
    {
        const ScopedEnv tmp("TMPDIR", QFile::encodeName(temp));
        reset();
        dsd_qt::recover_settings_files();
    }
    expect("leftovers: the work folder is gone", !QFileInfo::exists(leftover));
    for (const QString& file : kept) {
        expect(QStringLiteral("leftovers: %1 is untouched").arg(file.mid(data.size())), read_file(file) == "keep");
    }
    expect("leftovers: what a link pointed to is untouched",
           read_file(outside + QStringLiteral("/target.ini")) == "secret");
#ifndef Q_OS_WIN
    expect("leftovers: a link named as a work folder stays",
           QFileInfo(cache + QStringLiteral("/settings-check-Link00")).isSymLink());
    QFile::remove(cache + QStringLiteral("/settings-check-Link00"));
#endif
    QDir(temp).removeRecursively();
    QDir(outside).removeRecursively();
    QDir(cache + QStringLiteral("/settings-checkup")).removeRecursively();
    QDir(cache + QStringLiteral("/other")).removeRecursively();
    QDir(cache + QStringLiteral("/settings-check-Nested")).removeRecursively();
}

/* A start that leaves a format on its real file removes the scratch copy an earlier start left for it in the app cache
   (it can hold credentials); a format this start puts on scratch keeps its copy. */
void
test_clean_start_forgets_cache_scratch() {
    const QString scratch =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/settings-scratch");
    reset();
    expect("forget scratch: written", write_file(g_paths.prefs, kUnreadSection));
    take_every_kept_name("forget scratch", g_paths.prefs);
    dsd_qt::recover_settings_files();
    expect("forget scratch: a copy in the cache", QFileInfo::exists(scratch + QStringLiteral("/ini")));

    relaunch();
    reset();
    expect("forget scratch: the file is clean now", write_file(g_paths.prefs, "[decode]\nextraArgs=-Z\n"));
#ifdef HISTORY_SETTINGS_ARE_INI
    // Call history's file stays unclean, with nowhere to keep it: its format runs on scratch this start.
    expect("forget scratch: history written", write_file(g_paths.history, "[zz]\nno equals here\n"));
    take_every_kept_name("forget scratch, history", g_paths.history);
#endif
    dsd_qt::recover_settings_files();
    expect("forget scratch: AppPrefs' clean start removes its copy",
           !QFileInfo::exists(scratch + QStringLiteral("/ini")));
#ifdef HISTORY_SETTINGS_ARE_INI
    expect("forget scratch: history on scratch keeps its copy", QFileInfo::exists(scratch + QStringLiteral("/native")));
    relaunch();
    reset();
    dsd_qt::recover_settings_files();
    expect("forget scratch: a later start with no history file removes it",
           !QFileInfo::exists(scratch + QStringLiteral("/native")));
#endif
    relaunch();
    reset();
}

#ifndef _WIN32
/* Writes the files a link into @p outside must leave alone: one under each format folder's name, as a scratch copy
   lies there. */
void
write_outside(const QString& outside) {
    expect("scratch links: the outside files", write_file(outside + QStringLiteral("/ini/dsd-neo/keep.txt"), "keep")
                                                   && write_file(outside + QStringLiteral("/native/keep.txt"), "keep")
                                                   && write_file(outside + QStringLiteral("/keep.txt"), "keep"));
}

bool
outside_untouched(const QString& outside) {
    return read_file(outside + QStringLiteral("/ini/dsd-neo/keep.txt")) == "keep"
           && read_file(outside + QStringLiteral("/native/keep.txt")) == "keep"
           && read_file(outside + QStringLiteral("/keep.txt")) == "keep";
}

/* A link at the cache's scratch folder, or at a format folder in it, is never followed by the clean start that forgets
   a scratch copy, nor by the start that seeds one: what it points at is untouched, and the link stays. With no copy
   to be had, the settings run on defaults in memory. */
void
expect_scratch_link_left(const QString& what, const QString& link, const QString& outside) {
    const QString prefsName = dsd_qt::settings_recovery_prefs_file_name();
    write_outside(outside);
    expect(what + ": the link", QFile::link(outside, link));
    reset();
    dsd_qt::recover_settings_files(); // no settings file: Missing, so each format's copy is forgotten
    expect(what + ", missing: what the link points at is untouched", outside_untouched(outside));
    expect(what + ", missing: still a link", QFileInfo(link).isSymLink());

    reset();
    expect(what + ", seeded: written", write_file(g_paths.prefs, kUnreadSection));
    take_every_kept_name(what, g_paths.prefs);
    expect(what + ", seeded: Scratch", recover_prefs() == SettingsRecovery::Scratch);
    expect(what + ", seeded: in memory", dsd_qt::settings_recovery_in_memory(prefsName));
    expect(what + ", seeded: what the link points at is untouched", outside_untouched(outside));
    expect(what + ", seeded: still a link", QFileInfo(link).isSymLink());
    QFile::remove(link);
    QDir(outside).removeRecursively();
    relaunch();
    reset();
}

void
test_scratch_links_never_followed() {
    const QString scratch =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/settings-scratch");
    const QString outside =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/outside-scratch");
    QDir(scratch).removeRecursively();
    expect("scratch links: the cache", QDir().mkpath(QFileInfo(scratch).absolutePath()));
    expect_scratch_link_left("scratch folder linked", scratch, outside);
    expect("scratch links: a real scratch folder", QDir().mkpath(scratch));
    expect_scratch_link_left("format folder linked", scratch + QStringLiteral("/ini"), outside);
    QDir(scratch).removeRecursively();
}
#endif

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
/* Issue #828: call history's own QSettings writes its counters at its first save. */
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

/* Issue #828: a restore that applied onto the scratch copy would lose its settings at the next start. */
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
    test_leftover_work_folders();
    test_clean_start_forgets_cache_scratch();
#ifndef _WIN32
    test_scratch_links_never_followed();
#endif
    test_unread_section_kept();
    test_valid_prefix_read_error();
    test_fallback_ignored();
    test_preservation_failure();
    test_not_a_file();
    test_gone_before_kept();
    test_missing_forgets_scratch();
    test_replace_fails_after_rename();
#ifndef _WIN32
    test_fifo_before_replacement();
    test_settings_copies_owner_only();
#endif
    test_no_scratch_folder();
#ifndef Q_OS_WIN
    test_no_system_temp_folder();
#endif
    test_scratch_copy_not_in_cache();
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    test_no_cache_writes_nothing_in_temp();
#endif
#if defined(HISTORY_SETTINGS_ARE_INI) && !defined(Q_OS_MACOS)
    test_in_place_parses_apart();
#endif
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
