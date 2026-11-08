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
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <utility>

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>

#include "app_prefs.h"
#include "json_store.h"

namespace dsd_qt {

namespace {

/* A settings file that could not be read whole is kept as "<file>.unreadable-1" .. "-99", the first name that is free. */
constexpr int kKeptLimit = 99;

/* Set by the recovery at the start of ui_load(), read by the restore and the backup from any thread. */
std::atomic<bool> g_scratch{false};

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

/* The file QSettings(format, UserScope, organization, application) opens, found without opening it: Qt names it
   "<user path><organization>/<application><extension>" and the organization's own fallback file
   "<user path><organization><extension>", which is the only one an organization-only QSettings opens. Opening the
   settings file itself would read it unchecked (and wait forever on a FIFO), and Qt would keep that read for the next
   QSettings of the file. Empty without both names. */
QString
settings_file_path(QSettings::Format format, const QString& organization, const QString& application) {
    if (organization.isEmpty() || application.isEmpty()) {
        return QString();
    }
    const QString orgFile = QSettings(format, QSettings::UserScope, organization).fileName();
    const qsizetype at = orgFile.lastIndexOf(organization);
    if (at < 0) {
        return QString();
    }
    return orgFile.left(at) + organization + QDir::separator() + application + orgFile.mid(at + organization.size());
}

/* What reading a settings file found. */
enum class FileRead { Missing, Ok, Unreadable };

struct SettingsRead {
    FileRead status = FileRead::Missing;
    QByteArray bytes; // all of it when Ok; what was read before the error when Unreadable
    QString reason;   // why it was Unreadable, for the log
};

SettingsRead
unreadable(const QString& reason, const QByteArray& bytes = QByteArray()) {
    SettingsRead read;
    read.status = FileRead::Unreadable;
    read.bytes = bytes;
    read.reason = reason;
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
        return unreadable(QStringLiteral("could not be opened: %1").arg(file.errorString()));
    }
    QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return unreadable(QStringLiteral("read error: %1").arg(file.errorString()), bytes);
    }
    const qint64 failAfter = env.readFailsAfter ? env.readFailsAfter(path) : -1;
    if (failAfter >= 0) {
        bytes.truncate(failAfter);
        return unreadable(QStringLiteral("read error"), bytes);
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

/* Every QSettings of @p format made from now on reads and writes a scratch folder in the app cache, so nothing this
   run writes the settings file: constructor writes, deferred flushes, static helpers and destruction alike. The
   scratch copy starts as @p seed, the bytes that could be read, which QSettings parses as it would have the file. A
   scratch copy an earlier start left is removed first. */
void
use_scratch(QSettings::Format format, const QString& organization, const QString& application, const QByteArray& seed) {
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString leaf = format == QSettings::IniFormat ? QStringLiteral("ini") : QStringLiteral("native");
    const QString dir =
        cache.isEmpty()
            ? QDir::tempPath()
                  + QStringLiteral("/dsd-neo-settings-scratch-%1-%2").arg(QCoreApplication::applicationPid()).arg(leaf)
            : cache + QStringLiteral("/settings-scratch/") + leaf;
    if (!cache.isEmpty()) {
        QDir(dir).removeRecursively();
    }
    QSettings::setPath(format, QSettings::UserScope, dir);
    g_scratch.store(true);
    const QString scratch = settings_file_path(format, organization, application);
    if (scratch.isEmpty() || !QDir().mkpath(QFileInfo(scratch).absolutePath()) || !write_whole(scratch, seed)) {
        LOG_WARN("Settings run on defaults: their scratch copy could not be written.\n");
    }
}

void
report(const QString& path, const QString& reason, const QString& keptPath) {
    const QString name = QFileInfo(path).fileName();
    const QString keptName = keptPath.isEmpty() ? QString() : QFileInfo(keptPath).fileName();
    if (keptName.isEmpty()) {
        LOG_WARN("%s could not be read (%s) or kept. Settings changed now are not saved; the next start tries again.\n",
                 qUtf8Printable(name), qUtf8Printable(reason));
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
        return SettingsRecovery::Clean;
    }
    const QString reason = problem(read, parsed);
    QString keptPath;
    // Only a file read whole can be kept: what a read error left unread is not in the copy.
    if (read.status == FileRead::Ok && parsed.checked
        && keep_and_rewrite(path, read.bytes, parsed.rewritten, env, &keptPath)) {
        report(path, reason, keptPath);
        return SettingsRecovery::Kept;
    }
    use_scratch(format, organization, application, read.bytes);
    report(path, reason, QString());
    return SettingsRecovery::Scratch;
}

void
recover_settings_files(const SettingsRecoveryEnv& env) {
    g_scratch.store(false);
    (void)recover_ini_settings(QSettings::IniFormat, AppPrefs::settingsOrganization(), AppPrefs::settingsApplication(),
                               env);
    recover_call_history_settings(env);
}

bool
settings_recovery_scratch() {
    return g_scratch.load();
}

} // namespace dsd_qt
