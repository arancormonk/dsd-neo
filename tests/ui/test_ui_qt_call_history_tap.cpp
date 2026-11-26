// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Issue #708, B1: History lost calls while the app was in the background. Qt pauses the GUI thread there, the UI poll
 * tick was the only reader of the event-history ring, and the ring keeps 254 committed rows a slot, so a busy system
 * overwrote calls History never saw. CallHistoryTap now reads the published rings through the History tap
 * (dsd_app_history_tap_copy()) while the app is suspended or hidden, spills what it finds, and the model folds the
 * spill in at the first ingest after the app returns.
 *
 * Everything here runs the real publisher (dsd_app_telemetry_publish_snapshot()), the real tap and the real GUI
 * consumer (dsd_app_get_latest_snapshot()): 300 commits while suspended keep every row; a process that dies in the
 * background leaves a spill the relaunch folds in, once, and drops only after an acknowledged save; the commits a
 * stopped decoder left published are captured; a clear made while the tap runs keeps its rows cleared, and one made
 * after the app returned but before its first ingest hands the tap off instead of restarting it; the spill is written
 * in batches of about a kilobyte a row; and a spill that cannot be written holds a bounded backlog, the newest rows,
 * reported once, which reaches History with the handoff, or the model's teardown, when the spill is still unwritable
 * then (a call whose first sighting the bound dropped included, from its updates). Notices fold once like calls, a replay begun in the
 * background included. A spill far longer than the log folds bounded: the newest rows stay, the oldest session's
 * oldest go first, and neither the log nor the fold's reader and seen map ever hold the whole backlog. A read that fails
 * partway keeps the spill whole for the next fold. A session that changes while the tap runs hidden hands it off and
 * starts it again: the old session's calls keep its attribution, the new session's take theirs. A spilled row keeps
 * its site and call channel (issue #799), within the spill's size per row. A site text digest an older build's seen
 * entry takes from the tap's read reaches the seen store through the spill, so a text that changes alone after it is
 * read again at the next launch (issue #40). Issue #800, 2.7: a run the tap read first keeps a session ordinal no later
 * run's comes before, even when the process dies before its spill folds and the fold fails once after a newer run: the
 * merged call shows the newer run's fix, though the older run's revs are higher. The deliberate failures' warnings are
 * counted, not printed. */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QModelIndex>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <memory>
#include <qflags.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <utility>
#include "../../src/app_control/snapshot_internal.h"
#include "../test_support/qt_test_paths.h"
#include "call_history_heard.h"
#include "call_history_model.h"
#include "call_history_tap.h"
#include "heard_talkgroup_sink.h"
#include "heard_talkgroup_store.h"
#include "json_store.h"

using dsd_qt::CallHistoryModel;

namespace {

int g_failures = 0;

/* The model's and the tap's once-only warnings, counted (from any thread) and kept out of the output. */
std::atomic<int> g_saveFailedWarnings{0};
std::atomic<int> g_spillDropWarnings{0};
std::atomic<int> g_spillReadWarnings{0};
QtMessageHandler g_previousHandler = nullptr;

void
count_warnings(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (message.startsWith(QLatin1String("call history: saving the log failed"))) {
        g_saveFailedWarnings++;
        return;
    }
    if (message.startsWith(QLatin1String("call history: the background capture cannot write its spill"))) {
        g_spillDropWarnings++;
        return;
    }
    if (message.startsWith(QLatin1String("call history: reading the background spill failed"))) {
        g_spillReadWarnings++;
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

void
resetStorage(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    QSettings settings;
    settings.clear();
    settings.sync();
}

QString
spill_path(void) {
    return dsd_qt::json_store_path("call_history_spill.jsonl");
}

qint64
spill_size(void) {
    return QFileInfo(spill_path()).size();
}

int
spill_lines(void) {
    QFile file(spill_path());
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    return static_cast<int>(file.readAll().count('\n'));
}

/* A decoder state the real publisher copies. Each one is a new ring (initState() draws its identity), and its
 * revisions start past every earlier ring's, so the publisher copies its slots whatever the last ring reached. */
struct Decoder {
    dsd_state* state;
    std::unique_ptr<dsd_opts> opts;

    Decoder() : state(static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)))), opts(std::make_unique<dsd_opts>()) {
        static uint64_t revisionBase = 0;
        revisionBase += 1000000U;
        initOpts(opts.get());
        initState(state);
        for (int slot = 0; slot < 2; slot++) {
            state->event_history_s[slot].revision = revisionBase;
        }
        dsd_app_telemetry_publish_opts_snapshot(opts.get());
        publish();
    }

    ~Decoder() {
        freeState(state);
        free(state);
    }

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /** Commit a voice row the way push_event_history() does: shift, fill index 1. */
    void
    commit(int slot, uint32_t tg, uint32_t src, time_t start) {
        Event_History_I* ring = &state->event_history_s[slot];
        DSD_MEMMOVE(&ring->Event_History_Items[2], &ring->Event_History_Items[1],
                    sizeof(Event_History) * (DSD_EVENT_HISTORY_LEN - 2));
        Event_History* item = &ring->Event_History_Items[1];
        DSD_MEMSET(item, 0, sizeof(*item));
        item->category = DSD_EVENT_CATEGORY_VOICE;
        item->target_id = tg;
        item->source_id = src;
        item->event_start_time = start;
        item->event_time = start + 4;
        ring->push_seq++;
        ring->commit_rev++;
        ring->revision++;
    }

    /** The core's in-place merge of a reacquired segment into the newest committed row: its end moves on. */
    void
    extendNewest(int slot, time_t seconds) {
        Event_History_I* ring = &state->event_history_s[slot];
        ring->Event_History_Items[1].event_time += seconds;
        ring->commit_rev++;
        ring->revision++;
    }

    /** @p count calls on @p slot, talkgroups from @p tg up, 30 s apart from @p start (none merge). */
    void
    commitCalls(int slot, int count, uint32_t tg, time_t start) {
        for (int i = 0; i < count; i++) {
            commit(slot, tg + static_cast<uint32_t>(i), 100U + static_cast<uint32_t>(i),
                   start + 30 * static_cast<time_t>(i));
        }
    }

    /** Give the newest committed row the site its call was heard on and the call's channel, as the core freezes them. */
    void
    siteNewest(int slot, const dsd_received_identity& site, uint8_t kind, uint32_t channel) {
        Event_History* item = &state->event_history_s[slot].Event_History_Items[1];
        item->site_id = site;
        item->call_channel_kind = kind;
        item->channel = channel;
    }

    /** Give the newest committed row GPS text and the admitted position it describes (issue #800). */
    void
    gpsNewest(int slot, const char* text, int32_t lat_e7, uint64_t rev) {
        Event_History* item = &state->event_history_s[slot].Event_History_Items[1];
        DSD_SNPRINTF(item->gps_s, sizeof(item->gps_s), "%s", text);
        item->pos.valid = 1U;
        item->pos.lat_e7 = lat_e7;
        item->pos.lon_e7 = 450000000;
        item->pos.kind = DSD_POS_DMR_EMBEDDED;
        item->pos.rev = rev;
    }

    /** The core withdraws the newest committed row's report in place: its GPS text and position cleared together. */
    void
    clearGpsNewest(int slot) {
        Event_History_I* ring = &state->event_history_s[slot];
        Event_History* item = &ring->Event_History_Items[1];
        item->gps_s[0] = '\0';
        DSD_MEMSET(&item->pos, 0, sizeof(item->pos));
        ring->commit_rev++;
        ring->revision++;
    }

    /** Give the newest committed row a known reception, as the core stamps it (Event_History::rx_scope): one tuned
     * frequency, read under the record's first epoch. */
    void
    heardNewest(int slot) {
        Event_History* item = &state->event_history_s[slot].Event_History_Items[1];
        item->rx_scope.scope.kind = DSD_POS_SCOPE_FEED_TUNED_FREQ;
        item->rx_scope.scope.a = 450000000U;
        item->rx_scope.resolved = 1U;
        item->rx_scope.rx_gen = 1U;
        item->rx_scope.epoch_known = 1U;
        item->freq_hz = 450000000;
    }

    /** Commit a data notice the way dsd_event_emit_data_notice() does. */
    void
    commitNotice(int slot, time_t when, const char* text) {
        commit(slot, 0U, 0U, when);
        Event_History* item = &state->event_history_s[slot].Event_History_Items[1];
        item->category = DSD_EVENT_CATEGORY_DATA;
        item->event_time = when;
        DSD_SNPRINTF(item->event_string, sizeof(item->event_string), "%s", text);
    }

    void
    publish() {
        dsd_app_telemetry_publish_snapshot(state);
    }
};

/* What UiController::flushHistory() does: the GUI's ingest of the latest snapshot. */
void
gui_ingest(CallHistoryModel& model) {
    const dsd_opts* opts = dsd_app_get_latest_opts_snapshot();
    const dsd_state* snapshot = dsd_app_get_latest_snapshot();
    model.refresh(snapshot, opts);
}

void
set_app_state(Qt::ApplicationState state) {
    Q_EMIT qGuiApp->applicationStateChanged(state);
}

void
settle_save(CallHistoryModel& model) {
    model.waitForSaveForTest();
    QCoreApplication::processEvents();
}

int
notice_rows(const CallHistoryModel& model) {
    int notices = 0;
    for (int i = 0; i < model.count(); i++) {
        notices += model.data(model.index(i), CallHistoryModel::KindRole).toInt() == CallHistoryModel::KindNotice;
    }
    return notices;
}

int
rows_in_tg_range(const CallHistoryModel& model, qulonglong lo, qulonglong hi) {
    QSet<qulonglong> tgs;
    for (int i = 0; i < model.count(); i++) {
        const qulonglong tg = model.data(model.index(i), CallHistoryModel::TgRole).toULongLong();
        if (tg >= lo && tg <= hi) {
            tgs.insert(tg);
        }
    }
    return static_cast<int>(tgs.size());
}

/* The tap polls at its start and then only when a test asks (waitForBackgroundPollForTest()): a poll a minute
 * otherwise, so a loaded machine cannot slip extra polls between a test's commits and change the batching. */
void
start_tap(CallHistoryModel& model) {
    model.setHistoryTapSource(&dsd_app_history_tap_copy);
    model.setBackgroundCaptureTimingForTest(60000, 5);
}

/* B1: 300 commits while the app is suspended, more than the ring holds, and every one is in History after it
 * returns. Without the tap the GUI's first read finds the newest 254. The spill goes only once a save that covers it
 * is acknowledged: a failing save keeps it. */
void
test_background_commits_keep_every_row(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754800000;
    CallHistoryModel model;
    start_tap(model);
    decoder.commit(0, 31000U, 99U, when - 600);
    decoder.publish();
    gui_ingest(model);
    expect("the call before the suspend is ingested", model.count() == 1);

    set_app_state(Qt::ApplicationSuspended);
    expect("the tap runs while suspended", model.backgroundCaptureRunningForTest());
    for (int batch = 0; batch < 6; batch++) {
        decoder.commitCalls(0, 50, 31001U + static_cast<uint32_t>(batch) * 50U,
                            when + static_cast<time_t>(batch) * 1500);
        decoder.publish();
        model.waitForBackgroundPollForTest();
    }
    expect("the GUI reads nothing while suspended", model.count() == 1);

    set_app_state(Qt::ApplicationActive);
    expect("the return owes the handoff", model.backgroundCaptureHandoffOwed());
    gui_ingest(model);
    expect("the first ingest stops the tap",
           !model.backgroundCaptureRunningForTest() && !model.backgroundCaptureHandoffOwed());
    expect("every call committed while suspended is in History",
           model.count() == 301 && rows_in_tg_range(model, 31000U, 31300U) == 301);
    gui_ingest(model);
    expect("reading the ring again adds nothing", model.count() == 301);

    expect("the spill holds what the tap found", spill_lines() == 300);
    const QString seenPath = dsd_qt::json_store_path("call_history_seen.json");
    QFile::remove(seenPath);
    expect("the seen store is blocked", QDir().mkpath(seenPath));
    g_saveFailedWarnings = 0;
    model.saveNow();
    settle_save(model);
    expect("a failed save keeps the spill", model.saveFailing() && spill_lines() == 300);
    expect("the failure is reported once", g_saveFailedWarnings == 1);
    QDir(seenPath).removeRecursively();
    model.saveNow();
    settle_save(model);
    expect("an acknowledged save drops the spill", !model.saveFailing() && spill_size() == 0);
    CallHistoryModel relaunched;
    expect("the saved log has every call", relaunched.count() == 301);
}

/* A process killed in the background never runs its handoff. The tap's batches are already on the disk, and the
 * relaunch folds them in, once, whatever the ring still holds. */
void
test_spill_reload_after_process_death(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754810000;
    {
        CallHistoryModel model;
        start_tap(model);
        decoder.commit(0, 32000U, 99U, when - 600);
        decoder.publish();
        gui_ingest(model);
        set_app_state(Qt::ApplicationSuspended);
        decoder.commitCalls(0, 40, 32001U, when);
        decoder.commitCalls(1, 20, 32101U, when + 15);
        decoder.commitNotice(1, when + 700, "2025-08-06 12:00:00 SMS from 4321");
        decoder.publish();
        for (int poll = 0; poll < 6; poll++) {
            model.waitForBackgroundPollForTest();
        }
        expect("the batch reached the disk while the tap still runs",
               model.backgroundCaptureRunningForTest() && spill_lines() == 61 && model.spillAppendsForTest() == 1);
    } // the process "dies": nothing folds the spill or saves the rows it holds
    expect("the spill outlives the process", spill_lines() == 61);
    const QString seenPath = dsd_qt::json_store_path("call_history_seen.json");
    {
        CallHistoryModel relaunched;
        expect("the relaunch folds the spill",
               relaunched.count() == 62 && rows_in_tg_range(relaunched, 32000U, 32040U) == 41
                   && rows_in_tg_range(relaunched, 32101U, 32120U) == 20 && notice_rows(relaunched) == 1);
        gui_ingest(relaunched);
        expect("the ring the rows are still in adds none twice",
               relaunched.count() == 62 && notice_rows(relaunched) == 1);
        // This one cannot save either (its last save, on destruction, fails).
        QFile::remove(seenPath);
        expect("the seen store is blocked", QDir().mkpath(seenPath));
    }
    QDir(seenPath).removeRecursively();
    expect("a relaunch that folded but did not get its save acknowledged keeps the spill", spill_lines() == 61);
    {
        CallHistoryModel again;
        expect("folding it a second time adds nothing, a notice included",
               again.count() == 62 && notice_rows(again) == 1);
        again.saveNow();
        settle_save(again);
        expect("the acknowledged save drops it", spill_size() == 0);
    }
    CallHistoryModel last;
    expect("the log keeps every call", last.count() == 62 && notice_rows(last) == 1);
}

/* A decoder that stops while the app is away leaves its last rings published; the tap's next copy has its final
 * commits, and so does History. */
void
test_stop_while_suspended(void) {
    resetStorage();
    auto decoder = std::make_unique<Decoder>();
    const time_t when = 1754820000;
    CallHistoryModel model;
    start_tap(model);
    gui_ingest(model);
    set_app_state(Qt::ApplicationSuspended);
    decoder->commitCalls(0, 200, 33001U, when);
    decoder->publish();
    model.waitForBackgroundPollForTest();
    // The session's last calls, its last publish, and the decoder's state goes away.
    decoder->commitCalls(0, 100, 33201U, when + 9000);
    decoder->publish();
    decoder.reset();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("the stopped session's final commits are in History",
           model.count() == 300 && rows_in_tg_range(model, 33001U, 33300U) == 300);
}

/* A clear made while the tap runs (the app hidden with its loop still running) covers what the tap read: those rows
 * stay cleared after the return and across a relaunch, and only what came after the clear shows. */
void
test_clear_while_suspended(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754830000;
    {
        CallHistoryModel model;
        start_tap(model);
        decoder.commitCalls(0, 5, 34001U, when);
        decoder.publish();
        gui_ingest(model);
        expect("the calls before hiding are ingested", model.count() == 5);
        set_app_state(Qt::ApplicationHidden);
        decoder.commitCalls(0, 10, 34101U, when + 600);
        decoder.publish();
        for (int poll = 0; poll < 6; poll++) {
            model.waitForBackgroundPollForTest();
        }
        expect("the tap spilled them", spill_lines() == 10);
        model.clearAll();
        expect("the clear empties the log and the spill, and the tap goes on",
               model.count() == 0 && spill_size() == 0 && model.backgroundCaptureRunningForTest());
        decoder.commitCalls(1, 3, 34201U, when + 1200);
        decoder.publish();
        model.waitForBackgroundPollForTest();
        set_app_state(Qt::ApplicationActive);
        gui_ingest(model);
        expect("only the calls after the clear show",
               model.count() == 3 && rows_in_tg_range(model, 34201U, 34203U) == 3);
        model.saveNow();
        settle_save(model);
    }
    CallHistoryModel relaunched;
    gui_ingest(relaunched);
    expect("the cleared calls stay cleared across a relaunch",
           relaunched.count() == 3 && rows_in_tg_range(relaunched, 34201U, 34203U) == 3);
}

/* A clear made after the app returned but before the first ingest hands the tap off rather than restarting it in the
 * foreground: the tap stays stopped, its rows stay cleared, and what comes after shows. */
void
test_clear_before_the_first_ingest_after_return(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754850000;
    CallHistoryModel model;
    start_tap(model);
    decoder.commitCalls(0, 3, 36001U, when);
    decoder.publish();
    gui_ingest(model);
    set_app_state(Qt::ApplicationSuspended);
    decoder.commitCalls(0, 8, 36101U, when + 600);
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    expect("the return owes the handoff", model.backgroundCaptureHandoffOwed());
    model.clearAll();
    expect("the clear hands the tap off and leaves it stopped", !model.backgroundCaptureRunningForTest()
                                                                    && !model.backgroundCaptureHandoffOwed()
                                                                    && model.count() == 0 && spill_size() == 0);
    decoder.commitCalls(1, 2, 36201U, when + 1200);
    decoder.publish();
    gui_ingest(model);
    expect("only the calls after the clear show", model.count() == 2 && rows_in_tg_range(model, 36201U, 36202U) == 2
                                                      && !model.backgroundCaptureRunningForTest());
}

/* A spill that cannot be written (a full disk; here a directory in its place) leaves the tap holding at most twice one
 * ring's rows: the newest, the oldest dropped and reported once. What it held reaches History once the spill can be
 * written. */
void
test_unwritable_spill_keeps_a_bounded_backlog(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754860000;
    CallHistoryModel model;
    start_tap(model);
    gui_ingest(model);
    expect("the spill is blocked", QDir().mkpath(spill_path()));
    g_spillDropWarnings = 0;
    set_app_state(Qt::ApplicationSuspended);
    decoder.commitCalls(0, 250, 37001U, when);
    decoder.commitCalls(1, 250, 37301U, when);
    decoder.publish();
    model.waitForBackgroundPollForTest();
    decoder.commitCalls(0, 250, 37601U, when + 9000);
    decoder.publish();
    for (int poll = 0; poll < 6; poll++) {
        model.waitForBackgroundPollForTest();
    }
    expect("the backlog beyond the bound is reported once", g_spillDropWarnings == 1);
    // Cut to the bound, the backlog is retried on the batch cadence, not at every poll.
    const int attempts = model.spillAppendAttemptsForTest();
    for (int poll = 0; poll < 10; poll++) {
        model.waitForBackgroundPollForTest();
    }
    expect("a failing spill is retried every five polls",
           model.spillAppendAttemptsForTest() - attempts >= 1 && model.spillAppendAttemptsForTest() - attempts <= 2);
    expect("and reported no more", g_spillDropWarnings == 1);
    QDir(spill_path()).removeRecursively();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    // Held: the newest 508 of 750 (the first call batch's last 8, the second slot's 250, the last batch's 250); the
    // ring still had the first batch's last 4 besides.
    expect("the newest rows reach History", model.count() == 508 && rows_in_tg_range(model, 37601U, 37850U) == 250
                                                && rows_in_tg_range(model, 37301U, 37550U) == 250
                                                && rows_in_tg_range(model, 37243U, 37250U) == 8);
    expect("the oldest held rows were dropped", rows_in_tg_range(model, 37001U, 37242U) == 0);
}

/* A spill still unwritable when the app returns: the tap's last append fails too, and the rows it holds go to the
 * model with the handoff, not to the next start, which clears them. 350 calls on one slot while suspended, under the
 * 508-row bound, more than the ring keeps: every one is in History. Once the spill can be written, the next background
 * run writes only what is new, and no call comes twice. */
void
test_unwritable_spill_hands_off_what_it_holds(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754870000;
    CallHistoryModel model;
    start_tap(model);
    gui_ingest(model);
    expect("handoff: the spill is blocked", QDir().mkpath(spill_path()));
    g_spillDropWarnings = 0;
    set_app_state(Qt::ApplicationSuspended);
    for (int batch = 0; batch < 7; batch++) {
        decoder.commitCalls(0, 50, 38001U + static_cast<uint32_t>(batch) * 50U,
                            when + static_cast<time_t>(batch) * 1500);
        decoder.publish();
        model.waitForBackgroundPollForTest();
    }
    expect("handoff: nothing reached the spill", spill_lines() == 0);
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("handoff: every call held is in History",
           model.count() == 350 && rows_in_tg_range(model, 38001U, 38350U) == 350);
    expect("handoff: none was dropped", g_spillDropWarnings == 0);
    gui_ingest(model);
    expect("handoff: reading the ring again adds nothing", model.count() == 350);

    QDir(spill_path()).removeRecursively();
    set_app_state(Qt::ApplicationSuspended);
    decoder.commitCalls(0, 10, 38401U, when + 20000);
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("handoff: the spill, writable again, takes only the new calls", spill_lines() == 10);
    expect("handoff: no call twice", model.count() == 360 && rows_in_tg_range(model, 38001U, 38410U) == 360);
}

/* A model torn down while the spill is still unwritable (the app closed from the background) has the rows the tap's
 * last append could not write, as the handoff has: they go into the log before its last save. 300 calls on one slot
 * while suspended, more than the ring keeps: after the relaunch every one is in History, and the ring adds none
 * twice. */
void
test_unwritable_spill_teardown_keeps_what_it_holds(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754875000;
    {
        CallHistoryModel model;
        start_tap(model);
        gui_ingest(model);
        expect("teardown: the spill is blocked", QDir().mkpath(spill_path()));
        g_spillDropWarnings = 0;
        set_app_state(Qt::ApplicationSuspended);
        for (int batch = 0; batch < 6; batch++) {
            decoder.commitCalls(0, 50, 38501U + static_cast<uint32_t>(batch) * 50U,
                                when + static_cast<time_t>(batch) * 1500);
            decoder.publish();
            model.waitForBackgroundPollForTest();
        }
        expect("teardown: the tap runs and nothing reached the spill",
               model.backgroundCaptureRunningForTest() && spill_lines() == 0);
    }
    QDir(spill_path()).removeRecursively();
    CallHistoryModel relaunched;
    expect("teardown: every call held is in History",
           relaunched.count() == 300 && rows_in_tg_range(relaunched, 38501U, 38800U) == 300);
    gui_ingest(relaunched);
    expect("teardown: the ring adds none twice", relaunched.count() == 300);
    expect("teardown: none was dropped", g_spillDropWarnings == 0);
}

/* A long call heard again and again while the spill cannot be written: each reacquisition is an update the tap holds,
 * until the bound drops the call's first sighting and keeps only updates. Those carry the whole row, and the model,
 * which never logged the call, takes the first of them as the call: the ring walk after the handoff finds it seen. */
void
test_unwritable_spill_keeps_a_call_only_its_updates_hold(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754880000;
    CallHistoryModel model;
    start_tap(model);
    gui_ingest(model);
    expect("updates only: the spill is blocked", QDir().mkpath(spill_path()));
    g_spillDropWarnings = 0;
    set_app_state(Qt::ApplicationSuspended);
    decoder.commit(0, 39001U, 99U, when);
    decoder.publish();
    model.waitForBackgroundPollForTest();
    // One more update than the backlog holds rows: the first sighting goes.
    for (int update = 0; update < 2 * (DSD_EVENT_HISTORY_LEN - 1); update++) {
        decoder.extendNewest(0, 2);
        decoder.publish();
        model.waitForBackgroundPollForTest();
    }
    expect("updates only: the first sighting was dropped", g_spillDropWarnings == 1);
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("updates only: the call is in History once",
           model.count() == 1 && rows_in_tg_range(model, 39001U, 39001U) == 1);
    gui_ingest(model);
    expect("updates only: reading the ring again adds nothing", model.count() == 1);
    expect("updates only: with the end the last update gave it",
           model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt()
               == 4 + 4 * (DSD_EVENT_HISTORY_LEN - 1));
    QDir(spill_path()).removeRecursively();
}

/* A capture replayed twice logs its notice once: the second replay's copy takes the first's row as its twin. A third
 * replay begun while the app is away is read by the tap, and its notice folds at the handoff, before the model's own
 * walk reads the new ring: it is absorbed against the twins that ring has taken (none), not the last ring's, and the
 * notice still shows once. */
void
test_replayed_notice_folds_once_from_a_new_ring(void) {
    resetStorage();
    const time_t when = 1754890000;
    const char* text = "2025-08-06 12:00:00 SMS from 4321";
    CallHistoryModel model;
    start_tap(model);
    for (int replay = 0; replay < 2; replay++) {
        Decoder decoder;
        decoder.commitNotice(1, when, text);
        decoder.publish();
        gui_ingest(model);
    }
    expect("replayed notice: two replays show it once", notice_rows(model) == 1 && model.count() == 1);
    Decoder third;
    set_app_state(Qt::ApplicationSuspended);
    third.commitNotice(1, when, text);
    third.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("replayed notice: a third replay read in the background still shows it once",
           notice_rows(model) == 1 && model.count() == 1);
    gui_ingest(model);
    expect("replayed notice: reading the ring again adds nothing", notice_rows(model) == 1 && model.count() == 1);
}

/* The spill is written in batches, never per row: a call a poll for 100 polls appends at most once every five
 * polls (five seconds at the app's pace), about a kilobyte a row, so even ten calls a second stay under the 16 KiB/s
 * the History stores budget. */
void
test_sustained_write_volume(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754840000;
    CallHistoryModel model;
    start_tap(model);
    gui_ingest(model);
    set_app_state(Qt::ApplicationSuspended);
    const int polls = 100;
    for (int i = 0; i < polls; i++) {
        decoder.commit(i % 2, 35001U + static_cast<uint32_t>(i), 500U + static_cast<uint32_t>(i),
                       when + 30 * static_cast<time_t>(i));
        decoder.publish();
        model.waitForBackgroundPollForTest();
    }
    const int appends = model.spillAppendsForTest();
    expect("appends are batched", appends >= 1 && appends <= polls / 5 + 1);
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    const qint64 bytes = spill_size();
    const int lines = spill_lines();
    expect("every call is in History", model.count() == polls && lines == polls);
    expect("a row costs about a kilobyte of spill", lines > 0 && bytes / lines <= 1536);
    expect("ten calls a second stay inside the 16 KiB/s budget",
           lines > 0 && qint64{10} * bytes / lines < qint64{16} * 1024);
}

/* Issue #799: the rows the tap spills carry their site and channel back into History, and a DMR site with the longest
 * texts the record holds keeps a row's spill line within the size the write budget above assumes. */
void
test_spill_with_site_under_size_cap(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754845000;
    dsd_received_identity site = {};
    site.family = DSD_RXID_FAMILY_DMR;
    site.dmr_trunk = DSD_RXID_DMR_TRUNK_CONNECT_PLUS;
    site.valid = DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND;
    site.dmr_net = 4095U;
    site.dmr_site = 255U;
    DSD_MEMSET(site.dmr_site_text, 'N', sizeof(site.dmr_site_text) - 1U);
    DSD_MEMSET(site.dmr_brand, 'B', sizeof(site.dmr_brand) - 1U);
    CallHistoryModel model;
    start_tap(model);
    gui_ingest(model);
    set_app_state(Qt::ApplicationSuspended);
    const int polls = 100;
    for (int i = 0; i < polls; i++) {
        decoder.commit(i % 2, 36001U + static_cast<uint32_t>(i), 600U + static_cast<uint32_t>(i),
                       when + 30 * static_cast<time_t>(i));
        decoder.siteNewest(i % 2, site, DSD_CALL_CHANNEL_LCN, 1000U + static_cast<uint32_t>(i));
        decoder.publish();
        model.waitForBackgroundPollForTest();
    }
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    const qint64 bytes = spill_size();
    const int lines = spill_lines();
    expect("site: every call is in History", model.count() == polls && lines == polls);
    int sited = 0;
    for (int i = 0; i < model.count(); i++) {
        const QModelIndex idx = model.index(i);
        const qulonglong channel = 1000U + model.data(idx, CallHistoryModel::TgRole).toULongLong() - 36001U;
        const QString text = model.data(idx, CallHistoryModel::SiteTextRole).toString();
        sited += text.startsWith(QLatin1String("DMR ")) && text.contains(QString(63, QLatin1Char('N')))
                         && text.endsWith(QStringLiteral("LCN %1").arg(channel))
                     ? 1
                     : 0;
    }
    expect("site: each spilled row keeps its site and channel", sited == polls);
    expect("site: a row with the longest DMR texts stays within the spill's size", lines > 0 && bytes / lines <= 1536);
}

/* Issue #40: a Tier III site of the reporter's network (Large, raw NET 1, SITE 1) under @p text: the standard text or
 * the one naming Capacity Max carry the same raw parts. */
dsd_received_identity
t3_large_site(const char* text) {
    dsd_received_identity site = {};
    site.family = DSD_RXID_FAMILY_DMR;
    site.dmr_trunk = DSD_RXID_DMR_TRUNK_TIER3;
    site.valid = DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT;
    site.dmr_net = 1U;
    site.dmr_site = 1U;
    site.dmr_t3_model = 2U;
    DSD_SNPRINTF(site.dmr_site_text, sizeof(site.dmr_site_text), "%s", text);
    return site;
}

/* The site text digest each entry of the seen store records, by key (-1 when it records none). */
QHash<QString, qint64>
seen_digests(void) {
    QHash<QString, qint64> digests;
    for (const auto& value : dsd_qt::json_store_load_array("call_history_seen.json")) {
        const QJsonObject entry = value.toObject();
        digests.insert(entry.value(QLatin1String("key")).toString(),
                       entry.contains(QLatin1String("stx")) ? entry.value(QLatin1String("stx")).toInteger() : -1);
    }
    return digests;
}

/* A seen entry an older build stored has no site text digest. When the background tap is the first to read its row,
 * the digest it takes reaches the seen store through the spill, without the row read again: a text that changes alone
 * after that (the call's reacquired segment naming Capacity Max) re-reads the row at the next launch rather than being
 * taken silently again. */
void
test_tap_saves_a_settled_site_text_digest(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1754846000;
    const char standard[] = "TIII Large \xC2\xB7 Net 2 \xC2\xB7 Site 2 \xC2\xB7 SYS 2101";
    const char capmax[] = "TIII Large \xC2\xB7 Capacity Max \xC2\xB7 Net 1 \xC2\xB7 Site 1 \xC2\xB7 SYS 2101";
    decoder.commit(0, 37001U, 701U, when);
    decoder.siteNewest(0, t3_large_site(standard), DSD_CALL_CHANNEL_LPCN, 38U);
    decoder.publish();
    {
        CallHistoryModel model;
        gui_ingest(model);
        expect("digest: the call lands", model.count() == 1);
    } // destructor flushes the stores
    QJsonArray older = dsd_qt::json_store_load_array("call_history_seen.json");
    for (int i = 0; i < older.size(); i++) {
        QJsonObject entry = older.at(i).toObject();
        entry.remove(QLatin1String("stx"));
        older.replace(i, entry);
    }
    expect("digest: an older build's seen store saved",
           older.size() == 1 && dsd_qt::json_store_save_array("call_history_seen.json", older));

    {
        CallHistoryModel model;
        start_tap(model);
        set_app_state(Qt::ApplicationSuspended);
        model.waitForBackgroundPollForTest();
        expect("digest: only the background tap read the ring", model.count() == 1 && spill_lines() == 0);
    } // the tap stops with the model and writes what it holds
    expect("digest: the tap spilled the digest it took, and no row", spill_lines() == 1);

    /* The text changes alone while the app is down. */
    decoder.siteNewest(0, t3_large_site(capmax), DSD_CALL_CHANNEL_LPCN, 38U);
    decoder.state->event_history_s[0].commit_rev++;
    decoder.state->event_history_s[0].revision++;
    decoder.publish();
    set_app_state(Qt::ApplicationActive);
    CallHistoryModel relaunched;
    expect("digest: the relaunch logs nothing again", relaunched.count() == 1);
    relaunched.saveNow();
    settle_save(relaunched);
    const QHash<QString, qint64> digests = seen_digests();
    expect("digest: the seen store has the digest the tap took",
           digests.size() == 1 && digests.constBegin().value() >= 0);
    gui_ingest(relaunched);
    expect("digest: the text changed since is read",
           relaunched.count() == 1
               && relaunched.data(relaunched.index(0), CallHistoryModel::SiteTextRole).toString()
                      == QString::fromUtf8("DMR \xC2\xB7 TIII Large \xC2\xB7 Capacity Max \xC2\xB7 Net 1 \xC2\xB7 "
                                           "Site 1 \xC2\xB7 SYS 2101 \xC2\xB7 LPCN 38"));
}

/* A spill line as the tap writes one (CallHistoryModel::spillLine()) for a row of @p kind first read on @p slot of
 * ring 0x708 at push stamp @p seq, 4 s long; with @p again, the tap judged it heard again. */
QByteArray
spill_line_of(int kind, int slot, qulonglong seq, qint64 when, qulonglong tg, qint64 session, bool again) {
    const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(slot).arg(seq).arg(when).arg(tg).arg(kind);
    QJsonObject seen;
    seen.insert(QStringLiteral("when"), when);
    seen.insert(QStringLiteral("end"), when + 4);
    seen.insert(QStringLiteral("src"), 7);
    seen.insert(QStringLiteral("srcName"), QString());
    seen.insert(QStringLiteral("enc"), false);
    seen.insert(QStringLiteral("session"), session);
    seen.insert(QStringLiteral("ring"), QStringLiteral("708"));
    QJsonObject row;
    row.insert(QStringLiteral("when"), when);
    row.insert(QStringLiteral("name"), QString());
    row.insert(QStringLiteral("tg"), static_cast<qint64>(tg));
    row.insert(QStringLiteral("src"), 7);
    row.insert(QStringLiteral("durationSecs"), 4);
    row.insert(QStringLiteral("kind"), kind);
    row.insert(QStringLiteral("slot"), slot);
    row.insert(QStringLiteral("seq"), static_cast<qint64>(seq));
    row.insert(QStringLiteral("session"), session);
    row.insert(QStringLiteral("fr"), QJsonArray{QStringLiteral("0000000000000708:%1:%2").arg(slot).arg(seq)});
    QJsonObject obj;
    obj.insert(QStringLiteral("v"), 1);
    obj.insert(QStringLiteral("key"), key);
    obj.insert(QStringLiteral("seen"), seen);
    obj.insert(QStringLiteral("row"), row);
    if (again) {
        obj.insert(QStringLiteral("again"), true);
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

/* A voice call's spill line, read on slot 0. */
QByteArray
spill_line(qulonglong seq, qint64 when, qulonglong tg, qint64 session) {
    return spill_line_of(CallHistoryModel::KindVoice, 0, seq, when, tg, session, false);
}

bool
open_spill_for_writing(QFile& file) {
    file.setFileName(spill_path());
    return QDir().mkpath(QFileInfo(spill_path()).absolutePath()) && file.open(QIODevice::WriteOnly);
}

/* @p rows distinct calls, 30 s apart from @p when, talkgroups from @p tg up; with @p alternate the odd ones are a
 * second session's. */
bool
write_spill(int rows, qint64 when, qulonglong tg, bool alternate) {
    QFile file;
    bool written = open_spill_for_writing(file);
    for (int i = 0; written && i < rows; i++) {
        const qint64 session = alternate && i % 2 != 0 ? 2 : 1;
        const QByteArray line = spill_line(static_cast<qulonglong>(i) + 1U, when + 30 * static_cast<qint64>(i),
                                           tg + static_cast<qulonglong>(i), session);
        written = file.write(line) == line.size();
    }
    return written;
}

/* A long stretch in the background spills far more calls than History keeps (1,000 rows). The fold keeps the newest
 * 1,000 and never holds the whole backlog: it trims as it goes, each trim one pass over the log. Folding 50,000 rows
 * into the log and then trimming them one at a time, each a scan of the log, blocked the GUI thread for seconds. */
void
test_long_spill_folds_bounded(void) {
    resetStorage();
    const int spilled = 50000;
    const qulonglong tg = 100000U;
    expect("the long spill is written", write_spill(spilled, 1754900000, tg, false));
    CallHistoryModel model;
    expect("the fold keeps the newest 1,000 calls",
           model.count() == 1000 && rows_in_tg_range(model, tg + spilled - 1000, tg + spilled - 1) == 1000);
    expect("the log never grows far past its capacity while it folds", model.peakRowsForTest() <= 1500);
    expect("the trims' work is linear in the spill", model.trimRowsScannedForTest() <= qint64{10} * spilled);
}

/* The trims keep the oldest session's oldest rows as the first to go, as the log always has: a fold spanning two
 * sessions keeps the newer session's newest rows wherever they sort in the list. */
void
test_spill_fold_trims_oldest_session_first(void) {
    resetStorage();
    const int spilled = 5000;
    const qulonglong tg = 200000U;
    expect("the two-session spill is written", write_spill(spilled, 1754900000, tg, true));
    CallHistoryModel model;
    bool newest = model.count() == 1000;
    for (int i = 0; i < model.count(); i++) {
        const QModelIndex idx = model.index(i);
        const qulonglong rowTg = model.data(idx, CallHistoryModel::TgRole).toULongLong();
        newest = newest && model.data(idx, CallHistoryModel::SessionRole).toLongLong() == 2
                 && rowTg >= tg + spilled - 2000 && (rowTg - tg) % 2 != 0;
    }
    expect("the newer session's newest 1,000 calls stay", newest);
}

/* The fold reads the spill a line at a time and prunes its seen entries as it trims its rows, so neither the reader
 * nor the seen map holds the whole backlog. Read whole and split into lines, a 500,000-row spill (149 MB) took the
 * fold past 500 MB; its seen map held an entry a row until the fold ended. The seen map's bound is the one it always
 * had, twice its 2,048-entry store, and the line that passes it; the reader holds one line, at most a megabyte, and a
 * chunk. A line longer than that, which no build writes, and a torn last line are passed over. */
void
test_very_long_spill_folds_in_bounded_memory(void) {
    resetStorage();
    const int spilled = 200000;
    const qint64 when = 1754900000;
    const qulonglong tg = 300000U;
    QFile file;
    bool written = open_spill_for_writing(file);
    for (int i = 0; written && i < spilled; i++) {
        if (i == spilled - 500) {
            const QByteArray tooLong =
                "{\"v\":1,\"key\":\"" + QByteArray(2 * dsd_qt::CallHistorySpillReader::kMaxLineBytes, 'x') + "\"}\n";
            written = file.write(tooLong) == tooLong.size();
        }
        const QByteArray line = spill_line(static_cast<qulonglong>(i) + 1U, when + 30 * static_cast<qint64>(i),
                                           tg + static_cast<qulonglong>(i), 1);
        written = written && file.write(line) == line.size();
    }
    const QByteArray torn = spill_line(static_cast<qulonglong>(spilled) + 1U, when + 30 * qint64{spilled},
                                       tg + static_cast<qulonglong>(spilled), 1);
    written = written && file.write(torn.left(torn.size() / 2)) == torn.size() / 2;
    file.close();
    expect("the very long spill is written", written);
    CallHistoryModel model;
    expect("the very long spill keeps the newest 1,000 calls, past the over-long line and not the torn one",
           model.count() == 1000 && rows_in_tg_range(model, tg + spilled - 1000, tg + spilled - 1) == 1000);
    expect("the seen map never holds the backlog while it folds", model.peakFoldSeenForTest() <= 4097);
    expect("the reader holds one line at most",
           model.peakSpillReadBytesForTest() <= dsd_qt::CallHistorySpillReader::kMaxLineBytes + qsizetype{32} * 1024);
}

/* A notice heard again takes the logged one as its twin. The twins taken are pruned with the seen entries that record
 * them, so a long spill of notices each heard again leaves each in History once and never holds a twin a line. */
void
test_long_spill_of_repeated_notices_folds_bounded(void) {
    resetStorage();
    const int notices = 20000;
    const qint64 when = 1754900000;
    const qulonglong tg = 400000U;
    QFile file;
    bool written = open_spill_for_writing(file);
    for (int i = 0; written && i < notices; i++) {
        const qint64 at = when + 30 * static_cast<qint64>(i);
        const qulonglong seq = static_cast<qulonglong>(i) + 1U;
        const QByteArray first = spill_line_of(CallHistoryModel::KindNotice, 0, seq, at, tg + seq, 1, false);
        const QByteArray again = spill_line_of(CallHistoryModel::KindNotice, 1, seq, at, tg + seq, 1, true);
        written = file.write(first) == first.size() && file.write(again) == again.size();
    }
    file.close();
    expect("the notice spill is written", written);
    CallHistoryModel model;
    expect("each notice heard again is in History once, the newest 1,000",
           model.count() == 1000 && rows_in_tg_range(model, tg + notices - 999, tg + notices) == 1000);
    expect("the seen map never holds the notice backlog", model.peakFoldSeenForTest() <= 4097);
    expect("the twins taken never hold the notice backlog", model.peakFoldTwinsForTest() <= 4097);
    expect("the reader holds one short line and a chunk", model.peakSpillReadBytesForTest() <= qsizetype{32} * 1024);
}

/* A read error partway through the spill (an I/O error) used to end the read as its end would, and the whole spill
 * went at the next acknowledged save, the rows after the error with it. The fold keeps what it read before the error
 * and takes none of the spill as read: the save keeps it whole, and the next fold reads it again, adding only what the
 * failed one missed. */
void
test_failed_spill_read_keeps_the_spill(void) {
    resetStorage();
    const int spilled = 500;
    const qulonglong tg = 500000U;
    expect("read error: the spill is written", write_spill(spilled, 1754950000, tg, false));
    const qint64 size = spill_size();
    g_spillReadWarnings = 0;
    CallHistoryModel::failNextSpillReadAfterLinesForTest(120);
    {
        CallHistoryModel model;
        expect("read error: the rows before it fold",
               model.count() == 120 && rows_in_tg_range(model, tg, tg + 119) == 120);
        expect("read error: it is reported", g_spillReadWarnings == 1);
        model.saveNow();
        settle_save(model);
        expect("read error: an acknowledged save keeps the spill", !model.saveFailing() && spill_size() == size);
    }
    expect("read error: and so does the teardown", spill_size() == size);
    {
        CallHistoryModel retried;
        expect("read error: the next fold adds the rest",
               retried.count() == spilled && rows_in_tg_range(retried, tg, tg + spilled - 1) == spilled);
        retried.saveNow();
        settle_save(retried);
        expect("read error: the spill goes once a fold read it whole", spill_size() == 0);
    }
    CallHistoryModel last;
    expect("read error: the log has every call once",
           last.count() == spilled && rows_in_tg_range(last, tg, tg + spilled - 1) == spilled);
    expect("read error: reported only for the failed read", g_spillReadWarnings == 1);
}

/* Issue #800, 2.7. The tap reads a run's two fragments of one call in the background (fixes with revs 5 and 9) and the
 * process dies: its settings keep only what was saved when the tap started. The relaunch's fold of the spill fails, a
 * newer run's fragment of the same call (rev 2) is ingested live, and the next fold adds the older run's two. The
 * merged call shows the newer run's fix: the tap's ring took an ordinal from the block set aside before it ran. */
void
test_failed_spill_read_after_a_newer_run(void) {
    resetStorage();
    const time_t when = 1755000000;
    const uint32_t tg = 80701U;
    const uint32_t src = 8701U;
    const QStringList ordinalKeys = {QStringLiteral("callHistory/sessionOrd/next"),
                                     QStringLiteral("callHistory/sessionOrd/rings")};
    QVariantMap atTapStart;
    {
        Decoder first;
        CallHistoryModel model;
        start_tap(model);
        set_app_state(Qt::ApplicationSuspended);
        {
            QSettings settings;
            for (const QString& key : ordinalKeys) {
                atTapStart.insert(key, settings.value(key));
            }
        }
        // Every fragment is heard on one known reception: two runs' epochs say nothing of each other, so only that
        // lets one run's fix onto a row another run's fragment holds (call_history_same_reception()).
        first.commit(0, tg, src, when);
        first.heardNewest(0);
        first.gpsNewest(0, "GPS: A", 225000000, 5U);
        first.commit(0, tg, src, when + 6);
        first.heardNewest(0);
        first.gpsNewest(0, "GPS: B", 226000000, 9U);
        first.publish();
        for (int poll = 0; poll < 6; poll++) {
            model.waitForBackgroundPollForTest();
        }
        expect("2.7: the older run's fragments are spilled", spill_lines() == 2);
    }
    // What a process killed in the background leaves: the settings as they were when the tap started.
    {
        QSettings settings;
        for (const QString& key : ordinalKeys) {
            if (atTapStart.value(key).isValid()) {
                settings.setValue(key, atTapStart.value(key));
            } else {
                settings.remove(key);
            }
        }
    }
    set_app_state(Qt::ApplicationActive);
    g_spillReadWarnings = 0;
    CallHistoryModel::failNextSpillReadAfterLinesForTest(0);
    {
        Decoder second;
        CallHistoryModel model;
        expect("2.7: the failed fold adds nothing", model.count() == 0 && g_spillReadWarnings == 1);
        second.commit(0, tg, src, when + 12);
        second.heardNewest(0);
        second.gpsNewest(0, "GPS: C", 227000000, 2U);
        second.publish();
        gui_ingest(model);
        expect("2.7: the newer run's fragment is logged",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::DetailRole).toString() == QStringLiteral("GPS: C"));
    }
    expect("2.7: the spill waits for a whole read", spill_lines() == 2);
    {
        CallHistoryModel retried;
        expect("2.7: the retried fold joins the older run's fragments to the call", retried.count() == 1);
        expect("2.7: the newer run's fix wins over the older run's higher revs",
               retried.count() == 1
                   && retried.data(retried.index(0), CallHistoryModel::DetailRole).toString()
                          == QStringLiteral("GPS: C"));
        retried.saveNow();
        settle_save(retried);
    }
    CallHistoryModel last;
    expect("2.7: a reload keeps the newer run's fix",
           last.count() == 1
               && last.data(last.index(0), CallHistoryModel::DetailRole).toString() == QStringLiteral("GPS: C"));
}

/* Issue #800, 2.7: the core withdraws a committed fragment's fix (its GPS text and position cleared) while the app is
 * away. The tap spills that reading with no GPS to carry its run and version; folded on the return, or by the relaunch
 * after the process died in the background, it is still its fragment's reading and withdraws the fix, as the same
 * reading read live does: the old fix neither shows nor is saved. */
void
test_background_gps_withdrawal(void) {
    const time_t when = 1755100000;
    for (int death = 0; death < 2; death++) {
        resetStorage();
        Decoder decoder;
        {
            CallHistoryModel model;
            start_tap(model);
            decoder.commit(0, 80801U, 8801U, when);
            decoder.gpsNewest(0, "GPS: withdrawn", 225000000, 5U);
            decoder.publish();
            gui_ingest(model);
            expect("withdrawal: the fix shows before the suspend",
                   model.count() == 1
                       && model.data(model.index(0), CallHistoryModel::DetailRole).toString()
                              == QStringLiteral("GPS: withdrawn"));
            set_app_state(Qt::ApplicationSuspended);
            decoder.clearGpsNewest(0);
            decoder.publish();
            for (int poll = 0; poll < 6; poll++) {
                model.waitForBackgroundPollForTest();
            }
            expect("withdrawal: the tap spills the cleared reading", spill_lines() == 1);
            if (death == 0) {
                set_app_state(Qt::ApplicationActive);
                gui_ingest(model);
                expect("withdrawal: the return takes the fix back",
                       model.count() == 1
                           && model.data(model.index(0), CallHistoryModel::DetailRole).toString().isEmpty());
                model.saveNow();
                settle_save(model);
            }
        } // with death: the process dies in the background, and the relaunch folds the spill
        set_app_state(Qt::ApplicationActive);
        CallHistoryModel relaunched;
        expect(death == 0 ? "withdrawal: the saved row keeps no fix" : "withdrawal: the relaunch's fold takes it back",
               relaunched.count() == 1
                   && relaunched.data(relaunched.index(0), CallHistoryModel::DetailRole).toString().isEmpty());
    }
}

/* The row with talkgroup @p tg, or -1. */
int
row_of_tg(const CallHistoryModel& model, qulonglong tg) {
    for (int i = 0; i < model.count(); i++) {
        if (model.data(model.index(i), CallHistoryModel::TgRole).toULongLong() == tg) {
            return i;
        }
    }
    return -1;
}

/* Whether the row with talkgroup @p tg carries @p session, @p label and @p uid. */
bool
row_attributed(const CallHistoryModel& model, qulonglong tg, qint64 session, const QString& label, const QString& uid) {
    const int row = row_of_tg(model, tg);
    if (row < 0) {
        return false;
    }
    const QModelIndex index = model.index(row);
    return model.data(index, CallHistoryModel::SessionRole).toLongLong() == session
           && model.data(index, CallHistoryModel::SystemNameRole).toString() == label
           && model.data(index, CallHistoryModel::SystemUidRole).toString() == uid;
}

/* A start that completes while the app is away (a deferred restart, or a USB start's continuation while hidden)
 * changes the session the running tap attributes its rows to. The tap hands off what it found under the old session
 * first, and starts again on the new one while the app is still away; the ingest after the return finds the new
 * session's calls attributed to it, not seen already under the old one. */
void
test_session_change_while_hidden(void) {
    resetStorage();
    const QString oldLabel = QStringLiteral("Old source");
    const QString oldUid = QStringLiteral("old-source-uid");
    const QString newLabel = QStringLiteral("New source");
    const QString newUid = QStringLiteral("new-source-uid");
    CallHistoryModel model;
    start_tap(model);
    model.setSessionLabel(oldLabel);
    model.setSessionUid(oldUid);
    model.beginSession();
    const qint64 oldSession = model.session();
    Decoder oldDecoder;
    oldDecoder.commit(0, 33000U, 99U, 1754960000);
    oldDecoder.publish();
    gui_ingest(model);

    set_app_state(Qt::ApplicationHidden);
    expect("session change: the tap runs while hidden", model.backgroundCaptureRunningForTest());
    oldDecoder.commit(0, 33001U, 99U, 1754960060);
    oldDecoder.publish();
    model.waitForBackgroundPollForTest();
    // Main.startWithMap after an accepted asynchronous start: an ingest, then the new session.
    gui_ingest(model);
    model.setSessionLabel(newLabel);
    model.setSessionUid(newUid);
    model.beginSession();
    const qint64 newSession = model.session();
    expect("session change: the tap still runs while hidden", model.backgroundCaptureRunningForTest());

    set_app_state(Qt::ApplicationSuspended);
    Decoder newDecoder;
    newDecoder.commit(0, 33002U, 100U, 1754960100);
    newDecoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);

    expect("session change: every call is in History once", model.count() == 3);
    expect("session change: the call before the hide keeps the old session",
           row_attributed(model, 33000U, oldSession, oldLabel, oldUid));
    expect("session change: the old session's call the tap found keeps it",
           row_attributed(model, 33001U, oldSession, oldLabel, oldUid));
    expect("session change: the new session's call is attributed to it",
           row_attributed(model, 33002U, newSession, newLabel, newUid));
}

/* A session change while hidden moves its fields together: Main.startWithMap() begins the session on the new system,
 * and an explore detach moves the system, each in one call. A call the new decoder commits as soon as the change is
 * heard of (the first field's signal), and that the running tap polls then, is attributed wholly to the new session:
 * the tap never runs on half of the change. */
void
test_session_fields_change_together_while_hidden(void) {
    resetStorage();
    const QString oldLabel = QStringLiteral("Old system");
    const QString oldUid = QStringLiteral("old-system-uid");
    const QString newLabel = QStringLiteral("New system");
    const QString newUid = QStringLiteral("new-system-uid");
    const QString exploring = QStringLiteral("Exploring");
    CallHistoryModel model;
    start_tap(model);
    model.beginSessionOn(oldLabel, oldUid);
    const qint64 oldSession = model.session();
    Decoder oldDecoder;
    oldDecoder.commit(0, 34000U, 99U, 1754970000);
    oldDecoder.publish();
    gui_ingest(model);

    set_app_state(Qt::ApplicationHidden);
    expect("fields together: the tap runs while hidden", model.backgroundCaptureRunningForTest());
    Decoder newDecoder;
    uint32_t heardTg = 0U;
    time_t heardStart = 1754970060;
    QObject::connect(&model, &CallHistoryModel::sessionLabelChanged, &model, [&] {
        if (heardTg == 0U) {
            return;
        }
        newDecoder.commit(0, heardTg, 100U, heardStart);
        newDecoder.publish();
        model.waitForBackgroundPollForTest();
        heardTg = 0U;
    });

    // Main.startWithMap after an accepted asynchronous start: an ingest, then the session on the new system.
    gui_ingest(model);
    heardTg = 34001U;
    model.beginSessionOn(newLabel, newUid);
    const qint64 newSession = model.session();
    expect("fields together: the start's call was committed", heardTg == 0U);
    expect("fields together: the tap still runs after the start", model.backgroundCaptureRunningForTest());

    // The session detaches from its saved system (Main's explore detach): an ingest, then the system it now is.
    gui_ingest(model);
    heardTg = 34002U;
    heardStart = 1754970120;
    model.setSessionSystem(exploring, QString());
    expect("fields together: the detach's call was committed", heardTg == 0U);
    expect("fields together: the detach keeps the session", model.session() == newSession);
    expect("fields together: the tap still runs after the detach", model.backgroundCaptureRunningForTest());

    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);

    expect("fields together: every call is in History once", model.count() == 3);
    expect("fields together: the old session's call keeps it",
           row_attributed(model, 34000U, oldSession, oldLabel, oldUid));
    expect("fields together: the call heard at the start is wholly the new session's",
           row_attributed(model, 34001U, newSession, newLabel, newUid));
    expect("fields together: the call heard at the detach is wholly the detached system's",
           row_attributed(model, 34002U, newSession, exploring, QString()));
}

/* The tap attributes each row by its own tuner-owner stamp (issue #671), as the GUI's refresh() does, never by the scan
 * flags in force when it reads the ring: a plain call heard before a scan started keeps the saved system's UID even
 * when the tap reads it under the scan, a call heard under the scan gains none, and a call heard while the operator
 * held the tuner is Exploring. */
void
test_tap_attributes_by_row_stamp(void) {
    resetStorage();
    const QString label = QStringLiteral("Saved system");
    const QString uid = QStringLiteral("saved-system-uid");
    CallHistoryModel model;
    start_tap(model);
    model.beginSessionOn(label, uid);
    const qint64 session = model.session();
    Decoder decoder;
    gui_ingest(model);

    set_app_state(Qt::ApplicationHidden);
    expect("row stamp: the tap runs while hidden", model.backgroundCaptureRunningForTest());
    decoder.commit(0, 35000U, 99U, 1754980000);
    // A scan starts (a saved system's extra arguments) before the tap reads the ring.
    decoder.opts->scanner_mode = 1;
    dsd_app_telemetry_publish_opts_snapshot(decoder.opts.get());
    decoder.commit(0, 35001U, 99U, 1754980060);
    decoder.state->event_history_s[0].Event_History_Items[1].scan_member = 1U;
    decoder.commit(0, 35002U, 99U, 1754980120);
    decoder.state->event_history_s[0].Event_History_Items[1].operator_tuned = 1U;
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);

    expect("row stamp: every call is in History once", model.count() == 3);
    expect("row stamp: a call heard before the scan keeps the saved system's identity",
           row_attributed(model, 35000U, session, label, uid));
    expect("row stamp: a call heard under the scan gains no saved-system UID",
           row_attributed(model, 35001U, session, label, QString()));
    expect("row stamp: a call heard while the operator held the tuner is Exploring",
           row_attributed(model, 35002U, session, QStringLiteral("Exploring"), QString()));
}

