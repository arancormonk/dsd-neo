// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused P25p2 MAC VPDU tests to exercise additional opcode paths
 * and unknown-length fallback handling.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

#define setenv dsd_test_setenv

typedef struct dsdneoRuntimeConfig dsdneoRuntimeConfig;

static int g_nmea_harris_calls;
static uint32_t g_nmea_harris_src;
static int g_nmea_harris_slot;

// Runtime config
void dsd_neo_config_init(const dsd_opts* opts);
const dsdneoRuntimeConfig* dsd_neo_get_config(void);

// Test shims
void p25_test_process_mac_vpdu(int type, const unsigned char* mac_bytes, int mac_len);
void p25_test_process_mac_vpdu_ex(int type, const unsigned char* mac_bytes, int mac_len, int is_lcch, int currentslot);

// Stubs referenced by MAC VPDU path
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
    g_nmea_harris_calls++;
    g_nmea_harris_src = src;
    g_nmea_harris_slot = slot;
}

// Rigctl/rtl stubs
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

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* tag, const char* haystack, const char* needle) {
    if (!haystack || !needle || strstr(haystack, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle ? needle : "(null)",
                    haystack ? haystack : "(null)");
        return 1;
    }
    return 0;
}

static void
put_u24_ull(unsigned long long int* mac, int pos, unsigned value) {
    mac[pos + 0] = (unsigned long long int)((value >> 16) & 0xFFU);
    mac[pos + 1] = (unsigned long long int)((value >> 8) & 0xFFU);
    mac[pos + 2] = (unsigned long long int)(value & 0xFFU);
}

static void
put_fqid_tail_ull(unsigned long long int* mac, int pos, unsigned wacn, unsigned sysid, unsigned source) {
    mac[pos + 0] = (unsigned long long int)((wacn >> 12) & 0xFFU);
    mac[pos + 1] = (unsigned long long int)((wacn >> 4) & 0xFFU);
    mac[pos + 2] = (unsigned long long int)(((wacn & 0x0FU) << 4) | ((sysid >> 8) & 0x0FU));
    mac[pos + 3] = (unsigned long long int)(sysid & 0xFFU);
    put_u24_ull(mac, pos + 4, source);
}

static int
run_sccb_candidate_case(const unsigned char* mac_bytes, int current_rfss, int current_site, long* out_freqs,
                        int out_cap, int* out_rfss, int* out_site, int* out_lcn_count, long* out_lcn_freqs,
                        int out_lcn_cap) {
    static dsd_opts opts;
    dsd_state* state = NULL;
    DSD_MEMSET(&opts, 0, sizeof opts);
    state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return -1;
    }

    const int iden = 1;
    state->p25_iden_fdma[iden].chan_type = 1;
    state->p25_iden_fdma[iden].chan_spac = 100;
    state->p25_iden_fdma[iden].base_freq = 851000000 / 5;
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] = 1;
    state->p2_rfssid = current_rfss;
    state->p2_siteid = current_site;

    unsigned long long int MAC[24] = {0};
    for (int i = 0; i < 24; i++) {
        MAC[i] = mac_bytes[i];
    }

    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    int count = (cc != NULL) ? cc->count : 0;
    for (int i = 0; i < count && i < out_cap; i++) {
        out_freqs[i] = cc->candidates[i];
    }
    if (out_rfss) {
        *out_rfss = (int)state->p2_rfssid;
    }
    if (out_site) {
        *out_site = (int)state->p2_siteid;
    }
    if (out_lcn_count) {
        *out_lcn_count = state->lcn_freq_count;
    }
    if (out_lcn_freqs) {
        for (int i = 0; i < out_lcn_cap && i < 3; i++) {
            out_lcn_freqs[i] = state->trunk_lcn_freq[i];
        }
    }
    dsd_state_ext_free_all(state);
    free(state);
    return count;
}

static int
write_full_sccb_cache_fixture(const char* path, long first_freq) {
    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        DSD_FPRINTF(stderr, "failed to write SCCB cache fixture: %s\n", strerror(errno));
        return 0;
    }
    for (int i = 0; i < DSD_TRUNK_CC_CANDIDATES_MAX; i++) {
        DSD_FPRINTF(fp, "cc %ld\n", first_freq + (long)i * 12500L);
    }
    fclose(fp);
    return 1;
}

static int
cc_candidates_contains(const dsd_trunk_cc_candidates* cc, long freq) {
    if (!cc || cc->count <= 0 || cc->count > DSD_TRUNK_CC_CANDIDATES_MAX) {
        return 0;
    }
    for (int i = 0; i < cc->count; i++) {
        if (cc->candidates[i] == freq) {
            return 1;
        }
    }
    return 0;
}

static int
run_bridged_sccb_zero_channel_b_case(void) {
    static dsd_opts opts;
    dsd_state* state = NULL;
    int rc = 0;
    DSD_MEMSET(&opts, 0, sizeof opts);
    state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    state->p25_iden_fdma[0].chan_type = 1;
    state->p25_iden_fdma[0].chan_spac = 100;
    state->p25_iden_fdma[0].base_freq = 800000000 / 5;
    state->p25_iden_fdma[0].populated = 1;
    state->p25_iden_fdma[1].chan_type = 1;
    state->p25_iden_fdma[1].chan_spac = 100;
    state->p25_iden_fdma[1].base_freq = 851000000 / 5;
    state->p25_iden_fdma[1].populated = 1;
    state->p25_chan_tdma_explicit[1] = 1;

    unsigned long long int MAC[24] = {0};
    MAC[0] = 0x07;
    MAC[1] = 0x79;
    MAC[2] = 0x02;
    MAC[3] = 0x03;
    MAC[4] = 0x10;
    MAC[5] = 0x0A;
    MAC[6] = 0x01;
    MAC[7] = 0x00;
    MAC[8] = 0x00;
    MAC[9] = 0x00;

    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    const int count = (cc != NULL) ? cc->count : 0;
    rc |= expect_eq_long("p1_bridge_sccb_zero_ch_b_count", count, 1);
    rc |= expect_true("p1_bridge_sccb_zero_ch_b_has_ch1", cc_candidates_contains(cc, 851000000 + 10 * 100 * 125));
    rc |= expect_eq_long("p1_bridge_sccb_zero_ch_b_no_chan0_cache", state->trunk_chan_map[0], 0);
    rc |= expect_eq_long("p1_bridge_sccb_zero_ch_b_no_second_lcn", state->trunk_lcn_freq[2], 0);

    dsd_state_ext_free_all(state);
    free(state);
    return rc;
}

