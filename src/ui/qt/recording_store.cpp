// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "recording_store.h"

#include <QByteArray>
#include <QChar>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QMap>
#include <QPointer>
#include <QSaveFile>
#include <QTimeZone>
#include <QVariant>
#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <limits>
#include <qflags.h>
#include <stdint.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include "app_prefs.h"
#include "decoder_host.h"
#include "platform_path.h"
#include "realtime_clock.h"
#include "recording_journal.h"
#include "recording_selection_token.h"

namespace dsd_qt {

namespace {

constexpr QLatin1String kJournalName(kRecordingJournalFileName);
constexpr QLatin1String kFolderFile("folder.json");
constexpr QLatin1String kExplore("explore");
/* Folder names stay short enough for every filesystem and a phone's file browser. */
constexpr qsizetype kMaxNameLength = 48;
/* folder.json is a few dozen bytes; anything much larger is not ours. */
constexpr qint64 kMaxFolderFileBytes = qint64{64} * 1024;
/* Free space is read again this often while a session records, as well as after each publish. */
constexpr quint64 kFreeSpaceCheckMs = 60000U;
/* Journal records applied to the index per hold of the store's lock while a folder is read whole. */
constexpr qsizetype kApplySlice = 128;
/* Candidate folder names for one owner: `<base>`, `<base> 2`, ... */
constexpr int kMaxFolderCandidates = 100;

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

QString
clean_absolute(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool
same_path(const QString& a, const QString& b) {
    return QString::compare(a, b, kPathCase) == 0;
}

/* Leading and trailing spaces and dots: Windows and FAT drop trailing ones, and a leading dot hides a folder. */
void
trim_name(QString* name) {
    qsizetype begin = 0;
    qsizetype end = name->size();
    while (begin < end && (name->at(begin).isSpace() || name->at(begin) == QLatin1Char('.'))) {
        begin++;
    }
    while (end > begin && (name->at(end - 1).isSpace() || name->at(end - 1) == QLatin1Char('.'))) {
        end--;
    }
    *name = name->mid(begin, end - begin);
}

/* What a folder's folder.json says of it: the app's, with its owner (Read); not the app's (Absent: missing, no regular
 * file at the name, too large, not JSON, no owner); or not known now (Unreadable: no access to it, an I/O error), which
 * keeps the folder as the index knows it, unread, until a read can tell. Opened once, never through a link, without
 * waiting on a FIFO (dsd_fopen_existing_regular_file()). */
enum class FolderMarker { Read, Absent, Unreadable };

FolderMarker
read_folder_marker(const QString& dir, QString* uid, QString* kind, QString* name) {
    const QByteArray marker = platform_path(dir + QLatin1Char('/') + kFolderFile);
    FILE* fp = dsd_fopen_existing_regular_file(marker.constData(), "rb");
    if (fp == nullptr) {
        // The opener's errno does not tell a name that is no regular file from a lookup that failed (a socket fails
        // its open with ENXIO; Windows maps a failed attribute read to EINVAL): the name's own type, read without
        // following it, decides. Confirmed missing, or there and no regular file: not the app's. Anything else, a
        // regular file that would not open included: not known now.
        dsd_path_type type = DSD_PATH_TYPE_OTHER;
        const int present = dsd_path_type_nofollow(marker.constData(), &type);
        return present == 0 || (present == 1 && type != DSD_PATH_TYPE_FILE) ? FolderMarker::Absent
                                                                            : FolderMarker::Unreadable;
    }
    QFile file;
    QByteArray data;
    bool read = file.open(fp, QIODevice::ReadOnly);
    if (read) {
        data = file.read(kMaxFolderFileBytes + 1);
        // A read that stopped at an error may still return a prefix with QFile reporting none: the stream's own error
        // flag says so (stream_read_whole()).
        read = file.error() == QFileDevice::NoError && ferror(fp) == 0;
        file.close();
    }
    (void)fclose(fp);
    if (!read) {
        return FolderMarker::Unreadable;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (data.size() > kMaxFolderFileBytes || error.error != QJsonParseError::NoError || !document.isObject()) {
        return FolderMarker::Absent;
    }
    const QJsonObject object = document.object();
    *uid = object.value(QLatin1String("uid")).toString();
    *kind = object.value(QLatin1String("kind")).toString();
    *name = object.value(QLatin1String("name")).toString();
    return uid->isEmpty() ? FolderMarker::Absent : FolderMarker::Read;
}

bool
read_folder_json(const QString& dir, QString* uid, QString* kind, QString* name) {
    return read_folder_marker(dir, uid, kind, name) == FolderMarker::Read;
}

/* The journal at @p journalPath opened once to read, never through a link at its name and never waiting on a FIFO
 * (dsd_fopen_existing_regular_file()): what is read is that regular file. Null with errno set otherwise. */
FILE*
open_journal(const QString& journalPath) {
    return dsd_fopen_existing_regular_file(platform_path(journalPath).constData(), "rb");
}

/* The journal at @p journalPath's @p length bytes from @p offset into @p data, from the one file opened
 * (open_journal()); false at an open, seek or read error, or fewer bytes than the size said. */
bool
read_journal_tail(const QString& journalPath, qint64 offset, qint64 length, QByteArray* data) {
    FILE* fp = open_journal(journalPath);
    if (fp == nullptr) {
        return false;
    }
    QFile journal;
    bool read = journal.open(fp, QIODevice::ReadOnly) && journal.seek(offset);
    if (read) {
        *data = journal.read(length);
        read = data->size() == length && ferror(fp) == 0;
    }
    journal.close();
    (void)fclose(fp);
    return read;
}

bool
write_folder_json(const QString& dir, const QString& uid, const QString& kind, const QString& name) {
    QJsonObject object;
    object.insert(QLatin1String("uid"), uid);
    object.insert(QLatin1String("kind"), kind);
    object.insert(QLatin1String("name"), name);
    QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    data.append('\n');
    QSaveFile file(dir + QLatin1Char('/') + kFolderFile);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace

QString
recording_sanitize_name(const QString& name) {
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        const bool unsafe = u < 0x20U || u == 0x7FU || u == u'/' || u == u'\\' || u == u':' || u == u'*' || u == u'?'
                            || u == u'"' || u == u'<' || u == u'>' || u == u'|';
        out.append(unsafe ? QChar(u'_') : c);
    }
    trim_name(&out);
    if (out.size() > kMaxNameLength) {
        out.truncate(kMaxNameLength);
        if (out.back().isHighSurrogate()) {
            out.chop(1);
        }
        trim_name(&out);
    }
    return out;
}

QString
recording_folder_base_name(const QString& kind, const QString& uid, const QString& name) {
    if (kind == kExplore) {
        return QStringLiteral("Explore");
    }
    QString label = recording_sanitize_name(name);
    if (label.isEmpty()) {
        label = kind == QLatin1String("scan") ? QStringLiteral("Scan list") : QStringLiteral("System");
    }
    const QString tag = recording_sanitize_name(uid.left(8));
    return tag.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, tag);
}

QString
recording_entry_name(const QString& folder, const QString& file) {
    return QFileInfo(folder).fileName() + QLatin1Char('/') + file;
}

bool
recording_journal_tail_settled(QIODevice& file, qint64 offset) {
    if (offset < 0 || !file.seek(offset)) {
        return false;
    }
    const qint64 want = file.size() - offset;
    if (want < 0) {
        return false;
    }
    const QByteArray tail = file.read(want);
    return tail.size() == want && !tail.contains('\n');
}

bool
recording_export_days(const QString& preset, const QDate& today, QDate* first, QDate* last) {
    if (first == nullptr || last == nullptr || !today.isValid()) {
        return false;
    }
    qint64 back = 0;
    if (preset == QLatin1String("all")) {
        *first = QDate();
        *last = QDate();
        return true;
    }
    if (preset == QLatin1String("yesterday")) {
        *first = today.addDays(-1);
        *last = *first;
        return true;
    }
    if (preset == QLatin1String("last7")) {
        back = 6;
    } else if (preset == QLatin1String("last30")) {
        back = 29;
    } else if (preset != QLatin1String("today")) {
        return false;
    }
    *first = today.addDays(-back);
    *last = today;
    return true;
}

QTimeZone
recording_export_press_zone() {
    return QTimeZone(QTimeZone::LocalTime);
}

namespace {

/* The first instant of @p day in @p zone, or of the next day that has one: a date the zone skipped (Pacific/Apia
 * 2011-12-30) has no start of its own. */
qint64
export_day_start(const QDate& day, const QTimeZone& zone) {
    for (int ahead = 0; ahead < 3; ahead++) {
        const QDateTime start = day.addDays(ahead).startOfDay(zone);
        if (start.isValid()) {
            return start.toSecsSinceEpoch();
        }
    }
    return QDateTime(day, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch();
}

} // namespace

bool
recording_export_window(const QString& preset, const QDate& today, const QTimeZone& zone,
                        RecordingExportWindow* window) {
    QDate first;
    QDate last;
    if (window == nullptr || !zone.isValid() || !recording_export_days(preset, today, &first, &last)) {
        return false;
    }
    window->all = first.isNull();
    if (window->all) {
        return true;
    }
    // Resolved here, at the press, once: the scan compares instants only, so a zone change during it moves nothing.
    window->from = export_day_start(first, zone);
    window->to = export_day_start(last.addDays(1), zone);
    return true;
}

bool
recording_export_covers(const RecordingExportWindow& window, qint64 start) {
    return window.all || (start >= window.from && start < window.to);
}

bool
recording_path_inside_root(const QString& path, const QString& root) {
    const QFileInfo info(path);
    const QString leaf = info.fileName();
    if (leaf.isEmpty() || leaf == QLatin1String(".") || leaf == QLatin1String("..")) {
        return false;
    }
    const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    const QString base = QFileInfo(root).canonicalFilePath();
    if (parent.isEmpty() || base.isEmpty()) {
        return false;
    }
    const QStringList inner = (parent + QLatin1Char('/') + leaf).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QStringList outer = base.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (inner.size() <= outer.size()) {
        return false;
    }
    for (qsizetype i = 0; i < outer.size(); i++) {
        if (!same_path(inner.at(i), outer.at(i))) {
            return false;
        }
    }
    return true;
}

RecordingStore::RecordingStore(QObject* parent) : QObject(parent) {
    m_uploadHolds = [](const QString& path) {
        const QByteArray native = platform_path(path);
        return dsd_rdio_upload_holds(native.constData()) != 0;
    };
    // Free bytes only: QStorageInfo also reads the volume's label, which Android's SELinux policy denies the app.
    m_freeSpaceProbe = [](const QString& path) {
        return static_cast<qint64>(dsd_path_free_bytes(platform_path(path).constData()));
    };
    m_remove = [](const QString& path, QString* error) {
        QFile file(path);
        if (file.remove()) {
            return true;
        }
        *error = file.errorString();
        return false;
    };
    char token[DSD_WAV_TEMP_TOKEN_TEXT_SIZE];
    if (dsd_wav_temp_token_text(token, sizeof token) == 0) {
        m_liveToken = QString::fromLatin1(token);
    }
}

RecordingStore::~RecordingStore() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_stopping = true;
    }
    m_wake.notify_all();
    m_passed.notify_all();
    m_settled.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // A transfer still running holds the count, not the store: gone, its selection no longer stands.
    m_storageReads->fetch_add(1U, std::memory_order_release);
}

void
RecordingStore::setRoots(const QStringList& roots) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started) {
        return;
    }
    m_roots.clear();
    for (const QString& root : roots) {
        if (!root.isEmpty()) {
            m_roots.append(clean_absolute(root));
        }
    }
    m_rootAvailable = QList<bool>(m_roots.size(), false);
    m_rootDirMissing = QList<bool>(m_roots.size(), false);
    m_rootMade = QList<quint64>(m_roots.size(), 0U);
}

void
RecordingStore::start() {
    QString fallback;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_started) {
            return;
        }
        m_started = true;
        m_inPass = true;
        fallback = m_roots.isEmpty() ? QString() : m_roots.last();
    }
    // The fallback root lives in the app's own storage, whose folder a first launch may not have made yet; a missing
    // parent of any other root means its storage is away, and is never made in its place.
    if (!fallback.isEmpty()) {
        (void)QDir().mkpath(QFileInfo(fallback).absolutePath());
    }
    m_thread = std::thread(&RecordingStore::run, this);
}

void
RecordingStore::hostSessionReport(bool known, bool active, bool running) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!known) {
            return; // nothing learned: the journals stay held
        }
        const bool first = !m_hostKnown;
        m_hostKnown = true;
        if (active) {
            const bool began = !m_hostActive;
            if (began && !m_starterBegan) {
                m_allActive = true; // a session no starter began here: its folder is not known
            }
            m_hostActive = true;
            m_sawRunning = m_sawRunning || running;
            if (began) {
                // A worker on its idle wait takes up the active cadence from this pass.
                m_passRequested = true;
                m_wake.notify_all();
            }
            return;
        }
        const bool ended = m_hostActive;
        m_hostActive = false;
        if (!first && !ended) {
            return; // idle again (a start clearing an old failure): the marks a starter just made stay
        }
        if (ended) {
            // One decoder runs at a time and takes its starts in order: once a session ran and ended, no earlier start
            // is still on its way. A session that never ran may still be (a start the host gave up on too early).
            if (m_sawRunning) {
                m_unproven.clear();
            } else {
                m_unproven.unite(m_active);
            }
            m_active.clear();
            m_starterBegan = false;
            m_sawRunning = false;
        }
        // A first known state frees only what was held for not knowing it: a start may have marked its folder before
        // the host's first poll (Android polls inside start()), and that session is about to open the journal.
        m_allActive = false;
        m_compactDue = true;
        m_passRequested = true;
    }
    m_wake.notify_all();
}

void
recording_store_follow_host(RecordingStore* store, DecoderHost* host) {
    const auto report = [store, host]() {
        store->hostSessionReport(host->sessionStateKnown(), host->sessionActive(),
                                 host->sessionState() == DecoderHost::Running);
    };
    report();
    QObject::connect(host, &DecoderHost::sessionStateChanged, store, report);
    QObject::connect(host, &DecoderHost::sessionStateKnownChanged, store, report);
}

void
recording_store_serve_host(RecordingStore* store, DecoderHost* host) {
    const QPointer<RecordingStore> guarded(store);
    host->setRecordingIndex([guarded](const QString& path) { return guarded && guarded->isIndexedPath(path); });
    host->setRecordingSelectionCheck([guarded](int selection, RecordingSelectionToken* token) {
        return guarded && guarded->selectionToken(selection, token);
    });
    host->setRecordingHold([guarded](const QString& path) { return guarded ? guarded->selectPath(path) : 0; },
                           [guarded](int selection) {
                               if (guarded) {
                                   guarded->releaseSelection(selection);
                               }
                           });
    QObject::connect(host, &DecoderHost::recordingFilesMissing, store, &RecordingStore::recheckPaths);
    QObject::connect(host, &DecoderHost::recordingTransferFinished, store,
                     [store](int selection, int result, int missing) {
                         (void)result;
                         (void)missing;
                         store->releaseSelection(selection);
                     });
    // Held from Preparing until the player lets it go: a delete waits rather than pulling the file from under it. Only
    // a single recording's play holds its file (#909): parts and playlists hold none, and one starting lets it go.
    const auto follow = [store, host]() { store->setPlayingPath(host->playbackHoldPath()); };
    follow();
    QObject::connect(host, &DecoderHost::playbackChanged, store, follow);
}

void
recording_store_follow_limit(RecordingStore* store, const AppPrefs* prefs) {
    store->setLimitMb(prefs->recordingLimitMb());
    QObject::connect(prefs, &AppPrefs::recordingLimitMbChanged, store,
                     [store, prefs]() { store->setLimitMb(prefs->recordingLimitMb()); });
}

/* --- Worker. --- */

