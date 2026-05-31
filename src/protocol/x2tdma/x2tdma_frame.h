// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_X2TDMA_X2TDMA_FRAME_H_
#define DSD_NEO_SRC_PROTOCOL_X2TDMA_X2TDMA_FRAME_H_

#include <stdbool.h>

enum {
    DSD_X2TDMA_MI_BITS = 72,
    DSD_X2TDMA_MI_TEXT_LEN = DSD_X2TDMA_MI_BITS + 1,
};

bool dsd_x2tdma_sync_is_data(const char* sync);
bool dsd_x2tdma_sync_is_voice(const char* sync);
void dsd_x2tdma_init_mi_placeholder(char mi[DSD_X2TDMA_MI_TEXT_LEN]);

#endif /* DSD_NEO_SRC_PROTOCOL_X2TDMA_X2TDMA_FRAME_H_ */
