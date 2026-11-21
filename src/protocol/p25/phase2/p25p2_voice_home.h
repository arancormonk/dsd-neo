// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_VOICE_HOME_H_
#define DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_VOICE_HOME_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>

/* Records an extended voice user's checked home identity @p home on slot @p slot's open P25 call (display and alias
 * matching only, dsd_call_state_note_source_home()). When it replaces another radio's home on the same call (two
 * roaming radios with no working address and no end decoded between them), the alias the earlier radio's call showed
 * is not this radio's: the call's row and the live display drop it. */
static inline void
p25p2_note_voice_home(const dsd_opts* opts, dsd_state* state, int slot, const dsd_call_source_home* home) {
    if (!state || !home || home->valid == 0U || slot < 0 || slot > 1) {
        return;
    }
    int replaced = 0;
    if (dsd_call_state_note_source_home(state, (uint8_t)slot, DSD_SYNC_P25P2_POS, home, &replaced) <= 0 || !replaced) {
        return;
    }
    state->generic_talker_alias[slot][0] = '\0';
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) > 0) {
        (void)dsd_event_enrich_alias_ex(opts, state, (uint8_t)slot, DSD_SYNC_P25P2_POS, call.epoch, "");
    }
}

#endif
