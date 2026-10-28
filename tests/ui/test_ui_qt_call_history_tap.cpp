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
 * stopped decoder left published are captured; a clear made while the tap runs keeps its rows cleared; and the spill
 * is written in batches of about a kilobyte a row. */

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QModelIndex>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/events.h>
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
#include "json_store.h"

using dsd_qt::CallHistoryModel;

namespace {

int g_failures = 0;

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

    /** @p count calls on @p slot, talkgroups from @p tg up, 30 s apart from @p start (none merge). */
    void
    commitCalls(int slot, int count, uint32_t tg, time_t start) {
        for (int i = 0; i < count; i++) {
            commit(slot, tg + static_cast<uint32_t>(i), 100U + static_cast<uint32_t>(i),
                   start + 30 * static_cast<time_t>(i));
        }
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

void
start_tap(CallHistoryModel& model) {
    model.setHistoryTapSource(&dsd_app_history_tap_copy);
    model.setBackgroundCaptureTimingForTest(2, 5);
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
        decoder.commitCalls(0, 50, 31001U + static_cast<uint32_t>(batch) * 50U, when + batch * 1500);
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
    model.saveNow();
    settle_save(model);
    expect("a failed save keeps the spill", model.saveFailing() && spill_lines() == 300);
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
        decoder.publish();
        for (int poll = 0; poll < 6; poll++) {
            model.waitForBackgroundPollForTest();
        }
        expect("the batch reached the disk while the tap still runs",
               model.backgroundCaptureRunningForTest() && spill_lines() == 60 && model.spillAppendsForTest() == 1);
    } // the process "dies": nothing folds the spill or saves the rows it holds
    expect("the spill outlives the process", spill_lines() == 60);
    const QString seenPath = dsd_qt::json_store_path("call_history_seen.json");
    {
        CallHistoryModel relaunched;
        expect("the relaunch folds the spill", relaunched.count() == 61
                                                   && rows_in_tg_range(relaunched, 32000U, 32040U) == 41
                                                   && rows_in_tg_range(relaunched, 32101U, 32120U) == 20);
        gui_ingest(relaunched);
        expect("the ring the rows are still in adds none twice", relaunched.count() == 61);
        // This one cannot save either (its last save, on destruction, fails).
        QFile::remove(seenPath);
        expect("the seen store is blocked", QDir().mkpath(seenPath));
    }
    QDir(seenPath).removeRecursively();
    expect("a relaunch that folded but did not get its save acknowledged keeps the spill", spill_lines() == 60);
    {
        CallHistoryModel again;
        expect("folding it a second time adds nothing", again.count() == 61);
        again.saveNow();
        settle_save(again);
        expect("the acknowledged save drops it", spill_size() == 0);
    }
    CallHistoryModel last;
    expect("the log keeps every call", last.count() == 61);
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
    expect("ten calls a second stay inside the 16 KiB/s budget", lines > 0 && 10 * bytes / lines < 16 * 1024);
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

    test_background_commits_keep_every_row();
    test_spill_reload_after_process_death();
    test_stop_while_suspended();
    test_clear_while_suspended();
    test_sustained_write_volume();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
