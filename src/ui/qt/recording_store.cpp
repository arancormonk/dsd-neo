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
#include <QSaveFile>
#include <QStorageInfo>
#include <QVariant>
#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <limits>
#include <qflags.h>
#include <stdint.h>
#include <thread>
#include <utility>

#include "decoder_host.h"
#include "realtime_clock.h"
#include "recording_journal.h"

namespace dsd_qt {

namespace {

constexpr QLatin1String kJournalName("recordings.jsonl");
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

bool
read_folder_json(const QString& dir, QString* uid, QString* kind, QString* name) {
    QFile file(dir + QLatin1Char('/') + kFolderFile);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFolderFileBytes) {
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    *uid = object.value(QLatin1String("uid")).toString();
    *kind = object.value(QLatin1String("kind")).toString();
    *name = object.value(QLatin1String("name")).toString();
    return !uid->isEmpty();
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

bool
recording_export_range(const QString& preset, const QDate& today, qint64* from, qint64* to) {
    if (from == nullptr || to == nullptr || !today.isValid()) {
        return false;
    }
    const auto dayStart = [](const QDate& day) { return day.startOfDay().toSecsSinceEpoch(); };
    qint64 lo = std::numeric_limits<qint64>::min();
    qint64 hi = std::numeric_limits<qint64>::max();
    if (preset == QLatin1String("today")) {
        lo = dayStart(today);
    } else if (preset == QLatin1String("yesterday")) {
        lo = dayStart(today.addDays(-1));
        hi = dayStart(today);
    } else if (preset == QLatin1String("last7")) {
        lo = dayStart(today.addDays(-6));
    } else if (preset == QLatin1String("last30")) {
        lo = dayStart(today.addDays(-29));
    } else if (preset != QLatin1String("all")) {
        return false;
    }
    *from = lo;
    *to = hi;
    return true;
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
        const QByteArray native = QFile::encodeName(path);
        return dsd_rdio_upload_holds(native.constData()) != 0;
    };
    m_freeSpaceProbe = [](const QString& path) {
        QStorageInfo info(path);
        return info.isValid() && info.isReady() ? info.bytesAvailable() : qint64{-1};
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
RecordingStore::hostSessionReport(bool known, bool active) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!known) {
            return; // nothing learned: the journals stay held
        }
        const bool first = !m_hostKnown;
        m_hostKnown = true;
        if (active) {
            if (!m_hostActive && !m_starterBegan) {
                m_allActive = true; // a session no starter began here: its folder is not known
            }
            m_hostActive = true;
            return;
        }
        const bool ended = m_hostActive;
        m_hostActive = false;
        if (!first && !ended) {
            return; // idle again (a start clearing an old failure): the marks a starter just made stay
        }
        if (ended) {
            m_active.clear();
            m_starterBegan = false;
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
    const auto report = [store, host]() { store->hostSessionReport(host->sessionStateKnown(), host->sessionActive()); };
    report();
    QObject::connect(host, &DecoderHost::sessionStateChanged, store, report);
    QObject::connect(host, &DecoderHost::sessionStateKnownChanged, store, report);
}

/* --- Worker. --- */

void
RecordingStore::run() {
    const uint64_t began = dsd_realtime_mono_ns();
    pass(true);
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
        pass(false);
    }
}

void
RecordingStore::pass(bool first) {
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
    processRechecks(&p);
    processDeletes(&p);
    bool limitChanged = false;
    bool active = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        limitChanged = m_limitChanged;
        m_limitChanged = false;
        active = m_allActive || !m_active.isEmpty();
    }
    if (first || p.published || p.rebuilt || limitChanged) {
        enforceLimit(&p);
    }
    const quint64 now = dsd_realtime_mono_ms();
    if (first || p.published || p.rebuilt || (active && now - m_lastFreeCheckMs >= kFreeSpaceCheckMs)) {
        m_lastFreeCheckMs = now;
        if (checkFreeSpace()) {
            p.dirty = true;
        }
    }
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
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_inPass = false;
        m_passes++;
    }
    m_passed.notify_all();
}

RecordingStore::FileStat
RecordingStore::statOf(const QFileInfo& info, bool withTime) {
    FileStat stat;
    stat.regular = !info.isSymLink() && info.isFile();
    if (stat.regular) {
        stat.size = info.size();
        if (withTime) {
            stat.mtime = info.lastModified().toSecsSinceEpoch();
        }
    }
    return stat;
}

