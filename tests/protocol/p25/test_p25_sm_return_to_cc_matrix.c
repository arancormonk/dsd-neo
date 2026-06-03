// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Bounded return-to-control-channel stress matrix for the unified P25 trunk SM.
 *
 * The harness drives p25_sm_event()/p25_sm_tick_ctx() directly and injects
 * tune outcomes through runtime trunk-tuning hooks. It keeps the matrix small
 * enough for normal CTest while exercising the mode, terminal-event, and
 * retune-result combinations that can wedge CC return.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

enum {
    MATRIX_BASE_CC_HZ = 851000000,
    MATRIX_IDEN_FDMA_C4FM = 1,
    MATRIX_IDEN_FDMA_QPSK = 2,
    MATRIX_IDEN_MIXED_P1CC_P2VC = 3,
    MATRIX_IDEN_P2CC_P2VC = 4,
    MATRIX_IDEN_AMBIG_P1 = 5,
    MATRIX_IDEN_AMBIG_P2 = 6,
};

typedef struct {
    const char* name;
    int iden;
    int low_channel;
    int synctype;
    int cc_is_tdma;
    int sys_is_tdma;
    int tdma_hint;
    int populate_fdma;
    int populate_tdma;
    int initial_rf_mod;
    int mod_qpsk;
    int expect_tdma;
} matrix_mode_case;

typedef enum {
    MATRIX_FLOW_GRANT_TIMEOUT = 0,
    MATRIX_FLOW_VC_SYNC_HANG,
    MATRIX_FLOW_PTT_IDLE_HANG,
    MATRIX_FLOW_ACTIVE_IDLE_HANG,
    MATRIX_FLOW_EXPLICIT_END,
    MATRIX_FLOW_TDU,
    MATRIX_FLOW_DUAL_PARTIAL_END,
    MATRIX_FLOW_FORCE_RELEASE,
    MATRIX_FLOW_ENC_LOCKOUT,
} matrix_flow_kind;

typedef struct {
    const char* name;
    matrix_flow_kind kind;
    int tdma_only;
    int fdma_only;
} matrix_flow_case;

typedef struct {
    const char* name;
    dsd_trunk_tune_result results[2];
    int count;
} matrix_return_script;

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    p25_sm_ctx_t ctx;
} matrix_fixture;

typedef struct {
    dsd_trunk_tune_result vc_results[4];
    int vc_count;
    int vc_pos;
    dsd_trunk_tune_result return_results[4];
    int return_count;
    int return_pos;
    dsd_trunk_tune_result cc_results[4];
    int cc_count;
    int cc_pos;
    int vc_calls;
    int return_calls;
    int cc_calls;
    long last_vc_freq;
    long last_cc_freq;
    int last_vc_sps;
    int last_cc_sps;
} matrix_hook_state;

static matrix_hook_state g_hooks;
static dsd_opts g_fixture_opts;
static dsd_state g_fixture_state;
static matrix_fixture g_fixture = {&g_fixture_opts, &g_fixture_state, {0}};

static const matrix_mode_case g_modes[] = {
    {"p1-fdma-c4fm", MATRIX_IDEN_FDMA_C4FM, 10, DSD_SYNC_P25P1_POS, 0, 0, 0x01, 1, 0, 0, 0, 0},
    {"p1-fdma-qpsk", MATRIX_IDEN_FDMA_QPSK, 11, DSD_SYNC_P25P1_POS, 0, 0, 0x01, 1, 0, 1, 1, 0},
    {"mixed-p1cc-p2vc", MATRIX_IDEN_MIXED_P1CC_P2VC, 3, DSD_SYNC_P25P1_POS, 0, 1, 0x02, 0, 1, 0, 0, 1},
    {"p2cc-p2vc", MATRIX_IDEN_P2CC_P2VC, 3, DSD_SYNC_P25P2_POS, 1, 1, 0x02, 0, 1, 1, 1, 1},
    {"ambig-p1-context", MATRIX_IDEN_AMBIG_P1, 13, DSD_SYNC_P25P1_POS, 0, 0, 0x03, 1, 1, 0, 0, 0},
    {"ambig-p2-context", MATRIX_IDEN_AMBIG_P2, 3, DSD_SYNC_P25P2_POS, 1, 1, 0x03, 1, 1, 1, 1, 1},
};

