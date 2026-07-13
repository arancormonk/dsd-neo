// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for P25 Tier 3 protection, time/date, and status handlers.
 *
 * Exercises canonical ALGID lookup and production VPDU dispatch/state updates.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* External entry point under test */
void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]);

static void
mac_set_bits(unsigned long long MAC[24], int start_bit, int bit_count, uint64_t value) {
    for (int i = 0; i < bit_count; i++) {
        int bit = start_bit + i;
        int octet = 1 + (bit / 8);
        int shift = 7 - (bit % 8);
        unsigned long long mask = 1ULL << shift;
        if ((value >> (bit_count - 1 - i)) & 0x1U) {
            MAC[octet] |= mask;
        } else {
            MAC[octet] &= ~mask;
        }
    }
}

static void
build_time_date_mac(unsigned long long MAC[24], int vd, int vt, int vl, int lto_sign, int offset_minutes, int year,
                    int month, int day, int hours, int minutes, int seconds) {
    DSD_MEMSET(MAC, 0, 24 * sizeof(unsigned long long));
    MAC[0] = 0x09;
    MAC[1] = 0x75;
    mac_set_bits(MAC, 8, 1, (uint64_t)vd);
    mac_set_bits(MAC, 9, 1, (uint64_t)vt);
    mac_set_bits(MAC, 10, 1, (uint64_t)vl);
    if (vl) {
        mac_set_bits(MAC, 11, 1, (uint64_t)lto_sign);
        mac_set_bits(MAC, 12, 12, (uint64_t)offset_minutes);
    }
    if (vd) {
        mac_set_bits(MAC, 24, 4, (uint64_t)month);
        mac_set_bits(MAC, 28, 5, (uint64_t)day);
        mac_set_bits(MAC, 33, 13, (uint64_t)year);
    }
    if (vt) {
        mac_set_bits(MAC, 48, 5, (uint64_t)hours);
        mac_set_bits(MAC, 53, 6, (uint64_t)minutes);
        mac_set_bits(MAC, 59, 6, (uint64_t)seconds);
    }
}

static void
build_p1_bridged_time_date_offset_mac(unsigned long long MAC[24], int raw_offset, int reserved_bit) {
    DSD_MEMSET(MAC, 0, 24 * sizeof(*MAC));
    MAC[0] = 0x07;
    MAC[1] = 0x75;
    MAC[2] = (unsigned long long)(0x20 | (reserved_bit ? 0x10 : 0x00) | ((raw_offset >> 8) & 0x0F));
    MAC[3] = (unsigned long long)(raw_offset & 0xFF);
}

/* ============================================================================
 * Stubs for external hooks referenced by linked modules
 * ============================================================================ */

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

/* ============================================================================
 * 6.1 Protection Parameter Tests
 * ============================================================================ */

/**
 * test_prot_param_algid_unencrypted_name — verify ALGID 0x80 resolves to
 * "UNENCRYPTED"
 */
static int
test_prot_param_algid_unencrypted_name(void) {
    const char* name = p25_algid_name(0x80);
    if (name == NULL || strcmp(name, "UNENCRYPTED") != 0) {
        DSD_FPRINTF(stderr, "FAIL: test_prot_param_algid_unencrypted_name: expected 'UNENCRYPTED', got '%s'\n",
                    name ? name : "(null)");
        return 1;
    }
    return 0;
}

/**
 * test_prot_param_algid_des_ofb_name — verify ALGID 0x81 resolves to "DES-OFB"
 */
static int
test_prot_param_algid_des_ofb_name(void) {
    const char* name = p25_algid_name(0x81);
    if (name == NULL || strcmp(name, "DES-OFB") != 0) {
        DSD_FPRINTF(stderr, "FAIL: test_prot_param_algid_des_ofb_name: expected 'DES-OFB', got '%s'\n",
                    name ? name : "(null)");
        return 1;
    }
    return 0;
}

/**
 * test_prot_param_algid_aes256_name — verify ALGID 0x84 resolves to "AES-256"
 */
static int
test_prot_param_algid_aes256_name(void) {
    const char* name = p25_algid_name(0x84);
    if (name == NULL || strcmp(name, "AES-256") != 0) {
        DSD_FPRINTF(stderr, "FAIL: test_prot_param_algid_aes256_name: expected 'AES-256', got '%s'\n",
                    name ? name : "(null)");
        return 1;
    }
    return 0;
}

