// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/key_set.h>

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/runtime/log.h>

#include <stdlib.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static size_t
key_set_capacity(void) {
    return sizeof(((dsd_state*)0)->rkey_array) / sizeof(((dsd_state*)0)->rkey_array[0]);
}

static void
key_scalars_capture(dsd_key_scalars* out, const dsd_state* state) {
    out->K = state->K;
    out->K1 = state->K1;
    out->K2 = state->K2;
    out->K3 = state->K3;
    out->K4 = state->K4;
    out->R = state->R;
    out->RR = state->RR;
    out->H = state->H;
    out->hytera_key_segments = state->hytera_key_segments;
    DSD_MEMCPY(out->A1, state->A1, sizeof(out->A1));
    DSD_MEMCPY(out->A2, state->A2, sizeof(out->A2));
    DSD_MEMCPY(out->A3, state->A3, sizeof(out->A3));
    DSD_MEMCPY(out->A4, state->A4, sizeof(out->A4));
    DSD_MEMCPY(out->aes_key_loaded, state->aes_key_loaded, sizeof(out->aes_key_loaded));
    DSD_MEMCPY(out->aes_key_segments, state->aes_key_segments, sizeof(out->aes_key_segments));
}

static void
key_scalars_install(dsd_state* state, const dsd_key_scalars* in) {
    state->K = in->K;
    state->K1 = in->K1;
    state->K2 = in->K2;
    state->K3 = in->K3;
    state->K4 = in->K4;
    state->R = in->R;
    state->RR = in->RR;
    state->H = in->H;
    state->hytera_key_segments = in->hytera_key_segments;
    DSD_MEMCPY(state->A1, in->A1, sizeof(state->A1));
    DSD_MEMCPY(state->A2, in->A2, sizeof(state->A2));
    DSD_MEMCPY(state->A3, in->A3, sizeof(state->A3));
    DSD_MEMCPY(state->A4, in->A4, sizeof(state->A4));
    DSD_MEMCPY(state->aes_key_loaded, in->aes_key_loaded, sizeof(state->aes_key_loaded));
    DSD_MEMCPY(state->aes_key_segments, in->aes_key_segments, sizeof(state->aes_key_segments));
}

int
dsd_key_set_capture(dsd_key_set* out, const dsd_state* state) {
    if (out == NULL || state == NULL) {
        return -1;
    }
    const size_t capacity = key_set_capacity();
    size_t count = 0;
    for (size_t i = 0; i < capacity; i++) {
        if (state->rkey_array_loaded[i] != 0U || state->rkey_array[i] != 0ULL) {
            count++;
        }
    }
    dsd_key_set_entry* entries = NULL;
    if (count > 0) {
        entries = (dsd_key_set_entry*)calloc(count, sizeof(*entries));
        if (entries == NULL) {
            dsd_key_set_free(out);
            return -1;
        }
        size_t at = 0;
        for (size_t i = 0; i < capacity; i++) {
            if (state->rkey_array_loaded[i] != 0U || state->rkey_array[i] != 0ULL) {
                entries[at].index = (uint32_t)i;
                entries[at].value = state->rkey_array[i];
                entries[at].loaded = state->rkey_array_loaded[i] != 0U ? (uint8_t)1 : (uint8_t)0;
                at++;
            }
        }
    }
    dsd_key_set_free(out);
    out->entries = entries;
    out->count = count;
    out->present = 0;
    out->keyloader = state->keyloader;
    key_scalars_capture(&out->scalars, state);
    return 0;
}

