// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

// Weak fallback for trunk_tune_to_freq so unit tests that link only the P25
// library do not need the IO Control library. The real implementation lives in
// src/io/control/dsd_rigctl.c and overrides this weak symbol when linked.
__attribute__((weak)) void
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || !state || freq <= 0) {
        return;
    }
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->last_vc_sync_time = time(NULL);
}

// Weak fallback for return_to_cc so unit tests that link only the P25 library
// do not need the IO Control library. The real implementation lives in
// src/io/control/dsd_rigctl.c and overrides this weak symbol when linked.
__attribute__((weak)) void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
}

// Note: Do not use weak symbols here. Windows/COFF linkers handle them
// differently than ELF and that caused undefined references in CI.
// Expire regroup/patch entries older than this many seconds
#define P25_PATCH_TTL_SECONDS 600

static void
p25_sm_log_status(dsd_opts* opts, dsd_state* state, const char* tag) {
    if (!opts || !state) {
        return;
    }
    if (opts->verbose > 1) {
        fprintf(stderr, "\n  P25 SM: %s tunes=%u releases=%u cc_cand add=%u used=%u count=%d idx=%d\n",
                tag ? tag : "status", state->p25_sm_tune_count, state->p25_sm_release_count, state->p25_cc_cand_added,
                state->p25_cc_cand_used, state->p25_cc_cand_count, state->p25_cc_cand_idx);
    }
}

// Build a per-system cache file path for CC candidates. Returns 1 on success.
static int
p25_sm_build_cache_path(const dsd_state* state, char* out, size_t out_len) {
    if (!state || !out || out_len == 0) {
        return 0;
    }
    // Require system identity to be known
    if (state->p2_wacn == 0 || state->p2_sysid == 0) {
        return 0;
    }

    // Allow override via env var
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
            // Fallback to CWD
            snprintf(path, sizeof(path), ".dsdneo_cache");
        }
    }

    // Ensure directory exists (best-effort)
    struct stat st;
    if (stat(path, &st) != 0) {
#ifdef _WIN32
        (void)_mkdir(path);
#else
        (void)mkdir(path, 0700);
#endif
    }

    // Compose final file path
    int n = snprintf(out, out_len, "%s/p25_cc_%05lX_%03X.txt", path, state->p2_wacn, state->p2_sysid);
    return (n > 0 && (size_t)n < out_len);
}

// Load cached CC candidates for this system if not already loaded.
static void
p25_sm_try_load_cache(dsd_opts* opts, dsd_state* state) {
    if (!state || state->p25_cc_cache_loaded) {
        return;
    }
    // Optional opt-in via env flag; default On
    int enable = 1;
    const char* env = getenv("DSD_NEO_CC_CACHE");
    if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F')) {
        enable = 0;
    }
    if (!enable) {
        state->p25_cc_cache_loaded = 1; // mark checked to avoid repeated attempts
        return;
    }

    char fpath[1024];
    if (!p25_sm_build_cache_path(state, fpath, sizeof(fpath))) {
        return; // system identity not known yet or path failed
    }

    FILE* fp = fopen(fpath, "r");
    if (!fp) {
        state->p25_cc_cache_loaded = 1; // nothing to load
        return;
    }

    // Initialize if needed
    if (state->p25_cc_cand_count < 0 || state->p25_cc_cand_count > 16) {
        state->p25_cc_cand_count = 0;
        state->p25_cc_cand_idx = 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char* end = NULL;
        long f = strtol(line, &end, 10);
        if (end == line) {
            continue; // no number parsed
        }
        if (f == 0 || f == state->p25_cc_freq) {
            continue;
        }
        // dedup
        int exists = 0;
        for (int k = 0; k < state->p25_cc_cand_count; k++) {
            if (state->p25_cc_candidates[k] == f) {
                exists = 1;
                break;
            }
        }
        if (!exists) {
            if (state->p25_cc_cand_count < 16) {
                state->p25_cc_candidates[state->p25_cc_cand_count++] = f;
            }
        }
    }
    fclose(fp);
    state->p25_cc_cache_loaded = 1;
    if (opts && opts->verbose > 0 && state->p25_cc_cand_count > 0) {
        fprintf(stderr, "\n  P25 SM: Loaded %d CC candidates from cache\n", state->p25_cc_cand_count);
    }
}

