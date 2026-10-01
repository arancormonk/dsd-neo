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
 * relaunched model must not re-ingest rows its predecessor already logged.
 * Clear, the session views and retention go by session and ring order, never by
 * stamps: calls ingested after a clear show however old their stamps (a replay's
 * are the capture's), a relaunch keeps cleared calls cleared, a new session starts
 * fresh, and a full log or seen map gives up the oldest session first. */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QModelIndex>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <initializer_list>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../test_support/qt_test_paths.h"
#include "json_store.h"

#include "call_history_filter.h"
#include "call_history_model.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

using dsd_qt::CallHistoryFilterModel;
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

/* A minimal decoder state: the model only reads event_history_s. Each fixture is a new ring, with
 * an identity of its own as initState() draws one; a run on a reused state keeps reading the same
 * fixture. */
struct RingFixture {
    dsd_state* state;
    Event_History_I* rings;

    RingFixture() {
        static uint64_t nextInstance = 0x5EED0000ULL;
        nextInstance++;
        state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
        rings = static_cast<Event_History_I*>(calloc(2, sizeof(Event_History_I)));
        state->event_history_s = rings;
        for (int slot = 0; slot < 2; slot++) {
            rings[slot].revision = 1U;
            rings[slot].commit_rev = 1U;
            rings[slot].instance = nextInstance;
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
           const char* t_name = "", const char* channel_label = "") {
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
        DSD_SNPRINTF(item->channel_label, sizeof(item->channel_label), "%s", channel_label);
        ring->push_seq++;
        ring->commit_rev++;
        ring->revision++;
        return item;
    }

    /** Commit a data notice the way dsd_event_emit_data_notice() does: the
     *  emitter's rendered line (timestamp, optional "[label] " prefix, summary)
     *  in event_string and the channel label the row was stamped with. */
    Event_History*
    commitNotice(int slot, time_t when, const char* event_string, const char* channel_label = "") {
        Event_History* item = commit(slot, 0, 0, when, when);
        item->category = DSD_EVENT_CATEGORY_DATA;
        DSD_SNPRINTF(item->event_string, sizeof(item->event_string), "%s", event_string);
        DSD_SNPRINTF(item->channel_label, sizeof(item->channel_label), "%s", channel_label);
        return item;
    }

    /**
     * Commit @p count calls at once, as that many commit() calls would, oldest first:
     * talkgroups from @p tg up, sources from @p src up, starts 10 s apart from @p start.
     * One shift instead of one per row, for filling a log.
     */
    void
    commitBatch(int slot, int count, uint32_t tg, uint32_t src, time_t start) {
        Event_History_I* ring = &rings[slot];
        DSD_MEMMOVE(&ring->Event_History_Items[1 + count], &ring->Event_History_Items[1],
                    sizeof(Event_History) * (DSD_EVENT_HISTORY_LEN - 1 - count));
        for (int i = 0; i < count; i++) {
            // Index 1 holds the newest commit.
            Event_History* item = &ring->Event_History_Items[count - i];
            DSD_MEMSET(item, 0, sizeof(*item));
            item->category = DSD_EVENT_CATEGORY_VOICE;
            item->target_id = tg + static_cast<uint32_t>(i);
            item->source_id = src + static_cast<uint32_t>(i);
            item->event_start_time = start + 10 * i;
            item->event_time = start + 10 * i + 4;
        }
        ring->push_seq += static_cast<uint64_t>(count);
        ring->commit_rev++;
        ring->revision++;
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
test_system_identity_persistence(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    const time_t when = 1754500800;
    {
        CallHistoryModel model;
        model.setSessionLabel("Same name");
        model.setSessionUid("system-a");
        ring.commit(0, 1001, 2001, when, when + 4);
        model.refresh(ring.state, opts.get());
        expect("ingest exposes saved identity",
               model.data(model.index(0), CallHistoryModel::SystemUidRole) == "system-a");
        model.setSessionUid("system-b");
        ring.commit(0, 1001, 2001, when + 5, when + 9);
        model.refresh(ring.state, opts.get());
        expect("same-named systems do not merge", model.count() == 2);
        expect("changing session identity preserves old rows",
               model.data(model.index(1), CallHistoryModel::SystemUidRole) == "system-a");
        expect("new calls carry the new identity",
               model.data(model.index(0), CallHistoryModel::SystemUidRole) == "system-b");
    }
    {
        CallHistoryModel restored;
        expect("session identity persists", restored.sessionUid() == "system-b");
        expect("row identity persists",
               restored.data(restored.index(0), CallHistoryModel::SystemUidRole) == "system-b"
                   && restored.data(restored.index(1), CallHistoryModel::SystemUidRole) == "system-a");
        restored.refresh(ring.state, opts.get());
        expect("identity does not change ring deduplication", restored.count() == 2);
    }
    auto rows = dsd_qt::json_store_load_array("call_history.json");
    auto legacy = rows.at(1).toObject();
    legacy.remove("systemUid");
    rows.replace(1, legacy);
    expect("legacy history fixture saved", dsd_qt::json_store_save_array("call_history.json", rows));
    CallHistoryModel legacyModel;
    expect("legacy row has no hold identity",
           legacyModel.data(legacyModel.index(1), CallHistoryModel::SystemUidRole).toString().isEmpty());
}

void
test_scanning_rows_never_gain_hold_identity(void) {
    for (int scanMode = 0; scanMode < 2; ++scanMode) {
        resetStorage();
        RingFixture ring;
        auto opts = std::make_unique<dsd_opts>();
        opts->scanner_mode = scanMode == 0;
        opts->trunk_scan_enabled = scanMode == 1;
        const time_t when = 1754500900;
        {
            CallHistoryModel model;
            // A saved system can carry -Y or --trunk-scan in extra arguments.
            model.setSessionLabel("Saved source");
            model.setSessionUid("saved-source");
            ring.commit(0, 1001, 2001, when, when + 4);
            model.refresh(ring.state, opts.get());
            expect("effective scanning excludes row identity even without a channel label",
                   model.count() == 1
                       && model.data(model.index(0), CallHistoryModel::SystemUidRole).toString().isEmpty());
        }
        CallHistoryModel restored;
        expect("scanned row remains ineligible after reload",
               restored.count() == 1
                   && restored.data(restored.index(0), CallHistoryModel::SystemUidRole).toString().isEmpty());
        opts->scanner_mode = 0;
        opts->trunk_scan_enabled = 0;
        ring.commit(0, 1001, 2001, when + 5, when + 9);
        restored.refresh(ring.state, opts.get());
        expect("later single-system call is eligible and cannot merge into a scan row",
               restored.count() == 2
                   && restored.data(restored.index(0), CallHistoryModel::SystemUidRole) == "saved-source"
                   && restored.data(restored.index(1), CallHistoryModel::SystemUidRole).toString().isEmpty());
        ring.commit(0, 3001, 4001, when + 20, when + 24, "", "", "Retained scan target");
        restored.refresh(ring.state, opts.get());
        expect("retained scan-labelled rows stay ineligible after rotation stops",
               restored.count() == 3
                   && restored.data(restored.index(0), CallHistoryModel::SystemUidRole).toString().isEmpty());
        ring.commit(0, 5001, 6001, when + 30, when + 34);
        restored.refresh(ring.state);
        expect("unknown effective options cannot grant hold identity",
               restored.count() == 4
                   && restored.data(restored.index(0), CallHistoryModel::SystemUidRole).toString().isEmpty());
    }
}

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

/* Encrypted traffic on a scanned conventional list decodes no talkgroup at
 * all: tg 0, no textual target, no CSV name. Dropping those rows is right for
 * a stray sync, but wrong once the channel the call was heard on names it —
 * that name is the whole answer to "what was that?". */
void
test_channel_labelled_tg0_row_is_logged(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500250;
    ring.commit(0, 0, 0, when, when + 3, "", "", "Fire Dispatch");
    model.refresh(ring.state);
    expect("channel-labelled tg0 row is logged", model.count() == 1);
    expect("channel-labelled row is named from the channel",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::NameRole).toString() == QStringLiteral("Fire Dispatch"));

    /* Nothing names this one at all — still the noise row the drop exists for. */
    ring.commit(0, 0, 0, when + 10, when + 12);
    model.refresh(ring.state);
    expect("unlabelled tg0 row is still dropped", model.count() == 1);

    /* A different channel is a different call, tg 0 on both notwithstanding. */
    ring.commit(0, 0, 0, when + 20, when + 23, "", "", "PD Tac");
    model.refresh(ring.state);
    expect("a second channel keeps its own row", model.count() == 2);
}

/* The emitter renders a labelled notice as "[label] summary"; the history shows
 * the label on the row's meta line from its channel role, so the name must be
 * the bare summary. Only the row's own label is stripped: a summary that happens
 * to start with a bracket keeps it. */
void
test_labelled_notice_name_drops_its_own_channel_prefix(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500250;
    ring.commitNotice(0, when, "2026-04-30 00:00:00 [SiteA] LRRP position", "SiteA");
    model.refresh(ring.state);
    expect("labelled notice is logged", model.count() == 1);
    expect("labelled notice name is the bare summary",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::NameRole).toString() == QStringLiteral("LRRP position"));
    expect("labelled notice carries its channel in the channel role",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::ChannelRole).toString() == QStringLiteral("SiteA"));

    ring.commitNotice(0, when + 5, "2026-04-30 00:00:05 [Not a label] SMS from 1234");
    model.refresh(ring.state);
    expect("a bracket that is not the row's label survives",
           model.count() == 2
               && model.data(model.index(0), CallHistoryModel::NameRole).toString()
                      == QStringLiteral("[Not a label] SMS from 1234"));
}

/* The scan channel a row was heard on rides its own role. The system stays the
 * saved entry the session runs, for labelled and unlabelled rows alike, so the
 * "All systems" pill keeps offering what the Home screen lists and never a
 * channel name; and a talkgroup heard on two channels stays two rows. */
void
test_channel_label_is_its_own_role(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    model.setSessionLabel(QStringLiteral("Scan list"));
    const time_t when = 1754500250;
    ring.commit(0, 4001, 100, when, when + 3, "", "", "Fire Dispatch");
    model.refresh(ring.state);
    expect("labelled row carries the channel",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::ChannelRole).toString()
                      == QStringLiteral("Fire Dispatch"));
    expect("labelled row keeps the session as its system",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::SystemNameRole).toString()
                      == QStringLiteral("Scan list"));

    ring.commit(0, 4002, 101, when + 10, when + 12);
    model.refresh(ring.state);
    expect("unlabelled row has no channel",
           model.count() == 2 && model.data(model.index(0), CallHistoryModel::ChannelRole).toString().isEmpty());
    expect("unlabelled row keeps the session label",
           model.count() == 2
               && model.data(model.index(0), CallHistoryModel::SystemNameRole).toString()
                      == QStringLiteral("Scan list"));
    const QStringList labels = model.systemLabels();
    expect("system filter lists only the session", labels == QStringList{QStringLiteral("Scan list")});

    /* The same talkgroup and unit heard on another channel is another row. */
    ring.commit(0, 4001, 100, when + 20, when + 22, "", "", "PD Tac");
    model.refresh(ring.state);
    expect("a different channel keeps the same talkgroup on its own row", model.count() == 3);
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

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    item->emergency = 1;
    item->priority = 3;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("late emergency alone updates the row",
           model.count() == 1 && model.data(model.index(0), CallHistoryModel::EmergencyRole).toBool());
    expect("late emergency emits one dataChanged signal", changed.count() == 1);
    if (changed.count() == 1) {
        const QList<QVariant>& arguments = changed.at(0);
        expect("late emergency signals the existing row",
               qvariant_cast<QModelIndex>(arguments.at(0)) == model.index(0)
                   && qvariant_cast<QModelIndex>(arguments.at(1)) == model.index(0));
        expect("late emergency notifies the emergency role",
               qvariant_cast<QVector<int>>(arguments.at(2)).contains(CallHistoryModel::EmergencyRole));
    }

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
    ring.commit(1, 4005, 400, when, when + 8)->emergency = 1;
    {
        CallHistoryModel model;
        model.refresh(ring.state);
        expect("both rows land before the restart", model.count() == 2);
    } // destructor flushes the stores
    for (const char* store : {"call_history.json", "call_history_seen.json"}) {
        QFile file(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QLatin1Char('/')
                   + QLatin1String(store));
        expect("history store opens", file.open(QIODevice::ReadOnly));
        const QJsonArray rows = QJsonDocument::fromJson(file.readAll()).array();
        int marked = 0;
        expect("both rows persisted", rows.size() == 2);
        for (const auto& value : rows) {
            const QJsonObject row = value.toObject();
            if (row.contains(QLatin1String("em"))) {
                expect("em is written only when true", row.value(QLatin1String("em")).toBool());
                ++marked;
            }
        }
        expect("one emergency key, ordinary row omits it", marked == 1);
    }
    // The Activity restarts while the service's ring still holds both rows.
    CallHistoryModel relaunched;
    expect("relaunched model restores the log", relaunched.count() == 2);
    int emergencies = 0;
    for (int i = 0; i < relaunched.count(); ++i) {
        emergencies += relaunched.data(relaunched.index(i), CallHistoryModel::EmergencyRole).toBool() ? 1 : 0;
    }
    expect("only the emergency row restores its flag", emergencies == 1);
    relaunched.refresh(ring.state);
    expect("relaunched model does not re-ingest ring rows", relaunched.count() == 2);
}