static const matrix_flow_case g_flows[] = {
    {"grant-timeout", MATRIX_FLOW_GRANT_TIMEOUT, 0, 0},
    {"vc-sync-hang", MATRIX_FLOW_VC_SYNC_HANG, 0, 0},
    {"ptt-idle-hang", MATRIX_FLOW_PTT_IDLE_HANG, 0, 0},
    {"active-idle-hang", MATRIX_FLOW_ACTIVE_IDLE_HANG, 0, 0},
    {"explicit-end", MATRIX_FLOW_EXPLICIT_END, 0, 0},
    {"p1-tdu", MATRIX_FLOW_TDU, 0, 1},
    {"dual-slot-partial-end", MATRIX_FLOW_DUAL_PARTIAL_END, 1, 0},
    {"forced-release", MATRIX_FLOW_FORCE_RELEASE, 0, 0},
    {"enc-lockout", MATRIX_FLOW_ENC_LOCKOUT, 0, 0},
};

static const matrix_return_script g_return_scripts[] = {
    {"ok", {DSD_TRUNK_TUNE_RESULT_OK, DSD_TRUNK_TUNE_RESULT_OK}, 1},
    {"pending", {DSD_TRUNK_TUNE_RESULT_PENDING, DSD_TRUNK_TUNE_RESULT_OK}, 1},
    {"deferred-then-ok", {DSD_TRUNK_TUNE_RESULT_DEFERRED, DSD_TRUNK_TUNE_RESULT_OK}, 2},
    {"failed-then-ok", {DSD_TRUNK_TUNE_RESULT_FAILED, DSD_TRUNK_TUNE_RESULT_OK}, 2},
    {"timeout-then-ok", {DSD_TRUNK_TUNE_RESULT_TIMEOUT, DSD_TRUNK_TUNE_RESULT_OK}, 2},
};

static void
matrix_hook_reset(void) {
    DSD_MEMSET(&g_hooks, 0, sizeof(g_hooks));
}

static dsd_trunk_tune_result
matrix_pop_result(dsd_trunk_tune_result* results, int count, int* pos) {
    if (!results || !pos || count <= 0) {
        return DSD_TRUNK_TUNE_RESULT_OK;
    }
    int idx = (*pos < count) ? *pos : (count - 1);
    (*pos)++;
    return results[idx];
}

static dsd_trunk_tune_result
matrix_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    dsd_trunk_tune_result result = matrix_pop_result(g_hooks.vc_results, g_hooks.vc_count, &g_hooks.vc_pos);
    g_hooks.vc_calls++;
    g_hooks.last_vc_freq = freq;
    g_hooks.last_vc_sps = ted_sps;

    if (dsd_trunk_tune_result_is_ok(result)) {
        double now_m = dsd_time_now_monotonic_s();
        if (opts) {
            opts->p25_is_tuned = 1;
            opts->trunk_is_tuned = 1;
        }
        if (state) {
            state->p25_vc_freq[0] = freq;
            state->p25_vc_freq[1] = freq;
            state->trunk_vc_freq[0] = freq;
            state->trunk_vc_freq[1] = freq;
            state->last_vc_sync_time = time(NULL);
            state->last_vc_sync_time_m = now_m;
            state->p25_last_vc_tune_time = state->last_vc_sync_time;
            state->p25_last_vc_tune_time_m = now_m;
        }
    }
    return result;
}

static dsd_trunk_tune_result
matrix_return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_hooks.return_calls++;
    return matrix_pop_result(g_hooks.return_results, g_hooks.return_count, &g_hooks.return_pos);
}

static dsd_trunk_tune_result
matrix_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    dsd_trunk_tune_result result = matrix_pop_result(g_hooks.cc_results, g_hooks.cc_count, &g_hooks.cc_pos);
    g_hooks.cc_calls++;
    g_hooks.last_cc_freq = freq;
    g_hooks.last_cc_sps = ted_sps;

    if (dsd_trunk_tune_result_is_ok(result) && state) {
        state->trunk_cc_freq = freq;
        state->last_cc_sync_time = time(NULL);
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    }
    return result;
}

