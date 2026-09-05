// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA trunking state machine.
 * Frequency-hop on MAC channel allocation, with hangtime return to CC.
 *
 * States
 * ------
 *   IDLE   – trunking disabled or no CC known yet
 *   ON_CC  – parked on the CC, waiting for grants
 *   TUNED  – following an active VC (call in progress or hanging)
 *
 * Thread-safety: all calls must come from the single decoder thread.
 */

#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Singleton context
 * ============================================================================ */

typedef struct {
    tetra_sm_state_e state;
    long             vc_freq_hz;   /* freq we last tuned to            */
    uint8_t          vc_slot;      /* timeslot bitmap for that tune    */
    double           t_tune_m;     /* monotonic time of last tune()    */
} tetra_sm_ctx_t;

static tetra_sm_ctx_t g_tetra_sm;  /* zero-init == IDLE */
static double         g_sm_hangtime_s = 0.0; /* 0 = use opts->trunk_hangtime */

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

static inline double
now_m(void)
{
    return dsd_time_now_monotonic_s();
}

static void
sm_log(dsd_opts *opts, const char *msg)
{
    if (opts && opts->verbose > 1 && msg)
        fprintf(stderr, "\n[TETRA SM] %s\n", msg);
}

static const char *
state_name(tetra_sm_state_e s)
{
    switch (s) {
        case TETRA_SM_IDLE:  return "IDLE";
        case TETRA_SM_ON_CC: return "ON_CC";
        case TETRA_SM_TUNED: return "TUNED";
        default:             return "?";
    }
}

static int
do_release(dsd_opts *opts, dsd_state *state)
{
    dsd_trunk_tune_result result = dsd_trunk_tuning_hook_return_to_cc(opts, state, NULL);
    if (!dsd_trunk_tune_result_is_ok(result)) {
        sm_log(opts, result == DSD_TRUNK_TUNE_RESULT_DEFERRED
                         ? "return to CC deferred; keeping TUNED state"
                         : "return to CC failed; keeping TUNED state");
        return 0;
    }

    sm_log(opts, "do_release -> ON_CC");
    g_tetra_sm.state     = TETRA_SM_ON_CC;
    g_tetra_sm.vc_freq_hz = 0;
    g_tetra_sm.vc_slot    = 0;
    g_tetra_sm.t_tune_m   = 0.0;
    return 1;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void
tetra_sm_init(void)
{
    memset(&g_tetra_sm, 0, sizeof(g_tetra_sm));
    g_sm_hangtime_s = 0.0;
    /* g_tetra_sm.state == TETRA_SM_IDLE after memset */
}

tetra_sm_state_e
tetra_sm_get_state(void)
{
    return g_tetra_sm.state;
}

void
tetra_sm_set_hangtime(uint32_t seconds)
{
    g_sm_hangtime_s = (double)seconds;
}

uint32_t
tetra_sm_get_hangtime(void)
{
    return (uint32_t)g_sm_hangtime_s;
}

void
tetra_sm_on_cc_sync(dsd_opts *opts, dsd_state *state)
{
    if (!opts || !state)
        return;

    if (!opts->trunk_enable || state->trunk_cc_freq <= 0) {
        sm_log(opts, "cc_sync ignored: trunking disabled or CC frequency invalid");
        return;
    }

    dsd_trunk_cc_candidates_add(state, state->trunk_cc_freq, 0,
                                DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);

    if (g_tetra_sm.state == TETRA_SM_IDLE) {
        if (opts->verbose > 1)
            fprintf(stderr, "\n[TETRA SM] IDLE -> ON_CC (cc_sync, cc=%ld Hz)\n",
                    state->trunk_cc_freq);
        g_tetra_sm.state = TETRA_SM_ON_CC;
    }
}

void
tetra_sm_on_grant(dsd_opts *opts, dsd_state *state,
                  long vc_freq_hz, uint8_t slot)
{
    if (!opts || !state)
        return;

    /* Guard: trunking must be enabled and a CC must be known */
    if (!opts->trunk_enable || state->trunk_cc_freq == 0) {
        sm_log(opts, "on_grant: trunking disabled or no CC known – ignored");
        return;
    }

    /* If vc_freq_hz is zero fall back to CC (same-carrier grant with no offset) */
    if (vc_freq_hz <= 0) {
        sm_log(opts, "on_grant: vc_freq_hz=0 – staying on CC");
        return;
    }

    /* If already tuned to the same VC, skip redundant tune */
    if (g_tetra_sm.state == TETRA_SM_TUNED &&
        g_tetra_sm.vc_freq_hz == vc_freq_hz)
    {
        /* refresh hangtime even on duplicate grant */
        g_tetra_sm.vc_slot = slot;
        g_tetra_sm.t_tune_m = now_m();
        if (opts->verbose > 1)
            fprintf(stderr, "\n[TETRA SM] on_grant: same VC %ld Hz – refreshing hangtime\n",
                    vc_freq_hz);
        return;
    }

    if (opts->verbose > 0)
        fprintf(stderr, "\n[TETRA SM] %s -> TUNED (grant VC=%ld Hz slot=%u)\n",
                state_name(g_tetra_sm.state), vc_freq_hz, (unsigned)slot);

    /* Tune to VC */
    dsd_trunk_tune_result result =
        dsd_trunk_tuning_hook_tune_to_freq(opts, state, vc_freq_hz,
                                           state->samplesPerSymbol, NULL);
    if (!dsd_trunk_tune_result_is_ok(result)) {
        sm_log(opts, result == DSD_TRUNK_TUNE_RESULT_DEFERRED
                         ? "grant tune deferred; state unchanged"
                         : "grant tune failed; state unchanged");
        return;
    }

    g_tetra_sm.state     = TETRA_SM_TUNED;
    g_tetra_sm.vc_freq_hz = vc_freq_hz;
    g_tetra_sm.vc_slot    = slot;
    g_tetra_sm.t_tune_m   = now_m();
}

void
tetra_sm_on_release(dsd_opts *opts, dsd_state *state)
{
    if (!opts || !state)
        return;

    if (g_tetra_sm.state != TETRA_SM_TUNED) {
        /* Nothing to tear down */
        return;
    }

    if (opts->verbose > 0)
        fprintf(stderr, "\n[TETRA SM] TUNED -> ON_CC (release)\n");

    do_release(opts, state);
}

void
tetra_sm_tick(dsd_opts *opts, dsd_state *state)
{
    if (!opts || !state)
        return;

    if (g_tetra_sm.state != TETRA_SM_TUNED)
        return;

    if (!opts->trunk_enable) {
        sm_log(opts, "trunking disabled while tuned; returning to CC");
        do_release(opts, state);
        return;
    }

    double elapsed = now_m() - g_tetra_sm.t_tune_m;
    double hangtime = (g_sm_hangtime_s > 0.0)
                      ? g_sm_hangtime_s
                      : (double)opts->trunk_hangtime;
    if (elapsed > hangtime) {
        if (opts->verbose > 0)
            fprintf(stderr,
                    "\n[TETRA SM] hangtime %.1f s exceeded (%.1f s) -> release\n",
                    hangtime, elapsed);
        do_release(opts, state);
    }
}
