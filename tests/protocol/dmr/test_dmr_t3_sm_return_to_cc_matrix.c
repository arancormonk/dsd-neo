// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Bounded DMR Tier III return-to-control-channel matrix.
 *
 * The harness drives the DMR trunking state machine directly and injects
 * result-returning trunk tune hooks. It covers accepted asynchronous retunes,
 * rejected voice-channel tunes, and retryable CC returns without depending on
 * radio or rigctl backends.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

typedef struct {
    const char* name;
    long freq_hz;
    int lpcn;
    int trust;
    long mapped_freq_hz;
} dmr_grant_case;

typedef enum {
    DMR_FLOW_GRANT_TIMEOUT = 0,
    DMR_FLOW_VOICE_HANGTIME,
    DMR_FLOW_DATA_HANGTIME,
    DMR_FLOW_EXPLICIT_RELEASE,
    DMR_FLOW_DUAL_SLOT_PARTIAL_RELEASE,
    DMR_FLOW_FORCED_RELEASE,
} dmr_flow_kind;

typedef struct {
    const char* name;
    dmr_flow_kind kind;
} dmr_flow_case;

typedef struct {
    const char* name;
    dsd_trunk_tune_result results[2];
    int count;
} dmr_return_script;

typedef struct {
    dsd_trunk_tune_result tune_results[4];
    int tune_count;
    int tune_pos;
    dsd_trunk_tune_result return_results[4];
    int return_count;
    int return_pos;
    int tune_calls;
    int return_calls;
    long last_tune_freq;
} dmr_hook_state;

static dmr_hook_state g_hooks;
static dsd_opts g_opts;
static dsd_state g_state;
static dmr_sm_ctx_t g_ctx;

static const dmr_grant_case g_grants[] = {
    {"explicit-frequency", 852012500L, 0, 0, 0},
    {"trusted-lpcn", 0, 33, 2, 852512500L},
    {"untrusted-lpcn-on-cc", 0, 34, 0, 853012500L},
};

static const dmr_flow_case g_flows[] = {
    {"grant-timeout", DMR_FLOW_GRANT_TIMEOUT},
    {"voice-hangtime", DMR_FLOW_VOICE_HANGTIME},
    {"data-hangtime", DMR_FLOW_DATA_HANGTIME},
    {"explicit-release", DMR_FLOW_EXPLICIT_RELEASE},
    {"dual-slot-partial-release", DMR_FLOW_DUAL_SLOT_PARTIAL_RELEASE},
    {"forced-release", DMR_FLOW_FORCED_RELEASE},
};

static const dmr_return_script g_return_scripts[] = {
    {"ok", {DSD_TRUNK_TUNE_RESULT_OK, DSD_TRUNK_TUNE_RESULT_OK}, 1},
    {"pending", {DSD_TRUNK_TUNE_RESULT_PENDING, DSD_TRUNK_TUNE_RESULT_OK}, 1},
    {"deferred-then-ok", {DSD_TRUNK_TUNE_RESULT_DEFERRED, DSD_TRUNK_TUNE_RESULT_OK}, 2},
    {"failed-then-ok", {DSD_TRUNK_TUNE_RESULT_FAILED, DSD_TRUNK_TUNE_RESULT_OK}, 2},
    {"timeout-then-ok", {DSD_TRUNK_TUNE_RESULT_TIMEOUT, DSD_TRUNK_TUNE_RESULT_OK}, 2},
};

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

static long
dmr_case_expected_freq(const dmr_grant_case* grant) {
    return (grant->freq_hz > 0) ? grant->freq_hz : grant->mapped_freq_hz;
}

static dsd_trunk_tune_result
dmr_pop_result(const dsd_trunk_tune_result* results, int count, int* pos) {
    if (!results || !pos || count <= 0) {
        return DSD_TRUNK_TUNE_RESULT_OK;
    }
    int idx = (*pos < count) ? *pos : (count - 1);
    (*pos)++;
    return results[idx];
}

static dsd_trunk_tune_result
dmr_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    dsd_trunk_tune_result result = dmr_pop_result(g_hooks.tune_results, g_hooks.tune_count, &g_hooks.tune_pos);
    g_hooks.tune_calls++;
    g_hooks.last_tune_freq = freq;

    if (dsd_trunk_tune_result_is_ok(result)) {
        if (opts) {
            opts->trunk_is_tuned = 1;
        }
        if (state) {
            state->trunk_vc_freq[0] = freq;
            state->trunk_vc_freq[1] = freq;
            state->last_vc_sync_time = time(NULL);
            state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
        }
    }
    return result;
}