/* Issue #903: what History hands the remembered talkgroups (HeardTalkgroupSink), recorded. */
class RecordingHeardSink : public dsd_qt::HeardTalkgroupSink {
  public:
    struct Observation {
        QString owner;
        quint32 id = 0;
        qint64 when = 0;
    };

    QList<Observation> seen;

    void
    observe(const QString& ownerUid, int variant, quint32 id, qint64 whenSecs) override {
        (void)variant;
        seen.append(Observation{ownerUid, id, whenSecs});
    }

    /** How many observations of @p id under @p owner. */
    int
    count(const QString& owner, quint32 id) const {
        return static_cast<int>(std::count_if(seen.cbegin(), seen.cend(),
                                              [&](const Observation& o) { return o.owner == owner && o.id == id; }));
    }
};

/* The proof the core freezes on the newest committed row of @p slot (issue #903): ordinary talkgroup @p id, verified,
 * heard on live input unless @p live is false. */
void
prove_newest(Decoder& decoder, int slot, uint32_t id, bool live = true) {
    Event_History* item = &decoder.state->event_history_s[slot].Event_History_Items[1];
    item->heard_variant = 1U;
    item->heard_id = id;
    item->live_input = live ? 1U : 0U;
}

/* Main.startWithMap's accepted start on saved system @p uid: the session with its owner, in one call. */
void
start_saved(CallHistoryModel& model, const QString& uid) {
    model.beginSessionWithOwners(uid, uid, uid, {});
}

