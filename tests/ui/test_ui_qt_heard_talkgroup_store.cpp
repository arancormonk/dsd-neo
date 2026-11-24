// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the remembered talkgroups of each saved system (issue #903). An observation widens first/last and is
 * idempotent; each owner keeps 2000 entries, the oldest last heard evicted first (ties to the lower variant, then the
 * lower id), a late observation older than all of them included; elements of another v are written back verbatim; a
 * malformed or future file is never written, moved or purged, through observes, Stop flushes, due flushes and teardown;
 * a failed write stays owed and is retried; background writes come at most once, and at least once, per 30 s of real
 * time; forget and clear are written at once and no older write queued before them undoes them; purge removes only
 * owners missing from the live set; an owner the policy does not allow learns nothing; a burst queued behind a running
 * write is one more write, of the newest state; going off screen and quitting write what is owed. Registered only when the Qt
 * frontend is enabled (DSD_ENABLE_QT_UI). */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSemaphore>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QVector>
#include <Qt>
#include <algorithm>
#include <atomic>
#include <dsd-neo/core/safe_api.h>
#include <functional>
#include <memory>
#include <stdio.h>

#include "../test_support/qt_test_paths.h"
#include "heard_talkgroup_store.h"
#include "json_store.h"

namespace {

using dsd_qt::HeardTalkgroupStore;
using dsd_qt::json_store_path;

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

const QString kOwnerA = QStringLiteral("00000000-0000-4000-8000-00000000000a");
const QString kOwnerB = QStringLiteral("00000000-0000-4000-8000-00000000000b");
const QString kOwnerC = QStringLiteral("00000000-0000-4000-8000-00000000000c");

bool
allow_all(const QString&) {
    return true;
}

bool
write_raw(const QString& name, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(json_store_path(name)).absolutePath());
    QFile file(json_store_path(name));
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray
raw(const QString& name) {
    QFile file(json_store_path(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<unopenable>");
}

QJsonArray
stored(const QString& name) {
    return QJsonDocument::fromJson(raw(name)).array();
}

/* Whether the file @p name holds a v:1 entry for @p owner and @p id (any variant). */
bool
stored_has(const QString& name, const QString& owner, quint32 id) {
    const QJsonArray array = stored(name);
    return std::any_of(array.begin(), array.end(), [&](const QJsonValue& value) {
        const QJsonObject obj = value.toObject();
        return obj.value(QStringLiteral("v")).toInteger() == 1 && obj.value(QStringLiteral("owner")).toString() == owner
               && obj.value(QStringLiteral("id")).toInteger() == id;
    });
}

const HeardTalkgroupStore::Entry*
find(const QVector<HeardTalkgroupStore::Entry>& entries, int variant, quint32 id) {
    const auto it = std::find_if(entries.cbegin(), entries.cend(), [&](const HeardTalkgroupStore::Entry& entry) {
        return entry.variant == variant && entry.id == id;
    });
    return it == entries.cend() ? nullptr : &*it;
}

void
fresh(const QString& name) {
    QFile::remove(json_store_path(name));
}

/* Counts writes reaching the worker, and the real time each was queued at, through the test hook. */
struct WriteCounter {
    std::shared_ptr<std::atomic<int>> writes = std::make_shared<std::atomic<int>>(0);

    WriteCounter() {
        auto counter = writes;
        HeardTalkgroupStore::setBeforeWriteForTest([counter] { counter->fetch_add(1); });
    }

    ~WriteCounter() { HeardTalkgroupStore::setBeforeWriteForTest({}); }

    WriteCounter(const WriteCounter&) = delete;
    WriteCounter& operator=(const WriteCounter&) = delete;
};

void
test_upsert() {
    const QString name = QStringLiteral("heard_upsert.json");
    fresh(name);
    HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow_all);
    int signals_ = 0;
    QObject::connect(&store, &HeardTalkgroupStore::ownerChanged, [&](const QString& owner) {
        expect("upsert: the signal names the owner", owner == kOwnerA);
        signals_++;
    });
    expect("upsert: a missing file loads", store.loaded() && store.entriesFor(kOwnerA).isEmpty());
    store.observe(kOwnerA, 1, 100, 5000);
    store.observe(kOwnerA, 1, 100, 5000);
    expect("upsert: the same observation twice changes once", signals_ == 1);
    auto entries = store.entriesFor(kOwnerA);
    expect("upsert: one entry", entries.size() == 1 && entries.at(0).first == 5000 && entries.at(0).last == 5000);
    store.observe(kOwnerA, 1, 100, 4000); // older, delivered late
    entries = store.entriesFor(kOwnerA);
    expect("upsert: an older observation moves first only",
           entries.size() == 1 && entries.at(0).first == 4000 && entries.at(0).last == 5000);
    store.observe(kOwnerA, 1, 100, 4500);
    expect("upsert: one inside the span changes nothing", signals_ == 2);
    store.observe(kOwnerA, 1, 100, 6000);
    entries = store.entriesFor(kOwnerA);
    expect("upsert: a newer one moves last only",
           entries.size() == 1 && entries.at(0).first == 4000 && entries.at(0).last == 6000 && signals_ == 3);
    store.observe(kOwnerA, 2, 100, 6000);
    expect("upsert: another variant of the number is its own entry", store.entriesFor(kOwnerA).size() == 2);
    store.observe(kOwnerA, 0, 101, 6000);
    store.observe(kOwnerA, 256, 101, 6000);
    store.observe(kOwnerA, 1, 0, 6000);
    store.observe(kOwnerA, 1, 101, -1);
    store.observe(QString(), 1, 101, 6000);
    expect("upsert: out-of-range input is ignored", store.entriesFor(kOwnerA).size() == 2 && signals_ == 4);
    store.flushNow();
    store.waitForWrites();
    expect("upsert: written", !store.dirty() && stored(name).size() == 2);
    {
        HeardTalkgroupStore reloaded(name);
        const auto back = reloaded.entriesFor(kOwnerA);
        const auto* first = find(back, 1, 100);
        expect("upsert: round trip", back.size() == 2 && first != nullptr && first->first == 4000 && first->last == 6000
                                         && find(back, 2, 100) != nullptr);
    }
    fresh(name);
}

void
test_cap_and_eviction() {
    const QString name = QStringLiteral("heard_cap.json");
    fresh(name);
    HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow_all);
    const int cap = HeardTalkgroupStore::kMaxEntriesPerOwner;
    static_assert(HeardTalkgroupStore::kMaxEntriesPerOwner == 2000, "2000 remembered talkgroups per owner");
    /* Ids 1..cap, id n last heard at 10000 + n; ids 1 and 2 share the oldest time, under two variants each. */
    for (int n = 1; n <= cap; ++n) {
        store.observe(kOwnerA, 1, static_cast<quint32>(n), n <= 2 ? 10000 : 10000 + n);
    }
    store.observe(kOwnerB, 1, 1, 1); // another owner's entries do not count
    expect("cap: full", store.entriesFor(kOwnerA).size() == cap);
    int signals_ = 0;
    QObject::connect(&store, &HeardTalkgroupStore::ownerChanged, [&](const QString&) { signals_++; });

    store.observe(kOwnerA, 1, 5000, 20000);
    auto entries = store.entriesFor(kOwnerA);
    expect("cap: still 2000", entries.size() == cap);
    expect("cap: the tie goes to the lower id first", find(entries, 1, 1) == nullptr && find(entries, 1, 2) != nullptr);
    expect("cap: the new one is kept", find(entries, 1, 5000) != nullptr && signals_ == 1);
    expect("cap: newest first", !entries.isEmpty() && entries.at(0).id == 5000);

    /* id 2 under a second variant, at the same oldest time: the lower variant goes first. */
    store.observe(kOwnerA, 2, 2, 10000);
    entries = store.entriesFor(kOwnerA);
    expect("cap: a tie on time goes to the lower variant first",
           find(entries, 1, 2) == nullptr && find(entries, 2, 2) != nullptr && entries.size() == cap);

    /* A late observation older than every kept one is the one evicted: nothing changes. */
    store.observe(kOwnerA, 1, 6000, 50);
    entries = store.entriesFor(kOwnerA);
    expect("cap: a late, older one is evicted on arrival",
           find(entries, 1, 6000) == nullptr && entries.size() == cap && signals_ == 2);
    expect("cap: the other owner is untouched", store.entriesFor(kOwnerB).size() == 1);

    /* A file over the cap (written by hand) loads capped by the same order. */
    store.flushNow();
    store.waitForWrites();
    QJsonArray over = stored(name);
    QJsonObject extra;
    extra.insert(QStringLiteral("v"), 1);
    extra.insert(QStringLiteral("owner"), kOwnerA);
    extra.insert(QStringLiteral("variant"), 3);
    extra.insert(QStringLiteral("id"), 7000);
    extra.insert(QStringLiteral("first"), 1);
    extra.insert(QStringLiteral("last"), 1);
    over.append(extra);
    expect("cap: over-cap fixture", write_raw(name, QJsonDocument(over).toJson(QJsonDocument::Compact)));
    HeardTalkgroupStore reloaded(name);
    expect("cap: a file over the cap loads capped",
           reloaded.entriesFor(kOwnerA).size() == cap && find(reloaded.entriesFor(kOwnerA), 3, 7000) == nullptr);
    fresh(name);
}

void
test_unknown_kept() {
    const QString name = QStringLiteral("heard_unknown.json");
    fresh(name);
    const QByteArray fixture = "[{\"v\":2,\"owner\":\"" + kOwnerA.toUtf8()
                               + "\",\"tg\":\"x\",\"extra\":[1,2]},\"text\",7,"
                                 "{\"owner\":\"o\",\"id\":3},"
                                 "{\"v\":1,\"owner\":\""
                               + kOwnerA.toUtf8()
                               + "\",\"variant\":1,\"id\":9,\"first\":10,\"last\":20},"
                                 "{\"v\":1,\"owner\":\"\",\"variant\":1,\"id\":9,\"first\":10,\"last\":20},"
                                 "{\"v\":1,\"owner\":\"p\",\"variant\":1,\"id\":9,\"first\":30,\"last\":20}]";
    expect("unknown: fixture", write_raw(name, fixture));
    {
        HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow_all);
        expect("unknown: loads", store.loaded() && store.entriesFor(kOwnerA).size() == 1);
        store.observe(kOwnerA, 1, 10, 100);
        store.flushNow();
        store.waitForWrites();
        store.clear(kOwnerA);
        store.waitForWrites();
    }
    const QJsonArray back = stored(name);
    const QJsonArray original = QJsonDocument::fromJson(fixture).array();
    bool v2 = false;
    bool text = false;
    bool number = false;
    bool noV = false;
    for (const auto& value : back) {
        v2 |= value == original.at(0);
        text |= value == original.at(1);
        number |= value == original.at(2);
        noV |= value == original.at(3);
    }
    expect("unknown: another v is written back verbatim", v2);
    expect("unknown: elements that are not entries are written back", text && number && noV);
    expect("unknown: entries that break the schema are dropped", back.size() == 4);
    expect("unknown: clear leaves elements of another v",
           !stored_has(name, kOwnerA, 9) && !stored_has(name, kOwnerA, 10));
    fresh(name);
}