static dsd_trunk_tune_result
dmr_hook_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    g_hooks.return_calls++;
    return dmr_pop_result(g_hooks.return_results, g_hooks.return_count, &g_hooks.return_pos);
}

static void*
dmr_hook_scan_ctx(void) {
    return &g_ctx;
}

static void
dmr_install_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = dmr_hook_tune_to_freq;
    hooks.return_to_cc_request = dmr_hook_return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
dmr_reset_hooks(void) {
    DSD_MEMSET(&g_hooks, 0, sizeof(g_hooks));
}

static int
dmr_expect(int cond, const char* grant, const char* flow, const char* script, const char* check) {
    if (cond) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL grant=%s flow=%s script=%s check=%s\n", grant, flow, script, check);
    return 1;
}

static int
dmr_expect_close(double actual, double expected, double tolerance, const char* grant, const char* flow,
                 const char* script, const char* check) {
    double delta = actual - expected;
    if (delta < 0.0) {
        delta = -delta;
    }
    return dmr_expect(delta <= tolerance, grant, flow, script, check);
}

static void
dmr_setup_fixture(const dmr_grant_case* grant) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    DSD_MEMSET(&g_ctx, 0, sizeof(g_ctx));

    g_opts.trunk_enable = 1;
    g_opts.trunk_hangtime = 0.1f;
    g_opts.trunk_tune_data_calls = 1;
    g_state.trunk_cc_freq = 851012500L;
    g_state.dmr_rest_channel = -1;

    if (grant->lpcn > 0) {
        g_state.trunk_chan_map[grant->lpcn] = grant->mapped_freq_hz;
        g_state.dmr_lcn_trust[grant->lpcn] = (uint8_t)grant->trust;
    }

    dmr_sm_init_ctx(&g_ctx, &g_opts, &g_state);
    g_ctx.hangtime_s = 0.1;
    g_ctx.grant_timeout_s = 0.1;
    g_ctx.cc_grace_s = 0.1;
}

static void
dmr_setup_blank_fixture(void) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    DSD_MEMSET(&g_ctx, 0, sizeof(g_ctx));
}

static void
dmr_age_grant_timeout(void) {
    g_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    g_ctx.t_voice_m = 0.0;
}

static void
dmr_age_hangtime(void) {
    double old_m = dsd_time_now_monotonic_s() - 1.0;
    g_ctx.t_tune_m = old_m;
    g_ctx.t_voice_m = old_m;
    g_ctx.slots[0].last_active_m = old_m;
    g_ctx.slots[1].last_active_m = old_m;
    g_state.last_vc_sync_time = time(NULL) - 1;
    g_state.last_vc_sync_time_m = old_m;
}

static dmr_sm_event_t
dmr_case_grant_event(const dmr_grant_case* grant, int id, int use_indiv) {
    return use_indiv ? dmr_sm_ev_indiv_grant(grant->freq_hz, grant->lpcn, id, 7000 + id)
                     : dmr_sm_ev_group_grant(grant->freq_hz, grant->lpcn, id, 7000 + id);
}

static int
dmr_send_initial_grant(const dmr_grant_case* grant, int use_indiv, const char* flow, const char* script) {
    const int grant_id = 1201;
    dmr_sm_event_t ev = dmr_case_grant_event(grant, grant_id, use_indiv);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);

    long expected_freq = dmr_case_expected_freq(grant);
    int expected_tg = use_indiv ? 0 : grant_id;
    int rc = 0;
    rc |= dmr_expect(g_hooks.tune_calls == 1, grant->name, flow, script, "initial tune attempted once");
    rc |= dmr_expect(g_hooks.last_tune_freq == expected_freq, grant->name, flow, script, "initial tune frequency");
    rc |= dmr_expect(g_ctx.state == DMR_SM_TUNED, grant->name, flow, script, "initial grant reached TUNED");
    rc |= dmr_expect(g_ctx.vc_freq_hz == expected_freq, grant->name, flow, script, "ctx voice frequency committed");
    rc |= dmr_expect(g_ctx.vc_tg == expected_tg && g_ctx.vc_src == 7000 + grant_id, grant->name, flow, script,
                     use_indiv ? "individual grant metadata committed" : "group grant metadata committed");
    rc |= dmr_expect(g_opts.trunk_is_tuned == 1, grant->name, flow, script, "generic tuned flag committed");
    rc |= dmr_expect(g_state.trunk_vc_freq[0] == expected_freq && g_state.trunk_vc_freq[1] == expected_freq,
                     grant->name, flow, script, "voice frequencies committed");
    rc |= dmr_expect(g_state.p25_sm_tune_count == 1, grant->name, flow, script, "tune counter incremented");
    return rc;
}

