// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 LCW site/control-channel broadcast tests.
 *
 * Field layouts are aligned with sdrtrunk's LCSystemServiceBroadcast,
 * LCSecondaryControlChannelBroadcast, LCAdjacentSiteStatusBroadcast,
 * LCRFSSStatusBroadcast, and LCNetworkStatusBroadcast decoders. The state
 * expectations mirror the existing TSBK/MAC helper behavior in dsd-neo.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
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
expect_eq_u32(const char* tag, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got 0x%X want 0x%X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
put_bits(uint8_t* bits, int start, int width, unsigned long long value) {
    for (int i = 0; i < width; i++) {
        int shift = width - 1 - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1ULL);
    }
}

static void
build_lcw(uint8_t* bits, uint8_t format) {
    DSD_MEMSET(bits, 0, 96);
    put_bits(bits, 0, 8, format);
}

static void
seed_fdma_iden(dsd_state* state, int iden, long base_hz, int spacing_125hz) {
    state->p25_iden_fdma[iden].base_freq = base_hz / 5L;
    state->p25_iden_fdma[iden].chan_type = 1;
    state->p25_iden_fdma[iden].chan_spac = spacing_125hz;
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] |= 1U;
}

static const p25_nb_entry_t*
find_neighbor(const dsd_state* state, uint8_t rfss, uint8_t site) {
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX; i++) {
        if (state->p25_nb_entries[i].rfss == rfss && state->p25_nb_entries[i].site == site) {
            return &state->p25_nb_entries[i];
        }
    }
    return NULL;
}

static int
test_lcw_system_service_broadcast(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    build_lcw(bits, 0x60);
    put_bits(bits, 20, 4, 0x0A);
    put_bits(bits, 24, 24, 0x123456);
    put_bits(bits, 48, 24, 0xFEDCBA);

    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_int("ssb valid", state.p25_sys_services_valid, 1);
    rc |= expect_eq_u32("ssb available", state.p25_sys_services_available, 0x123456U);
    rc |= expect_eq_u32("ssb supported", state.p25_sys_services_supported, 0xFEDCBAU);
    rc |= expect_eq_int("ssb priority", state.p25_sys_services_request_priority, 0x0A);
    return rc;
}

static int
test_lcw_secondary_cc_implicit_resolves_and_filters(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    seed_fdma_iden(&state, 1, 851000000L, 100);

    build_lcw(bits, 0x61);
    put_bits(bits, 8, 8, 2);
    put_bits(bits, 16, 8, 3);
    put_bits(bits, 24, 16, 0x100A);
    put_bits(bits, 40, 8, 0x31);
    put_bits(bits, 48, 16, 0x1005);
    put_bits(bits, 64, 8, 0x32);
    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_int("sccb implicit notes rfss", (int)state.p2_rfssid, 2);
    rc |= expect_eq_int("sccb implicit notes site", (int)state.p2_siteid, 3);
    rc |= expect_eq_int("sccb implicit count", state.p25_secondary_cc_count, 2);
    rc |= expect_eq_long("sccb implicit ch a", state.p25_secondary_cc_entries[0].freq, 851125000L);
    rc |= expect_eq_long("sccb implicit ch b", state.p25_secondary_cc_entries[1].freq, 851062500L);
    rc |= expect_eq_int("sccb implicit ssc a", state.p25_secondary_cc_entries[0].ssc, 0x31);
    rc |= expect_eq_int("sccb implicit ssc b", state.p25_secondary_cc_entries[1].ssc, 0x32);

    build_lcw(bits, 0x61);
    put_bits(bits, 8, 8, 9);
    put_bits(bits, 16, 8, 9);
    put_bits(bits, 24, 16, 0x1006);
    put_bits(bits, 40, 8, 0x44);
    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_int("sccb implicit foreign filtered", state.p25_secondary_cc_count, 2);
    return rc;
}