static void
matrix_install_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_result = matrix_tune_to_freq;
    hooks.return_to_cc_result = matrix_return_to_cc;
    hooks.tune_to_cc_result = matrix_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static int
matrix_expect(int cond, const char* mode, const char* flow, const char* script, const char* check) {
    if (cond) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL mode=%s flow=%s script=%s check=%s\n", mode, flow, script, check);
    return 1;
}

static void
matrix_seed_fdma(dsd_state* state, int iden) {
    state->p25_iden_fdma[iden].base_freq = MATRIX_BASE_CC_HZ / 5;
    state->p25_iden_fdma[iden].chan_type = 1;
    state->p25_iden_fdma[iden].chan_spac = 100;
    state->p25_iden_fdma[iden].trust = 2;
    state->p25_iden_fdma[iden].populated = 1;
}

static void
matrix_seed_tdma(dsd_state* state, int iden) {
    state->p25_iden_tdma[iden].base_freq = MATRIX_BASE_CC_HZ / 5;
    state->p25_iden_tdma[iden].chan_type = 3;
    state->p25_iden_tdma[iden].chan_spac = 100;
    state->p25_iden_tdma[iden].trust = 2;
    state->p25_iden_tdma[iden].populated = 1;
}

static int
matrix_channel(const matrix_mode_case* mode) {
    return (mode->iden << 12) | (mode->low_channel & 0x0FFF);
}

static int
matrix_expected_slot(const matrix_mode_case* mode) {
    return mode->expect_tdma ? (mode->low_channel & 1) : -1;
}

static int
matrix_expected_sps(const matrix_fixture* fixture, const matrix_mode_case* mode) {
    return dsd_opts_compute_sps_rate(fixture->opts, mode->expect_tdma ? 6000 : 4800, 0);
}

static int
matrix_expected_cc_sps(const matrix_fixture* fixture, const matrix_mode_case* mode) {
    return dsd_opts_compute_sps_rate(fixture->opts, mode->cc_is_tdma == 1 ? 6000 : 4800, 0);
}

static void
matrix_setup_fixture(matrix_fixture* fixture, const matrix_mode_case* mode) {
    double now_m = dsd_time_now_monotonic_s();
    DSD_MEMSET(fixture->opts, 0, sizeof(*fixture->opts));
    DSD_MEMSET(fixture->state, 0, sizeof(*fixture->state));
    DSD_MEMSET(&fixture->ctx, 0, sizeof(fixture->ctx));

    fixture->opts->p25_trunk = 1;
    fixture->opts->trunk_enable = 1;
    fixture->opts->trunk_tune_group_calls = 1;
    fixture->opts->trunk_tune_private_calls = 1;
    fixture->opts->trunk_tune_enc_calls = 1;
    fixture->opts->p25_prefer_candidates = 1;
    fixture->opts->trunk_hangtime = 0.2f;
    fixture->opts->p25_grant_voice_to_s = 0.2;
    fixture->opts->mod_qpsk = mode->mod_qpsk;
    fixture->opts->verbose = 0;

    fixture->state->p25_cc_freq = MATRIX_BASE_CC_HZ;
    fixture->state->trunk_cc_freq = MATRIX_BASE_CC_HZ;
    fixture->state->last_cc_sync_time = time(NULL);
    fixture->state->last_cc_sync_time_m = now_m;
    fixture->state->nac = 0x293;
    fixture->state->p2_cc = 0x293;
    fixture->state->synctype = mode->synctype;
    fixture->state->lastsynctype = mode->synctype;
    fixture->state->p25_cc_is_tdma = mode->cc_is_tdma;
    fixture->state->p25_sys_is_tdma = mode->sys_is_tdma;
    fixture->state->p25_chan_tdma_explicit[mode->iden] = mode->tdma_hint;
    fixture->state->p25_vc_cqpsk_pref = -1;
    fixture->state->p25_vc_cqpsk_override = -1;
    fixture->state->rf_mod = mode->initial_rf_mod;
    fixture->state->p25_p2_active_slot = -1;

    if (mode->populate_fdma) {
        matrix_seed_fdma(fixture->state, mode->iden);
    }
    if (mode->populate_tdma) {
        matrix_seed_tdma(fixture->state, mode->iden);
    }

    p25_sm_init_ctx(&fixture->ctx, fixture->opts, fixture->state);
    fixture->ctx.config.hangtime_s = 0.2;
    fixture->ctx.config.grant_timeout_s = 0.2;
    fixture->ctx.config.cc_grace_s = 0.2;
}