void
test_source_labels_survive_merge_and_relaunch(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754500500;
    Event_History* item = ring.commit(0, 1201, 1201, when, when + 8, "", "Talkgroup 1201");
    DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Radio 1201");
    {
        CallHistoryModel model;
        model.refresh(ring.state);
        expect("source alias has its own role",
               model.data(model.index(0), CallHistoryModel::SourceNameRole).toString() == QStringLiteral("Radio 1201"));
        expect("source alias preserves numeric ID",
               model.data(model.index(0), CallHistoryModel::SrcRole).toUInt() == 1201U);
        expect("source alias preserves group name",
               model.data(model.index(0), CallHistoryModel::NameRole).toString() == QStringLiteral("Talkgroup 1201"));
        DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Enriched 1201");
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("alias-only enrichment updates the existing row",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Enriched 1201"));
        item = ring.commit(0, 1201, 1201, when + 9, when + 15, "", "Talkgroup 1201");
        DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Unit 1201");
        model.refresh(ring.state);
        expect("source alias follows merged fragment",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Unit 1201"));
    }
    CallHistoryModel restored;
    expect("source alias survives persistence",
           restored.data(restored.index(0), CallHistoryModel::SourceNameRole).toString()
               == QStringLiteral("Unit 1201"));
    item = ring.commit(1, 4005, 0, when + 60, when + 65);
    DSD_SNPRINTF(item->src_str, sizeof(item->src_str), "%s", "N0CALL");
    restored.refresh(ring.state);
    expect("textual source survives without an alias",
           restored.data(restored.index(0), CallHistoryModel::SourceNameRole).toString() == QStringLiteral("N0CALL"));
}

/* Refresh timing must not choose the label: backlog scans newest-first, while
 * incremental refresh sees the same fragments oldest-first. Also cover two
 * distinct fragments stamped in the same second. */
void
test_source_label_is_independent_of_refresh_timing(void) {
    const time_t when = 1754500600;
    for (int sameSecond = 0; sameSecond < 2; sameSecond++) {
        for (int incremental = 0; incremental < 2; incremental++) {
            resetStorage();
            RingFixture ring;
            CallHistoryModel model;
            Event_History* item = ring.commit(0, 1201, 1201, when, when + 8);
            DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Old alias");
            if (incremental) {
                model.refresh(ring.state);
            }
            item = ring.commit(0, 1201, 1201, when + (sameSecond ? 0 : 9), when + 15);
            DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "New alias");
            model.refresh(ring.state);
            expect("batched and incremental fragments select the latest alias",
                   model.count() == 1
                       && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                              == QStringLiteral("New alias"));
        }
    }
}

/* The selected label belongs to neither end of this merged span. A restart
 * must retain that provenance, not use the merged start/end for the next merge. */
void
test_source_label_provenance_survives_merged_span_and_relaunch(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754500700;
    ring.commit(0, 1201, 1201, when, when + 3);
    Event_History* item = ring.commit(0, 1201, 1201, when + 10, when + 15);
    DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Middle alias");
    ring.commit(0, 1201, 1201, when + 20, when + 25);
    {
        CallHistoryModel model;
        model.refresh(ring.state);
        expect("unlabelled fragments extend the call without replacing its label",
               model.count() == 1 && model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 25
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Middle alias"));
    }

    CallHistoryModel restored;
    item = ring.commit(1, 1201, 1201, when + 5, when + 8);
    DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Older alias");
    restored.refresh(ring.state);
    expect("backfilled alias newer than the merged start does not replace the selected label",
           restored.count() == 1
               && restored.data(restored.index(0), CallHistoryModel::SourceNameRole).toString()
                      == QStringLiteral("Middle alias"));

    item = ring.commit(1, 1201, 1201, when + 18, when + 19);
    DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Later alias");
    restored.refresh(ring.state);
    expect("newer alias older than the merged end replaces the selected label",
           restored.count() == 1
               && restored.data(restored.index(0), CallHistoryModel::SourceNameRole).toString()
                      == QStringLiteral("Later alias"));

    DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Enriched alias");
    ring.touchCommitted(1);
    restored.refresh(ring.state);
    expect("selected fragment still accepts alias-only enrichment after merging and relaunch",
           restored.count() == 1
               && restored.data(restored.index(0), CallHistoryModel::SourceNameRole).toString()
                      == QStringLiteral("Enriched alias"));
}

/* Logged rows whose talkgroup lies in [lo, hi]. */
int
rows_in_tg_range(const QAbstractItemModel& model, qulonglong lo, qulonglong hi) {
    int n = 0;
    for (int i = 0; i < model.rowCount(); i++) {
        const qulonglong tg = model.data(model.index(i, 0), CallHistoryModel::TgRole).toULongLong();
        n += (tg >= lo && tg <= hi) ? 1 : 0;
    }
    return n;
}

QString
seen_store_path(void) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QLatin1String("/call_history_seen.json");
}

/* A 2010 capture's start: a replay's calls carry the capture's time, years before any live call. */
constexpr time_t kCaptureStart = 1262304000;

/* Clear wipes what the ring held when it was tapped, by the ring's push order. A call the ring
 * takes in after the clear shows however old its stamps are. A replay's calls carry the
 * capture's time, and a watermark at the clear's decode time hid every one of them. */
void
test_clear_shows_later_calls_whatever_their_stamps(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    ring.commit(0, 5001, 100, 1754501000, 1754501004);
    ring.commit(1, 5002, 101, 1754501010, 1754501014);
    model.refresh(ring.state);
    expect("live calls land before the clear", model.count() == 2);
    model.clearAll();
    expect("clear empties the log", model.count() == 0);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("calls the ring still holds stay cleared", model.count() == 0);

    ring.commit(0, 5003, 200, kCaptureStart, kCaptureStart + 5);
    ring.commit(1, 5004, 201, kCaptureStart + 10, kCaptureStart + 12);
    model.refresh(ring.state);
    expect("calls ingested after a clear show whatever their stamps", model.count() == 2);
    expect("the cleared calls stay cleared", rows_in_tg_range(model, 5001, 5002) == 0);
}

/* Clear has to hold across an Activity restart while the service's ring still holds the
 * cleared calls. The seen store goes before each relaunch, so only the clear's own persisted
 * record can tell the relaunched model which of the ring's rows were wiped. */
void
test_clear_survives_a_relaunch(void) {
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        ring.commit(0, 6001, 100, 1754502000, 1754502004);
        ring.commit(1, 6002, 101, 1754502010, 1754502014);
        model.refresh(ring.state);
        model.clearAll();
    } // destructor flushes the stores
    expect("seen store removed", QFile::remove(seen_store_path()));
    {
        CallHistoryModel relaunched;
        expect("the relaunched log is still empty", relaunched.count() == 0);
        relaunched.refresh(ring.state);
        expect("a relaunch keeps the ring's cleared calls cleared", relaunched.count() == 0);
        ring.commit(0, 6003, 102, kCaptureStart, kCaptureStart + 4);
        relaunched.refresh(ring.state);
        expect("a call ingested after the clear shows after a relaunch, whatever its stamps",
               relaunched.count() == 1 && rows_in_tg_range(relaunched, 6003, 6003) == 1);
    }
    // A relaunched UI can clear before its first tick has read the ring. The clear then
    // covers what that first read finds, and nothing the ring takes in after it.
    expect("seen store removed again", QFile::remove(seen_store_path()));
    {
        CallHistoryModel relaunched;
        expect("the post-clear call was kept", relaunched.count() == 1);
        relaunched.clearAll();
        relaunched.refresh(ring.state);
        expect("a clear before the first read covers what the ring holds", relaunched.count() == 0);
        ring.commit(1, 6004, 103, kCaptureStart + 20, kCaptureStart + 24);
        relaunched.refresh(ring.state);
        expect("and shows what the ring takes in after it",
               relaunched.count() == 1 && rows_in_tg_range(relaunched, 6004, 6004) == 1);
    }
}

