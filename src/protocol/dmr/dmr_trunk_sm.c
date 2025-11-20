// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/stat.h>
#else
#include <direct.h>
#endif

// External helpers from core/trunking codepaths
void dmr_reset_blocks(dsd_opts* opts, dsd_state* state);
void trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq);
void return_to_cc(dsd_opts* opts, dsd_state* state);

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

// --- Simple per-system CC candidate cache (opt-in, mirrors P25 approach) ---
static void
dmr_sm_log_status(dsd_opts* opts, dsd_state* state, const char* tag) {
    if (!opts || !state) {
        return;
    }
    if (opts->verbose > 1) {
        fprintf(stderr, "\n  DMR SM: %s tunes=%u releases=%u cc_cand add=%u used=%u count=%d idx=%d\n",
                tag ? tag : "status",
                state->p25_sm_tune_count, // reuse P25 counters for cross-proto stats
                state->p25_sm_release_count, state->p25_cc_cand_added, state->p25_cc_cand_used,
                state->p25_cc_cand_count, state->p25_cc_cand_idx);
    }
}

static int
dmr_sm_build_cache_path(const dsd_state* state, char* out, size_t out_len) {
    if (!state || !out || out_len == 0) {
        return 0;
    }
    if (state->dmr_t3_syscode == 0) {
        return 0; // need identity
    }
    const char* root = getenv("DSD_NEO_CACHE_DIR");
    char path[1024] = {0};
    if (root && root[0] != '\0') {
        snprintf(path, sizeof(path), "%s", root);
    } else {
        const char* home = getenv("HOME");
#ifdef _WIN32
        if (!home || home[0] == '\0') {
            home = getenv("LOCALAPPDATA");
        }
#endif
        if (home && home[0] != '\0') {
            snprintf(path, sizeof(path), "%s/.cache/dsd-neo", home);
        } else {
            snprintf(path, sizeof(path), ".dsdneo_cache");
        }
    }
    // best-effort ensure directory exists (ignored errors)
#ifndef _WIN32
    (void)mkdir(path, 0700);
#else
    (void)_mkdir(path);
#endif
    int n = snprintf(out, out_len, "%s/dmr_cc_%04X.txt", path, (unsigned)state->dmr_t3_syscode);
    return (n > 0 && (size_t)n < out_len);
}

static void
dmr_sm_try_load_cache(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);
    if (!state || state->p25_cc_cache_loaded) {
        return;
    }
    int enable = 1;
    const char* env = getenv("DSD_NEO_CC_CACHE");
    if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F')) {
        enable = 0;
    }
    if (!enable) {
        state->p25_cc_cache_loaded = 1;
        return;
    }
    char fpath[1024];
    if (!dmr_sm_build_cache_path(state, fpath, sizeof(fpath))) {
        return;
    }
    FILE* fp = fopen(fpath, "r");
    if (!fp) {
        state->p25_cc_cache_loaded = 1;
        return;
    }
    if (state->p25_cc_cand_count < 0 || state->p25_cc_cand_count > 16) {
        state->p25_cc_cand_count = 0;
        state->p25_cc_cand_idx = 0;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char* end = NULL;
        long f = strtol(line, &end, 10);
        if (end == line || f == 0 || f == state->p25_cc_freq) {
            continue;
        }
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
        }
    }
    fclose(fp);
    state->p25_cc_cache_loaded = 1;
}

static void
dmr_sm_persist_cache(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    int enable = 1;
    const char* env = getenv("DSD_NEO_CC_CACHE");
    if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F')) {
        enable = 0;
    }
    if (!enable) {
        return;
    }
    char fpath[1024];
    if (!dmr_sm_build_cache_path(state, fpath, sizeof(fpath))) {
        return;
    }
    FILE* fp = fopen(fpath, "w");
    if (!fp) {
        (void)opts;
        return;
    }
    for (int i = 0; i < state->p25_cc_cand_count; i++) {
        if (state->p25_cc_candidates[i] != 0) {
            fprintf(fp, "%ld\n", state->p25_cc_candidates[i]);
        }
    }
    fclose(fp);
}

// Determine if a DMR slot burst indicates active voice
static inline int
dmr_sm_burst_is_voice(int burst) {
    // DMR VOICE (16), MAC_ACTIVE VOICE (21)
    return (burst == 16) || (burst == 21);
}

// Determine if a slot should be considered "active" for release decisions.
// Voice is always active. If the user opts to tune data calls, treat common
// data header/block indicators as active as well so we don’t prematurely
// return to CC while a data call is in progress on the opposite slot.
static inline int
dmr_sm_burst_is_activity(const dsd_opts* opts, int burst) {
    if (dmr_sm_burst_is_voice(burst)) {
        return 1;
    }
    if (opts && opts->trunk_tune_data_calls == 1) {
        // DATA (6), R12D (7), R34D (8), R1_D (10)
        if (burst == 6 || burst == 7 || burst == 8 || burst == 10) {
            return 1;
        }
    }
    return 0;
}

