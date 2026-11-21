// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Edge tests for the remembered-talkgroup classifier, and for the verified-talkgroup proof the call state carries onto
 * history rows (issue #903): set once per transmission by dsd_call_state_note_verified_target(), never inherited.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/heard_talkgroup.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

_Static_assert(sizeof(int) == sizeof(dsd_heard_tg_variant), "an unknown variant is written as an int");

static int
t(dsd_heard_tg_variant v, uint32_t id) {
    dsd_heard_tg_input in = {v, id, 1, 0, 0};
    return dsd_heard_tg_ordinary(&in);
}

static int
nx(dsd_heard_tg_variant v, uint32_t id, uint8_t ct, uint8_t inter) {
    dsd_heard_tg_input in = {v, id, ct, inter, 0};
    return dsd_heard_tg_ordinary(&in);
}

static void
test_classifier(void) {
    assert(dsd_heard_tg_ordinary(0) == 0);

    assert(t(DSD_HEARD_TG_P25, 0) == 0);
    assert(t(DSD_HEARD_TG_P25, 1) == 1);
    assert(t(DSD_HEARD_TG_P25, 0xFFFE) == 1);
    assert(t(DSD_HEARD_TG_P25, 0xFFFF) == 0);
    {
        dsd_heard_tg_input in = {DSD_HEARD_TG_P25, 100, 0, 0, 1};
        assert(dsd_heard_tg_ordinary(&in) == 0);
    }

    assert(t(DSD_HEARD_TG_DMR, 0) == 0);
    assert(t(DSD_HEARD_TG_DMR, 1) == 1);
    assert(t(DSD_HEARD_TG_DMR, 0xFFFCDF) == 1);
    assert(t(DSD_HEARD_TG_DMR, 0xFFFCE0) == 0);
    assert(t(DSD_HEARD_TG_DMR, 0xFFFFFF) == 0);

    assert(t(DSD_HEARD_TG_CAPPLUS, 0) == 0);
    assert(t(DSD_HEARD_TG_CAPPLUS, 1) == 1);
    assert(t(DSD_HEARD_TG_CAPPLUS, 254) == 1);
    assert(t(DSD_HEARD_TG_CAPPLUS, 255) == 0);

    assert(t(DSD_HEARD_TG_CONPLUS, 0) == 0);
    assert(t(DSD_HEARD_TG_CONPLUS, 1) == 1);
    assert(t(DSD_HEARD_TG_CONPLUS, 0xFFFC9F) == 1);
    assert(t(DSD_HEARD_TG_CONPLUS, 0xFFFCA0) == 0);
    assert(t(DSD_HEARD_TG_CONPLUS, 0xFFFFFF) == 0);

    assert(t(DSD_HEARD_TG_XPT, 0) == 0);
    assert(t(DSD_HEARD_TG_XPT, 1) == 1);
    assert(t(DSD_HEARD_TG_XPT, 240) == 1);
    assert(t(DSD_HEARD_TG_XPT, 241) == 0);
    assert(t(DSD_HEARD_TG_XPT, 250) == 0);
    assert(t(DSD_HEARD_TG_XPT, 255) == 0);

    assert(nx(DSD_HEARD_TG_NXDN_C, 0, 1, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 1, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_C, 0xFFEF, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_C, 0xFFF0, 1, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 0xFFFF, 1, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 100, 0, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_C, 100, 4, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 100, 2, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_C, 100, 1, 1) == 0);

    assert(nx(DSD_HEARD_TG_NXDN_CONV, 100, 0, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 100, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 100, 4, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 0, 1, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 0xFFEF, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_CONV, 0xFFF0, 1, 0) == 0);

    assert(t(DSD_HEARD_TG_NXDN_D, (0u << 11) | 1u) == 1);
    assert(t(DSD_HEARD_TG_NXDN_D, (0u << 11) | 0u) == 0);
    assert(t(DSD_HEARD_TG_NXDN_D, (30u << 11) | 2000u) == 1);
    assert(t(DSD_HEARD_TG_NXDN_D, (30u << 11) | 2001u) == 0);
    assert(t(DSD_HEARD_TG_NXDN_D, (30u << 11) | 2047u) == 0);
    assert(t(DSD_HEARD_TG_NXDN_D, (31u << 11) | 1u) == 0);
    assert(t(DSD_HEARD_TG_NXDN_D, (31u << 11) | 2000u) == 0);
    /* Type-D call types (TS 1-E Table 6.6-15): only 000 Broadcast and 001 Conference name a group. */
    assert(nx(DSD_HEARD_TG_NXDN_D, (3u << 11) | 5u, 0, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_D, (3u << 11) | 5u, 1, 0) == 1);
    assert(nx(DSD_HEARD_TG_NXDN_D, (3u << 11) | 5u, 2, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_D, (3u << 11) | 5u, 3, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_D, (3u << 11) | 5u, 4, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_D, (3u << 11) | 5u, 6, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_D, (3u << 11) | 5u, 7, 0) == 0);
    assert(nx(DSD_HEARD_TG_NXDN_D, (31u << 11) | 5u, 1, 0) == 0);

    assert(t(DSD_HEARD_TG_TETRA, 0) == 0);
    assert(t(DSD_HEARD_TG_TETRA, 1) == 1);
    assert(t(DSD_HEARD_TG_TETRA, 0xFFFFFE) == 1);
    assert(t(DSD_HEARD_TG_TETRA, 0xFFFFFF) == 0);

    assert(t(DSD_HEARD_TG_NONE, 1) == 0);
    assert(t(DSD_HEARD_TG_NONE, 100) == 0);
    {
        /* A variant no build of this classifier knows (a later persisted value): its bytes, not a cast to the enum. */
        dsd_heard_tg_input in = {DSD_HEARD_TG_NONE, 100, 1, 0, 0};
        const int unknown = 99;
        DSD_MEMCPY(&in.variant, &unknown, sizeof in.variant);
        assert(dsd_heard_tg_ordinary(&in) == 0);
    }
}

/* ---- Verified-talkgroup proof on calls and history rows ---- */

static double g_now_m = 1.0;

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    Event_History_I* history;
} proof_fixture;

