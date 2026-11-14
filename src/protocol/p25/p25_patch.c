// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 regroup/patch tracking utilities
 */

#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

static int
p25_patch_slot_count(const dsd_state* state) {
    if (!state || state->p25_patch_count < 0) {
        return 0;
    }
    return (state->p25_patch_count > DSD_P25_PATCH_MAX) ? DSD_P25_PATCH_MAX : state->p25_patch_count;
}

static int
find_patch_idx(const dsd_state* state, uint16_t sgid) {
    const int count = p25_patch_slot_count(state);
    for (int i = 0; i < count; i++) {
        if (state->p25_patch_sgid[i] == sgid) {
            return i;
        }
    }
    return -1;
}

/* An active entry whose SG was named within the TTL on the listening clock. */
static int
p25_patch_entry_live(const dsd_state* state, int idx) {
    return (state->p25_patch_active[idx] && dsd_p25_patch_entry_seen_current(state, idx)) ? 1 : 0;
}

/* Replacement order for a full table: an inactive slot, then one whose SG was not named within the TTL, then (only
   for an active newcomer) the least recently refreshed one. */
static int
p25_patch_replacement_idx(const dsd_state* state, int active) {
    if (!state) {
        return -1;
    }

    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        if (!state->p25_patch_active[i]) {
            return i;
        }
    }

    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        if (!dsd_p25_patch_entry_seen_current(state, i)) {
            return i;
        }
    }

    if (!active) {
        return -1;
    }

    int repl = 0;
    uint32_t oldest = dsd_p25_patch_seq_age(state, 0);
    for (int i = 1; i < DSD_P25_PATCH_MAX; i++) {
        const uint32_t age = dsd_p25_patch_seq_age(state, i);
        if (age > oldest) {
            oldest = age;
            repl = i;
        }
    }
    return repl;
}

/* A new entry in a free or replaced slot: no members, no KEY context, named now. */
static int
p25_patch_new_entry(dsd_state* state, uint16_t sgid, int is_patch, int active) {
    int idx = state->p25_patch_count;
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= DSD_P25_PATCH_MAX) {
        state->p25_patch_count = DSD_P25_PATCH_MAX;
        idx = p25_patch_replacement_idx(state, active ? 1 : 0);
        if (idx < 0) {
            return -1;
        }
    } else {
        state->p25_patch_count = idx + 1;
    }
    state->p25_patch_sgid[idx] = sgid;
    state->p25_patch_is_patch[idx] = is_patch ? 1 : 0;
    state->p25_patch_active[idx] = active ? 1 : 0;
    state->p25_patch_last_update[idx] = dsd_decode_time();
    dsd_p25_patch_expire_members(state, idx);
    dsd_p25_patch_touch_seen(state, idx);
    return idx;
}

/* The entry for @p sgid, created as an active patch when absent. An existing entry's non-current members expire
   before the caller changes it. */
static int
p25_patch_entry_for_change(dsd_state* state, int sgid) {
    int idx = find_patch_idx(state, (uint16_t)sgid);
    if (idx < 0) {
        return p25_patch_new_entry(state, (uint16_t)sgid, 1, 1);
    }
    (void)dsd_p25_patch_expire_stale_members(state, idx);
    return idx;
}

/* A membership message for entry @p idx was accepted: it is active and its row and members are fresh. */
static void
p25_patch_note_membership(dsd_state* state, int idx) {
    state->p25_patch_active[idx] = 1;
    state->p25_patch_last_update[idx] = dsd_decode_time();
    dsd_p25_patch_touch_members(state, idx);
}

void
p25_patch_update(dsd_state* state, int sgid, int is_patch, int active) {
    if (!state || sgid <= 0 || sgid > 0xFFFF) {
        return;
    }
    int idx = find_patch_idx(state, (uint16_t)sgid);
    if (idx < 0) {
        (void)p25_patch_new_entry(state, (uint16_t)sgid, is_patch, active);
        return;
    }
    // A message that only names the SG keeps the row; it never makes the member list current again.
    (void)dsd_p25_patch_expire_stale_members(state, idx);
    state->p25_patch_is_patch[idx] = is_patch ? 1 : 0;
    state->p25_patch_active[idx] = active ? 1 : 0;
    state->p25_patch_last_update[idx] = dsd_decode_time();
    dsd_p25_patch_touch_seen(state, idx);
}