// Persist the current CC candidate list to disk (best-effort).
static void
p25_sm_persist_cache(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    // Optional opt-in via env flag; default On
    int enable = 1;
    const char* env = getenv("DSD_NEO_CC_CACHE");
    if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F')) {
        enable = 0;
    }
    if (!enable) {
        return;
    }

    char fpath[1024];
    if (!p25_sm_build_cache_path(state, fpath, sizeof(fpath))) {
        return;
    }
    FILE* fp = fopen(fpath, "w");
    if (!fp) {
        if (opts && opts->verbose > 1) {
            fprintf(stderr, "\n  P25 SM: Failed to open CC cache for write: %s (errno=%d)\n", fpath, errno);
        }
        return;
    }
    for (int i = 0; i < state->p25_cc_cand_count; i++) {
        if (state->p25_cc_candidates[i] != 0) {
            fprintf(fp, "%ld\n", state->p25_cc_candidates[i]);
        }
    }
    fclose(fp);
}

// (moved) Patch group tracking lives in p25_patch.c

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

    // Tune via common helper
    trunk_tune_to_freq(opts, state, freq);
    // Reset Phase 2 per-slot audio gate and jitter buffers on new VC
    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    p25_p2_audio_ring_reset(state, -1);
    state->p25_sm_tune_count++;
    if (opts->verbose > 0) {
        fprintf(stderr, "\n  P25 SM: Tune VC ch=0x%04X freq=%.6lf MHz tdma=%d\n", channel, (double)freq / 1000000.0,
                is_tdma);
    }
    p25_sm_log_status(opts, state, "after-tune");
}

// Compute frequency from explicit channel and call p25_tune_to_vc
static void
p25_handle_grant(dsd_opts* opts, dsd_state* state, int channel) {
    uint16_t chan16 = (uint16_t)channel;
    int iden = (chan16 >> 12) & 0xF;
    long freq = process_channel_to_freq(opts, state, channel);
    if (freq == 0) {
        return;
    }
    // If channel not provided via explicit map, enforce IDEN trust: only tune
    // using IDEN params that were confirmed on the current CC.
    if (state->trunk_chan_map[chan16] == 0) {
        uint8_t trust = (iden >= 0 && iden < 16) ? state->p25_iden_trust[iden] : 0;
        int prov_unset =
            (iden >= 0 && iden < 16) ? (state->p25_iden_wacn[iden] == 0 && state->p25_iden_sysid[iden] == 0) : 1;
        int on_cc = (state->p25_cc_freq != 0 && opts->p25_is_tuned == 0);
        if (iden < 0 || iden > 15 || (trust < 2 && !(on_cc && prov_unset))) {
            if (opts->verbose > 0) {
                fprintf(stderr, "\n  P25 SM: block tune ch=0x%04X (iden %d unconfirmed)\n", chan16, iden);
            }
            return;
        }
    }
    p25_tune_to_vc(opts, state, freq, channel);
}

// Internal implementation symbols; public wrappers live in p25_trunk_sm_wrap.c
void
dsd_p25_sm_init_impl(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
}

void
dsd_p25_sm_on_group_grant_impl(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    UNUSED2(svc_bits, src);
    // TG may be used for future gating; tuning logic is centralized here
    if (tg == 0) {
        // proceed, some systems use TG 0 for special cases
    }
    // Track RID↔TG mapping when source is known and plausible
    if (src > 0 && tg > 0) {
        p25_ga_add(state, (uint32_t)src, (uint16_t)tg);
    }
    p25_handle_grant(opts, state, channel);
}