static void
fixture_open(proof_fixture* f, int audio_in_type) {
    f->opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    f->state = (dsd_state*)calloc(1, sizeof(dsd_state));
    f->history = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    assert(f->opts && f->state && f->history);
    f->opts->audio_in_type = audio_in_type;
    f->state->event_history_s = f->history;
    init_event_history(&f->history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&f->history[1], 0, DSD_EVENT_HISTORY_LEN);
    g_now_m = 1.0;
}

static void
fixture_close(proof_fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->history);
    free(f->state);
    free(f->opts);
}

static int
observe_as(proof_fixture* f, int protocol, uint64_t tg, uint64_t src, dsd_call_boundary boundary) {
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = tg,
        .policy_target_id = tg,
        .ota_source_id = src,
        .observed_m = g_now_m,
    };
    g_now_m += 0.1;
    const int rc = dsd_call_state_observe(f->state, &observation, boundary);
    dsd_event_sync_slot(f->opts, f->state, 0U);
    return rc;
}

static int
observe(proof_fixture* f, uint64_t tg, uint64_t src, dsd_call_boundary boundary) {
    return observe_as(f, DSD_SYNC_P25P1_POS, tg, src, boundary);
}

static void
end_call(proof_fixture* f, dsd_call_end_reason reason) {
    assert(dsd_call_state_end_ex(f->state, 0U, g_now_m, reason) > 0);
    g_now_m += 0.1;
    dsd_event_sync_slot(f->opts, f->state, 0U);
}

static dsd_call_snapshot
slot_call(const proof_fixture* f) {
    dsd_call_snapshot call;
    assert(dsd_call_state_get(f->state, 0U, &call) > 0);
    return call;
}

static void
note(proof_fixture* f, uint64_t transmission, dsd_heard_tg_variant variant, uint32_t id) {
    const dsd_heard_tg_input in = {variant, id, 0, 0, 0};
    dsd_call_state_note_verified_target(f->opts, f->state, 0U, transmission, &in);
}

static const Event_History*
row(const proof_fixture* f, int index) {
    return &f->history[0].Event_History_Items[index];
}

/* A checked message proves the open transmission's group once: the classifier runs on the first note only, repeated
   notes and staged-row rebuilds leave it alone, and the frozen proof reaches the row and its commit. */
