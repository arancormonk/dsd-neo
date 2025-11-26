// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Strong wrapper definitions for the P25 trunking state machine API.
 *
 * This design lets unit tests provide their own strong definitions of
 * p25_sm_* without conflicting with the library: the archive member
 * containing these wrappers is only extracted when the symbols are
 * actually undefined in the final link, preserving test overrides.
 *
 * All calls now dispatch to the unified v2 state machine.
 */

#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm_v2.h>

void
p25_sm_init(dsd_opts* opts, dsd_state* state) {
    p25_sm_v2_init(p25_sm_v2_get(), opts, state);
}

void
p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    p25_sm_event_t ev = p25_sm_ev_group_grant(channel, 0, tg, src, svc_bits);
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, &ev);
}

void
p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    p25_sm_event_t ev = p25_sm_ev_indiv_grant(channel, 0, dst, src, svc_bits);
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, &ev);
}

void
p25_sm_on_release(dsd_opts* opts, dsd_state* state) {
    p25_sm_v2_release(p25_sm_v2_get(), opts, state, "explicit-release");
}

void
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    p25_sm_v2_on_neighbor_update(opts, state, freqs, count);
}

void
p25_sm_tick(dsd_opts* opts, dsd_state* state) {
    p25_sm_v2_tick(p25_sm_v2_get(), opts, state);
}

int
p25_sm_next_cc_candidate(dsd_state* state, long* out_freq) {
    return p25_sm_v2_next_cc_candidate(state, out_freq);
}