/* A file that cannot be read whole is never replaced, moved or purged, whatever the session does. */
void
check_unreadable_untouched(const QString& name, const QByteArray& fixture, const char* label) {
    fresh(name);
    expect(label, write_raw(name, fixture));
    WriteCounter counter;
    {
        HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow_all);
        expect("unreadable: not loaded", !store.loaded());
        expect("unreadable: not remembering", !store.remembering(kOwnerA));
        store.observe(kOwnerA, 1, 100, 5000);
        expect("unreadable: nothing learnt", store.entriesFor(kOwnerA).isEmpty() && !store.dirty());
        store.flushNow(); // Stop
        store.flushIfDue(0);
        store.flushIfDue(HeardTalkgroupStore::kFlushIntervalMs * 10);
        store.forget(kOwnerA, 100);
        store.clear(kOwnerA);
        store.purgeMissingOwners({});
        store.waitForWrites();
        expect("unreadable: bytes unchanged in session", raw(name) == fixture);
    } // teardown
    expect("unreadable: bytes unchanged after teardown", raw(name) == fixture);
    expect("unreadable: never moved aside",
           !QFileInfo::exists(json_store_path(name) + QStringLiteral(".unreadable-1")));
    expect("unreadable: no write reached the worker", counter.writes->load() == 0);
    fresh(name);
}

