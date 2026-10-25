// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25p1 chain comparison's ending and the learned modulation it reports (dsd_p25p1_mod_compare). Kept apart from
 * dsd_frame_sync.c so the engine's tune tests link the real functions rather than copies of them. */

#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

/*
 * The decoder thread owns the comparison and the learned modulation; the hunt writes them outside the P25 SM tick
 * guard. A tune made on another thread (the P25 watchdog's) never touches them. The invariant:
 *
 *   An off-thread tune makes one coherent read of the decision the decoder publishes (s_published,
 *   dsd_frame_sync_p25p1_read_decision()), and uses that one read both for the chain it tunes and for the notice it
 *   posts (dsd_frame_sync_note_off_thread_tune()), which carries the read whole. The decoder thread reconciles every
 *   notice, whatever has happened to the comparison since (dsd_frame_sync_apply_off_thread_tunes()): the learned
 *   modulation becomes the chain the tune set, a comparison still running ends there without a verdict, and a verdict
 *   reached since on the comparison the tune captured is dropped.
 *
 * The serialization rule: the watchdog tunes under the P25 SM tick guard, and the decoder takes the same guard
 * (dsd_frame_sync_hook_tick_guard_enter(), re-entrant) around every step that changes the comparison or the chain --
 * opening it, a pass's end and its chain switch, a settle or an abort, a CQPSK trial's handback or a probe's switch, an
 * acquisition reset, and the reconciling below -- and reconciles any notice still pending inside the guard before it
 * acts. So no tune lands between a step's decision and the chain it installs, and a reconciled notice puts rf_mod and
 * the front end back on the tuned chain, which repairs a state already wrong. The guard is taken only on those rare
 * steps; the per-call checks are an atomic load or a phase read, and only while a comparison or a probe is in flight.
 *
 * The decision is one value: bit 2 set while a comparison runs, and the learned modulation
 * (dsd_frame_sync_p25p1_learned_rf_mod(): the leading chain during a comparison) plus one in bits 0-1. The notices are
 * one value too: their count above bit 2, the latest one's decision in bits 0-2. Both are seeded at a run boundary
 * (dsd_frame_sync_off_thread_tunes_reset()).
 */
static atomic_int s_published = 0;
static atomic_int s_off_thread_tunes = 0;
/* The notice count the decoder thread has reconciled up to; only it reads this. */
static unsigned int s_off_thread_tunes_seen = 0U;

#define DECISION_RUNNING 4
#define DECISION_MASK    7

static unsigned int
off_thread_tunes_count(int value) {
    return (unsigned int)value >> 3;
}

static int
decision_learned(int decision) {
    return (decision & 3) - 1;
}

void
dsd_frame_sync_p25p1_mod_compare_publish(const dsd_state* state) {
    if (!state) {
        return;
    }
    const int learned = dsd_frame_sync_p25p1_learned_rf_mod(state);
    const int running = state->p25_p1_mod_compare.phase != 0 ? DECISION_RUNNING : 0;
    atomic_store(&s_published, running | ((learned >= -1 && learned <= 1) ? learned + 1 : 0));
}

void
dsd_frame_sync_p25p1_read_decision(dsd_p25p1_decision* out) {
    if (!out) {
        return;
    }
    const int decision = atomic_load(&s_published);
    out->raw = decision & DECISION_MASK;
    out->running = (decision & DECISION_RUNNING) != 0;
    out->learned = decision_learned(decision);
}

void
frame_sync_p25p1_mod_compare_end(dsd_state* state) {
    state->p25_p1_mod_compare.phase = 0;
    state->p25_p1_mod_probe_active = 0;
    dsd_frame_sync_p25p1_mod_compare_publish(state);
}

void
dsd_frame_sync_note_off_thread_tune(const dsd_p25p1_decision* read) {
    const unsigned int code = read ? (unsigned int)(read->raw & DECISION_MASK) : 0U;
    int expected = atomic_load(&s_off_thread_tunes);
    for (;;) {
        const unsigned int next = ((off_thread_tunes_count(expected) + 1U) << 3) | code;
        if (atomic_compare_exchange_strong(&s_off_thread_tunes, &expected, (int)next)) {
            return;
        }
    }
}

int
dsd_frame_sync_off_thread_tune_pending(void) {
    return off_thread_tunes_count(atomic_load(&s_off_thread_tunes)) != s_off_thread_tunes_seen;
}