void
RecordingStore::run() {
    const uint64_t began = dsd_realtime_mono_ns();
    runPass(true);
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_rebuildNs = static_cast<qint64>(dsd_realtime_mono_ns() - began);
    }
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const int waitMs = m_allActive || !m_active.isEmpty() ? m_activeMs : m_idleMs;
            m_wake.wait_for(lock, std::chrono::milliseconds(waitMs), [this]() { return m_stop || m_passRequested; });
            if (m_stop) {
                break;
            }
            m_passRequested = false;
            m_inPass = true;
        }
        runPass(false);
    }
}

void
RecordingStore::runPass(bool first) {
    // From the wake (m_inPass, set with m_passRequested cleared, under one hold of the lock) to the end, every export
    // and delete request is refused (storageSettledLocked()): the pass reads storage (the roots, a returning root's
    // folders listed, folders read, journals tailed or read again in place), rechecks files and applies deletes, and
    // the index may meanwhile hold part of what the storage holds, or hold it under the owner a path had before.
    Pass p;
    p.dirty = first;
    refreshRoots(first, &p);
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_ready = true;
        p.dirty = p.dirty || m_foldersChanged;
        m_foldersChanged = false;
    }
    m_settled.notify_all();
    loadPendingFolders(&p);
    if (stopping()) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_inPass = false;
        return;
    }
    tailFolders(&p);
    if (p.reread) {
        // The one rule for deletes asked before a read of storage again (a root listed, a folder read, new or again, a
        // journal replaced in place): every one not done yet is dropped, and its file stays. The read may have given
        // its path, or its id, to another owner's recording (two folders swapped while the storage was away), so a
        // delete asked of what the index held before is never applied to what it holds now; the user can ask again.
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_deleteQueue.isEmpty()) {
            m_deletesCancelled += static_cast<int>(m_deleteQueue.size()); // said on the screens (deletesCancelled())
            m_deleteQueue.clear();
            p.dirty = true;
        }
    }
    processRechecks(&p);
    processDeletes(&p);
    checkLimitAndSpace(first, &p);
    bool compactDue = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        compactDue = m_compactDue;
        m_compactDue = false;
    }
    // Only when the index or the sessions changed: a pass that found nothing new has nothing more to compact.
    if (p.dirty || compactDue) {
        compactFolders();
    }
    publish(p);
    std::function<void()> ended;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_inPass = false;
        m_passEnding = true;
        ended = m_passEndHook;
    }
    // The pass is over and the store settled for the hook; the pass counts as done for a test waiting on it only
    // once the hook has returned (waitForPassForTest()).
    if (ended) {
        ended();
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_passEnding = false;
        m_passes++;
    }
    m_passed.notify_all();
}

void
RecordingStore::checkLimitAndSpace(bool first, Pass* pass) {
    bool limitDue = false;
    bool active = false;
    QList<bool> rootsThere;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        limitDue = m_limitChanged || m_limitPending;
        m_limitChanged = false;
        active = m_allActive || !m_active.isEmpty();
        rootsThere = m_rootAvailable;
    }
    if (first || pass->published || pass->rebuilt || limitDue) {
        enforceLimit(pass);
    }
    // spaceRoot() follows the session's folder and the roots there: a session ending or a root coming or going can move
    // the reading to another volume, so either reads it again at once.
    const bool spaceMoved = active != m_lastFreeActive || rootsThere != m_lastFreeRoots;
    const quint64 now = dsd_realtime_mono_ms();
    if (first || pass->published || pass->rebuilt || spaceMoved
        || (active && now - m_lastFreeCheckMs >= kFreeSpaceCheckMs)) {
        m_lastFreeCheckMs = now;
        m_lastFreeActive = active;
        m_lastFreeRoots = std::move(rootsThere);
        if (checkFreeSpace()) {
            pass->dirty = true;
        }
    }
}

RecordingStore::FileStat
RecordingStore::statOf(const QString& path) {
    // One read, never following a link: what an index entry and a delete's check of it both rest on.
    FileStat stat;
    stat.regular = dsd_path_file_identity(platform_path(path).constData(), &stat.identity) == 0;
    if (stat.regular) {
        stat.size = stat.identity.size;
        stat.mtime = static_cast<qint64>(stat.identity.mtime_s);
        return stat;
    }
    // Missing, or known to be no file (a folder, a link, a FIFO, a device); any other failure (no access to the name,
    // an I/O error) says nothing about whether a file is there.
    // Only ENOENT is a name confirmed missing; a parent that does not resolve (ENOTDIR, or EMLINK for too many links on
    // the way) leaves the name's own type unread, as any other failure does.
    const int err = errno;
    stat.missing = err == ENOENT;
    stat.unreadable = !stat.missing && err != EISDIR && err != ELOOP && err != ENODEV;
    return stat;
}

bool
RecordingStore::opensAgainLocked(const QString& path, const FileStat& stat) {
    // A recording a transfer could not open stays unreadable until an open succeeds: its metadata read is not enough.
    if (!stat.regular || !m_unopenable.contains(path)) {
        return true;
    }
    if (!opensToRead(path)) {
        return false;
    }
    m_unopenable.remove(path);
    return true;
}

bool
RecordingStore::opensToRead(const QString& path) {
    // As a transfer opens it: once, never through a link, never waiting on a FIFO.
    FILE* fp = dsd_fopen_existing_regular_file(platform_path(path).constData(), "rb");
    if (fp == nullptr) {
        return false;
    }
    (void)fclose(fp);
    return true;
}

bool
RecordingStore::rootAvailableNow(const QString& root) {
    const QFileInfo info(root);
    if (info.exists()) {
        return info.isDir() && QDir(root).isReadable();
    }
    return !info.isSymLink() && QFileInfo(info.absolutePath()).isDir();
}

bool
RecordingStore::rootThereForIndex(int root, const QString& path) const {
    return rootPlayableNow(path) || rootMissingTakenAsThere(root, path);
}

bool
RecordingStore::rootMissingTakenAsThere(int root, const QString& path) const {
    if (!rootAvailableNow(path)) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    return !rootHoldsRowsLocked(root);
}

bool
RecordingStore::rootThereForIndexLocked(int root) const {
    const QString& path = m_roots.at(root);
    return rootPlayableNow(path) || (rootAvailableNow(path) && !rootHoldsRowsLocked(root));
}

void
RecordingStore::noteRootMadeLocked(int root) {
    m_rootDirMissing[root] = false; // made here: nothing appeared that a listing must find
    m_rootMade[root]++;
    if (rootHoldsRowsLocked(root)) {
        // Rows indexed on a directory that was missing (renamed or removed, perhaps before any pass marked the root
        // away): what is there now is read in full, and their rows are unavailable until then (R74, R78).
        noteRootSeenAwayLocked(root);
    }
}

bool
RecordingStore::rootHoldsRowsLocked(int root) const {
    const auto onRoot = [this, root](const QString& path) {
        const auto folder = m_folders.constFind(path);
        return folder != m_folders.cend() && folder->root == root;
    };
    for (auto it = m_rowsInFolder.cbegin(); it != m_rowsInFolder.cend(); ++it) {
        if (it.value() > 0 && onRoot(it.key())) {
            return true;
        }
    }
    for (auto it = m_committed.cbegin(); it != m_committed.cend(); ++it) {
        if (!it->entries.isEmpty() && onRoot(it.key())) {
            return true;
        }
    }
    return false;
}

void
RecordingStore::refreshRoots(bool first, Pass* pass) {
    QStringList roots;
    QList<bool> was;
    QSet<int> seenAway;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        roots = m_roots;
        was = m_rootAvailable;
        seenAway.swap(m_rootsSeenAway);
        m_rootsRereading = seenAway; // still seen away to the readers until this read is done (R74)
    }
    for (int i = 0; i < roots.size(); i++) {
        // Its directory missing while it holds rows is away (R83): the rows stay, unavailable, until it is back.
        // Missing with nothing indexed on it (I1: a cold start) it is there and empty, and remembered as missing, so
        // its directory found since is listed in full, as a root back from away is. A seen-away note made since the
        // pass's top is taken into this listing, not left for another (m1).
        bool now = false;
        if (listRootNow(i, roots.at(i), first, was.at(i), &seenAway, &now)) {
            if (!rebuildRoot(i, pass) && was.at(i)) {
                // Gone again as it was listed: away, so its return is read in full too.
                const std::lock_guard<std::mutex> lock(m_mutex);
                markRootAwayLocked(i, pass);
            }
        } else if (now != was.at(i)) {
            // Gone: what it held stays indexed, untouched, until it comes back and is read again.
            const std::lock_guard<std::mutex> lock(m_mutex);
            markRootAwayLocked(i, pass);
        }
        if (seenAway.contains(i)) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_rootsRereading.remove(i); // read as it is now: the index says what is there
        }
    }
}

bool
RecordingStore::listRootNow(int root, const QString& path, bool first, bool was, QSet<int>* seenAway, bool* there) {
    bool missingBefore = false;
    quint64 made = 0U;
    {
        // The flag as it is at this root's turn, not at the pass's top: a session start may have made the directory.
        const std::lock_guard<std::mutex> lock(m_mutex);
        missingBefore = m_rootDirMissing.value(root);
        made = m_rootMade.value(root);
    }
    const bool playable = rootPlayableNow(path);
    const bool now = playable || rootMissingTakenAsThere(root, path);
    *there = now;
    const std::lock_guard<std::mutex> lock(m_mutex);
    // Written only from a look no session start overtook (m1): a directory made meanwhile was made by the store, and
    // its flag is already clear.
    const bool current = m_rootMade.value(root) == made;
    if (current) {
        m_rootDirMissing[root] = now && !playable;
    }
    const bool appeared = current && playable && missingBefore;
    const bool list = now && (first || !was || seenAway->contains(root) || appeared);
    if (list && m_rootsSeenAway.remove(root)) {
        // Noted seen away since the pass's top (an export's look at the flag, say): this listing covers it (m1).
        seenAway->insert(root);
        m_rootsRereading.insert(root);
    }
    return list;
}

void
RecordingStore::markRootAwayLocked(int root, Pass* pass) {
    if (root < 0 || root >= m_rootAvailable.size()) {
        return;
    }
    m_rootAvailable[root] = false;
    // The worker the first to see it gone: every selection made before is stale from now on, as for any loss seen.
    m_storageReads->fetch_add(1U, std::memory_order_release);
    noteQueryChangeLocked(pass); // its recordings read as away
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (!it->fragment.isEmpty() && m_folders.value(it->folder).root == root) {
            pass->fragments.insert(it->fragment);
        }
    }
}

bool
RecordingStore::rootStillAvailable(int root, Pass* pass) {
    QString rootPath;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (root < 0 || root >= m_roots.size()) {
            return true;
        }
        if (!m_rootAvailable.at(root)) {
            return false; // away since earlier in this pass: nothing more is decided about it until it is read again
        }
        rootPath = m_roots.at(root);
    }
    if (rootThereForIndex(root, rootPath)) {
        return true;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_rootAvailable.value(root)) {
        markRootAwayLocked(root, pass);
    }
    return false;
}

bool
RecordingStore::rootsStillAvailable(const QList<bool>& roots, Pass* pass) {
    bool all = true;
    for (int i = 0; i < roots.size(); i++) {
        // Every one is checked, so each that went is marked away.
        if (roots.at(i) && !rootStillAvailable(i, pass)) {
            all = false;
        }
    }
    return all;
}

bool
RecordingStore::rebuildRoot(int root, Pass* pass) {
    QString rootPath;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        rootPath = m_roots.at(root);
    }
    noteRereadStart(pass);
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listing.insert(root, {});
    }
    // The folder table first, folder.json only: what a session start needs. The journals are read after
    // (loadPendingFolders()), so a start never waits for them.
    QList<Folder> table;
    const QStringList subdirs = QDir(rootPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QString& subdir : subdirs) {
        Folder folder;
        folder.path = clean_absolute(rootPath + QLatin1Char('/') + subdir);
        folder.root = root;
        // One whose folder.json cannot be read now goes in with no owner: the index's owner for it is kept
        // (installRootTableLocked()), and it stays unread until a read can tell.
        if (!QFileInfo(folder.path).isSymLink()
            && read_folder_marker(folder.path, &folder.uid, &folder.kind, &folder.name) != FolderMarker::Absent) {
            table.append(folder);
        }
    }
    if (m_rootReadHook) {
        m_rootReadHook(rootPath); // listed and its folder.json read, the table not installed yet
    }
    // Gone again while it was listed: an empty or short listing is not a root emptied. It stays away, what it held
    // stays indexed, and the next pass reads it again.
    const bool there = rootThereForIndex(root, rootPath);
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (there) {
        installRootTableLocked(root, table, pass);
    }
    m_listing.remove(root);
    return there;
}

void
RecordingStore::installRootTableLocked(int root, QList<Folder>& table, Pass* pass) {
    // Whatever the index held for this root is read again from the disk. One hold of the lock, so a start never sees
    // the root available without its folders. A folder the listing no longer has leaves with its recordings; one it
    // still has keeps them until its read (loadFolder()) replaces them, so storage that leaves again during that read
    // has dropped nothing.
    QSet<QString> stale;
    QSet<QString> held;
    for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
        if (it->root == root) {
            stale.insert(it.key());
            if (it->loaded || it->rereading) {
                held.insert(it.key());
            }
        }
    }
    for (Folder& folder : table) {
        stale.remove(folder.path);
        folder.rereading = held.contains(folder.path);
        const auto known = folder.uid.isEmpty() ? m_folders.constFind(folder.path) : m_folders.cend();
        if (known != m_folders.cend()) {
            folder.uid = known->uid; // folder.json not readable now: the owner the index knew
            folder.kind = known->kind;
            folder.name = known->name;
        }
    }
    const QSet<QString> made = keepSessionFoldersLocked(root, &table, &stale);
    QStringList ids;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (stale.contains(it->folder) || made.contains(it->folder)) {
            ids.append(it.key());
        }
    }
    for (const QString& id : ids) {
        removeEntryLocked(id, pass);
    }
    for (const QString& path : stale) {
        m_folders.remove(path);
    }
    for (const Folder& folder : std::as_const(table)) {
        m_folders.insert(folder.path, folder);
    }
    m_rootAvailable[root] = true;
    pass->rebuilt = true;
    // What it kept indexed reads as there again. Today the read of its folders later in this pass replaces every entry
    // it kept, which moves the generation too; the rule is kept here so it does not rest on that read.
    noteQueryChangeLocked(pass);
}

void
RecordingStore::noteRereadStart(Pass* pass) {
    if (pass->reread) {
        return;
    }
    // A pass's first read of storage again: every selection made before it is stale from now on (selectionCurrent()).
    pass->reread = true;
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_storageReads->fetch_add(1U, std::memory_order_release);
}

void
RecordingStore::loadPendingFolders(Pass* pass) {
    struct Pending {
        QString path;
        int root = -1;
        bool rereading = false;
    };

    QList<Pending> pending;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const qint64 now = static_cast<qint64>(dsd_realtime_mono_ms());
        for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
            // One whose last read failed waits for its retry (deferFolderReadLocked()); it stays unread meanwhile.
            if (!it->loaded && m_rootAvailable.value(it->root) && it->retryAtMs <= now) {
                pending.append({it.key(), it->root, it->rereading});
            }
        }
    }
    for (const Pending& folder : std::as_const(pending)) {
        if (stopping()) {
            return;
        }
        loadFolder(folder.path, folder.root, folder.rereading, pass);
    }
}