void
test_unreadable() {
    check_unreadable_untouched(QStringLiteral("heard_malformed.json"), "[{\"v\":1,\"owner\":", "malformed: fixture");
    check_unreadable_untouched(QStringLiteral("heard_future.json"), "{\"version\":2,\"entries\":[]}",
                               "future: fixture");
}

void
test_dirty_retry() {
    const QString name = QStringLiteral("heard_retry.json");
    fresh(name);
    HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow_all);
    /* A folder where the file goes: every write fails. */
    expect("retry: folder in the way", QDir().mkpath(json_store_path(name)));
    store.observe(kOwnerA, 1, 100, 5000);
    expect("retry: owed", store.dirty());
    store.flushIfDue(1000);
    store.waitForWrites();
    expect("retry: a failed write stays owed", store.dirty());
    expect("retry: folder removed", QDir().rmdir(json_store_path(name)));
    store.flushIfDue(1000 + HeardTalkgroupStore::kFlushIntervalMs - 1);
    store.waitForWrites();
    expect("retry: not again inside the interval", store.dirty() && !QFileInfo::exists(json_store_path(name)));
    store.flushIfDue(1000 + HeardTalkgroupStore::kFlushIntervalMs);
    store.waitForWrites();
    expect("retry: written when due again", !store.dirty() && stored_has(name, kOwnerA, 100));
    /* The same through Stop: a failed Stop flush is retried by the next one. */
    QFile::remove(json_store_path(name));
    expect("retry: folder again", QDir().mkpath(json_store_path(name)));
    store.observe(kOwnerA, 1, 101, 5000);
    store.flushNow();
    store.waitForWrites();
    expect("retry: a failed Stop write stays owed", store.dirty());
    expect("retry: folder removed again", QDir().rmdir(json_store_path(name)));
    store.flushNow();
    store.waitForWrites();
    expect("retry: the next Stop writes it", !store.dirty() && stored_has(name, kOwnerA, 101));
    fresh(name);
}