static int
run_native_sccb_zero_channel_b_case(void) {
    static dsd_opts opts;
    dsd_state* state = NULL;
    int rc = 0;
    DSD_MEMSET(&opts, 0, sizeof opts);
    state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    state->p25_iden_fdma[0].chan_type = 1;
    state->p25_iden_fdma[0].chan_spac = 100;
    state->p25_iden_fdma[0].base_freq = 800000000 / 5;
    state->p25_iden_fdma[0].populated = 1;
    state->p25_iden_fdma[1].chan_type = 1;
    state->p25_iden_fdma[1].chan_spac = 100;
    state->p25_iden_fdma[1].base_freq = 851000000 / 5;
    state->p25_iden_fdma[1].populated = 1;
    state->p25_chan_tdma_explicit[1] = 1;

    unsigned long long int MAC[24] = {0};
    MAC[1] = 0x79;
    MAC[2] = 0x02;
    MAC[3] = 0x03;
    MAC[4] = 0x10;
    MAC[5] = 0x0A;
    MAC[6] = 0x01;
    MAC[7] = 0x00;
    MAC[8] = 0x00;
    MAC[9] = 0x01;

    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    const int count = (cc != NULL) ? cc->count : 0;
    rc |= expect_eq_long("p2_sccb_zero_ch_b_count", count, 2);
    rc |= expect_true("p2_sccb_zero_ch_b_has_ch1", cc_candidates_contains(cc, 851000000 + 10 * 100 * 125));
    rc |= expect_true("p2_sccb_zero_ch_b_has_ch0", cc_candidates_contains(cc, 800000000));
    rc |= expect_eq_long("p2_sccb_zero_ch_b_chan0_cache", state->trunk_chan_map[0], 800000000);
    rc |= expect_eq_long("p2_sccb_zero_ch_b_second_lcn", state->trunk_lcn_freq[2], 800000000);

    dsd_state_ext_free_all(state);
    free(state);
    return rc;
}

static void
put_iden_base(unsigned char* mac, int pos, long base_freq) {
    mac[pos + 0] = (unsigned char)((base_freq >> 24) & 0xFF);
    mac[pos + 1] = (unsigned char)((base_freq >> 16) & 0xFF);
    mac[pos + 2] = (unsigned char)((base_freq >> 8) & 0xFF);
    mac[pos + 3] = (unsigned char)(base_freq & 0xFF);
}

static void
build_standard_iden(unsigned char* mac, int iden, int spacing, long base_freq) {
    DSD_MEMSET(mac, 0, 24);
    mac[1] = 0x7D;
    mac[2] = (unsigned char)((iden & 0x0F) << 4);
    mac[3] = 0x00;
    mac[4] = (unsigned char)((spacing >> 8) & 0x03);
    mac[5] = (unsigned char)(spacing & 0xFF);
    put_iden_base(mac, 6, base_freq);
}

static int
run_deferred_sccb_resolution_case(void) {
    int rc = 0;
    static dsd_opts opts;
    dsd_state* state = NULL;
    DSD_MEMSET(&opts, 0, sizeof opts);
    state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    unsigned long long int MAC[24] = {0};
    MAC[1] = 0xE9;
    MAC[2] = 0x02;
    MAC[3] = 0x03;
    MAC[4] = 0x10;
    MAC[5] = 0x0A;
    MAC[6] = 0x10;
    MAC[7] = 0x05;
    MAC[8] = 0x01;
    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    rc |= expect_eq_long("p2_sccb_deferred_initial_candidates", cc ? cc->count : 0, 0);
    rc |= expect_eq_long("p2_sccb_deferred_initial_pending", state->p25_pending_announcement_count, 1);

    unsigned char iden_mac[24];
    build_standard_iden(iden_mac, 1, 100, 851000000L / 5L);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    for (int i = 0; i < 24; i++) {
        MAC[i] = iden_mac[i];
    }
    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    const long want = 851000000L + 10L * 100L * 125L;
    cc = dsd_trunk_cc_candidates_peek(state);
    rc |= expect_true("p2_sccb_deferred_resolved_candidate", cc_candidates_contains(cc, want));
    rc |= expect_eq_long("p2_sccb_deferred_pending_empty", state->p25_pending_announcement_count, 0);
    rc |= expect_eq_long("p2_sccb_deferred_rfss", state->p2_rfssid, 0x02);
    rc |= expect_eq_long("p2_sccb_deferred_site", state->p2_siteid, 0x03);

    dsd_state_ext_free_all(state);
    free(state);
    return rc;
}

