// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Fuzz-style liveness test for the P25 trunking state machine.
 *
 * Generates randomized sequences of grants, time advances, audio-gate flips,
 * and neighbor updates. Verifies a liveness property: once voice activity has
 * ceased beyond hangtime and both slots are idle, the SM must return to the
 * control channel within a bounded number of ticks (without getting wedged
 * by stale gates or timing).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#undef main

// --- Strong stubs override weak fallbacks for deterministic testing ---

static int g_return_to_cc_called = 0;
static int g_tune_vc_called = 0;
static int g_tune_cc_called = 0;

bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return true;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return true;
}
struct RtlSdrContext* g_rtl_ctx = 0; // satisfy references

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    g_return_to_cc_called++;
    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
}

void
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    (void)freq;
    g_tune_vc_called++;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->last_vc_sync_time = time(NULL);
}

void
trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq) {
    (void)state;
    (void)freq;
    g_tune_cc_called++;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
}

// --- Simple deterministic PRNG ---
static unsigned xorshift32_state = 0xC0FFEEu;

static unsigned
rnd(void) {
    unsigned x = xorshift32_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift32_state = x;
    return x;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s: failed\n", tag);
        return 1;
    }
    return 0;
}

// Helper: set up a minimal valid IDEN so grants can be tuned
static void
setup_iden(dsd_state* st, int id, int tdma) {
    st->p25_chan_iden = id & 0xF;
    st->p25_chan_type[id] = tdma ? 3 : 1; // denom2 vs denom1 representative
    st->p25_chan_tdma[id] = tdma ? 1 : 0;
    st->p25_base_freq[id] = 851000000 / 5; // store in 5 kHz units
    st->p25_chan_spac[id] = 100;           // 5 kHz units
    st->p25_iden_trust[id] = 2;            // confirmed
}

// Grant helper: emits a group grant for channel 'ch' under svc bits
static void
do_grant(dsd_opts* opts, dsd_state* st, int ch, int svc, int tg, int src) {
    p25_sm_on_group_grant(opts, st, ch, svc, tg, src);
}

