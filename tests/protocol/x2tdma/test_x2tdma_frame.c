// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/sync_patterns.h>
#include <string.h>
#include "x2tdma_frame.h"

static void
test_sync_classification_includes_mobile_data(void) {
    assert(dsd_x2tdma_sync_is_data(X2TDMA_BS_DATA_SYNC));
    assert(dsd_x2tdma_sync_is_data(X2TDMA_MS_DATA_SYNC));
    assert(!dsd_x2tdma_sync_is_data(X2TDMA_BS_VOICE_SYNC));
    assert(!dsd_x2tdma_sync_is_data(NULL));

    assert(dsd_x2tdma_sync_is_voice(X2TDMA_BS_VOICE_SYNC));
    assert(dsd_x2tdma_sync_is_voice(X2TDMA_MS_VOICE_SYNC));
    assert(!dsd_x2tdma_sync_is_voice(X2TDMA_MS_DATA_SYNC));
    assert(!dsd_x2tdma_sync_is_voice(NULL));
}

static void
test_mi_placeholder_is_terminated(void) {
    char mi[DSD_X2TDMA_MI_TEXT_LEN];
    dsd_x2tdma_init_mi_placeholder(mi);

    for (int i = 0; i < DSD_X2TDMA_MI_BITS; i++) {
        assert(mi[i] == '_');
    }
    assert(mi[DSD_X2TDMA_MI_BITS] == '\0');
    assert(strlen(mi) == DSD_X2TDMA_MI_BITS);

    dsd_x2tdma_init_mi_placeholder(NULL);
}

int
main(void) {
    test_sync_classification_includes_mobile_data();
    test_mi_placeholder_is_terminated();
    return 0;
}