void
p25_patch_add_wgid(dsd_state* state, int sgid, int wgid) {
    if (!state || sgid <= 0 || sgid > 0xFFFF || wgid <= 0 || wgid > 0xFFFF) {
        return;
    }
    if (wgid == sgid) {
        return; // the SG is never its own member
    }
    int idx = p25_patch_entry_for_change(state, sgid);
    if (idx < 0) {
        return;
    }
    p25_patch_note_membership(state, idx);
    const int cnt = state->p25_patch_wgid_count[idx];
    for (int i = 0; i < cnt && i < DSD_P25_PATCH_GROUP_MAX; i++) {
        if (state->p25_patch_wgid[idx][i] == (uint16_t)wgid) {
            return;
        }
    }
    if (cnt >= DSD_P25_PATCH_GROUP_MAX) {
        // Annotates the handler's own line, which named the member just before: "GA1: 1433 (WG 1433 dropped: ...)".
        DSD_FPRINTF(stderr, " (WG %d dropped: SG %d holds %d groups)", wgid, sgid, DSD_P25_PATCH_GROUP_MAX);
        return;
    }
    state->p25_patch_wgid[idx][cnt] = (uint16_t)wgid;
    state->p25_patch_wgid_count[idx] = (uint8_t)(cnt + 1);
}

void
p25_patch_add_wuid(dsd_state* state, int sgid, uint32_t wuid) {
    if (!state || sgid <= 0 || sgid > 0xFFFF || wuid == 0) {
        return;
    }
    int idx = p25_patch_entry_for_change(state, sgid);
    if (idx < 0) {
        return;
    }
    p25_patch_note_membership(state, idx);
    const int cnt = state->p25_patch_wuid_count[idx];
    for (int i = 0; i < cnt && i < DSD_P25_PATCH_RADIO_MAX; i++) {
        if (state->p25_patch_wuid[idx][i] == wuid) {
            return;
        }
    }
    if (cnt >= DSD_P25_PATCH_RADIO_MAX) {
        DSD_FPRINTF(stderr, " (RID %u dropped: SG %d holds %d radios)", (unsigned)wuid, sgid, DSD_P25_PATCH_RADIO_MAX);
        return;
    }
    state->p25_patch_wuid[idx][cnt] = wuid;
    state->p25_patch_wuid_count[idx] = (uint8_t)(cnt + 1);
}

static void p25_patch_append(char* out, size_t cap, int* n, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 4, 5);

static void
p25_patch_append(char* out, size_t cap, int* n, const char* fmt, ...) {
    if (!out || !n || !fmt) {
        return;
    }
    size_t rem = (cap > (size_t)(*n)) ? (cap - (size_t)(*n)) : 0U;
    if (rem == 0U) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int wrote = DSD_VSNPRINTF(out + *n, rem, fmt, ap);
    va_end(ap);
    if (wrote > 0) {
        *n += wrote;
    }
}

static void
p25_patch_append_wg_summary(const dsd_state* state, int idx, int* n, char* out, size_t cap) {
    uint8_t wgc = state->p25_patch_wgid_count[idx];
    if (wgc == 0U) {
        return;
    }
    if (wgc <= 3U) {
        p25_patch_append(out, cap, n, " WG:");
        for (int k = 0; k < wgc && k < 3; k++) {
            p25_patch_append(out, cap, n, (k == 0) ? "%04u" : ",%04u", state->p25_patch_wgid[idx][k]);
        }
        return;
    }
    p25_patch_append(out, cap, n, " WG:%u(%04u,%04u+)", wgc, state->p25_patch_wgid[idx][0],
                     state->p25_patch_wgid[idx][1]);
}