bool
RecordingStore::rootAvailableNow(const QString& root) {
    const QFileInfo info(root);
    if (info.exists()) {
        return info.isDir() && QDir(root).isReadable();
    }
    return !info.isSymLink() && QFileInfo(info.absolutePath()).isDir();
}

void
RecordingStore::refreshRoots(bool first, Pass* pass) {
    QStringList roots;
    QList<bool> was;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        roots = m_roots;
        was = m_rootAvailable;
    }
    for (int i = 0; i < roots.size(); i++) {
        const bool now = rootAvailableNow(roots.at(i));
        if (now && (first || !was.at(i))) {
            rebuildRoot(i, pass);
        } else if (now != was.at(i)) {
            // Gone: what it held stays indexed, untouched, until it comes back and is read again.
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_rootAvailable[i] = now;
            pass->dirty = true;
        }
    }
}

void
RecordingStore::rebuildRoot(int root, Pass* pass) {
    QString rootPath;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        rootPath = m_roots.at(root);
    }
    // The folder table first, folder.json only: what a session start needs. The journals are read after
    // (loadPendingFolders()), so a start never waits for them.
    QList<Folder> table;
    const QStringList subdirs = QDir(rootPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QString& subdir : subdirs) {
        Folder folder;
        folder.path = clean_absolute(rootPath + QLatin1Char('/') + subdir);
        folder.root = root;
        if (!QFileInfo(folder.path).isSymLink()
            && read_folder_json(folder.path, &folder.uid, &folder.kind, &folder.name)) {
            table.append(folder);
        }
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    // Whatever the index held for this root is read again from the disk. One hold of the lock, so a start never sees
    // the root available without its folders.
    QSet<QString> stale;
    for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
        if (it->root == root) {
            stale.insert(it.key());
        }
    }
    QStringList ids;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (stale.contains(it->folder)) {
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
    pass->dirty = true;
}

void
RecordingStore::loadPendingFolders(Pass* pass) {
    QList<QPair<QString, int>> pending;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
            if (!it->loaded && m_rootAvailable.value(it->root)) {
                pending.append({it.key(), it->root});
            }
        }
    }
    for (const auto& [path, root] : pending) {
        if (stopping()) {
            return;
        }
        loadFolder(path, root, false, pass);
    }
}

void
RecordingStore::loadFolder(const QString& path, int root, bool replace, Pass* pass) {
    Folder folder;
    if (!read_folder_json(path, &folder.uid, &folder.kind, &folder.name)) {
        // Not a folder the app made, or not any more (removed from a computer while its root is there, perhaps
        // between the folder table's read and this one): nothing in it is managed, and no session is given it.
        const std::lock_guard<std::mutex> lock(m_mutex);
        removeFolderEntriesLocked(path, pass);
        m_folders.remove(path);
        pass->dirty = true;
        return;
    }
    folder.path = path;
    folder.root = root;
    if (m_folderLoadHook) {
        m_folderLoadHook(path);
    }

    // One listing serves every publish whose file sits in the folder itself, which is all of them in a folder the app
    // made; a name in a subfolder gets its own stat below.
    QHash<QString, FileStat> files;
    QList<Entry> unfinished;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString name = info.fileName();
        QString token;
        if (recording_temp_name_token(name, &token)) {
            // This process's own temp files may still be open for writing: never indexed, never touched.
            const FileStat stat = statOf(info, true);
            if (token != m_liveToken && stat.regular) {
                Entry entry;
                entry.path = clean_absolute(info.absoluteFilePath());
                entry.id = QStringLiteral("unfinished:") + entry.path;
                entry.folder = path;
                entry.file = name;
                entry.start = stat.mtime;
                entry.bytes = stat.size;
                entry.unfinished = true;
                unfinished.append(entry);
            }
            continue;
        }
        files.insert(name, statOf(info, false));
    }

    QByteArray data;
    QFile journal(path + QLatin1Char('/') + kJournalName);
    if (journal.open(QIODevice::ReadOnly)) {
        data = journal.readAll();
    }
    const qsizetype consumed = data.lastIndexOf('\n') + 1;
    QList<RecordingJournalRecord> records;
    const qint64 lines = parseLines(data.left(consumed), &records);
    statPublished(path, records, &files);

    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (replace) {
            removeFolderEntriesLocked(path, pass);
        }
        folder.offset = consumed;
        folder.lines = lines;
        folder.loaded = true;
        // The owner as the table holds it now: a session start may have relabelled the folder since folder.json was
        // read above, and the newer label wins.
        const auto table = m_folders.constFind(path);
        if (table != m_folders.cend()) {
            folder.uid = table->uid;
            folder.kind = table->kind;
            folder.name = table->name;
        }
        m_folders.insert(path, folder);
        // Grown once here, not rehashed under the lock slice by slice.
        m_entries.reserve(m_entries.size() + records.size() + unfinished.size());
        m_byPath.reserve(m_byPath.size() + records.size());
        m_byFragment.reserve(m_byFragment.size() + records.size());
        for (const Entry& entry : unfinished) {
            insertEntryLocked(entry, pass);
        }
    }
    // In slices, so History's reads on the GUI thread never wait for a whole large folder. The lock is not fair: a
    // reader the unlock woke would lose it to the next slice, so the next slice waits until the readers are through.
    for (qsizetype begin = 0; begin < records.size() && !stopping(); begin += kApplySlice) {
        yieldToReaders();
        const std::lock_guard<std::mutex> lock(m_mutex);
        const qsizetype end = qMin(records.size(), begin + kApplySlice);
        for (qsizetype i = begin; i < end; i++) {
            applyRecordLocked(folder, records.at(i), files, pass);
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
            files->insert(record.file, statOf(QFileInfo(folder + QLatin1Char('/') + record.file), false));
        }
    }
}

