// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "json_store.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <utility>

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>

#include "platform_path.h"

#ifdef DSD_NEO_TEST_HOOKS
#include "json_store_test_hooks.h"
#endif

namespace dsd_qt {

namespace {

/* A file the store could not read is kept as "<name>.unreadable-1" .. "-99", the first name that is free. */
constexpr int kKeptLimit = 99;

/* A marked file is Marked until the first save renames it aside, then Kept until a save replaces its path, which
   clears the mark. */
enum class MarkState { Marked, Kept };

/* Marks and notices are process-wide: call history saves on its worker while the GUI thread loads and saves the other
   stores. The lock is held across a marked file's whole save, so two saves of a marked file never interleave; unmarked
   saves run outside it, and each model keeps its own saves of one file apart (call history: one worker, and the
   destructor's save after it). A std::mutex, which
   ThreadSanitizer understands, unlike QMutex's futex. */
struct Registry {
    std::mutex lock;
    QHash<QString, MarkState> marks; // by absolute path
    QList<JsonStoreNotice> notices;
    std::function<void()> sink;
#ifdef DSD_NEO_TEST_HOOKS
    QString failRead;
    std::function<void(const QString&, const QString&)> afterAside;
#endif
};

Registry&
registry() {
    static Registry r;
    return r;
}

JsonStoreRead
unreadable(const QString& reason) {
    JsonStoreRead read;
    read.status = JsonStoreLoad::Unreadable;
    read.reason = reason;
    return read;
}

#ifdef DSD_NEO_TEST_HOOKS
bool
read_fails_for_test(const QString& fileName) {
    Registry& r = registry();
    const std::lock_guard<std::mutex> locker(r.lock);
    return !r.failRead.isEmpty() && r.failRead == fileName;
}
#endif

/* Adds @p notice, or replaces the one with its file name in place. False when that one already said the same. */
bool
upsert_notice(QList<JsonStoreNotice>& notices, const JsonStoreNotice& notice) {
    for (JsonStoreNotice& existing : notices) {
        if (existing.fileName == notice.fileName) {
            if (existing.reason == notice.reason && existing.keptName == notice.keptName) {
                return false;
            }
            existing = notice;
            return true;
        }
    }
    notices.append(notice);
    return true;
}

QString
reason_for(const QList<JsonStoreNotice>& notices, const QString& fileName) {
    const auto it = std::find_if(notices.cbegin(), notices.cend(),
                                 [&fileName](const JsonStoreNotice& notice) { return notice.fileName == fileName; });
    return it == notices.cend() ? QString() : it->reason;
}

void
mark_unreadable(const QString& fileName, const QString& reason) {
    LOG_WARN("%s could not be read (%s). It is kept aside before it is next saved.\n", qUtf8Printable(fileName),
             qUtf8Printable(reason));
    Registry& r = registry();
    std::function<void()> sink;
    {
        const std::lock_guard<std::mutex> locker(r.lock);
        const QString path = json_store_path(fileName);
        const auto it = r.marks.constFind(path);
        if (it != r.marks.constEnd() && *it == MarkState::Kept) {
            return; // already moved aside: the save that is owed only writes
        }
        r.marks.insert(path, MarkState::Marked);
        if (upsert_notice(r.notices, {fileName, reason, QString()})) {
            sink = r.sink;
        }
    }
    if (sink) {
        sink();
    }
}

bool
save_through_qsavefile(const QString& path, const QByteArray& data, qint64* bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return false;
    }
    if (bytes != nullptr) {
        *bytes = data.size();
    }
    return file.write(data) == data.size() && file.commit();
}

/* What renaming a marked file aside found. */
enum class Aside { None, Kept, Gone, Refused };

/* Renames the marked @p path, never following it, to its first free kept name, which @p keptName receives without its
   folder. A file or a link (the link itself) is moved. Gone when the path is confirmed absent: nothing is left to keep.
   Refused, leaving everything as it was, when it cannot be told whether the path exists, when it is a folder, FIFO,
   socket or device, which is never moved or replaced, or when no kept name is free. A no-replace rename that made the
   new name but could not remove the old one (its out_source_left) has still kept the file. */
Aside
keep_aside(const QString& path, QString* keptName) {
    const QByteArray from = platform_path(path);
    dsd_path_type type = DSD_PATH_TYPE_OTHER;
    const int present = dsd_path_type_nofollow(from.constData(), &type);
    if (present == 0) {
        return Aside::Gone;
    }
    if (present < 0 || (type != DSD_PATH_TYPE_FILE && type != DSD_PATH_TYPE_LINK)) {
        return Aside::Refused;
    }
    for (int n = 1; n <= kKeptLimit; ++n) {
        const QString keptPath = path + QStringLiteral(".unreadable-%1").arg(n);
        const QByteArray to = platform_path(keptPath); // held, so errno is read before anything is freed
        if (dsd_rename_noreplace(from.constData(), to.constData(), nullptr) == 0) {
#ifdef DSD_NEO_TEST_HOOKS
            if (registry().afterAside) { // the caller holds the lock
                registry().afterAside(path, keptPath);
            }
#endif
            *keptName = QFileInfo(keptPath).fileName();
            return Aside::Kept;
        }
        if (errno != EEXIST) {
            return Aside::Refused;
        }
    }
    return Aside::Refused;
}

/* Whether a marked file's path may take the replacement: nothing there (moved aside, or gone), or a file or a link (a
   rename that left its source, or one put back since). A folder, FIFO, socket or device is never replaced, and neither
   is a path that cannot be told. */
bool
replaceable(const QByteArray& target) {
    dsd_path_type type = DSD_PATH_TYPE_OTHER;
    const int present = dsd_path_type_nofollow(target.constData(), &type);
    return present == 0 || (present == 1 && (type == DSD_PATH_TYPE_FILE || type == DSD_PATH_TYPE_LINK));
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

/* How a marked file's save went, and what renaming it aside first found. */
struct MarkedSave {
    bool ok = false;
    Aside aside = Aside::None;
    QString keptName;
};

/* Replaces the marked @p path in the order that keeps it: (1) write and sync a temporary file beside it, (2) rename
   it aside unless it already was (@p kept), (3) replace the path. A failure at (1) or (2) moves nothing; one at (3)
   leaves the file kept. (3) replaces only nothing, a file or a link, so the retry after a kept file never replaces
   what the move would have refused. QSaveFile is not used: it resolves a link at the path and remembers its target. */
MarkedSave
save_marked(const QString& path, bool kept, const QByteArray& data, qint64* bytes) {
    MarkedSave out;
    const QByteArray target = platform_path(path);
    char temp[4096];
    FILE* f = dsd_fopen_private_temp_for_replace(target.constData(), temp, sizeof(temp), "wb");
    if (f == nullptr) {
        return out;
    }
    if (bytes != nullptr) {
        *bytes = data.size();
    }
    bool ok = write_and_sync(f, data);
    if (ok && !kept) {
        out.aside = keep_aside(path, &out.keptName);
        ok = out.aside == Aside::Kept || out.aside == Aside::Gone;
    }
    if (ok) {
        ok = replaceable(target) && dsd_replace_file_with_temp(temp, target.constData()) == 0;
    }
    if (!ok) {
        (void)dsd_remove_file(temp);
    }
    out.ok = ok;
    return out;
}

/* Records what a marked save of @p fileName did. True when the notice list changed. Called with the lock held. */
bool
settle_marked_save(Registry& r, const QString& fileName, const QString& path, const MarkedSave& save) {
    bool changed = false;
    if (save.aside == Aside::Kept) {
        LOG_WARN("%s could not be read. It was kept as %s.\n", qUtf8Printable(fileName), qUtf8Printable(save.keptName));
        r.marks.insert(path, MarkState::Kept);
        changed = upsert_notice(r.notices, {fileName, reason_for(r.notices, fileName), save.keptName});
    }
    if (save.ok || save.aside == Aside::Gone) {
        r.marks.remove(path);
    }
    return changed;
}

} // namespace

QString
json_store_root() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString
json_store_path(const QString& fileName) {
    return json_store_root() + QLatin1Char('/') + fileName;
}

bool
json_store_definitely_missing(const QString& path) {
    const QByteArray native = platform_path(path); // held, so errno is read before anything is freed
    dsd_stat_t st;
    if (dsd_stat_path(native.constData(), &st) == 0) {
        return false;
    }
    return errno == ENOENT || errno == ENOTDIR;
}

JsonStoreRead
json_store_read(const QString& fileName) {
    const QString path = json_store_path(fileName);
    const QByteArray native = platform_path(path); // held, so errno is read before anything is freed
    dsd_stat_t st;
    if (dsd_stat_path(native.constData(), &st) != 0) {
        const int err = errno;
        if (err == ENOENT || err == ENOTDIR) {
            return {};
        }
        return unreadable(QStringLiteral("could not be checked: %1").arg(qt_error_string(err)));
    }
    // Checked before the open, which would wait for a writer on a FIFO.
    if (dsd_stat_is_regular(&st) == 0) {
        return unreadable(QStringLiteral("not a regular file"));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return unreadable(QStringLiteral("could not be opened: %1").arg(file.errorString()));
    }
    const QByteArray bytes = file.readAll();
#ifdef DSD_NEO_TEST_HOOKS
    const bool readError = file.error() != QFileDevice::NoError || read_fails_for_test(fileName);
#else
    const bool readError = file.error() != QFileDevice::NoError;
#endif
    if (readError) {
        return unreadable(QStringLiteral("read error: %1").arg(file.errorString()));
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError) {
        return unreadable(QStringLiteral("not valid JSON at byte %1: %2").arg(parse.offset).arg(parse.errorString()));
    }
    if (!doc.isArray()) {
        return unreadable(QStringLiteral("not a JSON array"));
    }
    JsonStoreRead read;
    read.status = JsonStoreLoad::Ok;
    read.array = doc.array();
    return read;
}

QJsonArray
json_store_load_array(const QString& fileName) {
    JsonStoreRead read = json_store_read(fileName);
    if (read.status == JsonStoreLoad::Unreadable) {
        mark_unreadable(fileName, read.reason);
    }
    return std::move(read.array);
}

bool
json_store_save_array(const QString& fileName, const QJsonArray& array, qint64* bytes) {
    if (bytes != nullptr) {
        *bytes = 0;
    }
    const QString path = json_store_path(fileName);
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray data = QJsonDocument(array).toJson(QJsonDocument::Compact);
    Registry& r = registry();
    std::unique_lock<std::mutex> locker(r.lock);
    const auto it = r.marks.constFind(path);
    if (it == r.marks.constEnd()) {
        locker.unlock();
        return save_through_qsavefile(path, data, bytes);
    }
    const MarkedSave save = save_marked(path, *it == MarkState::Kept, data, bytes);
    const std::function<void()> sink = settle_marked_save(r, fileName, path, save) ? r.sink : std::function<void()>();
    locker.unlock();
    if (sink) {
        sink();
    }
    return save.ok;
}

QList<JsonStoreNotice>
json_store_notices() {
    Registry& r = registry();
    const std::lock_guard<std::mutex> locker(r.lock);
    return r.notices;
}

void
json_store_post_notice(const JsonStoreNotice& notice) {
    Registry& r = registry();
    std::function<void()> sink;
    {
        const std::lock_guard<std::mutex> locker(r.lock);
        if (upsert_notice(r.notices, notice)) {
            sink = r.sink;
        }
    }
    if (sink) {
        sink();
    }
}

void
json_store_set_notice_sink(std::function<void()> sink) {
    Registry& r = registry();
    const std::lock_guard<std::mutex> locker(r.lock);
    r.sink = std::move(sink);
}

#ifdef DSD_NEO_TEST_HOOKS
void
json_store_test_fail_read(const QString& fileName) {
    Registry& r = registry();
    const std::lock_guard<std::mutex> locker(r.lock);
    r.failRead = fileName;
}

void
json_store_test_after_aside(std::function<void(const QString& path, const QString& keptPath)> hook) {
    Registry& r = registry();
    const std::lock_guard<std::mutex> locker(r.lock);
    r.afterAside = std::move(hook);
}
#endif

} // namespace dsd_qt
