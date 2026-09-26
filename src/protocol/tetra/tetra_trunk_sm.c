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
#include <dsd-neo/protocol/tetra/tetra_mle.h>

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Singleton context
 * ============================================================================ */

typedef struct {
    int    trunk_is_tuned;
    long   p25_vc_freq[2];
    long   trunk_vc_freq[2];
    time_t last_vc_sync_time;
    double last_vc_sync_time_m;
    time_t last_cc_sync_time;
    double last_cc_sync_time_m;
    time_t p25_last_vc_tune_time;
    double p25_last_vc_tune_time_m;
} tetra_sm_tune_snapshot_t;

typedef struct {
    tetra_sm_state_e state;
    tetra_sm_state_e pending_prev_state; /* rollback state for an async tune */
    long             pending_prev_vc_freq_hz;
    uint8_t          pending_prev_vc_slot;
    double           pending_prev_t_tune_m;
    long             vc_freq_hz;   /* freq we last tuned to            */
    uint8_t          vc_slot;      /* timeslot bitmap for that tune    */
    double           t_tune_m;     /* monotonic time of last tune()    */
    uint64_t         pending_request_id;
    uint8_t          pending_kind; /* 1 = grant to VC, 2 = return to CC */
    tetra_sm_tune_snapshot_t pending_snapshot;
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

static void
set_state(dsd_state *state, tetra_sm_state_e next)
{
    g_tetra_sm.state = next;
    if (state)
        state->tetra_trunk_state = (uint8_t)next;
}

static tetra_sm_tune_snapshot_t
capture_tune_snapshot(const dsd_opts *opts, const dsd_state *state)
{
    tetra_sm_tune_snapshot_t snapshot = {0};
    if (!opts || !state)
        return snapshot;
    snapshot.trunk_is_tuned = opts->trunk_is_tuned;
    memcpy(snapshot.p25_vc_freq, state->p25_vc_freq, sizeof(snapshot.p25_vc_freq));
    memcpy(snapshot.trunk_vc_freq, state->trunk_vc_freq, sizeof(snapshot.trunk_vc_freq));
    snapshot.last_vc_sync_time = state->last_vc_sync_time;
    snapshot.last_vc_sync_time_m = state->last_vc_sync_time_m;
    snapshot.last_cc_sync_time = state->last_cc_sync_time;
    snapshot.last_cc_sync_time_m = state->last_cc_sync_time_m;
    snapshot.p25_last_vc_tune_time = state->p25_last_vc_tune_time;
    snapshot.p25_last_vc_tune_time_m = state->p25_last_vc_tune_time_m;
    return snapshot;
}

static void
restore_tune_snapshot(dsd_opts *opts, dsd_state *state,
                      const tetra_sm_tune_snapshot_t *snapshot)
{
    if (!opts || !state || !snapshot)
        return;
    opts->trunk_is_tuned = snapshot->trunk_is_tuned;
    memcpy(state->p25_vc_freq, snapshot->p25_vc_freq, sizeof(snapshot->p25_vc_freq));
    memcpy(state->trunk_vc_freq, snapshot->trunk_vc_freq, sizeof(snapshot->trunk_vc_freq));
    state->last_vc_sync_time = snapshot->last_vc_sync_time;
    state->last_vc_sync_time_m = snapshot->last_vc_sync_time_m;
    state->last_cc_sync_time = snapshot->last_cc_sync_time;
    state->last_cc_sync_time_m = snapshot->last_cc_sync_time_m;
    state->p25_last_vc_tune_time = snapshot->p25_last_vc_tune_time;
    state->p25_last_vc_tune_time_m = snapshot->p25_last_vc_tune_time_m;
}

static void
stamp_completed_vc_tune(dsd_state *state, double completed_m)
{
    if (!state)
        return;
    const time_t completed_wall = time(NULL);
    const double completed_monotonic = completed_m > 0.0 ? completed_m : now_m();
    state->last_vc_sync_time = completed_wall;
    state->last_vc_sync_time_m = completed_monotonic;
    state->last_cc_sync_time = completed_wall;
    state->last_cc_sync_time_m = completed_monotonic;
    state->p25_last_vc_tune_time = completed_wall;
    state->p25_last_vc_tune_time_m = completed_monotonic;
}

static int
settle_pending(dsd_opts *opts, dsd_state *state)
{
    if (g_tetra_sm.pending_request_id == 0U)
        return 1;

    double completed_m = 0.0;
    const uint64_t request_id = g_tetra_sm.pending_request_id;
    const uint8_t kind = g_tetra_sm.pending_kind;
    const dsd_trunk_tune_result result =
        dsd_trunk_tuning_request_status(request_id, &completed_m);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING)
        return 0;

    g_tetra_sm.pending_request_id = 0U;
    g_tetra_sm.pending_kind = 0;
    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        if (kind == 1) {
            if (opts)
                opts->trunk_is_tuned = 1;
            stamp_completed_vc_tune(state, completed_m);
            g_tetra_sm.t_tune_m = completed_m > 0.0 ? completed_m : now_m();
            sm_log(opts, "asynchronous VC tune completed");
        } else {
            if (opts)
                opts->trunk_is_tuned = 0;
            g_tetra_sm.vc_freq_hz = 0;
            g_tetra_sm.vc_slot = 0;
            g_tetra_sm.t_tune_m = 0.0;
            sm_log(opts, "asynchronous return to CC completed");
        }
        return 1;
    }

    /* The hook wrapper marked the staged state ready. A failed backend keeps
     * the process-wide frame gate closed until its owner explicitly rolls the
     * decoder state back with the same terminal result. */
    dsd_trunk_tuning_request_complete(request_id, result);
    set_state(state, g_tetra_sm.pending_prev_state);
    restore_tune_snapshot(opts, state, &g_tetra_sm.pending_snapshot);
    if (kind == 1) {
        g_tetra_sm.vc_freq_hz = g_tetra_sm.pending_prev_vc_freq_hz;
        g_tetra_sm.vc_slot = g_tetra_sm.pending_prev_vc_slot;
        g_tetra_sm.t_tune_m = g_tetra_sm.pending_prev_t_tune_m;
        sm_log(opts, "asynchronous VC tune failed; state rolled back");
    } else {
        sm_log(opts, "asynchronous return to CC failed; keeping TUNED state");
    }
    return 1;
}