static int
test_lcw_secondary_cc_explicit_resolves(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    seed_fdma_iden(&state, 1, 851000000L, 100);

    build_lcw(bits, 0x66);
    put_bits(bits, 8, 8, 4);
    put_bits(bits, 16, 8, 5);
    put_bits(bits, 24, 16, 0x1002);
    put_bits(bits, 40, 16, 0x1003);
    put_bits(bits, 56, 8, 0x81);
    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_int("sccb explicit count", state.p25_secondary_cc_count, 1);
    rc |= expect_eq_long("sccb explicit downlink", state.p25_secondary_cc_entries[0].freq, 851025000L);
    rc |= expect_eq_int("sccb explicit rfss", state.p25_secondary_cc_entries[0].rfss, 4);
    rc |= expect_eq_int("sccb explicit site", state.p25_secondary_cc_entries[0].site, 5);
    rc |= expect_eq_int("sccb explicit ssc", state.p25_secondary_cc_entries[0].ssc, 0x81);
    return rc;
}

static int
test_lcw_adjacent_site_implicit_and_explicit(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    state.p2_sysid = 0x123;
    seed_fdma_iden(&state, 1, 851000000L, 100);

    build_lcw(bits, 0x62);
    put_bits(bits, 8, 8, 0x44);
    put_bits(bits, 16, 4, 0x0B);
    put_bits(bits, 20, 12, 0x123);
    put_bits(bits, 32, 8, 6);
    put_bits(bits, 40, 8, 7);
    put_bits(bits, 48, 16, 0x100A);
    put_bits(bits, 64, 8, 0x22);
    p25_lcw(&opts, &state, bits, 0);

    const p25_nb_entry_t* implicit = find_neighbor(&state, 6, 7);
    rc |= expect_eq_int("adj implicit count", state.p25_nb_count, 1);
    rc |= expect_eq_int("adj implicit present", implicit != NULL, 1);
    if (implicit) {
        rc |= expect_eq_long("adj implicit freq", implicit->freq, 851125000L);
        rc |= expect_eq_int("adj implicit sysid", implicit->sysid, 0x123);
        rc |= expect_eq_int("adj implicit lra", implicit->lra, 0x44);
        rc |= expect_eq_int("adj implicit lra valid", implicit->lra_valid, 1);
        rc |= expect_eq_int("adj implicit cfva", implicit->cfva, 0x0B);
        rc |= expect_eq_int("adj implicit cfva valid", implicit->cfva_valid, 1);
    }

    build_lcw(bits, 0x67);
    put_bits(bits, 8, 8, 0x45);
    put_bits(bits, 16, 16, 0x1005);
    put_bits(bits, 32, 8, 6);
    put_bits(bits, 40, 8, 8);
    put_bits(bits, 48, 16, 0x1001);
    put_bits(bits, 64, 8, 0x99);
    p25_lcw(&opts, &state, bits, 0);

    const p25_nb_entry_t* explicit_entry = find_neighbor(&state, 6, 8);
    rc |= expect_eq_int("adj explicit count", state.p25_nb_count, 2);
    rc |= expect_eq_int("adj explicit present", explicit_entry != NULL, 1);
    if (explicit_entry) {
        rc |= expect_eq_long("adj explicit freq", explicit_entry->freq, 851062500L);
        rc |= expect_eq_int("adj explicit lra", explicit_entry->lra, 0x45);
        rc |= expect_eq_int("adj explicit lra valid", explicit_entry->lra_valid, 1);
        rc |= expect_eq_int("adj explicit cfva not valid", explicit_entry->cfva_valid, 0);
    }
    return rc;
}

