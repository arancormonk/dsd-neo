// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A valid P25 network status names the control channel it arrived on. Without an operator-imported channel list it
 * seeds row 0 of the list with that frequency; with one (-Y channel list or trunk scan targets) row 0 belongs to the
 * operator and stays, while the learned control channel (p25_cc_freq, the neighbor records) still updates. Each path
 * that seeds row 0 is covered: the neighbor recorder, the MBT, the LCW, and the Phase 2 abbreviated and extended MAC.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif
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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
}

static int
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

enum { kRowA = 0, kRowB = 1, kRowC = 2 };

static const long kListFreq[3] = {460125000L, 460350000L, 460575000L};
static const long kCcFreq = 851012500L; // IDEN 1 base 851 MHz, 12.5 kHz spacing, channel 1

static dsd_opts g_opts;
static dsd_state g_state;

/* A fresh decoder with the IDEN a CHAN-T of 0x1001 resolves through, and optionally a 3-row imported list. */
static void
reset(int with_list) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_state.p25_iden_fdma[1].base_freq = 851000000L / 5L;
    g_state.p25_iden_fdma[1].chan_type = 1;
    g_state.p25_iden_fdma[1].chan_spac = 100;
    g_state.p25_iden_fdma[1].trust = 2;
    g_state.p25_iden_fdma[1].populated = 1;
    g_state.p25_chan_tdma_explicit[1] |= 1U;
    if (with_list) {
        DSD_SNPRINTF(g_opts.chan_in_file, sizeof(g_opts.chan_in_file), "%s", "channels.csv");
        for (int i = 0; i < 3; i++) {
            g_state.trunk_lcn_freq[i] = kListFreq[i];
        }
        g_state.lcn_freq_count = 3;
    }
}

/* The learned control channel is recorded either way; row 0 and the other rows follow the list. */
static int
check(const char* tag, int with_list) {
    int rc = 0;
    char name[96];
    DSD_SNPRINTF(name, sizeof(name), "%s learns the cc", tag);
    rc |= expect_int(name, g_state.p25_cc_freq, kCcFreq);
    DSD_SNPRINTF(name, sizeof(name), "%s row 0", tag);
    rc |= expect_int(name, g_state.trunk_lcn_freq[0], with_list ? kListFreq[kRowA] : kCcFreq);
    if (with_list) {
        DSD_SNPRINTF(name, sizeof(name), "%s row 1", tag);
        rc |= expect_int(name, g_state.trunk_lcn_freq[kRowB], kListFreq[kRowB]);
        DSD_SNPRINTF(name, sizeof(name), "%s row 2", tag);
        rc |= expect_int(name, g_state.trunk_lcn_freq[kRowC], kListFreq[kRowC]);
        DSD_SNPRINTF(name, sizeof(name), "%s count", tag);
        rc |= expect_int(name, g_state.lcn_freq_count, 3);
    } else {
        DSD_SNPRINTF(name, sizeof(name), "%s count", tag);
        rc |= expect_int(name, g_state.lcn_freq_count, 1);
    }
    return rc;
}

static int
test_neighbor_recorder(int with_list) {
    reset(with_list);
    g_state.p25_cc_freq = kCcFreq;
    const long freqs[1] = {kCcFreq};
    p25_cc_record_neighbor_frequencies(&g_opts, &g_state, freqs, 1);
    return check("neighbor", with_list);
}

static int
test_mbt(int with_list) {
    uint8_t mbt[48];
    DSD_MEMSET(mbt, 0, sizeof(mbt));
    mbt[0] = 0x37; // OSP ALT format
    mbt[3] = 0x01; // LRA
    mbt[4] = 0x01;
    mbt[5] = 0x23; // SYSID 0x123
    mbt[6] = 0x02;
    mbt[7] = 0x3B; // network status broadcast
    mbt[12] = 0xBE;
    mbt[13] = 0xE0;
    mbt[14] = 0x00;
    mbt[15] = 0x10; // CHAN-T 0x1001
    mbt[16] = 0x01;
    reset(with_list);
    (void)p25_decode_pdu_trunking(&g_opts, &g_state, mbt, sizeof(mbt));
    return check("mbt", with_list);
}

static void
put_bits(uint8_t* bits, int start, int width, unsigned long long value) {
    for (int i = 0; i < width; i++) {
        bits[start + i] = (uint8_t)((value >> (width - 1 - i)) & 1ULL);
    }
}

static int
test_lcw(int with_list) {
    uint8_t lcw[96];
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    put_bits(lcw, 0, 8, 0x64);
    put_bits(lcw, 16, 20, 0xBEE00);
    put_bits(lcw, 36, 12, 0x123);
    put_bits(lcw, 48, 16, 0x1001);
    reset(with_list);
    p25_lcw(&g_opts, &g_state, lcw, 0);
    return check("lcw", with_list);
}

static int
test_vpdu(int with_list, unsigned long long opcode) {
    unsigned long long mac[24] = {0};
    mac[1] = opcode;
    mac[2] = 0x01; // LRA
    mac[3] = 0xBE;
    mac[4] = 0xE0;
    mac[5] = 0x01; // WACN low nibble 0, SYSID high nibble 1
    mac[6] = 0x23; // SYSID 0x123
    mac[7] = 0x10; // CHAN-T 0x1001
    mac[8] = 0x01;
    const int nac_at = opcode == 0xFB ? 12 : 10;
    mac[nac_at] = 0x02;
    mac[nac_at + 1] = 0x93;
    reset(with_list);
    process_MAC_VPDU_crc(&g_opts, &g_state, 1, P25_MAC_PDU_SIGNAL, mac, 1);
    return check(opcode == 0xFB ? "vpdu extended" : "vpdu abbreviated", with_list);
}

int
main(void) {
    int rc = 0;
    for (int with_list = 0; with_list < 2; with_list++) {
        rc |= test_neighbor_recorder(with_list);
        rc |= test_mbt(with_list);
        rc |= test_lcw(with_list);
        rc |= test_vpdu(with_list, 0x7B);
        rc |= test_vpdu(with_list, 0xFB);
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