static int
run_deferred_adjacent_wacn_resolution_case(void) {
    int rc = 0;
    static dsd_opts opts;
    dsd_state* state = NULL;
    DSD_MEMSET(&opts, 0, sizeof opts);
    state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    unsigned long long int MAC[24] = {0};
    MAC[1] = 0xFE;
    MAC[2] = 0x01;
    MAC[3] = 0x21; /* CFVA=2, SYSID high nibble=1 */
    MAC[4] = 0x23;
    MAC[5] = 0x04;
    MAC[6] = 0x05;
    MAC[7] = 0x10;
    MAC[8] = 0x0A;
    MAC[9] = 0x10;
    MAC[10] = 0x05;
    MAC[11] = 0x01;
    MAC[12] = 0xAB;
    MAC[13] = 0xCD;
    MAC[14] = 0xE0;
    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    rc |= expect_eq_long("p2_adj_deferred_initial_neighbors", state->p25_nb_count, 0);
    rc |= expect_eq_long("p2_adj_deferred_initial_pending", state->p25_pending_announcement_count, 1);

    unsigned char iden_mac[24];
    build_standard_iden(iden_mac, 1, 100, 851000000L / 5L);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    for (int i = 0; i < 24; i++) {
        MAC[i] = iden_mac[i];
    }
    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    const long want = 851000000L + 10L * 100L * 125L;
    rc |= expect_eq_long("p2_adj_deferred_pending_empty", state->p25_pending_announcement_count, 0);
    rc |= expect_eq_long("p2_adj_deferred_neighbor_count", state->p25_nb_count, 1);
    rc |= expect_eq_long("p2_adj_deferred_neighbor_freq", state->p25_nb_entries[0].freq, want);
    rc |= expect_eq_long("p2_adj_deferred_neighbor_wacn_valid", state->p25_nb_entries[0].wacn_valid, 1);
    rc |= expect_eq_long("p2_adj_deferred_neighbor_wacn", state->p25_nb_entries[0].wacn, 0xABCDE);

    dsd_state_ext_free_all(state);
    free(state);
    return rc;
}

static int
run_p1_bridged_adjacent_unknown_sysid_case(void) {
    int rc = 0;
    static dsd_opts opts;
    dsd_state* state = NULL;
    DSD_MEMSET(&opts, 0, sizeof opts);
    state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    const int iden = 1;
    state->p25_iden_fdma[iden].chan_type = 1;
    state->p25_iden_fdma[iden].chan_spac = 100;
    state->p25_iden_fdma[iden].base_freq = 851000000 / 5;
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] = 1;

    unsigned long long int MAC[24] = {0};
    MAC[0] = 0x07;
    MAC[1] = 0x7C;
    MAC[2] = 0x02; /* LRA */
    MAC[3] = 0xAF; /* CFVA=0xA, low nibble must not become SYSID high bits */
    MAC[4] = 0xEE; /* reserved/SYSID-looking byte */
    MAC[5] = 0x04; /* RFSS */
    MAC[6] = 0x05; /* SITE */
    MAC[7] = 0x10;
    MAC[8] = 0x0A; /* CHAN-T 0x100A */
    MAC[9] = 0x01;
    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    const long want = 851000000L + 10L * 100L * 125L;
    rc |= expect_eq_long("p1_bridge_adjacent_neighbor_count", state->p25_nb_count, 1);
    rc |= expect_eq_long("p1_bridge_adjacent_neighbor_freq", state->p25_nb_entries[0].freq, want);
    rc |= expect_eq_long("p1_bridge_adjacent_neighbor_sysid_unknown", state->p25_nb_entries[0].sysid, 0);
    rc |= expect_eq_long("p1_bridge_adjacent_neighbor_wacn_invalid", state->p25_nb_entries[0].wacn_valid, 0);
    rc |= expect_eq_long("p1_bridge_adjacent_neighbor_lra", state->p25_nb_entries[0].lra, 0x02);
    rc |= expect_eq_long("p1_bridge_adjacent_neighbor_lra_valid", state->p25_nb_entries[0].lra_valid, 1);
    rc |= expect_eq_long("p1_bridge_adjacent_neighbor_cfva", state->p25_nb_entries[0].cfva, 0x0A);
    rc |= expect_eq_long("p1_bridge_adjacent_neighbor_cfva_valid", state->p25_nb_entries[0].cfva_valid, 1);

    dsd_state_ext_free_all(state);
    free(state);
    return rc;
}

static int
run_sccb_full_cache_preservation_case(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_p2_sccb_cache")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    setenv("DSD_NEO_CACHE_DIR", dir, 1);
    setenv("DSD_NEO_CC_CACHE", "1", 1);
    dsd_neo_config_init(NULL);

    static dsd_opts opts;
    dsd_state* state = NULL;
    DSD_MEMSET(&opts, 0, sizeof opts);
    state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    const int iden = 1;
    state->p25_iden_fdma[iden].chan_type = 1;
    state->p25_iden_fdma[iden].chan_spac = 100;
    state->p25_iden_fdma[iden].base_freq = 851000000 / 5;
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] = 1;
    state->p2_wacn = 0xABCDE;
    state->p2_sysid = 0x123;
    state->p2_rfssid = 0x02;
    state->p2_siteid = 0x03;

    char cache_path[1024];
    if (!p25_cc_build_cache_path(state, cache_path, sizeof(cache_path))) {
        dsd_state_ext_free_all(state);
        free(state);
        return 1;
    }
    if (!write_full_sccb_cache_fixture(cache_path, 852000000L)) {
        dsd_state_ext_free_all(state);
        free(state);
        return 1;
    }

    unsigned long long int MAC[24] = {0};
    MAC[1] = 0xE9;
    MAC[2] = 0x02;
    MAC[3] = 0x03;
    MAC[4] = 0x10;
    MAC[5] = 0x0A;
    MAC[6] = 0x10;
    MAC[7] = 0x05;
    MAC[8] = 0x01;
    process_MAC_VPDU(&opts, state, 0 /* FACCH */, MAC);

    const long sccb_freq = 851000000 + 10 * 100 * 125;
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    rc |= expect_eq_long("p2_sccb_cache_loaded", state->p25_cc_cache_loaded, 1);
    rc |= expect_eq_long("p2_sccb_full_cache_count", cc ? cc->count : 0, DSD_TRUNK_CC_CANDIDATES_MAX);
    rc |= expect_true("p2_sccb_preserved_after_full_cache_load", cc_candidates_contains(cc, sccb_freq));

    dsd_state_ext_free_all(state);
    free(state);
    return rc;
}