/* Older builds persisted Clear as a decode-time watermark and hid every later call stamped
 * before it. Loading retires it: the rows it cleared left the store at the clear, and the ring
 * it guarded did not outlive the process an app update replaces. */
void
test_legacy_clear_watermark_is_retired(void) {
    resetStorage();
    {
        QSettings settings;
        settings.setValue(QStringLiteral("callHistory/clearedThrough"), static_cast<qlonglong>(4102444800LL));
        settings.sync();
    }
    RingFixture ring;
    CallHistoryModel model;
    expect("the legacy watermark is gone after the first load",
           !QSettings().contains(QStringLiteral("callHistory/clearedThrough")));
    ring.commit(0, 8001, 100, 1754504000, 1754504004);
    model.refresh(ring.state);
    expect("a legacy watermark no longer hides calls by their stamps", model.count() == 1);
}

/* A start begins a new session, whose ring counts its pushes from zero again. A clear made in
 * the previous session must not reach it: the new ring's first calls sit at push positions the
 * clear covered in the old one. */
void
test_new_session_starts_fresh(void) {
    resetStorage();
    auto ring = std::make_unique<RingFixture>();
    CallHistoryModel model;
    for (uint32_t i = 0; i < 4; i++) {
        ring->commit(0, 7001 + i, 100 + i, 1754503000 + 10 * i, 1754503004 + 10 * i);
    }
    model.refresh(ring->state);
    model.clearAll();
    const qlonglong clearedIn = model.session();

    // The next session's ring has already passed the clear's push position when it is first read.
    ring = std::make_unique<RingFixture>();
    model.beginSession();
    expect("a start moves the session on", model.session() == clearedIn + 1);
    for (uint32_t i = 0; i < 6; i++) {
        ring->commit(0, 7101 + i, 200 + i, kCaptureStart + 10 * i, kCaptureStart + 4 + 10 * i);
    }
    model.refresh(ring->state);
    expect("the new session's first calls show although the old clear covered their push positions",
           model.count() == 6);
    expect("rows carry the session that logged them",
           model.count() == 6
               && model.data(model.index(0), CallHistoryModel::SessionRole).toLongLong() == model.session());
    {
        CallHistoryModel relaunched;
        expect("the session survives a relaunch", relaunched.session() == model.session());
    }

    // A ring replaced without a start: push_seq never falls within one ring, so a position
    // below the clear's is a ring the clear never saw.
    model.clearAll();
    ring = std::make_unique<RingFixture>();
    ring->commit(0, 7201, 300, kCaptureStart + 100, kCaptureStart + 104);
    model.refresh(ring->state);
    expect("a clear does not reach a ring whose position fell below it", model.count() == 1);
}