/* Issue #903 (r3 finding 1): a suspended phone spills 1,001 distinct verified groups, more than History's 1,000 rows.
 * Every one reaches the remembered talkgroups, whether the app returns and hands off or the process dies and the
 * relaunch folds the spill at construction (held until the sink is set): fed before History trims. */
void
test_heard_long_spill_reaches_the_sink_before_the_trim(void) {
    for (int death = 0; death < 2; death++) {
        resetStorage();
        Decoder decoder;
        const time_t when = 1755100000 + death * 100000;
        RecordingHeardSink sink;
        {
            CallHistoryModel model;
            start_tap(model);
            model.setHeardSink(&sink);
            start_saved(model, QStringLiteral("sys-a"));
            gui_ingest(model);
            set_app_state(Qt::ApplicationSuspended);
            for (int done = 0; done < 1001; done += 200) {
                const int batch = qMin(200, 1001 - done);
                for (int i = 0; i < batch; i++) {
                    const uint32_t tg = 42000U + static_cast<uint32_t>(done + i);
                    decoder.commit(0, tg, 100U, when + 30 * static_cast<time_t>(done + i));
                    prove_newest(decoder, 0, tg);
                }
                decoder.publish();
                model.waitForBackgroundPollForTest();
            }
            expect("long spill: nothing is handed on while the app is away", sink.seen.isEmpty());
            if (death == 0) {
                set_app_state(Qt::ApplicationActive);
                gui_ingest(model);
                expect("long spill: the handoff trims History to its bound", model.count() == 1000);
            }
        } // death: the tap's stop writes what it holds, and nothing folds it
        if (death == 1) {
            expect("long spill: the dead process handed nothing on", sink.seen.isEmpty());
            CallHistoryModel relaunched;
            expect("long spill: the relaunch trims History to its bound", relaunched.count() == 1000);
            relaunched.setHeardSink(&sink);
        }
        int each = 0;
        for (uint32_t tg = 42000U; tg <= 43000U; tg++) {
            each += sink.count(QStringLiteral("sys-a"), tg) == 1;
        }
        expect(death == 0 ? "long spill: the handoff hands on all 1,001 groups, each once"
                          : "long spill: the relaunch hands on all 1,001 groups, each once",
               sink.seen.size() == 1001 && each == 1001);
    }
}

