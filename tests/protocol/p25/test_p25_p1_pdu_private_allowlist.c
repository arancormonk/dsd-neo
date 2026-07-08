// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify private-grant allow-list behavior in the P25p1 PDU helper path. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/protocol/p25/p25_cc_candidates.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

static int g_seed_count;
static int g_group_grant_count;
static int g_enc_lockout_count;
static int g_last_group_channel;
static int g_last_group_svc;
static int g_last_group_tg;
static int g_last_group_src;

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
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

static void
reset_calls(void) {
    g_seed_count = 0;
    g_group_grant_count = 0;
    g_enc_lockout_count = 0;
    g_last_group_channel = 0;
    g_last_group_svc = 0;
    g_last_group_tg = 0;
    g_last_group_src = 0;
}

static int
seed_policy_group(dsd_state* st, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    return dsd_tg_policy_append_exact(st, &row);
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_channel_to_freq(dsd_opts* opts, dsd_state* state, int channel) {
    (void)opts;
    (void)state;
    if (channel == 0x100A) {
        return 851125000;
    }
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    (void)freqs;
    (void)count;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_confirm_idens_for_current_site(dsd_state* state) {
    (void)state;
}

int
p25_cc_add_candidate(dsd_state* state, long freq_hz, int bump_added) {
    (void)state;
    (void)freq_hz;
    (void)bump_added;
    return 0;
}

void
p25_nb_add_ex(dsd_state* state, long freq, uint16_t sysid, uint8_t rfss, uint8_t site, uint8_t cfva) {
    (void)state;
    (void)freq;
    (void)sysid;
    (void)rfss;
    (void)site;
    (void)cfva;
}

void
p25_store_site_lra(dsd_state* state, uint8_t lra) {
    (void)state;
    (void)lra;
}

void
p25_store_site_network_active(dsd_state* state, uint8_t network_active) {
    (void)state;
    (void)network_active;
}

void
p25_store_protected_control_channel(dsd_state* state, uint8_t algid) {
    (void)state;
    (void)algid;
}

size_t
p25_format_adjacent_cfva(uint8_t cfva, char* out, size_t out_len) {
    (void)cfva;
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    return 0;
}

int
p25_announce_neighbor_channel(const dsd_opts* opts, dsd_state* state, uint16_t channel, uint32_t wacn, int wacn_valid,
                              uint16_t sysid, uint8_t rfss, uint8_t site, uint8_t cfva) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)wacn;
    (void)wacn_valid;
    (void)sysid;
    (void)rfss;
    (void)site;
    (void)cfva;
    return 0;
}

