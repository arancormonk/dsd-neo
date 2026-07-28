// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <stddef.h>
#include "dsd-neo/core/state_fwd.h"

static int
isCustomAfsBits(int a_bits, int f_bits, int s_bits) {
    return a_bits != 4 || f_bits != 4 || s_bits != 3;
}

int
getAfsStringLengthFromBits(int a_bits, int f_bits, int s_bits) {
    if (!isCustomAfsBits(a_bits, f_bits, s_bits)) {
        return 6;
    }

    int length = 0;
    length += (a_bits + 2) / 3;
    length += (f_bits + 2) / 3;
    length += (s_bits + 2) / 3;
    length += 2;

    return length;
}

int
getAfsStringLength(const dsd_state* state) {
    return getAfsStringLengthFromBits(state->edacs_a_bits, state->edacs_f_bits, state->edacs_s_bits);
}

static int
edacs_digits_for_bits(int bits) {
    if (bits <= 3) {
        return 1;
    }
    if (bits <= 6) {
        return 2;
    }
    return 3;
}

static int
edacs_append_decimal_field(char* buffer, size_t buf_len, size_t* printed_chars, int value, int digits, int add_colon) {
    int written;
    if (add_colon) {
        written = DSD_SNPRINTF(buffer + *printed_chars, buf_len - *printed_chars, "%0*d:", digits, value);
    } else {
        written = DSD_SNPRINTF(buffer + *printed_chars, buf_len - *printed_chars, "%0*d", digits, value);
    }
    if (written < 0) {
        return 0;
    }
    *printed_chars += (size_t)written;
    if (*printed_chars >= buf_len) {
        *printed_chars = buf_len - 1;
    }
    return 1;
}

int
getAfsStringFromBits(int a_bits, int f_bits, int s_bits, char* buffer, int a, int f, int s) {
    if (!isCustomAfsBits(a_bits, f_bits, s_bits)) {
        DSD_SNPRINTF(buffer, 6 + 1, "%02d-%02d%01d", a, f, s);
        return 6;
    }

    size_t printed_chars = 0;
    const size_t need = (size_t)getAfsStringLengthFromBits(a_bits, f_bits, s_bits);
    const size_t buf_len = need + 1;

    int a_digits = edacs_digits_for_bits(a_bits);
    int f_digits = edacs_digits_for_bits(f_bits);
    int s_digits = edacs_digits_for_bits(s_bits);

    if (!edacs_append_decimal_field(buffer, buf_len, &printed_chars, a, a_digits, 1)) {
        return 0;
    }
    if (!edacs_append_decimal_field(buffer, buf_len, &printed_chars, f, f_digits, 1)) {
        return (int)printed_chars;
    }
    if (!edacs_append_decimal_field(buffer, buf_len, &printed_chars, s, s_digits, 0)) {
        return (int)printed_chars;
    }

    return (int)printed_chars;
}

int
getAfsString(const dsd_state* state, char* buffer, int a, int f, int s) {
    return getAfsStringFromBits(state->edacs_a_bits, state->edacs_f_bits, state->edacs_s_bits, buffer, a, f, s);
}