static int
do_release(dsd_opts *opts, dsd_state *state)
{
    const tetra_sm_state_e next =
        opts && opts->trunk_enable && state && state->trunk_cc_freq > 0
            ? TETRA_SM_ON_CC : TETRA_SM_IDLE;
    if (g_tetra_sm.vc_freq_hz == state->trunk_cc_freq &&
        g_tetra_sm.vc_freq_hz > 0) {
        /* The traffic slot was on the main carrier. Keep the receiver and
         * demodulator synchronized while switching back to control traffic. */
        opts->trunk_is_tuned = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        set_state(state, next);
        g_tetra_sm.vc_freq_hz = 0;
        g_tetra_sm.vc_slot = 0;
        g_tetra_sm.t_tune_m = 0.0;
        return 1;
    }
    uint64_t request_id = 0U;
    const tetra_sm_tune_snapshot_t snapshot = capture_tune_snapshot(opts, state);
    dsd_trunk_tune_result result =
        dsd_trunk_tuning_hook_return_to_cc(opts, state, &request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
        sm_log(opts, result == DSD_TRUNK_TUNE_RESULT_DEFERRED
                         ? "return to CC deferred; keeping TUNED state"
                         : "return to CC failed; keeping TUNED state");
        return 0;
    }

    set_state(state, next);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        g_tetra_sm.pending_prev_state = TETRA_SM_TUNED;
        g_tetra_sm.pending_request_id = request_id;
        g_tetra_sm.pending_kind = 2;
        g_tetra_sm.pending_snapshot = snapshot;
        sm_log(opts, next == TETRA_SM_ON_CC
                         ? "return to CC pending"
                         : "trunk disable cleanup pending");
        return 1;
    }

    sm_log(opts, next == TETRA_SM_ON_CC ? "do_release -> ON_CC" : "do_release -> IDLE");
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
    tetra_sds_concat_reset();
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

    state->tetra_trunk_state = (uint8_t)g_tetra_sm.state;

    if (!settle_pending(opts, state))
        return;

    if (!opts->trunk_enable || state->trunk_cc_freq <= 0) {
        sm_log(opts, "cc_sync ignored: trunking disabled or CC frequency invalid");
        return;
    }

    dsd_trunk_cc_candidates_add(state, state->trunk_cc_freq, 0,
                                DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);

    if (g_tetra_sm.state == TETRA_SM_TUNED && !opts->trunk_is_tuned) {
        /* The generic return path already moved the receiver. Reconcile
         * on confirmed CC sync without issuing another hardware request. */
        memset(&g_tetra_sm, 0, sizeof(g_tetra_sm));
    }

    if (g_tetra_sm.state == TETRA_SM_IDLE) {
        if (opts->verbose > 1)
            fprintf(stderr, "\n[TETRA SM] IDLE -> ON_CC (cc_sync, cc=%ld Hz)\n",
                    state->trunk_cc_freq);
        set_state(state, TETRA_SM_ON_CC);
    }
}

