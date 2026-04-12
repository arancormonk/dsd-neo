// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 options gating tests for group/private grants via MAC VPDU.
 * Ensures trunk_tune_group_calls / trunk_tune_private_calls disable tuning.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]);
void p25_sm_on_release(dsd_opts* opts, dsd_state* state);

// Stubs for external hooks
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

// Alias decode helpers referenced by MAC VPDU handler
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

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_legacy_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    return dsd_tg_policy_upsert_legacy_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

int
main(void) {
    int rc = 0;

    // Build a TSBK-mapped vPDU Group Voice grant frame (DUID=0x07, op=0x40)
    unsigned long long MAC[24] = {0};
    MAC[0] = 0x07; // TSBK marker
    MAC[1] = 0x40; // Group Voice Channel Grant
    MAC[2] = 0x00; // svc
    MAC[3] = 0x10; // channel hi (iden=1)
    MAC[4] = 0x0A; // channel lo (ch=10)
    MAC[5] = 0x45; // group hi
    MAC[6] = 0x67; // group lo
    MAC[7] = 0xAB; // src hi
    MAC[8] = 0xCD;
    MAC[9] = 0xEF; // src lo

    // Shared opts/state with seeded IDEN mapping
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    opts.p25_trunk = 1;
    st.p25_cc_freq = 851000000;
    int iden = 1;
    st.p25_chan_iden = iden;
    st.p25_chan_type[iden] = 1;
    st.p25_chan_tdma[iden] = 0;
    st.p25_base_freq[iden] = 851000000 / 5;
    st.p25_chan_spac[iden] = 100;
    st.p25_iden_trust[iden] = 2; // trusted

    // Case A: group calls gated off -> no tune
    opts.trunk_tune_group_calls = 0;
    // Not testing ENC gating here; allow encrypted to ensure unknown-SVC paths do not block
    opts.trunk_tune_enc_calls = 1;
    int before = st.p25_sm_tune_count;
    process_MAC_VPDU(&opts, &st, 0 /*FACCH path*/, MAC);
    rc |= expect_true("group gating honored", st.p25_sm_tune_count == before);

    // Case B: group calls on -> tune occurs
    opts.trunk_tune_group_calls = 1;
    before = st.p25_sm_tune_count;
    process_MAC_VPDU(&opts, &st, 0, MAC);
    rc |= expect_true("group allowed tunes", st.p25_sm_tune_count == before + 1);
    p25_sm_on_release(&opts, &st);
    opts.p25_is_tuned = 0;

    // Case C: private grant gating — reuse MAC with private opcode mapping
    // Use MFID std (0) and UU opcode 0x44 in UU map (P2 handler honors private gate)
    unsigned long long MAC2[24] = {0};
    MAC2[1] = 0x44; // UU Voice Service Channel Grant
    MAC2[2] = 0x10; // channel hi
    MAC2[3] = 0x0A; // channel lo
    MAC2[4] = 0x00;
    MAC2[5] = 0x01; // target (private)
    MAC2[6] = 0x00;
    MAC2[7] = 0x00;
    MAC2[8] = 0x02; // src
    unsigned long long MAC3[24] = {0};
    memcpy(MAC3, MAC2, sizeof(MAC3));
    MAC3[3] = 0x0B; // use a different channel so grant de-dup doesn't mask this case

    // Reset tuned flag before private tests to allow tuning path
    opts.p25_is_tuned = 0;
    opts.trunk_tune_private_calls = 0;
    opts.trunk_tune_enc_calls = 1; // ensure ENC gating does not suppress UU grant
    before = st.p25_sm_tune_count;
    process_MAC_VPDU(&opts, &st, 0, MAC2);
    rc |= expect_true("private gating honored", st.p25_sm_tune_count == before);

    opts.p25_is_tuned = 0;
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_enc_calls = 1; // ensure ENC gating does not suppress UU grant
    before = st.p25_sm_tune_count;
    process_MAC_VPDU(&opts, &st, 0, MAC2);
    rc |= expect_true("private allowed tunes", st.p25_sm_tune_count == before + 1);

    // Case D: helper-path private allow-list behavior: unknown private IDs block.
    p25_sm_on_release(&opts, &st);
    opts.p25_is_tuned = 0;
    opts.trunk_use_allow_list = 1;
    opts.trunk_tune_private_calls = 1;
    before = st.p25_sm_tune_count;
    process_MAC_VPDU(&opts, &st, 0, MAC2);
    rc |= expect_true("private allow-list unknown blocked", st.p25_sm_tune_count == before);

    // Case E: helper-path private allow-list known target tunes.
    rc |= expect_true("seed private allow-list target", seed_exact(&st, 256, "A", "UU-ALLOW") == 0);
    opts.p25_is_tuned = 0;
    before = st.p25_sm_tune_count;
    process_MAC_VPDU(&opts, &st, 0, MAC3);
    rc |= expect_true("private allow-list known target tunes", st.p25_sm_tune_count == before + 1);

    return rc;
}