void
RecordingStore::loadFolder(const QString& path, int root, bool replace, Pass* pass) {
    noteRereadStart(pass);
    // A session start that adopts or relabels the folder while it is read (the lock is not held for the read) makes
    // what was read another owner's, or another label's: nothing is committed then, and the folder is read again.
    quint64 starts = 0U;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        starts = m_folders.value(path).starts;
    }
    Folder folder;
    if (!readFolderOwner(path, root, starts, &folder, pass)) {
        return;
    }
    folder.path = path;
    folder.root = root;
    if (m_folderLoadHook) {
        m_folderLoadHook(path);
    }

    // The journal first: one that cannot be read whole leaves the folder unread, with what the index held for it, and
    // none of its files is listed or looked at for a read that would be dropped. It is read again once its retry is
    // due (deferFolderReadLocked()). Until then the store is not settled, so exports and deletes are refused rather
    // than taken from part of the recordings. (A recording it names whose own name cannot be read is kept,
    // applyPublishLocked(), and holds nothing.) One that went away with its root is left as the index held it.
    QByteArray data;
    if (!readJournal(path + QLatin1Char('/') + kJournalName, &data)) {
        if (rootStillAvailable(root, pass)) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            deferFolderReadLocked(path, pass);
        }
        return;
    }
    QHash<QString, FileStat> files;
    QList<Entry> unfinished;
    listFolder(path, &files, &unfinished);
    const qsizetype consumed = data.lastIndexOf('\n') + 1;
    QList<RecordingJournalRecord> records;
    const qint64 lines = parseLines(data.left(consumed), &records);
    statPublished(path, records, &files);
    // What was read stands only with the root still there: one that went away while the folder was read took its
    // files and journal with it, and the folder stays as the index held it.
    if (!rootStillAvailable(root, pass)) {
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_folders.value(path).starts != starts) {
            scheduleFolderReadLocked(path, pass);
            m_passRequested = true;
            return;
        }
        if (replace) {
            holdCommittedFolderLocked(path);
            removeFolderEntriesLocked(path, pass);
        }
        folder.offset = consumed;
        folder.starts = starts;
        folder.lines = lines;
        folder.loaded = true;
        // The owner is the one folder.json names. A session start may have relabelled the same owner's folder since
        // it was read above, and that newer label wins; a table that holds another owner for this path (B's folder
        // put where A's was) never keeps it: the owner on disk is installed, and its recordings are what the read
        // finds (a first read holds none of A's; a read again dropped them above).
        // The label read here is the newest: a session start that relabelled the folder during the read moved its
        // starts count, and the read was not committed above.
        const auto table = m_folders.constFind(path);
        if (table != m_folders.cend() && table->uid != folder.uid) {
            removeFolderEntriesLocked(path, pass);
            m_foldersChanged = true;
        }
        // Read: the GUI learns the folder is (indexed), even when no recording in it changed.
        pass->dirty = true;
        m_folders.insert(path, folder);
        // Grown once here, not rehashed under the lock slice by slice.
        m_entries.reserve(m_entries.size() + records.size() + unfinished.size());
        m_byPath.reserve(m_byPath.size() + records.size());
        m_byFragment.reserve(m_byFragment.size() + records.size());
        for (const Entry& entry : unfinished) {
            insertEntryLocked(entry, pass);
        }
    }
    applyRecordsInSlices(folder, records, files, pass);
    if (replace) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        commitFolderLocked(path, pass);
    }
}

bool
RecordingStore::tailCursorCurrentLocked(const QString& path, qint64 offset, quint64 starts) const {
    // Applied only to the folder as the cursor found it: still read, at that offset, and no session start since that
    // adopted it or could not confirm its owner (starts). Otherwise its whole read comes first.
    const auto folder = m_folders.constFind(path);
    return folder != m_folders.cend() && folder->loaded && folder->offset == offset && folder->starts == starts;
}

bool
RecordingStore::readFolderOwner(const QString& path, int root, quint64 starts, Folder* folder, Pass* pass) {
    const FolderMarker marker = read_folder_marker(path, &folder->uid, &folder->kind, &folder->name);
    if (marker == FolderMarker::Read) {
        return true;
    }
    if (marker == FolderMarker::Unreadable) {
        // There and not readable now (no access, an I/O error): the folder stays known, unread, with what the index
        // held for it, and is read again once its retry is due.
        if (rootStillAvailable(root, pass)) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            deferFolderReadLocked(path, pass);
        }
        return false;
    }
    // A root that went away since the pass began (unmounted, a USB session) took the folder.json with it: nothing in it
    // is deleted.
    if (!rootStillAvailable(root, pass)) {
        return false;
    }
    // Not a folder the app made, or not any more (removed from a computer while its root is there, perhaps between the
    // folder table's read and this one): nothing in it is managed, and no session is given it. One a start adopted
    // meanwhile is read again instead.
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_folders.value(path).starts != starts) {
        scheduleFolderReadLocked(path, pass);
        m_passRequested = true;
        return false;
    }
    removeFolderEntriesLocked(path, pass);
    m_folders.remove(path);
    pass->dirty = true;
    return false;
}

bool
RecordingStore::readJournal(const QString& journalPath, QByteArray* data) {
    // Opened once and read from that file (open_journal()). Only a name confirmed missing (ENOENT) is an empty journal
    // (a folder a session made, nothing published); anything else not read whole is a read that failed: no access to
    // it, an I/O error, a parent that does not resolve, a link, a folder or a FIFO at its name.
    FILE* fp = open_journal(journalPath);
    if (fp == nullptr) {
        // Only the name not in its folder, which is there (dsd_path_file_identity() checks the parent), is no journal.
        dsd_file_identity_t none{};
        return errno == ENOENT && dsd_path_file_identity(platform_path(journalPath).constData(), &none) != 0
               && errno == ENOENT;
    }
    QFile journal;
    bool read = journal.open(fp, QIODevice::ReadOnly);
    if (read) {
        *data = journal.readAll();
        // A read that stopped at an error can return a prefix with QFile reporting none; the stream's flag tells.
        read = journal.error() == QFileDevice::NoError && ferror(fp) == 0;
    }
    journal.close();
    (void)fclose(fp);
    return read;
}

void
RecordingStore::noteTailReadFailed(const QString& path, Pass* pass) {
    // The new lines could not be read (no access, an I/O error): the folder is not settled until a whole read of it, at
    // the next pass, succeeds. One that went away since the pass began is left as it is.
    int root = -1;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        root = m_folders.value(path).root;
    }
    if (rootStillAvailable(root, pass)) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        scheduleFolderReadLocked(path, pass);
    }
}

void
RecordingStore::scheduleFolderReadLocked(const QString& path, Pass* pass) {
    markFolderUnreadLocked(path);
    pass->dirty = true; // the next pass reads it whole (loadPendingFolders())
}

void
RecordingStore::markFolderUnreadLocked(const QString& path) {
    const auto it = m_folders.find(path);
    if (it != m_folders.end()) {
        it->rereading = it->rereading || it->loaded;
        it->loaded = false;
        it->retryAtMs = 0; // read at the next pass
    }
}

void
RecordingStore::deferFolderReadLocked(const QString& path, Pass* pass) {
    scheduleFolderReadLocked(path, pass);
    const auto it = m_folders.find(path);
    if (it != m_folders.end()) {
        // A folder.json or journal that could not be read is tried again no more often than this: a bad folder does
        // not cost a read (and the passes it keeps asking for) every pass.
        it->retryAtMs = static_cast<qint64>(dsd_realtime_mono_ms()) + m_readRetryMs;
    }
}

void
RecordingStore::unindexSearchLocked(const Entry& entry) {
    const auto bySearch = m_bySearch.find(entry.search);
    if (bySearch != m_bySearch.end()) {
        bySearch->removeAll(entry.id);
        if (bySearch->isEmpty()) {
            m_bySearch.erase(bySearch);
        }
    }
}

void
RecordingStore::markEntryUnreadableLocked(const QString& id, Pass* pass) {
    const auto it = m_entries.find(id);
    if (it == m_entries.end() || it->unreadable) {
        return;
    }
    unindexFragmentLocked(*it, pass);
    unindexSearchLocked(*it);
    it->unreadable = true;
    noteQueryChangeLocked(pass);
    pass->dirty = true;
}

void
RecordingStore::listFolder(const QString& path, QHash<QString, FileStat>* files, QList<Entry>* unfinished) const {
    // One listing serves every publish whose file sits in the folder itself, which is all of them in a folder the app
    // made; a name in a subfolder gets its own stat (statPublished()).
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString name = info.fileName();
        QString token;
        if (recording_temp_name_token(name, &token)) {
            // This process's own temp files may still be open for writing: never indexed, never touched.
            const FileStat stat = statOf(path + QLatin1Char('/') + name);
            if (token != m_liveToken && stat.regular) {
                Entry entry;
                entry.path = clean_absolute(info.absoluteFilePath());
                entry.id = QStringLiteral("unfinished:") + entry.path;
                entry.folder = path;
                entry.file = name;
                entry.start = stat.mtime;
                entry.bytes = stat.size;
                entry.identity = stat.identity;
                entry.unfinished = true;
                unfinished->append(entry);
            }
            continue;
        }
        files->insert(name, statOf(path + QLatin1Char('/') + name));
    }
}

void
RecordingStore::applyRecordsInSlices(const Folder& folder, const QList<RecordingJournalRecord>& records,
                                     const QHash<QString, FileStat>& files, Pass* pass) {
    // In slices, so History's reads on the GUI thread never wait for a whole large folder. The lock is not fair: a
    // reader the unlock woke would lose it to the next slice, so the next slice waits until the readers are through.
    for (qsizetype begin = 0; begin < records.size() && !stopping(); begin += kApplySlice) {
        yieldToReaders();
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const qsizetype end = qMin(records.size(), begin + kApplySlice);
            for (qsizetype i = begin; i < end; i++) {
                applyRecordLocked(folder, records.at(i), files, pass);
            }
        }
        if (m_sliceHook) {
            m_sliceHook(folder.path);
        }
    }
}

qint64
RecordingStore::parseLines(const QByteArray& data, QList<RecordingJournalRecord>* records) {
    qint64 lines = 0;
    qsizetype begin = 0;
    while (begin < data.size()) {
        qsizetype end = data.indexOf('\n', begin);
        if (end < 0) {
            end = data.size();
        }
        lines++;
        RecordingJournalRecord record;
        // A line that does not parse (a torn tail, ended by the next run's newline) or of another version is skipped.
        if (recording_journal_parse(data.mid(begin, end - begin), &record)) {
            records->append(record);
        }
        begin = end + 1;
    }
    return lines;
}

void
RecordingStore::statPublished(const QString& folder, const QList<RecordingJournalRecord>& records,
                              QHash<QString, FileStat>* files) {
    for (const RecordingJournalRecord& record : records) {
        if (record.type == RecordingJournalRecord::Publish && !files->contains(record.file)) {
            files->insert(record.file, statOf(folder + QLatin1Char('/') + record.file));
        }
    }
}

void
RecordingStore::tailFolders(Pass* pass) {
    struct Cursor {
        QString path;
        qint64 offset = 0;
        quint64 starts = 0U;
    };

    QList<Cursor> cursors;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
            if (it->loaded && m_rootAvailable.value(it->root)) {
                cursors.append({it.key(), it->offset, it->starts});
            }
        }
    }
    for (const Cursor& cursor : std::as_const(cursors)) {
        tailFolder(cursor.path, cursor.offset, cursor.starts, pass);
    }
}

void
RecordingStore::tailFolder(const QString& path, qint64 offset, quint64 starts, Pass* pass) {
    QFile journal(path + QLatin1Char('/') + kJournalName);
    const qint64 size = journal.exists() ? journal.size() : 0;
    const bool marked = QFileInfo::exists(path + QLatin1Char('/') + kFolderFile);
    if (size == offset && marked) {
        return;
    }
    if (size < offset || !marked) {
        // Shorter than what was read: replaced or truncated behind the store's back, or gone with a root that went
        // away since the pass began, which is left as it is. Read it again from the start. So is a folder whose
        // folder.json is gone: the read drops a folder that is no longer the app's, and retention never takes its files.
        int root = -1;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            root = m_folders.value(path).root;
        }
        if (!rootStillAvailable(root, pass)) {
            return;
        }
        loadFolder(path, root, true, pass);
        return;
    }
    QByteArray data;
    if (!read_journal_tail(journal.fileName(), offset, size - offset, &data)) {
        noteTailReadFailed(path, pass);
        return;
    }
    // Complete lines only: a line still being written is read once its newline lands.
    const qsizetype consumed = data.lastIndexOf('\n') + 1;
    if (consumed == 0) {
        return;
    }
    if (m_tailHook) {
        m_tailHook(path);
    }
    QList<RecordingJournalRecord> records;
    const qint64 lines = parseLines(data.left(consumed), &records);
    QHash<QString, FileStat> files;
    statPublished(path, records, &files);
    // A file that reads as missing is deleted only with its root still there; lines read as the root went stay unread.
    int root = -1;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        root = m_folders.value(path).root;
    }
    if (!rootStillAvailable(root, pass)) {
        return;
    }

    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!tailCursorCurrentLocked(path, offset, starts)) {
        return;
    }
    const auto folder = m_folders.find(path);
    folder->offset = offset + consumed;
    folder->lines += lines;
    const Folder& current = *folder; // applying a record never changes the folder table
    for (const RecordingJournalRecord& record : records) {
        applyRecordLocked(current, record, files, pass);
    }
}

void
RecordingStore::applyRecordLocked(const Folder& folder, const RecordingJournalRecord& record,
                                  const QHash<QString, FileStat>& files, Pass* pass) {
    switch (record.type) {
        case RecordingJournalRecord::Publish: applyPublishLocked(folder, record, files, pass); return;
        case RecordingJournalRecord::Link: applyLinkLocked(record, pass); return;
        case RecordingJournalRecord::Unlink: {
            const auto it = m_entries.find(record.id);
            if (it != m_entries.end()) {
                it->pending = false; // its row will never exist: it stays, unattributed
            }
            return;
        }
        case RecordingJournalRecord::Discard:
        case RecordingJournalRecord::Error: return;
    }
}

void
RecordingStore::applyPublishLocked(const Folder& folder, const RecordingJournalRecord& record,
                                   const QHash<QString, FileStat>& files, Pass* pass) {
    const QString id =
        record.id.isEmpty() ? QStringLiteral("file:") + folder.path + QLatin1Char('/') + record.file : record.id;
    const FileStat stat = files.value(record.file);
    const QString path = QDir::cleanPath(folder.path + QLatin1Char('/') + record.file);
    if (!stat.regular && !stat.unreadable) {
        m_unopenable.remove(path);
        // Missing on disk with its root available (deleted, or not a plain file any more): not in the index.
        if (m_entries.contains(id)) {
            removeEntryLocked(id, pass);
        }
        return;
    }
    Entry entry;
    entry.id = id;
    entry.path = path;
    entry.folder = folder.path;
    entry.file = record.file;
    entry.t = record.t;
    entry.start = record.start;
    entry.durationMs = record.durationMs;
    entry.rate = record.rate;
    entry.bytes = stat.size;
    entry.identity = stat.identity;
    // Its name cannot be read (no access, an I/O error, a name too long here): kept, its journal line with it, but
    // never deleted (no identity matches it), counted, exported, played or shared (readerEntryLocked()).
    entry.unreadable = stat.unreadable || !opensAgainLocked(path, stat);
    entry.hasInstance = record.hasInstance;
    entry.instance = record.instance;
    entry.slot = record.slot;
    entry.hasRow = record.hasRow && !record.pending;
    entry.row = record.row;
    entry.pending = record.pending;
    entry.rdio = record.rdio;
    entry.failed = record.failed;
    entry.system = record.system;
    entry.target = record.target;
    entry.source = record.source;
    entry.search = record.search;
    if (entry.hasInstance && entry.slot >= 0 && entry.hasRow) {
        entry.fragment = call_history_fragment_key(entry.instance, entry.slot, entry.row);
    }
    insertEntryLocked(entry, pass);
    pass->published = true;
}

void
RecordingStore::applyLinkLocked(const RecordingJournalRecord& record, Pass* pass) {
    const auto it = m_entries.find(record.id);
    if (it == m_entries.end() || it->unfinished) {
        return; // deleted since, or never kept
    }
    const QString fragment = call_history_fragment_key(record.instance, record.slot, record.row);
    if (it->fragment != fragment) {
        unindexFragmentLocked(*it, pass);
        it->fragment = fragment;
        // Kept for the journal; a recording whose name cannot be read is in no reader's index.
        if (!it->unreadable) {
            m_byFragment[fragment].append(it->id);
            pass->fragments.insert(fragment);
        }
        noteQueryChangeLocked(pass);
    }
    it->hasInstance = true;
    it->instance = record.instance;
    it->slot = record.slot;
    it->hasRow = true;
    it->row = record.row;
    it->pending = false;
    pass->dirty = true;
}

