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
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <cerrno>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <qsystemdetection.h>
#include <utility>

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>
#ifndef Q_OS_WIN
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "app_prefs.h"
#include "json_store.h"
#include "settings_path.h"

namespace dsd_qt {

namespace {

/* A settings file that could not be read whole is kept as "<file>.unreadable-1" .. "-99", the first name that is free. */
constexpr int kKeptLimit = 99;

/* The settings files (by name) running on their scratch copy, each with whether it had none and runs on defaults held
   in memory: set by the recovery at the start of ui_load(), read by the restore, the backup and the notice card from any
   thread. */
std::mutex g_lock;
QMap<QString, bool> g_scratchFiles;

void
note_scratch(const QString& fileName, bool scratch, bool inMemory = false) {
    const std::lock_guard<std::mutex> locker(g_lock);
    g_scratchFiles.remove(fileName);
    if (scratch) {
        g_scratchFiles.insert(fileName, inMemory);
    }
}

/* The settings formats put on a scratch copy this start: a clean start removes the app cache's copy of any other
   (forget_cache_scratch()). Read and written by the recovery alone, on the GUI thread. */
QList<QSettings::Format> g_scratchFormats;

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

/* An open settings file, closed with its last holder. */
using OpenFile = std::shared_ptr<FILE>;

struct SettingsRead {
    FileRead status = FileRead::Missing;
    QByteArray bytes;     // all of it when Ok; what was read before the error when Unreadable
    QString reason;       // why it was Unreadable, for the log
    bool regular = false; // Unreadable, but a regular file: one that could not be opened or read through
    OpenFile file;        // POSIX, when Ok: the file read, held so a parse in place reads it and not the name again
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

#ifndef Q_OS_WIN
/* Opens the settings file at @p native for reading without ever waiting on it: its path resolved first (a link to the
   file still opens the file), then opened non-blocking with the resolved name never followed, and refused (EINVAL)
   unless it is a regular file, so a FIFO swapped in after the check is refused rather than waited on. Null with errno
   set otherwise. */
OpenFile
open_regular(const QByteArray& native) {
    char resolved[4096];
    if (dsd_path_canonical(native.constData(), resolved, sizeof(resolved)) != 0) {
        return nullptr;
    }
    FILE* f = dsd_fopen_existing_regular_file(resolved, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    return {f, [](FILE* open) { (void)std::fclose(open); }};
}
#endif

/* Opens the settings file checked a moment ago for @p file to read: never by a name a FIFO could have taken meanwhile
   on POSIX (Windows has none at a file path). Empty when it opened, else why not; @p handle holds the POSIX file. */
QString
open_checked(const QString& path, const QByteArray& native, QFile& file, OpenFile& handle) {
#ifdef Q_OS_WIN
    (void)native;
    (void)handle;
    file.setFileName(path);
    return file.open(QIODevice::ReadOnly) ? QString() : file.errorString();
#else
    (void)path;
    handle = open_regular(native);
    if (handle == nullptr) {
        return qt_error_string(errno);
    }
    return file.open(handle.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle) ? QString() : file.errorString();
#endif
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
    QFile file;
    OpenFile handle;
    const QString notOpened = open_checked(path, native, file, handle);
    if (!notOpened.isEmpty()) {
        return unreadable(QStringLiteral("could not be opened: %1").arg(notOpened), QByteArray(), true);
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
    read.file = std::move(handle);
    return read;
}

/* What QSettings makes of a settings file's bytes. */
struct SettingsParse {
    bool checked = false;    // whether `clean` is known: false when the bytes could not be parsed at all
    bool clean = false;      // every section parsed with no error
    bool rewritable = false; // `rewritten` holds the clean file: false when no folder for the parse's files was had
    QByteArray rewritten;    // the file written from what parsed: what Qt's next write would produce
};

bool
write_whole(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

/* Where a parse's work folders go, each named for this code: the app cache, else the system temp folder, where the
   name also carries the app's, since other programs share that folder. */
struct WorkPlace {
    QString dir;
    QString prefix;
};

QList<WorkPlace>
work_places() {
    QList<WorkPlace> places;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cache.isEmpty()) {
        places.append({cache, QStringLiteral("settings-check-")});
    }
    const QString temp = QDir::tempPath(); // empty when TMPDIR names nothing
    if (!temp.isEmpty()) {
        places.append({temp, QStringLiteral("dsd-neo-settings-check-")});
    }
    return places;
}

/* A private folder for a parse's files (0700, a random name, removed with what is in it): in the app cache, else in
   the system temp folder. The app cache comes first because Android has no system temp folder an app may write:
   QDir::tempPath() is /tmp there, which refuses it (EACCES). Null when neither can be had. */
std::unique_ptr<QTemporaryDir>
make_work_folder() {
    const QList<WorkPlace> places = work_places();
    for (const WorkPlace& place : places) {
        if (!QDir().mkpath(place.dir)) {
            continue;
        }
        auto work =
            std::make_unique<QTemporaryDir>(place.dir + QLatin1Char('/') + place.prefix + QStringLiteral("XXXXXX"));
        if (work->isValid()) {
            return work;
        }
    }
    return nullptr;
}

/* The private temporary folder a scratch copy goes to where the app cache cannot take it, named for this code. */
constexpr char kTempScratchPrefix[] = "dsd-neo-settings-scratch-";

#ifndef Q_OS_WIN
/* The names in the folder open as @p fd, "." and ".." left out; @p fd stays open. */
QList<QByteArray>
entries_at(int fd) {
    QList<QByteArray> names;
    const int listed = ::dup(fd);
    DIR* dir = listed >= 0 ? ::fdopendir(listed) : nullptr;
    if (dir == nullptr) {
        if (listed >= 0) {
            (void)::close(listed);
        }
        return names;
    }
    ::rewinddir(dir); // the duplicate shares the folder's read position
    while (const dirent* entry = ::readdir(dir)) {
        const QByteArray name(entry->d_name);
        if (name != "." && name != "..") {
            names.append(name);
        }
    }
    (void)::closedir(dir);
    return names;
}

/* Removes @p name in the folder open as @p parent, never following a link at any step. A regular file or a link (the
   link itself) is unlinked. A folder goes only when this user owns it and it lies at most @p depth folders down: it is
   opened no-follow and checked to be the folder looked at, and its entries are removed through that descriptor, so a
   name swapped for a link meanwhile reaches nothing else. Anything else is left. True when @p name is gone. */
bool
remove_at(int parent, const char* name, int depth) {
    struct stat seen{};
    if (::fstatat(parent, name, &seen, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT;
    }
    if (S_ISREG(seen.st_mode) || S_ISLNK(seen.st_mode)) {
        return ::unlinkat(parent, name, 0) == 0;
    }
    if (!S_ISDIR(seen.st_mode) || seen.st_uid != ::geteuid() || depth < 0) {
        return false;
    }
    const int folder = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (folder < 0) {
        return false;
    }
    struct stat opened{};
    bool emptied = ::fstat(folder, &opened) == 0 && opened.st_dev == seen.st_dev && opened.st_ino == seen.st_ino;
    if (emptied) {
        const QList<QByteArray> names = entries_at(folder);
        for (const QByteArray& entry : names) {
            emptied = remove_at(folder, entry.constData(), depth - 1) && emptied;
        }
    }
    (void)::close(folder);
    return emptied && ::unlinkat(parent, name, AT_REMOVEDIR) == 0;
}

/* Removes the folder @p name in @p dir as remove_at() does, when it is a folder and not a link to one (a file or a link
   of that name is no folder of this code's and is left). @p dir itself may be reached through a link: macOS's /tmp is
   one. */
void
remove_folder(const QString& dir, const QString& name, int depth) {
    const int parent = ::openat(AT_FDCWD, QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) {
        return;
    }
    const QByteArray native = QFile::encodeName(name);
    struct stat seen{};
    if (::fstatat(parent, native.constData(), &seen, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(seen.st_mode)) {
        (void)remove_at(parent, native.constData(), depth);
    }
    (void)::close(parent);
}

QStringList
names_in(const QString& dir) {
    QStringList names;
    const int fd = ::openat(AT_FDCWD, QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return names;
    }
    const QList<QByteArray> entries = entries_at(fd);
    for (const QByteArray& entry : entries) {
        names.append(QFile::decodeName(entry));
    }
    (void)::close(fd);
    return names;
}
#else
/* Windows: the temp folder is the user's own, so who owns a folder is not asked. A link or junction is removed itself,
   never followed; a folder at most @p depth folders down is emptied and removed. */
bool
remove_entry(const QString& path, int depth) {
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction()) {
        return QFile::remove(path) || QDir().rmdir(path);
    }
    if (info.isFile()) {
        return QFile::remove(path);
    }
    if (!info.isDir() || depth < 0) {
        return !info.exists();
    }
    bool emptied = true;
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        emptied = remove_entry(entry.absoluteFilePath(), depth - 1) && emptied;
    }
    return emptied && QDir().rmdir(path);
}

void
remove_folder(const QString& dir, const QString& name, int depth) {
    const QFileInfo info(dir + QLatin1Char('/') + name);
    if (info.isDir() && !info.isSymLink() && !info.isJunction()) {
        (void)remove_entry(info.filePath(), depth);
    }
}

QStringList
names_in(const QString& dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}
#endif

/* What a crash can leave of this code's: a check's work folder (its copy and clean file, so no folder in it), or a
   scratch copy's private temporary folder (<folder>/<ini|native>/<organization>/<file>). */
struct Leftover {
    QString dir;
    QString prefix;
    int depth = 0;
};

/* A check or a run on scratch that never finished (a crash) left a copy of a settings file, which can hold
   credentials. Removed at the next start: only folders named as this code names them, in the folders it makes them
   in, owned by this user, and never through a link (see remove_at()). */
void
sweep_work_folders() {
    QList<Leftover> leftovers;
    const QList<WorkPlace> places = work_places();
    for (const WorkPlace& place : places) {
        leftovers.append({place.dir, place.prefix, 0});
    }
    const QString temp = QDir::tempPath();
    if (!temp.isEmpty()) {
        leftovers.append({temp, QLatin1String(kTempScratchPrefix), 2});
    }
    for (const Leftover& leftover : leftovers) {
        const QStringList names = names_in(leftover.dir);
        for (const QString& name : names) {
            if (name.startsWith(leftover.prefix)) {
                remove_folder(leftover.dir, name, leftover.depth);
            }
        }
    }
}

/* Parses @p bytes with QSettings' own parser, apart from any fallback file: a copy in a private work folder, read by
   an explicit-file QSettings with every section parsed (QSettings parses a section only when something reads it), then
   written back from the keys it parsed. Nothing is known when no work folder can be had. */
SettingsParse
parse_settings(const QByteArray& bytes) {
    SettingsParse out;
    const std::unique_ptr<QTemporaryDir> work = make_work_folder();
    if (work == nullptr) {
        return out;
    }
    const QString snapshot = work->filePath(QStringLiteral("snapshot.ini"));
    const QString rewritten = work->filePath(QStringLiteral("rewritten.ini"));
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
    out.rewritable = true;
    return out;
}

/* With no work folder, a file read whole is parsed by an explicit-file QSettings that only reads: enough to leave a
   healthy file alone, so a folder that cannot be had never makes one "could not be checked". One that is not clean
   gets no clean file. On POSIX Qt reads the file the check opened and read, through its descriptor, never the name
   again, so a FIFO swapped in meanwhile cannot hold the start; Windows has no FIFO at a file path. */
SettingsParse
parse_in_place(const QString& path, const SettingsRead& read) {
    SettingsParse out;
#ifdef Q_OS_WIN
    const QString source = path;
#else
    (void)path;
    if (read.file == nullptr) {
        return out;
    }
    if (std::fseek(read.file.get(), 0, SEEK_SET) != 0) { // /dev/fd duplicates the descriptor, read position included
        return out;
    }
#ifdef Q_OS_LINUX
    const QString source = QStringLiteral("/proc/self/fd/%1").arg(dsd_fileno(read.file.get()));
#else
    const QString source = QStringLiteral("/dev/fd/%1").arg(dsd_fileno(read.file.get()));
#endif
#endif
    const QSettings parsed(source, QSettings::IniFormat);
    (void)parsed.allKeys();
    out.checked = true;
    out.clean = parsed.status() == QSettings::NoError;
    return out;
}

/* What @p read's bytes parse to: through a work folder, else, for a file read whole, in place. */
SettingsParse
check_settings(const QString& path, const SettingsRead& read) {
    SettingsParse parsed = parse_settings(read.bytes);
    if (!parsed.checked && read.status == FileRead::Ok) {
        parsed = parse_in_place(path, read);
    }
    return parsed;
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

/* What a step that keeps or replaces the file found at its path. Gone: the file was confirmed gone since it was read
   (removed by someone else), which counts as a missing file, as if the check had found none. */
enum class Step { Done, Failed, Gone };

/* The file at @p target, never followed: a file or a link may be kept or replaced; Gone when it is confirmed absent;
   Failed when it cannot be told or is a folder, FIFO or other special file (never moved or replaced). */
Step
probe_target(const QByteArray& target) {
    dsd_path_type type = DSD_PATH_TYPE_OTHER;
    const int present = dsd_path_type_nofollow(target.constData(), &type);
    if (present == 0) {
        return Step::Gone;
    }
    return present == 1 && (type == DSD_PATH_TYPE_FILE || type == DSD_PATH_TYPE_LINK) ? Step::Done : Step::Failed;
}

/* Replaces @p path with @p bytes: a synced temporary file beside it, renamed over it. Only a file or a link there is
   replaced (the link itself, as json_store does), which is what was read there a moment ago. */
Step
rewrite(const QString& path, const QByteArray& bytes, const SettingsRecoveryEnv& env) {
    if (step_fails(env, "rewrite", path)) {
        return Step::Failed;
    }
    const QByteArray target = platform_path(path);
    char temp[4096];
    FILE* f = dsd_fopen_private_temp_for_replace(target.constData(), temp, sizeof(temp), "wb");
    if (f == nullptr) {
        return Step::Failed;
    }
    Step step = write_and_sync(f, bytes) ? probe_target(target) : Step::Failed;
    if (step == Step::Done && dsd_replace_file_with_temp(temp, target.constData()) != 0) {
        step = Step::Failed;
    }
    if (step != Step::Done) {
        (void)dsd_remove_file(temp);
    }
    return step;
}

/* Keeps the file aside and puts the clean one in its place. Kept, or Scratch with the file as it was and no copy left
   (the file itself is still there to try again) when either step fails, or Missing with no copy left when the file
   went before its replacement. */
SettingsRecovery
keep_and_rewrite(const QString& path, const QByteArray& bytes, const QByteArray& rewritten,
                 const SettingsRecoveryEnv& env, QString* keptPath) {
    *keptPath = keep_copy(path, bytes, env);
    if (keptPath->isEmpty()) {
        return SettingsRecovery::Scratch;
    }
    const Step step = rewrite(path, rewritten, env);
    if (step == Step::Done) {
        return SettingsRecovery::Kept;
    }
    (void)dsd_remove_file(platform_path(*keptPath).constData());
    keptPath->clear();
    return step == Step::Gone ? SettingsRecovery::Missing : SettingsRecovery::Scratch;
}

/* Renames @p path, never following it (a link moves itself), to its first free kept name, which @p keptPath receives.
   Gone, with nothing kept, when the path is confirmed gone. Failed, with nothing moved, when the path cannot be told,
   is a folder, FIFO or other special file (never moved), or no name is free. */
Step
rename_aside(const QString& path, const SettingsRecoveryEnv& env, QString* keptPath) {
    if (step_fails(env, "keep", path)) {
        return Step::Failed;
    }
    const QByteArray from = platform_path(path);
    const Step present = probe_target(from);
    if (present != Step::Done) {
        return present;
    }
    for (int n = 1; n <= kKeptLimit; ++n) {
        QString candidate = path + QStringLiteral(".unreadable-%1").arg(n);
        const QByteArray to = platform_path(candidate); // held, so errno is read before anything is freed
        if (dsd_rename_noreplace(from.constData(), to.constData(), nullptr) == 0) {
            *keptPath = std::move(candidate);
            return Step::Done;
        }
        if (errno != EEXIST) {
            return Step::Failed;
        }
    }
    return Step::Failed;
}

/* A file that could not be read through cannot be copied, so it is renamed aside unread, in the order the stores keep a
   file (json_store): (1) write and sync the clean file's temp beside it, (2) rename the path aside, (3) put the temp in
   its place. Kept; Scratch when (1) or (2) fails, with nothing moved, or when (3) fails, with the file kept and nothing
   at the path; Missing, with nothing kept or written, when the file was confirmed gone at (2). */
SettingsRecovery
rename_aside_and_write(const QString& path, const QByteArray& rewritten, const SettingsRecoveryEnv& env,
                       QString* keptPath) {
    if (step_fails(env, "rewrite", path)) {
        return SettingsRecovery::Scratch;
    }
    const QByteArray target = platform_path(path);
    char temp[4096];
    FILE* f = dsd_fopen_private_temp_for_replace(target.constData(), temp, sizeof(temp), "wb");
    if (f == nullptr) {
        return SettingsRecovery::Scratch;
    }
    const Step aside = write_and_sync(f, rewritten) ? rename_aside(path, env, keptPath) : Step::Failed;
    if (aside != Step::Done) {
        (void)dsd_remove_file(temp);
        return aside == Step::Gone ? SettingsRecovery::Missing : SettingsRecovery::Scratch;
    }
    if (step_fails(env, "replace", path) || dsd_replace_file_with_temp(temp, target.constData()) != 0) {
        (void)dsd_remove_file(temp);
        return SettingsRecovery::Scratch;
    }
    return SettingsRecovery::Kept;
}

/* Keeps a file that is not clean and puts the clean file in its place, by the copy when it was read whole (the file
   stays in place until the clean one replaces it) or by the rename when it could not be read through. Kept, Scratch
   when that could not be done (@p keptPath names what was kept either way), or Missing when the file went meanwhile. */
SettingsRecovery
keep_unclean(const QString& path, const SettingsRead& read, const SettingsParse& parsed, const SettingsRecoveryEnv& env,
             QString* keptPath) {
    if (!parsed.rewritable) {
        return SettingsRecovery::Scratch;
    }
    if (read.status == FileRead::Ok) {
        return keep_and_rewrite(path, read.bytes, parsed.rewritten, env, keptPath);
    }
    return read.regular ? rename_aside_and_write(path, parsed.rewritten, env, keptPath) : SettingsRecovery::Scratch;
}

/* The private temporary folders (0700, a random name) holding a scratch copy for the rest of the process, by format:
   settings hold credentials, so never a predictable name in a shared folder. */
std::map<QString, std::unique_ptr<QTemporaryDir>> g_tempScratch;

/* A new private temporary folder for @p leaf's scratch copy, held for the process and named for this code, so a start
   after a crash removes it (sweep_work_folders()); empty when none can be made. */
QString
temp_scratch_folder(const QString& leaf) {
    const QString temp = QDir::tempPath();
    if (temp.isEmpty()) {
        return QString();
    }
    auto folder = std::make_unique<QTemporaryDir>(temp + QLatin1Char('/') + QLatin1String(kTempScratchPrefix)
                                                  + QStringLiteral("XXXXXX"));
    if (!folder->isValid()) {
        return QString();
    }
    QString dir = folder->path() + QLatin1Char('/') + leaf;
    g_tempScratch[leaf] = std::move(folder);
    return dir;
}

/* A folder no process can create, root included, so a QSettings there reads nothing and every write it makes fails:
   one under the null device on POSIX (ENOTDIR), and one whose name holds a character Windows refuses in any name. */
QString
uncreatable_folder() {
#ifdef Q_OS_WIN
    return QDir::rootPath() + QStringLiteral("dsd-neo|no-settings-folder");
#else
    return QStringLiteral("/dev/null/dsd-neo-no-settings-folder");
#endif
}

/* Points @p format at @p dir (a copy an earlier start left there removed first) and writes its scratch copy there,
   starting as @p seed. False, with nothing left there, when the copy could not be written. */
bool
seed_scratch(QSettings::Format format, const QString& organization, const QString& application, const QString& dir,
             const QByteArray& seed) {
    QDir(dir).removeRecursively();
    QSettings::setPath(format, QSettings::UserScope, dir);
    const QString scratch = settings_file_path(format, organization, application);
    if (!scratch.isEmpty() && QDir().mkpath(QFileInfo(scratch).absolutePath()) && write_whole(scratch, seed)) {
        return true;
    }
    QDir(dir).removeRecursively();
    return false;
}

/* Every QSettings of @p format made from now on reads and writes a scratch copy, so nothing this run writes the
   settings file: constructor writes, deferred flushes, static helpers and destruction alike. The copy starts as
   @p seed, the keys that parsed, in the app cache, else in a private temporary folder. Where neither takes it, the
   format is pointed where nothing can be written: its settings are defaults held in memory this run, and nothing is
   written anywhere. False then. */
QString
scratch_leaf(QSettings::Format format) {
    return format == QSettings::IniFormat ? QStringLiteral("ini") : QStringLiteral("native");
}

/* A start that leaves @p format on its real file removes the scratch copy an earlier start left in the app cache for
   it, which can hold credentials, unless this start put the format on scratch (another file of it may run there). */
void
forget_cache_scratch(QSettings::Format format) {
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cache.isEmpty() && !g_scratchFormats.contains(format)) {
        remove_folder(cache + QStringLiteral("/settings-scratch"), scratch_leaf(format), 1);
    }
}

bool
use_scratch(QSettings::Format format, const QString& organization, const QString& application, const QByteArray& seed,
            const SettingsRecoveryEnv& env) {
    g_scratchFormats.append(format);
    const QString leaf = scratch_leaf(format);
    if (!env.noScratchFolder || !env.noScratchFolder()) {
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!cache.isEmpty()
            && seed_scratch(format, organization, application, cache + QStringLiteral("/settings-scratch/") + leaf,
                            seed)) {
            return true;
        }
        const QString temp = temp_scratch_folder(leaf);
        if (!temp.isEmpty() && seed_scratch(format, organization, application, temp, seed)) {
            return true;
        }
    }
    QSettings::setPath(format, QSettings::UserScope, uncreatable_folder());
    return false;
}

void
report(const QString& path, const QString& reason, const QString& keptPath, bool scratch, bool inMemory) {
    const QString name = QFileInfo(path).fileName();
    const QString keptName = keptPath.isEmpty() ? QString() : QFileInfo(keptPath).fileName();
    if (inMemory) {
        LOG_WARN("%s could not be read (%s) or replaced%s%s, and no scratch copy could be written. This run uses "
                 "default settings and saves no changes; the next start tries again.\n",
                 qUtf8Printable(name), qUtf8Printable(reason), keptName.isEmpty() ? "" : "; it was kept as ",
                 qUtf8Printable(keptName));
    } else if (scratch) {
        LOG_WARN("%s could not be read (%s) or replaced%s%s. Settings changed now are not saved; the next start tries "
                 "again.\n",
                 qUtf8Printable(name), qUtf8Printable(reason), keptName.isEmpty() ? "" : "; it was kept as ",
                 qUtf8Printable(keptName));
    } else {
        LOG_WARN("%s could not be read (%s). It was kept as %s.\n", qUtf8Printable(name), qUtf8Printable(reason),
                 qUtf8Printable(keptName));
    }
    // The notice says it too, so it differs from a scratch-copy notice for the same file and the card shows the change.
    json_store_post_notice(
        {name, inMemory ? reason + QStringLiteral("; no scratch copy could be written") : reason, keptName});
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

/* recover_ini_settings() for the settings file at @p path. */
SettingsRecovery
recover_file(const QString& path, QSettings::Format format, const QString& organization, const QString& application,
             const SettingsRecoveryEnv& env) {
    // Every outcome replaces what an earlier check noted for the file (a test's simulated second start makes one).
    const QString name = QFileInfo(path).fileName();
    const SettingsRead read = read_settings_file(path, env);
    if (read.status == FileRead::Missing) {
        note_scratch(name, false);
        return SettingsRecovery::Missing;
    }
    const SettingsParse parsed = check_settings(path, read);
    if (read.status == FileRead::Ok && parsed.checked && parsed.clean) {
        note_scratch(name, false);
        return SettingsRecovery::Clean;
    }
    QString keptPath;
    const SettingsRecovery outcome = keep_unclean(path, read, parsed, env, &keptPath);
    if (outcome == SettingsRecovery::Missing) {
        note_scratch(name, false); // gone since it was read: nothing kept, nothing to report
        return outcome;
    }
    const bool scratch = outcome == SettingsRecovery::Scratch;
    // The keys that parsed, as the clean file would hold them; the bytes read only when nothing could be parsed.
    const bool inMemory =
        scratch
        && !use_scratch(format, organization, application, parsed.rewritable ? parsed.rewritten : read.bytes, env);
    note_scratch(name, scratch, inMemory);
    report(path, problem(read, parsed), keptPath, scratch, inMemory);
    return outcome;
}

} // namespace

SettingsRecovery
recover_ini_settings(QSettings::Format format, const QString& organization, const QString& application,
                     const SettingsRecoveryEnv& env) {
    const QString path = settings_file_path(format, organization, application);
    if (path.isEmpty()) {
        return SettingsRecovery::Missing; // no file name, so nothing noted under one
    }
    const SettingsRecovery outcome = recover_file(path, format, organization, application, env);
    if (outcome != SettingsRecovery::Scratch) {
        forget_cache_scratch(format);
    }
    return outcome;
}

void
recover_settings_files(const SettingsRecoveryEnv& env) {
    {
        const std::lock_guard<std::mutex> locker(g_lock);
        g_scratchFiles.clear();
    }
    g_scratchFormats.clear();
    sweep_work_folders();
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

bool
settings_recovery_in_memory(const QString& fileName) {
    const std::lock_guard<std::mutex> locker(g_lock);
    return g_scratchFiles.value(fileName, false);
}

QString
settings_recovery_prefs_file_name() {
    return AppPrefs::settingsApplication() + QStringLiteral(".ini");
}

} // namespace dsd_qt
