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

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
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
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic>
#include <chrono>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include "../test_support/qt_test_paths.h"
#include "call_history_model.h"
#include "call_recording_provider.h"
#include "decoder_host.h"
#include "recording_journal.h"
#include "recording_store.h"
#include "wav_journal.h"

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
        store.hostSessionReport(true, false);
        store.setTimingForTest(3600000, 3600000);
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

    void
    pass() {
        store.waitForPassForTest();
        QCoreApplication::processEvents();
    }
};

void
append(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append) || file.write(bytes) != bytes.size()) {
        DSD_FPRINTF(stderr, "FAIL: cannot append to %s\n", qPrintable(path));
        g_failures++;
    }
}

void
make_file(const QString& path, qint64 bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(QByteArray(bytes, 'w')) != bytes) {
        DSD_FPRINTF(stderr, "FAIL: cannot write %s\n", qPrintable(path));
        g_failures++;
    }
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
        s.store.hostSessionReport(true, true); // a session no starter here began: its folder is not known
        s.start();
        expect("a new store reads the rewrite", s.store.recordingCount() == 3
                                                    && s.store.hasRecordingForFragments({fragment(50)})
                                                    && s.store.hasRecordingForFragments({fragment(53)})
                                                    && s.store.hasRecordingForFragments({fragment(54)}));
        s.store.deleteRecording(QStringLiteral("00000000000000DD-6"));
        s.store.deleteRecording(QStringLiteral("00000000000000DD-1"));
        s.pass();
        expect("a session running when the store started holds every journal", s.store.compactionsForTest() == 0);
        s.store.endSession();
        s.pass();
        expect("a starter's endSession() does not free it", s.store.compactionsForTest() == 0);
        s.store.hostSessionReport(true, false);
        s.pass();
        expect("until it ends", s.store.compactionsForTest() == 1 && read_all(journal_of(folder)).count('\n') == 1);
    }
}

/* Storage that is away is not storage that was emptied: what an unavailable root held stays indexed and untouched,
 * a delete waits for it, and it is read again when it comes back. A file missing with its root there leaves the
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

        expect("the storage goes away", QDir().rename(roots.dir.path() + QStringLiteral("/ext"), away));
        s.pass();
        const QVariantMap a = s.store.recordingsForFragments({fragment(60)}).value(0).toMap();
        expect("what it held stays indexed, marked away",
               s.store.recordingCount() == 2 && !a.isEmpty() && !a.value("available").toBool());
        expect("the other root is still there", s.store.available());
        s.store.deleteRecording(QStringLiteral("00000000000000EE-1"));
        s.pass();
        expect("a delete waits for it", s.store.deletesWaiting() == 1 && s.store.recordingCount() == 2);
        expect("an export leaves it out",
               s.store.selectForExport(QString(), QStringLiteral("all")).value("count").toInt() == 0);
        const QString moved = s.store.beginSession(QStringLiteral("saved"), kUidA, QStringLiteral("County"));
        expect("a session meanwhile records in the other root",
               moved == roots.internal + QStringLiteral("/County (1a2b3c4d)"));
        s.store.endSession();

        expect("the storage comes back", QDir().rename(away, roots.dir.path() + QStringLiteral("/ext")));
        s.pass();
        expect("read again, and the waiting delete done",
               s.store.recordingCount() == 1 && !QFileInfo::exists(folder + QStringLiteral("/a.wav"))
                   && s.store.hasRecordingForFragments({fragment(61)}) && s.store.deletesWaiting() == 0);
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

    record(folder, QStringLiteral("0000000000000011-20"), QStringLiteral("small.wav"), 1754708000, 200, 40000);
    s.pass();
    expect("between 95% and the limit nothing goes", s.store.usedBytes() == 940000 && s.store.removedByLimit() == 3);
    record(folder, QStringLiteral("0000000000000011-21"), QStringLiteral("more.wav"), 1754708010, 201, 70000);
    s.pass();
    expect("past the limit again, after a publish, the next oldest goes",
           s.store.usedBytes() == 910000 && !QFileInfo::exists(folder + QStringLiteral("/c3.wav"))
               && QFileInfo::exists(folder + QStringLiteral("/c0.wav")) && s.store.removedByLimit() == 4);
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
    qint64 from = 0;
    qint64 to = 0;
    const QDate today(2026, 10, 28);
    const qint64 midnight = today.startOfDay().toSecsSinceEpoch();
    expect("today", dsd_qt::recording_export_range(QStringLiteral("today"), today, &from, &to) && from == midnight);
    expect("yesterday", dsd_qt::recording_export_range(QStringLiteral("yesterday"), today, &from, &to)
                            && from == today.addDays(-1).startOfDay().toSecsSinceEpoch() && to == midnight);
    expect("last 7 days", dsd_qt::recording_export_range(QStringLiteral("last7"), today, &from, &to)
                              && from == today.addDays(-6).startOfDay().toSecsSinceEpoch());
    expect("last 30 days", dsd_qt::recording_export_range(QStringLiteral("last30"), today, &from, &to)
                               && from == today.addDays(-29).startOfDay().toSecsSinceEpoch());
    expect("all", dsd_qt::recording_export_range(QStringLiteral("all"), today, &from, &to) && from < 0);
    expect("nothing else", !dsd_qt::recording_export_range(QStringLiteral("week"), today, &from, &to));

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
    QFile json(folder + QStringLiteral("/folder.json"));
    if (!json.open(QIODevice::WriteOnly)
        || json.write(QStringLiteral("{\"uid\":\"%1\",\"kind\":\"saved\",\"name\":\"County\"}\n").arg(kUidA).toUtf8())
               < 0) {
        g_failures++;
    }
    json.close();
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
    store.hostSessionReport(true, false);
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
        QFile json(second + QStringLiteral("/folder.json"));
        if (!json.open(QIODevice::WriteOnly)
            || json.write(
                   QStringLiteral("{\"uid\":\"%1\",\"kind\":\"saved\",\"name\":\"Metro\"}\n").arg(kUidC).toUtf8())
                   < 0) {
            g_failures++;
        }
        json.close();
        std::atomic<bool> release{false};
        std::mutex heldMutex;
        QString held;
        RecordingStore store;
        store.setRoots(roots.list());
        store.setTimingForTest(3600000, 3600000);
        store.hostSessionReport(true, false);
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
    expect("a file still there has its size read again", s.store.usedBytes() == 1150000);

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
}

/* Issue #708, M5: an exit during a long first read does not wait for the rest of it. */
void
test_stop_during_the_first_read(void) {
    Roots roots;
    QDir().mkpath(roots.external);
    for (int i = 0; i < 30; i++) {
        const QString folder = roots.external + QStringLiteral("/F%1").arg(i);
        QDir().mkpath(folder);
        QFile json(folder + QStringLiteral("/folder.json"));
        if (!json.open(QIODevice::WriteOnly) || json.write(QStringLiteral("{\"uid\":\"u%1\"}\n").arg(i).toUtf8()) < 0) {
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
    test_roots_unavailable_vs_deleted();
    test_changes_behind_the_store();
    test_folders_follow_their_owner();
    test_deletion_safety();
    test_retention();
    test_free_space_warning();
    test_export_selection();
    test_session_running_at_ui_load();
    test_start_does_not_wait_for_journals();
    test_folder_gone_before_its_read();
    test_recheck_recordings();
    test_delete_failures_are_reported();
    test_stop_during_the_first_read();
    test_player_release_wakes_a_waiting_delete();

    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