static int
run_standard_mac_supplemental_display_cases(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x03;
    MAC[2] = 0x90;
    MAC[4] = 0x0A;
    put_u24_ull(MAC, 5, 0x010203);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0x03 telephone user active", state.active_channel[0], "TELE Target: 66051");
    rc |= expect_eq_long("0x03 telephone user last active", state.last_active_time != 0, 1);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x4C;
    MAC[3] = 0x12;
    MAC[4] = 0x81;
    put_u24_ull(MAC, 5, 0x123456);
    put_u24_ull(MAC, 8, 0x654321);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0x4C RUM target", state.active_channel[0], "RUM Target: 1193046");
    rc |= expect_contains("0x4C RUM source", state.active_channel[0], "Source: 6636321");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x5E;
    put_u24_ull(MAC, 2, 0x010203);
    put_u24_ull(MAC, 5, 0x004567);
    MAC[10] = 0x40;
    MAC[11] = 0x05;
    MAC[12] = 0x12;
    MAC[13] = 0x34;
    MAC[14] = 0x80;
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0x5E RUM enhanced target", state.active_channel[0], "RUM-E Target: 66051");
    rc |= expect_contains("0x5E RUM enhanced tg", state.active_channel[0], "TG: 17767");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xCC;
    MAC[3] = 0x20;
    MAC[4] = 0x82;
    put_u24_ull(MAC, 5, 0x010203);
    put_fqid_tail_ull(MAC, 8, 0xABCDE, 0x123, 0x112233);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xCC RUM extended target", state.active_channel[0], "RUM-X Target: 66051");
    rc |= expect_contains("0xCC RUM extended source", state.active_channel[0], "Source: 1122867");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xD8;
    MAC[3] = 0x12;
    MAC[4] = 0x34;
    put_u24_ull(MAC, 5, 0x010203);
    put_fqid_tail_ull(MAC, 8, 0xABCDE, 0x123, 0x112233);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xD8 status extended", state.active_channel[0], "STATUS-X Target: 66051");
    rc |= expect_contains("0xD8 status source", state.active_channel[0], "Source: 1122867");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xDA;
    put_u24_ull(MAC, 2, 0x010203);
    put_fqid_tail_ull(MAC, 5, 0xABCDE, 0x123, 0x112233);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xDA query extended", state.active_channel[0], "Status Query-X Target: 66051");
    rc |= expect_contains("0xDA query source", state.active_channel[0], "Source: 1122867");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xDC;
    MAC[3] = 0xCA;
    MAC[4] = 0xFE;
    put_u24_ull(MAC, 5, 0x010203);
    put_fqid_tail_ull(MAC, 8, 0xABCDE, 0x123, 0x112233);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xDC message extended", state.active_channel[0], "MSG-X Target: 66051");
    rc |= expect_contains("0xDC message payload", state.active_channel[0], "Message: CAFE");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xDF;
    put_u24_ull(MAC, 2, 0x010203);
    put_fqid_tail_ull(MAC, 5, 0xABCDE, 0x123, 0x112233);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xDF alert extended", state.active_channel[0], "Call Alert-X Target: 66051");
    rc |= expect_contains("0xDF alert source", state.active_channel[0], "Source: 1122867");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xE4;
    MAC[3] = 0x00;
    MAC[4] = 0x10;
    put_u24_ull(MAC, 5, 0x0BADF0);
    put_u24_ull(MAC, 8, 0x010203);
    put_fqid_tail_ull(MAC, 11, 0xABCDE, 0x123, 0x112233);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xE4 extfunc target", state.active_channel[0], "EXTFUNC-X Target: 66051");
    rc |= expect_contains("0xE4 extfunc source", state.active_channel[0], "Source: 1122867");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xE5;
    MAC[3] = 0x00;
    MAC[4] = 0x10;
    put_u24_ull(MAC, 5, 0x0BADF0);
    put_u24_ull(MAC, 8, 0x010203);
    MAC[11] = 0xAB;
    MAC[12] = 0xCD;
    MAC[13] = 0xE1;
    MAC[14] = 0x23;
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xE5 extfunc lcch target", state.active_channel[0], "EXTFUNC-L Target: 66051");
    rc |= expect_contains("0xE5 extfunc lcch source", state.active_channel[0], "Source: ABCDE:123");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xEA;
    put_u24_ull(MAC, 2, 0x010203);
    put_fqid_tail_ull(MAC, 5, 0xABCDE, 0x123, 0x112233);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xEA affiliation query extended", state.active_channel[0],
                          "Group Affiliation Query-X Target: 66051");
    rc |= expect_contains("0xEA affiliation query source", state.active_channel[0], "Source: 1122867");
    dsd_state_ext_free_all(&state);

    return rc;
}

static int
run_standard_mac_unit_to_unit_extended_cases(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xC4;
    MAC[2] = 0x10;
    MAC[3] = 0x0A;
    MAC[4] = 0x10;
    MAC[5] = 0x05;
    put_fqid_tail_ull(MAC, 6, 0xABCDE, 0x123, 0xAABBCC);
    put_u24_ull(MAC, 13, 0x010203);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xC4 corrected target", state.active_channel[0], "TGT: 66051");
    rc |= expect_contains("0xC4 corrected source", state.active_channel[0], "SRC: 11189196");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xC6;
    MAC[2] = 0x10;
    MAC[3] = 0x0B;
    MAC[4] = 0x10;
    MAC[5] = 0x06;
    put_fqid_tail_ull(MAC, 6, 0xABCDE, 0x123, 0x112233);
    put_u24_ull(MAC, 13, 0x445566);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xC6 update target", state.active_channel[0], "TGT: 4478310");
    rc |= expect_contains("0xC6 update source", state.active_channel[0], "SRC: 1122867");
    dsd_state_ext_free_all(&state);

    return rc;
}

