// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P1 LCW gating tests: verify Packet (0x10) and Encrypted (0x40)
 * service options block tuning via the trunk SM when tuning policies are
 * disabled.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Strong stub to capture VC tuning attempts from the SM path
static int g_tunes = 0;
static int g_returns = 0;

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    g_tunes++;
    opts->trunk_is_tuned = 1;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

// No-op stubs to satisfy link of LCW path helpers

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_returns++;
    opts->trunk_is_tuned = 0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = trunk_tune_to_freq;
    hooks.return_to_cc_request = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

// No-op alias/GPS helpers for LCW
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[], const dsd_gps_report_ctx* ctx) {
    (void)opts;
    (void)state;
    (void)lc_bits;
    (void)ctx;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* body, const dsd_gps_report_ctx* ctx) {
    (void)opts;
    (void)state;
    (void)body;
    (void)ctx;
    return 0;
}

static void
set_bits_msb(uint8_t* bits, int start, int width, unsigned value) {
    for (int i = 0; i < width; i++) {
        int bit = (value >> (width - 1 - i)) & 1;
        bits[start + i] = (uint8_t)bit;
    }
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* The epoch of the call slot 0 holds: the call a TDULC that ended it speaks for (processTDULC()). */
static uint64_t
slot0_call_epoch(const dsd_state* state) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, 0U, &call) > 0 ? call.epoch : 0U;
}

static int
init_retained_p1_call(dsd_opts* opts, dsd_state* state, int tg, int src) {
    const long freq = 851125000;
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    opts->trunk_hangtime = 2.0F;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_iden_fdma[1].base_freq = 851000000 / 5;
    state->p25_iden_fdma[1].chan_type = 0;
    state->p25_iden_fdma[1].chan_spac = 100;
    state->p25_iden_fdma[1].trust = 2;
    state->p25_iden_fdma[1].populated = 1;
    state->p25_chan_tdma_explicit[1] = 1;
    g_tunes = 0;
    g_returns = 0;

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    p25_sm_init_ctx(sm, opts, state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x100A, freq, tg, src, 0);
    p25_sm_event(sm, opts, state, &ev);
    if (sm->state != P25_SM_TUNED || g_tunes != 1) {
        return 0;
    }
    if (!p25_sm_emit_active_call(opts, state, 0, tg, 0, src, 1, 0)) {
        return 0;
    }
    if (!p25_sm_emit_end_call_at(opts, state, 0, tg, src, dsd_decode_now_mono_s())) {
        return 0;
    }
    return sm->state == P25_SM_TUNED && sm->slots[0].grant_active && !sm->slots[0].voice_active;
}