/* A clear tapped after a start, before the new ring has been read, covers none of that ring:
 * it counts its pushes from zero, and nothing from it has been taken in. When the previous
 * session heard nothing, both rings sit at the same commit_rev, so the new ring's snapshots look
 * unchanged and are first read at its first commit. That call must still be logged. */
void
test_clear_after_a_quiet_start_keeps_the_next_call(void) {
    resetStorage();
    auto ring = std::make_unique<RingFixture>();
    {
        CallHistoryModel model;
        model.beginSession();
        model.refresh(ring->state); // a session that heard nothing
        model.beginSession();
        ring = std::make_unique<RingFixture>(); // the next session's fresh ring, at the same commit_rev
        model.refresh(ring->state);
        model.clearAll();
        ring->commit(0, 4242, 1, kCaptureStart, kCaptureStart + 4);
        model.refresh(ring->state);
        expect("the first call after a clear in a session that started quiet is logged", model.count() == 1);
        ring->commit(0, 4243, 1, kCaptureStart + 10, kCaptureStart + 14);
        model.refresh(ring->state);
        expect("and so is the next", model.count() == 2);
    }
    CallHistoryModel relaunched;
    relaunched.refresh(ring->state);
    expect("the persisted clear does not hide it after a relaunch either",
           relaunched.count() == 2 && rows_in_tg_range(relaunched, 4242, 4242) == 1);
}

