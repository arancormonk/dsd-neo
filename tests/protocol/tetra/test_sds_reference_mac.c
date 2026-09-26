// SPDX-License-Identifier: GPL-3.0-or-later
/* Full MAC-to-SDS interoperability vector from smarek/kaitai-tetra-sds.
 * Upstream: Apache-2.0, commit 33f9fe3647e23e5a344cd96bfc6555531815e6bd,
 * sample2.text blob 70e397d01cfc6e95eef97afac6961214ad9037c9. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    static const uint8_t packed[] = {
        0x20,0xC9,0x00,0x00,0x65,0x04,0x9E,0xFF,0xFF,0xF0,0x61,0x02,
        0x08,0x00,0x80,0x05,0x05,0xA1,0xBD,0xAB,0x20,0x28,0x20,0x00,
        0x28,0x00,0x10,0x80,0x00,0x00,0x00,0x00,0x00,0x00
    };
    uint8_t bits[sizeof(packed) * 8];
    for (size_t byte = 0; byte < sizeof(packed); byte++)
        for (int bit = 0; bit < 8; bit++)
            bits[byte * 8 + (size_t)bit] = (packed[byte] >> (7 - bit)) & 1u;

    dsd_opts *opts = (dsd_opts *)calloc(1, sizeof(*opts));
    dsd_state *state = (dsd_state *)calloc(1, sizeof(*state));
    if (!opts || !state) return 1;
    tetra_mac_parse_schd(bits, (int)sizeof(bits), 7, opts, state);

    const int ok = state->tetra_frames_resource == 1
        && state->tetra_cmce_sds_data_type == 3
        && state->tetra_sds_msg_ref == 32
        && state->tetra_sds_text_len == 4
        && strcmp(state->tetra_sds_text, "Ahoj") == 0
        && state->tetra_message_sequence == 1
        && state->tetra_messages[0].category == DSD_TETRA_MESSAGE_SDS
        && strstr(state->tetra_messages[0].text, "Ahoj") != NULL;
    if (!ok) {
        fprintf(stderr,
                "reference SDS mismatch: resources=%llu type=%u ref=%u len=%u text='%s'\n",
                (unsigned long long)state->tetra_frames_resource,
                (unsigned)state->tetra_cmce_sds_data_type,
                (unsigned)state->tetra_sds_msg_ref,
                (unsigned)state->tetra_sds_text_len,
                state->tetra_sds_text);
    }
    free(state);
    free(opts);
    if (!ok) return 1;
    puts("PASS test_sds_reference_mac: full 272-bit MAC PDU -> SDS 'Ahoj'");
    return 0;
}
