// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

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

// Weak fallback so unit tests that link only the P25 library do not need the
// IO Radio library just for Auto-DSP status. Returns zeros.
__attribute__((weak)) void
rtl_stream_auto_dsp_get_status(rtl_auto_dsp_status* out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}
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

// Return 1 when running in a stripped-down "basic" mode that disables added
// safeties/fallbacks and post-hang gating. Enabled via either of these env vars:
//  - DSD_NEO_P25_SM_BASIC=1
//  - DSD_NEO_P25_SM_NO_SAFETY=1
static int
p25_sm_basic_mode(void) {
    const char* s = getenv("DSD_NEO_P25_SM_BASIC");
    if (s && s[0] != '\0' && !(s[0] == '0' || s[0] == 'n' || s[0] == 'N' || s[0] == 'f' || s[0] == 'F')) {
        return 1;
    }
    s = getenv("DSD_NEO_P25_SM_NO_SAFETY");
    if (s && s[0] != '\0' && !(s[0] == '0' || s[0] == 'n' || s[0] == 'N' || s[0] == 'f' || s[0] == 'F')) {
        return 1;
    }
    return 0;
}

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
    int iden = (channel >> 12) & 0xF;
    int is_tdma = 0;
    if (iden >= 0 && iden < 16) {
        is_tdma = (state->p25_chan_tdma[iden] & 0x1) ? 1 : 0;
        if (!is_tdma && state->p25_sys_is_tdma == 1) {
            is_tdma = 1; // conservative fallback until IDEN_UP_TDMA arrives
        }
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

    // Tune via common helper only if not already tuned to this freq. Avoid
    // clearing gates/contexts on duplicate GRANTs while already on the VC,
    // which previously caused audible bounce and premature returns.
    int already_tuned_same = (opts->p25_is_tuned == 1 && state->p25_vc_freq[0] == freq);
    if (!already_tuned_same) {
        trunk_tune_to_freq(opts, state, freq);
        // High-level SM mode: now on VC (grant-following or armed)
        state->p25_sm_mode = DSD_P25_SM_MODE_ON_VC;
        // Reset per-slot audio gate and jitter buffers on new VC
        state->p25_p2_audio_allowed[0] = 0;
        state->p25_p2_audio_allowed[1] = 0;
        p25_p2_audio_ring_reset(state, -1);
        // Clear per-tune adaptive flag so first voice can seed EMA
        state->p25_adapt_updated_for_tune = 0;
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
    int iden = (chan16 >> 12) & 0xF;
    long freq = process_channel_to_freq(opts, state, channel);
    if (freq == 0) {
        return;
    }
    // Determine TDMA slot (when applicable)
    int is_tdma = 0;
    if (iden >= 0 && iden < 16) {
        is_tdma = (state->p25_chan_tdma[iden] & 0x1) ? 1 : 0;
        if (!is_tdma && state->p25_sys_is_tdma == 1) {
            is_tdma = 1; // conservative fallback until IDEN_UP_TDMA arrives
        }
    }
    int grant_slot = is_tdma ? (chan16 & 1) : -1;

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
    p25_tune_to_vc(opts, state, freq, channel);
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
    // VC grace
    state->p25_cfg_vc_grace_s = (opts && opts->p25_vc_grace_s > 0.0) ? opts->p25_vc_grace_s : 1.5;
    if (!(opts && opts->p25_vc_grace_s > 0.0)) {
        const char* sg = getenv("DSD_NEO_P25_VC_GRACE");
        if (sg && sg[0] != '\0') {
            double v = atof(sg);
            if (v >= 0.0 && v < 10.0) {
                state->p25_cfg_vc_grace_s = v;
            }
        }
    }
    // Grant→voice timeout
    state->p25_cfg_grant_voice_to_s = (opts && opts->p25_grant_voice_to_s > 0.0) ? opts->p25_grant_voice_to_s : 4.0;
    if (!(opts && opts->p25_grant_voice_to_s > 0.0)) {
        const char* sg = getenv("DSD_NEO_P25_GRANT_VOICE_TO");
        if (sg && sg[0] != '\0') {
            double v = atof(sg);
            if (v >= 0.0 && v <= 10.0) {
                state->p25_cfg_grant_voice_to_s = v;
            }
        }
    }
    // Minimal follower dwell
    state->p25_cfg_min_follow_dwell_s =
        (opts && opts->p25_min_follow_dwell_s > 0.0) ? opts->p25_min_follow_dwell_s : 0.7;
    if (!(opts && opts->p25_min_follow_dwell_s > 0.0)) {
        const char* sd = getenv("DSD_NEO_P25_MIN_FOLLOW_DWELL");
        if (sd && sd[0] != '\0') {
            double v = atof(sd);
            if (v >= 0.0 && v < 5.0) {
                state->p25_cfg_min_follow_dwell_s = v;
            }
        }
    }
    // Retune backoff
    state->p25_cfg_retune_backoff_s = (opts && opts->p25_retune_backoff_s > 0.0) ? opts->p25_retune_backoff_s : 1.0;
    if (!(opts && opts->p25_retune_backoff_s > 0.0)) {
        const char* sb = getenv("DSD_NEO_P25_RETUNE_BACKOFF");
        if (sb && sb[0] != '\0') {
            double v = atof(sb);
            if (v >= 0.0 && v <= 10.0) {
                state->p25_cfg_retune_backoff_s = v;
            }
        }
    }
    // MAC hold
    state->p25_cfg_mac_hold_s = 3.0;
    {
        const char* sm = getenv("DSD_NEO_P25_MAC_HOLD");
        if (sm && sm[0] != '\0') {
            double v = atof(sm);
            if (v >= 0.0 && v < 10.0) {
                state->p25_cfg_mac_hold_s = v;
            }
        }
    }
    // CC grace
    state->p25_cfg_cc_grace_s = 2.0;
    {
        const char* sc = getenv("DSD_NEO_P25_CC_GRACE");
        if (sc && sc[0] != '\0') {
            double v = atof(sc);
            if (v >= 0.0 && v < 30.0) {
                state->p25_cfg_cc_grace_s = v;
            }
        }
    }
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

    // Centralized gating: ensure we never tune to an encrypted/blocked talkgroup
    // even if a caller forgets to gate before invoking the SM.
    // - Respect ENC lockout based on SVC bits when available
    // - Respect user group list modes ("DE" encrypted lockout, "B" block)
    // - Respect TG Hold when set (allow only the held TG)
    // - Respect Data-call policy (svc bit 0x10) when data tuning is disabled
    if ((svc_bits & 0x10) && opts->trunk_tune_data_calls == 0) {
        p25_sm_log_status(opts, state, "grant-blocked-data");
        return;
    }
    if ((svc_bits & 0x40) && opts->trunk_tune_enc_calls == 0) {
        // Harris regroup/patch override: if GRG policy signals KEY=0000 (clear)
        // for this WGID, do not block tuning even if SVC marks it encrypted.
        if (p25_patch_tg_key_is_clear(state, tg) || p25_patch_sg_key_is_clear(state, tg)) {
            p25_sm_log_status(opts, state, "enc-override-clear");
        } else {
            p25_sm_log_status(opts, state, "grant-blocked-enc");
            // Centralized, once-per-TG emit
            p25_emit_enc_lockout_once(opts, state, 0, tg, svc_bits);
            return;
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
        return;
    }

    // TG Hold: when active, allow only matching TG
    if (state->tg_hold != 0 && (uint32_t)tg != state->tg_hold) {
        p25_sm_log_status(opts, state, "grant-blocked-hold");
        return;
    }

    // Proceed with tuned grant
    p25_handle_grant(opts, state, channel);
}

void
dsd_p25_sm_on_indiv_grant_impl(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    UNUSED(src);
    // Centralized gating mirroring group-grant policy to avoid accidental
    // bypass when new call paths reach the SM directly.
    // 1) Respect Data-call policy (svc bit 0x10) when data tuning is disabled
    if ((svc_bits & 0x10) && opts->trunk_tune_data_calls == 0) {
        p25_sm_log_status(opts, state, "indiv-blocked-data");
        return;
    }
    // 2) Respect Private-call tuning policy
    if (opts->trunk_tune_private_calls == 0) {
        p25_sm_log_status(opts, state, "indiv-blocked-private");
        return;
    }
    // 3) Respect ENC policy (svc bit 0x40) for private calls
    if ((svc_bits & 0x40) && opts->trunk_tune_enc_calls == 0) {
        p25_sm_log_status(opts, state, "indiv-blocked-enc");
        return;
    }
    // 4) TG Hold: when active, suppress individual calls entirely
    if (state->tg_hold != 0) {
        p25_sm_log_status(opts, state, "indiv-blocked-hold");
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
    // Snapshot whether we observed MAC_ACTIVE/PTT after the last tune so we
    // can decide later if a retune backoff is appropriate. This must be
    // captured before we clear per-slot MAC hints below.
    int had_mac_since_tune = 0;
    if (state) {
        time_t t_tune_snapshot = state->p25_last_vc_tune_time;
        if (t_tune_snapshot != 0) {
            time_t lmac_snap = state->p25_p2_last_mac_active[0];
            time_t rmac_snap = state->p25_p2_last_mac_active[1];
            if ((lmac_snap != 0 && lmac_snap >= t_tune_snapshot) || (rmac_snap != 0 && rmac_snap >= t_tune_snapshot)) {
                had_mac_since_tune = 1;
            }
        }
    }
    int is_p2_vc = (state && state->p25_p2_active_slot != -1);
    if ((opts && opts->p25_sm_basic_mode) || p25_sm_basic_mode()) {
        p25_sm_log_status(opts, state, "release-cc");
        state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
        // Flush per-slot audio gates and jitter buffers on release
        state->p25_p2_audio_allowed[0] = 0;
        state->p25_p2_audio_allowed[1] = 0;
        p25_p2_audio_ring_reset(state, -1);
        state->p25_p2_last_mac_active[0] = 0;
        state->p25_p2_last_mac_active[1] = 0;
        snprintf(state->call_string[0], sizeof state->call_string[0], "%s", "                     ");
        snprintf(state->call_string[1], sizeof state->call_string[1], "%s", "                     ");
        state->p25_call_emergency[0] = state->p25_call_emergency[1] = 0;
        state->p25_call_priority[0] = state->p25_call_priority[1] = 0;
        state->payload_algid = state->payload_algidR = 0;
        state->payload_keyid = state->payload_keyidR = 0;
        state->payload_miP = 0;
        state->payload_miN = 0;
        state->p25_p2_enc_pending[0] = state->p25_p2_enc_pending[1] = 0;
        state->p25_p2_enc_pending_ttg[0] = state->p25_p2_enc_pending_ttg[1] = 0;
        state->p25_p2_last_end_ptt[0] = state->p25_p2_last_end_ptt[1] = 0;
        state->p25_call_is_packet[0] = state->p25_call_is_packet[1] = 0;
        state->p25_p1_last_tdu = 0;
        state->p25_sm_posthang_start = 0;
        return_to_cc(opts, state);
        if (state) {
            state->p25_sm_cc_return_count++;
        }
        p25_sm_log_status(opts, state, "after-release");
        return;
    }
    if (is_p2_vc) {
        // Determine activity using P25-specific gates and a short recent-voice window.
        // Do NOT rely on DMR burst flags here; they can be stale across protocol
        // transitions and wedge the SM on a dead VC.
        // Treat a slot as active if audio is allowed, jitter has queued audio,
        // or we saw recent MAC_ACTIVE/PTT on that slot.
        time_t now = time(NULL);
        double mac_hold = 3.0; // seconds; override via DSD_NEO_P25_MAC_HOLD
        {
            const char* s = getenv("DSD_NEO_P25_MAC_HOLD");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v < 10.0) {
                    mac_hold = v;
                }
            }
        }
        // Ignore ring_count as an activity source if no recent MAC_ACTIVE/PTT
        // has been observed on that slot within a short ring-hold window.
        double ring_hold = 0.75; // seconds; override via DSD_NEO_P25_RING_HOLD (clamped to safety-net)
        {
            const char* s = getenv("DSD_NEO_P25_RING_HOLD");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v <= 5.0) {
                    ring_hold = v;
                }
            }
        }
        // Clamp ring_hold so ring-gated activity can never outlive the
        // safety-net window. If EXTRA is smaller, use it as an upper bound.
        {
            double extra = 3.0;
            const char* se = getenv("DSD_NEO_P25_FORCE_RELEASE_EXTRA");
            if (se && se[0] != '\0') {
                double ve = atof(se);
                if (ve >= 0.0 && ve <= 10.0) {
                    extra = ve;
                }
            }
            if (ring_hold > extra) {
                ring_hold = extra;
            }
        }
        int left_ring = (state->p25_p2_audio_ring_count[0] > 0) && (state->p25_p2_last_mac_active[0] != 0)
                        && ((double)(now - state->p25_p2_last_mac_active[0]) <= ring_hold);
        int right_ring = (state->p25_p2_audio_ring_count[1] > 0) && (state->p25_p2_last_mac_active[1] != 0)
                         && ((double)(now - state->p25_p2_last_mac_active[1]) <= ring_hold);
        int mac_recent_l =
            (state->p25_p2_last_mac_active[0] != 0 && (double)(now - state->p25_p2_last_mac_active[0]) <= mac_hold);
        int mac_recent_r =
            (state->p25_p2_last_mac_active[1] != 0 && (double)(now - state->p25_p2_last_mac_active[1]) <= mac_hold);
        int left_audio = (state->p25_p2_audio_allowed[0] != 0) || left_ring || mac_recent_l;
        int right_audio = (state->p25_p2_audio_allowed[1] != 0) || right_ring || mac_recent_r;
        double trunk_hang = (opts ? opts->trunk_hangtime : 1.0);
        int recent_voice = (state->last_vc_sync_time != 0 && (now - state->last_vc_sync_time) <= trunk_hang);
        // After hangtime, require recent MAC activity (or ring gated by MAC) to
        // treat a slot as active; ignore stale audio_allowed alone.
        if (!recent_voice) {
            left_audio = (left_ring || mac_recent_l);
            right_audio = (right_ring || mac_recent_r);
        }
        int stale_activity = (state->last_vc_sync_time != 0 && (now - state->last_vc_sync_time) > (trunk_hang + 2));
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
    state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
    // Reduce side effects: rely on decode/audio teardown to clear per-slot
    // gates and encryption indicators on TDMA. Always reset the small SM
    // watchdog and clear per-slot audio gates + MAC/ring hints to prevent
    // any residual post-hang activity from carrying over after a return.
    if (forced) {
        // Full cleanup (legacy behavior)
        state->p25_p2_audio_allowed[0] = 0;
        state->p25_p2_audio_allowed[1] = 0;
        p25_p2_audio_ring_reset(state, -1);
        state->p25_p2_last_mac_active[0] = 0;
        state->p25_p2_last_mac_active[1] = 0;
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
    } else {
        // Light cleanup on non-forced release as well: clear gates + hints.
        state->p25_p2_audio_allowed[0] = 0;
        state->p25_p2_audio_allowed[1] = 0;
        p25_p2_audio_ring_reset(state, -1);
        state->p25_p2_last_mac_active[0] = 0;
        state->p25_p2_last_mac_active[1] = 0;
    }
    // Reset SM post-hang watchdog
    state->p25_sm_posthang_start = 0;
    // Capture last VC frequency before return_to_cc clears it
    long last_vc = state->p25_vc_freq[0] ? state->p25_vc_freq[0] : state->trunk_vc_freq[0];
    int last_slot = state->p25_p2_active_slot;
    return_to_cc(opts, state);
    if (state) {
        state->p25_sm_cc_return_count++;
    }
    p25_sm_log_status(opts, state, "after-release");
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
        time_t now_bk = time(NULL);
        double dt_since_tune =
            (state && state->p25_last_vc_tune_time != 0) ? (double)(now_bk - state->p25_last_vc_tune_time) : 1e9;
        double grant_voice_to =
            (opts && opts->p25_grant_voice_to_s > 0.0) ? opts->p25_grant_voice_to_s : 4.0; // seconds
        if (!(opts && opts->p25_grant_voice_to_s > 0.0)) {
            const char* sg = getenv("DSD_NEO_P25_GRANT_VOICE_TO");
            if (sg && sg[0] != '\0') {
                double vg = atof(sg);
                if (vg >= 0.0 && vg <= 10.0) {
                    grant_voice_to = vg;
                }
            }
        }
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
                double backoff_s = (state->p25_cfg_retune_backoff_s > 0.0) ? state->p25_cfg_retune_backoff_s : 1.0;
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
        // Optionally log via status tag; detailed frequency print omitted
        p25_sm_log_status(opts, state, "cc-cand-add");
    }
    // Best-effort persistence for warm start in future runs
    p25_cc_persist_cache(opts, state);
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
    const double nowm = dsd_time_now_monotonic_s();

    // Drive the minimal follower each tick so it can honor GRANT/ACTIVE/IDLE
    // events observed on both single-carrier and TDMA voice channels. The
    // follower's return callback uses the public SM release path, which keeps
    // existing gating consistent.
    dsd_p25p2_min_tick(dsd_p25p2_min_get(), opts, state);

    // If currently tuned to a P25 VC and we've observed no recent voice
    // activity for longer than hangtime, force a safe return to CC. This
    // complements the inline fallbacks in the TDMA frame path and protects
    // against cases where frame processing halts due to signal loss.
    if (opts->p25_is_tuned == 1) {
        double dt = (state->last_vc_sync_time_m > 0.0) ? (nowm - state->last_vc_sync_time_m) : 1e9;
        double dt_since_tune = (state->p25_last_vc_tune_time_m > 0.0) ? (nowm - state->p25_last_vc_tune_time_m) : 1e9;
        // Small startup grace window after a VC tune to avoid bouncing back to
        // CC before MAC_PTT/ACTIVE and audio arrive. Override via
        // DSD_NEO_P25_VC_GRACE (seconds).
        double vc_grace = state->p25_cfg_vc_grace_s;
        // Prefer adapted value when enabled
        if (opts->p25_auto_adapt == 1 && state->p25_adapt_vc_grace_s > 0.0) {
            vc_grace = state->p25_adapt_vc_grace_s;
        }
        int is_p2_vc = (state->p25_p2_active_slot != -1);

        // Note: do not unconditionally release here; rely on the post-hang
        // gating and safety-net logic below. An unconditional release causes
        // premature VC->CC bounce before MAC/voice activity appears.
        if ((opts && opts->p25_sm_basic_mode) || p25_sm_basic_mode()) {
            // Basic mode: unconditionally release after hangtime (and grace)
            // when voice activity is stale, without post-hang gating or extra
            // safety windows.
            if (dt_since_tune >= vc_grace && dt >= opts->trunk_hangtime) {
                p25_sm_log_status(opts, state, "tick-basic-release");
                if (state->p25_cc_freq != 0) {
                    state->p25_sm_force_release = 1;
                    p25_sm_on_release(opts, state);
                } else {
                    state->p25_p2_audio_allowed[0] = 0;
                    state->p25_p2_audio_allowed[1] = 0;
                    state->p25_p2_active_slot = -1;
                    state->p25_vc_freq[0] = 0;
                    state->p25_vc_freq[1] = 0;
                    opts->p25_is_tuned = 0;
                    opts->trunk_is_tuned = 0;
                    state->last_cc_sync_time = now;
                    state->last_cc_sync_time_m = nowm;
                }
                p25_sm_log_status(opts, state, "tick-basic-release");
                return;
            }
        }
        int cur_is_p25p2_sync = (state->lastsynctype == 35 || state->lastsynctype == 36);

        // Optional Phase 1 elevated-error hold: when average IMBE error is
        // elevated on P25p1, extend hangtime-based release checks slightly to
        // avoid VC↔CC thrash on marginal signals.
        double p1_err_hold_s = 0.0;
        if (!is_p2_vc && dt_since_tune >= vc_grace) {
            double thr_pct = (opts->p25_p1_err_hold_pct > 0.0) ? opts->p25_p1_err_hold_pct : 8.0; // default threshold
            double add_s = (opts->p25_p1_err_hold_s > 0.0) ? opts->p25_p1_err_hold_s : 2.0;       // additional seconds
            if (!(opts->p25_p1_err_hold_pct > 0.0)) {
                const char* sp = getenv("DSD_NEO_P25P1_ERR_HOLD_PCT");
                if (sp && sp[0] != '\0') {
                    double v = atof(sp);
                    if (v >= 0.0 && v <= 100.0) {
                        thr_pct = v;
                    }
                }
            }
            if (!(opts->p25_p1_err_hold_s > 0.0)) {
                const char* ss = getenv("DSD_NEO_P25P1_ERR_HOLD_S");
                if (ss && ss[0] != '\0') {
                    double v = atof(ss);
                    if (v >= 0.0 && v <= 10.0) {
                        add_s = v;
                    }
                }
            }
            double avg_pct = -1.0;
            if (state->p25_p1_voice_err_hist_len > 0) {
                int len = state->p25_p1_voice_err_hist_len;
                if (len > 0) {
                    avg_pct = (double)state->p25_p1_voice_err_hist_sum / (double)len;
                }
            }
#ifdef USE_RTLSDR
            if (avg_pct < 0.0) {
                rtl_auto_dsp_status st = {0};
                rtl_stream_auto_dsp_get_status(&st);
                if (st.p25p1_ema_pct > 0) {
                    avg_pct = (double)st.p25p1_ema_pct;
                }
            }
#endif
            if (avg_pct >= 0.0 && avg_pct >= thr_pct) {
                p1_err_hold_s = add_s;
            }
        }

        // Global safety net: after hangtime + EXTRA seconds without voice,
        // force release regardless of per-slot gates or jitter. Place this
        // early to avoid being short-circuited by post-hang gating branches.
        {
            double extra = (opts->p25_force_release_extra_s > 0.0) ? opts->p25_force_release_extra_s : 3.0;
            if (!(opts->p25_force_release_extra_s > 0.0)) {
                const char* s = getenv("DSD_NEO_P25_FORCE_RELEASE_EXTRA");
                if (s && s[0] != '\0') {
                    double v = atof(s);
                    if (v >= 0.0 && v <= 10.0) {
                        extra = v;
                    }
                }
            }
            if (dt >= (opts->trunk_hangtime + extra + p1_err_hold_s) && dt_since_tune >= vc_grace) {
                if (opts->verbose > 1) {
                    p25_sm_log_status(opts, state, "tick-safety-net");
                }
                if (state->p25_cc_freq != 0) {
                    state->p25_sm_force_release = 1;
                    p25_sm_on_release(opts, state);
                } else {
                    state->p25_p2_audio_allowed[0] = 0;
                    state->p25_p2_audio_allowed[1] = 0;
                    state->p25_p2_active_slot = -1;
                    state->p25_vc_freq[0] = 0;
                    state->p25_vc_freq[1] = 0;
                    opts->p25_is_tuned = 0;
                    opts->trunk_is_tuned = 0;
                    state->last_cc_sync_time = now;
                    state->last_cc_sync_time_m = nowm;
                }
                p25_sm_log_status(opts, state, "tick-safety-net");
                return; // done this tick
            }
        }

        // Ultra-failsafe: after hangtime + (extra + margin) seconds, force
        // release unconditionally (regardless of slot activity, sync, or
        // ring/MAC gating). This prevents any lingering state from wedging
        // the tuner on a dead VC under unexpected conditions.
        {
            double extra = (opts->p25_force_release_extra_s > 0.0) ? opts->p25_force_release_extra_s : 3.0;
            if (!(opts->p25_force_release_extra_s > 0.0)) {
                const char* se = getenv("DSD_NEO_P25_FORCE_RELEASE_EXTRA");
                if (se && se[0] != '\0') {
                    double ve = atof(se);
                    if (ve >= 0.0 && ve <= 10.0) {
                        extra = ve;
                    }
                }
            }
            double margin = (opts->p25_force_release_margin_s > 0.0) ? opts->p25_force_release_margin_s : 5.0;
            if (!(opts->p25_force_release_margin_s > 0.0)) {
                const char* sm = getenv("DSD_NEO_P25_FORCE_RELEASE_MARGIN");
                if (sm && sm[0] != '\0') {
                    double vm = atof(sm);
                    if (vm >= 0.0 && vm <= 30.0) {
                        margin = vm;
                    }
                }
            }
            if (dt >= (opts->trunk_hangtime + extra + margin) && dt_since_tune >= vc_grace) {
                if (state->p25_cc_freq != 0) {
                    state->p25_sm_force_release = 1;
                    p25_sm_on_release(opts, state);
                } else {
                    state->p25_p2_audio_allowed[0] = 0;
                    state->p25_p2_audio_allowed[1] = 0;
                    state->p25_p2_active_slot = -1;
                    state->p25_vc_freq[0] = 0;
                    state->p25_vc_freq[1] = 0;
                    opts->p25_is_tuned = 0;
                    opts->trunk_is_tuned = 0;
                    state->last_cc_sync_time = now;
                }
                p25_sm_log_status(opts, state, "tick-safety-net-hard");
                return;
            }
        }

        // Mismatch guard: if we still believe we are on a TDMA VC
        // (per-slot SM state), but current sync is not TDMA and hangtime
        // + extra has elapsed since last voice, treat this as a stale P2 VC
        // and force release. This covers cases where the decoder drifted to a
        // P1/CC context but stale P2 gates kept post-hang logic alive.
        if (is_p2_vc && !cur_is_p25p2_sync) {
            double extra = 3.0;
            const char* s = getenv("DSD_NEO_P25_FORCE_RELEASE_EXTRA");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v <= 10.0) {
                    extra = v;
                }
            }
            // Prefer an earlier teardown on no-sync when both slots show no
            // audio or MAC-gated jitter, ignoring mac_recent holds. This
            // prevents multi-second wedges after MAC_END/IDLE storms.
            int nosync_idle = (state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_ring_count[0] == 0
                               && state->p25_p2_audio_allowed[1] == 0 && state->p25_p2_audio_ring_count[1] == 0);
            if ((nosync_idle && dt >= opts->trunk_hangtime && dt_since_tune >= vc_grace)
                || (dt >= (opts->trunk_hangtime + extra) && dt_since_tune >= vc_grace)) {
                if (opts->verbose > 0) {
                    p25_sm_log_status(opts, state, "tick-safety-net-nosync");
                }
                if (state->p25_cc_freq != 0) {
                    state->p25_sm_force_release = 1;
                    p25_sm_on_release(opts, state);
                } else {
                    state->p25_p2_audio_allowed[0] = 0;
                    state->p25_p2_audio_allowed[1] = 0;
                    state->p25_p2_active_slot = -1;
                    state->p25_vc_freq[0] = 0;
                    state->p25_vc_freq[1] = 0;
                    opts->p25_is_tuned = 0;
                    opts->trunk_is_tuned = 0;
                    state->last_cc_sync_time = now;
                }
                p25_sm_log_status(opts, state, "tick-safety-net-nosync");
                return;
            }
        }

        // Early teardown on MAC_END_PTT once per-slot jitter/audio drains.
        // If an END_PTT was observed and both slots have drained (or the
        // opposite slot is not active), return to CC immediately without
        // waiting for MAC_IDLE.
        if (is_p2_vc) {
            double tail_ms_cfg = 500.0; // max tail wait
            {
                const char* s = getenv("DSD_NEO_P25_TAIL_MS");
                if (s && s[0] != '\0') {
                    double v = atof(s);
                    if (v >= 0.0 && v <= 5000.0) {
                        tail_ms_cfg = v;
                    }
                }
            }
            int ldrain = (state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_ring_count[0] == 0);
            int rdrain = (state->p25_p2_audio_allowed[1] == 0 && state->p25_p2_audio_ring_count[1] == 0);
            double lend_ms = (state->p25_p2_last_end_ptt[0] ? (now - state->p25_p2_last_end_ptt[0]) * 1000.0 : 0.0);
            double rend_ms = (state->p25_p2_last_end_ptt[1] ? (now - state->p25_p2_last_end_ptt[1]) * 1000.0 : 0.0);
            int left_end = state->p25_p2_last_end_ptt[0] != 0;
            int right_end = state->p25_p2_last_end_ptt[1] != 0;
            int end_seen = left_end || right_end;
            // Treat tail exceeded as drained
            if (left_end && !ldrain && lend_ms >= tail_ms_cfg) {
                ldrain = 1;
            }
            if (right_end && !rdrain && rend_ms >= tail_ms_cfg) {
                rdrain = 1;
            }
            // Opposite slot activity consideration (recent MAC_ACTIVE hold)
            int other_active = 0;
            double mac_hold = 3.0;
            {
                const char* s = getenv("DSD_NEO_P25_MAC_HOLD");
                if (s && s[0] != '\0') {
                    double v = atof(s);
                    if (v >= 0.0 && v < 10.0) {
                        mac_hold = v;
                    }
                }
            }
            if (state->p25_p2_last_mac_active[0] != 0 && (double)(now - state->p25_p2_last_mac_active[0]) <= mac_hold) {
                other_active = 1;
            }
            if (state->p25_p2_last_mac_active[1] != 0 && (double)(now - state->p25_p2_last_mac_active[1]) <= mac_hold) {
                other_active = 1;
            }
            if (end_seen) {
                int drained_both = ldrain && rdrain;
                int drained_left_only = left_end && ldrain && !other_active;
                int drained_right_only = right_end && rdrain && !other_active;
                if (drained_both || drained_left_only || drained_right_only) {
                    if (opts->verbose > 0) {
                        p25_sm_log_status(opts, state, "release-end-ptt-drain");
                    }
                    if (state->p25_cc_freq != 0) {
                        state->p25_sm_force_release = 1;
                        p25_sm_on_release(opts, state);
                    } else {
                        state->p25_p2_audio_allowed[0] = 0;
                        state->p25_p2_audio_allowed[1] = 0;
                        state->p25_p2_active_slot = -1;
                        state->p25_vc_freq[0] = 0;
                        state->p25_vc_freq[1] = 0;
                        opts->p25_is_tuned = 0;
                        opts->trunk_is_tuned = 0;
                        state->last_cc_sync_time = now;
                    }
                    return;
                }
            }
        } else {
            // Phase 1 early teardown: if TDULC/TDU was observed recently and
            // no new voice has refreshed last_vc_sync_time within a small tail
            // window, return to CC immediately (no need to wait for LCW).
            double tail_ms_cfg = 300.0; // default 300ms for P1
            {
                const char* s = getenv("DSD_NEO_P25P1_TAIL_MS");
                if (s && s[0] != '\0') {
                    double v = atof(s);
                    if (v >= 0.0 && v <= 3000.0) {
                        tail_ms_cfg = v;
                    }
                }
            }
            if (state->p25_p1_last_tdu != 0 && dt_since_tune >= vc_grace) {
                double since_tdu = (double)(now - state->p25_p1_last_tdu) * 1000.0;
                double since_voice =
                    (state->last_vc_sync_time != 0) ? (double)(now - state->last_vc_sync_time) * 1000.0 : 1e12;
                if (since_tdu >= tail_ms_cfg && since_voice >= tail_ms_cfg) {
                    p25_sm_log_status(opts, state, "release-p1-tdu-drain");
                    if (state->p25_cc_freq != 0) {
                        state->p25_sm_force_release = 1;
                        p25_sm_on_release(opts, state);
                    } else {
                        opts->p25_is_tuned = 0;
                        opts->trunk_is_tuned = 0;
                        state->p25_vc_freq[0] = 0;
                        state->p25_vc_freq[1] = 0;
                        state->last_cc_sync_time = now;
                    }
                    return;
                }
            }
        }

        // Note: defer the "no sync" forced-release guard until after
        // evaluating per-slot activity to avoid bouncing during brief sync
        // drops when MAC activity indicates an active call.
        // Determine per-slot activity: use audio gate or queued audio. Also
        // honor recent MAC_ACTIVE/PTT indications to bridge initial setup and
        // short fades; after hangtime expires, ignore stale MAC flags but keep
        // a small recent MAC hold window.
        // Treat jitter ring_count as activity only if we have seen a recent
        // MAC_ACTIVE/PTT on that slot. This avoids stale ring_count values
        // holding the SM release gate when decode stalls (e.g., ncurses UI).
        double ring_hold = 0.75; // seconds; override via DSD_NEO_P25_RING_HOLD
        {
            const char* s = getenv("DSD_NEO_P25_RING_HOLD");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v <= 5.0) {
                    ring_hold = v;
                }
            }
        }
        int left_ring = (state->p25_p2_audio_ring_count[0] > 0) && (state->p25_p2_last_mac_active_m[0] > 0.0)
                        && ((nowm - state->p25_p2_last_mac_active_m[0]) <= ring_hold);
        int right_ring = (state->p25_p2_audio_ring_count[1] > 0) && (state->p25_p2_last_mac_active_m[1] > 0.0)
                         && ((nowm - state->p25_p2_last_mac_active_m[1]) <= ring_hold);
        // If rings are non-zero but older than ring_hold (no recent MAC),
        // they are stale. When beyond hangtime grace, proactively flush to
        // avoid indefinite post-hang gating on residual buffered samples.
        if (dt >= opts->trunk_hangtime && dt_since_tune >= vc_grace) {
            int l_stale_ring = (state->p25_p2_audio_ring_count[0] > 0)
                               && (state->p25_p2_last_mac_active_m[0] <= 0.0
                                   || (nowm - state->p25_p2_last_mac_active_m[0]) > ring_hold);
            int r_stale_ring = (state->p25_p2_audio_ring_count[1] > 0)
                               && (state->p25_p2_last_mac_active_m[1] <= 0.0
                                   || (nowm - state->p25_p2_last_mac_active_m[1]) > ring_hold);
            if (l_stale_ring || r_stale_ring) {
                p25_p2_audio_ring_reset(state, (l_stale_ring && r_stale_ring) ? -1 : (l_stale_ring ? 0 : 1));
                left_ring = right_ring = 0; // treated as drained
                p25_sm_log_status(opts, state, "tick-ring-flush");
            }
        }
        int left_has_audio = state->p25_p2_audio_allowed[0] || left_ring;
        int right_has_audio = state->p25_p2_audio_allowed[1] || right_ring;
        // After hangtime, do not let stale audio_allowed hold activity: require
        // recent MAC (handled below) or ring gated by MAC recency.
        if (dt >= opts->trunk_hangtime) {
            left_has_audio = left_ring;
            right_has_audio = right_ring;
        }
        int left_active = left_has_audio;
        int right_active = right_has_audio;
        double mac_hold = state->p25_cfg_mac_hold_s;
        if (state->p25_p2_last_mac_active_m[0] > 0.0 && (nowm - state->p25_p2_last_mac_active_m[0]) <= mac_hold) {
            left_active = 1;
        }
        if (state->p25_p2_last_mac_active_m[1] > 0.0 && (nowm - state->p25_p2_last_mac_active_m[1]) <= mac_hold) {
            right_active = 1;
        }
        int both_slots_idle = (!is_p2_vc) ? 1 : !(left_active || right_active);

        // Additional guard: if we have lost sync (no valid synctype) while
        // voice tuned for longer than hangtime + grace, treat as stale VC and
        // force release only when both slots are idle (no recent MAC or
        // MAC-gated jitter activity). This prevents sub-second VC↔CC bounce
        // on marginal signals during real calls.
        if (state->lastsynctype < 0 && dt_since_tune >= vc_grace && dt >= (opts->trunk_hangtime + p1_err_hold_s)
            && both_slots_idle) {
            if (opts->verbose > 0) {
                p25_sm_log_status(opts, state, "tick-nosync-release");
            }
            if (state->p25_cc_freq != 0) {
                state->p25_sm_force_release = 1;
                p25_sm_on_release(opts, state);
            } else {
                state->p25_p2_audio_allowed[0] = 0;
                state->p25_p2_audio_allowed[1] = 0;
                state->p25_p2_active_slot = -1;
                state->p25_vc_freq[0] = 0;
                state->p25_vc_freq[1] = 0;
                opts->p25_is_tuned = 0;
                opts->trunk_is_tuned = 0;
                state->last_cc_sync_time = now;
            }
            return; // done this tick
        }
        if (dt < opts->trunk_hangtime || both_slots_idle) {
            // Not in post-hang gating; reset watchdog
            state->p25_sm_posthang_start = 0;
        }
        if (dt >= (opts->trunk_hangtime + p1_err_hold_s) && both_slots_idle && dt_since_tune >= vc_grace) {
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
        } else if (dt >= opts->trunk_hangtime && dt_since_tune >= vc_grace && is_p2_vc && !both_slots_idle) {
            // Post-hangtime gating is keeping the VC active (recent MAC or
            // MAC-gated ring). Record a light-weight tag for UI diagnostics.
            // Start/advance watchdog to break pathological wedges where gates
            // are refreshed without genuine voice (e.g., stray vendor PDUs).
            if (state->p25_sm_posthang_start == 0) {
                state->p25_sm_posthang_start = now;
            } else {
                double extra = 3.0;
                const char* s = getenv("DSD_NEO_P25_FORCE_RELEASE_EXTRA");
                if (s && s[0] != '\0') {
                    double v = atof(s);
                    if (v >= 0.0 && v <= 10.0) {
                        extra = v;
                    }
                }
                if ((double)(now - state->p25_sm_posthang_start) >= extra) {
                    if (opts->verbose > 0) {
                        fprintf(stderr, "\n  P25 SM: Post-hang watchdog forced release (held %.1fs >= extra %.1f)\n",
                                (double)(now - state->p25_sm_posthang_start), extra);
                    }
                    state->p25_sm_posthang_start = 0;
                    if (state->p25_cc_freq != 0) {
                        state->p25_sm_force_release = 1;
                        p25_sm_on_release(opts, state);
                    } else {
                        state->p25_p2_audio_allowed[0] = 0;
                        state->p25_p2_audio_allowed[1] = 0;
                        state->p25_p2_active_slot = -1;
                        state->p25_vc_freq[0] = 0;
                        state->p25_vc_freq[1] = 0;
                        opts->p25_is_tuned = 0;
                        opts->trunk_is_tuned = 0;
                        state->last_cc_sync_time = now;
                    }
                    return;
                }
            }
            // Post-hang gating: report via status tag only
            p25_sm_log_status(opts, state, "tick-posthang-gate");
        }
    }

    // Final idle fallback: if decode reports we are still on a P25p2 voice
    // sync, but both slots are idle well past hangtime (no audio gate, no
    // MAC-gated jitter, no recent MAC activity), force a release even if the
    // p25_is_tuned flag is inconsistent. This protects against rare flag
    // mismatches that can wedge the tuner on a dead VC while the SM believes
    // it is on CC.
    {
        const time_t now2 = now;
        double dt_v = (state->last_vc_sync_time_m > 0.0) ? (nowm - state->last_vc_sync_time_m) : 1e9;
        double dt_tune = (state->p25_last_vc_tune_time_m > 0.0) ? (nowm - state->p25_last_vc_tune_time_m) : 1e9;
        int cur_is_p2 = (state->lastsynctype == 35 || state->lastsynctype == 36);
        double mac_hold2 = state->p25_cfg_mac_hold_s;
        int l_recent =
            (state->p25_p2_last_mac_active[0] != 0) && ((double)(now2 - state->p25_p2_last_mac_active[0]) <= mac_hold2);
        int r_recent =
            (state->p25_p2_last_mac_active[1] != 0) && ((double)(now2 - state->p25_p2_last_mac_active[1]) <= mac_hold2);
        int l_ring = (state->p25_p2_audio_ring_count[0] > 0) && l_recent;
        int r_ring = (state->p25_p2_audio_ring_count[1] > 0) && r_recent;
        int l_act = state->p25_p2_audio_allowed[0] || l_ring || l_recent;
        int r_act = state->p25_p2_audio_allowed[1] || r_ring || r_recent;
        int idle = (l_act == 0 && r_act == 0);
        double vc_grace2 = 1.5;
        if (opts->p25_auto_adapt == 1 && state->p25_adapt_vc_grace_s > 0.0) {
            vc_grace2 = state->p25_adapt_vc_grace_s;
        } else {
            const char* s = getenv("DSD_NEO_P25_VC_GRACE");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v < 10.0) {
                    vc_grace2 = v;
                }
            }
        }
        if (cur_is_p2 && idle && dt_v >= opts->trunk_hangtime && dt_tune >= vc_grace2) {
            if (state->p25_cc_freq != 0) {
                state->p25_sm_force_release = 1;
                p25_sm_on_release(opts, state);
            } else {
                // Minimal teardown to enable CC hunt
                state->p25_p2_audio_allowed[0] = 0;
                state->p25_p2_audio_allowed[1] = 0;
                state->p25_p2_active_slot = -1;
                state->p25_vc_freq[0] = 0;
                state->p25_vc_freq[1] = 0;
                opts->p25_is_tuned = 0;
                opts->trunk_is_tuned = 0;
                state->last_cc_sync_time = now2;
                state->last_cc_sync_time_m = nowm;
                state->p25_sm_mode = DSD_P25_SM_MODE_HUNTING;
            }
            p25_sm_log_status(opts, state, "tick-idle-fallback");
            return;
        }
    }

    // If not currently voice-tuned and we haven't observed CC sync for a
    // while, proactively hunt for a CC. Prefer learned candidates first, then
    // fall back to the imported LCN/frequency list.
    if (opts->p25_is_tuned == 0) {
        double dt_cc = (state->last_cc_sync_time_m > 0.0) ? (nowm - state->last_cc_sync_time_m) : 1e9;
        // Add a small grace window before hunting to avoid thrashing on brief
        // CC fades between TSBKs. Allow override via env var DSD_NEO_P25_CC_GRACE.
        double cc_grace = state->p25_cfg_cc_grace_s;
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
