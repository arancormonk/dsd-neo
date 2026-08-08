// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: CallHistoryModel's ring ingest — the slot/seq/start keying that
 * keeps committed ring rows one-to-one with logged rows. Regressions covered:
 * two TDMA slots committing the same talkgroup in the same second must both
 * land; two textual-target (tg==0) calls to different destinations must not
 * merge; an alias-only row must not be dropped; an in-place reacquisition merge
 * (end extends, enc flips) must update the logged row, not duplicate it; the
 * ring walk must be gated on commit_rev, not on staged-row renders; and a
 * relaunched model must not re-ingest rows its predecessor already logged. */

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "call_history_model.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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

/* Each test owns a fresh persisted world: the model writes its stores in its
 * destructor, and leakage between tests would fake (or hide) regressions. */
void
resetStorage(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    QSettings settings;
    settings.clear();
    settings.sync();
}

/* A minimal decoder state: the model only reads event_history_s. */
struct RingFixture {
    dsd_state* state;
    Event_History_I* rings;

    RingFixture() {
        state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
        rings = static_cast<Event_History_I*>(calloc(2, sizeof(Event_History_I)));
        state->event_history_s = rings;
        for (int slot = 0; slot < 2; slot++) {
            rings[slot].revision = 1U;
            rings[slot].commit_rev = 1U;
        }
    }

    ~RingFixture() {
        free(rings);
        free(state);
    }

    RingFixture(const RingFixture&) = delete;
    RingFixture& operator=(const RingFixture&) = delete;

    /** Commit a row the way push_event_history() does: shift, fill index 1. */
    Event_History*
    commit(int slot, uint32_t tg, uint32_t src, time_t start, time_t end, const char* tgt_str = "",
           const char* t_name = "") {
        Event_History_I* ring = &rings[slot];
        DSD_MEMMOVE(&ring->Event_History_Items[2], &ring->Event_History_Items[1],
                    sizeof(Event_History) * (DSD_EVENT_HISTORY_LEN - 2));
        Event_History* item = &ring->Event_History_Items[1];
        DSD_MEMSET(item, 0, sizeof(*item));
        item->category = DSD_EVENT_CATEGORY_VOICE;
        item->target_id = tg;
        item->source_id = src;
        item->event_start_time = start;
        item->event_time = end;
        DSD_SNPRINTF(item->tgt_str, sizeof(item->tgt_str), "%s", tgt_str);
        DSD_SNPRINTF(item->t_name, sizeof(item->t_name), "%s", t_name);
        ring->push_seq++;
        ring->commit_rev++;
        ring->revision++;
        return item;
    }

    /** A staged-row render: bumps revision only, exactly like the core. */
    void
    stagedRender(int slot) {
        rings[slot].revision++;
    }

    /** An in-place committed-row mutation (reacquisition merge, enrichment). */
    void
    touchCommitted(int slot) {
        rings[slot].commit_rev++;
        rings[slot].revision++;
    }
};

void
test_two_slots_same_second_same_talkgroup(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500000;
    // The same talkgroup keys up on both TDMA slots in the same wall-clock
    // second — two real transmissions from two different units.
    ring.commit(0, 4001, 100, when, when + 4);
    ring.commit(1, 4001, 200, when, when + 6);
    model.refresh(ring.state);
    expect("both slots' same-second calls are logged", model.count() == 2);
}

void
test_textual_targets_stay_distinct(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500100;
    // Two M17-style calls to different callsign destinations, both tg==0 and
    // src unknown, back to back within the src-unknown merge window.
    ring.commit(0, 0, 0, when, when + 2, "ALPHA");
    ring.commit(0, 0, 0, when + 3, when + 5, "BRAVO");
    model.refresh(ring.state);
    expect("textual destinations do not merge", model.count() == 2);
    bool sawAlpha = false;
    bool sawBravo = false;
    for (int i = 0; i < model.count(); i++) {
        const QString name = model.data(model.index(i), CallHistoryModel::NameRole).toString();
        sawAlpha = sawAlpha || name == QStringLiteral("ALPHA");
        sawBravo = sawBravo || name == QStringLiteral("BRAVO");
    }
    expect("both callsigns are kept", sawAlpha && sawBravo);
}

void
test_alias_only_row_is_logged(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500200;
    ring.commit(0, 0, 0, when, when + 3, "", "County Dispatch");
    model.refresh(ring.state);
    expect("alias-only voice row is logged", model.count() == 1);
    expect("alias-only row is named from the label",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::NameRole).toString()
                      == QStringLiteral("County Dispatch"));
}

void
test_reacquisition_merge_updates_in_place(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500300;
    Event_History* item = ring.commit(0, 4002, 0, when, when + 3);
    model.refresh(ring.state);
    expect("first fragment lands", model.count() == 1);

    // The core's reacquisition merge: end extends, src fills, enc flips — in
    // place, same slot/seq/start.
    item->event_time = when + 45;
    item->source_id = 777;
    item->enc = 1;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("merged row stays one row", model.count() == 1);
    expect("merged row's duration extends",
           model.count() == 1 && model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 45);
    expect("merged row learns enc",
           model.count() == 1 && model.data(model.index(0), CallHistoryModel::EncRole).toBool());
}

void
test_ring_walk_gated_on_commit_rev(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500400;
    ring.commit(0, 4003, 0, when, when + 2);
    model.refresh(ring.state);
    expect("seed row lands", model.count() == 1);

    // Plant a new committed row but bump only `revision` — the staged-render
    // signature. The gate must hold: the walk runs on commits, not renders.
    ring.rings[0].push_seq++;
    DSD_MEMMOVE(&ring.rings[0].Event_History_Items[2], &ring.rings[0].Event_History_Items[1],
                sizeof(Event_History) * (DSD_EVENT_HISTORY_LEN - 2));
    Event_History* item = &ring.rings[0].Event_History_Items[1];
    DSD_MEMSET(item, 0, sizeof(*item));
    item->category = DSD_EVENT_CATEGORY_VOICE;
    item->target_id = 4004;
    item->event_start_time = when + 60;
    item->event_time = when + 62;
    ring.stagedRender(0);
    model.refresh(ring.state);
    expect("staged-render revision bump does not walk the ring", model.count() == 1);

    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("commit_rev bump ingests the new row", model.count() == 2);
}

void
test_relaunch_does_not_reingest(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754500500;
    ring.commit(0, 4005, 300, when, when + 8);
    ring.commit(1, 4005, 400, when, when + 8);
    {
        CallHistoryModel model;
        model.refresh(ring.state);
        expect("both rows land before the restart", model.count() == 2);
    } // destructor flushes the stores
    // The Activity restarts while the service's ring still holds both rows.
    CallHistoryModel relaunched;
    expect("relaunched model restores the log", relaunched.count() == 2);
    relaunched.refresh(ring.state);
    expect("relaunched model does not re-ingest ring rows", relaunched.count() == 2);
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    /* Isolated, disposable storage, same pattern as the persistence test. */
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-call-history-%1").arg(QCoreApplication::applicationPid()));
    QStandardPaths::setTestModeEnabled(true);
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        DSD_FPRINTF(stderr, "FAIL: could not create settings dir\n");
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());

    test_two_slots_same_second_same_talkgroup();
    test_textual_targets_stay_distinct();
    test_alias_only_row_is_logged();
    test_reacquisition_merge_updates_in_place();
    test_ring_walk_gated_on_commit_rev();
    test_relaunch_does_not_reingest();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