static void
test_note_sets_once(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot call = slot_call(&f);
    assert(call.heard_variant == DSD_HEARD_TG_NONE && call.heard_id == 0U);
    assert(row(&f, 0)->heard_variant == DSD_HEARD_TG_NONE);

    const long before = dsd_heard_tg_test_eval_count();
    const uint64_t transmission = dsd_call_snapshot_transmission(&call);
    note(&f, transmission, DSD_HEARD_TG_P25, 100U);
    assert(dsd_heard_tg_test_eval_count() == before + 1);
    call = slot_call(&f);
    assert(call.heard_variant == DSD_HEARD_TG_P25 && call.heard_id == 100U);
    /* Written to the staged row at once, so a commit before the next render still carries it. */
    assert(row(&f, 0)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 0)->heard_id == 100U);

    for (int i = 0; i < 5; i++) {
        note(&f, transmission, DSD_HEARD_TG_P25, 4242U); /* a later checked message: frozen, never re-classified */
        assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 0);
        dsd_event_sync_slot(f.opts, f.state, 0U);
    }
    assert(dsd_heard_tg_test_eval_count() == before + 1);
    call = slot_call(&f);
    assert(call.heard_variant == DSD_HEARD_TG_P25 && call.heard_id == 100U);
    assert(row(&f, 0)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 0)->heard_id == 100U);
    assert(row(&f, 0)->live_input == 1U);

    end_call(&f, DSD_CALL_END_TERMINATOR);
    assert(dsd_heard_tg_test_eval_count() == before + 1);
    assert(row(&f, 1)->event_string[0] != '\0');
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->heard_id == 100U);
    assert(row(&f, 1)->live_input == 1U);
    /* An ended transmission takes no note. */
    note(&f, transmission, DSD_HEARD_TG_P25, 4242U);
    assert(dsd_heard_tg_test_eval_count() == before + 1);

    /* A later CRC failure on the transmission marks the committed row and leaves its proof (fragment trust). */
    f.state->event_crc_invalid[0] = DSD_CRC_FAILED;
    assert(dsd_event_enrich_alias(f.state, 0U, DSD_SYNC_P25P1_POS, call.epoch, "LATE") == 1);
    f.state->event_crc_invalid[0] = 0U;
    assert(dsd_crc_is_failed(row(&f, 1)->crc_invalid));
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->heard_id == 100U);

    /* A row that moves deeper keeps it; a reset clears it. */
    push_event_history(&f.history[0]);
    assert(row(&f, 2)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 2)->heard_id == 100U && row(&f, 2)->live_input);
    init_event_history(&f.history[0], 0, DSD_EVENT_HISTORY_LEN);
    assert(row(&f, 2)->heard_variant == DSD_HEARD_TG_NONE && row(&f, 2)->heard_id == 0U && !row(&f, 2)->live_input);
    fixture_close(&f);
}