static matrix_fixture*
matrix_reset_fixture(const matrix_mode_case* mode) {
    g_fixture.opts = &g_fixture_opts;
    g_fixture.state = &g_fixture_state;
    matrix_setup_fixture(&g_fixture, mode);
    return &g_fixture;
}

static p25_sm_event_t
matrix_group_grant(const matrix_mode_case* mode, int tg) {
    return p25_sm_ev_group_grant(matrix_channel(mode), 0, tg, 1000 + tg, 0);
}

static p25_sm_event_t
matrix_indiv_grant(const matrix_mode_case* mode, int dst) {
    return p25_sm_ev_indiv_grant(matrix_channel(mode), 0, dst, 7000 + dst, 0);
}

static int
matrix_send_initial_grant(matrix_fixture* fixture, const matrix_mode_case* mode, int use_indiv, const char* flow,
                          const char* script) {
    p25_sm_event_t ev = use_indiv ? matrix_indiv_grant(mode, 3000) : matrix_group_grant(mode, 2000);
    p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);

    int rc = 0;
    rc |= matrix_expect(g_hooks.vc_calls == 1, mode->name, flow, script, "initial grant called tune once");
    rc |= matrix_expect(fixture->ctx.state == P25_SM_TUNED, mode->name, flow, script, "grant reached TUNED");
    rc |= matrix_expect(fixture->opts->p25_is_tuned == 1 && fixture->opts->trunk_is_tuned == 1, mode->name, flow,
                        script, "grant set tuned flags");
    rc |= matrix_expect(g_hooks.last_vc_sps == matrix_expected_sps(fixture, mode), mode->name, flow, script,
                        "grant sps matches mode");
    rc |= matrix_expect(fixture->state->samplesPerSymbol == matrix_expected_sps(fixture, mode), mode->name, flow,
                        script, "state sps matches mode");
    rc |= matrix_expect(fixture->state->symbolCenter == dsd_opts_symbol_center(matrix_expected_sps(fixture, mode)),
                        mode->name, flow, script, "state symbol center matches mode");
    rc |= matrix_expect(fixture->state->p25_p2_active_slot == matrix_expected_slot(mode), mode->name, flow, script,
                        "active slot matches mode");
    rc |= matrix_expect(fixture->ctx.vc_is_tdma == mode->expect_tdma, mode->name, flow, script,
                        "ctx tdma flag matches mode");
    rc |= matrix_expect(fixture->state->rf_mod == (mode->expect_tdma ? 1 : mode->initial_rf_mod), mode->name, flow,
                        script, "rf_mod matches mode");
    return rc;
}

static void
matrix_age_tuned_timers(matrix_fixture* fixture) {
    double now_m = dsd_time_now_monotonic_s();
    fixture->ctx.t_tune_m = now_m - 1.0;
    fixture->ctx.t_voice_m = now_m - 1.0;
    fixture->state->last_vc_sync_time = time(NULL) - 1;
    fixture->state->last_vc_sync_time_m = now_m - 1.0;
    fixture->state->p25_last_vc_tune_time = time(NULL) - 1;
    fixture->state->p25_last_vc_tune_time_m = now_m - 1.0;
}