void
tetra_sm_on_grant(dsd_opts *opts, dsd_state *state,
                  long vc_freq_hz, uint8_t slot)
{
    if (!opts || !state)
        return;

    state->tetra_trunk_state = (uint8_t)g_tetra_sm.state;

    if (!settle_pending(opts, state)) {
        sm_log(opts, "on_grant: tune transition already pending");
        return;
    }

    /* Guard: trunking must be enabled and a CC must be known */
    if (!opts->trunk_enable || state->trunk_cc_freq <= 0) {
        sm_log(opts, "on_grant: trunking disabled or no CC known – ignored");
        return;
    }

    /* If vc_freq_hz is zero fall back to CC (same-carrier grant with no offset) */
    if (vc_freq_hz <= 0) {
        sm_log(opts, "on_grant: vc_freq_hz=0 – staying on CC");
        return;
    }

    /* Generic no-carrier handling can return to CC independently of this
     * singleton. Require the shared tuning state before suppressing a tune. */
    if (g_tetra_sm.state == TETRA_SM_TUNED &&
        opts->trunk_is_tuned == 1 &&
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

    if (vc_freq_hz == state->trunk_cc_freq) {
        /* A main-carrier traffic slot needs slot gating, not an RTL retune.
         * Retuning the same frequency resets timing and can lose the call. */
        opts->trunk_is_tuned = 1;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = vc_freq_hz;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = vc_freq_hz;
        stamp_completed_vc_tune(state, now_m());
        set_state(state, TETRA_SM_TUNED);
        g_tetra_sm.vc_freq_hz = vc_freq_hz;
        g_tetra_sm.vc_slot = slot;
        g_tetra_sm.t_tune_m = now_m();
        return;
    }

    /* Tune to VC */
    const tetra_sm_state_e previous = g_tetra_sm.state;
    const long previous_vc_freq_hz = g_tetra_sm.vc_freq_hz;
    const uint8_t previous_vc_slot = g_tetra_sm.vc_slot;
    const double previous_t_tune_m = g_tetra_sm.t_tune_m;
    const tetra_sm_tune_snapshot_t snapshot = capture_tune_snapshot(opts, state);
    uint64_t request_id = 0U;
    dsd_trunk_tune_result result =
        dsd_trunk_tuning_hook_tune_to_freq(opts, state, vc_freq_hz,
                                           state->samplesPerSymbol, &request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
        sm_log(opts, result == DSD_TRUNK_TUNE_RESULT_DEFERRED
                         ? "grant tune deferred; state unchanged"
                         : "grant tune failed; state unchanged");
        return;
    }

    set_state(state, TETRA_SM_TUNED);
    g_tetra_sm.vc_freq_hz = vc_freq_hz;
    g_tetra_sm.vc_slot    = slot;
    g_tetra_sm.t_tune_m   = now_m();
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        g_tetra_sm.pending_prev_state = previous;
        g_tetra_sm.pending_prev_vc_freq_hz = previous_vc_freq_hz;
        g_tetra_sm.pending_prev_vc_slot = previous_vc_slot;
        g_tetra_sm.pending_prev_t_tune_m = previous_t_tune_m;
        g_tetra_sm.pending_request_id = request_id;
        g_tetra_sm.pending_kind = 1;
        g_tetra_sm.pending_snapshot = snapshot;
    }
}