/* Issue #903 (r3 finding 8): the last call of system A, flushed while B starts in the background, lands under A: the
 * owners are bound to the ring the tap read, not to the session the UI moved on to. A's final publish after B's start
 * is A's too. */
void
test_heard_last_call_of_a_during_b_start(void) {
    resetStorage();
    RecordingHeardSink sink;
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&sink);
    start_saved(model, QStringLiteral("sys-a"));
    const time_t when = 1755300000;
    Decoder a;
    a.commit(0, 43100U, 99U, when);
    prove_newest(a, 0, 43100U);
    a.publish();
    gui_ingest(model);
    set_app_state(Qt::ApplicationHidden);
    a.commit(0, 43101U, 99U, when + 60);
    prove_newest(a, 0, 43101U);
    a.publish();
    // A start that completes while hidden: the new session, then its owners.
    start_saved(model, QStringLiteral("sys-b"));
    Decoder b;
    b.commit(0, 43102U, 100U, when + 120);
    prove_newest(b, 0, 43102U);
    b.publish();
    model.waitForBackgroundPollForTest();
    a.commit(0, 43103U, 99U, when + 180);
    prove_newest(a, 0, 43103U);
    a.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("A during B: A's calls are A's, the one flushed at B's start and A's late one included",
           sink.count(QStringLiteral("sys-a"), 43100U) == 1 && sink.count(QStringLiteral("sys-a"), 43101U) == 1
               && sink.count(QStringLiteral("sys-a"), 43103U) == 1);
    expect("A during B: B's call is B's", sink.count(QStringLiteral("sys-b"), 43102U) == 1);
    expect("A during B: nothing else", sink.seen.size() == 4);
}

