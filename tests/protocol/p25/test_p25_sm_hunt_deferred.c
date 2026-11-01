// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #666 (B5): a control-channel hunt probe whose tune is deferred schedules no retune and must be retried without
 * advancing state (trunk_tuning_hooks.h). The hunt picked its entry -- the next current-site candidate, or the next
 * row of the user's channel list -- before it tuned, so a deferral passed the entry. It now retries the same entry; a
 * refused tune still passes it.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static long g_tuned[8];
static int g_tunes;
static dsd_trunk_tune_result g_result = DSD_TRUNK_TUNE_RESULT_OK;

static dsd_trunk_tune_result
hunt_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    if (g_tunes < (int)(sizeof g_tuned / sizeof g_tuned[0])) {
        g_tuned[g_tunes] = freq;
    }
    g_tunes++;
    return g_result;
}

static void
init_hunt(dsd_opts* o, dsd_state* s) {
    DSD_MEMSET(o, 0, sizeof(*o));
    DSD_MEMSET(s, 0, sizeof(*s));
    o->trunk_enable = 1;
    o->trunk_hangtime = 0.2f;
    s->p25_cc_freq = 851000000;
    g_tunes = 0;
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_cc_request = hunt_tune_to_cc});
    p25_sm_init_ctx(p25_sm_get_ctx(), o, s);
    s->last_cc_sync_time_m = dsd_decode_now_mono_s() - 10.0; // the control channel is lost: the next tick hunts
}

/* One hunt probe now: the hunt interval since the last one has run out. */
static void
hunt_once(dsd_opts* o, dsd_state* s, dsd_trunk_tune_result result) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    g_result = result;
    ctx->t_hunt_try_m = 0.0;
    const int tunes = g_tunes;
    p25_sm_tick_ctx(ctx, o, s);
    assert(g_tunes == tunes + 1);
    assert(ctx->state == P25_SM_HUNTING);
}

static void
test_deferred_candidate_is_retried(void) {
    static dsd_opts o;
    static dsd_state s;
    const long a = 852000000;
    const long b = 853000000;
    init_hunt(&o, &s);
    o.p25_prefer_candidates = 1;
    (void)dsd_trunk_cc_candidates_add(&s, a, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    (void)dsd_trunk_cc_candidates_add(&s, b, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(&s);
    assert(cc != NULL);

    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(g_tuned[0] == a);
    assert(cc->idx == 0 && cc->used == 0U);

    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(g_tuned[1] == a); // the same candidate, not B
    assert(cc->idx == 0 && cc->used == 0U);

    // A refusal passes the candidate, as before.
    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_tuned[2] == a);
    assert(cc->idx == 1 && cc->used == 1U);
    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_tuned[3] == b);
    assert(cc->idx == 2 && cc->used == 2U);
}

static void
test_deferred_channel_row_is_retried(void) {
    static dsd_opts o;
    static dsd_state s;
    init_hunt(&o, &s);
    DSD_SNPRINTF(o.chan_in_file, sizeof(o.chan_in_file), "%s", "site.csv");
    s.trunk_lcn_freq[0] = 851000000;
    s.trunk_lcn_freq[1] = 852000000;
    s.trunk_lcn_freq[2] = 853000000;
    s.lcn_freq_count = 3;
    s.lcn_freq_roll = 1;

    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(g_tuned[0] == 852000000);
    assert(s.lcn_freq_roll == 1);

    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(g_tuned[1] == 852000000); // the same row, not the next one
    assert(s.lcn_freq_roll == 1);

    // A refusal passes the row, as before.
    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_tuned[2] == 852000000);
    assert(s.lcn_freq_roll == 2);
    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_tuned[3] == 853000000);
    assert(s.lcn_freq_roll == 3);
}

/* One hunt probe now whose tune is accepted: the SM moves on to the control channel it tuned. */
static void
hunt_accepted(dsd_opts* o, dsd_state* s) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    ctx->state = P25_SM_HUNTING;
    ctx->t_hunt_try_m = 0.0;
    s->last_cc_sync_time_m = dsd_decode_now_mono_s() - 10.0;
    const int tunes = g_tunes;
    p25_sm_tick_ctx(ctx, o, s);
    assert(g_tunes == tunes + 1);
    assert(ctx->state == P25_SM_ON_CC);
}

