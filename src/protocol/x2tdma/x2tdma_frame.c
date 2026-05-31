// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "x2tdma_frame.h"
#include <dsd-neo/core/sync_patterns.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

bool
dsd_x2tdma_sync_is_data(const char* sync) {
    return sync != NULL && (strcmp(sync, X2TDMA_BS_DATA_SYNC) == 0 || strcmp(sync, X2TDMA_MS_DATA_SYNC) == 0);
}

bool
dsd_x2tdma_sync_is_voice(const char* sync) {
    return sync != NULL && (strcmp(sync, X2TDMA_BS_VOICE_SYNC) == 0 || strcmp(sync, X2TDMA_MS_VOICE_SYNC) == 0);
}

void
dsd_x2tdma_init_mi_placeholder(char mi[DSD_X2TDMA_MI_TEXT_LEN]) {
    if (mi == NULL) {
        return;
    }
    DSD_MEMSET(mi, '_', DSD_X2TDMA_MI_BITS);
    mi[DSD_X2TDMA_MI_BITS] = '\0';
}
