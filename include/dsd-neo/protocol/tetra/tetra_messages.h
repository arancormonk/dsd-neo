// SPDX-License-Identifier: GPL-3.0-or-later
/* Bounded decoded-message snapshots for the terminal frontend. */
#ifndef DSD_NEO_PROTOCOL_TETRA_MESSAGES_H_
#define DSD_NEO_PROTOCOL_TETRA_MESSAGES_H_

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>

static inline void
tetra_message_push(dsd_state* state, dsd_tetra_message_category category, const char* text) {
    if (state == NULL || text == NULL) {
        return;
    }
    dsd_tetra_message* item = &state->tetra_messages[state->tetra_message_sequence % DSD_TETRA_MESSAGE_CAPACITY];
    item->category = (uint8_t)category;
    DSD_SNPRINTF(item->text, sizeof item->text, "%s", text);
    for (char* p = item->text; *p != '\0'; ++p) {
        if ((unsigned char)*p < 0x20u || (unsigned char)*p == 0x7fu) {
            *p = ' ';
        }
    }
    state->tetra_message_sequence++;
}

#endif /* DSD_NEO_PROTOCOL_TETRA_MESSAGES_H_ */
