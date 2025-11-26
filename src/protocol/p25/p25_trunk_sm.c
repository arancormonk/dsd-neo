// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_p2_sm_min.h>
#include <dsd-neo/protocol/p25/p25_sm_ui.h>
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
    time_t nowt = time(NULL);
    double nowm = dsd_time_now_monotonic_s();
    state->last_vc_sync_time = nowt;
    state->p25_last_vc_tune_time = nowt;
    state->last_vc_sync_time_m = nowm;
    state->p25_last_vc_tune_time_m = nowm;
}

// Weak fallback for return_to_cc so unit tests that link only the P25 library
// do not need the IO Control library. The real implementation lives in
// src/io/control/dsd_rigctl.c and overrides this weak symbol when linked.
__attribute__((weak)) void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
}

// Weak fallback for CC tuning so unit tests that link only the P25 library
// can still exercise the state machine without the IO Control library.
__attribute__((weak)) void
trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq) {
    UNUSED(opts);
    if (!state || freq <= 0) {
        return;
    }
    state->trunk_cc_freq = (long int)freq;
    time_t nowt = time(NULL);
    double nowm = dsd_time_now_monotonic_s();
    state->last_cc_sync_time = nowt;
    state->last_cc_sync_time_m = nowm;
    state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
}

// Weak fallbacks for event-history utilities so protocol unit tests can link
// the P25 library without pulling in the full core/UI events implementation.
__attribute__((weak)) void
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    UNUSED3(opts, state, slot);
}

__attribute__((weak)) void
write_event_to_log_file(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite, char* event_string) {
    UNUSED5(opts, state, slot, swrite, event_string);
}

__attribute__((weak)) void
push_event_history(Event_History_I* event_struct) {
    UNUSED(event_struct);
}

__attribute__((weak)) void
init_event_history(Event_History_I* event_struct, uint8_t start, uint8_t stop) {
    UNUSED3(event_struct, start, stop);
}

// Note: Do not use weak symbols here. Windows/COFF linkers handle them
// differently than ELF and that caused undefined references in CI.
// Expire regroup/patch entries older than this many seconds
#define P25_PATCH_TTL_SECONDS 600

// UI/status tagging moved to p25_sm_ui.c

// Central helper: mark a talkgroup as ENC locked-out and emit a single event.
// No-op if the TG is already marked as encrypted (mode "DE").
void
p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc_bits) {
    if (!opts || !state || tg <= 0) {
        return;
    }

    // Locate existing entry
    int idx = -1;
    for (unsigned int i = 0; i < state->group_tally; i++) {
        if (state->group_array[i].groupNumber == (unsigned long)tg) {
            idx = (int)i;
            break;
        }
    }

    int already_de = 0;
    if (idx >= 0) {
        already_de = (strcmp(state->group_array[idx].groupMode, "DE") == 0);
    }
    if (already_de) {
        return; // already marked; event previously emitted
    }

    // Create or update entry to mark encrypted
    if (idx < 0 && state->group_tally < (unsigned)(sizeof(state->group_array) / sizeof(state->group_array[0]))) {
        state->group_array[state->group_tally].groupNumber = (uint32_t)tg;
        snprintf(state->group_array[state->group_tally].groupMode,
                 sizeof state->group_array[state->group_tally].groupMode, "%s", "DE");
        snprintf(state->group_array[state->group_tally].groupName,
                 sizeof state->group_array[state->group_tally].groupName, "%s", "ENC LO");
        state->group_tally++;
    } else if (idx >= 0) {
        snprintf(state->group_array[idx].groupMode, sizeof state->group_array[idx].groupMode, "%s", "DE");
        // Preserve alias/name if present; avoid overwriting user labels
        if (strcmp(state->group_array[idx].groupName, "") == 0) {
            snprintf(state->group_array[idx].groupName, sizeof state->group_array[idx].groupName, "%s", "ENC LO");
        }
    }

    // Prepare per-slot context so watchdog composes headers correctly
    if ((slot & 1) == 0) {
        state->lasttg = (uint32_t)tg;
        state->dmr_so = (uint16_t)svc_bits;
    } else {
        state->lasttgR = (uint32_t)tg;
        state->dmr_soR = (uint16_t)svc_bits;
    }
    state->gi[slot & 1] = 0; // group

    // Compose event text and push if not a duplicate of the most-recent entry
    Event_History_I* eh = (state->event_history_s != NULL) ? &state->event_history_s[slot & 1] : NULL;
    if (eh) {
        snprintf(eh->Event_History_Items[0].internal_str, sizeof eh->Event_History_Items[0].internal_str,
                 "Target: %d; has been locked out; Encryption Lock Out Enabled.", tg);
        watchdog_event_current(opts, state, (uint8_t)(slot & 1));
        if (strncmp(eh->Event_History_Items[1].internal_str, eh->Event_History_Items[0].internal_str,
                    sizeof eh->Event_History_Items[0].internal_str)
            != 0) {
            if (opts->event_out_file[0] != '\0') {
                // TDMA slots use special swrite handling
                uint8_t swrite = (state->lastsynctype == 35 || state->lastsynctype == 36) ? 1 : 0;
                write_event_to_log_file(opts, state, (uint8_t)(slot & 1), swrite,
                                        eh->Event_History_Items[0].event_string);
            }
            push_event_history(eh);
            init_event_history(eh, 0, 1);
        }
    } else if (opts && opts->verbose > 1) {
        p25_sm_log_status(opts, state, "enc-lo-skip-nohist");
    }
}

// (moved) Patch group tracking lives in p25_patch.c

// ---- Helper functions to reduce code duplication ----

// Compute elapsed time since a timestamp, handling both monotonic and wall-clock fallback.
// Returns a large sentinel (1e9) if no timestamp is available.
static inline double
p25_elapsed_since(double mono_ts, time_t wall_ts, double nowm, time_t now) {
    if (mono_ts > 0.0) {
        return nowm - mono_ts;
    }
    if (wall_ts != 0) {
        return (double)(now - wall_ts);
    }
    return 1e9;
}

// Check if a slot has recent MAC activity (within mac_hold seconds).
// Returns 1 if recent, 0 otherwise.
static inline int
p25_slot_mac_recent(const dsd_state* state, int slot, double mac_hold, double nowm, time_t now) {
    if (state->p25_p2_last_mac_active_m[slot] > 0.0) {
        return ((nowm - state->p25_p2_last_mac_active_m[slot]) <= mac_hold) ? 1 : 0;
    }
    if (state->p25_p2_last_mac_active[slot] != 0) {
        return (((double)(now - state->p25_p2_last_mac_active[slot])) <= mac_hold) ? 1 : 0;
    }
    return 0;
}

// Check if a slot has valid ring activity (ring_count > 0 and recent MAC within ring_hold).
// Returns 1 if active ring, 0 otherwise.
static inline int
p25_slot_ring_active(const dsd_state* state, int slot, double ring_hold, double nowm, time_t now) {
    if (state->p25_p2_audio_ring_count[slot] == 0) {
        return 0;
    }
    if (state->p25_p2_last_mac_active_m[slot] > 0.0) {
        return ((nowm - state->p25_p2_last_mac_active_m[slot]) <= ring_hold) ? 1 : 0;
    }
    if (state->p25_p2_last_mac_active[slot] != 0) {
        return (((double)(now - state->p25_p2_last_mac_active[slot])) <= ring_hold) ? 1 : 0;
    }
    return 0;
}

