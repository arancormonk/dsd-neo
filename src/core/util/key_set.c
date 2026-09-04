// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/key_set.h>

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/runtime/log.h>

#include <stdlib.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "key_set_internal.h"

static size_t
key_set_capacity(void) {
    return sizeof(((dsd_state*)0)->rkey_array) / sizeof(((dsd_state*)0)->rkey_array[0]);
}

void
dsd_key_state_secure_wipe(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    DSD_SECURE_ZERO(state->rkey_array, sizeof(state->rkey_array));
    DSD_SECURE_ZERO(state->rkey_array_loaded, sizeof(state->rkey_array_loaded));
    DSD_SECURE_ZERO(state->aes_key, sizeof(state->aes_key));
    DSD_SECURE_ZERO(&state->K, sizeof(state->K));
    DSD_SECURE_ZERO(&state->K1, sizeof(state->K1));
    DSD_SECURE_ZERO(&state->K2, sizeof(state->K2));
    DSD_SECURE_ZERO(&state->K3, sizeof(state->K3));
    DSD_SECURE_ZERO(&state->K4, sizeof(state->K4));
    DSD_SECURE_ZERO(&state->R, sizeof(state->R));
    DSD_SECURE_ZERO(&state->RR, sizeof(state->RR));
    DSD_SECURE_ZERO(&state->H, sizeof(state->H));
    DSD_SECURE_ZERO(state->A1, sizeof(state->A1));
    DSD_SECURE_ZERO(state->A2, sizeof(state->A2));
    DSD_SECURE_ZERO(state->A3, sizeof(state->A3));
    DSD_SECURE_ZERO(state->A4, sizeof(state->A4));
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
    DSD_MEMCPY(out->aes_key, state->aes_key, sizeof(out->aes_key));
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
    DSD_MEMCPY(state->aes_key, in->aes_key, sizeof(state->aes_key));
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

static int
key_scalar_words_equal(const dsd_key_scalars* a, const dsd_key_scalars* b) {
    return a->K == b->K && a->K1 == b->K1 && a->K2 == b->K2 && a->K3 == b->K3 && a->K4 == b->K4 && a->R == b->R
           && a->RR == b->RR && a->H == b->H && a->hytera_key_segments == b->hytera_key_segments;
}

static int
key_scalar_slot_equal(const dsd_key_scalars* a, const dsd_key_scalars* b, size_t slot) {
    return a->A1[slot] == b->A1[slot] && a->A2[slot] == b->A2[slot] && a->A3[slot] == b->A3[slot]
           && a->A4[slot] == b->A4[slot] && a->aes_key_loaded[slot] == b->aes_key_loaded[slot]
           && a->aes_key_segments[slot] == b->aes_key_segments[slot];
}

static int
key_scalars_equal(const dsd_key_scalars* a, const dsd_key_scalars* b) {
    if (!key_scalar_words_equal(a, b)) {
        return 0;
    }
    for (size_t i = 0; i < 2U; i++) {
        if (!key_scalar_slot_equal(a, b, i)) {
            return 0;
        }
    }
    for (size_t i = 0; i < sizeof(a->aes_key); i++) {
        if (a->aes_key[i] != b->aes_key[i]) {
            return 0;
        }
    }
    return 1;
}

int
dsd_key_set_equal(const dsd_key_set* a, const dsd_key_set* b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a == b) {
        return 1;
    }
    if (a->present != b->present || a->keyloader != b->keyloader || a->count != b->count
        || !key_scalars_equal(&a->scalars, &b->scalars)) {
        return 0;
    }
    if (a->count > 0U && (a->entries == NULL || b->entries == NULL)) {
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

static int
key_set_is_ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int
key_set_text_present(const char* text) {
    if (text == NULL) {
        return 0;
    }
    while (*text != '\0') {
        if (!key_set_is_ascii_space((unsigned char)*text)) {
            return 1;
        }
        text++;
    }
    return 0;
}

static int
key_set_parse_direct_dec(const char* text, unsigned long long* out) {
    const unsigned char* p = (const unsigned char*)text;
    while (*p != '\0' && key_set_is_ascii_space(*p)) {
        p++;
    }
    if (*p < (unsigned char)'0' || *p > (unsigned char)'9') {
        return -1;
    }
    unsigned int value = 0U;
    while (*p >= (unsigned char)'0' && *p <= (unsigned char)'9') {
        const unsigned int digit = (unsigned int)(*p - (unsigned char)'0');
        if (value > (255U - digit) / 10U) {
            return -1;
        }
        value = (value * 10U) + digit;
        p++;
    }
    while (*p != '\0' && key_set_is_ascii_space(*p)) {
        p++;
    }
    if (*p != '\0') {
        return -1;
    }
    *out = (unsigned long long)value;
    return 0;
}

static int
key_set_collect_direct_hex(const char* text, char out[65], size_t* out_len) {
    const unsigned char* p = (const unsigned char*)text;
    while (*p != '\0' && key_set_is_ascii_space(*p)) {
        p++;
    }
    if (p[0] == (unsigned char)'0' && (p[1] == (unsigned char)'x' || p[1] == (unsigned char)'X')) {
        p += 2;
    }
    size_t count = 0U;
    while (*p != '\0') {
        if (key_set_is_ascii_space(*p)) {
            p++;
            continue;
        }
        if (dsd_hex_nibble_value(*p) < 0 || count >= 64U) {
            DSD_SECURE_ZERO(out, 65U);
            return -1;
        }
        out[count++] = (char)*p++;
    }
    out[count] = '\0';
    *out_len = count;
    return 0;
}

static void
key_set_direct_store_segment(dsd_key_scalars* scalars, size_t segment, uint64_t value) {
    unsigned long long* const slots[4] = {scalars->A1, scalars->A2, scalars->A3, scalars->A4};
    slots[segment][0] = value;
    slots[segment][1] = value;
    for (size_t i = 0U; i < 8U; i++) {
        scalars->aes_key[(segment * 8U) + i] = (uint8_t)((value >> (56U - (i * 8U))) & 0xFFU);
    }
}

static int
key_set_direct_hex_width_valid(size_t nhex) {
    return nhex == 10U || nhex == 32U || nhex == 64U;
}

static int
key_set_direct_segments_nonzero(const uint64_t segments[4], size_t count) {
    for (size_t i = 0U; i < count; i++) {
        if (segments[i] != 0U) {
            return 1;
        }
    }
    return 0;
}

static int
key_set_parse_direct_hex(const char* text, dsd_key_scalars* scalars) {
    char hex[65];
    DSD_MEMSET(hex, 0, sizeof(hex));
    size_t nhex = 0U;
    if (key_set_collect_direct_hex(text, hex, &nhex) != 0 || !key_set_direct_hex_width_valid(nhex)) {
        DSD_SECURE_ZERO(hex, sizeof(hex));
        return -1;
    }

    uint64_t segments[4] = {0U, 0U, 0U, 0U};
    const size_t segment_count = nhex == 10U ? 1U : nhex / 16U;
    const size_t first_width = nhex == 10U ? 10U : 16U;
    for (size_t i = 0U; i < segment_count; i++) {
        const size_t offset = i * 16U;
        const size_t width = (i == 0U) ? first_width : 16U;
        if (dsd_parse_hex_u64_n(hex + offset, width, &segments[i]) != 0) {
            DSD_SECURE_ZERO(segments, sizeof(segments));
            DSD_SECURE_ZERO(hex, sizeof(hex));
            return -1;
        }
    }

    scalars->H = segments[0];
    scalars->K1 = segments[0];
    scalars->K2 = segments[1];
    scalars->K3 = segments[2];
    scalars->K4 = segments[3];
    const int any_nonzero = key_set_direct_segments_nonzero(segments, segment_count);
    scalars->hytera_key_segments = any_nonzero ? (uint8_t)segment_count : 0U;
    if (segment_count > 1U) {
        for (size_t i = 0U; i < segment_count; i++) {
            key_set_direct_store_segment(scalars, i, segments[i]);
        }
        scalars->aes_key_loaded[0] = scalars->aes_key_loaded[1] = any_nonzero;
        scalars->aes_key_segments[0] = scalars->aes_key_segments[1] = (uint8_t)segment_count;
    }

    DSD_SECURE_ZERO(segments, sizeof(segments));
    DSD_SECURE_ZERO(hex, sizeof(hex));
    return 0;
}

dsd_key_direct_result
dsd_key_set_load_direct(dsd_key_set* out, const char* single_hex, const char* single_dec) {
    const int have_hex = key_set_text_present(single_hex);
    const int have_dec = key_set_text_present(single_dec);
    if (out == NULL || (!have_hex && !have_dec)) {
        return DSD_KEY_DIRECT_INVALID_ARGUMENT;
    }

    dsd_key_set loaded;
    DSD_MEMSET(&loaded, 0, sizeof(loaded));
    loaded.present = 1U;
    loaded.keyloader = 0;
    if (have_dec && key_set_parse_direct_dec(single_dec, &loaded.scalars.K) != 0) {
        dsd_key_set_free(&loaded);
        return DSD_KEY_DIRECT_INVALID_DEC;
    }
    if (have_hex && key_set_parse_direct_hex(single_hex, &loaded.scalars) != 0) {
        dsd_key_set_free(&loaded);
        return DSD_KEY_DIRECT_INVALID_HEX;
    }

    dsd_key_set_free(out);
    *out = loaded;
    DSD_SECURE_ZERO(&loaded, sizeof(loaded));
    return DSD_KEY_DIRECT_OK;
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
    dsd_state_ext_free_all(tmp);
    dsd_key_state_secure_wipe(tmp);
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
    DSD_SECURE_ZERO(&loaded, sizeof(loaded));
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
        DSD_SECURE_ZERO(&fresh, sizeof(fresh));
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