void
dsd_p25_sm_on_indiv_grant_impl(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    UNUSED2(svc_bits, src);
    if (dst == 0) {
        // proceed regardless
    }
    p25_handle_grant(opts, state, channel);
}

void
dsd_p25_sm_on_release_impl(dsd_opts* opts, dsd_state* state) {
    // Centralized release handling. For Phase 2 (TDMA) voice channels with two
    // logical slots, do not return to the control channel if the other slot is
    // still active. This prevents dropping an in-progress call on the opposite
    // timeslot when only one slot sends an END/IDLE indication.
    state->p25_sm_release_count++;

    int forced = (state && state->p25_sm_force_release) ? 1 : 0;
    if (state) {
        state->p25_sm_force_release = 0; // consume one-shot flag
    }
    int is_p2_vc = (state && state->p25_p2_active_slot != -1);
    if (is_p2_vc) {
        // Determine activity using P25-specific gates and a short recent-voice window.
        // Avoid relying on DMR burst flags here, as they may be stale across protocol transitions
        // and can wedge the state machine on a dead VC.
        int left_audio = (state->p25_p2_audio_allowed[0] != 0);
        int right_audio = (state->p25_p2_audio_allowed[1] != 0);
        time_t now = time(NULL);
        int recent_voice = (state->last_vc_sync_time != 0 && (now - state->last_vc_sync_time) <= opts->trunk_hangtime);
        int stale_activity =
            (state->last_vc_sync_time != 0 && (now - state->last_vc_sync_time) > (opts->trunk_hangtime + 2));
        // Treat forced release as an unconditional directive: ignore audio gates
        // and recent-voice window when explicitly requested by higher layers
        // (e.g., MAC_IDLE on both slots, early ENC lockout, or teardown PDUs).
        if (!forced && !stale_activity && (left_audio || right_audio)) {
            if (opts && opts->verbose > 0) {
                fprintf(stderr, "\n  P25 SM: Release ignored (audio gate) L=%d R=%d recent=%d hang=%d\n", left_audio,
                        right_audio, recent_voice, opts ? opts->trunk_hangtime : -1);
            }
            p25_sm_log_status(opts, state, "release-deferred-gated");
            return; // keep current VC; do not return to CC yet
        }
        // If neither slot has audio and we were not forced here, still respect
        // a brief hangtime based on the most recent voice activity.
        if (!forced && recent_voice) {
            if (opts && opts->verbose > 0) {
                fprintf(stderr, "\n  P25 SM: Release delayed (recent voice within hangtime)\n");
            }
            p25_sm_log_status(opts, state, "release-delayed-recent");
            return;
        }
    }

    // Either not a P25p2 VC or no other slot is active: return to CC.
    if (opts && opts->verbose > 0) {
        fprintf(stderr, "\n  P25 SM: Release -> CC\n");
    }
    // Flush per-slot audio gates and jitter buffers on release
    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    p25_p2_audio_ring_reset(state, -1);
    // Reset encryption detection fields to avoid stale ALG/KID/MI affecting next call
    state->payload_algid = 0;
    state->payload_algidR = 0;
    state->payload_keyid = 0;
    state->payload_keyidR = 0;
    state->payload_miP = 0;
    state->payload_miN = 0;
    // Clear early ENC pending flags
    state->p25_p2_enc_pending[0] = 0;
    state->p25_p2_enc_pending[1] = 0;
    state->p25_p2_enc_pending_ttg[0] = 0;
    state->p25_p2_enc_pending_ttg[1] = 0;
    return_to_cc(opts, state);
    p25_sm_log_status(opts, state, "after-release");
}

