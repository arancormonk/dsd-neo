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
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

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
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

void
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

// Rigctl/rtl stubs
bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        fprintf(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
run_sccb_candidate_case(const unsigned char* mac_bytes, int current_rfss, int current_site, long* out_freqs,
                        int out_cap, int* out_rfss, int* out_site, int* out_lcn_count) {
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof opts);
    memset(&state, 0, sizeof state);

    const int iden = 1;
    state.p25_chan_type[iden] = 1;
    state.p25_chan_tdma_explicit[iden] = 1;
    state.p25_chan_spac[iden] = 100;
    state.p25_base_freq[iden] = 851000000 / 5;
    state.p2_rfssid = current_rfss;
    state.p2_siteid = current_site;

    unsigned long long int MAC[24] = {0};
    for (int i = 0; i < 24; i++) {
        MAC[i] = mac_bytes[i];
    }

    process_MAC_VPDU(&opts, &state, 0 /* FACCH */, MAC);

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(&state);
    int count = (cc != NULL) ? cc->count : 0;
    for (int i = 0; i < count && i < out_cap; i++) {
        out_freqs[i] = cc->candidates[i];
    }
    if (out_rfss) {
        *out_rfss = (int)state.p2_rfssid;
    }
    if (out_site) {
        *out_site = (int)state.p2_siteid;
    }
    if (out_lcn_count) {
        *out_lcn_count = state.lcn_freq_count;
    }
    dsd_state_ext_free_all(&state);
    return count;
}