/**
 * test_prot_param_algid_des_xl_name — verify ALGID 0x9F resolves to "DES-XL"
 */
static int
test_prot_param_algid_des_xl_name(void) {
    const char* name = p25_algid_name(0x9F);
    if (name == NULL || strcmp(name, "DES-XL") != 0) {
        DSD_FPRINTF(stderr, "FAIL: test_prot_param_algid_des_xl_name: expected 'DES-XL', got '%s'\n",
                    name ? name : "(null)");
        return 1;
    }
    return 0;
}

/**
 * test_tsbk_0x3e_not_protection — TSBK 0x3E is not a protection parameter
 * message in sdrtrunk; it must not update protection state.
 */
static int
test_tsbk_0x3e_not_protection(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    DSD_MEMSET(MAC, 0, sizeof(MAC));
    MAC[0] = 0x07;
    MAC[1] = 0x7E;
    MAC[2] = 0x80;
    MAC[3] = 0x56;
    MAC[4] = 0x78;
    MAC[5] = 0xAB;
    MAC[6] = 0xCD;
    MAC[7] = 0xEF;

    process_MAC_VPDU(&opts, &st, 0, MAC);

    if (st.p25_prot_algid != 0 || st.p25_prot_kid != 0) {
        DSD_FPRINTF(stderr, "FAIL: test_tsbk_0x3e_not_protection: protection state changed ALGID=0x%02X KID=0x%04X\n",
                    st.p25_prot_algid, st.p25_prot_kid);
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * 6.3 Time and Date Announcement Tests
 * ============================================================================ */

/**
 * test_time_date_state_stores_time_t — verify known date/time produces
 * expected time_t. Uses 2025-03-15 14:30:45 UTC.
 */
static int
test_time_date_state_stores_time_t(void) {
    int rc = 0;

    const time_t expected = (time_t)1742049045;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 1, 1, 0, 0, 0, 2025, 3, 15, 14, 30, 45);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    if (st.p25_sys_time != expected) {
        DSD_FPRINTF(stderr, "FAIL: test_time_date_state_stores_time_t: UTC time_t mismatch, expected %ld, got %ld\n",
                    (long)expected, (long)st.p25_sys_time);
        rc = 1;
    }
    if (st.p25_sys_time_valid != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_time_date_state_stores_time_t: expected valid flag set\n");
        rc = 1;
    }

    struct tm result;
    if (dsd_gmtime(&st.p25_sys_time, &result) != 0) {
        DSD_FPRINTF(stderr, "FAIL: test_time_date_state_stores_time_t: gmtime failed\n");
        rc = 1;
    } else if (result.tm_year + 1900 != 2025 || result.tm_mon + 1 != 3 || result.tm_mday != 15 || result.tm_hour != 14
               || result.tm_min != 30 || result.tm_sec != 45) {
        DSD_FPRINTF(
            stderr, "FAIL: test_time_date_state_stores_time_t: UTC round-trip failed: %04d-%02d-%02d %02d:%02d:%02d\n",
            result.tm_year + 1900, result.tm_mon + 1, result.tm_mday, result.tm_hour, result.tm_min, result.tm_sec);
        rc = 1;
    }

    return rc;
}

/**
 * test_time_date_state_stores_offset — verify LTO sign and minute offset are
 * stored directly.
 */
static int
test_time_date_state_stores_offset(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 0, 0, 1, 1, 330, 0, 0, 0, 0, 0, 0);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    if (st.p25_sys_time_offset != -330) {
        DSD_FPRINTF(stderr, "FAIL: test_time_date_state_stores_offset: expected -330, got %d\n",
                    st.p25_sys_time_offset);
        rc = 1;
    }
    if (st.p25_sys_time_offset_valid != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_time_date_state_stores_offset: expected offset valid flag set\n");
        rc = 1;
    }

    return rc;
}