/* The monitor's recent calls select the running session's rows by the session that logged
 * them. A replayed call shows whatever its stamps, a call from before the start does not, and
 * a call heard again after the start joins the session through the merge. */
void
test_session_view_selects_by_session(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    ring.commit(0, 9001, 500, 1754505000, 1754505004);
    ring.commit(0, 9002, 501, 1754505010, 1754505014);
    model.refresh(ring.state);
    model.beginSession();
    view.setHistorySession(model.session());
    expect("the previous session's calls are not the new session's", view.count() == 0);

    // TG 9002's unit keys up again 2 s after its last call: the same conversation.
    ring.commit(0, 9002, 501, 1754505016, 1754505020);
    ring.commit(1, 9003, 502, kCaptureStart, kCaptureStart + 4);
    model.refresh(ring.state);
    expect("the merged call joins the session", model.count() == 3 && rows_in_tg_range(view, 9002, 9002) == 1);
    expect("a replayed call shows whatever its stamps", rows_in_tg_range(view, 9003, 9003) == 1);
    expect("and nothing else from before the start", view.count() == 2);
    view.setHistorySession(0);
    expect("no session selected shows the whole log", view.count() == 3);
}

/* Fill the log with @p batches x 250 live calls from one session, 125 per slot per tick. */
void
fill_live_session(CallHistoryModel& model, RingFixture& ring, int batches, uint32_t tg) {
    for (int b = 0; b < batches; b++) {
        const time_t start = 1754600000 + static_cast<time_t>(b) * 5000;
        ring.commitBatch(0, 125, tg + static_cast<uint32_t>(b) * 250U, 1000, start);
        ring.commitBatch(1, 125, tg + static_cast<uint32_t>(b) * 250U + 125U, 2000, start + 2500);
        model.refresh(ring.state);
    }
}

