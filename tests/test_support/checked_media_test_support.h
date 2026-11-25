// SPDX-License-Identifier: GPL-3.0-or-later
/* A voice frame its decoder output as speech, for the scan tests that drive the call state without a decoder. */
#ifndef DSD_NEO_TEST_CHECKED_MEDIA_TEST_SUPPORT_H
#define DSD_NEO_TEST_CHECKED_MEDIA_TEST_SUPPORT_H
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

/* A frame its decoder output as speech (issue #890): the call's media mark and its checked media, as a decoder makes
   both for a good frame, at @p t; the voice-only scan credits only the checked media. Returns the media mark's
   result (dsd_call_state_update_media()). */
static inline int
decoded_media(dsd_state* state, uint8_t slot, int media_active, double t) {
    const int rc = dsd_call_state_update_media(state, slot, media_active, t);
    dsd_call_snapshot call;
    if (rc == 1 && media_active && dsd_call_state_get(state, slot, &call) > 0) {
        (void)dsd_call_state_note_checked_media(state, slot, call.protocol, t);
    }
    return rc;
}

#endif