void
dsd_key_set_install(dsd_state* state, const dsd_key_set* ks) {
    if (state == NULL || ks == NULL) {
        return;
    }
    const size_t capacity = key_set_capacity();
    DSD_MEMSET(state->rkey_array, 0, sizeof(state->rkey_array));
    DSD_MEMSET(state->rkey_array_loaded, 0, sizeof(state->rkey_array_loaded));
    /* A NULL entry table always means an empty set; count is authoritative only with a table. */
    const size_t count = (ks->entries != NULL) ? ks->count : 0U;
    for (size_t i = 0; i < count; i++) {
        const size_t idx = (size_t)ks->entries[i].index;
        if (idx >= capacity) {
            continue;
        }
        state->rkey_array[idx] = ks->entries[i].value;
        state->rkey_array_loaded[idx] = ks->entries[i].loaded != 0U ? (unsigned char)1 : (unsigned char)0;
    }
    state->keyloader = ks->keyloader;
    key_scalars_install(state, &ks->scalars);
}

int
dsd_key_set_copy(dsd_key_set* dst, const dsd_key_set* src) {
    if (dst == NULL || src == NULL) {
        return -1;
    }
    if (dst == src) {
        return 0;
    }
    dsd_key_set_entry* entries = NULL;
    if (src->count > 0) {
        if (src->entries == NULL) {
            return -1;
        }
        entries = (dsd_key_set_entry*)calloc(src->count, sizeof(*entries));
        if (entries == NULL) {
            return -1;
        }
        DSD_MEMCPY(entries, src->entries, src->count * sizeof(*entries));
    }
    dsd_key_set_free(dst);
    dst->entries = entries;
    dst->count = src->count;
    dst->present = src->present;
    dst->keyloader = src->keyloader;
    dst->scalars = src->scalars;
    return 0;
}

int
dsd_key_set_equal(const dsd_key_set* a, const dsd_key_set* b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a == b) {
        return 1;
    }
    if (a->present != b->present || a->keyloader != b->keyloader || a->count != b->count) {
        return 0;
    }
    for (size_t i = 0; i < a->count; i++) {
        if (a->entries[i].index != b->entries[i].index || a->entries[i].value != b->entries[i].value
            || a->entries[i].loaded != b->entries[i].loaded) {
            return 0;
        }
    }
    return 1;
}

static int
key_set_path_empty(const char* path) {
    return path == NULL || path[0] == '\0';
}

static void
key_set_log_summary(const char* kind, const char* path, const dsd_csv_validation* stats) {
    /* Counts are pre-formatted so no numeric conversion shares a log line with a key word. */
    char count_text[64] = "0 of 0";
    if (stats != NULL) {
        (void)DSD_SNPRINTF(count_text, sizeof(count_text), "%u of %u", stats->accepted, stats->total);
    }
    LOG_INFO("NOTICE: Loaded %s entries (%s file) from '%s'.\n", count_text, kind, path);
}

/* The throwaway state held key material: wipe the keyring before the memory goes back. */
static void
key_set_free_throwaway(dsd_state* tmp) {
    DSD_MEMSET(tmp->rkey_array, 0, sizeof(tmp->rkey_array));
    DSD_MEMSET(tmp->rkey_array_loaded, 0, sizeof(tmp->rkey_array_loaded));
    dsd_state_ext_free_all(tmp);
    free(tmp);
}

int
dsd_key_set_load_csv(dsd_key_set* out, const char* hex_path, const char* dec_path, int show_keys) {
    if (out == NULL || (key_set_path_empty(hex_path) && key_set_path_empty(dec_path))) {
        return -1;
    }
    dsd_state* tmp = (dsd_state*)calloc(1, sizeof(*tmp));
    if (tmp == NULL) {
        return -1;
    }
    if (!key_set_path_empty(hex_path)) {
        dsd_csv_validation stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        if (csvKeyImportHexPath(hex_path, show_keys, tmp, &stats) != 0) {
            key_set_free_throwaway(tmp);
            return -1;
        }
        key_set_log_summary("hex", hex_path, &stats);
    }
    if (!key_set_path_empty(dec_path)) {
        dsd_csv_validation stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        if (csvKeyImportDecPath(dec_path, show_keys, tmp, &stats) != 0) {
            key_set_free_throwaway(tmp);
            return -1;
        }
        key_set_log_summary("dec", dec_path, &stats);
    }
    dsd_key_set loaded;
    DSD_MEMSET(&loaded, 0, sizeof(loaded));
    const int capture_rc = dsd_key_set_capture(&loaded, tmp);
    key_set_free_throwaway(tmp);
    if (capture_rc != 0) {
        return -1;
    }
    loaded.present = 1;
    loaded.keyloader = 1;
    DSD_MEMSET(&loaded.scalars, 0, sizeof(loaded.scalars));
    dsd_key_set_free(out);
    *out = loaded;
    return 0;
}

