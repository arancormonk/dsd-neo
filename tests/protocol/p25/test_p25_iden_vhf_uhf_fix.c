// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 IDEN field extraction tests.
 *
 * These assertions follow sdrtrunk's FrequencyBandUpdate,
 * FrequencyBandUpdateVUHF, and FrequencyBandUpdateTDMAAbbreviated classes:
 * opcode 0x7D always uses the standard FDMA layout, while opcode 0x74 uses
 * the VHF/UHF layout. Base frequency alone does not change a 0x7D payload into
 * VHF/UHF format.
 */

#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25p2_mac_parse.h>
#include <stdio.h>
#include <string.h>

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
signed_units(int sign_bit, int raw_offset) {
    return sign_bit ? raw_offset : -raw_offset;
}

static void
put_base(unsigned char* mac, int pos, long base_freq) {
    mac[pos + 0] = (unsigned char)((base_freq >> 24) & 0xFF);
    mac[pos + 1] = (unsigned char)((base_freq >> 16) & 0xFF);
    mac[pos + 2] = (unsigned char)((base_freq >> 8) & 0xFF);
    mac[pos + 3] = (unsigned char)(base_freq & 0xFF);
}

static void
build_standard_iden(unsigned char* mac, int iden, int bandwidth, int sign_bit, int tx_raw, int spacing,
                    long base_freq) {
    memset(mac, 0, 24);
    mac[1] = 0x7D;
    mac[2] = (unsigned char)(((iden & 0x0F) << 4) | ((bandwidth >> 5) & 0x0F));
    mac[3] = (unsigned char)(((bandwidth & 0x1F) << 3) | ((sign_bit & 0x01) << 2) | ((tx_raw >> 6) & 0x03));
    mac[4] = (unsigned char)(((tx_raw & 0x3F) << 2) | ((spacing >> 8) & 0x03));
    mac[5] = (unsigned char)(spacing & 0xFF);
    put_base(mac, 6, base_freq);
}

static void
build_vuhf_iden(unsigned char* mac, int opcode, int iden, int low_nibble, int sign_bit, int tx_raw, int spacing,
                long base_freq) {
    memset(mac, 0, 24);
    mac[1] = (unsigned char)opcode;
    mac[2] = (unsigned char)(((iden & 0x0F) << 4) | (low_nibble & 0x0F));
    mac[3] = (unsigned char)(((sign_bit & 0x01) << 7) | ((tx_raw >> 6) & 0x7F));
    mac[4] = (unsigned char)(((tx_raw & 0x3F) << 2) | ((spacing >> 8) & 0x03));
    mac[5] = (unsigned char)(spacing & 0xFF);
    put_base(mac, 6, base_freq);
}

static int
test_standard_0x7d_stays_standard_on_vhf_base(void) {
    int rc = 0;
    unsigned char mac[24];
    unsigned long long words[24] = {0};
    struct p25p2_iden_update up = {0};

    const int iden = 4;
    const int bandwidth = 0x12A;
    const int sign = 0;
    const int tx_raw = 0x5A;
    const int spacing = 0x064;
    const long base = 155000000L / 5L;

    build_standard_iden(mac, iden, bandwidth, sign, tx_raw, spacing, base);
    for (int i = 0; i < 24; i++) {
        words[i] = mac[i];
    }

    rc |= expect_eq_int("0x7D decode", p25p2_mac_decode_iden_standard(words, 2, &up), 0);

    rc |= expect_eq_int("0x7D iden", up.iden, iden);
    rc |= expect_eq_int("0x7D standard bandwidth", up.bandwidth, bandwidth);
    rc |= expect_eq_int("0x7D bw_vu unused", up.bw_vu, 0);
    rc |= expect_eq_int("0x7D signed offset", up.trans_off, signed_units(sign, tx_raw));
    rc |= expect_eq_int("0x7D spacing", up.chan_spac, spacing);
    rc |= expect_eq_long("0x7D base", up.base_freq, base);

    if (rc == 0) {
        fprintf(stderr, "PASS test_standard_0x7d_stays_standard_on_vhf_base\n");
    }
    return rc;
}