// Compute overall slot activity (audio allowed, ring, or recent MAC).
// When recent_voice is false, audio_allowed alone is not considered active.
static inline int
p25_slot_is_active(const dsd_state* state, int slot, double mac_hold, double ring_hold, double nowm, time_t now,
                   int recent_voice) {
    int mac_recent = p25_slot_mac_recent(state, slot, mac_hold, nowm, now);
    int ring_active = p25_slot_ring_active(state, slot, ring_hold, nowm, now);
    if (recent_voice) {
        return (state->p25_p2_audio_allowed[slot] != 0) || ring_active || mac_recent;
    }
    return ring_active || mac_recent;
}

// Collect per-slot activity hints once so hot paths do not re-run the same checks.
typedef struct {
    int mac_recent[2];
    int ring_active[2];
    int audio_allowed[2];
} p25_slot_activity_basic;

static void
p25_sm_collect_slot_activity(const dsd_state* state, double mac_hold, double ring_hold, double nowm, time_t now,
                             p25_slot_activity_basic* out) {
    if (!out) {
        return;
    }
    out->mac_recent[0] = p25_slot_mac_recent(state, 0, mac_hold, nowm, now);
    out->mac_recent[1] = p25_slot_mac_recent(state, 1, mac_hold, nowm, now);
    out->ring_active[0] = p25_slot_ring_active(state, 0, ring_hold, nowm, now);
    out->ring_active[1] = p25_slot_ring_active(state, 1, ring_hold, nowm, now);
    out->audio_allowed[0] = state ? state->p25_p2_audio_allowed[0] : 0;
    out->audio_allowed[1] = state ? state->p25_p2_audio_allowed[1] : 0;
}

static inline int
p25_slot_active_from_summary(const p25_slot_activity_basic* sa, int slot, int recent_voice) {
    if (!sa || slot < 0 || slot > 1) {
        return 0;
    }
    int mac = sa->mac_recent[slot];
    int ring = sa->ring_active[slot];
    int audio = sa->audio_allowed[slot];
    return recent_voice ? (audio || ring || mac) : (ring || mac);
}

// Resolve a double config value with CLI > 0 taking precedence, then env, else default.
static double
p25_cfg_resolve_double(double cli_val, const char* env_name, double def_val, double min_val, double max_val,
                       int inclusive_max) {
    if (cli_val > 0.0) {
        return cli_val;
    }
    if (env_name && env_name[0] != '\0') {
        const char* s = getenv(env_name);
        if (s && s[0] != '\0') {
            double v = atof(s);
            int within = inclusive_max ? (v >= min_val && v <= max_val) : (v >= min_val && v < max_val);
            if (within) {
                return v;
            }
        }
    }
    return def_val;
}

// Common cached-config getters with local fallbacks to keep hot paths cleaner.
static inline double
p25_cfg_mac_hold(const dsd_state* state) {
    return (state && state->p25_cfg_mac_hold_s > 0.0) ? state->p25_cfg_mac_hold_s : 0.75;
}

static inline double
p25_cfg_ring_hold(const dsd_state* state) {
    return (state && state->p25_cfg_ring_hold_s > 0.0) ? state->p25_cfg_ring_hold_s : 0.75;
}

static inline double
p25_cfg_vc_grace(const dsd_state* state) {
    return (state && state->p25_cfg_vc_grace_s > 0.0) ? state->p25_cfg_vc_grace_s : 0.75;
}

static inline double
p25_cfg_force_rel_extra(const dsd_state* state) {
    return (state && state->p25_cfg_force_rel_extra_s > 0.0) ? state->p25_cfg_force_rel_extra_s : 0.5;
}

static inline double
p25_cfg_force_rel_margin(const dsd_state* state) {
    return (state && state->p25_cfg_force_rel_margin_s > 0.0) ? state->p25_cfg_force_rel_margin_s : 0.25;
}

static inline double
p25_cfg_tail_ms(const dsd_state* state) {
    return (state && state->p25_cfg_tail_ms > 0.0) ? state->p25_cfg_tail_ms : 500.0;
}

static inline double
p25_cfg_p1_tail_ms(const dsd_state* state) {
    return (state && state->p25_cfg_p1_tail_ms > 0.0) ? state->p25_cfg_p1_tail_ms : 300.0;
}

static inline double
p25_cfg_p1_err_hold_pct(const dsd_state* state) {
    return (state && state->p25_cfg_p1_err_hold_pct > 0.0) ? state->p25_cfg_p1_err_hold_pct : 8.0;
}

static inline double
p25_cfg_p1_err_hold_s(const dsd_state* state) {
    return (state && state->p25_cfg_p1_err_hold_s > 0.0) ? state->p25_cfg_p1_err_hold_s : 2.0;
}

static inline double
p25_cfg_grant_voice_to(const dsd_state* state) {
    return (state && state->p25_cfg_grant_voice_to_s > 0.0) ? state->p25_cfg_grant_voice_to_s : 4.0;
}

static inline double
p25_cfg_retune_backoff(const dsd_state* state) {
    return (state && state->p25_cfg_retune_backoff_s > 0.0) ? state->p25_cfg_retune_backoff_s : 1.0;
}

// Determine whether a channel maps to TDMA based on IDEN hints and system TDMA flag.
static inline int
p25_is_tdma_channel(const dsd_state* state, int channel) {
    if (!state) {
        return 0;
    }
    int iden = (channel >> 12) & 0xF;
    if (iden >= 0 && iden < 16) {
        int is_tdma = (state->p25_chan_tdma[iden] & 0x1) ? 1 : 0;
        if (!is_tdma && state->p25_sys_is_tdma == 1) {
            is_tdma = 1; // conservative fallback until IDEN_UP_TDMA arrives
        }
        return is_tdma;
    }
    return 0;
}

static inline int
p25_channel_slot(const dsd_state* state, int channel) {
    return p25_is_tdma_channel(state, channel) ? ((channel & 1) ? 1 : 0) : -1;
}

// Perform a forced release to CC with common cleanup logic.
// reason: a short tag for logging.
// Returns 1 if release was performed, 0 if no CC known (minimal cleanup only).
static int
p25_force_release_to_cc(dsd_opts* opts, dsd_state* state, const char* reason, time_t now, double nowm) {
    if (opts->verbose > 0) {
        p25_sm_log_status(opts, state, reason);
    }
    if (state->p25_cc_freq != 0) {
        state->p25_sm_force_release = 1;
        p25_sm_on_release(opts, state);
        return 1;
    }
    // No CC known: minimal VC teardown
    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    state->p25_p2_active_slot = -1;
    state->p25_vc_freq[0] = 0;
    state->p25_vc_freq[1] = 0;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
    state->last_cc_sync_time = now;
    state->last_cc_sync_time_m = nowm;
    return 0;
}

