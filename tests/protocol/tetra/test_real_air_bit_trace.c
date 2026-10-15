// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Public real-air decoded-bit regression from the Osmocom TETRA mailing list:
 * https://lists.osmocom.org/hyperkitty/list/tetra@lists.osmocom.org/thread/6DN55M2TG23RWBDOLGJYJPNQB337C4H6/
 *
 * The February 2013 trace reports both blocks CRC=OK and publishes the decoded
 * type-1 bits plus MCC=293, MNC=7, colour=3 and DL=393712500 Hz. Keeping the
 * factual bit fields here exercises our parsers against an on-air observation;
 * it is intentionally not described as an IQ/frontend validation.
 */
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_mac_pdu.h"
#include "tetra_state.h"

static int
decode_bits(const char* text, uint8_t* out, size_t capacity) {
    size_t len = strlen(text);
    if (len > capacity) {
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        if (text[i] != '0' && text[i] != '1') {
            return -1;
        }
        out[i] = (uint8_t)(text[i] - '0');
    }
    return (int)len;
}

int
main(void) {
    static const char bsch_text[] = "001000001111001011110100000000001001001010000000000011111001";
    static const char sysinfo_text[] = "100011101010010000111100000011000110011010010000000000000000"
                                       "1101100000100000000000000000101000001111111111111101110101110111";
    uint8_t bsch[60];
    uint8_t sysinfo[124];
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        free(state);
        free(opts);
        return 1;
    }

    int bsch_len = decode_bits(bsch_text, bsch, sizeof(bsch));
    int sysinfo_len = decode_bits(sysinfo_text, sysinfo, sizeof(sysinfo));
    int ok = bsch_len == 60 && sysinfo_len == 124 && tetra_bsch_parse(bsch, bsch_len, opts, state);
    ok = ok && tetra_st(state)->mcc == 293 && tetra_st(state)->mnc == 7 && tetra_st(state)->colour == 3
         && tetra_st(state)->tdma_valid && tetra_st(state)->tn == 4 && tetra_st(state)->fn == 5
         && tetra_st(state)->mn == 58;

    tetra_mac_broadcast bc;
    DSD_MEMSET(&bc, 0, sizeof bc);
    ok = ok && tetra_mac_broadcast_decode(sysinfo, sysinfo_len, &bc) && bc.sysinfo.mle_present
         && bc.sysinfo.bs_service == 0x0D77u;
    if (ok) {
        tetra_mac_parse_schd(sysinfo, sysinfo_len, 0, opts, state);
    }
    ok = ok && tetra_st(state)->sysinfo_known && tetra_st(state)->dl_carrier_hz == 393712500L;

    if (!ok) {
        DSD_FPRINTF(stderr,
                    "FAIL real-air trace: MCC=%u MNC=%u CC=%u TN/FN/MN=%u/%u/%u "
                    "DL=%ld service=0x%04X\n",
                    tetra_st(state)->mcc, tetra_st(state)->mnc, tetra_st(state)->colour, tetra_st(state)->tn,
                    tetra_st(state)->fn, tetra_st(state)->mn, tetra_st(state)->dl_carrier_hz,
                    (unsigned)bc.sysinfo.bs_service);
    }
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return ok ? 0 : 1;
}