/* Observes every second for two minutes, the poll every 250 ms: writes at most and at least every 30 s. */
void
test_flush_interval() {
    const QString name = QStringLiteral("heard_interval.json");
    fresh(name);
    WriteCounter counter;
    HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow_all);
    const qint64 start = 100000;
    QVector<qint64> writeTimes;
    int lastCount = 0;
    for (qint64 now = start; now <= start + 120000; now += 250) {
        if ((now - start) % 1000 == 0) {
            store.observe(kOwnerA, 1, static_cast<quint32>(1 + (now - start) / 1000), now / 1000);
        }
        store.flushIfDue(now);
        store.waitForWrites();
        if (counter.writes->load() != lastCount) {
            lastCount = counter.writes->load();
            writeTimes.append(now);
        }
    }
    expect("interval: the first write is not held back", !writeTimes.isEmpty() && writeTimes.first() == start);
    bool atMost = true;
    bool atLeast = true;
    for (int i = 1; i < writeTimes.size(); ++i) {
        const qint64 gap = writeTimes.at(i) - writeTimes.at(i - 1);
        atMost &= gap >= HeardTalkgroupStore::kFlushIntervalMs;
        atLeast &= gap <= HeardTalkgroupStore::kFlushIntervalMs + 250;
    }
    expect("interval: at most once per 30 s", atMost);
    expect("interval: at least once per 30 s", atLeast && writeTimes.size() == 5);
    /* Nothing owed, nothing written. */
    const int written = counter.writes->load();
    store.flushIfDue(start + 1000000);
    store.flushNow();
    store.waitForWrites();
    expect("interval: a clean store does not write", counter.writes->load() == written);
    fresh(name);
}