// Clear per-slot P25P2 gates without touching heavier call context.
static void
p25_clear_p2_gates(dsd_state* state) {
    if (!state) {
        return;
    }
    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    p25_p2_audio_ring_reset(state, -1);
    state->p25_p2_last_mac_active[0] = 0;
    state->p25_p2_last_mac_active[1] = 0;
}

// Clear full Phase 2 call context (includes gates).
static void
p25_clear_p2_call_context(dsd_state* state) {
    if (!state) {
        return;
    }
    p25_clear_p2_gates(state);
    snprintf(state->call_string[0], sizeof state->call_string[0], "%s", "                     ");
    snprintf(state->call_string[1], sizeof state->call_string[1], "%s", "                     ");
    state->p25_call_emergency[0] = state->p25_call_emergency[1] = 0;
    state->p25_call_priority[0] = state->p25_call_priority[1] = 0;
    state->payload_algid = 0;
    state->payload_algidR = 0;
    state->payload_keyid = 0;
    state->payload_keyidR = 0;
    state->payload_miP = 0;
    state->payload_miN = 0;
    state->p25_p2_enc_pending[0] = 0;
    state->p25_p2_enc_pending[1] = 0;
    state->p25_p2_enc_pending_ttg[0] = 0;
    state->p25_p2_enc_pending_ttg[1] = 0;
    state->p25_p2_last_end_ptt[0] = 0;
    state->p25_p2_last_end_ptt[1] = 0;
    state->p25_call_is_packet[0] = 0;
    state->p25_call_is_packet[1] = 0;
    state->p25_p1_last_tdu = 0;
    state->p25_p1_last_tdu_m = 0.0;
}

// Internal helper: compute and tune to a P25 VC, set symbol/slot appropriately
static void
p25_tune_to_vc(dsd_opts* opts, dsd_state* state, long freq, int channel) {
    if (freq == 0) {
        return;
    }
    if (opts->p25_trunk != 1) {
        return;
    }
    if (state->p25_cc_freq == 0) {
        return;
    }

    // TDMA channel detection by channel iden type. If TDMA not yet learned for
    // this IDEN but the system is known to carry Phase 2 voice, treat as TDMA
    // so slot tracking and symbol timing are set correctly on early grants.
    int is_tdma = p25_is_tdma_channel(state, channel);

    if (is_tdma) {
        state->samplesPerSymbol = 8;
        state->symbolCenter = 3;
        // Track active slot from channel LSB without changing user slot toggles
        state->p25_p2_active_slot = p25_channel_slot(state, channel);
    } else {
        // Single‑carrier channel: set baseline symbol timing and enable both slots
        state->samplesPerSymbol = 10;
        state->symbolCenter = 4;
        state->p25_p2_active_slot = -1;
    }

    // Tune via common helper only if not already tuned to this freq. Avoid
    // clearing gates/contexts on duplicate GRANTs while already on the VC,
    // which previously caused audible bounce and premature returns.
    int already_tuned_same = (opts->p25_is_tuned == 1 && state->p25_vc_freq[0] == freq);
    if (!already_tuned_same) {
        trunk_tune_to_freq(opts, state, freq);
        // High-level SM mode: tuned to VC and awaiting voice (ARMED)
        state->p25_sm_mode = DSD_P25_SM_MODE_ARMED;
        // Reset per-slot audio gate and jitter buffers on new VC
        state->p25_p2_audio_allowed[0] = 0;
        state->p25_p2_audio_allowed[1] = 0;
        p25_p2_audio_ring_reset(state, -1);
        // Clear any stale encryption context so early ENC checks on MAC_ACTIVE
        // do not inherit ALG/KID/MI from a prior call before MAC_PTT arrives.
        state->payload_algid = 0;
        state->payload_algidR = 0;
        state->payload_keyid = 0;
        state->payload_keyidR = 0;
        state->payload_miP = 0ULL;
        state->payload_miN = 0ULL;
        state->p25_sm_tune_count++;
        p25_sm_log_status(opts, state, "after-tune");
    }
}

// Compute frequency from explicit channel and call p25_tune_to_vc
// Retune backoff window moved to dsd_state to avoid global/static coupling.
// Track by TDMA slot as well so a new grant on the opposite slot at the same
// RF can proceed immediately.

static void
p25_handle_grant(dsd_opts* opts, dsd_state* state, int channel) {
    uint16_t chan16 = (uint16_t)channel;
    long freq = process_channel_to_freq(opts, state, channel);
    if (freq == 0) {
        return;
    }
    int grant_slot = p25_channel_slot(state, channel);

    // Retune backoff: block immediate re-tune to the same VC/slot after a recent return.
    time_t now = time(NULL);
    if (state->p25_retune_block_until != 0 && now < state->p25_retune_block_until
        && state->p25_retune_block_freq == freq && state->p25_retune_block_slot == grant_slot) {
        p25_sm_log_status(opts, state, "grant-blocked-backoff");
        return;
    }
    // If channel not provided via explicit map, enforce IDEN trust only when
    // we are not parked on a known CC. While on CC, allow tuning when a
    // frequency could be computed (above) to avoid missing early calls before
    // IDEN records are promoted to trusted.
    if (state->trunk_chan_map[chan16] == 0) {
        int on_cc = (state->p25_cc_freq != 0 && opts->p25_is_tuned == 0);
        if (!on_cc) {
            int iden = (chan16 >> 12) & 0xF;
            uint8_t trust = (iden >= 0 && iden < 16) ? state->p25_iden_trust[iden] : 0;
            if (iden < 0 || iden > 15 || trust < 2) {
                p25_sm_log_status(opts, state, "grant-blocked-iden");
                return;
            }
        }
    }
    // Notify the minimal follower of the grant so it can arm timing/backoff
    {
        dsd_p25p2_min_evt ev = {DSD_P25P2_MIN_EV_GRANT, -1, channel, freq};
        dsd_p25p2_min_handle_event(dsd_p25p2_min_get(), opts, state, &ev);
    }
    // Expose ARMED state immediately on grant
    state->p25_sm_mode = DSD_P25_SM_MODE_ARMED;
    p25_tune_to_vc(opts, state, freq, channel);
}

typedef enum {
    P25_GRANT_GROUP = 0,
    P25_GRANT_INDIV = 1,
} p25_grant_kind;