static int
matrix_drive_to_cc(matrix_fixture* fixture, const matrix_mode_case* mode, const matrix_flow_case* flow,
                   const matrix_return_script* script) {
    for (int i = 0; i < 4 && fixture->ctx.state != P25_SM_ON_CC; i++) {
        matrix_age_tuned_timers(fixture);
        p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);
    }

    int rc = 0;
    rc |= matrix_expect(fixture->ctx.state == P25_SM_ON_CC, mode->name, flow->name, script->name, "returned to ON_CC");
    rc |= matrix_expect(fixture->opts->p25_is_tuned == 0 && fixture->opts->trunk_is_tuned == 0, mode->name, flow->name,
                        script->name, "tuned flags cleared");
    rc |= matrix_expect(fixture->state->p25_vc_freq[0] == 0 && fixture->state->p25_vc_freq[1] == 0, mode->name,
                        flow->name, script->name, "p25 vc frequencies cleared");
    rc |= matrix_expect(fixture->state->trunk_vc_freq[0] == 0 && fixture->state->trunk_vc_freq[1] == 0, mode->name,
                        flow->name, script->name, "trunk vc frequencies cleared");
    rc |= matrix_expect(fixture->state->p25_p2_active_slot == -1, mode->name, flow->name, script->name,
                        "active slot reset");
    rc |= matrix_expect(fixture->state->p25_p2_audio_allowed[0] == 0 && fixture->state->p25_p2_audio_allowed[1] == 0,
                        mode->name, flow->name, script->name, "audio gates cleared");
    rc |= matrix_expect(fixture->ctx.vc_freq_hz == 0 && fixture->ctx.slots[0].voice_active == 0
                            && fixture->ctx.slots[1].voice_active == 0,
                        mode->name, flow->name, script->name, "ctx vc state cleared");
    rc |= matrix_expect(g_hooks.return_calls == script->count, mode->name, flow->name, script->name,
                        "return-to-cc call count matches script");
    return rc;
}

static int
matrix_apply_terminal_flow(matrix_fixture* fixture, const matrix_mode_case* mode, const matrix_flow_case* flow,
                           const matrix_return_script* script) {
    int slot = matrix_expected_slot(mode);
    int event_slot = (slot >= 0) ? slot : 0;
    p25_sm_event_t ev;
    int rc = 0;

    switch (flow->kind) {
        case MATRIX_FLOW_GRANT_TIMEOUT:
            matrix_age_tuned_timers(fixture);
            fixture->ctx.t_voice_m = 0.0;
            p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);
            break;

        case MATRIX_FLOW_VC_SYNC_HANG:
            ev.type = P25_SM_EV_VC_SYNC;
            ev.slot = event_slot;
            ev.channel = 0;
            ev.freq_hz = 0;
            ev.tg = 0;
            ev.src = 0;
            ev.dst = 0;
            ev.svc_bits = 0;
            ev.is_group = 0;
            ev.algid = 0;
            ev.keyid = 0;
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            matrix_age_tuned_timers(fixture);
            p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);
            break;

        case MATRIX_FLOW_PTT_IDLE_HANG:
            ev = p25_sm_ev_ptt(event_slot);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[event_slot] = 1;
            ev = p25_sm_ev_idle(event_slot);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[event_slot] = 0;
            matrix_age_tuned_timers(fixture);
            p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);
            break;

        case MATRIX_FLOW_ACTIVE_IDLE_HANG:
            ev = p25_sm_ev_active(event_slot);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[event_slot] = 1;
            ev = p25_sm_ev_idle(event_slot);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[event_slot] = 0;
            matrix_age_tuned_timers(fixture);
            p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);
            break;

        case MATRIX_FLOW_EXPLICIT_END:
            ev = p25_sm_ev_ptt(event_slot);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[event_slot] = 1;
            fixture->state->p25_p2_audio_ring_count[event_slot] = 3;
            ev = p25_sm_ev_end(event_slot);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            break;

        case MATRIX_FLOW_TDU:
            ev = p25_sm_ev_ptt(0);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[0] = 1;
            ev = p25_sm_ev_tdu();
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            break;

        case MATRIX_FLOW_DUAL_PARTIAL_END:
            ev = p25_sm_ev_ptt(0);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            ev = p25_sm_ev_ptt(1);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[0] = 1;
            fixture->state->p25_p2_audio_allowed[1] = 1;
            ev = p25_sm_ev_end(0);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            rc |= matrix_expect(fixture->ctx.state == P25_SM_TUNED, mode->name, flow->name, script->name,
                                "partial dual-slot end remains tuned");
            rc |= matrix_expect(g_hooks.return_calls == 0, mode->name, flow->name, script->name,
                                "partial dual-slot end does not return");
            fixture->state->p25_p2_audio_allowed[1] = 0;
            ev = p25_sm_ev_end(1);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            break;

        case MATRIX_FLOW_FORCE_RELEASE:
            ev = p25_sm_ev_ptt(event_slot);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[event_slot] = 1;
            fixture->state->p25_sm_force_release = 1;
            p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);
            break;

        case MATRIX_FLOW_ENC_LOCKOUT:
            fixture->opts->trunk_tune_enc_calls = 0;
            ev = p25_sm_ev_ptt(event_slot);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            fixture->state->p25_p2_audio_allowed[event_slot] = 1;
            ev = p25_sm_ev_enc(event_slot, 0x84, 0x1234, 2000);
            p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
            break;
    }

    rc |= matrix_drive_to_cc(fixture, mode, flow, script);
    return rc;
}