static int
test_lcw_rfss_status_implicit_and_explicit(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    seed_fdma_iden(&state, 1, 851000000L, 100);

    build_lcw(bits, 0x63);
    put_bits(bits, 8, 8, 0x22);
    put_bits(bits, 16, 4, 0x01);
    put_bits(bits, 20, 12, 0x123);
    put_bits(bits, 32, 8, 9);
    put_bits(bits, 40, 8, 10);
    put_bits(bits, 48, 16, 0x100A);
    put_bits(bits, 64, 8, 0x77);
    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_int("rfss implicit lra valid", state.p25_site_lra_valid, 1);
    rc |= expect_eq_int("rfss implicit lra", state.p25_site_lra, 0x22);
    rc |= expect_eq_int("rfss implicit network active valid", state.p25_site_network_active_valid, 1);
    rc |= expect_eq_int("rfss implicit network active", state.p25_site_network_active, 1);
    rc |= expect_eq_int("rfss implicit rfss", (int)state.p2_rfssid, 9);
    rc |= expect_eq_int("rfss implicit site", (int)state.p2_siteid, 10);

    build_lcw(bits, 0x68);
    put_bits(bits, 8, 8, 0x33);
    put_bits(bits, 16, 16, 0x1003);
    put_bits(bits, 32, 8, 11);
    put_bits(bits, 40, 8, 12);
    put_bits(bits, 48, 16, 0x1006);
    put_bits(bits, 64, 8, 0x88);
    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_int("rfss explicit lra", state.p25_site_lra, 0x33);
    rc |= expect_eq_int("rfss explicit rfss", (int)state.p2_rfssid, 11);
    rc |= expect_eq_int("rfss explicit site", (int)state.p2_siteid, 12);
    return rc;
}

static int
test_lcw_network_status_primary_cc_and_voice_guard(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    seed_fdma_iden(&state, 1, 851000000L, 100);

    build_lcw(bits, 0x64);
    put_bits(bits, 16, 20, 0xABCDE);
    put_bits(bits, 36, 12, 0x123);
    put_bits(bits, 48, 16, 0x100A);
    put_bits(bits, 64, 8, 0x55);
    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_long("nsb implicit cc", state.p25_cc_freq, 851125000L);
    rc |= expect_eq_long("nsb implicit trunk cc", state.trunk_cc_freq, 851125000L);
    rc |= expect_eq_long("nsb implicit lcn0", state.trunk_lcn_freq[0], 851125000L);
    rc |= expect_eq_long("nsb implicit wacn", (long)state.p2_wacn, 0xABCDE);
    rc |= expect_eq_int("nsb implicit sysid", (int)state.p2_sysid, 0x123);

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.p25_is_tuned = 1;
    state.p25_cc_freq = 851000000L;
    state.trunk_cc_freq = 851000000L;
    state.p2_wacn = 0x11111;
    state.p2_sysid = 0x222;
    seed_fdma_iden(&state, 1, 851000000L, 100);

    build_lcw(bits, 0x69);
    put_bits(bits, 8, 20, 0xABCDE);
    put_bits(bits, 28, 12, 0x123);
    put_bits(bits, 40, 16, 0x100A);
    put_bits(bits, 56, 16, 0x100B);
    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_long("nsb explicit voice guard cc", state.p25_cc_freq, 851000000L);
    rc |= expect_eq_long("nsb explicit voice guard trunk cc", state.trunk_cc_freq, 851000000L);
    rc |= expect_eq_long("nsb explicit voice guard wacn", (long)state.p2_wacn, 0x11111);
    rc |= expect_eq_int("nsb explicit voice guard sysid", (int)state.p2_sysid, 0x222);
    return rc;
}

static int
test_lcw_protection_parameter_still_updates_call_protection(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[96];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    build_lcw(bits, 0x65);
    put_bits(bits, 24, 8, 0x80);
    put_bits(bits, 32, 16, 0x1234);
    put_bits(bits, 48, 24, 0x010203);
    p25_lcw(&opts, &state, bits, 0);

    rc |= expect_eq_int("prot valid", state.p25_prot_valid, 1);
    rc |= expect_eq_int("prot algid", state.p25_prot_algid, 0x80);
    rc |= expect_eq_int("prot kid", state.p25_prot_kid, 0x1234);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_lcw_system_service_broadcast();
    rc |= test_lcw_secondary_cc_implicit_resolves_and_filters();
    rc |= test_lcw_secondary_cc_explicit_resolves();
    rc |= test_lcw_adjacent_site_implicit_and_explicit();
    rc |= test_lcw_rfss_status_implicit_and_explicit();
    rc |= test_lcw_network_status_primary_cc_and_voice_guard();
    rc |= test_lcw_protection_parameter_still_updates_call_protection();

    if (rc == 0) {
        DSD_FPRINTF(stderr, "\nAll test_p25_lcw_site_broadcasts tests PASSED\n");
    } else {
        DSD_FPRINTF(stderr, "\nSome test_p25_lcw_site_broadcasts tests FAILED\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