int
p25_announce_neighbor_channel_ex(const dsd_opts* opts, dsd_state* state,
                                 const p25_neighbor_channel_announcement_t* announcement) {
    if (!announcement) {
        return 0;
    }
    return p25_announce_neighbor_channel(opts, state, announcement->channel, announcement->wacn,
                                         announcement->wacn_valid, announcement->sysid, announcement->rfss,
                                         announcement->site, announcement->cfva);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_reset_iden_tables(dsd_state* state) {
    (void)state;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_update_system_identity(dsd_state* state, unsigned long long wacn, unsigned long long sysid) {
    if (!state || (wacn == 0 && sysid == 0)) {
        return 0;
    }
    if ((state->p2_wacn != 0 || state->p2_sysid != 0) && (state->p2_wacn != wacn || state->p2_sysid != sysid)) {
        p25_reset_iden_tables(state);
    }
    state->p2_wacn = wacn;
    state->p2_sysid = sysid;
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]) {
    (void)opts;
    (void)state;
    (void)type;
    (void)MAC;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_tg_key_is_clear(const dsd_state* state, int group) {
    (void)state;
    (void)group;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_sg_key_is_clear(const dsd_state* state, int group) {
    (void)state;
    (void)group;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)tg;
    (void)svc_bits;
    g_enc_lockout_count++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_seed_cc_from_current_tuner_if_unknown(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_seed_count++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    (void)opts;
    (void)state;
    g_group_grant_count++;
    g_last_group_channel = channel;
    g_last_group_svc = svc_bits;
    g_last_group_tg = tg;
    g_last_group_src = src;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_apply_group_grant_policy(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    (void)channel;
    (void)src;
    if (opts && state && opts->p25_trunk == 1 && opts->trunk_tune_enc_calls == 0 && (svc_bits & 0x40) && tg > 0) {
        p25_emit_enc_lockout_once(opts, state, 0, tg, svc_bits);
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_on_group_data_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    p25_sm_on_group_grant(opts, state, channel, svc_bits, tg, src);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
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
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_on_indiv_data_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    p25_sm_on_indiv_grant(opts, state, channel, svc_bits, dst, src);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_aff_register(dsd_state* state, uint32_t rid) {
    (void)state;
    (void)rid;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_ga_add(dsd_state* state, uint32_t rid, uint16_t tg) {
    (void)state;
    (void)rid;
    (void)tg;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    DSD_MEMSET(mpdu, 0, sizeof mpdu);

    opts.p25_trunk = 1;
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.trunk_use_allow_list = 1;
    st.p25_cc_freq = 851000000;

    // FDMA IDEN for channel 0x100A.
    int id = 1;
    st.p25_chan_iden = id;
    // Populate new dual-array
    st.p25_iden_fdma[id].base_freq = 851000000 / 5;
    st.p25_iden_fdma[id].chan_type = 1;
    st.p25_iden_fdma[id].chan_spac = 100;
    st.p25_iden_fdma[id].trust = 2;
    st.p25_iden_fdma[id].populated = 1;
    st.p25_chan_tdma_explicit[id] = 1; // FDMA known

    // Build ALT MBT Unit-to-Unit Voice Channel Grant - Extended (opcode 0x06).
    mpdu[0] = 0x37; // outbound ALT MBT format
    mpdu[2] = 0x00; // MFID standard
    mpdu[6] = 0x02; // header plus two data blocks
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

    rc |= expect_true("policy seed group grant", seed_policy_group(&st, 0x1234u, "A", "TG-ALLOW") == 0);
    DSD_MEMSET(mpdu, 0, sizeof mpdu);
    mpdu[0] = 0x37; // outbound ALT MBT
    mpdu[2] = 0x00;
    mpdu[6] = 0x01; // header plus one data block
    mpdu[7] = 0x00; // Group Voice Channel Grant Update - Extended
    mpdu[8] = 0x00; // clear voice service
    mpdu[3] = 0x01;
    mpdu[4] = 0x02;
    mpdu[5] = 0x03; // source
    mpdu[14] = 0x10;
    mpdu[15] = 0x0A;
    mpdu[16] = 0x10;
    mpdu[17] = 0x0A;
    mpdu[18] = 0x12;
    mpdu[19] = 0x34; // group
    opts.p25_trunk = 1;
    opts.trunk_use_allow_list = 0;
    opts.trunk_tune_enc_calls = 1;
    opts.payload = 0;
    opts.p25_is_tuned = 0;
    reset_calls();
    p25_decode_pdu_trunking(&opts, &st, mpdu);
    rc |= expect_true("p1 pdu group emergency state", st.p25_call_emergency[0] == 0);
    rc |= expect_true("p1 pdu group priority state", st.p25_call_priority[0] == 0);
    rc |= expect_true("p1 pdu group active channel", strstr(st.active_channel[0], "TG: 4660") != NULL);

    DSD_MEMSET(mpdu, 0, sizeof mpdu);
    mpdu[0] = 0x37;
    mpdu[2] = 0x00;
    mpdu[6] = 0x01; // header plus one data block
    mpdu[7] = 0x08; // Telephone Interconnect Voice Channel Grant
    mpdu[3] = 0x01;
    mpdu[4] = 0x02;
    mpdu[5] = 0x03; // target
    mpdu[12] = 0x10;
    mpdu[13] = 0x0A;
    mpdu[16] = 0x00;
    mpdu[17] = 0x20; // timer
    opts.p25_trunk = 0;
    opts.trunk_use_allow_list = 0;
    opts.payload = 0;
    opts.p25_is_tuned = 0;
    st.lasttg = 0x010203;
    st.synctype = DSD_SYNC_P25P1_POS;
    st.p25_vc_freq[0] = 0;
    st.p25_vc_freq[1] = 0;
    reset_calls();
    p25_decode_pdu_trunking(&opts, &st, mpdu);
    rc |= expect_true("p1 pdu telephone nontrunk p1 vc freq", st.p25_vc_freq[0] == 851125000);
    rc |= expect_true("p1 pdu telephone p1 leaves slot 2 freq", st.p25_vc_freq[1] == 0);
    rc |= expect_true("p1 pdu telephone no trunk tune hook",
                      g_group_grant_count == 0 && st.p25_sm_tune_count == before + 1);
    rc |= expect_true("p1 pdu telephone active channel", strstr(st.active_channel[0], "Active Tele Ch: 100A") != NULL);

    rc |= expect_true("policy seed mfid90 sg", seed_policy_group(&st, 0x2222u, "A", "SG-ALLOW") == 0);
    DSD_MEMSET(mpdu, 0, sizeof mpdu);
    mpdu[0] = 0x37;
    mpdu[2] = 0x90;
    mpdu[6] = 0x01; // header plus one data block
    mpdu[7] = 0x02; // MFID90 Group Regroup Channel Grant - Explicit
    mpdu[3] = 0x04;
    mpdu[4] = 0x05;
    mpdu[5] = 0x06; // source
    mpdu[12] = 0x10;
    mpdu[13] = 0x0A;
    mpdu[14] = 0x10;
    mpdu[15] = 0x0A;
    mpdu[16] = 0x22;
    mpdu[17] = 0x22; // supergroup
    opts.p25_trunk = 1;
    opts.trunk_use_allow_list = 0;
    opts.trunk_tune_enc_calls = 1;
    opts.p25_is_tuned = 0;
    reset_calls();
    p25_decode_pdu_trunking(&opts, &st, mpdu);
    rc |= expect_true("p1 pdu mfid90 active channel", strstr(st.active_channel[0], "SG: 8738") != NULL);

    DSD_MEMSET(mpdu, 0, sizeof mpdu);
    mpdu[0] = 0x17; // inbound ALT MBT ISP
    mpdu[2] = 0x00;
    mpdu[6] = 0x01; // header plus one data block
    mpdu[7] = 0x04; // inbound UU voice service request, not an outbound grant
    mpdu[8] = 0x00;
    mpdu[14] = 0x10;
    mpdu[15] = 0x0A;
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 0;
    reset_calls();
    before = st.p25_sm_tune_count;
    st.active_channel[0][0] = '\0';
    p25_decode_pdu_trunking(&opts, &st, mpdu);
    rc |= expect_true("inbound ambtc uu no tune", st.p25_sm_tune_count == before);
    rc |= expect_true("inbound ambtc uu no active grant", strstr(st.active_channel[0], "Active UU") == NULL);
    rc |= expect_true("inbound ambtc uu no group callback", g_group_grant_count == 0);

    DSD_MEMSET(mpdu, 0, sizeof mpdu);
    mpdu[0] = 0x15; // inbound UMBTC ISP
    mpdu[2] = 0x00;
    mpdu[6] = 0x01;  // header plus one data block
    mpdu[12] = 0x08; // explicit telephone dial request
    opts.p25_is_tuned = 0;
    reset_calls();
    before = st.p25_sm_tune_count;
    p25_decode_pdu_trunking(&opts, &st, mpdu);
    rc |= expect_true("inbound umbtc dial no tune", st.p25_sm_tune_count == before);
    rc |= expect_true("inbound umbtc dial no group callback", g_group_grant_count == 0);

    DSD_MEMSET(mpdu, 0, sizeof mpdu);
    mpdu[0] = 0x17; // inbound Motorola protected ISP
    mpdu[2] = 0x90;
    mpdu[6] = 0x01; // header plus one data block
    mpdu[7] = 0x00; // group regroup voice request, not MFID90 grant
    mpdu[16] = 0x22;
    mpdu[17] = 0x22;
    opts.p25_is_tuned = 0;
    reset_calls();
    st.active_channel[0][0] = '\0';
    p25_decode_pdu_trunking(&opts, &st, mpdu);
    rc |= expect_true("inbound mfid90 regroup no group callback", g_group_grant_count == 0);
    rc |= expect_true("inbound mfid90 regroup no active grant", strstr(st.active_channel[0], "MFID90 Ch") == NULL);

    dsd_state_ext_free_all(&st);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
