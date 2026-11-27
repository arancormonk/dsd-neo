// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Issue #708: the call recording store, the app's one index of call recordings.
 *
 * Journals are written here by hand, line by line as the format contract in docs/code_map.md spells them, and once
 * through the decoder's own writer (src/core/file/wav_journal.c). Every case runs the real store with its worker
 * thread, stepped one pass at a time: the tail reads complete lines only; publish, link and unlink from the writer;
 * History rows learn their recordings from the store, early or late, and lose them on a delete; a journal is compacted
 * only when no session uses its folder, and read once after; storage that is away is not storage that was emptied;
 * folders follow their owner's uid; deletion refuses what is not a managed regular file inside a root and waits for
 * every hold; the storage limit removes the oldest first, down to 95%; temp files are unfinished recordings only when
 * an earlier process left them; low free space is reported; exports select by owner and date and hold their files. */

#include <QBuffer>
#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QModelIndex>
#include <QObject>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <qflags.h>
#include <qtenvironmentvariables.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <utility>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "call_history_playlist.h"
#include "call_recording_provider.h"
#include "decoder_host.h"
#include "platform_path.h"
#include "recording_journal.h"
#include "recording_selection_token.h"
#include "recording_store.h"
#include "wav_journal.h"

using dsd_qt::RecordingSelectionToken;
using dsd_qt::RecordingStore;

namespace {

int g_failures = 0;

/* The store's once-only warnings, counted and kept out of the output. */
std::atomic<int> g_storeWarnings{0};
QtMessageHandler g_previousHandler = nullptr;

void
count_warnings(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (message.startsWith(QLatin1String("call recordings: "))) {
        g_storeWarnings++;
        return;
    }
    if (g_previousHandler != nullptr) {
        g_previousHandler(type, context, message);
    }
}

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

constexpr QLatin1String kUidA("1a2b3c4d-0000-4000-8000-00000000000a");
constexpr QLatin1String kUidB("1a2b3c4d-0000-4000-8000-00000000000b");
constexpr QLatin1String kUidC("5e6f7a8b-0000-4000-8000-00000000000c");
const quint64 kRing = 0x00C0FFEE12345678ULL;

/* Two roots under one disposable folder, as the Android host gives them: external first, internal second. */
struct Roots {
    QTemporaryDir dir;
    QString external;
    QString internal;

    Roots() {
        external = dir.path() + QStringLiteral("/ext/Call recordings");
        internal = dir.path() + QStringLiteral("/int/Call recordings");
        QDir().mkpath(dir.path() + QStringLiteral("/ext"));
        QDir().mkpath(dir.path() + QStringLiteral("/int"));
    }

    QStringList
    list() const {
        return {external, internal};
    }
};

/* A store that passes only when a test asks, sees no upload holds and plenty of space unless told otherwise. */
struct Store {
    RecordingStore store;
    std::atomic<qint64> freeBytes{qint64{50000} * RecordingStore::kBytesPerMb};
    QSet<QString> uploading;
    std::atomic<bool> uploadingSet{false};

    /* The host is known and idle unless a test says otherwise (hostSessionReport() before start()). */
    explicit Store(const QStringList& roots, bool startNow = true) {
        store.setRoots(roots);
        store.hostSessionReport(true, false, false);
        store.setTimingForTest(3600000, 3600000);
        store.setReadRetryForTest(0); // a failed read is tried again at the next pass a test asks for
        store.setUploadHoldsForTest([this](const QString& path) {
            return uploadingSet.load() && uploading.contains(QFileInfo(path).fileName());
        });
        store.setFreeSpaceProbeForTest([this](const QString&) { return freeBytes.load(); });
        if (startNow) {
            start();
        }
    }

    void
    start() {
        store.start();
        pass();
    }

    /* The limit set to @p mb once the store has started and settled, and the pass that applies it waited for without
     * asking for another while it runs: a limit walk sees no pass asked for, so its count does not go stale under it
     * (a test hooking that walk sees it whole). */
    void
    limitQuietly(int mb) {
        start();
        const quint64 before = store.passCountForTest();
        store.setLimitMb(mb);
        store.waitForPassCountForTest(before + 1U);
        QCoreApplication::processEvents();
    }

    void
    pass() {
        store.waitForPassForTest();
        QCoreApplication::processEvents();
    }

    /* The deletes waiting, taken back, and the pass the request woke let finish: while the worker reads storage the
     * next export or delete request would be refused. GUI thread only (a worker hook calls takeDeleteQueueForTest()). */
    QStringList
    taken() {
        const QStringList ids = store.takeDeleteQueueForTest();
        pass();
        return ids;
    }
};

/* @p bytes written to @p path, created private (dsd_fopen_private()), appended (@p mode "ab") or replacing what was
 * there ("wb"). */
bool
write_private(const QString& path, const QByteArray& bytes, const char* mode) {
    FILE* file = dsd_fopen_private(dsd_qt::platform_path(path).constData(), mode);
    const bool written =
        file != nullptr
        && fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), file) == static_cast<size_t>(bytes.size());
    return file != nullptr && fclose(file) == 0 && written;
}

void
append(const QString& path, const QByteArray& bytes) {
    if (!write_private(path, bytes, "ab")) {
        DSD_FPRINTF(stderr, "FAIL: cannot append to %s\n", qPrintable(path));
        g_failures++;
    }
}

void
make_file(const QString& path, qint64 bytes) {
    if (!write_private(path, QByteArray(bytes, 'w'), "wb")) {
        DSD_FPRINTF(stderr, "FAIL: cannot write %s\n", qPrintable(path));
        g_failures++;
    }
}

/* @p path's modification time moved to @p seconds since the Unix epoch, the file and its size left as they are. */
void
set_mtime(const QString& path, qint64 seconds) {
    QFile f(path);
    const bool set = f.open(QIODevice::ReadWrite)
                     && f.setFileTime(QDateTime::fromSecsSinceEpoch(seconds), QFileDevice::FileModificationTime);
    expect("a modification time is set", set);
}

QString
journal_of(const QString& folder) {
    return folder + QStringLiteral("/recordings.jsonl");
}

QByteArray
read_all(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/* A `publish` line as the contract spells it; @p row < 0 for none, @p pending for a recording waiting for its row. */
QByteArray
publish_line(const QString& id, const QString& file, qint64 start, qint64 row, bool pending = false, int slot = 0) {
    QJsonObject o;
    o.insert(QStringLiteral("v"), 1);
    o.insert(QStringLiteral("type"), QStringLiteral("publish"));
    o.insert(QStringLiteral("t"), start + 5);
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("file"), file);
    o.insert(QStringLiteral("folder"), QStringLiteral("/somewhere"));
    o.insert(QStringLiteral("start"), start);
    o.insert(QStringLiteral("duration_ms"), 4000);
    o.insert(QStringLiteral("rate"), 8000);
    o.insert(QStringLiteral("instance"), QStringLiteral("%1").arg(kRing, 16, 16, QLatin1Char('0')).toUpper());
    o.insert(QStringLiteral("slot"), slot);
    o.insert(QStringLiteral("row"), row >= 0 && !pending ? QJsonValue(row) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("pending"), pending);
    o.insert(QStringLiteral("epoch"), pending ? 7 : 0);
    o.insert(QStringLiteral("rdio"), false);
    o.insert(QStringLiteral("failed"), false);
    o.insert(QStringLiteral("system"), QStringLiteral("County"));
    o.insert(QStringLiteral("target"), QStringLiteral("Fire"));
    o.insert(QStringLiteral("source"), QStringLiteral("1234"));
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray
link_line(const QString& id, qint64 row, int slot = 0) {
    return QStringLiteral(
               "{\"v\":1,\"type\":\"link\",\"t\":1,\"id\":\"%1\",\"instance\":\"%2\",\"slot\":%3,\"row\":%4}\n")
        .arg(id, QStringLiteral("%1").arg(kRing, 16, 16, QLatin1Char('0')).toUpper())
        .arg(slot)
        .arg(row)
        .toUtf8();
}

QString
fragment(qint64 row, int slot = 0) {
    return dsd_qt::call_history_fragment_key(kRing, slot, static_cast<qulonglong>(row));
}

/* One recording of row @p row listed, available or not as @p available says. */
bool
listed_as(const RecordingStore& store, qint64 row, bool available) {
    const QVariantList list = store.recordingsForFragments({fragment(row)});
    return list.size() == 1 && list.value(0).toMap().value(QStringLiteral("available")).toBool() == available;
}

/* A recording published into @p folder with its file's modification time set to @p mtime first: the file is whole,
 * time included, before its journal line can be read. */
void
record_at(const QString& folder, const QString& id, const QString& file, qint64 start, qint64 row, qint64 bytes,
          qint64 mtime) {
    make_file(folder + QLatin1Char('/') + file, bytes);
    set_mtime(folder + QLatin1Char('/') + file, mtime);
    append(journal_of(folder), publish_line(id, file, start, row));
}

/* A recording published into @p folder: the file and its journal line. */
void
record(const QString& folder, const QString& id, const QString& file, qint64 start, qint64 row, qint64 bytes = 1000) {
    make_file(folder + QLatin1Char('/') + file, bytes);
    append(journal_of(folder), publish_line(id, file, start, row));
}

QString
live_token() {
    char token[DSD_WAV_TEMP_TOKEN_TEXT_SIZE];
    (void)dsd_wav_temp_token_text(token, sizeof token);
    return QString::fromLatin1(token);
}

bool
spied(const QSignalSpy& spy, const QString& key) {
    for (const QList<QVariant>& args : spy) {
        if (args.at(0).toStringList().contains(key)) {
            return true;
        }
    }
    return false;
}

/* --- Tests. --- */

/* The tail reads complete lines only: a line still being written waits for its newline, a line a crash tore is
 * skipped once the next run's newline ends it, and records of another version or naming a file outside the folder
 * are ignored. */
void
test_tail_reads_complete_lines_only(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    expect("the session's folder is the owner's, in the first root",
           folder == roots.external + QStringLiteral("/County (1a2b3c4d)"));
    QSignalSpy news(&s.store, &RecordingStore::recordingFragmentsChanged);

    make_file(folder + QStringLiteral("/a.wav"), 1000);
    QByteArray line = publish_line(QStringLiteral("00000000000000AA-1"), QStringLiteral("a.wav"), 1754700000, 11);
    append(journal_of(folder), line.left(line.size() - 1));
    s.pass();
    expect("a line without its newline is not read yet",
           s.store.recordingCount() == 0 && !s.store.hasRecordingForFragments({fragment(11)}));
    append(journal_of(folder), "\n");
    s.pass();
    expect("the line is read once its newline lands",
           s.store.recordingCount() == 1 && s.store.hasRecordingForFragments({fragment(11)}));
    expect("the row's fragment is announced", spied(news, fragment(11)));
    const QVariantList recordings = s.store.recordingsForFragments({fragment(11)});
    const QVariantMap first = recordings.value(0).toMap();
    expect("the recording's fields",
           recordings.size() == 1 && first.value("id").toString() == QStringLiteral("00000000000000AA-1")
               && first.value("path").toString() == folder + QStringLiteral("/a.wav")
               && first.value("start").toLongLong() == 1754700000 && first.value("durationMs").toLongLong() == 4000
               && first.value("bytes").toLongLong() == 1000 && first.value("folderUid").toString() == kUidA
               && first.value("available").toBool());
    expect("its path is indexed", s.store.isIndexedPath(folder + QStringLiteral("/./a.wav")));

    // A crash tore a line; the next run starts a new line before its first record.
    append(journal_of(folder), "{\"v\":1,\"type\":\"publ");
    s.pass();
    make_file(folder + QStringLiteral("/b.wav"), 500);
    append(journal_of(folder),
           "\n" + publish_line(QStringLiteral("00000000000000BB-1"), QStringLiteral("b.wav"), 1754700100, 12));
    // Another version, and a file outside the folder, are not the store's.
    make_file(roots.external + QStringLiteral("/escape.wav"), 10);
    append(journal_of(folder),
           publish_line(QStringLiteral("00000000000000BB-2"), QStringLiteral("../escape.wav"), 1754700200, 13));
    QByteArray v2 = publish_line(QStringLiteral("00000000000000BB-3"), QStringLiteral("b.wav"), 1754700300, 14);
    v2.replace("\"v\":1", "\"v\":2");
    append(journal_of(folder), v2);
    s.pass();
    expect("the torn line is skipped and the next record read", s.store.hasRecordingForFragments({fragment(12)}));
    expect("a file outside the folder and another version are ignored",
           s.store.recordingCount() == 2 && !s.store.hasRecordingForFragments({fragment(13), fragment(14)}));
    expect("parser: no way out of the folder", [] {
        dsd_qt::RecordingJournalRecord r;
        for (const char* file : {"/abs.wav", "a/../b.wav", "C:x.wav", "a\\b.wav", "./a.wav", "a//b.wav"}) {
            if (dsd_qt::recording_journal_parse(
                    publish_line(QStringLiteral("X-1"), QString::fromLatin1(file), 1, 1).chopped(1), &r)) {
                return false;
            }
        }
        return dsd_qt::recording_journal_parse(
            publish_line(QStringLiteral("X-1"), QStringLiteral("sub/a.wav"), 1, 1).chopped(1), &r);
    }());
    s.store.endSession();
}

/* The decoder's own writer: a recording closed before its row commits is published pending, without a fragment, and
 * gets one with its `link`; one whose row never comes stays, unattributed; a committed row's recording has its
 * fragment at once; discard and error records index nothing. */
void
test_journal_from_the_writer(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    QSignalSpy news(&s.store, &RecordingStore::recordingFragmentsChanged);
    expect("the writer opens the folder's journal", dsd_wav_journal_open(qPrintable(journal_of(canonical))) == 0);

    const auto publish = [&canonical](const char* id, const char* name, dsd_wav_owner owner) {
        make_file(canonical + QLatin1Char('/') + QLatin1String(name), 2000);
        const QByteArray path = (canonical + QLatin1Char('/') + QLatin1String(name)).toUtf8();
        const QByteArray dir = canonical.toUtf8();
        wav_journal_publish_record rec;
        DSD_MEMSET(&rec, 0, sizeof rec);
        rec.id = id;
        rec.path = path.constData();
        rec.dir = dir.constData();
        rec.start = 1754701000;
        rec.duration_ms = 3000;
        rec.rate = 8000;
        rec.has_instance = 1;
        rec.instance = kRing;
        rec.owner = owner;
        rec.system = "S";
        rec.target = "T";
        rec.source = "R";
        wav_journal_publish(&rec);
    };
    dsd_wav_owner pending = {DSD_WAV_OWNER_PENDING, 1U, 0U, 70U};
    publish("00000000000000AA-1", "pending.wav", pending);
    dsd_wav_owner dropped = {DSD_WAV_OWNER_PENDING, 0U, 0U, 71U};
    publish("00000000000000AA-2", "dropped.wav", dropped);
    dsd_wav_owner row = {DSD_WAV_OWNER_ROW, 0U, 40U, 0U};
    publish("00000000000000AA-3", "row.wav", row);
    wav_journal_discard("00000000000000AA-4");
    wav_journal_error("00000000000000AA-5", "close", "TEMP_x", qPrintable(canonical));
    s.pass();
    expect("every published recording is indexed", s.store.recordingCount() == 3);
    expect("a committed row's recording has its fragment", s.store.hasRecordingForFragments({fragment(40)}));
    expect("a pending recording has none yet", s.store.recordingsForFragments({fragment(41, 1)}).isEmpty());

    news.clear();
    wav_journal_link_pending(1U, 70U, kRing, 41U);
    wav_journal_unlink_pending_epoch(0U, 71U);
    s.pass();
    expect("the link gives the pending recording its row", s.store.hasRecordingForFragments({fragment(41, 1)}));
    expect("and announces it", spied(news, fragment(41, 1)));
    expect("the unlinked one stays, unattributed", s.store.recordingCount() == 3);
    dsd_wav_journal_close();
    s.store.endSession();
}

/* A RingFixture's worth of event history for the model: rows committed the way push_event_history() does. */
struct Ring {
    dsd_state* state;
    Event_History_I* rings;

    Ring() {
        state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
        rings = static_cast<Event_History_I*>(calloc(2, sizeof(Event_History_I)));
        state->event_history_s = rings;
        for (int slot = 0; slot < 2; slot++) {
            rings[slot].revision = 1U;
            rings[slot].commit_rev = 1U;
            rings[slot].instance = kRing;
        }
    }

    ~Ring() {
        free(rings);
        free(state);
    }

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    void
    commit(uint32_t tg, uint32_t src, time_t start, time_t end) {
        Event_History_I* ring = &rings[0];
        DSD_MEMMOVE(&ring->Event_History_Items[2], &ring->Event_History_Items[1],
                    sizeof(Event_History) * (DSD_EVENT_HISTORY_LEN - 2));
        Event_History* item = &ring->Event_History_Items[1];
        DSD_MEMSET(item, 0, sizeof(*item));
        item->category = DSD_EVENT_CATEGORY_VOICE;
        item->target_id = tg;
        item->source_id = src;
        item->event_start_time = start;
        item->event_time = end;
        ring->push_seq++;
        ring->commit_rev++;
        ring->revision++;
    }
};

/* History's recording roles come from the store: a journal line read before its row exists shows when the row
 * arrives, a later link reaches the row it names, and a deleted recording leaves its row (and the disk). */
void
test_recording_roles_from_the_store(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    expect("an Explore session records into Explore", folder == roots.external + QStringLiteral("/Explore"));

    Ring ring;
    dsd_qt::CallHistoryModel model;
    model.setRecordingProvider(&s.store);
    QObject::connect(&s.store, &RecordingStore::recordingFragmentsChanged, &model,
                     &dsd_qt::CallHistoryModel::recordingFragmentsChanged);
    const time_t when = 1754703000;
    ring.commit(31001, 501, when, when + 4);
    model.refresh(ring.state);
    const qint64 firstRow = static_cast<qint64>(ring.rings[0].push_seq);

    // The recording of the next row is journaled before the History reads it.
    record(folder, QStringLiteral("00000000000000CC-1"), QStringLiteral("early.wav"), when + 60, firstRow + 1);
    s.pass();
    ring.commit(31002, 502, when + 60, when + 64);
    model.refresh(ring.state);
    const auto has = [&model](int row) {
        return model.data(model.index(row), dsd_qt::CallHistoryModel::HasRecordingRole).toBool();
    };
    expect("a recording journaled before its row shows when the row arrives",
           model.count() == 2 && has(0)
               && model.data(model.index(0), dsd_qt::CallHistoryModel::RecordingsRole).toList().size() == 1);
    expect("the older row has none", !has(1));

    // A recording that waited for its row is linked to the older one.
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    make_file(folder + QStringLiteral("/late.wav"), 800);
    append(journal_of(folder),
           publish_line(QStringLiteral("00000000000000CC-2"), QStringLiteral("late.wav"), when, -1, true));
    append(journal_of(folder), link_line(QStringLiteral("00000000000000CC-2"), firstRow));
    s.pass();
    expect("a later link reaches the row it names", has(1) && changed.count() >= 1);

    changed.clear();
    s.store.deleteRecording(QStringLiteral("00000000000000CC-1"));
    s.pass();
    expect("a deleted recording leaves its row", !has(0) && changed.count() >= 1);
    expect("and the disk", !QFileInfo::exists(folder + QStringLiteral("/early.wav")));
    model.setRecordingProvider(nullptr);
    s.store.endSession();
}

/* A journal is rewritten with its live entries once it holds more than twice as many lines, but never while a session
 * may be writing it; the tail then reads only what comes after the rewrite, and a new store reads the rewrite. */
void
test_compaction(void) {
    Roots roots;
    QString folder;
    {
        Store s(roots.list());
        folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        record(folder, QStringLiteral("00000000000000DD-1"), QStringLiteral("keep.wav"), 1754704000, 50);
        record(folder, QStringLiteral("00000000000000DD-2"), QStringLiteral("gone1.wav"), 1754704010, 51);
        record(folder, QStringLiteral("00000000000000DD-3"), QStringLiteral("gone2.wav"), 1754704020, 52);
        append(journal_of(folder), "{\"v\":1,\"type\":\"discard\",\"t\":1,\"id\":\"00000000000000DD-4\"}\n");
        make_file(folder + QStringLiteral("/linked.wav"), 1000);
        append(journal_of(folder),
               publish_line(QStringLiteral("00000000000000DD-5"), QStringLiteral("linked.wav"), 1754704030, -1, true));
        append(journal_of(folder), link_line(QStringLiteral("00000000000000DD-5"), 53));
        s.pass();
        s.store.deleteRecordings({QStringLiteral("00000000000000DD-2"), QStringLiteral("00000000000000DD-3")});
        s.pass();
        const QByteArray before = read_all(journal_of(folder));
        expect("6 lines, 2 live: but the folder is in use",
               before.count('\n') == 6 && s.store.compactionsForTest() == 0);

        s.store.endSession();
        s.pass();
        const QByteArray after = read_all(journal_of(folder));
        expect("the session over, the journal is compacted to its live entries",
               s.store.compactionsForTest() == 1 && after.count('\n') == 2);
        dsd_qt::RecordingJournalRecord rec;
        expect("each line is a plain publish with its row",
               dsd_qt::recording_journal_parse(after.split('\n').value(1), &rec) && rec.id.endsWith("-5") && rec.hasRow
                   && rec.row == 53U && !rec.pending && rec.file == QStringLiteral("linked.wav"));
        expect("the index is unchanged", s.store.recordingCount() == 2
                                             && s.store.hasRecordingForFragments({fragment(50)})
                                             && s.store.hasRecordingForFragments({fragment(53)}));

        // The next session appends after the rewrite; the tail reads that once.
        (void)s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        record(folder, QStringLiteral("00000000000000DD-6"), QStringLiteral("next.wav"), 1754704040, 54);
        s.pass();
        expect("the cursor restarted after the rewrite",
               s.store.recordingCount() == 3 && s.store.hasRecordingForFragments({fragment(54)}));
        s.store.endSession();
        s.pass();
        expect("nothing to compact", s.store.compactionsForTest() == 1);
    }
    {
        Store s(roots.list(), false);
        s.store.hostSessionReport(true, true, true); // a session no starter here began: its folder is not known
        s.start();
        expect("a new store reads the rewrite", s.store.recordingCount() == 3
                                                    && s.store.hasRecordingForFragments({fragment(50)})
                                                    && s.store.hasRecordingForFragments({fragment(53)})
                                                    && s.store.hasRecordingForFragments({fragment(54)}));
        // One request: a second right after would land while the pass the first woke reads storage, and be refused.
        expect("both deletes asked",
               s.store.deleteRecordings({QStringLiteral("00000000000000DD-6"), QStringLiteral("00000000000000DD-1")}));
        s.pass();
        expect("a session running when the store started holds every journal", s.store.compactionsForTest() == 0);
        s.store.endSession();
        s.pass();
        expect("a starter's endSession() does not free it", s.store.compactionsForTest() == 0);
        s.store.hostSessionReport(true, false, false);
        s.pass();
        expect("until it ends", s.store.compactionsForTest() == 1 && read_all(journal_of(folder)).count('\n') == 1);
    }
}

/* A journal whose bytes past the cursor fail to read is not settled: a compaction must not drop records it never saw. */
class FailingJournal : public QBuffer {
  public:
    FailingJournal(const QByteArray& bytes, qint64 failFrom) : m_failFrom(failFrom) {
        setData(bytes);
        (void)open(QIODevice::ReadOnly);
    }

  protected:
    qint64
    readData(char* data, qint64 maxSize) override {
        if (pos() + maxSize > m_failFrom) {
            return -1;
        }
        return QBuffer::readData(data, maxSize);
    }

  private:
    qint64 m_failFrom;
};

void
test_journal_tail_settled(void) {
    const QByteArray read = "{\"v\":1}\n";
    const QByteArray journal = read + "{\"v\":1,\"later\":1}\n";
    QBuffer whole;
    whole.setData(read + "{\"torn");
    (void)whole.open(QIODevice::ReadOnly);
    expect("a torn last line past the cursor is settled", dsd_qt::recording_journal_tail_settled(whole, read.size()));
    QBuffer unread;
    unread.setData(journal);
    (void)unread.open(QIODevice::ReadOnly);
    expect("a whole line past the cursor is not", !dsd_qt::recording_journal_tail_settled(unread, read.size()));
    FailingJournal failing(journal, read.size());
    expect("nor is a tail that cannot be read", !dsd_qt::recording_journal_tail_settled(failing, read.size()));
    QBuffer empty;
    (void)empty.open(QIODevice::ReadOnly);
    expect("an offset past the end is not", !dsd_qt::recording_journal_tail_settled(empty, 4));
}

/* A folder whose folder.json goes away after it was read is no longer the app's: an ordinary pass drops it from the
 * index, and its files stay where they are, beyond retention and deletes. */
void
test_folder_marker_gone_after_read(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    record(folder, QStringLiteral("00000000000000EE-1"), QStringLiteral("kept.wav"), 1754704000, 70);
    s.pass();
    s.store.endSession();
    s.pass();
    expect("the folder's recording is indexed",
           s.store.recordingCount() == 1 && s.store.hasRecordingForFragments({fragment(70)}));
    expect("its folder.json is removed", QFile::remove(folder + QStringLiteral("/folder.json")));
    s.pass();
    expect("the next ordinary pass drops it from the index",
           s.store.recordingCount() == 0 && !s.store.hasRecordingForFragments({fragment(70)}));
    s.store.deleteRecording(QStringLiteral("00000000000000EE-1"));
    s.pass();
    expect("and its file stays, beyond deletes", QFileInfo::exists(folder + QStringLiteral("/kept.wav")));
}

/* A session that never ran may still reach the decoder: Android's start grace can show a start Failed while its intent
 * waits on a busy main thread, and the engine then opens that folder's journal. Such a folder is compacted only after
 * the host has seen a session running and then inactive. */
void
test_compaction_waits_for_a_run(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    record(folder, QStringLiteral("00000000000000EE-1"), QStringLiteral("keep.wav"), 1754704000, 60);
    record(folder, QStringLiteral("00000000000000EE-2"), QStringLiteral("gone1.wav"), 1754704010, 61);
    record(folder, QStringLiteral("00000000000000EE-3"), QStringLiteral("gone2.wav"), 1754704020, 62);
    s.pass();
    s.store.deleteRecordings({QStringLiteral("00000000000000EE-2"), QStringLiteral("00000000000000EE-3")});
    s.pass();
    s.store.hostSessionReport(true, true, false); // Starting
    s.pass();
    s.store.hostSessionReport(true, false, false); // the start grace gave up: Failed
    s.pass();
    expect("a session that ended without running leaves its journal alone", s.store.compactionsForTest() == 0);
    s.store.hostSessionReport(true, true, false); // the late intent: Starting
    s.store.hostSessionReport(true, true, true);  // Running
    s.pass();
    expect("nor while the late session runs", s.store.compactionsForTest() == 0);
    s.store.hostSessionReport(true, false, false); // Idle
    s.pass();
    expect("once a session ran and ended, the folder is compacted",
           s.store.compactionsForTest() == 1 && read_all(journal_of(folder)).count('\n') == 1);

    // The next start fails before it runs too: its folder stays held, pass after pass.
    const QString again = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    record(again, QStringLiteral("00000000000000EE-4"), QStringLiteral("x.wav"), 1754704030, 63);
    record(again, QStringLiteral("00000000000000EE-5"), QStringLiteral("y.wav"), 1754704040, 64);
    s.pass();
    s.store.deleteRecordings({QStringLiteral("00000000000000EE-4"), QStringLiteral("00000000000000EE-5")});
    s.store.hostSessionReport(true, true, false);
    s.store.hostSessionReport(true, false, false);
    s.pass();
    expect("a failed start keeps the folder held across later passes", s.store.compactionsForTest() == 1);
    s.pass();
    expect("still held", s.store.compactionsForTest() == 1);
}

/* Storage that is away is not storage that was emptied: what an unavailable root held stays indexed and untouched,
 * a delete of it is refused at once (issue #902: nothing waits for storage to come back, where a read again could give
 * its path to another recording), and it is read again when it comes back; asked again then, the delete runs. A file missing with its root there leaves the
 * index, and a file the journal never named is never managed. */
void
test_roots_unavailable_vs_deleted(void) {
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    QString folder;
    {
        Store s(roots.list());
        folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        record(folder, QStringLiteral("00000000000000EE-1"), QStringLiteral("a.wav"), 1754705000, 60);
        record(folder, QStringLiteral("00000000000000EE-2"), QStringLiteral("b.wav"), 1754705010, 61);
        make_file(folder + QStringLiteral("/stray.wav"), 5000);
        s.store.endSession();
        s.pass();
        expect("both recordings, not the stray file", s.store.recordingCount() == 2 && s.store.usedBytes() == 2000);

        const QSignalSpy away_news(&s.store, &RecordingStore::recordingFragmentsChanged);
        expect("the storage goes away", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
        s.pass();
        const QVariantMap a = s.store.recordingsForFragments({fragment(60)}).value(0).toMap();
        expect("what it held stays indexed, marked away",
               s.store.recordingCount() == 2 && !a.isEmpty() && !a.value("available").toBool());
        expect("and the rows it belongs to are told, so a player shows the storage away",
               spied(away_news, fragment(60)) && spied(away_news, fragment(61)));
        expect("the other root is still there", s.store.available());
        expect("a delete of it is refused at once", !s.store.deleteRecording(QStringLiteral("00000000000000EE-1"))
                                                        && s.store.deleteRefusal() == QLatin1String("storageAway"));
        s.pass();
        expect("and nothing waits", s.store.deletesWaiting() == 0 && s.store.recordingCount() == 2);
        expect("an export leaves it out",
               s.store.selectForExport(QString(), QStringLiteral("all")).value("count").toInt() == 0);
        const QString moved = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        expect("a session meanwhile records in the other root",
               moved == roots.internal + QStringLiteral("/County (1a2b3c4d)"));
        s.store.endSession();

        expect("the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
        s.pass();
        expect("read again, nothing of it deleted",
               s.store.recordingCount() == 2 && QFileInfo::exists(folder + QStringLiteral("/a.wav"))
                   && s.store.hasRecordingForFragments({fragment(61)}) && s.store.deletesWaiting() == 0);
        expect("asked again, the delete runs", s.store.deleteRecording(QStringLiteral("00000000000000EE-1")));
        s.pass();
        expect("asked again, the recording gone",
               s.store.recordingCount() == 1 && !QFileInfo::exists(folder + QStringLiteral("/a.wav")));
        expect("one owner, two folders, one entry",
               s.store.folders().size() == 1 && s.store.folders().value(0).toMap().value("count").toInt() == 1);
    }
    QFile::remove(folder + QStringLiteral("/b.wav"));
    {
        Store s(roots.list());
        expect("a file missing with its root there leaves the index", s.store.recordingCount() == 0);
        s.store.deleteAll();
        s.pass();
        expect("a file the journal never named is never managed",
               QFileInfo::exists(folder + QStringLiteral("/stray.wav")) && s.store.usedBytes() == 0);
    }
    {
        // Every root away: the store says so. The fallback's parent is made at start, unless it cannot be (here a file
        // stands where a folder on its path would go).
        expect("both roots go away", QDir(roots.dir.path() + QStringLiteral("/ext")).removeRecursively()
                                         && QDir(roots.dir.path() + QStringLiteral("/int")).removeRecursively());
        make_file(roots.dir.path() + QStringLiteral("/int"), 1);
        Store s(roots.list());
        expect("no storage", !s.store.available() && s.store.ready());
        expect("no folder without storage",
               s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County")).isEmpty());
    }
}

/* A journal shorter than what the store read was replaced behind its back and is read again from the start; a folder
 * removed from a computer while its root is there takes its recordings out of the index. */
void
test_changes_behind_the_store(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000044-1"), QStringLiteral("a.wav"), 1754704500, 90);
    record(folder, QStringLiteral("0000000000000044-2"), QStringLiteral("b.wav"), 1754704510, 91);
    s.pass();
    expect("two recordings", s.store.recordingCount() == 2);
    QFile::remove(journal_of(folder));
    append(journal_of(folder),
           publish_line(QStringLiteral("0000000000000044-2"), QStringLiteral("b.wav"), 1754704510, 92));
    s.pass();
    expect("a shorter journal is read again from the start", s.store.recordingCount() == 1
                                                                 && s.store.hasRecordingForFragments({fragment(92)})
                                                                 && !s.store.hasRecordingForFragments({fragment(90)})
                                                                 && !s.store.hasRecordingForFragments({fragment(91)}));
    expect("the folder is gone", QDir(folder).removeRecursively());
    s.pass();
    expect("and with it its recordings", s.store.recordingCount() == 0 && s.store.folderPathsForTest().isEmpty());
}

/* Session starts on the GUI thread add and relabel folders while the worker publishes: every start wakes a pass, which
 * sums the folder table outside the lock. The worker sums a copy of its own, so a start never writes in place into a
 * table the worker read after letting the lock go (issue #902). Each copy a sum takes is checked to share nothing with
 * the table (the hook, under the lock: no scheduling luck); the starts racing the passes are what the tsan-debug build
 * reports when it does. The labels end as the last start of each owner left them. */
void
test_session_starts_while_the_worker_publishes(void) {
    constexpr int kOwners = 8;
    constexpr int kStarts = 240;
    Roots roots;
    Store s(roots.list(), false);
    std::atomic<int> copies{0};
    std::atomic<int> shared{0};
    s.store.setFolderCopyHookForTest([&copies, &shared](bool isShared) {
        copies++;
        shared += isShared ? 1 : 0;
    });
    s.store.setLimitMb(1000); // the limit's sum takes a copy too
    s.start();
    int refused = 0;
    for (int i = 0; i < kStarts; i++) {
        const QString uid = QStringLiteral("%1-0000-4000-8000-00000000000d").arg(0x7a000000 + i % kOwners, 8, 16);
        refused +=
            s.store.beginSession(QStringLiteral("saved"), uid, QStringLiteral("Owner %1").arg(i)).isEmpty() ? 1 : 0;
        s.store.endSession();
    }
    s.pass();
    expect("starts during publishes: every start took a folder", refused == 0);
    const QVariantList folders = s.store.folders();
    QStringList names;
    for (const QVariant& folder : folders) {
        names.append(folder.toMap().value(QStringLiteral("name")).toString());
    }
    std::sort(names.begin(), names.end());
    QStringList want;
    for (int i = kStarts - kOwners; i < kStarts; i++) {
        want.append(QStringLiteral("Owner %1").arg(i));
    }
    std::sort(want.begin(), want.end());
    expect("starts during publishes: one folder per owner, its last label", folders.size() == kOwners && names == want);
    expect("starts during publishes: every sum took a copy of its own", copies.load() > 0 && shared.load() == 0);
    s.store.setFolderCopyHookForTest({});
}

/* Folders are found by their owner's full uid: a rename keeps the folder (and updates its label), two owners whose
 * uids share eight characters get two folders, a folder the app did not make is never taken over, and names become
 * safe folder names. */
void
test_folders_follow_their_owner(void) {
    Roots roots;
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County / Fire: \"Ops\""));
    expect("a safe name with the short uid", a == roots.external + QStringLiteral("/County _ Fire_ _Ops_ (1a2b3c4d)"));
    QFile json(a + QStringLiteral("/folder.json"));
    const QJsonObject meta =
        json.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(json.readAll()).object() : QJsonObject();
    json.close();
    expect("folder.json names the owner",
           meta.value("uid").toString() == kUidA && meta.value("kind").toString() == QStringLiteral("saved")
               && meta.value("name").toString() == QStringLiteral("County / Fire: \"Ops\""));
    s.store.endSession();

    const QString renamed = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("Metro"));
    expect("a rename keeps the folder", renamed == a);
    s.store.endSession();
    s.pass();
    expect("and its label follows",
           s.store.folders().value(0).toMap().value("name").toString() == QStringLiteral("Metro"));

    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("County / Fire: \"Ops\""));
    expect("another owner with the same short uid gets its own folder", b == a + QStringLiteral(" 2"));
    s.store.endSession();

    QDir().mkpath(roots.external + QStringLiteral("/Nameless (99999999)"));
    const QString c =
        s.store.beginSession(QStringLiteral("scan"), QStringLiteral("99999999-aaaa"), QStringLiteral("Nameless"));
    expect("a folder the app did not make is passed over",
           c == roots.external + QStringLiteral("/Nameless (99999999) 2"));
    s.store.endSession();

    // A name holding "%1" keeps it: the folder's number is filled in once, not into the name.
    QDir().mkpath(roots.external + QStringLiteral("/Rate%1 (99999998)"));
    const QString percent =
        s.store.beginSession(QStringLiteral("scan"), QStringLiteral("99999998-bbbb"), QStringLiteral("Rate%1"));
    expect("a name with a placeholder is kept as written",
           percent == roots.external + QStringLiteral("/Rate%1 (99999998) 2"));
    s.store.endSession();

    const QString d =
        s.store.beginSession(QStringLiteral("scan"), QStringLiteral("77777777-bbbb"), QStringLiteral(" ..."));
    expect("an empty name falls back to the kind", d == roots.external + QStringLiteral("/Scan list (77777777)"));
    s.store.endSession();
    expect("explore and a map no one saved share Explore",
           s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("x"))
               == s.store.beginSession(QStringLiteral("saved"), QString(), QStringLiteral("y")));
    s.store.endSession();
    expect("long names are cut", dsd_qt::recording_sanitize_name(QString(80, QLatin1Char('n'))).size() == 48);
    expect("a cut never splits a character",
           !dsd_qt::recording_sanitize_name(QString(47, QLatin1Char('n')) + QString::fromUtf8("\xF0\x9F\x93\xBB"))
                .back()
                .isHighSurrogate());

    {
        Store again(roots.list());
        expect("a new store finds each owner by uid",
               again.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other")) == b);
        again.store.endSession();
    }
}

/* Deletion removes only indexed regular files inside a known root, re-checked right before the unlink; it waits for
 * the rdio uploader, the player and an export; and this process's temp files are never touched, while an earlier
 * process's are unfinished recordings the user may delete. */
void
test_deletion_safety(void) {
    Roots roots;
    const QString outside = roots.dir.path() + QStringLiteral("/outside");
    QDir().mkpath(outside);
    const QString live = QStringLiteral("TEMP_20261028_120000_%1_5").arg(live_token());
    const QString old = QStringLiteral("TEMP_20261027_120000_0123456789ABCDEF_9.wav");
    QString folder;
    {
        Store s(roots.list());
        folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        s.store.endSession();
    }
    // Temp files are found when a folder is read whole, at the store's start.
    make_file(folder + QLatin1Char('/') + live, 3000);
    make_file(folder + QLatin1Char('/') + old, 7000);
    for (int i = 1; i <= 6; i++) {
        record(folder, QStringLiteral("00000000000000FF-%1").arg(i), QStringLiteral("r%1.wav").arg(i), 1754706000 + i,
               70 + i);
    }
    Store t(roots.list());
    expect("six recordings; an earlier process's temp file is unfinished, this one's is not",
           t.store.recordingCount() == 6 && t.store.unfinishedCount() == 1 && t.store.unfinishedBytes() == 7000);

    // r1: a symlink in its place by the time it is deleted. r2: a directory. Both leave the index, the disk alone.
    make_file(outside + QStringLiteral("/target.wav"), 10);
    QFile::remove(folder + QStringLiteral("/r1.wav"));
    expect("symlink made", QFile::link(outside + QStringLiteral("/target.wav"), folder + QStringLiteral("/r1.wav")));
    QFile::remove(folder + QStringLiteral("/r2.wav"));
    QDir().mkdir(folder + QStringLiteral("/r2.wav"));
    // r3: held by the uploader; r4: playing; r5: selected for an export.
    t.uploading.insert(QStringLiteral("r3.wav"));
    t.uploadingSet = true;
    t.store.setPlayingPath(folder + QStringLiteral("/r4.wav"));
    const QVariantMap selection = t.store.selectRecordings({QStringLiteral("00000000000000FF-5")});
    expect("the selection holds r5",
           selection.value("count").toInt() == 1
               && selection.value("names").toStringList().value(0) == QStringLiteral("County (1a2b3c4d)/r5.wav"));
    t.store.deleteAll();
    t.store.deleteRecording(QStringLiteral("not-a-recording"));
    t.pass();
    expect("a symlink is never followed or removed", QFileInfo(folder + QStringLiteral("/r1.wav")).isSymLink()
                                                         && QFileInfo::exists(outside + QStringLiteral("/target.wav")));
    expect("a directory is left alone", QFileInfo(folder + QStringLiteral("/r2.wav")).isDir());
    expect("held files wait", QFileInfo::exists(folder + QStringLiteral("/r3.wav"))
                                  && QFileInfo::exists(folder + QStringLiteral("/r4.wav"))
                                  && QFileInfo::exists(folder + QStringLiteral("/r5.wav"))
                                  && t.store.deletesWaiting() == 3);
    expect("the rest is gone", !QFileInfo::exists(folder + QStringLiteral("/r6.wav"))
                                   && !QFileInfo::exists(folder + QLatin1Char('/') + old));
    expect("this process's temp file is never touched", QFileInfo::exists(folder + QLatin1Char('/') + live));
    expect("only the held ones are left", t.store.recordingCount() == 3 && t.store.unfinishedCount() == 0);

    t.uploadingSet = false;
    t.pass();
    expect("the upload over, its delete is done", !QFileInfo::exists(folder + QStringLiteral("/r3.wav")));
    t.store.setPlayingPath(QString());
    t.pass();
    expect("playback over, its delete is done", !QFileInfo::exists(folder + QStringLiteral("/r4.wav")));
    t.store.releaseSelection(selection.value("selection").toInt());
    t.pass();
    expect("the export over, its delete is done", !QFileInfo::exists(folder + QStringLiteral("/r5.wav"))
                                                      && t.store.deletesWaiting() == 0
                                                      && t.store.recordingCount() == 0);
    expect("an unknown path is not indexed", !t.store.isIndexedPath(folder + QStringLiteral("/r6.wav")));

    // A folder swapped for a link to somewhere else: its files are outside every root now.
    record(folder, QStringLiteral("00000000000000FF-7"), QStringLiteral("r7.wav"), 1754706007, 77);
    t.pass();
    const QString elsewhere = outside + QStringLiteral("/moved");
    expect("folder moved out", QDir().rename(folder, elsewhere) && QFile::link(elsewhere, folder));
    t.store.deleteRecording(QStringLiteral("00000000000000FF-7"));
    t.pass();
    expect("a file reached through a link out of the root is not deleted",
           QFileInfo::exists(elsewhere + QStringLiteral("/r7.wav")) && t.store.recordingCount() == 0);
    QDir().mkpath(roots.internal);
    QDir().mkpath(roots.internal + QStringLiteral("2")); // a sibling whose name starts with the root's
    expect(
        "inside a root, component by component",
        dsd_qt::recording_path_inside_root(roots.internal + QStringLiteral("/../Call recordings/x.wav"), roots.internal)
            && !dsd_qt::recording_path_inside_root(roots.internal, roots.internal)
            && !dsd_qt::recording_path_inside_root(roots.internal + QStringLiteral("2/x.wav"), roots.internal)
            && !dsd_qt::recording_path_inside_root(roots.internal + QStringLiteral("/.."), roots.internal));
}

/* The storage limit removes the oldest recordings first (by journaled start, an unfinished one by its file's time)
 * until the total is at 95% of the limit, skips held files, and leaves a total between 95% and 100% alone. */
void
test_retention(void) {
    Roots roots;
    QString folder;
    {
        Store s(roots.list());
        folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        s.store.endSession();
        s.pass();
    }
    // An earlier process left an unfinished recording, older than every published one.
    const QString unfinished = folder + QStringLiteral("/TEMP_20250101_000000_0123456789ABCDEF_1");
    make_file(unfinished, 100000);
    {
        QFile f(unfinished);
        (void)f.open(QIODevice::ReadWrite);
        (void)f.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000), QFileDevice::FileModificationTime);
    }
    // Eleven published recordings of 100 kB, journaled out of start order.
    const int order[] = {5, 2, 9, 0, 7, 3, 10, 1, 8, 4, 6};
    for (const int i : order) {
        record(folder, QStringLiteral("0000000000000011-%1").arg(i), QStringLiteral("c%1.wav").arg(i),
               1754707000 + i * 10, 100 + i, 100000);
    }
    Store s(roots.list(), false);
    s.store.setLimitMb(0);
    s.start();
    expect("no limit, nothing removed", s.store.usedBytes() == 1200000 && s.store.removedByLimit() == 0);

    // The oldest published one is selected for an export: the limit passes it over.
    const QVariantMap held = s.store.selectRecordings({QStringLiteral("0000000000000011-0")});
    QSignalSpy reached(&s.store, &RecordingStore::limitReached);
    s.store.setLimitMb(1);
    s.pass();
    expect("over 1 MB: the oldest go until 95%",
           !QFileInfo::exists(unfinished) && QFileInfo::exists(folder + QStringLiteral("/c0.wav"))
               && !QFileInfo::exists(folder + QStringLiteral("/c1.wav"))
               && !QFileInfo::exists(folder + QStringLiteral("/c2.wav"))
               && QFileInfo::exists(folder + QStringLiteral("/c3.wav")) && s.store.usedBytes() == 900000);
    expect("and say so", s.store.removedByLimit() == 3 && reached.count() == 1 && reached.at(0).at(0).toInt() == 3);
    // Seen on the Call recordings screen, the count starts again; a second look changes nothing.
    QSignalSpy noticed(&s.store, &RecordingStore::changed);
    s.store.acknowledgeNotices();
    expect("acknowledged, nothing removed is shown", s.store.removedByLimit() == 0 && noticed.count() == 1);
    s.store.acknowledgeNotices();
    expect("acknowledged twice, no change", noticed.count() == 1);

    record(folder, QStringLiteral("0000000000000011-20"), QStringLiteral("small.wav"), 1754708000, 200, 40000);
    s.pass();
    expect("between 95% and the limit nothing goes", s.store.usedBytes() == 940000 && s.store.removedByLimit() == 0);
    record(folder, QStringLiteral("0000000000000011-21"), QStringLiteral("more.wav"), 1754708010, 201, 70000);
    s.pass();
    expect("past the limit again, after a publish, the next oldest goes",
           s.store.usedBytes() == 910000 && !QFileInfo::exists(folder + QStringLiteral("/c3.wav"))
               && QFileInfo::exists(folder + QStringLiteral("/c0.wav")) && s.store.removedByLimit() == 1
               && reached.count() == 2);
    s.store.releaseSelection(held.value("selection").toInt());
}

/* Free space under 200 MB where recordings go is a warning, read at start and after each publish. */
void
test_free_space_warning(void) {
    Roots roots;
    Store s(roots.list(), false);
    s.freeBytes = 150 * RecordingStore::kBytesPerMb;
    s.start();
    expect("low space is reported", s.store.lowSpace() && s.store.freeBytes() == 150 * RecordingStore::kBytesPerMb);
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.freeBytes = 900 * RecordingStore::kBytesPerMb;
    record(folder, QStringLiteral("0000000000000022-1"), QStringLiteral("a.wav"), 1754709000, 300);
    s.pass();
    expect("and cleared after a publish", !s.store.lowSpace());
    s.store.endSession();
}

/* An export selects finished recordings by owner and date preset, oldest first, and holds them until released. */
void
test_export_selection(void) {
    QDate first;
    QDate last;
    const QDate today(2026, 10, 28);
    // Every preset but "all" ends with today: a recording dated later (a clock that ran ahead) is not "today".
    expect("today", dsd_qt::recording_export_days(QStringLiteral("today"), today, &first, &last) && first == today
                        && last == today);
    expect("yesterday", dsd_qt::recording_export_days(QStringLiteral("yesterday"), today, &first, &last)
                            && first == today.addDays(-1) && last == today.addDays(-1));
    expect("last 7 days", dsd_qt::recording_export_days(QStringLiteral("last7"), today, &first, &last)
                              && first == today.addDays(-6) && last == today);
    expect("last 30 days", dsd_qt::recording_export_days(QStringLiteral("last30"), today, &first, &last)
                               && first == today.addDays(-29) && last == today);
    expect("all", dsd_qt::recording_export_days(QStringLiteral("all"), today, &first, &last) && first.isNull()
                      && last.isNull());
    expect("nothing else", !dsd_qt::recording_export_days(QStringLiteral("week"), today, &first, &last));
    expect("no day, no range", !dsd_qt::recording_export_days(QStringLiteral("today"), QDate(), &first, &last));
    // Fixed offsets, which every host has: the window's dates are the given zone's, whatever the process's zone is.
    for (const int offset : {-12 * 3600, 0, 13 * 3600 + 45 * 60, 14 * 3600}) {
        const QTimeZone zone(offset);
        const auto at = [&zone](const QDate& day, const QTime& time) {
            return QDateTime(day, time, zone).toSecsSinceEpoch();
        };
        dsd_qt::RecordingExportWindow window;
        expect("window: today", dsd_qt::recording_export_window(QStringLiteral("today"), today, zone, &window));
        expect("covers: today's first and last second",
               dsd_qt::recording_export_covers(window, at(today, QTime(0, 0)))
                   && dsd_qt::recording_export_covers(window, at(today, QTime(23, 59, 59))));
        expect("covers: not the seconds either side",
               !dsd_qt::recording_export_covers(window, at(today, QTime(0, 0)) - 1)
                   && !dsd_qt::recording_export_covers(window, at(today.addDays(1), QTime(0, 0))));
        expect("window: last 30", dsd_qt::recording_export_window(QStringLiteral("last30"), today, zone, &window));
        expect("covers: last 30's edges and middle",
               dsd_qt::recording_export_covers(window, at(today.addDays(-29), QTime(0, 0)))
                   && !dsd_qt::recording_export_covers(window, at(today.addDays(-29), QTime(0, 0)) - 1)
                   && dsd_qt::recording_export_covers(window, at(today.addDays(-15), QTime(12, 0)))
                   && dsd_qt::recording_export_covers(window, at(today, QTime(23, 59, 59)))
                   && !dsd_qt::recording_export_covers(window, at(today.addDays(1), QTime(0, 0))));
        expect("window: instants resolved at the press",
               window.from == at(today.addDays(-29), QTime(0, 0)) && window.to == at(today.addDays(1), QTime(0, 0)));
    }
    dsd_qt::RecordingExportWindow all;
    expect("covers: all, every start", dsd_qt::recording_export_window(QStringLiteral("all"), today, QTimeZone(0), &all)
                                           && dsd_qt::recording_export_covers(all, 0)
                                           && dsd_qt::recording_export_covers(all, 1754000000));
    expect("window: no zone, no window",
           !dsd_qt::recording_export_window(QStringLiteral("today"), today, QTimeZone(), &all));

    Roots roots;
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
    s.store.endSession();
    const qint64 now = QDateTime(today, QTime(12, 0)).toSecsSinceEpoch();
    record(a, QStringLiteral("0000000000000033-1"), QStringLiteral("a2.wav"), now - 100, 400, 300);
    record(a, QStringLiteral("0000000000000033-2"), QStringLiteral("a1.wav"), now - 200, 401, 200);
    record(b, QStringLiteral("0000000000000033-3"), QStringLiteral("b1.wav"), now - 300, 402, 100);
    s.pass();
    const QVariantMap mine = s.store.selectForExport(kUidA, QStringLiteral("all"));
    expect("one owner's, oldest first", mine.value("count").toInt() == 2 && mine.value("bytes").toLongLong() == 500
                                            && mine.value("names").toStringList()
                                                   == QStringList({QStringLiteral("County (1a2b3c4d)/a1.wav"),
                                                                   QStringLiteral("County (1a2b3c4d)/a2.wav")}));
    const QVariantMap every = s.store.selectForExport(QString(), QStringLiteral("all"));
    expect("every owner's", every.value("count").toInt() == 3 && every.value("selection").toInt() > 0);
    expect("an unknown preset selects nothing",
           s.store.selectForExport(QString(), QStringLiteral("week")).value("selection").toInt() == 0);
    s.store.deleteFolder(kUidA);
    s.pass();
    expect("selected files wait", QFileInfo::exists(a + QStringLiteral("/a1.wav")) && s.store.deletesWaiting() == 2);
    s.store.releaseSelection(mine.value("selection").toInt());
    s.pass();
    expect("still held by the other export", QFileInfo::exists(a + QStringLiteral("/a1.wav")));
    s.store.releaseSelection(every.value("selection").toInt());
    s.pass();
    expect("released, the owner's recordings go and the other's stay",
           !QFileInfo::exists(a + QStringLiteral("/a1.wav")) && QFileInfo::exists(b + QStringLiteral("/b1.wav"))
               && s.store.recordingCount() == 1);
}

/* What an export of @p preset over every owner selects on @p today in @p zone: its zip names, its hold released at
 * once; its day in @p day. */
QStringList
export_names(RecordingStore& store, const QString& preset, const QDate& today, const QTimeZone& zone,
             QString* day = nullptr) {
    const QVariantMap selection = store.selectForExportOn(QString(), preset, today, zone);
    store.releaseSelection(selection.value(QStringLiteral("selection")).toInt());
    if (day != nullptr) {
        *day = selection.value(QStringLiteral("day")).toString();
    }
    return selection.value(QStringLiteral("names")).toStringList();
}

/* One recording per (file, start) in a fresh Explore folder. */
void
seed_dated(Store& s, const QList<QPair<QString, qint64>>& recordings) {
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    s.store.endSession();
    int n = 0;
    for (const auto& recording : recordings) {
        n++;
        record(folder, QStringLiteral("0000000000000201-%1").arg(n), recording.first, recording.second, n);
    }
    s.pass();
}

QStringList
explore_names(const QStringList& files) {
    QStringList names;
    for (const QString& file : files) {
        names.append(QStringLiteral("Explore/") + file);
    }
    return names;
}

/* Issue #902 B6: a preset's days are resolved once, at the press, to the instants they span in the press's zone.
 * Pacific/Apia skipped 2011-12-30 (23:59:59 UTC-10 on the 29th, then 00:00 UTC+14 on the 31st): that date has no
 * midnight of its own, so it spans no instant, where an unchecked startOfDay() bound of 0 once made Yesterday on the
 * 31st take every older recording. The zone is given, as the press samples it; a host without its history skips the
 * case. */
void
test_export_presets_on_a_skipped_date(void) {
    const QTimeZone apia(QByteArrayLiteral("Pacific/Apia"));
    if (!apia.isValid() || apia.offsetFromUtc(QDateTime::fromSecsSinceEpoch(1325239140, QTimeZone::UTC)) != -10 * 3600
        || apia.offsetFromUtc(QDateTime::fromSecsSinceEpoch(1325239200, QTimeZone::UTC)) != 14 * 3600) {
        DSD_FPRINTF(stdout,
                    "SKIP export presets: Pacific/Apia's 2011 date-line change is not available on this host\n");
        return;
    }
    const QDate today(2011, 12, 31);
    Roots roots;
    Store s(roots.list());
    seed_dated(s, {{QStringLiteral("dec01.wav"), 1322776800},      // 2011-12-01 12:00 -10
                   {QStringLiteral("dec24.wav"), 1324764000},      // 2011-12-24 12:00 -10
                   {QStringLiteral("dec25.wav"), 1324850400},      // 2011-12-25 12:00 -10
                   {QStringLiteral("dec29.wav"), 1325196000},      // 2011-12-29 12:00 -10
                   {QStringLiteral("dec29-end.wav"), 1325239140},  // 2011-12-29 23:59 -10
                   {QStringLiteral("dec31.wav"), 1325239200},      // 2011-12-31 00:00 +14
                   {QStringLiteral("dec31-noon.wav"), 1325282400}, // 2011-12-31 12:00 +14
                   {QStringLiteral("jan01.wav"), 1325368800}});    // 2012-01-01 12:00 +14, a clock ahead
    QString day;
    expect("apia: today", export_names(s.store, QStringLiteral("today"), today, apia, &day)
                                  == explore_names({QStringLiteral("dec31.wav"), QStringLiteral("dec31-noon.wav")})
                              && day == QStringLiteral("2011-12-31"));
    dsd_qt::RecordingExportWindow skipped;
    expect("apia: a skipped day spans no instant",
           dsd_qt::recording_export_window(QStringLiteral("yesterday"), today, apia, &skipped)
               && skipped.from == skipped.to && skipped.from == 1325239200);
    expect("apia: yesterday was skipped, so it has none",
           export_names(s.store, QStringLiteral("yesterday"), today, apia, &day).isEmpty()
               && day == QStringLiteral("2011-12-31"));
    expect("apia: last 7 days", export_names(s.store, QStringLiteral("last7"), today, apia)
                                    == explore_names({QStringLiteral("dec25.wav"), QStringLiteral("dec29.wav"),
                                                      QStringLiteral("dec29-end.wav"), QStringLiteral("dec31.wav"),
                                                      QStringLiteral("dec31-noon.wav")}));
    expect("apia: last 30 days", export_names(s.store, QStringLiteral("last30"), today, apia)
                                     == explore_names({QStringLiteral("dec24.wav"), QStringLiteral("dec25.wav"),
                                                       QStringLiteral("dec29.wav"), QStringLiteral("dec29-end.wav"),
                                                       QStringLiteral("dec31.wav"), QStringLiteral("dec31-noon.wav")}));
    expect("apia: all", export_names(s.store, QStringLiteral("all"), today, apia).size() == 8);
    expect("apia: an unknown preset still names its day",
           s.store.selectForExportOn(QString(), QStringLiteral("week"), today, apia)
                   .value(QStringLiteral("day"))
                   .toString()
               == QStringLiteral("2011-12-31"));
}

/* Issue #902 B6: on a 25-hour day (America/Los_Angeles 2026-11-01, 01:00-02:00 twice) every recording of that date is
 * Yesterday's on the 2nd, both 01:30s and the last half hour included, and nothing of the days either side. */
void
test_export_presets_on_a_long_day(void) {
    const QTimeZone angeles(QByteArrayLiteral("America/Los_Angeles"));
    if (!angeles.isValid()
        || QDate(2026, 11, 1).startOfDay(angeles).secsTo(QDate(2026, 11, 2).startOfDay(angeles)) != qint64{25} * 3600) {
        DSD_FPRINTF(stdout, "SKIP export presets: America/Los_Angeles's 2026 rules are not available on this host\n");
        return;
    }
    const QDate today(2026, 11, 2);
    Roots roots;
    Store s(roots.list());
    seed_dated(s, {{QStringLiteral("oct26-late.wav"), 1793082600}, // 2026-10-26 23:30 -7
                   {QStringLiteral("oct27.wav"), 1793086200},      // 2026-10-27 00:30 -7
                   {QStringLiteral("oct31-late.wav"), 1793514600}, // 2026-10-31 23:30 -7
                   {QStringLiteral("nov01.wav"), 1793518200},      // 2026-11-01 00:30 -7
                   {QStringLiteral("nov01-pdt.wav"), 1793521800},  // 2026-11-01 01:30 -7
                   {QStringLiteral("nov01-pst.wav"), 1793525400},  // 2026-11-01 01:30 -8
                   {QStringLiteral("nov01-late.wav"), 1793604600}, // 2026-11-01 23:30 -8
                   {QStringLiteral("nov02.wav"), 1793608200}});    // 2026-11-02 00:30 -8
    QString day;
    expect("long day: today", export_names(s.store, QStringLiteral("today"), today, angeles, &day)
                                      == explore_names({QStringLiteral("nov02.wav")})
                                  && day == QStringLiteral("2026-11-02"));
    expect("long day: yesterday, all 25 hours of it",
           export_names(s.store, QStringLiteral("yesterday"), today, angeles)
               == explore_names({QStringLiteral("nov01.wav"), QStringLiteral("nov01-pdt.wav"),
                                 QStringLiteral("nov01-pst.wav"), QStringLiteral("nov01-late.wav")}));
    expect("long day: last 7 days",
           export_names(s.store, QStringLiteral("last7"), today, angeles)
               == explore_names({QStringLiteral("oct27.wav"), QStringLiteral("oct31-late.wav"),
                                 QStringLiteral("nov01.wav"), QStringLiteral("nov01-pdt.wav"),
                                 QStringLiteral("nov01-pst.wav"), QStringLiteral("nov01-late.wav"),
                                 QStringLiteral("nov02.wav")}));
    expect("long day: last 30 days", export_names(s.store, QStringLiteral("last30"), today, angeles).size() == 8);
}

/* Issue #902 B6: the window's day boundaries come from the zone the press sampled, never the process's zone: one
 * start is Today in UTC+14 and Yesterday in UTC-12, whatever zone the host runs in. */
void
test_export_dates_follow_the_given_zone(void) {
    Roots roots;
    Store s(roots.list());
    const qint64 start = QDateTime(QDate(2026, 11, 21), QTime(13, 0), QTimeZone::UTC).toSecsSinceEpoch();
    seed_dated(s, {{QStringLiteral("one.wav"), start}});
    const QDate today(2026, 11, 22);
    expect("zone: today in UTC+14",
           export_names(s.store, QStringLiteral("today"), today, QTimeZone(14 * 3600)).size() == 1);
    expect("zone: yesterday, not today, in UTC-12",
           export_names(s.store, QStringLiteral("today"), today, QTimeZone(-12 * 3600)).isEmpty()
               && export_names(s.store, QStringLiteral("yesterday"), today, QTimeZone(-12 * 3600)).size() == 1);
    const QVariantMap pressed = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(pressed.value(QStringLiteral("selection")).toInt());
    expect("zone: the press names its own day",
           QDate::fromString(pressed.value(QStringLiteral("day")).toString(), QStringLiteral("yyyy-MM-dd")).isValid());
}

/* Issue #902 B6: a press reads its dates in local time as the C library keeps it. For a POSIX TZ string, TZ=UTC+09
 * means nine hours behind UTC, and so the press zone is; QTimeZone::systemTimeZone() names a zone nine hours ahead
 * there. A host whose runtime does not take the string skips the case. */
void
test_export_press_zone_is_local_time(void) {
    const bool had = qEnvironmentVariableIsSet("TZ");
    const QByteArray previous = qgetenv("TZ");
    const auto apply = []() {
#ifdef _WIN32
        _tzset();
#else
        tzset();
#endif
    };
    qputenv("TZ", "UTC+09");
    apply();
    const QDateTime noon(QDate(2026, 11, 21), QTime(12, 0), QTimeZone::UTC);
    const QTimeZone zone = dsd_qt::recording_export_press_zone();
    if (noon.toTimeZone(QTimeZone(QTimeZone::LocalTime)).offsetFromUtc() != -9 * 3600) {
        DSD_FPRINTF(stdout, "SKIP press zone: this host does not take the POSIX TZ string UTC+09\n");
    } else {
        expect("press zone: nine hours behind UTC", noon.toTimeZone(zone).offsetFromUtc() == -9 * 3600);
        // 00:30 UTC on the 21st is the 20th here: yesterday at a noon press, not today.
        const qint64 early = QDateTime(QDate(2026, 11, 21), QTime(0, 30), QTimeZone::UTC).toSecsSinceEpoch();
        dsd_qt::RecordingExportWindow window;
        const QDate today = noon.toTimeZone(zone).date();
        expect("press zone: the day", today == QDate(2026, 11, 21));
        expect("press zone: not today", dsd_qt::recording_export_window(QStringLiteral("today"), today, zone, &window)
                                            && !dsd_qt::recording_export_covers(window, early));
        expect("press zone: yesterday",
               dsd_qt::recording_export_window(QStringLiteral("yesterday"), today, zone, &window)
                   && dsd_qt::recording_export_covers(window, early));
    }
    // The press itself, at an instant whose local date is a day ahead of UTC's (TZ=UTC-09: nine hours ahead): 20:00
    // UTC on the 21st is 05:00 on the 22nd here. Today is the 22nd, named so, and takes the recording made at 01:00
    // here (16:00 UTC on the 21st), not the one at 23:00 here on the 21st (14:00 UTC on the 21st), as a UTC reading
    // of today would.
    qputenv("TZ", "UTC-09");
    apply();
    const QDateTime press(QDate(2026, 11, 21), QTime(20, 0), QTimeZone::UTC);
    if (press.toTimeZone(QTimeZone(QTimeZone::LocalTime)).offsetFromUtc() != 9 * 3600) {
        DSD_FPRINTF(stdout, "SKIP press day: this host does not take the POSIX TZ string UTC-09\n");
    } else {
        Roots roots;
        Store s(roots.list());
        const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        s.store.endSession();
        const qint64 earlyHere = QDateTime(QDate(2026, 11, 21), QTime(16, 0), QTimeZone::UTC).toSecsSinceEpoch();
        const qint64 lateYesterday = QDateTime(QDate(2026, 11, 21), QTime(14, 0), QTimeZone::UTC).toSecsSinceEpoch();
        record(folder, QStringLiteral("0000000000000801-1"), QStringLiteral("today.wav"), earlyHere, 1);
        record(folder, QStringLiteral("0000000000000801-2"), QStringLiteral("yesterday.wav"), lateYesterday, 2);
        s.pass();
        const QVariantMap today =
            s.store.selectForExportAt(QString(), QStringLiteral("today"), press.toMSecsSinceEpoch());
        s.store.releaseSelection(today.value(QStringLiteral("selection")).toInt());
        expect("press day: today is the local date",
               today.value(QStringLiteral("day")).toString() == QLatin1String("2026-11-22"));
        expect("press day: today takes the local day's recording",
               today.value(QStringLiteral("names")).toStringList()
                   == QStringList{QFileInfo(folder).fileName() + QStringLiteral("/today.wav")});
        const QVariantMap yesterday =
            s.store.selectForExportAt(QString(), QStringLiteral("yesterday"), press.toMSecsSinceEpoch());
        s.store.releaseSelection(yesterday.value(QStringLiteral("selection")).toInt());
        expect("press day: yesterday takes the day before's",
               yesterday.value(QStringLiteral("names")).toStringList()
                   == QStringList{QFileInfo(folder).fileName() + QStringLiteral("/yesterday.wav")});
    }
    if (had) {
        qputenv("TZ", previous);
    } else {
        qunsetenv("TZ");
    }
    apply();
}

/* A host whose session state arrives by polling, as Android's does: Idle and unknown until its first poll. */
class PolledHost : public dsd_qt::DecoderHost {
  public:
    bool
    isRunning() const override {
        return m_phase == Running;
    }

    QString
    statusText() const override {
        return {};
    }

    SessionState
    sessionState() const override {
        return m_phase;
    }

    bool
    sessionStateKnown() const override {
        return m_known;
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return true;
    }

    void
    stop() override {}

    /* One poll, in Android's order: the phase is published first, then the state turns known. */
    void
    poll(SessionState phase) {
        if (phase != m_phase) {
            m_phase = phase;
            Q_EMIT sessionStateChanged();
        }
        if (!m_known) {
            m_known = true;
            Q_EMIT sessionStateKnownChanged();
        }
    }

  private:
    SessionState m_phase = Idle;
    bool m_known = false;
};

/* A folder the app made, with one live recording and three more lines: more than twice its live entries. */
QString
compactable_folder(const Roots& roots) {
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    QDir().mkpath(folder);
    if (!write_private(folder + QStringLiteral("/folder.json"),
                       QStringLiteral("{\"uid\":\"%1\",\"kind\":\"saved\",\"name\":\"County\"}\n").arg(kUidA).toUtf8(),
                       "wb")) {
        g_failures++;
    }
    record(folder, QStringLiteral("0000000000000055-1"), QStringLiteral("live.wav"), 1754710000, 500);
    for (int i = 2; i <= 4; i++) {
        append(
            journal_of(folder),
            QStringLiteral("{\"v\":1,\"type\":\"discard\",\"t\":1,\"id\":\"0000000000000055-%1\"}\n").arg(i).toUtf8());
    }
    return folder;
}

/* Issue #708, C1: on Android the host does not know its session state until its first poll of the decoder service, so a
 * session that kept running while the UI was recreated reads as Idle when the store starts. Every journal is held until
 * the host knows; a running session nobody here began holds them until it ends; a host that knows it is idle frees
 * them at once. */
void
test_session_running_at_ui_load(void) {
    {
        Roots roots;
        const QString folder = compactable_folder(roots);
        PolledHost host;
        RecordingStore store;
        store.setRoots(roots.list());
        store.setTimingForTest(3600000, 3600000);
        dsd_qt::recording_store_follow_host(&store, &host);
        store.start();
        store.waitForPassForTest();
        expect("a journal is held while the host does not know its state", store.compactionsForTest() == 0);
        host.poll(dsd_qt::DecoderHost::Running);
        store.waitForPassForTest();
        expect("a session running when the UI loaded holds every journal",
               store.compactionsForTest() == 0 && read_all(journal_of(folder)).count('\n') == 4);
        host.poll(dsd_qt::DecoderHost::Stopping);
        store.waitForPassForTest();
        expect("and while it winds down", store.compactionsForTest() == 0);
        host.poll(dsd_qt::DecoderHost::Idle);
        store.waitForPassForTest();
        expect("until it ends", store.compactionsForTest() == 1 && read_all(journal_of(folder)).count('\n') == 1);
    }
    {
        Roots roots;
        (void)compactable_folder(roots);
        PolledHost host;
        RecordingStore store;
        store.setRoots(roots.list());
        store.setTimingForTest(3600000, 3600000);
        dsd_qt::recording_store_follow_host(&store, &host);
        store.start();
        host.poll(dsd_qt::DecoderHost::Idle);
        store.waitForPassForTest();
        expect("a first poll that finds no session frees the journals", store.compactionsForTest() == 1);
    }
    {
        // A start: the starter marks the folder, the host goes Starting, Running, then Idle.
        Roots roots;
        const QString folder = compactable_folder(roots);
        PolledHost host;
        host.poll(dsd_qt::DecoderHost::Idle);
        // The first read waits for the start, so the folder is marked before any compaction could look at it. Declared
        // before the store, so it outlives the worker.
        std::atomic<bool> begun{false};
        RecordingStore store;
        store.setRoots(roots.list());
        store.setTimingForTest(3600000, 3600000);
        store.setFolderLoadHookForTest([&begun](const QString&) {
            while (!begun.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        dsd_qt::recording_store_follow_host(&store, &host);
        store.start();
        expect("the starter's folder",
               store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County")) == folder);
        begun = true;
        store.waitForPassForTest();
        const int before = store.compactionsForTest();
        expect("held from the start", before == 0);
        host.poll(dsd_qt::DecoderHost::Idle); // a start clearing an old failure reports Idle again: nothing changes
        host.poll(dsd_qt::DecoderHost::Starting);
        host.poll(dsd_qt::DecoderHost::Running);
        store.waitForPassForTest();
        expect("the session's folder is held", store.compactionsForTest() == before);
        host.poll(dsd_qt::DecoderHost::Idle);
        store.waitForPassForTest();
        expect("and freed when it ends", store.compactionsForTest() == before + 1);
    }
    {
        // Android polls for the first time inside start(): the starter's mark comes before the host knows anything,
        // and the first known state (Idle, the start not yet sent) must not free it.
        Roots roots;
        const QString folder = compactable_folder(roots);
        PolledHost host;
        RecordingStore store;
        store.setRoots(roots.list());
        store.setTimingForTest(3600000, 3600000);
        dsd_qt::recording_store_follow_host(&store, &host);
        store.start();
        expect("marked before the host knows",
               store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County")) == folder);
        host.poll(dsd_qt::DecoderHost::Idle);
        store.waitForPassForTest();
        expect("the first known state keeps the starter's mark", store.compactionsForTest() == 0);
        host.poll(dsd_qt::DecoderHost::Running);
        host.poll(dsd_qt::DecoderHost::Idle);
        store.waitForPassForTest();
        expect("which the session's end frees", store.compactionsForTest() == 1);
    }
}

/* Issue #708, I1: a session start needs only the folder table, never the journals: beginSession() returns while the
 * worker is still reading them (here held at the first one), finding a renamed owner's folder by uid. */
void
test_start_does_not_wait_for_journals(void) {
    Roots roots;
    const QString folder = compactable_folder(roots);
    std::atomic<bool> release{false};
    std::atomic<int> reads{0};
    RecordingStore store;
    store.setRoots(roots.list());
    store.setTimingForTest(3600000, 3600000);
    store.hostSessionReport(true, false, false);
    store.setFolderLoadHookForTest([&release, &reads](const QString&) {
        reads++;
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    store.start();
    while (reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const QString path = store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("Renamed"));
    expect("a start does not wait for the journals, and finds the owner's folder by uid", path == folder);
    expect("whose journal is not read yet", !store.hasRecordingForFragments({fragment(500)}));
    release = true;
    store.waitForPassForTest();
    QCoreApplication::processEvents();
    expect("then it is", store.hasRecordingForFragments({fragment(500)}) && store.indexed());
    // The worker read folder.json ("County") before the start relabelled it; the newer label stands.
    expect("the start's relabel survives the read that began before it",
           store.folders().value(0).toMap().value("name").toString() == QStringLiteral("Renamed")
               && read_all(folder + QStringLiteral("/folder.json")).contains("Renamed"));
    store.endSession();
}

/* Issue #708, N1: a folder removed from a computer between the folder table's read and its own (folder.json gone)
 * leaves the table. A session start is not handed it (the owner gets a new folder), and without one the worker drops
 * it when it gets there; either way the store reaches `indexed`. */
void
test_folder_gone_before_its_read(void) {
    for (const bool withSession : {true, false}) {
        Roots roots;
        const QString first = QDir::cleanPath(compactable_folder(roots));
        const QString second = QDir::cleanPath(roots.external + QStringLiteral("/Metro (5e6f7a8b)"));
        QDir().mkpath(second);
        if (!write_private(
                second + QStringLiteral("/folder.json"),
                QStringLiteral("{\"uid\":\"%1\",\"kind\":\"saved\",\"name\":\"Metro\"}\n").arg(kUidC).toUtf8(), "wb")) {
            g_failures++;
        }
        std::atomic<bool> release{false};
        std::mutex heldMutex;
        QString held;
        RecordingStore store;
        store.setRoots(roots.list());
        store.setTimingForTest(3600000, 3600000);
        store.hostSessionReport(true, false, false);
        store.setFolderLoadHookForTest([&](const QString& path) {
            {
                const std::lock_guard<std::mutex> lock(heldMutex);
                if (!held.isEmpty()) {
                    return;
                }
                held = path;
            }
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        store.start();
        QString heldPath;
        while (heldPath.isEmpty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const std::lock_guard<std::mutex> lock(heldMutex);
            heldPath = held;
        }
        // The worker holds at one folder's read; the other is tabled, not read, and loses its folder.json.
        const QString other = heldPath == first ? second : first;
        const QString otherUid = other == second ? QString(kUidC) : QString(kUidA);
        expect("the folder.json is removed", QFile::remove(other + QStringLiteral("/folder.json")));
        if (withSession) {
            const QString given = store.beginSession(QStringLiteral("saved"), otherUid, QStringLiteral("Again"));
            expect("a session is not handed a folder whose folder.json is gone", !given.isEmpty() && given != other);
            store.endSession();
        }
        release = true;
        store.waitForPassForTest();
        QCoreApplication::processEvents();
        expect("the store still finishes its first read", store.indexed());
        expect("and the folder is out of the table",
               !store.folderPathsForTest().contains(other) && store.folderPathsForTest().contains(heldPath));
    }
}

/* A folder.json for @p uid named @p name in @p folder, made if need be. */
void
write_folder_json(const QString& folder, const QString& uid, const QString& name) {
    QDir().mkpath(folder);
    if (!write_private(folder + QStringLiteral("/folder.json"),
                       QStringLiteral("{\"uid\":\"%1\",\"kind\":\"saved\",\"name\":\"%2\"}\n").arg(uid, name).toUtf8(),
                       "wb")) {
        g_failures++;
    }
}

/* Issue #708: a root that goes away within one pass, after the pass found it there, is not read as its folders being
 * deleted. The folder-load hook takes the external storage away in the middle of a pass. Tail: a folder already read
 * finds its journal gone; it keeps its recordings, which read as away. Read: a folder not read yet finds its
 * folder.json gone; it stays in the table. */
void
test_root_gone_mid_pass(void) {
    {
        Roots roots;
        const QString away = roots.dir.path() + QStringLiteral("/ext-away");
        const QString internalParent = roots.dir.path() + QStringLiteral("/int");
        // Away at start: a file stands where its parent folder would go, so the store cannot make it.
        expect("the internal root starts away", QDir(internalParent).removeRecursively());
        make_file(internalParent, 1);
        Store s(roots.list());
        const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        record(folder, QStringLiteral("00000000000000EE-1"), QStringLiteral("a.wav"), 1754705000, 60);
        s.store.endSession();
        s.pass();
        expect("tail: the recording is indexed", s.store.recordingCount() == 1);

        // The internal root comes back holding a folder, so the next pass reads it, and that read takes the
        // external storage away, after the pass found it there and before it tails the external folder.
        expect("the internal root comes back", QFile::remove(internalParent));
        write_folder_json(roots.internal + QStringLiteral("/Metro (5e6f7a8b)"), QString(kUidC),
                          QStringLiteral("Metro"));
        std::atomic<bool> moved{false};
        s.store.setFolderLoadHookForTest([&](const QString&) {
            if (!moved.exchange(true)) {
                (void)QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away);
            }
        });
        const QSignalSpy news(&s.store, &RecordingStore::recordingFragmentsChanged);
        s.pass();
        expect("tail: the storage went mid-pass", moved.load() && !QFileInfo::exists(folder));
        const QVariantMap a = s.store.recordingsForFragments({fragment(60)}).value(0).toMap();
        expect("tail: the recording stays indexed, marked away",
               s.store.recordingCount() == 1 && !a.isEmpty() && !a.value("available").toBool());
        expect("tail: its row is told", spied(news, fragment(60)));
        expect("tail: its folder stays in the table", s.store.folderPathsForTest().contains(QDir::cleanPath(folder)));
        s.store.setFolderLoadHookForTest({});
        expect("tail: the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
        s.pass();
        const QVariantMap back = s.store.recordingsForFragments({fragment(60)}).value(0).toMap();
        expect("tail: read again, available", s.store.recordingCount() == 1 && back.value("available").toBool());
    }
    {
        Roots roots;
        const QString away = roots.dir.path() + QStringLiteral("/ext-away");
        const QString first = QDir::cleanPath(roots.external + QStringLiteral("/County (1a2b3c4d)"));
        const QString second = QDir::cleanPath(roots.external + QStringLiteral("/Metro (5e6f7a8b)"));
        write_folder_json(first, QString(kUidA), QStringLiteral("County"));
        write_folder_json(second, QString(kUidC), QStringLiteral("Metro"));
        std::atomic<bool> moved{false};
        RecordingStore store;
        store.setRoots(roots.list());
        store.setTimingForTest(3600000, 3600000);
        store.hostSessionReport(true, false, false);
        // The first folder read takes the storage away; the second then finds no folder.json.
        store.setFolderLoadHookForTest([&](const QString&) {
            if (!moved.exchange(true)) {
                (void)QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away);
            }
        });
        store.start();
        store.waitForPassForTest();
        QCoreApplication::processEvents();
        expect("read: the storage went mid-pass", moved.load());
        expect("read: both folders stay in the table",
               store.folderPathsForTest().contains(first) && store.folderPathsForTest().contains(second));
        store.setFolderLoadHookForTest({});
    }
}

/* Issue #708: a recheck that finds a file missing after its root went away within the pass (a share that failed as
 * the USB storage left) marks the root away and leaves the index as it is. The external root holds one folder with an
 * earlier process's unfinished recording and no journal, so no tail notices the root going; the internal root coming
 * back is read first, and the folder-load hook takes the external storage away in that read. */
void
test_root_gone_before_a_recheck(void) {
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString internalParent = roots.dir.path() + QStringLiteral("/int");
    // Away at start: a file stands where its parent folder would go, so the store cannot make it.
    expect("the internal root starts away", QDir(internalParent).removeRecursively());
    make_file(internalParent, 1);
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    write_folder_json(folder, QString(kUidA), QStringLiteral("County"));
    const QString temp = folder + QStringLiteral("/TEMP_20261027_120000_0123456789ABCDEF_9.wav");
    make_file(temp, 7000);
    Store s(roots.list());
    expect("the unfinished recording is indexed", s.store.unfinishedCount() == 1);

    std::atomic<bool> moved{false};
    s.store.setFolderLoadHookForTest([&](const QString&) {
        if (!moved.exchange(true)) {
            (void)QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away);
        }
    });
    expect("the internal root comes back", QFile::remove(internalParent));
    write_folder_json(roots.internal + QStringLiteral("/Metro (5e6f7a8b)"), QString(kUidC), QStringLiteral("Metro"));
    s.store.recheckRecordings({QStringLiteral("unfinished:") + QDir::cleanPath(QFileInfo(temp).absoluteFilePath())});
    s.pass();
    expect("the storage went mid-pass", moved.load() && !QFileInfo::exists(temp));
    expect("the recording stays indexed", s.store.unfinishedCount() == 1);
    bool countyAway = false;
    for (const QVariant& f : s.store.folders()) {
        const QVariantMap m = f.toMap();
        countyAway = countyAway || (m.value("uid").toString() == kUidA && !m.value("available").toBool());
    }
    expect("its folder reads as away", countyAway);
    s.store.setTailHookForTest({});
    expect("the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
    s.pass();
    expect("read again, still there", s.store.unfinishedCount() == 1 && QFileInfo::exists(temp));
}

/* Issue #708: a delete that finds its file missing after the root went away within the pass (the USB storage left as
 * the user deleted) marks the root away, and keeps the recording indexed and its delete queued while it is away. Laid
 * out as the recheck case above: no journal in the external root, so no tail notices it going. The storage goes as the
 * other root's journal is tailed, a pass that reads nothing again. Once it is back and read again, the delete is
 * dropped (issue #902 B12: a read again drops every delete not done yet) and runs when asked again. */
void
test_root_gone_before_a_delete(void) {
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString internalParent = roots.dir.path() + QStringLiteral("/int");
    expect("the internal root starts away", QDir(internalParent).removeRecursively());
    make_file(internalParent, 1);
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    write_folder_json(folder, QString(kUidA), QStringLiteral("County"));
    const QString temp = folder + QStringLiteral("/TEMP_20261027_120000_0123456789ABCDEF_9.wav");
    make_file(temp, 7000);
    Store s(roots.list());
    expect("the unfinished recording is indexed", s.store.unfinishedCount() == 1);

    expect("the internal root comes back", QFile::remove(internalParent));
    const QString metro = roots.internal + QStringLiteral("/Metro (5e6f7a8b)");
    write_folder_json(metro, QString(kUidC), QStringLiteral("Metro"));
    s.pass();
    std::atomic<bool> moved{false};
    s.store.setTailHookForTest([&](const QString&) {
        if (!moved.exchange(true)) {
            (void)QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away);
        }
    });
    record(metro, QStringLiteral("00000000000000F9-1"), QStringLiteral("m.wav"), 1754705000, 60);
    expect("the delete asked", s.store.deleteRecording(QStringLiteral("unfinished:")
                                                       + QDir::cleanPath(QFileInfo(temp).absoluteFilePath())));
    s.pass();
    expect("the storage went mid-pass", moved.load() && !QFileInfo::exists(temp));
    expect("the recording stays indexed, its delete waiting",
           s.store.unfinishedCount() == 1 && s.store.deletesWaiting() == 1);
    bool countyAway = false;
    for (const QVariant& f : s.store.folders()) {
        const QVariantMap m = f.toMap();
        countyAway = countyAway || (m.value("uid").toString() == kUidA && !m.value("available").toBool());
    }
    expect("its folder reads as away", countyAway);
    s.store.setFolderLoadHookForTest({});
    expect("the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
    s.pass();
    expect("the delete is dropped once it is back",
           QFileInfo::exists(temp) && s.store.unfinishedCount() == 1 && s.store.deletesWaiting() == 0);
    expect("asked again", s.store.deleteRecording(QStringLiteral("unfinished:")
                                                  + QDir::cleanPath(QFileInfo(temp).absoluteFilePath())));
    s.pass();
    expect("asked again, the delete runs", !QFileInfo::exists(temp) && s.store.unfinishedCount() == 0);
}

/* Whether the owner @p uid's folders read as away in the store's folder list. */
bool
folder_away(const RecordingStore& store, const QString& uid) {
    for (const QVariant& f : store.folders()) {
        const QVariantMap m = f.toMap();
        if (m.value("uid").toString() == uid) {
            return !m.value("available").toBool();
        }
    }
    return false;
}

/* Issue #708: storage that leaves again as the store reads it back (a USB connection dropping while the root is
 * listed) is not storage emptied: its empty listing drops nothing, so its recording stays indexed and away. A delete
 * of it while it is away is refused at once (issue #902), and runs once asked again with the storage back for good. */
void
test_root_gone_as_it_is_read_again(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    record(folder, QStringLiteral("00000000000000F0-1"), QStringLiteral("a.wav"), 1754705000, 60);
    s.store.endSession();
    s.pass();
    expect("read again: the recording is indexed", s.store.recordingCount() == 1);
    expect("read again: the storage goes away", QDir().rename(ext, away));
    s.pass();
    expect("read again: its delete is refused while it is away",
           !s.store.deleteRecording(QStringLiteral("00000000000000F0-1"))
               && s.store.deleteRefusal() == QLatin1String("storageAway"));
    s.pass();
    expect("read again: nothing waits", s.store.deletesWaiting() == 0 && folder_away(s.store, kUidA));

    std::atomic<bool> moved{false};
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.endsWith(QLatin1String("/ext/Call recordings")) && !moved.exchange(true)) {
            (void)QDir().rename(ext, away);
        }
    });
    expect("read again: the storage comes back", QDir().rename(away, ext));
    s.pass();
    expect("read again: and leaves as it is listed", moved.load() && !QFileInfo::exists(folder));
    expect("read again: the recording stays indexed", s.store.recordingCount() == 1 && s.store.deletesWaiting() == 0);
    expect("read again: its folder reads as away", folder_away(s.store, kUidA));
    s.store.setRootReadHookForTest({});
    expect("read again: the storage is back for good", QDir().rename(away, ext));
    s.pass();
    expect("read again: nothing deleted", QFileInfo::exists(folder + QStringLiteral("/a.wav"))
                                              && s.store.recordingCount() == 1 && s.store.deletesWaiting() == 0);
    expect("read again: asked again", s.store.deleteRecording(QStringLiteral("00000000000000F0-1")));
    s.pass();
    expect("read again: the delete runs",
           !QFileInfo::exists(folder + QStringLiteral("/a.wav")) && s.store.recordingCount() == 0);
}

/* Issue #708: storage that comes back is listed whole and then has its journals read; storage that leaves again
 * during that read (after the listing, as a folder is read) is not storage emptied either. What the index held for
 * it stays until a read of its folders completes: the recording stays indexed and away. A delete of it while it is
 * away is refused at once (issue #902), and runs once asked again with the storage back for good. */
void
test_root_gone_as_its_folders_are_read_again(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    record(folder, QStringLiteral("00000000000000F8-1"), QStringLiteral("a.wav"), 1754705000, 60);
    s.store.endSession();
    s.pass();
    expect("folders again: the recording is indexed", s.store.recordingCount() == 1);
    expect("folders again: the storage goes away", QDir().rename(ext, away));
    s.pass();
    expect("folders again: its delete is refused while it is away",
           !s.store.deleteRecording(QStringLiteral("00000000000000F8-1"))
               && s.store.deleteRefusal() == QLatin1String("storageAway"));
    s.pass();
    expect("folders again: nothing waits", s.store.deletesWaiting() == 0 && folder_away(s.store, kUidA));

    std::atomic<bool> moved{false};
    s.store.setFolderLoadHookForTest([&](const QString&) {
        if (!moved.exchange(true)) {
            (void)QDir().rename(ext, away);
        }
    });
    expect("folders again: the storage comes back", QDir().rename(away, ext));
    s.pass();
    expect("folders again: and leaves as its folder is read", moved.load() && !QFileInfo::exists(folder));
    expect("folders again: the recording stays indexed",
           s.store.recordingCount() == 1 && s.store.deletesWaiting() == 0);
    expect("folders again: its folder reads as away", folder_away(s.store, kUidA));
    s.store.setFolderLoadHookForTest({});
    expect("folders again: the storage is back for good", QDir().rename(away, ext));
    s.pass();
    expect("folders again: nothing deleted", QFileInfo::exists(folder + QStringLiteral("/a.wav"))
                                                 && s.store.recordingCount() == 1 && s.store.deletesWaiting() == 0);
    expect("folders again: asked again", s.store.deleteRecording(QStringLiteral("00000000000000F8-1")));
    s.pass();
    expect("folders again: the delete runs",
           !QFileInfo::exists(folder + QStringLiteral("/a.wav")) && s.store.recordingCount() == 0);
}

/* Issue #708: a journal shorter than its cursor is read again from its start; storage that leaves during that read
 * (after the folder.json, before the journal) drops none of the folder's recordings, which read as away. */
void
test_root_gone_as_a_journal_is_read_again(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    record(folder, QStringLiteral("00000000000000F1-1"), QStringLiteral("a.wav"), 1754705000, 60);
    record(folder, QStringLiteral("00000000000000F1-2"), QStringLiteral("b.wav"), 1754705010, 61);
    s.store.endSession();
    s.pass();
    expect("reread: both recordings are indexed", s.store.recordingCount() == 2);
    {
        // Replaced behind the store's back by a shorter journal naming only the first.
        const QByteArray line =
            publish_line(QStringLiteral("00000000000000F1-1"), QStringLiteral("a.wav"), 1754705000, 60);
        expect("reread: the journal is replaced", write_private(journal_of(folder), line, "wb"));
    }
    std::atomic<bool> moved{false};
    s.store.setFolderLoadHookForTest([&](const QString&) {
        if (!moved.exchange(true)) {
            (void)QDir().rename(ext, away);
        }
    });
    s.pass();
    expect("reread: the storage went as the folder was read", moved.load() && !QFileInfo::exists(folder));
    expect("reread: both recordings stay indexed, away", s.store.recordingCount() == 2 && folder_away(s.store, kUidA));
    s.store.setFolderLoadHookForTest({});
    expect("reread: the storage comes back", QDir().rename(away, ext));
    s.pass();
    expect("reread: read again, as the new journal says",
           s.store.recordingCount() == 1 && !folder_away(s.store, kUidA)
               && s.store.isIndexedPath(folder + QStringLiteral("/a.wav")));
}

/* Issue #708: storage that leaves between a journal's new lines and the look at their files is not those files
 * deleted. The root is marked away and the lines stay unread, so a recording published then is indexed once the
 * storage is back, even when it is back before the next pass begins. */
void
test_root_gone_as_a_journal_is_tailed(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    record(folder, QStringLiteral("00000000000000F2-1"), QStringLiteral("a.wav"), 1754705000, 60);
    s.store.endSession();
    s.pass();
    expect("tailed: the recording is indexed", s.store.recordingCount() == 1);
    record(folder, QStringLiteral("00000000000000F2-2"), QStringLiteral("b.wav"), 1754705010, 61);
    std::atomic<bool> moved{false};
    s.store.setTailHookForTest([&](const QString&) {
        if (!moved.exchange(true)) {
            (void)QDir().rename(ext, away);
        }
    });
    s.pass();
    expect("tailed: the storage went as the lines were read", moved.load() && !QFileInfo::exists(folder));
    expect("tailed: the folder reads as away, its recording kept",
           folder_away(s.store, kUidA) && s.store.recordingCount() == 1);
    s.store.setTailHookForTest({});
    expect("tailed: the storage comes back", QDir().rename(away, ext));
    s.pass();
    expect("tailed: the new recording is indexed",
           s.store.recordingCount() == 2 && s.store.isIndexedPath(folder + QStringLiteral("/b.wav")));
}

/* Issue #708: storage that leaves while the limit removes recordings takes its usage with it. A 1 MB limit, an older
 * 900 kB recording on the external storage and a newer 200 kB one inside: the external one is the first to go, and
 * its storage leaves as it is checked. What is left counts 200 kB, so the internal one stays; the limit tries again
 * once the storage is back, and the oldest goes then. */
void
test_root_gone_during_the_limit(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString outer = roots.external + QStringLiteral("/County (1a2b3c4d)");
    const QString inner = roots.internal + QStringLiteral("/Metro (5e6f7a8b)");
    write_folder_json(outer, QString(kUidA), QStringLiteral("County"));
    write_folder_json(inner, QString(kUidC), QStringLiteral("Metro"));
    record(outer, QStringLiteral("00000000000000F3-1"), QStringLiteral("old.wav"), 1754705000, 60, 900000);
    record(inner, QStringLiteral("00000000000000F3-2"), QStringLiteral("new.wav"), 1754705100, 61, 200000);
    Store s(roots.list(), false);
    std::atomic<bool> moved{false};
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/old.wav")) && !moved.exchange(true)) {
            (void)QDir().rename(ext, away);
        }
        return false;
    });
    s.store.setLimitMb(1);
    s.start();
    s.pass();
    expect("limit: the storage went as its recording was checked", moved.load() && !QFileInfo::exists(outer));
    expect("limit: the recording inside stays, what is left being under the limit",
           QFileInfo::exists(inner + QStringLiteral("/new.wav")) && s.store.removedByLimit() == 0);
    expect("limit: the one away stays indexed", s.store.recordingCount() == 2 && folder_away(s.store, kUidA));
    expect("limit: the storage comes back", QDir().rename(away, ext));
    s.pass();
    expect("limit: back over the limit, the oldest goes", !QFileInfo::exists(outer + QStringLiteral("/old.wav"))
                                                              && QFileInfo::exists(inner + QStringLiteral("/new.wav"))
                                                              && s.store.removedByLimit() == 1);
}

/* Issue #708: storage that leaves after the limit summed the usage, before its first removal, takes its usage with
 * it even when the recording to remove is elsewhere. A 1 MB limit, an older 200 kB recording inside and a newer
 * 900 kB one on the external storage: the inside one is the first to go, and the external storage leaves as it is
 * checked. What is left counts 200 kB, under the limit, so it stays; once the storage is back the limit is over again
 * and the oldest goes then. */
void
test_root_gone_before_the_limit_removes(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString outer = roots.external + QStringLiteral("/County (1a2b3c4d)");
    const QString inner = roots.internal + QStringLiteral("/Metro (5e6f7a8b)");
    write_folder_json(outer, QString(kUidA), QStringLiteral("County"));
    write_folder_json(inner, QString(kUidC), QStringLiteral("Metro"));
    record(inner, QStringLiteral("00000000000000F7-1"), QStringLiteral("old.wav"), 1754705000, 60, 200000);
    record(outer, QStringLiteral("00000000000000F7-2"), QStringLiteral("new.wav"), 1754705100, 61, 900000);
    Store s(roots.list(), false);
    std::atomic<bool> moved{false};
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/old.wav")) && !moved.exchange(true)) {
            (void)QDir().rename(ext, away);
        }
        return false;
    });
    s.store.setLimitMb(1);
    s.start();
    s.pass();
    expect("limit first: the storage went before the first removal", moved.load() && !QFileInfo::exists(outer));
    expect("limit first: the recording inside stays, what is left being under the limit",
           QFileInfo::exists(inner + QStringLiteral("/old.wav")) && s.store.removedByLimit() == 0);
    expect("limit first: the one away stays indexed", s.store.recordingCount() == 2 && folder_away(s.store, kUidA));
    expect("limit first: the storage comes back", QDir().rename(away, ext));
    s.pass();
    expect("limit first: back over the limit, the oldest goes",
           !QFileInfo::exists(inner + QStringLiteral("/old.wav"))
               && QFileInfo::exists(outer + QStringLiteral("/new.wav")) && s.store.removedByLimit() == 1);
}

/* Issue #708: an unlink that fails because the storage went as it ran is not a refused delete: the root is marked
 * away, the recording stays indexed, its delete stays queued and no failure is counted. The read once the storage is
 * back drops it (issue #902 B12), and it runs once asked again. */
void
test_root_gone_during_an_unlink(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list(), false);
    std::atomic<bool> moved{false};
    s.store.setRemoveForTest([&](const QString& path, QString* error) {
        if (!moved.exchange(true)) {
            (void)QDir().rename(ext, away);
            *error = QStringLiteral("Input/output error");
            return false;
        }
        return QFile::remove(path);
    });
    s.start();
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    record(folder, QStringLiteral("00000000000000F4-1"), QStringLiteral("a.wav"), 1754705000, 60);
    s.store.endSession();
    s.pass();
    expect("unlink: the recording is indexed", s.store.recordingCount() == 1);
    s.store.deleteRecording(QStringLiteral("00000000000000F4-1"));
    s.pass();
    expect("unlink: the storage went as it ran", moved.load() && !QFileInfo::exists(folder));
    expect("unlink: the recording stays indexed, its delete waiting, no failure counted",
           s.store.recordingCount() == 1 && s.store.deletesWaiting() == 1 && s.store.deleteFailures() == 0);
    expect("unlink: its folder reads as away", folder_away(s.store, kUidA));
    expect("unlink: the storage comes back", QDir().rename(away, ext));
    s.pass();
    expect("unlink: the delete is dropped once it is back",
           QFileInfo::exists(folder + QStringLiteral("/a.wav")) && s.store.recordingCount() == 1
               && s.store.deletesWaiting() == 0 && s.store.deleteFailures() == 0);
    expect("unlink: asked again", s.store.deleteRecording(QStringLiteral("00000000000000F4-1")));
    s.pass();
    expect("unlink: the delete runs", !QFileInfo::exists(folder + QStringLiteral("/a.wav"))
                                          && s.store.recordingCount() == 0 && s.store.deleteFailures() == 0);
}

/* Issue #708: storage that left after the last pass and before a session start is not made again under the absent
 * mount (recordings there would hide once the storage came back over it): the start checks the roots as they are now
 * and makes its folder in the fallback, and the pass it asks for marks the storage away. */
void
test_root_gone_before_a_session_start(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString old = s.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Metro"));
    record(old, QStringLiteral("00000000000000F5-1"), QStringLiteral("m.wav"), 1754705000, 70);
    s.store.endSession();
    s.pass();
    expect("start: folders go to the external storage while it is there",
           QDir::cleanPath(old).startsWith(QDir::cleanPath(roots.external) + QLatin1Char('/')));
    expect("start: the storage goes away", QDir().rename(ext, away));
    const QString fresh = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    expect("start: a new owner's folder is made in the fallback",
           QDir::cleanPath(fresh).startsWith(QDir::cleanPath(roots.internal) + QLatin1Char('/'))
               && QFileInfo(fresh).isDir());
    expect("start: nothing is made where the storage was", !QFileInfo::exists(ext));
    const QString again = s.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Metro"));
    expect("start: an owner whose folder is away gets one in the fallback",
           QDir::cleanPath(again).startsWith(QDir::cleanPath(roots.internal) + QLatin1Char('/'))
               && !QFileInfo::exists(ext));
    s.store.endSession();
    s.pass();
    const QVariantMap m = s.store.recordingsForFragments({fragment(70)}).value(0).toMap();
    expect("start: the next pass marks the storage away", !m.isEmpty() && !m.value("available").toBool());
    expect("start: still nothing where the storage was", !QFileInfo::exists(ext));
}

/* Whether this process is refused new files in a read-only folder (not, as root). */
bool
permissions_enforced(const QString& dir) {
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QFile probe(dir + QStringLiteral("/probe"));
    const bool refused = !probe.open(QIODevice::WriteOnly);
    probe.close();
    QFile::remove(dir + QStringLiteral("/probe"));
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return refused;
}

/* Issue #708: an owner's folder that no longer takes new files (storage mounted read-only, its permissions changed)
 * is not handed to a session, which would record nothing: the next root takes it, as when a folder cannot be made.
 * Both ways the start finds a folder: one the store has read, and one made for the owner that it has not read yet. */
void
test_unwritable_folder_falls_back(void) {
    const Roots roots;
    Store s(roots.list());
    const QString old = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    s.pass();
    expect("read-only: the folder goes to the external storage first",
           old == roots.external + QStringLiteral("/County (1a2b3c4d)"));
    if (!permissions_enforced(old)) {
        return; // as root, a read-only folder takes new files
    }
    const QFileDevice::Permissions open = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    const QFileDevice::Permissions readOnly = QFileDevice::ReadOwner | QFileDevice::ExeOwner;

    QFile::setPermissions(old, readOnly);
    const QString fallback = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    expect("read-only: a read folder that takes no new files gives way to the fallback",
           fallback == roots.internal + QStringLiteral("/County (1a2b3c4d)") && QFileInfo(fallback).isDir());
    QFile::setPermissions(old, open);
    const QString back = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    s.pass();
    expect("read-only: writable again, the preferred folder", back == old);

    const QString unread = roots.external + QStringLiteral("/Metro (1a2b3c4d)");
    expect("read-only: a folder made for an owner, not read yet", QDir().mkpath(unread));
    append(unread + QStringLiteral("/folder.json"),
           QStringLiteral("{\"uid\":\"%1\",\"kind\":\"saved\",\"name\":\"Metro\"}\n").arg(kUidB).toUtf8());
    QFile::setPermissions(unread, readOnly);
    const QString other = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
    s.store.endSession();
    expect("read-only: an unread folder that takes no new files gives way to the fallback",
           other == roots.internal + QStringLiteral("/Metro (1a2b3c4d)") && QFileInfo(other).isDir());
    QFile::setPermissions(unread, open);
    s.pass();
}

/* A free-space probe that answers by volume: 900 MB on the external storage, @p internalMb on the internal one, and the
 * paths it was asked about into @p asked. */
void
probe_by_volume(Store& s, const Roots& roots, qint64 internalMb, QStringList* asked) {
    const QString ext = QDir::cleanPath(roots.dir.path() + QStringLiteral("/ext"));
    s.store.setFreeSpaceProbeForTest([ext, internalMb, asked](const QString& path) {
        asked->append(path);
        const QString clean = QDir::cleanPath(path);
        const bool external = clean == ext || clean.startsWith(ext + QLatin1Char('/'));
        return (external ? qint64{900} : internalMb) * RecordingStore::kBytesPerMb;
    });
}

/* Issue #708: free space is read where recordings go. External storage that is there but takes no new files (mounted
 * read-only) sends a session to the internal one, so the internal volume's space is the one reported, idle or in the
 * session. */
void
test_free_space_where_a_session_goes(void) {
    const Roots roots;
    expect("space, read-only: the external root is there", QDir().mkpath(roots.external));
    if (!permissions_enforced(roots.external)) {
        return; // as root, a read-only folder takes new files
    }
    const QFileDevice::Permissions open = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    QFile::setPermissions(roots.external, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QStringList asked;
    Store s(roots.list(), false);
    probe_by_volume(s, roots, 100, &asked);
    s.start();
    expect("space, read-only: idle, the internal storage a session would take is read",
           s.store.lowSpace() && s.store.freeBytes() == 100 * RecordingStore::kBytesPerMb);
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    expect("space, read-only: the session falls back to the internal storage",
           QDir::cleanPath(folder).startsWith(QDir::cleanPath(roots.internal) + QLatin1Char('/')));
    record(folder, QStringLiteral("00000000000000F7-1"), QStringLiteral("a.wav"), 1754705000, 80);
    s.pass();
    expect("space, read-only: in the session, the fallback's free space and its warning",
           s.store.lowSpace() && s.store.freeBytes() == 100 * RecordingStore::kBytesPerMb);
    expect("space, read-only: the probe was asked about the internal storage",
           !asked.isEmpty()
               && QDir::cleanPath(asked.last()).startsWith(QDir::cleanPath(roots.dir.path() + QStringLiteral("/int"))));
    s.store.endSession();
    QFile::setPermissions(roots.external, open);
    s.pass();
}

/* Issue #708: a session that started in the fallback while the external storage was away keeps recording there when
 * the external storage comes back, so the fallback's free space is still the one reported until the session ends. */
void
test_free_space_follows_the_session(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    expect("space, back: the external storage starts away", QDir().rename(ext, away));
    QStringList asked;
    Store s(roots.list(), false);
    probe_by_volume(s, roots, 150, &asked);
    s.start();
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    expect("space, back: the session goes to the fallback",
           QDir::cleanPath(folder).startsWith(QDir::cleanPath(roots.internal) + QLatin1Char('/')));
    expect("space, back: the external storage comes back", QDir().rename(away, ext));
    record(folder, QStringLiteral("00000000000000F8-1"), QStringLiteral("a.wav"), 1754705000, 81);
    s.pass();
    expect("space, back: the session's fallback is still the volume read",
           s.store.lowSpace() && s.store.freeBytes() == 150 * RecordingStore::kBytesPerMb);
    s.store.endSession();
    record(folder, QStringLiteral("00000000000000F8-2"), QStringLiteral("b.wav"), 1754705010, 82);
    s.pass();
    expect("space, back: idle, the external storage the next session takes is read",
           !s.store.lowSpace() && s.store.freeBytes() == 900 * RecordingStore::kBytesPerMb);
}

/* Issue #708: the session ending is enough to read the free space again, with nothing published after it: idle, the
 * reading moves to the volume the next session takes, and a warning that came from the fallback goes. */
void
test_free_space_after_the_session_ends(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    expect("space, end: the external storage starts away", QDir().rename(ext, away));
    QStringList asked;
    Store s(roots.list(), false);
    probe_by_volume(s, roots, 150, &asked);
    s.start();
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    expect("space, end: the external storage comes back", QDir().rename(away, ext));
    record(folder, QStringLiteral("00000000000000F9-1"), QStringLiteral("a.wav"), 1754705000, 83);
    s.pass();
    expect("space, end: the session's fallback is read", s.store.lowSpace());
    s.store.endSession();
    s.pass();
    expect("space, end: once the session ends, the external storage is read without a publish",
           !s.store.lowSpace() && s.store.freeBytes() == 900 * RecordingStore::kBytesPerMb);
}

/* Wait (at most five seconds) for the store to deliver @p count recordings from a pass the test did not ask for. */
bool
counted_soon(const RecordingStore& store, int count) {
    const uint64_t deadline = dsd_realtime_mono_ms() + 5000U;
    while (store.recordingCount() != count && dsd_realtime_mono_ms() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return store.recordingCount() == count;
}

/* Issue #708: a folder that turns active wakes the worker from its idle wait (an hour here, a minute in the app), so
 * the recordings of a session in an existing folder are read at the active cadence from its start, whether a starter
 * begins it in an unchanged folder or the host reports a session no starter began. */
void
test_session_start_wakes_the_worker(void) {
    const Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    write_folder_json(folder, QString(kUidA), QStringLiteral("County"));
    Store s(roots.list());
    expect("wake: nothing indexed yet", s.store.recordingCount() == 0);
    record(folder, QStringLiteral("00000000000000F6-1"), QStringLiteral("a.wav"), 1754705000, 60);
    expect("wake: the session reuses the folder as it is",
           QDir::cleanPath(s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County")))
               == QDir::cleanPath(folder));
    expect("wake: a starter's session in an unchanged folder wakes a pass", counted_soon(s.store, 1));
    s.store.endSession();
    s.pass();
    record(folder, QStringLiteral("00000000000000F6-2"), QStringLiteral("b.wav"), 1754705010, 61);
    s.store.hostSessionReport(true, true, false);
    expect("wake: a session the host reports wakes a pass", counted_soon(s.store, 2));
    s.store.hostSessionReport(true, false, false);
}

/* Issue #708, I2: files removed outside the app (a computer over USB) leave the index when the player, an export or a
 * share finds them missing and asks the store to look again; the limit never counts an already gone file as one it
 * removed. */
void
test_recheck_recordings(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    for (int i = 0; i < 12; i++) {
        record(folder, QStringLiteral("0000000000000066-%1").arg(i), QStringLiteral("g%1.wav").arg(i), 1754711000 + i,
               600 + i, 100000);
    }
    s.pass();
    QSignalSpy news(&s.store, &RecordingStore::recordingFragmentsChanged);
    QFile::remove(folder + QStringLiteral("/g5.wav"));
    make_file(folder + QStringLiteral("/g6.wav"), 150000);
    expect("still indexed until asked",
           s.store.recordingCount() == 12 && s.store.isIndexedPath(folder + QStringLiteral("/g5.wav")));
    s.store.recheckRecordings({QStringLiteral("0000000000000066-5"), QStringLiteral("0000000000000066-6"),
                               QStringLiteral("not-a-recording")});
    s.pass();
    expect("a missing file leaves the index, announced",
           s.store.recordingCount() == 11 && !s.store.hasRecordingForFragments({fragment(605)})
               && spied(news, fragment(605)) && !s.store.isIndexedPath(folder + QStringLiteral("/g5.wav")));
    // Another file than the index knew (written to since): its folder is read again whole, at the next pass.
    s.pass();
    expect("a file changed is read again with its folder", s.store.usedBytes() == 1150000);

    // Removed behind the store's back without a recheck: the limit finds it gone and does not count it.
    QFile::remove(folder + QStringLiteral("/g0.wav"));
    QSignalSpy reached(&s.store, &RecordingStore::limitReached);
    s.store.setLimitMb(1);
    s.pass();
    // 1.15 MB indexed, 1.05 MB on disk: the gone g0 is taken off, g1 removed, and 0.95 MB is reached.
    expect("the limit removes what it must, not counting the gone file",
           s.store.removedByLimit() == 1 && reached.count() == 1 && reached.at(0).at(0).toInt() == 1
               && !QFileInfo::exists(folder + QStringLiteral("/g1.wav"))
               && QFileInfo::exists(folder + QStringLiteral("/g2.wav")) && s.store.usedBytes() == 950000);
}

/* Issue #708: a recording deleted behind the store's back (over USB, by a file manager) still counted its indexed size,
 * and the limit removed an older recording to make room it did not need. The 1 GB case at a thousandth of its size: a
 * 1 MB limit, 400 kB and 500 kB indexed, the newer 500 kB deleted outside the app, then 200 kB more. What is on disk is
 * 600 kB, under the limit: the limit looks at the files it counts first, the gone one leaves the index, and the 400 kB
 * recording stays. */
void
test_limit_counts_only_files_still_there(void) {
    const Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    s.store.setLimitMb(1);
    record(folder, QStringLiteral("0000000000000067-1"), QStringLiteral("older.wav"), 1754712000, 700, 400000);
    record(folder, QStringLiteral("0000000000000067-2"), QStringLiteral("newer.wav"), 1754712100, 701, 500000);
    s.pass();
    expect("gone behind its back: both under the limit",
           s.store.recordingCount() == 2 && s.store.usedBytes() == 900000 && s.store.removedByLimit() == 0);
    const QSignalSpy news(&s.store, &RecordingStore::recordingFragmentsChanged);
    expect("gone behind its back: the newer one is deleted outside the app",
           QFile::remove(folder + QStringLiteral("/newer.wav")));
    record(folder, QStringLiteral("0000000000000067-3"), QStringLiteral("latest.wav"), 1754712200, 702, 200000);
    s.pass();
    expect("gone behind its back: the older recording stays",
           QFileInfo::exists(folder + QStringLiteral("/older.wav")) && s.store.removedByLimit() == 0);
    expect("gone behind its back: the gone one leaves the index, announced",
           s.store.recordingCount() == 2 && s.store.usedBytes() == 600000
               && !s.store.isIndexedPath(folder + QStringLiteral("/newer.wav")) && spied(news, fragment(701)));
}

/* Issue #708: an I/Q replay can reuse a final file name once its 16-bit suffix repeats, after the first recording of
 * that name was deleted (by hand or by the limit) but before its journal was compacted. Rebuilt from that journal, both
 * publishes named the one file under two ids: the deleted recording came back on its History row, the file counted
 * twice, and deleting the old id unlinked the new recording. One path has one owner, the newest publish in journal
 * order. */
void
test_path_reused_by_a_later_publish(void) {
    const Roots roots;
    const QString a = QStringLiteral("00000000000000AA-1");
    const QString b = QStringLiteral("00000000000000AA-2");
    const QString name = QStringLiteral("20261020_120000_01234_NFM.wav");
    QString path;
    {
        Store s(roots.list());
        const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        path = folder + QLatin1Char('/') + name;
        record(folder, a, name, 1792497600, 1, 4000);
        s.pass();
        s.store.deleteRecording(a);
        s.pass();
        expect("path reused: the first recording is deleted",
               !QFileInfo::exists(path) && s.store.recordingCount() == 0);
        record(folder, b, name, 1792497600, 2, 6000);
        s.pass();
        expect("path reused: the later publish owns the file", s.store.recordingCount() == 1
                                                                   && s.store.usedBytes() == 6000
                                                                   && !s.store.hasRecordingForFragments({fragment(1)})
                                                                   && s.store.hasRecordingForFragments({fragment(2)}));
    }
    Store s(roots.list());
    expect("path reused: rebuilt from the journal, one recording owns the file",
           s.store.recordingCount() == 1 && s.store.isIndexedPath(path));
    expect("path reused: the deleted recording stays off its row", !s.store.hasRecordingForFragments({fragment(1)}));
    expect("path reused: the later one is on its row", s.store.recordingsForFragments({fragment(2)}).size() == 1);
    expect("path reused: the file counts once", s.store.usedBytes() == 6000);
    s.store.deleteRecording(a);
    s.pass();
    expect("path reused: deleting the old id leaves the new file",
           QFileInfo::exists(path) && s.store.recordingCount() == 1);
}

/* Issue #708: the limit's look at the files it counts keeps the storage-loss rule. A file missing because its storage
 * went as the pass ran is away, not deleted: it stays indexed and its storage reads as away. Laid out so nothing else
 * notices the storage going first: the external folder holds no journal for a tail to miss, only an unfinished
 * recording an earlier process left, and the storage leaves as the internal journal is tailed. */
void
test_limit_count_keeps_storage_that_went(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString outer = roots.external + QStringLiteral("/County (1a2b3c4d)");
    const QString inner = roots.internal + QStringLiteral("/Metro (5e6f7a8b)");
    write_folder_json(outer, QString(kUidA), QStringLiteral("County"));
    write_folder_json(inner, QString(kUidC), QStringLiteral("Metro"));
    const QString temp = outer + QStringLiteral("/TEMP_20261027_120000_0123456789ABCDEF_9.wav");
    make_file(temp, 500000);
    record(inner, QStringLiteral("00000000000000F9-1"), QStringLiteral("old.wav"), 1754705000, 60, 400000);
    Store s(roots.list(), false);
    s.store.setLimitMb(1);
    s.start();
    expect("storage went: both indexed, under the limit",
           s.store.unfinishedCount() == 1 && s.store.recordingCount() == 1 && s.store.removedByLimit() == 0);
    std::atomic<bool> moved{false};
    s.store.setTailHookForTest([&](const QString&) {
        if (!moved.exchange(true)) {
            (void)QDir().rename(ext, away);
        }
    });
    record(inner, QStringLiteral("00000000000000F9-2"), QStringLiteral("new.wav"), 1754705100, 61, 200000);
    s.pass();
    expect("storage went: as the journal was tailed", moved.load() && !QFileInfo::exists(temp));
    expect("storage went: the recording there stays indexed, its storage away",
           s.store.unfinishedCount() == 1 && folder_away(s.store, kUidA));
    expect("storage went: what is left is under the limit",
           QFileInfo::exists(inner + QStringLiteral("/old.wav")) && s.store.removedByLimit() == 0);
    s.store.setTailHookForTest({});
    expect("storage went: it comes back", QDir().rename(away, ext));
    s.pass();
    expect("storage went: back over the limit, the oldest goes", !QFileInfo::exists(inner + QStringLiteral("/old.wav"))
                                                                     && QFileInfo::exists(temp)
                                                                     && s.store.removedByLimit() == 1);
}

/* Issue #708, I3: a delete the filesystem refuses is counted and its reason kept for the Call recordings screen, the
 * recording stays indexed, and each cause (asked for, the limit) is logged once per run. */
void
test_delete_failures_are_reported(void) {
    Roots roots;
    Store s(roots.list(), false);
    s.store.setRemoveForTest([](const QString& path, QString* error) {
        if (path.endsWith(QLatin1String("/stuck.wav")) || path.endsWith(QLatin1String("/big1.wav"))) {
            *error = QStringLiteral("Permission denied");
            return false;
        }
        return QFile::remove(path);
    });
    s.start();
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000077-1"), QStringLiteral("stuck.wav"), 1754712000, 700);
    s.pass();
    const int warnings = g_storeWarnings.load();
    s.store.deleteRecording(QStringLiteral("0000000000000077-1"));
    s.pass();
    expect("a refused delete is counted with its reason",
           s.store.deleteFailures() == 1 && s.store.lastDeleteError() == QStringLiteral("Permission denied")
               && s.store.deletesWaiting() == 0);
    expect("the recording stays",
           s.store.recordingCount() == 1 && QFileInfo::exists(folder + QStringLiteral("/stuck.wav")));
    expect("logged once", g_storeWarnings.load() == warnings + 1);
    s.store.deleteRecording(QStringLiteral("0000000000000077-1"));
    s.pass();
    expect("a second failure is counted, not logged",
           s.store.deleteFailures() == 2 && g_storeWarnings.load() == warnings + 1);

    record(folder, QStringLiteral("0000000000000077-2"), QStringLiteral("big1.wav"), 1754712010, 701, 600000);
    record(folder, QStringLiteral("0000000000000077-3"), QStringLiteral("big2.wav"), 1754712020, 702, 600000);
    s.store.setLimitMb(1);
    s.pass();
    expect("the limit's failures are counted and logged once on their own",
           s.store.deleteFailures() == 4 && g_storeWarnings.load() == warnings + 2
               && QFileInfo::exists(folder + QStringLiteral("/big1.wav"))
               && !QFileInfo::exists(folder + QStringLiteral("/big2.wav")) && s.store.removedByLimit() == 1);

    // Seen, the failures and their reason count from zero again; the next one shows with its own reason.
    s.store.acknowledgeNotices();
    expect("acknowledged, no failure is shown",
           s.store.deleteFailures() == 0 && s.store.lastDeleteError().isEmpty() && s.store.removedByLimit() == 0);
    s.store.deleteRecording(QStringLiteral("0000000000000077-1"));
    s.pass();
    expect("a later failure shows on its own",
           s.store.deleteFailures() == 1 && s.store.lastDeleteError() == QStringLiteral("Permission denied"));
}

/* Issue #708, M5: an exit during a long first read does not wait for the rest of it. */
void
test_stop_during_the_first_read(void) {
    Roots roots;
    QDir().mkpath(roots.external);
    for (int i = 0; i < 30; i++) {
        const QString folder = roots.external + QStringLiteral("/F%1").arg(i);
        QDir().mkpath(folder);
        if (!write_private(folder + QStringLiteral("/folder.json"),
                           QStringLiteral("{\"uid\":\"u%1\"}\n").arg(i).toUtf8(), "wb")) {
            g_failures++;
        }
    }
    std::atomic<int> reads{0};
    auto store = std::make_unique<RecordingStore>();
    store->setRoots(roots.list());
    store->setFolderLoadHookForTest([&reads](const QString&) {
        reads++;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    store->start();
    while (reads.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    store.reset();
    expect("the worker stops between folders", reads.load() <= 2);
}

/* Issue #708, M6: a delete waiting on the file the player had open runs as soon as the player moves on, not at the next
 * timed pass (an hour away here). */
void
test_player_release_wakes_a_waiting_delete(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000088-1"), QStringLiteral("p1.wav"), 1754713000, 800);
    record(folder, QStringLiteral("0000000000000088-2"), QStringLiteral("p2.wav"), 1754713010, 801);
    s.pass();
    s.store.setPlayingPath(folder + QStringLiteral("/p1.wav"));
    s.store.deleteRecording(QStringLiteral("0000000000000088-1"));
    s.pass();
    expect("held while it plays", QFileInfo::exists(folder + QStringLiteral("/p1.wav")));
    s.store.setPlayingPath(folder + QStringLiteral("/p2.wav"));
    const uint64_t deadline = dsd_realtime_mono_ms() + 5000U;
    while (QFileInfo::exists(folder + QStringLiteral("/p1.wav")) && dsd_realtime_mono_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect("deleted once the player moves on", !QFileInfo::exists(folder + QStringLiteral("/p1.wav")));
    s.store.setPlayingPath(QString());
}

/* Wait (at most five seconds) for the worker to remove @p path on a pass something other than the test asked for. */
bool
removed_soon(const QString& path) {
    const uint64_t deadline = dsd_realtime_mono_ms() + 5000U;
    while (QFileInfo::exists(path) && dsd_realtime_mono_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return !QFileInfo::exists(path);
}

/* Issue #708: a limit the held recordings kept the store over is enforced again once they are let go: an export's
 * selection and the player wake a pass when they release, and an rdio upload, which has no release, waits for the
 * timed pass (here the test's). Before, nothing tried again until the next publish, rebuild or limit change. */
void
test_held_recordings_over_the_limit_go_once_released(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    for (int i = 0; i < 4; i++) {
        record(folder, QStringLiteral("0000000000000099-%1").arg(i), QStringLiteral("h%1.wav").arg(i),
               1754714000 + i * 10, 900 + i, 400000);
    }
    s.pass();
    // The three oldest are in an export when the limit drops to 1 MB: only the newest can go, and 1.2 MB stays.
    const QVariantMap exporting =
        s.store.selectRecordings({QStringLiteral("0000000000000099-0"), QStringLiteral("0000000000000099-1"),
                                  QStringLiteral("0000000000000099-2")});
    s.store.setLimitMb(1);
    s.pass();
    expect("an export's selection keeps the store over its limit",
           s.store.usedBytes() == 1200000 && !QFileInfo::exists(folder + QStringLiteral("/h3.wav")));
    s.store.releaseSelection(exporting.value("selection").toInt());
    expect("the export's end lets the oldest go at once", removed_soon(folder + QStringLiteral("/h0.wav")));
    s.pass();
    expect("down to 95%", s.store.usedBytes() == 800000 && QFileInfo::exists(folder + QStringLiteral("/h1.wav")));

    // A recording larger than the limit plays as it is published: the rest go, it stays until the player lets it go.
    s.store.setPlayingPath(folder + QStringLiteral("/h4.wav"));
    record(folder, QStringLiteral("0000000000000099-4"), QStringLiteral("h4.wav"), 1754714040, 904, 1200000);
    s.pass();
    expect("the recording that plays keeps the store over its limit",
           QFileInfo::exists(folder + QStringLiteral("/h4.wav"))
               && !QFileInfo::exists(folder + QStringLiteral("/h2.wav")) && s.store.usedBytes() == 1200000);
    s.store.setPlayingPath(QString());
    expect("stopping it lets it go at once", removed_soon(folder + QStringLiteral("/h4.wav")));
    s.pass();
    expect("and nothing is left over the limit", s.store.usedBytes() == 0);

    // One that uploads as it is published stays too; the upload has no release, and the next timed pass removes it.
    s.uploading = {QStringLiteral("h5.wav")};
    s.uploadingSet = true;
    record(folder, QStringLiteral("0000000000000099-5"), QStringLiteral("h5.wav"), 1754714050, 905, 1200000);
    s.pass();
    expect("an upload keeps the store over its limit",
           QFileInfo::exists(folder + QStringLiteral("/h5.wav")) && s.store.usedBytes() == 1200000);
    s.uploadingSet = false;
    s.pass();
    expect("the next timed pass after the upload removes it",
           !QFileInfo::exists(folder + QStringLiteral("/h5.wav")) && s.store.usedBytes() == 0);
}

/* A host with a player, share and a transfer it finishes when the test says (Android's run on their own threads). */
class ServedHost : public dsd_qt::DecoderHost {
  public:
    QStringList played;
    QStringList shared;
    QStringList transferred;
    /* The files an export was handed, as the store selected them. */
    QList<dsd_file_identity_t> selectedFiles;
    RecordingSelectionToken transferToken;
    using DecoderHost::openRecordingInput;
    using DecoderHost::RecordingInput;
    using DecoderHost::recordingInputCheck;
    bool zipTransfers = true;

    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return {};
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}

    bool
    playbackSupported() const override {
        return true;
    }

    bool
    shareSupported() const override {
        return true;
    }

    bool
    recordingExportSupported() const override {
        return zipTransfers;
    }

    void
    playing(const QString& path) {
        publishPlayback(path, PlaybackPlaying, 1000, 4000, QString());
    }

    /* Issue #909: @p path playing as an item of a @p mode play (a sheet's parts or History's playlist). */
    void
    playingItem(const QString& path, int mode) {
        PlaybackReading reading;
        reading.generation = 3;
        reading.mode = mode;
        reading.path = path;
        reading.state = PlaybackPlaying;
        reading.positionMs = 1000;
        reading.durationMs = 4000;
        reading.token = 7;
        reading.callIndex = 0;
        reading.callCount = 1;
        publishPlayback(reading);
    }

    void
    finish(int result, const QStringList& missing = {}) {
        finishRecordingTransfer(result, missing);
    }

    void
    progress(qint64 bytes, qint64 total) {
        publishRecordingTransferProgress(bytes, total);
    }

    /* What played when the player was stopped, and whether its file was still there then. */
    QStringList stoppedPaths;
    QList<bool> stoppedFileThere;

    void
    stopPlayback() override {
        stoppedPaths.append(playbackPath());
        stoppedFileThere.append(QFileInfo::exists(playbackPath()));
        DecoderHost::stopPlayback();
    }

  protected:
    void
    beginPlayback(const QString& path) override {
        played.append(path);
    }

    bool
    beginRecordingTransfer(const QStringList& paths, const QStringList& names,
                           const QList<dsd_file_identity_t>& expected, const QString& reference, bool zip,
                           const RecordingSelectionToken& token) override {
        transferToken = token;
        if (!zip) {
            return DecoderHost::beginRecordingTransfer(paths, names, expected, reference, zip, token);
        }
        selectedFiles = expected;
        transferred = paths;
        return true;
    }

    bool
    shareRecordingFile(const QString& path, const QString& title) override {
        (void)title;
        shared.append(path);
        return true;
    }
};

/* Issue #909: only a single recording's play holds its file. A sheet's parts and History's playlist hold none: a
 * delete goes ahead under them, and one starting lets go of the single recording held before. */
void
test_single_play_hold_only(void) {
    Roots roots;
    Store s(roots.list());
    ServedHost host;
    dsd_qt::recording_store_serve_host(&s.store, &host);
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    for (int i = 1; i <= 3; i++) {
        record(folder, QStringLiteral("0000000000000098-%1").arg(i), QStringLiteral("p%1.wav").arg(i), 1754715000 + i,
               900 + i);
    }
    s.pass();
    const auto path = [&folder](int i) { return folder + QStringLiteral("/p%1.wav").arg(i); };
    const auto id = [](int i) { return QStringLiteral("0000000000000098-%1").arg(i); };
    const auto waitGone = [&s](const QString& file) {
        const uint64_t deadline = dsd_realtime_mono_ms() + 5000U;
        while (QFileInfo::exists(file) && dsd_realtime_mono_ms() < deadline) {
            s.pass();
        }
        return !QFileInfo::exists(file);
    };

    host.playRecording(path(1));
    host.playing(path(1));
    expect("a single recording's play holds its file", host.playbackHoldPath() == path(1));
    s.store.deleteRecording(id(1));
    s.pass();
    expect("so its delete waits", QFileInfo::exists(path(1)) && s.store.deletesWaiting() == 1);
    host.playingItem(path(2), dsd_qt::DecoderHost::PlaybackParts);
    expect("parts starting hold nothing", host.playbackHoldPath().isEmpty());
    expect("and let go of the single file held before", waitGone(path(1)));
    s.store.deleteRecording(id(2));
    expect("a part playing is deleted under it", waitGone(path(2)));
    host.playingItem(path(3), dsd_qt::DecoderHost::PlaybackPlaylist);
    s.store.deleteRecording(id(3));
    expect("so is a playlist item", waitGone(path(3)) && host.playbackHoldPath().isEmpty());
    s.store.endSession();
}

/* Issue #708: the host's playback, export, save and share through the store: the file playing and the files a
 * transfer selected are held until the player and the transfer let them go, every transfer releases its selection,
 * only indexed recordings are shared, and whatever the host finds gone is looked at again. */
void
test_host_surface(void) {
    Roots roots;
    Store s(roots.list());
    ServedHost host;
    dsd_qt::recording_store_serve_host(&s.store, &host);
    // The session stays open while this case writes the journal, as the decoder writes one only during a session: a
    // pass a release or a recheck wakes may still be running when a line is appended, and with no session it could
    // compact the journal over a line it had not read.
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    for (int i = 1; i <= 4; i++) {
        record(folder, QStringLiteral("0000000000000099-%1").arg(i), QStringLiteral("h%1.wav").arg(i), 1754714000 + i,
               900 + i);
    }
    s.pass();
    const auto path = [&folder](int i) { return folder + QStringLiteral("/h%1.wav").arg(i); };
    const auto id = [](int i) { return QStringLiteral("0000000000000099-%1").arg(i); };
    const auto waitGone = [&s](const QString& file) {
        const uint64_t deadline = dsd_realtime_mono_ms() + 5000U;
        while (QFileInfo::exists(file) && dsd_realtime_mono_ms() < deadline) {
            s.pass();
        }
        return !QFileInfo::exists(file);
    };

    // Playback holds its file from Preparing until it stops.
    host.playRecording(path(1));
    expect("a play is published Preparing before the player is asked",
           host.played == QStringList{path(1)} && host.playbackState() == dsd_qt::DecoderHost::PlaybackPreparing
               && host.playbackPath() == path(1));
    host.playing(path(1));
    s.store.deleteRecording(id(1));
    s.pass();
    expect("the file playing waits for its delete", QFileInfo::exists(path(1)) && s.store.deletesWaiting() == 1);
    host.stopPlayback();
    expect("stopped", host.playbackState() == dsd_qt::DecoderHost::PlaybackStopped && host.playbackPath().isEmpty());
    expect("and deleted once it stops", waitGone(path(1)));

    // A file gone behind the store's back, played while another plays: the player stops first (the store holding
    // that file until then), the play fails as missing, and the store looks again.
    record(folder, id(8), QStringLiteral("h8.wav"), 1754714008, 908);
    s.pass();
    host.playRecording(path(8));
    host.playing(path(8));
    s.store.deleteRecording(id(8));
    s.pass();
    expect("another file plays, its delete waiting", QFileInfo::exists(path(8)) && s.store.deletesWaiting() == 1);
    host.stoppedPaths.clear();
    host.stoppedFileThere.clear();
    QFile::remove(path(2));
    host.playRecording(path(2));
    expect("the player stops the one playing first, its file still held",
           host.stoppedPaths == QStringList{path(8)} && host.stoppedFileThere == QList<bool>{true});
    expect("a missing file fails as missing, never reaching the player",
           host.playbackState() == dsd_qt::DecoderHost::PlaybackFailed
               && host.playbackError() == QStringLiteral("missing") && host.played.size() == 2
               && host.playbackPath() == path(2));
    s.pass();
    expect("and leaves the index", !s.store.isIndexedPath(path(2)));
    expect("the stopped file's delete runs once nothing plays it", waitGone(path(8)));
    expect("two recordings are left", s.store.recordingCount() == 2);

    // A save copies one recording and releases its selection; a delete asked meanwhile waits for it. The release
    // (delivered by s.pass()) wakes the worker, which removes the original on its own time: the original's bytes are
    // read before the save, and the copy is checked once the original is gone, never racing that delete.
    const QString dest = roots.dir.path() + QStringLiteral("/saved.wav");
    const QByteArray original = read_all(path(3));
    QSignalSpy finished(&host, &dsd_qt::DecoderHost::recordingTransferFinished);
    const QVariantMap one = s.store.selectRecordings({id(3)});
    expect("a save begins", host.saveRecording(one, QUrl::fromLocalFile(dest).toString()));
    s.store.deleteRecording(id(3));
    s.pass();
    expect("it finished, once, done", finished.count() == 1
                                          && finished.at(0).at(0).toInt() == one.value("selection").toInt()
                                          && finished.at(0).at(1).toInt() == dsd_qt::DecoderHost::TransferDone);
    expect("and the delete it held ran after", waitGone(path(3)));
    expect("the save wrote the file, which outlives the original", !original.isEmpty() && read_all(dest) == original);

    // An export runs on the host's own time; a second one meanwhile is refused as busy, releasing its own selection.
    record(folder, id(5), QStringLiteral("h5.wav"), 1754714005, 905);
    s.pass();
    finished.clear();
    const QVariantMap all = s.store.selectForExport(kUidA, QStringLiteral("all"));
    const QVariantMap again = s.store.selectRecordings({id(4)});
    expect("an export begins", host.exportRecordings(all, QStringLiteral("content://picked/zip"))
                                   && host.recordingTransferActive() && host.transferred.size() == 2);
    expect("its total starts as the selection's",
           host.recordingTransferTotalBytes() == all.value("bytes").toLongLong());
    host.progress(500, 1000);
    expect("and is the one the platform publishes",
           host.recordingTransferBytes() == 500 && host.recordingTransferTotalBytes() == 1000);
    expect("a second is refused", !host.saveRecording(again, QStringLiteral("content://picked/one")));
    s.pass();
    expect("as busy, with its own selection",
           finished.count() == 1 && finished.at(0).at(0).toInt() == again.value("selection").toInt()
               && finished.at(0).at(1).toInt() == dsd_qt::DecoderHost::TransferBusy && host.recordingTransferActive());
    s.store.deleteRecording(id(4));
    s.pass();
    expect("the export still holds its files", QFileInfo::exists(path(4)));
    QFile::remove(path(5));
    host.finish(dsd_qt::DecoderHost::TransferDone, {path(5)});
    s.pass();
    expect("its end counts the file it found gone",
           finished.count() == 2 && finished.at(1).at(0).toInt() == all.value("selection").toInt()
               && finished.at(1).at(2).toInt() == 1 && !host.recordingTransferActive());
    expect("which leaves the index", !s.store.isIndexedPath(path(5)));
    expect("and lets the waiting delete run", waitGone(path(4)));

    // A save whose file is already gone: the host is handed it unlooked (no stat on the GUI thread), and the save
    // finds it gone on its own path, finishes as missing and has the store look again.
    record(folder, id(9), QStringLiteral("h9.wav"), 1754714009, 909);
    s.pass();
    finished.clear();
    const QVariantMap gone = s.store.selectRecordings({id(9)});
    QFile::remove(path(9));
    expect("the save begins", host.saveRecording(gone, QUrl::fromLocalFile(dest).toString()));
    s.pass();
    expect("and finishes as missing, counting it",
           finished.count() == 1 && finished.at(0).at(1).toInt() == dsd_qt::DecoderHost::TransferMissing
               && finished.at(0).at(2).toInt() == 1);
    expect("which leaves the index", !s.store.isIndexedPath(path(9)));

    // A host with no zip refuses an export, and the selection is still released.
    host.zipTransfers = false;
    record(folder, id(6), QStringLiteral("h6.wav"), 1754714006, 906);
    s.pass();
    finished.clear();
    expect("no zip here", !host.exportRecordings(s.store.selectForExport(kUidA, QStringLiteral("all")),
                                                 QStringLiteral("content://picked/zip")));
    s.store.deleteRecording(id(6));
    s.pass();
    expect("refused as unsupported",
           finished.count() == 1 && finished.at(0).at(1).toInt() == dsd_qt::DecoderHost::TransferUnsupported);
    expect("and released", waitGone(path(6)));

    // Share: indexed recordings only; a missing one is looked at again.
    record(folder, id(7), QStringLiteral("h7.wav"), 1754714007, 907);
    s.pass();
    const QString stray = folder + QStringLiteral("/stray.wav");
    make_file(stray, 100);
    expect("an indexed recording is shared", host.shareRecording(path(7), QStringLiteral("County")));
    expect("anything else is not",
           !host.shareRecording(stray, QStringLiteral("County")) && host.shared == QStringList{path(7)});
    QFile::remove(path(7));
    expect("a missing one is not shared", !host.shareRecording(path(7), QStringLiteral("County")));
    s.pass();
    expect("and leaves the index", !s.store.isIndexedPath(path(7)));
    s.store.endSession();
}

/* Issue #902: a transfer reads only the files the store selected. Each file goes to the host with its size and
 * modification time as selected; a save whose file is another one now (here another size) writes nothing, is told as
 * changed and has the store look again, so the next selection takes the file as it is now; a selection made before the
 * store read its storage again (two owners' folders may have swapped) is refused as changed at once; one made after
 * the read is saved. The check itself: the file selected, one gone, and a link or another time are told apart. */
void
test_transfer_reads_only_the_files_selected(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    ServedHost host;
    dsd_qt::recording_store_serve_host(&s.store, &host);
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString r1 = folder + QStringLiteral("/r1.wav");
    const QString r2 = folder + QStringLiteral("/r2.wav");
    const QString id1 = QStringLiteral("0000000000000915-1");
    const QString id2 = QStringLiteral("0000000000000915-2");
    record_at(folder, id1, QStringLiteral("r1.wav"), 1754000000, 1, 1000, 1754000000);
    record_at(folder, id2, QStringLiteral("r2.wav"), 1754000100, 2, 2000, 1754000100);
    s.pass();
    QSignalSpy finished(&host, &dsd_qt::DecoderHost::recordingTransferFinished);
    const QString dest = roots.dir.path() + QStringLiteral("/saved.wav");
    const QString destUrl = QUrl::fromLocalFile(dest).toString();
    const auto lastResult = [&finished]() { return finished.isEmpty() ? -1 : finished.last().at(1).toInt(); };

    // An export hands the host every file as selected.
    const QVariantMap all = s.store.selectForExport(QString(kUidA), QStringLiteral("all"));
    expect("selected: the files go with their size and time",
           host.exportRecordings(all, QStringLiteral("content://picked/zip")) && host.selectedFiles.size() == 2
               && host.selectedFiles.at(0).size == 1000 && host.selectedFiles.at(0).mtime_s == 1754000000
               && host.selectedFiles.at(1).size == 2000 && host.selectedFiles.at(1).mtime_s == 1754000100);
    host.finish(dsd_qt::DecoderHost::TransferDone);
    s.pass();

    // Another file at the name since the selection: nothing written, told as changed, and looked at again.
    const QVariantMap one = s.store.selectRecordings({id1});
    const int other = s.store.selectRecordings({id2}).value(QStringLiteral("selection")).toInt();
    make_file(r1, 1500);
    finished.clear();
    expect("changed: the save begins", host.saveRecording(one, destUrl));
    s.pass();
    expect("changed: nothing written, told as changed",
           lastResult() == dsd_qt::DecoderHost::TransferChanged && !QFileInfo::exists(dest));
    s.pass();
    // The store does not track outside changes: its folder read again whole, the next selection takes the file as the
    // read found it, and one made before the read is stale.
    expect("changed: a selection from before the read is stale", !s.store.selectionCurrent(other));
    s.store.releaseSelection(other);
    const QVariantMap retry = s.store.selectRecordings({id1});
    s.store.releaseSelection(retry.value(QStringLiteral("selection")).toInt());
    expect("changed: its folder read again, the next selection takes the file as it is now",
           retry.value(QStringLiteral("sizes")).toList().value(0).toLongLong() == 1500);

    // A read of the storage again between the selection and the transfer: refused at once.
    const QVariantMap before = s.store.selectRecordings({id2});
    expect("read again: the storage goes", QDir().rename(ext, away));
    s.pass();
    expect("read again: the storage comes back", QDir().rename(away, ext));
    s.pass();
    finished.clear();
    expect("read again: the save is refused", !host.saveRecording(before, destUrl));
    s.pass();
    expect("read again: as changed, nothing written",
           lastResult() == dsd_qt::DecoderHost::TransferChanged && !QFileInfo::exists(dest));
    const QVariantMap after = s.store.selectRecordings({id2});
    finished.clear();
    expect("read again: a selection made after the read is saved", host.saveRecording(after, destUrl));
    s.pass();
    expect("read again: saved", lastResult() == dsd_qt::DecoderHost::TransferDone && read_all(dest) == read_all(r2));

    // The check alone.
    dsd_file_identity_t known{};
    expect("check: a file read", dsd_path_file_identity(dsd_qt::platform_path(r2).constData(), &known) == 0);
    using Input = ServedHost::RecordingInput;
    expect("check: the file selected", ServedHost::recordingInputCheck(r2, known) == Input::Selected);
    expect("check: gone",
           ServedHost::recordingInputCheck(folder + QStringLiteral("/none.wav"), known) == Input::Missing);
    set_mtime(r2, 1754000101);
    expect("check: another time", ServedHost::recordingInputCheck(r2, known) == Input::Changed);
#ifndef _WIN32
    QFile::remove(r2);
    expect("check: a link", QFile::link(dest, r2) && ServedHost::recordingInputCheck(r2, known) == Input::Changed);
#endif
}

/* Issue #708: the store has the user's storage limit before its first pass, and every change after. */
void
test_limit_follows_the_preference(void) {
    Roots roots;
    dsd_qt::AppPrefs prefs;
    prefs.setRecordingLimitMb(250);
    Store s(roots.list(), false);
    dsd_qt::recording_store_follow_limit(&s.store, &prefs);
    expect("the user's limit before the store starts", s.store.limitMb() == 250);
    s.start();
    prefs.setRecordingLimitMb(0);
    expect("no limit, as soon as it is chosen", s.store.limitMb() == 0);
    prefs.setRecordingLimitMb(5000);
    expect("and every change after", s.store.limitMb() == 5000);
    prefs.setRecordingLimitMb(1000);
}

/* --- Issue #809: recordings by the Signal search detection they were heard on. --- */

/* @p line (a publish line) with its `search` set to @p key: a JSON value, so a test can write a number or a fraction. */
QByteArray
with_search(const QByteArray& line, const QJsonValue& key) {
    QJsonObject o = QJsonDocument::fromJson(line).object();
    o.insert(QStringLiteral("search"), key);
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

/* A recording published into @p folder, heard on the detection @p key (none when empty). */
void
record_heard(const QString& folder, const QString& id, const QString& file, qint64 start, qint64 row,
             const QString& key, qint64 bytes = 1000) {
    make_file(folder + QLatin1Char('/') + file, bytes);
    const QByteArray line = publish_line(id, file, start, row);
    append(journal_of(folder), key.isEmpty() ? line : with_search(line, key));
}

/* The search key a publish line parses to; "<invalid>" when the line does not parse at all. */
QString
parsed_search(const QJsonValue& key) {
    dsd_qt::RecordingJournalRecord r;
    const QByteArray line = publish_line(QStringLiteral("X-1"), QStringLiteral("a.wav"), 1, 1);
    const QByteArray keyed = key.isUndefined() ? line : with_search(line, key);
    return dsd_qt::recording_journal_parse(keyed.chopped(1), &r) ? r.search : QStringLiteral("<invalid>");
}

QStringList
ids_of(const QVariantList& maps) {
    QStringList ids;
    for (const QVariant& map : maps) {
        ids.append(map.toMap().value(QStringLiteral("id")).toString());
    }
    return ids;
}

double
revision_of(const RecordingStore& store) {
    return store.property("revision").toDouble();
}

/* The key is two non-zero decimal u64s: anything else (an older build's line, a hand edit) is no key, never a reason to
 * drop the recording. A compaction writes a key back as read and never adds one. */
void
expect_search_key_parse(void) {
    expect("search key: a valid key", parsed_search(QStringLiteral("12:34")) == QStringLiteral("12:34"));
    expect("search key: missing", parsed_search(QJsonValue(QJsonValue::Undefined)).isEmpty());
    expect("search key: a zero session", parsed_search(QStringLiteral("0:5")).isEmpty());
    expect("search key: a zero hit", parsed_search(QStringLiteral("5:0")).isEmpty());
    expect("search key: a fraction",
           parsed_search(QStringLiteral("1.5:2")).isEmpty() && parsed_search(QStringLiteral("1:2.0")).isEmpty());
    expect("search key: negative",
           parsed_search(QStringLiteral("-1:2")).isEmpty() && parsed_search(QStringLiteral("1:-2")).isEmpty());
    expect("search key: a leading zero, a sign, spaces",
           parsed_search(QStringLiteral("01:2")).isEmpty() && parsed_search(QStringLiteral("+1:2")).isEmpty()
               && parsed_search(QStringLiteral(" 1:2")).isEmpty() && parsed_search(QStringLiteral("1:2 ")).isEmpty());
    expect("search key: one part, three parts, an empty part, empty",
           parsed_search(QStringLiteral("12")).isEmpty() && parsed_search(QStringLiteral("1:2:3")).isEmpty()
               && parsed_search(QStringLiteral(":2")).isEmpty() && parsed_search(QStringLiteral(":5")).isEmpty()
               && parsed_search(QStringLiteral("1:")).isEmpty() && parsed_search(QStringLiteral(":")).isEmpty()
               && parsed_search(QStringLiteral("")).isEmpty());
    expect("search key: 21 digits", parsed_search(QStringLiteral("100000000000000000000:1")).isEmpty()
                                        && parsed_search(QStringLiteral("1:100000000000000000000")).isEmpty());
    expect("search key: past UINT64_MAX", parsed_search(QStringLiteral("18446744073709551616:1")).isEmpty()
                                              && parsed_search(QStringLiteral("1:99999999999999999999")).isEmpty());
    expect("search key: UINT64_MAX itself", parsed_search(QStringLiteral("18446744073709551615:18446744073709551615"))
                                                == QStringLiteral("18446744073709551615:18446744073709551615"));
    expect("search key: a number, not a string", parsed_search(QJsonValue(12)).isEmpty());
    expect("search key: a bad key keeps the recording", parsed_search(QStringLiteral("0:5")) != "<invalid>");

    dsd_qt::RecordingJournalRecord plain;
    plain.file = QStringLiteral("a.wav");
    expect("search key: the writer adds none when there is none",
           !QJsonDocument::fromJson(dsd_qt::recording_journal_publish_line(plain)).object().contains("search"));
    plain.search = QStringLiteral("9007199254740993:7");
    expect("search key: the writer writes it as read",
           QJsonDocument::fromJson(dsd_qt::recording_journal_publish_line(plain)).object().value("search").toString()
               == QStringLiteral("9007199254740993:7"));
}

void
test_search_key_parse_and_compaction(void) {
    expect_search_key_parse();
    Roots roots;
    QString folder;
    {
        Store s(roots.list());
        folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
        record_heard(folder, QStringLiteral("0000000000000091-1"), QStringLiteral("keyed.wav"), 1754720000, 1,
                     QStringLiteral("3:4"));
        record_heard(folder, QStringLiteral("0000000000000091-2"), QStringLiteral("plain.wav"), 1754720010, 2,
                     QString());
        record_heard(folder, QStringLiteral("0000000000000091-3"), QStringLiteral("zero.wav"), 1754720020, 3,
                     QStringLiteral("0:5"));
        for (int i = 0; i < 4; i++) {
            record_heard(folder, QStringLiteral("0000000000000091-%1").arg(10 + i), QStringLiteral("x%1.wav").arg(i),
                         1754720100 + i, 10 + i, QStringLiteral("3:4"));
        }
        s.pass();
        expect("search key: listed by the valid key only",
               s.store.recordingCountForDetection(QStringLiteral("3:4")) == 5
                   && s.store.recordingCountForDetection(QStringLiteral("0:5")) == 0);
        QStringList gone;
        for (int i = 0; i < 4; i++) {
            gone.append(QStringLiteral("0000000000000091-%1").arg(10 + i));
        }
        s.store.deleteRecordings(gone);
        s.pass();
        s.store.endSession();
        s.pass();
        expect("search key: the journal is compacted", s.store.compactionsForTest() == 1);
        QMap<QString, QJsonObject> lines;
        for (const QByteArray& line : read_all(journal_of(folder)).split('\n')) {
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            if (!o.isEmpty()) {
                lines.insert(o.value("file").toString(), o);
            }
        }
        expect("search key: compaction keeps a key",
               lines.size() == 3 && lines.value("keyed.wav").value("search").toString() == QStringLiteral("3:4"));
        expect("search key: compaction never adds one",
               !lines.value("plain.wav").contains("search") && !lines.value("zero.wav").contains("search"));
    }
    Store s(roots.list());
    expect("search key: a new store reads the rewrite",
           s.store.recordingCountForDetection(QStringLiteral("3:4")) == 1 && s.store.recordingCount() == 3);
}

/* The detection index follows every way a recording comes and goes: a publish, a delete, the limit, a file gone behind
 * the store's back, a folder read again or removed, a path a later publish reuses; a pending recording linked to its
 * row stays listed. */
void
test_detection_index(void) {
    const QString k1 = QStringLiteral("7:1");
    const QString k2 = QStringLiteral("7:2");
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    s.store.endSession();
    record_heard(folder, QStringLiteral("0000000000000092-1"), QStringLiteral("a.wav"), 1754721000, 1, k1);
    record_heard(folder, QStringLiteral("0000000000000092-2"), QStringLiteral("b.wav"), 1754721010, 2, k1);
    record_heard(folder, QStringLiteral("0000000000000092-3"), QStringLiteral("c.wav"), 1754721020, 3, k2);
    record_heard(folder, QStringLiteral("0000000000000092-4"), QStringLiteral("d.wav"), 1754721030, 4, QString());
    s.pass();
    expect("detection index: each detection counts its own",
           s.store.recordingCountForDetection(k1) == 2 && s.store.recordingCountForDetection(k2) == 1
               && s.store.recordingCountForDetection(QStringLiteral("9:9")) == 0
               && s.store.recordingCountForDetection(QString()) == 0
               && s.store.recordingsForDetection(QString()).isEmpty());

    s.store.deleteRecording(QStringLiteral("0000000000000092-2"));
    s.pass();
    expect("detection index: a delete",
           s.store.recordingCountForDetection(k1) == 1
               && ids_of(s.store.recordingsForDetection(k1)) == QStringList{QStringLiteral("0000000000000092-1")});

    QFile::remove(folder + QStringLiteral("/c.wav"));
    s.store.recheckRecordings({QStringLiteral("0000000000000092-3")});
    s.pass();
    expect("detection index: a file gone behind the store's back", s.store.recordingCountForDetection(k2) == 0);

    // A pending recording, then its link: listed all along.
    make_file(folder + QStringLiteral("/p.wav"), 1000);
    append(journal_of(folder),
           with_search(
               publish_line(QStringLiteral("0000000000000092-5"), QStringLiteral("p.wav"), 1754721040, -1, true), k2));
    s.pass();
    expect("detection index: a pending recording is listed", s.store.recordingCountForDetection(k2) == 1);
    append(journal_of(folder), link_line(QStringLiteral("0000000000000092-5"), 5));
    s.pass();
    const QVariantMap linked = s.store.recordingsForDetection(k2).value(0).toMap();
    expect("detection index: linked, it stays listed and names its row",
           s.store.recordingCountForDetection(k2) == 1 && linked.value("fragment").toString() == fragment(5));

    // A later publish that reuses a path owns it: the earlier id leaves its detection.
    record_heard(folder, QStringLiteral("0000000000000092-6"), QStringLiteral("a.wav"), 1754721050, 6, k2);
    s.pass();
    expect("detection index: a reused path moves to the later detection",
           s.store.recordingCountForDetection(k1) == 0 && s.store.recordingCountForDetection(k2) == 2);

    // The limit takes the oldest first: here a recording heard long before the rest.
    const QString old = QStringLiteral("8:1");
    record_heard(folder, QStringLiteral("0000000000000092-7"), QStringLiteral("old.wav"), 1754720000, 7, old, 600000);
    s.pass();
    expect("detection index: an old recording listed", s.store.recordingCountForDetection(old) == 1);
    s.store.setLimitMb(1);
    record_heard(folder, QStringLiteral("0000000000000092-8"), QStringLiteral("new.wav"), 1754721060, 8, QString(),
                 400000);
    s.pass();
    expect("detection index: the limit", s.store.removedByLimit() == 1 && s.store.recordingCountForDetection(old) == 0
                                             && s.store.recordingCountForDetection(k2) == 2);
    s.store.setLimitMb(0);
    s.pass();

    // A journal replaced behind the store's back is read again: only what it names now is listed.
    QFile::remove(journal_of(folder));
    record_heard(folder, QStringLiteral("0000000000000092-9"), QStringLiteral("e.wav"), 1754721070, 9,
                 QStringLiteral("8:2"));
    s.pass();
    expect("detection index: a folder read again",
           s.store.recordingCountForDetection(k2) == 0
               && s.store.recordingCountForDetection(QStringLiteral("8:2")) == 1);
    expect("detection index: the folder removed", QDir(folder).removeRecursively());
    s.pass();
    expect("detection index: a removed folder takes its recordings",
           s.store.recordingCountForDetection(QStringLiteral("8:2")) == 0);
}

/* Storage that goes away keeps its recordings listed under their detection, as unavailable, and plays again when it
 * comes back; the revision moves both times, so an open list reads the change. */
void
test_detection_root_away_keeps_membership(void) {
    const QString key = QStringLiteral("11:22");
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    s.store.endSession();
    record_heard(folder, QStringLiteral("0000000000000093-1"), QStringLiteral("a.wav"), 1754722000, 1, key);
    record_heard(folder, QStringLiteral("0000000000000093-2"), QStringLiteral("b.wav"), 1754722010, 2, key);
    s.pass();
    const auto all_available = [&s, &key](bool want) {
        const QVariantList maps = s.store.recordingsForDetection(key);
        int matching = 0;
        for (const QVariant& map : maps) {
            matching += map.toMap().value("available").toBool() == want ? 1 : 0;
        }
        return maps.size() == 2 && matching == 2;
    };
    expect("root away: listed and available", all_available(true));
    double revision = revision_of(s.store);

    expect("root away: the storage goes", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
    s.pass();
    expect("root away: still listed, unavailable",
           s.store.recordingCountForDetection(key) == 2 && all_available(false));
    expect("root away: the revision moved", revision_of(s.store) > revision);
    revision = revision_of(s.store);

    expect("root away: the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
    s.pass();
    expect("root away: back, available again", s.store.recordingCountForDetection(key) == 2 && all_available(true));
    expect("root away: the revision moved again", revision_of(s.store) > revision);
}

/* Newest first: by start, a tie by the order they were published in, latest first; each map carries what the list
 * shows and the History fragment to open. */
void
test_detection_order_newest_first(void) {
    const QString key = QStringLiteral("5:6");
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    s.store.endSession();
    record_heard(folder, QStringLiteral("0000000000000094-1"), QStringLiteral("a.wav"), 1754723100, 1, key);
    record_heard(folder, QStringLiteral("0000000000000094-2"), QStringLiteral("b.wav"), 1754723300, 2, key);
    record_heard(folder, QStringLiteral("0000000000000094-3"), QStringLiteral("c.wav"), 1754723200, 3, key);
    record_heard(folder, QStringLiteral("0000000000000094-4"), QStringLiteral("d.wav"), 1754723200, 4, key);
    s.pass();
    const QVariantList maps = s.store.recordingsForDetection(key);
    expect("order: start desc, then publish order desc",
           ids_of(maps)
               == QStringList{QStringLiteral("0000000000000094-2"), QStringLiteral("0000000000000094-4"),
                              QStringLiteral("0000000000000094-3"), QStringLiteral("0000000000000094-1")});
    const QVariantMap first = maps.value(0).toMap();
    expect("order: the map's fields",
           first.value("path").toString() == folder + QStringLiteral("/b.wav")
               && first.value("fileName").toString() == QStringLiteral("b.wav")
               && first.value("start").toLongLong() == 1754723300 && first.value("durationMs").toLongLong() == 4000
               && first.value("rate").toInt() == 8000 && first.value("bytes").toLongLong() == 1000
               && first.contains("folderUid") && !first.value("failed").toBool() && first.value("available").toBool()
               && first.value("system").toString() == QStringLiteral("County")
               && first.value("target").toString() == QStringLiteral("Fire")
               && first.value("source").toString() == QStringLiteral("1234")
               && first.value("fragment").toString() == fragment(2));
}

/* Storage that comes back has each folder read again, slice by slice. Until the read is complete, the GUI reads the
 * folder as it was indexed: a query between two slices (a delivery queued earlier that the GUI handles then) never finds
 * a detection's or a History row's recordings gone, so an open detection list does not close on its own, and a listed
 * recording can still be played. */
void
test_queries_between_rebuild_slices(void) {
    const QString key = QStringLiteral("13:17");
    const QString pendingId = QStringLiteral("0000000000000095-1");
    const QString keyedId = QStringLiteral("0000000000000095-2");
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    s.store.endSession();
    // A pending recording first and its link last, many recordings of no detection between, and the detection's other
    // recording after them: every slice but the last leaves something of the folder unread.
    QByteArray journal;
    make_file(folder + QStringLiteral("/p.wav"), 1000);
    journal.append(with_search(publish_line(pendingId, QStringLiteral("p.wav"), 1754724000, -1, true), key));
    for (int i = 0; i < 400; i++) {
        const QString file = QStringLiteral("n%1.wav").arg(i);
        make_file(folder + QLatin1Char('/') + file, 1000);
        journal.append(publish_line(QStringLiteral("0000000000000095-%1").arg(100 + i), file, 1754724100 + i, -1));
    }
    make_file(folder + QStringLiteral("/k.wav"), 1000);
    journal.append(with_search(publish_line(keyedId, QStringLiteral("k.wav"), 1754724010, 1), key));
    journal.append(link_line(pendingId, 2));
    append(journal_of(folder), journal);
    s.pass();
    const QStringList listed = ids_of(s.store.recordingsForDetection(key));
    expect("between slices: the detection's two recordings", listed == QStringList{keyedId, pendingId});
    // A History export's batched read of names sees the same recordings, in the same order.
    const QList<QStringList> names{{QStringLiteral("Explore/p.wav"), QStringLiteral("Explore/k.wav")},
                                   {QStringLiteral("Explore/k.wav")}};
    const auto whole = [&s, &key, &listed, &pendingId, &keyedId, &names]() {
        return ids_of(s.store.recordingsForDetection(key)) == listed && s.store.recordingCountForDetection(key) == 2
               && s.store.hasRecordingForFragments({fragment(1)}) && s.store.hasRecordingForFragments({fragment(2)})
               && ids_of(s.store.recordingsForFragments({fragment(1), fragment(2)})) == QStringList{pendingId, keyedId}
               && s.store.recordingEntryNames({{fragment(1), fragment(2)}, {fragment(1)}}) == names;
    };
    expect("between slices: indexed whole", whole());
    // Issue #902 B12: an export and each delete request take the folder whole, and are refused while it is read again.
    const QVariantMap before = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(before.value(QStringLiteral("selection")).toInt());
    const QStringList exportNames = before.value(QStringLiteral("names")).toStringList();
    expect("between slices: the export, before", exportNames.size() == 402);
    const QString uid = s.store.folders().value(0).toMap().value(QStringLiteral("uid")).toString();
    QStringList everyId{pendingId, keyedId};
    for (int i = 0; i < 400; i++) {
        everyId.append(QStringLiteral("0000000000000095-%1").arg(100 + i));
    }
    std::sort(everyId.begin(), everyId.end());
    const auto exportWhole = [&s, &exportNames]() {
        const QVariantMap selection = s.store.selectForExport(QString(), QStringLiteral("all"));
        s.store.releaseSelection(selection.value(QStringLiteral("selection")).toInt());
        return selection.value(QStringLiteral("names")).toStringList() == exportNames
               && selection.value(QStringLiteral("count")).toInt() == 402
               && selection.value(QStringLiteral("bytes")).toLongLong() == 402000;
    };
    // Each request is taken back before anything is deleted: the files are held meanwhile, so a pass a request wakes
    // leaves every one of them queued, and that pass is over before the hold ends.
    const auto deletesWhole = [&s, &uid, &everyId]() {
        const QVariantMap held = s.store.selectRecordings(everyId);
        const bool all = s.store.deleteAll() && s.taken() == everyId;
        const bool folder = s.store.deleteFolder(uid) && s.taken() == everyId;
        const bool ids = s.store.deleteRecordings(everyId) && s.taken() == everyId;
        s.pass();
        s.store.releaseSelection(held.value(QStringLiteral("selection")).toInt());
        return held.value(QStringLiteral("count")).toInt() == 402 && all && folder && ids;
    };
    expect("between slices: the export and deletes, before", exportWhole() && deletesWhole());
    // Refused: nothing selected or held, nothing queued, and the export says why.
    const auto refused = [&s, &uid, &everyId]() {
        const QVariantMap selection = s.store.selectForExport(QString(), QStringLiteral("all"));
        const bool exportRefused = selection.value(QStringLiteral("selection")).toInt() == 0
                                   && selection.value(QStringLiteral("count")).toInt() == 0
                                   && selection.value(QStringLiteral("loading")).toBool();
        const bool deletesRefused = !s.store.deleteAll() && !s.store.deleteFolder(uid)
                                    && !s.store.deleteRecordings(everyId) && !s.store.deleteRecording(everyId.value(0))
                                    && !s.store.deleteUnfinished();
        return exportRefused && deletesRefused && s.store.takeDeleteQueueForTest().isEmpty();
    };

    expect("between slices: the storage goes", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
    s.pass();
    expect("between slices: the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
    int slices = 0;
    int partial = 0;
    int shareHeld = 0;
    int accepted = 0;
    s.store.setSliceHookForTest([&](const QString& path) {
        if (path != folder) {
            return;
        }
        slices++;
        partial += whole() ? 0 : 1;
        accepted += refused() ? 0 : 1;
        // A share's hold (selectPath()) is refused too (r12): the rows held from before the read again are not
        // available (R78), and a save between slices is refused as loading.
        const int selection = s.store.selectPath(folder + QStringLiteral("/k.wav"));
        if (selection > 0) {
            s.store.releaseSelection(selection);
            shareHeld++;
        }
        const QVariantMap save = s.store.selectRecordings({keyedId});
        accepted +=
            save.value(QStringLiteral("selection")).toInt() == 0 && save.value(QStringLiteral("loading")).toBool() ? 0
                                                                                                                   : 1;
    });
    s.pass();
    s.store.setSliceHookForTest({});
    expect("between slices: read again slice by slice", slices >= 3);
    expect("between slices: every query read the folder whole", partial == 0);
    expect("between slices: no share took a held row", shareHeld == 0);
    const int after = s.store.selectPath(folder + QStringLiteral("/k.wav"));
    expect("between slices: read again, its share is held", after > 0);
    s.store.releaseSelection(after);
    expect("between slices: every export, save and delete request was refused", accepted == 0);
    expect("between slices: read again, whole", whole() && exportWhole() && deletesWhole());
}

/* Issue #902 B12: storage that comes back changed is read whole before any export or delete runs. While it was away
 * two owners' folders swapped places, a recording's file was replaced by a newer publish at the same path, and a new
 * owner's folder of 300 recordings appeared. Through every slice of that read, an export and every delete request are
 * refused (a folder delete then would have taken the other owner's recordings, an export part of the new folder or
 * the replaced file under its old date); once it is read, each takes exactly what it should. */
void
test_changed_storage_is_read_before_bulk_requests(void) {
    constexpr int kEach = 200;
    constexpr int kNew = 300;
    const QDate today(2026, 11, 21);
    const QTimeZone utc(QTimeZone::UTC);
    const qint64 yesterdayNoon = QDateTime(today.addDays(-1), QTime(12, 0), utc).toSecsSinceEpoch();
    const qint64 todayNoon = QDateTime(today, QTime(12, 0), utc).toSecsSinceEpoch();
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
    s.store.endSession();
    const auto fill = [](const QString& folder, const QString& prefix, int count, qint64 firstStart) {
        QByteArray journal;
        for (int i = 0; i < count; i++) {
            const QString file = QStringLiteral("r%1.wav").arg(i);
            make_file(folder + QLatin1Char('/') + file, 1000);
            journal.append(publish_line(prefix + QString::number(i), file, firstStart + i, -1));
        }
        append(journal_of(folder), journal);
    };
    fill(a, QStringLiteral("0000000000000301-"), kEach, 1754000000);
    fill(b, QStringLiteral("0000000000000302-"), kEach, 1755000000);
    // A's first recording is yesterday's.
    record(a, QStringLiteral("0000000000000301-y"), QStringLiteral("y.wav"), yesterdayNoon, 1);
    s.pass();
    QStringList idsA;
    QStringList idsB;
    for (int i = 0; i < kEach; i++) {
        idsA.append(QStringLiteral("0000000000000301-%1").arg(i));
        idsB.append(QStringLiteral("0000000000000302-%1").arg(i));
    }

    // Away: the two folders swap places, y.wav is replaced by today's recording at the same path, and a new owner's
    // folder appears.
    expect("changed: the storage goes", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
    s.pass();
    const QString awayRoot = away + QStringLiteral("/Call recordings/");
    const QString awayA = awayRoot + QFileInfo(a).fileName();
    const QString awayB = awayRoot + QFileInfo(b).fileName();
    record(awayA, QStringLiteral("0000000000000301-t"), QStringLiteral("y.wav"), todayNoon, 2, 2000);
    expect("changed: the folders swap", QDir().rename(awayA, awayRoot + QStringLiteral("swap"))
                                            && QDir().rename(awayB, awayA)
                                            && QDir().rename(awayRoot + QStringLiteral("swap"), awayB));
    const QString awayC = awayRoot + QStringLiteral("Newcomer (5e6f7a8b)");
    QDir().mkpath(awayC);
    {
        const QJsonObject meta{{QStringLiteral("uid"), QString(kUidC)},
                               {QStringLiteral("kind"), QStringLiteral("saved")},
                               {QStringLiteral("name"), QStringLiteral("Newcomer")}};
        expect("changed: a new folder", write_private(awayC + QStringLiteral("/folder.json"),
                                                      QJsonDocument(meta).toJson(QJsonDocument::Compact), "wb"));
    }
    fill(awayC, QStringLiteral("0000000000000303-"), kNew, 1756000000);
    expect("changed: the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));

    int slices = 0;
    int accepted = 0;
    s.store.setSliceHookForTest([&](const QString&) {
        slices++;
        const QVariantMap selection = s.store.selectForExportOn(QString(), QStringLiteral("all"), today, utc);
        const bool exportRefused = selection.value(QStringLiteral("selection")).toInt() == 0
                                   && selection.value(QStringLiteral("loading")).toBool();
        const bool deletesRefused = !s.store.deleteFolder(kUidB) && !s.store.deleteFolder(kUidC) && !s.store.deleteAll()
                                    && !s.store.deleteRecordings(idsA);
        accepted += exportRefused && deletesRefused && s.store.takeDeleteQueueForTest().isEmpty() ? 0 : 1;
    });
    s.pass();
    s.store.setSliceHookForTest({});
    expect("changed: read slice by slice, three folders", slices >= 7);
    expect("changed: every export and delete request was refused meanwhile", accepted == 0);

    // Read whole: each owner's delete names its own recordings, wherever its folder now is.
    QStringList wantA = idsA;
    wantA.append(QStringLiteral("0000000000000301-t"));
    std::sort(wantA.begin(), wantA.end());
    std::sort(idsB.begin(), idsB.end());
    const auto queued = [&s](bool asked) {
        const QStringList ids = asked ? s.taken() : QStringList{QStringLiteral("refused")};
        return ids;
    };
    const QVariantMap held = s.store.selectForExportOn(QString(), QStringLiteral("all"), today, utc);
    expect("changed: the export, whole", held.value(QStringLiteral("count")).toInt() == 2 * kEach + 1 + kNew
                                             && !held.value(QStringLiteral("loading")).toBool());
    expect("changed: B's delete, B's recordings", queued(s.store.deleteFolder(kUidB)) == idsB);
    expect("changed: A's delete, A's recordings and the replacement", queued(s.store.deleteFolder(kUidA)) == wantA);
    expect("changed: the new owner's delete, all of its recordings",
           queued(s.store.deleteFolder(kUidC)).size() == kNew);
    s.pass();
    s.store.releaseSelection(held.value(QStringLiteral("selection")).toInt());
    // The replaced file is today's recording, not yesterday's.
    expect("changed: yesterday has nothing", export_names(s.store, QStringLiteral("yesterday"), today, utc).isEmpty());
    expect("changed: today has the replacement",
           export_names(s.store, QStringLiteral("today"), today, utc).size() == 1);
}

/* Issue #902 B12: a journal replaced in place, on storage that stayed mounted (shorter than what was read), is read
 * again whole. From before any of the folder is read again (the folder-load hook, ahead of its files and journal) an
 * export and every delete request are refused: one then would export yesterday's path, now today's recording, or
 * delete by an id the new journal no longer has. Once it is read, each takes what the new journal holds. */
void
test_journal_replaced_in_place_is_read_before_bulk_requests(void) {
    const QDate today(2026, 11, 21);
    const QTimeZone utc(QTimeZone::UTC);
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString oldId = QStringLiteral("0000000000000401-y");
    record(folder, oldId, QStringLiteral("y.wav"), QDateTime(today.addDays(-1), QTime(12, 0), utc).toSecsSinceEpoch(),
           1);
    for (int i = 0; i < 3; i++) {
        record(folder, QStringLiteral("0000000000000401-%1").arg(i), QStringLiteral("o%1.wav").arg(i), 1754000000 + i,
               2 + i);
    }
    s.pass();
    expect("in place: yesterday's recording",
           export_names(s.store, QStringLiteral("yesterday"), today, utc).size() == 1);

    // Replaced in place: one line, today's recording at yesterday's path.
    make_file(folder + QStringLiteral("/y.wav"), 2000);
    {
        const QByteArray line = publish_line(QStringLiteral("0000000000000401-t"), QStringLiteral("y.wav"),
                                             QDateTime(today, QTime(12, 0), utc).toSecsSinceEpoch(), 9);
        expect("in place: the journal is replaced", write_private(journal_of(folder), line, "wb"));
    }
    int reads = 0;
    int accepted = 0;
    s.store.setFolderLoadHookForTest([&](const QString& path) {
        if (path != folder) {
            return;
        }
        reads++;
        const QVariantMap selection = s.store.selectForExportOn(QString(), QStringLiteral("yesterday"), today, utc);
        const bool exportRefused = selection.value(QStringLiteral("selection")).toInt() == 0
                                   && selection.value(QStringLiteral("loading")).toBool();
        const bool deletesRefused = !s.store.deleteAll() && !s.store.deleteFolder(kUidA)
                                    && !s.store.deleteRecording(oldId) && !s.store.deleteUnfinished();
        accepted += exportRefused && deletesRefused && s.store.takeDeleteQueueForTest().isEmpty() ? 0 : 1;
    });
    s.pass();
    s.store.setFolderLoadHookForTest({});
    expect("in place: read again", reads == 1);
    expect("in place: refused before the folder was read", accepted == 0);
    expect("in place: yesterday has nothing now",
           export_names(s.store, QStringLiteral("yesterday"), today, utc).isEmpty());
    const QVariantMap now = s.store.selectForExportOn(QString(), QStringLiteral("today"), today, utc);
    s.store.releaseSelection(now.value(QStringLiteral("selection")).toInt());
    expect("in place: today has the replacement",
           now.value(QStringLiteral("count")).toInt() == 1 && now.value(QStringLiteral("bytes")).toLongLong() == 2000);
    expect("in place: an id the new journal lacks deletes nothing",
           s.store.deleteRecording(oldId) && s.store.takeDeleteQueueForTest().isEmpty());
}

/* Issue #902 B12: while the worker reads storage at all, from the moment a returning root is listed, an export and
 * every delete request are refused. Two owners whose recordings carry no journal ids (each named for its path) swap
 * folders while the storage is away: a folder delete during the listing named the other owner's files, which kept
 * those paths after the read. Once the pass ends, each owner's delete names its own recordings where they now are. */
void
test_bulk_requests_wait_for_a_root_being_listed(void) {
    constexpr int kEach = 3;
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
    s.store.endSession();
    const auto idless = [](const QString& file, qint64 start, qint64 row) {
        QJsonObject o = QJsonDocument::fromJson(publish_line(QStringLiteral("x"), file, start, row).trimmed()).object();
        o.remove(QStringLiteral("id"));
        return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    };
    for (int i = 0; i < kEach; i++) {
        const QString file = QStringLiteral("r%1.wav").arg(i);
        make_file(a + QLatin1Char('/') + file, 1000);
        append(journal_of(a), idless(file, 1754000000 + i, 1 + i));
        make_file(b + QLatin1Char('/') + file, 1000);
        append(journal_of(b), idless(file, 1755000000 + i, 10 + i));
    }
    s.pass();
    const auto ids_at = [](const QString& folder) {
        QStringList ids;
        for (int i = 0; i < kEach; i++) {
            ids.append(QStringLiteral("file:") + folder + QStringLiteral("/r%1.wav").arg(i));
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    // Each delete is taken back with its files held: the pass a request wakes leaves them queued, whenever it runs.
    const auto held_delete = [&s](const QString& uid, const QStringList& ids) {
        const QVariantMap held = s.store.selectRecordings(ids);
        const bool asked = s.store.deleteFolder(uid);
        const QStringList queued = asked ? s.taken() : QStringList{QStringLiteral("refused")};
        s.store.releaseSelection(held.value(QStringLiteral("selection")).toInt());
        s.pass();
        return held.value(QStringLiteral("count")).toInt() == ids.size() ? queued
                                                                         : QStringList{QStringLiteral("unheld")};
    };
    expect("listing: B's delete before, B's files", held_delete(kUidB, ids_at(b)) == ids_at(b));

    expect("listing: the storage goes", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
    s.pass();
    const QString awayRoot = away + QStringLiteral("/Call recordings/");
    const QString awayA = awayRoot + QFileInfo(a).fileName();
    const QString awayB = awayRoot + QFileInfo(b).fileName();
    expect("listing: the folders swap", QDir().rename(awayA, awayRoot + QStringLiteral("swap"))
                                            && QDir().rename(awayB, awayA)
                                            && QDir().rename(awayRoot + QStringLiteral("swap"), awayB));
    expect("listing: the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
    int listings = 0;
    int accepted = 0;
    s.store.setRootReadHookForTest([&](const QString&) {
        listings++;
        const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
        const bool exportRefused =
            all.value(QStringLiteral("selection")).toInt() == 0 && all.value(QStringLiteral("loading")).toBool();
        const bool deletesRefused = !s.store.deleteFolder(kUidB) && !s.store.deleteAll()
                                    && !s.store.deleteRecordings(ids_at(b)) && !s.store.deleteUnfinished();
        accepted += exportRefused && deletesRefused && s.store.takeDeleteQueueForTest().isEmpty() ? 0 : 1;
    });
    s.pass();
    s.store.setRootReadHookForTest({});
    expect("listing: the root was listed again", listings >= 1);
    expect("listing: refused while it was listed", accepted == 0);
    // B's recordings now sit at A's former path, and A's at B's.
    expect("listing: B's delete after, B's files where they are now", held_delete(kUidB, ids_at(a)) == ids_at(a));
    expect("listing: A's delete after, A's files where they are now", held_delete(kUidA, ids_at(b)) == ids_at(b));
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("listing: Export all, whole", all.value(QStringLiteral("count")).toInt() == 2 * kEach);
}

/* Issue #902 B12: a delete asked but not done yet (its file held by an export) is dropped by any read of storage
 * again. Two owners whose recordings carry no journal ids (each named for its path) swap folders while the storage is
 * away, with B's delete waiting on its hold: the read back gives B's old paths to A's recordings, so the delete, if it
 * stayed, would take A's files once the hold ended. It is dropped instead: every file stays, and nothing waits. */
void
test_held_delete_dropped_by_a_reread(void) {
    constexpr int kEach = 2;
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
    s.store.endSession();
    const auto idless = [](const QString& file, qint64 start, qint64 row) {
        QJsonObject o = QJsonDocument::fromJson(publish_line(QStringLiteral("x"), file, start, row).trimmed()).object();
        o.remove(QStringLiteral("id"));
        return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    };
    for (int i = 0; i < kEach; i++) {
        const QString file = QStringLiteral("r%1.wav").arg(i);
        make_file(a + QLatin1Char('/') + file, 1000);
        append(journal_of(a), idless(file, 1754000000 + i, 1 + i));
        make_file(b + QLatin1Char('/') + file, 2000);
        append(journal_of(b), idless(file, 1755000000 + i, 10 + i));
    }
    s.pass();
    QStringList idsB;
    for (int i = 0; i < kEach; i++) {
        idsB.append(QStringLiteral("file:") + b + QStringLiteral("/r%1.wav").arg(i));
    }
    // B's delete waits for the export holding its files.
    const QVariantMap held = s.store.selectRecordings(idsB);
    expect("held delete: B's files held", held.value(QStringLiteral("count")).toInt() == kEach);
    expect("held delete: B's delete asked", s.store.deleteFolder(kUidB));
    s.pass();
    expect("held delete: waiting on the hold", s.store.deletesWaiting() == kEach);

    expect("held delete: the storage goes", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
    s.pass();
    const QString awayRoot = away + QStringLiteral("/Call recordings/");
    const QString awayA = awayRoot + QFileInfo(a).fileName();
    const QString awayB = awayRoot + QFileInfo(b).fileName();
    expect("held delete: the folders swap", QDir().rename(awayA, awayRoot + QStringLiteral("swap"))
                                                && QDir().rename(awayB, awayA)
                                                && QDir().rename(awayRoot + QStringLiteral("swap"), awayB));
    expect("held delete: the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
    s.pass();
    expect("held delete: dropped by the read", s.store.deletesWaiting() == 0);
    s.store.releaseSelection(held.value(QStringLiteral("selection")).toInt());
    s.pass();
    // A's recordings (1000 bytes) now at B's old path, B's (2000 bytes) at A's: all of them still there.
    bool kept = true;
    for (int i = 0; i < kEach; i++) {
        const QString file = QStringLiteral("/r%1.wav").arg(i);
        kept = kept && QFileInfo(b + file).size() == 1000 && QFileInfo(a + file).size() == 2000;
    }
    expect("held delete: nothing of either owner deleted", kept && s.store.recordingCount() == 2 * kEach);
    expect("held delete: nothing waits", s.store.deletesWaiting() == 0 && s.store.takeDeleteQueueForTest().isEmpty());
    // Said, not silently zeroed: the screens show the cancelled deletes until they are acknowledged.
    expect("held delete: the cancellation is told", s.store.deletesCancelled() == kEach);
    s.store.acknowledgeNotices();
    expect("held delete: and acknowledged", s.store.deletesCancelled() == 0);
}

/* Issue #902: a delete request that names any recording on storage that is not there is refused whole, at once, and
 * queues nothing: an owner with folders on storage there and away, Delete all and the unfinished recordings delete none
 * of their recordings, while a delete of recordings only on storage there runs. Storage that left since the last pass
 * is found by the request itself, and the pass it asks for marks it away. Back again, the same requests run. */
void
test_deletes_refused_for_storage_away(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    // An earlier run's unfinished recording, there as the store starts.
    const QString made = roots.external + QStringLiteral("/County (1a2b3c4d)");
    write_folder_json(made, QString(kUidA), QStringLiteral("County"));
    const QString temp = made + QStringLiteral("/TEMP_20261027_120000_0123456789ABCDEF_9.wav");
    make_file(temp, 7000);
    Store s(roots.list());
    const QString outer = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    expect("away: the owner's folder", outer == made && s.store.unfinishedCount() == 1);
    record(outer, QStringLiteral("0000000000000601-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.store.endSession();
    s.pass();
    expect("away: the storage goes", QDir().rename(ext, away));
    s.pass();
    const QString inner = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    expect("away: the owner records on the other storage", inner.startsWith(roots.internal));
    record(inner, QStringLiteral("0000000000000601-2"), QStringLiteral("b.wav"), 1754000100, 2);
    s.store.endSession();
    s.pass();
    const auto refused_away = [&s](bool asked) {
        return !asked && s.store.deleteRefusal() == QLatin1String("storageAway")
               && s.store.takeDeleteQueueForTest().isEmpty();
    };
    expect("away: the owner's delete, refused whole", refused_away(s.store.deleteFolder(QString(kUidA))));
    expect("away: Delete all, refused whole", refused_away(s.store.deleteAll()));
    expect("away: the unfinished recordings, refused", refused_away(s.store.deleteUnfinished()));
    expect("away: one recording there and one away, refused",
           refused_away(
               s.store.deleteRecordings({QStringLiteral("0000000000000601-1"), QStringLiteral("0000000000000601-2")})));
    s.pass();
    expect("away: nothing waits, nothing deleted", s.store.deletesWaiting() == 0 && s.store.recordingCount() == 2
                                                       && s.store.unfinishedCount() == 1
                                                       && QFileInfo::exists(inner + QStringLiteral("/b.wav")));

    expect("away: the storage comes back", QDir().rename(away, ext));
    s.pass();
    // Gone again since that pass: the request finds it, and the pass it asks for marks it away.
    expect("away: the storage goes again", QDir().rename(ext, away));
    expect("away: gone since the last pass, refused",
           refused_away(s.store.deleteRecording(QStringLiteral("0000000000000601-1"))));
    s.pass();
    const QVariantMap outerRecording = s.store.recordingsForFragments({fragment(1)}).value(0).toMap();
    expect("away: and marked away", !outerRecording.isEmpty() && !outerRecording.value("available").toBool());
    expect("away: a recording only on storage there is deleted",
           s.store.deleteRecording(QStringLiteral("0000000000000601-2")) && s.store.deleteRefusal().isEmpty());
    s.pass();
    expect("away: the one there gone", !QFileInfo::exists(inner + QStringLiteral("/b.wav")));

    expect("away: the storage comes back again", QDir().rename(away, ext));
    s.pass();
    expect("away: back, the owner's delete runs", s.store.deleteFolder(QString(kUidA)));
    s.pass();
    expect("away: back, everything of it deleted", !QFileInfo::exists(outer + QStringLiteral("/a.wav"))
                                                       && !QFileInfo::exists(temp) && s.store.recordingCount() == 0
                                                       && s.store.unfinishedCount() == 0);
}

/* A publish line with no journal id (the recording is named for its path), every value of one width so two journals
 * of these are the same length. */
QByteArray
idless_line(const QString& file, qint64 start, qint64 row) {
    QJsonObject o = QJsonDocument::fromJson(publish_line(QStringLiteral("x"), file, start, row).trimmed()).object();
    o.remove(QStringLiteral("id"));
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

/* A folder.json for @p uid, written privately. */
bool
write_owner_json(const QString& folder, const QString& uid, const QString& name) {
    QDir().mkpath(folder);
    const QJsonObject meta{{QStringLiteral("uid"), uid},
                           {QStringLiteral("kind"), QStringLiteral("saved")},
                           {QStringLiteral("name"), name}};
    return write_private(folder + QStringLiteral("/folder.json"), QJsonDocument(meta).toJson(QJsonDocument::Compact),
                         "wb");
}

/* Issue #902: a delete removes only the file it was asked of. Two owners whose recordings carry no journal ids swap
 * folders on storage that stays mounted, their journals the same length, so no pass reads either again and the index
 * still names A's recordings at what are now B's paths. A delete of A held by an export across the swap, and one asked
 * after it, each find another file at the path (its identity is not the one the index knew) and leave it: nothing of
 * B's is deleted, nothing of A's either, and the screens are told the deletes did not happen. */
void
test_delete_checks_the_file_it_removes(void) {
    constexpr int kEach = 2;
    Roots roots;
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
    s.store.endSession();
    for (int i = 0; i < kEach; i++) {
        const QString file = QStringLiteral("r%1.wav").arg(i);
        // The same size, each its own modification time: another file by the delete's check (size and time).
        make_file(a + QLatin1Char('/') + file, 1000);
        set_mtime(a + QLatin1Char('/') + file, 1754000000 + i);
        append(journal_of(a), idless_line(file, 1754000000 + i, 10 + i));
        make_file(b + QLatin1Char('/') + file, 1000);
        set_mtime(b + QLatin1Char('/') + file, 1755000000 + i);
        append(journal_of(b), idless_line(file, 1755000000 + i, 20 + i));
    }
    s.pass();
    expect("identity: the journals are the same length",
           read_all(journal_of(a)).size() == read_all(journal_of(b)).size());
    QStringList idsA;
    for (int i = 0; i < kEach; i++) {
        idsA.append(QStringLiteral("file:") + a + QStringLiteral("/r%1.wav").arg(i));
    }
    const QVariantMap held = s.store.selectRecordings(idsA);
    expect("identity: A's delete asked, held by an export", s.store.deleteFolder(QString(kUidA)));
    s.pass();
    expect("identity: waiting on the hold", s.store.deletesWaiting() == kEach);
    const QString aside = roots.dir.path() + QStringLiteral("/swap");
    expect("identity: the folders swap in place",
           QDir().rename(a, aside) && QDir().rename(b, a) && QDir().rename(aside, b));
    s.store.releaseSelection(held.value(QStringLiteral("selection")).toInt());
    s.pass();
    const auto all_there = [&a, &b]() {
        for (int i = 0; i < kEach; i++) {
            const QString file = QStringLiteral("/r%1.wav").arg(i);
            if (!QFileInfo::exists(a + file) || !QFileInfo::exists(b + file)) {
                return false;
            }
        }
        return true;
    };
    expect("identity: the held delete removed nothing", all_there() && s.store.deletesWaiting() == 0);
    expect("identity: and is told", s.store.deletesSkipped() == kEach);
    // Asked again with the index still stale: refused by nothing, and still removes nothing of the other owner.
    expect("identity: asked again", s.store.deleteFolder(QString(kUidA)));
    s.pass();
    expect("identity: asked again, nothing removed", all_there() && s.store.deletesSkipped() == 2 * kEach);
}

/* Issue #902: a folder a session start adopts while a delete of the owner the index still holds there waits. The
 * folder is restored for B at A's path (its journal as long as A's, so no tail reads it), and B's session start adopts
 * it in the middle of a pass, after the folders to read were taken; the hold on A's delete ends in the same pass. The
 * delete finds B's file at the path and leaves it. */
void
test_delete_leaves_a_folder_adopted_meanwhile(void) {
    Roots roots;
    Store s(roots.list());
    const QString x = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("Metro"));
    s.store.endSession();
    const QString c = s.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Other"));
    s.store.endSession();
    make_file(x + QStringLiteral("/r0.wav"), 1000);
    set_mtime(x + QStringLiteral("/r0.wav"), 1754000000);
    append(journal_of(x), idless_line(QStringLiteral("r0.wav"), 1754000000, 10));
    s.pass();
    const QString idA = QStringLiteral("file:") + x + QStringLiteral("/r0.wav");
    const QVariantMap held = s.store.selectRecordings({idA});
    expect("adopted meanwhile: A's delete asked, held", s.store.deleteRecording(idA));
    s.pass();
    expect("adopted meanwhile: waiting", s.store.deletesWaiting() == 1);
    // Restored for B at the same path: A's folder moves out of the root, B's takes its name.
    const QString aside = roots.dir.path() + QStringLiteral("/a-aside");
    expect("adopted meanwhile: A's folder leaves", QDir().rename(x, aside));
    expect("adopted meanwhile: B's folder", write_owner_json(x, QString(kUidB), QStringLiteral("Metro")));
    make_file(x + QStringLiteral("/r0.wav"), 1000);
    set_mtime(x + QStringLiteral("/r0.wav"), 1755000000); // the same size, its own time
    append(journal_of(x), idless_line(QStringLiteral("r0.wav"), 1755000000, 20));
    expect("adopted meanwhile: the journals are the same length",
           read_all(journal_of(x)).size() == read_all(journal_of(aside)).size());
    QString adopted;
    s.store.setTailHookForTest([&](const QString& path) {
        if (path == c && adopted.isEmpty()) {
            adopted = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
            s.store.releaseSelection(held.value(QStringLiteral("selection")).toInt());
        }
    });
    record(c, QStringLiteral("0000000000000701-1"), QStringLiteral("c.wav"), 1754000100, 30);
    s.pass();
    s.store.setTailHookForTest({});
    s.store.endSession();
    s.pass();
    expect("adopted meanwhile: B's start took the restored folder", adopted == x);
    expect("adopted meanwhile: B's recording stays", QFileInfo::exists(x + QStringLiteral("/r0.wav")));
    expect("adopted meanwhile: A's too", QFileInfo::exists(aside + QStringLiteral("/r0.wav")));
    // A's delete waits for the folder's read (unread once B's start took it), which drops it: told as cancelled.
    expect("adopted meanwhile: nothing waits, the cancellation is told",
           s.store.deletesWaiting() == 0 && s.store.deletesCancelled() == 1 && s.store.deletesSkipped() == 0);
}

/* Issue #902: storage a delete request finds missing, back before any pass reads it (a quick reconnect, two owners'
 * folders swapped meanwhile, journals the same length), is listed again in full before another delete of it is taken:
 * the retry is refused as loading, not as storage away, and once the pass has read it the delete takes A's recordings
 * where they are now, and nothing of B's. */
void
test_storage_a_request_saw_missing_is_read_again(void) {
    constexpr int kEach = 2;
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
    s.store.endSession();
    for (int i = 0; i < kEach; i++) {
        const QString file = QStringLiteral("r%1.wav").arg(i);
        make_file(a + QLatin1Char('/') + file, 1000);
        append(journal_of(a), idless_line(file, 1754000000 + i, 10 + i));
        make_file(b + QLatin1Char('/') + file, 2000);
        append(journal_of(b), idless_line(file, 1755000000 + i, 20 + i));
    }
    s.pass();
    // Counted on the worker, read here: atomic, and the baseline taken before the request below wakes the worker.
    std::atomic<int> listings{0};
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.startsWith(ext + QLatin1Char('/'))) {
            listings++;
        }
    });
    const int before = listings.load();
    expect("seen missing: the storage goes", QDir().rename(ext, away));
    expect("seen missing: the delete finds it missing",
           !s.store.deleteFolder(QString(kUidA)) && s.store.deleteRefusal() == QLatin1String("storageAway"));
    const QString awayRoot = away + QStringLiteral("/Call recordings/");
    const QString awayA = awayRoot + QFileInfo(a).fileName();
    const QString awayB = awayRoot + QFileInfo(b).fileName();
    expect("seen missing: the folders swap", QDir().rename(awayA, awayRoot + QStringLiteral("swap"))
                                                 && QDir().rename(awayB, awayA)
                                                 && QDir().rename(awayRoot + QStringLiteral("swap"), awayB));
    expect("seen missing: the storage is back", QDir().rename(away, ext));
    // Whichever way the worker took the request's wake (marking the root away before it came back, or listing it again
    // after), the retry is not taken against the index as it was.
    const bool retried = s.store.deleteFolder(QString(kUidA));
    expect("seen missing: the retry, loading or after a full listing",
           (!retried && s.store.deleteRefusal() == QLatin1String("loading")) || (retried && listings.load() > before));
    if (retried) {
        (void)s.taken();
    }
    s.pass();
    expect("seen missing: listed again in full", listings.load() > before);
    expect("seen missing: A's delete now", s.store.deleteFolder(QString(kUidA)));
    s.pass();
    s.store.setRootReadHookForTest({});
    // A's recordings (1000 bytes) are at B's old path now, B's (2000 bytes) at A's.
    bool kept = true;
    bool gone = true;
    for (int i = 0; i < kEach; i++) {
        const QString file = QStringLiteral("/r%1.wav").arg(i);
        kept = kept && QFileInfo(a + file).size() == 2000;
        gone = gone && !QFileInfo::exists(b + file);
    }
    expect("seen missing: B's recordings stay", kept);
    expect("seen missing: A's are deleted", gone);
}

/* An export and every delete request, refused: nothing selected or held, nothing queued. */
bool
bulk_requests_refused(RecordingStore& store, const QString& uid) {
    const QVariantMap selection = store.selectForExport(QString(), QStringLiteral("all"));
    const bool exportRefused = selection.value(QStringLiteral("selection")).toInt() == 0
                               && selection.value(QStringLiteral("count")).toInt() == 0
                               && selection.value(QStringLiteral("loading")).toBool();
    const bool deletesRefused = !store.deleteFolder(uid) && !store.deleteAll() && !store.deleteUnfinished();
    return exportRefused && deletesRefused && store.takeDeleteQueueForTest().isEmpty();
}

/* Issue #902: a recording's name that became a link (to anything) before its accepted delete ran is not the file asked
 * of: the link stays, and the delete is told as not done. */
void
test_delete_of_a_name_now_a_link_is_told(void) {
#ifdef _WIN32
    DSD_FPRINTF(stdout, "SKIP link: symbolic links need a privilege on Windows\n");
#else
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000904-1"), QStringLiteral("r.wav"), 1754000000, 1);
    make_file(folder + QStringLiteral("/elsewhere.bin"), 1000);
    s.pass();
    const QVariantMap held = s.store.selectRecordings({QStringLiteral("0000000000000904-1")});
    expect("link: the delete asked, held", s.store.deleteRecording(QStringLiteral("0000000000000904-1")));
    s.pass();
    QFile::remove(folder + QStringLiteral("/r.wav"));
    expect("link: the name is a link now",
           QFile::link(QStringLiteral("elsewhere.bin"), folder + QStringLiteral("/r.wav")));
    s.store.releaseSelection(held.value(QStringLiteral("selection")).toInt());
    s.pass();
    expect("link: the link stays", QFileInfo(folder + QStringLiteral("/r.wav")).isSymLink()
                                       && QFileInfo::exists(folder + QStringLiteral("/elsewhere.bin")));
    expect("link: the delete is told as not done", s.store.deletesSkipped() == 1 && s.store.deletesWaiting() == 0);
#endif
}

/* Issue #902, a decided limit (size and modification time only): another file at a recording's name with exactly the
 * same size and modification time is the recording, whatever its inode, and a delete removes it. One a second off, at
 * the same size, is not: the delete skips it, leaves it on disk and is told as not done. */
void
test_same_size_and_time_reads_as_the_recording(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString same = folder + QStringLiteral("/same.wav");
    const QString later = folder + QStringLiteral("/later.wav");
    make_file(same, 5000);
    set_mtime(same, 1754000000);
    make_file(later, 5000);
    set_mtime(later, 1754000100);
    append(journal_of(folder),
           publish_line(QStringLiteral("0000000000000914-1"), QStringLiteral("same.wav"), 1754000000, 1));
    append(journal_of(folder),
           publish_line(QStringLiteral("0000000000000914-2"), QStringLiteral("later.wav"), 1754000100, 2));
    s.pass();
    // Each name now holds another file (a new inode) of the same size: one with the recording's time, one a second on.
    QFile::remove(same);
    make_file(same, 5000);
    set_mtime(same, 1754000000);
    QFile::remove(later);
    make_file(later, 5000);
    set_mtime(later, 1754000101);
    expect("size and time: both deletes asked",
           s.store.deleteRecordings({QStringLiteral("0000000000000914-1"), QStringLiteral("0000000000000914-2")}));
    s.pass();
    expect("size and time: the same size and time is removed as the recording", !QFileInfo::exists(same));
    expect("size and time: another time is left, the delete told as not done",
           QFileInfo::exists(later) && s.store.deletesSkipped() == 1);
}

/* Issue #902: the storage limit's removal checks each file right before its unlink as a user's delete does. With a
 * 1 MB limit over two 600 kB recordings, the older one's file rewritten since (its time moved) is skipped, left on
 * disk and told, and the limit goes on with the next. */
void
test_limit_skips_a_file_not_as_known(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000925-1"), QStringLiteral("old.wav"), 1754000000, 1, 600000);
    record(folder, QStringLiteral("0000000000000925-2"), QStringLiteral("new.wav"), 1754000100, 2, 600000);
    s.pass();
    set_mtime(folder + QStringLiteral("/old.wav"), 1600000000);
    s.store.setLimitMb(1);
    s.pass();
    expect("limit skip: the older file, not as known, stays and is told",
           QFileInfo::exists(folder + QStringLiteral("/old.wav")) && s.store.deletesSkipped() == 1);
    expect("limit skip: the limit goes on with the next",
           !QFileInfo::exists(folder + QStringLiteral("/new.wav")) && s.store.removedByLimit() == 1);
}

/* Issue #902: a recording whose file cannot be read when its delete runs (its folder lost search permission) is never
 * taken as missing: the delete skips it and is told, and the recording stays indexed with its journal record. */
void
test_delete_of_an_unreadable_file_skips_it(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString sub = folder + QStringLiteral("/locked");
    QDir().mkpath(sub);
    if (!permissions_enforced(sub)) {
        DSD_FPRINTF(stdout, "SKIP unreadable delete: this process reads any folder\n");
        return;
    }
    const QString id = QStringLiteral("0000000000000926-1");
    make_file(sub + QStringLiteral("/x.wav"), 1000);
    append(journal_of(folder), publish_line(id, QStringLiteral("locked/x.wav"), 1754000000, 1));
    s.pass();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("unreadable delete: asked", s.store.deleteRecording(id));
    s.pass();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    // Kept with its record, and out of every count from then on (the unreadable recording's rule).
    expect("unreadable delete: skipped and told, kept with its record",
           QFileInfo::exists(sub + QStringLiteral("/x.wav")) && s.store.deletesSkipped() == 1
               && s.store.recordingCount() == 0 && read_all(journal_of(folder)).contains("locked/x.wav"));
}

/* Issue #902: a selection made before a read of storage again is stale from the moment that read starts,
 * not only once the pass ends: a transfer begun while the read is still going on is refused. */
void
test_selection_goes_stale_as_a_read_starts(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000919-1"), QStringLiteral("r.wav"), 1754000000, 1);
    s.pass();
    const int selection =
        s.store.selectRecordings({QStringLiteral("0000000000000919-1")}).value(QStringLiteral("selection")).toInt();
    expect("stale: a selection", selection > 0 && s.store.selectionCurrent(selection));
    expect("stale: the storage goes", QDir().rename(ext, away));
    s.pass();
    std::atomic<int> during{-1};
    s.store.setFolderLoadHookForTest([&](const QString&) {
        if (during.load() < 0) {
            during = s.store.selectionCurrent(selection) ? 1 : 0;
        }
    });
    expect("stale: the storage comes back", QDir().rename(away, ext));
    s.pass();
    s.store.setFolderLoadHookForTest({});
    expect("stale: refused while the read is still going on", during.load() == 0);
    s.store.releaseSelection(selection);
}

/* Issue #902: a folder whose journal is there but cannot be read is not read: the index would hold part of the
 * recordings, so an export and every delete request are refused, not taken from the other owner's folder alone, until
 * a pass reads it whole. */
void
test_unread_journal_is_not_settled(void) {
    Roots roots;
    const QString a = roots.external + QStringLiteral("/County (1a2b3c4d)");
    const QString b = roots.external + QStringLiteral("/Metro (1a2b3c4d)");
    expect("unread journal: two owners' folders", write_owner_json(a, QString(kUidA), QStringLiteral("County"))
                                                      && write_owner_json(b, QString(kUidB), QStringLiteral("Metro")));
    record(a, QStringLiteral("0000000000000908-a"), QStringLiteral("a.wav"), 1754000000, 1);
    record(b, QStringLiteral("0000000000000908-b"), QStringLiteral("b.wav"), 1754000100, 2);
    QFile::setPermissions(journal_of(a), QFileDevice::WriteOwner);
    QFile probe(journal_of(a));
    if (probe.open(QIODevice::ReadOnly)) {
        probe.close();
        QFile::setPermissions(journal_of(a), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        DSD_FPRINTF(stdout, "SKIP unread journal: this process reads any file\n");
        return;
    }
    Store s(roots.list());
    expect("unread journal: requests refused",
           bulk_requests_refused(s.store, QString(kUidB)) && s.store.deleteRefusal() == QLatin1String("loading"));
    QFile::setPermissions(journal_of(a), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("unread journal: read whole, Export all takes both", all.value(QStringLiteral("count")).toInt() == 2);
}

/* Issue #902: a journal that cannot be read (no access) as its storage comes back. */
bool
take_read_access(const QString& path) {
    QFile::setPermissions(path, QFileDevice::WriteOwner);
    QFile probe(path);
    if (probe.open(QIODevice::ReadOnly)) {
        probe.close();
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return false;
    }
    return true;
}

/* Issue #902: the storage limit waits for the index to be settled as the refusals have it. A 1.1 MB recording held
 * over from before its storage went, a 1 MB limit, and storage back with a journal that no longer names it and cannot
 * be read: the folder is unread, so the limit removes nothing (it would have removed the recording on what the old
 * journal said). Once the journal is read, the recording is no longer managed and the limit leaves it on disk; the
 * folder's read publishes that the store is indexed again, though no recording in it changed. */
void
test_limit_waits_for_a_returning_folder(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("limit waits: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    record_at(folder, QStringLiteral("0000000000000930-1"), QStringLiteral("held.wav"), 1754000000, 1, 1100000,
              1754000000);
    Store s(roots.list());
    expect("limit waits: indexed", s.store.recordingCount() == 1 && s.store.usedBytes() == 1100000);
    expect("limit waits: the storage goes", QDir().rename(ext, away));
    s.pass();
    const QString awayJournal = away + QStringLiteral("/Call recordings/County (1a2b3c4d)/recordings.jsonl");
    QFile journal(awayJournal);
    expect("limit waits: a journal naming nothing", journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
    journal.close();
    if (!take_read_access(awayJournal)) {
        DSD_FPRINTF(stdout, "SKIP limit waits: this process reads any file\n");
        return;
    }
    s.store.setLimitMb(1);
    s.pass();
    expect("limit waits: the storage comes back", QDir().rename(away, ext));
    s.pass();
    expect("limit waits: nothing removed while the folder is unread",
           QFileInfo::exists(folder + QStringLiteral("/held.wav")) && s.store.removedByLimit() == 0
               && !s.store.indexed());
    QFile::setPermissions(journal_of(folder), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.pass();
    expect("limit waits: read, the recording is no longer managed and stays on disk",
           QFileInfo::exists(folder + QStringLiteral("/held.wav")) && s.store.removedByLimit() == 0
               && s.store.recordingCount() == 0);
    expect("limit waits: indexed again", s.store.indexed());
}

/* Issue #902: a journal that cannot be read at the first pass, and is empty once it can: its read changes no
 * recording, and still publishes that the store is indexed. */
void
test_unread_journal_retry_publishes(void) {
    Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("retry publishes: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    expect("retry publishes: an empty journal", write_private(journal_of(folder), QByteArray(), "wb"));
    if (!take_read_access(journal_of(folder))) {
        DSD_FPRINTF(stdout, "SKIP retry publishes: this process reads any file\n");
        return;
    }
    Store s(roots.list());
    expect("retry publishes: unread, not indexed", !s.store.indexed());
    QFile::setPermissions(journal_of(folder), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    expect("retry publishes: read, indexed", s.store.indexed() && !all.value(QStringLiteral("loading")).toBool());
}

/* Issue #902: new journal lines that cannot be read (no access to the journal as it grew) leave the folder unsettled:
 * an export or a delete would leave out the recordings they name. A read that succeeds settles it, with them. */
void
test_tail_read_failure_unsettles(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000931-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    record(folder, QStringLiteral("0000000000000931-2"), QStringLiteral("b.wav"), 1754000100, 2);
    if (!take_read_access(journal_of(folder))) {
        DSD_FPRINTF(stdout, "SKIP tail failure: this process reads any file\n");
        return;
    }
    s.pass();
    expect("tail failure: requests refused", bulk_requests_refused(s.store, QString(kUidA)));
    QFile::setPermissions(journal_of(folder), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("tail failure: read again, both", all.value(QStringLiteral("count")).toInt() == 2);
}

/* Issue #902: a recheck (the player, an export or a share found the file changed) of a name that cannot be read keeps
 * the recording indexed, its journal line too: no access says nothing about whether it is there. */
void
test_recheck_keeps_an_unreadable_name(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString sub = folder + QStringLiteral("/locked");
    QDir().mkpath(sub);
    if (!permissions_enforced(sub)) {
        DSD_FPRINTF(stdout, "SKIP unreadable recheck: this process reads any folder\n");
        return;
    }
    const QString id = QStringLiteral("0000000000000932-1");
    make_file(sub + QStringLiteral("/x.wav"), 1000);
    append(journal_of(folder), publish_line(id, QStringLiteral("locked/x.wav"), 1754000000, 1));
    s.pass();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.store.recheckRecordings({id});
    s.pass();
    s.pass();
    const qint64 counted = s.store.recordingCount();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    // Kept, out of every count and index, its journal line with it; a later read of its folder sees it again.
    expect("unreadable recheck: kept with its journal line, counted nowhere",
           counted == 0 && read_all(journal_of(folder)).contains("locked/x.wav")
               && QFileInfo::exists(sub + QStringLiteral("/x.wav")));
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    expect("unreadable recheck: the storage goes", QDir().rename(ext, away));
    s.pass();
    expect("unreadable recheck: the storage comes back", QDir().rename(away, ext));
    s.pass();
    s.pass();
    expect("unreadable recheck: read again with access back, a recording as before", s.store.recordingCount() == 1);
}

/* Issue #902: a recording a transfer found changed has its folder read again, the owner and the file taken from what
 * is on disk, never put onto the entry the index held. Two owners' folders swapped in place (journals the same length,
 * so nothing else reads them again): A's save finds B's recording at A's path; once A's former folder is read again it
 * is B's, and a delete of A's recordings removes nothing of B's. */
void
test_changed_file_reads_its_folder_again(void) {
    Roots roots;
    const QString a = roots.external + QStringLiteral("/County (1a2b3c4d)");
    const QString b = roots.external + QStringLiteral("/Metro (1a2b3c4d)");
    expect("read again: two owners' folders", write_owner_json(a, QString(kUidA), QStringLiteral("County"))
                                                  && write_owner_json(b, QString(kUidB), QStringLiteral("County")));
    append(journal_of(a), idless_line(QStringLiteral("r.wav"), 1754000000, 10));
    append(journal_of(b), idless_line(QStringLiteral("r.wav"), 1754000000, 20));
    make_file(a + QStringLiteral("/r.wav"), 1000);
    set_mtime(a + QStringLiteral("/r.wav"), 1754000000);
    make_file(b + QStringLiteral("/r.wav"), 1000);
    set_mtime(b + QStringLiteral("/r.wav"), 1755000000);
    Store s(roots.list());
    ServedHost host;
    dsd_qt::recording_store_serve_host(&s.store, &host);
    expect("read again: both indexed", s.store.recordingCount() == 2);
    const QString swap = roots.external + QStringLiteral("/swap");
    expect("read again: the folders swap in place",
           QDir().rename(a, swap) && QDir().rename(b, a) && QDir().rename(swap, b));
    s.pass();
    const QString idA = QStringLiteral("file:") + a + QStringLiteral("/r.wav");
    const QString dest = roots.dir.path() + QStringLiteral("/saved.wav");
    QSignalSpy finished(&host, &dsd_qt::DecoderHost::recordingTransferFinished);
    expect("read again: A's save begins",
           host.saveRecording(s.store.selectRecordings({idA}), QUrl::fromLocalFile(dest).toString()));
    s.pass();
    expect("read again: A's save finds another file",
           !finished.isEmpty() && finished.last().at(1).toInt() == dsd_qt::DecoderHost::TransferChanged);
    s.pass();
    s.pass();
    const QVariantList folders = s.store.folders();
    int ownersAtA = 0;
    for (const QVariant& folder : folders) {
        const QVariantMap map = folder.toMap();
        ownersAtA += map.value(QStringLiteral("uid")).toString() == QString(kUidA) ? 1 : 0;
    }
    expect("read again: A's former folder is B's now", ownersAtA == 0);
    (void)s.store.deleteFolder(QString(kUidA));
    s.pass();
    expect("read again: A's delete removes nothing of B's",
           QFileInfo::exists(a + QStringLiteral("/r.wav")) && QFileInfo::exists(b + QStringLiteral("/r.wav")));
}

/* Issue #902, r15t: a start while the worker lists a root again (a delete request saw the root missing, back before the
 * pass) never records onto it, the root not read in full since it came back: it records on the fallback, and its
 * recordings are indexed. A start during a listing of a root that is not away (its directory found again, I1) makes
 * its folder there, and that folder is the start's: the listing never saw it (keepSessionFoldersLocked()). */
void
test_rescan_keeps_a_session_folder_made_meanwhile(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000933-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    // Between two passes, on the worker (so no pass marks the root away meanwhile): the storage goes, a delete finds it
    // missing, and it comes back.
    std::atomic<int> ends{0};
    std::atomic<bool> seen{false};
    s.store.setPassEndHookForTest([&]() {
        if (ends++ == 0 && QDir().rename(ext, away)) {
            seen = !s.store.deleteFolder(QString(kUidA)) && s.store.deleteRefusal() == QLatin1String("storageAway");
            (void)QDir().rename(away, ext);
        }
    });
    QString made;
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.startsWith(ext + QLatin1Char('/')) && made.isEmpty()) {
            made = s.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Metro"));
        }
    });
    s.pass();
    s.pass();
    s.store.setPassEndHookForTest({});
    s.store.setRootReadHookForTest({});
    expect("made meanwhile: the delete found the storage missing, back before the next pass",
           seen.load() && QFileInfo::exists(ext));
    expect("made meanwhile: the start records on the fallback, not the unlisted root",
           made.startsWith(roots.internal + QLatin1Char('/')));
    record(made, QStringLiteral("0000000000000933-2"), QStringLiteral("c.wav"), 1754000100, 2);
    s.store.endSession();
    s.pass();
    expect("made meanwhile: its recording is indexed",
           s.store.recordingCount() == 2 && s.store.isIndexedPath(made + QStringLiteral("/c.wav")));

    // A root not away, listed because its directory is there again (I1): a start during that listing records there.
    const Roots fresh;
    Store t(fresh.list());
    const QString staged = fresh.dir.path() + QStringLiteral("/staged");
    expect("made meanwhile: a folder made elsewhere",
           write_owner_json(staged + QStringLiteral("/County (1a2b3c4d)"), QString(kUidA), QStringLiteral("County")));
    QString there;
    t.store.setRootReadHookForTest([&](const QString& root) {
        if (root == fresh.external && there.isEmpty()) {
            there = t.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Metro"));
        }
    });
    expect("made meanwhile: the directory there again", QDir().rename(staged, fresh.external));
    t.pass();
    t.store.setRootReadHookForTest({});
    expect("made meanwhile: during that listing the start's folder is on that root",
           there.startsWith(fresh.external + QLatin1Char('/')));
    record(there, QStringLiteral("0000000000000933-3"), QStringLiteral("d.wav"), 1754000200, 3);
    t.store.endSession();
    t.pass();
    expect("made meanwhile: kept, its recording is indexed", t.store.isIndexedPath(there + QStringLiteral("/d.wav")));
}

/* Issue #902: at the end of the next pass, on the worker (so no pass marks the root away meanwhile), the storage goes,
 * a delete of @p uid's recordings finds it missing, @p whileAway runs on the storage away, and it comes back: the next
 * pass lists it again in full while it reads as available. Returns whether the delete saw it missing. */
bool
storage_seen_away_and_back(Store& s, const QString& ext, const QString& away, const QString& uid,
                           const std::function<void()>& whileAway) {
    std::atomic<int> ends{0};
    std::atomic<bool> seen{false};
    s.store.setPassEndHookForTest([&]() {
        if (ends++ == 0 && QDir().rename(ext, away)) {
            seen = !s.store.deleteFolder(uid) && s.store.deleteRefusal() == QLatin1String("storageAway");
            if (whileAway) {
                whileAway();
            }
            (void)QDir().rename(away, ext);
        }
    });
    s.pass();
    s.pass();
    s.store.setPassEndHookForTest({});
    return seen.load();
}

/* Issue #902, r15t: a session folder removed while its storage was away is not made again by a start while the
 * returning root is listed (not read in full since it came back): the start records on the fallback, and the listing
 * drops the removed folder's earlier recordings; only what is recorded now counts. */
void
test_rescan_drops_a_recreated_folders_old_recordings(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000936-1"), QStringLiteral("old.wav"), 1754000000, 1, 5000);
    s.pass();
    expect("recreated: indexed", s.store.recordingCount() == 1 && s.store.usedBytes() == 5000);
    QString made;
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.startsWith(ext + QLatin1Char('/')) && made.isEmpty()) {
            made = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        }
    });
    const QString awayFolder = away + QStringLiteral("/Call recordings/") + QFileInfo(a).fileName();
    expect("recreated: seen away and back, its folder removed meanwhile",
           storage_seen_away_and_back(s, ext, away, QString(kUidA),
                                      [&awayFolder]() { (void)QDir(awayFolder).removeRecursively(); }));
    s.store.setRootReadHookForTest({});
    expect("recreated: the start records on the fallback", made.startsWith(roots.internal + QLatin1Char('/')));
    record(made, QStringLiteral("0000000000000936-2"), QStringLiteral("new.wav"), 1754000100, 2, 3000);
    s.store.endSession();
    s.pass();
    expect("recreated: only the new recording counts", s.store.recordingCount() == 1 && s.store.usedBytes() == 3000
                                                           && !s.store.isIndexedPath(a + QStringLiteral("/old.wav")));

    // r16t: the same path made again, through the one exception (a start that finds the directory absent may make it
    // anew). The listing omits the removed folder; in its read hook the recordings directory is moved aside and the
    // start makes it, and the same owner path, again. At that pass's end, before the read again it asks for, the old
    // row is gone (installRootTableLocked() drops what the index held at a path a start made during the listing).
    const Roots again;
    const QString ext2 = again.dir.path() + QStringLiteral("/ext");
    const QString away2 = again.dir.path() + QStringLiteral("/ext-away");
    const QString aside = again.dir.path() + QStringLiteral("/aside");
    Store t(again.list());
    const QString b = t.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    t.store.endSession();
    record(b, QStringLiteral("0000000000000936-3"), QStringLiteral("old.wav"), 1754000200, 3, 5000);
    t.pass();
    expect("recreated, same path: indexed", listed_as(t.store, 3, true));
    std::atomic<int> ends{0};
    std::atomic<bool> seen{false};
    std::atomic<bool> oldGone{false};
    QString remade;
    t.store.setPassEndHookForTest([&]() {
        const int end = ends++;
        if (end == 0 && QDir().rename(ext2, away2)) {
            seen = !t.store.deleteFolder(QString(kUidA));
            (void)QDir(away2 + QStringLiteral("/Call recordings/") + QFileInfo(b).fileName()).removeRecursively();
            (void)QDir().rename(away2, ext2);
        } else if (end == 1) {
            oldGone = t.store.recordingsForFragments({fragment(3)}).isEmpty();
        }
    });
    t.store.setRootReadHookForTest([&](const QString& root) {
        if (root == again.external && remade.isEmpty() && QDir().rename(again.external, aside)) {
            remade = t.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        }
    });
    t.pass();
    t.pass();
    t.store.setRootReadHookForTest({});
    t.store.setPassEndHookForTest({});
    expect("recreated, same path: seen away and back", seen.load());
    expect("recreated, same path: the start made the same path again", remade == b);
    expect("recreated, same path: at that pass's end the old row is gone", oldGone.load());
    t.store.endSession();
}

/* Issue #902, r15t: a start while the returning root is listed (the listing read the old folder.json already) leaves
 * its owner's folder there alone, that root not read in full since it came back: it records on the fallback under the
 * new label, and the folder on the returning root keeps its own. */
void
test_rescan_keeps_a_relabel_made_meanwhile(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000937-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    QString relabelled;
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.startsWith(ext + QLatin1Char('/')) && relabelled.isEmpty()) {
            relabelled = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("Metro"));
            s.store.endSession();
        }
    });
    expect("relabel: seen away and back", storage_seen_away_and_back(s, ext, away, QString(kUidA), {}));
    s.store.setRootReadHookForTest({});
    s.pass();
    expect("relabel: the start records on the fallback", relabelled.startsWith(roots.internal + QLatin1Char('/')));
    QFile marker(a + QStringLiteral("/folder.json"));
    const QString onDisk = marker.open(QIODevice::ReadOnly) ? QString::fromUtf8(marker.readAll()) : QString();
    expect("relabel: the folder on the returning root keeps its label",
           onDisk.contains(QStringLiteral("County")) && !onDisk.contains(QStringLiteral("Metro"))
               && s.store.folderPathsForTest().contains(a));

    // r16t: a relabel during a listing that is permitted, of an available root whose directory is there again (I1).
    // In the listing's read hook one start adopts the listed folder and a second relabels it; the table installed
    // keeps the start's label (keepSessionFoldersLocked()), seen before the folder's own read could repair it.
    const Roots fresh;
    Store t(fresh.list());
    const QString staged = fresh.dir.path() + QStringLiteral("/staged");
    const QString listed = fresh.external + QStringLiteral("/County (1a2b3c4d)");
    expect("relabel, listed: a folder made elsewhere",
           write_owner_json(staged + QStringLiteral("/County (1a2b3c4d)"), QString(kUidA), QStringLiteral("County")));
    QString adopted;
    QString relabel;
    t.store.setRootReadHookForTest([&](const QString& root) {
        if (root == fresh.external && adopted.isEmpty()) {
            adopted = t.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
            t.store.endSession();
            relabel = t.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("Metro"));
            t.store.endSession();
        }
    });
    QString indexedLabel;
    t.store.setFolderLoadHookForTest([&](const QString& path) {
        if (path == listed && indexedLabel.isEmpty()) {
            indexedLabel = t.store.folderNameForTest(path); // the table's, before this read installs its own
        }
    });
    expect("relabel, listed: the directory there again", QDir().rename(staged, fresh.external));
    t.pass();
    t.store.setRootReadHookForTest({});
    t.store.setFolderLoadHookForTest({});
    expect("relabel, listed: adopted and relabelled during the listing", adopted == listed && relabel == listed);
    expect("relabel, listed: the installed table keeps the new label", indexedLabel == QStringLiteral("Metro"));
}

/* Issue #902: storage plugged in again after a pass marked it away wakes no pass on its own; an export (or a save)
 * pressed then probes it, is refused as loading, and asks for the pass that reads it, so Export all never leaves out
 * the recordings on it. */
void
test_export_probes_a_root_back(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString outer = roots.external + QStringLiteral("/County (1a2b3c4d)");
    const QString inner = roots.internal + QStringLiteral("/Metro (5e6f7a8b)");
    expect("root back: two folders", write_owner_json(outer, QString(kUidA), QStringLiteral("County"))
                                         && write_owner_json(inner, QString(kUidC), QStringLiteral("Metro")));
    record(outer, QStringLiteral("0000000000000938-1"), QStringLiteral("a.wav"), 1754000000, 1);
    record(inner, QStringLiteral("0000000000000938-2"), QStringLiteral("b.wav"), 1754000100, 2);
    Store s(roots.list());
    expect("root back: the storage goes", QDir().rename(ext, away));
    s.pass();
    expect("root back: the storage comes back, no pass yet", QDir().rename(away, ext));
    const QVariantMap first = s.store.selectForExport(QString(), QStringLiteral("all"));
    expect("root back: Export all refused as loading",
           first.value(QStringLiteral("selection")).toInt() == 0 && first.value(QStringLiteral("loading")).toBool());
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("root back: read, Export all takes both", all.value(QStringLiteral("count")).toInt() == 2);
}

/* Issue #902: a held delete whose folder's new journal lines cannot be read in the pass that frees it is not run:
 * the folder is unread, and its read (which drops the delete) comes first. */
void
test_held_delete_waits_for_an_unread_tail(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString id = QStringLiteral("0000000000000939-1");
    record(folder, id, QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    const int held = s.store.selectRecordings({id}).value(QStringLiteral("selection")).toInt();
    expect("unread tail: the delete asked, held", held > 0 && s.store.deleteRecording(id));
    s.pass();
    expect("unread tail: waiting", s.store.deletesWaiting() == 1);
    record(folder, QStringLiteral("0000000000000939-2"), QStringLiteral("b.wav"), 1754000100, 2);
    if (!take_read_access(journal_of(folder))) {
        DSD_FPRINTF(stdout, "SKIP unread tail: this process reads any file\n");
        s.store.releaseSelection(held);
        return;
    }
    s.store.releaseSelection(held);
    s.pass();
    expect("unread tail: nothing deleted while the folder is unread",
           QFileInfo::exists(folder + QStringLiteral("/a.wav")));
    QFile::setPermissions(journal_of(folder), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.pass();
    expect("unread tail: read, the delete dropped and told",
           QFileInfo::exists(folder + QStringLiteral("/a.wav")) && s.store.deletesWaiting() == 0
               && s.store.deletesCancelled() == 1 && s.store.recordingCount() == 2);
}

/* Issue #902: a recording whose own name cannot be read when its folder is read (its subfolder lost search permission)
 * is kept, and holds nothing else: the folder is read and settled, the recording is counted nowhere, never exported or
 * deleted (a delete naming it is told as skipped), and its journal line stays. Once its folder is read again with
 * access back, it is a recording as before. */
void
test_unreadable_recording_is_kept(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString sub = folder + QStringLiteral("/locked");
    QDir().mkpath(sub);
    if (!permissions_enforced(sub)) {
        DSD_FPRINTF(stdout, "SKIP unreadable kept: this process reads any folder\n");
        return;
    }
    const QString idA = QStringLiteral("0000000000000940-1");
    const QString idX = QStringLiteral("0000000000000940-2");
    record(folder, idA, QStringLiteral("a.wav"), 1754000000, 1);
    make_file(sub + QStringLiteral("/x.wav"), 1000);
    append(journal_of(folder), publish_line(idX, QStringLiteral("locked/x.wav"), 1754000100, 2));
    s.pass();
    expect("unreadable kept: both indexed", s.store.recordingCount() == 2);
    // a.wav written to since: a recheck reads the folder again whole, with x.wav's subfolder closed.
    make_file(folder + QStringLiteral("/a.wav"), 1500);
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.store.recheckRecordings({idA});
    s.pass();
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    const bool settled =
        !all.value(QStringLiteral("loading")).toBool() && all.value(QStringLiteral("count")).toInt() == 1;
    const bool counted = s.store.recordingCount() == 1 && s.store.usedBytes() == 1500;
    const bool deleteAsked = s.store.deleteRecording(idX);
    s.pass();
    const bool skipped = s.store.deletesSkipped() == 1 && s.store.deletesWaiting() == 0;
    const bool journalKept = read_all(journal_of(folder)).contains("locked/x.wav");
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    expect("unreadable kept: the folder is read and settled, the export takes only a.wav", settled);
    expect("unreadable kept: counted nowhere", counted);
    expect("unreadable kept: its delete is told as skipped, the file and line stay",
           deleteAsked && skipped && journalKept && QFileInfo::exists(sub + QStringLiteral("/x.wav")));
    make_file(folder + QStringLiteral("/a.wav"), 1600);
    s.store.recheckRecordings({idA});
    s.pass();
    s.pass();
    expect("unreadable kept: read again with access back, a recording as before", s.store.recordingCount() == 2);
}

/* Issue #902: a folder.json that is there and cannot be read (no access) as its folder is read again keeps the folder
 * known and unread, with its recordings, retried each pass; once it can be read the folder is read as before. */
void
test_unreadable_marker_keeps_the_folder(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString id = QStringLiteral("0000000000000941-1");
    record(folder, id, QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    const QString marker = folder + QStringLiteral("/folder.json");
    make_file(folder + QStringLiteral("/a.wav"), 1500);
    if (!take_read_access(marker)) {
        DSD_FPRINTF(stdout, "SKIP unreadable marker: this process reads any file\n");
        return;
    }
    s.store.recheckRecordings({id});
    s.pass();
    s.pass();
    const bool refused = bulk_requests_refused(s.store, QString(kUidA));
    const bool kept = s.store.folders().size() == 1 && s.store.recordingCount() == 1;
    s.pass();
    const bool stillKept = s.store.folders().size() == 1 && s.store.recordingCount() == 1;
    QFile::setPermissions(marker, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("unreadable marker: refused while unread", refused);
    expect("unreadable marker: the folder and its recording kept, pass after pass", kept && stillKept);
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("unreadable marker: read once it can be", all.value(QStringLiteral("count")).toInt() == 1
                                                         && !all.value(QStringLiteral("loading")).toBool()
                                                         && s.store.usedBytes() == 1500);
}

/* Issue #902: a delete's folder taken by a session start between the delete's checks and its unlink (asked of the
 * uploader without the store's lock): folder.json names another owner now and its journal no longer names the
 * recording, whose file is unchanged. The unlink's own check finds the folder unread and holds the delete, and the
 * folder's read drops it: the file stays. */
void
test_delete_rechecks_its_folder_at_the_unlink(void) {
    Roots roots;
    const QString x = roots.external + QStringLiteral("/Metro (1a2b3c4d)");
    expect("unlink folder: A's folder", write_owner_json(x, QString(kUidA), QStringLiteral("Metro")));
    const QString id = QStringLiteral("0000000000000942-1");
    record(x, id, QStringLiteral("r.wav"), 1754000000, 1);
    Store s(roots.list(), false);
    std::atomic<bool> adopted{false};
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/r.wav")) && !adopted.exchange(true)) {
            (void)write_owner_json(x, QString(kUidB), QStringLiteral("Metro"));
            QFile journal(journal_of(x));
            (void)journal.open(QIODevice::WriteOnly | QIODevice::Truncate);
            journal.close();
            (void)s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
        }
        return false;
    });
    s.start();
    expect("unlink folder: A's delete asked", s.store.deleteRecording(id));
    s.pass();
    s.store.endSession();
    s.pass();
    expect("unlink folder: the start took the folder", adopted.load());
    expect("unlink folder: the file stays, the delete dropped and told", QFileInfo::exists(x + QStringLiteral("/r.wav"))
                                                                             && s.store.deletesWaiting() == 0
                                                                             && s.store.deletesCancelled() == 1);
}

/* Issue #902: the storage limit's walk ends when its count no longer covers the whole index: a session start adopts a
 * folder of older recordings (restored onto the root) while the limit removes. Nothing more goes in that walk; once the
 * adopted folder is read, the limit runs again over everything. */
void
test_limit_walk_stops_when_its_count_goes_stale(void) {
    Roots roots;
    const QString a = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("limit stale: A's folder", write_owner_json(a, QString(kUidA), QStringLiteral("County")));
    for (int i = 0; i < 5; i++) {
        record(a, QStringLiteral("0000000000000943-%1").arg(i), QStringLiteral("r%1.wav").arg(i), 1754000000 + i, i,
               300000);
    }
    Store s(roots.list(), false);
    // C's folder restored onto the root with two older recordings once the walk began, taken by C's start in it: the
    // table gains a folder not read yet.
    const QString c =
        roots.external + QLatin1Char('/')
        + dsd_qt::recording_folder_base_name(QStringLiteral("saved"), QString(kUidC), QStringLiteral("Other"));
    std::atomic<bool> adopted{false};
    QString restored;
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/r0.wav")) && !adopted.exchange(true)) {
            (void)write_owner_json(c, QString(kUidC), QStringLiteral("Other"));
            record(c, QStringLiteral("0000000000000943-c0"), QStringLiteral("c0.wav"), 1753000000, 10, 300000);
            record(c, QStringLiteral("0000000000000943-c1"), QStringLiteral("c1.wav"), 1753000001, 11, 300000);
            restored = s.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Other"));
            s.store.endSession();
        }
        return false;
    });
    const auto left = [&a]() {
        int n = 0;
        for (int i = 0; i < 5; i++) {
            n += QFileInfo::exists(a + QStringLiteral("/r%1.wav").arg(i)) ? 1 : 0;
        }
        return n;
    };
    // What the walk left, read at the end of the pass it ran in (later passes read C's folder and run it again).
    std::atomic<int> leftInWalk{-1};
    s.store.setPassEndHookForTest([&]() {
        if (adopted.load() && leftInWalk.load() < 0) {
            leftInWalk = left() + (QFileInfo::exists(c + QStringLiteral("/c0.wav")) ? 10 : 0);
        }
    });
    s.store.setLimitMb(1);
    s.start();
    s.pass();
    s.pass();
    s.store.setPassEndHookForTest({});
    expect("limit stale: the start took the restored folder in the walk", adopted.load() && restored == c);
    expect("limit stale: nothing removed once the count went stale", leftInWalk.load() == 15);
    // Run again over everything: 2.1 MB down to 950 kB, oldest first, C's two and then A's two oldest.
    expect("limit stale: run again once settled, oldest first",
           left() == 3 && !QFileInfo::exists(c + QStringLiteral("/c0.wav"))
               && !QFileInfo::exists(c + QStringLiteral("/c1.wav")) && !QFileInfo::exists(a + QStringLiteral("/r0.wav"))
               && s.store.removedByLimit() == 4);
}

/* Issue #902: storage plugged in again after a pass marked it away: Delete all, Delete unfinished and an owner's delete
 * probe it as Export does, and are refused as loading while it is read. */
void
test_bulk_deletes_probe_a_root_back(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString inner = roots.internal + QStringLiteral("/Metro (5e6f7a8b)");
    expect("bulk root back: a folder inside", write_owner_json(inner, QString(kUidC), QStringLiteral("Metro")));
    record(inner, QStringLiteral("0000000000000944-1"), QStringLiteral("b.wav"), 1754000100, 2);
    expect("bulk root back: the storage away", QDir().rename(ext, away));
    Store s(roots.list());
    expect("bulk root back: the storage comes back, no pass yet", QDir().rename(away, ext));
    expect("bulk root back: Delete all refused as loading", !s.store.deleteAll()
                                                                && s.store.deleteRefusal() == QLatin1String("loading")
                                                                && s.store.takeDeleteQueueForTest().isEmpty());
    s.pass();
    expect("bulk root back: read, Delete all taken", s.store.deleteAll());
}

/* Issue #902: a session start that adopts the folder while the worker reads it (B's folder.json and journal put where
 * A's were, B's start taking it after the read saw A's folder.json) commits nothing of that read: the folder is read
 * again, and A's delete then removes nothing of B's. */
void
test_adoption_during_a_read_is_read_again(void) {
    Roots roots;
    const QString x = roots.external + QStringLiteral("/Metro (1a2b3c4d)");
    expect("adopted in read: A's folder", write_owner_json(x, QString(kUidA), QStringLiteral("Metro")));
    record_at(x, QStringLiteral("0000000000000945-a"), QStringLiteral("a.wav"), 1754000000, 1, 1000, 1754000000);
    Store s(roots.list());
    std::atomic<bool> adopted{false};
    s.store.setFolderLoadHookForTest([&](const QString& path) {
        if (path == x && !adopted.exchange(true)) {
            (void)write_owner_json(x, QString(kUidB), QStringLiteral("Metro"));
            (void)write_private(journal_of(x), QByteArray(), "wb");
            QFile::remove(x + QStringLiteral("/a.wav"));
            record_at(x, QStringLiteral("0000000000000945-b"), QStringLiteral("b.wav"), 1755000000, 2, 2000,
                      1755000000);
            (void)s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Metro"));
            s.store.endSession();
        }
    });
    // a.wav written to since: the recheck reads the folder again whole.
    make_file(x + QStringLiteral("/a.wav"), 1500);
    s.store.recheckRecordings({QStringLiteral("0000000000000945-a")});
    s.pass();
    s.pass();
    s.store.setFolderLoadHookForTest({});
    s.pass();
    QString owner;
    for (const QVariant& folder : s.store.folders()) {
        owner = folder.toMap().value(QStringLiteral("uid")).toString();
    }
    expect("adopted in read: the start took the folder in the read", adopted.load());
    expect("adopted in read: the folder is B's", owner == QString(kUidB));
    (void)s.store.deleteFolder(QString(kUidA));
    s.pass();
    expect("adopted in read: A's delete removes nothing of B's", QFileInfo::exists(x + QStringLiteral("/b.wav")));
}

/* Issue #902: a session start that cannot read a loaded folder's folder.json (no access now) leaves that folder's
 * recordings as they are, but no request is taken against it until a read can tell its owner again. */
void
test_owner_lookup_failure_unsettles(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000946-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    if (!take_read_access(folder + QStringLiteral("/folder.json"))) {
        DSD_FPRINTF(stdout, "SKIP owner lookup: this process reads any file\n");
        return;
    }
    (void)s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    s.pass();
    const bool refused = bulk_requests_refused(s.store, QString(kUidA));
    const bool kept = QFileInfo::exists(folder + QStringLiteral("/a.wav"));
    QFile::setPermissions(folder + QStringLiteral("/folder.json"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("owner lookup: refused until it can be read, the recording kept", refused && kept);
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("owner lookup: read again, settled",
           !all.value(QStringLiteral("loading")).toBool() && all.value(QStringLiteral("count")).toInt() == 1);
}

/* Issue #902: a recording whose name cannot be read is in no reader's index, linked to its History row afterwards
 * (a pending publish, then its link) included. */
void
test_unreadable_linked_recording_is_in_no_index(void) {
    Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("unreadable link: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    const QString sub = folder + QStringLiteral("/locked");
    QDir().mkpath(sub);
    if (!permissions_enforced(sub)) {
        DSD_FPRINTF(stdout, "SKIP unreadable link: this process reads any folder\n");
        return;
    }
    const QString id = QStringLiteral("0000000000000947-1");
    make_file(sub + QStringLiteral("/x.wav"), 1000);
    append(journal_of(folder), publish_line(id, QStringLiteral("locked/x.wav"), 1754000000, -1, true));
    append(journal_of(folder), link_line(id, 7));
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    Store s(roots.list());
    const bool listed =
        s.store.hasRecordingForFragments({fragment(7)}) || !s.store.recordingsForFragments({fragment(7)}).isEmpty();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    expect("unreadable link: History offers no recording for its row", !listed);
}

/* Issue #902: the limit's look at what it counts finding a name it cannot read (EACCES here) takes that recording out
 * of the usage: two 600 kB recordings, a 1 MB limit, the older one's subfolder closed: 600 kB counted, nothing goes. */
void
test_limit_look_drops_an_unreadable_recording_from_usage(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString sub = folder + QStringLiteral("/locked");
    QDir().mkpath(sub);
    if (!permissions_enforced(sub)) {
        DSD_FPRINTF(stdout, "SKIP limit look: this process reads any folder\n");
        return;
    }
    make_file(sub + QStringLiteral("/old.wav"), 600000);
    append(journal_of(folder),
           publish_line(QStringLiteral("0000000000000948-1"), QStringLiteral("locked/old.wav"), 1754000000, 1));
    record(folder, QStringLiteral("0000000000000948-2"), QStringLiteral("new.wav"), 1754000100, 2, 600000);
    s.pass();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.store.setLimitMb(1);
    s.pass();
    const bool healthyKept = QFileInfo::exists(folder + QStringLiteral("/new.wav"));
    const qint64 removed = s.store.removedByLimit();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    expect("limit look: the healthy recording stays, nothing removed", healthyKept && removed == 0);
    expect("limit look: the unreadable one kept", QFileInfo::exists(sub + QStringLiteral("/old.wav")));
}

#ifndef _WIN32
/* Issue #902: a folder.json that is a Unix socket is no folder of the app's (its open fails with ENXIO, not the
 * regular-file check): the folder is dropped rather than held unread forever. */
void
test_socket_marker_is_no_folder(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString id = QStringLiteral("0000000000000949-1");
    record(folder, id, QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    const QString marker = folder + QStringLiteral("/folder.json");
    QFile::remove(marker);
    // Bound by its name in the folder (a socket's path is short), from there: every path the store uses is absolute.
    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    DSD_MEMCPY(address.sun_path, "folder.json", sizeof "folder.json");
    const QString before = QDir::currentPath();
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    const bool bound = fd >= 0 && QDir::setCurrent(folder)
                       && bind(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof address) == 0;
    (void)QDir::setCurrent(before);
    if (fd >= 0) {
        (void)close(fd);
    }
    if (!bound) {
        DSD_FPRINTF(stdout, "SKIP socket marker: no socket here\n");
        return;
    }
    make_file(folder + QStringLiteral("/a.wav"), 1500);
    s.store.recheckRecordings({id});
    s.pass();
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    expect("socket marker: the folder dropped, the store settled",
           !all.value(QStringLiteral("loading")).toBool() && s.store.folders().isEmpty());
}
#endif

/* Issue #902: a recording a recheck found unreadable is never deleted, by a request queued before or after, even with
 * access back before its folder is read again: Delete all skips it and tells it. */
void
test_unreadable_entry_is_never_deleted(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString sub = folder + QStringLiteral("/locked");
    QDir().mkpath(sub);
    if (!permissions_enforced(sub)) {
        DSD_FPRINTF(stdout, "SKIP unreadable delete all: this process reads any folder\n");
        return;
    }
    const QString id = QStringLiteral("0000000000000950-1");
    make_file(sub + QStringLiteral("/x.wav"), 1000);
    append(journal_of(folder), publish_line(id, QStringLiteral("locked/x.wav"), 1754000000, 1));
    s.pass();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.store.recheckRecordings({id});
    s.pass();
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    expect("unreadable delete all: Delete all asked with access back", s.store.deleteAll());
    s.pass();
    expect("unreadable delete all: the file stays, told as skipped",
           QFileInfo::exists(sub + QStringLiteral("/x.wav")) && s.store.deletesSkipped() == 1);
}

/* Issue #902: the limit's check right before an unlink finding the name unreadable (its subfolder closed just then)
 * takes it out of the usage and goes on: two 600 kB recordings and a 1 MB limit leave 600 kB counted, so the healthy
 * recording stays. */
void
test_limit_unlink_check_drops_an_unreadable_recording(void) {
    Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("limit unreadable: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    const QString sub = folder + QStringLiteral("/locked");
    QDir().mkpath(sub);
    if (!permissions_enforced(sub)) {
        DSD_FPRINTF(stdout, "SKIP limit unreadable: this process reads any folder\n");
        return;
    }
    make_file(sub + QStringLiteral("/old.wav"), 600000);
    append(journal_of(folder),
           publish_line(QStringLiteral("0000000000000951-1"), QStringLiteral("locked/old.wav"), 1754000000, 1));
    record(folder, QStringLiteral("0000000000000951-2"), QStringLiteral("new.wav"), 1754000100, 2, 600000);
    Store s(roots.list(), false);
    std::atomic<bool> closed{false};
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/old.wav")) && !closed.exchange(true)) {
            QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
        return false;
    });
    s.limitQuietly(1);
    const bool healthy = QFileInfo::exists(folder + QStringLiteral("/new.wav"));
    QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    expect("limit unreadable: closed right before its unlink", closed.load());
    expect("limit unreadable: the healthy recording stays, the unreadable one kept and told",
           healthy && QFileInfo::exists(sub + QStringLiteral("/old.wav")) && s.store.removedByLimit() == 0
               && s.store.deletesSkipped() == 1);
}

#ifndef _WIN32
/* Issue #902: a journal naming sub/r.wav where sub is a dangling link at startup: the name was never looked up, so the
 * recording is kept (unreadable), never dropped, and its journal line stays through compaction. */
void
test_dangling_parent_keeps_the_record(void) {
    Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("dangling parent: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    expect("dangling parent: a dangling link", QFile::link(QStringLiteral("nowhere"), folder + QStringLiteral("/sub")));
    append(journal_of(folder),
           publish_line(QStringLiteral("0000000000000952-1"), QStringLiteral("sub/r.wav"), 1754000000, 1));
    Store s(roots.list());
    s.pass();
    expect("dangling parent: the journal line stays", read_all(journal_of(folder)).contains("sub/r.wav"));
}
#endif

/* Issue #902: a session start that cannot read the folder.json of a folder the worker is reading (the read saw it
 * before) ends that read: nothing is committed, and the folder stays unread until a whole read can tell its owner. */
void
test_owner_lookup_failure_ends_a_read_in_flight(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString id = QStringLiteral("0000000000000953-1");
    record(folder, id, QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    const QString marker = folder + QStringLiteral("/folder.json");
    std::atomic<int> lookedUp{-1};
    s.store.setFolderLoadHookForTest([&](const QString& path) {
        if (path == folder && lookedUp.load() < 0) {
            lookedUp = take_read_access(marker) ? 1 : 0;
            (void)s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
            s.store.endSession();
        }
    });
    make_file(folder + QStringLiteral("/a.wav"), 1500);
    s.store.recheckRecordings({id});
    s.pass();
    s.pass();
    s.store.setFolderLoadHookForTest({});
    s.pass();
    if (lookedUp.load() == 0) {
        DSD_FPRINTF(stdout, "SKIP read in flight: this process reads any file\n");
        return;
    }
    const bool refused = bulk_requests_refused(s.store, QString(kUidA));
    QFile::setPermissions(marker, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("read in flight: the lookup failed during the read", lookedUp.load() == 1);
    expect("read in flight: refused while the owner cannot be read", refused);
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("read in flight: read whole once it can be",
           !all.value(QStringLiteral("loading")).toBool() && all.value(QStringLiteral("count")).toInt() == 1);
}

/* Issue #902: an owner's label changed on storage while it was away, and its folder.json unreadable as it came back:
 * the folder keeps the index's label until a read succeeds, then takes the label that read finds. */
void
test_marker_retry_takes_the_label_on_disk(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("label retry: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    record(folder, QStringLiteral("0000000000000954-1"), QStringLiteral("a.wav"), 1754000000, 1);
    Store s(roots.list());
    expect("label retry: the storage goes", QDir().rename(ext, away));
    s.pass();
    const QString awayFolder = away + QStringLiteral("/Call recordings/County (1a2b3c4d)");
    expect("label retry: relabelled while away", write_owner_json(awayFolder, QString(kUidA), QStringLiteral("Metro")));
    if (!take_read_access(awayFolder + QStringLiteral("/folder.json"))) {
        DSD_FPRINTF(stdout, "SKIP label retry: this process reads any file\n");
        return;
    }
    expect("label retry: the storage comes back", QDir().rename(away, ext));
    s.pass();
    QFile::setPermissions(folder + QStringLiteral("/folder.json"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    s.pass();
    QString name;
    for (const QVariant& entry : s.store.folders()) {
        name = entry.toMap().value(QStringLiteral("name")).toString();
    }
    expect("label retry: the label read from disk", name == QStringLiteral("Metro"));
}

/* Issue #902: a session start that finds the preferred storage missing (it records on the next) remembers it, so
 * storage back before any pass samples it is still listed in full: an owner folder put on it meanwhile is exported. */
void
test_session_start_remembers_storage_away(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000955-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    std::atomic<int> ends{0};
    QString elsewhere;
    s.store.setPassEndHookForTest([&]() {
        if (ends++ == 0 && QDir().rename(ext, away)) {
            const QString d = away + QStringLiteral("/Call recordings/Metro (5e6f7a8b)");
            (void)write_owner_json(d, QString(kUidC), QStringLiteral("Metro"));
            record(d, QStringLiteral("0000000000000955-2"), QStringLiteral("d.wav"), 1754000100, 2);
            elsewhere = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other"));
            s.store.endSession();
            (void)QDir().rename(away, ext);
        }
    });
    s.pass();
    s.pass();
    s.store.setPassEndHookForTest({});
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("seen at start: the session went to the other storage", elsewhere.startsWith(roots.internal));
    expect("seen at start: Export all has the folder put on it while away",
           all.value(QStringLiteral("count")).toInt() == 2);
}

/* I1: a root whose recordings folder is missing at a cold start (its parent there) holds nothing, and is looked at
 * again each pass: once the folder is there, holding recordings, it is listed in full and they are indexed. A fresh
 * install records into it as before: the session's start makes the folder, and its recordings are indexed. */
void
test_folder_missing_at_a_cold_start(void) {
    {
        const Roots roots;
        Store s(roots.list());
        expect("cold start, folder missing: the store is there to record into", s.store.available());
        const QString staged = roots.dir.path() + QStringLiteral("/staged");
        const QString owner = staged + QStringLiteral("/County (1a2b3c4d)");
        expect("cold start: a folder made elsewhere",
               write_owner_json(owner, QString(kUidA), QStringLiteral("County")));
        record(owner, QStringLiteral("0000000000000961-1"), QStringLiteral("a.wav"), 1754000000, 1);
        expect("cold start: the folder put in place", QDir().rename(staged, roots.external));
        // An export pressed before any pass finds it there: refused as loading, its read asked for at once (I1).
        const QVariantMap early = s.store.selectForExport(QString(), QStringLiteral("all"));
        s.store.releaseSelection(early.value(QStringLiteral("selection")).toInt());
        expect("cold start: an export before the pass is refused as loading",
               early.value(QStringLiteral("loading")).toBool() && early.value(QStringLiteral("count")).toInt() == 0);
        s.pass();
        expect("cold start: its recording is indexed and available", listed_as(s.store, 1, true));
        const QVariantMap after = s.store.selectForExport(QString(), QStringLiteral("all"));
        s.store.releaseSelection(after.value(QStringLiteral("selection")).toInt());
        expect("cold start: after the pass, the export has it", after.value(QStringLiteral("count")).toInt() == 1);
    }
    {
        const Roots roots;
        Store s(roots.list());
        // The folder the session makes is the store's own: no pass after the start lists it (its flag cleared, I1).
        // Counted from before the start, so the pass the start wakes cannot run before the count does.
        std::atomic<int> listings{0};
        s.store.setRootReadHookForTest([&](const QString& root) {
            if (root.startsWith(roots.external)) {
                listings++;
            }
        });
        const QString made = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        expect("fresh install: the session made its folder", made.startsWith(roots.external + QLatin1Char('/')));
        record(made, QStringLiteral("0000000000000961-2"), QStringLiteral("b.wav"), 1754000100, 2);
        s.store.endSession();
        s.pass();
        s.store.setRootReadHookForTest({});
        expect("fresh install: the pass after the session lists nothing", listings.load() == 0);
        expect("fresh install: its recording is indexed and available", listed_as(s.store, 2, true));
    }
}

/* m1, scenario B: a session start that makes a flagged root's directory between a pass's look at that root (which
 * found it missing) and the write of what the look found: the stale look writes no flag, so the next pass does not
 * list the store's own new folder. */
void
test_session_made_root_inside_a_look_writes_no_flag(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    expect("made in a look: the external storage away at the start", QDir(ext).removeRecursively());
    Store s(roots.list());
    QString made;
    s.store.setRootLookHookForTest([&](const QString& root) {
        if (root == roots.internal && made.isEmpty()) {
            made = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other"));
            s.store.endSession();
        }
    });
    s.pass();
    s.store.setRootLookHookForTest({});
    expect("made in a look: the session made the internal folder inside the look",
           made.startsWith(roots.internal + QLatin1Char('/')) && QFileInfo(made).isDir());
    std::atomic<int> listings{0};
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.startsWith(roots.internal)) {
            listings++;
        }
    });
    s.pass();
    s.pass();
    s.store.setRootReadHookForTest({});
    expect("made in a look: the internal root was not listed after", listings.load() == 0);
}

/* m1: a pass looks at each root's missing flag as it is at that root's turn. A session start during the pass (here,
 * while the storage before it is listed) that makes a flagged root's directory clears the flag, and the pass then
 * lists nothing there: its folder is the store's own, new and read. */
void
test_session_made_root_during_a_pass_is_not_listed(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    expect("made in a pass: the external storage away at the start", QDir(ext).removeRecursively());
    Store s(roots.list());
    const QString staged = roots.dir.path() + QStringLiteral("/staged");
    const QString owner = staged + QStringLiteral("/Call recordings/County (1a2b3c4d)");
    expect("made in a pass: a folder made elsewhere",
           write_owner_json(owner, QString(kUidA), QStringLiteral("County")));
    record(owner, QStringLiteral("0000000000000964-1"), QStringLiteral("a.wav"), 1754000000, 1);
    std::atomic<int> internalListings{0};
    QString made;
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.startsWith(roots.internal)) {
            internalListings++;
        } else if (root.startsWith(ext + QLatin1Char('/')) && made.isEmpty()) {
            made = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other"));
            s.store.endSession();
        }
    });
    expect("made in a pass: the external storage back", QDir().rename(staged, ext));
    s.pass();
    s.pass();
    s.store.setRootReadHookForTest({});
    expect("made in a pass: the session made the internal folder", made.startsWith(roots.internal + QLatin1Char('/')));
    expect("made in a pass: the internal root was not listed", internalListings.load() == 0);
    expect("made in a pass: the external recording is indexed", listed_as(s.store, 1, true));
}

/* m1: an export pressed during a pass, before a flagged root's turn, that finds its folder there notes it seen away;
 * the pass's listing of it (its folder appeared) takes that note, so the next pass does not list it again. */
void
test_export_note_during_a_pass_is_taken_by_its_listing(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    expect("note in a pass: the external storage away at the start", QDir(ext).removeRecursively());
    Store s(roots.list());
    const QString staged = roots.dir.path() + QStringLiteral("/staged");
    expect("note in a pass: a parent made elsewhere", QDir().mkpath(staged + QStringLiteral("/Call recordings")));
    const QString inbound = roots.dir.path() + QStringLiteral("/inbound");
    const QString owner = inbound + QStringLiteral("/County (1a2b3c4d)");
    expect("note in a pass: a folder made elsewhere",
           write_owner_json(owner, QString(kUidA), QStringLiteral("County")));
    record(owner, QStringLiteral("0000000000000965-1"), QStringLiteral("a.wav"), 1754000000, 1);
    std::atomic<int> internalListings{0};
    std::atomic<bool> pressed{false};
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.startsWith(roots.internal)) {
            internalListings++;
        } else if (root.startsWith(ext + QLatin1Char('/')) && !pressed.exchange(true)) {
            // Before the internal root's turn: its folder appears, and an export finds it there.
            (void)QDir().rename(inbound, roots.internal);
            const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
            s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
        }
    });
    expect("note in a pass: the external storage back", QDir().rename(staged, ext));
    s.pass();
    s.pass();
    s.store.setRootReadHookForTest({});
    expect("note in a pass: the export was pressed during the pass", pressed.load());
    expect("note in a pass: the internal root was listed once", internalListings.load() == 1);
    expect("note in a pass: its recording is indexed", listed_as(s.store, 1, true));
}

/* r12 Part B: a root's directory that goes missing during its listing, after entryList() and before the final check,
 * with nothing indexed on it, is installed there and empty but remembered as missing, so the pass after it is back
 * lists it and indexes what it holds. */
void
test_directory_gone_during_its_listing_is_remembered_missing(void) {
    const Roots roots;
    expect("gone in listing: the recordings folder there at the start", QDir().mkpath(roots.external));
    const QString staged = roots.dir.path() + QStringLiteral("/staged");
    const QString owner = staged + QStringLiteral("/County (1a2b3c4d)");
    Store s(roots.list(), false);
    std::atomic<bool> taken{false};
    std::atomic<bool> back{false};
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root == roots.external && !taken.exchange(true)) {
            (void)QDir().rename(roots.external, staged); // listed: the directory goes before the final check
        }
    });
    // Back, holding a recording, as that pass ends: before any other pass looks at it.
    s.store.setPassEndHookForTest([&]() {
        if (taken.load() && !back.exchange(true)) {
            (void)write_owner_json(owner, QString(kUidA), QStringLiteral("County"));
            record(owner, QStringLiteral("0000000000000966-1"), QStringLiteral("a.wav"), 1754000000, 1);
            (void)QDir().rename(staged, roots.external);
        }
    });
    s.start();
    s.pass();
    s.store.setRootReadHookForTest({});
    s.store.setPassEndHookForTest({});
    expect("gone in listing: it went during the listing and came back", taken.load() && back.load());
    expect("gone in listing: the store reads as there", s.store.available());
    expect("gone in listing: back, its recording is indexed", listed_as(s.store, 1, true));
}

/* r13: a root with no rows whose directory goes after the pass's look at it, seen by a folder read in that pass, is
 * remembered as missing as the look itself would, so the pass after it is back lists it: an owner folder put on it
 * meanwhile is found. */
void
test_directory_gone_during_a_folder_read_is_remembered_missing(void) {
    const Roots roots;
    const QString emptied = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("gone in a read: a folder with no recording",
           write_owner_json(emptied, QString(kUidA), QStringLiteral("County")));
    const QString staged = roots.dir.path() + QStringLiteral("/staged");
    const QString other = staged + QStringLiteral("/Metro (5e6f7a8b)");
    Store s(roots.list(), false);
    std::atomic<bool> taken{false};
    std::atomic<bool> back{false};
    s.store.setFolderLoadHookForTest([&](const QString& folder) {
        if (folder == emptied && !taken.exchange(true)) {
            (void)QDir().rename(roots.external, staged); // the folder's read is under way: the directory goes
        }
    });
    s.store.setPassEndHookForTest([&]() {
        if (taken.load() && !back.exchange(true)) {
            (void)write_owner_json(other, QString(kUidC), QStringLiteral("Metro"));
            record(other, QStringLiteral("0000000000000967-1"), QStringLiteral("m.wav"), 1754000000, 1);
            (void)QDir().rename(staged, roots.external);
        }
    });
    s.start();
    s.pass();
    s.store.setFolderLoadHookForTest({});
    s.store.setPassEndHookForTest({});
    expect("gone in a read: it went during the read and came back", taken.load() && back.load());
    expect("gone in a read: the store reads as there", s.store.available());
    expect("gone in a read: back, the folder put on it is found", listed_as(s.store, 1, true));
}

/* r13: a session start that sees a returning root's directory gone while its listing is in flight queues another
 * listing, though it records elsewhere (its owner's folder on the internal storage): a different directory back before
 * the listing's end has its folders found by the listing after it. */
void
test_session_seeing_a_listing_root_gone_queues_another(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString extAway = roots.dir.path() + QStringLiteral("/ext-away");
    const QString aside = roots.dir.path() + QStringLiteral("/aside");
    expect("listing gone: the external storage away", QDir().rename(ext, extAway));
    Store s(roots.list());
    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other"));
    s.store.endSession();
    expect("listing gone: the owner's folder on the internal storage", b.startsWith(roots.internal + QLatin1Char('/')));
    s.pass();
    expect("listing gone: the external storage back",
           QDir().rename(extAway, ext)
               && write_owner_json(roots.external + QStringLiteral("/County (1a2b3c4d)"), QString(kUidA),
                                   QStringLiteral("County")));
    std::atomic<int> listings{0};
    QString again;
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root != roots.external || listings++ != 0) {
            return;
        }
        // Listed: its directory goes, a session start sees it gone (recording on the internal storage), and another
        // directory is there before the listing's last look.
        (void)QDir().rename(roots.external, aside);
        again = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other"));
        s.store.endSession();
        const QString metro = roots.external + QStringLiteral("/Metro (5e6f7a8b)");
        (void)write_owner_json(metro, QString(kUidC), QStringLiteral("Metro"));
        record(metro, QStringLiteral("0000000000000968-1"), QStringLiteral("m.wav"), 1754000000, 1);
    });
    s.pass();
    s.pass();
    s.store.setRootReadHookForTest({});
    expect("listing gone: the session recorded on the internal storage", again == b);
    expect("listing gone: another listing ran after it", listings.load() >= 2);
    expect("listing gone: the other directory's folder is found", listed_as(s.store, 1, true));
}

/* r14t: a folder's first read is unavailable to every reader until its last slice, as a read again is (R78): between
 * slices a sheet's read, a playlist's admission and a share's hold all refuse its rows; after the last, they take
 * them. */
void
test_first_read_waits_for_its_last_slice(void) {
    const Roots roots;
    Store s(roots.list());
    const QString staged = roots.dir.path() + QStringLiteral("/staged");
    const QString owner = staged + QStringLiteral("/County (1a2b3c4d)");
    expect("first read: a folder made elsewhere", write_owner_json(owner, QString(kUidA), QStringLiteral("County")));
    QByteArray journal;
    for (int i = 0; i < 400; i++) {
        const QString file = QStringLiteral("n%1.wav").arg(i);
        make_file(owner + QLatin1Char('/') + file, 1000);
        journal.append(publish_line(QStringLiteral("0000000000000969-%1").arg(100 + i), file, 1754000100 + i, -1));
    }
    make_file(owner + QStringLiteral("/k.wav"), 1000);
    journal.append(publish_line(QStringLiteral("0000000000000969-1"), QStringLiteral("k.wav"), 1754000000, 1));
    append(journal_of(owner), journal);
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("first read: the folder put in place",
           QDir().mkpath(QFileInfo(roots.external).absolutePath()) && QDir().rename(staged, roots.external));
    std::atomic<int> slices{0};
    std::atomic<int> taken{0};
    s.store.setSliceHookForTest([&](const QString& path) {
        if (path != folder) {
            return;
        }
        slices++;
        const QVariantList sheet = s.store.recordingsForFragments({fragment(1)});
        const QVariantList listed = s.store.recordingsForFragmentLists({{fragment(1)}}).value(0);
        const int hold = s.store.selectPath(folder + QStringLiteral("/n0.wav"));
        s.store.releaseSelection(hold);
        const auto available = [](const QVariantList& rows) {
            return !rows.isEmpty() && rows.value(0).toMap().value(QStringLiteral("available")).toBool();
        };
        taken += (available(sheet) ? 1 : 0) + (available(listed) ? 1 : 0) + (hold > 0 ? 1 : 0);
    });
    s.pass();
    s.store.setSliceHookForTest({});
    expect("first read: read slice by slice", slices.load() >= 2);
    expect("first read: between slices no reader took a row", taken.load() == 0);
    expect("first read: after its last slice, the row is available", listed_as(s.store, 1, true));
    const int after = s.store.selectPath(folder + QStringLiteral("/n0.wav"));
    expect("first read: after its last slice, a share's hold is taken", after > 0);
    s.store.releaseSelection(after);
}

/* r14t: a session start that sees an indexed root's recordings folder missing (its parent there) notes the root seen
 * away though it records on another root: its rows are unavailable at once, and the folder back before any pass is
 * read again in full, its rows available after. */
void
test_session_on_another_root_notes_a_missing_indexed_root(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString extAway = roots.dir.path() + QStringLiteral("/ext-away");
    const QString renamed = roots.dir.path() + QStringLiteral("/ext/renamed");
    expect("other root: the external storage away", QDir().rename(ext, extAway));
    // Its folder made whole while it is away, so no pass can list it half made.
    const QString awayFolder = extAway + QStringLiteral("/Call recordings/County (1a2b3c4d)");
    expect("other root: a folder on it", write_owner_json(awayFolder, QString(kUidA), QStringLiteral("County")));
    record(awayFolder, QStringLiteral("0000000000000970-1"), QStringLiteral("a.wav"), 1754000000, 1);
    Store s(roots.list());
    const QString b = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other"));
    s.store.endSession();
    expect("other root: the owner's folder on the internal storage", b.startsWith(roots.internal + QLatin1Char('/')));
    s.pass();
    expect("other root: the external storage back", QDir().rename(extAway, ext));
    s.pass();
    expect("other root: indexed", listed_as(s.store, 1, true));
    // On the worker, between two passes (so no pass marks the root away meanwhile): the recordings folder goes, a
    // session start on the other root sees it missing, a new owner folder is put on it, and it comes back.
    std::atomic<int> ends{0};
    std::atomic<bool> unavailableAtOnce{false};
    QString again;
    s.store.setPassEndHookForTest([&]() {
        if (ends++ != 0 || !QDir().rename(roots.external, renamed)) {
            return;
        }
        again = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other"));
        s.store.endSession();
        unavailableAtOnce = listed_as(s.store, 1, false);
        const QString metro = renamed + QStringLiteral("/Metro (5e6f7a8b)");
        (void)write_owner_json(metro, QString(kUidC), QStringLiteral("Metro"));
        record(metro, QStringLiteral("0000000000000970-2"), QStringLiteral("m.wav"), 1754000100, 2);
        (void)QDir().rename(renamed, roots.external);
    });
    s.pass();
    s.pass();
    s.store.setPassEndHookForTest({});
    expect("other root: the session recorded on the internal storage", ends.load() >= 1 && again == b);
    expect("other root: the missing root's row is unavailable at once", unavailableAtOnce.load());
    expect("other root: read again, the row is available", listed_as(s.store, 1, true));
    expect("other root: read again in full, the folder put on it is found", listed_as(s.store, 2, true));
}

/* r15t: a session start never records onto a root held away that is back but not read in full. The card comes back
 * between the start's look at its roots (its directory missing then) and its choice of folder, with this owner's folder
 * under another name: the start records on the fallback; the pass after lists the card, and the next start takes the
 * card's own folder, with no second folder made there. */
void
test_session_never_records_onto_a_root_back_unlisted(void) {
    const Roots roots;
    const QString moved = roots.dir.path() + QStringLiteral("/ext/moved");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    expect("back unlisted: the owner's folder on the card", a.startsWith(roots.external + QLatin1Char('/')));
    record(a, QStringLiteral("0000000000000971-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    expect("back unlisted: the card's folder moved away", QDir().rename(roots.external, moved));
    s.pass(); // held away, its row kept
    expect("back unlisted: held away", listed_as(s.store, 1, false));
    const QString renamed = QStringLiteral("Older (1a2b3c4d)");
    expect("back unlisted: the owner's folder renamed on the card",
           QDir(moved).rename(QFileInfo(a).fileName(), renamed));
    std::atomic<bool> restored{false};
    s.store.setSessionRootsHookForTest([&]() {
        if (!restored.exchange(true)) {
            (void)QDir().rename(moved, roots.external); // back between the look and the choice of folder
        }
    });
    const QString during = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    s.store.setSessionRootsHookForTest({});
    expect("back unlisted: the card came back inside the start", restored.load() && QFileInfo(roots.external).isDir());
    expect("back unlisted: the start records on the fallback", during.startsWith(roots.internal + QLatin1Char('/')));
    s.pass();
    expect("back unlisted: listed, the card's row is available", listed_as(s.store, 1, true));
    const QString after = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    expect("back unlisted: the next start takes the card's own folder",
           after.startsWith(roots.external + QLatin1Char('/')));
    int onCard = 0;
    for (const QString& path : s.store.folderPathsForTest()) {
        onCard += path.startsWith(roots.external + QLatin1Char('/')) ? 1 : 0;
    }
    expect("back unlisted: no second folder made on the card", onCard == 1);
}

/* r16t: a first read that read recordings, whose recordings directory goes before the read's last look at its root,
 * is not committed: none of its rows is offered while the directory is missing; once it is back, a listing reads it
 * and they are available. */
void
test_first_read_of_a_vanished_directory_is_not_committed(void) {
    const Roots roots;
    Store s(roots.list());
    const QString staged = roots.dir.path() + QStringLiteral("/staged");
    const QString aside = roots.dir.path() + QStringLiteral("/aside");
    const QString owner = staged + QStringLiteral("/County (1a2b3c4d)");
    expect("vanished read: a folder made elsewhere", write_owner_json(owner, QString(kUidA), QStringLiteral("County")));
    record(owner, QStringLiteral("0000000000000972-1"), QStringLiteral("a.wav"), 1754000000, 1);
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    std::atomic<bool> taken{false};
    s.store.setJournalReadHookForTest([&](const QString& path) {
        if (path == folder && !taken.exchange(true)) {
            (void)QDir().rename(roots.external, aside); // read: the directory goes before the read's last look
        }
    });
    expect("vanished read: the folder put in place", QDir().rename(staged, roots.external));
    s.pass();
    s.store.setJournalReadHookForTest({});
    expect("vanished read: it went during the read", taken.load() && !QFileInfo::exists(roots.external));
    s.pass(); // the pass the discarded read asked for
    expect("vanished read: none of its rows is offered",
           s.store.recordingsForFragments({fragment(1)}).isEmpty()
               && s.store.recordingsForFragmentLists({{fragment(1)}}).value(0).isEmpty());
    expect("vanished read: the directory back", QDir().rename(aside, roots.external));
    s.pass();
    expect("vanished read: back and listed, its row is available", listed_as(s.store, 1, true));
}

/* r16t: a start that passes over a root back but not listed asks for its listing even when it finds no folder. One
 * root, held away; it comes back between the start's look and its choice of folder: the start gets no folder, and the
 * listing runs with no other request (no endSession(), no pass() asked for here). */
void
test_start_with_no_folder_asks_for_the_listing(void) {
    const Roots roots;
    const QString moved = roots.dir.path() + QStringLiteral("/ext/moved");
    Store s({roots.external});
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000973-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    expect("no folder: the card's folder moved away", QDir().rename(roots.external, moved));
    s.pass(); // held away
    expect("no folder: held away", listed_as(s.store, 1, false));
    std::atomic<bool> restored{false};
    s.store.setSessionRootsHookForTest([&]() {
        if (!restored.exchange(true)) {
            (void)QDir().rename(moved, roots.external);
        }
    });
    const quint64 before = s.store.passCountForTest();
    const QString none = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.setSessionRootsHookForTest({});
    expect("no folder: the start gets none", restored.load() && none.isEmpty());
    QElapsedTimer waited;
    waited.start();
    while (!listed_as(s.store, 1, true) && waited.elapsed() < 5000) {
        QThread::msleep(5);
    }
    expect("no folder: the listing it asked for ran",
           s.store.passCountForTest() > before && listed_as(s.store, 1, true));
}

/* r16t: the owner-folder choice is made only among roots still usable once its checks are done. The card holds two
 * folders for one owner. A test hook walks them worst first and makes the second owner check (of the better folder,
 * after the other was saved as best) find the card gone: the start records on the fallback, never in the saved one. */
void
test_owner_choice_drops_a_root_its_checks_found_gone(void) {
    const Roots roots;
    Store s(roots.list());
    const QString first = roots.external + QStringLiteral("/A (1a2b3c4d)");
    const QString second = roots.external + QStringLiteral("/B (1a2b3c4d)");
    expect("owner choice: two folders for one owner",
           write_owner_json(first, QString(kUidA), QStringLiteral("County"))
               && write_owner_json(second, QString(kUidA), QStringLiteral("County")));
    s.pass();
    s.pass();
    QStringList checked;
    s.store.setOwnerCheckHookForTest([&checked](const QString& folder) {
        checked.append(folder);
        return checked.size() == 2; // the check after a best was saved finds the card gone
    });
    const QString path = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    s.store.setOwnerCheckHookForTest({});
    expect("owner choice: the worse folder was saved, then the better one checked",
           checked.size() == 2 && checked.value(0) == second && checked.value(1) == first);
    expect("owner choice: the start found the card gone and recorded on the fallback",
           path.startsWith(roots.internal + QLatin1Char('/')));
}

/* m2: only a root with indexed rows is held away. A desktop store's one root whose folder holds no recording (its only
 * one deleted in the app) reads as there and empty when its directory is removed, so the screens never say its storage
 * is away; it is listed again once the directory is back. */
void
test_root_without_rows_is_not_held_away(void) {
    const Roots roots;
    Store s({roots.external});
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000963-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    expect("no rows: indexed", listed_as(s.store, 1, true));
    expect("no rows: its only recording deleted", s.store.deleteRecording(QStringLiteral("0000000000000963-1")));
    s.pass();
    expect("no rows: none left, the folder kept",
           s.store.recordingsForFragments({fragment(1)}).isEmpty() && s.store.folderPathsForTest().contains(a));
    const QString staged = roots.dir.path() + QStringLiteral("/kept");
    expect("no rows: the root's directory removed", QDir().rename(roots.external, staged));
    s.pass();
    expect("no rows: the storage still reads as there", s.store.available());
    record(staged + QLatin1Char('/') + QFileInfo(a).fileName(), QStringLiteral("0000000000000963-2"),
           QStringLiteral("b.wav"), 1754000100, 2);
    expect("no rows: the directory back", QDir().rename(staged, roots.external));
    s.pass();
    expect("no rows: back, listed again", listed_as(s.store, 2, true));
}

/* I2: a session start that makes a root's recordings folder over folders the index holds on it (renamed away before
 * any pass marked the root away) has the root read again in full at once: the old rows are unavailable from the start
 * on, and after the read they are gone and the session's recordings are indexed. */
void
test_session_making_the_folder_reads_its_root_again(void) {
    const Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-renamed");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000962-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    expect("made again: indexed", listed_as(s.store, 1, true));
    expect("made again: the folder renamed away, no pass since", QDir().rename(roots.external, away));
    // The read again the start asks for is held at its listing, before anything of it is installed, until the row
    // has been looked at (r12): the pass the start wakes cannot finish first.
    std::mutex gate;
    std::condition_variable turned;
    bool reached = false;
    bool released = false;
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (!root.startsWith(roots.external)) {
            return;
        }
        std::unique_lock<std::mutex> lock(gate);
        reached = true;
        turned.notify_all();
        turned.wait_for(lock, std::chrono::seconds(10), [&released] { return released; });
    });
    const QString made = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    expect("made again: the session made the folder again", made == a && QFileInfo(made).isDir());
    {
        std::unique_lock<std::mutex> lock(gate);
        expect("made again: its read again is held at its listing",
               turned.wait_for(lock, std::chrono::seconds(10), [&reached] { return reached; }));
    }
    expect("made again: the old row is unavailable at once", listed_as(s.store, 1, false));
    {
        const std::lock_guard<std::mutex> lock(gate);
        released = true;
    }
    turned.notify_all();
    record(made, QStringLiteral("0000000000000962-2"), QStringLiteral("b.wav"), 1754000100, 2);
    s.store.endSession();
    s.pass();
    s.store.setRootReadHookForTest({});
    expect("made again: read again, the old row is gone", s.store.recordingsForFragments({fragment(1)}).isEmpty());
    expect("made again: the session's recording is indexed", listed_as(s.store, 2, true));
}

/* Issue #902: a delete whose root a session start found missing between the delete's checks and its unlink (back by
 * then) is held for the root's listing in full, which drops it: the file stays. */
void
test_delete_held_when_its_root_was_seen_away(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("seen away at unlink: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    const QString id = QStringLiteral("0000000000000956-1");
    record(folder, id, QStringLiteral("a.wav"), 1754000000, 1);
    Store s(roots.list(), false);
    std::atomic<bool> seen{false};
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/a.wav")) && !seen.exchange(true) && QDir().rename(ext, away)) {
            (void)s.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Other"));
            s.store.endSession();
            (void)QDir().rename(away, ext);
        }
        return false;
    });
    s.start();
    expect("seen away at unlink: the delete asked", s.store.deleteRecording(id));
    s.pass();
    s.pass();
    expect("seen away at unlink: the start found it missing", seen.load());
    expect("seen away at unlink: the file stays, the delete dropped and told",
           QFileInfo::exists(folder + QStringLiteral("/a.wav")) && s.store.deletesWaiting() == 0
               && s.store.deletesCancelled() == 1);
}

/* Issue #902: a session start that cannot read the folder.json of a folder whose new journal lines the worker has read
 * (not applied yet) ends that tail: the new recording is not shown, and the folder is read whole once it can be. */
void
test_owner_lookup_failure_ends_a_tail_in_flight(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000957-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    const QString marker = folder + QStringLiteral("/folder.json");
    std::atomic<int> lookedUp{-1};
    s.store.setTailHookForTest([&](const QString& path) {
        if (path == folder && lookedUp.load() < 0) {
            lookedUp = take_read_access(marker) ? 1 : 0;
            (void)s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
            s.store.endSession();
        }
    });
    record(folder, QStringLiteral("0000000000000957-2"), QStringLiteral("b.wav"), 1754000100, 2);
    s.pass();
    s.store.setTailHookForTest({});
    if (lookedUp.load() == 0) {
        DSD_FPRINTF(stdout, "SKIP tail in flight: this process reads any file\n");
        return;
    }
    const bool hidden = !s.store.hasRecordingForFragments({fragment(2)});
    const bool refused = bulk_requests_refused(s.store, QString(kUidA));
    QFile::setPermissions(marker, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("tail in flight: the lookup failed during the tail", lookedUp.load() == 1);
    expect("tail in flight: the new recording not shown, requests refused", hidden && refused);
    s.pass();
    expect("tail in flight: read whole once it can be", s.store.hasRecordingForFragments({fragment(2)}));
}

#ifndef _WIN32
/* Issue #902: a recheck that finds a link where a recording was, with a delete of it waiting, settles that delete as
 * not done and tells it (not as a recording gone): the link stays. */
void
test_recheck_finding_a_link_tells_its_delete(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString id = QStringLiteral("0000000000000958-1");
    record(folder, id, QStringLiteral("r.wav"), 1754000000, 1);
    make_file(folder + QStringLiteral("/elsewhere.bin"), 1000);
    s.pass();
    const int held = s.store.selectRecordings({id}).value(QStringLiteral("selection")).toInt();
    expect("recheck link: the delete asked, held", held > 0 && s.store.deleteRecording(id));
    s.pass();
    QFile::remove(folder + QStringLiteral("/r.wav"));
    expect("recheck link: the name is a link now",
           QFile::link(QStringLiteral("elsewhere.bin"), folder + QStringLiteral("/r.wav")));
    s.store.recheckRecordings({id});
    s.store.releaseSelection(held);
    s.pass();
    expect("recheck link: the link stays, the delete told as not done",
           QFileInfo(folder + QStringLiteral("/r.wav")).isSymLink() && s.store.deletesSkipped() == 1
               && s.store.deletesWaiting() == 0);
}
#endif

/* Issue #902: a folder whose journal cannot be read is not read again every pass: its retry waits (30 s by default),
 * and it stays unread meanwhile. */
void
test_unreadable_journal_retry_backs_off(void) {
    Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("retry backs off: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    record(folder, QStringLiteral("0000000000000959-1"), QStringLiteral("a.wav"), 1754000000, 1);
    if (!take_read_access(journal_of(folder))) {
        DSD_FPRINTF(stdout, "SKIP retry backs off: this process reads any file\n");
        return;
    }
    Store s(roots.list(), false);
    s.store.setReadRetryForTest(3600000);
    std::atomic<int> reads{0};
    s.store.setFolderLoadHookForTest([&](const QString& path) {
        if (path == folder) {
            reads++;
        }
    });
    s.start();
    s.pass();
    s.pass();
    s.pass();
    const bool refused = bulk_requests_refused(s.store, QString(kUidA));
    QFile::setPermissions(journal_of(folder), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("retry backs off: read once in four passes", reads.load() == 1);
    expect("retry backs off: unread meanwhile", refused);
}

/* Issue #902: a delete request that finds a root missing makes every selection stale at once, before any pass: a
 * transfer's token from before the loss no longer stands. */
void
test_root_seen_away_by_a_delete_ends_tokens(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString id = QStringLiteral("0000000000000960-1");
    record(folder, id, QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    const int selection = s.store.selectRecordings({id}).value(QStringLiteral("selection")).toInt();
    RecordingSelectionToken token;
    expect("loss seen by a delete: a token", s.store.selectionToken(selection, &token) && token.current());
    expect("loss seen by a delete: the storage goes", QDir().rename(ext, away));
    expect("loss seen by a delete: the delete finds it missing",
           !s.store.deleteFolder(QString(kUidA)) && s.store.deleteRefusal() == QLatin1String("storageAway"));
    const bool stale = !token.current();
    expect("loss seen by a delete: the storage comes back", QDir().rename(away, ext));
    s.store.releaseSelection(selection);
    expect("loss seen by a delete: the token is stale at once", stale);
}

/* Issue #902: the limit's check right before an unlink finding the older recording shrunk (600 kB counted, 100 kB
 * found) counts it at the size found: 700 kB is under the 1 MB limit's target, so nothing else goes. */
void
test_limit_counts_the_size_its_unlink_check_found(void) {
    Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("limit size found: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    record(folder, QStringLiteral("0000000000000961-1"), QStringLiteral("old.wav"), 1754000000, 1, 600000);
    record(folder, QStringLiteral("0000000000000961-2"), QStringLiteral("new.wav"), 1754000100, 2, 600000);
    Store s(roots.list(), false);
    std::atomic<bool> shrunk{false};
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/old.wav")) && !shrunk.exchange(true)) {
            make_file(path, 100000);
        }
        return false;
    });
    s.limitQuietly(1);
    expect("limit size found: shrunk right before its unlink", shrunk.load());
    expect("limit size found: skipped and told, nothing else removed",
           QFileInfo::exists(folder + QStringLiteral("/old.wav"))
               && QFileInfo::exists(folder + QStringLiteral("/new.wav")) && s.store.removedByLimit() == 0
               && s.store.deletesSkipped() == 1);
    // Accounted at the size found from then on (Settings, the next limit pass).
    expect("limit size found: the usage shows the size found", s.store.usedBytes() == 700000);
}

/* Issue #902: a recording a save could not open (mode 000 in a folder that can be searched: its size and time still
 * read) is looked at again by opening it: still not readable, it is out of every selection, its journal line kept, so
 * Export all takes the others. */
void
test_unopenable_recording_leaves_selections(void) {
    Roots roots;
    Store s(roots.list());
    ServedHost host;
    dsd_qt::recording_store_serve_host(&s.store, &host);
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString idA = QStringLiteral("0000000000000962-1");
    record(folder, idA, QStringLiteral("a.wav"), 1754000000, 1);
    record(folder, QStringLiteral("0000000000000962-2"), QStringLiteral("b.wav"), 1754000100, 2);
    s.pass();
    const QString closed = folder + QStringLiteral("/a.wav");
    QFile::setPermissions(closed, QFileDevice::WriteOwner);
    QFile probe(closed);
    if (probe.open(QIODevice::ReadOnly)) {
        probe.close();
        QFile::setPermissions(closed, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        DSD_FPRINTF(stdout, "SKIP unopenable: this process reads any file\n");
        return;
    }
    QSignalSpy finished(&host, &dsd_qt::DecoderHost::recordingTransferFinished);
    const QString dest = roots.dir.path() + QStringLiteral("/saved.wav");
    expect("unopenable: the save begins",
           host.saveRecording(s.store.selectRecordings({idA}), QUrl::fromLocalFile(dest).toString()));
    s.pass();
    s.pass();
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    const bool journalKept = read_all(journal_of(folder)).contains("a.wav");
    QFile::setPermissions(closed, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("unopenable: the save told as changed",
           !finished.isEmpty() && finished.last().at(1).toInt() == dsd_qt::DecoderHost::TransferChanged);
    expect("unopenable: Export all takes the other, the line kept",
           all.value(QStringLiteral("count")).toInt() == 1 && journalKept);
}

/* Issue #902: storage back with another owner's folder.json where A's folder was and a journal that cannot be read:
 * A's recordings are kept, but no path of them is offered (share, play) until the folder is read. */
void
test_unread_folder_offers_no_paths(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    const QString x = roots.external + QStringLiteral("/Metro (1a2b3c4d)");
    expect("unread paths: A's folder", write_owner_json(x, QString(kUidA), QStringLiteral("Metro")));
    record(x, QStringLiteral("0000000000000963-1"), QStringLiteral("r.wav"), 1754000000, 1);
    Store s(roots.list());
    expect("unread paths: indexed", s.store.isIndexedPath(x + QStringLiteral("/r.wav")));
    expect("unread paths: the storage goes", QDir().rename(ext, away));
    s.pass();
    const QString awayX = away + QStringLiteral("/Call recordings/Metro (1a2b3c4d)");
    expect("unread paths: B's folder.json there", write_owner_json(awayX, QString(kUidB), QStringLiteral("Metro")));
    if (!take_read_access(awayX + QStringLiteral("/recordings.jsonl"))) {
        DSD_FPRINTF(stdout, "SKIP unread paths: this process reads any file\n");
        return;
    }
    expect("unread paths: the storage comes back", QDir().rename(away, ext));
    s.pass();
    s.pass();
    const bool offered = s.store.isIndexedPath(x + QStringLiteral("/r.wav"));
    QFile::setPermissions(journal_of(x), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("unread paths: the path is not offered while the folder is unread", !offered);
}

/* Issue #902: a journal due for compaction is never rewritten while its storage awaits a rescan: a session start saw
 * it missing, and it came back with a journal of the same length naming b.wav where a.wav was. The rescan reads
 * b.wav's record; it is never overwritten from the old index. */
void
test_compaction_waits_for_a_rescan(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(folder, QStringLiteral("0000000000000964-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    std::atomic<bool> swapped{false};
    s.store.setTailHookForTest([&](const QString& path) {
        if (path == folder && !swapped.exchange(true) && QDir().rename(ext, away)) {
            (void)s.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Other"));
            s.store.endSession();
            const QString awayJournal = away + QStringLiteral("/Call recordings/") + QFileInfo(folder).fileName()
                                        + QStringLiteral("/recordings.jsonl");
            QByteArray journal = read_all(awayJournal);
            journal.replace("a.wav", "b.wav");
            (void)write_private(awayJournal, journal, "wb");
            make_file(away + QStringLiteral("/Call recordings/") + QFileInfo(folder).fileName()
                          + QStringLiteral("/b.wav"),
                      1000);
            (void)QDir().rename(away, ext);
        }
    });
    // Two lines naming files that are not there: three lines for one recording, due for compaction in the pass that
    // tails them, the one the swap runs in.
    append(journal_of(folder),
           publish_line(QStringLiteral("0000000000000964-2"), QStringLiteral("g2.wav"), 1754000001, 2)
               + publish_line(QStringLiteral("0000000000000964-3"), QStringLiteral("g3.wav"), 1754000002, 3));
    s.pass();
    s.store.setTailHookForTest({});
    s.pass();
    s.pass();
    expect("compaction rescan: the swap ran in the pass", swapped.load());
    expect("compaction rescan: b.wav's record survives",
           read_all(journal_of(folder)).contains("b.wav") && s.store.isIndexedPath(folder + QStringLiteral("/b.wav")));
}

/* Issue #902: a recording a transfer could not open (write-only) stays out of the index through a read of its folder
 * again (storage unplugged and back): its metadata reads, but it still does not open, so a delete never unlinks it. */
void
test_unopenable_mark_survives_a_reread(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    ServedHost host;
    dsd_qt::recording_store_serve_host(&s.store, &host);
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString id = QStringLiteral("0000000000000965-1");
    record(folder, id, QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    const QString file = folder + QStringLiteral("/a.wav");
    QFile::setPermissions(file, QFileDevice::WriteOwner);
    QFile probe(file);
    if (probe.open(QIODevice::ReadOnly)) {
        probe.close();
        QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        DSD_FPRINTF(stdout, "SKIP unopenable reread: this process reads any file\n");
        return;
    }
    const QString dest = roots.dir.path() + QStringLiteral("/saved.wav");
    expect("unopenable reread: the save begins",
           host.saveRecording(s.store.selectRecordings({id}), QUrl::fromLocalFile(dest).toString()));
    s.pass();
    s.pass();
    expect("unopenable reread: the storage goes", QDir().rename(ext, away));
    s.pass();
    expect("unopenable reread: the storage comes back", QDir().rename(away, ext));
    s.pass();
    s.pass();
    (void)s.store.deleteFolder(QString(kUidA));
    s.pass();
    const bool kept = QFileInfo::exists(file);
    QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    expect("unopenable reread: a delete after the read again leaves it", kept);
}

/* Issue #902: the limit's check right before an unlink finding a link at the name leaves it and tells the removal as
 * not done (deletesSkipped()), and takes the recording out of the usage: with a 1 MB limit over two 600 kB recordings,
 * the older one's name a link by then, the other is under the limit and stays. */
void
test_limit_skip_of_a_link_is_told(void) {
#ifdef _WIN32
    DSD_FPRINTF(stdout, "SKIP limit link: symbolic links need a privilege on Windows\n");
#else
    Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("limit link: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    record(folder, QStringLiteral("0000000000000934-1"), QStringLiteral("old.wav"), 1754000000, 1, 600000);
    record(folder, QStringLiteral("0000000000000934-2"), QStringLiteral("new.wav"), 1754000100, 2, 600000);
    make_file(folder + QStringLiteral("/elsewhere.bin"), 1000);
    Store s(roots.list(), false);
    std::atomic<bool> linked{false};
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/old.wav")) && !linked.exchange(true)) {
            QFile::remove(path);
            (void)QFile::link(QStringLiteral("elsewhere.bin"), path);
        }
        return false;
    });
    s.limitQuietly(1);
    expect("limit link: the link stays, told as not done",
           linked.load() && QFileInfo(folder + QStringLiteral("/old.wav")).isSymLink()
               && s.store.deletesSkipped() == 1);
    expect("limit link: out of the usage, the other stays", QFileInfo::exists(folder + QStringLiteral("/new.wav"))
                                                                && s.store.removedByLimit() == 0
                                                                && s.store.usedBytes() == 600000);
#endif
}

/* Issue #902: a save selected while the store is not settled is refused as loading, as an export is; a selection
 * made settled carries a token that goes stale as a read of storage again starts, and once the store is gone. */
void
test_selection_token(void) {
    Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    auto s = std::make_unique<Store>(roots.list());
    const QString folder = s->store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s->store.endSession();
    const QString id = QStringLiteral("0000000000000935-1");
    record(folder, id, QStringLiteral("r.wav"), 1754000000, 1);
    s->pass();
    const QVariantMap selected = s->store.selectRecordings({id});
    const int selection = selected.value(QStringLiteral("selection")).toInt();
    RecordingSelectionToken token;
    expect("token: a settled selection is current, with a token",
           selection > 0 && s->store.selectionToken(selection, &token) && token.current());
    std::atomic<int> during{-1};
    s->store.setRootReadHookForTest([&](const QString&) {
        const QVariantMap save = s->store.selectRecordings({id});
        during = save.value(QStringLiteral("selection")).toInt() == 0 && save.value(QStringLiteral("loading")).toBool()
                     ? (token.current() ? 1 : 0)
                     : 2;
    });
    expect("token: the storage goes", QDir().rename(ext, away));
    s->pass();
    expect("token: the storage comes back", QDir().rename(away, ext));
    s->pass();
    s->store.setRootReadHookForTest({});
    expect("token: a save while storage is read is refused as loading, the token already stale", during.load() == 0);
    expect("token: stale after the read", !token.current() && !s->store.selectionCurrent(selection));
    s->store.releaseSelection(selection);
    const int again = s->store.selectRecordings({id}).value(QStringLiteral("selection")).toInt();
    RecordingSelectionToken fresh;
    expect("token: a selection after the read is current", s->store.selectionToken(again, &fresh) && fresh.current());
    s->store.releaseSelection(again);
    s.reset();
    expect("token: stale once the store is gone", !fresh.current());
    expect("token: a default token stands", RecordingSelectionToken().current());
}

/* Issue #902: the base host's save opens the recording once, never through a link, and checks what it opened: a link,
 * a FIFO (never waited on) and a missing name each leave nothing open. */
void
test_open_recording_input(void) {
    QTemporaryDir dir;
    const QString file = dir.path() + QStringLiteral("/r.wav");
    make_file(file, 1000);
    dsd_file_identity_t known{};
    expect("open: a file read", dsd_path_file_identity(dsd_qt::platform_path(file).constData(), &known) == 0);
    using Input = ServedHost::RecordingInput;
    FILE* fp = nullptr;
    expect("open: the file selected", ServedHost::openRecordingInput(file, known, &fp) == Input::Selected && fp);
    if (fp != nullptr) {
        (void)fclose(fp);
    }
#ifndef _WIN32
    // A link to the very file selected (its identity the one known): only a no-follow open tells it apart.
    const QString link = dir.path() + QStringLiteral("/l.wav");
    expect("open: a link to the file selected",
           QFile::link(file, link) && ServedHost::openRecordingInput(link, known, &fp) == Input::Changed && !fp);
#endif
    expect("open: gone",
           ServedHost::openRecordingInput(dir.path() + QStringLiteral("/none.wav"), known, &fp) == Input::Missing
               && fp == nullptr);
    make_file(file, 1001);
    expect("open: another size", ServedHost::openRecordingInput(file, known, &fp) == Input::Changed && fp == nullptr);
#ifndef _WIN32
    // Under a dangling link the name was never looked up: not gone, changed.
    expect("open: a dangling parent is no missing name",
           QFile::link(dir.path() + QStringLiteral("/nowhere"), dir.path() + QStringLiteral("/sub"))
               && ServedHost::openRecordingInput(dir.path() + QStringLiteral("/sub/r.wav"), known, &fp)
                      == Input::Changed
               && !fp);
    const QString fifo = dir.path() + QStringLiteral("/f.wav");
    expect("open: a FIFO, not waited on", mkfifo(dsd_qt::platform_path(fifo).constData(), 0600) == 0
                                              && ServedHost::openRecordingInput(fifo, known, &fp) == Input::Changed
                                              && !fp);
#endif
}

/* Issue #902 B12: a session start adopts a managed folder restored onto a mounted root (one the folder table did not
 * have) after the worker collected the folders to read in that pass. The folder waits for its read in the next pass;
 * between the two passes an export would have left it out and a delete of its owner would have queued nothing, so both
 * are refused. Once it is read, each takes its recording. */
void
test_requests_wait_for_an_adopted_folder(void) {
    const QString restoredId = QStringLiteral("0000000000000501-1");
    Roots roots;
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    s.pass();
    const QString c = roots.external + QStringLiteral("/Restored (5e6f7a8b)");
    QDir().mkpath(c);
    {
        const QJsonObject meta{{QStringLiteral("uid"), QString(kUidC)},
                               {QStringLiteral("kind"), QStringLiteral("saved")},
                               {QStringLiteral("name"), QStringLiteral("Restored")}};
        expect("adopted: a restored folder", write_private(c + QStringLiteral("/folder.json"),
                                                           QJsonDocument(meta).toJson(QJsonDocument::Compact), "wb"));
    }
    record(c, restoredId, QStringLiteral("r.wav"), 1754000000, 1);
    // New lines in A's journal: the pass tails it, after the folders to read were collected.
    record(a, QStringLiteral("0000000000000501-a"), QStringLiteral("a.wav"), 1754000100, 2);
    QString adopted;
    int ends = 0;
    int accepted = 0;
    s.store.setTailHookForTest([&](const QString& path) {
        if (path == a && adopted.isEmpty()) {
            adopted = s.store.beginSession(QStringLiteral("saved"), kUidC, QStringLiteral("Restored"));
        }
    });
    s.store.setPassEndHookForTest([&]() {
        if (!adopted.isEmpty() && ends++ == 0) {
            accepted += bulk_requests_refused(s.store, QString(kUidC)) ? 0 : 1;
        }
    });
    s.pass();
    s.store.setTailHookForTest({});
    s.store.endSession();
    s.pass();
    s.store.setPassEndHookForTest({});
    expect("adopted: the restored folder taken", adopted == c);
    expect("adopted: refused between the passes", ends >= 1 && accepted == 0);
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    expect("adopted: the export has its recording",
           all.value(QStringLiteral("count")).toInt() == 2 && !all.value(QStringLiteral("loading")).toBool());
    const bool asked = s.store.deleteFolder(QString(kUidC));
    expect("adopted: its owner's delete queues it", asked && s.taken() == QStringList{restoredId});
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
}

/* Issue #902 B12: a recheck reads storage one recording at a time and takes the gone ones out of the index. While one
 * runs (asked for, or the limit's look at what it counts) an export would have listed files no longer there, so an
 * export and every delete request are refused until the pass ends. */
void
test_requests_wait_for_a_recheck(void) {
    constexpr int kCount = 20;
    Roots roots;
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    QStringList ids;
    for (int i = 0; i < kCount; i++) {
        ids.append(QStringLiteral("0000000000000502-%1").arg(i));
        record(a, ids.last(), QStringLiteral("g%1.wav").arg(i), 1754000000 + i, 1 + i, 200000);
    }
    s.pass();
    int checks = 0;
    int accepted = 0;
    s.store.setEntryCheckHookForTest([&](const QString&) {
        checks++;
        accepted += bulk_requests_refused(s.store, QString(kUidA)) ? 0 : 1;
    });
    for (int i = 0; i < kCount / 2; i++) {
        QFile::remove(a + QStringLiteral("/g%1.wav").arg(i));
    }
    s.store.recheckRecordings(ids);
    s.pass();
    expect("recheck: asked, every recording looked at", checks == kCount);
    expect("recheck: asked, the gone ones out", s.store.recordingCount() == kCount / 2);
    expect("recheck: asked, refused meanwhile", accepted == 0);
    // Gone behind the store's back, then a limit under what the index counts (2 MB) but over what is on disk (400 kB):
    // the limit looks at every file first, and removes nothing.
    checks = 0;
    for (int i = kCount / 2; i < kCount - 2; i++) {
        QFile::remove(a + QStringLiteral("/g%1.wav").arg(i));
    }
    s.store.setLimitMb(1);
    s.pass();
    s.store.setEntryCheckHookForTest({});
    expect("recheck: the limit's look, every recording looked at", checks >= kCount / 2);
    expect("recheck: the limit's look, the gone ones out, nothing removed",
           s.store.recordingCount() == 2 && s.store.removedByLimit() == 0);
    expect("recheck: the limit's look, refused meanwhile", accepted == 0);
    const QVariantMap all = s.store.selectForExport(QString(), QStringLiteral("all"));
    s.store.releaseSelection(all.value(QStringLiteral("selection")).toInt());
    expect("recheck: then the export, whole", all.value(QStringLiteral("count")).toInt() == 2);
}

/* --- History CSV export: every row's recording names in one read. --- */

/* The names a selection of @p ids gives a zip, its hold released at once. */
QStringList
zip_names_of(RecordingStore& store, const QStringList& ids) {
    const QVariantMap selection = store.selectRecordings(ids);
    store.releaseSelection(selection.value(QStringLiteral("selection")).toInt());
    return selection.value(QStringLiteral("names")).toStringList();
}

/* A provider that implements only what History's rows read: the batched names keep their default. */
class RolesOnlyProvider : public dsd_qt::CallRecordingProvider {
  public:
    QVariantList
    recordingsForFragments(const QStringList&) const override {
        return {};
    }

    bool
    hasRecordingForFragments(const QStringList&) const override {
        return false;
    }
};

/* Each row's names are the entry names Export as zip gives the same recordings (`<folder name>/<file>`, a nested file
 * keeping its sub-folder), in the order recordingsForFragments() lists them (start, then publish order); a recording
 * reached twice is named once, and a row with no recordings, or no fragments, has none. */
void
test_entry_names_match_zip_selection(void) {
    expect("names: the helper", dsd_qt::recording_entry_name(QStringLiteral("/r/Call recordings/County (1a2b3c4d)"),
                                                             QStringLiteral("sub/a.wav"))
                                    == QStringLiteral("County (1a2b3c4d)/sub/a.wav"));
    const RolesOnlyProvider rolesOnly;
    expect("names: a provider without them gives one empty list per row",
           rolesOnly.recordingEntryNames({{fragment(1)}, {}, {fragment(2)}}) == QList<QStringList>(3)
               && rolesOnly.recordingEntryNames({}).isEmpty());

    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    expect("names: a sub-folder", QDir().mkpath(folder + QStringLiteral("/sub")));
    record(folder, QStringLiteral("0000000000000096-1"), QStringLiteral("b.wav"), 1754725200, 1);
    record(folder, QStringLiteral("0000000000000096-2"), QStringLiteral("sub/a.wav"), 1754725100, 1);
    record(folder, QStringLiteral("0000000000000096-3"), QStringLiteral("d.wav"), 1754725300, 2);
    record(folder, QStringLiteral("0000000000000096-4"), QStringLiteral("c.wav"), 1754725300, 2);
    s.pass();
    const QString base = QStringLiteral("County (1a2b3c4d)/");
    const QStringList merged{fragment(2), fragment(1), fragment(2)};
    const QList<QStringList> rows{merged, {fragment(1)}, {fragment(3)}, {}};
    const QList<QStringList> names = s.store.recordingEntryNames(rows);
    expect("names: one list per row", names.size() == rows.size());
    expect("names: start, then publish order; nested kept; each once",
           names.value(0)
               == QStringList{base + QStringLiteral("sub/a.wav"), base + QStringLiteral("b.wav"),
                              base + QStringLiteral("d.wav"), base + QStringLiteral("c.wav")});
    expect("names: a row with none, and one without fragments", names.value(2).isEmpty() && names.value(3).isEmpty());
    for (int i = 0; i < rows.size(); i++) {
        const QVariantList maps = s.store.recordingsForFragments(rows.value(i));
        expect("names: the zip's names for the row's recordings, in its list's order",
               names.value(i) == zip_names_of(s.store, ids_of(maps)) && names.value(i).size() == maps.size());
    }
    const QVariantMap every = s.store.selectForExport(kUidA, QStringLiteral("all"));
    s.store.releaseSelection(every.value(QStringLiteral("selection")).toInt());
    expect("names: the owner's Export as zip names them the same",
           every.value(QStringLiteral("names")).toStringList() == names.value(0));
    expect("names: no rows, no lists", s.store.recordingEntryNames({}).isEmpty());
}

/* An export reads up to 1,000 rows, each with many fragments, in one call: every row still gets exactly its own
 * recordings' names. */
void
test_entry_names_batch(void) {
    constexpr int kRows = 1000;
    constexpr int kFragments = 6;
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    s.store.endSession();
    QByteArray journal;
    QList<QStringList> rows;
    QList<QStringList> want;
    for (int row = 0; row < kRows; row++) {
        QStringList fragments;
        QStringList names;
        for (int part = 0; part < kFragments; part++) {
            const qint64 stamp = qint64{row} * kFragments + part + 1;
            fragments.append(fragment(stamp));
            if (part % 2 == 0) {
                const QString file = QStringLiteral("r%1-%2.wav").arg(row).arg(part);
                make_file(folder + QLatin1Char('/') + file, 10);
                journal.append(
                    publish_line(QStringLiteral("0000000000000097-%1").arg(stamp), file, 1754726000 + stamp, stamp));
                names.append(QStringLiteral("Explore/") + file);
            }
        }
        rows.append(fragments);
        want.append(names);
    }
    append(journal_of(folder), journal);
    s.pass();
    expect("one read: every recording indexed", s.store.recordingCount() == kRows * kFragments / 2);
    expect("one read: each row its own names, in order", s.store.recordingEntryNames(rows) == want);
}

/* --- History playlist (issue #909): every row's recordings in one read, and the delete of the file pressed. --- */

/* The value under @p key of each of @p maps, as text. */
QStringList
values_of(const QVariantList& maps, const QString& key) {
    QStringList values;
    for (const QVariant& map : maps) {
        values.append(map.toMap().value(key).toString());
    }
    return values;
}

/* Each row's list is its own recordings, oldest first by start then publish order within the row (two rows whose starts
 * interleave are never sorted together), each once, the same as recordingsForFragments() gives the row, all under one
 * hold of the lock; every map carries the file's indexed identity, its duration, whether it plays now and whether a
 * delete waits for it. */
void
test_playlist_recordings_per_row(void) {
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const qint64 base = 1754800000;
    // Row 1's parts start at +0 and +30 (a later publish at +30 too), row 2's at +10 and +40.
    record_at(folder, QStringLiteral("0000000000000909-1"), QStringLiteral("a1.wav"), base, 1, 1100, base + 4);
    record_at(folder, QStringLiteral("0000000000000909-2"), QStringLiteral("b1.wav"), base + 10, 2, 1200, base + 14);
    record_at(folder, QStringLiteral("0000000000000909-3"), QStringLiteral("a2.wav"), base + 30, 1, 1300, base + 34);
    record_at(folder, QStringLiteral("0000000000000909-4"), QStringLiteral("a3.wav"), base + 30, 1, 1400, base + 35);
    record_at(folder, QStringLiteral("0000000000000909-5"), QStringLiteral("b2.wav"), base + 40, 2, 1500, base + 44);
    s.pass();
    const QStringList rowOne{fragment(1)};
    const QStringList rowTwo{fragment(2)};
    const QStringList merged{fragment(2), fragment(1), fragment(2)};
    const QList<QStringList> asked{rowOne, rowTwo, {}, merged, {fragment(77)}};

    const int locks = s.store.readerLocksForTest();
    const QList<QVariantList> rows = s.store.recordingsForFragmentLists(asked);
    expect("playlist read: one hold of the lock", s.store.readerLocksForTest() == locks + 1);
    expect("playlist read: one list per row", rows.size() == asked.size());
    if (rows.size() != asked.size()) {
        return;
    }
    expect("playlist read: a row's parts in its own order",
           values_of(rows.at(0), QStringLiteral("path"))
               == QStringList{folder + QStringLiteral("/a1.wav"), folder + QStringLiteral("/a2.wav"),
                              folder + QStringLiteral("/a3.wav")});
    expect("playlist read: the other row's in its own",
           values_of(rows.at(1), QStringLiteral("path"))
               == QStringList{folder + QStringLiteral("/b1.wav"), folder + QStringLiteral("/b2.wav")});
    expect("playlist read: a row without fragments or recordings lists none",
           rows.at(2).isEmpty() && rows.at(4).isEmpty());
    expect("playlist read: a merged row's parts in start order, each once",
           ids_of(rows.at(3))
               == QStringList{QStringLiteral("0000000000000909-1"), QStringLiteral("0000000000000909-2"),
                              QStringLiteral("0000000000000909-3"), QStringLiteral("0000000000000909-4"),
                              QStringLiteral("0000000000000909-5")});
    for (int i = 0; i < asked.size(); i++) {
        expect("playlist read: the same recordings as the row's own read",
               ids_of(rows.at(i)) == ids_of(s.store.recordingsForFragments(asked.at(i))));
    }
    const QVariantMap a3 = rows.at(0).value(2).toMap();
    expect("playlist read: the map's identity is the file's",
           a3.value(QStringLiteral("identitySize")).toLongLong() == 1400
               && a3.value(QStringLiteral("mtimeSec")).toLongLong() == base + 35
               && a3.value(QStringLiteral("mtimeNanos")).toInt() == 0
               && a3.value(QStringLiteral("durationMs")).toLongLong() == 4000);
    expect("playlist read: plays now, no delete waits", a3.value(QStringLiteral("available")).toBool()
                                                            && a3.contains(QStringLiteral("deleteQueued"))
                                                            && !a3.value(QStringLiteral("deleteQueued")).toBool());

    // A delete waiting on an export's hold: that recording, and only it, says so.
    const int held =
        s.store.selectRecordings({QStringLiteral("0000000000000909-3")}).value(QStringLiteral("selection")).toInt();
    expect("playlist read: a delete held by an export", s.store.deleteRecording(QStringLiteral("0000000000000909-3")));
    s.pass();
    expect("playlist read: it waits", s.store.deletesWaiting() == 1);
    const QVariantList waiting = s.store.recordingsForFragmentLists({rowOne}).value(0);
    expect("playlist read: the waiting delete is told",
           values_of(waiting, QStringLiteral("deleteQueued"))
               == QStringList{QStringLiteral("false"), QStringLiteral("true"), QStringLiteral("false")});
    s.store.releaseSelection(held);
    s.pass();
    s.pass();
    expect("playlist read: done once released",
           ids_of(s.store.recordingsForFragmentLists({rowOne}).value(0))
               == QStringList{QStringLiteral("0000000000000909-1"), QStringLiteral("0000000000000909-4")});

    // Storage away: still listed, none plays.
    expect("playlist read: the storage goes", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
    s.pass();
    const QVariantList gone = s.store.recordingsForFragmentLists({rowTwo}).value(0);
    expect("playlist read: storage away keeps them listed, not playable",
           gone.size() == 2
               && values_of(gone, QStringLiteral("available"))
                      == QStringList{QStringLiteral("false"), QStringLiteral("false")});
    const RolesOnlyProvider rolesOnly;
    expect("playlist read: a provider without them gives one empty list per row",
           rolesOnly.recordingsForFragmentLists({rowOne, {}, rowTwo}) == QList<QVariantList>(3)
               && rolesOnly.recordingsForFragmentLists({}).isEmpty());
}

/* A map's identity is the file as the read that indexed it saw it, not the size the limit accounts: the limit's check
 * found the older file shrunk (600 kB indexed, 100 kB found) and counts it at 100 kB, while the map's identity still
 * names the 600 kB file, which a playlist then finds changed at its turn. */
void
test_recording_map_identity_is_the_indexed_one(void) {
    Roots roots;
    const QString folder = roots.external + QStringLiteral("/County (1a2b3c4d)");
    expect("map identity: the folder", write_owner_json(folder, QString(kUidA), QStringLiteral("County")));
    record_at(folder, QStringLiteral("000000000000090A-1"), QStringLiteral("old.wav"), 1754000000, 1, 600000,
              1754000004);
    record(folder, QStringLiteral("000000000000090A-2"), QStringLiteral("new.wav"), 1754000100, 2, 600000);
    Store s(roots.list(), false);
    std::atomic<bool> shrunk{false};
    s.store.setUploadHoldsForTest([&](const QString& path) {
        if (path.endsWith(QLatin1String("/old.wav")) && !shrunk.exchange(true)) {
            make_file(path, 100000);
        }
        return false;
    });
    s.limitQuietly(1);
    expect("map identity: shrunk right before its unlink, skipped", shrunk.load() && s.store.deletesSkipped() == 1);
    const QVariantMap map = s.store.recordingsForFragments({fragment(1)}).value(0).toMap();
    expect("map identity: the accounting moved", map.value(QStringLiteral("bytes")).toLongLong() == 100000);
    expect("map identity: the identity did not", map.value(QStringLiteral("identitySize")).toLongLong() == 600000
                                                     && map.value(QStringLiteral("mtimeSec")).toLongLong() == 1754000004
                                                     && map.value(QStringLiteral("mtimeNanos")).toInt() == 0);
    const QVariantMap batched = s.store.recordingsForFragmentLists({{fragment(1)}}).value(0).value(0).toMap();
    expect("map identity: the playlist read gives the same",
           batched.value(QStringLiteral("identitySize")).toLongLong() == 600000
               && batched.value(QStringLiteral("bytes")).toLongLong() == 100000);
}

/* B11: a delete carries the file the user pressed it on. A read again that put another file under the id while the
 * confirmation was open makes it refused as "changed", queuing and deleting nothing; an id no longer indexed is refused
 * as "gone", where the plain delete answers as asked; the file pressed is deleted. */
void
test_checked_delete_refuses_another_file(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString id = QStringLiteral("000000000000090B-1");
    const QString path = folder + QStringLiteral("/r.wav");
    record_at(folder, id, QStringLiteral("r.wav"), 1754000000, 1, 1000, 1754000004);
    s.pass();
    const QVariantMap pressed = s.store.recordingsForFragments({fragment(1)}).value(0).toMap();
    const qint64 size = pressed.value(QStringLiteral("identitySize")).toLongLong();
    const qint64 sec = pressed.value(QStringLiteral("mtimeSec")).toLongLong();
    const int nanos = pressed.value(QStringLiteral("mtimeNanos")).toInt();
    expect("checked delete: the identity pressed", size == 1000 && sec == 1754000004);

    // Another file under the id: rewritten, then read again with its folder.
    make_file(path, 1500);
    set_mtime(path, 1754000100);
    s.store.recheckRecordings({id});
    s.pass();
    s.pass();
    const QVariantMap now = s.store.recordingsForFragments({fragment(1)}).value(0).toMap();
    expect("checked delete: the read again indexed the other file",
           now.value(QStringLiteral("id")).toString() == id
               && now.value(QStringLiteral("identitySize")).toLongLong() == 1500);
    expect("checked delete: the file pressed is no longer there: refused",
           !s.store.deleteRecordingChecked(id, path, size, sec, nanos)
               && s.store.deleteRefusal() == QLatin1String("changed"));
    expect("checked delete: nothing queued", s.taken().isEmpty());
    s.pass();
    expect("checked delete: nothing deleted", QFileInfo::exists(path) && s.store.recordingCount() == 1);

    expect("checked delete: an id not indexed is gone",
           !s.store.deleteRecordingChecked(QStringLiteral("000000000000090B-9"), path, size, sec, nanos)
               && s.store.deleteRefusal() == QLatin1String("gone"));
    expect("checked delete: the plain delete of it answers as before",
           s.store.deleteRecording(QStringLiteral("000000000000090B-9")) && s.store.deleteRefusal().isEmpty());
    s.pass();

    expect("checked delete: the file as it is now is deleted",
           s.store.deleteRecordingChecked(id, path, now.value(QStringLiteral("identitySize")).toLongLong(),
                                          now.value(QStringLiteral("mtimeSec")).toLongLong(),
                                          now.value(QStringLiteral("mtimeNanos")).toInt())
               && s.store.deleteRefusal().isEmpty());
    s.pass();
    expect("checked delete: gone from disk and the index", !QFileInfo::exists(path) && s.store.recordingCount() == 0);
}

/* R68: a delete carries the path the user pressed it on. A read again that indexed the same id under another path,
 * with the same size and time (a copied journal and WAV), makes the delete refused as "changed", whether it comes
 * before the request is taken or between the request and its unlink; neither file is removed. */
void
test_checked_delete_refuses_another_path(void) {
    Roots roots;
    Store s(roots.list(), false);
    // Armed for the last case: the id moved between the delete's two looks, as a read again would (set before start).
    const QString id = QStringLiteral("000000000000090B-1");
    std::atomic<bool> armed{false};
    QString movedTo;
    s.store.setUploadHoldsForTest([&s, &armed, &id, &movedTo](const QString&) {
        if (armed.exchange(false)) {
            s.store.moveEntryPathForTest(id, movedTo);
        }
        return false;
    });
    s.start();
    const QString folder = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    const QString pressedPath = folder + QStringLiteral("/r.wav");
    const QString copyPath = folder + QStringLiteral("/s.wav");
    record_at(folder, id, QStringLiteral("r.wav"), 1754000000, 1, 1000, 1754000004);
    s.pass();
    const QVariantMap pressed = s.store.recordingsForFragments({fragment(1)}).value(0).toMap();
    expect("checked path: pressed at r.wav", pressed.value(QStringLiteral("path")).toString() == pressedPath);
    const qint64 size = pressed.value(QStringLiteral("identitySize")).toLongLong();
    const qint64 sec = pressed.value(QStringLiteral("mtimeSec")).toLongLong();
    const int nanos = pressed.value(QStringLiteral("mtimeNanos")).toInt();

    // Before the request: the copy indexed under the same id at another path, with the same identity.
    record_at(folder, id, QStringLiteral("s.wav"), 1754000000, 1, 1000, 1754000004);
    s.store.recheckRecordings({id});
    s.pass();
    s.pass();
    const QVariantMap now = s.store.recordingsForFragments({fragment(1)}).value(0).toMap();
    expect("checked path: the read again indexed the id at s.wav",
           now.value(QStringLiteral("path")).toString() == copyPath
               && now.value(QStringLiteral("identitySize")).toLongLong() == size
               && now.value(QStringLiteral("mtimeSec")).toLongLong() == sec);
    expect("checked path: another path under the id: refused as changed",
           !s.store.deleteRecordingChecked(id, pressedPath, size, sec, nanos)
               && s.store.deleteRefusal() == QLatin1String("changed"));
    s.pass();
    expect("checked path: neither file removed", QFileInfo::exists(pressedPath) && QFileInfo::exists(copyPath));

    // Between the request and its unlink: taken at s.wav and held there (the player holds the file), so the worker
    // leaves it queued; the id moves to r.wav while the worker is idle, then the hold ends and the pass takes it.
    const int skipped = s.store.deletesSkipped();
    s.store.setPlayingPath(copyPath);
    expect("checked path: the file as it is now is taken",
           s.store.deleteRecordingChecked(id, copyPath, size, sec, nanos));
    s.pass();
    expect("checked path: held while it plays", QFileInfo::exists(copyPath) && s.store.deletesSkipped() == skipped);
    s.store.moveEntryPathForTest(id, pressedPath);
    s.store.setPlayingPath(QString());
    s.pass();
    expect("checked path: the delete finds another path under the id: neither file removed",
           QFileInfo::exists(pressedPath) && QFileInfo::exists(copyPath) && s.store.deletesSkipped() == skipped + 1);

    // And right before the unlink, between its two looks under the lock (the uploader is asked in between). Armed
    // before the request, with the worker idle, so the pass the request wakes meets the hook.
    s.store.moveEntryPathForTest(id, copyPath);
    s.pass();
    movedTo = pressedPath;
    armed.store(true);
    expect("checked path: taken at s.wav again", s.store.deleteRecordingChecked(id, copyPath, size, sec, nanos));
    s.pass();
    expect("checked path: the unlink's look finds another path: neither file removed",
           !armed.load() && QFileInfo::exists(pressedPath) && QFileInfo::exists(copyPath)
               && s.store.deletesSkipped() == skipped + 2);
}

/* R74: a root seen away is not admitted. Its recordings read as unavailable to a playlist (and every reader) from the
 * first look that found it missing, before the worker's pass marks it away, and while the pass that reads it again
 * after it came back is still reading it; once that read is done they are available again. */
void
test_seen_away_root_is_not_admitted(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000934-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    const auto admitted = [&s] {
        const QVariantMap single = s.store.recordingsForFragments({fragment(1)}).value(0).toMap();
        const QVariantMap listed = s.store.recordingsForFragmentLists({{fragment(1)}}).value(0).value(0).toMap();
        return single.value(QStringLiteral("available")).toBool() && listed.value(QStringLiteral("available")).toBool();
    };
    expect("seen away: on storage that is there, the recording is admitted", admitted());
    std::atomic<bool> beforePass{true};
    std::atomic<bool> duringRead{true};
    std::atomic<bool> readHooked{false};
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (root.startsWith(ext + QLatin1Char('/')) && !readHooked.exchange(true)) {
            duringRead = admitted();
        }
    });
    // R78: through the folder's read again too, slice by slice, until its rows replace the ones held from before: those
    // stay listed, and unavailable, to a sheet and to a playlist's admission alike.
    std::atomic<int> slices{0};
    std::atomic<bool> listedHeld{true};
    std::atomic<bool> sheetAvailable{false};
    std::atomic<bool> admissionAvailable{false};
    s.store.setSliceHookForTest([&](const QString& path) {
        if (path != a) {
            return;
        }
        slices++;
        const QVariantList single = s.store.recordingsForFragments({fragment(1)});
        const QVariantList listed = s.store.recordingsForFragmentLists({{fragment(1)}}).value(0);
        listedHeld = listedHeld && single.size() == 1 && listed.size() == 1;
        // Each read on its own (M2): the sheet's and the admission's.
        const auto available = [](const QVariantList& rows) {
            return rows.value(0).toMap().value(QStringLiteral("available")).toBool();
        };
        sheetAvailable = sheetAvailable || available(single);
        admissionAvailable = admissionAvailable || available(listed);
    });
    const bool seen = storage_seen_away_and_back(s, ext, away, QString(kUidA), [&] { beforePass = admitted(); });
    s.store.setRootReadHookForTest({});
    s.store.setSliceHookForTest({});
    expect("seen away: the delete found the storage missing", seen);
    expect("seen away: between the read again's slices, the rows are listed", slices.load() >= 1 && listedHeld.load());
    expect("seen away: between the read again's slices, the sheet read sees them unavailable", !sheetAvailable.load());
    expect("seen away: between the read again's slices, the admission sees them unavailable",
           !admissionAvailable.load());
    expect("seen away: before the worker's pass marks it, the recording is not admitted", !beforePass.load());
    expect("seen away: back, while the pass reads it again, still not admitted",
           readHooked.load() && !duringRead.load());
    expect("seen away: read again, it is admitted again", admitted());
}

/* R79: a loss seen outside the pass while the root's listing in full is in flight (after its folders were listed,
 * before the table is installed; here by a session start, which then records elsewhere) is not dropped as already
 * seen: the storage there at the listing's end may be another card, so another listing in full runs after it. */
void
test_loss_during_a_listing_asks_for_another(void) {
    const Roots roots;
    const QString ext = roots.dir.path() + QStringLiteral("/ext");
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString a = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
    s.store.endSession();
    record(a, QStringLiteral("0000000000000937-1"), QStringLiteral("a.wav"), 1754000000, 1);
    s.pass();
    std::atomic<int> listings{0};
    std::atomic<bool> lossSeen{false};
    s.store.setRootReadHookForTest([&](const QString& root) {
        if (!root.startsWith(ext + QLatin1Char('/')) || listings++ != 0) {
            return;
        }
        // Listed: the storage goes, a session start sees it gone, and storage is back before the listing's last look.
        if (QDir().rename(ext, away)) {
            const QString elsewhere = s.store.beginSession(QStringLiteral("saved"), kUidB, QStringLiteral("Other"));
            s.store.endSession();
            lossSeen = elsewhere.startsWith(roots.internal + QLatin1Char('/'));
            (void)QDir().rename(away, ext);
        }
    });
    expect("listing loss: seen away and back", storage_seen_away_and_back(s, ext, away, QString(kUidA), {}));
    expect("listing loss: the session start saw the loss during the listing", lossSeen.load());
    s.pass(); // the listing asked for has run by now, at once or in this pass
    s.store.setRootReadHookForTest({});
    expect("listing loss: another listing in full ran after it", listings.load() == 2);
}

/* A host that plays: what History's playlist hands it is counted, the play running named by its generation. */
class PlayingHost : public dsd_qt::DecoderHost {
  public:
    int plays = 0;

    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return {};
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}

    bool
    playbackSupported() const override {
        return true;
    }

    int
    playRecordings(const QVariantList& items, int mode, const QString& origin) override {
        (void)items;
        (void)mode;
        (void)origin;
        return ++plays;
    }
};

/* R83 through a playlist: a recordings folder renamed away keeps its rows indexed and unavailable once the store has
 * noticed, so Play all is refused with the play running unchanged; renamed back and read again, Play all plays. A
 * session start while it is away makes the folder anew, and the read after it drops the old rows. Play all admits
 * from what the store knows (r12 Part B): pressed before the store has noticed, it admits as before (a decided limit;
 * the play then ends "Couldn't play these recordings"). */
void
test_playlist_missing_folder_keeps_its_rows_away(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    const Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    s.store.endSession();
    Ring ring;
    dsd_qt::CallHistoryModel model;
    model.setRecordingProvider(&s.store);
    QObject::connect(&s.store, &RecordingStore::recordingFragmentsChanged, &model,
                     &dsd_qt::CallHistoryModel::recordingFragmentsChanged);
    const time_t when = 1754703000;
    ring.commit(31001, 501, when, when + 4);
    model.refresh(ring.state);
    record(folder, QStringLiteral("00000000000000CE-1"), QStringLiteral("a.wav"), when, ring.rings[0].push_seq);
    s.pass();
    dsd_qt::CallHistoryFilterModel view;
    view.setSourceModel(&model);
    PlayingHost host;
    dsd_qt::CallHistoryPlaylist playlist(&view, &model, &host, nullptr);
    const auto result = [&playlist] { return playlist.start(0).value(QStringLiteral("result")).toString(); };
    expect("missing folder: on storage that is there, Play all plays",
           result() == QLatin1String("accepted") && host.plays == 1);

    // The decided limit: renamed, with no pass since, Play all admits from what the store knows.
    expect("missing folder: the recordings folder renamed", QDir().rename(roots.external, away));
    expect("missing folder: not noticed yet, Play all admits as before",
           result() == QLatin1String("accepted") && host.plays == 2);

    // R83: a root whose directory is missing (its parent there) is away, not empty. After a pass its recording is still
    // indexed, unavailable, and Play all is refused.
    const QString row = fragment(static_cast<qint64>(ring.rings[0].push_seq));
    const auto held = [&s, &row](bool available) {
        const QVariantList list = s.store.recordingsForFragments({row});
        return list.size() == 1 && list.value(0).toMap().value(QStringLiteral("available")).toBool() == available;
    };
    s.pass();
    expect("missing folder: renamed, a pass: still indexed, unavailable", held(false));
    expect("missing folder: renamed, a pass: Play all is refused",
           result() == QLatin1String("none") && host.plays == 2);
    // M3, a decided limit: a delete naming its rows is refused as storage away; their files are not there to delete.
    expect("missing folder: renamed, a pass: Delete all is refused as storage away",
           !s.store.deleteAll() && s.store.deleteRefusal() == QLatin1String("storageAway"));
    // Renamed back: read again, available, admitted.
    expect("missing folder: renamed back", QDir().rename(away, roots.external));
    s.pass();
    expect("missing folder: renamed back, a pass: available", held(true));
    expect("missing folder: renamed back: Play all plays", result() == QLatin1String("accepted") && host.plays == 3);

    // Away again, a session start keeps its own rule (a root that can be made is one it records into) and makes the
    // folder anew, empty: the root's read again finds what is really there, the old recording gone, the new one there.
    expect("missing folder: renamed away again", QDir().rename(roots.external, away));
    s.pass();
    expect("missing folder: away again: still indexed, unavailable", held(false));
    const QString made = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    expect("missing folder: away: a session start makes its folder there anew",
           made.startsWith(roots.external + QLatin1Char('/')) && QFileInfo(made).isDir());
    const QString fresh = fragment(4242);
    record(made, QStringLiteral("00000000000000CE-2"), QStringLiteral("b.wav"), when + 60, 4242);
    s.store.endSession();
    s.pass();
    const QVariantList now = s.store.recordingsForFragments({fresh});
    expect("missing folder: read again: the old recording is gone", s.store.recordingsForFragments({row}).isEmpty());
    expect("missing folder: read again: the new session's recording is indexed",
           now.size() == 1 && now.value(0).toMap().value(QStringLiteral("available")).toBool());
    model.setRecordingProvider(nullptr);
}

/* The revision moves on every change a detection list can see, carried through coalesced deliveries: membership that
 * changes while every summary figure stays the same, a later publication with nothing new before the GUI took the
 * first, and a change of availability alone. */
void
test_revision_coalesced_passes(void) {
    Roots roots;
    const QString away = roots.dir.path() + QStringLiteral("/ext-away");
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Exploring"));
    s.store.endSession();
    record_heard(folder, QStringLiteral("0000000000000095-1"), QStringLiteral("a.wav"), 1754724000, 1,
                 QStringLiteral("1:1"));
    s.pass();
    QSignalSpy changed(&s.store, &RecordingStore::changed);
    QSignalSpy revisions(&s.store, &RecordingStore::revisionChanged);
    double revision = revision_of(s.store);
    expect("revision: a number once something was indexed", revision > 0.0);

    // Two passes before the GUI takes either: one recording goes, another of the same size and start comes.
    s.store.deleteRecording(QStringLiteral("0000000000000095-1"));
    s.store.waitForPassForTest();
    record_heard(folder, QStringLiteral("0000000000000095-2"), QStringLiteral("b.wav"), 1754724000, 2,
                 QStringLiteral("1:2"));
    s.store.waitForPassForTest();
    QCoreApplication::processEvents();
    expect("revision: membership moved, the summary did not",
           changed.isEmpty() && s.store.recordingCountForDetection(QStringLiteral("1:1")) == 0
               && s.store.recordingCountForDetection(QStringLiteral("1:2")) == 1);
    expect("revision: moved, once", revision_of(s.store) > revision && revisions.count() == 1);
    revision = revision_of(s.store);

    // A publication with nothing new for the index (a delete held by the player) after one that changed it, both before
    // the GUI takes them.
    record_heard(folder, QStringLiteral("0000000000000095-3"), QStringLiteral("c.wav"), 1754724010, 3,
                 QStringLiteral("1:3"));
    s.store.waitForPassForTest();
    s.store.setPlayingPath(folder + QStringLiteral("/c.wav"));
    s.store.deleteRecording(QStringLiteral("0000000000000095-3"));
    s.store.waitForPassForTest();
    QCoreApplication::processEvents();
    expect("revision: the later quiet publication went out", s.store.deletesWaiting() == 1);
    expect("revision: not lost to it", revision_of(s.store) > revision && revisions.count() == 2);
    s.store.setPlayingPath(QString());
    s.pass();
    revision = revision_of(s.store);
    revisions.clear();

    // Availability alone: nothing listed changes but whether it plays.
    expect("revision: the storage goes", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
    s.pass();
    expect("revision: away moves it", revision_of(s.store) > revision && revisions.count() == 1);
    revision = revision_of(s.store);
    expect("revision: the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
    s.pass();
    expect("revision: back moves it", revision_of(s.store) > revision && revisions.count() == 2);
    revision = revision_of(s.store);
    s.pass();
    expect("revision: a pass that changed nothing leaves it",
           revision_of(s.store) <= revision && revisions.count() == 2);
}

/* The decoder's own writer names the detection: listed by it, every id exact past 2^53; a recording heard on no visit
 * is under none. */
void
test_detection_from_c_writer(void) {
    Roots roots;
    Store s(roots.list());
    const QString folder = s.store.beginSession(QStringLiteral("explore"), QString(), QStringLiteral("Search"));
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    expect("writer: the journal opens", dsd_wav_journal_open(qPrintable(journal_of(canonical))) == 0);
    const auto publish = [&canonical](const char* id, const char* name, qint64 start, uint64_t session, uint64_t hit) {
        make_file(canonical + QLatin1Char('/') + QLatin1String(name), 2000);
        const QByteArray path = (canonical + QLatin1Char('/') + QLatin1String(name)).toUtf8();
        const QByteArray dir = canonical.toUtf8();
        wav_journal_publish_record rec;
        DSD_MEMSET(&rec, 0, sizeof rec);
        rec.id = id;
        rec.path = path.constData();
        rec.dir = dir.constData();
        rec.start = start;
        rec.duration_ms = 3000;
        rec.rate = 8000;
        rec.owner.kind = DSD_WAV_OWNER_NONE;
        rec.owner.slot = DSD_WAV_OWNER_SLOT_UNKNOWN;
        rec.system = "S";
        rec.target = "T";
        rec.source = "R";
        rec.has_search = session != 0U || hit != 0U ? 1 : 0;
        rec.search_session = session;
        rec.search_hit = hit;
        wav_journal_publish(&rec);
    };
    const uint64_t big = std::numeric_limits<uint64_t>::max();
    publish("0000000000000096-1", "first.wav", 1754725000, 9007199254740993ULL, 3U);
    publish("0000000000000096-2", "second.wav", 1754725100, 9007199254740993ULL, 3U);
    publish("0000000000000096-3", "max.wav", 1754725200, big, big);
    publish("0000000000000096-4", "none.wav", 1754725300, 0U, 0U);
    s.pass();
    const QString key = QStringLiteral("9007199254740993:3");
    expect("writer: every recording indexed", s.store.recordingCount() == 4);
    expect("writer: listed by its detection, newest first",
           s.store.recordingCountForDetection(key) == 2
               && ids_of(s.store.recordingsForDetection(key))
                      == QStringList{QStringLiteral("0000000000000096-2"), QStringLiteral("0000000000000096-1")});
    expect("writer: the largest ids",
           s.store.recordingCountForDetection(QStringLiteral("18446744073709551615:18446744073709551615")) == 1);
    expect("writer: 2^53 + 1 is not read as 2^53",
           s.store.recordingCountForDetection(QStringLiteral("9007199254740992:3")) == 0);
    dsd_wav_journal_close();
    s.store.endSession();
}

/* Issue #828: the store hands a path to the native dsd_* APIs (the upload hold, free space) in the bytes they read it
   as: UTF-8 on Windows, where QFile::encodeName() gives the ANSI code page, which has no Hangul; QFile's own bytes
   elsewhere. So a file QFile made under a name outside ASCII and outside any one ANSI code page is the file the
   platform layer finds. */
void
test_platform_path_names_the_qt_file(void) {
    const QString name = QString::fromUtf8("Caf\xC3\xA9 \xEB\xB0\x95.wav");
#ifdef Q_OS_WIN
    expect("a Windows path goes to the platform layer in UTF-8", dsd_qt::platform_path(name) == name.toUtf8());
#else
    expect("a path goes to the platform layer in QFile's own bytes",
           dsd_qt::platform_path(name) == QFile::encodeName(name));
#endif
    const QTemporaryDir dir;
    expect("a scratch folder", dir.isValid());
    const QString path = dir.filePath(name);
    QFile file(path);
    expect("QFile makes the file", file.open(QIODevice::WriteOnly) && file.write("x", 1) == 1);
    file.close();
    dsd_stat_t st;
    expect("the platform layer finds the file QFile made",
           dsd_stat_path(dsd_qt::platform_path(path).constData(), &st) == 0 && st.st_size == 1);
    expect("free space reads for its folder", dsd_path_free_bytes(dsd_qt::platform_path(dir.path()).constData()) >= 0);
}

} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-recording-store-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    g_previousHandler = qInstallMessageHandler(count_warnings);

    test_tail_reads_complete_lines_only();
    test_journal_from_the_writer();
    test_recording_roles_from_the_store();
    test_compaction();
    test_journal_tail_settled();
    test_folder_marker_gone_after_read();
    test_compaction_waits_for_a_run();
    test_roots_unavailable_vs_deleted();
    test_changes_behind_the_store();
    test_folders_follow_their_owner();
    test_session_starts_while_the_worker_publishes();
    test_deletion_safety();
    test_retention();
    test_free_space_warning();
    test_free_space_where_a_session_goes();
    test_free_space_follows_the_session();
    test_free_space_after_the_session_ends();
    test_export_selection();
    test_export_presets_on_a_skipped_date();
    test_export_presets_on_a_long_day();
    test_export_dates_follow_the_given_zone();
    test_export_press_zone_is_local_time();
    test_session_running_at_ui_load();
    test_start_does_not_wait_for_journals();
    test_folder_gone_before_its_read();
    test_root_gone_mid_pass();
    test_root_gone_before_a_recheck();
    test_root_gone_before_a_delete();
    test_root_gone_as_it_is_read_again();
    test_root_gone_as_its_folders_are_read_again();
    test_root_gone_as_a_journal_is_read_again();
    test_root_gone_as_a_journal_is_tailed();
    test_root_gone_during_the_limit();
    test_root_gone_before_the_limit_removes();
    test_root_gone_during_an_unlink();
    test_root_gone_before_a_session_start();
    test_unwritable_folder_falls_back();
    test_session_start_wakes_the_worker();
    test_recheck_recordings();
    test_limit_counts_only_files_still_there();
    test_path_reused_by_a_later_publish();
    test_limit_count_keeps_storage_that_went();
    test_delete_failures_are_reported();
    test_stop_during_the_first_read();
    test_player_release_wakes_a_waiting_delete();
    test_held_recordings_over_the_limit_go_once_released();
    test_host_surface();
    test_single_play_hold_only();
    test_transfer_reads_only_the_files_selected();
    test_limit_follows_the_preference();
    test_search_key_parse_and_compaction();
    test_detection_index();
    test_detection_root_away_keeps_membership();
    test_detection_order_newest_first();
    test_queries_between_rebuild_slices();
    test_changed_storage_is_read_before_bulk_requests();
    test_journal_replaced_in_place_is_read_before_bulk_requests();
    test_bulk_requests_wait_for_a_root_being_listed();
    test_held_delete_dropped_by_a_reread();
    test_deletes_refused_for_storage_away();
    test_delete_checks_the_file_it_removes();
    test_delete_leaves_a_folder_adopted_meanwhile();
    test_delete_of_a_name_now_a_link_is_told();
    test_same_size_and_time_reads_as_the_recording();
    test_limit_skips_a_file_not_as_known();
    test_delete_of_an_unreadable_file_skips_it();
    test_unread_journal_is_not_settled();
    test_limit_waits_for_a_returning_folder();
    test_unread_journal_retry_publishes();
    test_tail_read_failure_unsettles();
    test_recheck_keeps_an_unreadable_name();
    test_changed_file_reads_its_folder_again();
    test_rescan_keeps_a_session_folder_made_meanwhile();
    test_limit_skip_of_a_link_is_told();
    test_rescan_drops_a_recreated_folders_old_recordings();
    test_rescan_keeps_a_relabel_made_meanwhile();
    test_export_probes_a_root_back();
    test_held_delete_waits_for_an_unread_tail();
    test_unreadable_recording_is_kept();
    test_unreadable_marker_keeps_the_folder();
    test_delete_rechecks_its_folder_at_the_unlink();
    test_limit_walk_stops_when_its_count_goes_stale();
    test_bulk_deletes_probe_a_root_back();
    test_adoption_during_a_read_is_read_again();
    test_owner_lookup_failure_unsettles();
    test_unreadable_linked_recording_is_in_no_index();
    test_limit_look_drops_an_unreadable_recording_from_usage();
#ifndef _WIN32
    test_socket_marker_is_no_folder();
#endif
    test_unreadable_entry_is_never_deleted();
    test_limit_unlink_check_drops_an_unreadable_recording();
#ifndef _WIN32
    test_dangling_parent_keeps_the_record();
#endif
    test_owner_lookup_failure_ends_a_read_in_flight();
    test_marker_retry_takes_the_label_on_disk();
    test_session_start_remembers_storage_away();
    test_folder_missing_at_a_cold_start();
    test_root_without_rows_is_not_held_away();
    test_directory_gone_during_its_listing_is_remembered_missing();
    test_directory_gone_during_a_folder_read_is_remembered_missing();
    test_session_seeing_a_listing_root_gone_queues_another();
    test_first_read_waits_for_its_last_slice();
    test_session_on_another_root_notes_a_missing_indexed_root();
    test_session_never_records_onto_a_root_back_unlisted();
    test_first_read_of_a_vanished_directory_is_not_committed();
    test_start_with_no_folder_asks_for_the_listing();
    test_owner_choice_drops_a_root_its_checks_found_gone();
    test_session_made_root_during_a_pass_is_not_listed();
    test_session_made_root_inside_a_look_writes_no_flag();
    test_export_note_during_a_pass_is_taken_by_its_listing();
    test_session_making_the_folder_reads_its_root_again();
    test_delete_held_when_its_root_was_seen_away();
    test_owner_lookup_failure_ends_a_tail_in_flight();
#ifndef _WIN32
    test_recheck_finding_a_link_tells_its_delete();
#endif
    test_unreadable_journal_retry_backs_off();
    test_root_seen_away_by_a_delete_ends_tokens();
    test_limit_counts_the_size_its_unlink_check_found();
    test_unopenable_recording_leaves_selections();
    test_unread_folder_offers_no_paths();
    test_compaction_waits_for_a_rescan();
    test_unopenable_mark_survives_a_reread();
    test_selection_token();
    test_open_recording_input();
    test_selection_goes_stale_as_a_read_starts();
    test_storage_a_request_saw_missing_is_read_again();
    test_requests_wait_for_an_adopted_folder();
    test_requests_wait_for_a_recheck();
    test_entry_names_match_zip_selection();
    test_entry_names_batch();
    test_playlist_recordings_per_row();
    test_recording_map_identity_is_the_indexed_one();
    test_checked_delete_refuses_another_file();
    test_checked_delete_refuses_another_path();
    test_seen_away_root_is_not_admitted();
    test_playlist_missing_folder_keeps_its_rows_away();
    test_loss_during_a_listing_asks_for_another();
    test_revision_coalesced_passes();
    test_detection_from_c_writer();
    test_platform_path_names_the_qt_file();

    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
