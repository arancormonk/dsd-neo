// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Strong wrapper definitions for the P25 trunking state machine API.
 * These forward to implementation symbols in p25_trunk_sm.c.
 *
 * This design lets unit tests provide their own strong definitions of
 * p25_sm_* without conflicting with the library: the archive member
 * containing these wrappers is only extracted when the symbols are
 * actually undefined in the final link, preserving test overrides.
 */

#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

// Forward declarations of implementation functions
extern void dsd_p25_sm_init_impl(dsd_opts* opts, dsd_state* state);
extern void dsd_p25_sm_on_group_grant_impl(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg,
                                           int src);
extern void dsd_p25_sm_on_indiv_grant_impl(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst,
                                           int src);
extern void dsd_p25_sm_on_release_impl(dsd_opts* opts, dsd_state* state);
extern void dsd_p25_sm_on_neighbor_update_impl(dsd_opts* opts, dsd_state* state, const long* freqs, int count);
extern void dsd_p25_sm_tick_impl(dsd_opts* opts, dsd_state* state);
extern int dsd_p25_sm_next_cc_candidate_impl(dsd_state* state, long* out_freq);

void
p25_sm_init(dsd_opts* opts, dsd_state* state) {
    dsd_p25_sm_init_impl(opts, state);
}

void
p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    dsd_p25_sm_on_group_grant_impl(opts, state, channel, svc_bits, tg, src);
}

void
p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    dsd_p25_sm_on_indiv_grant_impl(opts, state, channel, svc_bits, dst, src);
}

void
p25_sm_on_release(dsd_opts* opts, dsd_state* state) {
    dsd_p25_sm_on_release_impl(opts, state);
}

void
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    dsd_p25_sm_on_neighbor_update_impl(opts, state, freqs, count);
}

void
p25_sm_tick(dsd_opts* opts, dsd_state* state) {
    dsd_p25_sm_tick_impl(opts, state);
}

int
p25_sm_next_cc_candidate(dsd_state* state, long* out_freq) {
    return dsd_p25_sm_next_cc_candidate_impl(state, out_freq);
}