/* Review gate G4: an accepted channel-list probe is the row heard, as a manual step's landing is, so Previous goes back
   from the row the hunt tuned, not from the last row a manual step landed. The row is the one tuned, past a duplicate
   and across the wrap; a 0 Hz row hands the probe to the control channel, which records no row. */
static void
test_accepted_channel_row_is_the_row_heard(void) {
    static dsd_opts o;
    static dsd_state s;
    init_hunt(&o, &s);
    DSD_SNPRINTF(o.chan_in_file, sizeof(o.chan_in_file), "%s", "site.csv");
    s.trunk_lcn_freq[0] = 852000000;
    s.trunk_lcn_freq[1] = 853000000;
    s.trunk_lcn_freq[2] = 853000000;
    s.trunk_lcn_freq[3] = 0;
    s.lcn_freq_count = 4;
    s.lcn_freq_roll = 1;
    dsd_state_lcn_nav_reset(&s);
    dsd_state_lcn_on_air_note(&s, 0); // Next landed row 0
    dsd_state_lcn_back_retry_note(&s, 0);
    assert(dsd_state_lcn_heard_row(&s) == 0);

    hunt_accepted(&o, &s);
    assert(g_tuned[0] == 853000000);
    assert(s.lcn_freq_roll == 2);
    assert(dsd_state_lcn_heard_row(&s) == 1);
    assert(dsd_state_lcn_back_retry_row(&s) == -1); // a forward start drops a refused Previous's retry

    // Row 2 repeats row 1 and row 3 is a placeholder: the control channel is tuned and row 1 stays the row heard.
    hunt_accepted(&o, &s);
    assert(g_tuned[1] == 851000000);
    assert(s.lcn_freq_roll == 4);
    assert(dsd_state_lcn_heard_row(&s) == 1);

    // Past the end the walk wraps to row 0.
    hunt_accepted(&o, &s);
    assert(g_tuned[2] == 852000000);
    assert(s.lcn_freq_roll == 1);
    assert(dsd_state_lcn_heard_row(&s) == 0);
}

/* Review gate fix 3: the rest of a manual Next's bookkeeping. A refused channel-list probe passes its row, which the
   receiver was never on, so with no row recorded the row heard is none, not the refused row before the cursor; a
   recorded row stays. Every channel-list probe is a forward step and drops a refused Previous's retry row, deferred
   or not. */
static void
test_refused_channel_row_is_not_the_row_heard(void) {
    static dsd_opts o;
    static dsd_state s;
    init_hunt(&o, &s);
    DSD_SNPRINTF(o.chan_in_file, sizeof(o.chan_in_file), "%s", "site.csv");
    s.trunk_lcn_freq[0] = 852000000;
    s.trunk_lcn_freq[1] = 853000000;
    s.trunk_lcn_freq[2] = 854000000;
    s.trunk_lcn_freq[3] = 855000000;
    s.lcn_freq_count = 4;
    s.lcn_freq_roll = 0;
    dsd_state_lcn_nav_reset(&s);

    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_tuned[0] == 852000000);
    assert(s.lcn_freq_roll == 1);
    assert(dsd_state_lcn_heard_row(&s) == -1); // not row 0, which was refused

    // A recorded row stays through a refusal, and each probe drops the retry, a deferred one included.
    dsd_state_lcn_on_air_note(&s, 0);
    dsd_state_lcn_back_retry_note(&s, 3);
    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_tuned[1] == 853000000);
    assert(s.lcn_freq_roll == 2);
    assert(dsd_state_lcn_heard_row(&s) == 0);
    assert(dsd_state_lcn_back_retry_row(&s) == -1);

    dsd_state_lcn_back_retry_note(&s, 3);
    hunt_once(&o, &s, DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(g_tuned[2] == 854000000);
    assert(s.lcn_freq_roll == 2);
    assert(dsd_state_lcn_heard_row(&s) == 0);
    assert(dsd_state_lcn_back_retry_row(&s) == -1);
}

int
main(void) {
    test_deferred_candidate_is_retried();
    test_deferred_channel_row_is_retried();
    test_accepted_channel_row_is_the_row_heard();
    test_refused_channel_row_is_not_the_row_heard();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    return 0;
}