// Centralized grant gating to keep group/individual paths consistent.
static int
p25_grant_allowed(dsd_opts* opts, dsd_state* state, p25_grant_kind kind, int svc_bits, int tg) {
    if (!opts || !state) {
        return 0;
    }

    // Data call policy (common)
    if ((svc_bits & 0x10) && opts->trunk_tune_data_calls == 0) {
        p25_sm_log_status(opts, state, (kind == P25_GRANT_INDIV) ? "indiv-blocked-data" : "grant-blocked-data");
        return 0;
    }

    if (kind == P25_GRANT_INDIV) {
        if (opts->trunk_tune_private_calls == 0) {
            p25_sm_log_status(opts, state, "indiv-blocked-private");
            return 0;
        }
        if ((svc_bits & 0x40) && opts->trunk_tune_enc_calls == 0) {
            p25_sm_log_status(opts, state, "indiv-blocked-enc");
            return 0;
        }
        if (state->tg_hold != 0) {
            p25_sm_log_status(opts, state, "indiv-blocked-hold");
            return 0;
        }
        return 1;
    }

    // Group grant: ENC policy with patch override
    if ((svc_bits & 0x40) && opts->trunk_tune_enc_calls == 0) {
        if (p25_patch_tg_key_is_clear(state, tg) || p25_patch_sg_key_is_clear(state, tg)) {
            p25_sm_log_status(opts, state, "enc-override-clear");
        } else {
            p25_sm_log_status(opts, state, "grant-blocked-enc");
            p25_emit_enc_lockout_once(opts, state, 0, tg, svc_bits);
            return 0;
        }
    }

    // Group list mode check
    char mode[8] = {0};
    if (tg > 0) {
        for (unsigned int i = 0; i < state->group_tally; i++) {
            if (state->group_array[i].groupNumber == (unsigned long)tg) {
                snprintf(mode, sizeof mode, "%s", state->group_array[i].groupMode);
                break;
            }
        }
    }
    if (strcmp(mode, "DE") == 0 || strcmp(mode, "B") == 0) {
        p25_sm_log_status(opts, state, "grant-blocked-mode");
        return 0;
    }

    // TG Hold
    if (state->tg_hold != 0 && (uint32_t)tg != state->tg_hold) {
        p25_sm_log_status(opts, state, "grant-blocked-hold");
        return 0;
    }

    return 1;
}

// Internal implementation symbols; public wrappers live in p25_trunk_sm_wrap.c
void
dsd_p25_sm_init_impl(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);
    if (!state) {
        return;
    }
    // Initialize high-level SM mode and clear retune backoff
    state->p25_sm_mode = (state->p25_cc_freq != 0) ? DSD_P25_SM_MODE_ON_CC : DSD_P25_SM_MODE_UNKNOWN;
    state->p25_retune_block_until = 0;
    state->p25_retune_block_freq = 0;
    state->p25_retune_block_slot = -1;
    // Cache SM tunables once (prefer CLI, then env, then defaults)
    state->p25_cfg_vc_grace_s =
        p25_cfg_resolve_double((opts ? opts->p25_vc_grace_s : 0.0), "DSD_NEO_P25_VC_GRACE", 0.75, 0.0, 10.0, 0);
    state->p25_cfg_grant_voice_to_s = p25_cfg_resolve_double((opts ? opts->p25_grant_voice_to_s : 0.0),
                                                             "DSD_NEO_P25_GRANT_VOICE_TO", 4.0, 0.0, 10.0, 1);
    state->p25_cfg_min_follow_dwell_s = p25_cfg_resolve_double((opts ? opts->p25_min_follow_dwell_s : 0.0),
                                                               "DSD_NEO_P25_MIN_FOLLOW_DWELL", 0.7, 0.0, 5.0, 0);
    state->p25_cfg_retune_backoff_s = p25_cfg_resolve_double((opts ? opts->p25_retune_backoff_s : 0.0),
                                                             "DSD_NEO_P25_RETUNE_BACKOFF", 1.0, 0.0, 10.0, 1);
    state->p25_cfg_mac_hold_s = p25_cfg_resolve_double(-1.0, "DSD_NEO_P25_MAC_HOLD", 0.75, 0.0, 10.0, 0);
    state->p25_cfg_cc_grace_s = p25_cfg_resolve_double(-1.0, "DSD_NEO_P25_CC_GRACE", 2.0, 0.0, 30.0, 0);
    // Ring hold (bound by safety-net extra later)
    state->p25_cfg_ring_hold_s = p25_cfg_resolve_double(-1.0, "DSD_NEO_P25_RING_HOLD", 0.75, 0.0, 5.0, 1);
    // Force-release safety-net parameters
    state->p25_cfg_force_rel_extra_s = p25_cfg_resolve_double((opts ? opts->p25_force_release_extra_s : 0.0),
                                                              "DSD_NEO_P25_FORCE_RELEASE_EXTRA", 0.5, 0.0, 10.0, 1);
    state->p25_cfg_force_rel_margin_s = p25_cfg_resolve_double((opts ? opts->p25_force_release_margin_s : 0.0),
                                                               "DSD_NEO_P25_FORCE_RELEASE_MARGIN", 0.25, 0.0, 30.0, 1);
    // Tail waits
    state->p25_cfg_tail_ms = p25_cfg_resolve_double(-1.0, "DSD_NEO_P25_TAIL_MS", 500.0, 0.0, 5000.0, 1);
    state->p25_cfg_p1_tail_ms = p25_cfg_resolve_double(-1.0, "DSD_NEO_P25P1_TAIL_MS", 300.0, 0.0, 3000.0, 1);
    // P1 elevated-error hold threshold/duration
    state->p25_cfg_p1_err_hold_pct = p25_cfg_resolve_double((opts ? opts->p25_p1_err_hold_pct : 0.0),
                                                            "DSD_NEO_P25P1_ERR_HOLD_PCT", 8.0, 0.0, 100.0, 1);
    state->p25_cfg_p1_err_hold_s =
        p25_cfg_resolve_double((opts ? opts->p25_p1_err_hold_s : 0.0), "DSD_NEO_P25P1_ERR_HOLD_S", 2.0, 0.0, 10.0, 1);
    // Clear CC candidate cooldowns and eval tracking
    state->p25_cc_eval_freq = 0;
    state->p25_cc_eval_start_m = 0.0;
    for (int i = 0; i < 16; i++) {
        state->p25_cc_cand_cool_until[i] = 0.0;
    }
}

void
dsd_p25_sm_on_group_grant_impl(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    if (!opts || !state) {
        return;
    }
    // Track RID↔TG mapping when source is known and plausible
    if (src > 0 && tg > 0) {
        p25_ga_add(state, (uint32_t)src, (uint16_t)tg);
    }

    if (!p25_grant_allowed(opts, state, P25_GRANT_GROUP, svc_bits, tg)) {
        return;
    }

    // Proceed with tuned grant
    p25_handle_grant(opts, state, channel);
}

void
dsd_p25_sm_on_indiv_grant_impl(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    UNUSED(src);
    UNUSED(dst);
    if (!p25_grant_allowed(opts, state, P25_GRANT_INDIV, svc_bits, 0)) {
        return;
    }

    // Proceed with tuned grant
    p25_handle_grant(opts, state, channel);
}

