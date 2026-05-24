// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/freq_parse.h>

#include "fuzz_support.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL) {
        return 0;
    }

    char text[256];
    size = size >= sizeof(text) ? sizeof(text) - 1U : size;
    if (size > 0U) {
        DSD_MEMCPY(text, data, size);
    }
    text[size] = '\0';

    (void)dsd_parse_freq_hz(text);
    return 0;
}
