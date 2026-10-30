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
 * notice heard again. Saves are bounded by bytes written, not by calls: a large store
 * waits longer between saves, and the moments the log must reach the disk (saveNow(),
 * the app leaving the screen) write it at once, even with a save already in flight.
 * Issue #708: every row has a stable uid that rowForUid() finds wherever the row moves, and
 * carries the ring fragments it absorbed through merges, load folding and reloads; the
 * recording roles come from the recording provider by those fragments and follow its change
 * news (a recording known before its row, one added later, one deleted). A save that fails,
 * in either store, keeps the log owed, is retried on a growing delay without another change,
 * reports once, and a restart after a half-written save logs nothing twice (B6). */

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
#include <QMetaType>
#include <QModelIndex>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
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

#include "call_history_filter.h"
#include "call_history_model.h"
#include "call_recording_provider.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "teardown_trace_capture.h"

using dsd_qt::CallHistoryFilterModel;
using dsd_qt::CallHistoryModel;

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
        expect("tone is available in history",
               model.data(model.index(0), CallHistoryModel::ReceivedToneRole) == "141.3");
    }
    {
        CallHistoryModel model;
        expect("service rows persist separately", model.count() == 3);
        expect("service and tone survive reload",
               model.data(model.index(0), CallHistoryModel::ServiceNameRole) == "20R Traveler"
                   && model.data(model.index(0), CallHistoryModel::ReceivedToneRole) == "141.3");
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

/* Issue #637: two anonymous transmissions (no target, no source: conventional NFM) heard on different frequencies a
   second apart are two calls; on the same frequency they still fold. */
void
test_anonymous_rows_on_different_frequencies_stay_separate(void) {
    const time_t when = kCaptureStart + 400;
    resetStorage();
    RingFixture ring;
    CallHistoryModel model;
    auto nfm = [&ring](time_t start, time_t end, int64_t freq_hz) {
        Event_History* item = ring.commit(0, 0, 0, start, end);
        item->systype = DSD_SYNC_ANALOG;
        item->receive_identity = {0, DSD_RECEIVE_NONE, 1, 0, 0};
        RingFixture::carrier(item, freq_hz);
    };
    nfm(when, when + 4, 154430000);
    model.refresh(ring.state);
    nfm(when + 5, when + 9, 155010000);
    model.refresh(ring.state);
    expect("anonymous transmissions on two frequencies stay two rows", model.count() == 2);
    nfm(when + 10, when + 14, 155010000);
    model.refresh(ring.state);
    expect("anonymous fragments on one frequency still fold", model.count() == 2);
    /* A fragment whose tone was still acquiring ("NFM") folds into the named one that confirmed it on the same
       frequency, and the row takes the service's name. */
    Event_History* acquiring = ring.commit(0, 0, 0, when + 40, when + 44);
    acquiring->systype = DSD_SYNC_ANALOG;
    acquiring->receive_identity = {0, DSD_RECEIVE_NONE, 0, 0, 0};
    RingFixture::carrier(acquiring, 156000000);
    model.refresh(ring.state);
    expect("an unconfirmed fragment is a row of its own first", model.count() == 3);
    Event_History* toned = ring.commit(0, 0, 0, when + 45, when + 49);
    toned->systype = DSD_SYNC_ANALOG;
    toned->receive_identity = {1413, DSD_RECEIVE_CTCSS, 1, 0, 0};
    DSD_SNPRINTF(toned->service_label, sizeof toned->service_label, "%s", "Traveler");
    RingFixture::carrier(toned, 156000000);
    model.refresh(ring.state);
    expect("a confirmed tone folds into its unconfirmed fragment", model.count() == 3);
    expect("the folded row is named by the confirmed service",
           model.data(model.index(0), CallHistoryModel::NameRole) == "Traveler");
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
    test_target_ids_read_as_their_names();
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
    test_receive_service_identity();
    test_slot_proof_merges_independently_of_code();
    test_receive_only_updates_and_label_renames();
    test_unconfirmed_fragment_joins_named_service();
    test_anonymous_rows_on_different_frequencies_stay_separate();
    test_search_by_frequency();
    test_search_matches_what_the_row_shows();
    test_frequency_search_follows_the_row();
    test_stores_serialise_off_the_gui_thread();
    test_save_delay_follows_the_bytes_written();
    test_save_now_writes_at_once();
    test_save_now_mid_write_queues_the_next_save();
    test_leaving_the_screen_saves();
    test_destructor_marks_its_teardown_stage();
    test_rows_have_stable_uids();
    test_fragments_follow_merges_reloads_and_trims();
    test_recording_roles_follow_the_provider();
    test_failed_save_is_kept_and_retried();
    test_restart_after_a_half_written_save();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
