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
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
decode_bits(const char *text, uint8_t *out, size_t capacity)
{
    size_t len = strlen(text);
    if (len > capacity) return -1;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] != '0' && text[i] != '1') return -1;
        out[i] = (uint8_t)(text[i] - '0');
    }
    return (int)len;
}

int
main(void)
{
    static const char bsch_text[] =
        "001000001111001011110100000000001001001010000000000011111001";
    static const char sysinfo_text[] =
        "100011101010010000111100000011000110011010010000000000000000"
        "1101100000100000000000000000101000001111111111111101110101110111";
    uint8_t bsch[60];
    uint8_t sysinfo[124];
    dsd_state *state = (dsd_state *)calloc(1, sizeof(*state));
    dsd_opts *opts = (dsd_opts *)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        free(state); free(opts);
        return 1;
    }

    int bsch_len = decode_bits(bsch_text, bsch, sizeof(bsch));
    int sysinfo_len = decode_bits(sysinfo_text, sysinfo, sizeof(sysinfo));
    int ok = bsch_len == 60 && sysinfo_len == 124
          && tetra_bsch_parse(bsch, bsch_len, opts, state);
    ok = ok && state->tetra_mcc == 293 && state->tetra_mnc == 7
         && state->tetra_colour == 3 && state->tetra_tn == 4
         && state->tetra_fn == 5 && state->tetra_mn == 58;

    if (ok)
        tetra_mac_parse_schd(sysinfo, sysinfo_len, 0, opts, state);
    ok = ok && state->tetra_sysinfo_known
         && state->tetra_dl_carrier_hz == 393712500L
         && state->tetra_bs_service_det == 0x0D77u;

    if (!ok) {
        fprintf(stderr,
                "FAIL real-air trace: MCC=%u MNC=%u CC=%u TN/FN/MN=%u/%u/%u "
                "DL=%ld service=0x%04X\n",
                state->tetra_mcc, state->tetra_mnc, state->tetra_colour,
                state->tetra_tn, state->tetra_fn, state->tetra_mn,
                state->tetra_dl_carrier_hz, state->tetra_bs_service_det);
    }
    free(state); free(opts);
    return ok ? 0 : 1;
}