void
RecordingStore::tailFolders(Pass* pass) {
    QList<QPair<QString, qint64>> folders;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
            if (it->loaded && m_rootAvailable.value(it->root)) {
                folders.append({it.key(), it->offset});
            }
        }
    }
    for (const auto& [path, offset] : folders) {
        tailFolder(path, offset, pass);
    }
}

void
RecordingStore::tailFolder(const QString& path, qint64 offset, Pass* pass) {
    QFile journal(path + QLatin1Char('/') + kJournalName);
    const qint64 size = journal.exists() ? journal.size() : 0;
    if (size == offset) {
        return;
    }
    if (size < offset) {
        // Shorter than what was read: replaced or truncated behind the store's back. Read it again from the start.
        int root = -1;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            root = m_folders.value(path).root;
        }
        loadFolder(path, root, true, pass);
        return;
    }
    if (!journal.open(QIODevice::ReadOnly) || !journal.seek(offset)) {
        return;
    }
    const QByteArray data = journal.read(size - offset);
    // Complete lines only: a line still being written is read once its newline lands.
    const qsizetype consumed = data.lastIndexOf('\n') + 1;
    if (consumed == 0) {
        return;
    }
    QList<RecordingJournalRecord> records;
    const qint64 lines = parseLines(data.left(consumed), &records);
    QHash<QString, FileStat> files;
    statPublished(path, records, &files);

    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto folder = m_folders.find(path);
    if (folder == m_folders.end() || folder->offset != offset) {
        return;
    }
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
        case RecordingJournalRecord::Publish: {
            const QString id = record.id.isEmpty()
                                   ? QStringLiteral("file:") + folder.path + QLatin1Char('/') + record.file
                                   : record.id;
            const FileStat stat = files.value(record.file);
            if (!stat.regular) {
                // Missing on disk with its root available (deleted, or not a plain file any more): not in the index.
                if (m_entries.contains(id)) {
                    removeEntryLocked(id, pass);
                }
                return;
            }
            Entry entry;
            entry.id = id;
            entry.path = QDir::cleanPath(folder.path + QLatin1Char('/') + record.file);
            entry.folder = folder.path;
            entry.file = record.file;
            entry.t = record.t;
            entry.start = record.start;
            entry.durationMs = record.durationMs;
            entry.rate = record.rate;
            entry.bytes = stat.size;
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
            if (entry.hasInstance && entry.slot >= 0 && entry.hasRow) {
                entry.fragment = call_history_fragment_key(entry.instance, entry.slot, entry.row);
            }
            insertEntryLocked(entry, pass);
            pass->published = true;
            return;
        }
        case RecordingJournalRecord::Link: {
            const auto it = m_entries.find(record.id);
            if (it == m_entries.end() || it->unfinished) {
                return; // deleted since, or never kept
            }
            const QString fragment = call_history_fragment_key(record.instance, record.slot, record.row);
            if (it->fragment != fragment) {
                unindexFragmentLocked(*it, pass);
                it->fragment = fragment;
                m_byFragment[fragment].append(it->id);
                pass->fragments.insert(fragment);
            }
            it->hasInstance = true;
            it->instance = record.instance;
            it->slot = record.slot;
            it->hasRow = true;
            it->row = record.row;
            it->pending = false;
            pass->dirty = true;
            return;
        }
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
RecordingStore::insertEntryLocked(Entry entry, Pass* pass) {
    if (m_entries.contains(entry.id)) {
        removeEntryLocked(entry.id, pass);
    }
    entry.order = m_nextOrder++;
    if (!entry.unfinished) {
        m_byPath.insert(entry.path, entry.id);
    }
    if (!entry.fragment.isEmpty()) {
        m_byFragment[entry.fragment].append(entry.id);
        pass->fragments.insert(entry.fragment);
    }
    m_entries.insert(entry.id, entry);
    pass->dirty = true;
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
RecordingStore::removeEntryLocked(const QString& id, Pass* pass) {
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) {
        return;
    }
    unindexFragmentLocked(*it, pass);
    const auto byPath = m_byPath.find(it->path);
    if (byPath != m_byPath.end() && byPath.value() == id) {
        m_byPath.erase(byPath);
    }
    m_entries.erase(it);
    pass->dirty = true;
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

bool
RecordingStore::folderAvailableLocked(const QString& folder) const {
    const auto it = m_folders.constFind(folder);
    return it != m_folders.cend() && m_rootAvailable.value(it->root);
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
    for (const QString& id : std::as_const(ids)) {
        yieldToReaders();
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.find(id);
        if (it == m_entries.end() || !folderAvailableLocked(it->folder)) {
            continue; // gone already, or its storage is away (never read as deleted)
        }
        const FileStat stat = statOf(QFileInfo(it->path), false);
        if (!stat.regular) {
            removeEntryLocked(id, pass);
        } else if (stat.size != it->bytes) {
            it->bytes = stat.size;
            pass->dirty = true;
        }
    }
}

void
RecordingStore::processDeletes(Pass* pass) {
    QStringList ids;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ids = m_deleteQueue.values();
    }
    for (const QString& id : std::as_const(ids)) {
        if (deleteEntry(id, DeleteCause::Asked, pass) != DeleteResult::Held) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_deleteQueue.remove(id);
        }
    }
}