void
dsd_frame_sync_off_thread_tunes_reset(const dsd_state* state) {
    s_off_thread_tunes_seen = off_thread_tunes_count(atomic_load(&s_off_thread_tunes));
    if (state) {
        dsd_frame_sync_p25p1_mod_compare_publish(state);
    } else {
        atomic_store(&s_published, 0);
    }
}

int
dsd_frame_sync_apply_off_thread_tunes(const dsd_opts* opts, dsd_state* state) {
    if (!state || !dsd_frame_sync_off_thread_tune_pending()) {
        return 0;
    }
    const int taken = dsd_frame_sync_hook_tick_guard_enter();
    const int tunes = atomic_load(&s_off_thread_tunes);
    if (off_thread_tunes_count(tunes) == s_off_thread_tunes_seen) {
        dsd_frame_sync_hook_tick_guard_leave(taken);
        return 0;
    }
    s_off_thread_tunes_seen = off_thread_tunes_count(tunes);
    /* The carrier the tune left sent the histories: nothing of them pairs with the carrier it reached. */
    dsd_state_forget_sync_join_histories(state);
    /* The latest tune set the decoder's modulation and the front end on the chain it read (the learned modulation of
     * its read): the decoder follows it, whatever the comparison did since. */
    const int chain = decision_learned(tunes);
    dsd_p25p1_mod_compare* cmp = &state->p25_p1_mod_compare;
    const int pass_chain = cmp->phase == 2 ? !cmp->incumbent_rf_mod : cmp->incumbent_rf_mod;
    const int was_running = cmp->phase != 0;
    if (was_running) {
        /* Still running (or opened since): it ends there without a verdict, as the decoder thread's own tunes end
         * one. */
        cmp->phase = 0;
        state->p25_p1_mod_probe_active = 0;
    }
    if ((tunes & DECISION_RUNNING) && chain >= 0 && state->p25_p2_active_slot == -1) {
        /* The tune put the decoder's modulation and the front end on its chain: they are put there again if a step
         * since moved them, which makes this idempotent and repairs a state already wrong; the CQPSK dwell follows
         * the chain. */
        if (state->rf_mod != chain) {
            frame_sync_p25p1_set_chain(opts, state, chain);
        } else if (was_running && pass_chain != chain) {
            frame_sync_qpsk_dwell_set(chain == 1);
        }
    }
    if (tunes & DECISION_RUNNING) {
        /* The tune captured a comparison, which may have reached a verdict since: one measured on the carrier the tune
         * left, which says nothing of the one it reached and would stop that one being compared. Dropped, not
         * rewritten to the tune's chain, which no comparison measured. */
        cmp->verdict_valid = 0;
    }
    state->p25_p1_validated_rf_mod = chain;
    cmp->seen_symbolcnt = state->p25_p1_validated_symbolcnt;
    dsd_frame_sync_p25p1_mod_compare_publish(state);
    dsd_frame_sync_hook_tick_guard_leave(taken);
    return 1;
}

int
frame_sync_p25p1_mod_compare_leader(const dsd_state* state) {
    const dsd_p25p1_mod_compare* cmp = &state->p25_p1_mod_compare;
    return (cmp->phase == 2 && cmp->trial_read > cmp->incumbent_read) ? !cmp->incumbent_rf_mod : cmp->incumbent_rf_mod;
}

int
dsd_frame_sync_p25p1_learned_rf_mod(const dsd_state* state) {
    if (!state) {
        return -1;
    }
    if (state->p25_p1_mod_compare.phase != 0) {
        return frame_sync_p25p1_mod_compare_leader(state);
    }
    return state->p25_p1_validated_rf_mod;
}

void
dsd_frame_sync_p25p1_mod_compare_end_for_tune(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_p25p1_mod_compare* cmp = &state->p25_p1_mod_compare;
    /* The tune leaves the carrier: the stamp of its last frame is consumed with or without a comparison, so the hunt
     * on the carrier reached opens none from it. */
    cmp->seen_symbolcnt = state->p25_p1_validated_symbolcnt;
    if (cmp->phase == 0) {
        return;
    }
    const int leader = frame_sync_p25p1_mod_compare_leader(state);
    state->p25_p1_validated_rf_mod = leader;
    /* The decoder's modulation goes back to the leading chain unless a Phase 2 grant has already claimed it: a TDMA
     * voice channel is CQPSK whatever the control channel's chain, and the grant says so by naming its slot
     * (p25_p2_active_slot) before it asks for the tune. */
    if (state->p25_p2_active_slot == -1 && state->rf_mod != leader) {
        state->rf_mod = leader;
        frame_sync_qpsk_dwell_set(leader == 1);
    }
    frame_sync_p25p1_mod_compare_end(state);
}