static int
dmr_drive_to_cc(const dmr_grant_case* grant, const dmr_flow_case* flow, const dmr_return_script* script) {
    for (int i = 0; i < 4 && g_ctx.state != DMR_SM_ON_CC; i++) {
        dmr_age_hangtime();
        dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
    }

    int rc = 0;
    rc |= dmr_expect(g_ctx.state == DMR_SM_ON_CC, grant->name, flow->name, script->name, "returned to ON_CC");
    rc |= dmr_expect(g_opts.trunk_is_tuned == 0, grant->name, flow->name, script->name, "tuned flag cleared");
    rc |= dmr_expect(g_state.trunk_vc_freq[0] == 0 && g_state.trunk_vc_freq[1] == 0, grant->name, flow->name,
                     script->name, "voice frequencies cleared");
    rc |= dmr_expect(g_ctx.vc_freq_hz == 0 && g_ctx.vc_lpcn == 0 && g_ctx.vc_tg == 0, grant->name, flow->name,
                     script->name, "ctx voice grant state cleared");
    rc |= dmr_expect(g_ctx.slots[0].voice_active == 0 && g_ctx.slots[1].voice_active == 0, grant->name, flow->name,
                     script->name, "slot activity cleared");
    rc |= dmr_expect(g_state.p25_sm_release_count == 1, grant->name, flow->name, script->name,
                     "release counter incremented once");
    rc |= dmr_expect(g_hooks.return_calls == script->count, grant->name, flow->name, script->name,
                     "return calls match script");
    return rc;
}

static int
dmr_expect_rejected_return_preserved(const dmr_grant_case* grant, const dmr_flow_case* flow,
                                     const dmr_return_script* script) {
    if (dsd_trunk_tune_result_is_ok(script->results[0])) {
        return 0;
    }

    long expected_freq = dmr_case_expected_freq(grant);
    int rc = 0;
    rc |= dmr_expect(g_hooks.return_calls == 1, grant->name, flow->name, script->name,
                     "rejected return attempted once before retry");
    rc |= dmr_expect(g_ctx.state == DMR_SM_TUNED, grant->name, flow->name, script->name,
                     "rejected return preserved tuned state");
    rc |= dmr_expect(g_opts.trunk_is_tuned == 1, grant->name, flow->name, script->name,
                     "rejected return preserved tuned flag");
    rc |= dmr_expect(g_state.trunk_vc_freq[0] == expected_freq && g_state.trunk_vc_freq[1] == expected_freq,
                     grant->name, flow->name, script->name, "rejected return preserved voice frequencies");
    rc |= dmr_expect(g_ctx.vc_freq_hz == expected_freq, grant->name, flow->name, script->name,
                     "rejected return preserved ctx voice frequency");
    rc |= dmr_expect(g_state.p25_sm_release_count == 0, grant->name, flow->name, script->name,
                     "rejected return did not count release");
    rc |= dmr_expect(g_state.trunk_sm_force_release == (flow->kind == DMR_FLOW_FORCED_RELEASE ? 1 : 0), grant->name,
                     flow->name, script->name, "rejected return force latch state");
    return rc;
}

static int
dmr_apply_terminal_flow(const dmr_grant_case* grant, const dmr_flow_case* flow, const dmr_return_script* script) {
    dmr_sm_event_t ev;
    int rc = 0;

    switch (flow->kind) {
        case DMR_FLOW_GRANT_TIMEOUT:
            dmr_age_grant_timeout();
            dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
            break;

        case DMR_FLOW_VOICE_HANGTIME:
            ev = dmr_sm_ev_voice_sync(0);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            g_ctx.slots[0].voice_active = 0;
            dmr_age_hangtime();
            dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
            break;

        case DMR_FLOW_DATA_HANGTIME:
            ev = dmr_sm_ev_data_sync(1);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            dmr_age_hangtime();
            dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
            break;

        case DMR_FLOW_EXPLICIT_RELEASE:
            ev = dmr_sm_ev_voice_sync(0);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            ev = dmr_sm_ev_release(-1);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            dmr_age_hangtime();
            dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
            break;

        case DMR_FLOW_DUAL_SLOT_PARTIAL_RELEASE:
            ev = dmr_sm_ev_voice_sync(0);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            ev = dmr_sm_ev_voice_sync(1);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            ev = dmr_sm_ev_release(0);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            g_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
            dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
            rc |= dmr_expect(g_ctx.state == DMR_SM_TUNED, grant->name, flow->name, script->name,
                             "partial slot release remains tuned");
            rc |= dmr_expect(g_hooks.return_calls == 0, grant->name, flow->name, script->name,
                             "partial slot release does not return");
            ev = dmr_sm_ev_release(1);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            dmr_age_hangtime();
            dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
            break;

        case DMR_FLOW_FORCED_RELEASE:
            ev = dmr_sm_ev_voice_sync(0);
            dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
            g_state.trunk_sm_force_release = 1;
            dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
            break;
    }

    rc |= dmr_expect_rejected_return_preserved(grant, flow, script);
    rc |= dmr_drive_to_cc(grant, flow, script);
    rc |= dmr_expect(g_state.trunk_sm_force_release == 0, grant->name, flow->name, script->name,
                     "force release latch cleared");
    return rc;
}

