// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 AMBTC/MBT opcode 0x33 foreign IDEN tests.
 *
 * SDRTrunk's AMBTCFrequencyBandUpdateTDMA identifies this message as a
 * foreign-system frequency band update and deliberately does not process it as
 * an IFrequencyBand for the current system. DSD-neo should therefore decode it
 * for logging only and leave the current IDEN tables untouched.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

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
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static void
build_mbt_0x33(uint8_t* mbt, int iden, int chan_type) {
    DSD_MEMSET(mbt, 0, 32);
    mbt[0] = 0x17; /* ALT MBT */
    mbt[2] = 0x00; /* standard MFID */
    mbt[6] = 0x01; /* one data block */
    mbt[7] = 0x33;

    mbt[3] = (uint8_t)(((iden & 0x0F) << 4) | (chan_type & 0x0F));

    /* Foreign WACN/SYSID fields from SDRTrunk AMBTCFrequencyBandUpdateTDMA. */
    mbt[4] = 0xAB;
    mbt[5] = 0xCD;
    mbt[8] = 0xE1;
    mbt[9] = 0x23;

    /* Base, signed transmit offset, and channel spacing in data block 0. */
    mbt[12] = 0x05;
    mbt[13] = 0x15;
    mbt[14] = 0x5A;
    mbt[15] = 0x40;
    mbt[16] = 0x82;
    mbt[17] = 0x34;
    mbt[18] = 0x56;
}

static void
seed_current_iden(dsd_state* st, int iden) {
    st->p25_chan_iden = 7;
    st->p25_chan_tdma_explicit[iden] = 3;

    st->p25_iden_fdma[iden].base_freq = 11111111L;
    st->p25_iden_fdma[iden].chan_type = 1;
    st->p25_iden_fdma[iden].chan_spac = 50;
    st->p25_iden_fdma[iden].trans_off = -2200;
    st->p25_iden_fdma[iden].trust = 2;
    st->p25_iden_fdma[iden].populated = 1;

    st->p25_iden_tdma[iden].base_freq = 22222222L;
    st->p25_iden_tdma[iden].chan_type = 3;
    st->p25_iden_tdma[iden].chan_spac = 100;
    st->p25_iden_tdma[iden].trans_off = 7200;
    st->p25_iden_tdma[iden].trust = 2;
    st->p25_iden_tdma[iden].populated = 1;
}

static int
test_mbt_0x33_does_not_create_current_iden(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    uint8_t mbt[32];

    for (int chan_type = 0; chan_type < 16; chan_type++) {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        build_mbt_0x33(mbt, 5, chan_type);

        p25_decode_pdu_trunking(&opts, &st, mbt);

        char tag[96];
        DSD_SNPRINTF(tag, sizeof tag, "type %d fdma empty", chan_type);
        rc |= expect_eq_u8(tag, st.p25_iden_fdma[5].populated, 0);
        DSD_SNPRINTF(tag, sizeof tag, "type %d tdma empty", chan_type);
        rc |= expect_eq_u8(tag, st.p25_iden_tdma[5].populated, 0);
        DSD_SNPRINTF(tag, sizeof tag, "type %d bitmask empty", chan_type);
        rc |= expect_eq_u8(tag, st.p25_chan_tdma_explicit[5], 0);
        DSD_SNPRINTF(tag, sizeof tag, "type %d p25_chan_iden unchanged", chan_type);
        rc |= expect_eq_int(tag, st.p25_chan_iden, 0);
    }

    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS test_mbt_0x33_does_not_create_current_iden\n");
    }
    return rc;
}

static int
test_mbt_0x33_does_not_overwrite_current_iden(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    uint8_t mbt[32];

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    build_mbt_0x33(mbt, 5, 3);
    seed_current_iden(&st, 5);

    p25_decode_pdu_trunking(&opts, &st, mbt);

    rc |= expect_eq_int("p25_chan_iden preserved", st.p25_chan_iden, 7);
    rc |= expect_eq_u8("bitmask preserved", st.p25_chan_tdma_explicit[5], 3);

    rc |= expect_eq_u8("fdma populated preserved", st.p25_iden_fdma[5].populated, 1);
    rc |= expect_eq_long("fdma base preserved", st.p25_iden_fdma[5].base_freq, 11111111L);
    rc |= expect_eq_int("fdma type preserved", st.p25_iden_fdma[5].chan_type, 1);
    rc |= expect_eq_int("fdma spacing preserved", st.p25_iden_fdma[5].chan_spac, 50);
    rc |= expect_eq_int("fdma offset preserved", st.p25_iden_fdma[5].trans_off, -2200);

    rc |= expect_eq_u8("tdma populated preserved", st.p25_iden_tdma[5].populated, 1);
    rc |= expect_eq_long("tdma base preserved", st.p25_iden_tdma[5].base_freq, 22222222L);
    rc |= expect_eq_int("tdma type preserved", st.p25_iden_tdma[5].chan_type, 3);
    rc |= expect_eq_int("tdma spacing preserved", st.p25_iden_tdma[5].chan_spac, 100);
    rc |= expect_eq_int("tdma offset preserved", st.p25_iden_tdma[5].trans_off, 7200);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS test_mbt_0x33_does_not_overwrite_current_iden\n");
    }
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_mbt_0x33_does_not_create_current_iden();
    rc |= test_mbt_0x33_does_not_overwrite_current_iden();

    if (rc == 0) {
        DSD_FPRINTF(stderr, "\nAll test_p25_mbt_0x33_foreign_iden tests PASSED\n");
    } else {
        DSD_FPRINTF(stderr, "\nSome test_p25_mbt_0x33_foreign_iden tests FAILED\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