/* The first write is held on the worker while forget (or clear) queues its own: the file ends with the later state. */
void
check_not_undone(bool clearAll) {
    const QString name = QStringLiteral("heard_inflight.json");
    fresh(name);
    auto gate = std::make_shared<QSemaphore>(0);
    auto entered = std::make_shared<QSemaphore>(0);
    auto first = std::make_shared<std::atomic<bool>>(true);
    HeardTalkgroupStore::setBeforeWriteForTest([gate, entered, first] {
        if (first->exchange(false)) {
            entered->release();
            gate->acquire();
        }
    });
    {
        HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow_all);
        int signals_ = 0;
        QObject::connect(&store, &HeardTalkgroupStore::ownerChanged, [&](const QString&) { signals_++; });
        store.observe(kOwnerA, 1, 100, 5000);
        store.observe(kOwnerA, 2, 100, 5000);
        store.observe(kOwnerA, 1, 200, 5000);
        store.flushNow();
        entered->acquire(); // the write of all three is on the worker now
        if (clearAll) {
            store.clear(kOwnerA);
            expect("in flight: clear empties at once", store.entriesFor(kOwnerA).isEmpty() && signals_ == 4);
        } else {
            store.forget(kOwnerA, 100);
            const auto entries = store.entriesFor(kOwnerA);
            expect("in flight: forget removes every variant at once",
                   entries.size() == 1 && entries.at(0).id == 200 && signals_ == 4);
        }
        gate->release();
        store.waitForWrites();
        expect("in flight: written straight away, no flush asked", !store.dirty());
        expect("in flight: the older write does not undo it", !stored_has(name, kOwnerA, 100));
        expect("in flight: the rest kept", stored_has(name, kOwnerA, 200) == !clearAll);
        store.forget(kOwnerA, 999);
        store.clear(kOwnerC);
        expect("in flight: forgetting what is not there changes nothing", signals_ == 4 && !store.dirty());
    }
    HeardTalkgroupStore::setBeforeWriteForTest({});
    HeardTalkgroupStore reloaded(name);
    expect("in flight: reloads as forgotten", find(reloaded.entriesFor(kOwnerA), 1, 100) == nullptr
                                                  && find(reloaded.entriesFor(kOwnerA), 2, 100) == nullptr);
    fresh(name);
}

void
test_forget_clear_not_undone() {
    check_not_undone(false);
    check_not_undone(true);
}

/* A burst queued behind a running write is one more write, of the newest state. */
void
test_burst_coalesced() {
    const QString name = QStringLiteral("heard_burst.json");
    fresh(name);
    auto gate = std::make_shared<QSemaphore>(0);
    auto entered = std::make_shared<QSemaphore>(0);
    auto writes = std::make_shared<std::atomic<int>>(0);
    HeardTalkgroupStore::setBeforeWriteForTest([gate, entered, writes] {
        if (writes->fetch_add(1) == 0) {
            entered->release();
            gate->acquire();
        }
    });
    {
        HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow_all);
        for (quint32 id = 1; id <= 3; ++id) {
            store.observe(kOwnerA, 1, id, 100);
        }
        store.observe(kOwnerB, 1, 9, 100);
        store.flushNow();
        entered->acquire(); // the first write runs with all four entries
        store.forget(kOwnerA, 1);
        store.forget(kOwnerA, 2);
        store.clear(kOwnerB);
        store.observe(kOwnerC, 1, 7, 200);
        store.flushNow();
        gate->release();
        store.waitForWrites();
        expect("burst: one write after the running one", writes->load() == 2);
        expect("burst: it holds the newest state",
               !store.dirty() && !stored_has(name, kOwnerA, 1) && !stored_has(name, kOwnerA, 2)
                   && stored_has(name, kOwnerA, 3) && !stored_has(name, kOwnerB, 9) && stored_has(name, kOwnerC, 7));
    }
    HeardTalkgroupStore::setBeforeWriteForTest({});
    expect("burst: nothing more at teardown", writes->load() == 2);
    fresh(name);
}