/* A full log (1000 rows) gives up the oldest session's calls first, not the oldest stamps. A
 * replay's calls sort below every live one, and trimming the bottom dropped them as they landed. */
void
test_full_log_keeps_the_running_session(void) {
    resetStorage();
    auto ring = std::make_unique<RingFixture>();
    CallHistoryModel model;
    fill_live_session(model, *ring, 4, 10000);
    expect("the previous session fills the log", model.count() == 1000);
    model.beginSession();
    ring = std::make_unique<RingFixture>();
    ring->commit(0, 20001, 1, kCaptureStart, kCaptureStart + 4);
    ring->commit(1, 20002, 2, kCaptureStart + 10, kCaptureStart + 14);
    model.refresh(ring->state);
    expect("a full log keeps the running session's calls",
           model.count() == 1000 && rows_in_tg_range(model, 20001, 20002) == 2);
    expect("and gives up the previous session's oldest",
           rows_in_tg_range(model, 10000, 10001) == 0 && rows_in_tg_range(model, 10002, 10002) == 1);
}

/* The seen map keeps the newest sessions' entries, not the newest stamps. Ranked by stamps, a
 * replay's entries went first once the map passed its bound (4096), and the ring rows they stand
 * for came back as new calls on the next scan. */
void
test_seen_map_keeps_the_running_session(void) {
    resetStorage();
    auto ring = std::make_unique<RingFixture>();
    CallHistoryModel model;
    fill_live_session(model, *ring, 16, 40000);
    model.beginSession();
    ring = std::make_unique<RingFixture>();
    ring->commitBatch(0, 100, 30000, 3000, kCaptureStart);
    model.refresh(ring->state);
    expect("the replay's calls land", rows_in_tg_range(model, 30000, 30099) == 100);
    // The next commit on that slot makes the scan re-read its ring.
    ring->commit(0, 30100, 3100, kCaptureStart + 2000, kCaptureStart + 2004);
    model.refresh(ring->state);
    expect("past the seen bound the replay's ring rows are not taken in again",
           rows_in_tg_range(model, 30000, 30100) == 101);
}

/* The persisted seen store keeps the newest 2048 entries in the same order. Fragments a merge
 * absorbed are known only there, so a relaunched model that lost the replay's would log each
 * one again as a call of its own. */
void
test_seen_store_keeps_the_running_session(void) {
    resetStorage();
    auto ring = std::make_unique<RingFixture>();
    {
        CallHistoryModel model;
        fill_live_session(model, *ring, 9, 50000);
        model.clearAll();
        model.beginSession();
        ring = std::make_unique<RingFixture>();
        // One replayed call in two fragments, merged into one row...
        ring->commit(0, 31000, 777, kCaptureStart, kCaptureStart + 4);
        model.refresh(ring->state);
        ring->commit(0, 31000, 777, kCaptureStart + 6, kCaptureStart + 10);
        model.refresh(ring->state);
        expect("the fragments merge", model.count() == 1);
        // ...then later calls from the capture push it below the rows a merge looks at.
        ring->commitBatch(1, 40, 31001, 4000, kCaptureStart + 600);
        model.refresh(ring->state);
        expect("the replay is logged", model.count() == 41);
    }
    CallHistoryModel relaunched;
    relaunched.refresh(ring->state);
    expect("a relaunch does not log an absorbed fragment again",
           relaunched.count() == 41 && rows_in_tg_range(relaunched, 31000, 31000) == 1);
}

/* One replay of a capture: two calls, the second in two fragments the merge folds together, and a
 * data notice, committed in the ring's order. A capture replayed in a fresh state pushes exactly these
 * rows at exactly these push stamps again. */
