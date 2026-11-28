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
 * fresh, and a full log or seen map gives up the oldest session first. Each call's
 * frequency and access code (issue #575) reach the roles, survive a save and reload,
 * fold across fragments by the fragment they came from, fill in place, and fill a
 * notice heard again. Issue #799: each call's site identity and DMR call channel reach
 * one role, survive a save, a reload and the spill, refuse a hand-edited part that no
 * record could hold, fill in place, keep fragments heard on two sites (or under two
 * channel numberings) apart across a relaunch, and an update of a fragment already
 * folded into a row reaches that row while a conflicting identity it brings is
 * discarded. A site text the core changes alone in place (issue #40: the text that
 * names Capacity Max) re-reads the row, also across a reload and the spill, while a
 * seen store rebuilt from the rows, or one an older build wrote with no text digest,
 * re-reads nothing, and the digest such an entry takes is saved. Saves are bounded by
 * bytes written, not by calls: a large store waits longer between saves, and the
 * moments the log must reach the disk (saveNow(), the app leaving the screen) write it
 * at once, even with a save already in flight.
 * Issue #708: every row has a stable uid that rowForUid() finds wherever the row moves, and
 * carries the ring fragments it absorbed through merges, load folding and reloads; the
 * recording roles come from the recording provider by those fragments and follow its change
 * news (a recording known before its row, one added later, one deleted). A save that fails,
 * in either store, keeps the log owed, is retried on a growing delay without another change,
 * reports once, and a restart after a half-written save logs nothing twice (B6).
 * Issue #809: uidForFragment() finds the row holding a fragment, through a merge and a reload, and none once the row
 * is trimmed or the log cleared. B1: a row keeps its end, so a merged or updated span past the plausible cap reads
 * unknown and stays so (after a later update of an earlier fragment, a reload and a spill fold), a row past the cap
 * chains merges from its start, not its end (a clock-shifted span absorbs nothing), a row an older build stored ends
 * where its known duration says, and a row with no start stamp keeps its duration unknown through an update.
 * Issue #800: a voice row keeps its GPS text and message (B2), a late enrichment re-reads it, and both survive a save,
 * a reload and the spill with the structured position; a fragment's fix joins only its own radio's row; a merged row
 * shows the fix of its newest fragment by run and rev (2.7); two callsign callers (B3), two textual destinations under
 * one service (B4) and two conventional systems (B5) stay apart; a notice decoded again collapses only on the same
 * fix. */

#include <QAbstractItemModel>
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDate>
#include <QDebug>
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
#include <QLocale>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/analog_mode.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "json_store.h"
#include "local_day_watch.h"

#include "call_history_filter.h"
#include "call_history_merge.h"
#include "call_history_model.h"
#include "call_history_payload.h"
#include "call_recording_provider.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "heard_talkgroup_sink.h"
#include "heard_talkgroup_source.h"
#include "teardown_trace_capture.h"

using dsd_qt::CallHistoryFilterModel;
using dsd_qt::CallHistoryModel;
using dsd_qt::LocalDayWatch;

class QThread;

namespace {

int g_failures = 0;

/* The model's once-only save-failure warning, counted and kept out of the output (the tests fail saves on purpose). */
std::atomic<int> g_saveFailedWarnings{0};
QtMessageHandler g_previousHandler = nullptr;

void
count_warnings(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (message.startsWith(QLatin1String("call history: saving the log failed"))) {
        g_saveFailedWarnings++;
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
        // Every row the core commits was read under the reception's received identity epoch (issue #800): one epoch
        // for the whole fixture, as a run with no system change has, unless a test moves it (with_epoch()).
        item->rx_scope.rx_gen = 1U;
        item->rx_scope.epoch_known = 1U;
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
            item->event_start_time = start + 10 * static_cast<time_t>(i);
            item->event_time = start + 10 * static_cast<time_t>(i) + 4;
        }
        ring->push_seq += static_cast<uint64_t>(count);
        ring->commit_rev++;
        ring->revision++;
    }

    /** Give a row the frequency and access code its call was heard with, as the core stamps them. */
    static Event_History*
    carrier(Event_History* item, int64_t freq_hz, uint8_t kind = DSD_ACCESS_CODE_NONE, uint16_t code = 0U) {
        item->freq_hz = freq_hz;
        item->access_code_kind = kind;
        item->access_code = code;
        return item;
    }

    /** Give a row the site its call was heard on and the call's own channel, as the core freezes them (issue #799). */
    static Event_History*
    site(Event_History* item, const dsd_received_identity& id, uint8_t kind = DSD_CALL_CHANNEL_NONE,
         uint32_t channel = 0U) {
        item->site_id = id;
        item->call_channel_kind = kind;
        item->channel = channel;
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
            // Heard under the running scan: the decoder stamps the row a scan member (issue #671).
            ring.commit(0, 1001, 2001, when, when + 4)->scan_member = 1U;
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

/* Issue #671: who held the tuner is the row's own stamp, frozen by the decoder at the call's first render, never the
 * session label or scan flags at the moment the UI happens to ingest the row. A call heard while the operator held the
 * tuner (a paused scan, a tuner release) is Exploring with no saved-system identity, whatever the session is called and
 * however late the row is read; a call begun before the release keeps the session's identity though the release ended
 * it; a call heard under a running scan never gains the saved-system UID, even when the scan stopped before the UI read
 * the row (N11); and a call heard before a scan started keeps it, though the scan runs when the row is read. */
void
test_tuner_owner_stamps_set_row_identity(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    const time_t when = 1754501000;
    {
        CallHistoryModel model;
        model.setSessionLabel("Saved site");
        model.setSessionUid("saved-site");
        ring.commit(0, 1001, 2001, when, when + 4)->operator_tuned = 1U;
        model.refresh(ring.state, opts.get());
        expect("a call heard while the operator held the tuner is Exploring",
               model.count() == 1 && model.data(model.index(0), CallHistoryModel::SystemNameRole) == "Exploring"
                   && model.data(model.index(0), CallHistoryModel::SystemUidRole).toString().isEmpty());
        ring.commit(0, 1002, 2002, when + 10, when + 14);
        model.refresh(ring.state, opts.get());
        expect("a call the release ended but did not begin keeps the session's identity",
               model.count() == 2 && model.data(model.index(0), CallHistoryModel::SystemNameRole) == "Saved site"
                   && model.data(model.index(0), CallHistoryModel::SystemUidRole) == "saved-site");
        ring.commit(0, 1003, 2003, when + 20, when + 24)->scan_member = 1U;
        model.refresh(ring.state, opts.get());
        expect("a call heard under a scan that stopped before the read gains no UID (N11)",
               model.count() == 3 && model.data(model.index(0), CallHistoryModel::SystemNameRole) == "Saved site"
                   && model.data(model.index(0), CallHistoryModel::SystemUidRole).toString().isEmpty());
        ring.commit(0, 1004, 2004, when + 30, when + 34);
        opts->scanner_mode = 1;
        model.refresh(ring.state, opts.get());
        expect("a call heard before the scan started keeps the UID while the scan runs",
               model.count() == 4 && model.data(model.index(0), CallHistoryModel::SystemUidRole) == "saved-site");
        opts->scanner_mode = 0;
        /* Rows the UI never read before it went away: a relaunched model ingests them with the identity they carry. */
        ring.commit(0, 1005, 2005, when + 40, when + 44)->operator_tuned = 1U;
    }
    CallHistoryModel relaunched;
    relaunched.refresh(ring.state, opts.get());
    expect("an unread operator-tuned row keeps its identity across a relaunch",
           relaunched.count() == 5
               && relaunched.data(relaunched.index(0), CallHistoryModel::SystemNameRole) == "Exploring"
               && relaunched.data(relaunched.index(0), CallHistoryModel::SystemUidRole).toString().isEmpty());
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

/* The emitter puts a notice's CRC/RAS marker ahead of its label, so the channel prefix is found
 * after that marker, which stays. A notice a service name labels (issue #637) carries the service
 * there instead of the channel, and keeps it in its title, the only place the row shows it: search
 * and the accessible name read the title. */
void
test_marked_and_serviced_notice_titles(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    const time_t when = 1754500350;
    Event_History* crc = ring.commitNotice(0, when, "2026-04-30 00:00:00 [CRC ERR] [SiteA] LRRP position", "SiteA");
    crc->crc_invalid = DSD_CRC_FAILED;
    model.refresh(ring.state);
    expect("a CRC-marked notice drops its channel and keeps its marker",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::NameRole).toString()
                      == QStringLiteral("[CRC ERR] LRRP position"));

    Event_History* ras = ring.commitNotice(0, when + 5, "2026-04-30 00:00:05 [RAS] [SiteA] SMS from 1234", "SiteA");
    ras->crc_invalid = DSD_CRC_RAS_UNVERIFIED;
    model.refresh(ring.state);
    expect("a RAS-marked notice drops its channel and keeps its marker",
           model.count() == 2
               && model.data(model.index(0), CallHistoryModel::NameRole).toString()
                      == QStringLiteral("[RAS] SMS from 1234"));

    Event_History* named = ring.commitNotice(0, when + 10, "2026-04-30 00:00:10 [Hospital] LRRP position", "SiteA");
    DSD_SNPRINTF(named->service_label, sizeof(named->service_label), "%s", "Hospital");
    model.refresh(ring.state);
    expect("a serviced notice keeps its service in its title",
           model.count() == 3
               && model.data(model.index(0), CallHistoryModel::NameRole).toString()
                      == QStringLiteral("[Hospital] LRRP position")
               && model.data(model.index(0), CallHistoryModel::ServiceNameRole).toString() == QStringLiteral("Hospital")
               && model.data(model.index(0), CallHistoryModel::ChannelRole).toString() == QStringLiteral("SiteA"));

    Event_History* both =
        ring.commitNotice(0, when + 15, "2026-04-30 00:00:15 [CRC ERR] [Hospital] SMS from 77", "SiteA");
    DSD_SNPRINTF(both->service_label, sizeof(both->service_label), "%s", "Hospital");
    both->crc_invalid = DSD_CRC_FAILED;
    model.refresh(ring.state);
    expect("a CRC-marked serviced notice keeps both",
           model.count() == 4
               && model.data(model.index(0), CallHistoryModel::NameRole).toString()
                      == QStringLiteral("[CRC ERR] [Hospital] SMS from 77"));

    view.setFilterText(QStringLiteral("Hospital"));
    expect("search finds the serviced notices by their service", view.count() == 2);
}

/* Issue #903: two NXDN Type-D calls to one 11-bit group under two prefixes, 03-0005 and 07-0005, from one radio
 * number inside the merge window are two rows; the same prefix twice still folds into one conversation. */
void
test_type_d_prefixes_do_not_merge(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500800;
    Event_History* a = ring.commit(0, 5, 100, when, when + 2, "03-0005");
    a->systype = DSD_SYNC_NXDN_POS;
    DSD_SNPRINTF(a->src_str, sizeof(a->src_str), "%s", "03-0100");
    Event_History* b = ring.commit(0, 5, 100, when + 3, when + 5, "07-0005");
    b->systype = DSD_SYNC_NXDN_POS;
    DSD_SNPRINTF(b->src_str, sizeof(b->src_str), "%s", "07-0100");
    model.refresh(ring.state);
    expect("type-d prefixes: two rows", model.count() == 2);
    Event_History* c = ring.commit(0, 5, 100, when + 6, when + 8, "07-0005");
    c->systype = DSD_SYNC_NXDN_POS;
    DSD_SNPRINTF(c->src_str, sizeof(c->src_str), "%s", "07-0100");
    model.refresh(ring.state);
    expect("type-d same prefix folds", model.count() == 2);
}

/* Issue #903: a confirmed DCR call names no party; its call publishes the text identity "JPN DCR" with no number, so
 * history keeps the row under that name (never "Call to 0"), where a bare sync's identity-less row is dropped. */
void
test_dcr_row_is_logged_by_name(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500600;
    ring.commit(0, 0, 0, when, when + 3)->systype = DSD_SYNC_NXDN_POS;
    model.refresh(ring.state);
    expect("identity-less nxdn row dropped", model.count() == 0);
    ring.commit(0, 0, 0, when + 100, when + 103, "JPN DCR")->systype = DSD_SYNC_NXDN_POS;
    model.refresh(ring.state);
    expect("dcr row logged", model.count() == 1);
    expect("dcr row named",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::NameRole).toString() == QStringLiteral("JPN DCR"));
}

/* Issue #903: a DCR call ("JPN DCR", no numeric party) and an NXDN call that knows a party by number, here only its
 * source, are two calls, as the call layer splits them (call_state_dcr_meets_numbered_party()): two adjacent rows on
 * one named channel stay two rows, each way round. Two DCR rows still fold. */
void
test_dcr_and_numbered_rows_stay_apart(void) {
    for (int pass = 0; pass < 2; pass++) {
        const bool dcrFirst = pass == 1;
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        const time_t when = 1754500700;
        const time_t later = when + 4;
        Event_History* dcr =
            ring.commit(0, 0, 0, dcrFirst ? when : later, dcrFirst ? when + 3 : later + 3, "JPN DCR", "", "Dispatch");
        dcr->systype = DSD_SYNC_NXDN_POS;
        model.refresh(ring.state);
        Event_History* numbered =
            ring.commit(0, 0, 123, dcrFirst ? later : when, dcrFirst ? later + 3 : when + 3, "", "", "Dispatch");
        numbered->systype = DSD_SYNC_NXDN_POS;
        model.refresh(ring.state);
        expect(dcrFirst ? "dcr then source-only: two rows" : "source-only then dcr: two rows", model.count() == 2);
    }

    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500800;
    ring.commit(0, 0, 0, when, when + 3, "JPN DCR", "", "Dispatch")->systype = DSD_SYNC_NXDN_POS;
    model.refresh(ring.state);
    ring.commit(0, 0, 0, when + 4, when + 7, "JPN DCR", "", "Dispatch")->systype = DSD_SYNC_NXDN_POS;
    model.refresh(ring.state);
    expect("two dcr fragments fold", model.count() == 1);
}

/* Issue #903: an anonymous NXDN fragment on a named channel folds into the DCR row beside it (it names no party), but
 * when an update of that fragment names a source, the radio never lands on the DCR row: a DCR call meets no numbered
 * party on an update either (fragment_call_compatible()), so no "JPN DCR" row shows source 123. */
void
test_dcr_row_refuses_a_fragment_that_learns_a_source(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500900;
    ring.commit(0, 0, 0, when, when + 3, "JPN DCR", "", "Dispatch")->systype = DSD_SYNC_NXDN_POS;
    model.refresh(ring.state);
    Event_History* anonymous = ring.commit(0, 0, 0, when + 4, when + 7, "", "", "Dispatch");
    anonymous->systype = DSD_SYNC_NXDN_POS;
    model.refresh(ring.state);
    expect("dcr + anonymous fragment: one row", model.count() == 1);

    anonymous->source_id = 123;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    bool dcrWithSource = false;
    for (int i = 0; i < model.count(); i++) {
        const QModelIndex at = model.index(i);
        if (model.data(at, CallHistoryModel::NameRole).toString() == QStringLiteral("JPN DCR")
            && model.data(at, CallHistoryModel::SrcRole).toUInt() != 0U) {
            dcrWithSource = true;
        }
    }
    expect("dcr row never shows the fragment's later source", !dcrWithSource);
}

/* Issue #903: an NXDN Type-D group's "pp-iiii" text only spells its numeric talkgroup, so the row is named as a bare
 * number is (dsd_call_target_text_is_name()): its list name, else the channel it was heard on, else the text. A text
 * target with no number and an M17 callsign keep naming their rows over the channel. */
void
test_type_d_text_yields_to_names(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754500400;
    ring.commit(0, 5, 100, when, when + 3, "03-0005", "", "Fire Dispatch")->systype = DSD_SYNC_NXDN_POS;
    ring.commit(0, 5, 101, when + 100, when + 103, "03-0005", "Metro Fire", "Fire Dispatch")->systype =
        DSD_SYNC_NXDN_POS;
    ring.commit(0, 5, 102, when + 200, when + 203, "03-0005", "", "")->systype = DSD_SYNC_NXDN_POS;
    ring.commit(0, 0, 103, when + 300, when + 303, "CQCQCQ", "", "Fire Dispatch")->systype = DSD_SYNC_DSTAR_HD_POS;
    ring.commit(0, 123456, 104, when + 400, when + 403, "N0CALL", "", "Fire Dispatch")->systype = DSD_SYNC_M17_LSF_POS;
    model.refresh(ring.state);
    QStringList names;
    for (int i = 0; i < model.count(); i++) {
        names << model.data(model.index(i), CallHistoryModel::NameRole).toString();
    }
    expect("type-d rows: five rows", model.count() == 5);
    expect("type-d rows: channel, list name, text; text and callsign keep theirs",
           names.contains(QStringLiteral("Fire Dispatch")) && names.contains(QStringLiteral("Metro Fire"))
               && names.contains(QStringLiteral("03-0005")) && names.contains(QStringLiteral("CQCQCQ"))
               && names.contains(QStringLiteral("N0CALL")));
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

/* Issue #671: a trunk-scan target's channel label is the app's uid for its entry. A call with no name of its own is
 * named by its channel, so the row shows the entry's name in its place, in the title and the channel role alike; a
 * channel the names do not cover shows as it is. */
void
test_target_ids_read_as_their_names(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    model.setSessionLabel(QStringLiteral("County Scan"));
    model.setChannelNames(QVariantMap{{QStringLiteral("96e4230d-7220-4a76"), QStringLiteral("Benton County")}});
    const time_t when = 1754500300;
    ring.commit(0, 5001, 100, when, when + 3, "", "", "96e4230d-7220-4a76");
    model.refresh(ring.state);
    expect("a target id reads as its name",
           model.count() == 1 && model.data(model.index(0), CallHistoryModel::NameRole).toString() == "Benton County"
               && model.data(model.index(0), CallHistoryModel::ChannelRole).toString() == "Benton County");

    ring.commit(0, 5002, 101, when + 10, when + 12, "", "Fire Ops", "96e4230d-7220-4a76");
    model.refresh(ring.state);
    expect("a named talkgroup keeps its name, its channel the entry's",
           model.count() == 2 && model.data(model.index(0), CallHistoryModel::NameRole).toString() == "Fire Ops"
               && model.data(model.index(0), CallHistoryModel::ChannelRole).toString() == "Benton County");

    ring.commit(0, 5003, 102, when + 20, when + 22, "", "", "Dispatch");
    model.refresh(ring.state);
    expect("a channel the names do not cover shows as it is",
           model.count() == 3 && model.data(model.index(0), CallHistoryModel::NameRole).toString() == "Dispatch");

    /* Two targets of one name are still two channels: the same talkgroup and unit heard on each, close together, stay
       two rows (the name is for display; the row keeps the id it was heard on). */
    model.setChannelNames(QVariantMap{{QStringLiteral("target-a"), QStringLiteral("Dispatch")},
                                      {QStringLiteral("target-b"), QStringLiteral("Dispatch")}});
    ring.commit(0, 5004, 103, when + 30, when + 32, "", "", "target-a");
    ring.commit(0, 5004, 103, when + 33, when + 35, "", "", "target-b");
    model.refresh(ring.state);
    expect("two targets of one name stay two rows", model.count() == 5);
    expect("both read the shared name",
           model.data(model.index(0), CallHistoryModel::ChannelRole).toString() == "Dispatch"
               && model.data(model.index(1), CallHistoryModel::ChannelRole).toString() == "Dispatch");
}

/* Gate review: another session's names come in for a scan list or a saved system started next. The rows an earlier
 * session logged keep their target's name, and match a search for it, instead of reverting to the id; and a row keeps
 * the name it was logged with across a relaunch that names nothing. A newer name for the same id still replaces it. */
void
test_target_names_outlive_their_session(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754500400;
    {
        CallHistoryModel model;
        model.setChannelNames(QVariantMap{{QStringLiteral("96e4230d-7220-4a76"), QStringLiteral("Benton County")}});
        ring.commit(0, 5101, 100, when, when + 3, "", "", "96e4230d-7220-4a76");
        model.refresh(ring.state);
        model.setChannelNames(QVariantMap{{QStringLiteral("0b1f-other-list"), QStringLiteral("Marion County")}});
        expect("an earlier session's row keeps its target's name",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::NameRole).toString() == "Benton County"
                   && model.data(model.index(0), CallHistoryModel::ChannelRole).toString() == "Benton County");
        CallHistoryFilterModel filter;
        filter.setSourceModel(&model);
        filter.setFilterText(QStringLiteral("benton"));
        expect("a search still finds it by name", filter.rowCount() == 1);
    } // destructor flushes the stores
    {
        CallHistoryModel relaunched;
        expect("a relaunch that names nothing keeps the logged name",
               relaunched.count() == 1
                   && relaunched.data(relaunched.index(0), CallHistoryModel::NameRole).toString() == "Benton County");
        relaunched.setChannelNames(
            QVariantMap{{QStringLiteral("96e4230d-7220-4a76"), QStringLiteral("Benton County North")}});
        expect("a newer name for the id replaces it",
               relaunched.data(relaunched.index(0), CallHistoryModel::ChannelRole).toString() == "Benton County North");
    }
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

/* Clear removes the rows rather than resetting the model: a ListView still building a row in
 * its cache buffer cancels it by index on a reset, once the model is already empty. */
void
test_clear_removes_rows_without_a_reset(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    ring.commit(0, 5001, 100, 1754501000, 1754501004);
    ring.commit(1, 5002, 101, 1754501010, 1754501014);
    model.refresh(ring.state);
    expect("two calls before the clear", model.count() == 2);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    model.clearAll();
    expect("clear does not reset the model", resets.count() == 0);
    expect("clear removes every row at once",
           removed.count() == 1 && removed.at(0).at(1).toInt() == 0 && removed.at(0).at(2).toInt() == 1);
    expect("clear empties the log", model.count() == 0);
    model.clearAll();
    expect("clearing an empty log removes nothing", resets.count() == 0 && removed.count() == 1);
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
        ring->commit(0, 7001 + i, 100 + i, 1754503000 + 10 * static_cast<time_t>(i),
                     1754503004 + 10 * static_cast<time_t>(i));
    }
    model.refresh(ring->state);
    model.clearAll();
    const qlonglong clearedIn = model.session();

    // The next session's ring has already passed the clear's push position when it is first read.
    ring = std::make_unique<RingFixture>();
    model.beginSession();
    expect("a start moves the session on", model.session() == clearedIn + 1);
    for (uint32_t i = 0; i < 6; i++) {
        ring->commit(0, 7101 + i, 200 + i, kCaptureStart + 10 * static_cast<time_t>(i),
                     kCaptureStart + 4 + 10 * static_cast<time_t>(i));
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

    // A ring replaced without a start, here at a position below the clear's: its identity says the
    // clear never saw it (test_clear_is_bound_to_its_ring covers one already past the position).
    model.clearAll();
    ring = std::make_unique<RingFixture>();
    ring->commit(0, 7201, 300, kCaptureStart + 100, kCaptureStart + 104);
    model.refresh(ring->state);
    expect("a clear does not reach a ring whose position fell below it", model.count() == 1);
}

/* A clear covers the ring it was made on, named by its identity (Event_History_I::instance). A ring
 * replaced without a start may already sit past the clear's push position at its first read, and a
 * position alone cannot tell it from the cleared ring: all its calls are new. */
void
test_clear_is_bound_to_its_ring(void) {
    resetStorage();
    auto ring = std::make_unique<RingFixture>();
    CallHistoryModel model;
    for (uint32_t i = 0; i < 4; i++) {
        ring->commit(0, 7301 + i, 100 + i, 1754503000 + 10 * static_cast<time_t>(i),
                     1754503004 + 10 * static_cast<time_t>(i));
    }
    model.refresh(ring->state);
    expect("the four calls land before the clear", model.count() == 4);
    model.clearAll();
    ring = std::make_unique<RingFixture>(); // a replacement ring, no beginSession()
    for (uint32_t i = 0; i < 6; i++) {
        ring->commit(0, 7401 + i, 200 + i, kCaptureStart + 10 * static_cast<time_t>(i),
                     kCaptureStart + 4 + 10 * static_cast<time_t>(i));
    }
    model.refresh(ring->state);
    expect("a clear does not reach a replacement ring already past its position", model.count() == 6);
}

/* A clear mark written before marks named their ring cannot say which ring it covers. Its ring died with
 * the process an app update replaced, so it hides nothing: every call the first read finds shows. */
void
test_clear_without_a_ring_hides_nothing(void) {
    resetStorage();
    {
        QSettings settings;
        settings.setValue(QStringLiteral("callHistory/session"), 1);
        settings.setValue(QStringLiteral("callHistory/clear/session"), 1);
        settings.setValue(QStringLiteral("callHistory/clear/pending"), false);
        settings.setValue(QStringLiteral("callHistory/clear/pushSeq0"), 4);
        settings.setValue(QStringLiteral("callHistory/clear/pushSeq1"), 0);
        settings.sync();
    }
    RingFixture ring;
    CallHistoryModel model;
    for (uint32_t i = 0; i < 6; i++) {
        ring.commit(0, 7501 + i, 300 + i, kCaptureStart + 10 * static_cast<time_t>(i),
                    kCaptureStart + 4 + 10 * static_cast<time_t>(i));
    }
    model.refresh(ring.state);
    expect("a clear mark that names no ring hides none of a ring's calls", model.count() == 6);
    expect("and is dropped", !QSettings().contains(QStringLiteral("callHistory/clear/session")));
}

/* A clear tapped after a quiet start covers none of the new ring's later calls. When the previous
 * session heard nothing, both rings sit at the same commit_rev, so only the new ring's identity says
 * its snapshots are new. Its first read must still come before its first commit, which the clear
 * then leaves alone. That call must be logged, and stay so after a relaunch. */
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

/* A start does not always bring a fresh ring. An embedding host can run again on the state its last run
 * used (the Android service reuses it when a start races the previous run's stopSelfLatest()), and that
 * ring keeps its identity, its rows and its push count. A clear bound to that ring must go on covering
 * the rows it wiped. Here the service committed three calls while no UI was alive. A relaunched UI clears
 * before its first tick, and that first read binds the clear to them. A start then reuses the state. */
void
test_pending_clear_survives_a_start_on_a_reused_ring(void) {
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        model.beginSession();
        ring.commit(0, 8101, 1, 1754600000, 1754600004);
        model.refresh(ring.state);
        expect("the first session logs its call", model.count() == 1);
    } // destructor flushes the stores
    ring.commit(0, 8102, 2, 1754600100, 1754600104);
    ring.commit(0, 8103, 3, 1754600200, 1754600204);
    ring.commit(1, 8104, 4, 1754600300, 1754600304);
    CallHistoryModel relaunched;
    CallHistoryFilterModel view;
    view.setSourceModel(&relaunched);
    relaunched.clearAll();
    relaunched.refresh(ring.state);
    expect("the clear before the first read covers what the ring holds", relaunched.count() == 0);
    relaunched.beginSession(); // the start reuses the state, and so the ring
    view.setHistorySession(relaunched.session());
    ring.commit(0, 8105, 5, 1754600400, 1754600404);
    relaunched.refresh(ring.state);
    expect("a start on the reused ring keeps its cleared calls cleared",
           relaunched.count() == 1 && rows_in_tg_range(relaunched, 8101, 8104) == 0);
    expect("and the session shows only the new run's call", view.count() == 1);
}

/* The clear, not the seen store, is what keeps a ring's cleared calls cleared across a relaunch: the
 * store is written later and can lose them. A start that reuses the ring must keep that clear. A fresh
 * ring after a later start still shows its own first calls, whose push positions the clear covered in
 * the old ring. */
void
test_clear_survives_a_start_on_a_reused_ring_without_the_seen_store(void) {
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        model.beginSession();
        ring.commit(0, 8201, 1, 1754600000, 1754600004);
        ring.commit(0, 8202, 2, 1754600100, 1754600104);
        ring.commit(1, 8203, 3, 1754600200, 1754600204);
        model.refresh(ring.state);
        model.clearAll();
    } // destructor flushes the stores
    expect("seen store removed", QFile::remove(seen_store_path()));
    CallHistoryModel relaunched;
    CallHistoryFilterModel view;
    view.setSourceModel(&relaunched);
    relaunched.refresh(ring.state);
    expect("the relaunch keeps the ring's cleared calls cleared", relaunched.count() == 0);
    relaunched.beginSession(); // the start reuses the state, and so the ring
    view.setHistorySession(relaunched.session());
    ring.commit(0, 8204, 4, 1754600300, 1754600304);
    relaunched.refresh(ring.state);
    expect("a start on the reused ring keeps its cleared calls cleared without the seen store",
           relaunched.count() == 1 && rows_in_tg_range(relaunched, 8201, 8203) == 0);
    expect("and the session shows only the new run's call", view.count() == 1);

    relaunched.beginSession();
    view.setHistorySession(relaunched.session());
    RingFixture fresh; // the next start's fresh state
    fresh.commit(0, 8211, 11, 1754600500, 1754600504);
    fresh.commit(0, 8212, 12, 1754600600, 1754600604);
    fresh.commit(1, 8213, 13, 1754600700, 1754600704);
    relaunched.refresh(fresh.state);
    expect("a fresh ring after a start shows its own first calls", view.count() == 3 && relaunched.count() == 4);
}

/* A clear tapped after a start, before the ring's first read. When the start reused the state, the ring
 * the model last read is the one still running, and the clear covers what that read found. Without the
 * seen store, a relaunch must keep those calls cleared. */
void
test_clear_after_a_start_covers_a_reused_ring(void) {
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        model.beginSession();
        ring.commit(0, 8301, 1, 1754600000, 1754600004);
        ring.commit(0, 8302, 2, 1754600100, 1754600104);
        model.refresh(ring.state);
        model.beginSession(); // the start reuses the state, and so the ring
        model.clearAll();
        ring.commit(0, 8303, 3, 1754600200, 1754600204);
        model.refresh(ring.state);
        expect("the reused ring's new call is logged after the clear",
               model.count() == 1 && rows_in_tg_range(model, 8303, 8303) == 1);
    } // destructor flushes the stores
    expect("seen store removed", QFile::remove(seen_store_path()));
    CallHistoryModel relaunched;
    relaunched.refresh(ring.state);
    expect("a relaunch keeps the calls cleared after a start cleared",
           relaunched.count() == 1 && rows_in_tg_range(relaunched, 8301, 8302) == 0);
}

/* As above, with the Activity gone right after the start, before any read of the reused ring. The relaunched
 * UI rejoins the new session, and the clear made in the last one must still cover the ring it was made on. */
void
test_clear_survives_a_relaunch_after_a_start_on_a_reused_ring(void) {
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        model.beginSession();
        ring.commit(0, 8901, 1, 1754600000, 1754600004);
        ring.commit(1, 8902, 2, 1754600100, 1754600104);
        model.refresh(ring.state);
        model.clearAll();
        model.beginSession(); // the start reuses the state, and so the ring
    } // destructor flushes the stores
    expect("seen store removed", QFile::remove(seen_store_path()));
    CallHistoryModel relaunched;
    CallHistoryFilterModel view;
    view.setSourceModel(&relaunched);
    view.setHistorySession(relaunched.session());
    ring.commit(0, 8903, 3, 1754600200, 1754600204);
    relaunched.refresh(ring.state);
    expect("a relaunch after a start on the reused ring keeps its cleared calls cleared",
           relaunched.count() == 1 && rows_in_tg_range(relaunched, 8901, 8902) == 0);
    expect("and the session shows only the new run's call", view.count() == 1);
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

/* The first logged row whose talkgroup is @p tg, or -1. */
int
row_with_tg(const CallHistoryModel& model, qulonglong tg) {
    for (int i = 0; i < model.rowCount(); i++) {
        if (model.data(model.index(i), CallHistoryModel::TgRole).toULongLong() == tg) {
            return i;
        }
    }
    return -1;
}

/* A replayed call heard again folds into its logged row wherever that sits, here under 40 newer live rows,
 * past the newest 32 a live fragment searches. The ring then enriches the call in place (it runs longer, and
 * turns out encrypted). That update belongs to the same row and must reach it, as the sighting did, also
 * when the UI was relaunched between the two. */
void
test_update_of_a_call_heard_again_reaches_its_row(void) {
    for (int relaunch = 0; relaunch < 2; relaunch++) {
        resetStorage();
        auto model = std::make_unique<CallHistoryModel>();
        auto ring = std::make_unique<RingFixture>();
        model->beginSession();
        ring->commit(0, 9601, 61, kCaptureStart, kCaptureStart + 4);
        ring->commitBatch(0, 40, 9700, 1000, 1754600000);
        model->refresh(ring->state);
        expect("the capture's call and 40 newer live calls are logged",
               model->count() == 41 && row_with_tg(*model, 9601) == 40);

        model->beginSession();
        const qlonglong session = model->session();
        ring = std::make_unique<RingFixture>(); // the capture replayed in the next start's fresh state
        Event_History* call = ring->commit(0, 9601, 61, kCaptureStart, kCaptureStart + 4);
        model->refresh(ring->state);
        int row = row_with_tg(*model, 9601);
        expect("the call heard again folds into its logged row under the newer ones",
               model->count() == 41 && row == 40
                   && model->data(model->index(row), CallHistoryModel::SessionRole).toLongLong() == session);

        if (relaunch != 0) {
            model.reset(); // destructor flushes the stores
            model = std::make_unique<CallHistoryModel>();
            model->refresh(ring->state);
            expect("a relaunch reading the same ring logs nothing new", model->count() == 41);
        }
        call->event_time = kCaptureStart + 9;
        call->enc = 1U;
        ring->touchCommitted(0);
        model->refresh(ring->state);
        row = row_with_tg(*model, 9601);
        expect(relaunch != 0 ? "after a relaunch, the call's enrichment reaches its row"
                             : "the call's enrichment reaches its row",
               model->count() == 41 && row >= 0
                   && model->data(model->index(row), CallHistoryModel::DurationSecsRole).toInt() == 9
                   && model->data(model->index(row), CallHistoryModel::EncRole).toBool());
    }
}

/* An old capture replayed for the first time sorts by the capture's stamps, below every newer live row, here
 * 40 of them, past the newest 32 a first sighting searches. That is where its call is logged. The ring then
 * enriches the call in place (it runs longer, learns its source, turns out encrypted), and the update must
 * reach that row, as an update of a live row reaches its own, also after a relaunch between the two. */
void
test_update_of_a_replayed_call_under_newer_rows_reaches_its_row(void) {
    for (int relaunch = 0; relaunch < 2; relaunch++) {
        resetStorage();
        auto model = std::make_unique<CallHistoryModel>();
        RingFixture live;
        model->beginSession();
        live.commitBatch(0, 40, 9700, 1000, 1754600000);
        model->refresh(live.state);
        model->beginSession();
        RingFixture capture; // the replay's fresh state
        Event_History* call = capture.commit(0, 9801, 0, kCaptureStart, kCaptureStart + 4);
        model->refresh(capture.state);
        expect("a replayed call is logged below the 40 newer live calls",
               model->count() == 41 && row_with_tg(*model, 9801) == 40);

        if (relaunch != 0) {
            model.reset(); // destructor flushes the stores
            model = std::make_unique<CallHistoryModel>();
            model->refresh(capture.state);
            expect("a relaunch reading the replay's ring logs nothing new", model->count() == 41);
        }
        call->event_time = kCaptureStart + 9;
        call->source_id = 71;
        call->enc = 1U;
        capture.touchCommitted(0);
        model->refresh(capture.state);
        const int row = row_with_tg(*model, 9801);
        expect(relaunch != 0 ? "after a relaunch, the replayed call's enrichment reaches its row"
                             : "the replayed call's enrichment reaches its row",
               model->count() == 41 && row >= 0
                   && model->data(model->index(row), CallHistoryModel::DurationSecsRole).toInt() == 9
                   && model->data(model->index(row), CallHistoryModel::SrcRole).toULongLong() == 71U
                   && model->data(model->index(row), CallHistoryModel::EncRole).toBool());
    }
}

/* The control: an update of a live call still lands on that call's row, the newest one it merges with, and
 * not on an older row of the same talkgroup and source further down that it would also merge with. Here that
 * older row is a long call that spans the newer one, with 40 other calls between them in the log. */
void
test_update_of_a_live_call_keeps_landing_on_its_row(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    model.beginSession();
    ring.commit(0, 9901, 81, 1754700000, 1754700200); // a 200 s call
    model.refresh(ring.state);
    for (uint32_t i = 0; i < 40; i++) {
        ring.commit(1, 9910 + i, 82 + i, 1754700001 + i, 1754700003 + i);
    }
    model.refresh(ring.state);
    Event_History* call = ring.commit(0, 9901, 81, 1754700100, 1754700104);
    model.refresh(ring.state);
    expect("the later call of the same talkgroup is a row of its own above the long one",
           model.count() == 42 && rows_in_tg_range(model, 9901, 9901) == 2 && row_with_tg(model, 9901) == 0);

    call->event_time = 1754700110;
    call->enc = 1U;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    const int top = row_with_tg(model, 9901);
    int older = -1;
    for (int i = top + 1; i < model.rowCount(); i++) {
        if (model.data(model.index(i), CallHistoryModel::TgRole).toULongLong() == 9901U) {
            older = i;
        }
    }
    expect("the live update lands on its own row",
           model.count() == 42 && top == 0
               && model.data(model.index(top), CallHistoryModel::DurationSecsRole).toInt() == 10
               && model.data(model.index(top), CallHistoryModel::EncRole).toBool());
    expect("and leaves the older row of the same talkgroup alone",
           older > top && model.data(model.index(older), CallHistoryModel::DurationSecsRole).toInt() == 200
               && !model.data(model.index(older), CallHistoryModel::EncRole).toBool());
}

/* What one row shows for its frequency and access code. */
struct CarrierView {
    qint64 freqHz;
    QString accessCode;
    QString accessCodeText;
};

CarrierView
carrier_at(const CallHistoryModel& model, int row) {
    const QModelIndex idx = model.index(row);
    return CarrierView{model.data(idx, CallHistoryModel::FreqHzRole).toLongLong(),
                       model.data(idx, CallHistoryModel::AccessCodeRole).toString(),
                       model.data(idx, CallHistoryModel::AccessCodeTextRole).toString()};
}

bool
carrier_is(const CallHistoryModel& model, int row, qint64 freqHz, const char* accessCode, const char* accessCodeText) {
    if (row < 0 || row >= model.rowCount()) {
        return false;
    }
    const CarrierView view = carrier_at(model, row);
    return view.freqHz == freqHz && view.accessCode == QLatin1String(accessCode)
           && view.accessCodeText == QLatin1String(accessCodeText);
}

/* Whether @p spy saw a dataChanged naming every one of @p roles. */
bool
data_changed_with(const QSignalSpy& spy, std::initializer_list<int> roles) {
    for (const QList<QVariant>& arguments : spy) {
        const QVector<int> changed = qvariant_cast<QVector<int>>(arguments.at(2));
        bool all = true;
        for (int role : roles) {
            all = all && changed.contains(role);
        }
        if (all) {
            return true;
        }
    }
    return false;
}

QJsonObject
stored_row_with_tg(const QJsonArray& rows, qulonglong tg) {
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        if (row.value(QLatin1String("tg")).toVariant().toULongLong() == tg) {
            return row;
        }
    }
    return QJsonObject();
}

/* Issue #575: each call shows the frequency and access code it was heard with. The model carries the kind and value;
 * the text comes from app-control at read time, so every frontend spells a code alike. */
void
test_carrier_roles_and_text(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const QHash<int, QByteArray> names = model.roleNames();
    expect("the frequency role is named freqHz", names.value(CallHistoryModel::FreqHzRole) == "freqHz");
    expect("the code role is named accessCode", names.value(CallHistoryModel::AccessCodeRole) == "accessCode");
    expect("the long code role is named accessCodeText",
           names.value(CallHistoryModel::AccessCodeTextRole) == "accessCodeText");

    const time_t when = 1754510000;
    RingFixture::carrier(ring.commit(0, 11001, 501, when, when + 4), 851012500, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    RingFixture::carrier(ring.commit(0, 11002, 502, when + 30, when + 34), 852500000, DSD_ACCESS_CODE_NAC, 0x293U);
    RingFixture::carrier(ring.commit(0, 11003, 503, when + 60, when + 64), 853000000, DSD_ACCESS_CODE_NAC, 0x05U);
    ring.commit(0, 11004, 504, when + 90, when + 94);
    model.refresh(ring.state);
    expect("four calls land", model.count() == 4);
    expect("a DMR call shows its frequency and colour code",
           carrier_is(model, row_with_tg(model, 11001), 851012500, "CC 1", "Color code 1"));
    expect("a P25 call shows its NAC in hex",
           carrier_is(model, row_with_tg(model, 11002), 852500000, "NAC 293", "Network access code 293"));
    expect("a NAC keeps three hex digits",
           carrier_is(model, row_with_tg(model, 11003), 853000000, "NAC 005", "Network access code 005"));
    expect("a call heard with neither shows nothing", carrier_is(model, row_with_tg(model, 11004), 0, "", ""));
    expect("the frequency role is a qint64",
           model.data(model.index(row_with_tg(model, 11001)), CallHistoryModel::FreqHzRole).typeId()
               == QMetaType::LongLong);
}

/* The kind, value and each field's provenance persist; the text never does. A field nobody knows writes no key. */
void
test_carrier_survives_save_and_reload(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754511000;
    {
        CallHistoryModel model;
        RingFixture::carrier(ring.commit(0, 12001, 601, when, when + 4), 851012500, DSD_ACCESS_CODE_RAN, 5U);
        RingFixture::carrier(ring.commit(1, 12002, 602, when + 30, when + 34), 0, DSD_ACCESS_CODE_CAN, 0U);
        RingFixture::carrier(ring.commit(0, 12003, 603, when + 60, when + 64), 446006250);
        ring.commit(1, 12004, 604, when + 90, when + 94);
        model.refresh(ring.state);
        expect("four calls land before the save", model.count() == 4);
    } // destructor flushes the stores
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    const QJsonObject both = stored_row_with_tg(rows, 12001);
    expect("a known frequency and code are stored with their provenance",
           both.value(QLatin1String("freqHz")).toVariant().toLongLong() == 851012500
               && both.value(QLatin1String("acKind")).toInt() == DSD_ACCESS_CODE_RAN
               && both.value(QLatin1String("ac")).toInt() == 5 && both.contains(QLatin1String("freqWhen"))
               && both.contains(QLatin1String("freqSeq")) && both.contains(QLatin1String("freqSlot"))
               && both.contains(QLatin1String("acWhen")) && both.contains(QLatin1String("acSeq"))
               && both.contains(QLatin1String("acSlot")));
    expect("the display text is never stored",
           !both.contains(QLatin1String("accessCode")) && !both.contains(QLatin1String("accessCodeText")));
    const QJsonObject codeOnly = stored_row_with_tg(rows, 12002);
    expect("CAN 0 is stored as a code", codeOnly.value(QLatin1String("acKind")).toInt() == DSD_ACCESS_CODE_CAN
                                            && codeOnly.contains(QLatin1String("ac")));
    expect("an unknown frequency writes none of its keys",
           !codeOnly.contains(QLatin1String("freqHz")) && !codeOnly.contains(QLatin1String("freqWhen"))
               && !codeOnly.contains(QLatin1String("freqSeq")) && !codeOnly.contains(QLatin1String("freqSlot")));
    const QJsonObject freqOnly = stored_row_with_tg(rows, 12003);
    expect("an unknown code writes none of its keys",
           !freqOnly.contains(QLatin1String("acKind")) && !freqOnly.contains(QLatin1String("ac"))
               && !freqOnly.contains(QLatin1String("acWhen")) && !freqOnly.contains(QLatin1String("acSeq"))
               && !freqOnly.contains(QLatin1String("acSlot")));
    const QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    int seenWithBoth = 0;
    int seenWithNeither = 0;
    for (const auto& value : seen) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QLatin1String("freqHz")).toVariant().toLongLong() == 851012500
            && entry.value(QLatin1String("acKind")).toInt() == DSD_ACCESS_CODE_RAN
            && entry.value(QLatin1String("ac")).toInt() == 5) {
            seenWithBoth++;
        }
        if (!entry.contains(QLatin1String("freqHz")) && !entry.contains(QLatin1String("acKind"))
            && !entry.contains(QLatin1String("ac"))) {
            seenWithNeither++;
        }
    }
    expect("the seen store records what was read", seenWithBoth == 1);
    expect("and omits what was not", seenWithNeither == 1);

    CallHistoryModel restored;
    expect("the log reloads", restored.count() == 4);
    expect("frequency and code survive a reload",
           carrier_is(restored, row_with_tg(restored, 12001), 851012500, "RAN 5", "Radio access number 5"));
    expect("a code alone survives a reload",
           carrier_is(restored, row_with_tg(restored, 12002), 0, "CAN 0", "Channel access number 0"));
    expect("a frequency alone survives a reload",
           carrier_is(restored, row_with_tg(restored, 12003), 446006250, "", ""));
    expect("neither stays neither", carrier_is(restored, row_with_tg(restored, 12004), 0, "", ""));
    /* The seen store's frequency and code are what the ring rows are read against: lost, each ring row that knows
     * one would read as advanced and re-ingest as an update. */
    QSignalSpy changed(&restored, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    restored.refresh(ring.state);
    expect("the reloaded log ingests nothing twice", restored.count() == 4);
    expect("the reloaded seen store knows each row's frequency and code, so nothing re-reads as an update",
           changed.count() == 0);
}

/* A store an older build wrote has none of the keys: its rows load with neither field known. A row that has a
 * frequency but no provenance takes its own (start, push stamp, slot), as srcNameWhen does, so a fragment older than
 * the row does not replace it. */
void
test_store_without_carrier_keys_loads_unknown(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754512000;
    {
        CallHistoryModel model;
        RingFixture::carrier(ring.commit(0, 13001, 701, when, when + 4), 851012500, DSD_ACCESS_CODE_NAC, 0x293U);
        RingFixture::carrier(ring.commit(0, 13002, 702, when + 100, when + 104), 852000000, DSD_ACCESS_CODE_NAC,
                             0x293U);
        model.refresh(ring.state);
        expect("two calls land", model.count() == 2);
    } // destructor flushes the stores
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    for (int i = 0; i < rows.size(); i++) {
        QJsonObject row = rows.at(i).toObject();
        const bool keepFrequency = row.value(QLatin1String("tg")).toVariant().toULongLong() == 13002U;
        for (const char* key :
             {"freqHz", "freqWhen", "freqSeq", "freqSlot", "acKind", "ac", "acWhen", "acSeq", "acSlot"}) {
            if (!(keepFrequency && QLatin1String(key) == QLatin1String("freqHz"))) {
                row.remove(QLatin1String(key));
            }
        }
        rows.replace(i, row);
    }
    expect("an older build's history saved", dsd_qt::json_store_save_array("call_history.json", rows));
    QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    for (int i = 0; i < seen.size(); i++) {
        QJsonObject entry = seen.at(i).toObject();
        entry.remove(QLatin1String("freqHz"));
        entry.remove(QLatin1String("acKind"));
        entry.remove(QLatin1String("ac"));
        seen.replace(i, entry);
    }
    expect("an older build's seen store saved", dsd_qt::json_store_save_array("call_history_seen.json", seen));

    CallHistoryModel restored;
    expect("the older store loads", restored.count() == 2);
    expect("a row without the keys shows neither", carrier_is(restored, row_with_tg(restored, 13001), 0, "", ""));
    expect("a frequency without its provenance still loads",
           carrier_is(restored, row_with_tg(restored, 13002), 852000000, "", ""));

    /* A fragment of the same call that started before the stored row (a backfilled one) folds in: it is older than
     * the row's own start, so its frequency does not replace the row's, and its code fills the unknown one. */
    RingFixture other;
    RingFixture::carrier(other.commit(0, 13002, 702, when + 98, when + 99), 853000000, DSD_ACCESS_CODE_NAC, 0x294U);
    restored.refresh(other.state);
    expect("the older fragment merges into the row", restored.count() == 2);
    expect("an older fragment keeps the stored frequency and fills the code",
           carrier_is(restored, row_with_tg(restored, 13002), 852000000, "NAC 294", "Network access code 294"));
}

/* The core fills a committed row's frequency or code in place when a reacquisition merge learns one. The row's key,
 * end and source do not move, so only the seen ratchet can tell the row has advanced. A known value never changes in
 * the core, so a read that changes one is not an advance. */
void
test_carrier_filled_in_place_reingests(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754513000;
    Event_History* item = ring.commit(0, 14001, 801, when, when + 6);
    model.refresh(ring.state);
    expect("the call lands without a frequency or code", model.count() == 1 && carrier_is(model, 0, 0, "", ""));

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    RingFixture::carrier(item, 851012500);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a frequency filled in place updates the row",
           model.count() == 1 && carrier_is(model, 0, 851012500, "", ""));
    expect("the update names the frequency role", data_changed_with(changed, {CallHistoryModel::FreqHzRole}));
    expect("end and source stay", model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 6
                                      && model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 801U);

    changed.clear();
    RingFixture::carrier(item, 851012500, DSD_ACCESS_CODE_COLOR_CODE, 7U);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a code filled in place updates the row",
           model.count() == 1 && carrier_is(model, 0, 851012500, "CC 7", "Color code 7"));
    expect("the update names both code roles",
           data_changed_with(changed, {CallHistoryModel::AccessCodeRole, CallHistoryModel::AccessCodeTextRole}));

    changed.clear();
    RingFixture::carrier(item, 852000000, DSD_ACCESS_CODE_COLOR_CODE, 9U);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a known value read as another is not an update",
           changed.count() == 0 && carrier_is(model, 0, 851012500, "CC 7", "Color code 7"));
}

/* A trunked conversation lands on several voice channels: the fragments still merge into one row, and the row shows
 * the newest fragment's frequency, whichever order a refresh collects them in. A fragment that never learned the code
 * does not erase the one an earlier fragment knew. */
void
test_fragments_on_different_frequencies_merge(void) {
    const time_t when = 1754514000;
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        RingFixture::carrier(ring.commit(0, 15001, 901, when, when + 4), 851012500, DSD_ACCESS_CODE_NAC, 0x293U);
        if (incremental) {
            model.refresh(ring.state);
        }
        // The same slot, so a batched refresh walks the newer fragment first.
        RingFixture::carrier(ring.commit(0, 15001, 901, when + 6, when + 10), 851037500);
        model.refresh(ring.state);
        expect(incremental ? "fragments on two channels merge (incremental)"
                           : "fragments on two channels merge (batched)",
               model.count() == 1);
        expect(incremental ? "the row shows the newest fragment's frequency (incremental)"
                           : "the row shows the newest fragment's frequency (batched)",
               carrier_is(model, 0, 851037500, "NAC 293", "Network access code 293"));
    }
}

/* A notice logged before rows carried the fields, re-delivered from a fresh ring (a replay after an upgrade), is the
 * same delivery: it only joins the running session, and it fills the frequency and code it lacked. Filling is all it
 * does: across rings, push stamps say nothing about which delivery is newer. */
void
test_repeated_notice_gains_carrier(void) {
    resetStorage();
    const time_t when = kCaptureStart + 70;
    {
        CallHistoryModel model;
        model.beginSession();
        RingFixture ring;
        ring.commitNotice(1, when, "2010-01-01 00:01:10 SMS from 44");
        model.refresh(ring.state);
        expect("the notice is logged without the fields", model.count() == 1 && carrier_is(model, 0, 0, "", ""));
    } // destructor flushes the stores
    {
        CallHistoryModel model;
        model.beginSession();
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        expect("dataChanged spy connects", changed.isValid());
        RingFixture ring;
        RingFixture::carrier(ring.commitNotice(1, when, "2010-01-01 00:01:10 SMS from 44"), 851012500,
                             DSD_ACCESS_CODE_COLOR_CODE, 3U);
        model.refresh(ring.state);
        expect("the notice heard again logs no new row", model.count() == 1);
        expect("and joins the running session",
               model.data(model.index(0), CallHistoryModel::SessionRole).toLongLong() == model.session());
        expect("the stored notice gains the frequency and code",
               carrier_is(model, 0, 851012500, "CC 3", "Color code 3"));
        expect("the update names the new roles with the session",
               data_changed_with(changed, {CallHistoryModel::SessionRole, CallHistoryModel::FreqHzRole,
                                           CallHistoryModel::AccessCodeRole, CallHistoryModel::AccessCodeTextRole}));
    }
    {
        CallHistoryModel model;
        expect("the fields survive a save and reload",
               model.count() == 1 && carrier_is(model, 0, 851012500, "CC 3", "Color code 3"));
        model.beginSession();
        RingFixture ring;
        RingFixture::carrier(ring.commitNotice(1, when, "2010-01-01 00:01:10 SMS from 44"), 852000000,
                             DSD_ACCESS_CODE_COLOR_CODE, 4U);
        model.refresh(ring.state);
        expect("a twin with other values does not replace known ones",
               model.count() == 1 && carrier_is(model, 0, 851012500, "CC 3", "Color code 3"));
    }
}

/* Issue #799: site identities as the core freezes them on a row (dsd_rxid_site_snapshot(): site parts only). */
dsd_received_identity
p25_site(uint16_t sysid, uint8_t rfss, uint8_t site, uint32_t wacn = 0U) {
    dsd_received_identity id = {};
    id.family = DSD_RXID_FAMILY_P25;
    id.valid = DSD_RXID_P25_SYSID;
    id.p25_sysid = sysid;
    if (rfss != 0U || site != 0U) {
        id.valid |= DSD_RXID_P25_SITE;
        id.p25_rfss = rfss;
        id.p25_site = site;
    }
    if (wacn != 0U) {
        id.valid |= DSD_RXID_P25_WACN;
        id.p25_wacn = wacn;
    }
    return id;
}

/* A DMR site of trunking system @p trunk: its raw numbers and the text the decoder built from them (Capacity Plus
 * names no network). */
dsd_received_identity
dmr_site(uint8_t trunk, uint16_t net, uint16_t site, const char* text, const char* brand = "", uint8_t model = 0U) {
    dsd_received_identity id = {};
    id.family = DSD_RXID_FAMILY_DMR;
    id.dmr_trunk = trunk;
    id.valid = DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT;
    id.dmr_site = site;
    id.dmr_t3_model = model;
    if (trunk != DSD_RXID_DMR_TRUNK_CAPACITY_PLUS) {
        id.valid |= DSD_RXID_DMR_NET;
        id.dmr_net = net;
    }
    DSD_SNPRINTF(id.dmr_site_text, sizeof(id.dmr_site_text), "%s", text);
    if (brand[0] != '\0') {
        id.valid |= DSD_RXID_DMR_BRAND;
        DSD_SNPRINTF(id.dmr_brand, sizeof(id.dmr_brand), "%s", brand);
    }
    return id;
}

dsd_received_identity
con_plus_site(uint16_t net, uint16_t site) {
    char text[64];
    DSD_SNPRINTF(text, sizeof(text), "Con+ \xC2\xB7 Net %u \xC2\xB7 Site %u", (unsigned)net, (unsigned)site);
    return dmr_site(DSD_RXID_DMR_TRUNK_CONNECT_PLUS, net, site, text, "Connect Plus");
}

/* A Tier III site: raw network and site numbers, shown one-based in the text (or split by -D). */
dsd_received_identity
tier3_site(uint16_t net, uint16_t site, const char* text) {
    return dmr_site(DSD_RXID_DMR_TRUNK_TIER3, net, site, text, "", 1U);
}

dsd_received_identity
tetra_site(uint16_t mcc, uint16_t mnc, uint16_t la) {
    dsd_received_identity id = {};
    id.family = DSD_RXID_FAMILY_TETRA;
    id.valid = DSD_RXID_TETRA_NET | DSD_RXID_TETRA_LA;
    id.tetra_mcc = mcc;
    id.tetra_mnc = mnc;
    id.tetra_colour = 1U;
    id.tetra_la = la;
    return id;
}

QString
u8(const char* text) {
    return QString::fromUtf8(text);
}

QString
site_text(const CallHistoryModel& model, int row) {
    if (row < 0 || row >= model.rowCount()) {
        return QStringLiteral("<no row>");
    }
    return model.data(model.index(row), CallHistoryModel::SiteTextRole).toString();
}

int
rows_of_tg(const CallHistoryModel& model, qulonglong tg) {
    int rows = 0;
    for (int i = 0; i < model.rowCount(); i++) {
        rows += model.data(model.index(i), CallHistoryModel::TgRole).toULongLong() == tg ? 1 : 0;
    }
    return rows;
}

/* The site texts of every row of talkgroup @p tg, sorted. */
QStringList
site_texts_of_tg(const CallHistoryModel& model, qulonglong tg) {
    QStringList texts;
    for (int i = 0; i < model.rowCount(); i++) {
        if (model.data(model.index(i), CallHistoryModel::TgRole).toULongLong() == tg) {
            texts.append(site_text(model, i));
        }
    }
    texts.sort();
    return texts;
}

/* Issue #799: a row names the system and site its call was heard on, and the DMR call's own channel, in one line:
 * the Monitor's words, from app-control, spelled when read. */
void
test_site_roles_and_text(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    expect("the site role is named siteText", model.roleNames().value(CallHistoryModel::SiteTextRole) == "siteText");
    const time_t when = 1754520000;
    RingFixture::site(ring.commit(0, 16001, 1001, when, when + 4), p25_site(0x1A3, 3, 21, 0xBEE00));
    RingFixture::site(ring.commit(0, 16002, 1002, when + 30, when + 34), con_plus_site(3, 5), DSD_CALL_CHANNEL_LCN, 3U);
    RingFixture::site(ring.commit(0, 16003, 1003, when + 60, when + 64), dsd_received_identity{}, DSD_CALL_CHANNEL_LPCN,
                      12U);
    RingFixture::site(ring.commit(0, 16004, 1004, when + 90, when + 94), tetra_site(901, 9999, 12));
    // Another protocol's channel number is in no DMR numbering: there is nothing to name it by.
    RingFixture::site(ring.commit(0, 16005, 1005, when + 120, when + 124), p25_site(0x1A3, 0, 0), DSD_CALL_CHANNEL_NONE,
                      7U);
    ring.commit(0, 16006, 1006, when + 150, when + 154);
    RingFixture::site(ring.commitNotice(0, when + 180, "2025-08-06 12:00:00 SMS from 44"), p25_site(0x1A3, 3, 21));
    model.refresh(ring.state);
    expect("seven rows land", model.count() == 7);
    expect("a P25 call names its system, site and network",
           site_text(model, row_with_tg(model, 16001))
               == u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 3 \xC2\xB7 SITE 21 \xC2\xB7 WACN BEE00"));
    expect("a Connect Plus call names its site and its LCN",
           site_text(model, row_with_tg(model, 16002))
               == u8("DMR \xC2\xB7 Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5 \xC2\xB7 Connect Plus \xC2\xB7 LCN 3"));
    expect("a channel heard with no identity is a DMR one",
           site_text(model, row_with_tg(model, 16003)) == u8("DMR \xC2\xB7 LPCN 12"));
    expect("a TETRA call names its network and location area",
           site_text(model, row_with_tg(model, 16004))
               == u8("TETRA \xC2\xB7 MCC 901 \xC2\xB7 MNC 9999 \xC2\xB7 LA 12"));
    expect("a channel with no numbering adds nothing",
           site_text(model, row_with_tg(model, 16005)) == u8("P25 \xC2\xB7 SYS 1A3"));
    expect("a call heard with neither shows nothing", site_text(model, row_with_tg(model, 16006)).isEmpty());
    expect("a notice keeps no site",
           model.data(model.index(0), CallHistoryModel::KindRole).toInt() == CallHistoryModel::KindNotice
               && site_text(model, 0).isEmpty());
}

/* The raw parts and their bounded texts persist, the line itself never does; a row with no site writes no key. The
 * seen store keeps what was read, so a reload reads nothing as an update. */
void
test_site_survives_save_and_reload(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754521000;
    {
        CallHistoryModel model;
        RingFixture::site(ring.commit(0, 17001, 1701, when, when + 4), p25_site(0x1A3, 3, 21, 0xBEE00));
        RingFixture::site(ring.commit(1, 17002, 1702, when + 30, when + 34), con_plus_site(3, 5), DSD_CALL_CHANNEL_LCN,
                          3U);
        RingFixture::site(ring.commit(0, 17003, 1703, when + 60, when + 64), dsd_received_identity{},
                          DSD_CALL_CHANNEL_LPCN, 12U);
        ring.commit(1, 17004, 1704, when + 90, when + 94);
        model.refresh(ring.state);
        expect("four calls land before the save", model.count() == 4);
    } // destructor flushes the stores
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    const QJsonObject p25 = stored_row_with_tg(rows, 17001).value(QLatin1String("site")).toObject();
    expect("a site is stored by its parts", p25.value(QLatin1String("family")).toInt() == DSD_RXID_FAMILY_P25
                                                && p25.value(QLatin1String("sysid")).toInt() == 0x1A3
                                                && p25.value(QLatin1String("rfss")).toInt() == 3
                                                && p25.value(QLatin1String("site")).toInt() == 21
                                                && p25.value(QLatin1String("wacn")).toInt() == 0xBEE00);
    const QJsonObject conPlusRow = stored_row_with_tg(rows, 17002);
    const QJsonObject conPlus = conPlusRow.value(QLatin1String("site")).toObject();
    expect("a DMR site keeps its raw numbers and its texts",
           conPlus.value(QLatin1String("dmr_net")).toInt() == 3 && conPlus.value(QLatin1String("dmr_site")).toInt() == 5
               && conPlus.value(QLatin1String("dmr_trunk")).toInt() == DSD_RXID_DMR_TRUNK_CONNECT_PLUS
               && conPlus.value(QLatin1String("dmr_site_text")).toString() == u8("Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5")
               && conPlus.value(QLatin1String("dmr_brand")).toString() == QLatin1String("Connect Plus"));
    expect("the channel is stored with its kind and provenance",
           conPlusRow.value(QLatin1String("cch")).toInt() == 3
               && conPlusRow.value(QLatin1String("cchKind")).toInt() == DSD_CALL_CHANNEL_LCN
               && conPlusRow.contains(QLatin1String("cchWhen")) && conPlusRow.contains(QLatin1String("cchSeq"))
               && conPlusRow.contains(QLatin1String("cchSlot")));
    expect("the site line is never stored", !conPlusRow.contains(QLatin1String("siteText")));
    const QJsonObject none = stored_row_with_tg(rows, 17004);
    expect("a row with neither writes no key",
           !none.contains(QLatin1String("site")) && !none.contains(QLatin1String("cch"))
               && !none.contains(QLatin1String("cchKind")) && !none.contains(QLatin1String("cchWhen")));
    const QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    int seenWithSite = 0;
    int seenWithChannel = 0;
    int seenWithNeither = 0;
    for (const auto& value : seen) {
        const QJsonObject entry = value.toObject();
        seenWithSite += entry.value(QLatin1String("sm")).toInteger()
                                == static_cast<qint64>(DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE | DSD_RXID_P25_WACN)
                            ? 1
                            : 0;
        seenWithChannel += entry.value(QLatin1String("cch")).toInt() == 12
                                   && entry.value(QLatin1String("cchKind")).toInt() == DSD_CALL_CHANNEL_LPCN
                               ? 1
                               : 0;
        seenWithNeither += !entry.contains(QLatin1String("sm")) && !entry.contains(QLatin1String("cch")) ? 1 : 0;
    }
    expect("the seen store records the parts read", seenWithSite == 1);
    expect("and the channel read", seenWithChannel == 1);
    expect("and omits what was not", seenWithNeither == 1);

    CallHistoryModel restored;
    expect("the log reloads", restored.count() == 4);
    expect("a P25 site survives a reload",
           site_text(restored, row_with_tg(restored, 17001))
               == u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 3 \xC2\xB7 SITE 21 \xC2\xB7 WACN BEE00"));
    expect("a DMR site and channel survive a reload",
           site_text(restored, row_with_tg(restored, 17002))
               == u8("DMR \xC2\xB7 Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5 \xC2\xB7 Connect Plus \xC2\xB7 LCN 3"));
    expect("a channel alone survives a reload",
           site_text(restored, row_with_tg(restored, 17003)) == u8("DMR \xC2\xB7 LPCN 12"));
    expect("neither stays neither", site_text(restored, row_with_tg(restored, 17004)).isEmpty());
    QSignalSpy changed(&restored, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    restored.refresh(ring.state);
    expect("the reloaded log ingests nothing twice", restored.count() == 4);
    expect("the reloaded seen store knows each row's site and channel, so nothing re-reads as an update",
           changed.count() == 0);
}

/* A store an older build wrote has no site keys: its rows load with no site, and the ring rows the running service
 * still holds fill them in place (the seen store knew no parts, so each reads as an update of its row). */
void
test_store_without_site_keys_loads_unknown(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754522000;
    {
        CallHistoryModel model;
        RingFixture::site(ring.commit(0, 18001, 1801, when, when + 4), p25_site(0x1A3, 3, 21));
        RingFixture::site(ring.commit(0, 18002, 1802, when + 100, when + 104), con_plus_site(3, 5),
                          DSD_CALL_CHANNEL_LCN, 3U);
        model.refresh(ring.state);
        expect("two calls land", model.count() == 2);
    } // destructor flushes the stores
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    for (int i = 0; i < rows.size(); i++) {
        QJsonObject row = rows.at(i).toObject();
        for (const char* key : {"site", "cch", "cchKind", "cchWhen", "cchSeq", "cchSlot"}) {
            row.remove(QLatin1String(key));
        }
        rows.replace(i, row);
    }
    expect("an older build's history saved", dsd_qt::json_store_save_array("call_history.json", rows));
    QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    for (int i = 0; i < seen.size(); i++) {
        QJsonObject entry = seen.at(i).toObject();
        for (const char* key : {"sm", "cch", "cchKind"}) {
            entry.remove(QLatin1String(key));
        }
        seen.replace(i, entry);
    }
    expect("an older build's seen store saved", dsd_qt::json_store_save_array("call_history_seen.json", seen));

    CallHistoryModel restored;
    expect("the older store loads", restored.count() == 2);
    expect("its rows name no site", site_text(restored, row_with_tg(restored, 18001)).isEmpty()
                                        && site_text(restored, row_with_tg(restored, 18002)).isEmpty());
    restored.refresh(ring.state);
    expect("the ring the service still holds fills the rows in place", restored.count() == 2);
    expect("with their sites",
           site_text(restored, row_with_tg(restored, 18001))
                   == u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 3 \xC2\xB7 SITE 21")
               && site_text(restored, row_with_tg(restored, 18002))
                      == u8("DMR \xC2\xB7 Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5 \xC2\xB7 Connect Plus \xC2\xB7 LCN 3"));
}

/* A stored row for the hand-edited store below: talkgroup @p tg at @p when, with @p site (none when empty). */
QJsonObject
stored_site_row(qulonglong tg, qint64 when, const QJsonValue& site) {
    QJsonObject row;
    row.insert(QLatin1String("when"), when);
    row.insert(QLatin1String("name"), QStringLiteral("Talkgroup %1").arg(tg));
    row.insert(QLatin1String("tg"), static_cast<qint64>(tg));
    row.insert(QLatin1String("src"), 0);
    row.insert(QLatin1String("durationSecs"), 4);
    row.insert(QLatin1String("kind"), CallHistoryModel::KindVoice);
    row.insert(QLatin1String("slot"), 0);
    row.insert(QLatin1String("seq"), static_cast<qint64>(tg));
    row.insert(QLatin1String("session"), 1);
    if (!site.isUndefined()) {
        row.insert(QLatin1String("site"), site);
    }
    return row;
}

QJsonObject
p25_site_json(qint64 sysid) {
    QJsonObject site;
    site.insert(QLatin1String("valid"), static_cast<qint64>(DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE));
    site.insert(QLatin1String("family"), DSD_RXID_FAMILY_P25);
    site.insert(QLatin1String("sysid"), sysid);
    site.insert(QLatin1String("rfss"), 1);
    site.insert(QLatin1String("site"), 2);
    return site;
}

QJsonObject
nxdn_site_json(uint32_t valid) {
    QJsonObject site;
    site.insert(QLatin1String("valid"), static_cast<qint64>(valid));
    site.insert(QLatin1String("family"), DSD_RXID_FAMILY_NXDN);
    return site;
}

QJsonObject
con_plus_site_json(void) {
    QJsonObject site;
    site.insert(QLatin1String("valid"), static_cast<qint64>(DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE
                                                            | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND));
    site.insert(QLatin1String("family"), DSD_RXID_FAMILY_DMR);
    site.insert(QLatin1String("dmr_trunk"), DSD_RXID_DMR_TRUNK_CONNECT_PLUS);
    site.insert(QLatin1String("dmr_net"), 3);
    site.insert(QLatin1String("dmr_site"), 5);
    site.insert(QLatin1String("dmr_t3_model"), 0);
    site.insert(QLatin1String("dmr_site_text"), u8("Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5"));
    site.insert(QLatin1String("dmr_brand"), QStringLiteral("Connect Plus"));
    return site;
}

/* The hand-edited site rows below, each @p site under its own talkgroup, 100 s apart so none merge. */
struct BadSite {
    const char* what;
    QJsonValue site;
};

QList<BadSite>
bad_sites(void) {
    QList<BadSite> bad;
    QJsonObject site = p25_site_json(0x1000);
    bad.append({"a SYSID wider than 12 bits", site});
    bad.append({"SYSID 0", p25_site_json(0)});
    site = p25_site_json(0x1A3);
    site.insert(QLatin1String("valid"),
                static_cast<qint64>(DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE | DSD_RXID_P25_WACN));
    site.insert(QLatin1String("wacn"), static_cast<qint64>(0x100000000LL));
    bad.append({"a WACN wider than its field", site});
    site = p25_site_json(0x1A3);
    site.insert(QLatin1String("valid"), static_cast<qint64>(DSD_RXID_P25_SYSID | DSD_RXID_ACCESS));
    bad.append({"a part that is no site's", site});
    site = p25_site_json(0x1A3);
    site.remove(QLatin1String("rfss"));
    bad.append({"a valid part with no value", site});
    site = p25_site_json(0x1A3);
    site.insert(QLatin1String("sysid"), QStringLiteral("1A3"));
    bad.append({"a value that is no number", site});
    site = p25_site_json(0x1A3);
    site.insert(QLatin1String("rfss"), -1);
    bad.append({"a negative value", site});
    site = p25_site_json(0x1A3);
    site.insert(QLatin1String("sysid"), 419.5);
    bad.append({"a value that is no integer", site});
    site = p25_site_json(0x1A3);
    site.insert(QLatin1String("family"), 9);
    bad.append({"a family no record has", site});
    site = nxdn_site_json(DSD_RXID_NXDN_LOCATION);
    site.insert(QLatin1String("nxdn_category"), 0);
    site.insert(QLatin1String("nxdn_sys"), 0);
    site.insert(QLatin1String("nxdn_site"), 5);
    bad.append({"an NXDN system code the standard reserves", site});
    site.insert(QLatin1String("nxdn_category"), 2);
    site.insert(QLatin1String("nxdn_sys"), 131071);
    site.insert(QLatin1String("nxdn_site"), 31);
    bad.append({"an all-ones NXDN location", site});
    site = nxdn_site_json(DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM);
    site.insert(QLatin1String("idas_site_type"), 1);
    site.insert(QLatin1String("idas_site_code"), 7);
    site.insert(QLatin1String("nxdn_typed_sys"), 100);
    site.insert(QLatin1String("nxdn_typed_site"), 9);
    bad.append({"an IDAS site ID another than its system ID's", site});
    site = nxdn_site_json(DSD_RXID_NXDN_LOCATION | DSD_RXID_NXDN_IDAS_AREA);
    site.insert(QLatin1String("nxdn_category"), 0);
    site.insert(QLatin1String("nxdn_sys"), 12);
    site.insert(QLatin1String("nxdn_site"), 5);
    site.insert(QLatin1String("idas_area"), 1);
    bad.append({"a Type-C location beside an IDAS part", site});
    site = con_plus_site_json();
    site.insert(QLatin1String("dmr_site_text"), QString(64, QLatin1Char('x')));
    bad.append({"a DMR text longer than the record holds", site});
    site = con_plus_site_json();
    site.insert(QLatin1String("valid"), static_cast<qint64>(DSD_RXID_DMR_NET));
    bad.append({"a DMR network without its site", site});
    site = con_plus_site_json();
    site.insert(QLatin1String("dmr_trunk"), 7);
    bad.append({"a DMR trunking system no record has", site});
    QJsonObject tetra;
    tetra.insert(QLatin1String("valid"), static_cast<qint64>(DSD_RXID_TETRA_NET));
    tetra.insert(QLatin1String("family"), DSD_RXID_FAMILY_TETRA);
    tetra.insert(QLatin1String("tetra_mcc"), 1024);
    tetra.insert(QLatin1String("tetra_mnc"), 9999);
    tetra.insert(QLatin1String("tetra_colour"), 1);
    bad.append({"a TETRA MCC wider than 10 bits", tetra});
    bad.append({"a site that is no object", QJsonValue(5)});
    return bad;
}

/* Review focus 3: a hand-edited or corrupt store loads every row, and a site part no record could hold (out of its
 * range or its field, a cross part the record keeps apart, a reserved NXDN code) loads as no site, never as a made-up
 * one; so does a channel with no numbering, and a channel whose numbering is not its site's trunking system's. */
void
test_site_store_rejects_bad_parts(void) {
    resetStorage();
    const qint64 when = 1754523000;
    const QList<BadSite> bad = bad_sites();
    QJsonArray rows;
    rows.append(stored_site_row(19000, when, p25_site_json(0x1A3)));
    for (int i = 0; i < bad.size(); i++) {
        rows.append(stored_site_row(19001 + static_cast<qulonglong>(i), when + INT64_C(100) * (i + 1), bad.at(i).site));
    }
    const qint64 channelsAt = when + 100 * (bad.size() + 1);
    QJsonObject unknownKind = stored_site_row(19100, channelsAt, QJsonValue::Undefined);
    unknownKind.insert(QLatin1String("cch"), 12);
    unknownKind.insert(QLatin1String("cchKind"), 9);
    QJsonObject zeroChannel = stored_site_row(19101, channelsAt + 100, QJsonValue::Undefined);
    zeroChannel.insert(QLatin1String("cch"), 0);
    zeroChannel.insert(QLatin1String("cchKind"), DSD_CALL_CHANNEL_LPCN);
    QJsonObject wideChannel = stored_site_row(19102, channelsAt + 200, QJsonValue::Undefined);
    wideChannel.insert(QLatin1String("cch"), static_cast<qint64>(0x100000000LL));
    wideChannel.insert(QLatin1String("cchKind"), DSD_CALL_CHANNEL_LPCN);
    QJsonObject otherNumbering = stored_site_row(19103, channelsAt + 300, con_plus_site_json());
    otherNumbering.insert(QLatin1String("cch"), 12);
    otherNumbering.insert(QLatin1String("cchKind"), DSD_CALL_CHANNEL_LPCN);
    QJsonObject channelOnly = stored_site_row(19104, channelsAt + 400, QJsonValue::Undefined);
    channelOnly.insert(QLatin1String("cch"), 12);
    channelOnly.insert(QLatin1String("cchKind"), DSD_CALL_CHANNEL_LPCN);
    for (const QJsonObject& row : {unknownKind, zeroChannel, wideChannel, otherNumbering, channelOnly}) {
        rows.append(row);
    }
    expect("a hand-edited history saved", dsd_qt::json_store_save_array("call_history.json", rows));

    CallHistoryModel model;
    expect("every stored row loads", model.count() == static_cast<int>(rows.size()));
    expect("a good site still loads",
           site_text(model, row_with_tg(model, 19000)) == u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 SITE 2"));
    for (int i = 0; i < bad.size(); i++) {
        const QString text = site_text(model, row_with_tg(model, 19001 + static_cast<qulonglong>(i)));
        if (!text.isEmpty()) {
            DSD_FPRINTF(stderr, "FAIL: %s loads as \"%s\"\n", bad.at(i).what, text.toUtf8().constData());
            g_failures++;
        }
    }
    expect("a channel with no numbering loads as none", site_text(model, row_with_tg(model, 19100)).isEmpty());
    expect("channel 0 loads as none", site_text(model, row_with_tg(model, 19101)).isEmpty());
    expect("a channel wider than its field loads as none", site_text(model, row_with_tg(model, 19102)).isEmpty());
    expect("a channel another trunking system numbers loads as none, the site kept",
           site_text(model, row_with_tg(model, 19103))
               == u8("DMR \xC2\xB7 Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5 \xC2\xB7 Connect Plus"));
    expect("a channel alone still loads", site_text(model, row_with_tg(model, 19104)) == u8("DMR \xC2\xB7 LPCN 12"));
}

/* The core adds site parts to a committed row and fills its channel in place (a reacquisition merge). The row's key,
 * end and source do not move, so only the seen ratchet can tell the row has advanced: a part gained, or any change of
 * the channel, even with every stamp and field the same. */
void
test_site_filled_in_place_reingests(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754524000;
    Event_History* item = ring.commit(0, 20001, 2001, when, when + 6);
    model.refresh(ring.state);
    expect("the call lands without a site", model.count() == 1 && site_text(model, 0).isEmpty());

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    const dsd_received_identity site = tier3_site(1, 3, "TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 4");
    RingFixture::site(item, site);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a site filled in place updates the row",
           model.count() == 1 && site_text(model, 0) == u8("DMR \xC2\xB7 TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 4"));
    expect("the update names the site role", data_changed_with(changed, {CallHistoryModel::SiteTextRole}));
    expect("end and source stay", model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 6
                                      && model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 2001U);

    changed.clear();
    RingFixture::site(item, site, DSD_CALL_CHANNEL_LPCN, 12U);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a channel filled in place updates the row",
           site_text(model, 0) == u8("DMR \xC2\xB7 TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 4 \xC2\xB7 LPCN 12")
               && data_changed_with(changed, {CallHistoryModel::SiteTextRole}));

    changed.clear();
    RingFixture::site(item, site, DSD_CALL_CHANNEL_LPCN, 14U);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a channel changed with every stamp the same re-ingests",
           site_text(model, 0) == u8("DMR \xC2\xB7 TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 4 \xC2\xB7 LPCN 14")
               && data_changed_with(changed, {CallHistoryModel::SiteTextRole}));

    changed.clear();
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("nothing new is no update", changed.count() == 0);
}

/* Review focus 5: a radio that roams calls the same talkgroup a few seconds later on another site. Those are two
 * calls, two rows, in any order a refresh collects them. */
void
test_fragments_on_conflicting_sites_stay_apart(void) {
    const time_t when = 1754525000;
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        RingFixture::site(ring.commit(0, 21001, 2101, when, when + 4), p25_site(0x1A3, 1, 1));
        if (incremental) {
            model.refresh(ring.state);
        }
        RingFixture::site(ring.commit(0, 21001, 2101, when + 6, when + 10), p25_site(0x1A3, 1, 2));
        model.refresh(ring.state);
        expect(incremental ? "calls on two sites stay two rows (incremental)"
                           : "calls on two sites stay two rows (batched)",
               model.count() == 2);
        expect(incremental ? "each names its own site (incremental)" : "each names its own site (batched)",
               site_texts_of_tg(model, 21001)
                   == QStringList({u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 SITE 1"),
                                   u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 SITE 2")}));
    }
}

/* A fragment that knew less of the site (its system only, or nothing) is the same call as one that knew more: they
 * merge, and the row keeps every part either knew, in any order. */
void
test_fragment_gains_compatible_site(void) {
    const time_t when = 1754526000;
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        RingFixture::site(ring.commit(0, 22001, 2201, when, when + 4), p25_site(0x1A3, 0, 0));
        if (incremental) {
            model.refresh(ring.state);
        }
        RingFixture::site(ring.commit(0, 22001, 2201, when + 6, when + 10), p25_site(0x1A3, 1, 1, 0xBEE00));
        if (incremental) {
            model.refresh(ring.state);
        }
        ring.commit(0, 22001, 2201, when + 12, when + 16);
        model.refresh(ring.state);
        expect(incremental ? "the fragments merge (incremental)" : "the fragments merge (batched)", model.count() == 1);
        expect(incremental ? "the row keeps every part (incremental)" : "the row keeps every part (batched)",
               site_text(model, 0) == u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 SITE 1 \xC2\xB7 WACN BEE00"));
    }
}

/* load() folds the stored rows again through the same merge: a split on two sites stays split after a relaunch. */
void
test_site_split_survives_relaunch(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754527000;
    {
        CallHistoryModel model;
        RingFixture::site(ring.commit(0, 23001, 2301, when, when + 4), p25_site(0x1A3, 1, 1));
        RingFixture::site(ring.commit(0, 23001, 2301, when + 6, when + 10), p25_site(0x1A3, 1, 2));
        model.refresh(ring.state);
        expect("two sites, two rows", model.count() == 2);
    } // destructor flushes the stores
    CallHistoryModel restored;
    expect("the split survives a relaunch", restored.count() == 2);
    expect("each keeps its site", site_texts_of_tg(restored, 23001)
                                      == QStringList({u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 SITE 1"),
                                                      u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 SITE 2")}));
}

/* The decoder bounds a DMR site text and brand to their fields, so a text can be cut inside a character. The store
 * writes the whole characters only: a cut one would load as U+FFFD, three bytes, and the text would no longer fit its
 * field, losing the whole site on reload. */
void
test_site_text_cut_mid_character_survives_reload(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754529000;
    /* 62 whole bytes and the first byte of a middle dot: 63 bytes, the most the 64-byte field holds. */
    QByteArray text("Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5 \xC2\xB7 ");
    text.append(QByteArray(62 - text.size(), 'x'));
    text.append('\xC2');
    /* 30 whole bytes and the first byte of one: 31 bytes, the most the 32-byte brand holds. */
    QByteArray brand(30, 'b');
    brand.append('\xC2');
    dsd_received_identity id = con_plus_site(3, 5);
    expect("the cut text fills its field", static_cast<size_t>(text.size()) + 1U == sizeof(id.dmr_site_text));
    expect("the cut brand fills its field", static_cast<size_t>(brand.size()) + 1U == sizeof(id.dmr_brand));
    DSD_SNPRINTF(id.dmr_site_text, sizeof(id.dmr_site_text), "%s", text.constData());
    DSD_SNPRINTF(id.dmr_brand, sizeof(id.dmr_brand), "%s", brand.constData());
    {
        CallHistoryModel model;
        RingFixture::site(ring.commit(0, 21501, 2151, when, when + 4), id, DSD_CALL_CHANNEL_LCN, 3U);
        model.refresh(ring.state);
        expect("the call lands", model.count() == 1);
    } // destructor flushes the stores
    const QJsonObject site = stored_row_with_tg(dsd_qt::json_store_load_array("call_history.json"), 21501)
                                 .value(QLatin1String("site"))
                                 .toObject();
    expect("the stored text keeps its whole characters",
           site.value(QLatin1String("dmr_site_text")).toString().toUtf8() == text.left(62));
    expect("the stored brand keeps its whole characters",
           site.value(QLatin1String("dmr_brand")).toString().toUtf8() == brand.left(30));

    CallHistoryModel restored;
    expect("the log reloads", restored.count() == 1);
    const QString line = site_text(restored, row_with_tg(restored, 21501));
    expect("the site survives the reload", line.startsWith(u8("DMR \xC2\xB7 Con+ \xC2\xB7 Net 3")));
    expect("and reads no replacement character", !line.contains(QChar(0xFFFD)));
    expect("the channel survives with it", line.endsWith(u8("\xC2\xB7 LCN 3")));
}

/* A seen entry whose channel names no DMR numbering (a hand-edited or corrupt store) is a channel never read, as the
 * row store's is: a ring row heard with none does not read as an update. */
void
test_seen_store_rejects_unknown_channel_kind(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754529500;
    {
        CallHistoryModel model;
        ring.commit(0, 21601, 2161, when, when + 4);
        model.refresh(ring.state);
        expect("the call lands", model.count() == 1);
    } // destructor flushes the stores
    QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    expect("one seen entry", seen.size() == 1);
    QJsonObject entry = seen.at(0).toObject();
    entry.insert(QLatin1String("cch"), 12);
    entry.insert(QLatin1String("cchKind"), 9);
    seen.replace(0, entry);
    expect("a hand-edited seen store saved", dsd_qt::json_store_save_array("call_history_seen.json", seen));

    CallHistoryModel restored;
    expect("the log reloads", restored.count() == 1);
    QSignalSpy changed(&restored, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    restored.refresh(ring.state);
    expect("the reloaded log ingests nothing twice", restored.count() == 1);
    expect("a channel of no numbering in the seen store is none, so the row does not re-read", changed.count() == 0);
}

/* The background spill carries a row as the rows store writes it: a DMR row, its texts and its channel, spilled as
 * the tap would (the stored row and seen entry) and folded by a relaunched model, shows what it showed, and its seen
 * entry keeps the ring row from reading as an update. */
void
test_site_spill_round_trip(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754528000;
    const QString text = u8("DMR \xC2\xB7 TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 1.4 \xC2\xB7 LPCN 12");
    {
        CallHistoryModel model;
        RingFixture::site(ring.commit(0, 24001, 2401, when, when + 4),
                          tier3_site(12, 3, "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 1.4"), DSD_CALL_CHANNEL_LPCN,
                          12U);
        model.refresh(ring.state);
        expect("the call lands with its site", model.count() == 1 && site_text(model, 0) == text);
    } // destructor flushes the stores
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    const QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    expect("one row and one seen entry stored", rows.size() == 1 && seen.size() == 1);
    QJsonObject seenEntry = seen.at(0).toObject();
    QJsonObject line;
    line.insert(QLatin1String("v"), 1);
    line.insert(QLatin1String("key"), seenEntry.value(QLatin1String("key")));
    seenEntry.remove(QLatin1String("key"));
    line.insert(QLatin1String("seen"), seenEntry);
    line.insert(QLatin1String("row"), rows.at(0));
    QFile::remove(dsd_qt::json_store_path(QStringLiteral("call_history.json")));
    QFile::remove(dsd_qt::json_store_path(QStringLiteral("call_history_seen.json")));
    QFile spill(dsd_qt::json_store_path(QStringLiteral("call_history_spill.jsonl")));
    const QByteArray bytes = QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
    expect("the spill is written", spill.open(QIODevice::WriteOnly) && spill.write(bytes) == bytes.size());
    spill.close();

    CallHistoryModel restored;
    expect("the spilled row folds in with its site and channel",
           restored.count() == 1 && site_text(restored, 0) == text);
    QSignalSpy changed(&restored, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    restored.refresh(ring.state);
    expect("the spilled seen entry knows the site and channel, so the ring row is no update",
           restored.count() == 1 && changed.count() == 0);
}

/* Hazard H1 and limit L8: the core fills a fragment in place after Qt folded it into a row, with a site the row's other
 * fragment never shared. The update still reaches the row that holds the fragment (found by its fragment, not by
 * merging again): the end and source it brings are applied, and its site, with its channel, is discarded whole. A
 * compatible site an update brings is taken in. */
void
test_update_conflicting_after_fold_keeps_row_identity(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754529000;
    const QString text = u8("DMR \xC2\xB7 TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 4 \xC2\xB7 LPCN 12");
    RingFixture::site(ring.commit(0, 25001, 0, when, when + 4),
                      tier3_site(1, 3, "TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 4"), DSD_CALL_CHANNEL_LPCN, 12U);
    Event_History* later = ring.commit(0, 25001, 0, when + 6, when + 10);
    model.refresh(ring.state);
    expect("the two fragments are one call",
           model.count() == 1 && site_text(model, 0) == text
               && model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 10);

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    RingFixture::site(later, tier3_site(1, 4, "TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 5"), DSD_CALL_CHANNEL_LPCN, 20U);
    later->event_time = when + 14;
    later->source_id = 2501;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("the update reaches the row and logs no other", model.count() == 1);
    expect("its end and source are applied",
           model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 14
               && model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 2501U
               && data_changed_with(changed, {CallHistoryModel::DurationSecsRole, CallHistoryModel::SrcRole}));
    expect("its conflicting site and its channel are discarded", site_text(model, 0) == text);

    RingFixture::site(ring.commit(0, 25002, 2502, when + 100, when + 104), p25_site(0x1A3, 0, 0));
    Event_History* fill = ring.commit(0, 25002, 2502, when + 106, when + 110);
    model.refresh(ring.state);
    RingFixture::site(fill, p25_site(0x1A3, 1, 1, 0xBEE00));
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a compatible site an update brings is taken in",
           rows_of_tg(model, 25002) == 1
               && site_text(model, row_with_tg(model, 25002))
                      == u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 SITE 1 \xC2\xB7 WACN BEE00"));
}

/* An update found by its fragment is not merged again (H1), so nothing there checks the source: a fragment of unknown
 * source that folded into one radio's row, then learned another radio, keeps the row's radio and that radio's name.
 * The end it brings is still applied. */
void
test_update_learning_another_source_keeps_row_source(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754532000;
    Event_History* first = ring.commit(0, 27001, 222, when, when + 4);
    DSD_SNPRINTF(first->s_name, sizeof(first->s_name), "%s", "Unit 222");
    Event_History* later = ring.commit(0, 27001, 0, when + 5, when + 8);
    model.refresh(ring.state);
    expect("the unknown-source fragment folds into the radio's row",
           model.count() == 1 && model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 222U
               && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                      == QStringLiteral("Unit 222"));

    later->source_id = 111;
    DSD_SNPRINTF(later->s_name, sizeof(later->s_name), "%s", "Unit 111");
    later->event_time = when + 9;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("the update logs no other row", model.count() == 1);
    expect("the row keeps its radio and that radio's name",
           model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 222U
               && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                      == QStringLiteral("Unit 222"));
    expect("the end the update brings is applied",
           model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 9);
}

/* A channel names its numbering: a channel-only Tier III fragment (LPCN) is not the same call as a Connect Plus
 * fragment (LCN), nor as an LSN one, nor as a P25 site's. Two fragments in one numbering merge, and the row shows the
 * newest fragment's channel. */
void
test_channel_kind_conflict_stays_apart(void) {
    const time_t when = 1754530000;
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        const dsd_received_identity none = {};
        RingFixture::site(ring.commit(0, 26001, 2601, when, when + 4), none, DSD_CALL_CHANNEL_LPCN, 12U);
        RingFixture::site(ring.commit(0, 26002, 2602, when + 100, when + 104), none, DSD_CALL_CHANNEL_LPCN, 12U);
        RingFixture::site(ring.commit(0, 26003, 2603, when + 200, when + 204), p25_site(0x1A3, 1, 1));
        RingFixture::site(ring.commit(0, 26004, 2604, when + 300, when + 304), none, DSD_CALL_CHANNEL_LPCN, 12U);
        if (incremental) {
            model.refresh(ring.state);
        }
        RingFixture::site(ring.commit(0, 26001, 2601, when + 6, when + 10), con_plus_site(3, 5), DSD_CALL_CHANNEL_LCN,
                          3U);
        RingFixture::site(ring.commit(0, 26002, 2602, when + 106, when + 110), none, DSD_CALL_CHANNEL_LSN, 5U);
        RingFixture::site(ring.commit(0, 26003, 2603, when + 206, when + 210), none, DSD_CALL_CHANNEL_LSN, 5U);
        RingFixture::site(ring.commit(0, 26004, 2604, when + 306, when + 310),
                          tier3_site(1, 3, "TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 4"), DSD_CALL_CHANNEL_LPCN, 14U);
        model.refresh(ring.state);
        expect(incremental ? "an LPCN fragment and a Connect Plus one stay apart (incremental)"
                           : "an LPCN fragment and a Connect Plus one stay apart (batched)",
               rows_of_tg(model, 26001) == 2);
        expect(incremental ? "an LPCN fragment and an LSN one stay apart (incremental)"
                           : "an LPCN fragment and an LSN one stay apart (batched)",
               rows_of_tg(model, 26002) == 2);
        expect(incremental ? "a P25 fragment and an LSN one stay apart (incremental)"
                           : "a P25 fragment and an LSN one stay apart (batched)",
               rows_of_tg(model, 26003) == 2);
        expect(incremental ? "two LPCN fragments merge, the newest channel shown (incremental)"
                           : "two LPCN fragments merge, the newest channel shown (batched)",
               rows_of_tg(model, 26004) == 1
                   && site_text(model, row_with_tg(model, 26004))
                          == u8("DMR \xC2\xB7 TIII Small \xC2\xB7 Net 2 \xC2\xB7 Site 4 \xC2\xB7 LPCN 14"));
    }
}

/* Issue #40: a Tier III site of the reporter's network (Large, raw NET 1, SITE 1), as the decoder words it before and
 * after it recognises Capacity Max. Same raw parts, same mask: only the text differs. */
const char k_t3_standard_text[] = "TIII Large \xC2\xB7 Net 2 \xC2\xB7 Site 2 \xC2\xB7 SYS 2101";
const char k_t3_capmax_text[] = "TIII Large \xC2\xB7 Capacity Max \xC2\xB7 Net 1 \xC2\xB7 Site 1 \xC2\xB7 SYS 2101";

dsd_received_identity
t3_large_site(const char* text) {
    return dmr_site(DSD_RXID_DMR_TRUNK_TIER3, 1, 1, text, "", 2U);
}

/* FNV-1a, 32 bits, over a text up to its NUL: what the seen store records of a site text. */
qint64
fnv1a32(const char* text) {
    uint32_t hash = 2166136261U;
    for (const char* p = text; *p != '\0'; p++) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619U;
    }
    return static_cast<qint64>(hash);
}

/* The core can change a committed row's site text in place and nothing else (a reacquisition merge's union taking the
 * text that names Capacity Max): the mask, channel and stamps stay. The seen entry's text digest is what tells the row
 * has advanced. It is compared with the fragment last read, not with the row shown: a fragment read back to the
 * standard text re-reads too, and the row, whose only fragment it is, follows its newest reading (issue #800: a
 * fragment's part is its newest reading, whole). */
void
test_site_text_changed_in_place_reingests(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754524500;
    Event_History* item = RingFixture::site(ring.commit(0, 20101, 2011, when, when + 6),
                                            t3_large_site(k_t3_standard_text), DSD_CALL_CHANNEL_LPCN, 38U);
    model.refresh(ring.state);
    expect(
        "the call lands with the standard text",
        model.count() == 1
            && site_text(model, 0)
                   == u8("DMR \xC2\xB7 TIII Large \xC2\xB7 Net 2 \xC2\xB7 Site 2 \xC2\xB7 SYS 2101 \xC2\xB7 LPCN 38"));

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    RingFixture::site(item, t3_large_site(k_t3_capmax_text), DSD_CALL_CHANNEL_LPCN, 38U);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    const QString capmax =
        u8("DMR \xC2\xB7 TIII Large \xC2\xB7 Capacity Max \xC2\xB7 Net 1 \xC2\xB7 Site 1 \xC2\xB7 SYS 2101 \xC2\xB7 "
           "LPCN 38");
    expect("a text changed with every stamp, part and channel the same re-ingests",
           model.count() == 1 && site_text(model, 0) == capmax
               && data_changed_with(changed, {CallHistoryModel::SiteTextRole}));

    changed.clear();
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("the same text read again is no update", changed.count() == 0);

    changed.clear();
    RingFixture::site(item, t3_large_site(k_t3_standard_text), DSD_CALL_CHANNEL_LPCN, 38U);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a text changed back re-reads, as the fragment last read differs", changed.count() > 0);
    expect(
        "and the row follows the text its only fragment now reads",
        model.count() == 1
            && site_text(model, 0)
                   == u8("DMR \xC2\xB7 TIII Large \xC2\xB7 Net 2 \xC2\xB7 Site 2 \xC2\xB7 SYS 2101 \xC2\xB7 LPCN 38"));
    changed.clear();
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("then reads as no update", changed.count() == 0);
}

/* The seen store keeps the digest of the text read: a relaunched model re-reads a row whose text changed while it was
 * down, and nothing else. A store an older build wrote has no digest: the first read takes the text as it is, without
 * re-reading the row, and a change after it re-reads. */
void
test_site_text_digest_survives_save_and_reload(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754524700;
    const QString standard =
        u8("DMR \xC2\xB7 TIII Large \xC2\xB7 Net 2 \xC2\xB7 Site 2 \xC2\xB7 SYS 2101 \xC2\xB7 LPCN 38");
    const QString capmax =
        u8("DMR \xC2\xB7 TIII Large \xC2\xB7 Capacity Max \xC2\xB7 Net 1 \xC2\xB7 Site 1 \xC2\xB7 SYS 2101 \xC2\xB7 "
           "LPCN 38");
    Event_History* item = nullptr;
    {
        CallHistoryModel model;
        item = RingFixture::site(ring.commit(0, 20201, 2021, when, when + 6), t3_large_site(k_t3_standard_text),
                                 DSD_CALL_CHANNEL_LPCN, 38U);
        /* On the other slot: a commit on this one would move the row the pointer above names. */
        RingFixture::site(ring.commit(1, 20202, 2022, when + 30, when + 34), p25_site(0x1A3, 3, 21));
        model.refresh(ring.state);
        expect("two calls land", model.count() == 2);
    } // destructor flushes the stores
    const QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    int withDigest = 0;
    int withZero = 0;
    for (const auto& value : seen) {
        const QJsonObject entry = value.toObject();
        withDigest += entry.value(QLatin1String("stx")).toInteger(-1) == fnv1a32(k_t3_standard_text) ? 1 : 0;
        withZero +=
            entry.contains(QLatin1String("stx")) && entry.value(QLatin1String("stx")).toInteger(-1) == 0 ? 1 : 0;
    }
    expect("the seen store records the text read as its digest", withDigest == 1);
    expect("and a row read with no text as 0", withZero == 1);

    {
        CallHistoryModel restored;
        QSignalSpy changed(&restored, &QAbstractItemModel::dataChanged);
        expect("dataChanged spy connects", changed.isValid());
        restored.refresh(ring.state);
        expect("the reloaded store knows the text, so nothing re-reads", restored.count() == 2 && changed.count() == 0);
        RingFixture::site(item, t3_large_site(k_t3_capmax_text), DSD_CALL_CHANNEL_LPCN, 38U);
        ring.touchCommitted(0);
        restored.refresh(ring.state);
        expect("a text changed after the relaunch re-reads",
               restored.count() == 2 && site_text(restored, row_with_tg(restored, 20201)) == capmax);
    } // destructor flushes the stores

    /* An older build's store: the row and the ring hold the standard text again, and no digest is known. */
    RingFixture::site(item, t3_large_site(k_t3_standard_text), DSD_CALL_CHANNEL_LPCN, 38U);
    ring.touchCommitted(0);
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    for (int i = 0; i < rows.size(); i++) {
        QJsonObject row = rows.at(i).toObject();
        if (row.value(QLatin1String("tg")).toInteger() == 20201) {
            QJsonObject site = row.value(QLatin1String("site")).toObject();
            site.insert(QLatin1String("dmr_site_text"), u8(k_t3_standard_text));
            row.insert(QLatin1String("site"), site);
            rows.replace(i, row);
        }
    }
    expect("an older build's history saved", dsd_qt::json_store_save_array("call_history.json", rows));
    QJsonArray older = dsd_qt::json_store_load_array("call_history_seen.json");
    for (int i = 0; i < older.size(); i++) {
        QJsonObject entry = older.at(i).toObject();
        entry.remove(QLatin1String("stx"));
        older.replace(i, entry);
    }
    expect("an older build's seen store saved", dsd_qt::json_store_save_array("call_history_seen.json", older));
    CallHistoryModel legacy;
    expect("the older store loads", legacy.count() == 2 && site_text(legacy, row_with_tg(legacy, 20201)) == standard);
    QSignalSpy changed(&legacy, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    legacy.refresh(ring.state);
    expect("a seen entry with no digest takes the text read without re-reading the row",
           legacy.count() == 2 && changed.count() == 0);
    RingFixture::site(item, t3_large_site(k_t3_capmax_text), DSD_CALL_CHANNEL_LPCN, 38U);
    ring.touchCommitted(0);
    legacy.refresh(ring.state);
    expect("a change after that first read re-reads",
           legacy.count() == 2 && site_text(legacy, row_with_tg(legacy, 20201)) == capmax);
}

/* The seen store lost alone: load() seeds each row's entry from the row, its site text digest included, so a relaunch
 * re-reads nothing. A seed holding another digest (0, or another text's) would re-read the Tier III row. */
void
test_site_text_digest_seeded_from_rows(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754529500;
    {
        CallHistoryModel model;
        RingFixture::site(ring.commit(0, 24201, 2421, when, when + 6), t3_large_site(k_t3_capmax_text),
                          DSD_CALL_CHANNEL_LPCN, 38U);
        RingFixture::site(ring.commit(1, 24202, 2422, when + 30, when + 34), p25_site(0x1A3, 3, 21));
        model.refresh(ring.state);
        expect("two calls land", model.count() == 2);
    } // destructor flushes the stores
    expect("only the seen store is deleted",
           QFile::remove(dsd_qt::json_store_path(QStringLiteral("call_history_seen.json")))
               && QFileInfo::exists(dsd_qt::json_store_path(QStringLiteral("call_history.json"))));
    CallHistoryModel restored;
    expect("the rows load", restored.count() == 2);
    QSignalSpy changed(&restored, &QAbstractItemModel::dataChanged);
    expect("dataChanged spy connects", changed.isValid());
    restored.refresh(ring.state);
    expect("entries seeded from the rows re-read nothing", restored.count() == 2 && changed.count() == 0);
}

/* An older build's seen entry takes the digest on the first read, and the seen store is saved with it: the next launch
 * knows the text, so a change made while the app was down re-reads the row instead of being taken silently again. */
void
test_site_text_digest_adopted_is_saved(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754530500;
    Event_History* item = nullptr;
    {
        CallHistoryModel model;
        item = RingFixture::site(ring.commit(0, 24301, 2431, when, when + 6), t3_large_site(k_t3_standard_text),
                                 DSD_CALL_CHANNEL_LPCN, 38U);
        model.refresh(ring.state);
        expect("the call lands", model.count() == 1);
    } // destructor flushes the stores
    QJsonArray older = dsd_qt::json_store_load_array("call_history_seen.json");
    for (int i = 0; i < older.size(); i++) {
        QJsonObject entry = older.at(i).toObject();
        entry.remove(QLatin1String("stx"));
        older.replace(i, entry);
    }
    expect("an older build's seen store saved",
           older.size() == 1 && dsd_qt::json_store_save_array("call_history_seen.json", older));
    {
        CallHistoryModel legacy;
        QSignalSpy changed(&legacy, &QAbstractItemModel::dataChanged);
        expect("dataChanged spy connects", changed.isValid());
        legacy.refresh(ring.state);
        expect("the first read takes the digest without re-reading the row", changed.count() == 0);
    } // destructor flushes what is owed
    const QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    expect("the digest taken is saved",
           seen.size() == 1
               && seen.at(0).toObject().value(QLatin1String("stx")).toInteger(-1) == fnv1a32(k_t3_standard_text));

    RingFixture::site(item, t3_large_site(k_t3_capmax_text), DSD_CALL_CHANNEL_LPCN, 38U);
    ring.touchCommitted(0);
    CallHistoryModel next;
    next.refresh(ring.state);
    expect("a text changed before the next launch re-reads",
           next.count() == 1
               && site_text(next, 0)
                      == u8("DMR \xC2\xB7 TIII Large \xC2\xB7 Capacity Max \xC2\xB7 Net 1 \xC2\xB7 Site 1 \xC2\xB7 SYS "
                            "2101 \xC2\xB7 LPCN 38"));
}

/* The spilled seen entry carries the digest: a relaunched model that folds the spill re-reads the ring row whose text
 * changed after the tap spilled it, and only that. */
void
test_site_text_digest_spill_round_trip(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754528500;
    Event_History* item = nullptr;
    {
        CallHistoryModel model;
        item = RingFixture::site(ring.commit(0, 24101, 2411, when, when + 4), t3_large_site(k_t3_standard_text),
                                 DSD_CALL_CHANNEL_LPCN, 38U);
        model.refresh(ring.state);
        expect("the call lands", model.count() == 1);
    } // destructor flushes the stores
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    const QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    expect("one row and one seen entry stored", rows.size() == 1 && seen.size() == 1);
    QJsonObject seenEntry = seen.at(0).toObject();
    expect("the seen entry holds the digest",
           seenEntry.value(QLatin1String("stx")).toInteger(-1) == fnv1a32(k_t3_standard_text));
    QJsonObject line;
    line.insert(QLatin1String("v"), 1);
    line.insert(QLatin1String("key"), seenEntry.value(QLatin1String("key")));
    seenEntry.remove(QLatin1String("key"));
    line.insert(QLatin1String("seen"), seenEntry);
    line.insert(QLatin1String("row"), rows.at(0));
    const QByteArray bytes = QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
    const auto writeSpill = [&bytes]() {
        QFile::remove(dsd_qt::json_store_path(QStringLiteral("call_history.json")));
        QFile::remove(dsd_qt::json_store_path(QStringLiteral("call_history_seen.json")));
        QFile spill(dsd_qt::json_store_path(QStringLiteral("call_history_spill.jsonl")));
        expect("the spill is written", spill.open(QIODevice::WriteOnly) && spill.write(bytes) == bytes.size());
        spill.close();
    };

    writeSpill();
    {
        CallHistoryModel restored;
        QSignalSpy changed(&restored, &QAbstractItemModel::dataChanged);
        expect("dataChanged spy connects", changed.isValid());
        restored.refresh(ring.state);
        expect("the spilled digest matches the ring, so the row is no update",
               restored.count() == 1 && changed.count() == 0);
    }

    writeSpill();
    RingFixture::site(item, t3_large_site(k_t3_capmax_text), DSD_CALL_CHANNEL_LPCN, 38U);
    ring.touchCommitted(0);
    CallHistoryModel restored;
    restored.refresh(ring.state);
    expect("a text changed since the spill re-reads",
           restored.count() == 1
               && site_text(restored, 0)
                      == u8("DMR \xC2\xB7 TIII Large \xC2\xB7 Capacity Max \xC2\xB7 Net 1 \xC2\xB7 Site 1 \xC2\xB7 SYS "
                            "2101 \xC2\xB7 LPCN 38"));
}

/* The DMR site text follows -D, so one site can arrive under two texts with the same raw numbers. Those are one site:
 * the fragments merge, and the row keeps one of the texts (the same one in any order), through a save and reload. */
void
test_dmr_text_differs_raw_equal_merges_and_keeps_text(void) {
    const time_t when = 1754531000;
    const QString text = u8("DMR \xC2\xB7 TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 1.4");
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        {
            CallHistoryModel model;
            RingFixture::site(ring.commit(0, 27001, 2701, when, when + 4),
                              tier3_site(12, 3, "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4"));
            if (incremental) {
                model.refresh(ring.state);
            }
            RingFixture::site(ring.commit(0, 27001, 2701, when + 6, when + 10),
                              tier3_site(12, 3, "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 1.4"));
            model.refresh(ring.state);
            expect(incremental ? "two texts of one site merge (incremental)" : "two texts of one site merge (batched)",
                   model.count() == 1);
            expect(incremental ? "the row keeps one text, whatever the order (incremental)"
                               : "the row keeps one text, whatever the order (batched)",
                   site_text(model, 0) == text);
        } // destructor flushes the stores
        CallHistoryModel restored;
        expect(incremental ? "the text survives a reload (incremental)" : "the text survives a reload (batched)",
               restored.count() == 1 && site_text(restored, 0) == text);
    }
}

/* The detail sheet reads its row by uid: every detail the three entry points pass, by the same roles, and the site
 * line; a uid no row has now reads an empty map. */
void
test_record_for_uid(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754532000;
    Event_History* item = RingFixture::carrier(ring.commit(0, 28001, 2801, when, when + 7, "", "Fire Dispatch"),
                                               851012500, DSD_ACCESS_CODE_NAC, 0x293U);
    RingFixture::site(item, p25_site(0x1A3, 3, 21));
    DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Engine 5");
    item->emergency = 1U;
    model.refresh(ring.state);
    expect("the call lands", model.count() == 1);
    const QModelIndex idx = model.index(0);
    const qint64 uid = model.data(idx, CallHistoryModel::UidRole).toLongLong();
    const QVariantMap record = model.recordFor(QString::number(uid));

    const struct {
        const char* key;
        int role;
    } keys[] = {{"uid", CallHistoryModel::UidRole},
                {"name", CallHistoryModel::NameRole},
                {"when", CallHistoryModel::WhenRole},
                {"systemName", CallHistoryModel::SystemNameRole},
                {"systemUid", CallHistoryModel::SystemUidRole},
                {"channel", CallHistoryModel::ChannelRole},
                {"tg", CallHistoryModel::TgRole},
                {"src", CallHistoryModel::SrcRole},
                {"sourceName", CallHistoryModel::SourceNameRole},
                {"emergency", CallHistoryModel::EmergencyRole},
                {"enc", CallHistoryModel::EncRole},
                {"durationSecs", CallHistoryModel::DurationSecsRole},
                {"detail", CallHistoryModel::DetailRole},
                {"freqHz", CallHistoryModel::FreqHzRole},
                {"accessCode", CallHistoryModel::AccessCodeRole},
                {"accessCodeText", CallHistoryModel::AccessCodeTextRole},
                {"serviceName", CallHistoryModel::ServiceNameRole},
                {"receivedTone", CallHistoryModel::ReceivedToneRole},
                {"siteText", CallHistoryModel::SiteTextRole},
                {"analog", CallHistoryModel::AnalogRole},
                {"analogMode", CallHistoryModel::AnalogModeRole},
                {"position", CallHistoryModel::PositionRole},
                {"gi", CallHistoryModel::GiRole}};

    expect("the map has one key per detail", record.size() == static_cast<int>(sizeof(keys) / sizeof(keys[0])));
    for (const auto& key : keys) {
        if (!record.contains(QLatin1String(key.key))
            || record.value(QLatin1String(key.key)) != model.data(idx, key.role)) {
            DSD_FPRINTF(stderr, "FAIL: recordFor()'s %s is not the row's\n", key.key);
            g_failures++;
        }
    }
    expect("the map names the source and the site",
           record.value(QStringLiteral("sourceName")).toString() == QLatin1String("Engine 5")
               && record.value(QStringLiteral("siteText")).toString()
                      == u8("P25 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 3 \xC2\xB7 SITE 21")
               && record.value(QStringLiteral("durationSecs")).toInt() == 7
               && record.value(QStringLiteral("emergency")).toBool());
    expect("a digital row is no analog reception and has no mode",
           !record.value(QStringLiteral("analog")).toBool()
               && record.value(QStringLiteral("analogMode")).toString().isEmpty());
    expect("a uid no row has reads empty", model.recordFor(QString::number(uid + 1)).isEmpty());
    expect("no uid reads empty", model.recordFor(QString()).isEmpty() && model.recordFor(QStringLiteral("0")).isEmpty()
                                     && model.recordFor(QStringLiteral("x1")).isEmpty());
}

/* Co-channel services retain separate histories even when all numeric call IDs match. */
void
test_receive_service_identity(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = kCaptureStart + 200;
    {
        CallHistoryModel model;
        for (int code = 7; code <= 8; ++code) {
            Event_History* item = ring.commit(0, 1001, 2001, when, when + 4, "", "", "Base channel");
            item->receive_identity.known = 1;
            item->receive_identity.kind = DSD_RECEIVE_DCC;
            item->receive_identity.value = static_cast<uint16_t>(code);
            item->receive_identity.slot = 1;
            DSD_SNPRINTF(item->service_label, sizeof item->service_label, "%s", "Hospital");
            model.refresh(ring.state);
        }
        expect("different confirmed services cannot merge", model.count() == 2);
        Event_History* analog = ring.commit(0, 0, 0, when + 10, when + 20, "", "", "Base channel");
        analog->receive_identity = {1413, DSD_RECEIVE_CTCSS, 1, 0, 0};
        DSD_SNPRINTF(analog->service_label, sizeof analog->service_label, "%s", "20R Traveler");
        model.refresh(ring.state);
        expect("service names reach display without replacing the base channel",
               model.data(model.index(0), CallHistoryModel::NameRole) == "20R Traveler"
                   && model.data(model.index(0), CallHistoryModel::ChannelRole) == "Base channel");
        expect("tone is available in history, spelled as every other surface spells it",
               model.data(model.index(0), CallHistoryModel::ReceivedToneRole) == "CTCSS 141.3 Hz");
    }
    {
        CallHistoryModel model;
        expect("service rows persist separately", model.count() == 3);
        expect("service and tone survive reload",
               model.data(model.index(0), CallHistoryModel::ServiceNameRole) == "20R Traveler"
                   && model.data(model.index(0), CallHistoryModel::ReceivedToneRole) == "CTCSS 141.3 Hz");
    }
}

/* A fragment committed before its colour code was confirmed has no identity and no access-code name yet. It is the
   same call as the confirmed fragment after it: the two fold into one row, named by the confirmed service. */
void
test_unconfirmed_fragment_joins_named_service(void) {
    const time_t when = kCaptureStart + 300;
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 1001, 2001, when, when + 4, "", "", "Base channel");
        if (incremental) {
            model.refresh(ring.state);
        }
        Event_History* confirmed = ring.commit(0, 1001, 2001, when + 6, when + 10, "", "", "Base channel");
        confirmed->receive_identity = {7, DSD_RECEIVE_DCC, 1, 1, 0};
        DSD_SNPRINTF(confirmed->service_label, sizeof confirmed->service_label, "%s", "Hospital");
        model.refresh(ring.state);
        expect(incremental ? "an unconfirmed fragment joins the named call (incremental)"
                           : "an unconfirmed fragment joins the named call (batched)",
               model.count() == 1);
        expect(incremental ? "the merged row carries the confirmed service (incremental)"
                           : "the merged row carries the confirmed service (batched)",
               model.data(model.index(0), CallHistoryModel::ServiceNameRole) == "Hospital");
        expect(incremental ? "the merged row is named by the confirmed service (incremental)"
                           : "the merged row is named by the confirmed service (batched)",
               model.data(model.index(0), CallHistoryModel::NameRole) == "Hospital");
    }
}

/* Issue #637: the single-slot DMR decoder proves the over-the-air slot apart from the colour code. A fragment whose slot
   was still pending joins the fragment that proved it (and takes its slot and slot name), while fragments that proved
   different slots stay two calls even when neither colour code was confirmed. */
void
test_slot_proof_merges_independently_of_code(void) {
    const time_t when = kCaptureStart + 350;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* pending = ring.commit(0, 1001, 2001, when, when + 4, "", "", "Base channel");
        pending->receive_identity = {7, DSD_RECEIVE_DCC, 1, 0, 1};
        model.refresh(ring.state);
        Event_History* proven = ring.commit(0, 1001, 2001, when + 5, when + 9, "", "", "Base channel");
        proven->receive_identity = {7, DSD_RECEIVE_DCC, 1, 1, 0};
        DSD_SNPRINTF(proven->service_label, sizeof proven->service_label, "%s", "Dispatch");
        model.refresh(ring.state);
        expect("a pending-slot fragment joins the one that proved its slot", model.count() == 1);
        expect("the merged row takes the proven slot's name",
               model.data(model.index(0), CallHistoryModel::ServiceNameRole) == "Dispatch");

        Event_History* ts1 = ring.commit(0, 1002, 2002, when + 60, when + 64, "", "", "Base channel");
        ts1->receive_identity = {0, DSD_RECEIVE_DCC, 0, 1, 0};
        model.refresh(ring.state);
        Event_History* ts2 = ring.commit(0, 1002, 2002, when + 65, when + 69, "", "", "Base channel");
        ts2->receive_identity = {0, DSD_RECEIVE_DCC, 0, 2, 0};
        model.refresh(ring.state);
        expect("fragments that proved different slots stay two calls", model.count() == 3);
        Event_History* lone = ring.commit(0, 1003, 2003, when + 120, when + 124, "", "", "Base channel");
        lone->receive_identity = {0, DSD_RECEIVE_DCC, 0, 2, 0};
        model.refresh(ring.state);
        expect("a TS2 fragment with no confirmed code is a row of its own", model.count() == 4);

        /* A slot name never replaces the name a confirmed code gave the row. */
        Event_History* named = ring.commit(0, 1004, 2004, when + 200, when + 204, "", "", "Base channel");
        named->receive_identity = {7, DSD_RECEIVE_DCC, 1, 0, 1};
        DSD_SNPRINTF(named->service_label, sizeof named->service_label, "%s", "Hospital");
        model.refresh(ring.state);
        Event_History* slotted = ring.commit(0, 1004, 2004, when + 205, when + 209, "", "", "Base channel");
        slotted->receive_identity = {0, DSD_RECEIVE_DCC, 0, 1, 0};
        DSD_SNPRINTF(slotted->service_label, sizeof slotted->service_label, "%s", "Dispatch");
        model.refresh(ring.state);
        expect("a proven slot joins the code-named row", model.count() == 5);
        expect("the code's name outranks the slot name",
               model.data(model.index(0), CallHistoryModel::ServiceNameRole) == "Hospital");
    }
    {
        /* Its slot is persisted without a confirmed code, so a TS1 fragment after the reload stays apart from it. */
        CallHistoryModel model;
        Event_History* ts1 = ring.commit(0, 1003, 2003, when + 125, when + 129, "", "", "Base channel");
        ts1->receive_identity = {0, DSD_RECEIVE_DCC, 0, 1, 0};
        model.refresh(ring.state);
        expect("a reloaded row keeps its proven slot without a confirmed code", model.count() == 6);
    }
}

/* Issue #637: a committed row the core refines in place with receive evidence only (a reacquired segment that proved
   the slot and named the service, within the same second) is read again as an update. A digital row with no target
   whose name was only its channel label joins the fragment the service names: a label is no destination. */
void
test_receive_only_updates_and_label_renames(void) {
    const time_t when = kCaptureStart + 450;
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    Event_History* item = ring.commit(0, 1005, 2005, when, when + 4, "", "", "Base channel");
    item->receive_identity = {7, DSD_RECEIVE_DCC, 1, 0, 1};
    model.refresh(ring.state);
    expect("the pending-slot row is logged", model.count() == 1);
    item->receive_identity = {7, DSD_RECEIVE_DCC, 1, 1, 0};
    DSD_SNPRINTF(item->service_label, sizeof item->service_label, "%s", "Dispatch");
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a receive-only refinement updates the row in place",
           model.count() == 1 && model.data(model.index(0), CallHistoryModel::ServiceNameRole) == "Dispatch");
    /* A name added to a call already confirmed (code and slot) reaches its row in place. */
    Event_History* unnamed = ring.commit(0, 1006, 2006, when + 30, when + 34, "", "", "Base channel");
    unnamed->receive_identity = {9, DSD_RECEIVE_DCC, 1, 1, 0};
    model.refresh(ring.state);
    expect("the unnamed confirmed row is logged", model.count() == 2);
    DSD_SNPRINTF(unnamed->service_label, sizeof unnamed->service_label, "%s", "Fire");
    unnamed->event_time = when + 38;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("a name added to a confirmed call updates its row",
           model.count() == 2 && model.data(model.index(0), CallHistoryModel::ServiceNameRole) == "Fire");

    Event_History* labelled = ring.commit(0, 0, 0, when + 60, when + 64, "", "", "Base channel");
    labelled->systype = DSD_SYNC_DMR_BS_VOICE_POS;
    RingFixture::carrier(labelled, 452000000);
    model.refresh(ring.state);
    Event_History* serviced = ring.commit(0, 0, 0, when + 65, when + 69, "", "", "Base channel");
    serviced->systype = DSD_SYNC_DMR_BS_VOICE_POS;
    serviced->receive_identity = {7, DSD_RECEIVE_DCC, 1, 1, 0};
    DSD_SNPRINTF(serviced->service_label, sizeof serviced->service_label, "%s", "Hospital");
    RingFixture::carrier(serviced, 452000000);
    model.refresh(ring.state);
    expect("a label-only rename is no new destination", model.count() == 3);
    expect("the folded row takes the service name",
           model.data(model.index(0), CallHistoryModel::NameRole) == "Hospital");
    /* Two different textual targets stay apart, whatever their labels. */
    Event_History* m17a = ring.commit(0, 0, 0, when + 120, when + 124, "W1AW", "", "Base channel");
    RingFixture::carrier(m17a, 452000000);
    model.refresh(ring.state);
    Event_History* m17b = ring.commit(0, 0, 0, when + 125, when + 129, "K1ABC", "", "Base channel");
    RingFixture::carrier(m17b, 452000000);
    model.refresh(ring.state);
    expect("two textual targets stay two rows", model.count() == 5);
}

/* Commit a conventional analog reception (issues #637, #667): no target and no source, heard in @p mode at @p freq_hz,
   with @p tone (a known "no tone" by default). */
Event_History*
commit_analog(RingFixture& ring, time_t start, time_t end, int64_t freq_hz, uint8_t mode,
              dsd_receive_identity tone = {0, DSD_RECEIVE_NONE, 1, 0, 0}) {
    Event_History* item = ring.commit(0, 0, 0, start, end);
    item->systype = DSD_SYNC_ANALOG;
    item->analog_mode = mode;
    item->receive_identity = tone;
    RingFixture::carrier(item, freq_hz);
    return item;
}

/* Issue #667 (B2): the received tone reads as the event row, the terminal and the API spell it ("CTCSS 100.0 Hz",
   "DCS D023N / D047I" with the normal polarity first), never as a bare value ("100.0", "D023N"). A digital row's
   access code is no tone. */
void
test_received_tone_reads_as_its_label(void) {
    const time_t when = kCaptureStart + 500;
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    commit_analog(ring, when, when + 4, 154430000, DSD_ANALOG_MODE_NFM, {1000, DSD_RECEIVE_CTCSS, 1, 0, 0});
    commit_analog(ring, when + 10, when + 14, 154445000, DSD_ANALOG_MODE_NFM, {023 * 2, DSD_RECEIVE_DCS, 1, 0, 0});
    commit_analog(ring, when + 20, when + 24, 154460000, DSD_ANALOG_MODE_NFM, {023 * 2 + 1, DSD_RECEIVE_DCS, 1, 0, 0});
    commit_analog(ring, when + 30, when + 34, 154475000, DSD_ANALOG_MODE_NFM, {1000, DSD_RECEIVE_CTCSS, 0, 0, 0});
    RingFixture::carrier(ring.commit(0, 1001, 2001, when + 40, when + 44), 851012500, DSD_ACCESS_CODE_NAC, 0x293U)
        ->receive_identity = {0x293, DSD_RECEIVE_NAC, 1, 0, 0};
    model.refresh(ring.state);
    // Each on its own frequency, so the count holds whatever the merge does with analog rows.
    expect("five rows land", model.count() == 5);
    const auto tone = [&model](int row) {
        return model.data(model.index(row), CallHistoryModel::ReceivedToneRole).toString();
    };
    expect("a digital row has no tone", tone(0).isEmpty());
    expect("a tone not yet confirmed is not shown", tone(1).isEmpty());
    expect("an inverted DCS code names its normal spelling first", tone(2) == QLatin1String("DCS D047N / D023I"));
    expect("a DCS code reads as its label", tone(3) == QLatin1String("DCS D023N / D047I"));
    expect("a CTCSS tone reads as its label", tone(4) == QLatin1String("CTCSS 100.0 Hz"));
}

/* Issue #667 (B1): each analog reception is a row of its own. Two receptions never fold, on two frequencies or on
   one, a second apart, or the second confirming a tone the first was still acquiring; and an analog row never folds
   with a digital row heard on its frequency, either way round. Read one refresh at a time or all at once. Digital
   fragments still fold (test_receive_only_updates_and_label_renames and the merge tests above). */
void
test_analog_receptions_never_fold(void) {
    const time_t when = kCaptureStart + 400;
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        const auto read = [&]() {
            if (incremental) {
                model.refresh(ring.state);
            }
        };
        commit_analog(ring, when, when + 4, 154430000, DSD_ANALOG_MODE_NFM);
        read();
        commit_analog(ring, when + 5, when + 9, 155010000, DSD_ANALOG_MODE_NFM);
        read();
        commit_analog(ring, when + 10, when + 14, 155010000, DSD_ANALOG_MODE_NFM);
        read();
        commit_analog(ring, when + 15, when + 19, 155010000, DSD_ANALOG_MODE_AM);
        read();
        /* A reception whose tone was still acquiring, then one that confirmed it and is named by it. */
        commit_analog(ring, when + 40, when + 44, 156000000, DSD_ANALOG_MODE_NFM, {0, DSD_RECEIVE_NONE, 0, 0, 0});
        read();
        Event_History* toned = commit_analog(ring, when + 45, when + 49, 156000000, DSD_ANALOG_MODE_NFM,
                                             {1413, DSD_RECEIVE_CTCSS, 1, 0, 0});
        DSD_SNPRINTF(toned->service_label, sizeof toned->service_label, "%s", "Traveler");
        read();
        /* A digital row with no ids heard on the same channel and frequency, before and after an analog reception. */
        const auto digital = [&ring](time_t start, int64_t freq_hz) {
            RingFixture::carrier(ring.commit(0, 0, 0, start, start + 4, "", "", "Base channel"), freq_hz)->systype =
                DSD_SYNC_DMR_BS_VOICE_POS;
        };
        const auto on_channel = [&ring](time_t start, int64_t freq_hz) {
            Event_History* item = commit_analog(ring, start, start + 4, freq_hz, DSD_ANALOG_MODE_NFM);
            DSD_SNPRINTF(item->channel_label, sizeof item->channel_label, "%s", "Base channel");
        };
        digital(when + 60, 157000000);
        read();
        on_channel(when + 65, 157000000);
        read();
        on_channel(when + 90, 158000000);
        read();
        digital(when + 95, 158000000);
        model.refresh(ring.state);
        expect(incremental ? "every analog reception is a row of its own (incremental)"
                           : "every analog reception is a row of its own (batched)",
               model.count() == 10);
        // Newest first: two rows from +60 s and two from +90 s sort above the pair at +40 s and +45 s.
        const int traveler = 4;
        expect(incremental ? "the toned reception is named by its service (incremental)"
                           : "the toned reception is named by its service (batched)",
               model.data(model.index(traveler), CallHistoryModel::NameRole) == "Traveler"
                   && model.data(model.index(traveler), CallHistoryModel::DurationSecsRole).toInt() == 4);
        expect(incremental ? "the acquiring reception keeps its own row and span (incremental)"
                           : "the acquiring reception keeps its own row and span (batched)",
               model.data(model.index(traveler + 1), CallHistoryModel::NameRole) == "NFM"
                   && model.data(model.index(traveler + 1), CallHistoryModel::DurationSecsRole).toInt() == 4);
    }
}

std::atomic<int> g_storeBuildsOnGui{0};
std::atomic<int> g_storeBuildsElsewhere{0};

void
count_store_build(const QThread* thread) {
    if (thread == QCoreApplication::instance()->thread()) {
        g_storeBuildsOnGui++;
    } else {
        g_storeBuildsElsewhere++;
    }
}

/* Issue #718: at a full log, building the two stores' JSON cost the UI thread ~62 ms per debounced save.
 * The UI thread now hands the worker copies of the rows and the seen map, and the worker builds and writes
 * both. The log moving on under an in-flight save changes nothing that save writes, and what it missed is
 * still saved: by the next debounced save, or synchronously on destruction. */
void
test_stores_serialise_off_the_gui_thread(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754512000;
    {
        CallHistoryModel model;
        for (int i = 0; i < 40; i++) {
            ring.commit(i % 2, 13000U + static_cast<uint32_t>(i), 700U + static_cast<uint32_t>(i),
                        when + static_cast<time_t>(60) * i, when + static_cast<time_t>(60) * i + 4);
        }
        model.refresh(ring.state);
        expect("forty calls land before the save", model.count() == 40);
        g_storeBuildsOnGui = 0;
        g_storeBuildsElsewhere = 0;
        CallHistoryModel::setStoreBuildObserverForTest(count_store_build);
        model.startSaveForTest();
        ring.commit(0, 13999U, 799U, when + 9000, when + 9004);
        model.refresh(ring.state);
        expect("the log moves on while the save is in flight", model.count() == 41);
        model.waitForSaveForTest();
        CallHistoryModel::setStoreBuildObserverForTest(nullptr);
        expect("the UI thread builds no store JSON", g_storeBuildsOnGui == 0);
        expect("the save worker builds both stores", g_storeBuildsElsewhere == 2);
        expect("the in-flight save writes the log as it was handed over",
               dsd_qt::json_store_load_array("call_history.json").size() == 40
                   && dsd_qt::json_store_load_array("call_history_seen.json").size() == 40
                   && stored_row_with_tg(dsd_qt::json_store_load_array("call_history.json"), 13999).isEmpty());
    } // the destructor flushes the save the last ingest owed
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    expect("the owed save lands on destruction",
           rows.size() == 41 && !stored_row_with_tg(rows, 13999).isEmpty()
               && dsd_qt::json_store_load_array("call_history_seen.json").size() == 41);
    CallHistoryModel restored;
    expect("the saved log reloads", restored.count() == 41);
}

/** Total bytes of both stores on disk. */
qint64
stores_bytes(void) {
    return QFileInfo(dsd_qt::json_store_path("call_history.json")).size()
           + QFileInfo(dsd_qt::json_store_path("call_history_seen.json")).size();
}

/** Let the save worker finish and deliver its completion, as the event loop would. */
void
settle_save(CallHistoryModel& model) {
    model.waitForSaveForTest();
    QCoreApplication::processEvents();
}

bool
stored_tg(qulonglong tg) {
    return !stored_row_with_tg(dsd_qt::json_store_load_array("call_history.json"), tg).isEmpty();
}

/* A full log rewrote most of a megabyte every 3 s on a busy system. The delay after a save now
 * grows with what that save wrote (16 KiB/s of both stores), and a small log keeps 3 s. */
void
test_save_delay_follows_the_bytes_written(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754513000;
    CallHistoryModel model;
    ring.commit(0, 14000U, 800U, when, when + 4);
    model.refresh(ring.state);
    model.startSaveForTest();
    settle_save(model);
    expect("a small store keeps the 3 s delay", model.saveDelayMsForTest() == 3000);

    ring.commitBatch(0, 200, 14100U, 900U, when + 100);
    ring.commitBatch(1, 200, 14400U, 1200U, when + 100);
    model.refresh(ring.state);
    expect("the batch lands", model.count() == 401);
    model.startSaveForTest();
    settle_save(model);
    const qint64 bytes = stores_bytes();
    expect("the stores are large", bytes > qint64{48} * 1024);
    expect("a large store waits as long as its bytes take at 16 KiB/s",
           model.saveDelayMsForTest() > 3000 && model.saveDelayMsForTest() >= bytes * 1000 / (qint64{16} * 1024));
    ring.commit(0, 14900U, 1900U, when + 9000, when + 9004);
    model.refresh(ring.state);
    expect("the next call waits out the longer delay",
           model.savePendingForTest() && model.saveDelayMsForTest() >= bytes * 1000 / (qint64{16} * 1024));
}

std::atomic<int> g_saveNowBuilds{0};

void
count_save_now_build(const QThread*) {
    g_saveNowBuilds++;
}

/* saveNow() writes the newest row without waiting out the delay, and writes nothing when nothing is owed. */
void
test_save_now_writes_at_once(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754514000;
    CallHistoryModel model;
    ring.commit(0, 14201U, 801U, when, when + 4);
    model.refresh(ring.state);
    expect("the call arms the debounced save", model.savePendingForTest());
    model.saveNow();
    expect("saveNow leaves no save waiting", !model.savePendingForTest());
    settle_save(model);
    expect("saveNow writes the newest row", stored_tg(14201U));

    g_saveNowBuilds = 0;
    CallHistoryModel::setStoreBuildObserverForTest(count_save_now_build);
    model.saveNow();
    settle_save(model);
    CallHistoryModel::setStoreBuildObserverForTest(nullptr);
    expect("nothing owed, nothing written", g_saveNowBuilds == 0);
}

/* A saveNow() that lands mid-write copies the stores at once and queues their save behind the one in
 * flight. It runs as that one finishes, with no GUI event loop in between (Qt pauses it on Android in
 * the background), and the two completions leave ordinary debounced saves working. */
void
test_save_now_mid_write_queues_the_next_save(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754515000;
    CallHistoryModel model;
    ring.commit(0, 14301U, 802U, when, when + 4);
    model.refresh(ring.state);
    g_saveNowBuilds = 0;
    CallHistoryModel::setStoreBuildObserverForTest(count_save_now_build);
    model.startSaveForTest();
    ring.commit(0, 14302U, 803U, when + 60, when + 64);
    model.refresh(ring.state);
    model.saveNow();
    expect("no delayed retry", !model.savePendingForTest());
    model.waitForSaveForTest(); // the pool alone; no event is processed between the saves
    CallHistoryModel::setStoreBuildObserverForTest(nullptr);
    expect("both saves ran on the pool", g_saveNowBuilds == 4);
    expect("the queued save carries the row added mid-write", stored_tg(14301U) && stored_tg(14302U));

    // Both completions delivered, the next call takes the debounced path and is written.
    QCoreApplication::processEvents();
    expect("nothing is owed after the queued save", !model.savePendingForTest());
    ring.commit(0, 14303U, 804U, when + 120, when + 124);
    model.refresh(ring.state);
    expect("the next call arms the debounced save", model.savePendingForTest());
    model.startSaveForTest();
    settle_save(model);
    expect("the debounced save still writes after two queued saves", stored_tg(14303U));
}

/* Android may kill a process off screen without notice: leaving the screen writes the log. That holds
 * when it lands between a save's write and its completion reaching the GUI thread. */
void
test_leaving_the_screen_saves(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754516000;
    CallHistoryModel model;
    ring.commit(0, 14401U, 804U, when, when + 4);
    model.refresh(ring.state);
    Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationSuspended);
    settle_save(model);
    expect("a suspend writes the newest row", stored_tg(14401U));

    ring.commit(0, 14402U, 805U, when + 60, when + 64);
    model.refresh(ring.state);
    model.startSaveForTest();
    model.waitForSaveForTest(); // written; the completion is still queued
    ring.commit(0, 14403U, 806U, when + 120, when + 124);
    model.refresh(ring.state);
    Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationHidden);
    model.waitForSaveForTest(); // no event loop: the completion above is still undelivered
    expect("a hide before the completion still writes the newest row", stored_tg(14403U));
    QCoreApplication::processEvents();
}

/* Issue #708: a stand-in for the call recording store, with a recording on each fragment of @c recorded. */
class FakeRecordings : public dsd_qt::CallRecordingProvider {
  public:
    QSet<QString> recorded;

    QVariantList
    recordingsForFragments(const QStringList& fragments) const override {
        QVariantList list;
        for (const QString& fragment : fragments) {
            if (recorded.contains(fragment)) {
                list.append(QVariantMap{{QStringLiteral("fragment"), fragment}});
            }
        }
        return list;
    }

    bool
    hasRecordingForFragments(const QStringList& fragments) const override {
        for (const QString& fragment : fragments) {
            if (recorded.contains(fragment)) {
                return true;
            }
        }
        return false;
    }

    QList<QStringList>
    recordingEntryNames(const QList<QStringList>& fragmentsPerRow) const override {
        QList<QStringList> names;
        for (const QStringList& fragments : fragmentsPerRow) {
            QStringList row;
            for (const QString& fragment : fragments) {
                if (recorded.contains(fragment)) {
                    row.append(fragment);
                }
            }
            names.append(row);
        }
        return names;
    }

    QList<QVariantList>
    recordingsForFragmentLists(const QList<QStringList>& fragmentsPerRow) const override {
        batchReads++;
        lastBatch = fragmentsPerRow;
        QList<QVariantList> rows;
        if (shortAnswer) {
            return rows;
        }
        for (const QStringList& fragments : fragmentsPerRow) {
            rows.append(recordingsForFragments(fragments));
        }
        return rows;
    }

    mutable int batchReads = 0;
    mutable QList<QStringList> lastBatch;
    bool shortAnswer = false;
};

/* The fragment key of the newest row @p slot committed. */
QString
newest_fragment(const RingFixture& ring, int slot) {
    return dsd_qt::call_history_fragment_key(ring.rings[slot].instance, slot, ring.rings[slot].push_seq);
}

qint64
uid_at(const CallHistoryModel& model, int row) {
    return model.data(model.index(row), CallHistoryModel::UidRole).toLongLong();
}

bool
has_recording(const CallHistoryModel& model, int row) {
    return model.data(model.index(row), CallHistoryModel::HasRecordingRole).toBool();
}

/* One capture's analog receptions as a replay pushes them (issue #667): three on one frequency a few seconds apart,
   each a reception of its own. NFM ones carry a tone (CTCSS twice, as nfm_ctcss_real's do, then DCS); dsd-neo detects
   none on AM. Returns each one's ring fragment, oldest first. */
QStringList
replay_analog_capture(RingFixture& ring, uint8_t mode) {
    const bool nfm = mode == DSD_ANALOG_MODE_NFM;
    const dsd_receive_identity none = {0, DSD_RECEIVE_NONE, 1, 0, 0};
    const dsd_receive_identity ctcss = {1000, DSD_RECEIVE_CTCSS, 1, 0, 0};
    const dsd_receive_identity dcs = {023 * 2, DSD_RECEIVE_DCS, 1, 0, 0};
    QStringList fragments;
    commit_analog(ring, kCaptureStart + 100, kCaptureStart + 104, 154430000, mode, nfm ? ctcss : none);
    fragments.append(newest_fragment(ring, 0));
    commit_analog(ring, kCaptureStart + 106, kCaptureStart + 109, 154430000, mode, nfm ? ctcss : none);
    fragments.append(newest_fragment(ring, 0));
    commit_analog(ring, kCaptureStart + 110, kCaptureStart + 113, 154430000, mode, nfm ? dcs : none);
    fragments.append(newest_fragment(ring, 0));
    return fragments;
}

/* The row whose recordings @p fragment's recording is, or -1. */
int
row_recording(const CallHistoryModel& model, FakeRecordings& recordings, const QString& fragment) {
    recordings.recorded = {fragment};
    int found = -1;
    for (int i = 0; i < model.rowCount(); i++) {
        if (has_recording(model, i)) {
            found = found < 0 ? i : -2;
        }
    }
    return found;
}

/* Issue #667 (B1): a capture's analog receptions replayed in a fresh ring, in the same app or after a reload, are
   heard again. Each one's logged row joins the running session and takes the new ring's fragment, so both replays'
   recordings are that row's; no row is added and none folds into another. Matched exactly: a reception of the fresh
   ring with the same start but another tone is a reception of its own. */
void
test_analog_replay_heard_again(void) {
    for (uint8_t mode : {static_cast<uint8_t>(DSD_ANALOG_MODE_NFM), static_cast<uint8_t>(DSD_ANALOG_MODE_AM)}) {
        for (int reload = 0; reload < 2; reload++) {
            const bool am = mode == DSD_ANALOG_MODE_AM;
            resetStorage();
            FakeRecordings recordings;
            auto model = std::make_unique<CallHistoryModel>();
            model->setRecordingProvider(&recordings);
            auto ring = std::make_unique<RingFixture>();
            model->beginSession();
            const QStringList first = replay_analog_capture(*ring, mode);
            model->refresh(ring->state);
            expect(am ? "the first AM replay logs each reception" : "the first NFM replay logs each reception",
                   model->count() == 3);
            if (reload != 0) {
                model.reset(); // destructor flushes the stores
                model = std::make_unique<CallHistoryModel>();
                model->setRecordingProvider(&recordings);
            }
            model->beginSession();
            const qlonglong session = model->session();
            ring = std::make_unique<RingFixture>(); // the next start's fresh state
            const QStringList second = replay_analog_capture(*ring, mode);
            model->refresh(ring->state);
            expect(am ? "an AM replay heard again adds no row" : "an NFM replay heard again adds no row",
                   model->count() == 3);
            expect(am ? "each AM row joins the session that heard it again"
                      : "each NFM row joins the session that heard it again",
                   rows_in_session(*model, session) == 3);
            for (int i = 0; i < 3; i++) {
                const int row = row_recording(*model, recordings, first.at(i));
                expect(reload != 0 ? "after a reload, a reception's row holds both replays' recordings"
                                   : "a reception's row holds both replays' recordings",
                       row == 2 - i && row_recording(*model, recordings, second.at(i)) == row);
                expect("the row keeps its own span",
                       model->data(model->index(row), CallHistoryModel::DurationSecsRole).toInt() == (i == 0 ? 4 : 3));
            }
            ring = std::make_unique<RingFixture>();
            commit_analog(*ring, kCaptureStart + 100, kCaptureStart + 104, 154430000, mode,
                          {1035, DSD_RECEIVE_CTCSS, 1, 0, 0});
            model->refresh(ring->state);
            expect("a reception at the same start with another tone is a row of its own", model->count() == 4);
            ring = std::make_unique<RingFixture>();
            commit_analog(*ring, kCaptureStart + 100, kCaptureStart + 104, 154445000, mode,
                          am ? dsd_receive_identity{0, DSD_RECEIVE_NONE, 1, 0, 0}
                             : dsd_receive_identity{1000, DSD_RECEIVE_CTCSS, 1, 0, 0});
            model->refresh(ring->state);
            expect("a reception at the same start on another frequency is a row of its own", model->count() == 5);
            model->setRecordingProvider(nullptr);
        }
    }
}

/* Issue #667 (B1): a replay whose receptions take other push stamps (a tone filter changed to leave an earlier one out)
   is still heard again: a first sighting of an analog row matches the reception it is, exactly, across the log, so the
   repeat joins its row (and its recording too) and no duplicate is logged. */
void
test_analog_replay_with_other_push_stamps(void) {
    resetStorage();
    FakeRecordings recordings;
    CallHistoryModel model;
    model.setRecordingProvider(&recordings);
    const dsd_receive_identity ctcss = {1000, DSD_RECEIVE_CTCSS, 1, 0, 0};
    auto ring = std::make_unique<RingFixture>();
    model.beginSession();
    commit_analog(*ring, kCaptureStart + 300, kCaptureStart + 304, 154430000, DSD_ANALOG_MODE_NFM, ctcss);
    commit_analog(*ring, kCaptureStart + 306, kCaptureStart + 309, 154430000, DSD_ANALOG_MODE_NFM, ctcss);
    const QString first = newest_fragment(*ring, 0);
    model.refresh(ring->state);
    expect("two receptions log two rows", model.count() == 2);
    model.beginSession();
    ring = std::make_unique<RingFixture>(); // the replay leaves the first reception out: the second takes stamp 1
    commit_analog(*ring, kCaptureStart + 306, kCaptureStart + 309, 154430000, DSD_ANALOG_MODE_NFM, ctcss);
    const QString again = newest_fragment(*ring, 0);
    model.refresh(ring->state);
    expect("the repeat under another stamp adds no row", model.count() == 2);
    const int row = row_recording(model, recordings, first);
    expect("the repeat joins its own reception's row", row >= 0 && row_recording(model, recordings, again) == row);
    expect("that row joins the session that heard it again", rows_in_session(model, model.session()) == 1);
    model.setRecordingProvider(nullptr);
}

/* Issue #667 (B1): a capture replayed under another saved system is that system's reception, not the first one's heard
   again: its rows stay apart (as rows_mergeable() keeps two systems apart), each system's history shows its own, and
   each row holds only its own system's recordings. */
void
test_analog_replay_under_another_system(void) {
    resetStorage();
    FakeRecordings recordings;
    CallHistoryModel model;
    model.setRecordingProvider(&recordings);
    auto ring = std::make_unique<RingFixture>();
    model.beginSessionOn(QStringLiteral("Fire"), QStringLiteral("uid-fire"));
    const QStringList fire = replay_analog_capture(*ring, DSD_ANALOG_MODE_NFM);
    model.refresh(ring->state);
    ring = std::make_unique<RingFixture>();
    model.beginSessionOn(QStringLiteral("County"), QStringLiteral("uid-county"));
    const QStringList county = replay_analog_capture(*ring, DSD_ANALOG_MODE_NFM);
    model.refresh(ring->state);
    expect("a replay under another system logs its own rows", model.count() == 6);
    for (int i = 0; i < 3; i++) {
        const int fire_row = row_recording(model, recordings, fire.at(i));
        const int county_row = row_recording(model, recordings, county.at(i));
        expect("each system's reception keeps a row of its own",
               fire_row >= 0 && county_row >= 0 && fire_row != county_row);
        expect("the other system's row is that system's",
               county_row >= 0
                   && model.data(model.index(county_row), CallHistoryModel::SystemNameRole).toString()
                          == QStringLiteral("County"));
    }
    model.setRecordingProvider(nullptr);
}

/* Issue #667 (B1): two receptions on one frequency and tone that start in the same second (500 ms from x.00, then one
   from x.55) share their whole-second start. Replayed in a fresh ring, each is heard again one to one: both rows join
   the running session, and each row holds both replays' recordings of its own reception. */
void
test_analog_replay_same_second(void) {
    resetStorage();
    FakeRecordings recordings;
    CallHistoryModel model;
    model.setRecordingProvider(&recordings);
    const dsd_receive_identity ctcss = {1000, DSD_RECEIVE_CTCSS, 1, 0, 0};
    QStringList first;
    QStringList second;
    for (QStringList* fragments : {&first, &second}) {
        model.beginSession();
        auto ring = std::make_unique<RingFixture>(); // each replay's fresh state
        commit_analog(*ring, kCaptureStart + 200, kCaptureStart + 200, 154430000, DSD_ANALOG_MODE_NFM, ctcss);
        fragments->append(newest_fragment(*ring, 0));
        commit_analog(*ring, kCaptureStart + 200, kCaptureStart + 201, 154430000, DSD_ANALOG_MODE_NFM, ctcss);
        fragments->append(newest_fragment(*ring, 0));
        model.refresh(ring->state);
    }
    expect("same-second receptions heard again add no row", model.count() == 2);
    expect("both same-second rows join the session that heard them again",
           rows_in_session(model, model.session()) == 2);
    const int earlier = row_recording(model, recordings, first.at(0));
    const int later = row_recording(model, recordings, first.at(1));
    expect("each same-second reception keeps a row of its own", earlier >= 0 && later >= 0 && earlier != later);
    expect("each same-second row holds both replays' recordings of its own reception",
           row_recording(model, recordings, second.at(0)) == earlier
               && row_recording(model, recordings, second.at(1)) == later);
    model.setRecordingProvider(nullptr);
}

/* Issue #667 (B1): rows an older build stored keep no fragments, so an update the running service's ring brings (the
   reception ran on) finds its row by matching it, exactly: same start, tone and frequency. It reaches that row only,
   which takes the fragment (its recording is the row's now) and keeps its session; the reception beside it, on the
   same frequency with the same tone, stays apart. */
void
test_analog_update_of_an_old_store_row(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754800000;
    {
        CallHistoryModel model;
        commit_analog(ring, when, when + 4, 154430000, DSD_ANALOG_MODE_NFM, {1000, DSD_RECEIVE_CTCSS, 1, 0, 0});
        commit_analog(ring, when + 6, when + 9, 154430000, DSD_ANALOG_MODE_NFM, {1000, DSD_RECEIVE_CTCSS, 1, 0, 0});
        model.refresh(ring.state);
        expect("two receptions land", model.count() == 2);
    } // destructor flushes the stores
    // An older build wrote the rows: no fragments, no mode.
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    for (int i = 0; i < rows.size(); i++) {
        QJsonObject row = rows.at(i).toObject();
        row.remove(QLatin1String("fr"));
        row.remove(QLatin1String("analogMode"));
        rows.replace(i, row);
    }
    expect("an older build's history saved", dsd_qt::json_store_save_array("call_history.json", rows));

    FakeRecordings recordings;
    CallHistoryModel restored;
    restored.setRecordingProvider(&recordings);
    expect("the older store loads both receptions", restored.count() == 2);
    const qlonglong session = restored.data(restored.index(0), CallHistoryModel::SessionRole).toLongLong();
    Event_History* later = &ring.rings[0].Event_History_Items[1];
    later->event_time = when + 12;
    ring.touchCommitted(0);
    restored.refresh(ring.state);
    expect("the update adds no row", restored.count() == 2);
    expect("it reaches the reception it updates",
           restored.data(restored.index(0), CallHistoryModel::DurationSecsRole).toInt() == 6
               && restored.data(restored.index(0), CallHistoryModel::SessionRole).toLongLong() == session);
    expect("the reception beside it is unchanged",
           restored.data(restored.index(1), CallHistoryModel::DurationSecsRole).toInt() == 4);
    expect("the updated row takes the fragment", row_recording(restored, recordings, newest_fragment(ring, 0)) == 0);
    expect("the updated row learns the mode its stored copy lost",
           restored.data(restored.index(0), CallHistoryModel::AnalogModeRole) == QLatin1String("NFM"));
    expect("the row beside it keeps its stored name and no mode",
           restored.data(restored.index(1), CallHistoryModel::NameRole) == QLatin1String("NFM")
               && restored.data(restored.index(1), CallHistoryModel::AnalogModeRole).toString().isEmpty()
               && restored.data(restored.index(1), CallHistoryModel::AnalogRole).toBool());
    restored.setRecordingProvider(nullptr);
}

/* The analog roles of the row whose start is @p start, as "analog|mode|name", or "missing". */
QString
analog_roles_at(const CallHistoryModel& model, qint64 start) {
    for (int i = 0; i < model.rowCount(); i++) {
        const QModelIndex idx = model.index(i);
        if (model.data(idx, CallHistoryModel::WhenRole).toLongLong() == start) {
            return QStringLiteral("%1|%2|%3")
                .arg(model.data(idx, CallHistoryModel::AnalogRole).toBool() ? 1 : 0)
                .arg(model.data(idx, CallHistoryModel::AnalogModeRole).toString(),
                     model.data(idx, CallHistoryModel::NameRole).toString());
        }
    }
    return QStringLiteral("missing");
}

/* Issue #667: an analog row says it is one (analog) and what it was heard in (analogMode "NFM", "AM", or empty when not
   known: audio input, or a mode byte no demodulator has), and with no other name is named by its mode ("NFM", "AM",
   "Analog"). A digital row is neither. The detail sheet's map carries both; both survive a reload, and the store keeps
   only a known mode. */
void
test_analog_roles_and_persistence(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754810000;
    {
        CallHistoryModel model;
        commit_analog(ring, when, when + 4, 154430000, DSD_ANALOG_MODE_NFM, {1000, DSD_RECEIVE_CTCSS, 1, 0, 0});
        commit_analog(ring, when + 10, when + 14, 119100000, DSD_ANALOG_MODE_AM);
        commit_analog(ring, when + 20, when + 24, 0, DSD_ANALOG_MODE_UNKNOWN);
        commit_analog(ring, when + 30, when + 34, 0, 0xFFU);
        ring.commit(0, 1001, 2001, when + 40, when + 44)->analog_mode = DSD_ANALOG_MODE_AM; // a stray byte
        model.refresh(ring.state);
        expect("five rows land", model.count() == 5);
        expect("an NFM row", analog_roles_at(model, when) == QLatin1String("1|NFM|NFM"));
        expect("an AM row", analog_roles_at(model, when + 10) == QLatin1String("1|AM|AM"));
        expect("an analog row of unknown mode", analog_roles_at(model, when + 20) == QLatin1String("1||Analog"));
        expect("a mode byte no demodulator has reads unknown",
               analog_roles_at(model, when + 30) == QLatin1String("1||Analog"));
        expect("a digital row is neither, whatever its mode byte",
               analog_roles_at(model, when + 40) == QLatin1String("0||Talkgroup 1001"));
        const int am = 3; // newest first: +40, +30, +20, then the AM row at +10
        const QVariantMap record =
            model.recordFor(QString::number(model.data(model.index(am), CallHistoryModel::UidRole).toLongLong()));
        expect("the detail map carries the analog roles",
               record.value(QStringLiteral("analog")).toBool()
                   && record.value(QStringLiteral("analogMode")).toString() == QLatin1String("AM"));
    } // destructor flushes the stores
    const QJsonArray stored = dsd_qt::json_store_load_array("call_history.json");
    QStringList modes;
    for (const auto& value : stored) {
        const QJsonObject row = value.toObject();
        modes.append(row.contains(QLatin1String("analogMode"))
                         ? QString::number(row.value(QLatin1String("analogMode")).toInt())
                         : QStringLiteral("-"));
    }
    expect("the store keeps only a known mode", modes == QStringList({"-", "-", "-", "2", "1"}));
    {
        CallHistoryModel restored;
        expect("the roles survive a reload",
               analog_roles_at(restored, when) == QLatin1String("1|NFM|NFM")
                   && analog_roles_at(restored, when + 10) == QLatin1String("1|AM|AM")
                   && analog_roles_at(restored, when + 20) == QLatin1String("1||Analog")
                   && analog_roles_at(restored, when + 40) == QLatin1String("0||Talkgroup 1001"));
    }

    /* A store older builds wrote: no mode, and every analog row named "NFM" unless something else named it. Its rows
       read unknown and keep the name they were stored with; one stored with no name is named by its mode. A hand-edited
       mode no demodulator has, or one on a digital row, reads unknown. */
    QJsonArray rows = stored;
    for (int i = 0; i < rows.size(); i++) {
        QJsonObject row = rows.at(i).toObject();
        const qint64 start = row.value(QLatin1String("when")).toVariant().toLongLong();
        row.remove(QLatin1String("analogMode"));
        if (start == static_cast<qint64>(when) + 10) {
            row.insert(QLatin1String("name"), QStringLiteral("NFM"));
        } else if (start == static_cast<qint64>(when) + 20) {
            row.insert(QLatin1String("name"), QString());
        } else if (start == static_cast<qint64>(when) + 30) {
            row.insert(QLatin1String("name"), QString());
            row.insert(QLatin1String("analogMode"), 7);
        } else if (start == static_cast<qint64>(when) + 40) {
            row.insert(QLatin1String("analogMode"), 2);
        }
        rows.replace(i, row);
    }
    expect("an older build's history saved", dsd_qt::json_store_save_array("call_history.json", rows));
    CallHistoryModel older;
    expect("an older NFM row reads unknown and keeps its name",
           analog_roles_at(older, when) == QLatin1String("1||NFM"));
    expect("its mode is never guessed from its name", analog_roles_at(older, when + 10) == QLatin1String("1||NFM"));
    expect("an older row with no name is named by its mode",
           analog_roles_at(older, when + 20) == QLatin1String("1||Analog"));
    expect("a mode no demodulator has reads unknown", analog_roles_at(older, when + 30) == QLatin1String("1||Analog"));
    expect("a mode on a digital row reads none",
           analog_roles_at(older, when + 40) == QLatin1String("0||Talkgroup 1001"));
}

/* The detail sheet follows a row by its uid: unique, kept through merges and reloads, found by rowForUid() wherever
 * the row moved, announced by rowPositionsChanged(), and never handed out again, not even after a clear. */
void
test_rows_have_stable_uids(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754700000;
    qint64 uidA = 0;
    qint64 uidB = 0;
    {
        CallHistoryModel model;
        ring.commit(0, 21001, 501, when, when + 4);
        ring.commit(0, 21002, 502, when + 60, when + 64);
        model.refresh(ring.state);
        uidB = uid_at(model, 0);
        uidA = uid_at(model, 1);
        expect("rows get distinct uids", uidA > 0 && uidB > 0 && uidA != uidB);
        expect("rowForUid finds each row", model.rowForUid(uidA) == 1 && model.rowForUid(uidB) == 0);
        expect("no row has uid 0 or an unknown uid", model.rowForUid(0) == -1 && model.rowForUid(uidB + 1000) == -1);

        QSignalSpy moved(&model, &CallHistoryModel::rowPositionsChanged);
        ring.commit(0, 21003, 503, when + 120, when + 124);
        model.refresh(ring.state);
        expect("a new row announces that rows moved", moved.count() >= 1);
        expect("rowForUid follows a row pushed down", model.rowForUid(uidA) == 2 && model.rowForUid(uidB) == 1);

        // A second fragment of call A folds into its row, which keeps its uid.
        ring.commit(1, 21001, 501, when + 2, when + 10);
        model.refresh(ring.state);
        expect("the fragment merges", model.count() == 3);
        expect("a merge keeps the row's uid",
               uid_at(model, model.rowForUid(uidA)) == uidA
                   && model.data(model.index(model.rowForUid(uidA)), CallHistoryModel::TgRole).toULongLong() == 21001U);
    }
    qint64 highest = 0;
    {
        CallHistoryModel restored;
        expect("uids survive a reload",
               restored.count() == 3 && restored.rowForUid(uidA) == 2 && restored.rowForUid(uidB) == 1);
        for (int i = 0; i < restored.count(); i++) {
            highest = qMax(highest, uid_at(restored, i));
        }
        restored.clearAll();
        restored.waitForSaveForTest();
    }
    RingFixture fresh;
    CallHistoryModel relaunched;
    relaunched.beginSession(); // a start: the clear made before any read covers nothing new
    fresh.commit(0, 21004, 504, when + 300, when + 304);
    relaunched.refresh(fresh.state);
    expect("a uid is never handed out twice, not even after a clear",
           relaunched.count() == 1 && uid_at(relaunched, 0) > highest);
}

/* A row carries every ring fragment it absorbed (persisted as `fr`): through a merge, a reload and load folding. A
 * fragment leaves with its row when the log is trimmed or cleared. */
void
test_fragments_follow_merges_reloads_and_trims(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754701000;
    FakeRecordings recordings;
    QString first;
    QString second;
    {
        CallHistoryModel model;
        model.setRecordingProvider(&recordings);
        ring.commit(0, 22001, 601, when, when + 4);
        first = newest_fragment(ring, 0);
        model.refresh(ring.state);
        ring.commit(1, 22001, 601, when + 3, when + 9);
        second = newest_fragment(ring, 1);
        model.refresh(ring.state);
        expect("the two fragments are one row", model.count() == 1);
        recordings.recorded.insert(second);
        expect("the merged row holds the fragment it absorbed", has_recording(model, 0));
        recordings.recorded = {first};
        expect("and its own", has_recording(model, 0));
        model.startSaveForTest();
        model.waitForSaveForTest();
        QCoreApplication::processEvents();
        const QJsonArray fr = stored_row_with_tg(dsd_qt::json_store_load_array("call_history.json"), 22001)
                                  .value(QLatin1String("fr"))
                                  .toArray();
        expect("both fragments are stored", fr.size() == 2 && fr.contains(first) && fr.contains(second));
    }
    {
        CallHistoryModel restored;
        restored.setRecordingProvider(&recordings);
        recordings.recorded = {second};
        expect("the fragments survive a reload", restored.count() == 1 && has_recording(restored, 0));
    }

    // Two stored rows of one call fold at load (a log written before coalescing, or by two models): the survivor
    // holds both rows' fragments.
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    QJsonObject a = rows.at(0).toObject();
    QJsonObject b = a;
    a.insert(QLatin1String("fr"), QJsonArray{QStringLiteral("00000000000000AA:0:1")});
    a.remove(QLatin1String("uid"));
    b.insert(QLatin1String("when"), static_cast<qint64>(when) + 5);
    b.insert(QLatin1String("slot"), 1);
    b.insert(QLatin1String("seq"), 9);
    b.insert(QLatin1String("fr"), QJsonArray{QStringLiteral("00000000000000BB:1:9")});
    b.remove(QLatin1String("uid"));
    expect("two-row store written", dsd_qt::json_store_save_array("call_history.json", QJsonArray{a, b}));
    {
        CallHistoryModel folded;
        folded.setRecordingProvider(&recordings);
        expect("the stored rows fold into one", folded.count() == 1 && uid_at(folded, 0) > 0);
        recordings.recorded = {QStringLiteral("00000000000000AA:0:1")};
        const bool hasA = has_recording(folded, 0);
        recordings.recorded = {QStringLiteral("00000000000000BB:1:9")};
        expect("load folding keeps both rows' fragments", hasA && has_recording(folded, 0));
    }

    // Trimmed and cleared rows give their fragments up: news about them reaches no row.
    resetStorage();
    RingFixture busy;
    CallHistoryModel full;
    full.setRecordingProvider(&recordings);
    busy.commit(0, 9999, 1, 1754600000 - 100, 1754600000 - 96);
    const QString oldest = newest_fragment(busy, 0);
    full.refresh(busy.state);
    fill_live_session(full, busy, 4, 30000);
    expect("the oldest row is trimmed", full.count() == 1000 && rows_in_tg_range(full, 9999, 9999) == 0);
    QSignalSpy changed(&full, &QAbstractItemModel::dataChanged);
    recordings.recorded = {oldest};
    full.recordingFragmentsChanged({oldest});
    expect("a trimmed row's fragment reaches no row", changed.isEmpty());
    const QString kept = newest_fragment(busy, 0);
    full.clearAll();
    full.recordingFragmentsChanged({kept});
    expect("a cleared row's fragment reaches no row", changed.isEmpty());
}

/* Issue #809: a Signal search recording names its call by fragment, and uidForFragment() finds the row holding it: the
 * row it merged into, across a reload, and none (0) once its row is trimmed or the log cleared. */
void
test_uid_for_fragment(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754701500;
    QString first;
    QString second;
    qint64 uid = 0;
    {
        CallHistoryModel model;
        ring.commit(0, 22101, 611, when, when + 4);
        first = newest_fragment(ring, 0);
        model.refresh(ring.state);
        uid = uid_at(model, 0);
        expect("a fragment finds its row", uid > 0 && qRound64(model.uidForFragment(first)) == uid);
        ring.commit(1, 22101, 611, when + 3, when + 9);
        second = newest_fragment(ring, 1);
        model.refresh(ring.state);
        expect("the fragment merges", model.count() == 1);
        expect("a fragment merged into a row finds that row",
               qRound64(model.uidForFragment(second)) == uid && qRound64(model.uidForFragment(first)) == uid);
        expect("a fragment no row holds finds none",
               qRound64(model.uidForFragment(QStringLiteral("0000000000000000:0:1"))) == 0
                   && qRound64(model.uidForFragment(QString())) == 0);
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    {
        CallHistoryModel restored;
        expect("a reload keeps the lookup", qRound64(restored.uidForFragment(second)) == uid);
    }

    resetStorage();
    RingFixture busy;
    CallHistoryModel full;
    busy.commit(0, 9999, 1, 1754600000 - 100, 1754600000 - 96);
    const QString oldest = newest_fragment(busy, 0);
    full.refresh(busy.state);
    expect("the oldest call has a row", qRound64(full.uidForFragment(oldest)) > 0);
    fill_live_session(full, busy, 4, 30000);
    expect("the oldest row is trimmed", full.count() == 1000 && rows_in_tg_range(full, 9999, 9999) == 0);
    expect("a trimmed row's fragment finds none", qRound64(full.uidForFragment(oldest)) == 0);
    const QString kept = newest_fragment(busy, 0);
    expect("a kept row's fragment finds it", full.rowForUid(qRound64(full.uidForFragment(kept))) >= 0);
    full.clearAll();
    expect("a cleared row's fragment finds none", qRound64(full.uidForFragment(kept)) == 0);
}

/* The recording roles come from the provider by fragment: none without one; a recording the store knew before its
 * row arrived shows when the row does; one added or deleted later reaches its row through the change news. */
void
test_recording_roles_follow_the_provider(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754702000;
    CallHistoryModel model;
    ring.commit(0, 23001, 701, when, when + 4);
    const QString early = newest_fragment(ring, 0);
    model.refresh(ring.state);
    expect("no provider, no recordings",
           !has_recording(model, 0) && model.data(model.index(0), CallHistoryModel::RecordingsRole).toList().isEmpty());

    FakeRecordings recordings;
    recordings.recorded.insert(
        dsd_qt::call_history_fragment_key(ring.rings[0].instance, 0, ring.rings[0].push_seq + 1U));
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.setRecordingProvider(&recordings);
    expect("a new provider refreshes the recording roles",
           data_changed_with(changed, {CallHistoryModel::RecordingsRole, CallHistoryModel::HasRecordingRole}));
    ring.commit(0, 23002, 702, when + 60, when + 64);
    model.refresh(ring.state);
    expect("a recording journaled before its row shows when the row arrives",
           has_recording(model, 0)
               && model.data(model.index(0), CallHistoryModel::RecordingsRole).toList().size() == 1);
    expect("the older row has none", !has_recording(model, 1));

    changed.clear();
    model.recordingFragmentsChanged({QStringLiteral("0000000000000000:0:1")});
    expect("news about a fragment no row holds changes nothing", changed.isEmpty());
    recordings.recorded.insert(early);
    model.recordingFragmentsChanged({early});
    expect("a recording added later reaches its row",
           changed.count() == 1 && qvariant_cast<QModelIndex>(changed.at(0).at(0)) == model.index(1)
               && data_changed_with(changed, {CallHistoryModel::RecordingsRole, CallHistoryModel::HasRecordingRole})
               && has_recording(model, 1));
    changed.clear();
    recordings.recorded.remove(early);
    model.recordingFragmentsChanged({early});
    expect("a deleted recording leaves its row", changed.count() == 1 && !has_recording(model, 1));
    // The detail sheet follows its row by uid, wherever the row has moved.
    const qint64 newest = model.data(model.index(0), CallHistoryModel::UidRole).toLongLong();
    expect("a row's recordings by its uid",
           model.recordingsForUid(newest) == model.data(model.index(0), CallHistoryModel::RecordingsRole).toList()
               && model.recordingsForUid(newest).size() == 1);
    expect("a uid no row has has none",
           model.recordingsForUid(newest + 1000).isEmpty() && model.recordingsForUid(0).isEmpty());
    model.setRecordingProvider(nullptr);
    expect("without the provider no row has one", !has_recording(model, 0));
}

/* Issue #708, B6: a save that does not reach the disk was treated as done. A store that cannot be written (here a
 * directory in its place, which fails for root too) keeps the log owed: the save is retried on a growing delay with no
 * new change, the failure is reported once, and the next save that commits both stores ends it. */
void
test_failed_save_is_kept_and_retried(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754703000;
    CallHistoryModel model;
    ring.commit(0, 24001, 801, when, when + 4);
    model.refresh(ring.state);
    const QString seenPath = dsd_qt::json_store_path("call_history_seen.json");
    expect("the seen store is blocked", QDir().mkpath(seenPath));
    QSignalSpy failing(&model, &CallHistoryModel::saveFailingChanged);
    g_saveFailedWarnings = 0;
    model.startSaveForTest();
    settle_save(model);
    expect("a half-written save is a failure", model.saveFailing() && failing.count() == 1);
    expect("the rows store that could be written was", stored_tg(24001U));
    expect("the failed save is retried without another change", model.savePendingForTest());
    const int firstRetry = model.saveDelayMsForTest();
    expect("the first retry waits the save delay", firstRetry >= 3000);
    model.startSaveForTest(); // the retry, as its timer would
    settle_save(model);
    expect("a failure again keeps it owed, on a longer delay",
           model.saveFailing() && model.savePendingForTest() && model.saveDelayMsForTest() > firstRetry);
    expect("the failure is reported once", failing.count() == 1 && g_saveFailedWarnings == 1);

    expect("the seen store is unblocked", QDir(seenPath).removeRecursively());
    model.startSaveForTest();
    settle_save(model);
    expect("the next save that commits both stores ends the failure", !model.saveFailing() && failing.count() == 2);
    expect("both stores hold the log",
           stored_tg(24001U) && dsd_qt::json_store_load_array("call_history_seen.json").size() == 1);
    expect("nothing is owed after it", !model.savePendingForTest());

    // Both stores blocked: still one report, and saveNow() with the log owed tries at once.
    const QString rowsPath = dsd_qt::json_store_path("call_history.json");
    QFile::remove(rowsPath);
    QFile::remove(seenPath);
    expect("both stores are blocked", QDir().mkpath(rowsPath) && QDir().mkpath(seenPath));
    ring.commit(0, 24002, 802, when + 60, when + 64);
    model.refresh(ring.state);
    model.saveNow();
    settle_save(model);
    expect("a save of neither store is a failure, owed again", model.saveFailing() && model.savePendingForTest());
    expect("a new run of failures is reported once more", g_saveFailedWarnings == 2);
    QDir(rowsPath).removeRecursively();
    QDir(seenPath).removeRecursively();
    model.saveNow();
    settle_save(model);
    expect("saveNow on an owed log writes it", !model.saveFailing() && stored_tg(24002U));
}

/* Issue #828: history stores that could not be read load empty, and the first save, on the save worker, keeps each one
 * byte for byte instead of writing over it. */
void
test_unreadable_stores_are_kept_by_the_worker(void) {
    resetStorage();
    const QString rowsPath = dsd_qt::json_store_path("call_history.json");
    const QString seenPath = dsd_qt::json_store_path("call_history_seen.json");
    const auto put = [](const QString& path, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const auto bytes = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<unopenable>");
    };
    const QByteArray rowsJunk("[{\"tg\":26000,");
    const QByteArray seenJunk("{\"seen\":[]}");
    expect("unreadable history fixtures", put(rowsPath, rowsJunk) && put(seenPath, seenJunk));
    RingFixture ring;
    const time_t when = 1754705000;
    {
        CallHistoryModel model;
        expect("unreadable history loads empty", model.count() == 0);
        expect("nothing moves at load", bytes(rowsPath) == rowsJunk && bytes(seenPath) == seenJunk);
        ring.commit(0, 26001, 1001, when, when + 4);
        model.refresh(ring.state);
        model.startSaveForTest();
        settle_save(model);
        expect("the worker's save lands", !model.saveFailing() && stored_tg(26001U));
        expect("the rows store is kept", bytes(rowsPath + ".unreadable-1") == rowsJunk);
        expect("the seen store is kept", bytes(seenPath + ".unreadable-1") == seenJunk);
    }
    expect("later saves keep nothing more",
           !QFile::exists(rowsPath + ".unreadable-2") && !QFile::exists(seenPath + ".unreadable-2"));
}

/* B6 across a restart: a save that wrote the rows store but not the seen store leaves a log whose seen entries lag.
 * A relaunched model reading the same ring logs no row twice. */
void
test_restart_after_a_half_written_save(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754704000;
    const QString seenPath = dsd_qt::json_store_path("call_history_seen.json");
    {
        CallHistoryModel model;
        ring.commit(0, 25001, 901, when, when + 4);
        model.refresh(ring.state);
        model.startSaveForTest();
        settle_save(model);
        expect("the first save lands", stored_tg(25001U) && !model.saveFailing());
        expect("the seen store is blocked", QDir().mkpath(seenPath + QStringLiteral(".blocked"))
                                                && QFile::remove(seenPath)
                                                && QDir().rename(seenPath + QStringLiteral(".blocked"), seenPath));
        ring.commit(0, 25002, 902, when + 60, when + 64);
        ring.commit(1, 25003, 903, when + 120, when + 124);
        model.refresh(ring.state);
        model.startSaveForTest();
        settle_save(model);
        expect("the rows land, the seen entries do not", stored_tg(25002U) && stored_tg(25003U) && model.saveFailing());
    } // the destructor's save fails the same way
    QDir(seenPath).removeRecursively();
    CallHistoryModel relaunched;
    expect("the relaunch has every row", relaunched.count() == 3);
    relaunched.refresh(ring.state);
    expect("and logs none twice from the ring", relaunched.count() == 3);
}

/* The view's rows, by talkgroup, in view order. */
QList<qulonglong>
view_tgs(const CallHistoryFilterModel& view) {
    QList<qulonglong> tgs;
    for (int i = 0; i < view.rowCount(); i++) {
        tgs.append(view.data(view.index(i, 0), CallHistoryModel::TgRole).toULongLong());
    }
    std::sort(tgs.begin(), tgs.end());
    return tgs;
}

/* The search finds a call by the frequency it was heard on, but only when the text says it is a frequency: a decimal
 * point or the MHz unit. A bare number stays an ID search, so TG 851 on an 800 MHz system does not return every call.
 * Labels hold none of the probe texts, so each match is the frequency's or an ID's. */
void
test_search_by_frequency(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    const time_t when = 1754515000;
    RingFixture::carrier(ring.commit(0, 21001, 501, when, when + 4, "", "Alpha"), 851012500);
    RingFixture::carrier(ring.commit(0, 21002, 502, when + 10, when + 14, "", "Bravo"), 852012500);
    RingFixture::carrier(ring.commit(0, 851, 503, when + 20, when + 24, "", "Charlie"), 460000000);
    ring.commit(0, 21004, 504, when + 30, when + 34, "", "Delta");
    model.refresh(ring.state);
    expect("four calls land", model.count() == 4 && view.count() == 4);

    QSignalSpy textChanged(&view, &CallHistoryFilterModel::filterTextChanged);
    QSignalSpy anyChanged(&view, &CallHistoryFilterModel::filterChanged);
    expect("filter spies connect", textChanged.isValid() && anyChanged.isValid());
    expect("no frequency query before any search", !view.frequencyQuery());

    view.setFilterText(QStringLiteral("851.0125"));
    expect("a frequency finds the call heard on it", view_tgs(view) == QList<qulonglong>{21001});
    expect("the text reads as a frequency", view.frequencyQuery());
    expect("the frequency query is told apart per row",
           view.frequencyMatches(851012500) && !view.frequencyMatches(852012500) && !view.frequencyMatches(0));
    expect("a search change notifies the text and the filter", textChanged.count() == 1 && anyChanged.count() == 1);

    view.setFilterText(QStringLiteral("851.0125 MHz"));
    expect("the unit is accepted", view_tgs(view) == QList<qulonglong>{21001});
    view.setFilterText(QStringLiteral("851."));
    expect("whole MHz with the point", view_tgs(view) == QList<qulonglong>{21001});
    view.setFilterText(QStringLiteral("85"));
    expect("a bare number is no frequency query", !view.frequencyQuery());
    view.setFilterText(QStringLiteral("851"));
    expect("a bare number finds the talkgroup, not the calls on 851 MHz", view_tgs(view) == QList<qulonglong>{851});
    expect("and is no frequency query", !view.frequencyQuery() && !view.frequencyMatches(851012500));

    textChanged.clear();
    anyChanged.clear();
    view.setFilterKind(1);
    expect("a pill change leaves the search text's notify alone", textChanged.count() == 0 && anyChanged.count() == 1);
    view.setFilterKind(0);
    view.setFilterText(QString());
    expect("an empty search shows everything", view.count() == 4 && !view.frequencyQuery());
}

/* An ID the row does not print is not searched: a zero talkgroup or unit is "none decoded". The search text is
 * trimmed, and a blank one filters nothing. */
void
test_search_matches_what_the_row_shows(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    const time_t when = 1754516000;
    ring.commit(0, 0, 0, when, when + 4, "", "", "Echo");
    ring.commit(0, 4321, 0, when + 10, when + 14, "", "Golf");
    ring.commit(0, 0, 7654, when + 20, when + 24, "", "", "Hotel");
    ring.commit(0, 21001, 501, when + 30, when + 34, "", "Alpha");
    model.refresh(ring.state);
    expect("the four calls land, the talkgroup-less ones on their channels", model.count() == 4);

    view.setFilterText(QStringLiteral("0"));
    expect("a zero no row shows matches nothing", view_tgs(view) == QList<qulonglong>{21001});

    view.setFilterText(QStringLiteral("\t21001 \n"));
    expect("the search is trimmed", view_tgs(view) == QList<qulonglong>{21001});
    expect("the property keeps the text as typed", view.filterText() == QStringLiteral("\t21001 \n"));
    view.setFilterText(QStringLiteral(" \t"));
    expect("a blank search filters nothing", view.count() == 4);
}

/* The view follows a row whose frequency changes under an active frequency search: learned in place, the row
 * arrives; replaced by a newer fragment's, it leaves. Both by row signals, never a reset, and a reloaded log is
 * searched by its stored frequency. */
void
test_frequency_search_follows_the_row(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754517000;
    {
        CallHistoryModel model;
        CallHistoryFilterModel view;
        view.setSourceModel(&model);
        view.setFilterText(QStringLiteral("851.0125"));
        Event_History* item = ring.commit(0, 22001, 601, when, when + 6, "", "Alpha");
        model.refresh(ring.state);
        expect("the call lands without a frequency, out of the search", model.count() == 1 && view.count() == 0);

        QSignalSpy inserted(&view, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&view, &QAbstractItemModel::rowsRemoved);
        QSignalSpy reset(&view, &QAbstractItemModel::modelReset);
        expect("view spies connect", inserted.isValid() && removed.isValid() && reset.isValid());
        RingFixture::carrier(item, 851012500);
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("a frequency learned in place brings the row in",
               view.count() == 1 && inserted.count() == 1 && reset.count() == 0);

        RingFixture::carrier(ring.commit(0, 22001, 601, when + 8, when + 12, "", "Alpha"), 851037500);
        model.refresh(ring.state);
        expect("the newer fragment's frequency is the row's",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::FreqHzRole).toLongLong() == 851037500);
        expect("and takes the row out of the search", view.count() == 0 && removed.count() == 1 && reset.count() == 0);
    } // destructor flushes the stores
    CallHistoryModel restored;
    CallHistoryFilterModel view;
    view.setSourceModel(&restored);
    view.setFilterText(QStringLiteral("851.0375"));
    expect("a reloaded row is found by its stored frequency", restored.count() == 1 && view.count() == 1);
}

/* #782: the Android app's teardown times this destructor, which waits for its save pool and may save. Off unless the
 * markers are on, as everywhere but that teardown. */
void
test_destructor_marks_its_teardown_stage(void) {
    resetStorage();
    TeardownCapture::lines().clear();
    { CallHistoryModel model; }
    expect("no teardown marker while markers are off", TeardownCapture::lines().isEmpty());
    {
        const TeardownCapture capture;
        auto model = std::make_unique<CallHistoryModel>();
        model.reset();
        expect("~CallHistoryModel marks its teardown stage",
               TeardownCapture::lines()
                   == QStringList({QStringLiteral("teardown: ~CallHistoryModel started"),
                                   QStringLiteral("teardown: ~CallHistoryModel took 7 ms")}));
    }
    { CallHistoryModel model; }
    expect("no teardown marker after the markers go off", TeardownCapture::lines().size() == 2);
}

/* B1: a row keeps its end. A span longer than the plausible cap reads as unknown on every path (a fold, an update, a
 * reload, a spill fold). A row chains from its stored end only while its duration is known; a row past the cap chains
 * from its start (row_merge_end_secs()). */
int
duration_of(const CallHistoryModel& model, int row) {
    return row >= 0 && row < model.count() ? model.data(model.index(row), CallHistoryModel::DurationSecsRole).toInt()
                                           : -2;
}

/* The end the rows store keeps for the row of talkgroup @p tg, 0 when it keeps none. */
qint64
stored_end_of_tg(qulonglong tg) {
    const QJsonObject row = stored_row_with_tg(dsd_qt::json_store_load_array("call_history.json"), tg);
    return row.value(QLatin1String("end")).toVariant().toLongLong();
}

/* The spill line the background tap writes for the one call @p ring holds now: the row and seen entry a model that
 * logs it alone stores, as test_site_spill_round_trip() builds one. Leaves the stores empty. */
QByteArray
spill_line_of(const RingFixture& ring) {
    resetStorage();
    {
        CallHistoryModel model;
        model.refresh(ring.state);
    } // destructor flushes the stores
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    const QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    expect("one call to spill", rows.size() == 1 && seen.size() == 1);
    QJsonObject seenEntry = seen.at(0).toObject();
    QJsonObject line;
    line.insert(QLatin1String("v"), 1);
    line.insert(QLatin1String("key"), seenEntry.value(QLatin1String("key")));
    seenEntry.remove(QLatin1String("key"));
    line.insert(QLatin1String("seen"), seenEntry);
    line.insert(QLatin1String("row"), rows.at(0));
    resetStorage();
    return QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
}

/* An empty log with @p lines in the spill, for the next model to fold. */
void
write_spill(const QByteArray& lines) {
    resetStorage();
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QFile spill(dsd_qt::json_store_path(QStringLiteral("call_history_spill.jsonl")));
    expect("the spill is written", spill.open(QIODevice::WriteOnly) && spill.write(lines) == lines.size());
    spill.close();
}

constexpr uint32_t kB1Src = 3101U;

/* (a) Two fragments of one call, the second within the merge window of the first, span 3,610 s: past the cap, so the
 * row reads unknown, and keeps the later end. */
void
test_fold_past_cap_is_unknown(void) {
    const time_t when = 1754600000;
    const uint32_t tg = 31001U;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        ring.commit(0, tg, kB1Src, when, when + 1800);
        model.refresh(ring.state);
        expect("the first fragment lands with its duration", model.count() == 1 && duration_of(model, 0) == 1800);
        ring.commit(0, tg, kB1Src, when + 1810, when + 3610);
        model.refresh(ring.state);
        expect("fragments spanning past the cap fold into a row of unknown duration",
               model.count() == 1 && duration_of(model, 0) == -1);
    } // destructor flushes the stores
    expect("the folded row keeps the later end", stored_end_of_tg(tg) == when + 3610);
    {
        CallHistoryModel restored;
        expect("the folded row reads unknown after a reload", restored.count() == 1 && duration_of(restored, 0) == -1);
    }

    RingFixture first;
    RingFixture second;
    first.commit(0, tg, kB1Src, when, when + 1800);
    second.commit(0, tg, kB1Src, when + 1810, when + 3610);
    write_spill(spill_line_of(first) + spill_line_of(second));
    {
        CallHistoryModel folded;
        expect("fragments folded from the spill past the cap read unknown",
               folded.count() == 1 && duration_of(folded, 0) == -1);
    }
    expect("the spill-folded row keeps the later end", stored_end_of_tg(tg) == when + 3610);
}

/* (b) A logged 3,500 s fragment whose update ends past the cap reads unknown, and keeps the update's end. */
void
test_update_past_cap_is_unknown(void) {
    const time_t when = 1754604000;
    const uint32_t tg = 31002U;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* item = ring.commit(0, tg, kB1Src, when, when + 3500);
        model.refresh(ring.state);
        expect("the fragment lands with its duration", model.count() == 1 && duration_of(model, 0) == 3500);
        item->event_time = when + 3700;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("an update past the cap reads unknown", model.count() == 1 && duration_of(model, 0) == -1);
    } // destructor flushes the stores
    expect("the updated row keeps the update's end", stored_end_of_tg(tg) == when + 3700);
    {
        CallHistoryModel restored;
        expect("the updated row reads unknown after a reload", restored.count() == 1 && duration_of(restored, 0) == -1);
    }

    RingFixture call;
    Event_History* item = call.commit(0, tg, kB1Src, when, when + 3500);
    QByteArray lines = spill_line_of(call);
    item->event_time = when + 3700;
    lines += spill_line_of(call);
    write_spill(lines);
    {
        CallHistoryModel folded;
        expect("an update folded from the spill past the cap reads unknown",
               folded.count() == 1 && duration_of(folded, 0) == -1);
    }
    expect("the spill-folded update keeps its end", stored_end_of_tg(tg) == when + 3700);
}

/* After (a), an update of the first fragment does not rebuild a plausible duration from that fragment alone. */
void
test_fold_after_cap_keeps_unknown(void) {
    const time_t when = 1754608000;
    const uint32_t tg = 31003U;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        ring.commit(0, tg, kB1Src, when, when + 1800);
        model.refresh(ring.state);
        ring.commit(0, tg, kB1Src, when + 1810, when + 3610);
        model.refresh(ring.state);
        // The first fragment sits under the second now.
        ring.rings[0].Event_History_Items[2].event_time = when + 1805;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("an update of the first fragment keeps the row unknown",
               model.count() == 1 && duration_of(model, 0) == -1);
    } // destructor flushes the stores
    expect("the row keeps its real end", stored_end_of_tg(tg) == when + 3610);
    {
        CallHistoryModel restored;
        expect("the row reads unknown after a reload", restored.count() == 1 && duration_of(restored, 0) == -1);
        ring.rings[0].Event_History_Items[2].event_time = when + 1808;
        ring.touchCommitted(0);
        restored.refresh(ring.state);
        expect("an update of the first fragment after a reload keeps the row unknown",
               restored.count() == 1 && duration_of(restored, 0) == -1);
    }

    RingFixture first;
    RingFixture second;
    Event_History* item = first.commit(0, tg, kB1Src, when, when + 1800);
    QByteArray lines = spill_line_of(first);
    second.commit(0, tg, kB1Src, when + 1810, when + 3610);
    lines += spill_line_of(second);
    item->event_time = when + 1805;
    lines += spill_line_of(first);
    write_spill(lines);
    {
        CallHistoryModel folded;
        expect("an update of the first fragment folded from the spill keeps the row unknown",
               folded.count() == 1 && duration_of(folded, 0) == -1);
    }
    expect("the spill-folded row keeps its real end", stored_end_of_tg(tg) == when + 3610);
}

/* The end the rows store keeps for the row that starts at @p when, 0 when it keeps none. */
qint64
stored_end_of_when(qint64 when) {
    for (const auto& value : dsd_qt::json_store_load_array("call_history.json")) {
        const QJsonObject row = value.toObject();
        if (row.value(QLatin1String("when")).toVariant().toLongLong() == when) {
            return row.value(QLatin1String("end")).toVariant().toLongLong();
        }
    }
    return 0;
}

/* Whether @p model holds two rows: the newest of @p newestSecs, and under it one of unknown duration. */
bool
new_row_over_unknown(const CallHistoryModel& model, int newestSecs) {
    return model.count() == 2 && duration_of(model, 0) == newestSecs && duration_of(model, 1) == -1;
}

/* After (a) the row's span is past the cap, so the merge window chains from its start, not its end: a third fragment
 * within 20 s of the real end starts a new row (a chain longer than an hour continues in a new row once it crosses the
 * cap). The first row keeps its real end. */
void
test_chain_past_cap_starts_new_row(void) {
    const time_t when = 1754612000;
    const uint32_t tg = 31004U;
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, tg, kB1Src, when, when + 1800);
        ring.commit(0, tg, kB1Src, when + 1810, when + 3610);
        model.refresh(ring.state);
        ring.commit(0, tg, kB1Src, when + 3625, when + 3640);
        model.refresh(ring.state);
        expect("a third fragment past the cap starts a new row", new_row_over_unknown(model, 15));
    } // destructor flushes the stores
    expect("the row past the cap keeps its real end", stored_end_of_when(when) == when + 3610);

    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        ring.commit(0, tg, kB1Src, when, when + 1800);
        ring.commit(0, tg, kB1Src, when + 1810, when + 3610);
        model.refresh(ring.state);
        expect("the first two fragments fold", model.count() == 1);
    } // destructor flushes the stores
    {
        CallHistoryModel restored;
        ring.commit(0, tg, kB1Src, when + 3625, when + 3640);
        restored.refresh(ring.state);
        expect("a third fragment past the cap starts a new row after a reload", new_row_over_unknown(restored, 15));
    }

    RingFixture first;
    RingFixture second;
    RingFixture third;
    first.commit(0, tg, kB1Src, when, when + 1800);
    second.commit(0, tg, kB1Src, when + 1810, when + 3610);
    third.commit(0, tg, kB1Src, when + 3625, when + 3640);
    write_spill(spill_line_of(first) + spill_line_of(second) + spill_line_of(third));
    {
        CallHistoryModel folded;
        expect("a third fragment folded from the spill past the cap starts a new row",
               new_row_over_unknown(folded, 15));
    }
    expect("the spill-folded row past the cap keeps its real end", stored_end_of_when(when) == when + 3610);
}

/* A clock-shifted fragment (a span of hours: unknown duration, its end far away) absorbs no later call of the same
 * talkgroup and source that starts inside that span. */
void
test_clock_shifted_span_absorbs_nothing(void) {
    const time_t when = 1754640000;
    const uint32_t tg = 31012U;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        ring.commit(0, tg, kB1Src, when, when + INT64_C(5) * 3600);
        model.refresh(ring.state);
        expect("a span of hours reads unknown", model.count() == 1 && duration_of(model, 0) == -1);
        ring.commit(0, tg, kB1Src, when + 7200, when + 7210);
        model.refresh(ring.state);
        expect("a later call inside the span is a row of its own", new_row_over_unknown(model, 10));
    } // destructor flushes the stores
    expect("the clock-shifted row keeps its end", stored_end_of_when(when) == when + INT64_C(5) * 3600);
    CallHistoryModel restored;
    expect("both rows survive a reload", new_row_over_unknown(restored, 10));
    ring.commit(0, tg, kB1Src, when + 9000, when + 9004);
    restored.refresh(ring.state);
    expect("a call inside the span after a reload is a row of its own",
           restored.count() == 3 && duration_of(restored, 0) == 4);
}

/* A row an older build stored has no end: a known duration implies one, an unknown duration none. */
void
test_old_row_end_derived(void) {
    const time_t when = 1754616000;
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 31005U, kB1Src, when, when + 40);
        ring.commit(0, 31006U, kB1Src, when + 100, when + 130);
        model.refresh(ring.state);
        expect("two calls land", model.count() == 2);
    } // destructor flushes the stores
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    for (int i = 0; i < rows.size(); i++) {
        QJsonObject row = rows.at(i).toObject();
        row.remove(QLatin1String("end"));
        if (row.value(QLatin1String("tg")).toVariant().toULongLong() == 31006U) {
            row.insert(QLatin1String("durationSecs"), -1);
        }
        rows.replace(i, row);
    }
    expect("an older build's history saved", dsd_qt::json_store_save_array("call_history.json", rows));
    {
        CallHistoryModel restored;
        expect("the older store loads", restored.count() == 2);
        expect("an unknown duration stays unknown", duration_of(restored, row_with_tg(restored, 31006U)) == -1);
        RingFixture other;
        other.commit(0, 31007U, kB1Src, when + 500, when + 504);
        restored.refresh(other.state);
        expect("a new call is logged", restored.count() == 3);
    } // destructor flushes the stores
    expect("a known duration implies the end", stored_end_of_tg(31005U) == when + 40);
    expect("an unknown duration implies none", stored_end_of_tg(31006U) == 0);
    CallHistoryModel again;
    RingFixture later;
    later.commit(0, 31005U, kB1Src, when + 55, when + 60);
    again.refresh(later.state);
    expect("a fragment within the window of the derived end merges",
           again.count() == 3 && duration_of(again, row_with_tg(again, 31005U)) == 60);
}

/* A merged span of exactly the cap is plausible, and keeps it. */
void
test_cap_exact_keeps_3600(void) {
    const time_t when = 1754620000;
    const uint32_t tg = 31008U;
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, tg, kB1Src, when, when + 1800);
        model.refresh(ring.state);
        ring.commit(0, tg, kB1Src, when + 1810, when + 3600);
        model.refresh(ring.state);
        expect("a merged span of exactly the cap keeps it", model.count() == 1 && duration_of(model, 0) == 3600);
    } // destructor flushes the stores
    CallHistoryModel restored;
    expect("the cap span survives a reload", restored.count() == 1 && duration_of(restored, 0) == 3600);
}

/* A row whose end was not known (its end stamp before its start) learns it from an update, and gains a duration. */
void
test_unknown_learns_end(void) {
    const time_t when = 1754624000;
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    Event_History* item = ring.commit(0, 31009U, kB1Src, when, when - 5);
    model.refresh(ring.state);
    expect("a row with no plausible end reads unknown", model.count() == 1 && duration_of(model, 0) == -1);
    item->event_time = when + 30;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("the row learns its end and its duration", model.count() == 1 && duration_of(model, 0) == 30);
}

/* A row with no start stamp (--playfiles) keeps no end, so an update (a source label learned) never turns its unknown
 * duration into 0. */
void
test_startless_row_update_stays_unknown(void) {
    const time_t when = 1754628000;
    const uint32_t tg = 31010U;
    resetStorage();
    RingFixture ring;
    Event_History* item = ring.commit(0, tg, kB1Src, 0, when);
    {
        CallHistoryModel model;
        model.refresh(ring.state);
        expect("a start-less row reads unknown", model.count() == 1 && duration_of(model, 0) == -1);
        DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", "Unit 3101");
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("the label update lands", model.count() == 1
                                             && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                                                    == QStringLiteral("Unit 3101"));
        expect("the update keeps the duration unknown", duration_of(model, 0) == -1);
    } // destructor flushes the stores
    expect("a start-less row stores no end", stored_end_of_tg(tg) == 0);
    {
        CallHistoryModel restored;
        expect("the start-less row reads unknown after a reload",
               restored.count() == 1 && duration_of(restored, 0) == -1);
    }

    RingFixture call;
    Event_History* fragment = call.commit(0, tg, kB1Src, 0, when);
    QByteArray lines = spill_line_of(call);
    DSD_SNPRINTF(fragment->s_name, sizeof(fragment->s_name), "%s", "Unit 3101");
    lines += spill_line_of(call);
    write_spill(lines);
    CallHistoryModel folded;
    expect("a label update folded from the spill keeps the duration unknown",
           folded.count() == 1 && duration_of(folded, 0) == -1);
}

/* A start-less fragment folded into a row that knows its end still extends the row to the moment it was heard. */
void
test_startless_fragment_extends_known_end(void) {
    const time_t when = 1754632000;
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    ring.commit(0, 31011U, kB1Src, when, when + 10);
    model.refresh(ring.state);
    ring.commit(0, 31011U, kB1Src, 0, when + 15);
    model.refresh(ring.state);
    expect("a start-less fragment heard after the end extends it", model.count() == 1 && duration_of(model, 0) == 15);
}

/* Two fragments of @p tg: [@p when, @p when + 10], and a start-less one heard at @p when + 15. */
void
commit_known_then_startless(RingFixture& ring, uint32_t tg, time_t when) {
    ring.commit(0, tg, kB1Src, when, when + 10);
    ring.commit(0, tg, kB1Src, 0, when + 15);
}

/* Whether @p model holds one row of @p secs. */
bool
one_row_of(const CallHistoryModel& model, int secs) {
    return model.count() == 1 && duration_of(model, 0) == secs;
}

/* A fold reads both rows' ends before it moves the start. Read newest first (a backlog), the start-less fragment is
 * the row the known one folds into: its end is its start, which the fold must not lose by taking the earlier start
 * first. The row ends at the start-less fragment in every order, and a fragment within 20 s of that end merges. */
void
test_startless_fold_keeps_later_end(void) {
    const time_t when = 1754636000;
    const uint32_t tg = 31013U;
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        commit_known_then_startless(ring, tg, when);
        model.refresh(ring.state);
        expect("a backlog fold ends at the start-less fragment", one_row_of(model, 15));
        ring.commit(0, tg, kB1Src, when + 34, when + 40);
        model.refresh(ring.state);
        expect("a fragment within 20 s of the backlog row's end merges", one_row_of(model, 40));
    }

    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, tg, kB1Src, when, when + 10);
        model.refresh(ring.state);
        ring.commit(0, tg, kB1Src, 0, when + 15);
        model.refresh(ring.state);
        expect("a live fold ends at the start-less fragment", one_row_of(model, 15));
        ring.commit(0, tg, kB1Src, when + 34, when + 40);
        model.refresh(ring.state);
        expect("a fragment within 20 s of the live row's end merges", one_row_of(model, 40));
    }

    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        commit_known_then_startless(ring, tg, when);
        model.refresh(ring.state);
    } // destructor flushes the stores
    expect("the backlog row stores the start-less fragment's end", stored_end_of_tg(tg) == when + 15);
    {
        CallHistoryModel restored;
        expect("the backlog row keeps its end after a reload", one_row_of(restored, 15));
        ring.commit(0, tg, kB1Src, when + 34, when + 40);
        restored.refresh(ring.state);
        expect("a fragment within 20 s of the reloaded row's end merges", one_row_of(restored, 40));
    }

    RingFixture known;
    RingFixture startless;
    known.commit(0, tg, kB1Src, when, when + 10);
    startless.commit(0, tg, kB1Src, 0, when + 15);
    write_spill(spill_line_of(startless) + spill_line_of(known));
    {
        CallHistoryModel folded;
        expect("a known fragment folded from the spill into a start-less row ends at the start-less one",
               one_row_of(folded, 15));
    }
}

/* A row with no start time (load() keeps one: the detail sheet leaves its time out, the export its time cells empty)
 * shows no made-up date or time in the list: its day section reads "TIME UNKNOWN" and its time is empty, not the epoch
 * in local time. A row with a start keeps its day and time. */
void
test_row_without_start_time_reads_unknown(void) {
    const time_t when = 1754644000;
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 31014U, kB1Src, when, when + 5);
        ring.commit(0, 31015U, kB1Src, when + 100, when + 105);
        model.refresh(ring.state);
        expect("two calls land", model.count() == 2);
    } // destructor flushes the stores
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    for (int i = 0; i < rows.size(); i++) {
        QJsonObject row = rows.at(i).toObject();
        if (row.value(QLatin1String("tg")).toVariant().toULongLong() == 31015U) {
            row.insert(QLatin1String("when"), 0);
            row.remove(QLatin1String("end"));
            row.insert(QLatin1String("durationSecs"), -1);
        }
        rows.replace(i, row);
    }
    expect("a row without a start time saved", dsd_qt::json_store_save_array("call_history.json", rows));
    CallHistoryModel restored;
    const int unknown = row_with_tg(restored, 31015U);
    const int known = row_with_tg(restored, 31014U);
    expect("both rows load", restored.count() == 2 && unknown >= 0 && known >= 0);
    expect("a row without a start time is in the unknown-time section",
           restored.data(restored.index(unknown), CallHistoryModel::DayLabelRole).toString()
               == QStringLiteral("TIME UNKNOWN"));
    expect("a row without a start time shows no time",
           restored.data(restored.index(unknown), CallHistoryModel::TimeTextRole).toString().isEmpty());
    expect("a row with a start keeps its time",
           restored.data(restored.index(known), CallHistoryModel::TimeTextRole).toString()
               == QDateTime::fromSecsSinceEpoch(when).toString(QStringLiteral("HH:mm")));
    expect("a row with a start keeps its day",
           restored.data(restored.index(known), CallHistoryModel::DayLabelRole).toString()
               != QStringLiteral("TIME UNKNOWN"));
}

/* Issue #902: the day sections count from the shared local-day watch. */
qint64 g_dayNowMs = 0;

qint64
day_clock() {
    return g_dayNowMs;
}

time_t
local_secs(int y, int m, int d, int hh) {
    return static_cast<time_t>(QDateTime(QDate(y, m, d), QTime(hh, 0)).toSecsSinceEpoch());
}

QString
day_label_of(const CallHistoryModel& model, qulonglong tg) {
    return model.data(model.index(row_with_tg(model, tg)), CallHistoryModel::DayLabelRole).toString();
}

/* B2, B3: the labels follow the watch, so a phone that slept through midnight relabels as the watch's own timer next
 * runs (no other check), every row at once with DayLabelRole, YESTERDAY reaching back across 1 January; a date outside
 * the current year carries its year, so one date in two years is two sections (the label is the section key). */
void
test_day_labels_follow_the_local_day(void) {
    resetStorage();
    const QLocale previousLocale;
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    g_dayNowMs = QDateTime(QDate(2025, 12, 31), QTime(20, 0)).toMSecsSinceEpoch();
    LocalDayWatch::setClockForTest(&day_clock);
    {
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 90201U, 801U, local_secs(2015, 11, 21, 10), local_secs(2015, 11, 21, 10) + 5);
        ring.commit(0, 90202U, 802U, local_secs(2025, 11, 21, 10), local_secs(2025, 11, 21, 10) + 5);
        ring.commit(0, 90203U, 803U, local_secs(2025, 12, 30, 10), local_secs(2025, 12, 30, 10) + 5);
        ring.commit(0, 90204U, 804U, local_secs(2025, 12, 31, 10), local_secs(2025, 12, 31, 10) + 5);
        model.refresh(ring.state);
        expect("day labels: four calls land", model.count() == 4);
        expect("day labels: today", day_label_of(model, 90204U) == QStringLiteral("TODAY"));
        expect("day labels: yesterday", day_label_of(model, 90203U) == QStringLiteral("YESTERDAY"));
        expect("day labels: this year's date has no year", day_label_of(model, 90202U) == QStringLiteral("FRI 21 NOV"));
        expect("day labels: an earlier year's date carries it",
               day_label_of(model, 90201U) == QStringLiteral("SAT 21 NOV 2015"));

        int dayChanges = 0;
        bool allRows = false;
        bool timeToo = false;
        const QMetaObject::Connection watchRows =
            QObject::connect(&model, &QAbstractItemModel::dataChanged,
                             [&](const QModelIndex& from, const QModelIndex& to, const QList<int>& roles) {
                                 if (roles.contains(CallHistoryModel::DayLabelRole)) {
                                     dayChanges++;
                                     allRows = from.row() == 0 && to.row() == model.count() - 1;
                                     timeToo = roles.contains(CallHistoryModel::TimeTextRole);
                                 }
                             });
        g_dayNowMs = QDateTime(QDate(2026, 1, 1), QTime(0, 0, 2)).toMSecsSinceEpoch();
        expect("day labels: nothing relabels before a check", dayChanges == 0);
        LocalDayWatch* watch = LocalDayWatch::shared();
        QTimer* timer = watch != nullptr ? watch->timerForTest() : nullptr;
        expect("day labels: the watch's timer runs", timer != nullptr && timer->isActive());
        if (timer != nullptr) {
            /* As the event loop delivers it: an expired single-shot timer. */
            timer->stop();
            QMetaObject::invokeMethod(timer, "timeout");
        }
        QObject::disconnect(watchRows);
        expect("day labels: the timer's check relabels every row once", dayChanges == 1 && allRows);
        /* A day change can be a zone change that moved midnight: a row's clock text is re-read with its section, so
         * the two never come from different zones. */
        expect("day labels: the clock text is re-read with them", timeToo);
        expect("day labels: yesterday across 1 January", day_label_of(model, 90204U) == QStringLiteral("YESTERDAY"));
        expect("day labels: last year's dates carry the year",
               day_label_of(model, 90203U) == QStringLiteral("TUE 30 DEC 2025")
                   && day_label_of(model, 90202U) == QStringLiteral("FRI 21 NOV 2025"));
    }
    LocalDayWatch::resetClockForTest();
    QLocale::setDefault(previousLocale);
}

/* ---- Issue #800: texts, positions and raw identity (B2-B5, 2.7). Synthetic coordinates and identities only. ---- */

/** Give a ring row GPS text and the admitted position it describes, as the core's position writers do. */
Event_History*
with_gps(Event_History* item, const char* text, int32_t lat_e7, int32_t lon_e7, uint64_t rev) {
    DSD_SNPRINTF(item->gps_s, sizeof(item->gps_s), "%s", text);
    item->pos.valid = 1U;
    item->pos.lat_e7 = lat_e7;
    item->pos.lon_e7 = lon_e7;
    item->pos.err_m = 10U;
    item->pos.flags = 0U;
    item->pos.kind = DSD_POS_DMR_EMBEDDED;
    item->pos.rev = rev;
    item->pos.proc_instance = 0xC0FFEE00ULL;
    return item;
}

/** Give a ring row the reception identity the core stamps (Event_History::rx_scope). */
Event_History*
with_rx(Event_History* item, uint8_t kind, uint32_t a, uint32_t b, int64_t freq_hz) {
    item->rx_scope.scope.kind = kind;
    item->rx_scope.scope.a = a;
    item->rx_scope.scope.b = b;
    item->rx_scope.identity_freq_hz = freq_hz;
    item->rx_scope.resolved = 1U;
    item->freq_hz = kind == DSD_POS_SCOPE_FEED_TUNED_FREQ ? static_cast<int64_t>(a) : freq_hz;
    return item;
}

QString
detail_of(const CallHistoryModel& model, int row) {
    return model.data(model.index(row), CallHistoryModel::DetailRole).toString();
}

/** The stored row with talkgroup @p tg and source @p src. */
QJsonObject
stored_row_with(qulonglong tg, qulonglong src) {
    for (const auto& value : dsd_qt::json_store_load_array("call_history.json")) {
        const QJsonObject row = value.toObject();
        if (row.value(QLatin1String("tg")).toVariant().toULongLong() == tg
            && row.value(QLatin1String("src")).toVariant().toULongLong() == src) {
            return row;
        }
    }
    return QJsonObject();
}

/** The stored rows and seen entries as the background tap's spill lines. */
QByteArray
stores_as_spill_lines(void) {
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    const QJsonArray seen = dsd_qt::json_store_load_array("call_history_seen.json");
    QByteArray bytes;
    for (const auto& rowValue : rows) {
        const QJsonObject row = rowValue.toObject();
        const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                                .arg(row.value(QLatin1String("slot")).toInt())
                                .arg(row.value(QLatin1String("seq")).toVariant().toULongLong())
                                .arg(row.value(QLatin1String("when")).toVariant().toLongLong())
                                .arg(row.value(QLatin1String("tg")).toVariant().toULongLong())
                                .arg(row.value(QLatin1String("kind")).toInt());
        QJsonObject line;
        line.insert(QLatin1String("v"), 1);
        line.insert(QLatin1String("key"), key);
        for (const auto& seenValue : seen) {
            QJsonObject entry = seenValue.toObject();
            if (entry.value(QLatin1String("key")).toString() == key) {
                entry.remove(QLatin1String("key"));
                line.insert(QLatin1String("seen"), entry);
            }
        }
        line.insert(QLatin1String("row"), row);
        bytes += QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
    }
    return bytes;
}

/** Write @p bytes as the spill the next model folds, beside whatever the stores hold. */
void
write_spill_beside_stores(const QByteArray& bytes) {
    QFile spill(dsd_qt::json_store_path(QStringLiteral("call_history_spill.jsonl")));
    expect("the spill is written", spill.open(QIODevice::WriteOnly) && spill.write(bytes) == bytes.size());
}

/** Rewrite the stored rows and seen entries as the background tap's spill lines, and remove both stores. */
void
stores_to_spill(void) {
    const QByteArray bytes = stores_as_spill_lines();
    QFile::remove(dsd_qt::json_store_path(QStringLiteral("call_history.json")));
    QFile::remove(dsd_qt::json_store_path(QStringLiteral("call_history_seen.json")));
    write_spill_beside_stores(bytes);
}

/* B2: a voice row keeps its GPS text and text message (the terminal and the API show them), a notice with both keeps
 * both, and a late enrichment of a committed call (GPS, then a text-only GPS line, then a message) is read again in
 * place. */
void
test_voice_row_keeps_gps_and_message(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754700000;
    Event_History* call = ring.commit(0, 80001U, 8001U, when, when + 5);
    with_gps(call, "GPS: 22.50000 45.00000", 225000000, 450000000, 3U);
    DSD_SNPRINTF(call->text_message, sizeof(call->text_message), "%s", "Meet at the north gate");
    Event_History* notice = ring.commitNotice(0, when + 30, "2026-11-17 12:00:30 LRRP position");
    with_gps(notice, "LRRP: 10.00000 20.00000", 100000000, 200000000, 4U);
    DSD_SNPRINTF(notice->text_message, sizeof(notice->text_message), "%s", "Status 7");
    model.refresh(ring.state);
    expect("B2: the call and the notice land", model.count() == 2);
    const int voice = row_with_tg(model, 80001U);
    expect("B2: a voice row shows its message and its GPS text",
           voice >= 0 && detail_of(model, voice) == u8("Meet at the north gate \xC2\xB7 GPS: 22.50000 45.00000"));
    const int notice_row = voice == 0 ? 1 : 0;
    expect("B2: a notice with both keeps both, the GPS text included",
           detail_of(model, notice_row) == u8("Status 7 \xC2\xB7 LRRP: 10.00000 20.00000"));

    // A committed call the core enriches in place afterwards: read again, as the same row.
    Event_History* late = ring.commit(0, 80002U, 8002U, when + 100, when + 104);
    model.refresh(ring.state);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    with_gps(late, "GPS: 22.60000 45.10000", 226000000, 451000000, 5U);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    const int lateRow = row_with_tg(model, 80002U);
    expect("B2: a late GPS fix re-reads the committed call in place",
           model.count() == 3 && lateRow >= 0 && detail_of(model, lateRow) == QStringLiteral("GPS: 22.60000 45.10000"));
    expect("B2: the re-read announces the detail",
           !changed.isEmpty()
               && qvariant_cast<QVector<int>>(changed.last().at(2)).contains(CallHistoryModel::DetailRole));
    // The core's text-only GPS write: new text, the position dropped. The same fragment takes what it says now.
    DSD_SNPRINTF(late->gps_s, sizeof(late->gps_s), "%s", "GPS: Protected");
    late->pos.valid = 0U;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("B2: a text-only GPS update of the same call replaces its text",
           detail_of(model, row_with_tg(model, 80002U)) == QStringLiteral("GPS: Protected"));
    DSD_SNPRINTF(late->text_message, sizeof(late->text_message), "%s", "On my way");
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("B2: a late message joins the GPS text",
           model.count() == 3
               && detail_of(model, row_with_tg(model, 80002U)) == u8("On my way \xC2\xB7 GPS: Protected"));
}

/* Issue #800, P7: the map's "Show on map" pin reads a row's saved fix through the position role and recordFor(): its
 * place, error, states and format, and nothing for a row without one. */
void
test_position_role_feeds_the_map_pin(void) {
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    const time_t when = 1754701000;
    Event_History* call = ring.commit(0, 80051U, 8051U, when, when + 5);
    with_gps(call, "GPS: 22.50000 45.00000", 225000000, -450000000, 3U);
    call->pos.flags = static_cast<uint16_t>(DSD_POS_FLAG_VALIDITY_UNKNOWN | DSD_POS_FLAG_FORMAT_PROVISIONAL
                                            | DSD_POS_FLAG_ERR_LOWER_BOUND | DSD_POS_FLAG_EMERGENCY);
    call->pos.rx_time_s = when + 2;
    ring.commit(0, 80052U, 8052U, when + 20, when + 25);
    Event_History* group = ring.commit(0, 80053U, 8053U, when + 30, when + 35);
    with_gps(group, "GPS: 22.60000 45.10000", 226000000, -451000000, 3U);
    group->pos.flags = static_cast<uint16_t>(DSD_POS_FLAG_TARGET_GROUP);
    model.refresh(ring.state);
    const int withFix = row_with_tg(model, 80051U);
    const int without = row_with_tg(model, 80052U);
    const int toGroup = row_with_tg(model, 80053U);
    expect("P7: both rows land", withFix >= 0 && without >= 0);
    const QVariantMap pos = model.data(model.index(withFix), CallHistoryModel::PositionRole).toMap();
    expect("P7: the role is named for QML", model.roleNames().value(CallHistoryModel::PositionRole) == "position");
    expect("P7: the pin's place in degrees",
           std::fabs(pos.value(QStringLiteral("latitude")).toDouble() - 22.5) < 1e-9
               && std::fabs(pos.value(QStringLiteral("longitude")).toDouble() + 45.0) < 1e-9);
    expect("P7: the error and that it is a lower bound", pos.value(QStringLiteral("errorM")).toUInt() == 10U
                                                             && pos.value(QStringLiteral("errorLowerBound")).toBool());
    expect("P7: the states in their own keys",
           pos.value(QStringLiteral("validityUnknown")).toBool() && pos.value(QStringLiteral("provisional")).toBool()
               && pos.value(QStringLiteral("emergency")).toBool() && !pos.value(QStringLiteral("anonymous")).toBool()
               && !pos.value(QStringLiteral("weakCheck")).toBool());
    expect("P7: the format in words",
           pos.value(QStringLiteral("kindText")).toString() == QStringLiteral("DMR embedded GPS"));
    expect("P7: when it was received", pos.value(QStringLiteral("receivedTime")).toLongLong() == when + 2);
    // Review r2: the row's target is its call's, a talkgroup or a radio; the pin says "Talkgroup" only on the report's
    // own word.
    expect("r2: a report that names no talkgroup is not one",
           pos.contains(QStringLiteral("targetGroup")) && !pos.value(QStringLiteral("targetGroup")).toBool());
    expect("r2: a report to a talkgroup says so",
           toGroup >= 0
               && model.data(model.index(toGroup), CallHistoryModel::PositionRole)
                      .toMap()
                      .value(QStringLiteral("targetGroup"))
                      .toBool());
    expect("P7: a row without a fix has none",
           model.data(model.index(without), CallHistoryModel::PositionRole).toMap().isEmpty());
    const QString uid = model.data(model.index(withFix), CallHistoryModel::UidRole).toString();
    expect("P7: the detail sheet's record carries it",
           model.recordFor(uid).value(QStringLiteral("position")).toMap() == pos);
}

/* Review r2 (issue #800): who a call was to (Event_History::gi: 0 a talkgroup, 1 a radio, -1 not known) is the row's:
 * a role and a record key, the name a row with no other gets, part of the call's identity (a group call and a private
 * call to the same number from the same radio are two rows, a fragment that only learns the kind folds in), and kept
 * across a save and a reload (only when known: a row an older build stored reads -1). */
void
test_call_kind_is_kept_and_parts_calls(void) {
    const time_t when = 1754702000;
    {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 42U, 7U, when, when + 3)->gi = 1;
        ring.commit(1, 43U, 8U, when + 20, when + 23)->gi = -1;
        ring.commit(0, 44U, 9U, when + 40, when + 43)->gi = 0;
        model.refresh(ring.state);
        const int priv = row_with_tg(model, 42U), unknown = row_with_tg(model, 43U), group = row_with_tg(model, 44U);
        expect("r2: three rows", priv >= 0 && unknown >= 0 && group >= 0);
        expect("r2: the role names the kind",
               model.roleNames().value(CallHistoryModel::GiRole) == "gi"
                   && model.data(model.index(priv), CallHistoryModel::GiRole).toInt() == 1
                   && model.data(model.index(unknown), CallHistoryModel::GiRole).toInt() == -1
                   && model.data(model.index(group), CallHistoryModel::GiRole).toInt() == 0);
        expect("r2: a private call is named for its radio",
               model.data(model.index(priv), CallHistoryModel::NameRole).toString() == QStringLiteral("Radio 42"));
        expect("r2: not knowing the kind, for the number",
               model.data(model.index(unknown), CallHistoryModel::NameRole).toString() == QStringLiteral("Call to 43"));
        expect("r2: a group call for its talkgroup",
               model.data(model.index(group), CallHistoryModel::NameRole).toString() == QStringLiteral("Talkgroup 44"));
        const QString uid = model.data(model.index(priv), CallHistoryModel::UidRole).toString();
        expect("r2: the record carries it", model.recordFor(uid).value(QStringLiteral("gi")).toInt() == 1);
        expect("r2: the export row carries it",
               model.exportRows({priv, group}).value(0).gi == 1 && model.exportRows({priv, group}).value(1).gi == 0);
        // Re-review r1: a private call's radio is never a talkgroup heard; not knowing stays as before.
        const QList<dsd_qt::HeardTalkgroupRow> rows = model.heardTalkgroups(0, QString(), dsd_qt::HeardTarget());
        QList<uint32_t> heard;
        for (const dsd_qt::HeardTalkgroupRow& r : rows) {
            heard.append(r.id);
        }
        expect("r1: heard talkgroups leave the radio out",
               !heard.contains(42U) && heard.contains(43U) && heard.contains(44U));
    }
    {
        const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
        expect("r2: a known kind is stored, an unknown one is not",
               stored_row_with_tg(rows, 42U).value(QLatin1String("gi")).toInt(-9) == 1
                   && stored_row_with_tg(rows, 44U).value(QLatin1String("gi")).toInt(-9) == 0
                   && !stored_row_with_tg(rows, 43U).contains(QLatin1String("gi")));
        RingFixture ring;
        CallHistoryModel reloaded;
        expect("r2: a reload keeps it",
               reloaded.data(reloaded.index(row_with_tg(reloaded, 42U)), CallHistoryModel::GiRole).toInt() == 1
                   && reloaded.data(reloaded.index(row_with_tg(reloaded, 44U)), CallHistoryModel::GiRole).toInt() == 0
                   && reloaded.data(reloaded.index(row_with_tg(reloaded, 43U)), CallHistoryModel::GiRole).toInt()
                          == -1);
    }
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 50U, 9U, when, when + 3)->gi = 0;
        if (incremental) {
            model.refresh(ring.state);
        }
        ring.commit(0, 50U, 9U, when + 5, when + 8)->gi = 1;
        model.refresh(ring.state);
        expect(incremental ? "r2: a group call and a private call to one number are two rows (incremental)"
                           : "r2: a group call and a private call to one number are two rows (batched)",
               model.count() == 2);
    }
    for (int incremental = 0; incremental < 2; incremental++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 51U, 9U, when, when + 3)->gi = -1;
        if (incremental) {
            model.refresh(ring.state);
        }
        ring.commit(0, 51U, 9U, when + 5, when + 8)->gi = 0;
        model.refresh(ring.state);
        expect(incremental ? "r2: a fragment that learns the kind folds in (incremental)"
                           : "r2: a fragment that learns the kind folds in (batched)",
               model.count() == 1 && model.data(model.index(0), CallHistoryModel::GiRole).toInt() == 0
                   && model.data(model.index(0), CallHistoryModel::NameRole).toString()
                          == QStringLiteral("Talkgroup 51"));
    }
}

/* B2: the texts and the structured position survive a save, a reload and the spill, and a reloaded or spilled seen
 * entry knows them, so the ring row reads as no update. A stored position no report could hold loads as none. */
void
test_texts_and_position_survive_save_reload_and_spill(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754701000;
    {
        CallHistoryModel model;
        Event_History* call = ring.commit(0, 80101U, 8101U, when, when + 4);
        with_gps(call, "GPS: -22.50000 -45.00000", -225000000, -450000000, 11U);
        DSD_SNPRINTF(call->text_message, sizeof(call->text_message), "%s", "Copy");
        model.refresh(ring.state);
        expect("B2 store: the call lands", model.count() == 1);
    } // destructor flushes the stores
    const QJsonObject stored = stored_row_with(80101U, 8101U);
    const QJsonObject pos = stored.value(QLatin1String("pos")).toObject();
    expect("B2 store: the texts are stored apart",
           stored.value(QLatin1String("gps")).toString() == QStringLiteral("GPS: -22.50000 -45.00000")
               && stored.value(QLatin1String("msg")).toString() == QStringLiteral("Copy")
               && !stored.contains(QLatin1String("detail")));
    expect("B2 store: the position is stored with its rev and run",
           pos.value(QLatin1String("lat")).toInt() == -225000000
               && pos.value(QLatin1String("lon")).toInt() == -450000000
               && pos.value(QLatin1String("rev")).toString() == QStringLiteral("b")
               && stored.value(QLatin1String("gpsOrd")).toInt() > 0);
    const QString expected = u8("Copy \xC2\xB7 GPS: -22.50000 -45.00000");
    {
        CallHistoryModel reloaded;
        expect("B2 store: the reload shows both", reloaded.count() == 1 && detail_of(reloaded, 0) == expected);
        QSignalSpy changed(&reloaded, &QAbstractItemModel::dataChanged);
        reloaded.refresh(ring.state);
        expect("B2 store: the reloaded seen entry knows the texts and the fix", changed.isEmpty());
    }
    stores_to_spill();
    {
        CallHistoryModel spilled;
        expect("B2 spill: the spilled row shows both", spilled.count() == 1 && detail_of(spilled, 0) == expected);
        QSignalSpy changed(&spilled, &QAbstractItemModel::dataChanged);
        spilled.refresh(ring.state);
        expect("B2 spill: the spilled seen entry knows them too", changed.isEmpty() && spilled.count() == 1);
    }
    // A hand-edited position no report could hold (latitude past 90 degrees) loads as none; the text stays.
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    QJsonObject row = rows.at(0).toObject();
    QJsonObject bad = row.value(QLatin1String("pos")).toObject();
    bad.insert(QLatin1String("lat"), 900000001);
    row.insert(QLatin1String("pos"), bad);
    rows.replace(0, row);
    expect("B2 store: the edited store is written", dsd_qt::json_store_save_array("call_history.json", rows));
    {
        CallHistoryModel edited;
        edited.startSaveForTest();
        edited.waitForSaveForTest();
    }
    const QJsonObject resaved = stored_row_with(80101U, 8101U);
    expect("B2 store: an impossible stored position loads as none",
           !resaved.contains(QLatin1String("pos")) && resaved.value(QLatin1String("gps")).toString().size() > 0);
}

/* 2.7: a spill line kept after a failed read replays an older revision of a fragment whose newer revision the save
 * already holds (the same provenance and run): the newer revision stays, its text and its position together. */
void
test_spill_replay_keeps_newer_revision(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754702000;
    QByteArray older;
    {
        CallHistoryModel model;
        Event_History* call = ring.commit(0, 80201U, 8201U, when, when + 4);
        with_gps(call, "GPS: 22.50000 45.00000", 225000000, 450000000, 5U);
        model.refresh(ring.state);
        model.startSaveForTest();
        model.waitForSaveForTest();
        older = stores_as_spill_lines();
        with_gps(call, "GPS: 22.60000 45.10000", 226000000, 451000000, 9U);
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("2.7 replay: the newer revision reads in place",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: 22.60000 45.10000"));
    } // destructor flushes the stores
    expect("2.7 replay: the save holds revision 9",
           stored_row_with(80201U, 8201U).value(QLatin1String("pos")).toObject().value(QLatin1String("rev")).toString()
               == QStringLiteral("9"));
    write_spill_beside_stores(older);
    {
        CallHistoryModel reloaded;
        expect("2.7 replay: the older spill line does not replace the newer revision",
               reloaded.count() == 1 && detail_of(reloaded, 0) == QStringLiteral("GPS: 22.60000 45.10000"));
        reloaded.startSaveForTest();
        reloaded.waitForSaveForTest();
    }
    const QJsonObject pos = stored_row_with(80201U, 8201U).value(QLatin1String("pos")).toObject();
    expect("2.7 replay: revision 9 stays stored", pos.value(QLatin1String("rev")).toString() == QStringLiteral("9")
                                                      && pos.value(QLatin1String("lat")).toInt() == 226000000);
}

/* 2.7: a spill line from before a fragment had a position (text-only GPS) never replaces the saved position the
 * fragment gained since, but a later text-only update of the same fragment (the core's text-only write) still applies:
 * readings of one fragment are ordered by the ring's commit_rev, with or without a position. */
void
test_spill_text_only_replay_keeps_saved_position(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754703000;
    QByteArray older;
    Event_History* call = nullptr;
    {
        CallHistoryModel model;
        call = ring.commit(0, 80301U, 8301U, when, when + 4);
        DSD_SNPRINTF(call->gps_s, sizeof(call->gps_s), "%s", "GPS: Protected");
        model.refresh(ring.state);
        model.startSaveForTest();
        model.waitForSaveForTest();
        older = stores_as_spill_lines();
        with_gps(call, "GPS: 22.70000 45.20000", 227000000, 452000000, 9U);
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("2.7 text replay: the fix reads in place",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: 22.70000 45.20000"));
    } // destructor flushes the stores
    expect("2.7 text replay: the spill line is text-only", !older.contains("\"pos\"") && older.contains("Protected"));
    write_spill_beside_stores(older);
    {
        CallHistoryModel reloaded;
        expect("2.7 text replay: the older text-only line does not replace the saved fix",
               reloaded.count() == 1 && detail_of(reloaded, 0) == QStringLiteral("GPS: 22.70000 45.20000"));
        reloaded.startSaveForTest();
        reloaded.waitForSaveForTest();
        const QJsonObject pos = stored_row_with(80301U, 8301U).value(QLatin1String("pos")).toObject();
        expect("2.7 text replay: revision 9 stays stored",
               pos.value(QLatin1String("rev")).toString() == QStringLiteral("9"));
        // The core's later text-only write: the same fragment's newer reading applies.
        DSD_SNPRINTF(call->gps_s, sizeof(call->gps_s), "%s", "GPS: Hidden");
        call->pos.valid = 0U;
        ring.touchCommitted(0);
        reloaded.refresh(ring.state);
        expect("2.7 text replay: a later text-only update applies",
               reloaded.count() == 1 && detail_of(reloaded, 0) == QStringLiteral("GPS: Hidden"));
        reloaded.startSaveForTest();
        reloaded.waitForSaveForTest();
    }
    expect("2.7 text replay: the later text-only update is stored without the fix",
           !stored_row_with(80301U, 8301U).contains(QLatin1String("pos"))
               && stored_row_with(80301U, 8301U).value(QLatin1String("gps")).toString()
                      == QStringLiteral("GPS: Hidden"));
}

/* 2.7: a merged row keeps each fragment's GPS unit. When the fragment whose fix it shows loses that fix (the core's
 * later text-only write, or a write that clears its GPS text and position both), the row shows the best remaining
 * fragment's fix, whatever order the readings come in, and also after a reload. A fragment whose GPS the core cleared
 * keeps no unit on the row: its fix is neither shown nor saved. */
void
test_lost_fix_gives_way_to_remaining_fragment(void) {
    const time_t when = 1754703500;
    for (const char* lost : {"GPS: Protected", ""}) {
        const bool cleared = lost[0] == '\0';
        for (int order = 0; order < 3; order++) {
            resetStorage();
            RingFixture ring;
            CallHistoryModel model;
            with_gps(ring.commit(0, 80401U, 8401U, when, when + 3), "GPS: A", 225000000, 450000000, 5U);
            with_gps(ring.commit(0, 80401U, 8401U, when + 5, when + 9), "GPS: B", 226000000, 451000000, 9U);
            Event_History* a = &ring.rings[0].Event_History_Items[2];
            Event_History* b = &ring.rings[0].Event_History_Items[1];
            if (order == 1) {
                // Fragment A is read only after B lost its fix.
                Event_History saved = *a;
                DSD_MEMSET(a, 0, sizeof(*a));
                model.refresh(ring.state);
                DSD_SNPRINTF(b->gps_s, sizeof(b->gps_s), "%s", lost);
                b->pos.valid = 0U;
                ring.touchCommitted(0);
                model.refresh(ring.state);
                *a = saved;
                ring.touchCommitted(0);
                model.refresh(ring.state);
            } else {
                model.refresh(ring.state);
                expect("2.7 lost fix: the higher rev shows first",
                       model.count() == 1 && detail_of(model, 0) == "GPS: B");
                if (order == 2) {
                    model.startSaveForTest();
                    model.waitForSaveForTest();
                    CallHistoryModel reloaded;
                    DSD_SNPRINTF(b->gps_s, sizeof(b->gps_s), "%s", lost);
                    b->pos.valid = 0U;
                    ring.touchCommitted(0);
                    reloaded.refresh(ring.state);
                    expect("2.7 lost fix: after a reload the remaining fix shows",
                           reloaded.count() == 1 && detail_of(reloaded, 0) == QStringLiteral("GPS: A"));
                    reloaded.startSaveForTest();
                    reloaded.waitForSaveForTest();
                    const QJsonObject stored = stored_row_with(80401U, 8401U);
                    expect("2.7 lost fix: after a reload the cleared fragment's fix is not saved",
                           stored.value(QLatin1String("gps")).toString() == QStringLiteral("GPS: A")
                               && stored.value(QLatin1String("gpsAlt")).toArray().size() == (cleared ? 0 : 1));
                    continue;
                }
                DSD_SNPRINTF(b->gps_s, sizeof(b->gps_s), "%s", lost);
                b->pos.valid = 0U;
                ring.touchCommitted(0);
                model.refresh(ring.state);
            }
            expect(order == 0 ? "2.7 lost fix: the remaining fragment's fix shows"
                              : "2.7 lost fix: the same row when the text-only reading comes first",
                   model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: A"));
            model.startSaveForTest();
            model.waitForSaveForTest();
            const QJsonObject stored = stored_row_with(80401U, 8401U);
            expect(cleared ? "2.7 lost fix: the cleared fragment's fix is not saved"
                           : "2.7 lost fix: the shown fix and the other fragment's text are stored",
                   stored.value(QLatin1String("gps")).toString() == QStringLiteral("GPS: A")
                       && stored.value(QLatin1String("gpsAlt")).toArray().size() == (cleared ? 0 : 1));
            if (cleared) {
                CallHistoryModel reloaded;
                expect("2.7 lost fix: the cleared fragment's fix stays gone after a reload",
                       reloaded.count() == 1 && detail_of(reloaded, 0) == QStringLiteral("GPS: A"));
            }
        }
    }
}

/* 2.7: one fragment's row whose GPS text and position the core clears (the report withdrawn) shows and saves no fix,
 * live and after a reload. */
void
test_cleared_fix_leaves_single_fragment_row(void) {
    const time_t when = 1754703700;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* call =
            with_gps(ring.commit(0, 80411U, 8411U, when, when + 3), "GPS: A", 225000000, 450000000, 5U);
        model.refresh(ring.state);
        expect("2.7 cleared fix: the fix shows first", model.count() == 1 && detail_of(model, 0) == "GPS: A");
        call->gps_s[0] = '\0';
        call->pos.valid = 0U;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("2.7 cleared fix: the row shows no fix", model.count() == 1 && detail_of(model, 0).isEmpty());
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    const QJsonObject stored = stored_row_with(80411U, 8411U);
    expect("2.7 cleared fix: no fix is saved", !stored.contains(QLatin1String("gps"))
                                                   && !stored.contains(QLatin1String("pos"))
                                                   && !stored.contains(QLatin1String("gpsAlt")));
    CallHistoryModel reloaded;
    expect("2.7 cleared fix: none after a reload", reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty());
}

/* Issue #800, B2: a notice an older build stored kept one detail, the text message when there was one, else the GPS
 * text, and named an unsummarised notice by it. Re-delivered from a fresh ring after an upgrade (a replay), the same
 * delivery now carries its message and its GPS text apart: it is matched against the field that build chose, so it
 * joins the stored row and gives it its parts rather than logging a second row. A delivery whose message differs is
 * another one. Rows of this build still compare in full. */
void
test_repeated_legacy_notice_matches_its_detail(void) {
    resetStorage();
    const time_t when = kCaptureStart + 90;
    const char* sentence = "$GPRMC,120000,A,2230.000,N,04500.000,E,0.0,0.0,171126,,,A*6C";
    const auto deliver = [&](RingFixture& ring, int slot, time_t at, const char* summary, const char* message) {
        Event_History* item = ring.commitNotice(slot, at, summary);
        DSD_SNPRINTF(item->text_message, sizeof(item->text_message), "%s", message);
        return with_gps(item, "GPS: 22.50000, 45.00000", 225000000, 450000000, 3U);
    };
    {
        CallHistoryModel model;
        model.beginSession();
        RingFixture ring;
        deliver(ring, 1, when, "2010-01-01 00:01:30 NMEA from 44", sentence);
        deliver(ring, 1, when + 5, "2010-01-01 00:01:35 ", sentence);
        model.refresh(ring.state);
        expect("legacy notice: two notices are logged", model.count() == 2);
    } // destructor flushes the stores
    // Rewritten as the older build stored them: one detail, the message, and an unsummarised notice named by it.
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    for (qsizetype i = 0; i < rows.size(); i++) {
        QJsonObject legacy = rows.at(i).toObject();
        for (const QString& key : legacy.keys()) {
            if (key.startsWith(QLatin1String("msg")) || key.startsWith(QLatin1String("gps"))
                || key == QLatin1String("pos") || key == QLatin1String("fr") || key == QLatin1String("frv")) {
                legacy.remove(key);
            }
        }
        legacy.insert(QLatin1String("detail"), QString::fromLatin1(sentence));
        if (legacy.value(QLatin1String("name")).toString() != QLatin1String("NMEA from 44")) {
            legacy.insert(QLatin1String("name"), QString::fromLatin1(sentence));
        }
        rows.replace(i, legacy);
    }
    expect("legacy notice: the older store is written", dsd_qt::json_store_save_array("call_history.json", rows));
    {
        CallHistoryModel model;
        expect("legacy notice: the stored rows show their one detail",
               model.count() == 2 && detail_of(model, 0) == QLatin1String(sentence)
                   && detail_of(model, 1) == QLatin1String(sentence));
        model.beginSession();
        RingFixture ring;
        deliver(ring, 1, when, "2010-01-01 00:01:30 NMEA from 44", sentence);
        deliver(ring, 1, when + 5, "2010-01-01 00:01:35 ", sentence);
        model.refresh(ring.state);
        expect("legacy notice: the deliveries heard again log no new row", model.count() == 2);
        expect("legacy notice: the stored rows gain their message and GPS text",
               model.count() == 2
                   && detail_of(model, 0)
                          == QString::fromLatin1(sentence) + QStringLiteral(" \u00B7 GPS: 22.50000, 45.00000")
                   && detail_of(model, 1)
                          == QString::fromLatin1(sentence) + QStringLiteral(" \u00B7 GPS: 22.50000, 45.00000"));
        // Another message at the same stamp is another delivery.
        deliver(ring, 1, when, "2010-01-01 00:01:30 NMEA from 44", "Another message");
        model.refresh(ring.state);
        expect("legacy notice: a delivery with another message is another row", model.count() == 3);
        // The migrated rows carry the name this build gives the deliveries, so a second replay, with neither row legacy
        // any more, still finds them.
        const QString line = QString::fromLatin1(sentence) + QStringLiteral(" \u00B7 GPS: 22.50000, 45.00000");
        QStringList names;
        for (int i = 0; i < model.count(); i++) {
            names.append(model.data(model.index(i), CallHistoryModel::NameRole).toString());
        }
        expect("legacy notice: the unsummarised notice is named by its whole detail line now",
               names.contains(line) && names.contains(QStringLiteral("NMEA from 44")));
        RingFixture again;
        deliver(again, 1, when, "2010-01-01 00:01:30 NMEA from 44", sentence);
        deliver(again, 1, when + 5, "2010-01-01 00:01:35 ", sentence);
        model.refresh(again.state);
        expect("legacy notice: a second replay logs no new row", model.count() == 3);
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    CallHistoryModel reloaded;
    reloaded.beginSession();
    RingFixture third;
    deliver(third, 1, when, "2010-01-01 00:01:30 NMEA from 44", sentence);
    deliver(third, 1, when + 5, "2010-01-01 00:01:35 ", sentence);
    reloaded.refresh(third.state);
    expect("legacy notice: a replay after a reload logs no new row", reloaded.count() == 3);
}

/* Issue #800, B5: on the single-slot DMR decoder a fragment whose over-the-air slot was still pending merges into a
 * slot-1 call. When a later reading of it proves slot 2, that update (found by its fragment, never merged again) is
 * held to the receive-service rule the merge applies: it is another service's, so its part and its fix are taken back
 * and the slot-1 row shows none of it, live and after a reload. */
void
test_update_proving_other_slot_is_taken_back(void) {
    const time_t when = kCaptureStart + 450;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* slot1 = ring.commit(0, 1101, 2101, when, when + 4, "", "", "Base channel");
        slot1->receive_identity = {7, DSD_RECEIVE_DCC, 1, 1, 0};
        DSD_SNPRINTF(slot1->service_label, sizeof slot1->service_label, "%s", "Dispatch");
        model.refresh(ring.state);
        Event_History* pending = with_gps(ring.commit(0, 1101, 2101, when + 5, when + 9, "", "", "Base channel"),
                                          "GPS: P", 225000000, 450000000, 4U);
        pending->receive_identity = {7, DSD_RECEIVE_DCC, 1, 0, 1};
        model.refresh(ring.state);
        expect("slot proof: the pending-slot fragment joins the slot-1 call",
               model.count() == 1 && detail_of(model, 0) == "GPS: P");
        pending->receive_identity = {7, DSD_RECEIVE_DCC, 1, 2, 0};
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("slot proof: the fragment proven on slot 2 gives the slot-1 row nothing",
               model.count() == 1 && detail_of(model, 0).isEmpty()
                   && model.data(model.index(0), CallHistoryModel::ServiceNameRole) == "Dispatch");
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    CallHistoryModel reloaded;
    expect("slot proof: none of it after a reload", reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty());
}

/* Issue #800: the core fills a committed call's unknown target in place when a reacquired segment names it
 * (watchdog_event_merge_identity_fields()), without moving the row's push stamp. The reading is found by its fragment,
 * and the row takes the numeric target with it, as it takes the fix: a scan-channel DMR call that learns talkgroup 100
 * shows it (TgRole, notified) beside its fix, and keeps both after a reload, whether the learning fragment is the row's
 * only one or one of two. */
void
test_fragment_update_learns_target(void) {
    const time_t when = kCaptureStart + 470;
    for (int fragments = 1; fragments <= 2; fragments++) {
        resetStorage();
        RingFixture ring;
        {
            CallHistoryModel model;
            if (fragments == 2) {
                ring.commit(0, 0U, 3001U, when - 6, when - 2, "", "", "Scan A");
                model.refresh(ring.state);
            }
            Event_History* item = ring.commit(0, 0U, 3001U, when, when + 4, "", "", "Scan A");
            model.refresh(ring.state);
            expect("target: the call is one row with no target",
                   model.count() == 1 && model.data(model.index(0), CallHistoryModel::TgRole).toULongLong() == 0U);
            QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
            item->target_id = 100U;
            with_gps(item, "GPS: learned", 225000000, 450000000, 3U);
            ring.touchCommitted(0);
            model.refresh(ring.state);
            expect("target: the row shows the target its fragment learned, beside the fix",
                   model.count() == 1 && model.data(model.index(0), CallHistoryModel::TgRole).toULongLong() == 100U
                       && detail_of(model, 0) == QStringLiteral("GPS: learned"));
            expect("target: the learned target is notified", data_changed_with(changed, {CallHistoryModel::TgRole}));
            model.startSaveForTest();
            model.waitForSaveForTest();
        }
        CallHistoryModel reloaded;
        expect("target: the learned target and the fix after a reload",
               reloaded.count() == 1 && reloaded.data(reloaded.index(0), CallHistoryModel::TgRole).toULongLong() == 100U
                   && detail_of(reloaded, 0) == QStringLiteral("GPS: learned"));
    }
}

/* Issue #800: a long call folds many short fragments, each into a row holding all the earlier ones. A fold looks every
 * fragment of the row up among its parts; done by a scan per fragment that is N^2 for the row of N, N^3 over the call,
 * and the UI thread stalls. The lookups read a bounded number of parts per part the row holds, per fold. */
void
test_long_call_fold_lookups_stay_linear(void) {
    const time_t when = kCaptureStart + 900;
    const qint64 fragments = 200;
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    (void)CallHistoryModel::takePartLookupStepsForTest();
    for (qint64 i = 0; i < fragments; i++) {
        ring.commit(0, 1201, 2201, when + 2 * i, when + 2 * i + 1);
        model.refresh(ring.state);
    }
    const qint64 steps = CallHistoryModel::takePartLookupStepsForTest();
    expect("long call: one row holds every fragment", model.count() == 1);
    // A fold of a row of n parts reads at most ten per part: 10 * (1 + 2 + ... + fragments) over the call.
    expect("long call: the part lookups stay linear per fold", steps <= 10 * fragments * (fragments + 1) / 2);
    // Both recording roles ask which fragments' recordings are the row's, on the GUI thread, at every delegate read:
    // its parts are read once per ask, not once per fragment.
    FakeRecordings recordings;
    model.setRecordingProvider(&recordings);
    (void)model.data(model.index(0), CallHistoryModel::RecordingsRole);
    (void)model.data(model.index(0), CallHistoryModel::HasRecordingRole);
    const qint64 roleSteps = CallHistoryModel::takePartLookupStepsForTest();
    expect("long call: the recording roles read each part once", roleSteps > 0 && roleSteps <= 2 * fragments);
    model.setRecordingProvider(nullptr);
}

/* 2.7: a committed fragment with no start reads its end as its start; the core can fill the start in later
 * (watchdog_event_merge_stamps()) or move the end, so the same fragment (ring, slot and push stamp) is read with another
 * start. That reading still replaces and withdraws its own GPS unit: a fragment is known by its ring identity, never by
 * its stamps, so an obsolete fix neither stays beside the new one nor outlives a reading that clears it, live and after
 * a reload. */
void
test_restamped_fragment_withdraws_its_fix(void) {
    const time_t when = 1754703800;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* call = with_gps(ring.commit(0, 80421U, 8421U, 0, when + 6), "GPS: A", 225000000, 450000000, 5U);
        model.refresh(ring.state);
        expect("restamped: the fix shows first", model.count() == 1 && detail_of(model, 0) == "GPS: A");
        // The start is filled in and the report replaced: the row shows the new fix only, never the old one beside it.
        call->event_start_time = when;
        with_gps(call, "GPS: B", 226000000, 451000000, 6U);
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("restamped: the new reading replaces the fragment's fix",
               model.count() == 1 && detail_of(model, 0) == "GPS: B");
        model.startSaveForTest();
        model.waitForSaveForTest();
        expect("restamped: the replaced fix is not kept beside the new one",
               !stored_row_with(80421U, 8421U).contains(QLatin1String("gpsAlt")));
        // The end moves and the core clears the report: the fragment's fix goes.
        call->event_time = when + 9;
        call->gps_s[0] = '\0';
        call->pos.valid = 0U;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("restamped: a reading that clears the fix withdraws it",
               model.count() == 1 && detail_of(model, 0).isEmpty());
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    const QJsonObject stored = stored_row_with(80421U, 8421U);
    expect("restamped: no fix is saved", !stored.contains(QLatin1String("gps"))
                                             && !stored.contains(QLatin1String("pos"))
                                             && !stored.contains(QLatin1String("gpsAlt")));
    CallHistoryModel reloaded;
    expect("restamped: none after a reload", reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty());
}

/* 2.7: a fragment read again with another start is still the fragment its row holds, found by its ring identity
 * however far down the log the row sits: no second row for it, however far the merge search reaches. */
void
test_restamped_fragment_found_by_identity(void) {
    const time_t when = 1754704000;
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    with_gps(ring.commit(0, 80431U, 8431U, 0, when + 6), "GPS: A", 225000000, 450000000, 5U);
    model.refresh(ring.state);
    ring.commitBatch(0, 40, 81000U, 9100U, when + 100);
    model.refresh(ring.state);
    expect("restamped far: 41 rows", model.count() == 41);
    Event_History* call = &ring.rings[0].Event_History_Items[41];
    expect("restamped far: the ring row is the fragment", call->target_id == 80431U && call->source_id == 8431U);
    call->event_start_time = when;
    with_gps(call, "GPS: B", 226000000, 451000000, 6U);
    ring.touchCommitted(0);
    model.refresh(ring.state);
    expect("restamped far: no second row for the fragment", model.count() == 41);
    expect("restamped far: its row reads the new fix", detail_of(model, 40) == "GPS: B");
}

/* B2: a spill line kept after a failed read replays an older reading of a fragment whose newer reading the save
 * already holds (the same fragment of the same run): nothing of the older reading comes back, neither its message nor
 * its source label (an alias the core learned in place since), as no older GPS reading does. */
void
test_spill_replay_keeps_newer_message(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754703600;
    QByteArray older;
    {
        CallHistoryModel model;
        Event_History* call = ring.commit(0, 80501U, 8501U, when, when + 4);
        DSD_SNPRINTF(call->text_message, sizeof(call->text_message), "%s", "Earlier");
        DSD_SNPRINTF(call->s_name, sizeof(call->s_name), "%s", "Radio 8501");
        model.refresh(ring.state);
        model.startSaveForTest();
        model.waitForSaveForTest();
        older = stores_as_spill_lines();
        DSD_SNPRINTF(call->text_message, sizeof(call->text_message), "%s", "Updated");
        DSD_SNPRINTF(call->s_name, sizeof(call->s_name), "%s", "Alias 8501");
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("B2 replay: the newer reading reads in place",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("Updated")
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Alias 8501"));
    } // destructor flushes the stores
    expect("B2 replay: the spill line holds the earlier reading",
           older.contains("Earlier") && older.contains("Radio 8501"));
    write_spill_beside_stores(older);
    {
        CallHistoryModel reloaded;
        expect("B2 replay: the older spill line does not bring the earlier message back",
               reloaded.count() == 1 && detail_of(reloaded, 0) == QStringLiteral("Updated"));
        expect("B2 replay: the older spill line does not bring the earlier label back",
               reloaded.count() == 1
                   && reloaded.data(reloaded.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Alias 8501"));
        reloaded.startSaveForTest();
        reloaded.waitForSaveForTest();
    }
    expect("B2 replay: the newer reading stays stored",
           stored_row_with(80501U, 8501U).value(QLatin1String("msg")).toString() == QStringLiteral("Updated")
               && stored_row_with(80501U, 8501U).value(QLatin1String("srcName")).toString()
                      == QStringLiteral("Alias 8501"));
}

/* Spec 2.14: a row that names its source never shows an anonymous report. A ring row read while its call named no
 * source keeps its anonymous fix; once the same row names a radio with the fix still on it (as an older core left it),
 * the fix and its text go, also from a store that kept them. */
void
test_named_row_drops_anonymous_position(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754706000;
    {
        CallHistoryModel model;
        Event_History* call = ring.commit(0, 80401U, 0U, when, when + 4);
        call->systype = DSD_SYNC_P25P1_POS;
        with_gps(call, "GPS: 22.80000 45.30000", 228000000, 453000000, 7U);
        call->pos.flags = DSD_POS_FLAG_SOURCE_UNVERIFIED;
        call->pos.anonymous = 1U;
        model.refresh(ring.state);
        expect("2.14: a nameless call shows its anonymous fix",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: 22.80000 45.30000"));
        call->source_id = 3001U;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("2.14: once the call names its radio, the anonymous fix goes",
               model.count() == 1 && detail_of(model, 0).isEmpty());
    } // destructor flushes the stores
    const QJsonObject stored = stored_row_with(80401U, 3001U);
    expect("2.14: the named row stores no anonymous fix",
           !stored.isEmpty() && !stored.contains(QLatin1String("pos")) && !stored.contains(QLatin1String("gps")));
    // A store that kept one (written before this rule) loads without it.
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    QJsonObject row = rows.at(0).toObject();
    QJsonObject pos;
    pos.insert(QLatin1String("lat"), 228000000);
    pos.insert(QLatin1String("lon"), 453000000);
    pos.insert(QLatin1String("err"), 10);
    pos.insert(QLatin1String("flags"), static_cast<int>(DSD_POS_FLAG_SOURCE_UNVERIFIED));
    pos.insert(QLatin1String("kind"), static_cast<int>(DSD_POS_P25_HARRIS));
    pos.insert(QLatin1String("rev"), QStringLiteral("7"));
    row.insert(QLatin1String("pos"), pos);
    row.insert(QLatin1String("gps"), QStringLiteral("GPS: 22.80000 45.30000"));
    rows.replace(0, row);
    expect("2.14: the edited store is written", dsd_qt::json_store_save_array("call_history.json", rows));
    {
        CallHistoryModel reloaded;
        expect("2.14: a stored anonymous fix on a named row loads as none",
               reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty());
    }
}

/* B2: a fragment gives the row its text and position only when it is the row's radio. An anonymous fragment's fix never
 * enriches a named call, in either order, and an update of an absorbed fragment that later names another radio keeps
 * the row's radio and its fix. */
void
test_fragment_texts_follow_the_rows_radio(void) {
    resetStorage();
    const time_t when = 1754702000;
    {
        RingFixture ring;
        CallHistoryModel model;
        with_gps(ring.commit(0, 80201U, 0U, when, when + 3), "GPS: anon", 225000000, 450000000, 7U);
        model.refresh(ring.state);
        expect("B2 identity: the anonymous fragment shows its fix", detail_of(model, 0) == QStringLiteral("GPS: anon"));
        ring.commit(0, 80201U, 8201U, when + 5, when + 9);
        model.refresh(ring.state);
        expect("B2 identity: anonymous then named is one call", model.count() == 1);
        expect("B2 identity: the anonymous fix is not the named radio's",
               model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 8201U
                   && detail_of(model, 0).isEmpty());
    }
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 80202U, 8202U, when, when + 3);
        model.refresh(ring.state);
        with_gps(ring.commit(0, 80202U, 0U, when + 5, when + 9), "GPS: anon", 225000000, 450000000, 7U);
        model.refresh(ring.state);
        expect("B2 identity: named then anonymous is one call", model.count() == 1);
        expect("B2 identity: the anonymous fix does not join the named call", detail_of(model, 0).isEmpty());
    }
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        with_gps(ring.commit(0, 80203U, 8203U, when, when + 3), "GPS: radio 8203", 225000000, 450000000, 7U);
        Event_History* absorbed = ring.commit(0, 80203U, 0U, when + 5, when + 9);
        model.refresh(ring.state);
        expect("B2 identity: the source-less fragment joins", model.count() == 1);
        // The core fills the absorbed fragment later with another radio and that radio's fix.
        absorbed->source_id = 8299U;
        with_gps(absorbed, "GPS: radio 8299", 226000000, 451000000, 9U);
        absorbed->pos.scope.kind = DSD_POS_SCOPE_TETRA;
        absorbed->pos.scope.a = 901U;
        absorbed->pos.scope.b = 9U;
        DSD_SNPRINTF(absorbed->tgt_str, sizeof(absorbed->tgt_str), "%s", "OTHER");
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("B2 identity: the row keeps its radio and its radio's fix",
               model.count() == 1 && model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 8203U
                   && detail_of(model, 0) == QStringLiteral("GPS: radio 8203"));
        model.startSaveForTest();
        model.waitForSaveForTest();
        const QJsonObject stored = stored_row_with(80203U, 8203U);
        expect("B2 identity: the other radio's sender system and target are not the row's",
               !stored.isEmpty() && !stored.contains(QLatin1String("sscope"))
                   && !stored.contains(QLatin1String("tgtText")));
    }
}

/* B2/B3: an update of an absorbed fragment that later names another radio keeps the row's source label, as it keeps the
 * row's radio and fix: another callsign on a D-STAR row (no source number to tell them apart), and the same number from
 * another sender system. */
void
test_fragment_label_follows_the_rows_radio(void) {
    const time_t when = 1754702100;
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        Event_History* first = ring.commit(0, 0U, 0U, when, when + 3, "CQCQCQ");
        first->systype = DSD_SYNC_DSTAR_HD_POS;
        DSD_SNPRINTF(first->src_str, sizeof(first->src_str), "%s", "N0ONE");
        with_gps(first, "DPRS: one", 225000000, 450000000, 3U)->pos.kind = DSD_POS_DSTAR_DPRS;
        Event_History* absorbed = ring.commit(0, 0U, 0U, when + 4, when + 7, "CQCQCQ");
        absorbed->systype = DSD_SYNC_DSTAR_HD_POS;
        model.refresh(ring.state);
        expect("label: the anonymous D-STAR fragment joins N0ONE's call",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("N0ONE"));
        // The core fills the absorbed fragment later with another caller.
        DSD_SNPRINTF(absorbed->src_str, sizeof(absorbed->src_str), "%s", "N0TWO");
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("label: another callsign does not rename the row",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString() == QStringLiteral("N0ONE")
                   && detail_of(model, 0) == QStringLiteral("DPRS: one"));
    }
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        Event_History* named = ring.commit(0, 80211U, 8211U, when + 20, when + 23);
        DSD_SNPRINTF(named->s_name, sizeof(named->s_name), "%s", "Radio A");
        with_gps(named, "GPS: radio A", 225000000, 450000000, 3U);
        named->pos.scope.kind = DSD_POS_SCOPE_TETRA;
        named->pos.scope.a = 901U;
        named->pos.scope.b = 9U;
        Event_History* absorbed = ring.commit(0, 80211U, 0U, when + 24, when + 27);
        model.refresh(ring.state);
        expect("label: the source-less fragment joins radio A's call",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Radio A"));
        // The same number, learned later from another system.
        absorbed->source_id = 8211U;
        DSD_SNPRINTF(absorbed->s_name, sizeof(absorbed->s_name), "%s", "Radio B");
        with_gps(absorbed, "GPS: radio B", 226000000, 451000000, 9U);
        absorbed->pos.scope.kind = DSD_POS_SCOPE_TETRA;
        absorbed->pos.scope.a = 902U;
        absorbed->pos.scope.b = 9U;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("label: the same number of another system does not rename the row",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Radio A")
                   && detail_of(model, 0) == QStringLiteral("GPS: radio A"));
    }
    resetStorage();
    {
        // A nameless call heard on one system, whose absorbed fragment later names a radio of another: the row names
        // no radio of the other system, by number or by name, and keeps its own fix.
        RingFixture ring;
        CallHistoryModel model;
        Event_History* nameless = ring.commit(0, 80212U, 0U, when + 40, when + 43);
        with_gps(nameless, "GPS: nameless", 225000000, 450000000, 3U);
        nameless->pos.anonymous = 1U;
        nameless->pos.scope.kind = DSD_POS_SCOPE_TETRA;
        nameless->pos.scope.a = 901U;
        nameless->pos.scope.b = 9U;
        Event_History* absorbed = ring.commit(0, 80212U, 0U, when + 44, when + 47);
        model.refresh(ring.state);
        expect("label: two nameless fragments are one call", model.count() == 1);
        absorbed->source_id = 8212U;
        DSD_SNPRINTF(absorbed->s_name, sizeof(absorbed->s_name), "%s", "Radio B");
        with_gps(absorbed, "GPS: radio B", 226000000, 451000000, 9U);
        absorbed->pos.scope.kind = DSD_POS_SCOPE_TETRA;
        absorbed->pos.scope.a = 902U;
        absorbed->pos.scope.b = 9U;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("label: a radio of another system does not name the nameless row",
               model.count() == 1 && model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 0U
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          != QStringLiteral("Radio B")
                   && detail_of(model, 0) == QStringLiteral("GPS: nameless"));
    }
}

/* 2.7: a merged row shows the fix of its newest fragment by run and rev, never by stamps. The run after a backward wall
 * clock step still orders by rev; a later run's fix wins over an earlier run's higher rev, read live or as a backlog; a
 * reload keeps the run ordinals, so a ring read again after a relaunch is the same run. Every fragment is heard on one
 * known reception (the same tuned frequency): two runs' epochs say nothing of each other, so only a known, equal
 * reception lets a later run's fix onto the row (call_history_same_reception()). */
void
test_position_order_follows_runs_and_revs(void) {
    const time_t when = 1754703000;
    const auto heard = [](Event_History* item) {
        return with_rx(item, DSD_POS_SCOPE_FEED_TUNED_FREQ, 450000000U, 0U, 0);
    };
    for (int backlog = 0; backlog < 2; backlog++) {
        resetStorage();
        RingFixture first;
        RingFixture second;
        {
            CallHistoryModel model;
            // The second fragment starts before the first (the wall clock stepped back), but its fix is newer.
            with_gps(heard(first.commit(0, 80301U, 8301U, when + 10, when + 14)), "GPS: A", 225000000, 450000000, 5U);
            if (backlog == 0) {
                model.refresh(first.state);
            }
            with_gps(heard(first.commit(0, 80301U, 8301U, when + 2, when + 8)), "GPS: B", 226000000, 451000000, 9U);
            model.refresh(first.state);
            expect("2.7: one run's fragments are one call", model.count() == 1);
            expect("2.7: within a run the higher rev wins, whatever the stamps",
                   detail_of(model, 0) == QStringLiteral("GPS: B"));
            // The next run counts its revs from the start again.
            with_gps(heard(second.commit(0, 80301U, 8301U, when + 20, when + 24)), "GPS: C", 227000000, 452000000, 2U);
            model.refresh(second.state);
            expect("2.7: the next run's fragment joins the call", model.count() == 1);
            expect("2.7: a later run's fix beats an earlier run's higher rev",
                   detail_of(model, 0) == QStringLiteral("GPS: C"));
        }
        CallHistoryModel relaunched;
        expect("2.7: the reload shows the same fix",
               relaunched.count() == 1 && detail_of(relaunched, 0) == QStringLiteral("GPS: C"));
        // The running ring read again after the relaunch keeps its run: a fragment with a lower rev loses.
        with_gps(heard(second.commit(0, 80301U, 8301U, when + 26, when + 28)), "GPS: D", 228000000, 453000000, 1U);
        relaunched.refresh(second.state);
        expect("2.7: a relaunch keeps the ring's run",
               relaunched.count() == 1 && detail_of(relaunched, 0) == QStringLiteral("GPS: C"));
        const QJsonObject stored = stored_row_with(80301U, 8301U);
        expect("2.7: the run ordinals are stored with the fix", stored.value(QLatin1String("gpsOrd")).toInt() >= 2);
    }
}

/* B3: two callsign callers (D-STAR) to one destination on one frequency, seconds apart, are two calls, each with its
 * own fix, also when their display names are the same. */
void
test_callsign_callers_stay_apart(void) {
    const time_t when = 1754704000;
    for (int sameAlias = 0; sameAlias < 2; sameAlias++) {
        resetStorage();
        RingFixture ring;
        CallHistoryModel model;
        ring.commit(0, 0U, 0U, when, when + 3, "CQCQCQ");
        ring.commit(0, 0U, 0U, when + 4, when + 7, "CQCQCQ");
        // A commit shifts the ring: the older call is at index 2 now.
        Event_History* one = &ring.rings[0].Event_History_Items[2];
        Event_History* two = &ring.rings[0].Event_History_Items[1];
        const char* callers[2] = {"N0CALL", "N0CALL-1"};
        Event_History* items[2] = {one, two};
        for (int i = 0; i < 2; i++) {
            items[i]->systype = DSD_SYNC_DSTAR_HD_POS;
            items[i]->freq_hz = 145500000;
            DSD_SNPRINTF(items[i]->src_str, sizeof(items[i]->src_str), "%s", callers[i]);
            if (sameAlias != 0) {
                DSD_SNPRINTF(items[i]->s_name, sizeof(items[i]->s_name), "%s", "Club station");
            }
        }
        with_gps(one, "DPRS: caller 0", 225000000, 450000000, 3U)->pos.kind = DSD_POS_DSTAR_DPRS;
        with_gps(two, "DPRS: caller 1", 226000000, 451000000, 4U)->pos.kind = DSD_POS_DSTAR_DPRS;
        model.refresh(ring.state);
        expect(sameAlias != 0 ? "B3: same-alias callsign callers are two calls"
                              : "B3: two callsign callers are two calls",
               model.count() == 2);
        QSet<QString> details;
        for (int i = 0; i < model.count(); i++) {
            details.insert(detail_of(model, i));
        }
        expect("B3: each caller keeps its own fix", details.contains(QStringLiteral("DPRS: caller 0"))
                                                        && details.contains(QStringLiteral("DPRS: caller 1")));
    }
    // The same caller's two fragments still merge.
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    for (int i = 0; i < 2; i++) {
        Event_History* item =
            ring.commit(0, 0U, 0U, when + static_cast<time_t>(5 * i), when + static_cast<time_t>(5 * i) + 3, "CQCQCQ");
        item->systype = DSD_SYNC_DSTAR_HD_POS;
        DSD_SNPRINTF(item->src_str, sizeof(item->src_str), "%s", i == 0 ? "N0CALL" : "N0CALL  ");
    }
    model.refresh(ring.state);
    expect("B3: one caller's fragments (padding aside) are one call", model.count() == 1);
}

/* The text the core writes beside a numeric identity is no raw identity: an EDACS late-entry fragment with an unknown
 * source (`LID_UNK`) and its identified continuation a second later (`LID_<id>`) are one call, as their numbers say,
 * also through a reload. An M17 `UNKNOWN` source (the broadcast address) names nobody either. */
void
test_identity_placeholders_are_no_raw_identity(void) {
    const time_t when = 1754705000;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* first = ring.commit(0, 100U, 0U, when, when + 3, "100_AFS_06_041");
        first->systype = DSD_SYNC_EDACS_POS;
        DSD_SNPRINTF(first->src_str, sizeof(first->src_str), "%s", "LID_UNK");
        Event_History* second = ring.commit(0, 100U, 5001U, when + 4, when + 9, "100_AFS_06_041");
        second->systype = DSD_SYNC_EDACS_POS;
        DSD_SNPRINTF(second->src_str, sizeof(second->src_str), "%s", "LID_5001");
        model.refresh(ring.state);
        expect("EDACS placeholders: the unknown-source fragment joins its continuation", model.count() == 1);
    }
    const QJsonObject stored = stored_row_with(100U, 5001U);
    expect("EDACS placeholders: no raw text is stored", !stored.isEmpty() && !stored.contains(QLatin1String("srcText"))
                                                            && !stored.contains(QLatin1String("tgtText")));
    {
        CallHistoryModel reloaded;
        expect("EDACS placeholders: one call after a reload", reloaded.count() == 1);
    }
    expect("M17 UNKNOWN source is no identity",
           !dsd_qt::call_history_identity_text_is_raw(DSD_SYNC_M17_LSF_POS, true, "UNKNOWN"));
    expect("M17 BROADCAST destination is one",
           dsd_qt::call_history_identity_text_is_raw(DSD_SYNC_M17_LSF_POS, false, "BROADCAST"));
    expect("an EDACS LID is no identity",
           !dsd_qt::call_history_identity_text_is_raw(DSD_SYNC_EDACS_POS, true, "LID_5001"));
    expect("a D-STAR callsign is one",
           dsd_qt::call_history_identity_text_is_raw(DSD_SYNC_DSTAR_HD_POS, true, "N0CALL"));
}

/* B4: one M17 caller's calls to two destinations under one named CAN are two calls, though the service name names
 * both rows; and they stay two through a reload. */
void
test_textual_destinations_under_one_service_stay_apart(void) {
    resetStorage();
    RingFixture ring;
    const time_t when = 1754705000;
    {
        CallHistoryModel model;
        const char* destinations[2] = {"DEST-ONE", "DEST-TWO"};
        for (int i = 0; i < 2; i++) {
            Event_History* item = ring.commit(0, 0U, 0U, when + static_cast<time_t>(4 * i),
                                              when + static_cast<time_t>(4 * i) + 3, destinations[i]);
            item->systype = DSD_SYNC_M17_STR_POS;
            DSD_SNPRINTF(item->src_str, sizeof(item->src_str), "%s", "N0CALL");
            DSD_SNPRINTF(item->service_label, sizeof(item->service_label), "%s", "Club CAN 3");
        }
        model.refresh(ring.state);
        expect("B4: both rows are named by the service",
               model.count() == 2
                   && model.data(model.index(0), CallHistoryModel::NameRole).toString() == QStringLiteral("Club CAN 3")
                   && model.data(model.index(1), CallHistoryModel::NameRole).toString()
                          == QStringLiteral("Club CAN 3"));
    }
    CallHistoryModel reloaded;
    expect("B4: two destinations stay two calls after a reload", reloaded.count() == 2);
}

/* B5: rows whose known systems differ are two calls: two conventional DMR systems on two frequencies, and two
 * conventional P25 channels of one NAC; live, after a reload and through the spill. A trunked call on two voice
 * channels of one system, and a Capacity Plus radio before and after a rest channel move, stay one call; so does a row
 * an older build stored, which knows no system. */
void
test_system_identity_splits_conventional_systems(void) {
    const time_t when = 1754706000;

    struct Case {
        const char* what;
        uint8_t kind;
        uint32_t a[2];
        uint32_t b;
        int64_t freq[2];
        int rows;
    };

    // A Capacity Plus scope (alias_learn.h): a = (trunking system << 8) | colour code, b = site number.
    constexpr uint32_t kCapPlusCc1 = (static_cast<uint32_t>(DSD_RXID_DMR_TRUNK_CAPACITY_PLUS) << 8) | 1U;
    const Case cases[] = {
        {"B5: two conventional DMR systems", DSD_POS_SCOPE_FEED_TUNED_FREQ, {446006250U, 446093750U}, 0U, {0, 0}, 2},
        {"B5: two conventional P25 channels of one NAC",
         DSD_POS_SCOPE_P25_NAC,
         {0x293U, 0x293U},
         0U,
         {155100000, 155400000},
         2},
        {"B5: a trunked call on two voice channels", DSD_POS_SCOPE_P25, {0xBEE00U, 0xBEE00U}, 0x1A3U, {0, 0}, 1},
        {"B5: a Capacity Plus radio across a rest channel move",
         DSD_POS_SCOPE_DMR,
         {kCapPlusCc1, kCapPlusCc1},
         5U,
         {0, 0},
         1},
    };
    for (const Case& c : cases) {
        resetStorage();
        RingFixture ring;
        {
            CallHistoryModel model;
            for (int i = 0; i < 2; i++) {
                Event_History* item = ring.commit(0, 80601U, 8601U, when + static_cast<time_t>(5 * i),
                                                  when + static_cast<time_t>(5 * i) + 3);
                item->systype = c.kind == DSD_POS_SCOPE_P25 || c.kind == DSD_POS_SCOPE_P25_NAC
                                    ? DSD_SYNC_P25P1_POS
                                    : DSD_SYNC_DMR_BS_VOICE_POS;
                item->access_code_kind = DSD_ACCESS_CODE_NONE;
                with_rx(item, c.kind, c.a[i], c.b, c.freq[i]);
                if (c.kind == DSD_POS_SCOPE_P25 || c.kind == DSD_POS_SCOPE_DMR) {
                    item->freq_hz = 851000000 + 12500 * i; // a different voice or rest channel each time
                }
            }
            model.refresh(ring.state);
            expect(c.what, model.count() == c.rows);
        }
        CallHistoryModel reloaded;
        expect(c.what, reloaded.count() == c.rows);
        if (c.rows == 2) {
            const QJsonObject rx = stored_row_with(80601U, 8601U).value(QLatin1String("rx")).toObject();
            expect("B5: the reception system is stored", rx.value(QLatin1String("k")).toInt() == c.kind);
        }
    }
    // Through the spill: the two conventional cases are two calls there too.
    for (const Case& c : cases) {
        if (c.rows != 2) {
            continue;
        }
        resetStorage();
        RingFixture ring;
        {
            CallHistoryModel model;
            for (int i = 0; i < 2; i++) {
                Event_History* item = ring.commit(0, 80602U, 8602U, when + static_cast<time_t>(5 * i),
                                                  when + static_cast<time_t>(5 * i) + 3);
                item->systype = c.kind == DSD_POS_SCOPE_P25_NAC ? DSD_SYNC_P25P1_POS : DSD_SYNC_DMR_BS_VOICE_POS;
                with_rx(item, c.kind, c.a[i], c.b, c.freq[i]);
            }
            model.refresh(ring.state);
            expect(c.what, model.count() == 2);
        }
        stores_to_spill();
        {
            CallHistoryModel spilled;
            expect(c.what, spilled.count() == 2);
        }
    }
    // A row an older build stored knows no system and still merges with a fragment that knows one.
    resetStorage();
    RingFixture old;
    {
        CallHistoryModel model;
        old.commit(0, 80603U, 8603U, when, when + 3);
        model.refresh(old.state);
    }
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    QJsonObject row = rows.at(0).toObject();
    row.remove(QLatin1String("rx"));
    row.remove(QLatin1String("proto"));
    rows.replace(0, row);
    expect("B5: the older row is written", dsd_qt::json_store_save_array("call_history.json", rows));
    CallHistoryModel upgraded;
    with_rx(old.commit(0, 80603U, 8603U, when + 5, when + 8), DSD_POS_SCOPE_FEED_TUNED_FREQ, 446006250U, 0U, 0);
    upgraded.refresh(old.state);
    expect("B5: a row with no recorded system merges as before", upgraded.count() == 1);
}

/* B5 ruling: a weaker identity joins a stronger one of the same family. A trunked P25 call whose first fragment knew
 * only the NAC (each voice channel its own discriminator) joins the fragments that learned the WACN and SYSID, in
 * either order, and the row keeps the full scope; two full scopes that differ stay two calls. */
void
test_weaker_identity_joins_stronger(void) {
    const time_t when = 1754706500;
    for (int fullFirst = 0; fullFirst < 2; fullFirst++) {
        resetStorage();
        RingFixture ring;
        {
            CallHistoryModel model;
            const uint8_t kinds[3] = {
                fullFirst != 0 ? static_cast<uint8_t>(DSD_POS_SCOPE_P25) : static_cast<uint8_t>(DSD_POS_SCOPE_P25_NAC),
                fullFirst != 0 ? static_cast<uint8_t>(DSD_POS_SCOPE_P25_NAC) : static_cast<uint8_t>(DSD_POS_SCOPE_P25),
                DSD_POS_SCOPE_P25_NAC};
            for (int i = 0; i < 3; i++) {
                Event_History* item = ring.commit(0, 80611U, 8611U, when + static_cast<time_t>(5 * i),
                                                  when + static_cast<time_t>(5 * i) + 3);
                const bool full = kinds[i] == DSD_POS_SCOPE_P25;
                with_rx(item, kinds[i], full ? 0xBEE00U : 0x293U, full ? 0x1A3U : 0U, full ? 0 : 851000000 + 12500 * i);
                item->freq_hz = 851000000 + 12500 * i;
                model.refresh(ring.state);
            }
            expect(fullFirst != 0 ? "B5: a full scope then NAC fragments are one call"
                                  : "B5: NAC fragments and the full scope are one call",
                   model.count() == 1);
        } // destructor flushes the stores
        const QJsonObject rx = stored_row_with(80611U, 8611U).value(QLatin1String("rx")).toObject();
        expect("B5: the merged row keeps the full scope", rx.value(QLatin1String("k")).toInt() == DSD_POS_SCOPE_P25
                                                              && rx.value(QLatin1String("b")).toInt() == 0x1A3);
        CallHistoryModel reloaded;
        expect("B5: the merged call stays one after a reload", reloaded.count() == 1);
    }
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    for (int i = 0; i < 2; i++) {
        with_rx(ring.commit(0, 80612U, 8612U, when + static_cast<time_t>(5 * i), when + static_cast<time_t>(5 * i) + 3),
                DSD_POS_SCOPE_P25, 0xBEE00U, i == 0 ? 0x1A3U : 0x1A4U, 0);
    }
    model.refresh(ring.state);
    expect("B5: two full P25 scopes that differ are two calls", model.count() == 2);
}

/* Issue #800, fail closed: two carriers of one system are two receptions, though the system's scope (a P25 WACN and
 * SYSID) names no carrier. A call's fragment heard on 450 MHz and a fragment of the same talkgroup and radio heard on
 * 451 MHz with a fix, both under that system: the calls still merge, but the row takes none of the 451 MHz fix, live
 * and after a reload (the carrier its reception was read with is stored, `rxHz`). On 450 MHz both times it takes it. */
void
test_fix_stays_on_its_carrier(void) {
    const time_t when = 1754706500;
    for (int moved = 0; moved <= 1; moved++) {
        resetStorage();
        {
            RingFixture ring;
            CallHistoryModel model;
            Event_History* first =
                with_rx(ring.commit(0, 80625U, 8625U, when, when + 3), DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
            first->systype = DSD_SYNC_P25P1_POS;
            first->rx_scope.tuned_hz = 450000000;
            model.refresh(ring.state);
            Event_History* second = with_gps(
                with_rx(ring.commit(0, 80625U, 8625U, when + 5, when + 8), DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0),
                "GPS: second fragment", 225000000, 450000000, 4U);
            second->systype = DSD_SYNC_P25P1_POS;
            second->rx_scope.tuned_hz = moved ? 451000000 : 450000000;
            model.refresh(ring.state);
            expect("carrier: the fragments merge", model.count() == 1);
            expect(moved ? "carrier: the other carrier's fix is not the row's" : "carrier: one carrier's fix is",
                   model.count() == 1 && detail_of(model, 0).isEmpty() == (moved != 0));
        } // destructor flushes the stores
        const QJsonObject stored = stored_row_with(80625U, 8625U);
        expect("carrier: the row's carrier is stored",
               stored.value(QLatin1String("rxHz")).toVariant().toLongLong() == 450000000);
        expect("carrier: the stored row's fix", stored.contains(QLatin1String("gps")) == (moved == 0));
        CallHistoryModel reloaded;
        expect("carrier: after a reload", reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty() == (moved != 0));
    }
}

/* Issue #800, fail closed: a fragment gives a merged row its fix, and the row learns a stronger reception scope from it,
 * only when it was heard on the reception the row's other fragments were, read before anything is adopted (the core's
 * reception rule, call_history_same_reception()). A P25 call heard on 450 MHz with only a tuned-frequency scope, then
 * a fragment of the same talkgroup and radio under a NAC scoped to 451 MHz, in one epoch: the calls still merge, but
 * the row keeps its 450 MHz reception and none of the 451 MHz fragment's fix, live and after a reload. A fragment of
 * another decoder run with no known reception is no reception of the row's either: its epoch counts in another ring.
 * The same fragments on one tuned frequency still give the row their fix. */
void
test_fix_stays_on_its_reception(void) {
    const time_t when = 1754706400;
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        Event_History* first =
            with_rx(ring.commit(0, 80621U, 8621U, when, when + 3), DSD_POS_SCOPE_FEED_TUNED_FREQ, 450000000U, 0U, 0);
        first->systype = DSD_SYNC_P25P1_POS;
        model.refresh(ring.state);
        Event_History* second = with_gps(
            with_rx(ring.commit(0, 80621U, 8621U, when + 5, when + 8), DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 451000000),
            "GPS: other carrier", 225000000, 450000000, 4U);
        second->systype = DSD_SYNC_P25P1_POS;
        model.refresh(ring.state);
        expect("reception: the two carriers' fragments still merge", model.count() == 1);
        expect("reception: the other carrier's fix is not the row's",
               model.count() == 1 && detail_of(model, 0).isEmpty());
        // Another run's fragment with no known reception: its epoch says nothing of this ring's.
        RingFixture other;
        Event_History* unknown =
            with_gps(other.commit(0, 80621U, 8621U, when + 10, when + 13), "GPS: other run", 226000000, 451000000, 2U);
        unknown->systype = DSD_SYNC_P25P1_POS;
        model.refresh(other.state);
        expect("reception: another run's fragment of no known reception gives no fix",
               model.count() == 1 && detail_of(model, 0).isEmpty());
    } // destructor flushes the stores
    const QJsonObject rx = stored_row_with(80621U, 8621U).value(QLatin1String("rx")).toObject();
    expect("reception: the row keeps the reception it was heard on",
           rx.value(QLatin1String("k")).toInt() == DSD_POS_SCOPE_FEED_TUNED_FREQ
               && !stored_row_with(80621U, 8621U).contains(QLatin1String("gps")));
    CallHistoryModel reloaded;
    expect("reception: none of it after a reload", reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty());
    resetStorage();
    RingFixture same;
    CallHistoryModel model;
    for (int i = 0; i < 2; i++) {
        Event_History* item = with_rx(
            same.commit(0, 80622U, 8622U, when + 5 * static_cast<time_t>(i), when + 5 * static_cast<time_t>(i) + 3),
            DSD_POS_SCOPE_FEED_TUNED_FREQ, 450000000U, 0U, 0);
        item->systype = DSD_SYNC_P25P1_POS;
        if (i == 1) {
            with_gps(item, "GPS: same carrier", 225000000, 450000000, 4U);
        }
        model.refresh(same.state);
    }
    expect("reception: a fragment of the row's reception gives its fix",
           model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: same carrier"));
}

/* Issue #800, fail closed: the two P25 phases may be one conversation (call_history_protocol_family()), but a fix is
 * one protocol class's (call_history_position_class(), the core's dsd_position_attribution_class()). With the same
 * known system, frequency, talkgroup and radio, a Phase 1 fragment then a Phase 2 fragment carrying a fix are one row,
 * and the Phase 1 row takes none of the Phase 2 fix, in either order, live and after a reload. */
void
test_fix_stays_on_its_p25_phase(void) {
    const time_t when = 1754706450;
    for (int phase2First = 0; phase2First < 2; phase2First++) {
        resetStorage();
        {
            RingFixture ring;
            CallHistoryModel model;
            for (int i = 0; i < 2; i++) {
                const bool phase2 = (i == 0) == (phase2First != 0);
                Event_History* item = with_rx(ring.commit(0, 80631U, 8631U, when + 5 * static_cast<time_t>(i),
                                                          when + 5 * static_cast<time_t>(i) + 3),
                                              DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
                item->freq_hz = 851000000;
                item->systype = phase2 ? DSD_SYNC_P25P2_POS : DSD_SYNC_P25P1_POS;
                if (i == 1) {
                    with_gps(item, "GPS: other phase", 225000000, 450000000, 4U)->pos.kind = DSD_POS_P25_HARRIS;
                }
                model.refresh(ring.state);
            }
            expect("phase: the two phases' fragments are one row", model.count() == 1);
            expect("phase: the other phase's fix is not the row's",
                   model.count() == 1 && detail_of(model, 0).isEmpty());
        } // destructor flushes the stores
        expect("phase: no fix is stored", !stored_row_with(80631U, 8631U).contains(QLatin1String("gps")));
        CallHistoryModel reloaded;
        expect("phase: none after a reload", reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty());
    }
}

/* One fragment of a merged P25 row (@p tg, @p src) heard on Phase 1 or 2 of one known system and frequency, with a
 * Harris fix when @p gps is not null. */
Event_History*
commit_phase_fragment(RingFixture& ring, uint32_t tg, uint32_t src, time_t start, bool phase2, const char* gps) {
    Event_History* item = with_rx(ring.commit(0, tg, src, start, start + 3), DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
    item->freq_hz = 851000000;
    item->systype = phase2 ? DSD_SYNC_P25P2_POS : DSD_SYNC_P25P1_POS;
    if (gps != nullptr) {
        with_gps(item, gps, 225000000, 450000000, 4U)->pos.kind = DSD_POS_P25_HARRIS;
    }
    return item;
}

/* Issue #800, fail closed: a merged row stays anchored to the fragment it was first heard as. A Phase 1 fragment with
 * a fix, then a Phase 2 fragment of the same conversation: a new reading of the first fragment that changes only its
 * end leaves the row a Phase 1 row and keeps its fix (it was judged against the Phase 2 rest alone, which took it).
 * Phase 1, Phase 2, then Phase 1 with a fix: the same update keeps the row on Phase 1 with the third fragment's fix
 * (it was rebuilt Phase 2 first, keeping a Phase 1 fix on a Phase 2 row). And when the first fragment's new reading
 * names another call, the row is rebuilt from the Phase 2 fragment: a retained Phase 1 fix is no longer the row's. */
void
test_merged_row_keeps_its_anchor(void) {
    const time_t when = 1754706470;
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        commit_phase_fragment(ring, 80641U, 8641U, when, false, "GPS: phase one");
        model.refresh(ring.state);
        commit_phase_fragment(ring, 80641U, 8641U, when + 5, true, nullptr);
        model.refresh(ring.state);
        expect("anchor: one row with the Phase 1 fix",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: phase one"));
        ring.rings[0].Event_History_Items[2].event_time += 1; // the first fragment's end moves
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("anchor: an end update keeps the fix",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: phase one"));
    } // destructor flushes the stores
    expect("anchor: the row stays a Phase 1 row",
           stored_row_with(80641U, 8641U).value(QLatin1String("proto")).toInt() == DSD_SYNC_P25P1_POS);
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        commit_phase_fragment(ring, 80642U, 8642U, when, false, nullptr);
        model.refresh(ring.state);
        commit_phase_fragment(ring, 80642U, 8642U, when + 5, true, nullptr);
        model.refresh(ring.state);
        commit_phase_fragment(ring, 80642U, 8642U, when + 10, false, "GPS: phase one again");
        model.refresh(ring.state);
        expect("anchor: the Phase 1 row takes the third fragment's fix",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: phase one again"));
        ring.rings[0].Event_History_Items[3].event_time += 1;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("anchor: after the update the row keeps it",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: phase one again"));
    }
    expect("anchor: still a Phase 1 row",
           stored_row_with(80642U, 8642U).value(QLatin1String("proto")).toInt() == DSD_SYNC_P25P1_POS);
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        commit_phase_fragment(ring, 80643U, 8643U, when, false, nullptr);
        model.refresh(ring.state);
        commit_phase_fragment(ring, 80643U, 8643U, when + 5, true, nullptr);
        model.refresh(ring.state);
        commit_phase_fragment(ring, 80643U, 8643U, when + 10, false, "GPS: phase one kept");
        model.refresh(ring.state);
        // The first fragment read again on another system: another call, withdrawn from the row.
        (void)with_rx(&ring.rings[0].Event_History_Items[3], DSD_POS_SCOPE_P25, 0xBEE01U, 0x1A3U, 0);
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("anchor: a Phase 2 row keeps no Phase 1 fix", detail_of(model, 0).isEmpty());
    }
    const QJsonObject rebuilt = stored_row_with(80643U, 8643U);
    expect("anchor: the rebuilt row is a Phase 2 row with no fix",
           rebuilt.value(QLatin1String("proto")).toInt() == DSD_SYNC_P25P2_POS
               && !rebuilt.contains(QLatin1String("gps")));
    // Gate g62-r9: the first fragment withdrawn (another system), then read again on the row's system with a fix. It
    // is restored at its own place ahead of the Phase 2 fragment, so it is the row's anchor again: the row goes back to
    // Phase 1 and takes the fix heard there.
    resetStorage();
    {
        RingFixture ring;
        CallHistoryModel model;
        commit_phase_fragment(ring, 80644U, 8644U, when, false, nullptr);
        model.refresh(ring.state);
        commit_phase_fragment(ring, 80644U, 8644U, when + 5, true, nullptr);
        model.refresh(ring.state);
        Event_History* first = &ring.rings[0].Event_History_Items[2];
        (void)with_rx(first, DSD_POS_SCOPE_P25, 0xBEE01U, 0x1A3U, 0);
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("anchor: the withdrawn first fragment leaves a Phase 2 row", model.count() == 1);
        (void)with_rx(first, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
        with_gps(first, "GPS: phase one restored", 225000000, 450000000, 4U)->pos.kind = DSD_POS_P25_HARRIS;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("anchor: the restored anchor's fix is the row's",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("GPS: phase one restored"));
    }
    const QJsonObject restored = stored_row_with(80644U, 8644U);
    expect("anchor: the restored row is a Phase 1 row with the fix",
           restored.value(QLatin1String("proto")).toInt() == DSD_SYNC_P25P1_POS
               && restored.contains(QLatin1String("gps")));
}

/* B5: an identity known in part is the same system as one that knows more of it. An EDACS call whose first fragment knew
 * only the site joins the fragment that also learned the system, in either order, and the row keeps the site and the
 * system; a conventional P25 fragment whose tuned frequency was not known joins the one heard with it. Two fragments
 * whose known parts differ (another system on the same site, a site-only and a system-only identity that cannot be told
 * apart) stay two calls. */
void
test_partial_identity_joins_fuller(void) {
    const time_t when = 1754706700;
    constexpr uint32_t kSite = 0x10000U | 5U;
    constexpr uint32_t kSystem = 0x10000U | 7U;

    struct Case {
        const char* what;
        int64_t freq[2];
        int rows;
        uint32_t a[2];
        uint32_t b[2];
        uint8_t kind;
    };

    const Case cases[] = {
        {"B5: an EDACS site, then its system learned",
         {0, 0},
         1,
         {kSite, kSite},
         {0U, kSystem},
         DSD_POS_SCOPE_FEED_EDACS},
        {"B5: an EDACS site and system, then a site-only fragment",
         {0, 0},
         1,
         {kSite, kSite},
         {kSystem, 0U},
         DSD_POS_SCOPE_FEED_EDACS},
        {"B5: two EDACS systems on one site",
         {0, 0},
         2,
         {kSite, kSite},
         {kSystem, 0x10000U | 8U},
         DSD_POS_SCOPE_FEED_EDACS},
        {"B5: an EDACS site-only and a system-only identity",
         {0, 0},
         2,
         {kSite, 0U},
         {0U, kSystem},
         DSD_POS_SCOPE_FEED_EDACS},
        {"B5: a P25 NAC, then its tuned frequency known",
         {0, 155100000},
         1,
         {0x293U, 0x293U},
         {0U, 0U},
         DSD_POS_SCOPE_P25_NAC},
    };
    for (const Case& c : cases) {
        resetStorage();
        RingFixture ring;
        {
            CallHistoryModel model;
            for (int i = 0; i < 2; i++) {
                Event_History* item = ring.commit(0, 80631U, 8631U, when + static_cast<time_t>(5 * i),
                                                  when + static_cast<time_t>(5 * i) + 3);
                item->systype = c.kind == DSD_POS_SCOPE_P25_NAC ? DSD_SYNC_P25P1_POS : DSD_SYNC_EDACS_POS;
                item->access_code_kind = DSD_ACCESS_CODE_NONE;
                with_rx(item, c.kind, c.a[i], c.b[i], c.freq[i]);
                model.refresh(ring.state);
            }
            expect(c.what, model.count() == c.rows);
        }
        CallHistoryModel reloaded;
        expect(c.what, reloaded.count() == c.rows);
        if (c.rows == 1) {
            const QJsonObject rx = stored_row_with(80631U, 8631U).value(QLatin1String("rx")).toObject();
            const bool fuller = c.kind == DSD_POS_SCOPE_FEED_EDACS
                                    ? rx.value(QLatin1String("a")).toInt() == static_cast<int>(kSite)
                                          && rx.value(QLatin1String("b")).toInt() == static_cast<int>(kSystem)
                                    : rx.value(QLatin1String("f")).toInteger() == 155100000;
            expect("B5: the merged row keeps every part either fragment knew", fuller);
        }
    }
}

/* B2/B5: an update found by its fragment is not merged again, so its reception system is checked before it gives the
 * row anything. A P25 call row heard on one full system (its sender scope unknown: no fix yet) merges a NAC-only
 * fragment of the same radio; when that fragment's later reading names another full system and brings a fix local to
 * that system, the row keeps no position from it, live and after a reload. The same update from the row's own system
 * gives it the fix. */
void
test_late_position_rechecks_reception_system(void) {
    const time_t when = 1754706600;
    for (int sameSystem = 0; sameSystem < 2; sameSystem++) {
        resetStorage();
        RingFixture ring;
        const uint32_t sysid = sameSystem != 0 ? 0x1A3U : 0x1A4U;
        {
            CallHistoryModel model;
            Event_History* a = ring.commit(0, 80621U, 8621U, when, when + 3);
            a->systype = DSD_SYNC_P25P1_POS;
            with_rx(a, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
            Event_History* b = ring.commit(0, 80621U, 8621U, when + 5, when + 8);
            b->systype = DSD_SYNC_P25P1_POS;
            with_rx(b, DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 851012500);
            model.refresh(ring.state);
            expect("late position: the NAC-only fragment joins the call", model.count() == 1);
            b = &ring.rings[0].Event_History_Items[1];
            with_rx(b, DSD_POS_SCOPE_P25, 0xBEE00U, sysid, 0);
            with_gps(b, "GPS: B", 225000000, 450000000, 7U);
            b->pos.scope.kind = DSD_POS_SCOPE_P25;
            b->pos.scope.a = 0xBEE00U;
            b->pos.scope.b = sysid;
            ring.touchCommitted(0);
            model.refresh(ring.state);
            expect(sameSystem != 0 ? "late position: the row's own system gives it the fix"
                                   : "late position: another system's fix stays off the row",
                   model.count() == 1 && (detail_of(model, 0) == QStringLiteral("GPS: B")) == (sameSystem != 0));
            model.startSaveForTest();
            model.waitForSaveForTest();
        }
        const QJsonObject stored = stored_row_with(80621U, 8621U);
        expect(sameSystem != 0 ? "late position: the own system's fix is saved"
                               : "late position: another system's fix is not saved",
               stored.contains(QLatin1String("pos")) == (sameSystem != 0)
                   && stored.value(QLatin1String("rx")).toObject().value(QLatin1String("b")).toInt() == 0x1A3);
        CallHistoryModel reloaded;
        expect("late position: the same after a reload",
               reloaded.count() == 1 && (detail_of(reloaded, 0) == QStringLiteral("GPS: B")) == (sameSystem != 0));
    }
}

/* B2/B5: a fragment that gave a row its fix, message and source label while its identity agreed with the row's takes
 * them back when its later reading names another reception system. A P25 call row heard on one full system merges a
 * NAC-only fragment of the same radio that brings a fix, a message and a label; when that fragment's later reading
 * names another full system, the row shows none of them (or the best remaining fragment's fix, when its own fragment
 * has one), live, saved and after a reload. */
void
test_fragment_on_another_system_withdraws_payload(void) {
    const time_t when = 1754706650;
    for (int ownFix = 0; ownFix < 2; ownFix++) {
        resetStorage();
        RingFixture ring;
        const QString shownAfter = ownFix != 0 ? QStringLiteral("GPS: A") : QString();
        {
            CallHistoryModel model;
            Event_History* a = ring.commit(0, 80651U, 8651U, when, when + 3);
            a->systype = DSD_SYNC_P25P1_POS;
            with_rx(a, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
            if (ownFix != 0) {
                with_gps(a, "GPS: A", 100000000, 200000000, 3U);
                a->pos.scope.kind = DSD_POS_SCOPE_P25;
                a->pos.scope.a = 0xBEE00U;
                a->pos.scope.b = 0x1A3U;
            }
            Event_History* b = ring.commit(0, 80651U, 8651U, when + 5, when + 8);
            b->systype = DSD_SYNC_P25P1_POS;
            with_rx(b, DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000);
            with_gps(b, "GPS: B", 225000000, 450000000, 7U);
            b->pos.scope.kind = DSD_POS_SCOPE_P25_NAC;
            b->pos.scope.a = 0x293U;
            b->pos.identity_freq_hz = 452500000;
            DSD_SNPRINTF(b->text_message, sizeof(b->text_message), "%s", "Copy B");
            DSD_SNPRINTF(b->s_name, sizeof(b->s_name), "%s", "Alias B");
            model.refresh(ring.state);
            expect("withdraw: the NAC-only fragment joins the call and gives it its fix, message and label",
                   model.count() == 1 && detail_of(model, 0) == QStringLiteral("Copy B · GPS: B")
                       && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                              == QStringLiteral("Alias B"));
            b = &ring.rings[0].Event_History_Items[1];
            with_rx(b, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A4U, 0);
            b->pos.scope.kind = DSD_POS_SCOPE_P25;
            b->pos.scope.a = 0xBEE00U;
            b->pos.scope.b = 0x1A4U;
            b->pos.identity_freq_hz = 0;
            ring.touchCommitted(0);
            model.refresh(ring.state);
            expect(ownFix != 0 ? "withdraw: another system's fragment gives way to the row's own fix"
                               : "withdraw: another system's fragment leaves the row no fix and no message",
                   model.count() == 1 && detail_of(model, 0) == shownAfter);
            expect("withdraw: another system's fragment takes its label back",
                   model.count() == 1
                       && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                              != QStringLiteral("Alias B"));
            model.startSaveForTest();
            model.waitForSaveForTest();
        }
        const QJsonObject stored = stored_row_with(80651U, 8651U);
        const QJsonObject pos = stored.value(QLatin1String("pos")).toObject();
        const bool posSaved = stored.contains(QLatin1String("pos"));
        expect("withdraw: the withdrawn fix, message and label are not saved",
               !stored.contains(QLatin1String("msg")) && !stored.contains(QLatin1String("gpsAlt"))
                   && stored.value(QLatin1String("srcName")).toString() != QStringLiteral("Alias B")
                   && posSaved == (ownFix != 0) && (!posSaved || pos.value(QLatin1String("lat")).toInt() == 100000000));
        CallHistoryModel reloaded;
        expect("withdraw: the same after a reload",
               reloaded.count() == 1 && detail_of(reloaded, 0) == shownAfter
                   && reloaded.data(reloaded.index(0), CallHistoryModel::SourceNameRole).toString()
                          != QStringLiteral("Alias B"));
    }
}

/** The fields of row @p row that a fragment's part gives it (FragmentPart), as one line to compare. */
QString
part_fields_of(const CallHistoryModel& model, int row) {
    const QModelIndex idx = model.index(row);
    return QStringLiteral("freq=%1 code=%2 site=%3 svc=%4 name=%5 em=%6 enc=%7 dur=%8 when=%9")
        .arg(model.data(idx, CallHistoryModel::FreqHzRole).toLongLong())
        .arg(model.data(idx, CallHistoryModel::AccessCodeRole).toString())
        .arg(model.data(idx, CallHistoryModel::SiteTextRole).toString())
        .arg(model.data(idx, CallHistoryModel::ServiceNameRole).toString())
        .arg(model.data(idx, CallHistoryModel::NameRole).toString())
        .arg(model.data(idx, CallHistoryModel::EmergencyRole).toBool() ? 1 : 0)
        .arg(model.data(idx, CallHistoryModel::EncRole).toBool() ? 1 : 0)
        .arg(model.data(idx, CallHistoryModel::DurationSecsRole).toInt())
        .arg(model.data(idx, CallHistoryModel::WhenRole).toLongLong());
}

/* The ruling on B2/B5: a fragment that turns out to be another system's takes back every field it gave its row, not
 * only its texts: its frequency, access code, site, receive evidence and service (with the name it gave), emergency and
 * crypto flags, the span it added and the reception scope it strengthened. The row rebuilds them from its other
 * fragments by the merge's own rules, live and when the withdrawal comes after a reload (the parts are stored). */
void
test_fragment_on_another_system_withdraws_call_fields(void) {
    const time_t when = 1754706800;
    for (int reloadFirst = 0; reloadFirst < 2; reloadFirst++) {
        resetStorage();
        RingFixture ring;
        QString alone;
        auto model = std::make_unique<CallHistoryModel>();
        Event_History* a = ring.commit(0, 80671U, 8671U, when, when + 3);
        a->systype = DSD_SYNC_P25P1_POS;
        with_rx(a, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
        RingFixture::carrier(a, 452000000, DSD_ACCESS_CODE_NONE, 0U);
        model->refresh(ring.state);
        alone = part_fields_of(*model, 0);
        {
            Event_History* joining = ring.commit(0, 80671U, 8671U, when + 5, when + 12);
            joining->systype = DSD_SYNC_P25P1_POS;
            with_rx(joining, DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 453000000);
            RingFixture::carrier(joining, 453000000, DSD_ACCESS_CODE_NAC, 0x293U);
            joining->site_id.family = DSD_RXID_FAMILY_P25;
            joining->site_id.valid = DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE;
            joining->site_id.p25_sysid = 0x1A3U;
            joining->site_id.p25_rfss = 1U;
            joining->site_id.p25_site = 2U;
            DSD_SNPRINTF(joining->service_label, sizeof(joining->service_label), "%s", "Service B");
            joining->receive_identity.known = 1;
            joining->receive_identity.kind = DSD_RECEIVE_NAC;
            joining->receive_identity.value = 0x293U;
            joining->emergency = 1U;
            joining->enc = 1U;
        }
        model->refresh(ring.state);
        expect("fields: the NAC-only fragment joins the call and gives it its fields",
               model->count() == 1 && part_fields_of(*model, 0) != alone
                   && model->data(model->index(0), CallHistoryModel::FreqHzRole).toLongLong() == 453000000);
        if (reloadFirst != 0) {
            // Saved and loaded again before the core resolves the fragment: the row's parts come from the store.
            model->startSaveForTest();
            model->waitForSaveForTest();
            model.reset();
            model = std::make_unique<CallHistoryModel>();
            expect("fields: the merged row reloads as it was",
                   model->count() == 1 && part_fields_of(*model, 0) != alone);
        }
        Event_History* b = &ring.rings[0].Event_History_Items[1];
        with_rx(b, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A4U, 0);
        b->freq_hz = 453000000;
        ring.touchCommitted(0);
        model->refresh(ring.state);
        const QString after = model->count() == 1 ? part_fields_of(*model, 0) : QStringLiteral("<rows>");
        expect(reloadFirst != 0 ? "fields: after a reload, another system's fragment gives back every field it gave"
                                : "fields: another system's fragment gives back every field it gave",
               after == alone);
        if (after != alone) {
            DSD_FPRINTF(stderr, "  want %s\n  got  %s\n", alone.toUtf8().constData(), after.toUtf8().constData());
        }
        model->startSaveForTest();
        model->waitForSaveForTest();
        model.reset();
        const QJsonObject stored = stored_row_with(80671U, 8671U);
        expect("fields: the withdrawn fragment's receive evidence and service are not saved",
               !stored.isEmpty() && !stored.contains(QLatin1String("receiveKnown"))
                   && !stored.contains(QLatin1String("service")));
        expect("fields: the end the withdrawn fragment gave is not saved",
               stored.value(QLatin1String("end")).toInteger() == static_cast<qint64>(when) + 3);
        CallHistoryModel reloaded;
        expect("fields: the rebuilt row is the one saved",
               reloaded.count() == 1 && part_fields_of(reloaded, 0) == alone);
    }

    // The reception scope the withdrawn fragment strengthened goes back to what the rest knew. An update is judged
    // against what the row's other fragments gave: here the other fragment's sender system, which the update's
    // contradicts (its NAC-only reception alone would agree with either system).
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    Event_History* a = ring.commit(0, 80672U, 8672U, when, when + 3);
    a->systype = DSD_SYNC_P25P1_POS;
    with_rx(a, DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 453000000);
    a->pos.scope.kind = DSD_POS_SCOPE_P25;
    a->pos.scope.a = 0xBEE00U;
    a->pos.scope.b = 0x1A3U;
    Event_History* b = ring.commit(0, 80672U, 8672U, when - 6, when + 1);
    b->systype = DSD_SYNC_P25P1_POS;
    with_rx(b, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
    model.refresh(ring.state);
    expect("fields: an earlier fragment starts the row",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::WhenRole).toLongLong() == static_cast<qint64>(when - 6));
    b = &ring.rings[0].Event_History_Items[1];
    with_rx(b, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A4U, 0);
    b->pos.scope.kind = DSD_POS_SCOPE_P25;
    b->pos.scope.a = 0xBEE00U;
    b->pos.scope.b = 0x1A4U;
    ring.touchCommitted(0);
    model.refresh(ring.state);
    model.startSaveForTest();
    model.waitForSaveForTest();
    const QJsonObject rx = stored_row_with(80672U, 8672U).value(QLatin1String("rx")).toObject();
    expect("fields: the reception scope the withdrawn fragment strengthened is the rest's again",
           model.count() == 1 && rx.value(QLatin1String("k")).toInt() == DSD_POS_SCOPE_P25_NAC
               && rx.value(QLatin1String("a")).toInt() == 0x293);
    expect("fields: the start the withdrawn fragment gave is the rest's again",
           model.count() == 1
               && model.data(model.index(0), CallHistoryModel::WhenRole).toLongLong() == static_cast<qint64>(when)
               && model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 3);

    // A row an older build stored with several fragments keeps no parts to rebuild from: the frequency and access code
    // the withdrawn fragment gave are cleared rather than kept.
    resetStorage();
    RingFixture legacy;
    {
        CallHistoryModel merged;
        Event_History* first = legacy.commit(0, 80673U, 8673U, when, when + 3);
        first->systype = DSD_SYNC_P25P1_POS;
        with_rx(first, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
        RingFixture::carrier(first, 452000000, DSD_ACCESS_CODE_NONE, 0U);
        Event_History* second = legacy.commit(0, 80673U, 8673U, when + 5, when + 8);
        second->systype = DSD_SYNC_P25P1_POS;
        with_rx(second, DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 453000000);
        RingFixture::carrier(second, 453000000, DSD_ACCESS_CODE_NAC, 0x293U);
        merged.refresh(legacy.state);
        expect("fields: the older row merges", merged.count() == 1);
    }
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    QJsonObject row = rows.at(0).toObject();
    expect("fields: a merged row stores its parts", row.contains(QLatin1String("fp")));
    row.remove(QLatin1String("fp"));
    rows.replace(0, row);
    expect("fields: the older row is written", dsd_qt::json_store_save_array("call_history.json", rows));
    {
        CallHistoryModel older;
        Event_History* second = &legacy.rings[0].Event_History_Items[1];
        with_rx(second, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A4U, 0);
        second->freq_hz = 453000000;
        legacy.touchCommitted(0);
        older.refresh(legacy.state);
        expect("fields: an older row clears the frequency and code the withdrawn fragment gave",
               older.count() == 1 && older.data(older.index(0), CallHistoryModel::FreqHzRole).toLongLong() == 0
                   && older.data(older.index(0), CallHistoryModel::AccessCodeRole).toString().isEmpty());
    }
}

/* B2/B5: a fragment withdrawn from its row (its reading names another system) is another call's, so its recording is
 * not the row's: the row's recording roles, recordingsForUid(), the export's recording names and uidForFragment() leave
 * it out, live and after a reload, while the row keeps the fragment's key to know its later readings. A reading that
 * agrees again gives the recording back. */
void
test_withdrawn_fragment_recordings(void) {
    const time_t when = 1754706900;
    resetStorage();
    RingFixture ring;
    FakeRecordings recordings;
    QString joined;
    qint64 uid = 0;
    {
        CallHistoryModel model;
        model.setRecordingProvider(&recordings);
        Event_History* a = ring.commit(0, 80681U, 8681U, when, when + 3);
        a->systype = DSD_SYNC_P25P1_POS;
        with_rx(a, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
        Event_History* b = ring.commit(0, 80681U, 8681U, when + 5, when + 8);
        b->systype = DSD_SYNC_P25P1_POS;
        with_rx(b, DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000);
        joined = newest_fragment(ring, 0);
        recordings.recorded = {joined};
        model.refresh(ring.state);
        uid = uid_at(model, 0);
        expect("withdrawn recording: the NAC-only fragment joins the call with its recording",
               model.count() == 1 && has_recording(model, 0) && model.recordingsForUid(uid).size() == 1
                   && qRound64(model.uidForFragment(joined)) == uid);
        b = &ring.rings[0].Event_History_Items[1];
        with_rx(b, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A4U, 0);
        ring.touchCommitted(0);
        model.refresh(ring.state);
        const QList<CallHistoryModel::ExportRow> exported = model.exportRows({0});
        const QList<CallHistoryModel::PlaylistRow> playlist = model.playlistRows({0});
        expect("withdrawn recording: a playlist takes no recording of it",
               playlist.size() == 1 && playlist.at(0).uid == uid && playlist.at(0).recordings.isEmpty()
                   && recordings.lastBatch.size() == 1 && !recordings.lastBatch.at(0).contains(joined));
        expect("withdrawn recording: the row no longer offers the other system's recording",
               model.count() == 1 && !has_recording(model, 0)
                   && model.data(model.index(0), CallHistoryModel::RecordingsRole).toList().isEmpty()
                   && model.recordingsForUid(uid).isEmpty());
        expect("withdrawn recording: the export names no recording of it",
               exported.size() == 1 && exported.at(0).recordingNames.isEmpty());
        expect("withdrawn recording: its recording names no row", qRound64(model.uidForFragment(joined)) == 0);
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    {
        CallHistoryModel reloaded;
        reloaded.setRecordingProvider(&recordings);
        expect("withdrawn recording: the same after a reload", reloaded.count() == 1 && !has_recording(reloaded, 0)
                                                                   && reloaded.recordingsForUid(uid).isEmpty()
                                                                   && qRound64(reloaded.uidForFragment(joined)) == 0);
        Event_History* b = &ring.rings[0].Event_History_Items[1];
        with_rx(b, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
        ring.touchCommitted(0);
        reloaded.refresh(ring.state);
        expect("withdrawn recording: a reading that agrees again gives the recording back",
               reloaded.count() == 1 && has_recording(reloaded, 0) && reloaded.recordingsForUid(uid).size() == 1
                   && qRound64(reloaded.uidForFragment(joined)) == uid);
    }
}

/* Issue #909: a playlist's rows say what the row shows (its uid, its name as data() gives it, its start) and carry
 * every row's recordings from one provider read, in the order asked, rows out of range skipped; no provider, or one
 * that answers for fewer rows than asked, lists none rather than another row's. */
void
test_playlist_rows(void) {
    const time_t when = 1754707200;
    resetStorage();
    RingFixture ring;
    FakeRecordings recordings;
    CallHistoryModel model;
    ring.commit(0, 90901U, 9091U, when, when + 3);
    const QString older = newest_fragment(ring, 0);
    ring.commit(0, 90902U, 9092U, when + 60, when + 64);
    const QString newer = newest_fragment(ring, 0);
    model.refresh(ring.state);
    expect("playlist rows: two calls", model.count() == 2);
    const QList<CallHistoryModel::PlaylistRow> bare = model.playlistRows({0, 1});
    expect("playlist rows: without a provider, rows without recordings",
           bare.size() == 2 && bare.at(0).recordings.isEmpty() && bare.at(1).recordings.isEmpty());

    model.setRecordingProvider(&recordings);
    recordings.recorded = {older, newer};
    const QList<CallHistoryModel::PlaylistRow> rows = model.playlistRows({1, -1, 0, 2});
    expect("playlist rows: one read for every row kept, in the order asked",
           recordings.batchReads == 1 && recordings.lastBatch == (QList<QStringList>{{older}, {newer}}));
    expect("playlist rows: the rows kept", rows.size() == 2);
    if (rows.size() != 2) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        const QModelIndex idx = model.index(1 - i);
        expect("playlist rows: what the row shows",
               rows.at(i).uid == uid_at(model, 1 - i)
                   && rows.at(i).name == model.data(idx, CallHistoryModel::NameRole).toString()
                   && rows.at(i).when == model.data(idx, CallHistoryModel::WhenRole).toLongLong()
                   && rows.at(i).when > 0);
    }
    expect("playlist rows: each row its own recordings",
           rows.at(0).recordings.size() == 1 && rows.at(1).recordings.size() == 1
               && rows.at(0).recordings.at(0).toMap().value(QStringLiteral("fragment")).toString() == older
               && rows.at(1).recordings.at(0).toMap().value(QStringLiteral("fragment")).toString() == newer);

    recordings.shortAnswer = true;
    const QList<CallHistoryModel::PlaylistRow> shortRows = model.playlistRows({0, 1});
    expect("playlist rows: a short answer lists none",
           shortRows.size() == 2 && shortRows.at(0).recordings.isEmpty() && shortRows.at(1).recordings.isEmpty());
    expect("playlist rows: no rows, one empty read", model.playlistRows({}).isEmpty());
    model.setRecordingProvider(nullptr);
}

/** Everything row @p row shows that a fragment can give it: its sender, label and detail (message and GPS) beside the
 * call fields (part_fields_of()), as one line to compare. */
QString
contribution_fields_of(const CallHistoryModel& model, int row) {
    const QModelIndex idx = model.index(row);
    return QStringLiteral("session=%1 src=%2 label=%3 detail=%4 %5")
        .arg(model.data(idx, CallHistoryModel::SessionRole).toLongLong())
        .arg(model.data(idx, CallHistoryModel::SrcRole).toULongLong())
        .arg(model.data(idx, CallHistoryModel::SourceNameRole).toString())
        .arg(model.data(idx, CallHistoryModel::DetailRole).toString())
        .arg(part_fields_of(model, row));
}

/* B2/B5, one model for every field: a fragment's contribution is its part (FragmentPart), keyed by its ring identity,
 * and the row is the fold of its parts. A P25 row of unknown source heard on one full system merges a NAC-only
 * fragment, heard in the next decode session, that names the radio and gives the row its label, message, fix,
 * frequency, access code, site, service, flags, span and session. When that fragment's later reading names another system, the row shows exactly what its own
 * fragment alone gave it, the radio included, live and when the withdrawal comes after a reload; nothing of it is
 * saved; and a later fragment naming the row's real radio joins the row and names it. */
void
test_fragment_contributions_withdraw_together(void) {
    const time_t when = 1754707000;
    for (int reloadFirst = 0; reloadFirst < 2; reloadFirst++) {
        resetStorage();
        RingFixture ring;
        auto model = std::make_unique<CallHistoryModel>();
        Event_History* a = ring.commit(0, 80691U, 0U, when, when + 3);
        a->systype = DSD_SYNC_P25P1_POS;
        with_rx(a, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
        RingFixture::carrier(a, 452000000, DSD_ACCESS_CODE_NONE, 0U);
        model->refresh(ring.state);
        const QString alone = contribution_fields_of(*model, 0);
        // Heard in the next decode session: the row becomes that session's too, while the fragment is its own.
        model->beginSession();
        Event_History* f = ring.commit(0, 80691U, 8691U, when + 5, when + 12);
        f->systype = DSD_SYNC_P25P1_POS;
        with_rx(f, DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 453000000);
        RingFixture::carrier(f, 453000000, DSD_ACCESS_CODE_NAC, 0x293U);
        with_gps(f, "GPS: F", 225000000, 450000000, 9U);
        f->pos.scope.kind = DSD_POS_SCOPE_P25_NAC;
        f->pos.scope.a = 0x293U;
        f->pos.identity_freq_hz = 453000000;
        DSD_SNPRINTF(f->text_message, sizeof(f->text_message), "%s", "Copy F");
        DSD_SNPRINTF(f->s_name, sizeof(f->s_name), "%s", "Alias F");
        DSD_SNPRINTF(f->service_label, sizeof(f->service_label), "%s", "Service F");
        f->receive_identity.known = 1;
        f->receive_identity.kind = DSD_RECEIVE_NAC;
        f->receive_identity.value = 0x293U;
        f->site_id.family = DSD_RXID_FAMILY_P25;
        f->site_id.valid = DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE;
        f->site_id.p25_sysid = 0x1A3U;
        f->site_id.p25_rfss = 1U;
        f->site_id.p25_site = 2U;
        f->emergency = 1U;
        f->enc = 1U;
        model->refresh(ring.state);
        expect("contributions: the NAC-only fragment joins the call and names its radio",
               model->count() == 1 && model->data(model->index(0), CallHistoryModel::SrcRole).toULongLong() == 8691U
                   && model->data(model->index(0), CallHistoryModel::SessionRole).toLongLong() == model->session()
                   && model->data(model->index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Alias F")
                   && detail_of(*model, 0) == QStringLiteral("Copy F · GPS: F"));
        if (reloadFirst != 0) {
            model->startSaveForTest();
            model->waitForSaveForTest();
            model.reset();
            model = std::make_unique<CallHistoryModel>();
            expect("contributions: the merged row reloads with its radio",
                   model->count() == 1
                       && model->data(model->index(0), CallHistoryModel::SrcRole).toULongLong() == 8691U);
        }
        f = &ring.rings[0].Event_History_Items[1];
        with_rx(f, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A4U, 0);
        f->freq_hz = 453000000;
        f->pos.scope.kind = DSD_POS_SCOPE_P25;
        f->pos.scope.a = 0xBEE00U;
        f->pos.scope.b = 0x1A4U;
        f->pos.identity_freq_hz = 0;
        ring.touchCommitted(0);
        model->refresh(ring.state);
        const QString after = model->count() == 1 ? contribution_fields_of(*model, 0) : QStringLiteral("<rows>");
        expect(reloadFirst != 0 ? "contributions: after a reload, the withdrawn fragment gives back every field"
                                : "contributions: the withdrawn fragment gives back every field, its radio included",
               after == alone);
        if (after != alone) {
            DSD_FPRINTF(stderr, "  want %s\n  got  %s\n", alone.toUtf8().constData(), after.toUtf8().constData());
        }
        model->startSaveForTest();
        model->waitForSaveForTest();
        model.reset();
        const QJsonObject stored = stored_row_with(80691U, 0U);
        expect("contributions: nothing the withdrawn fragment gave is saved",
               !stored.isEmpty() && stored.value(QLatin1String("srcName")).toString().isEmpty()
                   && !stored.contains(QLatin1String("msg")) && !stored.contains(QLatin1String("gps"))
                   && !stored.contains(QLatin1String("pos")) && !stored.contains(QLatin1String("sscope"))
                   && !stored.contains(QLatin1String("service")));
        model = std::make_unique<CallHistoryModel>();
        expect("contributions: the rebuilt row is the one reloaded",
               model->count() == 1 && contribution_fields_of(*model, 0) == alone);
        Event_History* c = ring.commit(0, 80691U, 8692U, when + 4, when + 8);
        c->systype = DSD_SYNC_P25P1_POS;
        with_rx(c, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
        model->refresh(ring.state);
        expect("contributions: a fragment naming the row's real radio joins the row and names it",
               model->count() == 1 && model->data(model->index(0), CallHistoryModel::SrcRole).toULongLong() == 8692U);
    }
}

/* B2: what a fragment gave is its own by its ring identity, not by its start, push stamp and slot alone, which two
 * decoder runs replaying one call share. A call replayed in two rings at once: the first ring's fragment gives the row
 * a message, a label and a fix; the second ring's fragment, with the same stamps but none of them, later names another
 * system. Its withdrawal leaves the first ring's message, label and fix on the row, live and after a reload. */
void
test_fragment_withdrawal_keeps_other_ring_payload(void) {
    const time_t when = 1754707100;
    resetStorage();
    RingFixture first;
    RingFixture second;
    {
        CallHistoryModel model;
        Event_History* a = first.commit(0, 80693U, 8693U, when, when + 4);
        a->systype = DSD_SYNC_P25P1_POS;
        with_rx(a, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0);
        DSD_SNPRINTF(a->text_message, sizeof(a->text_message), "%s", "Copy A");
        DSD_SNPRINTF(a->s_name, sizeof(a->s_name), "%s", "Alias A");
        with_gps(a, "GPS: A", 100000000, 200000000, 3U);
        model.refresh(first.state);
        Event_History* b = second.commit(0, 80693U, 8693U, when, when + 4);
        b->systype = DSD_SYNC_P25P1_POS;
        with_rx(b, DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000);
        model.refresh(second.state);
        expect("other ring: the two rings' fragments are one call with the first ring's message and label",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("Copy A · GPS: A")
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Alias A"));
        b = &second.rings[0].Event_History_Items[1];
        with_rx(b, DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A4U, 0);
        second.touchCommitted(0);
        model.refresh(second.state);
        expect("other ring: the second ring's withdrawal leaves the first ring's message and label",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("Copy A · GPS: A")
                   && model.data(model.index(0), CallHistoryModel::SourceNameRole).toString()
                          == QStringLiteral("Alias A"));
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    CallHistoryModel reloaded;
    expect("other ring: the same after a reload",
           reloaded.count() == 1 && detail_of(reloaded, 0) == QStringLiteral("Copy A · GPS: A")
               && reloaded.data(reloaded.index(0), CallHistoryModel::SourceNameRole).toString()
                      == QStringLiteral("Alias A"));
}

/* B4: an update found by its fragment is not merged again, so its destination is checked again too. An M17 callsign
 * fragment with no destination yet joins one caller's call to DEST-ONE under the service both name; when its later
 * reading names DEST-TWO and carries a message, that message is not shown on DEST-ONE's row, live and after a
 * reload, and its earlier contribution goes. An update is judged against what the row's other fragments gave: a
 * fragment that gave the row its only destination and later names another simply names that one. */
void
test_fragment_update_rechecks_destination(void) {
    const time_t when = 1754707200;
    resetStorage();
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* a = ring.commit(0, 0U, 0U, when, when + 3, "DEST-ONE");
        a->systype = DSD_SYNC_M17_STR_POS;
        DSD_SNPRINTF(a->src_str, sizeof(a->src_str), "%s", "N0CALL");
        DSD_SNPRINTF(a->service_label, sizeof(a->service_label), "%s", "Club CAN 3");
        Event_History* f = ring.commit(0, 0U, 0U, when + 4, when + 7, "");
        f->systype = DSD_SYNC_M17_STR_POS;
        DSD_SNPRINTF(f->src_str, sizeof(f->src_str), "%s", "N0CALL");
        DSD_SNPRINTF(f->service_label, sizeof(f->service_label), "%s", "Club CAN 3");
        DSD_SNPRINTF(f->text_message, sizeof(f->text_message), "%s", "Hello");
        model.refresh(ring.state);
        expect("destination: the fragment with no destination joins the call and gives it its message",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("Hello"));
        f = &ring.rings[0].Event_History_Items[1];
        DSD_SNPRINTF(f->tgt_str, sizeof(f->tgt_str), "%s", "DEST-TWO");
        DSD_SNPRINTF(f->text_message, sizeof(f->text_message), "%s", "For two");
        f->event_time = when + 9;
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("destination: another destination's message and span stay off the row",
               model.count() == 1 && detail_of(model, 0).isEmpty()
                   && model.data(model.index(0), CallHistoryModel::DurationSecsRole).toInt() == 3);
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    {
        CallHistoryModel reloaded;
        expect("destination: the same after a reload", reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty());
    }

    resetStorage();
    RingFixture own;
    CallHistoryModel model;
    Event_History* a = own.commit(0, 0U, 0U, when, when + 3, "");
    a->systype = DSD_SYNC_M17_STR_POS;
    DSD_SNPRINTF(a->src_str, sizeof(a->src_str), "%s", "N0CALL");
    Event_History* f = own.commit(0, 0U, 0U, when + 4, when + 7, "DEST-ONE");
    f->systype = DSD_SYNC_M17_STR_POS;
    DSD_SNPRINTF(f->src_str, sizeof(f->src_str), "%s", "N0CALL");
    model.refresh(own.state);
    expect("destination: a fragment gives the row its only destination", model.count() == 1);
    f = &own.rings[0].Event_History_Items[1];
    DSD_SNPRINTF(f->tgt_str, sizeof(f->tgt_str), "%s", "DEST-TWO");
    DSD_SNPRINTF(f->text_message, sizeof(f->text_message), "%s", "For two");
    own.touchCommitted(0);
    model.refresh(own.state);
    model.startSaveForTest();
    model.waitForSaveForTest();
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    expect("destination: its later reading names its new destination on the row",
           model.count() == 1 && detail_of(model, 0) == QStringLiteral("For two") && rows.size() == 1
               && rows.at(0).toObject().value(QLatin1String("tgtText")).toString() == QStringLiteral("DEST-TWO"));
}

/* A stored value reads into a 64-bit field only when it is a whole number: a missing key, a string, a fraction or a
 * number past 64 bits is refused, and no stand-in value counts as a stored one. */
void
test_json_field_fits_judges_conversion(void) {
    const QJsonObject obj{{QStringLiteral("text"), QStringLiteral("12")},
                          {QStringLiteral("half"), 0.5},
                          {QStringLiteral("huge"), 1e300},
                          {QStringLiteral("neg"), static_cast<qint64>(-5)},
                          {QStringLiteral("big"), static_cast<qint64>(1) << 40}};
    qint64 out = 7;
    expect("json_field_fits: a missing key is refused", !dsd_qt::json_field_fits(obj, "none", &out) && out == 7);
    expect("json_field_fits: a string is refused", !dsd_qt::json_field_fits(obj, "text", &out) && out == 7);
    expect("json_field_fits: a fraction is refused", !dsd_qt::json_field_fits(obj, "half", &out) && out == 7);
    expect("json_field_fits: a number past 64 bits is refused",
           !dsd_qt::json_field_fits(obj, "huge", &out) && out == 7);
    expect("json_field_fits: a negative whole number fits", dsd_qt::json_field_fits(obj, "neg", &out) && out == -5);
    expect("json_field_fits: a 40-bit whole number fits",
           dsd_qt::json_field_fits(obj, "big", &out) && out == (static_cast<qint64>(1) << 40));
    uint32_t narrow = 3U;
    expect("json_field_fits: a value past the field is refused", !dsd_qt::json_field_fits(obj, "big", &narrow)
                                                                     && !dsd_qt::json_field_fits(obj, "neg", &narrow)
                                                                     && narrow == 3U);
}

/* A stored system identity (`rx`, `sscope`) with a part no record could hold reads as not known, so the stored row still
 * takes in a fragment of its call: a discriminator that is missing, no number, fractional, negative or past a 32-bit
 * frequency; a discriminator on a scope that has none; a b part where the kind has none; a frequency scope without its
 * frequency; an EDACS part not in its stored form. So does a stored sync type that is no integer. Each corruption is
 * applied to a row the model wrote, whose valid identity the next fragment repeats. */
void
test_stored_identity_parts_are_validated(void) {
    const time_t when = 1754706750;

    struct Case {
        const char* what;
        uint8_t kind;
        uint32_t a;
        uint32_t b;
        int64_t freq;
        const char* key;
        QJsonValue value;
    };

    const QJsonValue removed(QJsonValue::Undefined);
    const Case cases[] = {
        {"stored scope: a missing discriminator", DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000, "f", removed},
        {"stored scope: a discriminator that is no number", DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000, "f",
         QJsonValue(QStringLiteral("452500000"))},
        {"stored scope: a fractional discriminator", DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000, "f",
         QJsonValue(452500000.5)},
        {"stored scope: a negative discriminator", DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000, "f",
         QJsonValue(static_cast<qint64>(-452500000))},
        {"stored scope: a discriminator past 32 bits", DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000, "f",
         QJsonValue(static_cast<qint64>(0x100000000LL + 452500000LL))},
        {"stored scope: a NAC scope with a b part", DSD_POS_SCOPE_P25_NAC, 0x293U, 0U, 452500000, "b", QJsonValue(7)},
        {"stored scope: a discriminator on a full system", DSD_POS_SCOPE_P25, 0xBEE00U, 0x1A3U, 0, "f",
         QJsonValue(static_cast<qint64>(452500000))},
        {"stored scope: a tuned-frequency scope without its frequency", DSD_POS_SCOPE_FEED_TUNED_FREQ, 446006250U, 0U,
         0, "a", QJsonValue(0)},
        {"stored scope: an EDACS part not in its stored form", DSD_POS_SCOPE_FEED_EDACS, 0x10005U, 0U, 0, "a",
         QJsonValue(5)},
    };
    for (const Case& c : cases) {
        resetStorage();
        RingFixture ring;
        {
            CallHistoryModel model;
            Event_History* first = ring.commit(0, 80661U, 8661U, when, when + 3);
            first->systype = DSD_SYNC_P25P1_POS;
            with_rx(first, c.kind, c.a, c.b, c.freq);
            first->pos.scope.kind = c.kind;
            first->pos.scope.a = c.a;
            first->pos.scope.b = c.b;
            first->pos.identity_freq_hz = c.freq;
            model.refresh(ring.state);
        }
        QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
        QJsonObject row = rows.at(0).toObject();
        for (const char* scopeKey : {"rx", "sscope"}) {
            QJsonObject scope = row.value(QLatin1String(scopeKey)).toObject();
            expect(c.what, !scope.isEmpty());
            if (c.value.isUndefined()) {
                scope.remove(QLatin1String(c.key));
            } else {
                scope.insert(QLatin1String(c.key), c.value);
            }
            row.insert(QLatin1String(scopeKey), scope);
        }
        rows.replace(0, row);
        expect(c.what, dsd_qt::json_store_save_array("call_history.json", rows));
        {
            CallHistoryModel reloaded;
            Event_History* next = ring.commit(0, 80661U, 8661U, when + 5, when + 8);
            next->systype = DSD_SYNC_P25P1_POS;
            with_rx(next, c.kind, c.a, c.b, c.freq);
            next->pos.scope.kind = c.kind;
            next->pos.scope.a = c.a;
            next->pos.scope.b = c.b;
            next->pos.identity_freq_hz = c.freq;
            reloaded.refresh(ring.state);
            expect(c.what, reloaded.count() == 1);
        }
        const QJsonObject rx = stored_row_with(80661U, 8661U).value(QLatin1String("rx")).toObject();
        expect(c.what, rx.value(QLatin1String("k")).toInt() == c.kind
                           && rx.value(QLatin1String("a")).toInteger() == static_cast<qint64>(c.a)
                           && rx.value(QLatin1String("b")).toInteger() == static_cast<qint64>(c.b)
                           && rx.value(QLatin1String("f")).toInteger() == c.freq);
    }

    // A stored sync type that is no integer is not known: the row reads no protocol, not the sync type 0 a string
    // converts to.
    for (const QJsonValue& proto : {QJsonValue(QStringLiteral("x")), QJsonValue(0.5)}) {
        resetStorage();
        RingFixture ring;
        {
            CallHistoryModel model;
            ring.commit(0, 80662U, 8662U, when, when + 3)->systype = DSD_SYNC_DMR_BS_VOICE_POS;
            model.refresh(ring.state);
        }
        QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
        QJsonObject row = rows.at(0).toObject();
        expect("stored sync type: written", row.contains(QLatin1String("proto")));
        row.insert(QLatin1String("proto"), proto);
        rows.replace(0, row);
        expect("stored sync type: rewritten", dsd_qt::json_store_save_array("call_history.json", rows));
        {
            CallHistoryModel reloaded;
            reloaded.startSaveForTest();
            reloaded.waitForSaveForTest();
        }
        expect("stored sync type: one that is no integer reads as not known",
               !stored_row_with(80662U, 8662U).isEmpty()
                   && !stored_row_with(80662U, 8662U).contains(QLatin1String("proto")));
    }
}

/* A notice decoded again is the logged one only when it is the same fix: the same text with another position is another
 * delivery. The rev and the process of a decode in a fresh ring differ and do not count. */
void
test_repeated_notice_compares_the_fix(void) {
    resetStorage();
    const time_t when = 1754707000;
    CallHistoryModel model;
    {
        RingFixture ring;
        with_gps(ring.commitNotice(0, when, "2026-11-17 12:00:00 LRRP position"), "LRRP", 225000000, 450000000, 3U);
        model.refresh(ring.state);
        expect("repeat: the notice lands", model.count() == 1);
    }
    {
        RingFixture again;
        with_gps(again.commitNotice(0, when, "2026-11-17 12:00:00 LRRP position"), "LRRP", 225000000, 450000000, 40U)
            ->pos.proc_instance = 0xBEEFULL;
        model.refresh(again.state);
        expect("repeat: the same fix decoded again is the same delivery", model.count() == 1);
    }
    {
        RingFixture other;
        with_gps(other.commitNotice(0, when, "2026-11-17 12:00:00 LRRP position"), "LRRP", 226000000, 450000000, 41U);
        model.refresh(other.state);
        expect("repeat: the same text with another fix is another delivery", model.count() == 2);
    }
}

/* Fail closed, one model (issue #800): a log an older build stored before fragments were kept (no `fr`, no `fp`) is
 * converted into the fragment-contributions model on load, each stored row a part of its own. Two adjacent rows of one
 * talkgroup fold into one, and that row is the fold of both parts: the source ID and the emergency flag only the later
 * row learned are kept, at load and after a save and reload. */
void
test_legacy_rows_fold_as_parts(void) {
    resetStorage();
    const qint64 when = 1754707300;
    QJsonObject earlier{{QStringLiteral("when"), when},
                        {QStringLiteral("end"), when + 3},
                        {QStringLiteral("name"), QStringLiteral("Talkgroup 80701")},
                        {QStringLiteral("tg"), 80701},
                        {QStringLiteral("src"), 0},
                        {QStringLiteral("durationSecs"), 3},
                        {QStringLiteral("slot"), 0},
                        {QStringLiteral("seq"), 1},
                        {QStringLiteral("session"), 1}};
    QJsonObject later = earlier;
    later.insert(QStringLiteral("when"), when + 4);
    later.insert(QStringLiteral("end"), when + 9);
    later.insert(QStringLiteral("durationSecs"), 5);
    later.insert(QStringLiteral("src"), 8701);
    later.insert(QStringLiteral("em"), true);
    later.insert(QStringLiteral("seq"), 2);
    expect("legacy parts: an older build's store is written",
           dsd_qt::json_store_save_array("call_history.json", QJsonArray{later, earlier}));
    const auto folded = [](const CallHistoryModel& model) {
        const QModelIndex idx = model.index(0);
        return model.count() == 1 && model.data(idx, CallHistoryModel::SrcRole).toULongLong() == 8701U
               && model.data(idx, CallHistoryModel::EmergencyRole).toBool()
               && model.data(idx, CallHistoryModel::WhenRole).toLongLong() == when
               && model.data(idx, CallHistoryModel::DurationSecsRole).toInt() == 9;
    };
    {
        CallHistoryModel model;
        expect("legacy parts: the two rows fold into one that keeps what the later row learned", folded(model));
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    CallHistoryModel reloaded;
    expect("legacy parts: the same after a save and reload", folded(reloaded));
}

/* Fail closed (issue #800): a destination is the call's, whoever sent the fragment that names it. Three D-STAR
 * fragments on one named channel: N0CALL with no destination, an unknown caller to DEST-ONE, then N0CALL to DEST-TWO.
 * The senderless fragment may join N0CALL's call, but its destination is the row's then, so the third fragment, naming
 * another destination, is another call; the row shows DEST-ONE, the name its only destination gave it. */
void
test_senderless_fragment_keeps_its_destination(void) {
    resetStorage();
    const time_t when = 1754707400;
    RingFixture ring;
    CallHistoryModel model;
    Event_History* first = ring.commit(0, 0U, 0U, when, when + 2, "", "", "Club channel");
    first->systype = DSD_SYNC_DSTAR_HD_POS;
    DSD_SNPRINTF(first->src_str, sizeof(first->src_str), "%s", "N0CALL");
    model.refresh(ring.state);
    Event_History* anonymous = ring.commit(0, 0U, 0U, when + 3, when + 5, "DEST-ONE", "", "Club channel");
    anonymous->systype = DSD_SYNC_DSTAR_HD_POS;
    model.refresh(ring.state);
    expect("destination: the senderless fragment joins N0CALL's call", model.count() == 1);
    Event_History* other = ring.commit(0, 0U, 0U, when + 6, when + 8, "DEST-TWO", "", "Club channel");
    other->systype = DSD_SYNC_DSTAR_HD_POS;
    DSD_SNPRINTF(other->src_str, sizeof(other->src_str), "%s", "N0CALL");
    model.refresh(ring.state);
    expect("destination: a fragment naming another destination is another call", model.count() == 2);
    const int joined = model.count() == 2 ? 1 : 0;
    expect("destination: the joined row shows the destination its senderless fragment named",
           model.data(model.index(joined), CallHistoryModel::NameRole).toString() == QStringLiteral("DEST-ONE")
               && model.data(model.index(0), CallHistoryModel::NameRole).toString() == QStringLiteral("DEST-TWO"));
    model.startSaveForTest();
    model.waitForSaveForTest();
    const QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    expect("destination: the joined row stores its destination",
           rows.size() == 2 && rows.at(1).toObject().value(QLatin1String("tgtText")).toString() == "DEST-ONE");
}

/* Fail closed (issue #800): a fragment's part is its newest reading, whole. A single M17 fragment reread from DEST-ONE
 * to DEST-TWO shows DEST-TWO as its name beside its new message, live and after a reload: the name its destination gave
 * it changes with the destination. */
void
test_reread_destination_renames_row(void) {
    resetStorage();
    const time_t when = 1754707500;
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* f = ring.commit(0, 0U, 0U, when, when + 3, "DEST-ONE");
        f->systype = DSD_SYNC_M17_STR_POS;
        DSD_SNPRINTF(f->src_str, sizeof(f->src_str), "%s", "N0CALL");
        model.refresh(ring.state);
        expect("rename: the row is named by its destination",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::NameRole).toString() == QStringLiteral("DEST-ONE"));
        f = &ring.rings[0].Event_History_Items[1];
        DSD_SNPRINTF(f->tgt_str, sizeof(f->tgt_str), "%s", "DEST-TWO");
        DSD_SNPRINTF(f->text_message, sizeof(f->text_message), "%s", "For two");
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("rename: the reread destination names the row beside its message",
               model.count() == 1
                   && model.data(model.index(0), CallHistoryModel::NameRole).toString() == QStringLiteral("DEST-TWO")
                   && detail_of(model, 0) == QStringLiteral("For two"));
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    CallHistoryModel reloaded;
    expect("rename: the same after a reload",
           reloaded.count() == 1
               && reloaded.data(reloaded.index(0), CallHistoryModel::NameRole).toString()
                      == QStringLiteral("DEST-TWO"));
}

/* Fail closed (issue #800): a fragment first read with no source and a text message, then reread naming radio 9999 and
 * no message, shows no message: the anonymous text its earlier reading carried is not attributed to that radio, live
 * and after a reload. */
void
test_reread_sender_drops_withdrawn_text(void) {
    resetStorage();
    const time_t when = 1754707600;
    RingFixture ring;
    {
        CallHistoryModel model;
        Event_History* f = ring.commit(0, 80702U, 0U, when, when + 3);
        f->systype = DSD_SYNC_P25P1_POS;
        DSD_SNPRINTF(f->text_message, sizeof(f->text_message), "%s", "Anonymous text");
        model.refresh(ring.state);
        expect("sender: the anonymous reading shows its text",
               model.count() == 1 && detail_of(model, 0) == QStringLiteral("Anonymous text"));
        f = &ring.rings[0].Event_History_Items[1];
        f->source_id = 9999U;
        f->text_message[0] = '\0';
        ring.touchCommitted(0);
        model.refresh(ring.state);
        expect("sender: the named reading shows no withdrawn text",
               model.count() == 1 && model.data(model.index(0), CallHistoryModel::SrcRole).toULongLong() == 9999U
                   && detail_of(model, 0).isEmpty());
        model.startSaveForTest();
        model.waitForSaveForTest();
    }
    CallHistoryModel reloaded;
    expect("sender: the same after a reload", reloaded.count() == 1 && detail_of(reloaded, 0).isEmpty());
}

/* A stored integer, 0 when missing. */
qint64
json_i64(const QJsonObject& obj, const char* key) {
    return obj.value(QLatin1String(key)).toInteger(0);
}

/* Issue #903: what History hands the remembered talkgroups (HeardTalkgroupSink), recorded. */
struct HeardObservation {
    QString owner;
    int variant = 0;
    quint32 id = 0;
    qint64 when = 0;
};

class RecordingHeardSink : public dsd_qt::HeardTalkgroupSink {
  public:
    QList<HeardObservation> seen;

    void
    observe(const QString& ownerUid, int variant, quint32 id, qint64 whenSecs) override {
        seen.append(HeardObservation{ownerUid, variant, id, whenSecs});
    }

    bool
    has(const QString& owner, quint32 id, qint64 when) const {
        return std::any_of(seen.cbegin(), seen.cend(),
                           [&](const HeardObservation& o) { return o.owner == owner && o.id == id && o.when == when; });
    }
};

/* The proof the core freezes on a row whose checked message named ordinary talkgroup @p id (Task 10's fields), heard on
 * live input unless @p live is false. */
Event_History*
prove(Event_History* item, uint32_t id, bool live = true, uint8_t variant = 1U) {
    item->heard_variant = variant;
    item->heard_id = id;
    item->live_input = live ? 1U : 0U;
    return item;
}

/* The stored parts' proofs of the stored row of talkgroup @p tg: one entry per part, its `hid` (0 for none). */
QList<qint64>
stored_part_proofs(qulonglong tg) {
    QList<qint64> proofs;
    const QJsonObject row = stored_row_with_tg(dsd_qt::json_store_load_array("call_history.json"), tg);
    for (const auto& part : row.value(QLatin1String("fp")).toArray()) {
        proofs.append(json_i64(part.toObject(), "hid"));
    }
    return proofs;
}

/* Issue #903: a fragment heard on live input with a verified talkgroup is handed to the sink once, under the session's
 * saved system, at its own time; a later reading of it (its end extends) hands nothing again. A reading off live input,
 * one the operator tuned to, a scan member of a saved-system start, a session that names no owner and a read without
 * options hand nothing. */
void
test_heard_proof_reaches_the_sink_once(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    CallHistoryModel model;
    model.setHeardSink(&sink);
    model.beginSessionOn(QStringLiteral("System A"), QStringLiteral("sys-a"));
    model.setSessionOwners(model.session(), QStringLiteral("sys-a"), {});
    const time_t when = 1754600000;
    Event_History* item = prove(ring.commit(0, 7001, 501, when, when + 4), 7001);
    model.refresh(ring.state, opts.get());
    expect("heard: a live verified call is handed on", sink.seen.size() == 1 && sink.has("sys-a", 7001, when + 4));
    expect("heard: with its variant", sink.seen.size() == 1 && sink.seen.at(0).variant == 1);
    item->event_time = when + 9;
    ring.touchCommitted(0);
    model.refresh(ring.state, opts.get());
    expect("heard: a later reading of it hands nothing again", sink.seen.size() == 1);

    prove(ring.commit(0, 7002, 502, when + 30, when + 34), 7002, false);
    prove(ring.commit(0, 7003, 503, when + 60, when + 64), 7003)->operator_tuned = 1U;
    prove(ring.commit(0, 7004, 504, when + 90, when + 94), 7004)->scan_member = 1U;
    prove(ring.commit(0, 7005, 505, when + 120, when + 124, "", "", "minus-y-row"), 7005)->scan_member = 1U;
    model.refresh(ring.state, opts.get());
    expect("heard: file input, an operator tune and a saved system's scan members hand nothing",
           sink.seen.size() == 1 && model.count() == 5);
    prove(ring.commit(0, 7006, 506, when + 150, when + 154), 7006);
    model.refresh(ring.state);
    expect("heard: a read without options establishes no owner", sink.seen.size() == 1 && model.count() == 6);

    model.beginSessionOn(QStringLiteral("Exploring"), QString());
    RingFixture explore;
    prove(explore.commit(0, 7007, 507, when + 180, when + 184), 7007);
    model.refresh(explore.state, opts.get());
    expect("heard: a session that names no owner hands nothing", sink.seen.size() == 1);
}

/* Issue #903 (PR2 review r4, P1): a saved system's session whose receiver a live retune moved (a configuration from the
 * control API, a decode mode, a channel map: Event_History::tuning_moved, with no operator hold) learns nothing for
 * the saved system from then on, and names no owner for the call; the calls heard before the move stay its own. */
void
test_heard_after_a_live_retune_is_no_ones(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    CallHistoryModel model;
    model.setHeardSink(&sink);
    model.beginSessionOn(QStringLiteral("System A"), QStringLiteral("sys-a"));
    model.setSessionOwners(model.session(), QStringLiteral("sys-a"), {});
    const time_t when = 1754605000;
    prove(ring.commit(0, 7201, 701, when, when + 4), 7201);
    prove(ring.commit(0, 7202, 702, when + 30, when + 34), 7202)->tuning_moved = 1U;
    model.refresh(ring.state, opts.get());
    expect("live retune: the call before it is the saved system's",
           sink.seen.size() == 1 && sink.has("sys-a", 7201, when + 4));
    expect("live retune: the call after it is handed on to no one", model.count() == 2 && sink.seen.size() == 1);
    // A proof that comes late on the moved call hands nothing on either.
    Event_History* late = ring.commit(0, 7203, 703, when + 60, when + 64);
    late->tuning_moved = 1U;
    model.refresh(ring.state, opts.get());
    prove(late, 7203);
    ring.touchCommitted(0);
    model.refresh(ring.state, opts.get());
    expect("live retune: a late proof of a call after it is no one's", sink.seen.size() == 1);
}

/* Issue #903 (PR2 review r4 ruling): a scan list's Go to, then its resume. The calls heard while the operator holds
 * the tuner (operator_tuned, off any target) are learnt for no one; the Go to does not move the tuning generation
 * (APP_COMMAND_QUEUE test_go_to_and_resume_keep_the_tuning_generation), so once the resume lands each target's calls
 * are learnt under its own saved system again, as they were before it. */
void
test_heard_scan_list_learns_again_after_go_to_and_resume(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    CallHistoryModel model;
    model.setHeardSink(&sink);
    model.beginSessionOn(QStringLiteral("County scan"), QString());
    model.setSessionOwners(
        model.session(), QString(),
        {{QStringLiteral("entry-a"), QStringLiteral("sys-a")}, {QStringLiteral("entry-b"), QStringLiteral("sys-b")}});
    const time_t when = 1754607000;
    // A call as the decoder stamps it at its first render: a scan member on its target, or the operator's.
    const auto heard = [&](uint32_t tg, time_t start, const char* target, bool operatorTuned) {
        Event_History* item = prove(ring.commit(0, tg, 900, start, start + 4, "", "", target), tg);
        item->scan_member = operatorTuned ? 0U : 1U;
        item->operator_tuned = operatorTuned ? 1U : 0U;
        item->tuning_moved = static_cast<uint8_t>(dsd_state_tuning_moved(ring.state));
    };
    heard(7301, when, "entry-a", false);
    model.refresh(ring.state, opts.get());
    expect("go to: before it, the target's call is its system's",
           sink.seen.size() == 1 && sink.has("sys-a", 7301, when + 4));
    // Go to: the scan pauses, the operator holds the tuner off any target.
    heard(7302, when + 30, "", true);
    model.refresh(ring.state, opts.get());
    expect("go to: a call heard on the operator's frequency is no one's", sink.seen.size() == 1);
    // The resume lands: the scan has the tuner again, on its rows.
    heard(7303, when + 60, "entry-b", false);
    heard(7304, when + 90, "entry-a", false);
    model.refresh(ring.state, opts.get());
    expect("go to: after the resume each target's call is its own system's again",
           sink.seen.size() == 3 && sink.has("sys-b", 7303, when + 64) && sink.has("sys-a", 7304, when + 94));
    expect("go to: every call is in History", model.count() == 4);
}

/* Issue #903: change detection sees a proof that arrives late on a committed row (a checked message after the commit)
 * and hands it on then, at the fragment's own time, once; the proof is the row's from then on, saved with it. A later
 * CRC failure marked on the row (fragment trust) keeps it and retracts nothing. */
void
test_heard_late_proof_and_crc_revision(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    const time_t when = 1754601000;
    {
        CallHistoryModel model;
        model.setHeardSink(&sink);
        model.beginSessionOn(QStringLiteral("System A"), QStringLiteral("sys-a"));
        model.setSessionOwners(model.session(), QStringLiteral("sys-a"), {});
        Event_History* item = ring.commit(0, 7101, 601, when, when + 4);
        item->live_input = 1U;
        model.refresh(ring.state, opts.get());
        expect("late proof: nothing before the proof", sink.seen.isEmpty() && model.count() == 1);
        prove(item, 7101);
        ring.touchCommitted(0);
        model.refresh(ring.state, opts.get());
        expect("late proof: the re-read hands it on", sink.seen.size() == 1 && sink.has("sys-a", 7101, when + 4));
        expect("late proof: still one row", model.count() == 1);
        item->crc_invalid = DSD_CRC_FAILED;
        item->event_time = when + 6;
        ring.touchCommitted(0);
        model.refresh(ring.state, opts.get());
        expect("late proof: a CRC revision hands nothing more", sink.seen.size() == 1 && model.count() == 1);
        model.saveNow();
        model.waitForSaveForTest();
    }
    const QJsonObject stored = stored_row_with_tg(dsd_qt::json_store_load_array("call_history.json"), 7101U);
    expect("late proof: the row keeps its proof through the CRC revision",
           json_i64(stored, "hv") == 1 && json_i64(stored, "hid") == 7101 && stored.value("live").toBool());
}

/* Issue #903: a merged conversation keeps each fragment's own proof: a verified transmission and an unverified one of
 * the same talker and group merge into one row in either order, and only the verified one is handed on, at its own
 * time, never the conversation's end; the stored parts keep one proof. */
void
test_heard_merged_conversation_keeps_fragment_proof(void) {
    for (int order = 0; order < 2; order++) {
        resetStorage();
        RingFixture ring;
        auto opts = std::make_unique<dsd_opts>();
        RecordingHeardSink sink;
        CallHistoryModel model;
        model.setHeardSink(&sink);
        model.beginSessionOn(QStringLiteral("System A"), QStringLiteral("sys-a"));
        model.setSessionOwners(model.session(), QStringLiteral("sys-a"), {});
        const time_t when = 1754602000 + order * 1000;
        if (order == 0) {
            prove(ring.commit(0, 7201, 701, when, when + 4), 7201);
            model.refresh(ring.state, opts.get());
            ring.commit(0, 7201, 701, when + 6, when + 10)->live_input = 1U;
            model.refresh(ring.state, opts.get());
        } else {
            ring.commit(0, 7201, 701, when, when + 4)->live_input = 1U;
            model.refresh(ring.state, opts.get());
            prove(ring.commit(0, 7201, 701, when + 6, when + 10), 7201);
            model.refresh(ring.state, opts.get());
            ring.commit(0, 7201, 701, when + 12, when + 16)->live_input = 1U;
            model.refresh(ring.state, opts.get());
        }
        const qint64 provenEnd = order == 0 ? when + 4 : when + 10;
        expect("merged: one conversation", model.count() == 1);
        expect("merged: the verified fragment alone is handed on, at its own end",
               sink.seen.size() == 1 && sink.has("sys-a", 7201, provenEnd));
        model.saveNow();
        model.waitForSaveForTest();
        const QList<qint64> proofs = stored_part_proofs(7201U);
        expect("merged: the parts keep one proof", proofs.size() == (order == 0 ? 2 : 3) && proofs.count(7201) == 1
                                                       && proofs.count(0) == proofs.size() - 1);
    }
}

/* Issue #903: the owners are bound to the ring the session read: the last call of system A, read by the flush before
 * the host is asked to start B, is A's; one that only the flush after the host's accept reads is no one's (it could be
 * B's run's on a reused ring); B's own ring is B's, and once B's run has its own ring, a late row of A's is A's too. A
 * relaunched model keeps both bindings and owners, and re-reads nothing it already handed on. */
void
test_heard_owner_follows_the_ring(void) {
    resetStorage();
    RingFixture ringA;
    RingFixture ringB;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    const time_t when = 1754603000;
    {
        CallHistoryModel model;
        model.setHeardSink(&sink);
        model.beginSessionOn(QStringLiteral("System A"), QStringLiteral("sys-a"));
        model.setSessionOwners(model.session(), QStringLiteral("sys-a"), {});
        prove(ringA.commit(0, 7301, 801, when, when + 4), 7301);
        model.refresh(ringA.state, opts.get());
        // Main.startWithMap: A's last call is read by the flush before the host is asked; another lands after it, read
        // by the flush after the host's accept; then B's session begins.
        prove(ringA.commit(0, 7302, 802, when + 30, when + 34), 7302);
        model.holdCaptureForStart();
        model.refresh(ringA.state, opts.get());
        prove(ringA.commit(0, 7307, 807, when + 40, when + 44), 7307);
        model.refresh(ringA.state, opts.get(), false);
        model.beginSessionOn(QStringLiteral("System B"), QStringLiteral("sys-b"));
        model.setSessionOwners(model.session(), QStringLiteral("sys-b"), {});
        expect("ring owner: A's last call, read before B's start was asked, is A's",
               sink.seen.size() == 2 && sink.has("sys-a", 7301, when + 4) && sink.has("sys-a", 7302, when + 34));
        prove(ringB.commit(0, 7303, 803, when + 60, when + 64), 7303);
        model.refresh(ringB.state, opts.get());
        expect("ring owner: B's ring is B's", sink.seen.size() == 3 && sink.has("sys-b", 7303, when + 64));
        prove(ringA.commit(0, 7306, 806, when + 70, when + 74), 7306);
        model.refresh(ringA.state, opts.get());
        expect("ring owner: with B on its own ring, A's late row is A's",
               sink.seen.size() == 4 && sink.has("sys-a", 7306, when + 74));
        expect("ring owner: the call only the flush after the accept read is no one's",
               std::none_of(sink.seen.cbegin(), sink.seen.cend(),
                            [](const HeardObservation& o) { return o.id == 7307U; }));
        prove(ringB.commit(0, 7304, 804, when + 90, when + 94), 7304);
    }
    RecordingHeardSink relaunchedSink;
    CallHistoryModel relaunched;
    relaunched.setHeardSink(&relaunchedSink);
    expect("ring owner: a reload hands nothing on", relaunchedSink.seen.isEmpty());
    prove(ringA.commit(0, 7305, 805, when + 120, when + 124), 7305);
    relaunched.refresh(ringA.state, opts.get());
    expect("ring owner: after a relaunch A's ring is still A's, and nothing is handed on twice",
           relaunchedSink.seen.size() == 1 && relaunchedSink.has("sys-a", 7305, when + 124));
    relaunched.refresh(ringB.state, opts.get());
    expect("ring owner: B's unread call after a relaunch is B's",
           relaunchedSink.seen.size() == 2 && relaunchedSink.has("sys-b", 7304, when + 94));
}

/* Issue #903: a scan-list session owns its targets one by one: a call on a target the owner map names is that saved
 * system's; one on a target it leaves out (an id an imported target file or another system also gives) and a call
 * with no target are no one's. */
void
test_heard_scan_targets_own_their_calls(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    CallHistoryModel model;
    model.setHeardSink(&sink);
    model.beginSessionWithOwners(QStringLiteral("County scan"), QString(), QString(),
                                 QVariantMap{{QStringLiteral("entry-a"), QStringLiteral("sys-a")}});
    const time_t when = 1754604000;
    prove(ring.commit(0, 7401, 901, when, when + 4, "", "", "entry-a"), 7401)->scan_member = 1U;
    prove(ring.commit(0, 7402, 902, when + 30, when + 34, "", "", "entry-shared"), 7402)->scan_member = 1U;
    prove(ring.commit(0, 7403, 903, when + 60, when + 64), 7403)->scan_member = 1U;
    prove(ring.commit(0, 7404, 904, when + 90, when + 94), 7404);
    model.refresh(ring.state, opts.get());
    expect("scan owner: only the named target's call is handed on, to its system",
           model.count() == 4 && sink.seen.size() == 1 && sink.has("sys-a", 7401, when + 4));
}

/* Issue #903 (review, Important 1): Main's flush before a start runs after the host accepted it, so the latest snapshot
 * can already be the starting run's ring. That flush leaves a ring no session owns unbound: the new session owns it,
 * and its calls go to the new saved system, not the old one. The old run's ring stays the old session's. */
void
test_heard_new_ring_at_the_pre_start_flush(void) {
    resetStorage();
    RingFixture ringA;
    RingFixture ringB;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    CallHistoryModel model;
    model.setHeardSink(&sink);
    model.beginSessionWithOwners(QStringLiteral("System A"), QStringLiteral("sys-a"), QStringLiteral("sys-a"), {});
    const time_t when = 1754606000;
    prove(ringA.commit(0, 7601, 1101, when, when + 4), 7601);
    model.refresh(ringA.state, opts.get());
    // The host accepted B's start and published B's ring before Main's flush read it.
    model.refresh(ringB.state, opts.get(), false);
    model.beginSessionWithOwners(QStringLiteral("System B"), QStringLiteral("sys-b"), QStringLiteral("sys-b"), {});
    prove(ringB.commit(0, 7602, 1102, when + 30, when + 34), 7602);
    model.refresh(ringB.state, opts.get());
    expect("pre-start flush: the starting run's ring is the new session's",
           sink.seen.size() == 2 && sink.has("sys-b", 7602, when + 34));
    prove(ringA.commit(0, 7603, 1103, when + 60, when + 64), 7603);
    model.refresh(ringA.state, opts.get());
    expect("pre-start flush: the old run's late call is still the old session's",
           sink.seen.size() == 3 && sink.has("sys-a", 7603, when + 64));
}

/* Issue #903 (re-review, Minor 1): the old run's ring was never read during its run, and the new run has not published
 * when its session begins. Main's flush before the host is asked binds the old ring to the old session, so the rows it
 * reads keep the old owner; a row first read by the flush after the host's accept could be the new run's on a reused
 * ring, and is no one's (PR2 review r2, P1, fail closed); the new run's ring is the new one's. */
void
test_heard_unread_old_ring_keeps_its_owner(void) {
    resetStorage();
    RingFixture ringA;
    RingFixture ringB;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    CallHistoryModel model;
    model.setHeardSink(&sink);
    model.beginSessionWithOwners(QStringLiteral("System A"), QStringLiteral("sys-a"), QStringLiteral("sys-a"), {});
    const time_t when = 1754608000;
    prove(ringA.commit(0, 7801, 1301, when, when + 4), 7801);
    // Main.startWithMap: the hold and the flush before the host is asked, the old run's final publish, the flush after
    // the host's accept (B not yet published), then the new session.
    model.holdCaptureForStart();
    model.refresh(ringA.state, opts.get());
    prove(ringA.commit(0, 7802, 1302, when + 30, when + 34), 7802);
    model.refresh(ringA.state, opts.get(), false);
    model.beginSessionWithOwners(QStringLiteral("System B"), QStringLiteral("sys-b"), QStringLiteral("sys-b"), {});
    expect("unread old ring: the rows read before the start was asked keep the old owner, the later one is no one's",
           sink.seen.size() == 1 && sink.has("sys-a", 7801, when + 4));
    prove(ringB.commit(0, 7803, 1303, when + 60, when + 64), 7803);
    model.refresh(ringB.state, opts.get());
    expect("unread old ring: the new run's ring is the new owner's",
           sink.seen.size() == 2 && sink.has("sys-b", 7803, when + 64));
}

/* Issue #903 (PR2 review r1, P1): the starting run commits a verified call after the host accepted its start and before
 * Main's flush reads the new ring. That flush leaves the ring wholly unread, its commit revision included, so the read
 * after the new session begins takes the call in under it: the new saved system learns it, and History logs it in the
 * new session. */
void
test_heard_new_ring_call_before_the_pre_start_flush(void) {
    resetStorage();
    RingFixture ringA;
    RingFixture ringB;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    CallHistoryModel model;
    model.setHeardSink(&sink);
    model.beginSessionWithOwners(QStringLiteral("System A"), QStringLiteral("sys-a"), QStringLiteral("sys-a"), {});
    const time_t when = 1754609000;
    prove(ringA.commit(0, 7901, 1401, when, when + 4), 7901);
    model.holdCaptureForStart();
    model.refresh(ringA.state, opts.get());
    // The host accepted B's start, and B's run committed a verified call before Main's flush read its ring.
    prove(ringB.commit(0, 7902, 1402, when + 30, when + 34), 7902);
    model.refresh(ringB.state, opts.get(), false);
    expect("pre-start call: the flush reads nothing of the new ring", sink.seen.size() == 1 && model.count() == 1);
    model.beginSessionWithOwners(QStringLiteral("System B"), QStringLiteral("sys-b"), QStringLiteral("sys-b"), {});
    const qint64 sessionB = model.session();
    model.refresh(ringB.state, opts.get());
    expect("pre-start call: the new system learns it", sink.seen.size() == 2 && sink.has("sys-b", 7902, when + 34));
    const QModelIndex row = model.index(0, 0);
    expect("pre-start call: History logs it in the new session",
           model.count() == 2 && model.data(row, CallHistoryModel::TgRole).toULongLong() == 7902U
               && model.data(row, CallHistoryModel::SessionRole).toLongLong() == sessionB);
}

/* Issue #903 (PR2 review r1, P2): an embedding host runs B on the state A's run used, so B's run reuses A's ring. A
 * session's start marks where the ring read last stood: what the ring pushes after it is B's, and the rows before it,
 * revisions included, stay A's. The call on the air is B's, and a relaunched model keeps both generations. */
void
test_heard_reused_ring_is_a_new_generation(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    const time_t when = 1754610000;
    {
        CallHistoryModel model;
        model.setHeardSink(&sink);
        model.beginSessionWithOwners(QStringLiteral("System A"), QStringLiteral("sys-a"), QStringLiteral("sys-a"), {});
        prove(ring.commit(0, 8001, 1501, when, when + 4), 8001);
        ring.commit(0, 8002, 1502, when + 30, when + 34)->live_input = 1U;
        // Main.startWithMap, the host reusing the state: B's session begins on the ring A's run left.
        model.holdCaptureForStart();
        model.refresh(ring.state, opts.get());
        model.refresh(ring.state, opts.get(), false);
        model.beginSessionWithOwners(QStringLiteral("System B"), QStringLiteral("sys-b"), QStringLiteral("sys-b"), {});
        expect("reused ring: the call on the air is B's",
               model.heardOwnerOnAir(static_cast<quint64>(ring.rings[0].instance), false, false, QString())
                   == QStringLiteral("sys-b"));
        prove(ring.commit(0, 8003, 1503, when + 60, when + 64), 8003);
        model.refresh(ring.state, opts.get());
        expect("reused ring: B's call on A's ring is B's", sink.has("sys-b", 8003, when + 64));
        // A's row (one below B's now) gains its proof after B began: it is still A's row.
        prove(&ring.rings[0].Event_History_Items[2], 8002);
        ring.touchCommitted(0);
        model.refresh(ring.state, opts.get());
        expect("reused ring: a revision of A's row is A's",
               sink.seen.size() == 3 && sink.has("sys-a", 8001, when + 4) && sink.has("sys-a", 8002, when + 34));
    }
    RecordingHeardSink relaunchedSink;
    CallHistoryModel relaunched;
    relaunched.setHeardSink(&relaunchedSink);
    prove(ring.commit(0, 8004, 1504, when + 90, when + 94), 8004);
    relaunched.refresh(ring.state, opts.get());
    expect("reused ring: after a relaunch the ring's new rows are still B's",
           relaunchedSink.seen.size() == 1 && relaunchedSink.has("sys-b", 8004, when + 94));
}

/* Issue #903 (PR2 review r2, P1; ruling: fail closed): a run that reuses the old run's state (an embedding host) commits
 * verified calls on the old ring after the host accepted its start and before Main's flush after the accept reads them.
 * Nothing tells them from the old run's last commits, so they are no one's: neither system learns them, nor a late
 * proof of one, nor after a relaunch. History still logs them. The old run's rows read before the start stay its
 * owner's, and what the ring pushes after that flush is the new session's. */
void
test_heard_reused_ring_call_before_the_post_accept_flush(void) {
    resetStorage();
    RingFixture ring;
    const auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    const time_t when = 1754611000;
    const auto heardOf = [](const RecordingHeardSink& recorded, quint32 id) {
        return std::any_of(recorded.seen.cbegin(), recorded.seen.cend(),
                           [id](const HeardObservation& o) { return o.id == id; });
    };
    {
        CallHistoryModel model;
        model.setHeardSink(&sink);
        model.beginSessionWithOwners(QStringLiteral("System A"), QStringLiteral("sys-a"), QStringLiteral("sys-a"), {});
        prove(ring.commit(0, 8101, 1601, when, when + 4), 8101);
        // Main.startWithMap: the hold and the flush before the host is asked.
        model.holdCaptureForStart();
        model.refresh(ring.state, opts.get());
        // The host accepted B's start on A's state; B's run commits a verified call, and two whose proofs come later,
        // before the flush after the accept reads the ring.
        prove(ring.commit(0, 8102, 1602, when + 30, when + 34), 8102);
        ring.commit(0, 8103, 1603, when + 40, when + 44)->live_input = 1U;
        ring.commit(0, 8105, 1605, when + 50, when + 54)->live_input = 1U;
        model.refresh(ring.state, opts.get(), false);
        model.beginSessionWithOwners(QStringLiteral("System B"), QStringLiteral("sys-b"), QStringLiteral("sys-b"), {});
        expect("ambiguous commit: neither system learns it",
               sink.seen.size() == 1 && sink.has("sys-a", 8101, when + 4) && !heardOf(sink, 8102));
        expect("ambiguous commit: History logs every call", model.count() == 4);
        prove(&ring.rings[0].Event_History_Items[2], 8103);
        ring.touchCommitted(0);
        model.refresh(ring.state, opts.get());
        expect("ambiguous commit: a late proof is no one's either", sink.seen.size() == 1 && !heardOf(sink, 8103));
        prove(ring.commit(0, 8104, 1604, when + 60, when + 64), 8104);
        model.refresh(ring.state, opts.get());
        expect("ambiguous commit: B's next call is B's", sink.seen.size() == 2 && sink.has("sys-b", 8104, when + 64));
        model.saveNow();
        model.waitForSaveForTest();
    }
    RecordingHeardSink relaunchedSink;
    CallHistoryModel relaunched;
    relaunched.setHeardSink(&relaunchedSink);
    // The third ambiguous call's proof arrives after a relaunch.
    prove(&ring.rings[0].Event_History_Items[2], 8105);
    ring.touchCommitted(0);
    relaunched.refresh(ring.state, opts.get());
    expect("ambiguous commit: after a relaunch a late proof is still no one's", !heardOf(relaunchedSink, 8105));
    prove(ring.commit(0, 8106, 1606, when + 90, when + 94), 8106);
    relaunched.refresh(ring.state, opts.get());
    expect("ambiguous commit: after a relaunch B's new call is B's",
           relaunchedSink.seen.size() == 1 && relaunchedSink.has("sys-b", 8106, when + 94));
}

/* Issue #903 (review, Minor 4): History's Clear hides rows; it does not un-hear a talkgroup. A row the clear covers
 * whose proof arrives after the clear is learnt, once, and stays out of the log. A cleared row the seen map does not
 * know gives nothing. */
void
test_heard_late_proof_on_a_cleared_row(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    RecordingHeardSink sink;
    CallHistoryModel model;
    model.setHeardSink(&sink);
    model.beginSessionWithOwners(QStringLiteral("System A"), QStringLiteral("sys-a"), QStringLiteral("sys-a"), {});
    const time_t when = 1754607000;
    Event_History* item = ring.commit(0, 7701, 1201, when, when + 4);
    item->live_input = 1U;
    model.refresh(ring.state, opts.get());
    model.clearAll();
    expect("cleared proof: the clear empties the log", model.count() == 0);
    prove(item, 7701);
    ring.touchCommitted(0);
    model.refresh(ring.state, opts.get());
    expect("cleared proof: the late proof is learnt", sink.seen.size() == 1 && sink.has("sys-a", 7701, when + 4));
    expect("cleared proof: the row stays cleared", model.count() == 0);
    item->event_time = when + 8;
    ring.touchCommitted(0);
    model.refresh(ring.state, opts.get());
    expect("cleared proof: once", sink.seen.size() == 1);
}

/* Issue #903: the proof and the live latch are saved with the row and its parts, and read back; a row an older build
 * stored reads none. What a reload folds hands nothing on, the spill aside. */
void
test_heard_fields_round_trip(void) {
    resetStorage();
    RingFixture ring;
    auto opts = std::make_unique<dsd_opts>();
    const time_t when = 1754605000;
    {
        CallHistoryModel model;
        model.beginSessionOn(QStringLiteral("System A"), QStringLiteral("sys-a"));
        prove(ring.commit(0, 7501, 1001, when, when + 4), 7501, true, 3U);
        ring.commit(0, 7502, 1002, when + 30, when + 34);
        prove(ring.commit(0, 7503, 1003, when + 60, when + 64), 7503, false);
        model.refresh(ring.state, opts.get());
        model.saveNow();
        model.waitForSaveForTest();
    }
    QJsonArray rows = dsd_qt::json_store_load_array("call_history.json");
    const QJsonObject proven = stored_row_with_tg(rows, 7501U);
    const QJsonObject plain = stored_row_with_tg(rows, 7502U);
    const QJsonObject file = stored_row_with_tg(rows, 7503U);
    expect("round trip: a proven live row stores its proof and latch",
           json_i64(proven, "hv") == 3 && json_i64(proven, "hid") == 7501 && proven.value("live").toBool());
    expect("round trip: a row with no proof stores none", !plain.contains("hv") && !plain.contains("hid"));
    expect("round trip: a row off live input stores its proof and no latch",
           json_i64(file, "hid") == 7503 && !file.contains("live"));
    {
        RecordingHeardSink sink;
        CallHistoryModel reloaded;
        reloaded.setHeardSink(&sink);
        reloaded.saveNow();
        reloaded.waitForSaveForTest();
        expect("round trip: a reload hands nothing on", sink.seen.isEmpty() && reloaded.count() == 3);
    }
    rows = dsd_qt::json_store_load_array("call_history.json");
    expect("round trip: the reload keeps the fields", json_i64(stored_row_with_tg(rows, 7501U), "hv") == 3
                                                          && stored_row_with_tg(rows, 7501U).value("live").toBool()
                                                          && json_i64(stored_row_with_tg(rows, 7503U), "hid") == 7503);
    // An older build's row: no fields at all.
    for (int i = 0; i < rows.size(); i++) {
        QJsonObject row = rows.at(i).toObject();
        row.remove("hv");
        row.remove("hid");
        row.remove("live");
        rows.replace(i, row);
    }
    expect("round trip: legacy fixture saved", dsd_qt::json_store_save_array("call_history.json", rows));
    {
        CallHistoryModel legacy;
        legacy.saveNow();
        legacy.waitForSaveForTest();
    }
    const QJsonObject legacyRow = stored_row_with_tg(dsd_qt::json_store_load_array("call_history.json"), 7501U);
    expect("round trip: an older build's row reads no proof and not live",
           !legacyRow.contains("hv") && !legacyRow.contains("live"));
}

} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
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
    g_previousHandler = qInstallMessageHandler(count_warnings);

    test_system_identity_persistence();
    test_scanning_rows_never_gain_hold_identity();
    test_tuner_owner_stamps_set_row_identity();
    test_source_labels_survive_merge_and_relaunch();
    test_source_label_is_independent_of_refresh_timing();
    test_source_label_provenance_survives_merged_span_and_relaunch();
    test_two_slots_same_second_same_talkgroup();
    test_textual_targets_stay_distinct();
    test_alias_only_row_is_logged();
    test_channel_labelled_tg0_row_is_logged();
    test_labelled_notice_name_drops_its_own_channel_prefix();
    test_marked_and_serviced_notice_titles();
    test_channel_label_is_its_own_role();
    test_type_d_text_yields_to_names();
    test_dcr_row_is_logged_by_name();
    test_dcr_and_numbered_rows_stay_apart();
    test_dcr_row_refuses_a_fragment_that_learns_a_source();
    test_type_d_prefixes_do_not_merge();
    test_target_ids_read_as_their_names();
    test_target_names_outlive_their_session();
    test_reacquisition_merge_updates_in_place();
    test_ring_walk_gated_on_commit_rev();
    test_relaunch_does_not_reingest();
    test_clear_shows_later_calls_whatever_their_stamps();
    test_clear_removes_rows_without_a_reset();
    test_clear_survives_a_relaunch();
    test_legacy_clear_watermark_is_retired();
    test_new_session_starts_fresh();
    test_clear_is_bound_to_its_ring();
    test_clear_without_a_ring_hides_nothing();
    test_clear_after_a_quiet_start_keeps_the_next_call();
    test_pending_clear_survives_a_start_on_a_reused_ring();
    test_clear_survives_a_start_on_a_reused_ring_without_the_seen_store();
    test_clear_after_a_start_covers_a_reused_ring();
    test_clear_survives_a_relaunch_after_a_start_on_a_reused_ring();
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
    test_update_of_a_call_heard_again_reaches_its_row();
    test_update_of_a_replayed_call_under_newer_rows_reaches_its_row();
    test_update_of_a_live_call_keeps_landing_on_its_row();
    test_carrier_roles_and_text();
    test_carrier_survives_save_and_reload();
    test_store_without_carrier_keys_loads_unknown();
    test_carrier_filled_in_place_reingests();
    test_fragments_on_different_frequencies_merge();
    test_repeated_notice_gains_carrier();
    test_repeated_legacy_notice_matches_its_detail();
    test_site_roles_and_text();
    test_site_survives_save_and_reload();
    test_store_without_site_keys_loads_unknown();
    test_site_store_rejects_bad_parts();
    test_site_filled_in_place_reingests();
    test_fragments_on_conflicting_sites_stay_apart();
    test_fragment_gains_compatible_site();
    test_site_split_survives_relaunch();
    test_site_spill_round_trip();
    test_site_text_cut_mid_character_survives_reload();
    test_seen_store_rejects_unknown_channel_kind();
    test_update_conflicting_after_fold_keeps_row_identity();
    test_update_learning_another_source_keeps_row_source();
    test_channel_kind_conflict_stays_apart();
    test_dmr_text_differs_raw_equal_merges_and_keeps_text();
    test_site_text_changed_in_place_reingests();
    test_site_text_digest_survives_save_and_reload();
    test_site_text_digest_seeded_from_rows();
    test_site_text_digest_adopted_is_saved();
    test_site_text_digest_spill_round_trip();
    test_record_for_uid();
    test_receive_service_identity();
    test_slot_proof_merges_independently_of_code();
    test_receive_only_updates_and_label_renames();
    test_unconfirmed_fragment_joins_named_service();
    test_analog_receptions_never_fold();
    test_received_tone_reads_as_its_label();
    test_search_by_frequency();
    test_search_matches_what_the_row_shows();
    test_frequency_search_follows_the_row();
    test_stores_serialise_off_the_gui_thread();
    test_save_delay_follows_the_bytes_written();
    test_save_now_writes_at_once();
    test_save_now_mid_write_queues_the_next_save();
    test_leaving_the_screen_saves();
    test_day_labels_follow_the_local_day();
    test_destructor_marks_its_teardown_stage();
    test_analog_replay_heard_again();
    test_analog_replay_same_second();
    test_analog_replay_under_another_system();
    test_analog_replay_with_other_push_stamps();
    test_analog_update_of_an_old_store_row();
    test_analog_roles_and_persistence();
    test_rows_have_stable_uids();
    test_fragments_follow_merges_reloads_and_trims();
    test_uid_for_fragment();
    test_recording_roles_follow_the_provider();
    test_failed_save_is_kept_and_retried();
    test_restart_after_a_half_written_save();
    test_unreadable_stores_are_kept_by_the_worker();
    test_fold_past_cap_is_unknown();
    test_update_past_cap_is_unknown();
    test_fold_after_cap_keeps_unknown();
    test_chain_past_cap_starts_new_row();
    test_clock_shifted_span_absorbs_nothing();
    test_old_row_end_derived();
    test_cap_exact_keeps_3600();
    test_unknown_learns_end();
    test_startless_row_update_stays_unknown();
    test_startless_fragment_extends_known_end();
    test_startless_fold_keeps_later_end();
    test_row_without_start_time_reads_unknown();
    test_voice_row_keeps_gps_and_message();
    test_position_role_feeds_the_map_pin();
    test_call_kind_is_kept_and_parts_calls();
    test_texts_and_position_survive_save_reload_and_spill();
    test_spill_replay_keeps_newer_revision();
    test_spill_text_only_replay_keeps_saved_position();
    test_lost_fix_gives_way_to_remaining_fragment();
    test_cleared_fix_leaves_single_fragment_row();
    test_restamped_fragment_withdraws_its_fix();
    test_restamped_fragment_found_by_identity();
    test_update_proving_other_slot_is_taken_back();
    test_fragment_update_learns_target();
    test_long_call_fold_lookups_stay_linear();
    test_spill_replay_keeps_newer_message();
    test_fragment_texts_follow_the_rows_radio();
    test_position_order_follows_runs_and_revs();
    test_callsign_callers_stay_apart();
    test_identity_placeholders_are_no_raw_identity();
    test_named_row_drops_anonymous_position();
    test_fragment_label_follows_the_rows_radio();
    test_textual_destinations_under_one_service_stay_apart();
    test_system_identity_splits_conventional_systems();
    test_weaker_identity_joins_stronger();
    test_fix_stays_on_its_reception();
    test_fix_stays_on_its_carrier();
    test_fix_stays_on_its_p25_phase();
    test_merged_row_keeps_its_anchor();
    test_late_position_rechecks_reception_system();
    test_fragment_on_another_system_withdraws_payload();
    test_fragment_on_another_system_withdraws_call_fields();
    test_withdrawn_fragment_recordings();
    test_playlist_rows();
    test_fragment_contributions_withdraw_together();
    test_fragment_withdrawal_keeps_other_ring_payload();
    test_fragment_update_rechecks_destination();
    test_json_field_fits_judges_conversion();
    test_stored_identity_parts_are_validated();
    test_partial_identity_joins_fuller();
    test_repeated_notice_compares_the_fix();
    test_legacy_rows_fold_as_parts();
    test_senderless_fragment_keeps_its_destination();
    test_reread_destination_renames_row();
    test_reread_sender_drops_withdrawn_text();
    test_heard_proof_reaches_the_sink_once();
    test_heard_after_a_live_retune_is_no_ones();
    test_heard_scan_list_learns_again_after_go_to_and_resume();
    test_heard_late_proof_and_crc_revision();
    test_heard_merged_conversation_keeps_fragment_proof();
    test_heard_owner_follows_the_ring();
    test_heard_scan_targets_own_their_calls();
    test_heard_fields_round_trip();
    test_heard_new_ring_at_the_pre_start_flush();
    test_heard_late_proof_on_a_cleared_row();
    test_heard_unread_old_ring_keeps_its_owner();
    test_heard_new_ring_call_before_the_pre_start_flush();
    test_heard_reused_ring_is_a_new_generation();
    test_heard_reused_ring_call_before_the_post_accept_flush();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