/* Going off screen (Suspended or Hidden) writes what is owed; coming back on screen does not. */
void
test_pause_hook() {
    const QString name = QStringLiteral("heard_pause.json");
    auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    expect("pause: a GUI application", app != nullptr);
    if (app == nullptr) {
        return;
    }
    fresh(name);
    HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow_all);
    store.observe(kOwnerA, 1, 1, 10);
    Q_EMIT app->applicationStateChanged(Qt::ApplicationActive);
    store.waitForWrites();
    expect("pause: on screen writes nothing", store.dirty());
    Q_EMIT app->applicationStateChanged(Qt::ApplicationSuspended);
    store.waitForWrites();
    expect("pause: suspended writes", !store.dirty() && stored_has(name, kOwnerA, 1));
    store.observe(kOwnerA, 1, 2, 10);
    Q_EMIT app->applicationStateChanged(Qt::ApplicationHidden);
    store.waitForWrites();
    expect("pause: hidden writes", !store.dirty() && stored_has(name, kOwnerA, 2));
    fresh(name);
}

/* Quitting writes what is owed while the store is still alive (aboutToQuit). Runs the event loop, so it goes last. */
void
test_quit_hook() {
    const QString name = QStringLiteral("heard_quit.json");
    fresh(name);
    HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow_all);
    store.observe(kOwnerA, 1, 3, 10);
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
    (void)QCoreApplication::exec();
    store.waitForWrites();
    expect("quit: written at quit", !store.dirty() && stored_has(name, kOwnerA, 3));
    fresh(name);
}

void
test_purge() {
    const QString name = QStringLiteral("heard_purge.json");
    fresh(name);
    {
        HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow_all);
        store.observe(kOwnerA, 1, 1, 10);
        store.observe(kOwnerB, 1, 2, 10);
        store.observe(kOwnerC, 1, 3, 10);
        store.flushNow();
        store.waitForWrites();
        QSet<QString> changedOwners;
        QObject::connect(&store, &HeardTalkgroupStore::ownerChanged,
                         [&](const QString& owner) { changedOwners.insert(owner); });
        store.purgeMissingOwners({kOwnerA, kOwnerC});
        expect("purge: only the missing owner goes", store.entriesFor(kOwnerB).isEmpty()
                                                         && store.entriesFor(kOwnerA).size() == 1
                                                         && store.entriesFor(kOwnerC).size() == 1);
        expect("purge: its owner is signalled", changedOwners == QSet<QString>{kOwnerB});
        store.waitForWrites();
        expect("purge: written at once",
               !store.dirty() && !stored_has(name, kOwnerB, 2) && stored_has(name, kOwnerA, 1));
        store.purgeMissingOwners({kOwnerA, kOwnerB, kOwnerC});
        expect("purge: nothing missing, nothing changes", changedOwners.size() == 1);
    }
    fresh(name);
}

void
test_policy() {
    const QString name = QStringLiteral("heard_policy.json");
    fresh(name);
    HeardTalkgroupStore store(name);
    store.observe(kOwnerA, 1, 1, 10);
    expect("policy: no policy learns nothing", store.entriesFor(kOwnerA).isEmpty() && !store.dirty());
    auto on = std::make_shared<QSet<QString>>(QSet<QString>{kOwnerA});
    store.setOwnerPolicy([on](const QString& owner) { return on->contains(owner); });
    store.observe(kOwnerA, 1, 1, 10);
    store.observe(kOwnerB, 1, 1, 10);
    expect("policy: an allowed owner learns", store.entriesFor(kOwnerA).size() == 1 && store.remembering(kOwnerA));
    expect("policy: a switched-off owner learns nothing",
           store.entriesFor(kOwnerB).isEmpty() && !store.remembering(kOwnerB));
    on->remove(kOwnerA);
    store.observe(kOwnerA, 1, 2, 20);
    expect("policy: switched off later, new calls are not learnt", store.entriesFor(kOwnerA).size() == 1);
    store.clear(kOwnerA);
    store.waitForWrites();
    expect("policy: clear works with the switch off", store.entriesFor(kOwnerA).isEmpty() && !store.dirty());
    fresh(name);
}