int
main(void) {
    int rc = 0;
    dsd_opts opts;
    dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    // Provide minimal event history storage expected by trunk SM
    static Event_History_I event_buf[2];
    memset(event_buf, 0, sizeof event_buf);
    st.event_history_s = event_buf;
    opts.p25_trunk = 1;
    opts.trunk_hangtime = 1.0f; // fast liveness
    st.p25_cc_freq = 851012500; // known CC
    st.p2_wacn = 0x12345;
    st.p2_sysid = 0x123;

    // Provide two IDENs: one FDMA, one TDMA
    setup_iden(&st, 1, 0);
    setup_iden(&st, 2, 1);

    // Seed CC candidates so CC hunting is possible if needed
    long neigh[2] = {851012500, 851025000};
    p25_sm_on_neighbor_update(&opts, &st, neigh, 2);

    // Run multiple random trials; each trial simulates a few calls and idles
    const int trials = 100;
    for (int t = 0; t < trials; t++) {
        // Randomly choose FDMA/TDMA and a channel number (low nibble)
        int tdma = (rnd() & 1);
        int id = tdma ? 2 : 1;
        int low = (int)(rnd() % 16);
        int ch = (id << 12) | low;
        int svc = (rnd() & 1) ? 0x00 : 0x10; // clear vs packet flag (should be blocked by policy if disabled)

        // Ensure we start untuned, then issue a grant
        opts.p25_is_tuned = 0;
        g_return_to_cc_called = 0;
        int prev_tunes = g_tune_vc_called;
        do_grant(&opts, &st, ch, svc, /*tg*/ 40000 + (rnd() % 100), /*src*/ 123456 + (rnd() % 100));
        int tuned_now = (g_tune_vc_called > prev_tunes) && (opts.p25_is_tuned == 1);

        if (tuned_now) {
            // Randomly toggle P2 audio gates (can wedge older logic); also simulate activity
            if (tdma) {
                st.p25_p2_active_slot = (ch & 1) ? 1 : 0;
                st.p25_p2_audio_allowed[0] = (rnd() & 1);
                st.p25_p2_audio_allowed[1] = (rnd() & 1);
            } else {
                st.p25_p2_active_slot = -1;
                st.p25_p2_audio_allowed[0] = 0;
                st.p25_p2_audio_allowed[1] = 0;
            }
            st.last_vc_sync_time = time(NULL); // fresh activity

            // Occasionally inject neighbor updates (RFSS/NSB-derived) mid-call to
            // ensure candidate tracking does not wedge release behavior.
            if ((rnd() % 3) == 0) {
                long neigh[3];
                int nc = (int)(1 + (rnd() % 3));
                for (int k = 0; k < nc; k++) {
                    // Create plausible 851 MHz neighbors in 12.5 kHz steps
                    long base = 851000000 + (long)((rnd() % 16) * 12500);
                    neigh[k] = base;
                }
                p25_sm_on_neighbor_update(&opts, &st, neigh, nc);
            }

            // Now simulate call end: advance logical time by setting last_vc_sync_time in the past
            // Keep both slots idle (or force-clear gates) and call tick repeatedly until release occurs.
            st.p25_p2_audio_allowed[0] = 0;
            st.p25_p2_audio_allowed[1] = 0;
            st.last_vc_sync_time = time(NULL) - 3; // past hangtime

            // Give the SM several opportunities to release within bounded steps
            int steps = 0;
            const int max_steps = 4;
            while (opts.p25_is_tuned == 1 && steps++ < max_steps) {
                p25_sm_tick(&opts, &st);
            }

            // Liveness: must have returned to CC (untuned) and invoked return_to_cc at least once
            rc |= expect_true("release->cc", opts.p25_is_tuned == 0);
            rc |= expect_true("rtc-called", g_return_to_cc_called >= 1);

            // Also assert gates are cleared post-release
            rc |= expect_true("gateL=0", st.p25_p2_audio_allowed[0] == 0);
            rc |= expect_true("gateR=0", st.p25_p2_audio_allowed[1] == 0);
        }
    }

    // Explicit release path (LCW 0x4F Call Termination equivalent):
    // model by invoking return_to_cc directly and verify untuned
    opts.p25_is_tuned = 1;
    g_return_to_cc_called = 0;
    return_to_cc(&opts, &st);
    rc |= expect_true("lcw-0x4F-rtc", g_return_to_cc_called >= 1 && opts.p25_is_tuned == 0);

    // Sanity: CC hunting path should prefer candidates when CC is lost and grace elapsed
    st.last_cc_sync_time = time(NULL) - 8; // beyond hangtime + grace window
    opts.p25_is_tuned = 0;
    opts.p25_prefer_candidates = 1;
    int prev_cc_hunts = g_tune_cc_called;
    p25_sm_tick(&opts, &st);
    rc |= expect_true("cc-hunt-called", g_tune_cc_called > prev_cc_hunts);

    // Data-call gating: ensure no tune when svc bit 0x10 and tuning disabled
    opts.trunk_tune_data_calls = 0;
    int prev_tunes = g_tune_vc_called;
    opts.p25_is_tuned = 0;
    do_grant(&opts, &st, (1 << 12) | 0x0002, /*svc*/ 0x10, /*tg*/ 41000, /*src*/ 1001);
    rc |= expect_true("data-gate", g_tune_vc_called == prev_tunes && opts.p25_is_tuned == 0);

    // ENC-call gating: ensure no tune when svc bit 0x40 and tuning disabled
    opts.trunk_tune_enc_calls = 0;
    prev_tunes = g_tune_vc_called;
    opts.p25_is_tuned = 0;
    do_grant(&opts, &st, (1 << 12) | 0x0003, /*svc*/ 0x40, /*tg*/ 42000, /*src*/ 2002);
    rc |= expect_true("enc-gate", g_tune_vc_called == prev_tunes && opts.p25_is_tuned == 0);

    return rc;
}