static int
test_time_date_bridged_p1_survey_offset_layout(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    unsigned long long MAC[24];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    build_p1_bridged_time_date_offset_mac(MAC, 330, 1);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    if (st.p25_sys_time_offset != 330 || st.p25_sys_time_offset_valid != 1) {
        DSD_FPRINTF(stderr, "FAIL: bridged P1 positive UTC offset expected +330, got %d valid %u\n",
                    st.p25_sys_time_offset, st.p25_sys_time_offset_valid);
        rc = 1;
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    build_p1_bridged_time_date_offset_mac(MAC, 0x800 | 330, 0);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    if (st.p25_sys_time_offset != -330 || st.p25_sys_time_offset_valid != 1) {
        DSD_FPRINTF(stderr, "FAIL: bridged P1 negative UTC offset expected -330, got %d valid %u\n",
                    st.p25_sys_time_offset, st.p25_sys_time_offset_valid);
        rc = 1;
    }

    return rc;
}

/**
 * test_time_date_state_stores_utc_from_local_offset - verify the announced
 * local date/time plus UTC offset is stored as the corresponding UTC instant.
 */
static int
test_time_date_state_stores_utc_from_local_offset(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 1, 1, 1, 0, 330, 2025, 3, 15, 14, 30, 45);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    const time_t expected = (time_t)1742029245;
    if (st.p25_sys_time != expected) {
        DSD_FPRINTF(
            stderr,
            "FAIL: test_time_date_state_stores_utc_from_local_offset: UTC time_t mismatch, expected %ld, got %ld\n",
            (long)expected, (long)st.p25_sys_time);
        rc = 1;
    }
    if (st.p25_sys_time_valid != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_time_date_state_stores_utc_from_local_offset: expected valid flag set\n");
        rc = 1;
    }
    if (st.p25_sys_time_offset != 330) {
        DSD_FPRINTF(stderr, "FAIL: test_time_date_state_stores_utc_from_local_offset: expected +330, got %d\n",
                    st.p25_sys_time_offset);
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * 6.4 VPDU Dispatch Tests
 * ============================================================================ */

/**
 * test_vpdu_dispatch_0x75 — verify opcode 0x75 triggers Time/Date handler
 * (state updated).
 */
static int
test_vpdu_dispatch_0x75(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    build_time_date_mac(MAC, 1, 1, 1, 0, 0, 2025, 1, 1, 0, 0, 0);

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_sys_time != (time_t)1735689600) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x75: p25_sys_time not updated correctly\n");
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x78 — verify System Service Broadcast state retention.
 */
static int
test_vpdu_dispatch_0x78(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    DSD_MEMSET(MAC, 0, sizeof(MAC));
    MAC[1] = 0x78;
    MAC[3] = 0xAB;
    MAC[4] = 0xCD;
    MAC[5] = 0xEF;
    MAC[6] = 0x12;
    MAC[7] = 0x34;
    MAC[8] = 0x56;
    MAC[9] = 0x07;

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_sys_services_valid != 1 || st.p25_sys_services_available != 0xABCDEFU
        || st.p25_sys_services_supported != 0x123456U || st.p25_sys_services_request_priority != 0x07U) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x78: service state not retained\n");
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x7a — verify RFSS status LRA and active/failsoft state.
 */
static int
test_vpdu_dispatch_0x7a(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    DSD_MEMSET(MAC, 0, sizeof(MAC));
    MAC[1] = 0x7A;
    MAC[2] = 0xAB; /* LRA */
    MAC[3] = 0x11; /* A bit + SYSID high nibble */
    MAC[4] = 0x23;
    MAC[5] = 0x04;
    MAC[6] = 0x05;
    MAC[7] = 0x10;
    MAC[8] = 0x0A;
    MAC[9] = 0x01;

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_site_lra_valid != 1 || st.p25_site_lra != 0xAB || st.p25_site_network_active_valid != 1
        || st.p25_site_network_active != 1 || st.p2_rfssid != 0x04 || st.p2_siteid != 0x05) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x7a: RFSS status state not retained\n");
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x7c_adjacent_validity — verify adjacent-status LRA/CFVA
 * validity survives immediate channel resolution.
 */
static int
test_vpdu_dispatch_0x7c_adjacent_validity(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    st.p25_iden_fdma[1].base_freq = 851000000L / 5L;
    st.p25_iden_fdma[1].chan_type = 1;
    st.p25_iden_fdma[1].chan_spac = 100;
    st.p25_iden_fdma[1].populated = 1;
    st.p25_chan_tdma_explicit[1] = 1;

    unsigned long long MAC[24];
    DSD_MEMSET(MAC, 0, sizeof(MAC));
    MAC[1] = 0x7C;
    MAC[2] = 0xAB;
    MAC[3] = 0xB1;
    MAC[4] = 0x23;
    MAC[5] = 0x04;
    MAC[6] = 0x05;
    MAC[7] = 0x10;
    MAC[8] = 0x0A;
    MAC[9] = 0x02;

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_nb_count != 1 || st.p25_nb_entries[0].freq != 851125000L || st.p25_nb_entries[0].lra_valid != 1
        || st.p25_nb_entries[0].lra != 0xAB || st.p25_nb_entries[0].cfva_valid != 1
        || st.p25_nb_entries[0].cfva != 0x0B) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x7c_adjacent_validity: adjacent metadata not retained\n");
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x7e — verify bridged TSBK opcode 0x3E is handled as
 * Adjacent Status Broadcast, not as Protection Parameter Broadcast.
 */
static int
test_vpdu_dispatch_0x7e(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    DSD_MEMSET(MAC, 0, sizeof(MAC));
    MAC[0] = 0x07;
    MAC[1] = 0x7E;
    MAC[2] = 0x01;
    MAC[3] = 0x40;
    MAC[4] = 0x23;
    MAC[5] = 0x04;
    MAC[6] = 0x05;
    MAC[7] = 0x00;
    MAC[8] = 0x01;
    MAC[9] = 0x02;

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_prot_algid != 0 || st.p25_prot_kid != 0) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x7e: protection state changed ALGID=0x%02X KID=0x%04X\n",
                    st.p25_prot_algid, st.p25_prot_kid);
        rc = 1;
    }
    if (st.p25_pending_announcement_count != 1
        || st.p25_pending_announcements[0].kind != P25_PENDING_ANNOUNCEMENT_NEIGHBOR
        || st.p25_pending_announcements[0].sysid != 0 || st.p25_pending_announcements[0].lra_valid != 1
        || st.p25_pending_announcements[0].lra != 0x01 || st.p25_pending_announcements[0].cfva_valid != 1
        || st.p25_pending_announcements[0].cfva != 0x04) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x7e: pending adjacent metadata/sysid not retained\n");
        rc = 1;
    }

    return rc;
}

