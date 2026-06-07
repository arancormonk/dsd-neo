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
    (void)src;
    (void)slot;
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
    FILE* fp = fopen(path, "w");
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

    // Case 8: SCCB candidates are site-scoped when current RFSS/SITE are known.
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

    // Case 9: foreign-site bridged SCCB must not seed fallback LCN rotation.
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

    // Case 10: adjacent-site status remains neighbor/display data only. It
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

    // Case 11: native Phase 2 SCCB implicit keeps a resolved second secondary
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

    // Case 12: a full persisted cache loaded during SCCB handling must not
    // evict the freshly validated current-site SCCB candidate.
    rc |= run_sccb_full_cache_preservation_case();

    // Case 13: extended private voice (0x22) derives source from the SUID tail.
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
