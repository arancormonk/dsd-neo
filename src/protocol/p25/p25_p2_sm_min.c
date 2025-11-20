// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/protocol/p25/p25_p2_sm_min.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline time_t
now_s(void) {
    return time(NULL);
}

// Forward UI tag helper
static void min_tag(dsd_state* state, const char* tag);

static void
sm_set_state(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state, dsd_p25p2_min_state_e next, const char* reason) {
    if (!sm) {
        return;
    }
    dsd_p25p2_min_state_e prev = sm->state;
    if (prev == next) {
        return;
    }
    sm->state = next;
    if (sm->on_state_change) {
        sm->on_state_change(opts, state, prev, next, reason ? reason : "");
    }
}

void
dsd_p25p2_min_init(dsd_p25p2_min_sm* sm) {
    if (!sm) {
        return;
    }
    memset(sm, 0, sizeof(*sm));
    sm->hangtime_s = 1.0;
    sm->vc_grace_s = 1.5;
    sm->min_follow_dwell_s = 0.7;
    // Raise grant→voice timeout to reduce early VC→CC bounce on systems
    // that delay MAC_PTT/ACTIVE after issuing a grant.
    sm->grant_voice_timeout_s = 4.0;
    // Shorten re‑tune backoff so a quick follow‑up grant to the same VC/slot
    // is not ignored for long after an early (pre‑voice) return.
    sm->retune_backoff_s = 1.0;
    sm->state = DSD_P25P2_MIN_IDLE;
}

void
dsd_p25p2_min_set_callbacks(dsd_p25p2_min_sm* sm, dsd_p25p2_min_on_tune_vc_cb tune_cb,
                            dsd_p25p2_min_on_return_cc_cb ret_cb, dsd_p25p2_min_on_state_change_cb state_cb) {
    if (!sm) {
        return;
    }
    sm->on_tune_vc = tune_cb;
    sm->on_return_cc = ret_cb;
    sm->on_state_change = state_cb;
}

void
dsd_p25p2_min_configure(dsd_p25p2_min_sm* sm, double hangtime_s, double vc_grace_s) {
    if (!sm) {
        return;
    }
    if (hangtime_s >= 0.0) {
        sm->hangtime_s = hangtime_s;
    }
    if (vc_grace_s >= 0.0) {
        sm->vc_grace_s = vc_grace_s;
    }
}

void
dsd_p25p2_min_configure_ex(dsd_p25p2_min_sm* sm, double hangtime_s, double vc_grace_s, double min_follow_dwell_s,
                           double grant_voice_timeout_s, double retune_backoff_s) {
    if (!sm) {
        return;
    }
    dsd_p25p2_min_configure(sm, hangtime_s, vc_grace_s);
    if (min_follow_dwell_s >= 0.0) {
        sm->min_follow_dwell_s = min_follow_dwell_s;
    }
    if (grant_voice_timeout_s >= 0.0) {
        sm->grant_voice_timeout_s = grant_voice_timeout_s;
    }
    if (retune_backoff_s >= 0.0) {
        sm->retune_backoff_s = retune_backoff_s;
    }
}

static void
on_grant(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state, int channel, long freq_hz) {
    time_t t = now_s();
    // Backoff: ignore grants to same freq shortly after a return to avoid ping‑pong
    if (sm->last_return_freq != 0 && sm->t_last_return != 0 && freq_hz != 0) {
        double since_ret = (double)(t - sm->t_last_return);
        if (freq_hz == sm->last_return_freq && since_ret >= 0.0 && since_ret < sm->retune_backoff_s) {
            min_tag(state, "min-ignore-grant-backoff");
            return;
        }
    }
    sm->vc_channel = channel;
    sm->vc_freq_hz = freq_hz;
    sm->t_last_tune = t;
    sm->t_last_voice = 0;
    sm->slot_active[0] = sm->slot_active[1] = 0;
    if (sm->on_tune_vc && freq_hz > 0) {
        sm->on_tune_vc(opts, state, freq_hz, channel);
    }
    // Arm and wait for PTT/ACTIVE before following
    sm_set_state(sm, opts, state, DSD_P25P2_MIN_ARMED, "grant");
}

