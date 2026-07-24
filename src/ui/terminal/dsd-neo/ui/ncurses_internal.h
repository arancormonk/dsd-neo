// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file ncurses_internal.h
 * @brief Internal shared declarations for ncurses UI modules.
 *
 * Provides utility functions and shared state used across the modularized
 * ncurses display components.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_INTERNAL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_INTERNAL_H_

#include <stdint.h>
#include <string.h>

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared state (accessed by multiple display modules) */
extern int ncurses_last_synctype;

/* Utility functions */
void swap_int_local(int* a, int* b);
int select_k_int_local(int* a, int n, int k);
int cmp_int_asc(const void* a, const void* b);
int compute_percentiles_u8(const uint8_t* src, int len, double* p50, double* p95);
int ui_is_locked_from_label(const dsd_state* state, const char* label);
int ui_is_transient_enc_locked_from_label(const dsd_state* state, const char* label);

typedef struct {
    size_t begin;
    size_t end;
    uint32_t id;
    uint8_t is_group;
} ui_target_token;

static inline int
ui_target_prefix_at(const char* label, size_t length, size_t pos, size_t* prefix_length, uint8_t* is_group) {
    *prefix_length = 0U;
    *is_group = 0U;
    if (pos + 4U <= length && memcmp(label + pos, "TGT:", 4U) == 0) {
        *prefix_length = 4U;
        return 1;
    }
    if (pos + 3U > length) {
        return 0;
    }
    if (memcmp(label + pos, "TG:", 3U) != 0 && memcmp(label + pos, "SG:", 3U) != 0) {
        return 0;
    }
    *prefix_length = 3U;
    *is_group = 1U;
    return 1;
}

static inline int
ui_target_parse_id(const char* label, size_t length, size_t* pos, uint32_t* parsed_id) {
    while (*pos < length && label[*pos] == ' ') {
        (*pos)++;
    }
    const size_t digits_begin = *pos;
    uint64_t id = 0U;
    while (*pos < length && label[*pos] >= '0' && label[*pos] <= '9') {
        id = id * 10U + (uint64_t)(label[*pos] - '0');
        if (id > UINT32_MAX) {
            return 0;
        }
        (*pos)++;
    }
    if (*pos == digits_begin || id == 0U) {
        return 0;
    }
    *parsed_id = (uint32_t)id;
    return 1;
}

/** Iterate valid TG:, TGT:, and SG: target tokens in display order. */
static inline int
ui_target_token_next(const char* label, size_t* cursor, ui_target_token* token) {
    if (!label || !cursor || !token) {
        return 0;
    }
    const size_t length = strlen(label);
    for (size_t i = *cursor; i < length; i++) {
        size_t prefix_length = 0U;
        uint8_t is_group = 0U;
        if (!ui_target_prefix_at(label, length, i, &prefix_length, &is_group)) {
            continue;
        }

        size_t pos = i + prefix_length;
        uint32_t id = 0U;
        if (!ui_target_parse_id(label, length, &pos, &id)) {
            continue;
        }
        token->begin = i;
        token->end = pos;
        token->id = id;
        token->is_group = is_group;
        *cursor = pos;
        return 1;
    }
    *cursor = length;
    return 0;
}

static inline int
ui_burst_is_active_p25_call(int burst) {
    return (burst >= 20 && burst <= 22) || burst == 26 || burst == 27;
}

/* HDU can carry freshly decoded Phase 1 ESS metadata before IDs are active. */
static inline int
ui_burst_has_p25_crypto_metadata(int burst) {
    return ui_burst_is_active_p25_call(burst) || burst == 25;
}

static inline int
ui_burst_is_active_call(int burst) {
    return burst == 16 || ui_burst_is_active_p25_call(burst);
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_INTERNAL_H_ */