static int
run_group_affiliation_response_extended_case(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    MAC[1] = 0xE8;
    MAC[3] = 0x00; // global/local flag clear and response accepted
    MAC[4] = 0x12;
    MAC[5] = 0x34; // announcement group
    MAC[6] = 0x45;
    MAC[7] = 0x67; // local group
    MAC[8] = 0xAB;
    MAC[9] = 0xCD;
    MAC[10] = 0xE1;
    MAC[11] = 0x23;
    MAC[12] = 0x45;
    MAC[13] = 0x67; // source GID tail
    put_u24_ull(MAC, 14, 0x010203);

    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("0xE8 affiliation active", state.active_channel[0], "AFF-X Target: 66051");
    rc |= expect_eq_long("0xE8 accepted aff count", state.p25_aff_count, 1);
    rc |= expect_eq_long("0xE8 accepted ga count", state.p25_ga_count, 1);
    rc |= expect_eq_long("0xE8 accepted target", state.p25_aff_rid[0], 0x010203);
    rc |= expect_eq_long("0xE8 accepted ga rid", state.p25_ga_rid[0], 0x010203);
    rc |= expect_eq_long("0xE8 accepted ga tg", state.p25_ga_tg[0], 0x4567);

    dsd_state_ext_free_all(&state);
    return rc;
}

static void
seed_vendor_display_only_state(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    opts->p25_trunk = 1;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
    state->p25_cc_freq = 851000000L;
    state->lasttg = 0x1111;
    state->lasttgR = 0x2222;
    state->lastsrc = 0x010203;
    state->lastsrcR = 0x040506;
    state->p25_aff_count = 1;
    state->p25_aff_rid[0] = 0x112233;
    state->p25_ga_count = 1;
    state->p25_ga_rid[0] = 0x112233;
    state->p25_ga_tg[0] = 0x3344;
    state->p25_patch_count = 1;
    state->p25_patch_sgid[0] = 0x4567;
    state->p25_patch_active[0] = 1;
}

static int
expect_vendor_display_only_state(const char* tag, const dsd_opts* opts, const dsd_state* state) {
    int rc = 0;
    char label[128];

    DSD_SNPRINTF(label, sizeof label, "%s p25 tuned", tag);
    rc |= expect_eq_long(label, opts->p25_is_tuned, 0);
    DSD_SNPRINTF(label, sizeof label, "%s trunk tuned", tag);
    rc |= expect_eq_long(label, opts->trunk_is_tuned, 0);
    DSD_SNPRINTF(label, sizeof label, "%s cc freq", tag);
    rc |= expect_eq_long(label, state->p25_cc_freq, 851000000L);
    DSD_SNPRINTF(label, sizeof label, "%s lasttg", tag);
    rc |= expect_eq_long(label, state->lasttg, 0x1111);
    DSD_SNPRINTF(label, sizeof label, "%s lasttgR", tag);
    rc |= expect_eq_long(label, state->lasttgR, 0x2222);
    DSD_SNPRINTF(label, sizeof label, "%s lastsrc", tag);
    rc |= expect_eq_long(label, state->lastsrc, 0x010203);
    DSD_SNPRINTF(label, sizeof label, "%s lastsrcR", tag);
    rc |= expect_eq_long(label, state->lastsrcR, 0x040506);
    DSD_SNPRINTF(label, sizeof label, "%s aff count", tag);
    rc |= expect_eq_long(label, state->p25_aff_count, 1);
    DSD_SNPRINTF(label, sizeof label, "%s ga count", tag);
    rc |= expect_eq_long(label, state->p25_ga_count, 1);
    DSD_SNPRINTF(label, sizeof label, "%s patch count", tag);
    rc |= expect_eq_long(label, state->p25_patch_count, 1);
    DSD_SNPRINTF(label, sizeof label, "%s patch sg", tag);
    rc |= expect_eq_long(label, state->p25_patch_sgid[0], 0x4567);
    return rc;
}

