// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A Phase 2 TDMA voice channel cannot be descrambled until WACN/SYSID/NAC are
 * known, so the trunk SM must defer TDMA grants until those are valid instead
 * of burning the grant-voice timeout on an undecodable carrier. FDMA grants
 * carry no such dependency and must tune regardless. */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_tune_to_freq_calls = 0;
static long g_last_tuned_vc = 0;

static dsd_trunk_tune_result
tune_to_freq_stub(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    g_tune_to_freq_calls++;
    g_last_tuned_vc = freq;
    if (opts) {
        opts->trunk_is_tuned = 1;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_to_freq_stub;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
init_case(dsd_opts* o, dsd_state* s) {
    DSD_MEMSET(o, 0, sizeof(*o));
    DSD_MEMSET(s, 0, sizeof(*s));
    o->trunk_enable = 1;
    o->trunk_tune_group_calls = 1;
    s->p25_cc_freq = 851000000;
    s->trunk_cc_freq = 851000000;
}

static void
setup_tdma_iden(dsd_state* s, int id) {
    s->p25_chan_iden = id;
    s->p25_iden_tdma[id].base_freq = 851000000 / 5;
    s->p25_iden_tdma[id].chan_type = 3;
    s->p25_iden_tdma[id].chan_spac = 100;
    s->p25_iden_tdma[id].trust = 2;
    s->p25_iden_tdma[id].populated = 1;
    s->p25_chan_tdma_explicit[id] = 2;
}

static void
setup_fdma_iden(dsd_state* s, int id) {
    s->p25_chan_iden = id;
    s->p25_iden_fdma[id].base_freq = 851000000 / 5;
    s->p25_iden_fdma[id].chan_type = 1;
    s->p25_iden_fdma[id].chan_spac = 100;
    s->p25_iden_fdma[id].trust = 2;
    s->p25_iden_fdma[id].populated = 1;
    s->p25_chan_tdma_explicit[id] = 1;
}

static void
seed_valid_site(dsd_state* s) {
    s->p2_wacn = 0xBEE00;
    s->p2_sysid = 0x1A2;
    s->p2_cc = 0x293;
}

static p25_sm_event_t
group_grant_event(int channel) {
    p25_sm_event_t ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.type = P25_SM_EV_GRANT;
    ev.channel = channel;
    ev.tg = 1234;
    ev.src = 42;
    ev.is_group = 1;
    return ev;
}

int
main(void) {
    install_hooks();

    const int id = 1;
    const int tdma_ch = (id << 12) | 0x000A;
    const int fdma_ch = (id << 12) | 0x000A;

    // 1) TDMA grant before WACN/SYSID/NAC are known: deferred, nothing tuned.
    static dsd_opts o1;
    static dsd_state s1;
    init_case(&o1, &s1);
    setup_tdma_iden(&s1, id);
    p25_sm_ctx_t ctx1;
    p25_sm_init_ctx(&ctx1, &o1, &s1);
    p25_sm_event_t ev = group_grant_event(tdma_ch);
    g_tune_to_freq_calls = 0;
    p25_sm_event(&ctx1, &o1, &s1, &ev);
    assert(g_tune_to_freq_calls == 0);
    assert(o1.trunk_is_tuned == 0);
    assert(s1.p25_vc_freq[0] == 0);
    assert(ctx1.state != P25_SM_TUNED);
    assert(s1.p25_sm_tune_count == 0);

    // 2) Same grant after the site identity is decoded: tunes normally.
    seed_valid_site(&s1);
    p25_sm_event(&ctx1, &o1, &s1, &ev);
    assert(g_tune_to_freq_calls == 1);
    assert(o1.trunk_is_tuned == 1);
    assert(ctx1.state == P25_SM_TUNED);

    // 3) Placeholder site identity (all-ones broadcast values) is not valid:
    //    still deferred.
    static dsd_opts o2;
    static dsd_state s2;
    init_case(&o2, &s2);
    setup_tdma_iden(&s2, id);
    s2.p2_wacn = 0xFFFFF;
    s2.p2_sysid = 0xFFF;
    s2.p2_cc = 0xFFF;
    p25_sm_ctx_t ctx2;
    p25_sm_init_ctx(&ctx2, &o2, &s2);
    g_tune_to_freq_calls = 0;
    p25_sm_event(&ctx2, &o2, &s2, &ev);
    assert(g_tune_to_freq_calls == 0);
    assert(ctx2.state != P25_SM_TUNED);

    // 4) FDMA grant with an unknown site identity: no descrambler dependency,
    //    tunes immediately.
    static dsd_opts o3;
    static dsd_state s3;
    init_case(&o3, &s3);
    setup_fdma_iden(&s3, id);
    p25_sm_ctx_t ctx3;
    p25_sm_init_ctx(&ctx3, &o3, &s3);
    p25_sm_event_t ev_fdma = group_grant_event(fdma_ch);
    g_tune_to_freq_calls = 0;
    p25_sm_event(&ctx3, &o3, &s3, &ev_fdma);
    assert(g_tune_to_freq_calls == 1);
    assert(ctx3.state == P25_SM_TUNED);

    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