static void
on_voice(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state, int slot, const char* why) {
    (void)opts;
    (void)state;
    (void)why;
    time_t t = now_s();
    if (slot == 0 || slot == 1) {
        sm->slot_active[slot] = 1;
    }
    sm->t_last_voice = t;
    // If ARMED or HANG, start following and dwell timer
    if (sm->state == DSD_P25P2_MIN_ARMED || sm->state == DSD_P25P2_MIN_HANG) {
        sm_set_state(sm, opts, state, DSD_P25P2_MIN_FOLLOWING_VC, "voice");
        sm->t_follow_start = t;
    }
}

static void
on_quiet(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state, int slot, const char* why) {
    (void)opts;
    (void)state;
    (void)why;
    if (slot == 0 || slot == 1) {
        sm->slot_active[slot] = 0;
    }
    // If both inactive, enter HANG and mark start
    if (sm->slot_active[0] == 0 && sm->slot_active[1] == 0) {
        sm->t_hang_start = now_s();
        sm_set_state(sm, opts, state, DSD_P25P2_MIN_HANG, "quiet");
    }
}

static void
on_nosync(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state) {
    time_t t = now_s();
    double dt_tune = (sm->t_last_tune != 0) ? (double)(t - sm->t_last_tune) : 1e9;
    if ((sm->state == DSD_P25P2_MIN_FOLLOWING_VC || sm->state == DSD_P25P2_MIN_ARMED) && dt_tune >= sm->vc_grace_s) {
        // If not seeing slot activity and past grace, go to HANG
        if (sm->slot_active[0] == 0 && sm->slot_active[1] == 0) {
            sm->t_hang_start = t;
            sm_set_state(sm, opts, state, DSD_P25P2_MIN_HANG, "nosync");
        }
    }
}

static void
maybe_return_cc(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state, const char* why) {
    // Invoke return-to-CC action and transition to IDLE
    sm_set_state(sm, opts, state, DSD_P25P2_MIN_RETURN_CC, why);
    if (sm->on_return_cc) {
        sm->on_return_cc(opts, state);
    }
    // Reset VC context and go IDLE
    sm->last_return_freq = sm->vc_freq_hz;
    sm->t_last_return = now_s();
    sm->vc_freq_hz = 0;
    sm->vc_channel = 0;
    sm->slot_active[0] = sm->slot_active[1] = 0;
    sm->t_last_tune = 0;
    sm->t_last_voice = 0;
    sm->t_hang_start = 0;
    sm->t_follow_start = 0;
    sm_set_state(sm, opts, state, DSD_P25P2_MIN_IDLE, "returned");
}

void
dsd_p25p2_min_handle_event(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state, const dsd_p25p2_min_evt* ev) {
    if (!sm || !ev) {
        return;
    }
    // Apply configuration based on options/env once at first use
    if (opts && sm->hangtime_s == 1.0) {
        double hang = (opts->trunk_hangtime > 0.0) ? opts->trunk_hangtime : sm->hangtime_s;
        // Prefer CLI/opts overrides when provided; fall back to env, then defaults
        double grace = (opts->p25_vc_grace_s > 0.0) ? opts->p25_vc_grace_s : sm->vc_grace_s;
        if (state && state->p25_cfg_vc_grace_s > 0.0) {
            grace = state->p25_cfg_vc_grace_s;
        }
        if (!(opts->p25_vc_grace_s > 0.0)) {
            const char* s = getenv("DSD_NEO_P25_VC_GRACE");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v < 10.0) {
                    grace = v;
                }
            }
        }
        double min_follow =
            (opts->p25_min_follow_dwell_s > 0.0) ? opts->p25_min_follow_dwell_s : sm->min_follow_dwell_s;
        if (state && state->p25_cfg_min_follow_dwell_s > 0.0) {
            min_follow = state->p25_cfg_min_follow_dwell_s;
        }
        if (!(opts->p25_min_follow_dwell_s > 0.0)) {
            const char* s = getenv("DSD_NEO_P25_MIN_FOLLOW_DWELL");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v < 5.0) {
                    min_follow = v;
                }
            }
        }
        double gvt = (opts->p25_grant_voice_to_s > 0.0) ? opts->p25_grant_voice_to_s : sm->grant_voice_timeout_s;
        if (state && state->p25_cfg_grant_voice_to_s > 0.0) {
            gvt = state->p25_cfg_grant_voice_to_s;
        }
        if (!(opts->p25_grant_voice_to_s > 0.0)) {
            const char* s = getenv("DSD_NEO_P25_GRANT_VOICE_TO");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v < 10.0) {
                    gvt = v;
                }
            }
        }
        double backoff = (opts->p25_retune_backoff_s > 0.0) ? opts->p25_retune_backoff_s : sm->retune_backoff_s;
        if (state && state->p25_cfg_retune_backoff_s > 0.0) {
            backoff = state->p25_cfg_retune_backoff_s;
        }
        if (!(opts->p25_retune_backoff_s > 0.0)) {
            const char* s = getenv("DSD_NEO_P25_RETUNE_BACKOFF");
            if (s && s[0] != '\0') {
                double v = atof(s);
                if (v >= 0.0 && v < 10.0) {
                    backoff = v;
                }
            }
        }
        dsd_p25p2_min_configure_ex(sm, hang, grace, min_follow, gvt, backoff);
    }
    switch (ev->type) {
        case DSD_P25P2_MIN_EV_GRANT: on_grant(sm, opts, state, ev->channel, ev->freq_hz); break;
        case DSD_P25P2_MIN_EV_PTT:
        case DSD_P25P2_MIN_EV_ACTIVE:
            on_voice(sm, opts, state, ev->slot, ev->type == DSD_P25P2_MIN_EV_PTT ? "ptt" : "active");
            break;
        case DSD_P25P2_MIN_EV_END:
        case DSD_P25P2_MIN_EV_IDLE:
            on_quiet(sm, opts, state, ev->slot, ev->type == DSD_P25P2_MIN_EV_END ? "end" : "idle");
            break;
        case DSD_P25P2_MIN_EV_NOSYNC: on_nosync(sm, opts, state); break;
        default: break;
    }
}

