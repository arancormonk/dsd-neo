// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25p1 chain comparison's ending and the learned modulation it reports (dsd_p25p1_mod_compare). Kept apart from
 * dsd_frame_sync.c so the engine's tune tests link the real functions rather than copies of them. */

#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/atomic_compat.h>

#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

/* The decoder thread owns the comparison and the learned modulation, which the hunt writes outside the P25 SM tick
 * guard. A tune another thread makes (the P25 watchdog's) reads them from here instead, as one value so the two agree:
 * bit 2 set while a comparison runs, and the learned modulation (dsd_frame_sync_p25p1_learned_rf_mod(): the leading
 * chain during a comparison) plus one in the low bits. It leaves the comparison's ending, and the sync join histories'
 * clearing, to the decoder thread (s_off_thread_tunes). Nothing learned and no comparison until the first publish. */
static atomic_int s_published = 0;
/* Tunes made off the decoder thread, counted, with the chain the latest one landed a comparison on, as one value: the
 * count above the low two bits, which hold that chain plus one (0: none). The decoder thread applies them
 * (dsd_frame_sync_apply_off_thread_tunes()) up to the count it last saw, which only it reads. */
static atomic_int s_off_thread_tunes = 0;
static unsigned int s_off_thread_tunes_seen = 0U;

static unsigned int
off_thread_tunes_count(int value) {
    return (unsigned int)value >> 2;
}

void
dsd_frame_sync_p25p1_mod_compare_publish(const dsd_state* state) {
    if (!state) {
        return;
    }
    const int learned = dsd_frame_sync_p25p1_learned_rf_mod(state);
    const int running = state->p25_p1_mod_compare.phase != 0 ? 4 : 0;
    atomic_store(&s_published, running | ((learned >= -1 && learned <= 1) ? learned + 1 : 0));
}

int
dsd_frame_sync_p25p1_mod_compare_leading_rf_mod(void) {
    const int published = atomic_load(&s_published);
    return (published & 4) ? (published & 3) - 1 : -1;
}

int
dsd_frame_sync_p25p1_published_learned_rf_mod(void) {
    return (atomic_load(&s_published) & 3) - 1;
}

void
frame_sync_p25p1_mod_compare_end(dsd_state* state) {
    state->p25_p1_mod_compare.phase = 0;
    state->p25_p1_mod_probe_active = 0;
    dsd_frame_sync_p25p1_mod_compare_publish(state);
}

void
dsd_frame_sync_note_off_thread_tune(int chain) {
    const unsigned int code = (chain == 0 || chain == 1) ? (unsigned int)chain + 1U : 0U;
    int expected = atomic_load(&s_off_thread_tunes);
    for (;;) {
        const unsigned int next = ((off_thread_tunes_count(expected) + 1U) << 2) | code;
        if (atomic_compare_exchange_strong(&s_off_thread_tunes, &expected, (int)next)) {
            return;
        }
    }
}

int
dsd_frame_sync_off_thread_tune_pending(void) {
    return off_thread_tunes_count(atomic_load(&s_off_thread_tunes)) != s_off_thread_tunes_seen;
}

int
dsd_frame_sync_apply_off_thread_tunes(dsd_state* state) {
    if (!state) {
        return 0;
    }
    const int tunes = atomic_load(&s_off_thread_tunes);
    if (off_thread_tunes_count(tunes) == s_off_thread_tunes_seen) {
        return 0;
    }
    s_off_thread_tunes_seen = off_thread_tunes_count(tunes);
    /* The carrier the tune left sent the histories: nothing of them pairs with the carrier it reached. */
    dsd_state_forget_sync_join_histories(state);
    dsd_p25p1_mod_compare* cmp = &state->p25_p1_mod_compare;
    if (cmp->phase != 0) {
        /* The latest tune read the leading chain (dsd_frame_sync_p25p1_mod_compare_leading_rf_mod()) and landed on it,
         * setting the decoder's modulation itself: the comparison ends on that chain, as one the decoder thread's tune
         * ends, and the learned modulation goes back to it, though the comparison has counted frames since that put
         * the other chain in front. A tune that found no comparison to read names none: it ends on the leader. */
        const int tuned = (tunes & 3) - 1;
        const int leader = tuned >= 0 ? tuned : frame_sync_p25p1_mod_compare_leader(state);
        const int pass_chain = cmp->phase == 2 ? !cmp->incumbent_rf_mod : cmp->incumbent_rf_mod;
        state->p25_p1_validated_rf_mod = leader;
        cmp->seen_symbolcnt = state->p25_p1_validated_symbolcnt;
        if (state->p25_p2_active_slot == -1 && pass_chain != leader) {
            frame_sync_qpsk_dwell_set(leader == 1);
        }
        frame_sync_p25p1_mod_compare_end(state);
    }
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
    if (cmp->phase == 0) {
        return;
    }
    const int leader = frame_sync_p25p1_mod_compare_leader(state);
    state->p25_p1_validated_rf_mod = leader;
    cmp->seen_symbolcnt = state->p25_p1_validated_symbolcnt;
    /* The decoder's modulation goes back to the leading chain unless a Phase 2 grant has already claimed it: a TDMA
     * voice channel is CQPSK whatever the control channel's chain, and the grant says so by naming its slot
     * (p25_p2_active_slot) before it asks for the tune. */
    if (state->p25_p2_active_slot == -1 && state->rf_mod != leader) {
        state->rf_mod = leader;
        frame_sync_qpsk_dwell_set(leader == 1);
    }
    frame_sync_p25p1_mod_compare_end(state);
}