void
RecordingStore::insertEntryLocked(Entry entry, Pass* pass) {
    if (m_entries.contains(entry.id)) {
        removeEntryLocked(entry.id, pass);
    }
    entry.order = m_nextOrder++;
    if (!entry.unfinished) {
        // One path, one owner: the newest publish in journal order. A journal not yet compacted can still name a
        // recording deleted since whose final name a later one reused (an I/Q replay's 16-bit suffix repeated); that
        // id goes, so it never comes back on its History row, its file counts once, and deleting it cannot unlink
        // the newer recording.
        const QString owner = m_byPath.value(entry.path);
        if (!owner.isEmpty() && owner != entry.id) {
            removeEntryLocked(owner, pass);
        }
        m_byPath.insert(entry.path, entry.id);
    }
    if (!entry.fragment.isEmpty() && !entry.unreadable) {
        m_byFragment[entry.fragment].append(entry.id);
        pass->fragments.insert(entry.fragment);
    }
    if (!entry.search.isEmpty() && !entry.unreadable) {
        m_bySearch[entry.search].append(entry.id);
    }
    m_rowsInFolder[entry.folder]++;
    m_entries.insert(entry.id, entry);
    noteQueryChangeLocked(pass);
}

void
RecordingStore::unindexFragmentLocked(const Entry& entry, Pass* pass) {
    if (entry.fragment.isEmpty()) {
        return;
    }
    const auto it = m_byFragment.find(entry.fragment);
    if (it != m_byFragment.end()) {
        it->removeAll(entry.id);
        if (it->isEmpty()) {
            m_byFragment.erase(it);
        }
    }
    pass->fragments.insert(entry.fragment);
}

void
RecordingStore::noteQueryChangeLocked(Pass* pass) {
    m_generation++;
    pass->dirty = true;
}

/* Every way a recording leaves the index comes here (a delete, the limit, a file gone, a folder read again or gone, a
 * path a later publish took), so its detection forgets it here too; a relink keeps it (unindexFragmentLocked()). */
void
RecordingStore::removeEntryLocked(const QString& id, Pass* pass) {
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) {
        return;
    }
    unindexFragmentLocked(*it, pass);
    unindexSearchLocked(*it);
    const auto byPath = m_byPath.find(it->path);
    if (byPath != m_byPath.end() && byPath.value() == id) {
        m_byPath.erase(byPath);
    }
    const auto rows = m_rowsInFolder.find(it->folder);
    if (rows != m_rowsInFolder.end() && --rows.value() <= 0) {
        m_rowsInFolder.erase(rows);
    }
    m_entries.erase(it);
    noteQueryChangeLocked(pass);
}

void
RecordingStore::removeFolderEntriesLocked(const QString& folder, Pass* pass) {
    QStringList ids;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->folder == folder) {
            ids.append(it.key());
        }
    }
    for (const QString& id : ids) {
        removeEntryLocked(id, pass);
    }
}

void
RecordingStore::holdCommittedFolderLocked(const QString& folder) {
    CommittedFolder committed;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        const Entry& entry = it.value();
        if (entry.folder != folder) {
            continue;
        }
        committed.entries.insert(entry.id, entry);
        if (!entry.fragment.isEmpty() && !entry.unreadable) {
            committed.byFragment[entry.fragment].append(entry.id);
        }
        if (!entry.search.isEmpty() && !entry.unreadable) {
            committed.bySearch[entry.search].append(entry.id);
        }
        if (m_byPath.value(entry.path) == entry.id) {
            committed.byPath.insert(entry.path, entry.id);
        }
    }
    m_committed.insert(folder, committed);
}

void
RecordingStore::commitFolderLocked(const QString& folder, Pass* pass) {
    if (m_committed.remove(folder) > 0) {
        noteQueryChangeLocked(pass); // the GUI reads what the folder holds now
    }
}

const RecordingStore::Entry*
RecordingStore::readerEntryLocked(const QString& id) const {
    // A recording whose name cannot be read is no reader's: never played, saved, shared or listed.
    // Nor one in a folder not read (its journal not read since its storage came back, perhaps under another owner's
    // folder.json): its path may name another recording now.
    const auto live = m_entries.constFind(id);
    if (live != m_entries.cend() && !m_committed.contains(live->folder)) {
        return live->unreadable || !folderReadLocked(live->folder) ? nullptr : &live.value();
    }
    for (const CommittedFolder& committed : m_committed) {
        const auto held = committed.entries.constFind(id);
        if (held != committed.entries.cend()) {
            return held->unreadable ? nullptr : &held.value();
        }
    }
    return nullptr;
}

QString
RecordingStore::readerIdForPathLocked(const QString& path) const {
    for (const CommittedFolder& committed : m_committed) {
        const auto held = committed.byPath.constFind(path);
        if (held != committed.byPath.cend()) {
            const auto entry = committed.entries.constFind(held.value());
            return entry != committed.entries.cend() && !entry->unreadable ? held.value() : QString();
        }
    }
    const QString id = m_byPath.value(path);
    const auto live = m_entries.constFind(id);
    return live != m_entries.cend() && !m_committed.contains(live->folder) && !live->unreadable
                   && folderReadLocked(live->folder)
               ? id
               : QString();
}

QList<const RecordingStore::Entry*>
RecordingStore::readerEntriesLocked(const QString& key, bool bySearch) const {
    QList<const Entry*> found;
    for (const QString& id : (bySearch ? m_bySearch : m_byFragment).value(key)) {
        const auto live = m_entries.constFind(id);
        if (live != m_entries.cend() && !m_committed.contains(live->folder)) {
            found.append(&live.value());
        }
    }
    for (const CommittedFolder& committed : m_committed) {
        for (const QString& id : (bySearch ? committed.bySearch : committed.byFragment).value(key)) {
            const auto held = committed.entries.constFind(id);
            if (held != committed.entries.cend()) {
                found.append(&held.value());
            }
        }
    }
    return found;
}

bool
RecordingStore::storageSettledLocked() const {
    return m_ready && !m_inPass && storageReadLocked();
}

bool
RecordingStore::storageReadLocked() const {
    if (m_passRequested) {
        return false;
    }
    // A folder a session start adopted, or one on storage that came back, waits for its first read or its read again.
    return std::none_of(m_folders.cbegin(), m_folders.cend(),
                        [this](const Folder& folder) { return !folder.loaded && m_rootAvailable.value(folder.root); });
}

bool
RecordingStore::folderReadLocked(const QString& folder) const {
    const auto it = m_folders.constFind(folder);
    return it != m_folders.cend() && it->loaded;
}

bool
RecordingStore::folderAvailableLocked(const QString& folder) const {
    const auto it = m_folders.constFind(folder);
    return it != m_folders.cend() && m_rootAvailable.value(it->root);
}

bool
RecordingStore::rootSeenAwayLocked(int root) const {
    return m_rootsSeenAway.contains(root) || m_rootsRereading.contains(root);
}

QString
RecordingStore::folderUidLocked(const QString& folder) const {
    const auto it = m_folders.constFind(folder);
    return it != m_folders.cend() ? it->uid : QString();
}

bool
RecordingStore::liveTemp(const QString& path) const {
    QString token;
    return recording_temp_name_token(QFileInfo(path).fileName(), &token) && token == m_liveToken;
}

bool
RecordingStore::stopping() const {
    return m_stopping.load();
}

void
RecordingStore::yieldToReaders() const {
    while (m_readersWaiting.load() > 0 && !stopping()) {
        std::this_thread::yield();
    }
}

void
RecordingStore::processRechecks(Pass* pass) {
    QStringList ids;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ids = m_recheckQueue.values();
        m_recheckQueue.clear();
    }
    recheckEntries(ids, pass, true);
}

void
RecordingStore::recheckRegularLocked(Entry& entry, const FileStat& stat, bool readChanged, Pass* pass) {
    // A recheck the host asked for (the player, an export or a share found it changed) that finds another file than the
    // index knew reads its folder again whole: the owner and the file then come from what is on disk, never taken onto
    // this entry. The limit's own look reads the size only, so its check before each unlink still finds a file not as
    // the index knew it.
    if (readChanged && dsd_file_identity_equal(&stat.identity, &entry.identity) == 0) {
        scheduleFolderReadLocked(entry.folder, pass);
        m_passRequested = true; // read at once, not at the next pass the timer starts
    } else if (readChanged && !opensToRead(entry.path)) {
        // The file the index knows, which a transfer could not open (no read access to it): kept, its journal line with
        // it, and out of every index and selection (the unreadable recording's rule), so the next export takes the
        // others. Remembered by its path, so a later read of its folder that sees it by its metadata alone does not
        // bring it back: only an open that succeeds does (applyPublishLocked()).
        m_unopenable.insert(entry.path);
        markEntryUnreadableLocked(entry.id, pass);
    } else if (stat.size != entry.bytes) {
        entry.bytes = stat.size;
        noteQueryChangeLocked(pass);
    }
}

void
RecordingStore::recheckEntries(const QStringList& ids, Pass* pass, bool readChanged) {
    for (const QString& id : ids) {
        yieldToReaders();
        if (m_entryCheckHook) {
            m_entryCheckHook(id);
        }
        int root = -1;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_entries.find(id);
            if (it == m_entries.end() || !folderAvailableLocked(it->folder)) {
                continue; // gone already, or its storage is away (never read as deleted)
            }
            const FileStat stat = statOf(it->path);
            if (stat.regular) {
                recheckRegularLocked(*it, stat, readChanged, pass);
                continue;
            }
            root = m_folders.value(it->folder).root;
        }
        // Missing: gone with a root that went away since the pass began, which marks the root away and leaves the
        // index as it is, or deleted behind the store's back.
        if (!rootStillAvailable(root, pass)) {
            continue;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.constFind(id);
        if (it == m_entries.cend()) {
            continue;
        }
        // Only a name confirmed missing, or confirmed no recording (a link, a folder, a FIFO), leaves the index; one
        // that cannot be read (no access to it, an I/O error: it says nothing about whether it is there), with its root
        // still there, is kept with its journal line and out of every index, count and usage from now on (the limit's
        // look counts it no more).
        const FileStat again = statOf(it->path);
        if (again.unreadable) {
            markEntryUnreadableLocked(id, pass);
        } else if (!again.regular) {
            // Another kind of file at the name (a link, a folder, a FIFO), not one gone: a delete asked of the recording
            // is settled as not done, told (deletesSkipped()), before its entry leaves.
            if (!again.missing && m_deleteQueue.remove(id) > 0) {
                m_deletesSkipped++;
            }
            removeEntryLocked(id, pass);
        }
    }
}

void
RecordingStore::processDeletes(Pass* pass) {
    QHash<QString, DeleteAsk> asked;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        asked = m_deleteQueue;
    }
    for (auto it = asked.cbegin(); it != asked.cend(); ++it) {
        const DeleteResult result = deleteEntry(it.key(), DeleteCause::Asked, pass, nullptr, &it.value());
        if (result != DeleteResult::Held) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_deleteQueue.remove(it.key());
            if (result == DeleteResult::Changed || result == DeleteResult::Unreadable) {
                m_deletesSkipped++; // not the recording: the file stays; said on the screens (deletesSkipped())
                pass->dirty = true;
            }
        }
    }
}

namespace {

/* R68: a delete asked of the file at @p expected's path (none: the limit's) finds @p entry at another path now, or at
 * another one than the path it took (@p taken). */
template <typename EntryT, typename AskT>
bool
path_moved(const EntryT& entry, const AskT* expected, const QString& taken) {
    return expected != nullptr && (entry.path != expected->path || entry.path != taken);
}

} // namespace

RecordingStore::DeleteResult
RecordingStore::deleteEntry(const QString& id, DeleteCause cause, Pass* pass, const QList<bool>* counted,
                            const DeleteAsk* expected, qint64* foundBytes) {
    QString path;
    {
        yieldToReaders();
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.constFind(id);
        if (it == m_entries.cend()) {
            return DeleteResult::Gone;
        }
        if (!folderAvailableLocked(it->folder)) {
            return DeleteResult::Held; // waits for its storage to come back
        }
        // Nor against a folder not read (a journal or a recording it names could not be read, even in this pass): its
        // read, which drops every delete asked before it, comes first.
        if (!m_folders.value(it->folder).loaded) {
            return DeleteResult::Held;
        }
        // The entry under the id at another path now (a read again put another file under it, R68): not the file asked
        // of, which stays, and neither is removed.
        if (path_moved(*it, expected, it->path)) {
            return DeleteResult::Changed;
        }
        path = it->path;
    }
    // The rdio uploader takes its queue's own lock; asked here, outside the store's. A job for a file the journal
    // already named was queued before its line was written, so a hold can only end after this, never start.
    const bool uploading = m_uploadHolds(path);
    // A removal for the limit rests on the usage of every root its total counts, not only this recording's: each is
    // checked again right before the unlink, and one gone (marked away) holds it.
    if (counted != nullptr && !rootsStillAvailable(*counted, pass)) {
        return DeleteResult::Held;
    }

    UnlinkAttempt attempt;
    {
        yieldToReaders();
        const std::lock_guard<std::mutex> lock(m_mutex);
        const DeleteResult decided = unlinkEntryLocked(id, path, uploading, cause, expected, pass, &attempt);
        if (foundBytes != nullptr) {
            *foundBytes = attempt.bytes;
        }
        if (decided != DeleteResult::Failed) {
            return decided;
        }
    }
    // Missing, not a recording the store may remove any more, or a refused unlink: each drops the entry or the
    // request, so each waits for its root to be checked again. One that went away since the pass began explains all
    // three; it is marked away, and the entry and the request wait for it to come back.
    if (!rootStillAvailable(attempt.root, pass)) {
        return DeleteResult::Held;
    }
    if (attempt.found == OnDisk::Unreadable) {
        // Its name cannot be read right now, with its root still there: out of every index, count and usage from here
        // (the unreadable recording's rule), kept, and skipped.
        const std::lock_guard<std::mutex> lock(m_mutex);
        markEntryUnreadableLocked(id, pass);
        return DeleteResult::Unreadable;
    }
    if (attempt.found != OnDisk::Removable) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const DeleteResult dropped = dropUnremovableLocked(id, attempt.found, pass);
        // A link (or anything else) at the name of a recording a user asked to delete: not the file asked of, so the
        // request is told as not done, though the entry leaves the index.
        return cause == DeleteCause::Asked && attempt.found == OnDisk::Foreign && dropped == DeleteResult::Kept
                   ? DeleteResult::Changed
                   : dropped;
    }
    noteDeleteFailure(cause, attempt.error);
    pass->dirty = true;
    return DeleteResult::Failed;
}

RecordingStore::DeleteResult
RecordingStore::unlinkEntryLocked(const QString& id, const QString& path, bool uploading, DeleteCause cause,
                                  const DeleteAsk* expected, Pass* pass, UnlinkAttempt* attempt) {
    const auto it = m_entries.constFind(id);
    if (it == m_entries.cend()) {
        return DeleteResult::Gone;
    }
    const DeleteResult refused = unlinkRefusedLocked(*it, path, uploading, cause, expected);
    if (refused != DeleteResult::Failed) {
        return refused;
    }
    attempt->root = m_folders.value(it->folder).root;
    // Right before the unlink, without following a link: what is there now is what gets removed, and only when it is
    // the file the request was asked of (the limit's: the one the index knows). Storage that came back changed without
    // a read of it again (remounted between two passes, two owners' folders swapped; a folder a session start adopted
    // while a delete waited) has another file at the path, which stays.
    attempt->found =
        onDiskAsKnownLocked(path, expected != nullptr ? expected->identity : it->identity, &attempt->bytes);
    if (attempt->found == OnDisk::Replaced) {
        return DeleteResult::Changed;
    }
    if (attempt->found == OnDisk::Removable && m_remove(path, &attempt->error)) {
        removeEntryLocked(id, pass);
        return DeleteResult::Removed;
    }
    return DeleteResult::Failed;
}