static int
matrix_run_terminal_case(const matrix_mode_case* mode, const matrix_flow_case* flow,
                         const matrix_return_script* script) {
    if ((flow->tdma_only && !mode->expect_tdma) || (flow->fdma_only && mode->expect_tdma)) {
        return 0;
    }

    matrix_fixture* fixture;
    matrix_hook_reset();
    g_hooks.vc_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.vc_count = 1;
    for (int i = 0; i < script->count; i++) {
        g_hooks.return_results[i] = script->results[i];
    }
    g_hooks.return_count = script->count;
    matrix_install_hooks();
    fixture = matrix_reset_fixture(mode);

    int rc = matrix_send_initial_grant(fixture, mode, 0, flow->name, script->name);
    if (rc != 0) {
        return rc;
    }
    return matrix_apply_terminal_flow(fixture, mode, flow, script);
}

static int
matrix_run_initial_tune_reject_case(const matrix_mode_case* mode, dsd_trunk_tune_result result, const char* name) {
    matrix_fixture* fixture;
    matrix_hook_reset();
    g_hooks.vc_results[0] = result;
    g_hooks.vc_count = 1;
    matrix_install_hooks();
    fixture = matrix_reset_fixture(mode);

    p25_sm_event_t ev = matrix_group_grant(mode, 2200);
    p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);

    int rc = 0;
    rc |= matrix_expect(g_hooks.vc_calls == 1, mode->name, "initial-tune", name, "vc tune attempted");
    rc |= matrix_expect(fixture->ctx.state == P25_SM_ON_CC, mode->name, "initial-tune", name,
                        "failed/deferred vc tune stays ON_CC");
    rc |= matrix_expect(fixture->opts->p25_is_tuned == 0 && fixture->opts->trunk_is_tuned == 0, mode->name,
                        "initial-tune", name, "failed/deferred vc tune leaves tuned flags clear");
    rc |= matrix_expect(fixture->state->p25_vc_freq[0] == 0 && fixture->state->trunk_vc_freq[0] == 0, mode->name,
                        "initial-tune", name, "failed/deferred vc tune leaves vc frequencies clear");
    rc |= matrix_expect(fixture->state->p25_sm_tune_count == 0, mode->name, "initial-tune", name,
                        "failed/deferred vc tune does not increment counter");
    rc |= matrix_expect(fixture->state->p25_p2_active_slot == -1, mode->name, "initial-tune", name,
                        "failed/deferred vc tune restores active slot");
    rc |= matrix_expect(fixture->state->rf_mod == mode->initial_rf_mod, mode->name, "initial-tune", name,
                        "failed/deferred vc tune restores rf_mod");
    return rc;
}

static int
matrix_run_active_no_terminal_case(const matrix_mode_case* mode) {
    matrix_fixture* fixture;
    const char* flow = "active-no-terminal";
    matrix_hook_reset();
    g_hooks.vc_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.vc_count = 1;
    g_hooks.return_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.return_count = 1;
    matrix_install_hooks();
    fixture = matrix_reset_fixture(mode);

    int rc = matrix_send_initial_grant(fixture, mode, 0, flow, "no-return");
    if (rc != 0) {
        return rc;
    }
    int slot = matrix_expected_slot(mode);
    int event_slot = (slot >= 0) ? slot : 0;
    p25_sm_event_t ev = p25_sm_ev_ptt(event_slot);
    p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
    matrix_age_tuned_timers(fixture);
    p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);

    rc |= matrix_expect(fixture->ctx.state == P25_SM_TUNED, mode->name, flow, "no-return",
                        "active call remains tuned without terminal signal");
    rc |= matrix_expect(fixture->opts->p25_is_tuned == 1 && g_hooks.return_calls == 0, mode->name, flow, "no-return",
                        "active call did not spuriously return");
    return rc;
}

