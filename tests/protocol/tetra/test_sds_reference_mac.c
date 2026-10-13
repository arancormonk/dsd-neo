// SPDX-License-Identifier: GPL-3.0-or-later
/* Full MAC-to-SDS interoperability vector from smarek/kaitai-tetra-sds.
 * Upstream: Apache-2.0, commit 33f9fe3647e23e5a344cd96bfc6555531815e6bd,
 * sample2.text blob 70e397d01cfc6e95eef97afac6961214ad9037c9. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_state.h"

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
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    /* MAC block as published (34 octets): MAC-RESOURCE, length indication 25 octets, SSI 101, fill bits, BL-UDATA,
     * CMCE D-SDS-DATA; then a Null PDU and fill bits. */
    tetra_mac_parse_schd(bits, (int)sizeof(bits), 7, opts, state);

    const int ok = tetra_st(state)->frames_resource == 1 && tetra_st(state)->cmce_sds_data_type == 3
                   && tetra_st(state)->sds_msg_ref == 32 && tetra_st(state)->sds_text_len == 4
                   && strcmp(tetra_st(state)->sds_text, "Ahoj") == 0 && state->tetra_message_sequence == 1
                   && state->tetra_messages[0].category == DSD_TETRA_MESSAGE_SDS
                   && strstr(state->tetra_messages[0].text, "Ahoj") != NULL;
    if (!ok) {
        DSD_FPRINTF(stderr, "reference SDS mismatch: resources=%u type=%u ref=%u len=%u text='%s'\n",
                    (unsigned)tetra_st(state)->frames_resource, (unsigned)tetra_st(state)->cmce_sds_data_type,
                    (unsigned)tetra_st(state)->sds_msg_ref, (unsigned)tetra_st(state)->sds_text_len,
                    tetra_st(state)->sds_text);
    }
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    if (!ok) {
        return 1;
    }
    puts("PASS test_sds_reference_mac: full 272-bit MAC PDU -> SDS 'Ahoj'");
    return 0;
}