void
dsd_p25_sm_on_neighbor_update_impl(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    UNUSED(opts);
    if (count <= 0 || state == NULL || freqs == NULL) {
        return;
    }
    // Lazy-load any persisted candidates once system identity is known
    p25_sm_try_load_cache(opts, state);
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
            fprintf(stderr, "\n  P25 SM: Add CC cand=%.6lf MHz (count=%d)\n", (double)f / 1000000.0,
                    state->p25_cc_cand_count);
        }
    }
    // Best-effort persistence for warm start in future runs
    p25_sm_persist_cache(opts, state);
    p25_sm_log_status(opts, state, "after-neigh");
}

void
dsd_p25_sm_tick_impl(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (opts->p25_trunk != 1) {
        return;
    }

    const time_t now = time(NULL);

    // If currently tuned to a P25 VC and we've observed no recent voice
    // activity for longer than hangtime, force a safe return to CC. This
    // complements the inline fallbacks in the P25p2 frame path and protects
    // against cases where frame processing halts due to signal loss.
    if (opts->p25_is_tuned == 1) {
        double dt = (state->last_vc_sync_time != 0) ? (double)(now - state->last_vc_sync_time) : 1e9;
        int is_p2_vc = (state->p25_p2_active_slot != -1);
        // Safety: if we've exceeded hangtime (by a small margin), aggressively
        // clear per-slot audio gates so stale 'allowed' flags cannot wedge the
        // release logic when voice has clearly stopped.
        if (is_p2_vc && dt > (opts->trunk_hangtime + 1.0)) {
            state->p25_p2_audio_allowed[0] = 0;
            state->p25_p2_audio_allowed[1] = 0;
        }
        int both_slots_idle =
            (!is_p2_vc) ? 1 : (state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_allowed[1] == 0);
        if (dt > (opts->trunk_hangtime + 1.5) && both_slots_idle) {
            if (state->p25_cc_freq != 0) {
                state->p25_sm_force_release = 1;
                p25_sm_on_release(opts, state);
            } else {
                // If CC unknown, do a minimal VC teardown to allow the normal
                // CC-hunt path to engage without attempting to tune to 0 Hz.
                state->p25_p2_audio_allowed[0] = 0;
                state->p25_p2_audio_allowed[1] = 0;
                state->p25_p2_active_slot = -1;
                state->p25_vc_freq[0] = 0;
                state->p25_vc_freq[1] = 0;
                opts->p25_is_tuned = 0;
                opts->trunk_is_tuned = 0;
                state->last_cc_sync_time = now;
            }
        }
    }

    // Age-out any stale affiliated RIDs (runs at ~1 Hz)
    p25_aff_tick(state);
    // Age-out Group Affiliation pairs
    p25_ga_tick(state);
}

