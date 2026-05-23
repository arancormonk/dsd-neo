// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_PROTOCOL_DMR_DMR_CONFIDENCE_H
#define DSD_NEO_PROTOCOL_DMR_DMR_CONFIDENCE_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DMR_CONFIDENCE_REJECT = -1,
    DMR_CONFIDENCE_PENDING = 0,
    DMR_CONFIDENCE_LOCKED = 1,
} dmr_confidence_result;

void dmr_confidence_reset(dsd_state* state);
void dmr_confidence_reset_slot(dsd_state* state, unsigned int slot);
void dmr_confidence_note_voice_sync(dsd_state* state, unsigned int slot);
dmr_confidence_result dmr_confidence_note_voice_burst(dsd_state* state, unsigned int slot, unsigned int color_code);
dmr_confidence_result dmr_confidence_note_data_burst(dsd_state* state, unsigned int color_code, unsigned int burst);
int dmr_confidence_voice_slot_open(const dsd_state* state, unsigned int slot);
int dmr_confidence_any_voice_open(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_DMR_DMR_CONFIDENCE_H */