RecordingStore::DeleteResult
RecordingStore::deleteEntry(const QString& id, DeleteCause cause, Pass* pass) {
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
        path = it->path;
    }
    // The rdio uploader takes its queue's own lock; asked here, outside the store's. A job for a file the journal
    // already named was queued before its line was written, so a hold can only end after this, never start.
    const bool uploading = m_uploadHolds(path);

    QString error;
    {
        yieldToReaders();
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.constFind(id);
        if (it == m_entries.cend()) {
            return DeleteResult::Gone;
        }
        if (uploading || m_holds.contains(path) || (!m_playing.isEmpty() && same_path(m_playing, path))) {
            return DeleteResult::Held;
        }
        if (liveTemp(path)) {
            return DeleteResult::Kept; // this process may still be writing it
        }
        bool inside = false;
        for (int i = 0; i < m_roots.size() && !inside; i++) {
            inside = m_rootAvailable.at(i) && recording_path_inside_root(path, m_roots.at(i));
        }
        // Right before the unlink, without following a link: what is there now is what gets removed.
        const QFileInfo now(path);
        if (!now.isSymLink() && !now.exists()) {
            removeEntryLocked(id, pass); // already gone
            return DeleteResult::Gone;
        }
        if (!inside || now.isSymLink() || !now.isFile()) {
            // Not a recording the store may remove any more: it leaves the index, and the disk is left alone.
            removeEntryLocked(id, pass);
            return DeleteResult::Kept;
        }
        if (m_remove(path, &error)) {
            removeEntryLocked(id, pass);
            return DeleteResult::Removed;
        }
    }
    noteDeleteFailure(cause, error);
    pass->dirty = true;
    return DeleteResult::Failed;
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