void
replay_capture(RingFixture& ring) {
    ring.commit(0, 9301, 41, kCaptureStart, kCaptureStart + 4);
    ring.commit(0, 9302, 42, kCaptureStart + 30, kCaptureStart + 34);
    ring.commit(0, 9302, 42, kCaptureStart + 36, kCaptureStart + 40);
    ring.commitNotice(1, kCaptureStart + 50, "2010-01-01 00:00:50 SMS from 42");
}

/* Logged rows whose session is @p session. */
int
rows_in_session(const CallHistoryModel& model, qlonglong session) {
    int n = 0;
    for (int i = 0; i < model.rowCount(); i++) {
        n += model.data(model.index(i), CallHistoryModel::SessionRole).toLongLong() == session ? 1 : 0;
    }
    return n;
}

/* The same capture replayed in two sessions. The second replay's fresh state pushes the same rows at
 * the same push stamps, with the same starts and targets, so it reads exactly as the first did, and at
 * the same commit_rev. Those calls are heard again: they belong to the second session's views. The log
 * keeps one row for each, as it does for any fragment that overlaps a logged call, and that row joins
 * the running session. */
void
test_replay_heard_again_in_a_new_session(void) {
    resetStorage();
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    auto ring = std::make_unique<RingFixture>();
    model.beginSession();
    replay_capture(*ring);
    model.refresh(ring->state);
    view.setHistorySession(model.session());
    expect("the first replay logs two calls and a notice", model.count() == 3 && view.count() == 3);

    model.beginSession();
    view.setHistorySession(model.session());
    ring = std::make_unique<RingFixture>(); // the next start's fresh state
    replay_capture(*ring);
    model.refresh(ring->state);
    expect("a replayed capture's calls show in the next session's views", view.count() == 3);
    expect("the log keeps one row per call and notice", model.count() == 3);
    expect("each logged row joins the session that heard it again", rows_in_session(model, model.session()) == 3);
    expect("the calls are the replay's",
           rows_in_tg_range(view, 9301, 9301) == 1 && rows_in_tg_range(view, 9302, 9302) == 1);

    // Read again in the same ring, they are no new sighting.
    ring->touchCommitted(0);
    model.refresh(ring->state);
    expect("rereading the ring changes nothing", model.count() == 3 && view.count() == 3);
}

/* Clear, then the same capture replayed in the next session: the calls show again, in the log and in
 * the session's views, although the seen map still holds the cleared ring's keys. */
void
test_replay_heard_again_after_a_clear(void) {
    resetStorage();
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    auto ring = std::make_unique<RingFixture>();
    model.beginSession();
    replay_capture(*ring);
    model.refresh(ring->state);
    model.clearAll();
    expect("the clear empties the log", model.count() == 0);

    model.beginSession();
    view.setHistorySession(model.session());
    ring = std::make_unique<RingFixture>();
    replay_capture(*ring);
    model.refresh(ring->state);
    expect("the replayed calls are logged again after a clear", model.count() == 3);
    expect("and show in the session's views", view.count() == 3);

    // A relaunched UI reattaching to that ring logs nothing twice.
    CallHistoryModel relaunched;
    relaunched.refresh(ring->state);
    expect("a relaunch reading the same ring logs nothing twice", relaunched.count() == 3);
}

/* An embedding host can run again on the state its last run used (the Android service reuses it when
 * a start races the previous run's stopSelfLatest()). That ring keeps its identity and the rows the
 * last run pushed, and goes on counting its pushes. Its old rows are the last session's, read again;
 * only what the new run pushes is the new session's. Across a relaunch too, so the ring a seen entry
 * was read from has to survive the seen store. */
void
test_reused_ring_keeps_the_last_sessions_rows(void) {
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        model.beginSession();
        replay_capture(ring);
        model.refresh(ring.state);
        expect("the first run's calls are logged", model.count() == 3);
    } // destructor flushes the stores
    CallHistoryModel relaunched;
    CallHistoryFilterModel view;
    view.setSourceModel(&relaunched);
    relaunched.beginSession();
    view.setHistorySession(relaunched.session());
    ring.commit(0, 9401, 43, kCaptureStart + 600, kCaptureStart + 604);
    relaunched.refresh(ring.state);
    expect("the reused ring's new call is logged", relaunched.count() == 4);
    expect("and only it is the new session's", view.count() == 1 && rows_in_tg_range(view, 9401, 9401) == 1);
}

/* Two identical notices in one second (the same SMS delivered twice) are two rows. Replayed again,
 * both are heard again, and each promotes a twin of its own, so the new session shows both. */
void
test_identical_notices_heard_again_both_join_the_session(void) {
    resetStorage();
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    auto ring = std::make_unique<RingFixture>();
    model.beginSession();
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    model.refresh(ring->state);
    expect("two identical notices log two rows", model.count() == 2);

    model.beginSession();
    view.setHistorySession(model.session());
    ring = std::make_unique<RingFixture>();
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    model.refresh(ring->state);
    expect("both identical notices heard again show in the new session", view.count() == 2);
    expect("and the log keeps two rows", model.count() == 2);

    // Read in two ticks, the second notice still finds the twin the first one left.
    model.beginSession();
    view.setHistorySession(model.session());
    ring = std::make_unique<RingFixture>();
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    model.refresh(ring->state);
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    model.refresh(ring->state);
    expect("both show when read in two ticks", view.count() == 2 && model.count() == 2);
}