static int
dmr_run_initial_tune_case(const dmr_grant_case* grant, dsd_trunk_tune_result result, const char* result_name) {
    dmr_reset_hooks();
    g_hooks.tune_results[0] = result;
    g_hooks.tune_count = 1;
    dmr_install_hooks();
    dmr_setup_fixture(grant);

    dmr_sm_event_t ev = dmr_case_grant_event(grant, 1201, 0);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);

    int accepted = dsd_trunk_tune_result_is_ok(result);
    int rc = 0;
    rc |= dmr_expect(g_hooks.tune_calls == 1, grant->name, "initial-tune", result_name, "tune attempted");
    if (accepted) {
        rc |= dmr_expect(g_ctx.state == DMR_SM_TUNED, grant->name, "initial-tune", result_name,
                         "accepted tune reached TUNED");
        rc |= dmr_expect(g_opts.trunk_is_tuned == 1, grant->name, "initial-tune", result_name,
                         "accepted tune set tuned flag");
        rc |= dmr_expect(g_state.trunk_vc_freq[0] == dmr_case_expected_freq(grant), grant->name, "initial-tune",
                         result_name, "accepted tune set voice frequency");
        rc |= dmr_expect(g_state.p25_sm_tune_count == 1, grant->name, "initial-tune", result_name,
                         "accepted tune counted");
    } else {
        rc |= dmr_expect(g_ctx.state == DMR_SM_ON_CC, grant->name, "initial-tune", result_name,
                         "rejected tune stayed ON_CC");
        rc |= dmr_expect(g_opts.trunk_is_tuned == 0, grant->name, "initial-tune", result_name,
                         "rejected tune left tuned flag clear");
        rc |= dmr_expect(g_state.trunk_vc_freq[0] == 0 && g_state.trunk_vc_freq[1] == 0, grant->name, "initial-tune",
                         result_name, "rejected tune left voice frequencies clear");
        rc |= dmr_expect(g_state.p25_sm_tune_count == 0, grant->name, "initial-tune", result_name,
                         "rejected tune not counted");
    }
    return rc;
}

static int
dmr_run_terminal_case(const dmr_grant_case* grant, const dmr_flow_case* flow, const dmr_return_script* script) {
    dmr_reset_hooks();
    g_hooks.tune_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_count = 1;
    for (int i = 0; i < script->count; i++) {
        g_hooks.return_results[i] = script->results[i];
    }
    g_hooks.return_count = script->count;
    dmr_install_hooks();
    dmr_setup_fixture(grant);

    int rc = dmr_send_initial_grant(grant, 0, flow->name, script->name);
    if (rc != 0) {
        return rc;
    }
    return dmr_apply_terminal_flow(grant, flow, script);
}

static int
dmr_run_active_without_terminal_case(const dmr_grant_case* grant) {
    const char* flow = "active-without-terminal";
    dmr_reset_hooks();
    g_hooks.tune_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_count = 1;
    g_hooks.return_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.return_count = 1;
    dmr_install_hooks();
    dmr_setup_fixture(grant);

    int rc = dmr_send_initial_grant(grant, 0, flow, "no-return");
    if (rc != 0) {
        return rc;
    }

    dmr_sm_event_t ev = dmr_sm_ev_voice_sync(0);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    g_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);

    rc |= dmr_expect(g_ctx.state == DMR_SM_TUNED, grant->name, flow, "no-return", "active voice remains tuned");
    rc |= dmr_expect(g_opts.trunk_is_tuned == 1, grant->name, flow, "no-return", "active voice keeps tuned flag");
    rc |= dmr_expect(g_hooks.return_calls == 0, grant->name, flow, "no-return", "no return attempted");
    return rc;
}