void
RecordingStore::enforceLimit(Pass* pass) {
    struct Candidate {
        QString id;
        qint64 bytes;
        qint64 start;
        quint64 order;
    };

    QList<Candidate> candidates;
    qint64 total = 0;
    qint64 limit = 0;
    {
        // Summed outside the lock on shared copies, as publish() does; they are gone before the first delete.
        QHash<QString, Entry> entries;
        QHash<QString, Folder> folders;
        QList<bool> rootAvailable;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            limit = m_limitBytes;
            if (!m_limitSet || limit <= 0) {
                return; // no limit, or not the user's yet
            }
            entries = m_entries;
            folders = m_folders;
            rootAvailable = m_rootAvailable;
        }
        for (const Entry& entry : std::as_const(entries)) {
            const auto folder = folders.constFind(entry.folder);
            if (folder != folders.cend() && rootAvailable.value(folder->root)) {
                total += entry.bytes;
            }
        }
        if (total <= limit) {
            return;
        }
        for (const Entry& entry : std::as_const(entries)) {
            const auto folder = folders.constFind(entry.folder);
            if (folder != folders.cend() && rootAvailable.value(folder->root)) {
                candidates.append({entry.id, entry.bytes, entry.start, entry.order});
            }
        }
    }
    // Oldest first (an unfinished recording by its file's time), down to 95% so one more call does not start again.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.start != b.start ? a.start < b.start : a.order < b.order;
    });
    const qint64 target = limit / 100 * 95;
    int removed = 0;
    for (const Candidate& candidate : candidates) {
        if (total <= target) {
            break;
        }
        const DeleteResult result = deleteEntry(candidate.id, DeleteCause::Limit, pass);
        if (result == DeleteResult::Removed) {
            total -= candidate.bytes;
            removed++;
        } else if (result == DeleteResult::Gone) {
            total -= candidate.bytes; // already gone: not one the limit removed
        } else if (result == DeleteResult::Kept) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_entries.contains(candidate.id)) {
                total -= candidate.bytes; // no longer managed
            }
        }
    }
    if (removed > 0) {
        m_removedByLimit += removed;
        pass->dirty = true;
    }
}

bool
RecordingStore::checkFreeSpace() {
    QString where;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (int i = 0; i < m_roots.size() && where.isEmpty(); i++) {
            if (m_rootAvailable.at(i)) {
                where = m_roots.at(i);
            }
        }
    }
    qint64 free = -1;
    if (!where.isEmpty()) {
        free = m_freeSpaceProbe(QFileInfo::exists(where) ? where : QFileInfo(where).absolutePath());
    }
    const bool low = free >= 0 && free < kLowSpaceBytes;
    const bool changed = free != m_freeBytes || low != m_lowSpace;
    m_freeBytes = free;
    m_lowSpace = low;
    return changed;
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
        for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
            if (it->loaded && m_rootAvailable.value(it->root) && !m_active.contains(it.key()) && !m_allActive
                && it->lines > 0 && it->lines != it->compactFailedAt && it->lines > 2 * live.value(it.key())) {
                candidates.append(it.key());
            }
        }
    }
    for (const QString& path : candidates) {
        compactFolder(path);
    }
}

bool
RecordingStore::compactFolder(const QString& path) {
    QList<Entry> live;
    qint64 offset = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        // A session that starts now waits in beginSession() until this compaction is over.
        if (m_allActive || m_active.contains(path) || !m_folders.contains(path)) {
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

    const QString journalPath = path + QLatin1Char('/') + kJournalName;
    bool ok = false;
    qint64 written = 0;
    QFile current(journalPath);
    if (current.open(QIODevice::ReadOnly) && current.seek(offset)) {
        // Past the cursor there may only be a line a crash tore; a whole line the store has not read stops it.
        const bool unread = current.read(current.size() - offset).contains('\n');
        current.close();
        QSaveFile out(journalPath);
        if (!unread && out.open(QIODevice::WriteOnly)) {
            // Private, as the decoder creates it (dsd_fopen_private()), where the filesystem keeps modes at all: a
            // FUSE-backed shared storage may refuse the change, and that must not stop the compaction.
            (void)out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            ok = true;
            for (const Entry& entry : std::as_const(live)) {
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
                const QByteArray line = recording_journal_publish_line(record);
                if (out.write(line) != line.size()) {
                    ok = false;
                    break;
                }
                written += line.size();
            }
            if (ok) {
                ok = out.commit();
            } else {
                out.cancelWriting();
            }
        }
    }

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
        summary.usedBytes += entry.bytes;
        if (entry.unfinished) {
            summary.unfinishedCount++;
            summary.unfinishedBytes += entry.bytes;
            continue;
        }
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
    // The index is copied (shared, not deep) under the lock and summed outside it. This thread is the only one that
    // changes the recordings, and the copies are gone before it next does, so they are never deep-copied; a session
    // start that adds a folder meanwhile copies the small folder table, which is harmless.
    QHash<QString, Entry> entries;
    QHash<QString, Folder> folders;
    QList<bool> rootAvailable;
    Summary summary;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!pass.dirty && pass.fragments.isEmpty() && m_deleteQueue.size() == qsizetype{m_outbox.deletesWaiting}) {
            return;
        }
        entries = m_entries;
        folders = m_folders;
        rootAvailable = m_rootAvailable;
        summary.ready = m_ready;
        summary.indexed = m_ready;
        for (const Folder& folder : std::as_const(m_folders)) {
            summary.indexed = summary.indexed && (folder.loaded || !m_rootAvailable.value(folder.root));
        }
        summary.deletesWaiting = static_cast<int>(m_deleteQueue.size());
    }
    const Summary sums = summarize(entries, folders, rootAvailable);
    entries.clear();
    folders.clear();
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