static int
matrix_run_duplicate_grant_case(const matrix_mode_case* mode) {
    matrix_fixture* fixture;
    matrix_hook_reset();
    g_hooks.vc_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.vc_results[1] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.vc_count = 2;
    g_hooks.return_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.return_count = 1;
    matrix_install_hooks();
    fixture = matrix_reset_fixture(mode);

    int rc = matrix_send_initial_grant(fixture, mode, 0, "duplicate-grant", "ok");
    if (rc != 0) {
        return rc;
    }
    p25_sm_event_t ev = matrix_group_grant(mode, 2000);
    p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
    rc |= matrix_expect(g_hooks.vc_calls == 1, mode->name, "duplicate-grant", "ok", "duplicate grant did not retune");

    ev = p25_sm_ev_end((matrix_expected_slot(mode) >= 0) ? matrix_expected_slot(mode) : 0);
    p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
    rc |= matrix_drive_to_cc(fixture, mode, &(matrix_flow_case){"duplicate-grant", MATRIX_FLOW_EXPLICIT_END, 0, 0},
                             &(matrix_return_script){"ok", {DSD_TRUNK_TUNE_RESULT_OK, DSD_TRUNK_TUNE_RESULT_OK}, 1});
    return rc;
}

static int
matrix_run_retune_after_release_case(const matrix_mode_case* mode) {
    matrix_fixture* fixture;
    matrix_hook_reset();
    g_hooks.vc_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.vc_results[1] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.vc_count = 2;
    g_hooks.return_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.return_count = 1;
    matrix_install_hooks();
    fixture = matrix_reset_fixture(mode);

    matrix_flow_case flow = {"retune-after-release", MATRIX_FLOW_EXPLICIT_END, 0, 0};
    matrix_return_script script = {"ok", {DSD_TRUNK_TUNE_RESULT_OK, DSD_TRUNK_TUNE_RESULT_OK}, 1};
    int rc = matrix_send_initial_grant(fixture, mode, 1, flow.name, script.name);
    if (rc != 0) {
        return rc;
    }
    p25_sm_event_t ev = p25_sm_ev_end((matrix_expected_slot(mode) >= 0) ? matrix_expected_slot(mode) : 0);
    p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
    rc |= matrix_drive_to_cc(fixture, mode, &flow, &script);
    if (rc != 0) {
        return rc;
    }

    ev = matrix_group_grant(mode, 2400);
    p25_sm_event(&fixture->ctx, fixture->opts, fixture->state, &ev);
    rc |= matrix_expect(g_hooks.vc_calls == 2, mode->name, flow.name, "second-grant", "second grant retuned");
    rc |= matrix_expect(fixture->ctx.state == P25_SM_TUNED, mode->name, flow.name, "second-grant",
                        "second grant reached TUNED");
    return rc;
}