static int
test_mfid90_regroup_reassigns_retained_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    int rc = 0;

    rc |= expect_eq_int("MFID90 retained fixture", init_retained_p1_call(&opts, &state, 0x1234, 0x010203), 1);
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits_msb(lcw, 0, 8, 0x00);
    set_bits_msb(lcw, 8, 8, 0x90);
    set_bits_msb(lcw, 32, 16, 0x3456);
    set_bits_msb(lcw, 48, 24, 0x040506);

    p25_lcw(&opts, &state, lcw, 0);

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    rc |= expect_eq_int("MFID90 regroup remains tuned", sm->state, P25_SM_TUNED);
    rc |= expect_eq_int("MFID90 regroup no retune", g_tunes, 1);
    rc |= expect_eq_int("MFID90 regroup no release", g_returns, 0);
    rc |= expect_eq_int("MFID90 regroup SM target", sm->slots[0].ota_tg, 0x3456);
    rc |= expect_eq_int("MFID90 regroup SM source", sm->slots[0].src, 0x040506);
    rc |= expect_eq_int("MFID90 regroup voice active", sm->slots[0].voice_active, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_extended_private_resolves_retained_identity(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    const int target = 0x034567;
    const int source = 0x040506;
    const int svc = 0x81;
    int rc = 0;

    rc |= expect_eq_int("extended private retained fixture", init_retained_p1_call(&opts, &state, 0x1234, 0x010203), 1);
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits_msb(lcw, 0, 8, 0x4A);
    set_bits_msb(lcw, 8, 8, (unsigned)svc);
    set_bits_msb(lcw, 16, 8, 0x40); // Reserved octet must not be treated as service options.
    set_bits_msb(lcw, 24, 24, (unsigned)target);
    set_bits_msb(lcw, 48, 24, (unsigned)source);

    p25_lcw(&opts, &state, lcw, 0);

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    rc |= expect_eq_int("extended private remains tuned", sm->state, P25_SM_TUNED);
    rc |= expect_eq_int("extended private no retune", g_tunes, 1);
    rc |= expect_eq_int("extended private no release", g_returns, 0);
    rc |= expect_eq_int("extended private identity accepted", state.p25_p1_identity_pending, 0);
    rc |= expect_eq_int("extended private SM target", sm->slots[0].dst, target);
    rc |= expect_eq_int("extended private SM source", sm->slots[0].src, source);
    rc |= expect_eq_int("extended private SM call kind", sm->slots[0].is_group, 0);
    rc |= expect_eq_int("extended private service options", state.dmr_so, svc);
    rc |= expect_eq_int("extended private SM service options", sm->slots[0].svc_bits, svc);
    dsd_call_snapshot call;
    rc |= expect_eq_int("extended private canonical", dsd_call_state_get(&state, 0U, &call), 1);
    rc |= expect_eq_int("extended private canonical target", (int)call.ota_target_id, target);
    rc |= expect_eq_int("extended private canonical source", (int)call.ota_source_id, source);
    rc |= expect_eq_int("extended private canonical kind", call.kind, DSD_CALL_KIND_PRIVATE_VOICE);
    rc |= expect_eq_int("extended private emergency classification", call.emergency, 1);
    rc |= expect_eq_int("extended private canonical service options", call.service_options, svc);
    rc |= expect_eq_int("extended private voice active", sm->slots[0].voice_active, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_extended_private_encrypted_service_lockout(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    const int target = 0x034567;
    const int source = 0x040506;
    int rc = 0;

    rc |=
        expect_eq_int("extended private encrypted fixture", init_retained_p1_call(&opts, &state, 0x1234, 0x010203), 1);
    opts.trunk_tune_enc_calls = 0;
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits_msb(lcw, 0, 8, 0x4A);
    set_bits_msb(lcw, 8, 8, 0x40);
    set_bits_msb(lcw, 16, 8, 0x00); // Reserved octet must not make the call appear clear.
    set_bits_msb(lcw, 24, 24, (unsigned)target);
    set_bits_msb(lcw, 48, 24, (unsigned)source);

    p25_lcw(&opts, &state, lcw, 0);

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    rc |= expect_eq_int("extended private encrypted remains tuned", sm->state, P25_SM_TUNED);
    rc |= expect_eq_int("extended private encrypted no release", g_returns, 0);
    rc |= expect_eq_int("extended private encrypted no follow-up retune", g_tunes, 1);
    rc |= expect_eq_int("extended private encrypted service options", state.dmr_so, 0x40);
    rc |= expect_eq_int("extended private encrypted SM service options", sm->slots[0].svc_bits, 0x40);
    rc |= expect_eq_int("extended private encrypted classification", state.p25_crypto_state[0],
                        DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq_int("extended private encrypted audio gate", state.p25_p2_audio_allowed[0], 0);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_telephone_resolves_retained_identity(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    const int target = 0x034567;
    const int timer = 125;
    int rc = 0;

    rc |= expect_eq_int("telephone retained fixture", init_retained_p1_call(&opts, &state, 0x1234, 0x010203), 1);
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits_msb(lcw, 0, 8, 0x46);
    set_bits_msb(lcw, 16, 8, 0x00);
    set_bits_msb(lcw, 32, 16, (unsigned)timer);
    set_bits_msb(lcw, 48, 24, (unsigned)target);

    p25_lcw(&opts, &state, lcw, 0);

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    rc |= expect_eq_int("telephone remains tuned", sm->state, P25_SM_TUNED);
    rc |= expect_eq_int("telephone no retune", g_tunes, 1);
    rc |= expect_eq_int("telephone no release", g_returns, 0);
    rc |= expect_eq_int("telephone identity accepted", state.p25_p1_identity_pending, 0);
    rc |= expect_eq_int("telephone SM target", sm->slots[0].dst, target);
    rc |= expect_eq_int("telephone SM source unknown", sm->slots[0].src, 0);
    rc |= expect_eq_int("telephone SM call kind", sm->slots[0].is_group, 0);
    rc |= expect_eq_int("telephone voice active", sm->slots[0].voice_active, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_telephone_after_missed_boundary_drops_old_source(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    const int old_tg = 0x1234;
    const int old_source = 0x010203;
    const int target = 0x034567;
    int rc = 0;

    rc |=
        expect_eq_int("telephone missed-boundary fixture", init_retained_p1_call(&opts, &state, old_tg, old_source), 1);
    rc |= expect_eq_int("telephone prior epoch reopened",
                        p25_sm_emit_active_call(&opts, &state, 0, old_tg, 0, old_source, 1, 0), 1);

    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits_msb(lcw, 0, 8, 0x46);
    set_bits_msb(lcw, 16, 8, 0x00);
    set_bits_msb(lcw, 32, 16, 125);
    set_bits_msb(lcw, 48, 24, (unsigned)target);
    p25_lcw(&opts, &state, lcw, 0);

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    rc |= expect_eq_int("telephone missed-boundary remains tuned", sm->state, P25_SM_TUNED);
    rc |= expect_eq_int("telephone missed-boundary no retune", g_tunes, 1);
    rc |= expect_eq_int("telephone missed-boundary no release", g_returns, 0);
    rc |= expect_eq_int("telephone missed-boundary target", sm->slots[0].dst, target);
    rc |= expect_eq_int("telephone missed-boundary source unknown", sm->slots[0].src, 0);
    dsd_call_snapshot call;
    rc |= expect_eq_int("telephone missed-boundary canonical", dsd_call_state_get(&state, 0U, &call), 1);
    rc |= expect_eq_int("telephone missed-boundary canonical source cleared", (int)call.ota_source_id, 0);
    rc |= expect_eq_int("telephone missed-boundary call kind", sm->slots[0].is_group, 0);
    rc |= expect_eq_int("telephone missed-boundary voice active", sm->slots[0].voice_active, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_rejected_lcw_stops_post_processing(int format, int mfid) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    const int old_tg = 0x1234;
    int rc = 0;

    rc |= expect_eq_int("rejected LCW retained fixture", init_retained_p1_call(&opts, &state, old_tg, 0x010203), 1);
    if (format == 0x00) {
        state.tg_hold = old_tg;
    } else {
        opts.trunk_tune_private_calls = 0;
    }
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;

    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits_msb(lcw, 0, 8, (unsigned)format);
    set_bits_msb(lcw, 8, 8, (unsigned)mfid);
    set_bits_msb(lcw, 16, 8, 0x40);
    if (format == 0x00) {
        set_bits_msb(lcw, 32, 16, 0x3456);
    } else {
        set_bits_msb(lcw, 24, 24, 0x034567);
    }
    set_bits_msb(lcw, 48, 24, 0x040506);

    p25_lcw(&opts, &state, lcw, 0);

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    rc |= expect_eq_int("rejected LCW returned to CC", sm->state, P25_SM_ON_CC);
    rc |= expect_eq_int("rejected LCW released once", g_returns, 1);
    rc |= expect_eq_int("rejected LCW crypto remains reset", state.p25_crypto_state[0], DSD_P25_CRYPTO_UNKNOWN);
    rc |= expect_eq_int("rejected LCW crypto timer remains reset", sm->slots[0].crypto_attempt_m > 0.0, 0);
    dsd_call_snapshot call;
    rc |= expect_eq_int("rejected LCW canonical retained", dsd_call_state_get(&state, 0U, &call), 1);
    rc |= expect_eq_int("rejected LCW canonical remains ended", call.phase, DSD_CALL_PHASE_ENDED);
    dsd_state_ext_free_all(&state);
    return rc;
}

#define LCGRGR_SG  1400
#define LCGRGR_SRC 0x040506

/* Group Voice Channel User (LCO 0x00): standard (MFID 00) or Motorola LCGRGR (MFID 90), whose group is the patch
   supergroup. */
static void
build_group_voice_user_lcw(uint8_t lcw[72], int mfid, int svc, int group, int source) {
    DSD_MEMSET(lcw, 0, 72);
    set_bits_msb(lcw, 0, 8, 0x00);
    set_bits_msb(lcw, 8, 8, (unsigned)mfid);
    set_bits_msb(lcw, 16, 8, (unsigned)svc);
    set_bits_msb(lcw, 32, 16, (unsigned)group);
    set_bits_msb(lcw, 48, 24, (unsigned)source);
}

static int
ga_recorded(const dsd_state* state, uint32_t rid, uint16_t tg) {
    for (size_t i = 0; i < sizeof(state->p25_ga_rid) / sizeof(state->p25_ga_rid[0]); i++) {
        if (state->p25_ga_rid[i] == rid && state->p25_ga_tg[i] == tg) {
            return 1;
        }
    }
    return 0;
}

/* A clear call on the supergroup, followed, with audio open. Its HDU's definitive ALGID 0x80 classified it CLEAR, so
   a voice user of the same identity keeps that classification (p25_voice_start_should_preserve_p1_crypto()): only the
   LC's encrypted service option can close the gate. */
static int
init_established_clear_call(dsd_opts* opts, dsd_state* state) {
    if (!init_retained_p1_call(opts, state, LCGRGR_SG, LCGRGR_SRC)) {
        return 0;
    }
    if (!p25_sm_emit_active_call(opts, state, 0, LCGRGR_SG, 0, LCGRGR_SRC, 1, 0)) {
        return 0;
    }
    state->payload_algid = 0x80;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state->p25_p2_audio_allowed[0] = 1;
    DSD_SNPRINTF(state->generic_talker_alias[0], sizeof state->generic_talker_alias[0], "%s", "OLD ALIAS");
    return 1;
}

/* An encrypted voice user on the established clear call closes the gate until the crypto is classified again. The
   standard LCO 0x00 already does (preservation); LCGRGR must do the same for a patched call. */
static int
test_group_voice_user_parity(int mfid) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    char tag[96];
    int rc = 0;

    DSD_SNPRINTF(tag, sizeof tag, "mfid %02X established clear fixture", mfid);
    rc |= expect_eq_int(tag, init_established_clear_call(&opts, &state), 1);
    build_group_voice_user_lcw(lcw, mfid, 0x40, LCGRGR_SG, LCGRGR_SRC);
    p25_lcw(&opts, &state, lcw, 0);

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    DSD_SNPRINTF(tag, sizeof tag, "mfid %02X call kept", mfid);
    rc |= expect_eq_int(tag, sm->state == P25_SM_TUNED && sm->slots[0].ota_tg == LCGRGR_SG, 1);
    DSD_SNPRINTF(tag, sizeof tag, "mfid %02X encrypted revokes clear state", mfid);
    rc |= expect_eq_int(tag, state.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    DSD_SNPRINTF(tag, sizeof tag, "mfid %02X encrypted closes audio gate", mfid);
    rc |= expect_eq_int(tag, state.p25_p2_audio_allowed[0], 0);
    DSD_SNPRINTF(tag, sizeof tag, "mfid %02X service options", mfid);
    rc |= expect_eq_int(tag, state.dmr_so, 0x40);
    DSD_SNPRINTF(tag, sizeof tag, "mfid %02X talker alias cleared", mfid);
    rc |= expect_eq_int(tag, state.generic_talker_alias[0][0], '\0');
    DSD_SNPRINTF(tag, sizeof tag, "mfid %02X group affiliation", mfid);
    rc |= expect_eq_int(tag, ga_recorded(&state, LCGRGR_SRC, LCGRGR_SG), 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* Tags slot 0's live alias "OLD ALIAS" as published for call @p epoch under the reception now. */
static void
tag_alias(dsd_state* state, uint64_t epoch) {
    DSD_SNPRINTF(state->generic_talker_alias[0], sizeof state->generic_talker_alias[0], "%s", "OLD ALIAS");
    state->generic_talker_alias_epoch[0] = epoch;
    state->generic_talker_alias_rx_gen[0] = dsd_state_reception_generation(state);
}

/* A talker alias belongs to its call, and the voice user repeats in every LDU1 while the alias arrives in pieces after
   it: a voice user that goes on with the call the alias was published for keeps it. Dropping it at every LDU1 made a
   Phase 1 alias flash on and off in the terminal UI. A new talker, an alias left from another call and a TDULC drop
   it. */
static int
test_voice_user_keeps_its_calls_alias(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    uint8_t lcw[72];
    int rc = 0;
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }

    rc |= expect_eq_int("alias keep fixture", init_established_clear_call(opts, state), 1);
    const uint64_t epoch = slot0_call_epoch(state);
    /* Published as the decoder publishes it: alias enrichment, under a reception generation that is not zero. */
    Event_History_I* history = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    if (!history) {
        dsd_state_ext_free_all(state);
        free(state);
        free(opts);
        return 1;
    }
    init_event_history(&history[0], 0, 255);
    init_event_history(&history[1], 0, 255);
    state->event_history_s = history;
    state->rx_id.epoch = 7U;
    state->generic_talker_alias[0][0] = '\0';
    rc |= expect_eq_int("alias published",
                        dsd_event_enrich_alias(state, 0U, DSD_SYNC_P25P1_POS, epoch, "OLD ALIAS") >= 0, 1);
    rc |= expect_eq_int("alias shown", strcmp(state->generic_talker_alias[0], "OLD ALIAS") == 0, 1);
    build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, LCGRGR_SRC);
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("repeated voice user keeps the call", slot0_call_epoch(state) == epoch, 1);
    rc |= expect_eq_int("repeated voice user keeps the call's alias",
                        strcmp(state->generic_talker_alias[0], "OLD ALIAS") == 0, 1);

    /* Another system named on the carrier (a new reception generation): the alias was not published under it. */
    state->rx_id.epoch++;
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("another reception drops the alias", state->generic_talker_alias[0][0], '\0');

    tag_alias(state, slot0_call_epoch(state));
    build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, LCGRGR_SRC + 1);
    const uint64_t before_talker = slot0_call_epoch(state);
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("new talker begins a call", slot0_call_epoch(state) != before_talker, 1);
    rc |= expect_eq_int("new talker drops the old talker's alias", state->generic_talker_alias[0][0], '\0');

    /* An alias published for an earlier call, on a call that is open now, is not this call's. */
    tag_alias(state, before_talker);
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("earlier call's alias dropped", state->generic_talker_alias[0][0], '\0');

    /* The same unit number needing a Source ID Extension (S set) is another system's radio: not the alias's talker. */
    tag_alias(state, slot0_call_epoch(state));
    build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, LCGRGR_SRC + 1);
    lcw[31] = 1U;
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("extension-required source drops the alias", state->generic_talker_alias[0][0], '\0');

    /* A Source ID Extension naming another radio moves the call's talker to it; that radio's alias is not the alias of
       the radio the next ordinary voice user names, which the trunk assignment still holds. */
    build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, LCGRGR_SRC + 1);
    p25_lcw(opts, state, lcw, 0);
    uint8_t ext[72];
    DSD_MEMSET(ext, 0, sizeof ext);
    set_bits_msb(ext, 0, 8, 0x49);
    set_bits_msb(ext, 16, 20, 0xABCDEU);
    set_bits_msb(ext, 36, 12, 0x2A7U);
    set_bits_msb(ext, 48, 24, (unsigned)(LCGRGR_SRC + 7));
    p25_lcw(opts, state, ext, 0);
    dsd_call_snapshot moved;
    if (dsd_call_state_get(state, 0U, &moved) > 0 && moved.ota_source_id == (uint64_t)(LCGRGR_SRC + 7)) {
        tag_alias(state, moved.epoch);
        p25_lcw(opts, state, lcw, 0);
        rc |= expect_eq_int("assignment's talker does not take the extension radio's alias",
                            state->generic_talker_alias[0][0] == '\0' || (slot0_call_epoch(state) != moved.epoch), 1);
    }

    /* A foreign radio (Source ID Extension required) keeps its alias across its own repeats; a local radio with the
       same number on the same call (no end decoded) is another radio and drops it. */
    build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, LCGRGR_SRC + 5);
    lcw[31] = 1U;
    p25_lcw(opts, state, lcw, 0);
    tag_alias(state, slot0_call_epoch(state));
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("foreign radio's repeat keeps its alias",
                        strcmp(state->generic_talker_alias[0], "OLD ALIAS") == 0, 1);
    lcw[31] = 0U;
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("local radio with the same number drops the foreign alias", state->generic_talker_alias[0][0],
                        '\0');

    /* A voice user naming a special address (a dispatcher) on the same call names no radio: not the alias's talker. */
    tag_alias(state, slot0_call_epoch(state));
    build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, 0xFFFFFD);
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("special source drops the alias", state->generic_talker_alias[0][0], '\0');

    /* Text a decoder set outside call enrichment (epoch 0) is no P25 call's either, even on a repeat of the call's own
       local voice user. */
    build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, LCGRGR_SRC + 3);
    p25_lcw(opts, state, lcw, 0);
    tag_alias(state, slot0_call_epoch(state));
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("own talker's repeat keeps the tagged alias",
                        strcmp(state->generic_talker_alias[0], "OLD ALIAS") == 0, 1);
    tag_alias(state, 0U);
    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("unbound alias dropped", state->generic_talker_alias[0][0], '\0');
    state->event_history_s = NULL;
    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    free(opts);
    return rc;
}

/* A full SUID ($49) naming another system's home for the number a local voice user checked withdraws the verdict: no
   alias is learned for it (dsd_call_state_source_learnable()). Between two hard-decoded voice users of one radio, it
   stays learnable. In hangtime, a claim naming another source takes the verdict mirrored on the retained call away; a
   claim naming the call's own source leaves it. */
static int
test_foreign_suid_withdraws_learning(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    uint8_t lcw[72];
    int rc = 0;
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    rc |= expect_eq_int("suid fixture", init_retained_p1_call(opts, state, LCGRGR_SG, LCGRGR_SRC), 1);
    build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, LCGRGR_SRC);
    p25_lcw(opts, state, lcw, 0);
    dsd_call_snapshot call;
    rc |= expect_eq_int("hard voice user learnable",
                        dsd_call_state_get(state, 0U, &call) > 0 && dsd_call_state_source_learnable(state, &call), 1);
    uint8_t ext[72];
    DSD_MEMSET(ext, 0, sizeof ext);
    set_bits_msb(ext, 0, 8, 0x49);
    set_bits_msb(ext, 16, 20, 0xABCDEU);
    set_bits_msb(ext, 36, 12, 0x2A7U);
    set_bits_msb(ext, 48, 24, (unsigned)LCGRGR_SRC);
    p25_lcw(opts, state, ext, 0);
    rc |= expect_eq_int("foreign SUID for the same number withdraws learning",
                        dsd_call_state_get(state, 0U, &call) > 0 && dsd_call_state_source_learnable(state, &call), 0);

    p25_lcw(opts, state, lcw, 0);
    rc |= expect_eq_int("relearnable after a local voice user",
                        dsd_call_state_get(state, 0U, &call) > 0 && dsd_call_state_source_learnable(state, &call), 1);
    rc |= expect_eq_int(
        "call ends", p25_sm_emit_end_call_at(opts, state, 0, LCGRGR_SG, LCGRGR_SRC, dsd_decode_now_mono_s()) != 0, 1);
    p25_sm_note_call_identity(state, 0, LCGRGR_SRC, LCGRGR_SG, 1, P25_SM_IDENTITY_CHECKED_ALL, 0U);
    rc |= expect_eq_int("hangtime claim of the same source keeps learning",
                        dsd_call_state_get(state, 0U, &call) > 0 && dsd_call_state_source_learnable(state, &call), 1);
    p25_sm_note_call_identity(state, 0, LCGRGR_SRC + 9, LCGRGR_SG, 1, P25_SM_IDENTITY_CHECKED_ALL, 0U);
    rc |= expect_eq_int("hangtime claim of another source takes learning away",
                        dsd_call_state_get(state, 0U, &call) > 0 && dsd_call_state_source_learnable(state, &call), 0);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

/* The identity a voice-user link control names reaches the trunk state machine as checked only when the link control was
   hard-decoded (spec 2.20); one a soft-erasure RS retry recovered leaves it unchecked. */
static int
test_voice_user_identity_checked_follows_hard_decode(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    int rc = 0;

    for (int soft = 0; soft < 2; soft++) {
        rc |= expect_eq_int("identity fixture", init_retained_p1_call(&opts, &state, LCGRGR_SG, LCGRGR_SRC), 1);
        build_group_voice_user_lcw(lcw, 0x00, 0x00, LCGRGR_SG, LCGRGR_SRC + 1);
        state.p25_p1_lcw_soft = (uint8_t)soft;
        p25_lcw(&opts, &state, lcw, 0);
        state.p25_p1_lcw_soft = 0U;
        rc |= expect_eq_int(soft ? "soft LC leaves the identity unchecked" : "hard LC checks the identity",
                            p25_sm_get_ctx()->slots[0].published.checked, soft ? 0 : P25_SM_IDENTITY_CHECKED_ALL);
        dsd_state_ext_free_all(&state);
    }
    return rc;
}

/* Preservation: KEY=0 on the active patch is a clear classification, so an encrypted LCGRGR keeps the gate open. */
static int
test_lcgrgr_key0_keeps_clear_gate(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    int rc = 0;

    rc |= expect_eq_int("LCGRGR KEY=0 fixture", init_established_clear_call(&opts, &state), 1);
    p25_patch_update(&state, LCGRGR_SG, 1, 1);
    p25_patch_add_wgid(&state, LCGRGR_SG, LCGRGR_SG + 1);
    p25_patch_set_kas(&state, LCGRGR_SG, 0, 0x80, 1);
    build_group_voice_user_lcw(lcw, 0x90, 0x40, LCGRGR_SG, LCGRGR_SRC);
    p25_lcw(&opts, &state, lcw, 0);

    rc |= expect_eq_int("LCGRGR KEY=0 keeps clear state", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_eq_int("LCGRGR KEY=0 keeps audio gate", state.p25_p2_audio_allowed[0], 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* A TDULC ends the transmission: its LCGRGR starts no voice and marks no crypto, but still records the user. */
static int
test_lcgrgr_from_tdulc_records_without_voice_start(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lcw[72];
    int rc = 0;

    rc |= expect_eq_int("LCGRGR TDULC fixture", init_retained_p1_call(&opts, &state, 0x1234, 0x010203), 1);
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof state.generic_talker_alias[0], "%s", "OLD ALIAS");
    build_group_voice_user_lcw(lcw, 0x90, 0x40, LCGRGR_SG, LCGRGR_SRC);
    p25_lcw_from_tdulc(&opts, &state, lcw, 0, NULL, slot0_call_epoch(&state));

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    rc |= expect_eq_int("LCGRGR TDULC no voice start", sm->slots[0].voice_active, 0);
    rc |= expect_eq_int("LCGRGR TDULC keeps SM target", sm->slots[0].ota_tg, 0x1234);
    rc |= expect_eq_int("LCGRGR TDULC no crypto mark", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_eq_int("LCGRGR TDULC service options", state.dmr_so, 0x40);
    rc |= expect_eq_int("LCGRGR TDULC talker alias cleared", state.generic_talker_alias[0][0], '\0');
    rc |= expect_eq_int("LCGRGR TDULC group affiliation", ga_recorded(&state, LCGRGR_SRC, LCGRGR_SG), 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* Slot 0 holding a call of @p protocol, still open or already ended, on a reset receiver. */
static int
seed_slot0_call(dsd_opts* opts, dsd_state* state, int protocol, uint64_t target, uint64_t source, int ended) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, state);
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .observed_m = 1.0,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) <= 0) {
        return 0;
    }
    return !ended || dsd_call_state_end(state, 0U, 0.0) > 0;
}

/* Link control names a radio for the P25 Phase 1 call it belongs to, never for another protocol's call that slot 0
   still holds: before, a source extension rewrote an open DMR call's source. (The Harris GPS halves' call binding is
   covered by P25_P1_HARRIS_GPS.) */
static int
test_lcw_identities_go_only_to_the_p25_call(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    static const struct {
        const char* name;
        unsigned format;
        unsigned mfid;
    } suids[] = {{"source extension", 0x49U, 0x00U}, {"Tait FQ-SUID", 0x01U, 0xD8U}};

    for (size_t i = 0; i < sizeof(suids) / sizeof(suids[0]); i++) {
        char tag[96];
        DSD_SNPRINTF(tag, sizeof tag, "%s: DMR call seeded", suids[i].name);
        rc |= expect_eq_int(tag, seed_slot0_call(&opts, &st, DSD_SYNC_DMR_BS_VOICE_POS, 0x1111U, 111U, 0), 1);
        uint8_t lcw[72];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, suids[i].format);
        set_bits_msb(lcw, 8, 8, suids[i].mfid);
        set_bits_msb(lcw, 16, 20, 0xABCDEU);
        set_bits_msb(lcw, 36, 12, 0x123U);
        set_bits_msb(lcw, 48, 24, 0x456789U);
        p25_lcw(&opts, &st, lcw, 0);
        dsd_call_snapshot call;
        DSD_MEMSET(&call, 0, sizeof call);
        (void)dsd_call_state_get(&st, 0U, &call);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the DMR call keeps its source", suids[i].name);
        rc |= expect_eq_int(tag, (int)call.ota_source_id, 111);
        DSD_SNPRINTF(tag, sizeof tag, "%s: the DMR call stays DMR's", suids[i].name);
        rc |= expect_eq_int(tag, call.protocol, DSD_SYNC_DMR_BS_VOICE_POS);
    }

    dsd_state_ext_free_all(&st);
    return rc;
}

/* A Motorola talker end names its source, not its talkgroup: it takes the talkgroup of the call it ends. From an LDU1
   that is only the open P25 call, since a new transmission's first LDU1 decodes before its own call opens and slot 0
   still holds the previous transmission's ended call. Before, that call's talkgroup went with the end, and the state
   machine recorded it as the ended talkgroup its stale-regrant guard compares later grants with. From a TDULC, which
   ends its call first, the ended call is the one it ends. */
static int
test_talker_end_takes_its_talkgroup_from_the_call_it_ends(void) {
    static dsd_opts opts;
    static dsd_state st;

    static const struct {
        const char* name;
        int tdulc;
        int slot_tg;
    } cases[] = {
        {"LDU1 after an earlier call", 0, 0x0777},
        {"TDULC of the assigned call", 1, 0x1234},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char tag[96];
        dsd_state_ext_free_all(&st);
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&st, 0, sizeof(st));
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        opts.trunk_hangtime = 2.0F;
        st.p25_cc_freq = 851000000;
        st.trunk_cc_freq = 851000000;
        st.p25_iden_fdma[1].base_freq = 851000000 / 5;
        st.p25_iden_fdma[1].chan_type = 0;
        st.p25_iden_fdma[1].chan_spac = 100;
        st.p25_iden_fdma[1].trust = 2;
        st.p25_iden_fdma[1].populated = 1;
        st.p25_chan_tdma_explicit[1] = 1;
        p25_sm_ctx_t* sm = p25_sm_get_ctx();
        p25_sm_init_ctx(sm, &opts, &st);
        p25_sm_event_t ev = p25_sm_ev_group_grant(0x100A, 851125000, 0x1234, 0x111, 0);
        p25_sm_event(sm, &opts, &st, &ev);
        DSD_SNPRINTF(tag, sizeof tag, "talker end, %s: voice active", cases[i].name);
        rc |= expect_eq_int(
            tag, p25_sm_emit_active_call(&opts, &st, 0, 0x1234, 0, 0x111, 1, 0) != 0 && sm->slots[0].voice_active, 1);
        /* Slot 0 now holds an ended P25 call of the case's talkgroup. */
        const dsd_call_observation ended = {
            .protocol = DSD_SYNC_P25P1_POS,
            .slot = 0U,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = (uint64_t)cases[i].slot_tg,
            .policy_target_id = (uint64_t)cases[i].slot_tg,
            .ota_source_id = 0x222U,
        };
        (void)dsd_call_state_observe(&st, &ended, DSD_CALL_BOUNDARY_BEGIN);
        (void)dsd_call_state_end(&st, 0U, 0.0);
        uint8_t lcw[72];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x0FU);
        set_bits_msb(lcw, 8, 8, 0x90U);
        set_bits_msb(lcw, 48, 24, 0x111U);
        if (cases[i].tdulc) {
            p25_lcw_from_tdulc(&opts, &st, lcw, 0, NULL, slot0_call_epoch(&st));
        } else {
            p25_lcw(&opts, &st, lcw, 0);
        }
        DSD_SNPRINTF(tag, sizeof tag, "talker end, %s: the assigned voice ends", cases[i].name);
        rc |= expect_eq_int(tag, sm->slots[0].voice_active, 0);
        DSD_SNPRINTF(tag, sizeof tag, "talker end, %s: the ended talkgroup is the assigned one", cases[i].name);
        rc |= expect_eq_int(tag, sm->slots[0].last_end_tg, 0x1234);
    }
    dsd_state_ext_free_all(&st);
    return rc;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    install_trunk_tuning_hooks();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    opts.trunk_enable = 1;
    opts.p25_lcw_retune = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_enc_calls = 0;
    st.p25_cc_freq = 851000000;
    // Seed IDEN 1 (FDMA): base in 5 kHz units, spacing 100 (5 kHz → 500 kHz)
    // Populate new dual-array
    st.p25_iden_fdma[1].base_freq = 851000000 / 5;
    st.p25_iden_fdma[1].chan_type = 0;
    st.p25_iden_fdma[1].chan_spac = 100;
    st.p25_iden_fdma[1].trust = 2;
    st.p25_iden_fdma[1].populated = 1;
    st.p25_chan_tdma_explicit[1] = 1; // FDMA known

    // Base LCW for 0x44 (Group Voice Channel Update – Explicit)
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof lcw);
    const int svc = 0x00;
    const int tg = 0x1234;
    const int ch = 0x100A;
    set_bits_msb(lcw, 0, 8, 0x44);
    set_bits_msb(lcw, 8, 8, 0x00);
    set_bits_msb(lcw, 16, 8, (unsigned)svc);
    set_bits_msb(lcw, 24, 16, (unsigned)tg);
    set_bits_msb(lcw, 40, 16, (unsigned)ch);

    // Control case: clear SVC should tune once
    g_tunes = 0;
    p25_lcw(&opts, &st, lcw, 0);
    rc |= expect_eq_int("clear->tune", g_tunes, 1);

    // Trunking disabled: a decoded 0x44 grant may update display state, but it
    // must not be treated as a trunking grant.
    opts.trunk_enable = 0;
    g_tunes = 0;
    p25_lcw(&opts, &st, lcw, 0);
    rc |= expect_eq_int("no trunk->no-tune", g_tunes, 0);
    opts.trunk_enable = 1;

    // Retune disabled should warn only once and never dispatch a grant.
    opts.p25_lcw_retune = 0;
    st.p25_lcw_retune_disabled_warned = 0;
    g_tunes = 0;
    p25_lcw(&opts, &st, lcw, 0);
    rc |= expect_eq_int("retune disabled no tune", g_tunes, 0);
    rc |= expect_eq_int("retune disabled warned", st.p25_lcw_retune_disabled_warned, 1);
    p25_lcw(&opts, &st, lcw, 0);
    rc |= expect_eq_int("retune disabled warned once", st.p25_lcw_retune_disabled_warned, 1);
    opts.p25_lcw_retune = 1;

    // TG hold mismatch suppresses an otherwise tunable group grant.
    st.tg_hold = tg + 1;
    g_tunes = 0;
    p25_lcw(&opts, &st, lcw, 0);
    rc |= expect_eq_int("tg hold mismatch no tune", g_tunes, 0);
    st.tg_hold = 0;

    // An immediate reassignment remains eligible after an explicit release.
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "test-explicit-release");
    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    const double decoded_cc_m = sm->t_cc_tune_m + 0.01;
    st.last_cc_sync_time = time(NULL);
    st.last_cc_sync_time_m = decoded_cc_m;
    st.p25_last_cc_msg_time = st.last_cc_sync_time;
    st.p25_last_cc_msg_time_m = decoded_cc_m;
    g_tunes = 0;
    p25_lcw(&opts, &st, lcw, 0);
    rc |= expect_eq_int("clear retunes after release", g_tunes, 1);

    // Packet bit set: tuning disabled by default policy (trunk_tune_data_calls=0)
    set_bits_msb(lcw, 16, 8, (unsigned)(svc | 0x10));
    g_tunes = 0;
    p25_lcw(&opts, &st, lcw, 0);
    rc |= expect_eq_int("packet->no-tune", g_tunes, 0);

    // Encrypted bit set: tuning disabled by default (trunk_tune_enc_calls=0)
    set_bits_msb(lcw, 16, 8, (unsigned)(svc | 0x40));
    g_tunes = 0;
    p25_lcw(&opts, &st, lcw, 0);
    rc |= expect_eq_int("enc->no-tune", g_tunes, 0);

    // A 0x44 LCW decoded on a VC must not learn that VC as the control channel.
    {
        static dsd_opts vc_opts;
        static dsd_state vc_st;
        DSD_MEMSET(&vc_opts, 0, sizeof vc_opts);
        DSD_MEMSET(&vc_st, 0, sizeof vc_st);
        vc_opts.trunk_enable = 1;
        vc_opts.p25_lcw_retune = 0;
        vc_opts.trunk_is_tuned = 1;
        vc_opts.audio_in_type = AUDIO_IN_RTL;
        vc_opts.rtlsdr_center_freq = 852112500U;
        set_bits_msb(lcw, 16, 8, (unsigned)svc);

        p25_lcw(&vc_opts, &vc_st, lcw, 0);
        rc |= expect_eq_int("vc-lcw no p25 cc seed", (int)vc_st.p25_cc_freq, 0);
        rc |= expect_eq_int("vc-lcw no trunk cc seed", (int)vc_st.trunk_cc_freq, 0);
        rc |= expect_eq_int("vc-lcw no lcn0 seed", (int)vc_st.trunk_lcn_freq[0], 0);
    }

    // Some unmanaged VC monitoring paths have not set the tuned flags; LCW must
    // still avoid treating the current tuner frequency as a control channel.
    {
        static dsd_opts vc_opts;
        static dsd_state vc_st;
        DSD_MEMSET(&vc_opts, 0, sizeof vc_opts);
        DSD_MEMSET(&vc_st, 0, sizeof vc_st);
        vc_opts.trunk_enable = 1;
        vc_opts.p25_lcw_retune = 1;
        vc_opts.trunk_tune_group_calls = 1;
        vc_opts.audio_in_type = AUDIO_IN_RTL;
        vc_opts.rtlsdr_center_freq = 852112500U;

        g_tunes = 0;
        p25_lcw(&vc_opts, &vc_st, lcw, 0);
        rc |= expect_eq_int("unmanaged vc-lcw no tune", g_tunes, 0);
        rc |= expect_eq_int("unmanaged vc-lcw no p25 cc seed", (int)vc_st.p25_cc_freq, 0);
        rc |= expect_eq_int("unmanaged vc-lcw no trunk cc seed", (int)vc_st.trunk_cc_freq, 0);
        rc |= expect_eq_int("unmanaged vc-lcw no lcn0 seed", (int)vc_st.trunk_lcn_freq[0], 0);
    }

    // In-band voice-user LCWs arrive before the next LDU's voice frames. An
    // encrypted indication must revoke a grant-derived clear classification,
    // except when an active regroup explicitly declares KEY=0.
    {
        uint8_t voice_lcw[72];
        const int voice_tg = 0x3456;
        opts.trunk_tune_private_calls = 1;
        DSD_MEMSET(voice_lcw, 0, sizeof(voice_lcw));
        set_bits_msb(voice_lcw, 0, 8, 0x00);
        set_bits_msb(voice_lcw, 16, 8, 0x40);
        set_bits_msb(voice_lcw, 32, 16, (unsigned)voice_tg);
        set_bits_msb(voice_lcw, 48, 24, 0x123456U);

        st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
        st.p25_p2_audio_allowed[0] = 1;
        p25_lcw(&opts, &st, voice_lcw, 0);
        rc |= expect_eq_int("encrypted group user revokes clear state", st.p25_crypto_state[0],
                            DSD_P25_CRYPTO_ENCRYPTED_PENDING);
        rc |= expect_eq_int("encrypted group user closes audio gate", st.p25_p2_audio_allowed[0], 0);

        p25_patch_update(&st, 69, 1, 1);
        p25_patch_add_wgid(&st, 69, voice_tg);
        p25_patch_set_kas(&st, 69, 0, 0x84, 17);
        st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
        st.p25_p2_audio_allowed[0] = 1;
        p25_lcw(&opts, &st, voice_lcw, 0);
        rc |= expect_eq_int("regroup KEY=0 preserves group clear state", st.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
        rc |= expect_eq_int("regroup KEY=0 preserves group audio gate", st.p25_p2_audio_allowed[0], 1);

        DSD_MEMSET(voice_lcw, 0, sizeof(voice_lcw));
        set_bits_msb(voice_lcw, 0, 8, 0x03);
        set_bits_msb(voice_lcw, 16, 8, 0x40);
        set_bits_msb(voice_lcw, 24, 24, 0x234567U);
        set_bits_msb(voice_lcw, 48, 24, 0x123456U);
        st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
        st.p25_p2_audio_allowed[0] = 1;
        p25_lcw(&opts, &st, voice_lcw, 0);
        rc |= expect_eq_int("encrypted unit user revokes clear state", st.p25_crypto_state[0],
                            DSD_P25_CRYPTO_ENCRYPTED_PENDING);
        rc |= expect_eq_int("encrypted unit user closes audio gate", st.p25_p2_audio_allowed[0], 0);
    }

    rc |= test_mfid90_regroup_reassigns_retained_call();
    rc |= test_extended_private_resolves_retained_identity();
    rc |= test_extended_private_encrypted_service_lockout();
    rc |= test_telephone_resolves_retained_identity();
    rc |= test_telephone_after_missed_boundary_drops_old_source();
    rc |= test_rejected_lcw_stops_post_processing(0x00, 0x00);
    rc |= test_rejected_lcw_stops_post_processing(0x03, 0x00);
    // Preservation: a rejected MFID90 LCGRGR (patched supergroup) leaves no crypto mark either.
    rc |= test_rejected_lcw_stops_post_processing(0x00, 0x90);
    rc |= test_group_voice_user_parity(0x00);
    rc |= test_group_voice_user_parity(0x90);
    rc |= test_voice_user_keeps_its_calls_alias();
    rc |= test_foreign_suid_withdraws_learning();
    rc |= test_voice_user_identity_checked_follows_hard_decode();
    rc |= test_lcgrgr_key0_keeps_clear_gate();
    rc |= test_lcgrgr_from_tdulc_records_without_voice_start();
    rc |= test_lcw_identities_go_only_to_the_p25_call();
    rc |= test_talker_end_takes_its_talkgroup_from_the_call_it_ends();

    dsd_state_ext_free_all(&st);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
