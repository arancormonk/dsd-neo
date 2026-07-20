// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P1 LCW gating tests: verify Packet (0x10) and Encrypted (0x40)
 * service options block tuning via the trunk SM when tuning policies are
 * disabled.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
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

void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors);

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
    if (!p25_sm_emit_end_call_at(opts, state, 0, tg, src, dsd_time_now_monotonic_s())) {
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
    int rc = 0;

    rc |= expect_eq_int("extended private retained fixture", init_retained_p1_call(&opts, &state, 0x1234, 0x010203), 1);
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits_msb(lcw, 0, 8, 0x4A);
    set_bits_msb(lcw, 16, 8, 0x00);
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
    rc |= expect_eq_int("extended private voice active", sm->slots[0].voice_active, 1);
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
    rc |= expect_eq_int("telephone missed-boundary decoder source cleared", state.lastsrc, 0);
    rc |= expect_eq_int("telephone missed-boundary call kind", sm->slots[0].is_group, 0);
    rc |= expect_eq_int("telephone missed-boundary voice active", sm->slots[0].voice_active, 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_rejected_lcw_stops_post_processing(int format) {
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
    DSD_SNPRINTF(state.call_string[0], sizeof(state.call_string[0]), "%s", "stale traffic banner");

    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits_msb(lcw, 0, 8, (unsigned)format);
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
    rc |= expect_eq_int("rejected LCW banner remains blank", strcmp(state.call_string[0], "                     "), 0);
    dsd_state_ext_free_all(&state);
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
    rc |= test_telephone_resolves_retained_identity();
    rc |= test_telephone_after_missed_boundary_drops_old_source();
    rc |= test_rejected_lcw_stops_post_processing(0x00);
    rc |= test_rejected_lcw_stops_post_processing(0x03);

    dsd_state_ext_free_all(&st);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