// Internal helper: compute VC freq from inputs
static long
dmr_sm_resolve_freq(const dsd_state* state, long freq_hz, int lpcn) {
    if (freq_hz > 0) {
        return freq_hz;
    }
    if (state && lpcn > 0 && lpcn < 0xFFFF) {
        return state->trunk_chan_map[lpcn];
    }
    return 0;
}

// Internal helper: tune to DMR VC (mirrors P25 SM tune behavior, but leaves UI-string setup to caller)
static void
dmr_sm_tune_to_vc(dsd_opts* opts, dsd_state* state, long freq_hz) {
    if (!opts || !state) {
        return;
    }
    if (freq_hz <= 0) {
        return;
    }
    // Trunking disabled: do not leave CC
    if (opts->trunk_enable != 1) {
        return;
    }
    if (state->p25_cc_freq == 0) {
        return;
    }
    if (opts->p25_is_tuned == 1) {
        // Already off CC on a VC — allow retune only if target differs
        long cur = state->trunk_vc_freq[0];
        if (cur == freq_hz && cur != 0) {
            return; // no-op
        }
        // fall through to retune to new VC frequency
    }

    // Best-effort reset of data block assembly when leaving current frequency
    dmr_reset_blocks(opts, state);

    trunk_tune_to_freq(opts, state, freq_hz);
    // Record the actual tune time so release logic can apply a short VC grace
    // period even before any voice sync arrives.
    state->last_t3_tune_time = time(NULL);
    state->last_t3_tune_time_m = dsd_time_now_monotonic_s();
    state->p25_sm_tune_count++;
    if (opts->verbose > 0) {
        fprintf(stderr, "\n  DMR SM: Tune VC freq=%.6lf MHz\n", (double)freq_hz / 1000000.0);
    }
    dmr_sm_log_status(opts, state, "after-tune");
}

void
dmr_sm_init(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
}

void
dmr_sm_on_group_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int tg, int src) {
    UNUSED2(tg, src);
    long f = dmr_sm_resolve_freq(state, freq_hz, lpcn);
    // Clamp: if using LPCN-derived mapping, require trusted map unless on CC
    if (f == 0) {
        return;
    }
    if (freq_hz <= 0 && lpcn > 0 && lpcn < 0x1000) {
        uint8_t trust = state->dmr_lcn_trust[lpcn];
        int on_cc = (state->p25_cc_freq != 0 && opts && opts->p25_is_tuned == 0);
        if (trust < 2 && !on_cc) {
            if (opts && opts->verbose > 0) {
                fprintf(stderr, "\n  DMR SM: block tune LPCN=%d (untrusted off-CC)\n", lpcn);
            }
            return; // untrusted mapping off-CC → do not tune
        }
    }
    dmr_sm_tune_to_vc(opts, state, f);
}

void
dmr_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int dst, int src) {
    UNUSED2(dst, src);
    long f = dmr_sm_resolve_freq(state, freq_hz, lpcn);
    if (f == 0) {
        return;
    }
    if (freq_hz <= 0 && lpcn > 0 && lpcn < 0x1000) {
        uint8_t trust = state->dmr_lcn_trust[lpcn];
        int on_cc = (state->p25_cc_freq != 0 && opts && opts->p25_is_tuned == 0);
        if (trust < 2 && !on_cc) {
            if (opts && opts->verbose > 0) {
                fprintf(stderr, "\n  DMR SM: block tune LPCN=%d (untrusted off-CC)", lpcn);
            }
            return;
        }
    }
    dmr_sm_tune_to_vc(opts, state, f);
}