/* A note for another transmission, or for no call, does nothing; a new epoch starts with no proof. */
static void
test_stale_epoch_and_new_epoch(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    const long before = dsd_heard_tg_test_eval_count();
    note(&f, 1U, DSD_HEARD_TG_P25, 100U); /* no call state yet */
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_snapshot first = slot_call(&f);
    const uint64_t first_tx = dsd_call_snapshot_transmission(&first);
    note(&f, first_tx + 1U, DSD_HEARD_TG_P25, 100U);
    note(&f, 0U, DSD_HEARD_TG_P25, 100U);
    dsd_call_state_note_verified_target(f.opts, f.state, 0U, first_tx, NULL);
    dsd_call_state_note_verified_target(f.opts, NULL, 0U, first_tx, NULL);
    {
        const dsd_heard_tg_input in = {DSD_HEARD_TG_P25, 100U, 0, 0, 0};
        dsd_call_state_note_verified_target(f.opts, f.state, DSD_CALL_STATE_SLOT_COUNT, first_tx, &in);
    }
    assert(dsd_heard_tg_test_eval_count() == before);
    assert(slot_call(&f).heard_variant == DSD_HEARD_TG_NONE);
    note(&f, first_tx, DSD_HEARD_TG_P25, 100U);
    assert(slot_call(&f).heard_variant == DSD_HEARD_TG_P25);

    /* Another call takes the slot: its epoch has no proof, its row none, and the old transmission's note misses it. */
    assert(observe(&f, 101U, 201U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_snapshot second = slot_call(&f);
    assert(second.epoch != first.epoch);
    assert(second.heard_variant == DSD_HEARD_TG_NONE && second.heard_id == 0U);
    assert(row(&f, 0)->heard_variant == DSD_HEARD_TG_NONE && row(&f, 0)->heard_id == 0U);
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->heard_id == 100U);
    const long mid = dsd_heard_tg_test_eval_count();
    note(&f, first_tx, DSD_HEARD_TG_P25, 100U);
    assert(dsd_heard_tg_test_eval_count() == mid);
    assert(slot_call(&f).heard_variant == DSD_HEARD_TG_NONE);
    fixture_close(&f);
}

/* An id the classifier refuses freezes "none" for the transmission: a later note does not try again. */
static void
test_refused_id_freezes_none(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 0xFFFFU, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_snapshot call = slot_call(&f);
    const uint64_t tx = dsd_call_snapshot_transmission(&call);
    const long before = dsd_heard_tg_test_eval_count();
    note(&f, tx, DSD_HEARD_TG_P25, 0xFFFFU);
    note(&f, tx, DSD_HEARD_TG_P25, 100U);
    assert(dsd_heard_tg_test_eval_count() == before + 1);
    assert(slot_call(&f).heard_variant == DSD_HEARD_TG_NONE && slot_call(&f).heard_id == 0U);
    end_call(&f, DSD_CALL_END_TERMINATOR);
    assert(row(&f, 1)->event_string[0] != '\0' && row(&f, 1)->heard_variant == DSD_HEARD_TG_NONE);
    fixture_close(&f);
}

/* A reacquired epoch inherits no proof; the merged row keeps the first segment's, or takes the second's. */
static void
test_reacquisition(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_snapshot first = slot_call(&f);
    note(&f, dsd_call_snapshot_transmission(&first), DSD_HEARD_TG_P25, 100U);
    end_call(&f, DSD_CALL_END_SYNC_LOSS);
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    const dsd_call_snapshot second = slot_call(&f);
    assert(second.epoch != first.epoch);
    assert(dsd_call_snapshot_transmission(&second) == dsd_call_snapshot_transmission(&first));
    assert(second.heard_variant == DSD_HEARD_TG_NONE && second.heard_id == 0U);
    assert(row(&f, 0)->heard_variant == DSD_HEARD_TG_NONE);
    end_call(&f, DSD_CALL_END_TERMINATOR);
    assert(row(&f, 2)->event_string[0] == '\0'); /* one row: merged */
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->heard_id == 100U);
    fixture_close(&f);

    /* The first segment proved nothing; the reacquired one's own checked message fills the merged row. */
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_snapshot faded = slot_call(&f);
    end_call(&f, DSD_CALL_END_SYNC_LOSS);
    /* The faded segment is no longer open: a note for it does nothing, to the call or its row. */
    const long before = dsd_heard_tg_test_eval_count();
    note(&f, dsd_call_snapshot_transmission(&faded), DSD_HEARD_TG_P25, 100U);
    assert(dsd_heard_tg_test_eval_count() == before);
    assert(slot_call(&f).heard_variant == DSD_HEARD_TG_NONE && slot_call(&f).heard_noted == 0U);
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_NONE);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    const dsd_call_snapshot healed = slot_call(&f);
    note(&f, dsd_call_snapshot_transmission(&healed), DSD_HEARD_TG_P25, 100U);
    assert(slot_call(&f).heard_variant == DSD_HEARD_TG_P25);
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_NONE); /* the first epoch's row is not this epoch's */
    end_call(&f, DSD_CALL_END_TERMINATOR);
    assert(row(&f, 2)->event_string[0] == '\0');
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->heard_id == 100U);
    fixture_close(&f);
}

/* A row the epoch already committed mid-call (a call notice) takes a later proof in place, as a revision. */
static void
test_committed_row_revision(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot call = slot_call(&f);
    assert(dsd_event_emit_call_notice_nonfinalizing(f.opts, f.state, 0U, &call, "NOTICE") == 1);
    assert(row(&f, 1)->event_string[0] != '\0' && row(&f, 1)->heard_variant == DSD_HEARD_TG_NONE);
    const uint64_t commit_rev = f.history[0].commit_rev;
    note(&f, dsd_call_snapshot_transmission(&call), DSD_HEARD_TG_P25, 100U);
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->heard_id == 100U);
    assert(f.history[0].commit_rev != commit_rev);
    fixture_close(&f);
}

/* A notice for a call that is not the slot's canonical one (a grant's synthetic snapshot) never carries a proof, even
   one the snapshot claims, and leaves the canonical call's staged row as it was. */
static void
test_noncanonical_notice_carries_no_proof(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot call = slot_call(&f);
    note(&f, dsd_call_snapshot_transmission(&call), DSD_HEARD_TG_P25, 100U);
    dsd_call_snapshot grant = slot_call(&f);
    assert(grant.heard_variant == DSD_HEARD_TG_P25);
    grant.epoch += 1000U;
    grant.transmission_epoch = grant.epoch;
    grant.ota_target_id = 300U;
    grant.policy_target_id = 300U;
    assert(dsd_event_emit_call_notice(f.opts, f.state, 0U, &grant, "GRANT") == 1);
    assert(row(&f, 1)->event_string[0] != '\0' && row(&f, 1)->target_id == 300U);
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_NONE && row(&f, 1)->heard_id == 0U);
    assert(row(&f, 0)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 0)->heard_id == 100U);
    fixture_close(&f);
}