void
dsd_p25_sm_on_release_impl(dsd_opts* opts, dsd_state* state) {
    // Centralized release handling. For TDMA voice channels with two
    // logical slots, do not return to the control channel if the other slot is
    // still active. This prevents dropping an in-progress call on the opposite
    // timeslot when only one slot sends an END/IDLE indication.
    state->p25_sm_release_count++;
    if (state) {
        state->p25_sm_last_release_time = time(NULL);
    }

    int forced = (state && state->p25_sm_force_release) ? 1 : 0;
    if (state) {
        state->p25_sm_force_release = 0; // consume one-shot flag
    }

    const time_t now = time(NULL);
    const double nowm = dsd_time_now_monotonic_s();

    // Snapshot whether we observed MAC_ACTIVE/PTT after the last tune so we
    // can decide later if a retune backoff is appropriate.
    int had_mac_since_tune = 0;
    if (state && state->p25_last_vc_tune_time_m > 0.0) {
        double lmac = state->p25_p2_last_mac_active_m[0];
        double rmac = state->p25_p2_last_mac_active_m[1];
        double tune_m = state->p25_last_vc_tune_time_m;
        if ((lmac > 0.0 && lmac >= tune_m) || (rmac > 0.0 && rmac >= tune_m)) {
            had_mac_since_tune = 1;
        }
    }

    int is_p2_vc = (state && state->p25_p2_active_slot != -1);
    if (is_p2_vc) {
        double mac_hold = p25_cfg_mac_hold(state);
        double ring_hold = p25_cfg_ring_hold(state);
        double extra = p25_cfg_force_rel_extra(state);

        // Clamp ring_hold so ring-gated activity can never outlive the safety-net window
        if (ring_hold > extra) {
            ring_hold = extra;
        }

        p25_slot_activity_basic sa = {0};
        p25_sm_collect_slot_activity(state, mac_hold, ring_hold, nowm, now, &sa);

        double trunk_hang = (opts ? opts->trunk_hangtime : 0.75);
        double dt_voice = p25_elapsed_since(state->last_vc_sync_time_m, state->last_vc_sync_time, nowm, now);
        int recent_voice = (dt_voice <= trunk_hang);

        int left_audio = p25_slot_active_from_summary(&sa, 0, recent_voice);
        int right_audio = p25_slot_active_from_summary(&sa, 1, recent_voice);

        // stale_activity only applies if we have a valid voice sync timestamp;
        // if no voice was ever seen, don't treat activity as stale
        int have_voice_ts = (state->last_vc_sync_time_m > 0.0 || state->last_vc_sync_time != 0);
        int stale_activity = have_voice_ts && (dt_voice > (trunk_hang + 2));

        // Treat forced release as an unconditional directive: ignore audio gates
        // and recent-voice window when explicitly requested by higher layers
        // (e.g., MAC_IDLE on both slots, early ENC lockout, or teardown PDUs).
        if (!forced && !stale_activity && (left_audio || right_audio)) {
            state->p25_sm_mode = DSD_P25_SM_MODE_HANG;
            p25_sm_log_status(opts, state, (!recent_voice) ? "release-deferred-posthang" : "release-deferred-gated");
            return; // keep current VC; do not return to CC yet
        }
        // If neither slot has audio and we were not forced here, still respect
        // a brief hangtime based on the most recent voice activity.
        if (!forced && recent_voice) {
            state->p25_sm_mode = DSD_P25_SM_MODE_HANG;
            p25_sm_log_status(opts, state, "release-delayed-recent");
            return;
        }
    }

    // Either not a TDMA VC or no other slot is active: return to CC.
    p25_sm_log_status(opts, state, "release-cc");
    state->p25_sm_mode = DSD_P25_SM_MODE_RETURNING;
    // Reduce side effects: rely on decode/audio teardown to clear per-slot
    // gates and encryption indicators on TDMA. Always reset the small SM
    // watchdog and clear per-slot audio gates + MAC/ring hints to prevent
    // any residual post-hang activity from carrying over after a return.
    if (forced) {
        p25_clear_p2_call_context(state);
    } else {
        // Light cleanup on non-forced release as well: clear gates + hints.
        p25_clear_p2_gates(state);
    }
    // Reset SM post-hang watchdog
    state->p25_sm_posthang_start = 0;
    state->p25_sm_posthang_start_m = 0.0;
    // Capture last VC frequency before return_to_cc clears it
    long last_vc = state->p25_vc_freq[0] ? state->p25_vc_freq[0] : state->trunk_vc_freq[0];
    int last_slot = state->p25_p2_active_slot;
    return_to_cc(opts, state);
    if (state) {
        state->p25_sm_cc_return_count++;
    }
    p25_sm_log_status(opts, state, "after-release");
    state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
    // Program short backoff against re-tuning the same VC immediately, but
    // only when the last VC had no MAC_ACTIVE/PTT since tune (i.e., a dead
    // grant bounce). For normal calls with voice, allow immediate follow-up
    // grants on the same VC/slot without backoff.
    // Apply only for TDMA voice channels to prevent slot-thrash; allow
    // immediate re-tune on single-carrier voice grants.
    if (last_vc != 0 && last_slot != -1) {
        // Measure how long we were on this VC since the last tune. If we are
        // returning very shortly after tuning (no MAC voice observed), treat
        // this as a pre‑voice bounce and do not apply a retune backoff so a
        // subsequent grant to the same VC/slot can be honored immediately.
        double dt_since_tune =
            (state && state->p25_last_vc_tune_time_m > 0.0) ? (nowm - state->p25_last_vc_tune_time_m) : 1e9;
        double grant_voice_to = p25_cfg_grant_voice_to(state);
        // Consider any sign of voice/audio as proof this was a normal call:
        // - MAC_ACTIVE/PTT since tune (captured in had_mac_since_tune)
        // - Per-slot audio was allowed at release time
        // - Per-slot jitter ring has queued samples
        int had_audio_flags = (state->p25_p2_audio_allowed[0] != 0) || (state->p25_p2_audio_allowed[1] != 0);
        int had_ring_samples = (state->p25_p2_audio_ring_count[0] > 0) || (state->p25_p2_audio_ring_count[1] > 0);
        int had_voice = had_mac_since_tune || had_audio_flags || had_ring_samples;
        int apply_backoff = had_voice ? 0 : 1;
        if (apply_backoff) {
            // Skip backoff for pre‑voice bounce (returned before voice timeout)
            if (dt_since_tune < (grant_voice_to + 0.1)) {
                state->p25_retune_block_slot = -1; // no block
            } else {
                state->p25_retune_block_freq = last_vc;
                state->p25_retune_block_slot = last_slot;
                double backoff_s = p25_cfg_retune_backoff(state);
                state->p25_retune_block_until = time(NULL) + (time_t)(backoff_s + 0.5);
            }
        } else {
            state->p25_retune_block_slot = -1; // disable slot-specific block
        }
    } else {
        state->p25_retune_block_slot = -1;
    }
}

void
dsd_p25_sm_on_neighbor_update_impl(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    UNUSED(opts);
    if (count <= 0 || state == NULL || freqs == NULL) {
        return;
    }
    // Lazy-load any persisted candidates once system identity is known
    p25_cc_try_load_cache(opts, state);
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
        // Track neighbor list for UI
        p25_nb_add(state, f);
        // Add to candidate list (dedup + FIFO rollover)
        if (p25_cc_add_candidate(state, f, 1)) {
            // Optionally log via status tag; detailed frequency print omitted
            p25_sm_log_status(opts, state, "cc-cand-add");
        }
    }
    // Best-effort persistence for warm start in future runs
    p25_cc_persist_cache(opts, state);
    p25_sm_log_status(opts, state, "after-neigh");
}

typedef struct {
    time_t now;
    double nowm;
    double dt;
    double dt_since_tune;
    double vc_grace;
    double mac_hold;
    double ring_hold;
    double extra;
    double margin;
    int is_p2_vc;
    int cur_is_p25p2_sync;
} p25_sm_tick_ctx;