static int
test_vuhf_0x74_uses_vuhf_layout(void) {
    int rc = 0;
    unsigned char mac[24];
    unsigned long long words[24] = {0};
    struct p25p2_iden_update up = {0};

    const int iden = 5;
    const int bw_vu = 0x5;
    const int sign = 1;
    const int tx_raw = 0x1234;
    const int spacing = 0x064;
    const long base = 450000000L / 5L;

    build_vuhf_iden(mac, 0x74, iden, bw_vu, sign, tx_raw, spacing, base);
    for (int i = 0; i < 24; i++) {
        words[i] = mac[i];
    }

    rc |= expect_eq_int("0x74 decode", p25p2_mac_decode_iden_vuhf(words, 2, &up), 0);

    rc |= expect_eq_int("0x74 iden", up.iden, iden);
    rc |= expect_eq_int("0x74 bw_vu", up.bw_vu, bw_vu);
    rc |= expect_eq_int("0x74 signed offset", up.trans_off, signed_units(sign, tx_raw));
    rc |= expect_eq_int("0x74 spacing", up.chan_spac, spacing);
    rc |= expect_eq_long("0x74 base", up.base_freq, base);

    if (rc == 0) {
        fprintf(stderr, "PASS test_vuhf_0x74_uses_vuhf_layout\n");
    }
    return rc;
}

static int
test_tdma_0x73_uses_signed_tdma_layout(void) {
    int rc = 0;
    unsigned char mac[24];
    unsigned long long words[24] = {0};
    struct p25p2_iden_update up = {0};

    const int iden = 6;
    const int chan_type = 3;
    const int sign = 0;
    const int tx_raw = 0x0123;
    const int spacing = 0x032;
    const long base = 851000000L / 5L;

    build_vuhf_iden(mac, 0x73, iden, chan_type, sign, tx_raw, spacing, base);
    for (int i = 0; i < 24; i++) {
        words[i] = mac[i];
    }

    rc |= expect_eq_int("0x73 decode", p25p2_mac_decode_iden_tdma(words, 2, &up), 0);

    rc |= expect_eq_int("0x73 iden", up.iden, iden);
    rc |= expect_eq_int("0x73 type", up.chan_type, chan_type);
    rc |= expect_eq_int("0x73 signed offset", up.trans_off, signed_units(sign, tx_raw));
    rc |= expect_eq_int("0x73 spacing", up.chan_spac, spacing);
    rc |= expect_eq_long("0x73 base", up.base_freq, base);

    if (rc == 0) {
        fprintf(stderr, "PASS test_tdma_0x73_uses_signed_tdma_layout\n");
    }
    return rc;
}

static int
test_vhf_uhf_base_classifier(void) {
    int rc = 0;
    rc |= expect_eq_int("VHF base", p25_is_vhf_uhf_base_freq(155000000L / 5L), 1);
    rc |= expect_eq_int("UHF base", p25_is_vhf_uhf_base_freq(450000000L / 5L), 1);
    rc |= expect_eq_int("700 MHz base", p25_is_vhf_uhf_base_freq(770000000L / 5L), 0);
    rc |= expect_eq_int("850 MHz base", p25_is_vhf_uhf_base_freq(851000000L / 5L), 0);
    if (rc == 0) {
        fprintf(stderr, "PASS test_vhf_uhf_base_classifier\n");
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_standard_0x7d_stays_standard_on_vhf_base();
    rc |= test_vuhf_0x74_uses_vuhf_layout();
    rc |= test_tdma_0x73_uses_signed_tdma_layout();
    rc |= test_vhf_uhf_base_classifier();

    if (rc == 0) {
        fprintf(stderr, "\nAll test_p25_iden_vhf_uhf_fix tests PASSED\n");
    } else {
        fprintf(stderr, "\nSome test_p25_iden_vhf_uhf_fix tests FAILED\n");
    }
    return rc;
}