static int
run_vendor_mac_display_only_cases(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    seed_vendor_display_only_state(&opts, &state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x82;
    MAC[2] = 0x90;
    MAC[3] = 0x11;
    MAC[4] = 0x09;
    put_u24_ull(MAC, 5, 0x010203);
    put_u24_ull(MAC, 8, 0x040506);
    MAC[11] = 0x09;
    put_u24_ull(MAC, 12, 0x070809);
    put_u24_ull(MAC, 15, 0x0A0B0C);
    process_MAC_VPDU(&opts, &state, 1 /* SACCH */, MAC);
    rc |= expect_contains("moto 0x82 active radios label", state.active_channel[0], "MOT AGR 130");
    rc |= expect_contains("moto 0x82 active radios rid1", state.active_channel[0], "66051");
    rc |= expect_contains("moto 0x82 active radios rid4", state.active_channel[0], "658188");
    rc |= expect_vendor_display_only_state("moto 0x82", &opts, &state);
    dsd_state_ext_free_all(&state);

    seed_vendor_display_only_state(&opts, &state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x8F;
    MAC[2] = 0x90;
    MAC[3] = 0x0B;
    MAC[4] = 0x80;
    MAC[5] = 0x09;
    put_u24_ull(MAC, 6, 0x010203);
    put_u24_ull(MAC, 9, 0x040506);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("moto 0x8F active radios label", state.active_channel[0], "MOT AGR 143");
    rc |= expect_contains("moto 0x8F active radios status", state.active_channel[0], "Status: 80");
    rc |= expect_contains("moto 0x8F active radios rid2", state.active_channel[0], "263430");
    rc |= expect_vendor_display_only_state("moto 0x8F", &opts, &state);
    dsd_state_ext_free_all(&state);

    seed_vendor_display_only_state(&opts, &state);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xBF;
    MAC[2] = 0x90;
    MAC[3] = 0x03;
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_contains("moto 0xBF feature marker", state.active_channel[0], "MOT AGR Feature Active");
    rc |= expect_vendor_display_only_state("moto 0xBF", &opts, &state);
    dsd_state_ext_free_all(&state);

    seed_vendor_display_only_state(&opts, &state);
    g_nmea_harris_calls = 0;
    g_nmea_harris_src = 0;
    g_nmea_harris_slot = -1;
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xAA;
    MAC[2] = 0xA4;
    MAC[3] = 0x11;
    MAC[4] = 0x06;
    MAC[5] = 0xCC;
    MAC[6] = 0x03;
    MAC[7] = 0x1E;
    MAC[8] = 0x15;
    MAC[9] = 0x0E;
    MAC[10] = 0x8A;
    MAC[11] = 0x53;
    MAC[12] = 0xC0;
    MAC[13] = 0xE6;
    MAC[14] = 0x92;
    MAC[15] = 0x00;
    MAC[16] = 0x06;
    MAC[17] = 0x1C;
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
    rc |= expect_eq_long("harris 0xAA gps calls", g_nmea_harris_calls, 1);
    rc |= expect_eq_long("harris 0xAA gps source", g_nmea_harris_src, 0x010203);
    rc |= expect_eq_long("harris 0xAA gps slot", g_nmea_harris_slot, 0);
    rc |= expect_vendor_display_only_state("harris 0xAA", &opts, &state);
    dsd_state_ext_free_all(&state);

    return rc;
}

static void
init_multifragment_base(unsigned long long int* mac, int opcode, unsigned data_len) {
    DSD_MEMSET(mac, 0, sizeof(unsigned long long int) * 24U);
    mac[1] = (unsigned long long int)(opcode & 0xFF);
    mac[2] = 18; // base structure length, matching fixed multi-fragment opcode length
    mac[3] = (unsigned long long int)(data_len & 0xFFU);
}

static void
init_multifragment_continuation(unsigned long long int* mac, unsigned len) {
    DSD_MEMSET(mac, 0, sizeof(unsigned long long int) * 24U);
    mac[1] = 0x10;
    mac[2] = (unsigned long long int)(len & 0x3FU);
}

static int
run_standard_mac_multifragment_cases(void) {
    static const struct {
        int opcode;
        const char* label;
    } complete_cases[] = {
        {0x71, "AUTH-L"},   {0xC7, "UU-UP-L"},  {0xCB, "CALL-L"},    {0xCD, "RUM-L"},   {0xCE, "MSG-L"},
        {0xCF, "UU-SVC-L"}, {0xD9, "STATUS-L"}, {0xDB, "STATUSQ-L"}, {0xDE, "RUM-E-L"}, {0xE0, "ACK-L"},
    };

    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int base[24] = {0};
    unsigned long long int cont[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);

    DSD_MEMSET(&state, 0, sizeof state);
    init_multifragment_continuation(cont, 10);
    put_u24_ull(cont, 3, 0x010203);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, cont);
    rc |= expect_true("orphan continuation does not mutate active state", state.active_channel[0][0] == '\0');
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    init_multifragment_base(base, 0xD9, 24);
    base[4] = 0x12;
    base[5] = 0x34;
    put_u24_ull(base, 6, 0x010203);
    put_fqid_tail_ull(base, 9, 0xABCDE, 0x123, 0x112233);
    put_u24_ull(base, 16, 0x445566);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, base);
    rc |= expect_true("base fragment does not mutate active state", state.active_channel[0][0] == '\0');

    init_multifragment_continuation(cont, 10);
    put_fqid_tail_ull(cont, 3, 0x0BCDE, 0x234, 0x778899);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, cont);
    rc |= expect_contains("completed 0xD9 status label", state.active_channel[0], "STATUS-L");
    rc |= expect_contains("completed 0xD9 target", state.active_channel[0], "Target: 66051");
    rc |= expect_contains("completed 0xD9 source", state.active_channel[0], "Source: 4478310");
    rc |= expect_contains("completed 0xD9 unit", state.active_channel[0], "Unit: 12");
    rc |= expect_contains("completed 0xD9 user", state.active_channel[0], "User: 34");
    dsd_state_ext_free_all(&state);

    for (size_t i = 0; i < sizeof(complete_cases) / sizeof(complete_cases[0]); i++) {
        DSD_MEMSET(&state, 0, sizeof state);
        init_multifragment_base(base, complete_cases[i].opcode, 24);
        base[4] = 0x91;
        base[5] = 0x02;
        base[6] = 0x03;
        base[7] = 0x04;
        put_fqid_tail_ull(base, 8, 0xABCDE, 0x123, 0x112233);
        base[15] = 0x10;
        base[16] = 0x0A;
        base[17] = 0x10;
        base[18] = 0x05;

        init_multifragment_continuation(cont, 10);
        put_u24_ull(cont, 3, 0x445566);
        put_fqid_tail_ull(cont, 6, 0x0BCDE, 0x234, 0x778899);

        process_MAC_VPDU(&opts, &state, 0 /* FACCH */, base);
        process_MAC_VPDU(&opts, &state, 0 /* FACCH */, cont);
        rc |=
            expect_contains("completed multi-fragment opcode label", state.active_channel[0], complete_cases[i].label);
        dsd_state_ext_free_all(&state);
    }

    DSD_MEMSET(&state, 0, sizeof state);
    init_multifragment_base(base, 0xD9, 24);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, base);
    init_multifragment_continuation(cont, 2); // invalid: no continuation payload after opcode/length bytes
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, cont);
    rc |= expect_true("invalid continuation clears without active state", state.active_channel[0][0] == '\0');
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    init_multifragment_base(base, 0x71, 255);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, base);
    for (int i = 0; i < 11; i++) {
        init_multifragment_continuation(cont, 23);
        process_MAC_VPDU(&opts, &state, 0 /* FACCH */, cont);
    }
    init_multifragment_continuation(cont, 11);
    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, cont);
    rc |= expect_eq_long("max-length multi-fragment clears active", state.p25_mac_frag_active, 0);
    rc |= expect_eq_long("max-length multi-fragment clears collected", state.p25_mac_frag_collected, 0);
    rc |= expect_contains("max-length multi-fragment completes", state.active_channel[0], "AUTH-L");
    dsd_state_ext_free_all(&state);

    return rc;
}

