// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

static void
p25_sm_log_status(dsd_opts* opts, dsd_state* state, const char* tag) {
    if (!opts || !state) {
        return;
    }
    if (opts->verbose > 1) {
        fprintf(stderr, " P25 SM: %s tunes=%u releases=%u cc_cand add=%u used=%u count=%d idx=%d\n",
                tag ? tag : "status", state->p25_sm_tune_count, state->p25_sm_release_count, state->p25_cc_cand_added,
                state->p25_cc_cand_used, state->p25_cc_cand_count, state->p25_cc_cand_idx);
    }
}

// Internal helper: compute and tune to a P25 VC, set symbol/slot appropriately
static void
p25_tune_to_vc(dsd_opts* opts, dsd_state* state, long freq, int channel) {
    if (freq == 0 || opts->p25_is_tuned == 1) {
        return;
    }
    if (opts->p25_trunk != 1) {
        return;
    }
    if (state->p25_cc_freq == 0) {
        return;
    }

    // TDMA channel detection by channel iden type
    int iden = (channel >> 12) & 0xF;
    int is_tdma = 0;
    if (iden >= 0 && iden < 16) {
        is_tdma = state->p25_chan_tdma[iden];
    }

    if (is_tdma) {
        state->samplesPerSymbol = 8;
        state->symbolCenter = 3;
        // Track active slot from channel LSB without changing user slot toggles
        state->p25_p2_active_slot = (channel & 1) ? 1 : 0;
    } else {
        // Single‑carrier channel: set baseline symbol timing and enable both slots
        state->samplesPerSymbol = 10;
        state->symbolCenter = 4;
        state->p25_p2_active_slot = -1;
    }

    // Tune
    if (opts->use_rigctl == 1) {
        if (opts->setmod_bw != 0) {
            SetModulation(opts->rigctl_sockfd, opts->setmod_bw);
        }
        SetFreq(opts->rigctl_sockfd, freq);
    } else if (opts->audio_in_type == 3) {
#ifdef USE_RTLSDR
        if (g_rtl_ctx) {
            rtl_stream_tune(g_rtl_ctx, (uint32_t)freq);
        }
#endif
    }

    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    opts->p25_is_tuned = 1;
    state->last_vc_sync_time = time(NULL);
    state->p25_sm_tune_count++;
    if (opts->verbose > 0) {
        fprintf(stderr, "\n P25 SM: Tune VC ch=0x%04X freq=%.6lf MHz tdma=%d\n", channel, (double)freq / 1000000.0,
                is_tdma);
    }
    p25_sm_log_status(opts, state, "after-tune");
}

// Compute frequency from explicit channel and call p25_tune_to_vc
static void
p25_handle_grant(dsd_opts* opts, dsd_state* state, int channel) {
    long freq = process_channel_to_freq(opts, state, channel);
    if (freq != 0) {
        p25_tune_to_vc(opts, state, freq, channel);
    }
}

void
p25_sm_init(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
}

void
p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    UNUSED2(svc_bits, src);
    // TG may be used for future gating; tuning logic is centralized here
    if (tg == 0) {
        // proceed, some systems use TG 0 for special cases
    }
    p25_handle_grant(opts, state, channel);
}

void
p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    UNUSED2(svc_bits, src);
    if (dst == 0) {
        // proceed regardless
    }
    p25_handle_grant(opts, state, channel);
}

void
p25_sm_on_release(dsd_opts* opts, dsd_state* state) {
    // Centralized release handling. For Phase 2 (TDMA) voice channels with two
    // logical slots, do not return to the control channel if the other slot is
    // still active. This prevents dropping an in-progress call on the opposite
    // timeslot when only one slot sends an END/IDLE indication.
    state->p25_sm_release_count++;

    int is_p2_vc = (state && state->p25_p2_active_slot != -1);
    if (is_p2_vc) {
        int left_active = 0;
        int right_active = 0;
        // Consider a slot active if audio is allowed OR its burst state is
        // not idle (24). This preserves encrypted call following (audio gated)
        // while still honoring true idle determinations.
        if (state) {
            left_active = (state->p25_p2_audio_allowed[0] != 0) || (state->dmrburstL != 24 && state->dmrburstL != 0);
            right_active = (state->p25_p2_audio_allowed[1] != 0) || (state->dmrburstR != 24 && state->dmrburstR != 0);
        }

        if (left_active || right_active) {
            if (opts && opts->verbose > 0) {
                fprintf(stderr, "\n P25 SM: Release ignored (slot active) L=%d R=%d dL=%u dR=%u allowL=%d allowR=%d\n",
                        left_active, right_active, state->dmrburstL, state->dmrburstR, state->p25_p2_audio_allowed[0],
                        state->p25_p2_audio_allowed[1]);
            }
            p25_sm_log_status(opts, state, "release-deferred");
            return; // keep current VC; do not return to CC yet
        }
    }

    // Either not a P25p2 VC or no other slot is active: return to CC.
    if (opts && opts->verbose > 0) {
        fprintf(stderr, "\n P25 SM: Release -> CC\n");
    }
    return_to_cc(opts, state);
    p25_sm_log_status(opts, state, "after-release");
}

void
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    UNUSED(opts);
    if (count <= 0 || state == NULL || freqs == NULL) {
        return;
    }
    // Initialize if needed
    if (state->p25_cc_cand_count < 0 || state->p25_cc_cand_count > 16) {
        state->p25_cc_cand_count = 0;
        state->p25_cc_cand_idx = 0;
    }
    for (int i = 0; i < count; i++) {
        long f = freqs[i];
        if (f == 0) {
            continue;
        }
        // Dedup against current CC
        if (f == state->p25_cc_freq) {
            continue;
        }
        // Dedup against existing candidates
        int exists = 0;
        for (int k = 0; k < state->p25_cc_cand_count; k++) {
            if (state->p25_cc_candidates[k] == f) {
                exists = 1;
                break;
            }
        }
        if (exists) {
            continue;
        }
        if (state->p25_cc_cand_count < 16) {
            state->p25_cc_candidates[state->p25_cc_cand_count++] = f;
            state->p25_cc_cand_added++;
        } else {
            // Simple FIFO: shift left and append
            for (int k = 1; k < 16; k++) {
                state->p25_cc_candidates[k - 1] = state->p25_cc_candidates[k];
            }
            state->p25_cc_candidates[15] = f;
            if (state->p25_cc_cand_idx > 0) {
                state->p25_cc_cand_idx--;
            }
            state->p25_cc_cand_added++;
        }
        if (opts->verbose > 1) {
            fprintf(stderr, "\n P25 SM: Add CC cand=%.6lf MHz (count=%d)\n", (double)f / 1000000.0,
                    state->p25_cc_cand_count);
        }
    }
    p25_sm_log_status(opts, state, "after-neigh");
}

void
p25_sm_tick(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
}

int
p25_sm_next_cc_candidate(dsd_state* state, long* out_freq) {
    if (!state || !out_freq) {
        return 0;
    }
    for (int tries = 0; tries < state->p25_cc_cand_count; tries++) {
        if (state->p25_cc_cand_idx >= state->p25_cc_cand_count) {
            state->p25_cc_cand_idx = 0;
        }
        long f = state->p25_cc_candidates[state->p25_cc_cand_idx++];
        if (f != 0 && f != state->p25_cc_freq) {
            *out_freq = f;
            state->p25_cc_cand_used++;
            return 1;
        }
    }
    return 0;
}