RecordingStore::DeleteResult
RecordingStore::unlinkRefusedLocked(const Entry& entry, const QString& path, bool uploading, DeleteCause cause,
                                    const DeleteAsk* expected) const {
    // Again under the lock the unlink runs under: a read again since the path was taken may have put the id elsewhere.
    if (path_moved(entry, expected, path)) {
        return DeleteResult::Changed;
    }
    // Again under the lock the unlink runs under (the uploader was asked without it): the folder still there and read,
    // so a start that adopted it meanwhile, or a read that failed, holds the delete for the folder's read. The limit's
    // removals rest on the whole index it counted: any folder unread since, or a pass asked for, ends its walk.
    if (cause == DeleteCause::Limit && !storageReadLocked()) {
        return DeleteResult::Stale;
    }
    // Nor while its root has been seen away since (a session start or a request found it missing, the root back by now
    // perhaps): the root's listing in full, which drops every delete asked before it, comes first.
    if (!folderAvailableLocked(entry.folder) || !m_folders.value(entry.folder).loaded
        || rootSeenAwayLocked(m_folders.value(entry.folder).root)) {
        return DeleteResult::Held;
    }
    if (uploading || heldLocked(path)) {
        return DeleteResult::Held;
    }
    if (liveTemp(path)) {
        return DeleteResult::Kept; // this process may still be writing it
    }
    // A recording whose name could not be read is never removed, whoever asked and whenever (access back before its
    // folder is read again included): skipped, kept with its journal line.
    if (entry.unreadable) {
        return DeleteResult::Unreadable;
    }
    return DeleteResult::Failed;
}

RecordingStore::DeleteResult
RecordingStore::dropUnremovableLocked(const QString& id, OnDisk found, Pass* pass) {
    const auto it = m_entries.constFind(id);
    if (it == m_entries.cend()) {
        return DeleteResult::Gone;
    }
    const OnDisk again = onDiskLocked(it->path);
    if (again != found) {
        return DeleteResult::Held; // changed meanwhile: the next pass looks at it afresh
    }
    removeEntryLocked(id, pass);
    // Already gone; or left on disk, out of the index.
    return found == OnDisk::Missing ? DeleteResult::Gone : DeleteResult::Kept;
}

bool
RecordingStore::heldLocked(const QString& path) const {
    return m_holds.contains(path) || (!m_playing.isEmpty() && same_path(m_playing, path));
}

RecordingStore::OnDisk
RecordingStore::onDiskAsKnownLocked(const QString& path, const dsd_file_identity_t& known, qint64* bytes) const {
    // A name that cannot be read (no access to it, an I/O error) is never taken as missing: the file is skipped, as
    // one not known to be the file asked of.
    const FileStat now = statOf(path);
    if (now.unreadable) {
        return OnDisk::Unreadable;
    }
    if (bytes != nullptr && now.regular) {
        *bytes = now.size;
    }
    const OnDisk found = onDiskLocked(path);
    if (found != OnDisk::Removable) {
        return found;
    }
    return dsd_file_identity_equal(&known, &now.identity) != 0 ? OnDisk::Removable : OnDisk::Replaced;
}

RecordingStore::OnDisk
RecordingStore::onDiskLocked(const QString& path) const {
    const QFileInfo now(path);
    if (!now.isSymLink() && !now.exists()) {
        return OnDisk::Missing;
    }
    if (now.isSymLink() || !now.isFile()) {
        return OnDisk::Foreign;
    }
    for (int i = 0; i < m_roots.size(); i++) {
        if (m_rootAvailable.at(i) && recording_path_inside_root(path, m_roots.at(i))) {
            return OnDisk::Removable;
        }
    }
    return OnDisk::Foreign;
}

void
RecordingStore::noteDeleteFailure(DeleteCause cause, const QString& error) {
    m_deleteFailures++;
    m_lastDeleteError = error;
    bool& logged = m_deleteFailureLogged[cause == DeleteCause::Limit ? 1 : 0];
    if (!logged) {
        logged = true;
        qWarning("call recordings: %s could not be deleted (%s); later failures are counted, not logged",
                 cause == DeleteCause::Limit ? "a recording over the storage limit" : "a recording",
                 qUtf8Printable(error));
    }
}

QHash<QString, RecordingStore::Folder>
RecordingStore::foldersCopyLocked() const {
    QHash<QString, Folder> copy;
    copy.reserve(m_folders.size());
    for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
        copy.insert(it.key(), it.value());
    }
    return copy;
}

bool
RecordingStore::countLimitUsage(bool looked, LimitUsage* usage) {
    // Summed outside the lock, as publish() does: the recordings on a shared copy, gone before the first delete, and
    // the folders on a copy of its own (foldersCopyLocked()).
    QHash<QString, Entry> entries;
    QHash<QString, Folder> folderTable;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_limitPending = false;
        usage->limit = m_limitBytes;
        if (!m_limitSet || usage->limit <= 0) {
            return false; // no limit, or not the user's yet
        }
        // Only with the index settled as the refusals have it (storageReadLocked()): a folder on storage that is there
        // and not read (its journal unread, read again after a change) may hold entries its journal no longer names,
        // and a pass asked for may read storage again. Tried again by the pass that reads it.
        if (!storageReadLocked()) {
            m_limitPending = true;
            return false;
        }
        entries = m_entries;
        folderTable = foldersCopyLocked();
        if (m_folderCopyHook) {
            m_folderCopyHook(folderTable.isSharedWith(m_folders));
        }
        usage->rootAvailable = m_rootAvailable;
    }
    // A recording whose name cannot be read is neither counted nor a candidate: the limit never removes it.
    for (const Entry& entry : std::as_const(entries)) {
        const auto folder = folderTable.constFind(entry.folder);
        if (!entry.unreadable && folder != folderTable.cend() && usage->rootAvailable.value(folder->root)) {
            usage->total += entry.bytes;
        }
    }
    if (usage->total <= usage->limit) {
        return false;
    }
    for (const Entry& entry : std::as_const(entries)) {
        const auto folder = folderTable.constFind(entry.folder);
        if (!entry.unreadable && folder != folderTable.cend() && usage->rootAvailable.value(folder->root)) {
            if (looked) {
                usage->candidates.append({entry.id, entry.bytes, entry.start, entry.order});
            } else {
                usage->counted.append(entry.id);
            }
        }
    }
    return true;
}

bool
RecordingStore::removeForLimit(LimitUsage* usage, qint64 target, Pass* pass, int* removed, bool* held) {
    for (const LimitCandidate& candidate : std::as_const(usage->candidates)) {
        if (usage->total <= target) {
            break;
        }
        // Every root the total counts is checked again right before the unlink (deleteEntry()): one found gone
        // holds this removal, wherever the recording is.
        qint64 found = -1;
        const DeleteResult result =
            deleteEntry(candidate.id, DeleteCause::Limit, pass, &usage->rootAvailable, nullptr, &found);
        if (result == DeleteResult::Stale) {
            // What it counted may no longer be the whole index: nothing more goes now, and the limit runs again once
            // the store is settled (countLimitUsage()).
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_limitPending = true;
            return false;
        }
        if (result == DeleteResult::Unreadable) {
            // Its name cannot be read: kept, out of the usage (it no longer counts), told as not deleted, and the walk
            // goes on against the usage as it is now.
            const std::lock_guard<std::mutex> lock(m_mutex);
            usage->total -= candidate.bytes;
            m_deletesSkipped++;
            pass->dirty = true;
            continue;
        }
        if (result == DeleteResult::Changed) {
            // Not the file the index knows: skipped, left on disk and told (deletesSkipped()). It still counts, at the
            // size the check found (another size: the total moves by the difference; another time only: as it was),
            // and the limit goes on with the next only if that total is still over.
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (found >= 0) {
                usage->total += found - candidate.bytes;
                // Accounted at that size from now on (Settings, the next limit pass); its identity is kept, so the
                // file there is still not removed as the recording.
                const auto entry = m_entries.find(candidate.id);
                if (entry != m_entries.end() && entry->bytes != found) {
                    entry->bytes = found;
                    noteQueryChangeLocked(pass);
                }
            }
            m_deletesSkipped++;
            pass->dirty = true;
            continue;
        }
        if (result == DeleteResult::Held) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_rootAvailable != usage->rootAvailable) {
                return true; // a root marked away: the total no longer counts what is there
            }
            *held = true;
        }
        if (result == DeleteResult::Removed) {
            usage->total -= candidate.bytes;
            (*removed)++;
        } else if (result == DeleteResult::Gone) {
            usage->total -= candidate.bytes; // already gone: not one the limit removed
        } else if (result == DeleteResult::Kept) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_entries.contains(candidate.id)) {
                // A link or anything else not a recording file at its name: left on disk, out of the index and the
                // usage, and told as not deleted (deletesSkipped()).
                usage->total -= candidate.bytes;
                m_deletesSkipped++;
                pass->dirty = true;
            }
        }
    }
    return false;
}

void
RecordingStore::enforceLimit(Pass* pass) {
    int removed = 0;
    bool held = false;
    bool over = false;
    bool looked = false;
    // A root found gone during the run took its recordings out of the usage: the total is summed again over the roots
    // still there, and only then is it decided whether anything needs to go.
    for (bool recount = true; recount;) {
        held = false;
        over = false;
        LimitUsage usage;
        if (!countLimitUsage(looked, &usage)) {
            break;
        }
        if (!looked) {
            // Over on the sizes the index holds, which a file deleted behind the store's back (over USB, by a file
            // manager) still counts: every file the total counts is looked at as a recheck looks, a gone one leaving
            // the index (away, not deleted, when its root went), and the total is summed again before anything goes.
            looked = true;
            recheckEntries(usage.counted, pass, false);
            recount = true;
            continue;
        }
        // Oldest first (an unfinished recording by its file's time), down to 95% so one more call does not start again.
        std::sort(usage.candidates.begin(), usage.candidates.end(),
                  [](const LimitCandidate& a, const LimitCandidate& b) {
                      return a.start != b.start ? a.start < b.start : a.order < b.order;
                  });
        const qint64 target = usage.limit / 100 * 95;
        recount = removeForLimit(&usage, target, pass, &removed, &held);
        over = usage.total > target;
    }
    if (removed > 0) {
        m_removedByLimit += removed;
        pass->dirty = true;
    }
    if (held && over) {
        // What is left over the target is held: a hold's release, or the next timed pass, tries again.
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_limitPending = true;
    }
}

bool
RecordingStore::rootWritableNow(const QString& root) {
    // As a session start finds it: a root that is there takes a new folder, or a missing one can be made.
    const QFileInfo info(root);
    if (info.exists()) {
        return info.isDir() && info.isWritable();
    }
    return !info.isSymLink() && QFileInfo(info.absolutePath()).isWritable();
}

QString
RecordingStore::spaceRoot() const {
    QStringList roots;
    QList<bool> rootThere;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        // While a session records, the root of its folder: the decoder keeps writing there when a preferred root comes
        // back, or stays there when it takes no new files.
        int active = -1;
        for (const QString& path : std::as_const(m_active)) {
            const auto folder = m_folders.constFind(path);
            if (folder != m_folders.cend() && (active < 0 || folder->root < active)) {
                active = folder->root;
            }
        }
        if (active >= 0 && active < m_roots.size()) {
            return m_roots.at(active);
        }
        roots = m_roots;
        rootThere = m_rootAvailable;
    }
    // Idle: the root a session start would take now, the first one there that takes new folders (beginSession()).
    for (int i = 0; i < roots.size(); i++) {
        if (rootThere.value(i) && rootWritableNow(roots.at(i))) {
            return roots.at(i);
        }
    }
    return QString();
}

bool
RecordingStore::checkFreeSpace() {
    const QString where = spaceRoot();
    qint64 free = -1;
    if (!where.isEmpty()) {
        free = m_freeSpaceProbe(QFileInfo::exists(where) ? where : QFileInfo(where).absolutePath());
    }
    const bool low = free >= 0 && free < kLowSpaceBytes;
    const bool moved = free != m_freeBytes || low != m_lowSpace;
    m_freeBytes = free;
    m_lowSpace = low;
    return moved;
}

void
RecordingStore::compactFolders() {
    QStringList candidates;
    {
        QHash<QString, Entry> entries;
        {
            // Counted outside the lock on a shared copy, as publish() sums.
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_allActive) {
                return;
            }
            entries = m_entries;
        }
        QHash<QString, qint64> live;
        for (const Entry& entry : std::as_const(entries)) {
            if (!entry.unfinished) {
                live[entry.folder]++;
            }
        }
        entries.clear();
        const std::lock_guard<std::mutex> lock(m_mutex);
        // Asked again: a session report can hold every journal while the lock was let go for the count above.
        // cppcheck-suppress knownConditionTrueFalse -- another thread sets it between the two holds of the lock.
        if (m_allActive) {
            return;
        }
        for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
            if (compactableLocked(it.key()) && !m_active.contains(it.key()) && !m_unproven.contains(it.key())
                && it->lines > 0 && it->lines != it->compactFailedAt && it->lines > 2 * live.value(it.key())) {
                candidates.append(it.key());
            }
        }
    }
    for (const QString& path : candidates) {
        compactFolder(path);
    }
}

RecordingJournalRecord
RecordingStore::journalRecordOf(const Entry& entry) {
    RecordingJournalRecord record;
    record.t = entry.t;
    record.id = entry.id.startsWith(QLatin1String("file:")) ? QString() : entry.id;
    record.file = entry.file;
    record.start = entry.start;
    record.durationMs = entry.durationMs;
    record.rate = entry.rate;
    record.hasInstance = entry.hasInstance;
    record.instance = entry.instance;
    record.slot = entry.slot;
    record.hasRow = entry.hasRow;
    record.row = entry.row;
    record.rdio = entry.rdio;
    record.failed = entry.failed;
    record.system = entry.system;
    record.target = entry.target;
    record.source = entry.source;
    record.search = entry.search;
    return record;
}

bool
RecordingStore::compactableLocked(const QString& path) const {
    // Only a folder read and confirmed as it is: on storage that is there, not seen away since (a rescan of it asked
    // for), and its journal read. A journal a rescan has not read yet may be another owner's.
    const auto it = m_folders.constFind(path);
    return it != m_folders.cend() && it->loaded && m_rootAvailable.value(it->root) && !rootSeenAwayLocked(it->root);
}

bool
RecordingStore::rewriteJournal(const QString& journalPath, qint64 offset, const QList<Entry>& live, qint64* written,
                               const std::function<bool()>& stillDue) {
    bool ok = false;
    QFile current(journalPath);
    if (current.open(QIODevice::ReadOnly)) {
        // Past the cursor there may only be a line a crash tore; a whole line the store has not read, or a tail that
        // cannot be read in full, stops it.
        const bool settled = recording_journal_tail_settled(current, offset);
        current.close();
        QSaveFile out(journalPath);
        if (settled && out.open(QIODevice::WriteOnly)) {
            // Private, as the decoder creates it (dsd_fopen_private()), where the filesystem keeps modes at all: a
            // FUSE-backed shared storage may refuse the change, and that must not stop the compaction.
            (void)out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            ok = true;
            for (const Entry& entry : live) {
                const QByteArray line = recording_journal_publish_line(journalRecordOf(entry));
                if (out.write(line) != line.size()) {
                    ok = false;
                    break;
                }
                *written += line.size();
            }
            ok = ok && stillDue();
            if (ok) {
                ok = out.commit();
            } else {
                out.cancelWriting();
            }
        }
    }
    return ok;
}