/* live_input: stamped when the row begins, from the input in force then. */
static void
test_live_input(void) {
    static const struct {
        int type;
        const char* dev;
        int playfiles;
        int live;
    } k_inputs[] = {
        {AUDIO_IN_RTL, "", 0, 1},
        {AUDIO_IN_RTL, "rtltcp:127.0.0.1:1234", 0, 1},
        {AUDIO_IN_RTL, "iqtcp:127.0.0.1:1", 0, 1},
        {AUDIO_IN_RTL, "iqreplay:cap.iq", 0, 0},
        {AUDIO_IN_PULSE, "", 0, 1},
        {AUDIO_IN_UDP, "", 0, 1},
        {AUDIO_IN_TCP, "", 0, 1},
        {AUDIO_IN_WAV, "", 0, 0},
        {AUDIO_IN_STDIN, "", 0, 0},
        {AUDIO_IN_SYMBOL_BIN, "", 0, 0},
        {AUDIO_IN_SYMBOL_FLT, "", 0, 0},
        {AUDIO_IN_NULL, "", 0, 0},
        {AUDIO_IN_RTL, "", 1, 0},
    };

    assert(dsd_opts_input_is_live(NULL) == 0);
    for (size_t i = 0; i < sizeof k_inputs / sizeof k_inputs[0]; i++) {
        proof_fixture f;
        fixture_open(&f, k_inputs[i].type);
        DSD_SNPRINTF(f.opts->audio_in_dev, sizeof f.opts->audio_in_dev, "%s", k_inputs[i].dev);
        f.opts->playfiles = k_inputs[i].playfiles;
        assert(dsd_opts_input_is_live(f.opts) == k_inputs[i].live);
        assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
        assert(row(&f, 0)->live_input == (uint8_t)k_inputs[i].live);
        fixture_close(&f);
    }

    /* live_input is a latch (spec 4.1: a running live-to-file switch is covered): a row is live only while the input
       was live when it began and at every render and commit since. A row begun on a file stays not live after a
       switch to a radio. */
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_WAV);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    f.opts->audio_in_type = AUDIO_IN_RTL;
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(row(&f, 0)->live_input == 0U);
    end_call(&f, DSD_CALL_END_TERMINATOR);
    assert(row(&f, 1)->event_string[0] != '\0' && row(&f, 1)->live_input == 0U);
    /* A row begun live and switched to a symbol replay before its end commits not live. */
    assert(observe(&f, 101U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(row(&f, 0)->live_input == 1U);
    f.opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    assert(observe(&f, 101U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(row(&f, 0)->live_input == 0U); /* the render closes the latch */
    f.opts->audio_in_type = AUDIO_IN_RTL; /* and a switch back to the radio does not reopen it */
    assert(observe(&f, 101U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    end_call(&f, DSD_CALL_END_TERMINATOR);
    assert(row(&f, 1)->live_input == 0U && row(&f, 2)->live_input == 0U);
    fixture_close(&f);

    /* The epoch-change commit renders nothing: the commit itself reads the input. */
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_snapshot first = slot_call(&f);
    note(&f, dsd_call_snapshot_transmission(&first), DSD_HEARD_TG_P25, 100U);
    assert(row(&f, 0)->live_input == 1U && row(&f, 0)->heard_variant == DSD_HEARD_TG_P25);
    f.opts->audio_in_type = AUDIO_IN_WAV;
    assert(observe(&f, 101U, 201U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->live_input == 0U);
    fixture_close(&f);
}

/* A reacquisition merge ANDs the two segments' live input: a live segment healed from a file is not live. */
static void
test_live_input_merge(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    end_call(&f, DSD_CALL_END_SYNC_LOSS);
    assert(row(&f, 1)->live_input == 1U && row(&f, 1)->heard_variant == DSD_HEARD_TG_NONE);
    f.opts->audio_in_type = AUDIO_IN_WAV;
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    const dsd_call_snapshot healed = slot_call(&f);
    note(&f, dsd_call_snapshot_transmission(&healed), DSD_HEARD_TG_P25, 100U);
    assert(row(&f, 0)->live_input == 0U);
    f.opts->audio_in_type = AUDIO_IN_RTL; /* back on a radio before the end: the latch stays closed */
    end_call(&f, DSD_CALL_END_TERMINATOR);
    assert(row(&f, 2)->event_string[0] == '\0'); /* one row: merged */
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->live_input == 0U);
    fixture_close(&f);

    /* Both segments live: the merged row stays live. */
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    end_call(&f, DSD_CALL_END_SYNC_LOSS);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    end_call(&f, DSD_CALL_END_TERMINATOR);
    assert(row(&f, 2)->event_string[0] == '\0' && row(&f, 1)->live_input == 1U);
    fixture_close(&f);
}

/* A proof that revises an already committed row while the input is not live clears that row's live input. */
static void
test_live_input_committed_revision(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot call = slot_call(&f);
    assert(dsd_event_emit_call_notice_nonfinalizing(f.opts, f.state, 0U, &call, "NOTICE") == 1);
    assert(row(&f, 1)->live_input == 1U);
    f.opts->audio_in_type = AUDIO_IN_WAV;
    assert(observe(&f, 100U, 200U, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    note(&f, dsd_call_snapshot_transmission(&call), DSD_HEARD_TG_P25, 100U);
    assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_P25 && row(&f, 1)->live_input == 0U);
    fixture_close(&f);
}

/* ---- Receive filters (issue #903): traffic a filter says is not this system proves no talkgroup ---- */

/* One access-code filter and the protocol it judges: the code the carrier is decoded with (@ref set_code, known or
   not yet verified), how the filter spells it, and how it spells another code. */
typedef struct {
    int protocol;
    int kind;
    const char* code;
    const char* other;
    void (*set_code)(dsd_state* state, int known);
    void (*set_other)(dsd_state* state);
    dsd_heard_tg_variant variant;
} receive_case;

static void
set_p25p1_nac(dsd_state* state, int known) {
    state->nac = known ? 0x293 : 0;
}

static void
set_p25p2_nac(dsd_state* state, int known) {
    state->p2_proven_nac = known ? 0x293 : 0;
}

static void
set_dmr_cc(dsd_state* state, int known) {
    state->dmr_color_code = known ? 5U : 16U;
}

static void
set_nxdn_ran(dsd_state* state, int known) {
    state->nxdn_last_ran = known ? 23U : 64U;
    state->nxdn_last_ran_stand_in = 0U;
}

/* The other code each case's filter spells (@ref receive_case::other), verified on the carrier. */
static void
set_p25p1_other(dsd_state* state) {
    state->nac = 0x294;
}

static void
set_p25p2_other(dsd_state* state) {
    state->p2_proven_nac = 0x294;
}

static void
set_dmr_other(dsd_state* state) {
    state->dmr_color_code = 6U;
}

static void
set_nxdn_other(dsd_state* state) {
    state->nxdn_last_ran = 24U;
    state->nxdn_last_ran_stand_in = 0U;
}

/* P25 Phase 1 NAC, P25 Phase 2 NAC (the carrier's proven one), DMR colour code, NXDN RAN. */
static const receive_case k_receive_cases[] = {
    {DSD_SYNC_P25P1_POS, DSD_RECEIVE_NAC, "293", "294", set_p25p1_nac, set_p25p1_other, DSD_HEARD_TG_P25},
    {DSD_SYNC_P25P2_POS, DSD_RECEIVE_NAC, "293", "294", set_p25p2_nac, set_p25p2_other, DSD_HEARD_TG_P25},
    {DSD_SYNC_DMR_BS_VOICE_POS, DSD_RECEIVE_DCC, "5", "6", set_dmr_cc, set_dmr_other, DSD_HEARD_TG_DMR},
    {DSD_SYNC_NXDN_POS, DSD_RECEIVE_RAN, "23", "24", set_nxdn_ran, set_nxdn_other, DSD_HEARD_TG_NXDN_C},
};

static void
set_filter(proof_fixture* f, int kind, const char* allow, const char* block) {
    assert(dsd_receive_edit_parse(&f->opts->receive, kind, 1, allow, block, "", NULL, 0) == 0);
}

/* The call of @p rc's protocol on slot 0, begun with the carrier's code known or not, and its transmission. */
static uint64_t
receive_call_begin(proof_fixture* f, const receive_case* rc, int known) {
    rc->set_code(f->state, known);
    assert(observe_as(f, rc->protocol, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_snapshot call = slot_call(f);
    return dsd_call_snapshot_transmission(&call);
}

static void
expect_not_noted(const proof_fixture* f, long evals_before) {
    const dsd_call_snapshot call = slot_call(f);
    assert(dsd_heard_tg_test_eval_count() == evals_before);
    assert(call.heard_noted == 0U && call.heard_variant == DSD_HEARD_TG_NONE && call.heard_id == 0U);
    assert(row(f, 0)->heard_variant == DSD_HEARD_TG_NONE && row(f, 0)->heard_id == 0U);
}

static void
expect_noted(const proof_fixture* f, const receive_case* rc) {
    const dsd_call_snapshot call = slot_call(f);
    assert(call.heard_noted == 1U && call.heard_variant == (uint8_t)rc->variant && call.heard_id == 100U);
}

/* A filter that rejects the call's code, by its block list or by an allow list without it: the note does nothing, and
   the call stays open to a later note. The same call with the filter off, allowing its code, or blocking only another
   code is noted. */
static void
test_receive_filter_rejects(void) {
    for (size_t i = 0; i < sizeof k_receive_cases / sizeof k_receive_cases[0]; i++) {
        const receive_case* rc = &k_receive_cases[i];
        for (int variant = 0; variant < 2; variant++) {
            proof_fixture f;
            fixture_open(&f, AUDIO_IN_RTL);
            set_filter(&f, rc->kind, variant == 0 ? "" : rc->other, variant == 0 ? rc->code : "");
            const uint64_t tx = receive_call_begin(&f, rc, 1);
            const long before = dsd_heard_tg_test_eval_count();
            note(&f, tx, rc->variant, 100U);
            note(&f, tx, rc->variant, 100U);
            expect_not_noted(&f, before);
            end_call(&f, DSD_CALL_END_TERMINATOR);
            assert(row(&f, 1)->heard_variant == DSD_HEARD_TG_NONE && row(&f, 1)->heard_id == 0U);
            fixture_close(&f);
        }

        static const struct {
            int enabled;
            int allow; /* 1: allow the call's code; 0: block another code */
        } k_admitting[] = {{0, 0}, {1, 1}, {1, 0}};

        for (size_t j = 0; j < sizeof k_admitting / sizeof k_admitting[0]; j++) {
            proof_fixture f;
            fixture_open(&f, AUDIO_IN_RTL);
            if (k_admitting[j].enabled) {
                set_filter(&f, rc->kind, k_admitting[j].allow ? rc->code : "", k_admitting[j].allow ? "" : rc->other);
            }
            const uint64_t tx = receive_call_begin(&f, rc, 1);
            note(&f, tx, rc->variant, 100U);
            expect_noted(&f, rc);
            fixture_close(&f);
        }
    }
}

/* A filter still checking (its code not verified yet on the carrier) admits nothing yet: the note does nothing and
   leaves the call open, so a checked message after the code is verified and admitted proves it, and one after a code
   the filter rejects does not. */
static void
test_receive_filter_checking(void) {
    for (size_t i = 0; i < sizeof k_receive_cases / sizeof k_receive_cases[0]; i++) {
        const receive_case* rc = &k_receive_cases[i];
        for (int admitted = 0; admitted < 2; admitted++) {
            proof_fixture f;
            fixture_open(&f, AUDIO_IN_RTL);
            set_filter(&f, rc->kind, admitted ? rc->code : rc->other, "");
            const uint64_t tx = receive_call_begin(&f, rc, 0);
            const long before = dsd_heard_tg_test_eval_count();
            note(&f, tx, rc->variant, 100U);
            expect_not_noted(&f, before);
            rc->set_code(f.state, 1);
            note(&f, tx, rc->variant, 100U);
            if (admitted) {
                expect_noted(&f, rc);
                assert(row(&f, 0)->heard_variant == (uint8_t)rc->variant && row(&f, 0)->heard_id == 100U);
            } else {
                expect_not_noted(&f, before);
            }
            fixture_close(&f);
        }
    }
}

/* A call admitted when it began that a policy edit rejects part-way: a note after the rejection does nothing (fail
   closed), while a proof noted before it stays on the call and its row (fragment trust). */
static void
test_receive_filter_rejects_midcall(void) {
    for (size_t i = 0; i < sizeof k_receive_cases / sizeof k_receive_cases[0]; i++) {
        const receive_case* rc = &k_receive_cases[i];
        proof_fixture f;
        fixture_open(&f, AUDIO_IN_RTL);
        uint64_t tx = receive_call_begin(&f, rc, 1);
        set_filter(&f, rc->kind, "", rc->code);
        const long before = dsd_heard_tg_test_eval_count();
        note(&f, tx, rc->variant, 100U);
        expect_not_noted(&f, before);
        fixture_close(&f);

        fixture_open(&f, AUDIO_IN_RTL);
        tx = receive_call_begin(&f, rc, 1);
        note(&f, tx, rc->variant, 100U);
        expect_noted(&f, rc);
        assert(row(&f, 0)->heard_variant == (uint8_t)rc->variant);
        set_filter(&f, rc->kind, "", rc->code);
        note(&f, tx, rc->variant, 100U);
        expect_noted(&f, rc);
        assert(row(&f, 0)->heard_variant == (uint8_t)rc->variant && row(&f, 0)->heard_id == 100U);
        end_call(&f, DSD_CALL_END_TERMINATOR);
        assert(row(&f, 1)->heard_variant == (uint8_t)rc->variant && row(&f, 1)->heard_id == 100U);
        fixture_close(&f);
    }
}

/* The carrier verified another code since the call began (its service changed; the receive check would end the call
   at its next refresh): even a code the filter allows proves nothing for the call, which was received with the first
   one. With no filter on the policy is not asked. */
static void
test_receive_filter_code_changed(void) {
    for (size_t i = 0; i < sizeof k_receive_cases / sizeof k_receive_cases[0]; i++) {
        const receive_case* rc = &k_receive_cases[i];
        char both[16];
        DSD_SNPRINTF(both, sizeof both, "%s/%s", rc->code, rc->other);
        proof_fixture f;
        fixture_open(&f, AUDIO_IN_RTL);
        set_filter(&f, rc->kind, both, "");
        const uint64_t tx = receive_call_begin(&f, rc, 1);
        rc->set_other(f.state);
        const long before = dsd_heard_tg_test_eval_count();
        note(&f, tx, rc->variant, 100U);
        expect_not_noted(&f, before);
        fixture_close(&f);
    }
}

/* Only a filter that says the traffic is not this system counts. A slot filter chooses a timeslot of the same system,
   so a talkgroup heard on a slot it rejects is still the system's; another protocol's access-code filter does not judge
   this call; and TETRA has no receive filter at all. */
static void
test_receive_filter_scope(void) {
    proof_fixture f;
    fixture_open(&f, AUDIO_IN_RTL);
    assert(dsd_receive_edit_parse(&f.opts->receive, DSD_RECEIVE_SLOT, 1, "", "1", "", NULL, 0) == 0);
    uint64_t tx = receive_call_begin(&f, &k_receive_cases[2], 1); /* DMR, lane 0: over-the-air slot 1 */
    note(&f, tx, DSD_HEARD_TG_DMR, 100U);
    expect_noted(&f, &k_receive_cases[2]);
    fixture_close(&f);

    fixture_open(&f, AUDIO_IN_RTL);
    set_filter(&f, DSD_RECEIVE_DCC, "", "5");
    set_filter(&f, DSD_RECEIVE_RAN, "", "23");
    tx = receive_call_begin(&f, &k_receive_cases[0], 1); /* P25 Phase 1, NAC 293: no NAC filter */
    note(&f, tx, DSD_HEARD_TG_P25, 100U);
    expect_noted(&f, &k_receive_cases[0]);
    fixture_close(&f);

    fixture_open(&f, AUDIO_IN_RTL);
    set_filter(&f, DSD_RECEIVE_DCC, "", "0/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15");
    set_filter(&f, DSD_RECEIVE_NAC, "", "293");
    set_filter(&f, DSD_RECEIVE_RAN, "", "0");
    assert(observe_as(&f, DSD_SYNC_TETRA_NDB_POS, 100U, 200U, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_snapshot call = slot_call(&f);
    note(&f, dsd_call_snapshot_transmission(&call), DSD_HEARD_TG_TETRA, 100U);
    assert(slot_call(&f).heard_variant == DSD_HEARD_TG_TETRA && slot_call(&f).heard_id == 100U);
    fixture_close(&f);
}

int
main(void) {
    test_classifier();
    test_note_sets_once();
    test_stale_epoch_and_new_epoch();
    test_refused_id_freezes_none();
    test_reacquisition();
    test_committed_row_revision();
    test_noncanonical_notice_carries_no_proof();
    test_live_input();
    test_live_input_merge();
    test_live_input_committed_revision();
    test_receive_filter_rejects();
    test_receive_filter_checking();
    test_receive_filter_rejects_midcall();
    test_receive_filter_code_changed();
    test_receive_filter_scope();
    return 0;
}