static void
p25_sm_init_tick_ctx(const dsd_state* state, time_t now, double nowm, p25_sm_tick_ctx* ctx) {
    if (!ctx) {
        return;
    }
    ctx->now = now;
    ctx->nowm = nowm;
    ctx->dt = (state && state->last_vc_sync_time_m > 0.0)
                  ? (nowm - state->last_vc_sync_time_m)
                  : ((state && state->last_vc_sync_time != 0) ? (double)(now - state->last_vc_sync_time) : 1e9);
    ctx->dt_since_tune =
        (state && state->p25_last_vc_tune_time_m > 0.0) ? (nowm - state->p25_last_vc_tune_time_m) : 1e9;
    ctx->vc_grace = p25_cfg_vc_grace(state);
    ctx->mac_hold = p25_cfg_mac_hold(state);
    ctx->ring_hold = p25_cfg_ring_hold(state);
    ctx->extra = p25_cfg_force_rel_extra(state);
    ctx->margin = p25_cfg_force_rel_margin(state);
    ctx->is_p2_vc = (state && state->p25_p2_active_slot != -1);
    ctx->cur_is_p25p2_sync = (state && (state->lastsynctype == 35 || state->lastsynctype == 36));
}

static void
p25_sm_update_mode(dsd_opts* opts, dsd_state* state, const p25_sm_tick_ctx* ctx,
                   const p25_slot_activity_basic* sa_follow) {
    if (!opts || !state || !ctx || !sa_follow) {
        return;
    }
    int follow_l = p25_slot_active_from_summary(sa_follow, 0, 1);
    int follow_r = p25_slot_active_from_summary(sa_follow, 1, 1);
    int recent_voice_window =
        (state->last_vc_sync_time_m > 0.0 && (ctx->nowm - state->last_vc_sync_time_m) <= opts->trunk_hangtime);
    if (follow_l || follow_r || recent_voice_window) {
        state->p25_sm_mode = DSD_P25_SM_MODE_FOLLOW;
    } else if (ctx->dt_since_tune < ctx->vc_grace) {
        state->p25_sm_mode = DSD_P25_SM_MODE_ARMED;
    }
}

static double
p25_sm_calc_p1_err_hold(const dsd_state* state, const p25_sm_tick_ctx* ctx) {
    if (!state || !ctx || ctx->is_p2_vc || ctx->dt_since_tune < ctx->vc_grace) {
        return 0.0;
    }
    double avg_pct = -1.0;
    if (state->p25_p1_voice_err_hist_len > 0) {
        avg_pct = (double)state->p25_p1_voice_err_hist_sum / (double)state->p25_p1_voice_err_hist_len;
    }
    double thr_pct = p25_cfg_p1_err_hold_pct(state);
    double add_s = p25_cfg_p1_err_hold_s(state);
    if (avg_pct >= 0.0 && avg_pct >= thr_pct) {
        return add_s;
    }
    return 0.0;
}

static int
p25_sm_safety_net_check(dsd_opts* opts, dsd_state* state, const p25_sm_tick_ctx* ctx, double p1_err_hold_s) {
    if (!opts || !state || !ctx) {
        return 0;
    }
    if (ctx->dt_since_tune >= ctx->vc_grace) {
        if (ctx->dt >= (opts->trunk_hangtime + ctx->extra + ctx->margin)) {
            p25_force_release_to_cc(opts, state, "tick-safety-net-hard", ctx->now, ctx->nowm);
            return 1;
        }
        if (ctx->dt >= (opts->trunk_hangtime + ctx->extra + p1_err_hold_s)) {
            p25_force_release_to_cc(opts, state, "tick-safety-net", ctx->now, ctx->nowm);
            return 1;
        }
    }
    if (ctx->is_p2_vc && !ctx->cur_is_p25p2_sync) {
        int nosync_idle = (state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_ring_count[0] == 0
                           && state->p25_p2_audio_allowed[1] == 0 && state->p25_p2_audio_ring_count[1] == 0);
        if ((nosync_idle && ctx->dt >= opts->trunk_hangtime && ctx->dt_since_tune >= ctx->vc_grace)
            || (ctx->dt >= (opts->trunk_hangtime + ctx->extra) && ctx->dt_since_tune >= ctx->vc_grace)) {
            p25_force_release_to_cc(opts, state, "tick-safety-net-nosync", ctx->now, ctx->nowm);
            return 1;
        }
    }
    return 0;
}

static int
p25_sm_handle_p2_end_teardown(dsd_opts* opts, dsd_state* state, const p25_sm_tick_ctx* ctx) {
    if (!opts || !state || !ctx || !ctx->is_p2_vc) {
        return 0;
    }
    double tail_ms_cfg = p25_cfg_tail_ms(state);
    double mac_hold_end = ctx->mac_hold;

    int ldrain = (state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_ring_count[0] == 0);
    int rdrain = (state->p25_p2_audio_allowed[1] == 0 && state->p25_p2_audio_ring_count[1] == 0);
    double lend_ms = (state->p25_p2_last_end_ptt[0] ? (ctx->now - state->p25_p2_last_end_ptt[0]) * 1000.0 : 0.0);
    double rend_ms = (state->p25_p2_last_end_ptt[1] ? (ctx->now - state->p25_p2_last_end_ptt[1]) * 1000.0 : 0.0);
    int left_end = state->p25_p2_last_end_ptt[0] != 0;
    int right_end = state->p25_p2_last_end_ptt[1] != 0;
    int end_seen = left_end || right_end;

    if (left_end && !ldrain && lend_ms >= tail_ms_cfg) {
        ldrain = 1;
    }
    if (right_end && !rdrain && rend_ms >= tail_ms_cfg) {
        rdrain = 1;
    }

    int l_recent_mac = p25_slot_mac_recent(state, 0, mac_hold_end, ctx->nowm, ctx->now);
    int r_recent_mac = p25_slot_mac_recent(state, 1, mac_hold_end, ctx->nowm, ctx->now);
    int other_active = l_recent_mac || r_recent_mac;

    int l_after_end_mac_recent = 0;
    int r_after_end_mac_recent = 0;
    if (left_end && state->p25_p2_last_mac_active[0] != 0
        && state->p25_p2_last_mac_active[0] >= state->p25_p2_last_end_ptt[0]
        && (double)(ctx->now - state->p25_p2_last_mac_active[0]) <= mac_hold_end) {
        l_after_end_mac_recent = 1;
    }
    if (right_end && state->p25_p2_last_mac_active[1] != 0
        && state->p25_p2_last_mac_active[1] >= state->p25_p2_last_end_ptt[1]
        && (double)(ctx->now - state->p25_p2_last_mac_active[1]) <= mac_hold_end) {
        r_after_end_mac_recent = 1;
    }

    if (end_seen) {
        int drained_both = ldrain && rdrain && !l_after_end_mac_recent && !r_after_end_mac_recent;
        int drained_left_only = left_end && ldrain && !other_active && !l_after_end_mac_recent;
        int drained_right_only = right_end && rdrain && !other_active && !r_after_end_mac_recent;
        if (drained_both || drained_left_only || drained_right_only) {
            p25_force_release_to_cc(opts, state, "release-end-ptt-drain", ctx->now, ctx->nowm);
            return 1;
        }
    }
    return 0;
}