bool
RecordingStore::compactFolder(const QString& path) {
    QList<Entry> live;
    qint64 offset = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        // A session that starts now waits in beginSession() until this compaction is over.
        if (m_allActive || m_active.contains(path) || m_unproven.contains(path) || !compactableLocked(path)) {
            return false;
        }
        m_compacting = path;
        offset = m_folders.value(path).offset;
        for (const Entry& entry : std::as_const(m_entries)) {
            if (entry.folder == path && !entry.unfinished) {
                live.append(entry);
            }
        }
    }
    std::sort(live.begin(), live.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });

    qint64 written = 0;
    // Checked again right before the rewrite is committed: storage seen away since, or the folder unread, leaves the
    // journal as it is for the read that comes first.
    const bool ok = rewriteJournal(path + QLatin1Char('/') + kJournalName, offset, live, &written, [this, &path]() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return compactableLocked(path);
    });

    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_compacting.clear();
        const auto folder = m_folders.find(path);
        if (folder != m_folders.end()) {
            if (ok) {
                // The cursor starts again after the rewrite: the lines it holds are the live entries already indexed.
                folder->offset = written;
                folder->lines = live.size();
                folder->compactFailedAt = -1;
                m_compactions++;
            } else {
                folder->compactFailedAt = folder->lines;
            }
        }
    }
    m_settled.notify_all();
    return ok;
}

bool
RecordingStore::tallyUnlisted(const Entry& entry, Summary* summary) {
    if (entry.unreadable) {
        return true; // kept, and counted nowhere
    }
    if (!entry.unfinished) {
        return false;
    }
    summary->usedBytes += entry.bytes;
    summary->unfinishedCount++;
    summary->unfinishedBytes += entry.bytes;
    return true;
}

RecordingStore::Summary
RecordingStore::summarize(const QHash<QString, Entry>& entries, const QHash<QString, Folder>& folders,
                          const QList<bool>& rootAvailable) {
    struct Group {
        QString uid;
        QString kind;
        QString name;
        int root = std::numeric_limits<int>::max();
        int count = 0;
        qint64 bytes = 0;
        qint64 first = 0;
        qint64 last = 0;
        bool available = false;
    };

    struct Tally {
        int count = 0;
        qint64 bytes = 0;
        qint64 first = 0;
        qint64 last = 0;
    };

    Summary summary;
    for (const bool available : rootAvailable) {
        summary.available = summary.available || available;
    }
    // One lookup per recording, by its folder; the folders then fold into their owners.
    QHash<QString, Tally> tallies;
    tallies.reserve(folders.size());
    for (const Entry& entry : entries) {
        if (tallyUnlisted(entry, &summary)) {
            continue;
        }
        summary.usedBytes += entry.bytes;
        summary.count++;
        Tally& tally = tallies[entry.folder];
        tally.first = tally.count == 0 ? entry.start : qMin(tally.first, entry.start);
        tally.last = tally.count == 0 ? entry.start : qMax(tally.last, entry.start);
        tally.count++;
        tally.bytes += entry.bytes;
    }
    QHash<QString, Group> groups;
    for (const Folder& folder : folders) {
        Group& group = groups[folder.uid];
        group.uid = folder.uid;
        if (folder.root < group.root) {
            group.root = folder.root;
            group.kind = folder.kind;
            group.name = folder.name;
        }
        group.available = group.available || rootAvailable.value(folder.root);
        const Tally tally = tallies.value(folder.path);
        if (tally.count > 0) {
            group.first = group.count == 0 ? tally.first : qMin(group.first, tally.first);
            group.last = group.count == 0 ? tally.last : qMax(group.last, tally.last);
            group.count += tally.count;
            group.bytes += tally.bytes;
        }
    }
    QList<Group> sorted = groups.values();
    std::sort(sorted.begin(), sorted.end(), [](const Group& a, const Group& b) {
        return a.last != b.last ? a.last > b.last : QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    for (const Group& group : std::as_const(sorted)) {
        summary.folders.append(QVariantMap{
            {QStringLiteral("uid"), group.uid},
            {QStringLiteral("kind"), group.kind},
            {QStringLiteral("name"), group.name},
            {QStringLiteral("count"), group.count},
            {QStringLiteral("bytes"), group.bytes},
            {QStringLiteral("first"), group.first},
            {QStringLiteral("last"), group.last},
            {QStringLiteral("available"), group.available},
        });
    }
    return summary;
}

void
RecordingStore::publish(const Pass& pass) {
    // The index is copied under the lock and summed outside it. This thread is the only one that changes the
    // recordings, and their copy is gone before it next does, so it stays shared, never deep-copied. The folder table
    // is the GUI thread's to change too (a session start adds or relabels a folder): its copy is the worker's own
    // (foldersCopyLocked()).
    QHash<QString, Entry> entries;
    QHash<QString, Folder> folderTable;
    QList<bool> rootAvailable;
    Summary summary;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!pass.dirty && pass.fragments.isEmpty() && m_deleteQueue.size() == qsizetype{m_outbox.deletesWaiting}) {
            return;
        }
        entries = m_entries;
        folderTable = foldersCopyLocked();
        if (m_folderCopyHook) {
            m_folderCopyHook(folderTable.isSharedWith(m_folders));
        }
        rootAvailable = m_rootAvailable;
        summary.ready = m_ready;
        summary.indexed = m_ready;
        for (const Folder& folder : std::as_const(m_folders)) {
            summary.indexed = summary.indexed && (folder.loaded || !m_rootAvailable.value(folder.root));
        }
        summary.deletesWaiting = static_cast<int>(m_deleteQueue.size());
        summary.deletesCancelled = m_deletesCancelled;
        summary.deletesSkipped = m_deletesSkipped;
    }
    const Summary sums = summarize(entries, folderTable, rootAvailable);
    entries.clear();
    folderTable.clear();
    summary.available = sums.available;
    summary.usedBytes = sums.usedBytes;
    summary.count = sums.count;
    summary.unfinishedCount = sums.unfinishedCount;
    summary.unfinishedBytes = sums.unfinishedBytes;
    summary.folders = sums.folders;
    summary.freeBytes = m_freeBytes;
    summary.lowSpace = m_lowSpace;
    summary.removedByLimit = m_removedByLimit;
    summary.deleteFailures = m_deleteFailures;
    summary.lastDeleteError = m_lastDeleteError;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_outbox = std::move(summary);
        m_outGeneration = m_generation;
        if (m_outFragments.isEmpty()) {
            m_outFragments = pass.fragments; // shared, not copied: a rebuild names every fragment
        } else {
            m_outFragments.unite(pass.fragments);
        }
        if (m_deliveryQueued) {
            return;
        }
        m_deliveryQueued = true;
    }
    QMetaObject::invokeMethod(this, [this]() { deliver(); }, Qt::QueuedConnection);
}

bool
RecordingStore::Summary::differsFrom(const Summary& other) const {
    const auto shown = [](const Summary& s) {
        return std::tie(s.ready, s.indexed, s.available, s.usedBytes, s.count, s.unfinishedCount, s.unfinishedBytes,
                        s.folders, s.freeBytes, s.lowSpace, s.removedByLimit, s.deletesWaiting, s.deleteFailures,
                        s.deletesCancelled, s.deletesSkipped, s.lastDeleteError);
    };
    return shown(*this) != shown(other);
}

void
RecordingStore::deliver() {
    Summary summary;
    QSet<QString> fragments;
    quint64 generation = 0U;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        summary = m_outbox;
        generation = m_outGeneration;
        fragments.swap(m_outFragments);
        m_deliveryQueued = false;
    }
    const int removed = summary.removedByLimit - m_summary.removedByLimit;
    const bool differs = summary.differsFrom(m_summary);
    m_summary = std::move(summary);
    if (differs) {
        Q_EMIT changed();
    }
    if (!fragments.isEmpty()) {
        QStringList list = fragments.values();
        list.sort();
        Q_EMIT recordingFragmentsChanged(list);
    }
    if (generation != m_revision) {
        m_revision = generation;
        Q_EMIT revisionChanged();
    }
    if (removed > 0) {
        Q_EMIT limitReached(removed);
    }
}

/* --- Sessions. --- */

void
RecordingStore::noteRootSeenAwayLocked(int root) const {
    // The one way a root found missing (or back unread) outside a pass is recorded, by a delete request's probe, a
    // session start's or its owner lookup: back before the pass samples it, it is still listed in full, no delete runs
    // against it meanwhile, and every selection made before is stale at once, as for any read of storage again.
    m_rootsSeenAway.insert(root);
    m_storageReads->fetch_add(1U, std::memory_order_release);
    m_passRequested = true;
}

QList<bool>
RecordingStore::usableRootsLocked() {
    // The roots as they are now, not as the last pass found them: storage that left since is never made again under
    // its absent mount, where recordings would hide once it came back over them. The pass asked for marks it away.
    // A session keeps the rule that a root that can be made is one it records into (R83): one held away only because
    // its directory is missing (it can be made, and it is not back unread) is made again, and its next read again
    // finds what is really there, the old rows gone as for any folder emptied on disk.
    QList<bool> usable = m_rootAvailable;
    for (int i = 0; i < usable.size(); i++) {
        if (usable.at(i) && !rootAvailableNow(m_roots.at(i))) {
            usable[i] = false;
            noteRootSeenAwayLocked(i);
        } else if (!usable.at(i) && rootAvailableNow(m_roots.at(i)) && !rootPlayableNow(m_roots.at(i))) {
            usable[i] = true;
        }
    }
    return usable;
}

QString
RecordingStore::ownerFolderPathLocked(const QString& uid, QList<bool>* usable, QStringList* gone, QStringList* unread) {
    // The owner's folder in the first available root that has one: a rename keeps it, and a clash of the short
    // uid in the name never merges two owners.
    const Folder* best = nullptr;
    for (const Folder& folder : std::as_const(m_folders)) {
        if (folder.uid != uid || !usable->value(folder.root)
            || (best != nullptr
                && (folder.root > best->root || (folder.root == best->root && folder.path >= best->path)))) {
            continue;
        }
        if (!ownerFolderOnDiskLocked(folder, uid, usable, gone, unread)) {
            continue;
        }
        // One that no longer takes new files (storage mounted read-only, permissions changed) records nothing: the
        // next root takes the session, as for a folder that cannot be made.
        if (!QFileInfo(folder.path).isWritable()) {
            continue;
        }
        best = &folder;
    }
    return best != nullptr ? best->path : QString();
}

bool
RecordingStore::ownerFolderOnDiskLocked(const Folder& folder, const QString& uid, QList<bool>* usable,
                                        QStringList* gone, QStringList* unread) {
    // A folder whose folder.json no longer names this owner (removed from a computer) is not handed out. One the worker
    // has not read yet holds no recordings, so it leaves the table here; a read one, or one whose recordings from
    // before its storage went wait for their read, is the worker's. One whose folder.json cannot be read now is not
    // handed out either, and is kept as the index knows it.
    Folder onDisk;
    const FolderMarker marker = read_folder_marker(folder.path, &onDisk.uid, &onDisk.kind, &onDisk.name);
    if (marker == FolderMarker::Read && onDisk.uid == uid) {
        return true;
    }
    if (marker == FolderMarker::Unreadable) {
        // Its owner not known now: what the index holds for it is kept, but no request is taken against it until a
        // read can tell again (read whole at the next pass; marked once the table is no longer walked).
        unread->append(folder.path);
        return false;
    }
    if (!rootAvailableNow(m_roots.at(folder.root))) {
        (*usable)[folder.root] = false; // gone since the check above: its folders stay, as storage away
        noteRootSeenAwayLocked(folder.root);
    } else if (!folder.loaded && !folder.rereading) {
        gone->append(folder.path);
    }
    return false;
}

QString
RecordingStore::existingFolderLocked(const QString& kind, const QString& uid, const QString& name,
                                     QList<bool>* usable) {
    QStringList gone;
    QStringList unread;
    const QString bestPath = ownerFolderPathLocked(uid, usable, &gone, &unread);
    for (const QString& path : std::as_const(gone)) {
        m_folders.remove(path);
        m_foldersChanged = true;
    }
    for (const QString& path : std::as_const(unread)) {
        // An owner it could not confirm: a read in flight now commits nothing (starts), and the next pass reads the
        // folder whole, never a tail.
        markFolderUnreadLocked(path);
        m_folders[path].starts++;
        m_passRequested = true;
    }
    const auto found = bestPath.isEmpty() ? m_folders.cend() : m_folders.constFind(bestPath);
    const Folder* best = found != m_folders.cend() ? &found.value() : nullptr;
    if (best == nullptr) {
        return QString();
    }
    const QString path = best->path;
    if (kind != kExplore && !name.isEmpty() && best->name != name && write_folder_json(path, uid, kind, name)) {
        m_folders[path].name = name;
        m_folders[path].starts++;
        m_foldersChanged = true;
        noteFolderAddedLocked(path, best->root); // relabelled: a listing of its root going on now keeps the new label
    }
    return path;
}

RecordingStore::CandidateFolder
RecordingStore::takeCandidateFolderLocked(const QString& path, int root, const QString& uid, const QFileInfo& info) {
    Folder found;
    if (!info.isSymLink() && info.isDir() && read_folder_json(path, &found.uid, &found.kind, &found.name)
        && found.uid == uid) {
        if (!info.isWritable()) {
            return CandidateFolder::NextRoot; // this owner's, taking no new files: the next root
        }
        // Made for this owner, not read yet: the worker reads its journal from the start, in place of
        // what the index held for it under another owner.
        found.path = path;
        found.root = root;
        const auto held = m_folders.constFind(path);
        found.rereading = held != m_folders.cend() && (held->loaded || held->rereading);
        found.starts = held != m_folders.cend() ? held->starts + 1U : 1U;
        m_folders.insert(path, found); // not read yet: the worker reads its journal next
        noteFolderAddedLocked(path, root);
        m_passRequested = true;
        m_foldersChanged = true;
        return CandidateFolder::Taken;
    }
    return CandidateFolder::NextName; // another owner's, or not the app's
}

QSet<QString>
RecordingStore::keepSessionFoldersLocked(int root, QList<Folder>* table, QSet<QString>* stale) {
    // A folder a session start made, took or relabelled while the root was listed is the start's: one the listing has
    // keeps the start's owner and label (the listing may have read the folder.json before the start wrote it); one it
    // has not was made new, so only the table entry the start installed stands, and what the index held at that path
    // before (a folder removed while the storage was away) leaves with its recordings.
    const QSet<QString> touched = m_listing.value(root);
    QSet<QString> made = touched;
    for (Folder& folder : *table) {
        const auto live = touched.contains(folder.path) ? m_folders.constFind(folder.path) : m_folders.cend();
        if (live != m_folders.cend()) {
            folder.uid = live->uid;
            folder.kind = live->kind;
            folder.name = live->name;
            folder.starts = live->starts;
        }
        made.remove(folder.path);
    }
    for (const QString& path : std::as_const(made)) {
        stale->remove(path);
    }
    return made;
}

void
RecordingStore::noteFolderAddedLocked(const QString& path, int root) {
    const auto listing = m_listing.find(root);
    if (listing != m_listing.end()) {
        listing->insert(path);
    }
}

bool
RecordingStore::makeFolderLocked(const QString& path, int root, const QString& kind, const QString& uid,
                                 const QString& name) {
    if (!QDir().mkdir(path)) {
        return false;
    }
    if (!write_folder_json(path, uid, kind, kind == kExplore ? QStringLiteral("Explore") : name)) {
        (void)QDir().rmdir(path);
        return false;
    }
    Folder folder;
    folder.path = path;
    folder.uid = uid;
    folder.kind = kind;
    folder.name = kind == kExplore ? QStringLiteral("Explore") : name;
    folder.root = root;
    folder.loaded = true; // new: nothing to read
    folder.starts = m_folders.value(path).starts + 1U;
    m_folders.insert(path, folder);
    noteFolderAddedLocked(path, root);
    m_foldersChanged = true;
    return true;
}

QString
RecordingStore::newFolderLocked(const QString& kind, const QString& uid, const QString& name,
                                const QList<bool>& usable) {
    const QString base = recording_folder_base_name(kind, uid, name);
    for (int i = 0; i < m_roots.size(); i++) {
        // Never a parent: an available root's parent is there, and one gone since is not made again in its place.
        const bool missing = usable.at(i) && !QFileInfo(m_roots.at(i)).isDir();
        if (!usable.at(i) || (missing && !QDir().mkdir(m_roots.at(i)))) {
            continue;
        }
        if (missing) {
            noteRootMadeLocked(i); // I2: a directory made over indexed folders is read again in full
        }
        for (int n = 1; n <= kMaxFolderCandidates; n++) {
            const QString dirName = n == 1 ? base : QStringLiteral("%1 %2").arg(base, QString::number(n));
            const QString path = clean_absolute(m_roots.at(i) + QLatin1Char('/') + dirName);
            const QFileInfo info(path);
            if (info.exists() || info.isSymLink()) {
                const CandidateFolder taken = takeCandidateFolderLocked(path, i, uid, info);
                if (taken == CandidateFolder::Taken) {
                    return path;
                }
                if (taken == CandidateFolder::NextRoot) {
                    break;
                }
                continue;
            }
            if (makeFolderLocked(path, i, kind, uid, name)) {
                return path;
            }
        }
    }
    return QString();
}