/* Issue #903 (r3 finding 8): through the background tap, a scan target id an imported target file also gives (left out
 * of the owner map), a running switch from live input to a file, a call the operator tuned to, and (PR2 review r4) one
 * heard after a live retune moved the run's tuning learn nothing. */
void
test_heard_tap_learns_only_owned_live_calls(void) {
    resetStorage();
    RecordingHeardSink sink;
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&sink);
    model.beginSessionOn(QStringLiteral("County scan"), QString());
    model.setSessionOwners(model.session(), QString(), {{QStringLiteral("entry-a"), QStringLiteral("sys-a")}});
    const time_t when = 1755400000;
    Decoder decoder;
    gui_ingest(model);
    set_app_state(Qt::ApplicationSuspended);
    const auto scanCall = [&](uint32_t tg, const char* target, time_t start, bool live) {
        decoder.commit(0, tg, 99U, start);
        prove_newest(decoder, 0, tg, live);
        Event_History* item = &decoder.state->event_history_s[0].Event_History_Items[1];
        item->scan_member = 1U;
        DSD_SNPRINTF(item->channel_label, sizeof(item->channel_label), "%s", target);
    };
    scanCall(43200U, "entry-a", when, true);
    scanCall(43201U, "entry-shared", when + 60, true);
    // The running decoder switches to a file: every row after it is off live input.
    scanCall(43202U, "entry-a", when + 120, false);
    scanCall(43203U, "entry-a", when + 180, true);
    decoder.state->event_history_s[0].Event_History_Items[1].operator_tuned = 1U;
    scanCall(43204U, "entry-a", when + 240, true);
    decoder.state->event_history_s[0].Event_History_Items[1].tuning_moved = 1U;
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("tap learning: every call is in History", model.count() == 5);
    expect("tap learning: only the owned target's live call is handed on",
           sink.seen.size() == 1 && sink.count(QStringLiteral("sys-a"), 43200U) == 1);
}