void
dsd_p25p2_min_tick(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state) {
    if (!sm) {
        return;
    }
    // Apply configuration on first tick if not yet applied
    if (opts && sm->hangtime_s == 1.0) {
        const dsd_p25p2_min_evt ev = {DSD_P25P2_MIN_EV_NOSYNC, -1, 0, 0};
        dsd_p25p2_min_handle_event(sm, opts, state, &ev);
    }
    time_t t = now_s();
    if (sm->state == DSD_P25P2_MIN_ARMED) {
        // Wait for PTT/ACTIVE up to timeout; if none, return to CC
        double dt_tune = (sm->t_last_tune != 0) ? (double)(t - sm->t_last_tune) : 1e9;
        if (dt_tune >= sm->grant_voice_timeout_s && sm->slot_active[0] == 0 && sm->slot_active[1] == 0) {
            maybe_return_cc(sm, opts, state, "armed-timeout");
            return;
        }
    } else if (sm->state == DSD_P25P2_MIN_FOLLOWING_VC) {
        // If no voice observed since tune and beyond grace+hang → return
        double dt_tune = (sm->t_last_tune != 0) ? (double)(t - sm->t_last_tune) : 1e9;
        double dt_voice = (sm->t_last_voice != 0) ? (double)(t - sm->t_last_voice) : 1e9;
        double dt_follow = (sm->t_follow_start != 0) ? (double)(t - sm->t_follow_start) : 0.0;
        if (dt_tune >= sm->vc_grace_s && dt_voice >= sm->hangtime_s && dt_follow >= sm->min_follow_dwell_s
            && sm->slot_active[0] == 0 && sm->slot_active[1] == 0) {
            maybe_return_cc(sm, opts, state, "follow->return");
            return;
        }
    } else if (sm->state == DSD_P25P2_MIN_HANG) {
        double dt_hang = (sm->t_hang_start != 0) ? (double)(t - sm->t_hang_start) : 1e9;
        if (dt_hang >= sm->hangtime_s) {
            maybe_return_cc(sm, opts, state, "hang->return");
            return;
        }
    }
}

// --- Global singleton wiring ---

// Use rigctl/RTL helpers for tune/return actions
extern void trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq);
extern void return_to_cc(dsd_opts* opts, dsd_state* state);

// Note: tuning is performed by higher-level trunking logic. The minimal
// state machine records grant context and transitions state but does not
// directly initiate tuning.