QString
RecordingStore::folderLocked(const QString& kind, const QString& uid, const QString& name) {
    QList<bool> usable = usableRootsLocked();
    QString path = existingFolderLocked(kind, uid, name, &usable);
    if (path.isEmpty()) {
        path = newFolderLocked(kind, uid, name, usable);
    }
    return path;
}

QString
RecordingStore::beginSession(const QString& kind, const QString& uid, const QString& name) {
    const bool owned = (kind == QLatin1String("saved") || kind == QLatin1String("scan")) && !uid.isEmpty();
    const QString ownerKind = owned ? kind : QString(kExplore);
    const QString ownerUid = owned ? uid : QString(kExplore);

    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_started) {
        return QString();
    }
    m_settled.wait(lock, [this]() { return m_ready || m_stop; });
    QString path = folderLocked(ownerKind, ownerUid, name);
    if (path.isEmpty()) {
        if (m_passRequested) {
            m_wake.notify_all(); // a root found gone is marked away
        }
        return QString();
    }
    m_active.insert(path);
    m_starterBegan = true;
    // Always, a folder unchanged too: a worker on its idle wait takes up the active cadence from this pass.
    m_passRequested = true;
    m_wake.notify_all();
    // A compaction already rewriting this journal finishes before the decoder may append to it.
    m_settled.wait(lock, [this, &path]() { return m_compacting != path || m_stop; });
    return path;
}

void
RecordingStore::endSession() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active.isEmpty() && !m_starterBegan) {
            return;
        }
        // The start that marked them did not happen. What the host has not reported yet stays held.
        m_active.clear();
        m_starterBegan = false;
        if (m_hostKnown && !m_hostActive) {
            m_allActive = false;
        }
        m_compactDue = true;
        m_passRequested = true;
    }
    m_wake.notify_all();
}

/* --- History. --- */

QVariantMap
RecordingStore::entryMap(const Entry& entry) const {
    return QVariantMap{
        {QStringLiteral("id"), entry.id},
        {QStringLiteral("path"), entry.path},
        {QStringLiteral("fileName"), QFileInfo(entry.path).fileName()},
        {QStringLiteral("folderUid"), folderUidLocked(entry.folder)},
        {QStringLiteral("start"), entry.start},
        {QStringLiteral("durationMs"), entry.durationMs},
        {QStringLiteral("rate"), entry.rate},
        {QStringLiteral("bytes"), entry.bytes},
        // The file as the read that indexed it saw it (issue #909): what a playlist checks the file against at its
        // turn. Never bytes, which the limit moves to the size it finds while the identity stays.
        {QStringLiteral("identitySize"), static_cast<qint64>(entry.identity.size)},
        {QStringLiteral("mtimeSec"), static_cast<qint64>(entry.identity.mtime_s)},
        {QStringLiteral("mtimeNanos"), static_cast<int>(entry.identity.mtime_ns)},
        {QStringLiteral("failed"), entry.failed},
        // Not while its root is seen away (R74: found missing, or back unread, from the first look through its read
        // again), nor while its folder's read again is still applying (R78: the rows readers get are the ones held from
        // before it, m_committed, until commitFolderLocked() replaces them): a playlist then admits none of it, and a
        // sheet shows it as storage not available.
        {QStringLiteral("available"), folderAvailableLocked(entry.folder) && folderReadLocked(entry.folder)
                                          && !m_committed.contains(entry.folder)
                                          && !rootSeenAwayLocked(m_folders.value(entry.folder).root)},
    };
}

/* A History read: counted while it waits, so a rebuild's next slice lets it in first. */
class RecordingStore::ReaderLock {
  public:
    explicit ReaderLock(const RecordingStore& store) : m_store(store) {
        m_store.m_readersWaiting++;
        m_store.m_mutex.lock();
        m_store.m_readersWaiting--;
#ifdef DSD_NEO_TEST_HOOKS
        m_store.m_readerLocks++;
#endif
    }

    ~ReaderLock() { m_store.m_mutex.unlock(); }

    ReaderLock(const ReaderLock&) = delete;
    ReaderLock& operator=(const ReaderLock&) = delete;

  private:
    const RecordingStore& m_store;
};

QList<const RecordingStore::Entry*>
RecordingStore::fragmentEntriesLocked(const QStringList& fragments) const {
    QList<const Entry*> found;
    QSet<const Entry*> seen;
    for (const QString& fragment : fragments) {
        for (const Entry* entry : readerEntriesLocked(fragment, false)) {
            if (!seen.contains(entry)) {
                seen.insert(entry);
                found.append(entry);
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const Entry* a, const Entry* b) {
        return a->start != b->start ? a->start < b->start : a->order < b->order;
    });
    return found;
}

QVariantList
RecordingStore::recordingsForFragments(const QStringList& fragments) const {
    const ReaderLock lock(*this);
    QVariantList list;
    for (const Entry* entry : fragmentEntriesLocked(fragments)) {
        list.append(entryMap(*entry));
    }
    return list;
}

bool
RecordingStore::hasRecordingForFragments(const QStringList& fragments) const {
    const ReaderLock lock(*this);
    return std::any_of(fragments.cbegin(), fragments.cend(), [this](const QString& fragment) {
        return m_committed.isEmpty() ? m_byFragment.contains(fragment)
                                     : !readerEntriesLocked(fragment, false).isEmpty();
    });
}

QList<QStringList>
RecordingStore::recordingEntryNames(const QList<QStringList>& fragmentsPerRow) const {
    const ReaderLock lock(*this);
    QList<QStringList> rows;
    rows.reserve(fragmentsPerRow.size());
    for (const QStringList& fragments : fragmentsPerRow) {
        QStringList names;
        for (const Entry* entry : fragmentEntriesLocked(fragments)) {
            names.append(recording_entry_name(entry->folder, entry->file));
        }
        rows.append(names);
    }
    return rows;
}

QList<QVariantList>
RecordingStore::recordingsForFragmentLists(const QList<QStringList>& fragmentsPerRow) const {
    // A playlist's admission looks at its storage as it is now (R74's defect, R77): the roots it would admit from are
    // taken under the lock, looked at with no lock held (one look each, an existing readable directory), and the store
    // is looked at again. Changed meanwhile, the admission runs again from the top, once; changed again, it is refused
    // (no row covered). What the look found is recorded in the store's mutable bookkeeping, under the lock: a root gone
    // noted seen away, a root back asked to be read again.
    for (int attempt = 0; attempt < 2; attempt++) {
        QHash<int, AdmissionRoot> roots;
        std::function<void()> probeHook;
        {
            const ReaderLock lock(*this);
            QList<QList<const Entry*>> found;
            found.reserve(fragmentsPerRow.size());
            for (const QStringList& fragments : fragmentsPerRow) {
                found.append(fragmentEntriesLocked(fragments));
            }
            roots = admissionRootsLocked(found);
            probeHook = m_admissionProbeHook;
        }
        QHash<int, bool> playable;
        for (auto it = roots.cbegin(); it != roots.cend(); ++it) {
            playable.insert(it.key(), rootPlayableNow(it->path));
        }
        if (probeHook) {
            probeHook();
        }
        bool wake = false;
        bool current = false;
        QList<QVariantList> rows;
        {
            const ReaderLock lock(*this);
            QList<QList<const Entry*>> found;
            found.reserve(fragmentsPerRow.size());
            for (const QStringList& fragments : fragmentsPerRow) {
                found.append(fragmentEntriesLocked(fragments));
            }
            current = admissionRootsLocked(found) == roots;
            if (current) {
                wake = admissionLookRecordedLocked(roots, playable);
                rows.reserve(found.size());
                for (const QList<const Entry*>& entries : std::as_const(found)) {
                    QVariantList list;
                    for (const Entry* entry : entries) {
                        QVariantMap map = entryMap(*entry);
                        // Not from a root the look found gone (R77), whatever the store held for it.
                        const int root = m_folders.value(entry->folder).root;
                        if (!playable.value(root)) {
                            map.insert(QStringLiteral("available"), false);
                        }
                        map.insert(QStringLiteral("deleteQueued"), m_deleteQueue.contains(entry->id));
                        list.append(map);
                    }
                    rows.append(list);
                }
            }
        }
        if (wake) {
            m_wake.notify_all();
        }
        if (current) {
            return rows;
        }
    }
    return {};
}

QHash<int, RecordingStore::AdmissionRoot>
RecordingStore::admissionRootsLocked(const QList<QList<const Entry*>>& found) const {
    QHash<int, AdmissionRoot> roots;
    for (const QList<const Entry*>& entries : found) {
        for (const Entry* entry : entries) {
            const int root = m_folders.value(entry->folder).root;
            if (root < 0 || root >= m_roots.size() || roots.contains(root)) {
                continue;
            }
            AdmissionRoot look;
            look.path = m_roots.at(root);
            look.known = m_rootAvailable.value(root);
            look.pending = m_rootsSeenAway.contains(root);
            look.rereading = m_rootsRereading.contains(root);
            look.listing = m_listing.contains(root);
            roots.insert(root, look);
        }
    }
    return roots;
}

bool
RecordingStore::rootPlayableNow(const QString& root) {
    // One look: its type (following a link, as a session's use of it does) and whether it can be read.
    const QFileInfo info(root);
    return info.isDir() && info.isReadable();
}

bool
RecordingStore::admissionLookRecordedLocked(const QHash<int, AdmissionRoot>& roots,
                                            const QHash<int, bool>& playable) const {
    // Gone: noted seen away (R74), so none of its recordings is admitted and a pass lists it in full. Back while
    // marked away: its read again is asked for at once. Either way its recordings stay ineligible until that read is
    // done. A root whose listing in full is asked for and not begun (pending) needs nothing more; one whose listing is
    // in flight or done is another matter (R79): what that listing found may be from before this loss, and the
    // storage there at its end another card, so a loss seen now asks for another listing after it.
    bool wake = false;
    for (auto it = roots.cbegin(); it != roots.cend(); ++it) {
        const AdmissionRoot& look = it.value();
        if (look.pending) {
            continue;
        }
        if (!playable.value(it.key())) {
            if (look.known || look.listing) {
                noteRootSeenAwayLocked(it.key());
                wake = true;
            }
        } else if (!look.known && !look.listing) {
            // Back while marked away: its read again is asked for at once (a listing of it under way needs none).
            m_passRequested = true;
            wake = true;
        }
    }
    return wake;
}

/* --- Signal search (issue #809). --- */

QVariantMap
RecordingStore::detectionEntryMap(const Entry& entry) const {
    QVariantMap map = entryMap(entry);
    map.insert(QStringLiteral("system"), entry.system);
    map.insert(QStringLiteral("target"), entry.target);
    map.insert(QStringLiteral("source"), entry.source);
    map.insert(QStringLiteral("fragment"), entry.fragment);
    return map;
}

int
RecordingStore::recordingCountForDetection(const QString& key) const {
    if (key.isEmpty()) {
        return 0;
    }
    const ReaderLock lock(*this);
    return static_cast<int>(readerEntriesLocked(key, true).size());
}

QVariantList
RecordingStore::recordingsForDetection(const QString& key) const {
    QVariantList list;
    if (key.isEmpty()) {
        return list;
    }
    const ReaderLock lock(*this);
    QList<const Entry*> found = readerEntriesLocked(key, true);
    std::sort(found.begin(), found.end(), [](const Entry* a, const Entry* b) {
        return a->start != b->start ? a->start > b->start : a->order > b->order;
    });
    for (const Entry* entry : std::as_const(found)) {
        list.append(detectionEntryMap(*entry));
    }
    return list;
}

double
RecordingStore::revision() const {
    return static_cast<double>(m_revision);
}

/* --- QML. --- */

bool
RecordingStore::ready() const {
    return m_summary.ready;
}

bool
RecordingStore::indexed() const {
    return m_summary.indexed;
}

bool
RecordingStore::available() const {
    return m_summary.available;
}

qint64
RecordingStore::usedBytes() const {
    return m_summary.usedBytes;
}

int
RecordingStore::recordingCount() const {
    return m_summary.count;
}

int
RecordingStore::unfinishedCount() const {
    return m_summary.unfinishedCount;
}

qint64
RecordingStore::unfinishedBytes() const {
    return m_summary.unfinishedBytes;
}

QVariantList
RecordingStore::folders() const {
    return m_summary.folders;
}

qint64
RecordingStore::freeBytes() const {
    return m_summary.freeBytes;
}

bool
RecordingStore::lowSpace() const {
    return m_summary.lowSpace;
}

int
RecordingStore::removedByLimit() const {
    return m_summary.removedByLimit - m_removedByLimitSeen;
}

int
RecordingStore::deletesWaiting() const {
    return m_summary.deletesWaiting;
}

int
RecordingStore::deleteFailures() const {
    return m_summary.deleteFailures - m_deleteFailuresSeen;
}

int
RecordingStore::deletesCancelled() const {
    return m_summary.deletesCancelled - m_deletesCancelledSeen;
}

int
RecordingStore::deletesSkipped() const {
    return m_summary.deletesSkipped - m_deletesSkippedSeen;
}

QString
RecordingStore::lastDeleteError() const {
    return deleteFailures() > 0 ? m_summary.lastDeleteError : QString();
}

void
RecordingStore::acknowledgeNotices() {
    if (removedByLimit() == 0 && deleteFailures() == 0 && deletesCancelled() == 0 && deletesSkipped() == 0) {
        return;
    }
    m_removedByLimitSeen = m_summary.removedByLimit;
    m_deleteFailuresSeen = m_summary.deleteFailures;
    m_deletesCancelledSeen = m_summary.deletesCancelled;
    m_deletesSkippedSeen = m_summary.deletesSkipped;
    Q_EMIT changed();
}

int
RecordingStore::limitMb() const {
    return m_limitMb;
}

void
RecordingStore::setLimitMb(int mb) {
    const int value = qMax(0, mb);
    const bool differs = value != m_limitMb;
    m_limitMb = value;
    {
        // Set, even to the value it held: from now on it is the user's limit, and it applies.
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!differs && m_limitSet) {
            return;
        }
        m_limitBytes = qint64{value} * kBytesPerMb;
        m_limitSet = true;
        m_limitChanged = true;
        m_passRequested = true;
    }
    m_wake.notify_all();
    if (differs) {
        Q_EMIT limitMbChanged();
    }
}

bool
RecordingStore::queueDeletesLocked(const QStringList& ids, bool* wake) {
    // Refused whole when any recording it names is on storage that is not there, or that is back but not read again
    // yet: nothing waits for storage to come back, where a read again could give its path to another recording.
    QList<int> roots;
    for (const QString& id : ids) {
        const auto entry = m_entries.constFind(id);
        if (entry == m_entries.cend()) {
            continue;
        }
        const auto folder = m_folders.constFind(entry->folder);
        if (folder == m_folders.cend()) {
            m_deleteRefusal = DeleteRefusal::StorageAway;
            return false;
        }
        if (!roots.contains(folder->root)) {
            roots.append(folder->root);
        }
    }
    bool away = false;
    bool unread = false;
    const bool readAgain = checkRootsLocked(roots, &away, &unread);
    if (readAgain) {
        m_passRequested = true;
        *wake = true;
    }
    if (away || unread) {
        m_deleteRefusal = away ? DeleteRefusal::StorageAway : DeleteRefusal::Loading;
        return false;
    }
    for (const QString& id : ids) {
        const auto entry = m_entries.constFind(id);
        if (entry != m_entries.cend()) {
            m_deleteQueue.insert(id, DeleteAsk{entry->identity, entry->path});
        }
    }
    m_deleteRefusal = DeleteRefusal::None;
    m_passRequested = true;
    *wake = true;
    return true;
}

