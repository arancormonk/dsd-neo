// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/freq_parse.h>

#include <stdlib.h>
#include <string.h>

#include "fuzz_support.h"

#define FUZZ_ENTRY_SENTINEL 0xA5A5A5A5u

/* Text the entry parser must read back as exactly hz. */
static void
fuzz_entry_reparse(const char* text, uint32_t hz) {
    uint32_t back = FUZZ_ENTRY_SENTINEL;
    if (dsd_parse_freq_entry_hz(text, &back) != DSD_FREQ_ENTRY_OK || back != hz) {
        abort();
    }
}

static void
fuzz_entry(const char* text) {
    uint32_t hz = FUZZ_ENTRY_SENTINEL;
    const int rc = dsd_parse_freq_entry_hz(text, &hz);
    if (rc < DSD_FREQ_ENTRY_OK || rc > DSD_FREQ_ENTRY_TOO_HIGH) {
        abort();
    }
    if (rc != DSD_FREQ_ENTRY_OK) {
        /* A refusal never writes the output, and every refusal but EMPTY says why. */
        const char* message = dsd_freq_entry_result_text(rc);
        if (hz != FUZZ_ENTRY_SENTINEL || (rc != DSD_FREQ_ENTRY_EMPTY && message[0] == '\0') || strlen(message) > 56U) {
            abort();
        }
        return;
    }
    if (hz == 0U) {
        abort();
    }

    char back[48];
    int n = DSD_SNPRINTF(back, sizeof back, "%u Hz", (unsigned)hz);
    if (n < 0 || (size_t)n >= sizeof back) {
        abort();
    }
    fuzz_entry_reparse(back, hz);

    char mhz[24];
    n = dsd_freq_hz_mhz_text(hz, mhz, sizeof mhz);
    if (n < 0 || (size_t)n >= sizeof mhz) {
        abort();
    }
    n = DSD_SNPRINTF(back, sizeof back, "%s MHz", mhz);
    if (n < 0 || (size_t)n >= sizeof back) {
        abort();
    }
    fuzz_entry_reparse(back, hz);
}

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
    fuzz_entry(text);
    return 0;
}