static int
run_cases(void) {
    int rc = 0;

    // Case 1: SACCH, PTT opcode (0x01) with basic header → JSON emits summary "PTT"
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x01; // PTT
        mac[2] = 0x00; // standard MFID
        p25_test_process_mac_vpdu(1 /*SACCH*/, mac, 24);
    }

    // Case 2: FACCH, IDLE opcode (0x03)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[0] = 1;    // header-present hint
        mac[1] = 0x03; // IDLE
        mac[2] = 0x00;
        p25_test_process_mac_vpdu(0 /*FACCH*/, mac, 24);
    }

    // Case 3: Unknown opcode with no header (no MCO) to trigger unknown-length path
    // Use opcode 0x07 (reserved), MFID 0x00; MAC[0]==0 (no header) so table=0, MCO skip → unknown length warning path.
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x07; // reserved/unknown
        mac[2] = 0x00; // standard MFID
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, /*is_lcch*/ 0, /*slot*/ 0);
    }

    // Case 4: LCCH label with SIGNAL opcode to exercise LCCH gating inside VPDU
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x00; // SIGNAL
        mac[2] = 0x00;
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, /*is_lcch*/ 1, /*slot*/ 1);
    }

    // Case 5: native Phase 2 SCCB explicit (0xE9) exposes one downlink channel.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0xE9;
        mac[2] = 0x02; // RFSS
        mac[3] = 0x03; // SITE
        mac[4] = 0x10; // CHAN-T 0x100A
        mac[5] = 0x0A;
        mac[6] = 0x10; // CHAN-R 0x1005 (uplink)
        mac[7] = 0x05;
        mac[8] = 0x01; // service class

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, NULL, NULL, 0);
        rc |= expect_eq_long("p2_sccb_explicit_count", count, 1);
        rc |= expect_eq_long("p2_sccb_explicit_downlink", freqs[0], 851000000 + 10 * 100 * 125);
    }

    // Case 6: native Phase 2 SCCB implicit (0x79) exposes both channel slots when B is valid.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x79;
        mac[2] = 0x02; // RFSS
        mac[3] = 0x03; // SITE
        mac[4] = 0x10; // CHAN1 0x100A
        mac[5] = 0x0A;
        mac[6] = 0x01; // service class 1
        mac[7] = 0x10; // CHAN2 0x1005
        mac[8] = 0x05;
        mac[9] = 0x01; // service class 2 marks channel B present

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, NULL, NULL, 0);
        rc |= expect_eq_long("p2_sccb_implicit_count", count, 2);
        rc |= expect_eq_long("p2_sccb_implicit_ch1", freqs[0], 851000000 + 10 * 100 * 125);
        rc |= expect_eq_long("p2_sccb_implicit_ch2", freqs[1], 851000000 + 5 * 100 * 125);
    }

    // Case 7: P1-bridged SCCB explicit (0x69) also exposes one downlink channel.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[0] = 0x07; // P1 TSBK bridge marker used by this decoder path
        mac[1] = 0x69;
        mac[2] = 0x02; // RFSS
        mac[3] = 0x03; // SITE
        mac[4] = 0x10; // CHAN-T 0x100A
        mac[5] = 0x0A;
        mac[6] = 0x10; // CHAN-R 0x1005 (uplink)
        mac[7] = 0x05;
        mac[8] = 0x01; // service class

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, NULL, NULL, 0);
        rc |= expect_eq_long("p1_bridge_sccb_explicit_count", count, 1);
        rc |= expect_eq_long("p1_bridge_sccb_explicit_downlink", freqs[0], 851000000 + 10 * 100 * 125);
    }

    // Case 8: P1-bridged SCCB implicit keeps channel B even when SSC B is zero.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[0] = 0x07;
        mac[1] = 0x79;
        mac[2] = 0x02; // RFSS
        mac[3] = 0x03; // SITE
        mac[4] = 0x10; // CHAN1 0x100A
        mac[5] = 0x0A;
        mac[6] = 0x01; // SSC A
        mac[7] = 0x10; // CHAN2 0x1005
        mac[8] = 0x05;
        mac[9] = 0x00; // SSC B zero is still a valid bridged P1 channel B

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, NULL, NULL, 0);
        rc |= expect_eq_long("p1_bridge_sccb_implicit_zero_ssc_count", count, 2);
        rc |= expect_eq_long("p1_bridge_sccb_implicit_zero_ssc_ch1", freqs[0], 851000000 + 10 * 100 * 125);
        rc |= expect_eq_long("p1_bridge_sccb_implicit_zero_ssc_ch2", freqs[1], 851000000 + 5 * 100 * 125);
    }

    rc |= run_bridged_sccb_zero_channel_b_case();
    rc |= run_native_sccb_zero_channel_b_case();

    // Case 9: SCCB candidates are site-scoped when current RFSS/SITE are known.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        int rfss_after = 0;
        int site_after = 0;
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0xE9;
        mac[2] = 0x02;
        mac[3] = 0x03;
        mac[4] = 0x10;
        mac[5] = 0x0A;
        mac[6] = 0x10;
        mac[7] = 0x05;
        mac[8] = 0x01;

        int count = run_sccb_candidate_case(mac, 0x63, 0x63, freqs, 4, &rfss_after, &site_after, NULL, NULL, 0);
        rc |= expect_eq_long("p2_sccb_explicit_foreign_site_count", count, 0);
        rc |= expect_eq_long("p2_sccb_explicit_foreign_rfss_preserved", rfss_after, 0x63);
        rc |= expect_eq_long("p2_sccb_explicit_foreign_site_preserved", site_after, 0x63);
    }

    // Case 10: foreign-site bridged SCCB must not seed fallback LCN rotation.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        int lcn_count = -1;
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[0] = 0x07;
        mac[1] = 0x69;
        mac[2] = 0x02;
        mac[3] = 0x03;
        mac[4] = 0x10;
        mac[5] = 0x0A;
        mac[6] = 0x10;
        mac[7] = 0x05;
        mac[8] = 0x01;

        int count = run_sccb_candidate_case(mac, 0x63, 0x63, freqs, 4, NULL, NULL, &lcn_count, NULL, 0);
        rc |= expect_eq_long("p1_bridge_sccb_foreign_site_count", count, 0);
        rc |= expect_eq_long("p1_bridge_sccb_foreign_lcn_count", lcn_count, 0);
    }

    // Case 11: adjacent-site status remains neighbor/display data only. It
    // must not become a CC hunt candidate, matching OP25's rotation behavior.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x7C; // Adjacent Status Broadcast, abbreviated
        mac[2] = 0x01; // LRA
        mac[3] = 0x21; // CFVA=2 (valid/current), SYSID hi nibble=1
        mac[4] = 0x23; // SYSID low
        mac[5] = 0x04; // RFSS
        mac[6] = 0x05; // SITE
        mac[7] = 0x10; // CHAN-T 0x100A
        mac[8] = 0x0A;
        mac[9] = 0x01; // service class

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, NULL, NULL, 0);
        rc |= expect_eq_long("p2_adjacent_not_candidate", count, 0);
    }

    rc |= run_p1_bridged_adjacent_unknown_sysid_case();

    // Case 12: native Phase 2 SCCB implicit keeps a resolved second secondary
    // CC in fallback rotation even when the first channel's IDEN is unknown.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        long lcn_freqs[3] = {0};
        int lcn_count = -1;
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x79;
        mac[2] = 0x02;
        mac[3] = 0x03;
        mac[4] = 0x20; // CHAN1 0x200A uses unknown IDEN 2
        mac[5] = 0x0A;
        mac[6] = 0x01;
        mac[7] = 0x10; // CHAN2 0x1005 uses known IDEN 1
        mac[8] = 0x05;
        mac[9] = 0x01;

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, &lcn_count, lcn_freqs, 3);
        rc |= expect_eq_long("p2_sccb_implicit_partial_iden_count", count, 1);
        rc |= expect_eq_long("p2_sccb_implicit_partial_iden_ch2", freqs[0], 851000000 + 5 * 100 * 125);
        rc |= expect_eq_long("p2_sccb_implicit_partial_iden_lcn_count", lcn_count, 2);
        rc |= expect_eq_long("p2_sccb_implicit_partial_iden_lcn_ch2", lcn_freqs[1], 851000000 + 5 * 100 * 125);
    }

    // Case 13: a full persisted cache loaded during SCCB handling must not
    // evict the freshly validated current-site SCCB candidate.
    rc |= run_deferred_sccb_resolution_case();
    rc |= run_deferred_adjacent_wacn_resolution_case();
    rc |= run_sccb_full_cache_preservation_case();

    // Case 14: extended private voice (0x22) derives source from the SUID tail.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        state.currentslot = 0;

        MAC[1] = 0x22;
        MAC[2] = 0x00; // SVC
        MAC[3] = 0x01;
        MAC[4] = 0x23;
        MAC[5] = 0x45; // target
        MAC[6] = 0x01;
        MAC[7] = 0x02;
        MAC[8] = 0x03; // abbreviated source bytes, should be superseded
        MAC[9] = 0x10;
        MAC[10] = 0x20;
        MAC[11] = 0x30;
        MAC[12] = 0x40;
        MAC[13] = 0xAA;
        MAC[14] = 0xBB;
        MAC[15] = 0xCC; // SUID tail source

        process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);
        rc |= expect_eq_long("p2_private_ext_target", state.lasttg, 0x012345);
        rc |= expect_eq_long("p2_private_ext_suid_source", state.lastsrc, 0xAABBCC);
        dsd_state_ext_free_all(&state);
    }

    rc |= run_standard_mac_supplemental_display_cases();
    rc |= run_standard_mac_unit_to_unit_extended_cases();
    rc |= run_group_affiliation_response_extended_case();
    rc |= run_standard_mac_multifragment_cases();
    rc |= run_vendor_mac_display_only_cases();

    return rc;
}

int
main(void) {
    // Enable JSON emission to exercise emit paths
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init(NULL);

    // Capture stderr to a temp file to avoid polluting test logs; we don't need to parse it here.
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_p2_vpdu_core") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 101;
    }
    int rc = run_cases();
    dsd_test_capture_stderr_end(&cap);
    (void)remove(cap.path);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
