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
#include <stddef.h>
#include <stdint.h>

/* Another radio now talks on slot @p slot's open P25 call: the alias the earlier radio's call showed is not this
 * radio's, so the call's row and the live display drop it. */
static inline void
p25p2_drop_voice_alias(const dsd_opts* opts, dsd_state* state, int slot) {
    state->generic_talker_alias[slot][0] = '\0';
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) > 0) {
        (void)dsd_event_enrich_alias_ex(opts, state, (uint8_t)slot, DSD_SYNC_P25P2_POS, call.epoch, "");
    }
}

/* Records a checked voice user's identity on slot @p slot's open P25 call (display and alias matching only): an
 * extended voice user's home identity @p home (dsd_call_state_note_source_home(); @p home->source is the working address
 * it carried, 0 for none), or, with @p home NULL or naming no radio, the working address @p source alone
 * (dsd_call_state_note_source_address()). When the home it records replaces another radio's on the same call (two
 * roaming radios with no working address and no end decoded between them), or the working address it names is not the
 * one the call's home was named with (a radio named by its home alone, then another by its working address), the alias
 * the earlier radio's call showed goes (p25p2_drop_voice_alias()). */
static inline void
p25p2_note_voice_home(const dsd_opts* opts, dsd_state* state, int slot, uint32_t source,
                      const dsd_call_source_home* home) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    int replaced = 0;
    if (home != NULL && home->valid != 0U) {
        if (dsd_call_state_note_source_home(state, (uint8_t)slot, DSD_SYNC_P25P2_POS, home, &replaced) <= 0) {
            return;
        }
    } else if (source == 0U
               || dsd_call_state_note_source_address(state, (uint8_t)slot, DSD_SYNC_P25P2_POS, source, &replaced)
                      <= 0) {
        return;
    }
    if (replaced) {
        p25p2_drop_voice_alias(opts, state, slot);
    }
}

#endif