static void
p25_patch_append_crypt_summary(const dsd_state* state, int idx, int* n, char* out, size_t cap) {
    if (state->p25_patch_key[idx]) {
        p25_patch_append(out, cap, n, " K:%04X", state->p25_patch_key[idx] & 0xFFFF);
    }
    if (state->p25_patch_alg[idx]) {
        p25_patch_append(out, cap, n, " A:%02X", state->p25_patch_alg[idx] & 0xFF);
    }
    if (state->p25_patch_ssn[idx]) {
        p25_patch_append(out, cap, n, " S:%02u", state->p25_patch_ssn[idx] & 0x1F);
    }
}

int
p25_patch_compose_details(const dsd_state* state, char* out, size_t cap) {
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!state) {
        return 0;
    }
    int n = 0;
    const int count = p25_patch_slot_count(state);
    for (int i = 0; i < count; i++) {
        if (!p25_patch_entry_live(state, i)) {
            continue;
        }
        char t = state->p25_patch_is_patch[i] ? 'P' : 'S';
        uint16_t sg = state->p25_patch_sgid[i];
        if (n > 0) {
            p25_patch_append(out, cap, &n, "; ");
        }
        p25_patch_append(out, cap, &n, "SG%03u[%c]", sg, t);
        // Members and KEY context show only while current; a reader never expires them.
        if (dsd_p25_patch_members_current(state, i)) {
            uint8_t wgc = state->p25_patch_wgid_count[i];
            uint8_t wuc = state->p25_patch_wuid_count[i];
            if (wgc > 0) {
                p25_patch_append_wg_summary(state, i, &n, out, cap);
            } else if (wuc > 0) {
                p25_patch_append(out, cap, &n, " U:%u", wuc);
            }
            // Optional crypt context: print only fields that are present
            if (state->p25_patch_key[i] || state->p25_patch_alg[i] || state->p25_patch_ssn[i]) {
                p25_patch_append_crypt_summary(state, i, &n, out, cap);
            }
        }
        if (n >= (int)cap - 8) {
            break;
        }
    }
    return n;
}

void
p25_patch_remove_wgid(dsd_state* state, int sgid, int wgid) {
    if (!state || sgid <= 0 || sgid > 0xFFFF || wgid <= 0 || wgid > 0xFFFF) {
        return;
    }
    if (wgid == sgid) {
        // A Delete that lists the SG itself ends the whole patch.
        p25_patch_clear_sg(state, sgid);
        return;
    }
    int idx = find_patch_idx(state, (uint16_t)sgid);
    if (idx < 0) {
        return;
    }
    (void)dsd_p25_patch_expire_stale_members(state, idx);
    uint8_t cnt = state->p25_patch_wgid_count[idx];
    for (int i = 0; i < cnt && i < DSD_P25_PATCH_GROUP_MAX; i++) {
        if (state->p25_patch_wgid[idx][i] == (uint16_t)wgid) {
            // swap with last
            if (i != cnt - 1) {
                state->p25_patch_wgid[idx][i] = state->p25_patch_wgid[idx][cnt - 1];
            }
            state->p25_patch_wgid[idx][cnt - 1] = 0;
            state->p25_patch_wgid_count[idx] = (uint8_t)(cnt - 1);
            break;
        }
    }
    if (state->p25_patch_wgid_count[idx] == 0 && state->p25_patch_wuid_count[idx] == 0) {
        // The last member left: the patch ends and its KEY context goes with it.
        state->p25_patch_active[idx] = 0;
        dsd_p25_patch_expire_members(state, idx);
    }
}

void
p25_patch_clear_sg(dsd_state* state, int sgid) {
    if (!state || sgid <= 0 || sgid > 0xFFFF) {
        return;
    }
    int idx = find_patch_idx(state, (uint16_t)sgid);
    if (idx < 0) {
        return;
    }
    dsd_p25_patch_expire_members(state, idx);
    state->p25_patch_active[idx] = 0;
    state->p25_patch_last_update[idx] = dsd_decode_time();
}

