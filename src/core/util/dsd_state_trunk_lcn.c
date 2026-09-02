// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Scan-list heap tail (slots past the embedded trunk_lcn_freq[]).
 *
 * Kept out of dsd_init.c so test targets that link individual core sources
 * (menu services, UI snapshot, trunk scan) can attach the real implementation
 * without dragging in the full initState/freeState dependency chain.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_state_trunk_lcn_reserve(dsd_state* state, size_t count_needed) {
    if (!state) {
        return -1;
    }
    if (count_needed <= (size_t)DSD_TRUNK_LCN_EMBEDDED) {
        return 0;
    }
    const size_t ext_needed = count_needed - (size_t)DSD_TRUNK_LCN_EMBEDDED;
    if (ext_needed <= state->trunk_lcn_freq_ext_capacity) {
        return 0;
    }
    size_t capacity = state->trunk_lcn_freq_ext_capacity > 0 ? state->trunk_lcn_freq_ext_capacity : 16;
    while (capacity < ext_needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = ext_needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(long int)) {
        return -1;
    }
    long int* ext = (long int*)realloc(state->trunk_lcn_freq_ext, capacity * sizeof *ext);
    if (!ext) {
        return -1;
    }
    DSD_MEMSET(ext + state->trunk_lcn_freq_ext_capacity, 0,
               (capacity - state->trunk_lcn_freq_ext_capacity) * sizeof *ext);
    state->trunk_lcn_freq_ext = ext;
    state->trunk_lcn_freq_ext_capacity = capacity;
    return 0;
}

int
dsd_state_trunk_lcn_name_reserve(dsd_state* state, size_t count) {
    if (!state) {
        return -1;
    }
    if (count <= state->trunk_lcn_name_capacity) {
        return 0;
    }
    size_t capacity = state->trunk_lcn_name_capacity > 0 ? state->trunk_lcn_name_capacity : 16;
    while (capacity < count) {
        if (capacity > SIZE_MAX / 2) {
            capacity = count;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / DSD_CHANNEL_LABEL_SIZE) {
        return -1;
    }
    char (*names)[DSD_CHANNEL_LABEL_SIZE] =
        (char (*)[DSD_CHANNEL_LABEL_SIZE])realloc(state->trunk_lcn_name, capacity * sizeof *names);
    if (!names) {
        return -1;
    }
    DSD_MEMSET(names + state->trunk_lcn_name_capacity, 0, (capacity - state->trunk_lcn_name_capacity) * sizeof *names);
    state->trunk_lcn_name = names;
    state->trunk_lcn_name_capacity = capacity;
    return 0;
}

void
dsd_state_trunk_lcn_name_free(dsd_state* state) {
    if (!state) {
        return;
    }
    free(state->trunk_lcn_name);
    state->trunk_lcn_name = NULL;
    state->trunk_lcn_name_capacity = 0;
}

static int
trunk_lcn_name_is_space(unsigned char c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}

/*
 * Drop ASCII whitespace from both ends of the first @p len bytes of @p out and
 * zero-fill what that gives up, leaving the kept text at the front.
 */
static void
trunk_lcn_name_trim_in_place(char* out, size_t len) {
    size_t start = 0;
    size_t end = len;
    while (start < end && trunk_lcn_name_is_space((unsigned char)out[start])) {
        start++;
    }
    while (end > start && trunk_lcn_name_is_space((unsigned char)out[end - 1])) {
        end--;
    }
    const size_t kept = end - start;
    if (start > 0) {
        DSD_MEMMOVE(out, out + start, kept);
    }
    DSD_MEMSET(out + kept, 0, len - kept);
}

/*
 * Render one CSV name into a zero-filled entry-sized buffer: the padding the
 * file carried (CSV spacing, the line ending) dropped before it can spend the
 * budget, the rest truncated to fit, and control characters replaced with
 * spaces so a tab or a stray CR never reaches printw().
 *
 * Substitution runs before the final trim, not after: a leading control byte
 * ("\x01Dispatch") would otherwise be stored as a leading space, and a
 * truncation landing just after an interior control byte as a trailing one.
 */
static void
trunk_lcn_name_sanitize(const char* name, char* out, size_t out_sz) {
    DSD_MEMSET(out, 0, out_sz);
    if (!name || out_sz < 2) {
        return;
    }
    const unsigned char* p = (const unsigned char*)name;
    size_t len = strlen(name);
    while (len > 0 && trunk_lcn_name_is_space(p[0])) {
        p++;
        len--;
    }
    while (len > 0 && trunk_lcn_name_is_space(p[len - 1])) {
        len--;
    }
    if (len > out_sz - 1) {
        len = out_sz - 1;
        /* The cap is a byte count, so it can land inside a multi-byte character.
           Back off to a character start: a half-written UTF-8 sequence is not
           memory-unsafe, but it renders as garbage wherever the name is shown. */
        while (len > 0 && (p[len] & 0xC0U) == 0x80U) {
            len--;
        }
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = (p[i] < 0x20U || p[i] == 0x7FU) ? ' ' : (char)p[i];
    }
    trunk_lcn_name_trim_in_place(out, len);
}

int
dsd_state_trunk_lcn_name_set(dsd_state* state, size_t index, const char* name) {
    char entry[DSD_CHANNEL_LABEL_SIZE];
    if (!state || index == SIZE_MAX) {
        return -1;
    }
    trunk_lcn_name_sanitize(name, entry, sizeof entry);
    if (entry[0] == '\0' && index >= state->trunk_lcn_name_capacity) {
        // Nothing to store and no entry to clear: a file whose name column is
        // blank throughout never allocates the store.
        return 0;
    }
    if (dsd_state_trunk_lcn_name_reserve(state, index + 1) != 0) {
        return -1;
    }
    DSD_MEMCPY(state->trunk_lcn_name[index], entry, sizeof entry);
    return 0;
}

const char*
dsd_state_trunk_lcn_name_get(const dsd_state* state, size_t index) {
    static const char empty[] = "";
    if (!state || !state->trunk_lcn_name || index >= state->trunk_lcn_name_capacity) {
        return empty;
    }
    return state->trunk_lcn_name[index];
}

void
dsd_state_trunk_lcn_free(dsd_state* state) {
    if (!state) {
        return;
    }
    free(state->trunk_lcn_freq_ext);
    state->trunk_lcn_freq_ext = NULL;
    state->trunk_lcn_freq_ext_capacity = 0;
    dsd_state_trunk_lcn_name_free(state);
}

int
dsd_state_trunk_lcn_user_list_present(const dsd_opts* opts, const dsd_state* state) {
    if (opts && opts->chan_in_file[0] != '\0') {
        return 1;
    }
    if (!opts || !state || opts->trunk_scan_enabled != 1) {
        return 0;
    }
    return (state->lcn_freq_count > 1) ? 1 : 0;
}