/**
 * test_vpdu_dispatch_0x7f — verify TSBK opcode 0x3F Protection Parameter
 * Update handler stores ALGID/KID.
 */
static int
test_vpdu_dispatch_0x7f(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));

    unsigned long long MAC[24];
    DSD_MEMSET(MAC, 0, sizeof(MAC));
    MAC[0] = 0x07;
    MAC[1] = 0x7F;
    MAC[4] = 0x81;
    MAC[5] = 0xAB;
    MAC[6] = 0xCD;
    MAC[7] = 0x12;
    MAC[8] = 0x34;
    MAC[9] = 0x56;

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    if (st.p25_prot_valid != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x7f: p25_prot_valid expected 1, got %u\n", st.p25_prot_valid);
        rc = 1;
    }
    if (st.p25_prot_algid != 0x81) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x7f: p25_prot_algid expected 0x81, got 0x%02X\n",
                    st.p25_prot_algid);
        rc = 1;
    }
    if (st.p25_prot_kid != 0xABCD) {
        DSD_FPRINTF(stderr, "FAIL: test_vpdu_dispatch_0x7f: p25_prot_kid expected 0xABCD, got 0x%04X\n",
                    st.p25_prot_kid);
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * main
 * ============================================================================ */

int
main(void) {
    int rc = 0;

    /* 6.1 Protection Parameter tests */
    rc |= test_prot_param_algid_unencrypted_name();
    rc |= test_prot_param_algid_aes256_name();
    rc |= test_prot_param_algid_des_ofb_name();
    rc |= test_prot_param_algid_des_xl_name();
    rc |= test_tsbk_0x3e_not_protection();

    /* 6.3 Time and Date Announcement tests */
    rc |= test_time_date_state_stores_time_t();
    rc |= test_time_date_state_stores_offset();
    rc |= test_time_date_bridged_p1_survey_offset_layout();
    rc |= test_time_date_state_stores_utc_from_local_offset();

    /* 6.4 VPDU dispatch tests */
    rc |= test_vpdu_dispatch_0x75();
    rc |= test_vpdu_dispatch_0x78();
    rc |= test_vpdu_dispatch_0x7a();
    rc |= test_vpdu_dispatch_0x7c_adjacent_validity();
    rc |= test_vpdu_dispatch_0x7e();
    rc |= test_vpdu_dispatch_0x7f();

    if (rc == 0) {
        DSD_FPRINTF(stderr, "All P25 Tier 3 conformance tests passed.\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