/* The same two identical notices replayed again, with the UI relaunched between the two deliveries (an
 * Activity restart while the service decodes). The relaunched model must still know which twin the first
 * delivery took, or the second takes it again and the other logged notice never joins the session. */
void
test_identical_notices_heard_again_across_a_relaunch(void) {
    resetStorage();
    auto ring = std::make_unique<RingFixture>();
    {
        CallHistoryModel model;
        model.beginSession();
        ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
        ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
        model.refresh(ring->state);
        expect("two identical notices log two rows", model.count() == 2);

        model.beginSession();
        ring = std::make_unique<RingFixture>();
        ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
        model.refresh(ring->state);
        expect("the first delivery heard again joins the session", rows_in_session(model, model.session()) == 1);
    } // destructor flushes the stores
    CallHistoryModel relaunched;
    CallHistoryFilterModel view;
    view.setSourceModel(&relaunched);
    view.setHistorySession(relaunched.session());
    relaunched.refresh(ring->state);
    expect("the reattached ring's first delivery logs nothing new", relaunched.count() == 2 && view.count() == 1);
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    relaunched.refresh(ring->state);
    expect("both identical notices heard again show in the session across a relaunch", view.count() == 2);
    expect("and the log keeps two rows across a relaunch", relaunched.count() == 2);
}

/* A new ring within one session, with no start between (a state set up again without a start): its rows
 * repeat the session's own. A notice heard again that way finds its twin already in the session and
 * logs nothing new, as a call heard again does. */
void
test_new_ring_within_a_session_logs_nothing_twice(void) {
    resetStorage();
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    auto ring = std::make_unique<RingFixture>();
    model.beginSession();
    view.setHistorySession(model.session());
    replay_capture(*ring);
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    model.refresh(ring->state);
    expect("the session logs two calls and three notices", model.count() == 5 && view.count() == 5);

    ring = std::make_unique<RingFixture>(); // a new ring, no beginSession()
    replay_capture(*ring);
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    ring->commitNotice(1, kCaptureStart + 70, "2010-01-01 00:01:10 SMS from 44");
    model.refresh(ring->state);
    expect("a new ring in the same session logs no duplicate", model.count() == 5 && view.count() == 5);
}

/* A row's seed in load() knows no ring. When the seen store has lost the row's entry, a relaunched
 * UI reading the state's ring again (a reused state, after a start) must leave the row in the session
 * that logged it: only what the new run pushes is the new session's. */
void
test_seed_without_a_ring_keeps_its_session(void) {
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        model.beginSession();
        ring.commit(0, 9501, 51, kCaptureStart, kCaptureStart + 4);
        ring.commit(0, 9502, 52, kCaptureStart + 30, kCaptureStart + 34);
        model.refresh(ring.state);
        expect("the first run's calls are logged", model.count() == 2);
    } // destructor flushes the stores
    expect("seen store removed", QFile::remove(seen_store_path()));
    CallHistoryModel relaunched;
    CallHistoryFilterModel view;
    view.setSourceModel(&relaunched);
    relaunched.beginSession();
    view.setHistorySession(relaunched.session());
    ring.commit(0, 9503, 53, kCaptureStart + 600, kCaptureStart + 604);
    relaunched.refresh(ring.state);
    expect("the reused ring's new call is logged", relaunched.count() == 3);
    expect("and only it is the new session's", view.count() == 1 && rows_in_tg_range(view, 9503, 9503) == 1);
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    /* Isolated, disposable storage, same pattern as the persistence test. */
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-call-history-%1").arg(QCoreApplication::applicationPid()));
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

    test_system_identity_persistence();
    test_scanning_rows_never_gain_hold_identity();
    test_source_labels_survive_merge_and_relaunch();
    test_source_label_is_independent_of_refresh_timing();
    test_source_label_provenance_survives_merged_span_and_relaunch();
    test_two_slots_same_second_same_talkgroup();
    test_textual_targets_stay_distinct();
    test_alias_only_row_is_logged();
    test_channel_labelled_tg0_row_is_logged();
    test_labelled_notice_name_drops_its_own_channel_prefix();
    test_channel_label_is_its_own_role();
    test_reacquisition_merge_updates_in_place();
    test_ring_walk_gated_on_commit_rev();
    test_relaunch_does_not_reingest();
    test_clear_shows_later_calls_whatever_their_stamps();
    test_clear_survives_a_relaunch();
    test_legacy_clear_watermark_is_retired();
    test_new_session_starts_fresh();
    test_clear_after_a_quiet_start_keeps_the_next_call();
    test_session_view_selects_by_session();
    test_full_log_keeps_the_running_session();
    test_seen_map_keeps_the_running_session();
    test_seen_store_keeps_the_running_session();
    test_replay_heard_again_in_a_new_session();
    test_replay_heard_again_after_a_clear();
    test_reused_ring_keeps_the_last_sessions_rows();
    test_identical_notices_heard_again_both_join_the_session();
    test_identical_notices_heard_again_across_a_relaunch();
    test_new_ring_within_a_session_logs_nothing_twice();
    test_seed_without_a_ring_keeps_its_session();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