static int
p25_sm_handle_p1_tail_teardown(dsd_opts* opts, dsd_state* state, const p25_sm_tick_ctx* ctx) {
    if (!opts || !state || !ctx || ctx->is_p2_vc) {
        return 0;
    }
    double tail_ms_cfg = p25_cfg_p1_tail_ms(state);
    if ((state->p25_p1_last_tdu_m > 0.0 || state->p25_p1_last_tdu != 0) && ctx->dt_since_tune >= ctx->vc_grace) {
        double since_tdu =
            p25_elapsed_since(state->p25_p1_last_tdu_m, state->p25_p1_last_tdu, ctx->nowm, ctx->now) * 1000.0;
        double since_voice =
            p25_elapsed_since(state->last_vc_sync_time_m, state->last_vc_sync_time, ctx->nowm, ctx->now) * 1000.0;
        if (since_tdu >= tail_ms_cfg && since_voice >= tail_ms_cfg) {
            p25_force_release_to_cc(opts, state, "release-p1-tdu-drain", ctx->now, ctx->nowm);
            return 1;
        }
    }
    return 0;
}

static int
p25_sm_tick_on_vc(dsd_opts* opts, dsd_state* state, time_t now, double nowm) {
    if (!opts || !state || opts->p25_is_tuned != 1) {
        return 0;
    }

    p25_sm_tick_ctx ctx = {0};
    p25_sm_init_tick_ctx(state, now, nowm, &ctx);

    // Update high-level SM mode exposure (ARMED vs FOLLOW) while tuned
    p25_slot_activity_basic sa_follow = {0};
    p25_sm_collect_slot_activity(state, ctx.mac_hold, ctx.mac_hold, nowm, now, &sa_follow);
    p25_sm_update_mode(opts, state, &ctx, &sa_follow);

    double p1_err_hold_s = p25_sm_calc_p1_err_hold(state, &ctx);

    // Safety nets and mismatch guard
    if (p25_sm_safety_net_check(opts, state, &ctx, p1_err_hold_s)) {
        return 1;
    }

    // Early teardown paths
    if (ctx.is_p2_vc) {
        if (p25_sm_handle_p2_end_teardown(opts, state, &ctx)) {
            return 1;
        }
    } else {
        if (p25_sm_handle_p1_tail_teardown(opts, state, &ctx)) {
            return 1;
        }
    }

    // Note: defer the "no sync" forced-release guard until after
    // evaluating per-slot activity to avoid bouncing during brief sync
    // drops when MAC activity indicates an active call.
    p25_slot_activity_basic sa = {0};
    p25_sm_collect_slot_activity(state, ctx.mac_hold, ctx.ring_hold, nowm, now, &sa);

    int left_ring = sa.ring_active[0];
    int right_ring = sa.ring_active[1];
    // If rings are non-zero but older than ring_hold (no recent MAC),
    // they are stale. When beyond hangtime grace, proactively flush to
    // avoid indefinite post-hang gating on residual buffered samples.
    if (ctx.dt >= opts->trunk_hangtime && ctx.dt_since_tune >= ctx.vc_grace) {
        int l_stale_ring = (state->p25_p2_audio_ring_count[0] > 0)
                           && (state->p25_p2_last_mac_active_m[0] <= 0.0
                               || (nowm - state->p25_p2_last_mac_active_m[0]) > ctx.ring_hold);
        int r_stale_ring = (state->p25_p2_audio_ring_count[1] > 0)
                           && (state->p25_p2_last_mac_active_m[1] <= 0.0
                               || (nowm - state->p25_p2_last_mac_active_m[1]) > ctx.ring_hold);
        if (l_stale_ring || r_stale_ring) {
            p25_p2_audio_ring_reset(state, (l_stale_ring && r_stale_ring) ? -1 : (l_stale_ring ? 0 : 1));
            left_ring = right_ring = 0; // treated as drained
            p25_sm_log_status(opts, state, "tick-ring-flush");
        }
    }
    int left_has_audio = sa.audio_allowed[0] || left_ring;
    int right_has_audio = sa.audio_allowed[1] || right_ring;
    // After hangtime, do not let stale audio_allowed hold activity: require
    // recent MAC (handled below) or ring gated by MAC recency.
    if (ctx.dt >= opts->trunk_hangtime) {
        left_has_audio = left_ring;
        right_has_audio = right_ring;
    }

    int left_active = left_has_audio || sa.mac_recent[0];
    int right_active = right_has_audio || sa.mac_recent[1];
    int both_slots_idle = (!ctx.is_p2_vc) ? 1 : !(left_active || right_active);

    // Additional guard: if we have lost sync (no valid synctype) while
    // voice tuned for longer than hangtime + grace, treat as stale VC and
    // force release only when both slots are idle (no recent MAC or
    // MAC-gated jitter activity). This prevents sub-second VC↔CC bounce
    // on marginal signals during real calls.
    if (state->lastsynctype < 0 && ctx.dt_since_tune >= ctx.vc_grace && ctx.dt >= (opts->trunk_hangtime + p1_err_hold_s)
        && both_slots_idle) {
        p25_force_release_to_cc(opts, state, "tick-nosync-release", now, nowm);
        return 1;
    }

    if (ctx.dt < opts->trunk_hangtime || both_slots_idle) {
        // Not in post-hang gating; reset watchdog
        state->p25_sm_posthang_start = 0;
    }

    if (ctx.dt >= (opts->trunk_hangtime + p1_err_hold_s) && both_slots_idle && ctx.dt_since_tune >= ctx.vc_grace) {
        p25_force_release_to_cc(opts, state, "tick-idle-release", now, nowm);
    } else if (ctx.dt >= opts->trunk_hangtime && ctx.dt_since_tune >= ctx.vc_grace && ctx.is_p2_vc
               && !both_slots_idle) {
        // Post-hangtime gating is keeping the VC active (recent MAC or
        // MAC-gated ring). Record a light-weight tag for UI diagnostics.
        // Start/advance watchdog to break pathological wedges where gates
        // are refreshed without genuine voice (e.g., stray vendor PDUs).
        if (state->p25_sm_posthang_start == 0) {
            state->p25_sm_posthang_start = now;
            state->p25_sm_posthang_start_m = nowm;
        } else {
            if ((nowm - state->p25_sm_posthang_start_m) >= ctx.extra) {
                if (opts->verbose > 0) {
                    fprintf(stderr, "\n  P25 SM: Post-hang watchdog forced release (held %.1fs >= extra %.1f)\n",
                            (nowm - state->p25_sm_posthang_start_m), ctx.extra);
                }
                state->p25_sm_posthang_start = 0;
                state->p25_sm_posthang_start_m = 0.0;
                p25_force_release_to_cc(opts, state, "tick-posthang-wd", now, nowm);
                return 1;
            }
        }
        // Post-hang gating: report via status tag only
        p25_sm_log_status(opts, state, "tick-posthang-gate");
    }

    return 0;
}