bool
RecordingStore::checkRootsLocked(const QList<int>& roots, bool* away, bool* unread) {
    // Each root as it is now. One found missing is remembered, so the next pass lists it again in full even if it is
    // back by then (refreshRoots()); one away or remembered that is back is read again first, and the request is
    // refused as loading, not as storage away.
    bool readAgain = false;
    for (const int root : roots) {
        const bool now = rootThereForIndexLocked(root);
        const bool known = m_rootAvailable.value(root);
        const bool seen = m_rootsSeenAway.contains(root);
        if (now && known && !seen) {
            continue;
        }
        if (!now) {
            *away = true;
            // Found missing before any pass did: remembered, and a pass asked to mark it away. One a pass marked away
            // already is read in full when it is back.
            if (known && !seen) {
                noteRootSeenAwayLocked(root);
                readAgain = true;
            }
            continue;
        }
        // Back, not read again yet (marked away, or found missing by a request): the next pass reads it first.
        noteRootSeenAwayLocked(root);
        *unread = true;
        readAgain = true;
    }
    return readAgain;
}

bool
RecordingStore::requestDeletes(bool bulk, const std::function<QStringList()>& collect) {
    bool queued = false;
    bool wake = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        // A bulk delete (an owner's, every one, the unfinished) would name nothing on storage plugged in again, which
        // no pass has read yet: refused as loading, as an export is, and the read asked for (awayRootsBackLocked()).
        // Every request is refused before anything is collected while the store is not settled: a large index is not
        // walked only to be refused.
        wake = bulk && awayRootsBackLocked();
        if (wake || !storageSettledLocked()) {
            m_deleteRefusal = DeleteRefusal::Loading;
        } else {
            queued = queueDeletesLocked(collect(), &wake);
        }
    }
    if (wake) {
        m_wake.notify_all();
    }
    return queued;
}

bool
RecordingStore::deleteRecordings(const QStringList& ids) {
    return requestDeletes(false, [&ids] { return ids; });
}

bool
RecordingStore::deleteRecording(const QString& id) {
    return deleteRecordings({id});
}

bool
RecordingStore::deleteRecordingChecked(const QString& id, const QString& path, qint64 size, qint64 mtimeSec,
                                       int mtimeNanos) {
    dsd_file_identity_t pressed{};
    pressed.kind = 1;
    pressed.size = size;
    pressed.mtime_s = mtimeSec;
    pressed.mtime_ns = mtimeNanos;
    bool queued = false;
    bool wake = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        // The entry under the id is the one queueDeletesLocked() would take. Its file is checked here, with the request,
        // so a read again between the press and now never sends the delete to another file it put under the id.
        const auto entry = m_entries.constFind(id);
        if (!storageSettledLocked()) {
            m_deleteRefusal = DeleteRefusal::Loading;
        } else if (entry == m_entries.cend()) {
            m_deleteRefusal = DeleteRefusal::Gone;
        } else if (entry->path != path || dsd_file_identity_equal(&entry->identity, &pressed) == 0) {
            m_deleteRefusal = DeleteRefusal::Changed; // another file under the id: another path (R68) or identity
        } else {
            queued = queueDeletesLocked({id}, &wake);
        }
    }
    if (wake) {
        m_wake.notify_all();
    }
    return queued;
}

bool
RecordingStore::deleteFolder(const QString& uid) {
    return requestDeletes(true, [this, &uid] {
        QStringList ids;
        for (const Entry& entry : std::as_const(m_entries)) {
            if (folderUidLocked(entry.folder) == uid) {
                ids.append(entry.id);
            }
        }
        return ids;
    });
}

bool
RecordingStore::deleteUnfinished() {
    return requestDeletes(true, [this] {
        QStringList ids;
        for (const Entry& entry : std::as_const(m_entries)) {
            if (entry.unfinished) {
                ids.append(entry.id);
            }
        }
        return ids;
    });
}

bool
RecordingStore::deleteAll() {
    return requestDeletes(true, [this] { return m_entries.keys(); });
}

QString
RecordingStore::deleteRefusal() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    switch (m_deleteRefusal) {
        case DeleteRefusal::Loading: return QStringLiteral("loading");
        case DeleteRefusal::StorageAway: return QStringLiteral("storageAway");
        case DeleteRefusal::Changed: return QStringLiteral("changed");
        case DeleteRefusal::Gone: return QStringLiteral("gone");
        case DeleteRefusal::None: break;
    }
    return QString();
}

QVariantMap
RecordingStore::selectLocked(QList<const Entry*> entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry* a, const Entry* b) {
        return a->start != b->start ? a->start < b->start : a->order < b->order;
    });
    QStringList paths;
    QStringList names;
    // Each file as the index knows it (size and modification time): the transfer reads only a file that still is
    // (DecoderHost::recordingInputCheck()).
    QVariantList sizes;
    QVariantList mtimeSecs;
    QVariantList mtimeNanos;
    qint64 bytes = 0;
    for (const Entry* entry : std::as_const(entries)) {
        paths.append(entry->path);
        names.append(recording_entry_name(entry->folder, entry->file));
        sizes.append(static_cast<qint64>(entry->identity.size));
        mtimeSecs.append(static_cast<qint64>(entry->identity.mtime_s));
        mtimeNanos.append(static_cast<int>(entry->identity.mtime_ns));
        bytes += entry->bytes;
    }
    int selection = 0;
    if (!paths.isEmpty()) {
        selection = m_nextSelection++;
        for (const QString& path : std::as_const(paths)) {
            m_holds[path]++;
        }
        // Transferable only from a whole index: one taken while storage is read (a share's hold) never is.
        m_selections.insert(selection,
                            Selection{paths, m_storageReads->load(std::memory_order_relaxed), storageSettledLocked()});
    }
    return QVariantMap{
        {QStringLiteral("selection"), selection}, {QStringLiteral("paths"), paths},
        {QStringLiteral("names"), names},         {QStringLiteral("count"), static_cast<int>(paths.size())},
        {QStringLiteral("bytes"), bytes},         {QStringLiteral("sizes"), sizes},
        {QStringLiteral("mtimeSecs"), mtimeSecs}, {QStringLiteral("mtimeNanos"), mtimeNanos},
    };
}

QVariantMap
RecordingStore::selectForExport(const QString& folderUid, const QString& preset) {
    // One reading of today, at the press, in local time: the selection, every recording's date and the zip's name (the
    // returned day) all come from it (recording_export_press_zone()).
    return selectForExportAt(folderUid, preset, realtimeMSecsSinceEpoch());
}

QVariantMap
RecordingStore::selectForExportAt(const QString& folderUid, const QString& preset, qint64 pressMs) {
    const QTimeZone zone = recording_export_press_zone();
    return selectForExportOn(folderUid, preset, QDateTime::fromMSecsSinceEpoch(pressMs, zone).date(), zone);
}

QVariantMap
RecordingStore::selectForExportOn(const QString& folderUid, const QString& preset, const QDate& today,
                                  const QTimeZone& zone) {
    RecordingExportWindow window;
    const bool known = recording_export_window(preset, today, zone, &window);
    QVariantMap selection;
    bool loading = false;
    bool wake = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        QList<const Entry*> entries;
        wake = awayRootsBackLocked();
        loading = wake || !storageSettledLocked();
        if (known && !loading) {
            for (const Entry& entry : std::as_const(m_entries)) {
                if (!entry.unfinished && !entry.unreadable && folderAvailableLocked(entry.folder)
                    && !m_deleteQueue.contains(entry.id)
                    && (folderUid.isEmpty() || folderUidLocked(entry.folder) == folderUid)
                    && recording_export_covers(window, entry.start)) {
                    entries.append(&entry);
                }
            }
        }
        selection = selectLocked(entries);
    }
    if (wake) {
        m_wake.notify_all();
    }
    selection.insert(QStringLiteral("day"), today.toString(QStringLiteral("yyyy-MM-dd")));
    selection.insert(QStringLiteral("loading"), loading);
    return selection;
}

QVariantMap
RecordingStore::selectRecordings(const QStringList& ids) {
    bool wake = false;
    QVariantMap selection;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        // A save, as an export: refused while storage is read, so a selection always comes from a whole index.
        wake = awayRootsBackLocked();
        if (wake || !storageSettledLocked()) {
            selection = selectLocked({});
            selection.insert(QStringLiteral("loading"), true);
        } else {
            selection = selectIdsLocked(ids);
        }
    }
    if (wake) {
        m_wake.notify_all();
    }
    return selection;
}

bool
RecordingStore::awayRootsBackLocked() {
    // A root the last pass marked away and back since (removable storage plugged in again) wakes no pass on its own:
    // until one reads it, the index would leave out everything on it. Probed as a delete request probes its roots;
    // one back asks for a pass that reads it, and the request is refused as loading meanwhile.
    if (!m_ready) {
        return false; // the first pass reads every root
    }
    QList<int> away;
    bool appeared = false;
    for (int i = 0; i < m_rootAvailable.size(); i++) {
        if (m_listing.contains(i)) {
            continue; // one being listed now is being read already
        }
        if (!m_rootAvailable.at(i)) {
            away.append(i);
        } else if (m_rootDirMissing.value(i) && rootPlayableNow(m_roots.at(i))) {
            noteRootSeenAwayLocked(i); // I1: its directory, missing when it was listed, is there now
            appeared = true;
        }
    }
    bool missing = false;
    bool unread = false;
    if (!appeared && (away.isEmpty() || !checkRootsLocked(away, &missing, &unread))) {
        return false;
    }
    m_passRequested = true;
    return true;
}

QVariantMap
RecordingStore::selectIdsLocked(const QStringList& ids) {
    QList<const Entry*> entries;
    for (const QString& id : ids) {
        const Entry* entry = readerEntryLocked(id);
        if (entry != nullptr && !entry->unfinished && !m_deleteQueue.contains(id)
            && folderAvailableLocked(entry->folder) && !entries.contains(entry)) {
            entries.append(entry);
        }
    }
    return selectLocked(entries);
}

int
RecordingStore::selectPath(const QString& path) {
    QString id;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        id = path.isEmpty() ? QString() : readerIdForPathLocked(clean_absolute(path));
    }
    // A hold for a share's copy, taken whenever: never transferred (selectionToken() refuses it while not settled).
    const std::lock_guard<std::mutex> lock(m_mutex);
    return id.isEmpty() ? 0 : selectIdsLocked({id}).value(QStringLiteral("selection")).toInt();
}

void
RecordingStore::releaseSelection(int selection) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const QStringList paths = m_selections.take(selection).paths;
        for (const QString& path : paths) {
            const auto hold = m_holds.find(path);
            if (hold != m_holds.end() && --hold.value() <= 0) {
                m_holds.erase(hold);
            }
        }
        if (paths.isEmpty() || (m_deleteQueue.isEmpty() && !m_limitPending)) {
            return;
        }
        m_passRequested = true;
    }
    m_wake.notify_all();
}

void
RecordingStore::setPlayingPath(const QString& path) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const QString playing = path.isEmpty() ? QString() : clean_absolute(path);
        const bool released = !m_playing.isEmpty() && !same_path(m_playing, playing);
        m_playing = playing;
        // A delete, or the limit, waiting on the file the player let go of runs now, not at the next timed pass.
        if (!released || (m_deleteQueue.isEmpty() && !m_limitPending)) {
            return;
        }
        m_passRequested = true;
    }
    m_wake.notify_all();
}

bool
RecordingStore::selectionCurrent(int selection) const {
    return selectionToken(selection, nullptr);
}

bool
RecordingStore::selectionToken(int selection, RecordingSelectionToken* token) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_selections.constFind(selection);
    if (it == m_selections.cend() || !it->settled || it->storageReads != m_storageReads->load()) {
        return false;
    }
    if (token != nullptr) {
        *token = RecordingSelectionToken(m_storageReads, it->storageReads);
    }
    return true;
}

bool
RecordingStore::isIndexedPath(const QString& path) const {
    if (path.isEmpty()) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    return !readerIdForPathLocked(clean_absolute(path)).isEmpty();
}

void
RecordingStore::recheckRecordings(const QStringList& ids) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const QString& id : ids) {
            if (m_entries.contains(id)) {
                m_recheckQueue.insert(id);
            }
        }
        if (m_recheckQueue.isEmpty()) {
            return;
        }
        m_passRequested = true;
    }
    m_wake.notify_all();
}

void
RecordingStore::recheckPaths(const QStringList& paths) {
    QStringList ids;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const QString& path : paths) {
            const auto id = path.isEmpty() ? m_byPath.cend() : m_byPath.constFind(clean_absolute(path));
            if (id != m_byPath.cend()) {
                ids.append(id.value());
            }
        }
    }
    recheckRecordings(ids);
}

/* --- Tests. --- */

#ifdef DSD_NEO_TEST_HOOKS

void
RecordingStore::waitForPassCountForTest(quint64 count) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_passed.wait(lock, [this, count]() { return m_passes >= count || m_stop || !m_started; });
}

quint64
RecordingStore::passCountForTest() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_passes;
}

void
RecordingStore::waitForPassForTest() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_started || m_stop) {
        return;
    }
    const quint64 target = m_passes + (m_inPass || m_passEnding ? 2U : 1U);
    m_passRequested = true;
    m_wake.notify_all();
    m_passed.wait(lock, [this, target]() { return m_passes >= target || m_stop; });
}

void
RecordingStore::setTimingForTest(int activeMs, int idleMs) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_activeMs = activeMs;
    m_idleMs = idleMs;
}

void
RecordingStore::setReadRetryForTest(int ms) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_readRetryMs = ms;
}

void
RecordingStore::setUploadHoldsForTest(std::function<bool(const QString&)> holds) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_uploadHolds = std::move(holds);
}

void
RecordingStore::setFreeSpaceProbeForTest(std::function<qint64(const QString&)> probe) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_freeSpaceProbe = std::move(probe);
}

QStringList
RecordingStore::folderPathsForTest() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    QStringList paths = m_folders.keys();
    paths.sort();
    return paths;
}

int
RecordingStore::compactionsForTest() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_compactions;
}

qint64
RecordingStore::rebuildNsForTest() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_rebuildNs;
}

void
RecordingStore::setRemoveForTest(std::function<bool(const QString& path, QString* error)> remove) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_remove = std::move(remove);
}

void
RecordingStore::setFolderLoadHookForTest(std::function<void(const QString& folder)> hook) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_folderLoadHook = std::move(hook);
}

void
RecordingStore::setRootReadHookForTest(std::function<void(const QString& root)> hook) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_rootReadHook = std::move(hook);
}

void
RecordingStore::setTailHookForTest(std::function<void(const QString& folder)> hook) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_tailHook = std::move(hook);
}

void
RecordingStore::setSliceHookForTest(std::function<void(const QString& folder)> hook) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_sliceHook = std::move(hook);
}

void
RecordingStore::setAdmissionProbeHookForTest(std::function<void()> hook) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_admissionProbeHook = std::move(hook);
}

void
RecordingStore::setFolderCopyHookForTest(std::function<void(bool shared)> hook) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_folderCopyHook = std::move(hook);
}

void
RecordingStore::setEntryCheckHookForTest(std::function<void(const QString& id)> hook) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_entryCheckHook = std::move(hook);
}

void
RecordingStore::setPassEndHookForTest(std::function<void()> hook) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_passEndHook = std::move(hook);
}

void
RecordingStore::moveEntryPathForTest(const QString& id, const QString& path) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(id);
    if (it != m_entries.end()) {
        it->path = path;
    }
}

QStringList
RecordingStore::takeDeleteQueueForTest() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    QStringList ids = m_deleteQueue.keys();
    m_deleteQueue.clear();
    std::sort(ids.begin(), ids.end());
    return ids;
}

int
RecordingStore::readerLocksForTest() const {
    return m_readerLocks.load();
}
#endif

} // namespace dsd_qt