/* Issue #903 (PR2 review r4, P1): a saved-system session whose tuning a live retune moved (a configuration from the
 * control API, with no operator hold) learns nothing through the background tap either: the call heard before the move
 * is the saved system's, the one after it no one's. */
void
test_heard_tap_after_a_live_retune(void) {
    resetStorage();
    RecordingHeardSink sink;
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&sink);
    start_saved(model, QStringLiteral("sys-a"));
    const time_t when = 1755450000;
    Decoder decoder;
    gui_ingest(model);
    set_app_state(Qt::ApplicationSuspended);
    decoder.commit(0, 43300U, 99U, when);
    prove_newest(decoder, 0, 43300U);
    decoder.commit(0, 43301U, 99U, when + 60);
    prove_newest(decoder, 0, 43301U);
    decoder.state->event_history_s[0].Event_History_Items[1].tuning_moved = 1U;
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("tap retune: both calls are in History", model.count() == 2);
    expect("tap retune: only the call before the move is the saved system's",
           sink.seen.size() == 1 && sink.count(QStringLiteral("sys-a"), 43300U) == 1);
}

/* Run the event loop's posted work a few times, as the turns after Main's start would. */
void
spin_events(void) {
    for (int i = 0; i < 3; i++) {
        QCoreApplication::processEvents();
    }
}

/* Issue #903 (re-review): Main holds History's background capture from before it asks the host for a start
 * (holdCaptureForStart()) until the new session begins. A start accepted while the app is away publishes the new run's
 * ring at once, and no tap poll runs in between to bind it to the old session: B's call is B's, A's is A's. A start
 * that never begins a session releases the hold, by hand or by the next event-loop turn, and the tap runs again. */
void
test_heard_hold_for_start(void) {
    resetStorage();
    RecordingHeardSink sink;
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&sink);
    start_saved(model, QStringLiteral("sys-a"));
    const time_t when = 1755700000;
    Decoder a;
    a.commit(0, 44100U, 99U, when);
    prove_newest(a, 0, 44100U);
    a.publish();
    gui_ingest(model);
    set_app_state(Qt::ApplicationHidden);
    model.waitForBackgroundPollForTest();
    // Main.startWithMap: the hold and the flush before the host is asked, the host's accept (B's ring published at
    // once), the flush after it, then the session with its owners, all in one turn.
    model.holdCaptureForStart();
    expect("hold: the tap is stopped", !model.backgroundCaptureRunningForTest());
    gui_ingest(model);
    Decoder b;
    model.refresh(dsd_app_get_latest_snapshot(), dsd_app_get_latest_opts_snapshot(), false);
    expect("hold: the tap stays stopped through the accept", !model.backgroundCaptureRunningForTest());
    start_saved(model, QStringLiteral("sys-b"));
    expect("hold: the new session starts the tap", model.backgroundCaptureRunningForTest());
    b.commit(0, 44101U, 100U, when + 60);
    prove_newest(b, 0, 44101U);
    b.publish();
    model.waitForBackgroundPollForTest();
    spin_events();
    expect("hold: the safety release leaves the begun session's tap running", model.backgroundCaptureRunningForTest());
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("hold: A's call is A's", sink.count(QStringLiteral("sys-a"), 44100U) == 1);
    expect("hold: B's call is B's", sink.count(QStringLiteral("sys-b"), 44101U) == 1 && sink.seen.size() == 2);

    // Starts that never begin a session.
    set_app_state(Qt::ApplicationHidden);
    const qint64 session = model.session();
    model.holdCaptureForStart();
    expect("abandoned: the hold stops the tap", !model.backgroundCaptureRunningForTest());
    model.releaseCaptureHold();
    expect("abandoned: the release starts it again", model.backgroundCaptureRunningForTest());
    model.holdCaptureForStart();
    spin_events();
    expect("abandoned: the next turn releases a hold nothing consumed", model.backgroundCaptureRunningForTest());
    expect("abandoned: the session is unchanged", model.session() == session);
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    model.holdCaptureForStart();
    spin_events();
    expect("abandoned: back on screen, the release leaves the tap stopped", !model.backgroundCaptureRunningForTest());
}

/* Issue #903 (review, Minor 1): a Clear while the tap runs (the app hidden, its loop running) hands the tap off first,
 * so the verified talkgroups it found still reach the remembered list; the rows themselves are cleared. */
void
test_heard_clear_while_the_tap_runs(void) {
    resetStorage();
    RecordingHeardSink sink;
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&sink);
    start_saved(model, QStringLiteral("sys-a"));
    const time_t when = 1755800000;
    Decoder decoder;
    gui_ingest(model);
    set_app_state(Qt::ApplicationHidden);
    decoder.commit(0, 44200U, 99U, when);
    prove_newest(decoder, 0, 44200U);
    decoder.publish();
    model.waitForBackgroundPollForTest();
    model.clearAll();
    expect("clear while away: the tap's group is learnt", sink.count(QStringLiteral("sys-a"), 44200U) == 1);
    expect("clear while away: the log is empty and the tap runs again",
           model.count() == 0 && model.backgroundCaptureRunningForTest());
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("clear while away: nothing comes back, nothing twice", model.count() == 0 && sink.seen.size() == 1);
}

/* Issue #903 (review, Minor 4): a proof that arrives while the app is away, on a row History's Clear covers, reaches
 * the remembered list through the spill (clearedProofLine()), once, and the row stays cleared. */