int
dsd_p25_sm_next_cc_candidate_impl(dsd_state* state, long* out_freq) {
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

// ---- Affiliation (RID) table helpers ----

// Default aging window for affiliations: 15 minutes
#define P25_AFF_TTL_SEC ((time_t)15 * 60)

static int
p25_aff_find_idx(const dsd_state* state, uint32_t rid) {
    if (!state || rid == 0) {
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        if (state->p25_aff_rid[i] == rid) {
            return i;
        }
    }
    return -1;
}

// ---- Group Affiliation (RID↔TG) table helpers ----

#define P25_GA_TTL_SEC ((time_t)30 * 60)

static int
p25_ga_find_idx(const dsd_state* state, uint32_t rid, uint16_t tg) {
    if (!state || rid == 0 || tg == 0) {
        return -1;
    }
    for (int i = 0; i < 512; i++) {
        if (state->p25_ga_rid[i] == rid && state->p25_ga_tg[i] == tg) {
            return i;
        }
    }
    return -1;
}

static int
p25_ga_find_free(const dsd_state* state) {
    if (!state) {
        return -1;
    }
    for (int i = 0; i < 512; i++) {
        if (state->p25_ga_rid[i] == 0 || state->p25_ga_tg[i] == 0) {
            return i;
        }
    }
    return -1;
}

void
p25_ga_add(dsd_state* state, uint32_t rid, uint16_t tg) {
    if (!state || rid == 0 || tg == 0) {
        return;
    }
    int idx = p25_ga_find_idx(state, rid, tg);
    if (idx < 0) {
        idx = p25_ga_find_free(state);
        if (idx < 0) {
            // replace stalest entry
            time_t oldest = state->p25_ga_last_seen[0];
            int old_idx = 0;
            for (int i = 1; i < 512; i++) {
                if (state->p25_ga_last_seen[i] < oldest) {
                    oldest = state->p25_ga_last_seen[i];
                    old_idx = i;
                }
            }
            idx = old_idx;
        } else {
            state->p25_ga_count++;
        }
        state->p25_ga_rid[idx] = rid;
        state->p25_ga_tg[idx] = tg;
    }
    state->p25_ga_last_seen[idx] = time(NULL);
}

void
p25_ga_remove(dsd_state* state, uint32_t rid, uint16_t tg) {
    if (!state || rid == 0 || tg == 0) {
        return;
    }
    int idx = p25_ga_find_idx(state, rid, tg);
    if (idx >= 0) {
        state->p25_ga_rid[idx] = 0;
        state->p25_ga_tg[idx] = 0;
        state->p25_ga_last_seen[idx] = 0;
        if (state->p25_ga_count > 0) {
            state->p25_ga_count--;
        }
    }
}

void
p25_ga_tick(dsd_state* state) {
    if (!state) {
        return;
    }
    time_t now = time(NULL);
    for (int i = 0; i < 512; i++) {
        if (state->p25_ga_rid[i] != 0 && state->p25_ga_tg[i] != 0) {
            time_t last = state->p25_ga_last_seen[i];
            if (last != 0 && (now - last) > P25_GA_TTL_SEC) {
                state->p25_ga_rid[i] = 0;
                state->p25_ga_tg[i] = 0;
                state->p25_ga_last_seen[i] = 0;
                if (state->p25_ga_count > 0) {
                    state->p25_ga_count--;
                }
            }
        }
    }
}

static int
p25_aff_find_free(const dsd_state* state) {
    if (!state) {
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        if (state->p25_aff_rid[i] == 0) {
            return i;
        }
    }
    return -1;
}

void
p25_aff_register(dsd_state* state, uint32_t rid) {
    if (!state || rid == 0) {
        return;
    }
    int idx = p25_aff_find_idx(state, rid);
    if (idx < 0) {
        idx = p25_aff_find_free(state);
        if (idx < 0) {
            // If full, replace the stalest entry
            time_t oldest = state->p25_aff_last_seen[0];
            int old_idx = 0;
            for (int i = 1; i < 256; i++) {
                if (state->p25_aff_last_seen[i] < oldest) {
                    oldest = state->p25_aff_last_seen[i];
                    old_idx = i;
                }
            }
            idx = old_idx;
        } else {
            state->p25_aff_count++;
        }
        state->p25_aff_rid[idx] = rid;
    }
    state->p25_aff_last_seen[idx] = time(NULL);
}

void
p25_aff_deregister(dsd_state* state, uint32_t rid) {
    if (!state || rid == 0) {
        return;
    }
    int idx = p25_aff_find_idx(state, rid);
    if (idx >= 0) {
        state->p25_aff_rid[idx] = 0;
        state->p25_aff_last_seen[idx] = 0;
        if (state->p25_aff_count > 0) {
            state->p25_aff_count--;
        }
    }
}

void
p25_aff_tick(dsd_state* state) {
    if (!state) {
        return;
    }
    time_t now = time(NULL);
    for (int i = 0; i < 256; i++) {
        if (state->p25_aff_rid[i] != 0) {
            time_t last = state->p25_aff_last_seen[i];
            if (last != 0 && (now - last) > P25_AFF_TTL_SEC) {
                // Expire entry
                state->p25_aff_rid[i] = 0;
                state->p25_aff_last_seen[i] = 0;
                if (state->p25_aff_count > 0) {
                    state->p25_aff_count--;
                }
            }
        }
    }
}
