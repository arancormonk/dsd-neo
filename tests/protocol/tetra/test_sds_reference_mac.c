// SPDX-License-Identifier: GPL-3.0-or-later
/* Full MAC-to-SDS interoperability vector from smarek/kaitai-tetra-sds.
 * Upstream: Apache-2.0, commit 33f9fe3647e23e5a344cd96bfc6555531815e6bd,
 * sample2.text blob 70e397d01cfc6e95eef97afac6961214ad9037c9. */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_log.h"

int
main(void) {
    static const uint8_t packed[] = {
        0x20, 0xC9, 0x00, 0x00, 0x65, 0x04, 0x9E, 0xFF, 0xFF, 0xF0, 0x61, 0x02, 0x08, 0x00, 0x80, 0x05, 0x05,
        0xA1, 0xBD, 0xAB, 0x20, 0x28, 0x20, 0x00, 0x28, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t bits[sizeof(packed) * 8];
    for (size_t byte = 0; byte < sizeof(packed); byte++) {
        for (int bit = 0; bit < 8; bit++) {
            bits[byte * 8 + (size_t)bit] = (packed[byte] >> (7 - bit)) & 1u;
        }
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    Event_History_I* history = (Event_History_I*)calloc(2, sizeof(*history));
    if (!opts || !state || !history) {
        free(opts);
        free(state);
        free(history);
        return 1;
    }
    init_event_history(&history[0], 0, 255);
    init_event_history(&history[1], 0, 255);
    state->event_history_s = history;
    /* A CRC-passed block before this one proved the carrier, so the message is noted in the event history. */
    tetra_confirm_note_signalling(state);
    /* MAC block as published (34 octets): MAC-RESOURCE, length indication 25 octets, SSI 101, fill bits, BL-UDATA,
     * CMCE D-SDS-DATA; then a Null PDU and fill bits. */
    char log[1024];
    tetra_test_mac_logged(bits, (int)sizeof(bits), 7, opts, state, log, sizeof log);

    const int ok = tetra_st(state)->frames_resource == 1
                   && strstr(log, "[TETRA CMCE D-SDS-DATA] CC=7 src_SSI=16777184 CPTI=1 SDTI=3 data_bits=64") != NULL
                   && history[0].Event_History_Items[1].category == DSD_EVENT_CATEGORY_DATA
                   && history[0].Event_History_Items[1].target_id == 101U
                   && strcmp(history[0].Event_History_Items[1].text_message, "Ahoj") == 0;
    if (!ok) {
        DSD_FPRINTF(stderr, "reference SDS mismatch: resources=%u text='%s'\n%s",
                    (unsigned)tetra_st(state)->frames_resource, history[0].Event_History_Items[1].text_message, log);
    }
    dsd_state_ext_free_all(state);
    free(state);
    free(history);
    free(opts);
    if (!ok) {
        return 1;
    }
    puts("PASS test_sds_reference_mac: full 272-bit MAC PDU -> SDS 'Ahoj'");
    return 0;
}