int
dsd_scan_keys_enter(dsd_state* state, const dsd_key_set* row_set) {
    if (state == NULL || row_set == NULL) {
        return 0;
    }
    if (state->scan_keys_active_set == 0) {
        /* Both copies have to exist before anything touches the live keyring: a failed
         * baseline capture would otherwise let a later leave install an empty set over
         * the operator's globals. */
        if (dsd_key_set_capture(&state->scan_keys_baseline, state) != 0) {
            return 0;
        }
        if (dsd_key_set_copy(&state->scan_keys_active, row_set) != 0) {
            dsd_key_set_free(&state->scan_keys_baseline);
            return 0;
        }
        dsd_key_set_install(state, &state->scan_keys_active);
        state->scan_keys_active_set = 1;
        return 1;
    }
    if (dsd_key_set_equal(&state->scan_keys_active, row_set)) {
        return 0;
    }
    if (dsd_key_set_copy(&state->scan_keys_active, row_set) != 0) {
        return 0;
    }
    dsd_key_set_install(state, &state->scan_keys_active);
    return 1;
}

void
dsd_scan_keys_leave(dsd_state* state) {
    if (state == NULL || state->scan_keys_active_set == 0) {
        return;
    }
    dsd_key_set_install(state, &state->scan_keys_baseline);
    dsd_key_set_free(&state->scan_keys_baseline);
    dsd_key_set_free(&state->scan_keys_active);
    state->scan_keys_active_set = 0;
}

void
dsd_scan_keys_suspend(dsd_state* state) {
    if (state == NULL || state->scan_keys_active_set == 0) {
        return;
    }
    dsd_key_set_install(state, &state->scan_keys_baseline);
}

void
dsd_scan_keys_resume(dsd_state* state) {
    if (state == NULL || state->scan_keys_active_set == 0) {
        return;
    }
    /* A failed re-capture keeps the previous baseline rather than an empty one. */
    dsd_key_set fresh;
    DSD_MEMSET(&fresh, 0, sizeof(fresh));
    if (dsd_key_set_capture(&fresh, state) == 0) {
        dsd_key_set_free(&state->scan_keys_baseline);
        state->scan_keys_baseline = fresh;
    }
    dsd_key_set_install(state, &state->scan_keys_active);
}

int
dsd_scan_row_keys_apply(dsd_state* state, int row) {
    if (state == NULL || row < 0) {
        return 0;
    }
    const dsd_key_set* row_set = dsd_state_trunk_lcn_keys_get(state, (size_t)row);
    if (row_set != NULL && row_set->present != 0) {
        const int changed = dsd_scan_keys_enter(state, row_set);
        if (changed != 0) {
            dsd_enc_lockout_bump_key_epoch(state);
        }
        return changed;
    }
    if (state->scan_keys_active_set != 0) {
        dsd_scan_keys_leave(state);
        dsd_enc_lockout_bump_key_epoch(state);
        return 1;
    }
    return 0;
}

void
dsd_scan_row_keys_warn_if_unused(const dsd_state* state, int scanner_mode) {
    if (scanner_mode == 1 || !dsd_state_trunk_lcn_keys_present(state)) {
        return;
    }
    LOG_WARN("WARNING: Channel map row keys apply only with -Y scanner mode; ignoring per-row keys.\n");
}