void
RecordingStore::deliver() {
    Summary summary;
    QSet<QString> fragments;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        summary = m_outbox;
        fragments.swap(m_outFragments);
        m_deliveryQueued = false;
    }
    const int removed = summary.removedByLimit - m_summary.removedByLimit;
    const bool differs =
        summary.ready != m_summary.ready || summary.indexed != m_summary.indexed
        || summary.available != m_summary.available || summary.usedBytes != m_summary.usedBytes
        || summary.count != m_summary.count || summary.unfinishedCount != m_summary.unfinishedCount
        || summary.unfinishedBytes != m_summary.unfinishedBytes || summary.folders != m_summary.folders
        || summary.freeBytes != m_summary.freeBytes || summary.lowSpace != m_summary.lowSpace
        || summary.removedByLimit != m_summary.removedByLimit || summary.deletesWaiting != m_summary.deletesWaiting
        || summary.deleteFailures != m_summary.deleteFailures || summary.lastDeleteError != m_summary.lastDeleteError;
    m_summary = std::move(summary);
    if (differs) {
        Q_EMIT changed();
    }
    if (!fragments.isEmpty()) {
        QStringList list = fragments.values();
        list.sort();
        Q_EMIT recordingFragmentsChanged(list);
    }
    if (removed > 0) {
        Q_EMIT limitReached(removed);
    }
}

/* --- Sessions. --- */

QString
RecordingStore::folderLocked(const QString& kind, const QString& uid, const QString& name) {
    // The owner's folder in the first available root that has one: a rename keeps it, and a clash of the short
    // uid in the name never merges two owners.
    const Folder* best = nullptr;
    QStringList gone;
    for (const Folder& folder : std::as_const(m_folders)) {
        if (folder.uid != uid || !m_rootAvailable.value(folder.root)
            || (best != nullptr
                && (folder.root > best->root || (folder.root == best->root && folder.path >= best->path)))) {
            continue;
        }
        // A folder whose folder.json no longer names this owner (removed from a computer) is not handed out. One the
        // worker has not read yet holds no recordings, so it leaves the table here; a read one is the worker's.
        Folder onDisk;
        if (!read_folder_json(folder.path, &onDisk.uid, &onDisk.kind, &onDisk.name) || onDisk.uid != uid) {
            if (!folder.loaded) {
                gone.append(folder.path);
            }
            continue;
        }
        best = &folder;
    }
    const QString bestPath = best != nullptr ? best->path : QString();
    for (const QString& path : std::as_const(gone)) {
        m_folders.remove(path);
        m_foldersChanged = true;
    }
    const auto found = bestPath.isEmpty() ? m_folders.cend() : m_folders.constFind(bestPath);
    best = found != m_folders.cend() ? &found.value() : nullptr;
    if (best != nullptr) {
        const QString path = best->path;
        if (kind != kExplore && !name.isEmpty() && best->name != name && write_folder_json(path, uid, kind, name)) {
            m_folders[path].name = name;
            m_foldersChanged = true;
        }
        return path;
    }
    const QString base = recording_folder_base_name(kind, uid, name);
    for (int i = 0; i < m_roots.size(); i++) {
        if (!m_rootAvailable.at(i) || !QDir().mkpath(m_roots.at(i))) {
            continue;
        }
        for (int n = 1; n <= kMaxFolderCandidates; n++) {
            const QString dirName = n == 1 ? base : QStringLiteral("%1 %2").arg(base).arg(n);
            const QString path = clean_absolute(m_roots.at(i) + QLatin1Char('/') + dirName);
            const QFileInfo info(path);
            if (info.exists() || info.isSymLink()) {
                Folder found;
                if (!info.isSymLink() && info.isDir() && read_folder_json(path, &found.uid, &found.kind, &found.name)
                    && found.uid == uid) {
                    // Made for this owner, not read yet: the worker reads its journal from the start.
                    found.path = path;
                    found.root = i;
                    m_folders.insert(path, found); // not read yet: the worker reads its journal next
                    m_passRequested = true;
                    m_foldersChanged = true;
                    return path;
                }
                continue; // another owner's, or not the app's
            }
            if (!QDir().mkdir(path)) {
                continue;
            }
            if (!write_folder_json(path, uid, kind, kind == kExplore ? QStringLiteral("Explore") : name)) {
                (void)QDir().rmdir(path);
                continue;
            }
            Folder folder;
            folder.path = path;
            folder.uid = uid;
            folder.kind = kind;
            folder.name = kind == kExplore ? QStringLiteral("Explore") : name;
            folder.root = i;
            folder.loaded = true; // new: nothing to read
            m_folders.insert(path, folder);
            m_foldersChanged = true;
            return path;
        }
    }
    return QString();
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
        return QString();
    }
    m_active.insert(path);
    m_starterBegan = true;
    if (m_foldersChanged) {
        m_passRequested = true;
        m_wake.notify_all();
    }
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
        {QStringLiteral("failed"), entry.failed},
        {QStringLiteral("available"), folderAvailableLocked(entry.folder)},
    };
}