/* Issue #903: forget and clear run the before-edit hook first, and only when they remove something; what the hook
   observes (History folding what its background tap held) is removed with the rest. */
void
test_before_edit_hook() {
    const QString name = QStringLiteral("heard_before_edit.json");
    fresh(name);
    HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow_all);
    int runs = 0;
    store.setBeforeEdit([&store, &runs]() {
        runs++;
        store.observe(kOwnerA, 2, 100, 50);
        store.observe(kOwnerA, 1, 300, 60);
        return true;
    });
    store.forget(kOwnerA, 100);
    store.clear(kOwnerA);
    expect("before edit: nothing held, the hook does not run", runs == 0);
    store.observe(kOwnerA, 1, 100, 10);
    store.observe(kOwnerA, 1, 200, 20);
    store.forget(kOwnerA, 100);
    QVector<HeardTalkgroupStore::Entry> left = store.entriesFor(kOwnerA);
    expect("before edit: forget runs the hook once, first", runs == 1);
    expect("before edit: what the hook observed of the id goes with the forget",
           left.size() == 2 && std::none_of(left.cbegin(), left.cend(), [](const HeardTalkgroupStore::Entry& e) {
               return e.id == 100U;
           }));
    store.clear(kOwnerA);
    expect("before edit: clear runs the hook first and removes what it observed",
           runs == 2 && store.entriesFor(kOwnerA).isEmpty());
    store.waitForWrites();
    fresh(name);
}

/* Issue #903 (PR2 review r1): a before-edit hook that fails (History could not settle its spill) refuses the forget or
   clear: nothing is removed or written, and the next one, with the hook settled, goes through. */
void
test_before_edit_hook_refusal() {
    const QString name = QStringLiteral("heard_before_edit_refused.json");
    fresh(name);
    HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow_all);
    bool settles = false;
    store.setBeforeEdit([&settles]() { return settles; });
    store.observe(kOwnerA, 1, 100, 10);
    store.observe(kOwnerA, 1, 200, 20);
    store.flushNow();
    store.waitForWrites();
    int changes = 0;
    QObject::connect(&store, &HeardTalkgroupStore::ownerChanged, [&changes]() { changes++; });
    expect("refused edit: the forget is refused", !store.forget(kOwnerA, 100));
    expect("refused edit: the clear is refused", !store.clear(kOwnerA));
    expect("refused edit: nothing is removed", store.entriesFor(kOwnerA).size() == 2 && changes == 0);
    expect("refused edit: nothing is owed", !store.dirty());
    settles = true;
    expect("refused edit: a settled forget goes through",
           store.forget(kOwnerA, 100) && store.entriesFor(kOwnerA).size() == 1);
    expect("refused edit: a settled clear goes through", store.clear(kOwnerA) && store.entriesFor(kOwnerA).isEmpty());
    expect("refused edit: nothing held is nothing to refuse", store.forget(kOwnerA, 100) && store.clear(kOwnerA));
    store.waitForWrites();
    fresh(name);
}

/* What is owed at teardown is written by the destructor. */
void
test_teardown_writes() {
    const QString name = QStringLiteral("heard_teardown.json");
    fresh(name);
    {
        HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow_all);
        store.observe(kOwnerA, 9, 4242, 77);
    }
    HeardTalkgroupStore reloaded(name);
    const auto entries = reloaded.entriesFor(kOwnerA);
    expect("teardown: the owed state is written",
           entries.size() == 1 && entries.at(0).variant == 9 && entries.at(0).id == 4242 && entries.at(0).last == 77);
    fresh(name);
}

} // namespace

int
main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-heard-talkgroups-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    test_upsert();
    test_cap_and_eviction();
    test_unknown_kept();
    test_unreadable();
    test_dirty_retry();
    test_flush_interval();
    test_forget_clear_not_undone();
    test_purge();
    test_policy();
    test_before_edit_hook();
    test_before_edit_hook_refusal();
    test_teardown_writes();
    test_burst_coalesced();
    test_pause_hook();
    test_quit_hook(); // last: it runs the event loop

    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