void
dmr_sm_on_release(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    // Debug aid (verbose>3): summarize release gate inputs
    if (opts->verbose > 3) {
        fprintf(stderr, "DMR_SM_RELEASE: tuned=%d L=%d R=%d hang=%.2f last_vc=%ld last_tune=%ld now=%ld\n",
                opts->p25_is_tuned, state->dmrburstL, state->dmrburstR, (double)opts->trunk_hangtime,
                (long)state->last_vc_sync_time, (long)state->last_t3_tune_time, (long)time(NULL));
        fflush(stderr);
    }
    state->p25_sm_release_count++;
    // One-shot force: bypass gating and immediately return to CC
    int forced = (state->p25_sm_force_release != 0);
    if (forced) {
        state->p25_sm_force_release = 0; // consume
        return_to_cc(opts, state);
        dmr_sm_log_status(opts, state, "after-release-forced");
        return;
    }
    // Respect a brief hangtime based on recent voice activity rather than
    // initial tune time. This avoids bouncing back to CC between back-to-back
    // calls on the same VC when the first call exceeded the hangtime window.
    if (opts->trunk_hangtime > 0.0f) {
        double dt = 0.0;
        int have_voice_time = 0;
        if (state->last_vc_sync_time_m > 0.0) {
            double nowm = dsd_time_now_monotonic_s();
            dt = nowm - state->last_vc_sync_time_m;
            have_voice_time = 1;
        } else if (state->last_vc_sync_time != 0) {
            time_t now = time(NULL);
            dt = (double)(now - state->last_vc_sync_time);
            have_voice_time = 1;
        } else {
            dt = 0.0;
        }
        if (opts->verbose > 2) {
            fprintf(stderr, "\n  DMR SM: Hangtime check dt=%.2f hang=%.2f\n", dt, opts->trunk_hangtime);
        }
        if (have_voice_time && dt < opts->trunk_hangtime) {
            if (opts->verbose > 2) {
                fprintf(stderr, "\n  DMR SM: Release deferred (recent voice) dt=%.2f\n", dt);
            }
            return; // defer return to CC
        }
    }

    // If either slot still shows activity (voice, or data when enabled),
    // defer return-to-CC to avoid dropping an opposite-slot call that ends
    // slightly later. Hangtime check above takes precedence: once hangtime is
    // satisfied we allow release even if stale activity bits linger.
    int left_active = dmr_sm_burst_is_activity(opts, state->dmrburstL);
    int right_active = dmr_sm_burst_is_activity(opts, state->dmrburstR);
    if (opts->verbose > 2) {
        fprintf(stderr, "\n  DMR SM: Release check L_act=%d R_act=%d burstL=%d burstR=%d\n", left_active, right_active,
                state->dmrburstL, state->dmrburstR);
    }
    if (left_active || right_active) {
        if (opts->verbose > 0) {
            fprintf(stderr, "\n  DMR SM: Release ignored (slot active) L=%d R=%d dL=%u dR=%u\n", left_active,
                    right_active, state->dmrburstL, state->dmrburstR);
        }
        dmr_sm_log_status(opts, state, "release-deferred");
        return; // keep current VC
    }

    // Apply a short post-tune grace window only when we have not observed any
    // voice sync on this VC yet. Once voice has been seen (last_vc_sync_time != 0),
    // defer logic is governed by hangtime above.
    if (state->last_vc_sync_time_m == 0.0 && state->last_vc_sync_time == 0) {
        double dt_tune = 1e9;
        if (state->last_t3_tune_time_m > 0.0) {
            double nowm = dsd_time_now_monotonic_s();
            dt_tune = nowm - state->last_t3_tune_time_m;
        } else if (state->last_t3_tune_time != 0) {
            time_t now = time(NULL);
            dt_tune = (double)(now - state->last_t3_tune_time);
        }
        double vc_grace = 1.25; // seconds default
        const char* env = getenv("DSD_NEO_DMR_VC_GRACE");
        if (env && env[0] != '\0') {
            double v = atof(env);
            if (v >= 0.0 && v <= 5.0) {
                vc_grace = v;
            }
        }
        if (dt_tune < vc_grace) {
            if (opts->verbose > 1) {
                fprintf(stderr, "\n  DMR SM: Release deferred (VC grace) dt=%.2f < %.2f\n", dt_tune, vc_grace);
            }
            return;
        }
    }

    // Return to CC using shared tuner helper. Proactively clear VC tune flags
    // so unit tests that stub return_to_cc() still observe a release even if
    // the stub is a no-op beyond state mutation.
    if (opts->verbose > 2) {
        fprintf(stderr, "\n  DMR SM: Release -> CC\n");
    }
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return_to_cc(opts, state);
    dmr_sm_log_status(opts, state, "after-release");
}

void
dmr_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    if (!state || !freqs || count <= 0) {
        return;
    }
    // Lazy-load cached candidates once identity is known
    dmr_sm_try_load_cache(opts, state);
    // Reuse P25 candidate ring for now (shared fields in dsd_state)
    if (state->p25_cc_cand_count < 0 || state->p25_cc_cand_count > 16) {
        state->p25_cc_cand_count = 0;
        state->p25_cc_cand_idx = 0;
    }
    for (int i = 0; i < count; i++) {
        long f = freqs[i];
        if (f == 0 || f == state->p25_cc_freq) {
            continue;
        }
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
            for (int k = 1; k < 16; k++) {
                state->p25_cc_candidates[k - 1] = state->p25_cc_candidates[k];
            }
            state->p25_cc_candidates[15] = f;
            if (state->p25_cc_cand_idx > 0) {
                state->p25_cc_cand_idx--;
            }
            state->p25_cc_cand_added++;
        }
    }
    // Persist for warm start
    dmr_sm_persist_cache(opts, state);
    dmr_sm_log_status(opts, state, "after-neigh");
}

int
dmr_sm_next_cc_candidate(dsd_state* state, long* out_freq) {
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