static int
dmr_run_duplicate_grant_case(const dmr_grant_case* grant) {
    dmr_reset_hooks();
    g_hooks.tune_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_results[1] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_count = 2;
    g_hooks.return_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.return_count = 1;
    dmr_install_hooks();
    dmr_setup_fixture(grant);

    int rc = dmr_send_initial_grant(grant, 0, "duplicate-grant", "ok");
    if (rc != 0) {
        return rc;
    }

    dmr_sm_event_t ev = dmr_case_grant_event(grant, 1201, 0);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_hooks.tune_calls == 1, grant->name, "duplicate-grant", "ok", "duplicate grant did not retune");
    rc |=
        dmr_expect(g_ctx.state == DMR_SM_TUNED, grant->name, "duplicate-grant", "ok", "duplicate grant remained tuned");

    ev = dmr_sm_ev_release(-1);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    dmr_age_hangtime();
    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
    rc |= dmr_drive_to_cc(grant, &(dmr_flow_case){"duplicate-grant", DMR_FLOW_EXPLICIT_RELEASE},
                          &(dmr_return_script){"ok", {DSD_TRUNK_TUNE_RESULT_OK, DSD_TRUNK_TUNE_RESULT_OK}, 1});
    return rc;
}

static int
dmr_run_retune_after_release_case(const dmr_grant_case* grant) {
    dmr_return_script script = {"ok", {DSD_TRUNK_TUNE_RESULT_OK, DSD_TRUNK_TUNE_RESULT_OK}, 1};
    dmr_flow_case flow = {"retune-after-release", DMR_FLOW_GRANT_TIMEOUT};

    dmr_reset_hooks();
    g_hooks.tune_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_results[1] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_count = 2;
    g_hooks.return_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.return_count = 1;
    dmr_install_hooks();
    dmr_setup_fixture(grant);

    int rc = dmr_send_initial_grant(grant, 1, flow.name, script.name);
    if (rc != 0) {
        return rc;
    }
    dmr_age_grant_timeout();
    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
    rc |= dmr_drive_to_cc(grant, &flow, &script);
    if (rc != 0) {
        return rc;
    }

    dmr_sm_event_t ev = dmr_case_grant_event(grant, 1301, 0);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_hooks.tune_calls == 2, grant->name, flow.name, "second-grant", "second grant retuned");
    rc |= dmr_expect(g_ctx.state == DMR_SM_TUNED, grant->name, flow.name, "second-grant", "second grant reached TUNED");
    return rc;
}

static int
dmr_run_untrusted_off_cc_reject_case(void) {
    dmr_grant_case grant = {"untrusted-off-cc", 852012500L, 0, 0, 0};
    dmr_reset_hooks();
    g_hooks.tune_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_results[1] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_count = 2;
    dmr_install_hooks();
    dmr_setup_fixture(&grant);

    int rc = dmr_send_initial_grant(&grant, 0, "untrusted-off-cc", "reject");
    if (rc != 0) {
        return rc;
    }

    g_state.trunk_chan_map[40] = 854012500L;
    g_state.dmr_lcn_trust[40] = 0;
    dmr_sm_event_t ev = dmr_sm_ev_group_grant(0, 40, 1401, 8401);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);

    rc |= dmr_expect(g_hooks.tune_calls == 1, grant.name, "untrusted-off-cc", "reject",
                     "untrusted off-CC LPCN did not retune");
    rc |= dmr_expect(g_ctx.vc_freq_hz == 852012500L && g_state.trunk_vc_freq[0] == 852012500L, grant.name,
                     "untrusted-off-cc", "reject", "original tuned state preserved");
    return rc;
}

static int
dmr_run_data_sync_disabled_case(const dmr_grant_case* grant) {
    dmr_return_script script = {"ok", {DSD_TRUNK_TUNE_RESULT_OK, DSD_TRUNK_TUNE_RESULT_OK}, 1};
    dmr_flow_case flow = {"data-sync-disabled", DMR_FLOW_GRANT_TIMEOUT};

    dmr_reset_hooks();
    g_hooks.tune_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_count = 1;
    g_hooks.return_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.return_count = 1;
    dmr_install_hooks();
    dmr_setup_fixture(grant);
    g_opts.trunk_tune_data_calls = 0;

    int rc = dmr_send_initial_grant(grant, 0, flow.name, script.name);
    if (rc != 0) {
        return rc;
    }

    dmr_sm_event_t ev = dmr_sm_ev_data_sync(1);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_ctx.t_voice_m == 0.0, grant->name, flow.name, script.name,
                     "disabled data sync did not arm hangtime");

    dmr_age_grant_timeout();
    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
    rc |= dmr_drive_to_cc(grant, &flow, &script);
    return rc;
}