void
test_heard_cleared_row_proof_through_the_tap(void) {
    resetStorage();
    RecordingHeardSink sink;
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&sink);
    start_saved(model, QStringLiteral("sys-a"));
    const time_t when = 1755900000;
    Decoder decoder;
    decoder.commit(0, 44300U, 99U, when);
    decoder.state->event_history_s[0].Event_History_Items[1].live_input = 1U;
    decoder.publish();
    gui_ingest(model);
    model.clearAll();
    set_app_state(Qt::ApplicationSuspended);
    prove_newest(decoder, 0, 44300U);
    decoder.state->event_history_s[0].commit_rev++;
    decoder.state->event_history_s[0].revision++;
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("cleared through the tap: the late proof is learnt once",
           sink.seen.size() == 1 && sink.count(QStringLiteral("sys-a"), 44300U) == 1);
    expect("cleared through the tap: the row stays cleared", model.count() == 0);
}

/* Copy every file under @p from into @p to, keeping the folders (a process's disk at one instant). */
void
copy_tree(const QString& from, const QString& to) {
    const QDir source(from);
    QDir().mkpath(to);
    for (const QFileInfo& info : source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString target = QDir(to).filePath(info.fileName());
        if (info.isDir()) {
            copy_tree(info.absoluteFilePath(), target);
        } else {
            QFile::copy(info.absoluteFilePath(), target);
        }
    }
}

/* Issue #903 (ruling on the refold window): a Forget after the handoff folded the spill, then the process dies before
 * History's own save: the relaunch refolds nothing the Forget removed, because the Forget first had History write its
 * log, which acknowledges and empties the spill (CallHistoryModel::settleSpill(), as qt_ui.cpp wires it). The death is
 * the app data folder as it was right after the Forget's write, put back after the process's teardown wrote more. */
void
test_heard_forget_after_a_fold_survives_a_death(void) {
    resetStorage();
    const QString name = QString::fromLatin1(dsd_qt::HeardTalkgroupStore::kFileName);
    const QString owner = QStringLiteral("sys-a");
    const auto allow = [](const QString& uid) { return uid == QStringLiteral("sys-a"); };
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QTemporaryDir atDeath;
    expect("forget after fold: a folder for the disk at death", atDeath.isValid());
    Decoder decoder;
    const time_t when = 1755600000;
    {
        dsd_qt::HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow);
        CallHistoryModel model;
        start_tap(model);
        model.setHeardSink(&store);
        store.setBeforeEdit(model.spillSettler());
        start_saved(model, owner);
        gui_ingest(model);
        set_app_state(Qt::ApplicationSuspended);
        decoder.commit(0, 44000U, 99U, when);
        prove_newest(decoder, 0, 44000U);
        decoder.commit(0, 44001U, 99U, when + 60);
        prove_newest(decoder, 0, 44001U);
        decoder.publish();
        model.waitForBackgroundPollForTest();
        set_app_state(Qt::ApplicationActive);
        gui_ingest(model);
        expect("forget after fold: the fold taught both groups", store.entriesFor(owner).size() == 2);
        expect("forget after fold: the folded spill waits for History's save", spill_lines() == 2);
        store.forget(owner, 44000U);
        store.waitForWrites();
        model.waitForSaveForTest();
        expect("forget after fold: the forget emptied the spill first", spill_size() == 0);
        // The process dies here, before History's own (debounced) save.
        copy_tree(dataDir, atDeath.path());
    }
    QDir(dataDir).removeRecursively();
    copy_tree(atDeath.path(), dataDir);
    dsd_qt::HeardTalkgroupStore relaunchedStore(name);
    relaunchedStore.setOwnerPolicy(allow);
    CallHistoryModel relaunched;
    relaunched.setHeardSink(&relaunchedStore);
    gui_ingest(relaunched);
    const QVector<dsd_qt::HeardTalkgroupStore::Entry> left = relaunchedStore.entriesFor(owner);
    expect("forget after fold: the forgotten group does not come back", left.size() == 1 && left.at(0).id == 44001U);
    expect("forget after fold: History kept both calls", relaunched.count() == 2);
}

/* Issue #903 (PR2 review r3, P2): a spill whose stat fails for any reason but its absence (here a symlink loop, ELOOP)
 * is not an empty one. A Forget or Clear while History cannot check its spill is refused, as when it cannot save, so a
 * fold once the spill reads again restores nothing the user saw go; once the spill checks again, the Forget goes
 * through. */
void
test_heard_forget_refused_while_the_spill_cannot_be_checked(void) {
#ifndef _WIN32
    resetStorage();
    const QString name = QString::fromLatin1(dsd_qt::HeardTalkgroupStore::kFileName);
    const QString owner = QStringLiteral("sys-a");
    const auto allow = [](const QString& uid) { return uid == QStringLiteral("sys-a"); };
    Decoder decoder;
    const time_t when = 1755620000;
    dsd_qt::HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow);
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&store);
    store.setBeforeEdit(model.spillSettler());
    start_saved(model, owner);
    gui_ingest(model);
    set_app_state(Qt::ApplicationSuspended);
    decoder.commit(0, 44700U, 99U, when);
    prove_newest(decoder, 0, 44700U);
    decoder.commit(0, 44701U, 99U, when + 60);
    prove_newest(decoder, 0, 44701U);
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("unchecked spill: the fold taught both groups", store.entriesFor(owner).size() == 2);
    expect("unchecked spill: the folded spill waits for History's save", spill_lines() == 2);
    const QString aside = spill_path() + QStringLiteral(".aside");
    expect("unchecked spill: the spill is set aside", QFile::rename(spill_path(), aside));
    expect("unchecked spill: the spill path loops", QFile::link(spill_path(), spill_path()));
    expect("unchecked spill: the forget is refused", !store.forget(owner, 44700U));
    expect("unchecked spill: the clear is refused", !store.clear(owner));
    expect("unchecked spill: nothing is removed", store.entriesFor(owner).size() == 2);
    store.waitForWrites();
    model.waitForSaveForTest();
    expect("unchecked spill: the loop is removed", QFile::remove(spill_path()));
    expect("unchecked spill: the spill is back", QFile::rename(aside, spill_path()));
    expect("unchecked spill: with the spill checked again, the forget goes through", store.forget(owner, 44700U));
    store.waitForWrites();
    model.waitForSaveForTest();
    expect("unchecked spill: the forget emptied the spill first", spill_size() == 0);
    expect("unchecked spill: one group is left",
           store.entriesFor(owner).size() == 1 && store.entriesFor(owner).at(0).id == 44701U);
#endif
}

/* Issue #903 (PR2 review r1, P2): a Forget or Clear whose spill History cannot settle (its log cannot be written) is
 * refused, as qt_ui.cpp wires the store to History (CallHistoryModel::spillSettler()): nothing is removed, History says
 * it is not saving, and a death then refolds nothing the user saw go. Once History can write again, the Forget goes
 * through and survives a death. */
void
test_heard_forget_refused_while_history_cannot_save(void) {
    resetStorage();
    const QString name = QString::fromLatin1(dsd_qt::HeardTalkgroupStore::kFileName);
    const QString owner = QStringLiteral("sys-a");
    const auto allow = [](const QString& uid) { return uid == QStringLiteral("sys-a"); };
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QTemporaryDir atDeath;
    expect("refused forget: a folder for the disk at death", atDeath.isValid());
    Decoder decoder;
    const time_t when = 1755610000;
    {
        dsd_qt::HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow);
        CallHistoryModel model;
        start_tap(model);
        model.setHeardSink(&store);
        store.setBeforeEdit(model.spillSettler());
        start_saved(model, owner);
        gui_ingest(model);
        set_app_state(Qt::ApplicationSuspended);
        decoder.commit(0, 44500U, 99U, when);
        prove_newest(decoder, 0, 44500U);
        decoder.commit(0, 44501U, 99U, when + 60);
        prove_newest(decoder, 0, 44501U);
        decoder.publish();
        model.waitForBackgroundPollForTest();
        set_app_state(Qt::ApplicationActive);
        gui_ingest(model);
        expect("refused forget: the fold taught both groups", store.entriesFor(owner).size() == 2);
        const QString logPath = dsd_qt::json_store_path("call_history.json");
        expect("refused forget: History's log is blocked", QDir().mkpath(logPath));
        expect("refused forget: the forget is refused", !store.forget(owner, 44500U));
        expect("refused forget: the clear is refused", !store.clear(owner));
        expect("refused forget: nothing is removed", store.entriesFor(owner).size() == 2);
        expect("refused forget: History says it is not saving", model.saveFailing());
        expect("refused forget: the spill is kept", spill_lines() == 2);
        store.waitForWrites();
        model.waitForSaveForTest();
        copy_tree(dataDir, atDeath.path());
        expect("refused forget: History's log is unblocked", QDir(logPath).removeRecursively());
        expect("refused forget: with History saving again, the forget goes through", store.forget(owner, 44500U));
        expect("refused forget: History is saving again", !model.saveFailing());
        store.waitForWrites();
        model.waitForSaveForTest();
        expect("refused forget: the forget emptied the spill first", spill_size() == 0);
    }
    // The death after the refused forget: History's log never reached the disk, so the relaunch folds the spill again;
    // nothing the user saw go comes back, and both groups are remembered.
    QDir(dataDir).removeRecursively();
    copy_tree(atDeath.path(), dataDir);
    QDir(dsd_qt::json_store_path("call_history.json")).removeRecursively();
    {
        dsd_qt::HeardTalkgroupStore relaunchedStore(name);
        relaunchedStore.setOwnerPolicy(allow);
        CallHistoryModel relaunched;
        relaunched.setHeardSink(&relaunchedStore);
        expect("refused forget: a death after the refusal keeps both groups",
               relaunchedStore.entriesFor(owner).size() == 2);
    }
}

/* Fill @p slot's ring of @p decoder with @p count verified calls at once, newest at index 1, as that many commits would:
 * talkgroups from @p tg up, 30 s apart from @p start, heard on live input on scan target @p target. */
void
commit_proven_scan_calls(Decoder& decoder, int slot, int count, uint32_t tg, time_t start, const char* target) {
    Event_History_I* ring = &decoder.state->event_history_s[slot];
    for (int i = 0; i < count; i++) {
        Event_History* item = &ring->Event_History_Items[count - i];
        DSD_MEMSET(item, 0, sizeof(*item));
        item->category = DSD_EVENT_CATEGORY_VOICE;
        item->target_id = tg + static_cast<uint32_t>(i);
        item->source_id = 100U;
        item->event_start_time = start + 30 * static_cast<time_t>(i);
        item->event_time = item->event_start_time + 4;
        item->heard_variant = 1U;
        item->heard_id = tg + static_cast<uint32_t>(i);
        item->live_input = 1U;
        item->scan_member = 1U;
        DSD_SNPRINTF(item->channel_label, sizeof(item->channel_label), "%s", target);
    }
    ring->push_seq += static_cast<uint64_t>(count);
    ring->commit_rev++;
    ring->revision++;
}

/* Issue #903 (PR2 review r1, P2): a long multi-system scan spills while suspended and the process dies. The relaunch
 * folds the spill before the remembered talkgroups exist, holding what it finds for them: past the store's cap of one
 * owner, its newest, as the store keeps them. The last system's only talkgroup, after more than 20,000 of another's, is
 * kept, and so is each owner's newest. */
