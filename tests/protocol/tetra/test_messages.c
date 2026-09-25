// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/protocol/tetra/tetra_messages.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int
main(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    tetra_message_push(state, DSD_TETRA_MESSAGE_SDS, "hello\nworld\033[31m");
    assert(state->tetra_message_sequence == 1);
    assert(state->tetra_messages[0].category == DSD_TETRA_MESSAGE_SDS);
    assert(strcmp(state->tetra_messages[0].text, "hello world [31m") == 0);

    for (unsigned i = 0; i < DSD_TETRA_MESSAGE_CAPACITY; ++i) {
        tetra_message_push(state, DSD_TETRA_MESSAGE_CONTROL, "control");
    }
    assert(state->tetra_message_sequence == DSD_TETRA_MESSAGE_CAPACITY + 1);
    assert(state->tetra_messages[0].category == DSD_TETRA_MESSAGE_CONTROL);
    free(state);
    return 0;
}