static int
matrix_run_cc_hunt_case(const matrix_mode_case* mode, dsd_trunk_tune_result first_result, const char* result_name) {
    matrix_fixture* fixture;
    matrix_hook_reset();
    g_hooks.cc_results[0] = first_result;
    g_hooks.cc_results[1] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.cc_count = 2;
    matrix_install_hooks();
    fixture = matrix_reset_fixture(mode);

    long candidate = MATRIX_BASE_CC_HZ + 1000000;
    double now_m = dsd_time_now_monotonic_s();
    (void)dsd_trunk_cc_candidates_add(fixture->state, candidate, 0);
    fixture->state->last_cc_sync_time = time(NULL) - 10;
    fixture->state->last_cc_sync_time_m = now_m - 10.0;
    fixture->ctx.t_cc_sync_m = now_m - 10.0;

    p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);
    int rc = 0;
    rc |= matrix_expect(g_hooks.cc_calls == 1, mode->name, "cc-hunt", result_name, "cc tune attempted");
    rc |= matrix_expect(g_hooks.last_cc_sps == matrix_expected_cc_sps(fixture, mode), mode->name, "cc-hunt",
                        result_name, "cc hunt sps matches cc mode");

    if (dsd_trunk_tune_result_is_ok(first_result)) {
        rc |= matrix_expect(fixture->ctx.state == P25_SM_ON_CC, mode->name, "cc-hunt", result_name,
                            "accepted cc hunt returns ON_CC");
        rc |= matrix_expect(fixture->state->p25_cc_eval_freq == candidate, mode->name, "cc-hunt", result_name,
                            "accepted candidate is under evaluation");
        return rc;
    }

    rc |= matrix_expect(fixture->ctx.state == P25_SM_HUNTING, mode->name, "cc-hunt", result_name,
                        "failed/deferred cc hunt stays HUNTING");
    rc |= matrix_expect(fixture->state->trunk_cc_freq == MATRIX_BASE_CC_HZ, mode->name, "cc-hunt", result_name,
                        "failed/deferred cc hunt preserves tracked cc");
    rc |= matrix_expect(fixture->state->p25_cc_eval_freq == 0, mode->name, "cc-hunt", result_name,
                        "failed/deferred cc hunt does not enter eval");

    fixture->ctx.t_hunt_try_m = dsd_time_now_monotonic_s() - 6.0;
    p25_sm_tick_ctx(&fixture->ctx, fixture->opts, fixture->state);
    rc |= matrix_expect(g_hooks.cc_calls == 2, mode->name, "cc-hunt", result_name, "cc hunt retried");
    rc |= matrix_expect(fixture->ctx.state == P25_SM_ON_CC, mode->name, "cc-hunt", result_name,
                        "cc hunt recovered on retry");
    return rc;
}

int
main(void) {
    int rc = 0;

    static const struct {
        const char* name;
        dsd_trunk_tune_result result;
    } tune_rejects[] = {
        {"deferred", DSD_TRUNK_TUNE_RESULT_DEFERRED},
        {"failed", DSD_TRUNK_TUNE_RESULT_FAILED},
        {"timeout", DSD_TRUNK_TUNE_RESULT_TIMEOUT},
    };

    static const struct {
        const char* name;
        dsd_trunk_tune_result result;
    } cc_results[] = {
        {"ok", DSD_TRUNK_TUNE_RESULT_OK},
        {"pending", DSD_TRUNK_TUNE_RESULT_PENDING},
        {"deferred", DSD_TRUNK_TUNE_RESULT_DEFERRED},
        {"failed", DSD_TRUNK_TUNE_RESULT_FAILED},
        {"timeout", DSD_TRUNK_TUNE_RESULT_TIMEOUT},
    };

    for (size_t m = 0; m < sizeof(g_modes) / sizeof(g_modes[0]); m++) {
        for (size_t r = 0; r < sizeof(tune_rejects) / sizeof(tune_rejects[0]); r++) {
            rc |= matrix_run_initial_tune_reject_case(&g_modes[m], tune_rejects[r].result, tune_rejects[r].name);
        }
        rc |= matrix_run_active_no_terminal_case(&g_modes[m]);
        rc |= matrix_run_duplicate_grant_case(&g_modes[m]);
        rc |= matrix_run_retune_after_release_case(&g_modes[m]);

        for (size_t f = 0; f < sizeof(g_flows) / sizeof(g_flows[0]); f++) {
            for (size_t s = 0; s < sizeof(g_return_scripts) / sizeof(g_return_scripts[0]); s++) {
                rc |= matrix_run_terminal_case(&g_modes[m], &g_flows[f], &g_return_scripts[s]);
            }
        }

        for (size_t c = 0; c < sizeof(cc_results) / sizeof(cc_results[0]); c++) {
            rc |= matrix_run_cc_hunt_case(&g_modes[m], cc_results[c].result, cc_results[c].name);
        }
    }

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    if (rc != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "P25 return-to-CC matrix passed\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