int
p25_patch_prepare_grg_update(dsd_state* state, int sgid, int is_patch, int active, int ssn) {
    if (!state || sgid <= 0 || sgid > 0xFFFF) {
        return 0;
    }

    const int normalized_ssn = (ssn >= 0) ? (ssn & 0x1F) : -1;
    int idx = find_patch_idx(state, (uint16_t)sgid);
    if (!active) {
        if (idx < 0) {
            p25_patch_update(state, sgid, is_patch, 0);
        }
        p25_patch_clear_sg(state, sgid);
        idx = find_patch_idx(state, (uint16_t)sgid);
        if (idx >= 0) {
            state->p25_patch_is_patch[idx] = is_patch ? 1 : 0;
        }
        return 0;
    }

    if (idx >= 0) {
        (void)dsd_p25_patch_expire_stale_members(state, idx);
        if (state->p25_patch_active[idx] != 0 && normalized_ssn >= 0
            && state->p25_patch_ssn[idx] != (uint8_t)normalized_ssn) {
            dsd_p25_patch_expire_members(state, idx);
        }
    }

    // The command names the SG; its members and KEY context refresh through add/KAS.
    p25_patch_update(state, sgid, is_patch, 1);
    return 1;
}

void
p25_patch_set_kas(dsd_state* state, int sgid, int key, int alg, int ssn) {
    if (!state || sgid <= 0 || sgid > 0xFFFF) {
        return;
    }
    int idx = p25_patch_entry_for_change(state, sgid);
    if (idx < 0) {
        return;
    }
    if (key >= 0) {
        state->p25_patch_key[idx] = (uint16_t)key;
        state->p25_patch_key_valid[idx] = 1;
    }
    if (alg >= 0) {
        state->p25_patch_alg[idx] = (uint8_t)alg;
    }
    if (ssn >= 0) {
        state->p25_patch_ssn[idx] = (uint8_t)(ssn & 0x1F);
    }
    state->p25_patch_last_update[idx] = dsd_decode_time();
    dsd_p25_patch_touch_members(state, idx);
}

/* An active entry whose SG and members are both current: its member list and KEY context may be trusted. */
static int
p25_patch_entry_members_live(const dsd_state* state, int idx) {
    return (p25_patch_entry_live(state, idx) && dsd_p25_patch_members_current(state, idx)) ? 1 : 0;
}

// Return 1 if the given talkgroup (assumed WGID) is a member of an active
// regroup/patch whose policy key has been explicitly signaled as 0 (clear).
// Returns 0 otherwise.
int
p25_patch_tg_key_is_clear(const dsd_state* state, int tg) {
    if (!state || tg <= 0 || tg > 0xFFFF) {
        return 0;
    }
    const int count = p25_patch_slot_count(state);
    for (int i = 0; i < count; i++) {
        if (!p25_patch_entry_members_live(state, i)) {
            continue;
        }
        if (dsd_p25_patch_has_current_wgid(state, i, (uint16_t)tg)) {
            // explicit KEY=0000 policy for this SG/WG; membership found but key not clear (or unknown) otherwise
            return (state->p25_patch_key_valid[i] && state->p25_patch_key[i] == 0) ? 1 : 0;
        }
    }
    return 0;
}

// Return 1 if the given SGID has an explicitly signaled KEY of 0 (clear) and
// is currently active. Returns 0 otherwise.
int
p25_patch_sg_key_is_clear(const dsd_state* state, int sgid) {
    if (!state || sgid <= 0 || sgid > 0xFFFF) {
        return 0;
    }
    const int count = p25_patch_slot_count(state);
    for (int i = 0; i < count; i++) {
        if (state->p25_patch_sgid[i] != (uint16_t)sgid || !p25_patch_entry_members_live(state, i)) {
            continue;
        }
        return (state->p25_patch_key_valid[i] && state->p25_patch_key[i] == 0) ? 1 : 0;
    }
    return 0;
}
