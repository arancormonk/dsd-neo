// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify P25 Phase 1 MBT → MAC bridging for Identifier Update PDUs populates
 * IDEN tables and drives the channel→frequency calculator.
 */

#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Test shim helper (exposed by protocol library)
int p25_test_mbt_iden_bridge(const unsigned char* mbt, int mbt_len, long* out_base, int* out_spac, int* out_type,
                             int* out_tdma, long* out_freq);

// Additional stubs referenced by MAC VPDU path (unused in this test)
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Craft a minimal ALT MBT PDU carrying Identifier Update (UHF/VHF, opcode 0x74)
    // IDEN=1, spacing=100 (12.5 kHz), base=851.000000 MHz (base field in 5 Hz units)
    uint8_t mbt[48];
    DSD_MEMSET(mbt, 0, sizeof(mbt));

    mbt[0] = 0x37; // outbound ALT format
    mbt[2] = 0x00; // MFID (standard)
    mbt[6] = 0x02; // blks=2 (3x12=36 total bytes), ample for payload
    mbt[7] = 0x74; // Identifier Update VHF/UHF (MAC-coded opcode)

    // Payload directly after opcode (bridging places payload at MAC[2..])
    // Byte layout per decoder in p25p2_vpdu.c (for 0x74):
    // [2]: (IDEN<<4) | BW, [3..4]: tx_off (14 bits), [4..5]: spacing, [6..9]: base (32 bits)
    mbt[8] = 0x10;  // IDEN=1, BW=0
    mbt[9] = 0x00;  // tx_off hi
    mbt[10] = 0x00; // tx_off lo + spacing hi (top 2 bits)
    mbt[11] = 0x64; // spacing lo = 100 (12.5 kHz)
    mbt[12] = 0x0A; // base (851000000 / 5) = 0x0A250BC0
    mbt[13] = 0x25;
    mbt[14] = 0x0B;
    mbt[15] = 0xC0;

    long base = 0;
    int spac = 0;
    int type = -1;
    int tdma = -1;
    long freq = 0;

    // Exercise the bridge via the shim and extract state results
    int sh = p25_test_mbt_iden_bridge(mbt, (int)sizeof(mbt), &base, &spac, &type, &tdma, &freq);
    if (sh != 0) {
        DSD_FPRINTF(stderr, "shim invocation failed (%d)\n", sh);
        return 99;
    }

    // Verify IDEN tables were populated (iden=1)
    rc |= expect_eq_int("chan_type[1]", type, 1);
    rc |= expect_eq_int("chan_tdma[1]", tdma, 0);
    rc |= expect_eq_long("spacing[1]", spac, 100);
    rc |= expect_eq_long("base[1]", base, 851000000 / 5);

    // Verify frequency calculation (iden=1, ch=10)
    long want_freq = 851000000 + 10 * 100 * 125; // 851.125 MHz
    rc |= expect_eq_long("freq(0x100A)", freq, want_freq);

    // Non-extended MBTC carries the opcode as the first data-header byte
    // after the 12-byte PDU header. Preserve that bridge so IDEN updates with
    // opcode at byte 12 and payload at byte 13 still populate the band plan.
    {
        uint8_t umb[48];
        DSD_MEMSET(umb, 0, sizeof(umb));

        umb[0] = 0x35;  // outbound Unconfirmed MBTC format
        umb[2] = 0x00;  // MFID (standard)
        umb[6] = 0x02;  // blks=2
        umb[12] = 0x74; // Identifier Update VHF/UHF opcode in data header

        umb[13] = 0x20; // IDEN=2, BW=0
        umb[14] = 0x00; // tx_off hi
        umb[15] = 0x00; // tx_off lo + spacing hi
        umb[16] = 0x64; // spacing lo = 100
        umb[17] = 0x0A; // base (851000000 / 5) = 0x0A250BC0
        umb[18] = 0x25;
        umb[19] = 0x0B;
        umb[20] = 0xC0;

        base = -1;
        spac = -1;
        type = -1;
        tdma = -1;
        freq = -1;

        int umb_shim_rc = p25_test_mbt_iden_bridge(umb, (int)sizeof(umb), &base, &spac, &type, &tdma, &freq);
        if (umb_shim_rc != 0) {
            DSD_FPRINTF(stderr, "umbtc shim invocation failed (%d)\n", umb_shim_rc);
            return 97;
        }

        rc |= expect_eq_int("umbtc_chan_type[2]", type, 1);
        rc |= expect_eq_int("umbtc_chan_tdma[2]", tdma, 0);
        rc |= expect_eq_long("umbtc_spacing[2]", spac, 100);
        rc |= expect_eq_long("umbtc_base[2]", base, 851000000 / 5);
        rc |= expect_eq_long("umbtc_freq(0x200A)", freq, want_freq);
    }

    // AMBTC opcode 0x33 is a foreign-system TDMA identifier update in sdrtrunk.
    // It must not populate the active system IDEN table.
    {
        uint8_t tdma_mbt[48];
        DSD_MEMSET(tdma_mbt, 0, sizeof(tdma_mbt));

        tdma_mbt[0] = 0x37; // outbound ALT format
        tdma_mbt[2] = 0x00; // MFID (standard)
        tdma_mbt[3] = 0x34; // IDEN=3, channel type=4
        tdma_mbt[6] = 0x02; // blks=2
        tdma_mbt[7] = 0x33; // Identifier Update TDMA (AMBTC)
        tdma_mbt[12] = 0x0A;
        tdma_mbt[13] = 0x25;
        tdma_mbt[14] = 0x0B;
        tdma_mbt[15] = 0xC0; // base = 851000000 / 5
        tdma_mbt[17] = 0x00;
        tdma_mbt[18] = 0x64; // spacing = 100

        base = -1;
        spac = -1;
        type = -1;
        tdma = -1;
        freq = -1;
        int tdma_shim_rc = p25_test_mbt_iden_bridge(tdma_mbt, (int)sizeof(tdma_mbt), &base, &spac, &type, &tdma, &freq);
        if (tdma_shim_rc != 0) {
            DSD_FPRINTF(stderr, "tdma shim invocation failed (%d)\n", tdma_shim_rc);
            return 98;
        }

        rc |= expect_eq_int("ambtc_0x33_type_unchanged", type, 0);
        rc |= expect_eq_int("ambtc_0x33_tdma_unchanged", tdma, 0);
        rc |= expect_eq_long("ambtc_0x33_spacing_unchanged", spac, 0);
        rc |= expect_eq_long("ambtc_0x33_base_unchanged", base, 0);
        rc |= expect_eq_long("ambtc_0x33_freq_unresolved", freq, 0);
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