void
tetra_sm_on_release(dsd_opts *opts, dsd_state *state)
{
    if (!opts || !state)
        return;

    state->tetra_trunk_state = (uint8_t)g_tetra_sm.state;

    if (!settle_pending(opts, state))
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
tetra_sm_on_external_cc_return(dsd_state *state)
{
    if (state) {
        state->tetra_call_active = 0;
        state->tetra_tx_granted_valid = 0;
        state->tetra_tx_granted_ssi = 0;
        state->tetra_cmce_tx_granted_notification_valid = 0;
        state->tetra_cmce_tx_granted_party_type_valid = 0;
        state->tetra_cmce_tx_granted_party_extension_valid = 0;
        state->tetra_tx_continue = 0;
        state->tetra_tx_interrupted = 0;
        state->tetra_tx_wait = 0;
        state->tetra_tx_event_notification_valid = 0;
        state->tetra_tx_event_party_type_valid = 0;
        state->tetra_tx_event_party_ssi_valid = 0;
        state->tetra_tx_event_party_extension_valid = 0;
        state->tetra_vc_assignment_valid = 0;
        state->tetra_vc_assignment_type = 0;
        state->tetra_vc_timeslot_bitmap = 0;
        state->tetra_vc_slot = 0;
        state->tetra_vc_carrier = 0;
        state->tetra_vc_freq_hz = 0;
    }
    set_state(state, state && state->trunk_cc_freq > 0 ? TETRA_SM_ON_CC : TETRA_SM_IDLE);
    g_tetra_sm.vc_freq_hz = 0;
    g_tetra_sm.vc_slot = 0;
    g_tetra_sm.t_tune_m = 0.0;
    g_tetra_sm.pending_request_id = 0U;
    g_tetra_sm.pending_kind = 0;
}

void
tetra_sm_poll_tuning(dsd_opts *opts, dsd_state *state)
{
    if (!opts || !state)
        return;
    state->tetra_trunk_state = (uint8_t)g_tetra_sm.state;
    (void)settle_pending(opts, state);
}

void
tetra_sm_tick(dsd_opts *opts, dsd_state *state)
{
    if (!opts || !state)
        return;

    state->tetra_trunk_state = (uint8_t)g_tetra_sm.state;

    if (!settle_pending(opts, state))
        return;

    if (g_tetra_sm.state != TETRA_SM_TUNED)
        return;

    if (!opts->trunk_enable) {
        sm_log(opts, "trunking disabled while tuned; returning to CC");
        do_release(opts, state);
        return;
    }

    /* Hangtime is the idle dwell after a call, not a maximum call duration.
     * D-SETUP/D-CONNECT mark the call active after the MAC allocation has
     * tuned us, and D-RELEASE/D-DISCONNECT return immediately through
     * tetra_sm_on_release().  Keep the timer fresh while call control still
     * says the traffic channel is in use so a long call cannot be cut off. */
    if (state->tetra_call_active) {
        g_tetra_sm.t_tune_m = now_m();
        return;
    }

    double elapsed = now_m() - g_tetra_sm.t_tune_m;
    double hangtime = (g_sm_hangtime_s > 0.0)
                      ? g_sm_hangtime_s
                      : (double)opts->trunk_hangtime;
    if (elapsed >= hangtime) {
        if (opts->verbose > 0)
            fprintf(stderr,
                    "\n[TETRA SM] hangtime %.1f s exceeded (%.1f s) -> release\n",
                    hangtime, elapsed);
        do_release(opts, state);
    }
}
