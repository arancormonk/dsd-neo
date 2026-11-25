// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* One word of the M17 end-of-transmission marker, as the hunt reads it and as the M17 dispatcher drains the rest of the
 * marker by it (<dsd-neo/dsp/frame_sync.h>). The marker is 0x555D repeated for 192 symbols, +3 six times, -3, +3 (M17
 * spec 2.0.11, End of Transmission Marker). */

#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <stddef.h>

int
dsd_frame_sync_m17_eot_word(const char* window, int inverted) {
    if (window == NULL) {
        return 0;
    }
    const char* marker = inverted ? M17_EOT_INV : M17_EOT;
    /* The marker's one symbol of the other sign: a flat run, within one symbol of the rest, never has it. */
    if (window[6] != marker[6]) {
        return 0;
    }
    int errors = 0;
    for (int i = 0; i < (int)DSD_FRAME_SYNC_M17_EOT_WORD_SYMBOLS; i++) {
        errors += window[i] != marker[i];
    }
    return errors <= 1;
}