void
test_heard_spill_recovery_keeps_each_owners_newest(void) {
    resetStorage();
    Decoder decoder;
    const time_t when = 1755620000;
    constexpr int kFirstOwnerGroups = 20250;
    constexpr int kBatch = 250;
    {
        CallHistoryModel model;
        start_tap(model);
        model.beginSessionWithOwners(QStringLiteral("County scan"), QString(), QString(),
                                     QVariantMap{{QStringLiteral("entry-a"), QStringLiteral("sys-a")},
                                                 {QStringLiteral("entry-b"), QStringLiteral("sys-b")}});
        gui_ingest(model);
        set_app_state(Qt::ApplicationSuspended);
        for (int done = 0; done < kFirstOwnerGroups; done += kBatch) {
            commit_proven_scan_calls(decoder, 0, kBatch, 50000U + static_cast<uint32_t>(done),
                                     when + 30 * static_cast<time_t>(done), "entry-a");
            decoder.publish();
            model.waitForBackgroundPollForTest();
        }
        commit_proven_scan_calls(decoder, 0, 1, 90000U, when + 30 * static_cast<time_t>(kFirstOwnerGroups), "entry-b");
        decoder.publish();
        model.waitForBackgroundPollForTest();
    } // death: the tap's stop writes what it holds, and nothing folds it
    RecordingHeardSink sink;
    CallHistoryModel relaunched;
    relaunched.setHeardSink(&sink);
    expect("spill recovery: the last system's only group is kept", sink.count(QStringLiteral("sys-b"), 90000U) == 1);
    int newest = 0;
    for (uint32_t tg = 50000U + kFirstOwnerGroups - dsd_qt::kHeardPendingPerOwner; tg < 50000U + kFirstOwnerGroups;
         tg++) {
        newest += sink.count(QStringLiteral("sys-a"), tg) == 1;
    }
    expect("spill recovery: the other system keeps its newest", newest == dsd_qt::kHeardPendingPerOwner);
    expect("spill recovery: each owner holds at most twice the cap",
           sink.seen.size() <= 1 + 2 * dsd_qt::kHeardPendingPerOwner);
}

/* Issue #903 (PR2 review r2, P2): a Forget is settled only once no spill is left to replay. The handoff's read of the
 * spill fails partway (an I/O error), so no fold has read it whole and no save may empty it: the Forget reads it again,
 * has History write it, and empties it before anything is removed. A death right after the Forget then refolds nothing, and the forgotten group
 * stays forgotten. */
void
test_heard_forget_after_a_partial_spill_read(void) {
    resetStorage();
    const QString name = QString::fromLatin1(dsd_qt::HeardTalkgroupStore::kFileName);
    const QString owner = QStringLiteral("sys-a");
    const auto allow = [](const QString& uid) { return uid == QStringLiteral("sys-a"); };
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QTemporaryDir atDeath;
    expect("partial read forget: a folder for the disk at death", atDeath.isValid());
    Decoder decoder;
    const time_t when = 1755640000;
    {
        dsd_qt::HeardTalkgroupStore store(name);
        store.setOwnerPolicy(allow);
        CallHistoryModel model;
        start_tap(model);
        model.setHeardSink(&store);
        store.setBeforeEdit(model.spillSettler());
        start_saved(model, owner);
        gui_ingest(model);
        set_app_state(Qt::ApplicationSuspended);
        decoder.commit(0, 44700U, 99U, when);
        prove_newest(decoder, 0, 44700U);
        decoder.commit(0, 44701U, 99U, when + 60);
        prove_newest(decoder, 0, 44701U);
        decoder.publish();
        model.waitForBackgroundPollForTest();
        g_spillReadWarnings = 0;
        CallHistoryModel::failNextSpillReadAfterLinesForTest(1);
        set_app_state(Qt::ApplicationActive);
        gui_ingest(model);
        // The ring read after the handoff took in the call the failed read missed; the spill waits whole.
        expect("partial read forget: the handoff's read failed, and the spill is kept",
               g_spillReadWarnings == 1 && spill_lines() == 2 && store.entriesFor(owner).size() == 2);
        expect("partial read forget: the forget goes through", store.forget(owner, 44700U));
        expect("partial read forget: the forget emptied the spill first", spill_size() == 0);
        store.waitForWrites();
        model.waitForSaveForTest();
        // The process dies here, before History's own (debounced) save.
        copy_tree(dataDir, atDeath.path());
    }
    QDir(dataDir).removeRecursively();
    copy_tree(atDeath.path(), dataDir);
    dsd_qt::HeardTalkgroupStore relaunchedStore(name);
    relaunchedStore.setOwnerPolicy(allow);
    CallHistoryModel relaunched;
    relaunched.setHeardSink(&relaunchedStore);
    gui_ingest(relaunched);
    const QVector<dsd_qt::HeardTalkgroupStore::Entry> left = relaunchedStore.entriesFor(owner);
    expect("partial read forget: the forgotten group does not come back, the other is remembered",
           left.size() == 1 && left.at(0).id == 44701U);
}

/* Issue #903 (PR2 review r2, P2): a spill History can read and save but not empty (its truncate fails) can replay what
 * a Forget or Clear removed: both are refused, nothing is removed, and the spill is kept. Once the spill can be emptied,
 * the Forget goes through and empties it. */
void
test_heard_forget_refused_while_the_spill_cannot_be_emptied(void) {
    resetStorage();
    const QString name = QString::fromLatin1(dsd_qt::HeardTalkgroupStore::kFileName);
    const QString owner = QStringLiteral("sys-a");
    const auto allow = [](const QString& uid) { return uid == QStringLiteral("sys-a"); };
    Decoder decoder;
    const time_t when = 1755650000;
    dsd_qt::HeardTalkgroupStore store(name);
    store.setOwnerPolicy(allow);
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&store);
    store.setBeforeEdit(model.spillSettler());
    start_saved(model, owner);
    gui_ingest(model);
    set_app_state(Qt::ApplicationSuspended);
    decoder.commit(0, 44800U, 99U, when);
    prove_newest(decoder, 0, 44800U);
    decoder.commit(0, 44801U, 99U, when + 60);
    prove_newest(decoder, 0, 44801U);
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("unemptied spill: the fold taught both groups", store.entriesFor(owner).size() == 2);
    CallHistoryModel::failSpillTruncatesForTest(true);
    expect("unemptied spill: the forget is refused", !store.forget(owner, 44800U));
    expect("unemptied spill: asked again, it is refused again", !store.forget(owner, 44800U));
    expect("unemptied spill: the clear is refused", !store.clear(owner));
    expect("unemptied spill: nothing is removed", store.entriesFor(owner).size() == 2);
    expect("unemptied spill: the spill is kept", spill_lines() == 2);
    CallHistoryModel::failSpillTruncatesForTest(false);
    expect("unemptied spill: once it can be emptied, the forget goes through", store.forget(owner, 44800U));
    expect("unemptied spill: the forget emptied the spill first", spill_size() == 0);
    const QVector<dsd_qt::HeardTalkgroupStore::Entry> left = store.entriesFor(owner);
    expect("unemptied spill: only the forgotten group went", left.size() == 1 && left.at(0).id == 44801U);
    store.waitForWrites();
    model.waitForSaveForTest();
}

/* Issue #903 (PR2 review r1, P2): a start that completes while the app is away, on the state the last run used (an
 * embedding host that reuses it): the tap reads the reused ring for the new session, whose calls are its own. */
void
test_heard_reused_ring_through_the_tap(void) {
    resetStorage();
    RecordingHeardSink sink;
    CallHistoryModel model;
    start_tap(model);
    model.setHeardSink(&sink);
    start_saved(model, QStringLiteral("sys-a"));
    const time_t when = 1755630000;
    Decoder decoder;
    decoder.commit(0, 44600U, 99U, when);
    prove_newest(decoder, 0, 44600U);
    decoder.publish();
    gui_ingest(model);
    set_app_state(Qt::ApplicationHidden);
    model.waitForBackgroundPollForTest();
    start_saved(model, QStringLiteral("sys-b"));
    decoder.commit(0, 44601U, 100U, when + 60);
    prove_newest(decoder, 0, 44601U);
    decoder.publish();
    model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    gui_ingest(model);
    expect("reused ring tap: A's call is A's", sink.count(QStringLiteral("sys-a"), 44600U) == 1);
    expect("reused ring tap: B's call on A's ring is B's",
           sink.count(QStringLiteral("sys-b"), 44601U) == 1 && sink.seen.size() == 2);
}

/* Issue #903 (r2 finding 2): the backlog the tap could not spill reaches the remembered talkgroups at the model's
 * teardown, before its last save; the sink outlives the model, as the app builds them. */
void
test_heard_teardown_backlog_reaches_the_sink(void) {
    resetStorage();
    Decoder decoder;
    RecordingHeardSink sink;
    const time_t when = 1755500000;
    {
        CallHistoryModel model;
        start_tap(model);
        model.setHeardSink(&sink);
        start_saved(model, QStringLiteral("sys-a"));
        gui_ingest(model);
        expect("teardown heard: the spill is blocked", QDir().mkpath(spill_path()));
        set_app_state(Qt::ApplicationSuspended);
        for (int i = 0; i < 20; i++) {
            const uint32_t tg = 43300U + static_cast<uint32_t>(i);
            decoder.commit(0, tg, 99U, when + 30 * static_cast<time_t>(i));
            prove_newest(decoder, 0, tg);
        }
        decoder.publish();
        model.waitForBackgroundPollForTest();
        expect("teardown heard: nothing reached the spill or the sink", spill_lines() == 0 && sink.seen.isEmpty());
    }
    QDir(spill_path()).removeRecursively();
    expect("teardown heard: the teardown hands on every group held", sink.seen.size() == 20);
    CallHistoryModel relaunched;
    RecordingHeardSink later;
    relaunched.setHeardSink(&later);
    gui_ingest(relaunched);
    expect("teardown heard: the relaunch hands on none again", relaunched.count() == 20 && later.seen.isEmpty());
}

} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-call-history-tap-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        DSD_FPRINTF(stderr, "FAIL: could not create settings dir\n");
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    g_previousHandler = qInstallMessageHandler(count_warnings);

    test_background_commits_keep_every_row();
    test_spill_reload_after_process_death();
    test_stop_while_suspended();
    test_clear_while_suspended();
    test_sustained_write_volume();
    test_spill_with_site_under_size_cap();
    test_clear_before_the_first_ingest_after_return();
    test_unwritable_spill_keeps_a_bounded_backlog();
    test_unwritable_spill_hands_off_what_it_holds();
    test_unwritable_spill_teardown_keeps_what_it_holds();
    test_unwritable_spill_keeps_a_call_only_its_updates_hold();
    test_replayed_notice_folds_once_from_a_new_ring();
    test_long_spill_folds_bounded();
    test_spill_fold_trims_oldest_session_first();
    test_very_long_spill_folds_in_bounded_memory();
    test_long_spill_of_repeated_notices_folds_bounded();
    test_failed_spill_read_keeps_the_spill();
    test_session_change_while_hidden();
    test_session_fields_change_together_while_hidden();
    test_tap_attributes_by_row_stamp();
    test_tap_saves_a_settled_site_text_digest();
    test_failed_spill_read_after_a_newer_run();
    test_background_gps_withdrawal();
    test_heard_long_spill_reaches_the_sink_before_the_trim();
    test_heard_last_call_of_a_during_b_start();
    test_heard_tap_learns_only_owned_live_calls();
    test_heard_tap_after_a_live_retune();
    test_heard_teardown_backlog_reaches_the_sink();
    test_heard_forget_after_a_fold_survives_a_death();
    test_heard_forget_refused_while_history_cannot_save();
    test_heard_forget_refused_while_the_spill_cannot_be_checked();
    test_heard_forget_after_a_partial_spill_read();
    test_heard_forget_refused_while_the_spill_cannot_be_emptied();
    test_heard_spill_recovery_keeps_each_owners_newest();
    test_heard_reused_ring_through_the_tap();
    test_heard_hold_for_start();
    test_heard_clear_while_the_tap_runs();
    test_heard_cleared_row_proof_through_the_tap();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
