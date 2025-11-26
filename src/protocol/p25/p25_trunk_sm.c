// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * P25 trunk state machine utilities.
 *
 * This file provides weak fallbacks for tuning functions, encryption lockout,
 * and affiliation/group-affiliation table helpers used by the unified v2 state
 * machine in p25_trunk_sm_v2.c.
 */

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_sm_ui.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Weak Fallbacks for Tuning Functions
 *
 * Unit tests that link only the P25 library can use these stubs. The real
 * implementations live in src/io/control/dsd_rigctl.c.
 * ============================================================================ */

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

__attribute__((weak)) void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
}

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

/* ============================================================================
 * Weak Fallbacks for Event History
 * ============================================================================ */

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

/* ============================================================================
 * Encryption Lockout Helper
 * ============================================================================ */

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
        if (strcmp(state->group_array[idx].groupName, "") == 0) {
            snprintf(state->group_array[idx].groupName, sizeof state->group_array[idx].groupName, "%s", "ENC LO");
        }
    }

    // Prepare per-slot context
    if ((slot & 1) == 0) {
        state->lasttg = (uint32_t)tg;
        state->dmr_so = (uint16_t)svc_bits;
    } else {
        state->lasttgR = (uint32_t)tg;
        state->dmr_soR = (uint16_t)svc_bits;
    }
    state->gi[slot & 1] = 0;

    // Compose event text and push
    Event_History_I* eh = (state->event_history_s != NULL) ? &state->event_history_s[slot & 1] : NULL;
    if (eh) {
        snprintf(eh->Event_History_Items[0].internal_str, sizeof eh->Event_History_Items[0].internal_str,
                 "Target: %d; has been locked out; Encryption Lock Out Enabled.", tg);
        watchdog_event_current(opts, state, (uint8_t)(slot & 1));
        if (strncmp(eh->Event_History_Items[1].internal_str, eh->Event_History_Items[0].internal_str,
                    sizeof eh->Event_History_Items[0].internal_str)
            != 0) {
            if (opts->event_out_file[0] != '\0') {
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

/* ============================================================================
 * Affiliation (RID) Table Helpers
 * ============================================================================ */

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
                state->p25_aff_rid[i] = 0;
                state->p25_aff_last_seen[i] = 0;
                if (state->p25_aff_count > 0) {
                    state->p25_aff_count--;
                }
            }
        }
    }
}

/* ============================================================================
 * Group Affiliation (RID↔TG) Table Helpers
 * ============================================================================ */

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