static int
dmr_run_cc_loss_reacquire_case(void) {
    const char* flow = "cc-loss-reacquire";
    const char* script = "sync";
    dmr_grant_case grant = {"control-channel", 0, 0, 0, 0};

    dmr_reset_hooks();
    dmr_install_hooks();
    dmr_setup_fixture(&grant);

    g_ctx.t_cc_sync_m = dsd_time_now_monotonic_s() - 1.0;
    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);

    int rc = 0;
    rc |= dmr_expect(g_ctx.state == DMR_SM_HUNTING, grant.name, flow, script, "stale CC enters HUNTING");
    rc |= dmr_expect(g_hooks.tune_calls == 0 && g_hooks.return_calls == 0, grant.name, flow, script,
                     "cc loss does not invoke VC return hooks");

    dmr_sm_event_t ev = dmr_sm_ev_cc_sync();
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_ctx.state == DMR_SM_ON_CC, grant.name, flow, script, "cc sync returns ON_CC");
    return rc;
}

static int
dmr_run_state_name_and_guard_case(void) {
    const char* grant = "api";
    const char* flow = "guards";
    const char* script = "no-op";
    dmr_setup_blank_fixture();

    int rc = 0;
    rc |= dmr_expect(dmr_sm_state_name(DMR_SM_IDLE)[0] == 'I', grant, flow, script, "idle state name");
    rc |= dmr_expect(dmr_sm_state_name(DMR_SM_ON_CC)[0] == 'O', grant, flow, script, "on-cc state name");
    rc |= dmr_expect(dmr_sm_state_name(DMR_SM_TUNED)[0] == 'T', grant, flow, script, "tuned state name");
    rc |= dmr_expect(dmr_sm_state_name(DMR_SM_HUNTING)[0] == 'H', grant, flow, script, "hunting state name");
    rc |= dmr_expect(dmr_sm_state_name((dmr_sm_state_e)99)[0] == '?', grant, flow, script, "unknown state name");

    dmr_sm_init_ctx(NULL, &g_opts, &g_state);
    dmr_sm_event(NULL, &g_opts, &g_state, &(dmr_sm_event_t){.type = DMR_SM_EV_CC_SYNC});
    dmr_sm_event(&g_ctx, &g_opts, &g_state, NULL);
    dmr_sm_tick_ctx(NULL, &g_opts, &g_state);
    dmr_sm_on_neighbor_update(&g_opts, NULL, (const long[]){851012500L}, 1);
    dmr_sm_on_neighbor_update(&g_opts, &g_state, NULL, 1);
    dmr_sm_on_neighbor_update(&g_opts, &g_state, (const long[]){851012500L}, 0);
    rc |= dmr_expect(g_ctx.initialized == 0 && g_ctx.state == DMR_SM_IDLE, grant, flow, script,
                     "guard no-ops preserved blank context");
    return rc;
}

static int
dmr_run_auto_init_and_idle_hunting_case(void) {
    const char* grant = "api";
    const char* flow = "auto-init";
    const char* script = "sync";
    dmr_setup_blank_fixture();
    dmr_install_hooks();

    int rc = 0;
    dmr_sm_event_t ev = {.type = DMR_SM_EV_SYNC_LOST};
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_ctx.initialized == 1 && g_ctx.state == DMR_SM_IDLE, grant, flow, script,
                     "sync-lost auto-inits idle context");

    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
    rc |= dmr_expect(g_ctx.state == DMR_SM_IDLE, grant, flow, script, "idle tick is stable");

    g_ctx.state = DMR_SM_HUNTING;
    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
    rc |= dmr_expect(g_ctx.state == DMR_SM_HUNTING, grant, flow, script, "hunting tick is stable");

    ev = dmr_sm_ev_cc_sync();
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_ctx.state == DMR_SM_ON_CC && g_ctx.t_cc_sync_m > 0.0, grant, flow, script,
                     "cc sync reacquires from hunting");

    g_ctx.t_cc_sync_m = 0.0;
    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
    rc |= dmr_expect(g_ctx.state == DMR_SM_ON_CC, grant, flow, script, "on-cc tick without timestamp is stable");
    return rc;
}

