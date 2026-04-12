// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify private-grant allow-list behavior in the P25p1 PDU helper path. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

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
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
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
seed_policy_group(dsd_state* st, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_legacy_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    return dsd_tg_policy_append_legacy_exact(st, &row);
}

long int
process_channel_to_freq(dsd_opts* opts, dsd_state* state, int channel) {
    (void)opts;
    (void)state;
    if (channel == 0x100A) {
        return 851125000;
    }
    return 0;
}

void
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    (void)freqs;
    (void)count;
}

void
p25_confirm_idens_for_current_site(dsd_state* state) {
    (void)state;
}

void
p25_reset_iden_tables(dsd_state* state) {
    (void)state;
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]) {
    (void)opts;
    (void)state;
    (void)type;
    (void)MAC;
}

int
p25_patch_tg_key_is_clear(const dsd_state* state, int group) {
    (void)state;
    (void)group;
    return 0;
}

int
p25_patch_sg_key_is_clear(const dsd_state* state, int group) {
    (void)state;
    (void)group;
    return 0;
}

void
p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)tg;
    (void)svc_bits;
}

void
p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)svc_bits;
    (void)tg;
    (void)src;
}

void
p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    (void)channel;
    (void)svc_bits;
    (void)dst;
    (void)src;
    if (!opts || !state) {
        return;
    }
    state->p25_sm_tune_count++;
    opts->p25_is_tuned = 1;
}

void
p25_format_chan_suffix(const dsd_state* state, uint16_t channel, int slot_hint, char* out, size_t out_sz) {
    (void)state;
    (void)channel;
    (void)slot_hint;
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    uint8_t mpdu[64];
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    memset(mpdu, 0, sizeof mpdu);

    opts.p25_trunk = 1;
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.trunk_use_allow_list = 1;
    st.p25_cc_freq = 851000000;

    // FDMA IDEN for channel 0x100A.
    int id = 1;
    st.p25_chan_iden = id;
    st.p25_chan_type[id] = 1;
    st.p25_chan_tdma[id] = 0;
    st.p25_base_freq[id] = 851000000 / 5;
    st.p25_chan_spac[id] = 100;
    st.p25_iden_trust[id] = 2;

    // Build ALT MBT Unit-to-Unit Voice Channel Grant - Extended (opcode 0x06).
    mpdu[0] = 0x17; // ALT MBT format
    mpdu[2] = 0x00; // MFID standard
    mpdu[7] = 0x06; // opcode: UU Voice Channel Grant Extended
    mpdu[8] = 0x00; // svc clear
    mpdu[3] = 0x00;
    mpdu[4] = 0x00;
    mpdu[5] = 0x02; // source
    mpdu[19] = 0x00;
    mpdu[20] = 0x00;
    mpdu[21] = 0x01; // target
    mpdu[22] = 0x10;
    mpdu[23] = 0x0A; // channel
    mpdu[24] = 0x10;
    mpdu[25] = 0x0A; // channelr

    unsigned before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_decode_pdu_trunking(&opts, &st, mpdu);
    rc |= expect_true("p1 pdu private unknown blocked in allow-list", st.p25_sm_tune_count == before);

    rc |= expect_true("policy seed private target", seed_policy_group(&st, 1u, "A", "UU-ALLOW") == 0);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_decode_pdu_trunking(&opts, &st, mpdu);
    rc |= expect_true("p1 pdu private known target tunes", st.p25_sm_tune_count == before + 1);

    dsd_state_ext_free_all(&st);

    return rc;
}