/* A History read: counted while it waits, so a rebuild's next slice lets it in first. */
class RecordingStore::ReaderLock {
  public:
    explicit ReaderLock(const RecordingStore& store) : m_store(store) {
        m_store.m_readersWaiting++;
        m_store.m_mutex.lock();
        m_store.m_readersWaiting--;
    }

    ~ReaderLock() { m_store.m_mutex.unlock(); }

    ReaderLock(const ReaderLock&) = delete;
    ReaderLock& operator=(const ReaderLock&) = delete;

  private:
    const RecordingStore& m_store;
};

QVariantList
RecordingStore::recordingsForFragments(const QStringList& fragments) const {
    const ReaderLock lock(*this);
    QList<const Entry*> found;
    for (const QString& fragment : fragments) {
        const auto ids = m_byFragment.constFind(fragment);
        if (ids == m_byFragment.cend()) {
            continue;
        }
        for (const QString& id : *ids) {
            const auto entry = m_entries.constFind(id);
            if (entry != m_entries.cend() && !found.contains(&entry.value())) {
                found.append(&entry.value());
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const Entry* a, const Entry* b) {
        return a->start != b->start ? a->start < b->start : a->order < b->order;
    });
    QVariantList list;
    for (const Entry* entry : std::as_const(found)) {
        list.append(entryMap(*entry));
    }
    return list;
}

bool
RecordingStore::hasRecordingForFragments(const QStringList& fragments) const {
    const ReaderLock lock(*this);
    for (const QString& fragment : fragments) {
        if (m_byFragment.contains(fragment)) {
            return true;
        }
    }
    return false;
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
    return m_summary.removedByLimit;
}

int
RecordingStore::deletesWaiting() const {
    return m_summary.deletesWaiting;
}

int
RecordingStore::deleteFailures() const {
    return m_summary.deleteFailures;
}

QString
RecordingStore::lastDeleteError() const {
    return m_summary.lastDeleteError;
}

int
RecordingStore::limitMb() const {
    return m_limitMb;
}

void
RecordingStore::setLimitMb(int limitMb) {
    const int value = qMax(0, limitMb);
    const bool changed = value != m_limitMb;
    m_limitMb = value;
    {
        // Set, even to the value it held: from now on it is the user's limit, and it applies.
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!changed && m_limitSet) {
            return;
        }
        m_limitBytes = qint64{value} * kBytesPerMb;
        m_limitSet = true;
        m_limitChanged = true;
        m_passRequested = true;
    }
    m_wake.notify_all();
    if (changed) {
        Q_EMIT limitMbChanged();
    }
}