static int
dmr_run_rejected_grant_contracts(void) {
    const char* flow = "rejected-grants";
    const char* script = "no-tune";
    dmr_grant_case grant = {"explicit-disabled", 852012500L, 0, 0, 0};
    dmr_reset_hooks();
    dmr_install_hooks();
    dmr_setup_fixture(&grant);

    g_opts.trunk_enable = 0;
    dmr_sm_event_t ev = dmr_sm_ev_group_grant(grant.freq_hz, 0, 1201, 7201);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);

    int rc = 0;
    rc |= dmr_expect(g_hooks.tune_calls == 0, grant.name, flow, script, "disabled trunking did not tune");
    rc |= dmr_expect(g_ctx.state == DMR_SM_ON_CC && g_opts.trunk_is_tuned == 0, grant.name, flow, script,
                     "disabled trunking stayed on control channel");

    g_opts.trunk_enable = 1;
    g_state.trunk_cc_freq = 0;
    ev = dmr_sm_ev_group_grant(grant.freq_hz, 0, 1202, 7202);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_hooks.tune_calls == 0, grant.name, flow, script, "missing control channel did not tune");

    g_state.trunk_cc_freq = 851012500L;
    ev = dmr_sm_ev_group_grant(0, 0, 1203, 7203);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_hooks.tune_calls == 0, grant.name, flow, script, "unresolved grant did not tune");
    rc |= dmr_expect(g_state.p25_sm_tune_count == 0 && g_ctx.vc_freq_hz == 0, grant.name, flow, script,
                     "rejected grants preserved voice-channel state");
    return rc;
}

static int
dmr_run_data_sync_and_stale_slot_case(void) {
    const char* flow = "data-sync";
    const char* script = "slot-normalize";
    dmr_grant_case grant = {"explicit-frequency", 852012500L, 0, 0, 0};
    dmr_reset_hooks();
    g_hooks.tune_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.tune_count = 1;
    g_hooks.return_results[0] = DSD_TRUNK_TUNE_RESULT_OK;
    g_hooks.return_count = 1;
    dmr_install_hooks();
    dmr_setup_fixture(&grant);

    int rc = dmr_send_initial_grant(&grant, 0, flow, script);
    if (rc != 0) {
        return rc;
    }

    dmr_sm_event_t ev = dmr_sm_ev_data_sync(7);
    dmr_sm_event(&g_ctx, &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_ctx.slots[0].last_active_m > 0.0 && g_ctx.slots[1].last_active_m == 0.0, grant.name, flow,
                     script, "invalid data slot normalized to slot zero");
    rc |= dmr_expect(g_ctx.t_voice_m > 0.0, grant.name, flow, script, "data sync arms hangtime timestamp");
    rc |= dmr_expect_close(g_state.last_vc_sync_time_m, g_ctx.t_voice_m, 1.0e-9, grant.name, flow, script,
                           "data sync records matching voice sync time");

    double stale_m = dsd_time_now_monotonic_s() - 1.0;
    g_ctx.t_tune_m = 0.0;
    g_ctx.t_voice_m = 0.0;
    g_ctx.slots[0].voice_active = 1;
    g_ctx.slots[0].last_active_m = stale_m;
    dmr_sm_tick_ctx(&g_ctx, &g_opts, &g_state);
    rc |= dmr_expect(g_ctx.slots[0].voice_active == 0 && g_ctx.state == DMR_SM_TUNED, grant.name, flow, script,
                     "stale voice slot clears without grant timeout");

    double before = g_ctx.t_voice_m;
    ev = dmr_sm_ev_data_sync(0);
    dmr_sm_event(&g_ctx, NULL, &g_state, &ev);
    rc |= dmr_expect_close(g_ctx.t_voice_m, before, 1.0e-9, grant.name, flow, script,
                           "data sync without opts is ignored");
    return rc;
}