static int
p25_sm_tick_idle_fallback(dsd_opts* opts, dsd_state* state, time_t now, double nowm) {
    if (!opts || !state) {
        return 0;
    }
    // Final idle fallback: if decode reports we are still on a P25p2 voice
    // sync, but both slots are idle well past hangtime (no audio gate, no
    // MAC-gated jitter, no recent MAC activity), force a release even if the
    // p25_is_tuned flag is inconsistent. This protects against rare flag
    // mismatches that can wedge the tuner on a dead VC while the SM believes
    // it is on CC.
    double dt_v = p25_elapsed_since(state->last_vc_sync_time_m, state->last_vc_sync_time, nowm, now);
    double dt_tune = p25_elapsed_since(state->p25_last_vc_tune_time_m, 0, nowm, now);
    int cur_is_p2 = (state->lastsynctype == 35 || state->lastsynctype == 36);
    double mac_hold_fb = p25_cfg_mac_hold(state);
    double vc_grace_fb = p25_cfg_vc_grace(state);

    p25_slot_activity_basic sa = {0};
    p25_sm_collect_slot_activity(state, mac_hold_fb, mac_hold_fb, nowm, now, &sa);
    int l_ring = sa.ring_active[0];
    int r_ring = sa.ring_active[1];
    int l_act = sa.audio_allowed[0] || l_ring || sa.mac_recent[0];
    int r_act = sa.audio_allowed[1] || r_ring || sa.mac_recent[1];
    int idle = (l_act == 0 && r_act == 0);

    if (cur_is_p2 && idle && dt_v >= opts->trunk_hangtime && dt_tune >= vc_grace_fb) {
        p25_force_release_to_cc(opts, state, "tick-idle-fallback", now, nowm);
        state->p25_sm_mode = DSD_P25_SM_MODE_HUNTING;
        return 1;
    }
    return 0;
}

static void
p25_sm_tick_cc_hunt(dsd_opts* opts, dsd_state* state, double nowm) {
    if (!opts || !state) {
        return;
    }
    if (opts->p25_is_tuned == 0) {
        double dt_cc = (state->last_cc_sync_time_m > 0.0) ? (nowm - state->last_cc_sync_time_m) : 1e9;
        // Add a small grace window before hunting to avoid thrashing on brief
        // CC fades between TSBKs. Allow override via env var DSD_NEO_P25_CC_GRACE.
        double cc_grace = (state->p25_cfg_cc_grace_s > 0.0) ? state->p25_cfg_cc_grace_s : 0.5;
        if (dt_cc > (opts->trunk_hangtime + cc_grace)) {
            state->p25_sm_mode = DSD_P25_SM_MODE_HUNTING;
            int tuned = 0;
            long cand = 0;
            if (opts->p25_prefer_candidates == 1 && p25_sm_next_cc_candidate(state, &cand) && cand != 0) {
                trunk_tune_to_cc(opts, state, cand);
                state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
                // Start CC candidate evaluation
                state->p25_cc_eval_freq = cand;
                state->p25_cc_eval_start_m = nowm;
                if (opts->verbose > 0) {
                    p25_sm_log_status(opts, state, "cc-cand-tune");
                    if (opts->verbose > 1) {
                        p25_sm_log_status(opts, state, "cc-hunt-tune");
                    }
                }
                tuned = 1;
            }

            if (!tuned) {
                if (opts->verbose > 0) {
                    p25_sm_log_status(opts, state, "cc-lost-hunting");
                }
                if (state->lcn_freq_count > 0) {
                    if (state->lcn_freq_roll >= state->lcn_freq_count) {
                        state->lcn_freq_roll = 0; // wrap
                    }
                    if (state->lcn_freq_roll != 0) {
                        long prev = state->trunk_lcn_freq[state->lcn_freq_roll - 1];
                        long cur = state->trunk_lcn_freq[state->lcn_freq_roll];
                        if (prev == cur) {
                            state->lcn_freq_roll++;
                            if (state->lcn_freq_roll >= state->lcn_freq_count) {
                                state->lcn_freq_roll = 0;
                            }
                        }
                    }
                    long f = (state->lcn_freq_roll < state->lcn_freq_count)
                                 ? state->trunk_lcn_freq[state->lcn_freq_roll]
                                 : 0;
                    if (f != 0) {
                        trunk_tune_to_cc(opts, state, f);
                        state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
                        if (opts->verbose > 0) {
                            p25_sm_log_status(opts, state, "cc-fallback-tune");
                        }
                    }
                    state->lcn_freq_roll++;
                }
            }
        }
    }
}

static void
p25_sm_tick_cc_eval(dsd_opts* opts, dsd_state* state, double nowm) {
    if (!opts || !state) {
        return;
    }
    UNUSED(opts);
    // CC candidate evaluation cooldown handler: if we tuned to a CC candidate
    // and no CC activity appeared within a short window, penalize that
    // candidate for a while to avoid immediate re-tries.
    if (state->p25_cc_eval_freq != 0 && state->p25_sm_mode == DSD_P25_SM_MODE_ON_CC) {
        double eval_dt = (state->p25_cc_eval_start_m > 0.0) ? (nowm - state->p25_cc_eval_start_m) : 0.0;
        double eval_window_s = 3.0;
        if (eval_dt >= eval_window_s) {
            int stale = (state->last_cc_sync_time_m == 0.0) || ((nowm - state->last_cc_sync_time_m) >= eval_window_s);
            if (stale) {
                for (int i = 0; i < state->p25_cc_cand_count && i < 16; i++) {
                    if (state->p25_cc_candidates[i] == state->p25_cc_eval_freq) {
                        state->p25_cc_cand_cool_until[i] = nowm + 10.0; // cool down ~10s
                        break;
                    }
                }
            }
            state->p25_cc_eval_freq = 0;
            state->p25_cc_eval_start_m = 0.0;
        }
    }
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
    const double nowm = dsd_time_now_monotonic_s();

    // Drive the minimal follower each tick so it can honor GRANT/ACTIVE/IDLE
    // events observed on both single-carrier and TDMA voice channels. The
    // follower's return callback uses the public SM release path, which keeps
    // existing gating consistent.
    dsd_p25p2_min_tick(dsd_p25p2_min_get(), opts, state);
    int stop = p25_sm_tick_on_vc(opts, state, now, nowm);
    if (stop) {
        return;
    }
    stop = p25_sm_tick_idle_fallback(opts, state, now, nowm);
    if (stop) {
        return;
    }
    p25_sm_tick_cc_hunt(opts, state, nowm);
    p25_sm_tick_cc_eval(opts, state, nowm);

    // Age-out any stale affiliated RIDs (runs at ~1 Hz)
    p25_aff_tick(state);
    // Age-out Group Affiliation pairs
    p25_ga_tick(state);
    // Age-out stale neighbors
    p25_nb_tick(state);
}

int
dsd_p25_sm_next_cc_candidate_impl(dsd_state* state, long* out_freq) {
    if (!state || !out_freq) {
        return 0;
    }
    double nowm = dsd_time_now_monotonic_s();
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
