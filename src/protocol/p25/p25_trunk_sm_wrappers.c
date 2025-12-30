// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Legacy/compatibility wrapper APIs for the unified P25 trunking state machine.
 *
 * These entry points are intentionally weak on ELF/Mach-O so unit tests can
 * override them. On COFF targets (MSVC/MinGW), keep them strong and isolate
 * them in their own translation unit so tests that provide their own
 * definitions do not pull these from static archives.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include <string.h>

#if defined(_MSC_VER)
#define P25_WEAK_WRAPPER
#elif defined(__MINGW32__) || defined(__MINGW64__)
#define P25_WEAK_WRAPPER
#else
#define P25_WEAK_WRAPPER __attribute__((weak))
#endif

static inline double
now_monotonic(void) {
    return dsd_time_now_monotonic_s();
}

/* ============================================================================
 * Neighbor Update and CC Candidate Functions
 * ============================================================================ */

P25_WEAK_WRAPPER void
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    if (count <= 0 || !state || !freqs) {
        return;
    }
    // Lazy-load any persisted candidates once system identity is known
    p25_cc_try_load_cache(opts, state);

    for (int i = 0; i < count; i++) {
        long f = freqs[i];
        if (f == 0) {
            continue;
        }
        // Track neighbor list for UI
        p25_nb_add(state, f);
        // Add to candidate list (dedup + FIFO rollover)
        (void)p25_cc_add_candidate(state, f, 1);
    }
}

P25_WEAK_WRAPPER int
p25_sm_next_cc_candidate(dsd_state* state, long* out_freq) {
    if (!state || !out_freq) {
        return 0;
    }
    double nowm = now_monotonic();
    for (int tries = 0; tries < state->p25_cc_cand_count; tries++) {
        if (state->p25_cc_cand_idx >= state->p25_cc_cand_count) {
            state->p25_cc_cand_idx = 0;
        }
        int idx = state->p25_cc_cand_idx++;
        long f = state->p25_cc_candidates[idx];
        if (f != 0 && f != state->p25_cc_freq) {
            // Skip candidates currently in cooldown
            double cool_until = (idx >= 0 && idx < 16) ? state->p25_cc_cand_cool_until[idx] : 0.0;
            if (cool_until > 0.0 && nowm < cool_until) {
                continue;
            }
            *out_freq = f;
            state->p25_cc_cand_used++;
            return 1;
        }
    }
    return 0;
}

/* ============================================================================
 * Legacy Compatibility Wrappers
 * These use weak symbols (where supported) to allow tests to override them.
 * ============================================================================ */

P25_WEAK_WRAPPER void
p25_sm_init(dsd_opts* opts, dsd_state* state) {
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, state);
}

P25_WEAK_WRAPPER void
p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    p25_sm_event_t ev = p25_sm_ev_group_grant(channel, 0, tg, src, svc_bits);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

P25_WEAK_WRAPPER void
p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    p25_sm_event_t ev = p25_sm_ev_indiv_grant(channel, 0, dst, src, svc_bits);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

P25_WEAK_WRAPPER void
p25_sm_on_release(dsd_opts* opts, dsd_state* state) {
    p25_sm_release(p25_sm_get_ctx(), opts, state, "explicit-release");
}

P25_WEAK_WRAPPER void
p25_sm_tick(dsd_opts* opts, dsd_state* state) {
    p25_sm_tick_ctx(p25_sm_get_ctx(), opts, state);
}