static int
dmr_run_global_emit_and_scan_hook_case(void) {
    const char* grant = "global";
    const char* flow = "emit";
    const char* script = "scan-hook";
    dmr_setup_blank_fixture();
    dmr_install_hooks();
    g_opts.trunk_enable = 1;
    g_opts.trunk_tune_data_calls = 1;
    g_state.trunk_cc_freq = 851012500L;
    dmr_sm_init_ctx(&g_ctx, &g_opts, &g_state);

    dsd_trunk_scan_hooks hooks = {0};
    hooks.dmr_ctx = dmr_hook_scan_ctx;
    dsd_trunk_scan_hooks_set(hooks);

    int rc = 0;
    rc |= dmr_expect(dmr_sm_get_ctx() == &g_ctx, grant, flow, script, "scan hook supplies DMR context");

    dmr_sm_event_t ev = dmr_sm_ev_cc_sync();
    dmr_sm_event(dmr_sm_get_ctx(), &g_opts, &g_state, &ev);
    rc |= dmr_expect(g_ctx.state == DMR_SM_ON_CC && g_ctx.t_cc_sync_m > 0.0, grant, flow, script,
                     "generic emit delivered cc sync");

    dmr_sm_emit_data_sync(&g_opts, &g_state, 1);
    rc |= dmr_expect(g_ctx.slots[1].last_active_m > 0.0, grant, flow, script, "global data-sync emit delivered slot");

    dsd_trunk_scan_hooks_set((dsd_trunk_scan_hooks){0});
    return rc;
}

static int
dmr_run_config_override_case(void) {
    const char* grant = "config";
    const char* flow = "override";
    const char* script = "env";
    dmr_setup_blank_fixture();
    g_opts.trunk_hangtime = 0.25f;
    g_state.trunk_cc_freq = 851012500L;

    int rc = 0;
    rc |= dmr_expect(dsd_setenv("DSD_NEO_DMR_HANGTIME", "1.25", 1) == 0, grant, flow, script, "set hangtime env");
    rc |= dmr_expect(dsd_setenv("DSD_NEO_DMR_GRANT_TIMEOUT", "6.5", 1) == 0, grant, flow, script,
                     "set grant-timeout env");
    dsd_neo_config_init();
    dmr_sm_init_ctx(&g_ctx, &g_opts, &g_state);
    rc |= dmr_expect_close(g_ctx.hangtime_s, 1.25, 1e-9, grant, flow, script, "config hangtime overrides opts");
    rc |= dmr_expect_close(g_ctx.grant_timeout_s, 6.5, 1e-9, grant, flow, script,
                           "config grant timeout overrides default");
    rc |= dmr_expect(g_ctx.state == DMR_SM_ON_CC, grant, flow, script, "config fixture starts on control channel");

    dsd_unsetenv("DSD_NEO_DMR_HANGTIME");
    dsd_unsetenv("DSD_NEO_DMR_GRANT_TIMEOUT");
    dsd_neo_config_init();
    return rc;
}

int
main(void) {
    int rc = 0;

    static const struct {
        const char* name;
        dsd_trunk_tune_result result;
    } initial_results[] = {
        {"ok", DSD_TRUNK_TUNE_RESULT_OK},
        {"pending", DSD_TRUNK_TUNE_RESULT_PENDING},
        {"deferred", DSD_TRUNK_TUNE_RESULT_DEFERRED},
        {"failed", DSD_TRUNK_TUNE_RESULT_FAILED},
        {"timeout", DSD_TRUNK_TUNE_RESULT_TIMEOUT},
    };

    for (size_t g = 0; g < sizeof(g_grants) / sizeof(g_grants[0]); g++) {
        for (size_t r = 0; r < sizeof(initial_results) / sizeof(initial_results[0]); r++) {
            rc |= dmr_run_initial_tune_case(&g_grants[g], initial_results[r].result, initial_results[r].name);
        }
        rc |= dmr_run_active_without_terminal_case(&g_grants[g]);
        rc |= dmr_run_duplicate_grant_case(&g_grants[g]);
        rc |= dmr_run_retune_after_release_case(&g_grants[g]);
        rc |= dmr_run_data_sync_disabled_case(&g_grants[g]);

        for (size_t f = 0; f < sizeof(g_flows) / sizeof(g_flows[0]); f++) {
            for (size_t s = 0; s < sizeof(g_return_scripts) / sizeof(g_return_scripts[0]); s++) {
                rc |= dmr_run_terminal_case(&g_grants[g], &g_flows[f], &g_return_scripts[s]);
            }
        }
    }

    rc |= dmr_run_untrusted_off_cc_reject_case();
    rc |= dmr_run_cc_loss_reacquire_case();
    rc |= dmr_run_state_name_and_guard_case();
    rc |= dmr_run_auto_init_and_idle_hunting_case();
    rc |= dmr_run_rejected_grant_contracts();
    rc |= dmr_run_data_sync_and_stale_slot_case();
    rc |= dmr_run_global_emit_and_scan_hook_case();
    rc |= dmr_run_config_override_case();

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_scan_hooks_set((dsd_trunk_scan_hooks){0});
    if (rc == 0) {
        printf("DMR_T3_SM_RETURN_TO_CC_MATRIX: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