void
RecordingStore::deleteRecordings(const QStringList& ids) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const QString& id : ids) {
            if (m_entries.contains(id)) {
                m_deleteQueue.insert(id);
            }
        }
        m_passRequested = true;
    }
    m_wake.notify_all();
}

void
RecordingStore::deleteRecording(const QString& id) {
    deleteRecordings({id});
}

void
RecordingStore::deleteFolder(const QString& uid) {
    QStringList ids;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const Entry& entry : std::as_const(m_entries)) {
            if (folderUidLocked(entry.folder) == uid) {
                ids.append(entry.id);
            }
        }
    }
    deleteRecordings(ids);
}

void
RecordingStore::deleteUnfinished() {
    QStringList ids;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const Entry& entry : std::as_const(m_entries)) {
            if (entry.unfinished) {
                ids.append(entry.id);
            }
        }
    }
    deleteRecordings(ids);
}

void
RecordingStore::deleteAll() {
    QStringList ids;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ids = m_entries.keys();
    }
    deleteRecordings(ids);
}

QVariantMap
RecordingStore::selectLocked(QList<const Entry*> entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry* a, const Entry* b) {
        return a->start != b->start ? a->start < b->start : a->order < b->order;
    });
    QStringList paths;
    QStringList names;
    qint64 bytes = 0;
    for (const Entry* entry : std::as_const(entries)) {
        paths.append(entry->path);
        names.append(QFileInfo(entry->folder).fileName() + QLatin1Char('/') + entry->file);
        bytes += entry->bytes;
    }
    int selection = 0;
    if (!paths.isEmpty()) {
        selection = m_nextSelection++;
        for (const QString& path : std::as_const(paths)) {
            m_holds[path]++;
        }
        m_selections.insert(selection, paths);
    }
    return QVariantMap{
        {QStringLiteral("selection"), selection}, {QStringLiteral("paths"), paths},
        {QStringLiteral("names"), names},         {QStringLiteral("count"), static_cast<int>(paths.size())},
        {QStringLiteral("bytes"), bytes},
    };
}

QVariantMap
RecordingStore::selectForExport(const QString& folderUid, const QString& preset) {
    qint64 from = 0;
    qint64 to = 0;
    const bool known = recording_export_range(preset, realtimeCurrentDate(), &from, &to);
    const std::lock_guard<std::mutex> lock(m_mutex);
    QList<const Entry*> entries;
    if (known) {
        for (const Entry& entry : std::as_const(m_entries)) {
            if (!entry.unfinished && folderAvailableLocked(entry.folder) && !m_deleteQueue.contains(entry.id)
                && (folderUid.isEmpty() || folderUidLocked(entry.folder) == folderUid) && entry.start >= from
                && entry.start < to) {
                entries.append(&entry);
            }
        }
    }
    return selectLocked(entries);
}

QVariantMap
RecordingStore::selectRecordings(const QStringList& ids) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    QList<const Entry*> entries;
    for (const QString& id : ids) {
        const auto entry = m_entries.constFind(id);
        if (entry != m_entries.cend() && !entry->unfinished && !m_deleteQueue.contains(id)
            && folderAvailableLocked(entry->folder) && !entries.contains(&entry.value())) {
            entries.append(&entry.value());
        }
    }
    return selectLocked(entries);
}

void
RecordingStore::releaseSelection(int selection) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const QStringList paths = m_selections.take(selection);
        for (const QString& path : paths) {
            const auto hold = m_holds.find(path);
            if (hold != m_holds.end() && --hold.value() <= 0) {
                m_holds.erase(hold);
            }
        }
        if (paths.isEmpty() || m_deleteQueue.isEmpty()) {
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
        // A delete waiting on the file the player let go of runs now, not at the next timed pass.
        if (!released || m_deleteQueue.isEmpty()) {
            return;
        }
        m_passRequested = true;
    }
    m_wake.notify_all();
}

bool
RecordingStore::isIndexedPath(const QString& path) const {
    if (path.isEmpty()) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_byPath.contains(clean_absolute(path));
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

/* --- Tests. --- */

#ifdef DSD_NEO_TEST_HOOKS

void
RecordingStore::waitForPassForTest() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_started || m_stop) {
        return;
    }
    const quint64 target = m_passes + (m_inPass ? 2U : 1U);
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
#endif

} // namespace dsd_qt