static int
run_cases(void) {
    int rc = 0;

    // Case 1: SACCH, PTT opcode (0x01) with basic header → JSON emits summary "PTT"
    {
        unsigned char mac[24];
        memset(mac, 0, sizeof mac);
        mac[1] = 0x01; // PTT
        mac[2] = 0x00; // standard MFID
        p25_test_process_mac_vpdu(1 /*SACCH*/, mac, 24);
    }

    // Case 2: FACCH, IDLE opcode (0x03)
    {
        unsigned char mac[24];
        memset(mac, 0, sizeof mac);
        mac[0] = 1;    // header-present hint
        mac[1] = 0x03; // IDLE
        mac[2] = 0x00;
        p25_test_process_mac_vpdu(0 /*FACCH*/, mac, 24);
    }

    // Case 3: Unknown opcode with no header (no MCO) to trigger unknown-length path
    // Use opcode 0x07 (reserved), MFID 0x00; MAC[0]==0 (no header) so table=0, MCO skip → unknown length warning path.
    {
        unsigned char mac[24];
        memset(mac, 0, sizeof mac);
        mac[1] = 0x07; // reserved/unknown
        mac[2] = 0x00; // standard MFID
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, /*is_lcch*/ 0, /*slot*/ 0);
    }

    // Case 4: LCCH label with SIGNAL opcode to exercise LCCH gating inside VPDU
    {
        unsigned char mac[24];
        memset(mac, 0, sizeof mac);
        mac[1] = 0x00; // SIGNAL
        mac[2] = 0x00;
        p25_test_process_mac_vpdu_ex(0 /*FACCH*/, mac, 24, /*is_lcch*/ 1, /*slot*/ 1);
    }

    // Case 5: native Phase 2 SCCB explicit (0xE9) exposes one downlink channel.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        memset(mac, 0, sizeof mac);
        mac[1] = 0xE9;
        mac[2] = 0x02; // RFSS
        mac[3] = 0x03; // SITE
        mac[4] = 0x10; // CHAN-T 0x100A
        mac[5] = 0x0A;
        mac[6] = 0x10; // CHAN-R 0x1005 (uplink)
        mac[7] = 0x05;
        mac[8] = 0x01; // service class

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, NULL);
        rc |= expect_eq_long("p2_sccb_explicit_count", count, 1);
        rc |= expect_eq_long("p2_sccb_explicit_downlink", freqs[0], 851000000 + 10 * 100 * 125);
    }

    // Case 6: native Phase 2 SCCB implicit (0x79) exposes both channel slots when B is valid.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        memset(mac, 0, sizeof mac);
        mac[1] = 0x79;
        mac[2] = 0x02; // RFSS
        mac[3] = 0x03; // SITE
        mac[4] = 0x10; // CHAN1 0x100A
        mac[5] = 0x0A;
        mac[6] = 0x01; // service class 1
        mac[7] = 0x10; // CHAN2 0x1005
        mac[8] = 0x05;
        mac[9] = 0x01; // service class 2 marks channel B present

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, NULL);
        rc |= expect_eq_long("p2_sccb_implicit_count", count, 2);
        rc |= expect_eq_long("p2_sccb_implicit_ch1", freqs[0], 851000000 + 10 * 100 * 125);
        rc |= expect_eq_long("p2_sccb_implicit_ch2", freqs[1], 851000000 + 5 * 100 * 125);
    }

    // Case 7: P1-bridged SCCB explicit (0x69) also exposes one downlink channel.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        memset(mac, 0, sizeof mac);
        mac[0] = 0x07; // P1 TSBK bridge marker used by this decoder path
        mac[1] = 0x69;
        mac[2] = 0x02; // RFSS
        mac[3] = 0x03; // SITE
        mac[4] = 0x10; // CHAN-T 0x100A
        mac[5] = 0x0A;
        mac[6] = 0x10; // CHAN-R 0x1005 (uplink)
        mac[7] = 0x05;
        mac[8] = 0x01; // service class

        int count = run_sccb_candidate_case(mac, 0, 0, freqs, 4, NULL, NULL, NULL);
        rc |= expect_eq_long("p1_bridge_sccb_explicit_count", count, 1);
        rc |= expect_eq_long("p1_bridge_sccb_explicit_downlink", freqs[0], 851000000 + 10 * 100 * 125);
    }

    // Case 8: SCCB candidates are site-scoped when current RFSS/SITE are known.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        int rfss_after = 0;
        int site_after = 0;
        memset(mac, 0, sizeof mac);
        mac[1] = 0xE9;
        mac[2] = 0x02;
        mac[3] = 0x03;
        mac[4] = 0x10;
        mac[5] = 0x0A;
        mac[6] = 0x10;
        mac[7] = 0x05;
        mac[8] = 0x01;

        int count = run_sccb_candidate_case(mac, 0x63, 0x63, freqs, 4, &rfss_after, &site_after, NULL);
        rc |= expect_eq_long("p2_sccb_explicit_foreign_site_count", count, 0);
        rc |= expect_eq_long("p2_sccb_explicit_foreign_rfss_preserved", rfss_after, 0x63);
        rc |= expect_eq_long("p2_sccb_explicit_foreign_site_preserved", site_after, 0x63);
    }

    // Case 9: foreign-site bridged SCCB must not seed fallback LCN rotation.
    {
        unsigned char mac[24];
        long freqs[4] = {0};
        int lcn_count = -1;
        memset(mac, 0, sizeof mac);
        mac[0] = 0x07;
        mac[1] = 0x69;
        mac[2] = 0x02;
        mac[3] = 0x03;
        mac[4] = 0x10;
        mac[5] = 0x0A;
        mac[6] = 0x10;
        mac[7] = 0x05;
        mac[8] = 0x01;

        int count = run_sccb_candidate_case(mac, 0x63, 0x63, freqs, 4, NULL, NULL, &lcn_count);
        rc |= expect_eq_long("p1_bridge_sccb_foreign_site_count", count, 0);
        rc |= expect_eq_long("p1_bridge_sccb_foreign_lcn_count", lcn_count, 0);
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
        fprintf(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 101;
    }
    int rc = run_cases();
    dsd_test_capture_stderr_end(&cap);
    (void)remove(cap.path);
    return rc;
}