static void
min_return_cb(dsd_opts* opts, dsd_state* state) {
    // Use the public SM release path so teardown resets and backoff are applied
    // consistently across callers.
    // Only force the release when both slots look idle: no audio gate, no
    // queued jitter audio, and no recent MAC_ACTIVE/PTT within the MAC hold
    // window. Otherwise, request a normal (non-forced) release and let the
    // higher-level trunk SM gating decide whether to defer.
    int do_force = 0;
    if (state) {
        double nowm = dsd_time_now_monotonic_s();
        double mac_hold = state->p25_cfg_mac_hold_s > 0.0 ? state->p25_cfg_mac_hold_s : 3.0;
        int l_recent =
            (state->p25_p2_last_mac_active_m[0] > 0.0) && ((nowm - state->p25_p2_last_mac_active_m[0]) <= mac_hold);
        int r_recent =
            (state->p25_p2_last_mac_active_m[1] > 0.0) && ((nowm - state->p25_p2_last_mac_active_m[1]) <= mac_hold);
        int l_gate = state->p25_p2_audio_allowed[0] || state->p25_p2_audio_ring_count[0] > 0 || l_recent;
        int r_gate = state->p25_p2_audio_allowed[1] || state->p25_p2_audio_ring_count[1] > 0 || r_recent;
        do_force = (l_gate == 0 && r_gate == 0);
    }
    if (state && do_force) {
        state->p25_sm_force_release = 1;
    }
    p25_sm_on_release(opts, state);
}

static dsd_p25p2_min_sm g_min_sm;
static int g_min_sm_inited = 0;

// UI tag helper (mirror of p25_sm_log_status ring, minimal version)
static void
min_tag(dsd_state* state, const char* tag) {
    if (!state || !tag || !tag[0]) {
        return;
    }
    snprintf(state->p25_sm_last_reason, sizeof state->p25_sm_last_reason, "%s", tag);
    state->p25_sm_last_reason_time = time(NULL);
    int idx = state->p25_sm_tag_head % 8;
    snprintf(state->p25_sm_tags[idx], sizeof state->p25_sm_tags[idx], "%s", tag);
    state->p25_sm_tag_time[idx] = state->p25_sm_last_reason_time;
    state->p25_sm_tag_head++;
    if (state->p25_sm_tag_count < 8) {
        state->p25_sm_tag_count++;
    }
}

static const char*
min_state_name(dsd_p25p2_min_state_e s) {
    switch (s) {
        case DSD_P25P2_MIN_IDLE: return "IDLE";
        case DSD_P25P2_MIN_ARMED: return "ARMED";
        case DSD_P25P2_MIN_FOLLOWING_VC: return "FOLLOW";
        case DSD_P25P2_MIN_HANG: return "HANG";
        case DSD_P25P2_MIN_RETURN_CC: return "RETURN";
        default: return "?";
    }
}

static void
min_state_cb(dsd_opts* opts, dsd_state* state, dsd_p25p2_min_state_e old_state, dsd_p25p2_min_state_e new_state,
             const char* reason) {
    const char* ns = min_state_name(new_state);
    // Tag: short label for ncurses “SM Tags” line
    if (new_state == DSD_P25P2_MIN_FOLLOWING_VC) {
        min_tag(state, "min-follow");
    } else if (new_state == DSD_P25P2_MIN_ARMED) {
        min_tag(state, "min-armed");
    } else if (new_state == DSD_P25P2_MIN_HANG) {
        min_tag(state, "min-hang");
    } else if (new_state == DSD_P25P2_MIN_RETURN_CC) {
        min_tag(state, "min-return");
    } else if (new_state == DSD_P25P2_MIN_IDLE) {
        min_tag(state, "min-idle");
    }
    // Concise stderr log
    if (opts && opts->verbose > 0) {
        fprintf(stderr, "\n[minSM] %s -> %s (%s)\n", min_state_name(old_state), ns, reason ? reason : "");
    }
}

dsd_p25p2_min_sm*
dsd_p25p2_min_get(void) {
    if (!g_min_sm_inited) {
        dsd_p25p2_min_init(&g_min_sm);
        // Do not perform tuning here; grant handling and gating happen in the
        // higher-level trunking path. Still use state-change and return callbacks
        // so teardown resets are centralized.
        dsd_p25p2_min_set_callbacks(&g_min_sm, /*on_tune_vc*/ NULL, min_return_cb, min_state_cb);
        g_min_sm_inited = 1;
    }
    return &g_min_sm;
}
