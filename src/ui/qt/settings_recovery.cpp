// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "settings_recovery.h"

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <utility>

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>

#include "app_prefs.h"
#include "json_store.h"
#include "settings_path.h"

namespace dsd_qt {

namespace {

/* A settings file that could not be read whole is kept as "<file>.unreadable-1" .. "-99", the first name that is free. */
constexpr int kKeptLimit = 99;

/* The settings files (by name) running on their scratch copy: set by the recovery at the start of ui_load(), read by
   the restore and the backup from any thread. */
std::mutex g_lock;
QStringList g_scratchFiles;

void
note_scratch(const QString& fileName, bool scratch) {
    const std::lock_guard<std::mutex> locker(g_lock);
    g_scratchFiles.removeAll(fileName);
    if (scratch) {
        g_scratchFiles.append(fileName);
    }
}

/* The platform layer reads a path as UTF-8 on Windows (it calls the wide API) and in the local 8-bit encoding
   elsewhere, as QFile does. */
QByteArray
platform_path(const QString& path) {
#ifdef Q_OS_WIN
    return path.toUtf8();
#else
    return QFile::encodeName(path);
#endif
}

/* What reading a settings file found. */
enum class FileRead { Missing, Ok, Unreadable };

struct SettingsRead {
    FileRead status = FileRead::Missing;
    QByteArray bytes;     // all of it when Ok; what was read before the error when Unreadable
    QString reason;       // why it was Unreadable, for the log
    bool regular = false; // Unreadable, but a regular file: one that could not be opened or read through
};

SettingsRead
unreadable(const QString& reason, const QByteArray& bytes = QByteArray(), bool regular = false) {
    SettingsRead read;
    read.status = FileRead::Unreadable;
    read.bytes = bytes;
    read.reason = reason;
    read.regular = regular;
    return read;
}

/* Reads @p path whole, as json_store reads a store: Missing only on a confirmed absence, a path that is not a regular
   file refused before it is opened (a FIFO would wait for a writer), and a read error checked, which QSettings' own load
   does not. */
SettingsRead
read_settings_file(const QString& path, const SettingsRecoveryEnv& env) {
    const QByteArray native = platform_path(path); // held, so errno is read before anything is freed
    dsd_stat_t st;
    if (dsd_stat_path(native.constData(), &st) != 0) {
        const int err = errno;
        if (err == ENOENT || err == ENOTDIR) {
            return {};
        }
        return unreadable(QStringLiteral("could not be checked: %1").arg(qt_error_string(err)));
    }
    if (dsd_stat_is_regular(&st) == 0) {
        return unreadable(QStringLiteral("not a regular file"));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return unreadable(QStringLiteral("could not be opened: %1").arg(file.errorString()), QByteArray(), true);
    }
    QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return unreadable(QStringLiteral("read error: %1").arg(file.errorString()), bytes, true);
    }
    const qint64 failAfter = env.readFailsAfter ? env.readFailsAfter(path) : -1;
    if (failAfter >= 0) {
        bytes.truncate(failAfter);
        return unreadable(QStringLiteral("read error"), bytes, true);
    }
    SettingsRead read;
    read.status = FileRead::Ok;
    read.bytes = std::move(bytes);
    return read;
}

/* What QSettings makes of a settings file's bytes. */
struct SettingsParse {
    bool checked = false; // false when the temporary files it takes could not be made
    bool clean = false;   // every section parsed with no error
    QByteArray rewritten; // the file written from what parsed: what Qt's next write would produce
};

bool
write_whole(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

/* Parses @p bytes with QSettings' own parser, apart from any fallback file: a copy in a private temporary folder, read
   by an explicit-file QSettings with every section parsed (QSettings parses a section only when something reads it),
   then written back from the keys it parsed. */
SettingsParse
parse_settings(const QByteArray& bytes) {
    SettingsParse out;
    const QTemporaryDir work; // 0700, removed with what is in it
    if (!work.isValid()) {
        return out;
    }
    const QString snapshot = work.filePath(QStringLiteral("snapshot.ini"));
    const QString rewritten = work.filePath(QStringLiteral("rewritten.ini"));
    if (!write_whole(snapshot, bytes)) {
        return out;
    }
    const QSettings parsed(snapshot, QSettings::IniFormat);
    const QStringList keys = parsed.allKeys();
    out.clean = parsed.status() == QSettings::NoError;
    {
        QSettings clean(rewritten, QSettings::IniFormat);
        for (const QString& key : keys) {
            clean.setValue(key, parsed.value(key));
        }
        clean.sync();
        if (clean.status() != QSettings::NoError) {
            return out;
        }
    }
    QFile file(rewritten);
    if (file.exists()) { // nothing parsed: QSettings wrote nothing, and the clean file is empty
        if (!file.open(QIODevice::ReadOnly)) {
            return out;
        }
        out.rewritten = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            return out;
        }
    }
    out.checked = true;
    return out;
}

bool
write_and_sync(FILE* f, const QByteArray& data) {
    const size_t size = static_cast<size_t>(data.size());
    bool ok = size == 0 || std::fwrite(data.constData(), 1, size, f) == size;
    if (std::fflush(f) != 0 || dsd_fsync(dsd_fileno(f)) != 0) {
        ok = false;
    }
    if (std::fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

bool
step_fails(const SettingsRecoveryEnv& env, const char* step, const QString& path) {
    return env.writeFails && env.writeFails(step, path);
}

/* Copies @p bytes to the first free "<path>.unreadable-N", created there (never replacing anything, a link included)
   and synced, while the file itself stays in place. Its path, or empty, with nothing left behind, when no name was free
   or the write failed. */
QString
keep_copy(const QString& path, const QByteArray& bytes, const SettingsRecoveryEnv& env) {
    if (step_fails(env, "keep", path)) {
        return QString();
    }
    for (int n = 1; n <= kKeptLimit; ++n) {
        QString keptPath = path + QStringLiteral(".unreadable-%1").arg(n);
        const QByteArray native = platform_path(keptPath); // held, so errno is read before anything is freed
        FILE* f = dsd_fopen_private(native.constData(), "wbx");
        if (f == nullptr) {
            if (errno == EEXIST) {
                continue;
            }
            return QString();
        }
        if (!write_and_sync(f, bytes)) {
            (void)dsd_remove_file(native.constData());
            return QString();
        }
        dsd_fsync_parent_dir(native.constData());
        return keptPath;
    }
    return QString();
}

/* Replaces @p path with @p bytes: a synced temporary file beside it, renamed over it. Only a file or a link there is
   replaced (the link itself, as json_store does), which is what was read there a moment ago. */
bool
rewrite(const QString& path, const QByteArray& bytes, const SettingsRecoveryEnv& env) {
    if (step_fails(env, "rewrite", path)) {
        return false;
    }
    const QByteArray target = platform_path(path);
    char temp[4096];
    FILE* f = dsd_fopen_private_temp_for_replace(target.constData(), temp, sizeof(temp), "wb");
    if (f == nullptr) {
        return false;
    }
    dsd_path_type type = DSD_PATH_TYPE_OTHER;
    const bool ok = write_and_sync(f, bytes) && dsd_path_type_nofollow(target.constData(), &type) == 1
                    && (type == DSD_PATH_TYPE_FILE || type == DSD_PATH_TYPE_LINK)
                    && dsd_replace_file_with_temp(temp, target.constData()) == 0;
    if (!ok) {
        (void)dsd_remove_file(temp);
    }
    return ok;
}

/* Keeps the file aside and puts the clean one in its place. False, with the file as it was and no copy left (the file
   itself is still there to try again), when either step fails. */
bool
keep_and_rewrite(const QString& path, const QByteArray& bytes, const QByteArray& rewritten,
                 const SettingsRecoveryEnv& env, QString* keptPath) {
    *keptPath = keep_copy(path, bytes, env);
    if (keptPath->isEmpty()) {
        return false;
    }
    if (rewrite(path, rewritten, env)) {
        return true;
    }
    (void)dsd_remove_file(platform_path(*keptPath).constData());
    keptPath->clear();
    return false;
}

/* Renames @p path, never following it (a link moves itself), to its first free kept name, which @p keptPath receives.
   True also when the path is confirmed gone, with nothing kept. False, with nothing moved, when the path cannot be told,
   is a folder, FIFO or other special file (never moved), or no name is free. */
bool
rename_aside(const QString& path, const SettingsRecoveryEnv& env, QString* keptPath) {
    if (step_fails(env, "keep", path)) {
        return false;
    }
    const QByteArray from = platform_path(path);
    dsd_path_type type = DSD_PATH_TYPE_OTHER;
    const int present = dsd_path_type_nofollow(from.constData(), &type);
    if (present == 0) {
        return true;
    }
    if (present < 0 || (type != DSD_PATH_TYPE_FILE && type != DSD_PATH_TYPE_LINK)) {
        return false;
    }
    for (int n = 1; n <= kKeptLimit; ++n) {
        QString candidate = path + QStringLiteral(".unreadable-%1").arg(n);
        const QByteArray to = platform_path(candidate); // held, so errno is read before anything is freed
        if (dsd_rename_noreplace(from.constData(), to.constData(), nullptr) == 0) {
            *keptPath = std::move(candidate);
            return true;
        }
        if (errno != EEXIST) {
            return false;
        }
    }
    return false;
}

/* What renaming a file aside and writing its clean replacement did. */
enum class Renamed { Written, NothingMoved, KeptOnly };

/* A file that could not be read through cannot be copied, so it is renamed aside unread, in the order the stores keep a
   file (json_store): (1) write and sync the clean file's temp beside it, (2) rename the path aside, (3) put the temp in
   its place. A failure at (1) or (2) moves nothing; one at (3) leaves the file kept and nothing at the path. */
Renamed
rename_aside_and_write(const QString& path, const QByteArray& rewritten, const SettingsRecoveryEnv& env,
                       QString* keptPath) {
    if (step_fails(env, "rewrite", path)) {
        return Renamed::NothingMoved;
    }
    const QByteArray target = platform_path(path);
    char temp[4096];
    FILE* f = dsd_fopen_private_temp_for_replace(target.constData(), temp, sizeof(temp), "wb");
    if (f == nullptr) {
        return Renamed::NothingMoved;
    }
    if (!write_and_sync(f, rewritten) || !rename_aside(path, env, keptPath)) {
        (void)dsd_remove_file(temp);
        return Renamed::NothingMoved;
    }
    if (dsd_replace_file_with_temp(temp, target.constData()) != 0) {
        (void)dsd_remove_file(temp);
        return keptPath->isEmpty() ? Renamed::NothingMoved : Renamed::KeptOnly;
    }
    return Renamed::Written;
}

/* Keeps a file that is not clean and puts the clean file in its place, by the copy when it was read whole (the file
   stays in place until the clean one replaces it) or by the rename when it could not be read through. Kept, or
   Scratch when that could not be done; @p keptPath names what was kept either way. */
SettingsRecovery
keep_unclean(const QString& path, const SettingsRead& read, const SettingsParse& parsed, const SettingsRecoveryEnv& env,
             QString* keptPath) {
    if (!parsed.checked) {
        return SettingsRecovery::Scratch;
    }
    if (read.status == FileRead::Ok) {
        return keep_and_rewrite(path, read.bytes, parsed.rewritten, env, keptPath) ? SettingsRecovery::Kept
                                                                                   : SettingsRecovery::Scratch;
    }
    if (read.regular && rename_aside_and_write(path, parsed.rewritten, env, keptPath) == Renamed::Written) {
        return SettingsRecovery::Kept;
    }
    return SettingsRecovery::Scratch;
}

/* Every QSettings of @p format made from now on reads and writes a scratch folder in the app cache, so nothing this
   run writes the settings file: constructor writes, deferred flushes, static helpers and destruction alike. The
   scratch copy starts as @p seed, the keys that parsed. A scratch copy an earlier start left is removed first. Without
   an app cache it lives in a private temporary folder (0700, a random name) held for the process. */
void
use_scratch(QSettings::Format format, const QString& organization, const QString& application, const QByteArray& seed) {
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString leaf = format == QSettings::IniFormat ? QStringLiteral("ini") : QStringLiteral("native");
    QString dir;
    if (cache.isEmpty()) {
        static const QTemporaryDir fallback; // settings hold credentials: never a predictable name in a shared folder
        dir = fallback.isValid() ? fallback.path() + QLatin1Char('/') + leaf : QString();
    } else {
        dir = cache + QStringLiteral("/settings-scratch/") + leaf;
        QDir(dir).removeRecursively();
    }
    if (dir.isEmpty()) {
        // Nowhere to write: every QSettings of the format fails its writes, and the file is still never touched.
        dir = QStringLiteral("/nonexistent-dsd-neo-settings-scratch");
    }
    QSettings::setPath(format, QSettings::UserScope, dir);
    const QString scratch = settings_file_path(format, organization, application);
    if (scratch.isEmpty() || !QDir().mkpath(QFileInfo(scratch).absolutePath()) || !write_whole(scratch, seed)) {
        LOG_WARN("Settings run on defaults: their scratch copy could not be written.\n");
    }
}

void
report(const QString& path, const QString& reason, const QString& keptPath, bool scratch) {
    const QString name = QFileInfo(path).fileName();
    const QString keptName = keptPath.isEmpty() ? QString() : QFileInfo(keptPath).fileName();
    if (scratch) {
        LOG_WARN("%s could not be read (%s) or replaced%s%s. Settings changed now are not saved; the next start tries "
                 "again.\n",
                 qUtf8Printable(name), qUtf8Printable(reason), keptName.isEmpty() ? "" : "; it was kept as ",
                 qUtf8Printable(keptName));
    } else {
        LOG_WARN("%s could not be read (%s). It was kept as %s.\n", qUtf8Printable(name), qUtf8Printable(reason),
                 qUtf8Printable(keptName));
    }
    json_store_post_notice({name, reason, keptName});
}

QString
problem(const SettingsRead& read, const SettingsParse& parsed) {
    if (read.status == FileRead::Unreadable) {
        return read.reason;
    }
    return parsed.checked ? QStringLiteral("not valid INI") : QStringLiteral("could not be checked");
}

/* Call history's settings: a default QSettings (call_history_model.h), named by QSettings' own rule, and an INI file
   unless it is the Windows registry or an Apple plist, which are left alone. */
void
recover_call_history_settings(const SettingsRecoveryEnv& env) {
    const QSettings::Format format = QSettings::defaultFormat();
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    if (format != QSettings::IniFormat) {
        return;
    }
#endif
#ifdef Q_OS_DARWIN
    const QString organization = QCoreApplication::organizationDomain().isEmpty()
                                     ? QCoreApplication::organizationName()
                                     : QCoreApplication::organizationDomain();
#else
    const QString organization = QCoreApplication::organizationName().isEmpty() ? QCoreApplication::organizationDomain()
                                                                                : QCoreApplication::organizationName();
#endif
    (void)recover_ini_settings(format, organization, QCoreApplication::applicationName(), env);
}

} // namespace

SettingsRecovery
recover_ini_settings(QSettings::Format format, const QString& organization, const QString& application,
                     const SettingsRecoveryEnv& env) {
    const QString path = settings_file_path(format, organization, application);
    if (path.isEmpty()) {
        return SettingsRecovery::Missing;
    }
    const SettingsRead read = read_settings_file(path, env);
    if (read.status == FileRead::Missing) {
        return SettingsRecovery::Missing;
    }
    const SettingsParse parsed = parse_settings(read.bytes);
    if (read.status == FileRead::Ok && parsed.checked && parsed.clean) {
        note_scratch(QFileInfo(path).fileName(), false);
        return SettingsRecovery::Clean;
    }
    QString keptPath;
    const SettingsRecovery outcome = keep_unclean(path, read, parsed, env, &keptPath);
    const bool scratch = outcome == SettingsRecovery::Scratch;
    if (scratch) {
        // The keys that parsed, as the clean file would hold them; the bytes read only when nothing could be parsed.
        use_scratch(format, organization, application, parsed.checked ? parsed.rewritten : read.bytes);
    }
    note_scratch(QFileInfo(path).fileName(), scratch);
    report(path, problem(read, parsed), keptPath, scratch);
    return outcome;
}

void
recover_settings_files(const SettingsRecoveryEnv& env) {
    {
        const std::lock_guard<std::mutex> locker(g_lock);
        g_scratchFiles.clear();
    }
    (void)recover_ini_settings(QSettings::IniFormat, AppPrefs::settingsOrganization(), AppPrefs::settingsApplication(),
                               env);
    recover_call_history_settings(env);
}

bool
settings_recovery_scratch() {
    const std::lock_guard<std::mutex> locker(g_lock);
    return !g_scratchFiles.isEmpty();
}

bool
settings_recovery_scratch(const QString& fileName) {
    const std::lock_guard<std::mutex> locker(g_lock);
    return g_scratchFiles.contains(fileName);
}

QString
settings_recovery_prefs_file_name() {
    return AppPrefs::settingsApplication() + QStringLiteral(".ini");
}

} // namespace dsd_qt
