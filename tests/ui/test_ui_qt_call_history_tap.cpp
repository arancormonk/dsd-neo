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
 * oldest go first, and neither the log nor the fold's reader and seen map ever hold the whole backlog. The deliberate
 * failures' warnings are counted, not printed. */

#include <QByteArray>
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
#include <QLatin1String>
#include <QModelIndex>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <Qt>
#include <QtGlobal>
#include <atomic>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../src/app_control/snapshot_internal.h"
#include "../test_support/qt_test_paths.h"
#include "call_history_model.h"
#include "call_history_tap.h"
#include "json_store.h"

using dsd_qt::CallHistoryModel;

namespace {

int g_failures = 0;

/* The model's and the tap's once-only warnings, counted (from any thread) and kept out of the output. */
std::atomic<int> g_saveFailedWarnings{0};
std::atomic<int> g_spillDropWarnings{0};
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

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
